// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Client session, PSBT, submit, result, and recovery RPCs. */

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
#include <util/chaintype.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <initializer_list>
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

// Share the durable session schema between status and recovery RPCs.
RPCResult ClientSessionResult(std::string key, bool include_observed_at)
{
    std::vector<RPCResult> fields{
        {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
        {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
        {RPCResult::Type::STR_HEX, "canonical_request_hash", "Hash of the canonical authorized request"},
        {RPCResult::Type::BOOL, "resume_order_available", "Whether the complete original public order was validated; this grants no payment authorization"},
        {RPCResult::Type::OBJ, "resume_order", /*optional=*/true, "Exact original send arguments for explicit resumption; never inferred from current defaults", {
            {RPCResult::Type::STR, "request_id", "Original request UUID"},
            {RPCResult::Type::STR_HEX, "session_id", "Original session"},
            {RPCResult::Type::STR_HEX, "canonical_request_hash", "Existing immutable order hash"},
            {RPCResult::Type::STR, "to_address", "Original recipient"},
            {RPCResult::Type::NUM, "requested_amount_cents", "Original entered amount"},
            {RPCResult::Type::NUM, "maximum_paymaster_fee_cents", "Original fee ceiling"},
            {RPCResult::Type::NUM, "maximum_provider_attempts", "Original attempt limit"},
            {RPCResult::Type::STR, "fee_mode", "Original paymaster or auto mode"},
            {RPCResult::Type::STR, "privacy", "Original privacy profile"},
            {RPCResult::Type::STR, "selection", "Original selection policy"},
            {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", "Original amount semantics"},
            {RPCResult::Type::BOOL, "send_all_spendable_dd", "Original sweep selection"},
            {RPCResult::Type::ARR, "selected_inputs", "Original ordered DD inputs; omit explicit send inputs when resuming a sweep", {{RPCResult::Type::OBJ, "", "Original input", {
                {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                {RPCResult::Type::NUM, "vout", "Output index"},
            }}}},
        }},
        {RPCResult::Type::STR, "resume_order_error", /*optional=*/true, "Why replay metadata is unavailable; payment observations and signed recovery remain usable"},
        {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount; exact total outflow in subtract mode"},
        {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the service fee is deducted from requested_amount_cents"},
        {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the session is bound to all ordinary spendable DD"},
        {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
        {RPCResult::Type::STR, "fee_mode_used", "Persisted effective fee mode"},
        {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider bound by the latest durable client authorization"},
        {RPCResult::Type::STR, "privacy_profile", /*optional=*/true, "standard or high privacy profile of the latest durable attempt"},
        {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Offer bound by the latest durable client authorization"},
        {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Provider policy bound by the latest durable client authorization"},
        {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
        {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Exact recipient amount from the latest durable client authorization"},
        {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact rounded provider service fee"},
        {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Exact recipient amount plus service fee"},
        {RPCResult::Type::STR, "to_address", /*optional=*/true, "Canonical DigiDollar recipient from the bound persisted intent or durable authorization"},
        {RPCResult::Type::STR, "session_state", "Authoritative session state"},
        {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Persisted phase for PENDING_PROVIDER"},
        {RPCResult::Type::BOOL, "final", "Whether the state is terminal"},
        {RPCResult::Type::STR, "status", "success, canceled, failed, conflicted, or pending; success requires a locally confirmed recipient payment"},
        {RPCResult::Type::BOOL, "payment_confirmed", "Validated recipient payment has positive local confirmation depth"},
        PaymasterPaymentViewResult(),
        {RPCResult::Type::STR, "broadcast_state", "not_attempted, unknown, accepted_mempool, accepted_stempool, or confirmed"},
        {RPCResult::Type::STR, "confirmation_state", "unconfirmed, payment_confirmed, recovery_confirmed, or conflicted"},
        {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
        {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction id"},
        {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known idempotent self-recovery transaction id"},
        {RPCResult::Type::ARR, "reserved_user_inputs", "Immutable user input set", {
            {RPCResult::Type::OBJ, "", "A reserved user input", {
                {RPCResult::Type::STR_HEX, "txid", "Creating transaction id"},
                {RPCResult::Type::NUM, "vout", "Output index"},
            }},
        }},
        {RPCResult::Type::NUM, "provider_attempts", "Number of persistent provider attempts"},
    };
    if (include_observed_at) {
        fields.emplace_back(RPCResult::Type::NUM_TIME, "observed_at", "Time the current action capabilities were evaluated");
    }
    return RPCResult{RPCResult::Type::OBJ, std::move(key), "The authoritative persistent session", std::move(fields)};
}

} // namespace

RPCHelpMan getpaymasterclientinfo()
{
    return RPCHelpMan{
        "getpaymasterclientinfo",
        "Return wallet-scoped Paymaster capabilities and current local prerequisites.\n"
        "Read-only: no reservations, signatures, provider requests, or financial reconciliation.\n"
        "Readiness does not guarantee funds, provider connectivity, or a quote.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Client integration information", {
            {RPCResult::Type::NUM, "integration_version", "RPC contract version, independent of wire and wallet record versions"},
            {RPCResult::Type::STR, "network", "Node network name"},
            {RPCResult::Type::STR_HEX, "genesis_hash", "Full network genesis hash"},
            {RPCResult::Type::STR, "amount_unit", "cents"},
            {RPCResult::Type::NUM, "minimum_payment_cents", "Minimum recipient output"},
            {RPCResult::Type::NUM, "maximum_payment_cents", "Maximum recipient output; fees can further constrain the order"},
            {RPCResult::Type::NUM, "maximum_user_total_cents", "Upper bound on recipient amount plus service fee"},
            {RPCResult::Type::ARR, "fee_modes", "Supported senddigidollar fee modes", {{RPCResult::Type::STR, "", "Mode"}}},
            {RPCResult::Type::ARR, "funding_models", "Supported Paymaster funding models", {{RPCResult::Type::STR, "", "Model"}}},
            {RPCResult::Type::ARR, "sponsorship_scopes", "Supported sponsorship scopes", {{RPCResult::Type::STR, "", "Scope"}}},
            PaymasterTransportResult(),
            {RPCResult::Type::BOOL, "supported", "This binary implements the client integration contract"},
            {RPCResult::Type::BOOL, "ready", "Local prerequisites for authorization currently met"},
            {RPCResult::Type::ARR, "readiness_errors", "Local blockers; preparation and session reads may still work", {{RPCResult::Type::STR, "", "Stable error identifier"}}},
        }},
        RPCExamples{HelpExampleCli("getpaymasterclientinfo", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            WalletContext& context = EnsureWalletContext(request.context);
            UniValue errors{UniValue::VARR};
            if (!context.paymaster || !context.paymaster->Enabled()) errors.push_back("PAYMASTER_DISABLED");
            if (!context.args || context.args->GetIntArg("-prune", 0) != 0) errors.push_back("PAYMASTER_REQUIRES_PRUNE_0");
            if (!context.args || !context.args->GetBoolArg("-txindex", DEFAULT_TXINDEX)) errors.push_back("PAYMASTER_REQUIRES_TXINDEX");
            if (context.args && Params().GetChainType() == ChainType::MAIN &&
                context.args->GetBoolArg("-capturemessages", false)) errors.push_back("PAYMASTER_MESSAGE_CAPTURE_ENABLED");
            const auto* node = wallet->chain().context();
            if (!node || !node->connman || !(node->connman->GetLocalServices() & NODE_P2P_V2)) errors.push_back("PAYMASTER_REQUIRES_V2_TRANSPORT");
            if (!context.chain || context.chain->isInitialBlockDownload() ||
                !context.chain->isReadyToBroadcast()) errors.push_back("PAYMASTER_NODE_NOT_READY");
            const auto chain_height = wallet->chain().getHeight();
            const auto index = g_txindex ? g_txindex->GetSummary() : IndexSummary{};
            {
                LOCK(wallet->cs_wallet);
                if (wallet->IsLocked()) errors.push_back("PAYMASTER_WALLET_LOCKED");
                if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) errors.push_back("PAYMASTER_PRIVATE_KEYS_REQUIRED");
                if (!chain_height || wallet->GetLastBlockHeight() != *chain_height) errors.push_back("PAYMASTER_WALLET_NOT_SYNCED");
                if (wallet->GetLastBlockHeight() + 1 < Params().GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR)) errors.push_back("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE");
                if (!index.synced || index.best_block_hash != wallet->GetLastBlockHash()) errors.push_back("PAYMASTER_REQUIRES_READY_TXINDEX");
                ClientSafetyPolicy policy;
                std::string error;
                if (!GetPaymasterClientSafetyPolicy(*wallet, policy)) errors.push_back("PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED");
                else if (!ValidateClientSafetyPolicy(policy, error)) errors.push_back(error);
                ClientFeeLedger ledger;
                if (!GetPaymasterClientFeeLedger(*wallet, ledger)) errors.push_back("PAYMASTER_CLIENT_FEE_LEDGER_REQUIRED");
                else if (!ValidateClientFeeLedger(ledger, error)) errors.push_back(error);
            }
            const auto strings = [](std::initializer_list<const char*> values) {
                UniValue array{UniValue::VARR};
                for (const auto* value : values) array.push_back(value);
                return array;
            };
            UniValue result{UniValue::VOBJ};
            if (node && node->connman) {
                result.pushKV("transport", PaymasterTransportToJSON(*node->connman));
                if (!node->connman->GetPaymasterBudget().outbound) errors.push_back("PAYMASTER_NO_LOCAL_DIRECT_CAPACITY");
            }
            result.pushKV("integration_version", 1);
            result.pushKV("network", ChainTypeToString(Params().GetChainType()));
            result.pushKV("genesis_hash", Params().GenesisBlock().GetHash().GetHex());
            result.pushKV("amount_unit", "cents");
            result.pushKV("minimum_payment_cents", 100);
            result.pushKV("maximum_payment_cents", MAX_DD_OUTPUT_CENTS);
            result.pushKV("maximum_user_total_cents", MAX_DD_OUTPUT_CENTS);
            result.pushKV("fee_modes", strings({"dgb", "paymaster", "auto"}));
            result.pushKV("funding_models", strings({"user_paid", "sponsored"}));
            result.pushKV("sponsorship_scopes", strings({"public"}));
            result.pushKV("supported", true);
            result.pushKV("ready", errors.empty());
            result.pushKV("readiness_errors", std::move(errors));
            return result;
        }),
    };
}

RPCHelpMan getdigidollarsendsession()
{
    return RPCHelpMan{
        "getdigidollarsendsession",
        "Return one persistent DigiDollar send session without creating a new payment attempt.\n"
        "PAYMASTER_SESSION_NOT_FOUND means neither a session nor its retained completion record was found. "
        "PAYMASTER_SESSION_READ_FAILED and PAYMASTER_UNSUPPORTED_PERSISTED_VERSION never prove absence.\n",
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent 256-bit session identifier"},
                                                                                                             }},
        },
        ClientSessionResult("", /*include_observed_at=*/true),
        RPCExamples{HelpExampleCli("getdigidollarsendsession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            wallet->BlockUntilSyncedToCurrentChain();
            LOCK(wallet->cs_wallet);
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND",
                    "PAYMASTER_SESSION_NOT_FOUND: Paymaster session not found", ErrorDisposition::RECONCILE, true);
            }
            UniValue result = SessionToJSON(session, &store);
            result.pushKV("observed_at", GetTime());
            return result;
        }),
    };
}

RPCHelpMan listdigidollarsendsessions()
{
    return RPCHelpMan{
        "listdigidollarsendsessions",
        "List bounded wallet-local Paymaster client session summaries without "
        "creating, signing, retrying, broadcasting, or recovering a payment.\n"
        "The cursor is the exclusive request_id of the previous page. "
        "active_only retains conflicted and otherwise terminal sessions whenever "
        "durable authorization evidence may still require attention.\n",
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Read-only pagination options", {
                                                                                                           {"active_only", RPCArg::Type::BOOL, RPCArg::Default{true}, "Exclude completed and harmless unsigned-failed sessions"},
                                                                                                           {"limit", RPCArg::Type::NUM, RPCArg::Default{20}, "Maximum summaries to return, from 1 through 100"},
                                                                                                           {"cursor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Exclusive canonical request_id cursor"},
                                                                                                       }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "A bounded read-only session page", {
                                                                                       {RPCResult::Type::BOOL, "active_only", "Applied active-session filter"},
                                                                                       {RPCResult::Type::NUM, "count", "Number of summaries in this page"},
                                                                                       {RPCResult::Type::ARR, "sessions", "Wallet-local client session summaries", {
                                                                                                                                                                         {RPCResult::Type::OBJ, "", "One persistent session summary", {
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "fee_mode_used", "Effective fee mode"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "status", "success, canceled, failed, conflicted, or pending"},
                                                                                                                                                                                                                                        {RPCResult::Type::BOOL, "payment_confirmed", "Validated recipient payment is locally confirmed"},
                                                                                                                                                                                                                                        {RPCResult::Type::BOOL, "final", "Whether the persisted session state is terminal"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "broadcast_state", "Last known broadcast state"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "confirmation_state", "Locally observed confirmation state"},
                                                                                                                                                                                                                                        PaymasterPaymentViewResult(),
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Pending provider phase"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
        {RPCResult::Type::NUM_TIME, "observed_at", "Time the current action capabilities were evaluated"},
                                                                                                                                                                                                                                        {RPCResult::Type::NUM, "provider_attempts", "Persistent provider attempt count"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known payment transaction"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known recovery transaction"},
                                                                                                                                                                                                                                        {RPCResult::Type::STR, "artifact", "none, user_psbt, final_transaction, or alternative_recovery"},
                                                                                                                                                                                                                                        {RPCResult::Type::BOOL, "requires_attention", "Whether durable state requires explicit user attention"},
                                                                                                                                                                                                                                        {RPCResult::Type::ARR, "allowed_actions", "Core-derived actions; Qt may only remove entries", {{RPCResult::Type::STR, "", "Action name"}}},
                                                                                                                                                                                                                                    }},
                                                                                                                                                                     }},
                                                                                       {RPCResult::Type::STR, "next_cursor", /*optional=*/true, "Exclusive cursor for the next page"},
                                                                                   }},
        RPCExamples{HelpExampleCli("listdigidollarsendsessions", "'{\"active_only\":true,\"limit\":20}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            const UniValue options = request.params[0].isNull()
                ? UniValue{UniValue::VOBJ} : request.params[0].get_obj();
            RPCTypeCheckObj(options,
                            {{"active_only", UniValueType(UniValue::VBOOL)},
                             {"limit", UniValueType(UniValue::VNUM)},
                             {"cursor", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const bool active_only = options.find_value("active_only").isNull()
                ? true : options.find_value("active_only").get_bool();
            const int64_t limit = options.find_value("limit").isNull()
                ? 20 : options.find_value("limit").getInt<int64_t>();
            const std::string cursor = options.find_value("cursor").isNull()
                ? std::string{} : options.find_value("cursor").get_str();
            if (limit < 1 || limit > 100) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "limit must be between 1 and 100");
            }
            if (!cursor.empty() && !IsCanonicalRequestId(cursor)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "cursor must be a canonical lowercase request UUID");
            }

            wallet->BlockUntilSyncedToCurrentChain();
            // Keep the live-session list and each derived attempt/recovery
            // view on one wallet database snapshot. PaymasterStore uses the
            // same recursive wallet lock internally.
            LOCK(wallet->cs_wallet);
            PaymasterStore store{*wallet};
            std::vector<PaymentSession> sessions;
            std::string error;
            if (!store.ListClientSessions(sessions, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            std::sort(sessions.begin(), sessions.end(),
                      [](const PaymentSession& lhs,
                         const PaymentSession& rhs) {
                          return lhs.request_id < rhs.request_id;
                      });

            struct ListedSession {
                PaymentSession session;
                ClientSessionView view;
            };
            std::vector<ListedSession> listed;
            listed.reserve(static_cast<size_t>(limit) + 1);
            for (PaymentSession& session : sessions) {
                if (!cursor.empty() && session.request_id <= cursor) {
                    continue;
                }
                if (active_only &&
                    (session.state == SessionState::CONFIRMED ||
                     session.state == SessionState::CANCELED_SAFE)) {
                    continue;
                }
                ClientSessionView view;
                if (!LoadClientSessionView(
                        store, session, GetTime(), view, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                if (active_only && !IsActiveClientSession(session, view)) {
                    continue;
                }
                listed.push_back({std::move(session), std::move(view)});
                if (listed.size() > static_cast<size_t>(limit)) break;
            }

            const size_t page_size = std::min(
                listed.size(), static_cast<size_t>(limit));
            UniValue page{UniValue::VARR};
            for (size_t index = 0; index < page_size; ++index) {
                page.push_back(ClientSessionSummaryToJSON(
                    store, listed[index].session, listed[index].view));
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("active_only", active_only);
            result.pushKV("count", static_cast<uint64_t>(page_size));
            result.pushKV("sessions", std::move(page));
            if (page_size < listed.size() && page_size > 0) {
                result.pushKV("next_cursor",
                              listed[page_size - 1].session.request_id);
            }
            return result;
        }),
    };
}

RPCHelpMan resolvepaymastersession()
{
    // RPCResult cannot express nullable fields. Use the existing per-field
    // escape hatch (as getwalletinfo does for scanning) only for the four
    // object/string/number-or-null fields; retain their documented schemas.
    // Functional tests check both absent and persisted session artifacts.
    return RPCHelpMan{
        "resolvepaymastersession",
        "Inspect or recover an existing Paymaster session.\n"
        "refresh may persist already-received provider equivocation evidence, but creates no payment, signature or reservation. "
        "Use getdigidollarsendsession for a strictly read-only payment observation.\n"
        "PAYMASTER_SESSION_NOT_FOUND means no session or retained completion was found; "
        "PAYMASTER_SESSION_READ_FAILED and PAYMASTER_UNSUPPORTED_PERSISTED_VERSION leave status unknown.\n"
        "retry_same never creates a quote, attempt, reservation, or signature. "
        "fallback is allowed only before a user PSBT exists. abandon_unsigned "
        "atomically releases a session for which no transaction authorization can exist. "
        "cancel_to_self durably creates or resumes one exact same-input recovery transaction.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent session identifier"},
                                                                                                             }},
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self; mutations must be present in the current Core-derived allowed_actions"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Exact recovery selection and authorization", {
                                                                                                                        {"attempt_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Existing original attempt identifier"},
                                                                                                                        {"recovery_provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact distinct recovery provider"},
                                                                                                                        {"recovery_offer_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact USER_PAID recovery offer"},
                                                                                                                        {"maximum_recovery_service_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Local recovery service-fee ceiling"},
                                                                                                                        {"prepare_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stop after validating and persisting the exact authorization commitment"},
                                                                                                                        {"retry_transport", RPCArg::Type::BOOL, RPCArg::Default{false}, "Explicitly retry failed recovery transport without renewing financial authority"},
                                                                                                                        {"recovery_authorization_commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Exact commitment required before signing user inputs"},
                                                                                                                    }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative local recovery state", {
                                                                                      {RPCResult::Type::STR, "action", "Performed local action"},
                                                                                      ClientSessionResult("session", /*include_observed_at=*/false),
                                                                                       {RPCResult::Type::OBJ, "attempt", /*optional=*/false, "Selected persistent attempt, or null when none exists", {
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "attempt_id", "Persistent attempt identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "provider_id", "Provider identifier"},
                                                                                                                                                                              {RPCResult::Type::STR, "provider_endpoint", /*optional=*/true, "Direct provider endpoint"},
                                                                                                                                                                              {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                                                                                                              {RPCResult::Type::STR, "privacy_profile", "standard or high privacy profile"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Provider quote identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Bound transaction template"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "unsigned_txid", /*optional=*/true, "Unsigned transaction identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Provider commit key"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "quote_expires_at", /*optional=*/true, "Quote expiration time"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "retry_until", /*optional=*/true, "Exact retry deadline"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact client authorization"},
                                                                                                                                                                              {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                          }, /*skip_type_check=*/true},
                                                                                       {RPCResult::Type::STR, "artifact", "none, user_psbt, final_transaction, or alternative_recovery"},
                                                                                       {RPCResult::Type::BOOL, "requires_attention", "Whether durable state requires explicit user attention"},
                                                                                       {RPCResult::Type::ARR, "allowed_actions", "Core-derived actions returned for every resolve action; Qt may only remove entries", {{RPCResult::Type::STR, "", "Action name"}}},
                                                                                       {RPCResult::Type::NUM_TIME, "observed_at", "Time the current action capabilities were evaluated"},
                                                                                       {RPCResult::Type::OBJ, "recovery", /*optional=*/false, "Persistent alternative-recovery authority and state, or null when none exists", {
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "recovery_id", "Stable recovery identifier"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "recovery_provider_id", "Distinct recovery provider"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "offer_id", "Exact USER_PAID offer"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "policy_hash", "Exact provider policy"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "original_commit_key", "Ambiguous original provider commit"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "original_template_commitment", "Ambiguous original transaction template"},
                                                                                                                                                                                                       {RPCResult::Type::STR, "privacy_profile", "Inherited standard or high privacy profile"},
                                                                                                                                                                                                       {RPCResult::Type::STR, "phase", "capacity_pending, request_ready, response_validated, user_signed, or final_committed"},
                                                                                                                                                                                                       {RPCResult::Type::BOOL, "expired", "Whether a safely expirable unsigned recovery expired"},
                                                                                                                                                                                                       {RPCResult::Type::ARR, "user_dd_inputs", "Canonical original user inputs", {{RPCResult::Type::OBJ, "", "User DD input", {
                                                                                                                                                                                                                                                                                                                                   {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                                                                                                                   {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                                                                                               }}}},
                                                                                                                                                                                                       {RPCResult::Type::ARR, "wallet_returns", "Canonical wallet-owned recovery outputs", {{RPCResult::Type::OBJ, "", "Wallet return", {
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::STR_HEX, "script_pub_key", "Exact return script"},
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::STR, "address", /*optional=*/true, "Display address when decodable"},
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::NUM, "amount_cents", "Exact DD amount"},
                                                                                                                                                                                                                                                                                                                                        }}}},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Validated Capacity-v5 snapshot"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "capacity_resource_commitment", /*optional=*/true, "Exact capacity resources"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact recovery authorization"},
                                                                                                                                                                                                       {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                                                       {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "maximum_service_fee_cents", "Effective local fee ceiling"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "service_fee_cents", "Exact recovery service fee"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "Exact provider-paid network fee"},
                                                                                                                                                                                                       {RPCResult::Type::NUM_TIME, "expires_at", "Recovery authorization expiry"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact final recovery transaction"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final recovery txid"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "wtxid", /*optional=*/true, "Final recovery wtxid"},
                                                                                                                                                                                                   }, /*skip_type_check=*/true},
                                                                                      {RPCResult::Type::STR, "psbt", /*optional=*/true, "Exact persisted user-signed PSBT"},
                                                                                      {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact durably committed transaction"},
                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Committed transaction id"},
                                                                                      {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Idempotent self-recovery transaction id"},
                                                                                      {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether local broadcast succeeded"},
                                                                                      {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Local broadcast error; exact retry remains available"},
                                                                                      {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the exact persisted submit is queued"},
                                                                                      {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a dedicated provider reconnection was requested"},
                                                                                      {RPCResult::Type::BOOL, "route_available", /*optional=*/true, "Whether the persisted provider route is available"},
                                                                                       {RPCResult::Type::STR, "result_status", /*optional=*/false, "Latest signed provider result, or null when none exists", {}, /*skip_type_check=*/true},
                                                                                       {RPCResult::Type::NUM, "result_sequence", /*optional=*/false, "Latest monotonic result sequence, or null when none exists", {}, /*skip_type_check=*/true},
                                                                                  }},
        RPCExamples{HelpExampleCli("resolvepaymastersession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}' retry_same")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const std::string action = request.params[1].get_str();
            if (action != "refresh" && action != "retry_same" && action != "fallback" &&
                action != "abandon_unsigned" && action != "cancel_to_self") {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "action must be refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self");
            }

            UniValue options{UniValue::VOBJ};
            if (!request.params[2].isNull()) options = request.params[2].get_obj();
            RPCTypeCheckObj(options,
                            {{"attempt_id", UniValueType(UniValue::VSTR)},
                             {"recovery_provider_id", UniValueType(UniValue::VSTR)},
                             {"recovery_offer_id", UniValueType(UniValue::VSTR)},
                             {"maximum_recovery_service_fee_cents", UniValueType(UniValue::VNUM)},
                             {"prepare_only", UniValueType(UniValue::VBOOL)},
                             {"retry_transport", UniValueType(UniValue::VBOOL)},
                             {"recovery_authorization_commitment", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);

            ClientRecoveryOptions parsed;
            if (!options.find_value("attempt_id").isNull()) parsed.attempt_id = ParseHashO(options, "attempt_id");
            if (!options.find_value("recovery_provider_id").isNull()) parsed.provider_id = ParseHashO(options, "recovery_provider_id");
            if (!options.find_value("recovery_offer_id").isNull()) parsed.offer_id = ParseHashO(options, "recovery_offer_id");
            if (!options.find_value("maximum_recovery_service_fee_cents").isNull()) parsed.maximum_fee = options.find_value("maximum_recovery_service_fee_cents").getInt<int64_t>();
            if (!options.find_value("recovery_authorization_commitment").isNull()) parsed.authorization_commitment = ParseHashO(options, "recovery_authorization_commitment");
            parsed.prepare_only = options.find_value("prepare_only").isTrue();
            parsed.retry_transport = options.find_value("retry_transport").isTrue();
            return ToJSON(ResolvePaymasterSession(context, *wallet, ParseClientSessionLookup(request.params[0].get_obj()), action, parsed));
        }),
    };
}

RPCHelpMan walletprocesspaymasterpsbt()
{
    return RPCHelpMan{
        "walletprocesspaymasterpsbt",
        "Validate an unsigned Paymaster PSBT against its exact persistent wallet template and sign only USER inputs.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded unsigned Paymaster PSBT"},
            {"authorization_commitment", RPCArg::Type::STR_HEX,
             RPCArg::Optional::OMITTED,
             "Exact client authorization commitment. Required before a new USER signature; omitted only for an exact already-signed retry"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The durable user authorization artifact", {
                                                                                           {RPCResult::Type::STR, "psbt", "Canonical base64-encoded user-signed PSBT"},
                                                                                           {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                           {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                           {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                                           {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                                           {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Accepted client authorization commitment"},
                                                                                           {RPCResult::Type::BOOL, "authorization_accepted", "Whether the exact commitment is durably accepted"},
                                                                                           {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Monotonic durable acceptance time"},
                                                                                           {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                           {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                           {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction"},
                                                                                           {RPCResult::Type::NUM_TIME, "expires_at", "Provider retry deadline"},
                                                                                           {RPCResult::Type::BOOL, "queued", "Whether PMSUBMIT is queued to a connected v2 peer"},
                                                                                           {RPCResult::Type::BOOL, "connection_pending", "Whether reconnecting to the provider was requested"},
                                                                                           {RPCResult::Type::BOOL, "route_available", "Whether a current provider announcement supplies the retry route"},
                                                                                       }},
        RPCExamples{HelpExampleCli("walletprocesspaymasterpsbt", "\"cHNidP8...\" \"0123...\"")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::string readiness_error;
            if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }

            PartiallySignedTransaction candidate;
            std::string error;
            if (!DecodeBase64PSBT(candidate, request.params[0].get_str(), error) || !candidate.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   strprintf("Paymaster PSBT decode failed: %s", error));
            }
            std::optional<uint256> supplied_authorization_commitment;
            if (request.params.size() > 1 && !request.params[1].isNull()) {
                supplied_authorization_commitment =
                    ParseHashV(request.params[1], "authorization_commitment");
            }
            return ToJSON(AuthorizePaymasterPSBT(context, *wallet, std::move(candidate), supplied_authorization_commitment));
        }),
    };
}

RPCHelpMan submitpaymasterdigidollar()
{
    return RPCHelpMan{
        "submitpaymasterdigidollar",
        "Validate a user-signed Paymaster PSBT, sign only PROVIDER inputs, durably commit the final transaction, then broadcast it.\n"
        "An already committed transaction is inserted and broadcast idempotently without requiring another provider signature.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded user-signed Paymaster PSBT"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The durable provider commit", {
                                                                               {RPCResult::Type::STR, "psbt", /*optional=*/true, "Canonical fully signed PSBT when produced by this call"},
                                                                               {RPCResult::Type::STR_HEX, "hex", "Exact durably committed final transaction"},
                                                                               {RPCResult::Type::STR_HEX, "txid", "Final transaction identifier"},
                                                                               {RPCResult::Type::BOOL, "broadcast", "Whether the committed transaction is in a local pool or already confirmed"},
                                                                               {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable recovery error when the durable commit could not yet be broadcast"},
                                                                               {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                               {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                               {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                               {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                               {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                               {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                               {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                               {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                               {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                               {RPCResult::Type::STR, "result_status", "Signed provider result status"},
                                                                               {RPCResult::Type::NUM, "result_sequence", "Monotonic result sequence"},
                                                                               {RPCResult::Type::NUM_TIME, "expires_at", "Exact retry deadline"},
                                                                           }},
        RPCExamples{HelpExampleCli("submitpaymasterdigidollar", "\"cHNidP8...\"")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            PartiallySignedTransaction psbt;
            std::string error;
            if (!DecodeBase64PSBT(psbt, request.params[0].get_str(), error) || !psbt.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("Paymaster PSBT decode failed: %s", error));
            }
            return ToJSON(SubmitPaymasterTransaction(context, *wallet, std::move(psbt), ExecutionMode::MANUAL));
        }),
    };
}

} // namespace wallet
