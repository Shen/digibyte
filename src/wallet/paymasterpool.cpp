// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


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

#include <wallet/paymasterpool.h>
#include <paymaster/error.h>
#include <wallet/paymasterapplication_internal.h>

namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;

uint256 PoolPreparationAuthorization(CWallet& wallet)
{
    ProviderIdentityRecord identity;
    ProviderPolicy policy;
    if (!GetPaymasterIdentity(wallet, identity) || !GetPaymasterProviderPolicy(wallet, policy)) return {};
    HashWriter hash = TaggedHash("DigiByte Paymaster Pool Authorization v1");
    hash << Params().GenesisBlock().GetHash() << identity.provider_id << GetProviderPolicyHash(policy);
    return hash.GetSHA256();
}

ProviderMaintenanceLedger ReadPreparationLedger(CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    ProviderMaintenanceLedger ledger;
    const auto status = WalletBatch{wallet.GetDatabase()}.ReadPaymasterMaintenanceLedgerWithStatus(ledger);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
    }
    return ledger;
}

void WritePreparationLedger(CWallet& wallet, const ProviderMaintenanceLedger& ledger)
{
    LOCK(wallet.cs_wallet);
    if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterMaintenanceLedger(ledger)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DATABASE_WRITE");
    }
}

bool PreparationInputsConfirmed(CWallet& wallet, const CTransaction& tx)
{
    AssertLockHeld(wallet.cs_wallet);
    return std::all_of(tx.vin.begin(), tx.vin.end(), [&](const CTxIn& input) {
        const auto parent = wallet.mapWallet.find(input.prevout.hash);
        return parent != wallet.mapWallet.end() && wallet.GetTxDepthInMainChain(parent->second) > 0;
    });
}

std::string PreparationFeeLimit(const char* phase, CAmount fee, CAmount limit)
{
    return strprintf("PAYMASTER_POOL_FEE_LIMIT: %s fee %s DGB exceeds approved setup limit %s DGB",
                     phase, FormatMoney(fee), FormatMoney(limit));
}

void AddPreparationDiagnostic(UniValue& item, const ProviderMaintenanceRecord& record)
{
    const std::string error = record.state == ProviderMaintenanceState::CONFIRMED ? "" : record.preparation_error;
    const auto separator = error.find(':');
    // Keep the existing stable fee-limit token in error. Optional diagnostic
    // text carries amounts/phase without changing the journal format or caps.
    if (error.rfind("PAYMASTER_POOL_FEE_", 0) == 0 && separator != std::string::npos) {
        item.pushKV("error", error.substr(0, separator));
        item.pushKV("diagnostic", error.substr(separator + 2));
    } else {
        item.pushKV("error", error);
    }
}

// Current funding observations, not a fee quote or a new spending approval.
// Share these between pool and operator RPCs; a saved execution error can be
// older than an incoming payment. No transaction or change key is created here.
void AddPreparationFunding(UniValue& item, CWallet& wallet, const ProviderMaintenanceRecord& record)
{
    if (!record.IsPreparation() || !record.transaction_id.IsNull() ||
        record.state == ProviderMaintenanceState::CONFIRMED ||
        record.state == ProviderMaintenanceState::RELEASED ||
        record.state == ProviderMaintenanceState::FAILED) return;
    LOCK(wallet.cs_wallet);
    CAmount required_dgb{0}, required_dd{0};
    for (const auto& output : record.outputs) {
        required_dgb += record.kind == ProviderMaintenanceKind::PREPARE_DGB ? output.dgb_value.value : 0;
        required_dd += record.kind == ProviderMaintenanceKind::PREPARE_CARRIER ? output.carrier_value.value : 0;
    }
    CCoinControl control;
    control.m_min_depth = 1;
    const CAmount confirmed = AvailableCoins(wallet, &control).GetTotalAmount();
    control.m_min_depth = 0;
    const CAmount available = AvailableCoins(wallet, &control).GetTotalAmount();
    item.pushKV("required_output_satoshis", required_dgb);
    item.pushKV("confirmed_available_satoshis", confirmed);
    item.pushKV("available_including_unconfirmed_satoshis", available);
    item.pushKV("capital_shortfall_satoshis", std::max<CAmount>(0, required_dgb - confirmed));
    item.pushKV("funding_shortfall_upper_bound_satoshis", std::max<CAmount>(0, required_dgb + record.maximum_fee.value - available));
    if (record.kind == ProviderMaintenanceKind::PREPARE_CARRIER) {
        item.pushKV("required_output_cents", required_dd);
        if (const auto* dd_wallet = wallet.GetDDWallet()) {
            const auto balance = dd_wallet->GetDDBalanceSummary();
            item.pushKV("confirmed_available_cents", balance.spendable);
            item.pushKV("pending_cents", balance.pending);
            item.pushKV("capital_shortfall_cents", std::max<CAmount>(0, required_dd - balance.spendable));
        }
    }
}

std::string ExecutePreparationStep(CWallet& wallet, const ProviderMaintenanceRecord& record, PaymasterCheckpointOperation& checkpoint)
{
    LOCK(wallet.cs_wallet);
    ProviderSettings settings;
    if (!GetPaymasterProviderSettings(wallet, settings) || !settings.enabled) return "PAYMASTER_PROVIDER_DISABLED";
    if (record.preparation_authorization != PoolPreparationAuthorization(wallet)) return "PAYMASTER_POOL_POLICY_CHANGED";
    if (!wallet.GetBroadcastTransactions()) return "PAYMASTER_WALLET_BROADCAST_DISABLED";
    if (!record.transaction_id.IsNull()) {
        const auto found = wallet.mapWallet.find(record.transaction_id);
        if (found == wallet.mapWallet.end()) return "PAYMASTER_POOL_TRANSACTION_NOT_IN_WALLET";
        const auto& wtx = found->second;
        if (wtx.isAbandoned() || wtx.isConflicted()) return "PAYMASTER_POOL_TRANSACTION_CONFLICT";
        if (wallet.GetTxDepthInMainChain(wtx) > 0) return {};
        if (wtx.InMempool() || !PreparationInputsConfirmed(wallet, *wtx.tx)) return "PAYMASTER_POOL_WAITING_CONFIRMATION";
        const auto fee = GetProviderFinanceTransactionFee(wallet, wtx.tx);
        if (!fee) return "PAYMASTER_POOL_FEE_INVALID";
        if (fee->value > record.maximum_fee.value) return PreparationFeeLimit("saved", fee->value, record.maximum_fee.value);
        std::string error;
        // Retry only the exact transaction already saved by CommitTransaction.
        if (!wallet.chain().broadcastTransaction(wtx.tx, std::min(record.maximum_fee.value, wallet.m_default_max_tx_fee), true, error)) {
            return "PAYMASTER_POOL_TRANSACTION_REJECTED: " + error;
        }
        return "PAYMASTER_POOL_WAITING_CONFIRMATION";
    }
    std::string restore_error;
    if (!CheckPaymasterProviderRestoreGuard(wallet, restore_error)) return restore_error;
    if (wallet.IsLocked()) return "PAYMASTER_WALLET_LOCKED";
    if (record.kind == ProviderMaintenanceKind::PREPARE_DGB) {
        std::vector<CRecipient> recipients;
        for (const auto& output : record.outputs) {
            CTxDestination destination;
            if (!ExtractDestination(output.script_pub_key, destination)) return "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            recipients.push_back({destination, output.dgb_value.value, false});
        }
        // A stempool-only parent is invisible to the normal fee precheck.
        // Constrain setup funding without changing ordinary wallet selection.
        CCoinControl control;
        control.m_min_depth = 1;
        auto created = CreateTransaction(wallet, recipients, -1, control, true);
        if (!created) return "PAYMASTER_POOL_WAITING_DGB: " + util::ErrorString(created).original;
        if (created->fee > record.maximum_fee.value) return PreparationFeeLimit("signed", created->fee, record.maximum_fee.value);
        std::string error;
        if (!checkpoint.Begin(error)) return error;
        if (!wallet.CommitTransaction(created->tx, {}, {}, &error)) return "PAYMASTER_POOL_TRANSACTION_REJECTED: " + error;
    } else {
        auto* dd_wallet = wallet.GetDDWallet();
        if (!dd_wallet) return "PAYMASTER_DD_WALLET_UNAVAILABLE";
        std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
        for (const auto& output : record.outputs) {
            CTxDestination destination;
            if (!ExtractDestination(output.script_pub_key, destination)) return "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            recipients.emplace_back(CDigiDollarAddress{EncodeDigiDollarAddress(destination)}, output.carrier_value.value);
        }
        DDTransferPlan plan;
        std::string error;
        if (!dd_wallet->PlanFundedDigiDollarTransfer(recipients, plan, error)) return "PAYMASTER_POOL_WAITING_FUNDS: " + error;
        if (plan.estimated_fee <= 0) return "PAYMASTER_POOL_FEE_INVALID";
        if (plan.estimated_fee > record.maximum_fee.value) return PreparationFeeLimit("estimated", plan.estimated_fee, record.maximum_fee.value);
        CMutableTransaction inputs;
        for (const auto& input : plan.dd_utxos)
            inputs.vin.emplace_back(input);
        for (const auto& input : plan.fee_utxos)
            inputs.vin.emplace_back(input);
        if (!PreparationInputsConfirmed(wallet, CTransaction{inputs})) return "PAYMASTER_POOL_WAITING_CONFIRMATION";
        CMutableTransaction transaction;
        if (!dd_wallet->BuildDigiDollarTransfer(plan, transaction, error)) {
            return "PAYMASTER_CARRIER_TRANSACTION_FAILED: " + error;
        }
        const CTransactionRef tx = MakeTransactionRef(std::move(transaction));
        const auto fee = GetProviderFinanceTransactionFee(wallet, tx);
        if (!fee || fee->value <= 0) return "PAYMASTER_POOL_FEE_INVALID";
        if (fee->value > record.maximum_fee.value) return PreparationFeeLimit("signed", fee->value, record.maximum_fee.value);
        if (fee->value > plan.estimated_fee) {
            return strprintf("PAYMASTER_POOL_FEE_ESTIMATE_EXCEEDED: signed fee %s DGB exceeds planned fee %s DGB; approved setup limit %s DGB",
                             FormatMoney(fee->value), FormatMoney(plan.estimated_fee), FormatMoney(record.maximum_fee.value));
        }
        // Check the exact signed fee before commit. Use the wallet primitive
        // directly: a rejected pool transaction must remain recoverable, not
        // enter the ordinary DD send helper's automatic-abandon path.
        if (!checkpoint.Begin(error)) return error;
        if (!wallet.CommitTransaction(tx, {{"comment", "Paymaster carrier pool"}}, {}, &error)) {
            return "PAYMASTER_CARRIER_TRANSACTION_FAILED: " + error;
        }
    }
    return {};
}

// Caller owns the work guard shared with recurring maintenance. Recover wallet
// bytes before every construction, including a commit that failed to broadcast.
void ContinuePoolPreparation(CWallet& wallet)
{
    // Readiness queries also reconcile this journal. Keep their updates out of
    // the read-modify-write interval even though they do not take the work guard.
    LOCK(wallet.cs_wallet);
    size_t recovered{0};
    std::string error;
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) throw ApplicationError(ErrorKind::WALLET, error);
    const auto snapshot = ReadPreparationLedger(wallet);
    for (const auto& record : snapshot.records) {
        if (!record.IsPreparation() || record.state == ProviderMaintenanceState::CONFIRMED || record.state == ProviderMaintenanceState::RELEASED) continue;
        const bool conflict = std::any_of(snapshot.records.begin(), snapshot.records.end(), [&](const auto& other) {
            return other.IsPreparation() && other.plan_id == record.plan_id && other.state == ProviderMaintenanceState::FAILED;
        });
        PaymasterCheckpointOperation checkpoint{wallet};
        error = conflict ? "PAYMASTER_POOL_TRANSACTION_CONFLICT" : ExecutePreparationStep(wallet, record, checkpoint);
        size_t reconciled{0};
        std::string reconcile_error;
        if (!ReconcileProviderMaintenance(wallet, reconciled, reconcile_error)) throw ApplicationError(ErrorKind::WALLET, reconcile_error);
        if (checkpoint.Active() && error.empty() && !checkpoint.Complete(reconcile_error)) throw ApplicationError(ErrorKind::WALLET, reconcile_error);
        auto ledger = ReadPreparationLedger(wallet);
        for (auto& current : ledger.records) {
            if (current.operation_id == record.operation_id && current.preparation_error != error.substr(0, 256)) {
                current.preparation_error = error.substr(0, 256);
                current.updated_at = std::max(current.updated_at, GetTime());
                WritePreparationLedger(wallet, ledger);
                break;
            }
        }
    }
}

bool HasPendingPoolPreparation(CWallet& wallet)
{
    const auto ledger = ReadPreparationLedger(wallet);
    return std::any_of(ledger.records.begin(), ledger.records.end(), [](const auto& record) {
        return record.IsPreparation() && record.state != ProviderMaintenanceState::CONFIRMED &&
               record.state != ProviderMaintenanceState::RELEASED;
    });
}

UniValue PoolPreparationToJSON(CWallet& wallet)
{
    UniValue result{UniValue::VARR};
    for (const auto& record : ReadPreparationLedger(wallet).records) {
        if (!record.IsPreparation()) continue;
        UniValue item{UniValue::VOBJ};
        item.pushKV("plan_id", record.plan_id.GetHex());
        item.pushKV("operation_id", record.operation_id.GetHex());
        item.pushKV("asset", record.kind == ProviderMaintenanceKind::PREPARE_DGB ? "dgb" : "dd_carrier");
        item.pushKV("state", record.state == ProviderMaintenanceState::CONFIRMED ? "complete" : record.state == ProviderMaintenanceState::FAILED ? "conflict" : record.state == ProviderMaintenanceState::RELEASED ? "cancelled" : record.transaction_id.IsNull() ? "pending_creation" : "pending_confirmation");
        AddPreparationDiagnostic(item, record);
        item.pushKV("maximum_fee_satoshis", record.maximum_fee.value);
        item.pushKV("actual_fee_satoshis", record.actual_fee.value);
        if (!record.transaction_id.IsNull()) item.pushKV("txid", record.transaction_id.GetHex());
        AddPreparationFunding(item, wallet, record);
        result.push_back(std::move(item));
    }
    return result;
}

} // namespace wallet::paymaster_app

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_app;
void RunPaymasterPoolPreparation(WalletContext& context, CWallet& wallet)
{
    if (!context.paymaster || !context.paymaster->Enabled()) return;
    std::string restore_error;
    if (!CheckPaymasterProviderRestoreGuard(wallet, restore_error)) return;
    ProviderIdentityRecord identity;
    ProviderSettings settings;
    if (!GetPaymasterIdentity(wallet, identity) || !GetPaymasterProviderSettings(wallet, settings) || !settings.enabled || !AutomaticProviderStateIsSynchronized(wallet)) return;
    ProviderWorkGuard guard{*context.paymaster, wallet.GetName(), identity.provider_id, false};
    if (!guard.Acquired()) return;
    // The per-wallet background boundary reports failures and suppresses
    // evidence pruning for this maintenance pass.
    ContinuePoolPreparation(wallet);
}

} // namespace wallet
