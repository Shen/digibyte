// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Offer discovery, quote selection, reputation, and reservation RPCs. */

#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>

#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/validation.h>
#include <random.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>

#include <univalue.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_rpc::internal;



namespace {
ClientPaymentOptions ParseClientPaymentOptions(const UniValue& options, bool restricted_default)
{
    ClientPaymentOptions input;
    input.request_id = options.find_value("request_id").get_str();
    input.fee_cap = options.find_value("maximum_paymaster_fee_cents").getInt<int64_t>();
    input.subtract_fee = options.find_value("subtract_paymaster_fee_from_amount").isTrue();
    input.send_all = options.find_value("send_all_spendable_dd").isTrue();
    input.retry_transport = options.find_value("retry_transport").isTrue();
    const auto mode = options.find_value("fee_mode").isNull() ? "paymaster" : options.find_value("fee_mode").get_str();
    if (mode != "auto" && mode != "paymaster") throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_mode must be paymaster or auto");
    input.mode = mode == "auto" ? FeeMode::AUTO : FeeMode::PAYMASTER;
    const auto privacy = options.find_value("privacy").isNull() ? "standard" : options.find_value("privacy").get_str();
    if (privacy != "standard" && privacy != "high") throw JSONRPCError(RPC_INVALID_PARAMETER, "privacy must be standard or high");
    input.privacy = privacy == "high" ? PrivacyProfile::HIGH : PrivacyProfile::STANDARD;
    const auto selection = options.find_value("selection").isNull() ? "lowest_total_cost" : options.find_value("selection").get_str();
    if (selection != "lowest_total_cost" && selection != "privacy_weighted") throw JSONRPCError(RPC_INVALID_PARAMETER, "selection must be lowest_total_cost or privacy_weighted");
    input.selection = selection == "privacy_weighted" ? SelectionMode::PRIVACY_WEIGHTED : SelectionMode::LOWEST_TOTAL_COST;
    input.maximum_attempts = options.find_value("maximum_provider_attempts").isNull() ?
        ((input.privacy == PrivacyProfile::HIGH || (restricted_default && !options.find_value("sponsorship_capability").isNull())) ? 1 : 3) :
        options.find_value("maximum_provider_attempts").getInt<int>();
    if (!options.find_value("provider_identity_key").isNull()) input.identity_key = ParseHexV(options.find_value("provider_identity_key"), "provider_identity_key");
    if (!options.find_value("restricted_service_descriptor").isNull()) input.descriptor = ParseHexV(options.find_value("restricted_service_descriptor"), "restricted_service_descriptor");
    if (!options.find_value("sponsorship_capability").isNull()) input.capability = ParseHexV(options.find_value("sponsorship_capability"), "sponsorship_capability");
    return input;
}
} // namespace

RPCHelpMan getpaymasteroffers()
{
    return RPCHelpMan{
        "getpaymasteroffers",
        "Return a locally filtered and deterministically sorted snapshot of public Paymaster offers.\n"
        "Optional fee-rejected entries are diagnostic only and cannot authorize a payment.\n"
        "This does not contact a provider or reserve liquidity.\n",
        {
            {"amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Recipient amount, or exact total DD outflow when subtract_paymaster_fee_from_amount is true"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Optional local offer-preview semantics", {
                {"subtract_paymaster_fee_from_amount", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Treat amount_cents as exact total DD outflow and derive the recipient amount"},
                {"maximum_paymaster_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Requested fee ceiling, further limited by wallet policy and remaining daily budget"},
                {"privacy", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "standard or high; high includes only Tor endpoints"},
                {"include_fee_rejected", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Include otherwise matching offers rejected by client fee limits, marked eligible=false; default false"},
            }},
        },
        RPCResult{RPCResult::Type::ARR, "", "Eligible public offers", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Sorted offer", {
                                                                                                                                          {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                                                                          {RPCResult::Type::STR, "display_name", "Untrusted display label"},
                                                                                                                                          {RPCResult::Type::STR, "endpoint", "Authenticated P2P endpoint"},
                                                                                                                                          {RPCResult::Type::STR_HEX, "offer_id", "Public offer identifier"},
                                                                                                                                          {RPCResult::Type::STR_HEX, "policy_hash", "Bound provider policy"},
                                                                                                                                          {RPCResult::Type::STR, "funding_model", "sponsored or user_paid"},
                                                                                                                                          {RPCResult::Type::BOOL, "eligible", /*optional=*/true, "Whether client fee limits permit this offer; present with include_fee_rejected"},
                                                                                                                                          {RPCResult::Type::STR, "fee_limit_reason", /*optional=*/true, "Fee exclusion: percentage or absolute_or_daily; diagnostic only"},
                                                                                                                                          {RPCResult::Type::NUM, "maximum_service_fee_bps", /*optional=*/true, "Current percentage limit for a rejected offer; -1 means not configured"},
                                                                                                                                          {RPCResult::Type::NUM, "available_service_fee_cents", /*optional=*/true, "Current requested, absolute and remaining daily fee ceiling for a rejected offer"},
                                                                                                                                          {RPCResult::Type::NUM, "fee_rate_bps", "Service-fee rate in basis points"},
                                                                                                                                          {RPCResult::Type::NUM, "maximum_user_paid_service_fee_cents", "Maximum user-paid DD service fee; zero means no cap"},
                                                                                                                                          {RPCResult::Type::NUM, "payment_cents", "Requested recipient amount"},
                                                                                                                                          {RPCResult::Type::NUM, "service_fee_cents", "Exact rounded service fee"},
                                                                                                                                          {RPCResult::Type::NUM, "user_total_cents", "Exact total charged to the user"},
                                                                                                                                          {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", "Whether amount_cents was treated as an exact total outflow"},
                                                                                                                                          {RPCResult::Type::NUM_TIME, "expires_at", "Announcement expiration"},
                                                                                                                                          {RPCResult::Type::BOOL, "reputation_sufficient_data", "Whether enough local observations exist"},
                                                                                                                                          {RPCResult::Type::NUM, "success_rate_basis_points", /*optional=*/true, "Observed success rate when statistically meaningful"},
                                                                                                                                          {RPCResult::Type::BOOL, "recommendation_deprioritized", "A first failed local attempt lowers automatic preference while other eligible providers exist"},
                                                                                                                                          {RPCResult::Type::NUM, "latency_ewma_ms", "Locally observed latency estimate"},
                                                                                                                                      }}}},
        RPCExamples{HelpExampleCli("getpaymasteroffers", "1000") +
                    HelpExampleCli("getpaymasteroffers", "5000 '{\"subtract_paymaster_fee_from_amount\":true}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            if (!context.paymaster || !context.paymaster->Enabled()) {
                throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar Paymaster support is disabled");
            }
            PaymasterStore store{*wallet};
            std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord> records;
            if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            std::map<DigiDollar::Paymaster::PaymasterId,
                     DigiDollar::Paymaster::PaymasterReliabilityRecord>
                reputation;
            for (auto& record : records)
                reputation.emplace(record.provider_id, std::move(record));
            std::string error;
            const int64_t now = GetTime();
            bool subtract_fee{false};
            bool include_fee_rejected{false};
            int64_t requested_fee_limit{DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS};
            PrivacyProfile privacy{PrivacyProfile::STANDARD};
            if (!request.params[1].isNull()) {
                const UniValue& options = request.params[1].get_obj();
                RPCTypeCheckObj(
                    options,
                    {{"subtract_paymaster_fee_from_amount", UniValueType(UniValue::VBOOL)},
                     {"maximum_paymaster_fee_cents", UniValueType(UniValue::VNUM)},
                     {"privacy", UniValueType(UniValue::VSTR)},
                     {"include_fee_rejected", UniValueType(UniValue::VBOOL)}},
                    /*fAllowNull=*/true, /*fStrict=*/true);
                if (!options.find_value(
                        "subtract_paymaster_fee_from_amount").isNull()) {
                    subtract_fee = options.find_value(
                        "subtract_paymaster_fee_from_amount").get_bool();
                }
                if (!options.find_value("maximum_paymaster_fee_cents").isNull()) {
                    requested_fee_limit = options.find_value(
                        "maximum_paymaster_fee_cents").getInt<int64_t>();
                }
                include_fee_rejected = options.find_value("include_fee_rejected").isTrue();
                if (!options.find_value("privacy").isNull()) {
                    const std::string profile = options.find_value("privacy").get_str();
                    if (profile == "high") {
                        privacy = PrivacyProfile::HIGH;
                    } else if (profile != "standard") {
                        throw JSONRPCError(RPC_INVALID_PARAMETER,
                                           "privacy must be standard or high");
                    }
                }
            }
            const DDCents maximum_service_fee = PreviewMaximumServiceFee(
                *wallet, DDCents{requested_fee_limit}, now, error);
            if (maximum_service_fee.value < 0) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            const DigiDollar::Paymaster::DDCents requested_amount{
                request.params[0].getInt<int64_t>()};
            const DDCents candidate_fee_limit = include_fee_rejected
                ? DDCents{MAX_DD_OUTPUT_CENTS} : maximum_service_fee;
            const auto candidates = subtract_fee
                ? DigiDollar::Paymaster::BuildGrossOfferCandidates(
                      context.paymaster->GetDirectory().List(now),
                      requested_amount,
                      DigiDollar::Paymaster::FUNDING_MODEL_ALL,
                      candidate_fee_limit,
                      reputation, now, error)
                : DigiDollar::Paymaster::BuildOfferCandidates(
                      context.paymaster->GetDirectory().List(now),
                      requested_amount,
                      DigiDollar::Paymaster::FUNDING_MODEL_ALL,
                      candidate_fee_limit,
                      reputation, now, error);
            if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            UniValue result{UniValue::VARR};
            ClientSafetyPolicy client_policy;
            if (!GetPaymasterClientSafetyPolicy(*wallet, client_policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED");
            }
            for (const auto& candidate : candidates) {
                if (privacy == PrivacyProfile::HIGH &&
                    !candidate.endpoint.IsTor()) continue;
                const bool within_policy = CheckClientServiceFee(
                    client_policy, candidate.payment, candidate.service_fee, error);
                const bool eligible = within_policy && candidate.service_fee.value <= maximum_service_fee.value;
                if (!eligible && !include_fee_rejected) continue;
                UniValue offer{UniValue::VOBJ};
                if (include_fee_rejected) {
                    offer.pushKV("eligible", eligible);
                    if (!eligible) {
                        offer.pushKV("fee_limit_reason",
                            candidate.service_fee.value <= maximum_service_fee.value &&
                            error == "PAYMASTER_CLIENT_PERCENTAGE_FEE_LIMIT_EXCEEDED"
                                ? "percentage" : "absolute_or_daily");
                        offer.pushKV("maximum_service_fee_bps", client_policy.maximum_service_fee_bps);
                        offer.pushKV("available_service_fee_cents", maximum_service_fee.value);
                    }
                }
                offer.pushKV("provider_id", candidate.provider_id.GetHex());
                offer.pushKV("display_name", candidate.display_name);
                offer.pushKV("endpoint", candidate.endpoint.ToStringAddrPort());
                offer.pushKV("offer_id", candidate.terms.offer_id.GetHex());
                offer.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
                offer.pushKV("funding_model", candidate.terms.funding_model == DigiDollar::Paymaster::FundingModel::SPONSORED ? "sponsored" : "user_paid");
                offer.pushKV("fee_rate_bps", candidate.terms.fee_rate_bps);
                offer.pushKV("maximum_user_paid_service_fee_cents",
                              candidate.terms.maximum_user_paid_service_fee.value);
                offer.pushKV("payment_cents", candidate.payment.value);
                offer.pushKV("service_fee_cents", candidate.service_fee.value);
                offer.pushKV("user_total_cents", candidate.user_total.value);
                offer.pushKV("subtract_paymaster_fee_from_amount", subtract_fee);
                offer.pushKV("expires_at", candidate.announcement_expires_at);
                offer.pushKV("reputation_sufficient_data", candidate.reliability.sufficient_data);
                offer.pushKV("recommendation_deprioritized", candidate.IsRecommendationDeprioritized());
                if (candidate.reliability.sufficient_data) {
                    offer.pushKV("success_rate_basis_points", candidate.reliability.success_rate_basis_points);
                }
                offer.pushKV("latency_ewma_ms", candidate.latency_ewma_ms);
                result.push_back(std::move(offer));
            }
            return result;
        }),
    };
}

RPCHelpMan requestpaymasterquote()
{
    return RPCHelpMan{
        "requestpaymasterquote",
        "Prepare or resume one idempotent Paymaster quote request. The call never waits for the network.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Provider identity from getpaymasteroffers"},
            {"intent", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Locally authorized payment intent", {
                                                                                                         {"request_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Canonical lowercase UUID"},
                                                                                                         {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "DigiDollar recipient address"},
                                                                                                         {"amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Recipient amount in integer cents"},
                                                                                                         {"requested_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Original wallet-local amount; exact total outflow when subtract_paymaster_fee_from_amount is true"},
                                                                                                         {"subtract_paymaster_fee_from_amount", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Bind recipient plus service fee to requested_amount_cents"},
                                                                                                         {"send_all_spendable_dd", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Bind this request to an exact all-spendable-DD input snapshot"},
                                                                                                         {"maximum_paymaster_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Hard service-fee cap"},
                                                                                                         {"fee_mode", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Calling mode: paymaster or auto"},
                                                                                                         {"privacy", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Privacy profile: standard or high"},
                                                                                                         {"selection", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selection policy: lowest_total_cost or privacy_weighted"},
                                                                                                         {"maximum_provider_attempts", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Strict sequential attempt limit"},
                                                                                                         {"offer_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact public offer"},
                                                                                                         {"provider_identity_key", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Required x-only provider identity for a restricted offer"},
                                                                                                         {"restricted_service_descriptor", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Canonical provider-signed restricted descriptor"},
                                                                                                         {"sponsorship_capability", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "One-payment sponsor capability; never persist or place in shell history"},
                                                                                                         {"retry_transport", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Explicitly retry a failed local connection; never extends a signed request or changes its inputs"},
                                                                                                         {"selected_inputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "Exact USER_DD input set", {{"input", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "", {{"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Input transaction"}, {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "Output index"}}}}},
                                                                                                     }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative asynchronous request state", {
                                                                                            {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                             {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                             {RPCResult::Type::STR_HEX, "canonical_request_hash", "Canonical idempotent request binding"},
                                                                                             {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount; exact total outflow in subtract mode"},
                                                                                             {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the service fee is deducted from requested_amount_cents"},
                                                                                             {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the request is bound to all ordinary spendable DD"},
                                                                                             {RPCResult::Type::STR, "requested_fee_mode", "Requested funding mode"},
                                                                                            {RPCResult::Type::STR, "fee_mode_used", "Authoritative funding mode"},
                                                                                        {RPCResult::Type::STR, "privacy_profile", /*optional=*/true, "Persisted standard or high privacy profile"},
                                                                                        {RPCResult::Type::STR, "to_address", /*optional=*/true, "Canonical recipient from the durable client authorization"},
                                                                                            {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider from the current or persisted authorization"},
                                                                                            {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Offer from the current or persisted authorization"},
                                                                                            {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Policy from the current or persisted authorization"},
                                                                                            {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
                                                                                            {RPCResult::Type::STR, "session_state", "Authoritative wallet session state"},
                                                                                            {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Outstanding asynchronous phase"},
                                                                                            {RPCResult::Type::BOOL, "final", "Whether the session is terminal"},
                                                                                            {RPCResult::Type::STR, "status", "success, canceled, failed, conflicted, or pending; success requires a locally confirmed recipient payment"},
                                                                                            {RPCResult::Type::BOOL, "payment_confirmed", "Validated recipient payment has positive local confirmation depth"},
                                                                                            PaymasterPaymentViewResult(),
                                                                                            {RPCResult::Type::STR, "broadcast_state", "not_attempted, unknown, accepted_mempool, accepted_stempool, or confirmed"},
                                                                                            {RPCResult::Type::STR, "confirmation_state", "unconfirmed, payment_confirmed, recovery_confirmed, or conflicted"},
                                                                                            {RPCResult::Type::NUM_TIME, "created_at", "Persistent session creation time"},
                                                                                            {RPCResult::Type::NUM_TIME, "updated_at", "Last durable session update"},
        {RPCResult::Type::NUM_TIME, "observed_at", true, "Time the current action capabilities were evaluated"},
                                                                                            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final payment transaction id"},
                                                                                            {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known idempotent self-recovery transaction id"},
                                                                                            {RPCResult::Type::ARR, "reserved_user_inputs", "Hard-reserved USER_DD inputs", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reserved input", {
                                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "txid", "Input transaction"},
                                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "vout", "Input output index"},
                                                                                                                                                                                                                                             }}}},
                                                                                            {RPCResult::Type::NUM, "provider_attempts", "Number of persistent provider attempts"},
                                                                                            {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Recipient amount from the current or persisted authorization"},
                                                                                            {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact service fee from the current or persisted authorization"},
                                                                                            {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Total DD payment and fee from the current or persisted authorization"},
                                                                                            {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Intent expiration; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the request is queued to a connected v2 peer"},
                                                                                            {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a short-lived connection was requested"},
                                                                                            PaymasterTransportResult(),
                                                                                            {RPCResult::Type::BOOL, "capacity_pending", /*optional=*/true, "Whether only the privacy-preserving capacity handshake is pending"},
                                                                                            {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Fully validated provider capacity snapshot"},
                                                                                            {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Validated provider quote"},
                                                                                            {RPCResult::Type::STR_HEX, "unsigned_txid", /*optional=*/true, "Validated unsigned transaction"},
                                                                                            {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Validated exact template"},
                                                                                            {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Opaque commitment to the exact validated client authorization manifest"},
                                                                                            {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment has been durably accepted"},
                                                                                            {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Monotonic durable acceptance time"},
                                                                                            {RPCResult::Type::STR, "psbt", /*optional=*/true, "Canonical unsigned collaborative PSBT"},
                                                                                        }},
        RPCExamples{HelpExampleCli("requestpaymasterquote", "\"0123...\" '{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\","
                                                            "\"address\":\"DD...\",\"amount_cents\":1000,"
                                                            "\"maximum_paymaster_fee_cents\":25,"
                                                            "\"selected_inputs\":[{\"txid\":\"abcd...\",\"vout\":0}]}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::string readiness_error;
            if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }
            const UniValue& options = request.params[1];
            RPCTypeCheckObj(options,
                            {{"request_id", UniValueType(UniValue::VSTR)},
                             {"address", UniValueType(UniValue::VSTR)},
                             {"amount_cents", UniValueType(UniValue::VNUM)},
                             {"requested_amount_cents", UniValueType(UniValue::VNUM)},
                             {"subtract_paymaster_fee_from_amount", UniValueType(UniValue::VBOOL)},
                             {"send_all_spendable_dd", UniValueType(UniValue::VBOOL)},
                             {"maximum_paymaster_fee_cents", UniValueType(UniValue::VNUM)},
                             {"fee_mode", UniValueType(UniValue::VSTR)},
                             {"privacy", UniValueType(UniValue::VSTR)},
                             {"selection", UniValueType(UniValue::VSTR)},
                             {"maximum_provider_attempts", UniValueType(UniValue::VNUM)},
                             {"offer_id", UniValueType(UniValue::VSTR)},
                             {"provider_identity_key", UniValueType(UniValue::VSTR)},
                             {"restricted_service_descriptor", UniValueType(UniValue::VSTR)},
                             {"sponsorship_capability", UniValueType(UniValue::VSTR)},
                             {"retry_transport", UniValueType(UniValue::VBOOL)},
                             {"selected_inputs", UniValueType(UniValue::VARR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            QuoteParameters input;
            static_cast<ClientPaymentOptions&>(input) = ParseClientPaymentOptions(options, true);
            input.provider_id = ParseHashV(request.params[0], "provider_id");
            input.address = options.find_value("address").get_str();
            input.payment = options.find_value("amount_cents").getInt<int64_t>();
            input.requested_amount = options.find_value("requested_amount_cents").isNull() ? input.payment : options.find_value("requested_amount_cents").getInt<int64_t>();
            input.inputs = ParsePaymasterInputs(options.find_value("selected_inputs"));
            if (!options.find_value("offer_id").isNull()) input.offer_id = ParseHashO(options, "offer_id");
            return ToJSON(RequestPaymasterQuote(context, *wallet, input));
        }),
    };
}

UniValue RequestAutomaticPaymasterQuote(const JSONRPCRequest& request,
                                        const std::string& address,
                                        CAmount amount,
                                        const UniValue& options,
                                        const std::vector<COutPoint>* preset_inputs)
{
    return InvokePaymasterService([&]() -> UniValue {
    using namespace DigiDollar::Paymaster;
    WalletContext& context = EnsureWalletContext(request.context);
    std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return UniValue::VNULL;
    AutomaticPaymentParameters input;
    static_cast<ClientPaymentOptions&>(input) = ParseClientPaymentOptions(options, false);
    input.address = address;
    input.amount = amount;
    input.prepare_only = options.find_value("prepare_only").isTrue();
    if (!options.find_value("authorization_commitment").isNull()) input.authorization_commitment = ParseHashO(options, "authorization_commitment");
    if (!options.find_value("preferred_provider_id").isNull()) input.preferred_provider = ParseHashO(options, "preferred_provider_id");
    if (!options.find_value("preferred_offer_id").isNull()) input.preferred_offer = ParseHashO(options, "preferred_offer_id");
    return ToJSON(RequestAutomaticPaymasterPayment(context, *wallet, input, preset_inputs));
    });
}

RPCHelpMan getpaymasterreputation()
{
    return RPCHelpMan{
        "getpaymasterreputation",
        "Return privacy-preserving local reliability aggregates. No payment details are stored.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional provider identity; omit to list all local records"},
        },
        RPCResult{RPCResult::Type::ARR, "", "Local provider reliability records", {
            {RPCResult::Type::OBJ, "", "Reliability aggregate", {
                {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                {RPCResult::Type::NUM, "successful_attempts", "Locally observed successful attempts"},
                {RPCResult::Type::NUM, "provider_failures", "Locally observed provider-attributable failures"},
                {RPCResult::Type::NUM, "neutral_failures", "Failures not attributed to the provider"},
                {RPCResult::Type::NUM, "availability_timeouts", "Locally observed availability timeouts"},
                {RPCResult::Type::BOOL, "sufficient_data", "Whether enough observations exist to report a success rate"},
                {RPCResult::Type::NUM, "success_rate_basis_points", /*optional=*/true, "Success rate in basis points when sufficient data exists"},
                {RPCResult::Type::NUM, "latency_ewma_ms", "Locally observed smoothed latency in milliseconds"},
                {RPCResult::Type::NUM, "consecutive_provider_failures", "Consecutive provider-attributable failures"},
                {RPCResult::Type::NUM_TIME, "last_observation_at", "Last local observation time"},
                {RPCResult::Type::NUM_TIME, "cooldown_until", "Local cooldown expiry time, or zero"},
                {RPCResult::Type::BOOL, "cooldown_active", "Whether the local cooldown is active"},
            }},
        }},
        RPCExamples{HelpExampleCli("getpaymasterreputation", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            PaymasterStore store{*wallet};
            std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord> records;
            if (!request.params[0].isNull()) {
                DigiDollar::Paymaster::PaymasterReliabilityRecord record;
                const auto provider_id = ParseHashV(request.params[0], "provider_id");
                if (store.GetProviderReliability(provider_id, record)) records.push_back(std::move(record));
            } else if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.provider_id < rhs.provider_id;
            });
            const int64_t now = GetTime();
            UniValue result{UniValue::VARR};
            for (const auto& record : records)
                result.push_back(ReliabilityToJSON(record, now));
            return result;
        }),
    };
}

RPCHelpMan clearpaymasterreputation()
{
    return RPCHelpMan{
        "clearpaymasterreputation",
        "Clear local aggregate reliability history for one provider or all providers.\n"
        "Minimal per-attempt outcome markers remain to prevent duplicate accounting.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional provider identity; omit to clear all aggregates"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Clear result", {{RPCResult::Type::NUM, "cleared", "Number of aggregate records removed"}}},
        RPCExamples{HelpExampleCli("clearpaymasterreputation", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            PaymasterStore store{*wallet};
            std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord> records;
            if (!request.params[0].isNull()) {
                DigiDollar::Paymaster::PaymasterReliabilityRecord record;
                const auto provider_id = ParseHashV(request.params[0], "provider_id");
                if (store.GetProviderReliability(provider_id, record)) records.push_back(std::move(record));
            } else if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            uint64_t cleared{0};
            std::string error;
            for (const auto& record : records) {
                if (!store.ClearProviderReliability(record.provider_id, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                ++cleared;
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("cleared", cleared);
            return result;
        }),
    };
}

RPCHelpMan getpaymasterpoolinfo()
{
    return RPCHelpMan{
        "getpaymasterpoolinfo",
        "Return the persisted admission and operational Paymaster pool without modifying it.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider pool information", {
            PoolPreparationResultSchema(),
                                                                             {RPCResult::Type::BOOL, "prepared", "Whether a valid pool record exists"},
                                                                             {RPCResult::Type::BOOL, "ready", "Whether admission proof and an operational slot are ready"},
                                                                             {RPCResult::Type::NUM, "entries", "Number of pool entries"},
                                                                             {RPCResult::Type::NUM, "complete_operational_slots", "Complete usable operational slots"},
                                                                             {RPCResult::Type::ARR, "readiness_errors", "Pool readiness errors", {{RPCResult::Type::STR, "", "Stable pool error"}}},
                                                                             {RPCResult::Type::ARR, "pool", "Persisted entries", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Pool entry", {
                                                                                                                                                                                                   {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                   {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                   {RPCResult::Type::STR, "purpose", "admission or operational"},
                                                                                                                                                                                                   {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
                                                                                                                                                                                                   {RPCResult::Type::STR, "state", "Pool entry state"},
                                                                                                                                                                                                   {RPCResult::Type::NUM, "dgb_satoshis", "DGB value"},
                                                                                                                                                                                                   {RPCResult::Type::NUM, "dd_cents", "Carrier value"},
                                                                                                                                                                                                   {RPCResult::Type::NUM, "confirmation_height", "Confirmation height or zero"},
                                                                                                                                                                                                   {RPCResult::Type::STR_HEX, "reservation_id", /*optional=*/true, "Durable reservation"},
                                                                                                                                                                                                   {RPCResult::Type::STR_HEX, "origin_commit_key", /*optional=*/true, "Commit or maintenance operation that created this successor"},
                                                                                                                                                                                                   {RPCResult::Type::NUM_TIME, "updated_at", "Last persistent update"},
                                                                                                                                                                                               }}}},
                                                                         }},
        RPCExamples{HelpExampleCli("getpaymasterpoolinfo", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            std::vector<DigiDollar::Paymaster::ProviderPoolEntry> entries;
            const bool prepared = GetPaymasterProviderPoolEntries(*wallet, entries);
            std::string refresh_error;
            if (prepared && !RefreshPoolConfirmationHeights(*wallet, entries, refresh_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
            }
            DigiDollar::Paymaster::ProviderPolicy policy;
            const bool have_policy = GetPaymasterProviderPolicy(*wallet, policy);
            int32_t tip_height;
            {
                LOCK(wallet->cs_wallet);
                tip_height = wallet->GetLastBlockHeight();
            }
            DigiDollar::Paymaster::ProviderPoolReadiness readiness;
            if (prepared && have_policy) {
                readiness = GetPaymasterProviderPoolReadiness(*wallet, entries, policy, tip_height, 1);
            } else if (!prepared) {
                readiness.errors.push_back("PAYMASTER_POOLS_NOT_PREPARED");
            } else {
                readiness.errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
            }
            UniValue pool{UniValue::VARR};
            for (const auto& entry : entries)
                pool.push_back(PoolEntryToJSON(entry));
            UniValue result{UniValue::VOBJ};
            result.pushKV("prepared", prepared);
            result.pushKV("preparation", PoolPreparationToJSON(*wallet));
            result.pushKV("ready", prepared && have_policy && readiness.ready);
            result.pushKV("entries", static_cast<uint64_t>(entries.size()));
            result.pushKV("complete_operational_slots", static_cast<uint64_t>(readiness.complete_operational_slots));
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            result.pushKV("pool", std::move(pool));
            return result;
        }),
    };
}

RPCHelpMan listpaymasterreservations()
{
    return RPCHelpMan{
        "listpaymasterreservations",
        "List non-available provider pool entries and their durable reservation identifiers.\n",
        {},
        RPCResult{RPCResult::Type::ARR, "", "Provider pool reservations", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reserved, committed, pending-successor, or spent entry", {
                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                        {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                        {RPCResult::Type::STR, "purpose", "admission or operational"},
                                                                                                                                                                                        {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
                                                                                                                                                                                        {RPCResult::Type::STR, "state", "Pool entry state"},
                                                                                                                                                                                        {RPCResult::Type::NUM, "dgb_satoshis", "DGB value"},
                                                                                                                                                                                        {RPCResult::Type::NUM, "dd_cents", "Carrier value"},
                                                                                                                                                                                        {RPCResult::Type::NUM, "confirmation_height", "Confirmation height or zero"},
                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "reservation_id", /*optional=*/true, "Durable reservation"},
                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "origin_commit_key", /*optional=*/true, "Commit or maintenance operation that created this successor"},
                                                                                                                                                                                        {RPCResult::Type::NUM_TIME, "updated_at", "Last persistent update"},
                                                                                                                                                                                    }}}},
        RPCExamples{HelpExampleCli("listpaymasterreservations", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::vector<DigiDollar::Paymaster::ProviderPoolEntry> entries;
            if (!GetPaymasterProviderPoolEntries(*wallet, entries)) return UniValue{UniValue::VARR};
            UniValue result{UniValue::VARR};
            for (const auto& entry : entries) {
                if (entry.state != DigiDollar::Paymaster::PoolEntryState::AVAILABLE) {
                    result.push_back(PoolEntryToJSON(entry));
                }
            }
            return result;
        }),
    };
}

RPCHelpMan cancelpaymasterquote()
{
    return RPCHelpMan{
        "cancelpaymasterquote",
        "Atomically reject an unsigned provider quote and release its operational pool slot.\n"
        "Cancellation is refused once a user signature may exist.\n",
        {
            {"attempt_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Persistent provider attempt identifier"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative canceled quote state", {
                                                                                      {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                      {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                      {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                      {RPCResult::Type::STR_HEX, "attempt_id", "Canceled provider attempt"},
                                                                                      {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Bound quote identifier"},
                                                                                      {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Current provider policy hash"},
                                                                                      {RPCResult::Type::STR, "fee_mode", "Effective fee mode"},
                                                                                      {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Quote expiration time"},
                                                                                      {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                      {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                  }},
        RPCExamples{HelpExampleCli("cancelpaymasterquote", "\"0123...\"")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const uint256 attempt_id = ParseHashV(request.params[0], "attempt_id");
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt existing;
            if (!store.GetAttempt(attempt_id, existing)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_ATTEMPT_NOT_FOUND");
            }
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            if (!GetPaymasterIdentity(*wallet, identity) || identity.provider_id != existing.provider_id) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_IDENTITY_MISMATCH");
            }
            DigiDollar::Paymaster::ProviderAttempt canceled;
            std::string error;
            if (!store.CancelProviderQuote(attempt_id, GetTime(), canceled, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(canceled.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
            }
            DigiDollar::Paymaster::ProviderSettings settings;
            const bool have_settings = GetPaymasterProviderSettings(*wallet, settings);
            UniValue result{UniValue::VOBJ};
            result.pushKV("request_id", session.request_id);
            result.pushKV("session_id", session.session_id.GetHex());
            result.pushKV("provider_id", canceled.provider_id.GetHex());
            result.pushKV("attempt_id", canceled.attempt_id.GetHex());
            if (!canceled.quote_id.IsNull()) result.pushKV("quote_id", canceled.quote_id.GetHex());
            if (have_settings && !settings.policy_hash.IsNull()) result.pushKV("policy_hash", settings.policy_hash.GetHex());
            result.pushKV("fee_mode", FeeModeName(session.fee_mode_used));
            if (canceled.quote_expires_at > 0) result.pushKV("expires_at", canceled.quote_expires_at);
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(session.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(canceled.state)});
            return result;
        }),
    };
}

} // namespace wallet
