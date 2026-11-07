// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider identity, policy, liquidity, finance, and operator RPCs. */

#include <wallet/rpc/paymaster.h>
#include <wallet/paymasterproviderops.h>
#include <wallet/rpc/paymaster_internal.h>
#include <paymaster/finance.h>

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
#include <paymaster/setup.h>
#include <paymaster/validation.h>
#include <random.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <streams.h>
#include <util/chaintype.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/fees.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>

#include <univalue.h>
#include <util/overflow.h>
#include <util/moneystr.h>
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
PoolTargets ParsePoolTargets(const UniValue& options)
{
    PoolTargets target;
    target.admission = options.find_value("admission_dgb_slots").getInt<int>();
    target.operational = options.find_value("operational_dgb_slots").getInt<int>();
    if (!options.find_value("admission_carrier_slots").isNull()) target.admission_carriers = options.find_value("admission_carrier_slots").getInt<int>();
    if (!options.find_value("operational_carrier_slots").isNull()) target.operational_carriers = options.find_value("operational_carrier_slots").getInt<int>();
    target.execute = options.find_value("execute").isTrue();
    if (!options.find_value("plan_id").isNull()) target.plan_id = ParseHashV(options.find_value("plan_id"), "plan_id");
    return target;
}

} // namespace

RPCHelpMan createpaymasteridentity()
{
    return RPCHelpMan{
        "createpaymasteridentity",
        "Create or return this descriptor wallet's persistent BIP86/x-only Paymaster identity.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"display_name", RPCArg::Type::STR, RPCArg::Optional::NO, "Printable provider name, at most 32 bytes"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent public provider identity", {
                                                                                       {RPCResult::Type::STR_HEX, "provider_id", "Tagged hash of the identity key"},
                                                                                       {RPCResult::Type::STR_HEX, "identity_key", "Untweaked BIP86 x-only public key"},
                                                                                       {RPCResult::Type::STR, "display_name", "Persisted display name"},
                                                                                       {RPCResult::Type::NUM_TIME, "created_at", "Identity creation time"},
                                                                                   }},
        RPCExamples{HelpExampleCli("createpaymasteridentity", "\"Community Provider\"")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            return ToJSON(CreateProviderIdentity(*wallet, request.params[0].get_str()));
        }),
    };
}

RPCHelpMan setpaymasterpolicy()
{
    return RPCHelpMan{
        "setpaymasterpolicy",
        "Validate and atomically persist this wallet's Paymaster operating policy. "
        "The provider must be stopped so it cannot accept new work under a previously announced policy while the wallet commit changes; the next start publishes a replacement announcement.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete provider policy", {
                                                                                                {"funding_models", RPCArg::Type::ARR, RPCArg::Optional::NO, "Non-empty funding model allowlist", {{"model", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "sponsored or user_paid"}}},
                                                                                                {"sponsorship_scope", RPCArg::Type::STR, RPCArg::Optional::NO, "public or restricted"},
                                                                                                {"fee_rate_bps", RPCArg::Type::NUM, RPCArg::Optional::NO, "User-paid rate in basis points"},
                                                                                                {"maximum_user_paid_service_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Maximum user-paid DD service fee; zero means no cap"},
                                                                                                {"min_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Minimum recipient amount"},
                                                                                                {"max_amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum recipient amount"},
                                                                                                {"quote_ttl", RPCArg::Type::NUM, RPCArg::Optional::NO, "Quote lifetime in seconds"},
                                                                                                {"maximum_network_fee_dgb_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Positive absolute provider network-fee cap"},
                                                                                            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", /*optional=*/false, "Persisted canonical policy", {
                                                                                                  {RPCResult::Type::ARR, "funding_models", "Enabled funding models", {{RPCResult::Type::STR, "", "sponsored or user_paid"}}},
                                                                                                  {RPCResult::Type::STR, "sponsorship_scope", "public or restricted"},
                                                                                                  {RPCResult::Type::NUM, "fee_rate_bps", "User-paid rate in basis points"},
                                                                                                  {RPCResult::Type::NUM, "maximum_user_paid_service_fee_cents", "Maximum user-paid DD service fee; zero means no cap"},
                                                                                                  {RPCResult::Type::NUM, "min_amount_cents", "Minimum recipient amount"},
                                                                                                  {RPCResult::Type::NUM, "max_amount_cents", "Maximum recipient amount"},
                                                                                                  {RPCResult::Type::NUM, "quote_ttl", "Quote lifetime in seconds"},
                                                                                                  {RPCResult::Type::NUM, "maximum_network_fee_dgb_satoshis", "Absolute provider network-fee cap"},
                                                                                                  {RPCResult::Type::STR_HEX, "policy_hash", "Canonical provider policy hash"},
                                                                                              }},
        RPCExamples{HelpExampleCli("setpaymasterpolicy", "'{\"funding_models\":[\"sponsored\",\"user_paid\"],\"sponsorship_scope\":\"public\",\"fee_rate_bps\":50,\"maximum_user_paid_service_fee_cents\":100,\"min_amount_cents\":100,\"max_amount_cents\":100000,\"quote_ttl\":60,\"maximum_network_fee_dgb_satoshis\":20000000}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            return ToJSON(SaveProviderPolicy(context, *wallet, ParseProviderPolicy(request.params[0].get_obj())));
        }),
    };
}

RPCHelpMan setpaymastersafetypolicy()
{
    return RPCHelpMan{
        "setpaymastersafetypolicy",
        "Persist finite wallet-local Paymaster provider loss and rate ceilings. "
        "All six values for an advertised funding model must be positive. "
        "All-zero limits disable only a model that is not advertised.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete local provider safety policy", {
                                                                                                             {"user_paid", RPCArg::Type::OBJ, RPCArg::Optional::NO, "USER_PAID limits", FundingSafetyLimitArgs()},
                                                                                                             {"public_sponsored", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Public SPONSORED limits", FundingSafetyLimitArgs()},
                                                                                                             {"restricted_sponsored", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Restricted SPONSORED limits", FundingSafetyLimitArgs()},
                                                                                                             {"maximum_active_quotes_total", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum active quotes across all peers"},
                                                                                                             {"maximum_active_quotes_per_netgroup", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum active quotes from one network group"},
                                                                                                             {"maximum_active_quotes_per_recipient", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum active quotes for one pseudonymous recipient bucket"},
                                                                                                             {"maximum_quote_requests_per_netgroup_per_minute", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum quote requests per network group per minute"},
                                                                                                         }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persisted wallet-local safety policy", ProviderSafetyPolicyResults()},
        RPCExamples{HelpExampleCli("setpaymastersafetypolicy", "'{\"user_paid\":{\"maximum_network_fee_per_transaction_satoshis\":1000000,\"maximum_reserved_network_fee_satoshis\":5000000,\"maximum_network_fee_per_hour_satoshis\":10000000,\"maximum_network_fee_per_day_satoshis\":50000000,\"maximum_completed_per_hour\":10,\"maximum_completed_per_day\":100},\"public_sponsored\":{\"maximum_network_fee_per_transaction_satoshis\":0,\"maximum_reserved_network_fee_satoshis\":0,\"maximum_network_fee_per_hour_satoshis\":0,\"maximum_network_fee_per_day_satoshis\":0,\"maximum_completed_per_hour\":0,\"maximum_completed_per_day\":0},\"restricted_sponsored\":{\"maximum_network_fee_per_transaction_satoshis\":0,\"maximum_reserved_network_fee_satoshis\":0,\"maximum_network_fee_per_hour_satoshis\":0,\"maximum_network_fee_per_day_satoshis\":0,\"maximum_completed_per_hour\":0,\"maximum_completed_per_day\":0},\"maximum_active_quotes_total\":16,\"maximum_active_quotes_per_netgroup\":4,\"maximum_active_quotes_per_recipient\":2,\"maximum_quote_requests_per_netgroup_per_minute\":10}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            return ToJSON(SaveProviderSafetyPolicy(context, *wallet, ParseProviderSafetyPolicy(request.params[0].get_obj())));
        }),
    };
}

RPCHelpMan getpaymastersafetystatus()
{
    return RPCHelpMan{
        "getpaymastersafetystatus",
        "Return the provider's local safety policy and durable fee-budget usage.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider safety status", {
                                                                          {RPCResult::Type::BOOL, "configured", "Whether both valid policy and ledger exist"},
                                                                          {RPCResult::Type::BOOL, "ledger_present", "Whether a valid durable ledger exists"},
                                                                          {RPCResult::Type::OBJ, "policy", /*optional=*/true, "Wallet-local provider safety policy", ProviderSafetyPolicyResults()},
                                                                          {RPCResult::Type::NUM_TIME, "accounting_time_high_water", /*optional=*/true, "Monotonic accounting time persisted against clock rollback"},
                                                                          {RPCResult::Type::OBJ, "user_paid", /*optional=*/true, "USER_PAID rolling counters", ProviderSafetyClassStatusResults()},
                                                                          {RPCResult::Type::OBJ, "public_sponsored", /*optional=*/true, "Public SPONSORED rolling counters", ProviderSafetyClassStatusResults()},
                                                                          {RPCResult::Type::OBJ, "restricted_sponsored", /*optional=*/true, "Restricted SPONSORED rolling counters", ProviderSafetyClassStatusResults()},
                                                                      }},
        RPCExamples{HelpExampleCli("getpaymastersafetystatus", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            {
                PaymasterStore store{*wallet};
                size_t expired_quotes{0};
                std::string expiry_error;
                if (!store.ExpireProviderQuotes(GetTime(), expired_quotes,
                                                expiry_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        expiry_error.empty() ? "PAYMASTER_QUOTE_EXPIRY_FAILED" : expiry_error);
                }
                size_t expired_capacity_reservations{0};
                expiry_error.clear();
                if (!store.ExpireProviderCapacityReservations(
                        GetTime(), expired_capacity_reservations,
                        expiry_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        expiry_error.empty() ? "PAYMASTER_CAPACITY_EXPIRY_FAILED" : expiry_error);
                }
            }
            DigiDollar::Paymaster::ProviderSafetyPolicy policy;
            DigiDollar::Paymaster::ProviderBudgetLedger ledger;
            const bool have_policy = GetPaymasterProviderSafetyPolicy(*wallet, policy);
            const bool have_ledger = GetPaymasterProviderBudgetLedger(*wallet, ledger);
            UniValue result{UniValue::VOBJ};
            result.pushKV("configured", have_policy && have_ledger);
            result.pushKV("ledger_present", have_ledger);
            if (have_policy) result.pushKV("policy", ProviderSafetyPolicyToJSON(policy));
            if (have_ledger) {
                result.pushKV("accounting_time_high_water", ledger.accounting_time_high_water);
            }
            if (have_policy && have_ledger) {
                const int64_t now = GetTime();
                using namespace DigiDollar::Paymaster;
                result.pushKV("user_paid", ProviderSafetyClassStatusToJSON(
                                               ledger, policy, FundingModel::USER_PAID, SponsorshipScope::PUBLIC, now));
                result.pushKV("public_sponsored", ProviderSafetyClassStatusToJSON(
                                                      ledger, policy, FundingModel::SPONSORED, SponsorshipScope::PUBLIC, now));
                result.pushKV("restricted_sponsored", ProviderSafetyClassStatusToJSON(
                                                          ledger, policy, FundingModel::SPONSORED, SponsorshipScope::RESTRICTED, now));
            }
            return result;
        }),
    };
}

RPCHelpMan setpaymasterclientsafetypolicy()
{
    return RPCHelpMan{
        "setpaymasterclientsafetypolicy",
        "Persist wallet-local absolute, percentage and rolling-day Paymaster service-fee ceilings.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete local client safety policy", {
                                                                                                           {"maximum_service_fee_per_transaction_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum service fee for one transfer"},
                                                                                                           { "maximum_service_fee_bps", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Effective fee as basis points of the actual recipient amount (100 = 1%, 0 = free only, 0..10000). Omission preserves the saved percentage cap." },
                                                                                                           {"maximum_service_fee_per_day_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-day service-fee ceiling"},
                                                                                                       }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persisted client safety policy", {
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_per_transaction_cents", "Per-transfer service-fee ceiling"},
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_per_day_cents", "Rolling-day service-fee ceiling"},
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_bps", /*optional=*/true, "Effective percentage ceiling; absent for legacy absolute-only approval"},
                                                                                  {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
                                                                              }},
        RPCExamples{HelpExampleCli("setpaymasterclientsafetypolicy", "'{\"maximum_service_fee_per_transaction_cents\":100,\"maximum_service_fee_per_day_cents\":1000}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            return ToJSON(SaveClientSafetyPolicy(*wallet, ParseClientSafetyPolicy(request.params[0].get_obj())));
        }),
    };
}

RPCHelpMan getpaymasterclientsafetystatus()
{
    return RPCHelpMan{
        "getpaymasterclientsafetystatus",
        "Return the local client fee policy and durable rolling-day usage.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Client safety status", {
                                                                        {RPCResult::Type::BOOL, "configured", "Whether both valid policy and ledger exist"},
                                                                        {RPCResult::Type::BOOL, "ledger_present", "Whether a valid durable ledger exists"},
                                                                        {RPCResult::Type::OBJ, "policy", /*optional=*/true, "Client safety policy", {
                                                                                                                                                        {RPCResult::Type::NUM, "maximum_service_fee_per_transaction_cents", "Per-transfer ceiling"},
                                                                                                                                                        {RPCResult::Type::NUM, "maximum_service_fee_per_day_cents", "Rolling-day ceiling"},
                                                                                                                                                        {RPCResult::Type::NUM, "maximum_service_fee_bps", /*optional=*/true, "Effective percentage ceiling; absent for legacy absolute-only approval"},
                                                                                  {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
                                                                                                                                                    }},
                                                                        {RPCResult::Type::NUM_TIME, "accounting_time_high_water", /*optional=*/true, "Monotonic accounting time"},
                                                                        {RPCResult::Type::NUM, "active_reservations", /*optional=*/true, "Active fee reservations"},
                                                                        {RPCResult::Type::NUM, "reserved_service_fee_cents", /*optional=*/true, "Service fee held by active authorizations"},
                                                                        {RPCResult::Type::NUM, "spent_service_fee_last_day_cents", /*optional=*/true, "Committed service fees in the rolling day"},
                                                                        {RPCResult::Type::NUM, "available_service_fee_today_cents", /*optional=*/true, "Remaining rolling-day allowance after reservations"},
                                                                    }},
        RPCExamples{HelpExampleCli("getpaymasterclientsafetystatus", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ClientSafetyPolicy policy;
            ClientFeeLedger ledger;
            const bool have_policy = GetPaymasterClientSafetyPolicy(*wallet, policy);
            const bool have_ledger = GetPaymasterClientFeeLedger(*wallet, ledger);
            UniValue result{UniValue::VOBJ};
            result.pushKV("configured", have_policy && have_ledger);
            result.pushKV("ledger_present", have_ledger);
            if (have_policy) result.pushKV("policy", ClientSafetyPolicyToJSON(policy));
            if (have_ledger) {
                const int64_t effective_now = std::max(GetTime(), ledger.accounting_time_high_water);
                int64_t reserved{0};
                int64_t spent_day{0};
                uint64_t active{0};
                for (const ClientFeeReservation& reservation : ledger.reservations) {
                    if (reservation.state == BudgetReservationState::RESERVED) {
                        ++active;
                        reserved += reservation.service_fee.value;
                    } else if (reservation.state == BudgetReservationState::SPENT &&
                               !TimeDeltaExceeds(effective_now, reservation.updated_at, 24 * 60 * 60)) {
                        spent_day += reservation.service_fee.value;
                    }
                }
                result.pushKV("accounting_time_high_water", ledger.accounting_time_high_water);
                result.pushKV("active_reservations", active);
                result.pushKV("reserved_service_fee_cents", reserved);
                result.pushKV("spent_service_fee_last_day_cents", spent_day);
                if (have_policy) {
                    result.pushKV("available_service_fee_today_cents",
                                  std::max<int64_t>(0, policy.maximum_service_fee_per_day.value -
                                                           reserved - spent_day));
                }
            }
            return result;
        }),
    };
}

RPCHelpMan createrestrictedpaymasterdescriptor()
{
    return RPCHelpMan{
        "createrestrictedpaymasterdescriptor",
        "Currently disabled: restricted sponsorship is reserved for a future extension.\n"
        "Create a short-lived provider-signed restricted service descriptor. "
        "The descriptor is not announced or persisted by the node.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"sponsor_authorization_key", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Sponsor/app x-only Schnorr authorization key"},
            {"sponsor_display_name", RPCArg::Type::STR, RPCArg::Optional::NO,
             "Printable sponsor name, at most 32 bytes"},
            {"expires_in", RPCArg::Type::NUM, RPCArg::Default{300},
             "Descriptor lifetime in seconds, from 1 through 600"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Non-gossiped restricted descriptor", {
                                                                                      {RPCResult::Type::STR_HEX, "descriptor", "Canonical serialized descriptor"},
                                                                                      {RPCResult::Type::STR_HEX, "provider_id", "Bound provider identity"},
                                                                                      {RPCResult::Type::STR_HEX, "provider_identity_key", "Provider x-only verification key"},
                                                                                      {RPCResult::Type::STR_HEX, "offer_id", "Restricted offer identifier"},
                                                                                      {RPCResult::Type::STR_HEX, "policy_hash", "Restricted policy hash"},
                                                                                      {RPCResult::Type::STR, "endpoint", "Direct Paymaster endpoint"},
                                                                                      {RPCResult::Type::NUM_TIME, "expires_at", "Descriptor expiry"},
                                                                                  }},
        RPCExamples{HelpExampleCli("createrestrictedpaymasterdescriptor", "\"0123...\" \"Merchant Sponsor\" 300")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            const auto key_bytes = ParseHexV(request.params[0], "sponsor_authorization_key");
            if (key_bytes.size() != 32) throw JSONRPCError(RPC_INVALID_PARAMETER, "sponsor_authorization_key must be exactly 32 bytes");
            return ToJSON(CreateRestrictedDescriptor(context, *wallet, XOnlyPubKey{MakeUCharSpan(key_bytes)}, request.params[1].get_str(), request.params[2].isNull() ? 300 : request.params[2].getInt<int64_t>()));
        }),
    };
}

RPCHelpMan setpaymasterenabled()
{
    return RPCHelpMan{
        "setpaymasterenabled",
        "Persistently enable or disable Paymaster provider operation for this wallet.\n"
        "Enabling does not synchronously start the provider. A saved autostart setting may bring it online later when all readiness checks pass; otherwise use startpaymaster explicitly.\n",
        {
            {"enabled", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Provider enablement"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent provider configuration", {
                                                                                     {RPCResult::Type::BOOL, "enabled", "Configured provider enablement"},
                                                                                     {RPCResult::Type::BOOL, "running", "Whether the runtime provider is online"},
                                                                                     {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Bound provider policy"},
                                                                                 }},
        RPCExamples{HelpExampleCli("setpaymasterenabled", "true")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            return ToJSON(EnableProvider(context, *wallet, request.params[0].get_bool()));
        }),
    };
}

RPCHelpMan setpaymasterruntimesettings()
{
    return RPCHelpMan{
        "setpaymasterruntimesettings",
        "Persist this wallet's Paymaster provider processing mode and optional autostart. "
        "Changing operation_mode is allowed only while the provider is stopped. "
        "Autostart never stores a wallet passphrase.\n",
        {
            {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO,
             "Runtime settings; omitted members retain their current values",
             {
                 {"operation_mode", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                  "automatic (recommended) or manual (expert mode)"},
                 {"autostart", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Start the configured runtime after wallet load when readiness permits"},
             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent runtime settings",
                  {
                      {RPCResult::Type::STR, "operation_mode", "automatic or manual"},
                      {RPCResult::Type::BOOL, "autostart", "Whether optional runtime autostart is enabled"},
                      {RPCResult::Type::BOOL, "running", "Whether this wallet's provider runtime is active"},
                  }},
        RPCExamples{HelpExampleCli(
            "setpaymasterruntimesettings",
            "'{\"operation_mode\":\"automatic\",\"autostart\":false}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            const auto& value = request.params[0].get_obj();
            RPCTypeCheckObj(value, {{"operation_mode", UniValueType(UniValue::VSTR)}, {"autostart", UniValueType(UniValue::VBOOL)}}, true, true);
            std::optional<ProviderOperationMode> mode;
            std::optional<bool> autostart;
            if (!value.find_value("operation_mode").isNull()) {
                const auto name = value.find_value("operation_mode").get_str();
                if (name != "automatic" && name != "manual") throw JSONRPCError(RPC_INVALID_PARAMETER, "operation_mode must be automatic or manual");
                mode = name == "automatic" ? ProviderOperationMode::AUTOMATIC : ProviderOperationMode::MANUAL;
            }
            if (!value.find_value("autostart").isNull()) autostart = value.find_value("autostart").get_bool();
            return ToJSON(SaveProviderRuntime(context, *wallet, mode, autostart));
        }),
    };
}

RPCHelpMan setpaymasterliquiditypolicy()
{
    return RPCHelpMan{
        "setpaymasterliquiditypolicy",
        "Set wallet-local automatic Paymaster liquidity targets and finite maintenance-fee limits.\n"
        "Setting paid_maintenance_approved=true is the operator's explicit consent to paid replenishment.\n"
        "A zero maintenance limit disables paid maintenance; it never means unlimited.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Liquidity targets and maintenance budget", {
                {"automatic_replenishment", RPCArg::Type::BOOL, RPCArg::Default{true}, "Automatically restore missing slots while an automatic provider is running"},
                {"paid_maintenance_approved", RPCArg::Type::BOOL, RPCArg::Default{false}, "Explicitly approve paid maintenance transactions"},
                {"target_admission_dgb", RPCArg::Type::NUM, RPCArg::Optional::NO, "Admission DGB target (3-16)"},
                {"target_operational_dgb", RPCArg::Type::NUM, RPCArg::Optional::NO, "Operational DGB target (1-16)"},
                {"target_admission_carriers", RPCArg::Type::NUM, RPCArg::Default{0}, "Admission carrier target (0 or 3-16)"},
                {"target_operational_carriers", RPCArg::Type::NUM, RPCArg::Default{0}, "Operational carrier target (0 or 1-16)"},
                {"maximum_maintenance_fee_per_transaction_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum DGB fee for one maintenance transaction, in satoshis"},
                {"maximum_maintenance_fee_per_hour_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-hour maintenance fee ceiling, in satoshis"},
                {"maximum_maintenance_fee_per_day_satoshis", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-day maintenance fee ceiling, in satoshis"},
            }},
            {"expected_updated_at", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Reject a stale edit atomically; zero requires no saved policy"},
            {"expected_provider_policy_hash", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Reject atomically if the provider offer changed since reviewing reserve capital"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Saved policy",
                  ProviderLiquidityPolicyResults()},
        RPCExamples{HelpExampleCli("setpaymasterliquiditypolicy", "'{\"automatic_replenishment\":true,\"paid_maintenance_approved\":true,\"target_admission_dgb\":3,\"target_operational_dgb\":1,\"target_admission_carriers\":3,\"target_operational_carriers\":1,\"maximum_maintenance_fee_per_transaction_satoshis\":1000000,\"maximum_maintenance_fee_per_hour_satoshis\":5000000,\"maximum_maintenance_fee_per_day_satoshis\":20000000}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            const UniValue& value = request.params[0];
            RPCTypeCheckObj(value,
                            {{"automatic_replenishment", UniValueType(UniValue::VBOOL)},
                             {"paid_maintenance_approved", UniValueType(UniValue::VBOOL)},
                             {"target_admission_dgb", UniValueType(UniValue::VNUM)},
                             {"target_operational_dgb", UniValueType(UniValue::VNUM)},
                             {"target_admission_carriers", UniValueType(UniValue::VNUM)},
                             {"target_operational_carriers", UniValueType(UniValue::VNUM)},
                             {"maximum_maintenance_fee_per_transaction_satoshis", UniValueType(UniValue::VNUM)},
                             {"maximum_maintenance_fee_per_hour_satoshis", UniValueType(UniValue::VNUM)},
                             {"maximum_maintenance_fee_per_day_satoshis", UniValueType(UniValue::VNUM)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            ProviderLiquidityPolicy policy;
            policy.automatic_replenishment = value.find_value("automatic_replenishment").isNull()
                ? true : value.find_value("automatic_replenishment").get_bool();
            policy.paid_maintenance_approved = value.find_value("paid_maintenance_approved").isNull()
                ? false : value.find_value("paid_maintenance_approved").get_bool();
            policy.target_admission_dgb = value.find_value("target_admission_dgb").getInt<uint16_t>();
            policy.target_operational_dgb = value.find_value("target_operational_dgb").getInt<uint16_t>();
            policy.target_admission_carriers = value.find_value("target_admission_carriers").isNull()
                ? 0 : value.find_value("target_admission_carriers").getInt<uint16_t>();
            policy.target_operational_carriers = value.find_value("target_operational_carriers").isNull()
                ? 0 : value.find_value("target_operational_carriers").getInt<uint16_t>();
            policy.maximum_maintenance_fee_per_transaction = DGBSatoshis{
                value.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>()};
            policy.maximum_maintenance_fee_per_hour = DGBSatoshis{
                value.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>()};
            policy.maximum_maintenance_fee_per_day = DGBSatoshis{
                value.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>()};
            std::optional<int64_t> expected_revision;
            if (!request.params[1].isNull()) expected_revision = request.params[1].getInt<int64_t>();
            std::optional<uint256> expected_provider_policy_hash;
            if (!request.params[2].isNull()) {
                expected_provider_policy_hash = ParseHashV(request.params[2], "expected_provider_policy_hash");
            }
            return ToJSON(SaveLiquidityPolicy(context, *wallet, policy, expected_revision, expected_provider_policy_hash));
        }),
    };
}

RPCHelpMan getpaymasterliquiditystatus()
{
    return RPCHelpMan{
        "getpaymasterliquiditystatus",
        "Return Paymaster liquidity targets, ready/pending/missing slots, maintenance budget and recyclable carrier value.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Liquidity status",
                  ProviderLiquidityStatusResults()},
        RPCExamples{HelpExampleCli("getpaymasterliquiditystatus", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            const int64_t now = GetTime();
            auto result = ProviderLiquidityStatusToJSON(readiness, now, wallet.get());
            result.pushKV("automation_status", ToJSON(CaptureProviderAutomationStatus(*wallet, context, readiness, now,
                AutomaticProviderStateIsSynchronized(*wallet))));
            return result;
        }),
    };
}

RPCHelpMan cancelpaymastermaintenance()
{
    return RPCHelpMan{
        "cancelpaymastermaintenance",
        "Preview cancellation of selected uncreated DGB/DD reserve-funding operations. Execution requires the unchanged plan_id. "
        "Core rejects signed, ambiguous or withdrawal work and disables automatic refill in the same transaction to prevent replacement. "
        "Provider enablement, autostart, existing reserves, payments and other operations are unchanged. No transaction is signed or sent.\n",
        {{"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exact cancellation selection", {
            {"operation_ids", RPCArg::Type::ARR, RPCArg::Optional::NO, "One to 256 operation IDs from getpaymasteroperatorinfo", {
                {"operation_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Saved funding operation"}}},
            {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Execute the reviewed cancellation and turn automatic refill off"},
            {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged cancellation preview required for execution"},
        }}},
        RPCResult{RPCResult::Type::OBJ, "", "Cancellation review or receipt", {
            {RPCResult::Type::BOOL, "executed", "Whether cancellation was committed"},
            {RPCResult::Type::STR_HEX, "plan_id", "Reviewed cancellation plan"},
            {RPCResult::Type::ARR, "operation_ids", "Exact cancelled or reviewed operations", {
                {RPCResult::Type::STR_HEX, "", "Operation ID"}}},
            {RPCResult::Type::NUM, "released_fee_budget_satoshis", "Released or releasable fee allowance; not a payment or refund"},
            {RPCResult::Type::BOOL, "automatic_refill_disabled", "Execution disables recurring refill"},
        }},
        RPCExamples{HelpExampleCli("cancelpaymastermaintenance", "'{\"operation_ids\":[\"<operation_id>\"]}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            const auto& options = request.params[0];
            RPCTypeCheckObj(options, {{"operation_ids", UniValueType(UniValue::VARR)},
                {"execute", UniValueType(UniValue::VBOOL)}, {"plan_id", UniValueType(UniValue::VSTR)}}, true, true);
            const auto& ids = options.find_value("operation_ids");
            if (!ids.isArray() || ids.empty() || ids.size() > 256)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_INVALID_MAINTENANCE_SELECTION");
            std::vector<uint256> operations;
            for (const auto& id : ids.getValues()) operations.push_back(ParseHashV(id, "operation_id"));
            const bool execute = options.find_value("execute").isTrue();
            uint256 plan;
            if (execute) plan = ParseHashV(options.find_value("plan_id"), "plan_id");
            else if (!options.find_value("plan_id").isNull())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_MAINTENANCE_PREVIEW_TAKES_NO_PLAN");
            return ToJSON(CancelProviderMaintenance(context, *wallet, operations, execute, plan));
        })};
}

RPCHelpMan releasepaymastercapital()
{
    return RPCHelpMan{
        "releasepaymastercapital",
        "Preview or atomically release all quiescent Paymaster pool outputs to ordinary wallet coin selection.\n"
        "First stop persistently with stoppaymaster {\"persistent\":true,\"pause_setup\":true}.\n"
        "No payment, network fee, wallet deletion or forced reservation release is performed.\n"
        "Open payments, reservations, unresolved maintenance, manual coin locks and unconfirmed outputs block the entire operation.\n"
        "With cancel_uncreated=true, the reviewed release also cancels planned reserve funding with no saved transaction. Signed, ambiguous and withdrawal work still blocks release. This option is unavailable for restored-wallet recovery.\n"
        "Execution requires the unchanged preview plan. Recurring paid-maintenance consent is revoked; identity, targets, budgets and recovery history remain.\n"
        "An explicitly restored wallet can request recovery release only with a matching independently retained checkpoint and no unfinished work. This permanently retires the provider identity; it never clears signing guards or sends funds.\n"
        "A replay after success fails PLAN_CHANGED; inspect pool state and obtain a new preview, never abandon signed work.\n",
        {{"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Explicit release approval", {
            {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Release the reviewed capital"},
            {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged preview plan id required for execution"},
            {"recovery", RPCArg::Type::BOOL, RPCArg::Default{false}, "Recover a matching restored wallet's capital while permanently retiring this provider identity"},
            {"cancel_uncreated", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include cancellation of provably uncreated reserve-funding tasks in this exact release plan"},
        }}},
        RPCResult{RPCResult::Type::OBJ, "", "Capital release preview or receipt", {
            {RPCResult::Type::BOOL, "executed", "Whether the atomic wallet update completed"},
            {RPCResult::Type::STR_HEX, "plan_id", "Binding to the exact reviewed wallet state"},
            {RPCResult::Type::NUM, "pool_entries", "Number of released outputs"},
            {RPCResult::Type::NUM, "dgb_satoshis", "DGB pool capital"},
            {RPCResult::Type::NUM, "dd_cents", "DD carrier capital including earnings"},
            {RPCResult::Type::NUM, "network_fee_satoshis", "Always zero; wallet-local release"},
            {RPCResult::Type::NUM, "uncreated_maintenance", "Number of uncreated funding tasks to cancel, or cancelled by this execution"},
            {RPCResult::Type::BOOL, "recovery_release", "The reviewed release permanently retires this restored provider identity"},
        }},
        RPCExamples{HelpExampleCli("releasepaymastercapital", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            const auto& options = request.params[0];
            if (!options.isNull()) RPCTypeCheckObj(options, {
                {"execute", UniValueType(UniValue::VBOOL)}, {"plan_id", UniValueType(UniValue::VSTR)},
                {"recovery", UniValueType(UniValue::VBOOL)},
                {"cancel_uncreated", UniValueType(UniValue::VBOOL)},
            }, true, true);
            const bool execute = options.find_value("execute").isTrue();
            const bool recovery = options.find_value("recovery").isTrue();
            const bool cancel_uncreated = options.find_value("cancel_uncreated").isTrue();
            uint256 plan;
            if (execute) {
                if (options.find_value("plan_id").isNull())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CAPITAL_PLAN_REQUIRED");
                plan = ParseHashV(options.find_value("plan_id"), "plan_id");
            } else if (!options.find_value("plan_id").isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CAPITAL_PREVIEW_TAKES_NO_PLAN");
            }
            return ToJSON(ReleaseProviderCapital(context, *wallet, execute, plan, recovery, cancel_uncreated));
        }),
    };
}

RPCHelpMan adoptpaymasterbackup()
{
    return RPCHelpMan{
        "adoptpaymasterbackup",
        "Preview or adopt a current restored provider wallet. Stop persistently first.\n"
        "Requires its independently retained current checkpoint, a synchronized chain and no open payments, reserved budgets, unfinished maintenance or ambiguous reserves.\n"
        "Execution fences older copies using this checkpoint store before clearing restoration quarantine. No transaction or network fee is created.\n"
        "Reserves, policies, spent budgets and history are retained; the provider stays stopped and autostart remains off. Start separately after review.\n"
        "A jointly rolled-back wallet/checkpoint pair cannot be detected. Never operate this identity on independent hosts; only confirm independent_checkpoint if the node checkpoint was preserved independently of the restored wallet.\n",
        {{"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Reviewed adoption approval", {
            {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Adopt the unchanged reviewed backup"},
            {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged preview plan required for execution"},
            {"independent_checkpoint", RPCArg::Type::BOOL, RPCArg::Default{false}, "Explicitly confirm the current node checkpoint was retained independently; required for execution"},
        }}},
        RPCResult{RPCResult::Type::OBJ, "", "Backup review or adoption receipt", {
            {RPCResult::Type::BOOL, "executed", "Whether adoption committed"},
            {RPCResult::Type::STR_HEX, "plan_id", "Binding to the exact reviewed wallet and checkpoint"},
            {RPCResult::Type::STR_HEX, "provider_id", "Retained provider identity"},
            {RPCResult::Type::NUM, "pool_entries", "Unchanged prepared reserve count"},
            {RPCResult::Type::NUM, "dgb_satoshis", "Retained DGB reserve capital"},
            {RPCResult::Type::NUM, "dd_cents", "Retained DD reserve capital"},
            {RPCResult::Type::NUM, "network_fee_satoshis", "Always zero"},
            {RPCResult::Type::BOOL, "provider_stopped", "Always true; start separately"},
        }},
        RPCExamples{HelpExampleCli("adoptpaymasterbackup", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto& context = EnsureWalletContext(request.context);
            const auto& options = request.params[0];
            if (!options.isNull()) RPCTypeCheckObj(options, {
                {"execute", UniValueType(UniValue::VBOOL)}, {"plan_id", UniValueType(UniValue::VSTR)},
                {"independent_checkpoint", UniValueType(UniValue::VBOOL)},
            }, true, true);
            const bool execute = options.find_value("execute").isTrue();
            uint256 plan;
            if (execute) {
                if (!options.find_value("independent_checkpoint").isTrue())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_BACKUP_INDEPENDENT_CHECKPOINT_REQUIRED");
                if (options.find_value("plan_id").isNull())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_BACKUP_PLAN_REQUIRED");
                plan = ParseHashV(options.find_value("plan_id"), "plan_id");
            } else if (!options.find_value("plan_id").isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_BACKUP_PREVIEW_TAKES_NO_PLAN");
            }
            return ToJSON(AdoptProviderBackup(context, *wallet, execute, plan, options.find_value("independent_checkpoint").isTrue()));
        }),
    };
}

RPCHelpMan withdrawpaymastercarrier()
{
    return RPCHelpMan{
        "withdrawpaymastercarrier",
        "Preview or execute a carrier excess withdrawal, or release one operational carrier slot.\n"
        "Execution requires the unchanged plan_id returned by the immediately preceding preview.\n"
        "all_excess keeps exactly 1.00 DD in every selected carrier and sends only the combined excess to a fresh wallet address.\n"
        "release_slot is wallet-local, costs no network fee, requires a stopped provider, and reduces the operational carrier target by one.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Withdrawal mode and preview binding", {
                {"mode", RPCArg::Type::STR, RPCArg::Optional::NO, "all_excess or release_slot"},
                {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Execute the previously reviewed plan"},
                {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged plan id required for execution"},
                {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Carrier txid for release_slot preview"},
                {"vout", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Carrier output index for release_slot preview"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Carrier withdrawal preview or execution", {
            {RPCResult::Type::BOOL, "executed", "Whether the plan was executed"},
            {RPCResult::Type::STR_HEX, "plan_id", "Plan id bound to exact inputs, outputs, limits, and expiry"},
            {RPCResult::Type::STR, "mode", "all_excess or release_slot"},
            {RPCResult::Type::NUM, "source_carriers", /*optional=*/true, "Number of carrier outputs bound to the plan"},
            {RPCResult::Type::NUM, "withdrawable_excess_cents", /*optional=*/true, "Combined DD excess"},
            {RPCResult::Type::NUM, "retained_carrier_cents", /*optional=*/true, "DD retained in carrier slots"},
            {RPCResult::Type::NUM, "estimated_network_fee_satoshis", /*optional=*/true, "Exact reviewed maintenance fee estimate"},
            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Executed transaction id"},
            {RPCResult::Type::NUM, "operational_carrier_target", /*optional=*/true, "Target after a release_slot operation"},
            {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Preview expiry"},
        }},
        RPCExamples{
            HelpExampleCli("withdrawpaymastercarrier", "'{\"mode\":\"all_excess\"}'") +
            HelpExampleCli("withdrawpaymastercarrier", "'{\"mode\":\"all_excess\",\"execute\":true,\"plan_id\":\"PLAN\"}'") +
            HelpExampleCli("withdrawpaymastercarrier", "'{\"mode\":\"release_slot\",\"txid\":\"TXID\",\"vout\":0}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(
                options,
                {{"mode", UniValueType(UniValue::VSTR)},
                 {"execute", UniValueType(UniValue::VBOOL)},
                 {"plan_id", UniValueType(UniValue::VSTR)},
                 {"txid", UniValueType(UniValue::VSTR)},
                 {"vout", UniValueType(UniValue::VNUM)}},
                /*fAllowNull=*/true, /*fStrict=*/true);
            CarrierWithdrawalParameters parameters;
            const std::string mode_name = options.find_value("mode").get_str();

            if (mode_name == "all_excess") {
                parameters.mode = CarrierWithdrawalMode::ALL_EXCESS;
            } else if (mode_name == "release_slot") {
                parameters.mode = CarrierWithdrawalMode::RELEASE_SLOT;
            } else {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_MODE");
            }
            const UniValue& execute_value = options.find_value("execute");
            parameters.execute = !execute_value.isNull() && execute_value.get_bool();
            if (!options.find_value("plan_id").isNull()) parameters.plan_id = ParseHashV(options.find_value("plan_id"), "plan_id");
            if (!options.find_value("txid").isNull() && !options.find_value("vout").isNull()) {
                const auto index = options.find_value("vout").getInt<int64_t>();
                if (index < 0 || index > std::numeric_limits<uint32_t>::max()) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_INVALID_CARRIER_OUTPOINT");
                parameters.source = COutPoint{ParseHashV(options.find_value("txid"), "txid"), static_cast<uint32_t>(index)};
            }
            return ToJSON(WithdrawPaymasterCarrier(context, *wallet, parameters));
        }),
    };
}

RPCResult paymaster_rpc::internal::PoolPreparationResultSchema()
{
    return {RPCResult::Type::ARR, "preparation", "Durable finite setup steps; no provider start is implied", {
        {RPCResult::Type::OBJ, "", "Setup step", {
            {RPCResult::Type::STR_HEX, "plan_id", "Approved plan"},
            {RPCResult::Type::STR_HEX, "operation_id", "Durable step identity"},
            {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
            {RPCResult::Type::STR, "state", "pending_creation, pending_confirmation, complete, conflict or cancelled"},
            {RPCResult::Type::STR, "error", "Last execution diagnostic; empty when complete"},
            {RPCResult::Type::STR, "diagnostic", true, "Fee-check phase and amounts when available"},
            {RPCResult::Type::NUM, "maximum_fee_satoshis", "Approved maximum transaction fee"},
            {RPCResult::Type::NUM, "actual_fee_satoshis", "Saved transaction fee, zero before construction"},
            {RPCResult::Type::STR_HEX, "txid", true, "Existing transaction; never replaced by a retry"},
            {RPCResult::Type::NUM, "required_output_satoshis", true, "Uncreated DGB pool principal"},
            {RPCResult::Type::NUM, "confirmed_available_satoshis", true, "Eligible confirmed DGB funding"},
            {RPCResult::Type::NUM, "available_including_unconfirmed_satoshis", true, "Eligible funding including safe unconfirmed change"},
            {RPCResult::Type::NUM, "capital_shortfall_satoshis", true, "Missing confirmed DGB reserve capital; network fees are additional"},
            {RPCResult::Type::NUM, "funding_shortfall_upper_bound_satoshis", true, "Shortfall including the full fee ceiling, after unconfirmed change is counted; actual fee may be lower"},
            {RPCResult::Type::NUM, "required_output_cents", true, "DD capital for the approved uncreated step"},
            {RPCResult::Type::NUM, "confirmed_available_cents", true, "Currently spendable confirmed DD outside protected reserves"},
            {RPCResult::Type::NUM, "pending_cents", true, "Unconfirmed wallet DD; eligibility is rechecked after confirmation"},
            {RPCResult::Type::NUM, "capital_shortfall_cents", true, "Missing confirmed DD capital for this step"},
        }},
    }};
}

RPCHelpMan preparepaymasterpool()
{
    return RPCHelpMan{
        "preparepaymasterpool",
        "Preview or create dedicated P2TR DGB and DD-carrier outputs for the Paymaster admission and operational pools.\n"
        "The default is a side-effect-free preview. Execution requires the unchanged plan_id returned by that preview.\n"
        "Existing live entries count toward the requested totals, so an interrupted preparation can be resumed safely.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Pool target and explicit execution approval", {
                                                                                                                    {"admission_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Three to sixteen admission DGB slots"},
                                                                                                                    {"operational_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "One to sixteen operational DGB slots"},
                                                                                                                    {"admission_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Three to sixteen admission carriers for USER_PAID"},
                                                                                                                    {"operational_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "One to sixteen operational carriers for USER_PAID"},
                                                                                                                    {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Persist the reviewed finite setup and automatically continue it when funding is confirmed"},
                                                                                                                    {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged preview plan id required for execution"},
                {"cancel", RPCArg::Type::BOOL, RPCArg::Default{false}, "With execute and the original plan_id, cancel only steps with no saved transaction; signed transactions remain tracked"},
                {"preview_policy", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Read-only estimate with a proposed policy; cannot be executed", {}, RPCArgOptions{.skip_type_check = true}},
                {"maximum_fee_satoshis", RPCArg::Type::NUM, RPCArg::Default{20000000}, "Maximum fee per setup transaction; at most one DGB and one DD transaction are authorized"},
                {"recover_dgb_txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Explicitly adopt an existing wallet DGB transaction; never discover it from labels"},
                {"recover_dgb_outputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Exact outputs of the recovery transaction", {
                    {"output", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Pool output", {
                        {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "Output index"},
                        {"purpose", RPCArg::Type::STR, RPCArg::Optional::NO, "admission or operational"},
                    }},
                }},
                                                                                                                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Pool preparation preview or transaction", {
                PoolPreparationResultSchema(),
                                                                                           {RPCResult::Type::BOOL, "executed", "Whether this call committed a wallet transaction"},
                                                                                           {RPCResult::Type::BOOL, "cancelled", "Whether unsigned steps were cancelled"},
                {RPCResult::Type::BOOL, "accepted", "Whether a finite automatic setup was authorized"},
                {RPCResult::Type::NUM, "maximum_fee_satoshis", "Maximum approved fee per transaction"},
                {RPCResult::Type::NUM, "maximum_total_fee_satoshis", "Maximum approved fees for the new setup steps"},
                {RPCResult::Type::STR_HEX, "plan_id", "Plan bound to the policy, targets, current deficits, and output values"},
                {RPCResult::Type::BOOL, "preview_only", "Proposed-policy estimate; not executable"},
                                                                                           {RPCResult::Type::NUM, "admission_dgb_slots", "Admission DGB outputs"},
                                                                                           {RPCResult::Type::NUM, "operational_dgb_slots", "Operational DGB outputs"},
                                                                                           {RPCResult::Type::NUM, "admission_carrier_slots", "Admission DD carriers"},
                                                                                           {RPCResult::Type::NUM, "operational_carrier_slots", "Operational DD carriers"},
                                                                                           {RPCResult::Type::NUM, "missing_admission_dgb_slots", "Admission DGB outputs still to create"},
                                                                                           {RPCResult::Type::NUM, "missing_operational_dgb_slots", "Operational DGB outputs still to create"},
                                                                                           {RPCResult::Type::NUM, "missing_admission_carrier_slots", "Admission carriers still to create"},
                                                                                           {RPCResult::Type::NUM, "missing_operational_carrier_slots", "Operational carriers still to create"},
                                                                                           {RPCResult::Type::NUM, "admission_dgb_satoshis_each", "Value of every admission output"},
                                                                                           {RPCResult::Type::NUM, "operational_dgb_satoshis_each", "Value of every operational output"},
                                                                                           {RPCResult::Type::NUM, "carrier_cents_each", "Value of every DD carrier"},
                                                                                           {RPCResult::Type::NUM, "total_output_satoshis", "Total value assigned to pool outputs"},
                                                                                           {RPCResult::Type::NUM, "total_carrier_cents", "Total DD assigned to new carriers"},
                                                                                           {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Committed pool transaction"},
                                                                                           {RPCResult::Type::STR_HEX, "dd_txid", /*optional=*/true, "Committed DD carrier transaction"},
                                                                                           {RPCResult::Type::STR_HEX, "dgb_txid", /*optional=*/true, "Committed DGB pool transaction"},
                                                                                           {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "Operator-paid transaction fee"},
                                                                                           {RPCResult::Type::ARR, "pool", /*optional=*/true, "Persisted unconfirmed pool entries", {{RPCResult::Type::OBJ, "", "Pool entry", {
                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR, "purpose", "admission or operational"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR, "asset", "dgb or dd_carrier"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR, "state", "available"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "dgb_satoshis", "Output value"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "dd_cents", "Carrier value or zero for DGB entries"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "confirmation_height", "Zero until confirmed"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "reservation_id", /*optional=*/true, "Durable reservation"},
                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "origin_commit_key", /*optional=*/true, "Commit or maintenance operation that created this successor"},
                                                                                                                                                                                                                                 {RPCResult::Type::NUM_TIME, "updated_at", "Persistence time"},
                                                                                                                                                                                                                             }}}},
                                                                                       }},
        RPCExamples{HelpExampleCli("preparepaymasterpool", "'{\"admission_dgb_slots\":3,\"operational_dgb_slots\":1,\"admission_carrier_slots\":3,\"operational_carrier_slots\":1}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"admission_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"operational_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"admission_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"operational_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"execute", UniValueType(UniValue::VBOOL)},
                             {"plan_id", UniValueType(UniValue::VSTR)},
                             {"cancel", UniValueType(UniValue::VBOOL)},
                             {"maximum_fee_satoshis", UniValueType(UniValue::VNUM)},
                             {"preview_policy", UniValueType(UniValue::VOBJ)},
                             {"recover_dgb_txid", UniValueType(UniValue::VSTR)},
                             {"recover_dgb_outputs", UniValueType(UniValue::VARR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            PoolPreparationParameters parameters;
            static_cast<PoolTargets&>(parameters) = ParsePoolTargets(options);
            parameters.cancel = options.find_value("cancel").isTrue();
            if (!options.find_value("maximum_fee_satoshis").isNull()) parameters.maximum_fee = options.find_value("maximum_fee_satoshis").getInt<int64_t>();
            if (!options.find_value("preview_policy").isNull()) parameters.preview_policy = ParseProviderPolicy(options.find_value("preview_policy"));
            if (!options.find_value("recover_dgb_txid").isNull()) parameters.recovery_transaction = ParseHashV(options.find_value("recover_dgb_txid"), "recover_dgb_txid");
            if (!options.find_value("recover_dgb_outputs").isNull()) {
                parameters.recovery_outputs.emplace();
                for (const auto& item : options.find_value("recover_dgb_outputs").getValues()) {
                    RPCTypeCheckObj(item, {{"vout", UniValueType(UniValue::VNUM)}, {"purpose", UniValueType(UniValue::VSTR)}}, false, true);
                    const auto purpose = item.find_value("purpose").get_str();
                    if (purpose != "admission" && purpose != "operational") throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_POOL_RECOVERY_OUTPUT_INVALID");
                    parameters.recovery_outputs->push_back({item.find_value("vout").getInt<uint32_t>(), purpose == "admission" ? PoolPurpose::ADMISSION : PoolPurpose::OPERATIONAL});
                }
            }
            return ToJSON(PreparePaymasterPool(context, *wallet, parameters));
        }),
    };
}

RPCHelpMan rebalancepaymasterpool()
{
    return RPCHelpMan{
        "rebalancepaymasterpool",
        "Preview or retire excess available Paymaster pool entries.\n"
        "The default is side-effect-free. Execution requires the unchanged plan_id returned by that preview.\n"
        "Active reservations are never touched. Increasing a target remains an additive preparepaymasterpool operation.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Pool target and explicit execution approval", {
                                                                                                                    {"admission_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Remaining admission DGB slots"},
                                                                                                                    {"operational_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Remaining operational DGB slots"},
                                                                                                                    {"admission_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Remaining admission carriers"},
                                                                                                                    {"operational_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Remaining operational carriers"},
                                                                                                                    {"dgb_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Preserve every DD carrier; retire only excess DGB"},
                                                                                                                     {"maximum_fee_satoshis", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Explicit positive fee ceiling per retirement transaction"},
                                                                                                                     {"recommend_fee", RPCArg::Type::BOOL, RPCArg::Default{false}, "Preview only: propose a finite fee ceiling from current transaction estimates; does not approve spending or change refill limits"},
                                                                                                                     {"expected_liquidity_updated_at", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Require this saved liquidity revision and its exact targets; retain underfilled categories without creating missing reserves"},
                                                                                                                     {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Commit the reviewed retirement transactions"},
                                                                                                                    {"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Unchanged preview plan id required for execution"},
                                                                                                                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Pool rebalance preview or result", {
                                                                                    {RPCResult::Type::BOOL, "executed", "Whether a retirement transaction was committed"},
                                                                                    {RPCResult::Type::STR_HEX, "plan_id", "Plan bound to the policy, targets, and exact retired pool inputs"},
                                                                                    {RPCResult::Type::NUM, "retired_admission_dgb_slots", "Admission DGB entries selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_operational_dgb_slots", "Operational DGB entries selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_admission_carrier_slots", "Admission carriers selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_operational_carrier_slots", "Operational carriers selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_dgb_satoshis", "DGB returned to ordinary wallet liquidity before network fee"},
                                                                                    {RPCResult::Type::NUM, "retired_carrier_cents", "DD returned to ordinary wallet liquidity"},
                                                                                    {RPCResult::Type::NUM, "maximum_network_fee_satoshis", /*optional=*/true, "Reviewed fee ceiling per retirement transaction"},
                                                                                    {RPCResult::Type::NUM, "maximum_total_fee_satoshis", /*optional=*/true, "Maximum combined retirement fees"},
                                                                                    {RPCResult::Type::NUM, "estimated_dgb_fee_satoshis", /*optional=*/true, "Estimated DGB retirement fee when recommend_fee is requested"},
                                                                                    {RPCResult::Type::NUM, "estimated_dd_fee_satoshis", /*optional=*/true, "Estimated DD retirement fee when recommend_fee is requested"},
                                                                                    {RPCResult::Type::NUM, "wallet_maximum_fee_satoshis", /*optional=*/true, "Unchanged wallet-wide fee ceiling"},
                                                                                    {RPCResult::Type::NUM, "dd_network_fee_satoshis", /*optional=*/true, "DD retirement transaction fee"},
                                                                                     {RPCResult::Type::STR_HEX, "dd_txid", /*optional=*/true, "DD carrier retirement transaction"},
                                                                                    {RPCResult::Type::STR_HEX, "dgb_txid", /*optional=*/true, "DGB retirement transaction"},
                                                                                    {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "DGB retirement transaction fee"},
                                                                                    {RPCResult::Type::ARR, "pool", /*optional=*/true, "Authoritative persisted pool entries", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Pool entry", {
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
        RPCExamples{HelpExampleCli("rebalancepaymasterpool", "'{\"admission_dgb_slots\":3,\"operational_dgb_slots\":1,\"admission_carrier_slots\":3,\"operational_carrier_slots\":1}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"admission_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"operational_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"admission_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"operational_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"dgb_only", UniValueType(UniValue::VBOOL)},
                             {"maximum_fee_satoshis", UniValueType(UniValue::VNUM)},
                             {"recommend_fee", UniValueType(UniValue::VBOOL)},
                             {"expected_liquidity_updated_at", UniValueType(UniValue::VNUM)},
                             {"execute", UniValueType(UniValue::VBOOL)},
                             {"plan_id", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            PoolRebalanceParameters parameters;
            static_cast<PoolTargets&>(parameters) = ParsePoolTargets(options);
            parameters.dgb_only = options.find_value("dgb_only").isTrue();
            parameters.recommend_fee = options.find_value("recommend_fee").isTrue();
            if (!options.find_value("maximum_fee_satoshis").isNull()) parameters.maximum_fee = options.find_value("maximum_fee_satoshis").getInt<int64_t>();
            if (!options.find_value("expected_liquidity_updated_at").isNull()) parameters.expected_revision = options.find_value("expected_liquidity_updated_at").getInt<int64_t>();
            return ToJSON(RebalancePaymasterPool(context, *wallet, parameters));
        }),
    };
}

RPCHelpMan getpaymasterfinancestatus()
{
    return RPCHelpMan{
        "getpaymasterfinancestatus",
        "Return wallet-local provider income, DGB costs, pool capital, and optional accounting events.\n"
        "Native DD and DGB values are authoritative. Any USD result uses only the current oracle price.\n",
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Finance query", {
                {"period", RPCArg::Type::STR, RPCArg::Default{"30d"}, "UTC calendar days: today, 7d, 30d (including today), or all"},
                {"include_events", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include paginated accounting events"},
                {"limit", RPCArg::Type::NUM, RPCArg::Default{100}, "Maximum events returned (1-10000)"},
                {"cursor", RPCArg::Type::STR, RPCArg::Default{""}, "Opaque cursor from the previous page; restart pagination if the data revision changed"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Provider finance status", {
            {RPCResult::Type::STR_HEX, "provider_id", "Provider identity owning this ledger"},
            {RPCResult::Type::STR, "period", "Selected accounting period"},
            {RPCResult::Type::NUM, "revision", "Accounting revision binding all event pages"},
            {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
            {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
            {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
            {RPCResult::Type::NUM, "average_service_fee_cents", "Average service fee per confirmed recipient transfer"},
            {RPCResult::Type::NUM, "user_paid_transfers", "Confirmed user-paid transfers"},
            {RPCResult::Type::NUM, "public_sponsored_transfers", "Confirmed public sponsored transfers"},
            {RPCResult::Type::NUM, "restricted_sponsored_transfers", "Confirmed restricted sponsored transfers"},
            {RPCResult::Type::OBJ_DYN, "model_breakdown", "Selected-period transfer economics by funding model", {
                {RPCResult::Type::OBJ, "model", "One funding-model summary", {
                    {RPCResult::Type::NUM, "successful_transfers", "Confirmed transfers"},
                    {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                    {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB transfer costs"},
                }},
            }},
            {RPCResult::Type::OBJ_DYN, "period_summaries", "Keys are today, 7d, 30d, and all", {
                {RPCResult::Type::OBJ, "period", "Confirmed totals for one dashboard period", {
                    {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                    {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
                    {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
                    {RPCResult::Type::NUM, "average_service_fee_cents", "Average service fee per confirmed recipient transfer"},
                    {RPCResult::Type::NUM, "user_paid_transfers", "Confirmed user-paid transfers"},
                    {RPCResult::Type::NUM, "public_sponsored_transfers", "Confirmed public sponsored transfers"},
                    {RPCResult::Type::NUM, "restricted_sponsored_transfers", "Confirmed restricted sponsored transfers"},
                }},
            }},
            {RPCResult::Type::BOOL, "history_partially_reconstructable", "Whether earlier exact history is unavailable"},
            {RPCResult::Type::NUM_TIME, "history_complete_from", "Start of guaranteed complete accounting"},
            {RPCResult::Type::BOOL, "backup_required", "Whether the provider wallet should be backed up"},
            {RPCResult::Type::NUM_TIME, "last_successful_backup_at", "Last successful backupwallet completion"},
            {RPCResult::Type::NUM_TIME, "external_backup_acknowledged_at", "Last acknowledged external full-wallet backup"},
            {RPCResult::Type::NUM, "oracle_price_micro_usd", /*optional=*/true, "Current DGB/USD oracle price"},
            {RPCResult::Type::NUM_TIME, "valuation_time", /*optional=*/true, "Time of the current-price estimate"},
            {RPCResult::Type::NUM, "estimated_result_usd", /*optional=*/true, "Current-price estimate, not historical accounting"},
            {RPCResult::Type::OBJ, "pool_capital", "Current wallet-owned provider capital", {
                {RPCResult::Type::NUM, "dgb_available_satoshis", "Available DGB pool capital"},
                {RPCResult::Type::NUM, "dgb_reserved_satoshis", "Reserved or committed DGB pool capital"},
                {RPCResult::Type::NUM, "dgb_pending_satoshis", "Unconfirmed DGB successor capital"},
                {RPCResult::Type::NUM, "carrier_base_cents", "Reserved DD carrier base capital"},
                {RPCResult::Type::NUM, "carrier_earned_cents", "Service fees accumulated above carrier bases"},
                {RPCResult::Type::NUM, "carrier_withdrawable_cents", "Confirmed available carrier surplus"},
                {RPCResult::Type::NUM, "pending_maintenance_transactions", "Pending maintenance or withdrawal transactions"},
            }},
            {RPCResult::Type::ARR, "daily_totals", /*optional=*/true, "Confirmed UTC-day totals for the selected period, limited to the latest 366 days", {
                {RPCResult::Type::OBJ, "", "One UTC accounting day", {
                    {RPCResult::Type::NUM_TIME, "day_start", "UTC start time of this day"},
                    {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                    {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
                    {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
                    {RPCResult::Type::NUM, "maintenance_transactions", "Confirmed setup, maintenance, retirement, or withdrawal transactions"},
                }},
            }},
            {RPCResult::Type::ARR, "events", /*optional=*/true, "Accounting events", {
                {RPCResult::Type::OBJ, "", "Finance event", {
                    {RPCResult::Type::STR_HEX, "event_id", "Stable idempotency key"},
                    {RPCResult::Type::STR, "kind", "transfer, recovery, setup, replenishment, retirement, or withdrawal"},
                    {RPCResult::Type::STR, "state", "pending, confirmed, or invalidated"},
                    {RPCResult::Type::NUM, "dd_income_cents", "Native DD income"},
                    {RPCResult::Type::NUM, "dgb_cost_satoshis", "Native DGB cost"},
                    {RPCResult::Type::NUM_TIME, "created_at", "Event creation time"},
                    {RPCResult::Type::NUM_TIME, "confirmed_at", /*optional=*/true, "Confirmation time"},
                    {RPCResult::Type::STR, "funding_model", /*optional=*/true, "user_paid or sponsored for transfer events"},
                    {RPCResult::Type::STR, "sponsorship_scope", /*optional=*/true, "public or restricted for sponsored transfer events"},
                    {RPCResult::Type::STR_HEX, "transaction_id", "Wallet transaction represented by this event"},
                }},
            }},
            {RPCResult::Type::STR, "next_cursor", /*optional=*/true, "Cursor for the next event page"},
        }},
        RPCExamples{HelpExampleCli("getpaymasterfinancestatus", "'{\"period\":\"30d\",\"include_events\":true}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue options = request.params[0].isNull()
                ? UniValue{UniValue::VOBJ} : request.params[0];
            RPCTypeCheckObj(options,
                            {{"period", UniValueType(UniValue::VSTR)},
                             {"include_events", UniValueType(UniValue::VBOOL)},
                             {"limit", UniValueType(UniValue::VNUM)},
                             {"cursor", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const std::string period = options.find_value("period").isNull()
                ? "30d" : options.find_value("period").get_str();
            const bool include_events =
                !options.find_value("include_events").isNull() &&
                options.find_value("include_events").get_bool();
            const int limit = options.find_value("limit").isNull()
                ? 100 : options.find_value("limit").getInt<int>();
            const std::string cursor = options.find_value("cursor").isNull()
                ? std::string{} : options.find_value("cursor").get_str();
            if ((period != "today" && period != "7d" && period != "30d" &&
                 period != "all") || limit < 1 || limit > 10000) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_INVALID_FINANCE_QUERY");
            }

            ProviderFinanceQuery query;
            query.period = period == "today" ? ProviderFinancePeriod::TODAY :
                period == "7d" ? ProviderFinancePeriod::DAYS7 :
                period == "30d" ? ProviderFinancePeriod::DAYS30 : ProviderFinancePeriod::ALL;
            query.include_events = include_events;
            query.limit = limit;
            query.cursor = cursor;
            auto captured = CaptureProviderFinanceStatus(*wallet, std::move(query));
            if (const auto* failure = std::get_if<ProviderFinanceStatusFailure>(&captured)) {
                if (failure->disposition) {
                    throw ApplicationError(failure->kind, failure->code, *failure->disposition);
                }
                throw JSONRPCError(failure->kind == ErrorKind::INVALID_INPUT ? RPC_INVALID_PARAMETER : RPC_WALLET_ERROR,
                                   failure->code);
            }
            return ToJSON(std::get<ProviderFinanceStatus>(captured));
        }),
    };
}

RPCHelpMan acknowledgepaymasterproviderbackup()
{
    return RPCHelpMan{
        "acknowledgepaymasterproviderbackup",
        "Acknowledge an external full-wallet backup procedure for this provider wallet.\n"
        "This does not create a backup and must not be used for seed-only or descriptor-only exports.\n",
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Backup acknowledgement", {
                {"external_backup", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Must be true after an external full-wallet backup"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Updated backup reminder", {
            {RPCResult::Type::BOOL, "acknowledged", "Whether the external backup was acknowledged"},
            {RPCResult::Type::BOOL, "backup_required", "Whether another backup reminder remains"},
            {RPCResult::Type::NUM_TIME, "acknowledged_at", "Acknowledgement time"},
        }},
        RPCExamples{HelpExampleCli("acknowledgepaymasterproviderbackup", "'{\"external_backup\":true}'")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            RPCTypeCheckObj(request.params[0], {{"external_backup", UniValueType(UniValue::VBOOL)}}, false, true);
            return ToJSON(AcknowledgeProviderBackup(*wallet, request.params[0].find_value("external_backup").get_bool()));
        }),
    };
}

RPCHelpMan getpaymasterinfo()
{
    return RPCHelpMan{
        "getpaymasterinfo",
        "Return this wallet's authoritative Paymaster provider configuration and staged readiness.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider information", {
                                                                        {RPCResult::Type::BOOL, "settings_present", "Whether this wallet has a persisted provider settings record"},
                                                                        {RPCResult::Type::BOOL, "enabled", "Configured provider enablement"},
                                                                        {RPCResult::Type::BOOL, "running", "Runtime provider state"},
                                                                        {RPCResult::Type::STR, "operation_mode", "automatic or manual"},
                                                                        {RPCResult::Type::BOOL, "autostart", "Whether optional provider autostart is enabled"},
                                                                        {RPCResult::Type::STR, "service_state", "stopped, waiting_for_unlock, waiting_for_readiness, active, manual, drain_only, or error"},
                                                                        {RPCResult::Type::STR, "last_service_error", /*optional=*/true, "Stable privacy-neutral automatic-service error"},
                                                                        {RPCResult::Type::OBJ, "service_queue", "Provider-addressed in-memory queue depth", {
                                                                                                                                                              {RPCResult::Type::NUM, "waiting_requests", "Capacity, quote, and recovery requests waiting for this provider"},
                                                                                                                                                              {RPCResult::Type::NUM, "waiting_submits", "Payment and recovery submissions waiting for this provider"},
                                                                                                                                                          }},
                                                                        {RPCResult::Type::BOOL, "ready", "Whether all V1 readiness gates currently pass"},
                                                                        {RPCResult::Type::BOOL, "wallet_eligible", "Descriptor/local-key eligibility"},
                                                                        {RPCResult::Type::BOOL, "wallet_locked", "Wallet lock state"},
                                                                        {RPCResult::Type::BOOL, "pool_ready", "Whether the persisted admission and operational pools pass policy checks"},
                                                                        {RPCResult::Type::OBJ, "pool", "Persisted pool summary", {
                                                                            {RPCResult::Type::OBJ, "activity", "Observed current work; historical committed outputs do not imply an active payment", {
                                                                                {RPCResult::Type::NUM, "active_payments", "Current reserved payment budgets"},
                                                                                {RPCResult::Type::NUM, "capacity_requests", "Live pre-intent Capacity admissions"},
                                                                                {RPCResult::Type::NUM, "reserved_outputs", "Reserved operational outputs"},
                                                                                {RPCResult::Type::NUM, "pending_confirmations", "Unconfirmed available outputs and pending successors"},
                                                                            }},
                                                                                                                                     {RPCResult::Type::NUM, "entries", "Total validated entries"},
                                                                                                                                     {RPCResult::Type::NUM, "reserved", "Entries reserved or durably committed to a request"},
                                                                                                                                     {RPCResult::Type::NUM, "admission_dgb", "Confirmed available admission DGB slots"},
                                                                                                                                     {RPCResult::Type::NUM, "admission_carriers", "Confirmed available admission DD carriers"},
                                                                                                                                     {RPCResult::Type::NUM, "operational_dgb", "Confirmed available operational DGB entries"},
                                                                                                                                     {RPCResult::Type::NUM, "operational_carriers", "Confirmed available operational DD carriers"},
                                                                                                                                     {RPCResult::Type::NUM, "complete_operational_slots", "Usable complete operational slots"},
                                                                                                                                 }},
                                                                         {RPCResult::Type::OBJ, "liquidity", "Persisted liquidity targets, confirmed/pending/missing slots, and finite maintenance budget",
                                                                          ProviderLiquidityStatusResults()},
                                                                        {RPCResult::Type::OBJ, "backup_status", /*optional=*/true, "Provider-wallet backup reminder", {
                                                                            {RPCResult::Type::BOOL, "required", "Whether a fresh full-wallet backup or external-backup acknowledgement is recommended"},
                                                                            {RPCResult::Type::NUM_TIME, "reminder_updated_at", "Time of the latest identity or material configuration reminder"},
                                                                            {RPCResult::Type::NUM_TIME, "last_successful_backup_at", "Last backupwallet completion recorded by this wallet"},
                                                                            {RPCResult::Type::NUM_TIME, "external_backup_acknowledged_at", "Last acknowledged external full-wallet backup"},
                                                                        }},
                                                                        {RPCResult::Type::OBJ, "finance_summary", /*optional=*/true, "Confirmed native all-time provider accounting", {
                                                                            {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
                                                                            {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
                                                                            {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
                                                                            {RPCResult::Type::BOOL, "history_partially_reconstructable", "Whether older exact history is unavailable"},
                                                                            {RPCResult::Type::NUM, "revision", "Accounting revision of this summary"},
                                                                        }},
                                                                        {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                        {RPCResult::Type::STR, "endpoint", /*optional=*/true, "Published provider endpoint"},
                                                                        {RPCResult::Type::NUM, "announcement_sequence", /*optional=*/true, "Published monotonic announcement sequence"},
                                                                        {RPCResult::Type::STR_HEX, "identity_key", /*optional=*/true, "BIP86 x-only identity key"},
                                                                        {RPCResult::Type::STR, "display_name", /*optional=*/true, "Provider display name"},
                                                                        {RPCResult::Type::OBJ, "policy", /*optional=*/true, "Persisted policy", {
                                                                                                                                                    {RPCResult::Type::ARR, "funding_models", "Enabled funding models", {{RPCResult::Type::STR, "", "sponsored or user_paid"}}},
                                                                                                                                                    {RPCResult::Type::STR, "sponsorship_scope", "public or restricted"},
                                                                                                                                                    {RPCResult::Type::NUM, "fee_rate_bps", "User-paid rate in basis points"},
                                                                                                                                                      {RPCResult::Type::NUM, "maximum_user_paid_service_fee_cents", "Maximum user-paid DD service fee; zero means no cap"},
                                                                                                                                                    {RPCResult::Type::NUM, "min_amount_cents", "Minimum recipient amount"},
                                                                                                                                                    {RPCResult::Type::NUM, "max_amount_cents", "Maximum recipient amount"},
                                                                                                                                                    {RPCResult::Type::NUM, "quote_ttl", "Quote lifetime in seconds"},
                                                                                                                                                    {RPCResult::Type::NUM, "maximum_network_fee_dgb_satoshis", "Absolute provider network-fee cap"},
                                                                                                                                                    {RPCResult::Type::STR_HEX, "policy_hash", "Canonical provider policy hash"},
                                                                                                                                                }},
                                                                        PaymasterTransportResult(),
                                                                        {RPCResult::Type::ARR, "readiness_errors", "Outstanding readiness gates", {{RPCResult::Type::STR, "", "Stable readiness error"}}},
                                                                    }},
        RPCExamples{HelpExampleCli("getpaymasterinfo", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            std::string finance_error;
            const bool finance_current = ReconcileProviderFinanceReport(*wallet, true, finance_error);
            if (!finance_current) {
                LogPrint(BCLog::DIGIDOLLAR,
                         "Paymaster finance reconciliation skipped in "
                         "getpaymasterinfo: %s\n",
                         finance_error);
            }
            const ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            const bool running = readiness.have_identity && context.paymaster &&
                                 context.paymaster->IsProviderRunning(wallet->GetName(), readiness.identity.provider_id);
            ProviderServiceStatus service_status;
            if (context.paymaster) {
                service_status = ProviderServiceStatusForObservation(
                    context.paymaster->GetProviderServiceStatus(wallet->GetName()), readiness.ready,
                    AutomaticProviderStateIsSynchronized(*wallet));
            }
            if (!running) {
                if (readiness.have_settings && readiness.settings.autostart &&
                    wallet->IsLocked()) {
                    service_status.state = ProviderServiceState::WAITING_FOR_UNLOCK;
                } else if (readiness.have_settings &&
                           readiness.settings.autostart && !readiness.ready) {
                    service_status.state = ProviderServiceState::WAITING_FOR_READINESS;
                } else {
                    service_status.state = ProviderServiceState::STOPPED;
                }
            } else if (readiness.settings.operation_mode ==
                       ProviderOperationMode::MANUAL) {
                service_status.state = ProviderServiceState::MANUAL;
            } else if (service_status.state == ProviderServiceState::STOPPED) {
                service_status.state = ProviderServiceState::ACTIVE;
            }

            UniValue pool{UniValue::VOBJ};
            pool.pushKV("activity", ToJSON(CaptureProviderActivity(readiness, GetTime())));
            pool.pushKV("entries", static_cast<uint64_t>(readiness.pool_entries.size()));
            pool.pushKV("reserved", static_cast<uint64_t>(std::count_if(
                                        readiness.pool_entries.begin(), readiness.pool_entries.end(), [](const auto& entry) {
                                            return entry.state == DigiDollar::Paymaster::PoolEntryState::RESERVED ||
                                                   entry.state == DigiDollar::Paymaster::PoolEntryState::PENDING_SUCCESSOR ||
                                                   entry.state == DigiDollar::Paymaster::PoolEntryState::COMMITTED;
                                        })));
            pool.pushKV("admission_dgb", static_cast<uint64_t>(readiness.pool.admission_dgb));
            pool.pushKV("admission_carriers", static_cast<uint64_t>(readiness.pool.admission_carriers));
            pool.pushKV("operational_dgb", static_cast<uint64_t>(readiness.pool.operational_dgb));
            pool.pushKV("operational_carriers", static_cast<uint64_t>(readiness.pool.operational_carriers));
            pool.pushKV("complete_operational_slots",
                        static_cast<uint64_t>(readiness.pool.complete_operational_slots));

            UniValue result{UniValue::VOBJ};
            result.pushKV("settings_present", readiness.have_settings);
            result.pushKV("enabled", readiness.settings.enabled);
            result.pushKV("running", running);
            result.pushKV("operation_mode",
                          ProviderOperationModeName(
                              readiness.settings.operation_mode));
            result.pushKV("autostart", readiness.settings.autostart);
            result.pushKV("service_state",
                          ProviderServiceStateName(service_status.state));
            if (!service_status.last_error.empty()) {
                result.pushKV("last_service_error",
                              service_status.last_error);
            }
            ProviderQueueStatus queue_status;
            if (context.paymaster && readiness.have_identity) {
                queue_status = context.paymaster->GetProviderQueueStatus(
                    readiness.identity.provider_id);
            }
            UniValue service_queue{UniValue::VOBJ};
            service_queue.pushKV(
                "waiting_requests",
                static_cast<uint64_t>(queue_status.waiting_requests));
            service_queue.pushKV(
                "waiting_submits",
                static_cast<uint64_t>(queue_status.waiting_submits));
            result.pushKV("service_queue", std::move(service_queue));
            result.pushKV("ready", readiness.ready);
            result.pushKV("wallet_eligible", readiness.wallet_eligible);
            result.pushKV("wallet_locked", wallet->IsLocked());
            result.pushKV("pool_ready", readiness.pool_ready);
            result.pushKV("pool", std::move(pool));
            result.pushKV(
                "liquidity",
                ProviderLiquidityStatusToJSON(readiness, GetTime(), wallet.get()));
            if (readiness.have_identity) {
                result.pushKV("provider_id", readiness.identity.provider_id.GetHex());
                result.pushKV("identity_key", HexStr(readiness.identity.identity_key));
                result.pushKV("display_name", readiness.identity.display_name);

                ProviderBackupStatus backup;
                std::string backup_error;
                if (GetPaymasterProviderBackupStatus(
                        *wallet, backup, GetTime(), backup_error)) {
                    UniValue backup_status{UniValue::VOBJ};
                    backup_status.pushKV(
                        "required", ProviderBackupRequired(backup));
                    backup_status.pushKV(
                        "reminder_updated_at", backup.reminder_updated_at);
                    backup_status.pushKV(
                        "last_successful_backup_at",
                        backup.last_successful_backup_at);
                    backup_status.pushKV(
                        "external_backup_acknowledged_at",
                        backup.external_backup_acknowledged_at);
                    result.pushKV("backup_status", std::move(backup_status));
                }

                {
                    LOCK(wallet->cs_wallet);
                    ProviderFinanceMetadata finance;
                    if (finance_current && AreProviderFinancesCurrent(*wallet) &&
                        WalletBatch{wallet->GetDatabase()}.ReadPaymasterFinanceMetadata(finance) == DatabaseReadStatus::FOUND &&
                        finance.provider_id == readiness.identity.provider_id && finance.genesis_hash == Params().GenesisBlock().GetHash()) {
                        UniValue summary{UniValue::VOBJ};
                        summary.pushKV("service_fee_income_cents", finance.totals.total.dd_income.value);
                        summary.pushKV("dgb_operating_cost_satoshis", finance.totals.total.dgb_cost.value);
                        summary.pushKV("successful_transfers", finance.totals.total.successful_transfers);
                        summary.pushKV("history_partially_reconstructable", finance.earlier_history_partial);
                        summary.pushKV("revision", finance.revision);
                        result.pushKV("finance_summary", std::move(summary));
                    }
                }
            }
            if (readiness.endpoint.IsValid()) {
                result.pushKV("endpoint", readiness.endpoint.ToStringAddrPort());
            }
            uint64_t announcement_sequence{0};
            if (WalletBatch{wallet->GetDatabase()}.ReadPaymasterAnnouncementSequence(
                    announcement_sequence)) {
                result.pushKV("announcement_sequence", announcement_sequence);
            }
            if (readiness.have_policy) result.pushKV("policy", ProviderPolicyToJSON(readiness.policy));
            if (const auto* transport_node = wallet->chain().context(); transport_node && transport_node->connman) {
                result.pushKV("transport", PaymasterTransportToJSON(*transport_node->connman));
            }
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            return result;
        }),
    };
}

RPCHelpMan getpaymasteroperatorinfo()
{
    return RPCHelpMan{
        "getpaymasteroperatorinfo",
        "Read-only provider setup and operations snapshot. Does not reconcile, spend, sign, probe endpoints or start service. Node/chain readiness can change after the snapshot.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Operator snapshot", {
                                                                     {RPCResult::Type::NUM, "schema_version", "Operator schema version"},
                                                                     {RPCResult::Type::STR, "network", "Active network"},
                                                                     {RPCResult::Type::STR, "wallet", "Explicitly selected wallet"},
                                                                     {RPCResult::Type::STR_HEX, "wallet_generation", "Ephemeral wallet load identity; changes on reload"},
                                                                     {RPCResult::Type::NUM_TIME, "observed_at", "Observation time"},
                                                                     {RPCResult::Type::NUM_TIME, "unlocked_until", "Wallet relock time, zero when not timed; -1 for an explicit Paymaster unlock until lock/unload/shutdown"},
                                                                     {RPCResult::Type::BOOL, "encrypted", "Wallet uses key encryption"},
                                                                     {RPCResult::Type::OBJ, "provider", "Provider configuration and local readiness", {{RPCResult::Type::ELISION, "", "Same field meanings as getpaymasterinfo; no finance history scan"}}},
                                                                     {RPCResult::Type::OBJ, "node_settings", "Effective startup values, including ordinary_outbound_target; OS-limited capacity is reported separately in provider.transport", {{RPCResult::Type::ELISION, "", "Allowed Paymaster node options"}}},
                                                                     {RPCResult::Type::OBJ, "node_setting_overrides", "Options controlled outside the editable config: command_line, forced, or settings_json; advisory, configuration RPCs recheck conflicts", {{RPCResult::Type::ELISION, "", "Setting name and controlling source"}}},
                                                                     {RPCResult::Type::OBJ, "safety", "Finite policies and budget accounting", {{RPCResult::Type::ELISION, "", "Same field meanings as getpaymastersafetystatus; no expiry mutation"}}},
                                                                     {RPCResult::Type::ARR, "configuration_conflicts", "Known offer/payment fee-limit mismatches; not a readiness verdict", {{RPCResult::Type::OBJ, "", "Conflict", {
                                                                         {RPCResult::Type::STR, "code", "PAYMASTER_OFFER_FEE_LIMIT_MISMATCH"},
                                                                         {RPCResult::Type::STR, "model", "Affected funding model"},
                                                                         {RPCResult::Type::BOOL, "offered", "Model is offered by the saved policy"},
                                                                         {RPCResult::Type::NUM, "offer_fee_ceiling_satoshis", "Advertised network-fee ceiling"},
                                                                         {RPCResult::Type::NUM, "payment_fee_limit_satoshis", "Per-payment safety limit"},
                                                                     }}}},
                                                                     {RPCResult::Type::ARR, "diagnostics", "Ordered causes and action identifiers", {{RPCResult::Type::OBJ, "", "Diagnostic", {
                                                                                                                                                                                                  {RPCResult::Type::STR, "code", "Stable code"},
                                                                                                                                                                                                  {RPCResult::Type::STR, "state", "ready, waiting, action_required, error or unknown"},
                                                                                                                                                                                                  {RPCResult::Type::STR, "severity", "info, warning or error"},
                                                                                                                                                                                                  {RPCResult::Type::STR, "area", "Affected operator area"},
                                                                                                                                                                                                  {RPCResult::Type::STR, "action", "Suggested action; never automatic authority"},
                                                                                                                                                                                              }}}},
                                                                 }},
        RPCExamples{HelpExampleCli("getpaymasteroperatorinfo", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            auto& context = EnsureWalletContext(request.context);
            auto wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const auto readiness = GetProviderReadiness(*wallet, context, /*wait_for_sync=*/false, /*reconcile=*/false);
            const bool synchronized = AutomaticProviderStateIsSynchronized(*wallet);
            const int64_t now = GetTime();
            LOCK(wallet->cs_wallet);
            UniValue result{UniValue::VOBJ}, provider{UniValue::VOBJ}, safety{UniValue::VOBJ};
            const bool running = readiness.have_identity && context.paymaster && context.paymaster->IsProviderRunning(wallet->GetName(), readiness.identity.provider_id);
            std::string state = "stopped";
            if (running) {
                const auto service = ProviderServiceStatusForObservation(
                    context.paymaster->GetProviderServiceStatus(wallet->GetName()), readiness.ready, synchronized);
                state = ProviderServiceStateName(service.state);
                if (!service.last_error.empty()) provider.pushKV("last_service_error", service.last_error);
            } else if (readiness.settings.enabled && (readiness.settings.autostart || (context.paymaster && context.paymaster->HasRequestedProviderStart(wallet->GetName(), readiness.identity.provider_id))))
                state = wallet->IsLocked() ? "waiting_for_unlock" : "waiting_for_readiness";
            provider.pushKV("settings_present", readiness.have_settings);
            provider.pushKV("enabled", readiness.settings.enabled);
            provider.pushKV("running", running);
            provider.pushKV("start_requested", context.paymaster && context.paymaster->HasRequestedProviderStart(wallet->GetName(), readiness.identity.provider_id));
            provider.pushKV("operation_mode", ProviderOperationModeName(readiness.settings.operation_mode));
            provider.pushKV("autostart", readiness.settings.autostart);
            provider.pushKV("service_state", state);
            provider.pushKV("wallet_eligible", readiness.wallet_eligible);
            provider.pushKV("wallet_locked", wallet->IsLocked());
            provider.pushKV("ready", readiness.ready);
            provider.pushKV("pool_ready", readiness.pool_ready);
            UniValue pool_summary{UniValue::VOBJ};
            pool_summary.pushKV("activity", ToJSON(CaptureProviderActivity(readiness, now)));
            pool_summary.pushKV("entries", uint64_t(readiness.pool_entries.size()));
            pool_summary.pushKV("reserved", uint64_t(std::count_if(readiness.pool_entries.begin(), readiness.pool_entries.end(),
                [](const auto& entry) { return entry.state == PoolEntryState::RESERVED || entry.state == PoolEntryState::PENDING_SUCCESSOR || entry.state == PoolEntryState::COMMITTED; })));
            pool_summary.pushKV("admission_dgb", uint64_t(readiness.pool.admission_dgb));
            pool_summary.pushKV("operational_dgb", uint64_t(readiness.pool.operational_dgb));
            pool_summary.pushKV("admission_carriers", uint64_t(readiness.pool.admission_carriers));
            pool_summary.pushKV("operational_carriers", uint64_t(readiness.pool.operational_carriers));
            pool_summary.pushKV("complete_operational_slots", uint64_t(readiness.pool.complete_operational_slots));
            provider.pushKV("pool", pool_summary);
            provider.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            auto liquidity = ProviderLiquidityStatusToJSON(readiness, now, wallet.get());
            liquidity.pushKV("automation_status", ToJSON(CaptureProviderAutomationStatus(*wallet, context, readiness, now, synchronized)));
            for (const auto& key : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
                auto counts = liquidity.find_value(key);
                size_t reserved{0};
                for (const auto& entry : readiness.pool_entries) {
                    const bool admission = std::string{key}.find("admission") == 0;
                    const bool carrier = std::string{key}.find("carriers") != std::string::npos;
                    if (entry.state == PoolEntryState::RESERVED && (entry.purpose == PoolPurpose::ADMISSION) == admission && (entry.asset == PoolAsset::DD_CARRIER) == carrier) ++reserved;
                }
                counts.pushKV("reserved", reserved);
                liquidity.pushKV(key, counts);
            }
            bool exhausted{false};
            if (liquidity.find_value("policy").find_value("paid_maintenance_approved").isTrue()) {
                for (const auto& period : {std::string{"hour"}, std::string{"day"}})
                    exhausted |= OperatorRemaining(liquidity.find_value("policy").find_value("maximum_maintenance_fee_per_" + period + "_satoshis").getInt<int64_t>(), liquidity.find_value("maintenance_fee_spent_last_" + period + "_satoshis").getInt<int64_t>(), liquidity.find_value("maintenance_fee_reserved_satoshis").getInt<int64_t>()) == 0;
            }
            provider.pushKV("maintenance_budget_exhausted", exhausted);
            provider.pushKV("liquidity", liquidity);
            UniValue prerequisites{UniValue::VOBJ};
            prerequisites.pushKV("chain_height", wallet->chain().getHeight().value_or(-1));
            prerequisites.pushKV("txindex_enabled", context.args && context.args->GetBoolArg("-txindex", false));
            if (g_txindex) {
                const auto index = g_txindex->GetSummary();
                prerequisites.pushKV("txindex_synced", index.synced);
                prerequisites.pushKV("txindex_height", index.best_block_height);
            }
            provider.pushKV("prerequisites", prerequisites);
            if (readiness.have_policy) provider.pushKV("policy", ProviderPolicyToJSON(readiness.policy));
            if (readiness.endpoint.IsValid()) provider.pushKV("endpoint", readiness.endpoint.ToStringAddrPort());
            if (const auto* node = wallet->chain().context(); node && node->connman) provider.pushKV("transport", PaymasterTransportToJSON(*node->connman));
            if (readiness.have_identity) {
                provider.pushKV("provider_id", readiness.identity.provider_id.GetHex());
                provider.pushKV("display_name", readiness.identity.display_name);
                ProviderBackupStatus backup;
                std::string error;
                if (!GetPaymasterProviderBackupStatus(*wallet, backup, now, error, /*persist=*/false)) throw JSONRPCError(RPC_WALLET_ERROR, error);
                UniValue status{UniValue::VOBJ};
                status.pushKV("required", ProviderBackupRequired(backup));
                status.pushKV("reminder_updated_at", backup.reminder_updated_at);
                status.pushKV("last_successful_backup_at", backup.last_successful_backup_at);
                status.pushKV("external_backup_acknowledged_at", backup.external_backup_acknowledged_at);
                provider.pushKV("backup_status", status);
            }
            ProviderQueueStatus queue;
            if (readiness.have_identity && context.paymaster) queue = context.paymaster->GetProviderQueueStatus(readiness.identity.provider_id);
            UniValue work{UniValue::VOBJ};
            work.pushKV("waiting_requests", uint64_t(queue.waiting_requests));
            work.pushKV("waiting_submits", uint64_t(queue.waiting_submits));
            provider.pushKV("service_queue", work);
            UniValue preparation{UniValue::VARR};
            UniValue active_operations{UniValue::VARR};
            for (const auto& record : readiness.maintenance_ledger.records) {
                if (record.state == ProviderMaintenanceState::CONFIRMED || record.state == ProviderMaintenanceState::RELEASED) continue;
                UniValue item{UniValue::VOBJ};
                item.pushKV("plan_id", record.plan_id.GetHex());
                item.pushKV("operation_id", record.operation_id.GetHex());
                item.pushKV("asset", record.kind == ProviderMaintenanceKind::REPLENISH_DGB || record.kind == ProviderMaintenanceKind::PREPARE_DGB ? "dgb" : "dd_carrier");
                item.pushKV("kind", record.IsPreparation() ? "preparation" : record.kind == ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS ? "withdrawal" : "maintenance");
                item.pushKV("created_at", record.created_at);
                item.pushKV("updated_at", record.updated_at);
                item.pushKV("actual_fee_satoshis", record.actual_fee.value);
                int confirmations{0};
                if (!record.transaction_id.IsNull()) {
                    const auto tx = wallet->mapWallet.find(record.transaction_id);
                    if (tx != wallet->mapWallet.end()) confirmations = std::max(0, wallet->GetTxDepthInMainChain(tx->second));
                }
                item.pushKV("confirmations", confirmations);
                item.pushKV("required_confirmations", 1);
                item.pushKV("maximum_fee_satoshis", record.maximum_fee.value);
                item.pushKV("state", record.state == ProviderMaintenanceState::FAILED ? "conflict" : record.transaction_id.IsNull() ? "pending_creation" :
                                                                                                                                      "pending_confirmation");
                AddPreparationDiagnostic(item, record);
                AddPreparationFunding(item, *wallet, record);
                if (!record.transaction_id.IsNull()) item.pushKV("txid", record.transaction_id.GetHex());
                if (record.IsPreparation()) preparation.push_back(item);
                active_operations.push_back(std::move(item));
            }
            provider.pushKV("preparation", preparation);
            provider.pushKV("active_operations", active_operations);
            safety.pushKV("configured", readiness.have_safety_policy && readiness.have_budget_ledger);
            if (readiness.have_safety_policy) safety.pushKV("policy", ProviderSafetyPolicyToJSON(readiness.safety_policy));
            if (readiness.have_safety_policy && readiness.have_budget_ledger) {
                safety.pushKV("user_paid", ProviderSafetyClassStatusToJSON(readiness.budget_ledger, readiness.safety_policy, FundingModel::USER_PAID, SponsorshipScope::PUBLIC, now));
                safety.pushKV("public_sponsored", ProviderSafetyClassStatusToJSON(readiness.budget_ledger, readiness.safety_policy, FundingModel::SPONSORED, SponsorshipScope::PUBLIC, now));
                safety.pushKV("restricted_sponsored", ProviderSafetyClassStatusToJSON(readiness.budget_ledger, readiness.safety_policy, FundingModel::SPONSORED, SponsorshipScope::RESTRICTED, now));
            }
            UniValue budget_errors{UniValue::VARR};
            if (readiness.have_policy && readiness.have_safety_policy && readiness.have_budget_ledger) {
                const auto collect = [&](const char* name) { for (const auto& error : safety.find_value(name).find_value("errors").getValues()) budget_errors.push_back(error); };
                if (PolicyAllowsFundingModel(readiness.policy, FundingModel::USER_PAID)) collect("user_paid");
                if (PolicyAllowsFundingModel(readiness.policy, FundingModel::SPONSORED)) collect(readiness.policy.sponsorship_scope == SponsorshipScope::PUBLIC ? "public_sponsored" : "restricted_sponsored");
            }
            provider.pushKV("budget_errors", budget_errors);
            UniValue startup{UniValue::VOBJ};
            if (context.args) {
                for (const auto& key : {"digidollar", "paymaster", "txindex", "v2transport"})
                    startup.pushKV(key, int(context.args->GetBoolArg("-" + std::string{key}, std::string{key} == "paymaster")));
                startup.pushKV("prune", context.args->GetIntArg("-prune", 0));
                startup.pushKV("maxconnections", context.args->GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS));
                startup.pushKV("paymastermaxoutbound", context.args->GetIntArg("-paymastermaxoutbound", DEFAULT_DIRECT_OUTBOUND));
                startup.pushKV("paymastermaxinbound", context.args->GetIntArg("-paymastermaxinbound", MAX_DIRECT_INBOUND));
                UniValue binds{UniValue::VARR};
                for (const auto& bind : context.args->GetArgs("-paymasterbind"))
                    binds.push_back(bind);
                startup.pushKV("paymasterbind", binds);
                startup.pushKV("paymasterendpoint", context.args->GetArg("-paymasterendpoint", ""));
                if (const auto* node = wallet->chain().context(); node && node->connman) startup.pushKV("ordinary_outbound_target", node->connman->GetOrdinaryOutboundTarget());
            }
            result.pushKV("node_settings", startup);
            // Advisory provenance for the connection editor. The configuration
            // RPC still rechecks all overrides and included files before writing.
            UniValue overrides{UniValue::VOBJ};
            if (context.args) {
                context.args->LockSettings([&](const common::Settings& settings) {
                    for (const auto* key : {"digidollar", "paymaster", "txindex", "v2transport", "prune",
                                           "maxconnections", "paymastermaxoutbound", "paymastermaxinbound",
                                           "paymasterbind", "paymasterendpoint"}) {
                        if (settings.forced_settings.count(key)) overrides.pushKV(key, "forced");
                        else if (settings.command_line_options.count(key)) overrides.pushKV(key, "command_line");
                        else if (settings.rw_settings.count(key)) overrides.pushKV(key, "settings_json");
                    }
                });
            }
            result.pushKV("node_setting_overrides", overrides);
            result.pushKV("schema_version", 1);
            result.pushKV("network", ChainTypeToString(Params().GetChainType()));
            result.pushKV("wallet", wallet->GetName());
            result.pushKV("wallet_generation", wallet->m_paymaster_transport_owner.GetHex());
            result.pushKV("observed_at", now);
            result.pushKV("unlocked_until", wallet->IsLocked() ? 0 : wallet->nRelockTime);
            result.pushKV("encrypted", wallet->IsCrypted());
            result.pushKV("diagnostics", OperatorDiagnostics(provider, wallet->nRelockTime, now));
            result.pushKV("provider", provider);
            result.pushKV("safety", safety);
            result.pushKV("configuration_conflicts", OperatorFeeLimitConflicts(provider.find_value("policy"), safety.find_value("policy")));
            return result;
        }),
    };
}

} // namespace wallet
