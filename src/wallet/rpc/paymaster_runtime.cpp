// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider runtime start, stop, and automatic service scheduling. */

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
#include <chrono>
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

RPCHelpMan startpaymaster()
{
    return RPCHelpMan{
        "startpaymaster",
        "Start normal Paymaster provider operation after all readiness gates pass. "
        "When safety limits are exhausted, a non-announcing drain-only runtime "
        "is started only if durable quotes or signatures still require exact completion.\n",
        {{"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Start options", {
            {"wait_for_readiness", RPCArg::Type::BOOL, RPCArg::Default{false}, "Keep a one-shot start request until ready during this wallet load; does not enable autostart or approve spending"},
        }}},
        RPCResult{RPCResult::Type::OBJ, "", "Provider runtime result", {
                                                                           {RPCResult::Type::BOOL, "running", "Whether the provider is now online"},
                                                                           {RPCResult::Type::BOOL, "ready", "Whether every readiness gate passed"},
                                                                           {RPCResult::Type::BOOL, "start_requested", "An explicit start is waiting for readiness"},
                                                                           {RPCResult::Type::BOOL, "drain_only", "Whether only durable existing work may be completed"},
                                                                           {RPCResult::Type::STR, "operation_mode", "automatic or manual"},
                                                                           {RPCResult::Type::BOOL, "autostart", "Whether optional provider autostart is enabled"},
                                                                           {RPCResult::Type::STR, "service_state", "Effective provider service state"},
                                                                           {RPCResult::Type::OBJ, "safety_policy", /*optional=*/true, "Effective finite provider budgets", ProviderSafetyPolicyResults()},
                                                                           {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                           {RPCResult::Type::STR, "endpoint", /*optional=*/true, "Published provider endpoint"},
                                                                           {RPCResult::Type::NUM, "announcement_sequence", /*optional=*/true, "Published monotonic announcement sequence"},
                                                                           {RPCResult::Type::NUM, "recovery_candidates", "Durable provider commits and client finals considered for exact recovery"},
                                                                           {RPCResult::Type::NUM, "recovered_broadcasts", "Exact transactions reconciled or broadcast idempotently"},
                                                                           {RPCResult::Type::NUM, "expired_commits", "Deprecated compatibility field; durable signatures are never released merely because a local retry time elapsed"},
                                                                           {RPCResult::Type::ARR, "recovery_errors", "Stable per-transaction recovery failures", {{RPCResult::Type::STR, "", "Recovery failure"}}},
                                                                           {RPCResult::Type::ARR, "readiness_errors", "Stable reasons preventing start", {{RPCResult::Type::STR, "", "Readiness error"}}},
                                                                           PaymasterTransportResult(),
                                                                       }},
        RPCExamples{HelpExampleCli("startpaymaster", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            bool wait_for_readiness{false};
            if (!request.params[0].isNull()) {
                const auto& options = request.params[0].get_obj();
                RPCTypeCheckObj(options, {{"wait_for_readiness", UniValueType(UniValue::VBOOL)}}, true, true);
                wait_for_readiness = options.find_value("wait_for_readiness").isTrue();
            }
            return ToJSON(StartPaymaster(context, *wallet, wait_for_readiness, ExecutionMode::MANUAL));
        }),
    };
}

RPCHelpMan stoppaymaster()
{
    return RPCHelpMan{
        "stoppaymaster",
        "Stop the ephemeral Paymaster provider runtime for this wallet. Durable recovery records are retained. "
        "A registered stop prevents new provider work while waiting up to five seconds for active work to finish. Timeout or failure removes this temporary priority; it does not confirm a stop or cancel signed work. "
        "Without options a saved autostart setting may start it again later. Use {\"persistent\":true,\"pause_setup\":true} for the normal persistent pause: disable autostart and provider configuration under the provider work guard before stopping. Existing reservations and signed transactions remain recoverable and can still confirm.\n",
        {{"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Persistent pause options", {
                                                                                                   {"persistent", RPCArg::Type::BOOL, RPCArg::Default{false}, "Disable saved autostart before stopping"},
                                                                                                   {"pause_setup", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also disable provider configuration and new pool preparation signatures; requires persistent"},
                                                                                               }}},
        RPCResult{RPCResult::Type::OBJ, "", "Provider runtime result", {{RPCResult::Type::BOOL, "running", "Always false after a successful stop"}}},
        RPCExamples{HelpExampleCli("stoppaymaster", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            bool persistent{false}, pause_setup{false};
            if (!request.params[0].isNull()) {
                const UniValue& options = request.params[0].get_obj();
                RPCTypeCheckObj(options, {{"persistent", UniValueType(UniValue::VBOOL)}, {"pause_setup", UniValueType(UniValue::VBOOL)}}, true, true);
                if (!options.find_value("persistent").isNull()) persistent = options.find_value("persistent").get_bool();
                if (!options.find_value("pause_setup").isNull()) pause_setup = options.find_value("pause_setup").get_bool();
                if (pause_setup && !persistent) throw JSONRPCError(RPC_INVALID_PARAMETER, "pause_setup requires persistent=true");
            }
            return ToJSON(StopPaymaster(context, *wallet, persistent, pause_setup));
        }),
    };
}

} // namespace wallet
