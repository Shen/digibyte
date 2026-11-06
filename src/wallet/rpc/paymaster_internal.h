// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Internal helpers shared by the domain-specific Paymaster RPC units. */

#ifndef DIGIBYTE_WALLET_RPC_PAYMASTER_INTERNAL_H
#define DIGIBYTE_WALLET_RPC_PAYMASTER_INTERNAL_H

#include <logging.h>
#include <netaddress.h>
#include <net.h>
#include <paymaster/manager.h>
#include <paymaster/error.h>
#include <paymaster/validation.h>
#include <rpc/util.h>
#include <streams.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <version.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterapplication.h>
#include <wallet/paymasterservice.h>
#include <wallet/paymasterclient.h>
#include <wallet/paymasterclientview.h>
#include <wallet/paymasterrecoveryservice.h>
#include <wallet/paymasterapplication_internal.h>
#include <wallet/paymasterpool.h>
#include <wallet/paymasterproviderops.h>
#include <wallet/paymasterconfiguration.h>
#include <wallet/paymasterfinance.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_results.h>
#include <wallet/walletdb.h>

#include <exception>
#include <optional>
#include <string_view>
#include <vector>

namespace wallet::paymaster_rpc::internal {

using namespace paymaster_app;

template <typename Callable>
UniValue InvokePaymasterService(Callable&& call)
{
    using namespace DigiDollar::Paymaster;
    try {
        return call();
    } catch (const ApplicationError& failure) {
        int code = RPC_WALLET_ERROR;
        switch (failure.kind) {
        case ErrorKind::WALLET: break;
        case ErrorKind::INVALID_INPUT: code = RPC_INVALID_PARAMETER; break;
        case ErrorKind::DESERIALIZATION: code = RPC_DESERIALIZATION_ERROR; break;
        case ErrorKind::UNLOCK: code = RPC_WALLET_UNLOCK_NEEDED; break;
        case ErrorKind::REJECTED: code = RPC_TRANSACTION_REJECTED; break;
        case ErrorKind::INTERNAL: code = RPC_INTERNAL_ERROR; break;
        case ErrorKind::GENERAL: code = RPC_MISC_ERROR; break;
        case ErrorKind::NODE_CAPACITY: code = RPC_CLIENT_NODE_CAPACITY_REACHED; break;
        case ErrorKind::INSUFFICIENT_FUNDS: code = RPC_WALLET_INSUFFICIENT_FUNDS; break;
        case ErrorKind::INVALID_ADDRESS: code = RPC_INVALID_ADDRESS_OR_KEY; break;
        }
        UniValue response = JSONRPCError(code, failure.what());
        UniValue data{UniValue::VOBJ};
        data.pushKV("paymaster_code", failure.code);
        data.pushKV("disposition", ErrorDispositionName(failure.disposition));
        if (failure.session_absent) data.pushKV("session_absent", true);
        response.pushKV("data", std::move(data));
        throw response;
    }
}

template <typename Callable>
auto PaymasterRpcAdapter(Callable call)
{
    return [call = std::move(call)](const RPCHelpMan& method, const JSONRPCRequest& request) -> UniValue {
        return InvokePaymasterService([&] { return call(method, request); });
    };
}

template <typename T>
bool DeserializePaymasterHex(const UniValue& value,
                             const char* field,
                             T& decoded,
                             std::string& error)
{
    return DeserializePaymasterBytes(ParseHexV(value, field), field, decoded, error);
}


using FinalTransactionPresence = ExactFinalTransactionPresence;


// Written with the signed wallet transaction, before broadcast. This marker
// allows finance reconciliation to recover a missing pool-retirement event.
using wallet::PAYMASTER_RETIREMENT_PROVIDER_KEY;

// Provider lifecycle and readiness helpers. These functions reconcile durable
// wallet state before exposing an operator action or public announcement.
using paymaster_store::internal::ProviderPoolReadError;


/** Caller holds the provider work guard. Flags describe confirmed current targets.
 * Recover any committed transaction before releasing obsolete unsigned refill jobs. */

using wallet::ReconcileProviderFinances;

RPCResult PoolPreparationResultSchema();

// Liquidity planning and accounting. Maintenance builders must operate on a
// reconciled pool and persist restartable plans before broadcasting.


UniValue ProviderLiquidityStatusToJSON(const ProviderReadiness& readiness,
                                       int64_t now, CWallet* wallet = nullptr);

using wallet::GetProviderFinanceTransactionFee;
using wallet::RecordProviderFinanceTransaction;

// RPC presentation and canonical wire encodings. Redacted serializers are the
// only variants suitable for durable client-side diagnostic state.
UniValue ReliabilityToJSON(
    const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
    int64_t now);

/** False means confirmed absence; unreadable or unsupported records throw. */
ClientSessionLookup ParseClientSessionLookup(const UniValue& lookup);
bool FindSession(const UniValue& lookup,
                 PaymasterStore& store,
                 DigiDollar::Paymaster::PaymentSession& session);

std::vector<COutPoint> ParsePaymasterInputs(const UniValue& value);
DigiDollar::Paymaster::ProviderPolicy ParseProviderPolicy(
    const UniValue& value);

DigiDollar::Paymaster::ProviderSafetyPolicy ParseProviderSafetyPolicy(
    const UniValue& value);
UniValue ProviderSafetyClassStatusToJSON(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
    DigiDollar::Paymaster::FundingModel model,
    DigiDollar::Paymaster::SponsorshipScope scope,
    int64_t now);
DigiDollar::Paymaster::ClientSafetyPolicy ParseClientSafetyPolicy(
    const UniValue& value);
std::vector<RPCArg> FundingSafetyLimitArgs();
std::vector<RPCResult> ProviderSafetyClassStatusResults();
std::vector<RPCResult> ProviderSafetyPolicyResults();
std::vector<RPCResult> ProviderLiquidityPolicyResults();
std::vector<RPCResult> ProviderLiquidityStatusResults();

} // namespace wallet::paymaster_rpc::internal

#endif // DIGIBYTE_WALLET_RPC_PAYMASTER_INTERNAL_H
