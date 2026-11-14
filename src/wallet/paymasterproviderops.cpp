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
#include <wallet/paymasterstore_internal.h>
#include <wallet/spend.h>

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

#include <paymaster/error.h>
#include <wallet/paymasterproviderops.h>
#include <wallet/paymasterapplication_internal.h>
#include <wallet/paymasterpool.h>

namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;



uint256 CarrierWithdrawalPlanId(
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Carrier Withdrawal v1");
    hasher << static_cast<uint8_t>(plan.mode)
           << plan.operation_id << plan.source_carriers
           << plan.replacement_carriers
           << plan.excess_script_pub_key << plan.excess_amount
           << plan.estimated_fee
           << plan.target_operational_carriers_after_release
           << plan.liquidity_policy_updated_at
           << plan.created_at << plan.expires_at;
    return hasher.GetSHA256();
}


CarrierWithdrawalResult WithdrawPaymasterCarrier(WalletContext& context, CWallet& wallet_ref, const CarrierWithdrawalParameters& parameters)
{
    auto* wallet = &wallet_ref;
    EnsureProviderRestoreSafe(*wallet);

    const auto mode = parameters.mode;
    const bool execute = parameters.execute;
    if (mode != CarrierWithdrawalMode::ALL_EXCESS && mode != CarrierWithdrawalMode::RELEASE_SLOT)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_MODE");
    ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
    if (!context.paymaster || !readiness.have_identity) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
    }
    ProviderWorkGuard work_guard{
        *context.paymaster, wallet->GetName(),
        readiness.identity.provider_id,
        /*require_running=*/false};
    if (!work_guard.Acquired()) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
    }
    if (!readiness.have_liquidity_policy) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND");
    }
    ProviderLiquidityPolicy liquidity_policy = readiness.liquidity_policy;
    const int64_t now = GetTime();

    const auto provider_is_running = [&] {
        return context.paymaster && readiness.have_identity &&
               context.paymaster->IsProviderRunning(
                   wallet->GetName(), readiness.identity.provider_id);
    };

    if (!execute) {
        ProviderCarrierWithdrawalPlan plan;
        do {
            plan.operation_id = GetRandHash();
        } while (plan.operation_id.IsNull());
        plan.mode = mode;
        plan.liquidity_policy_updated_at = liquidity_policy.updated_at;
        plan.created_at = now;
        plan.expires_at = SaturatingAddSeconds(now, 10 * 60);

        if (mode == CarrierWithdrawalMode::RELEASE_SLOT) {

            if (!parameters.source) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_RELEASE_SLOT_OUTPOINT_REQUIRED");
            const COutPoint source = *parameters.source;
            const auto found = std::find_if(
                readiness.pool_entries.begin(),
                readiness.pool_entries.end(),
                [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint == source;
                });
            if (found == readiness.pool_entries.end() ||
                found->purpose != PoolPurpose::OPERATIONAL ||
                found->asset != PoolAsset::DD_CARRIER ||
                found->state != PoolEntryState::AVAILABLE ||
                found->confirmation_height <= 0 ||
                WITH_LOCK(wallet->cs_wallet, return wallet->IsSpent(found->outpoint))) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
            }
            if (liquidity_policy.target_operational_carriers == 0) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO");
            }
            plan.source_carriers.push_back(source);
            plan.target_operational_carriers_after_release =
                liquidity_policy.target_operational_carriers - 1;
        } else {
            DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
            if (!dd_wallet) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_DD_WALLET_UNAVAILABLE");
            }
            int64_t total_excess{0};
            for (const ProviderPoolEntry& entry :
                 readiness.pool_entries) {
                if (entry.asset != PoolAsset::DD_CARRIER ||
                    entry.state != PoolEntryState::AVAILABLE ||
                    entry.confirmation_height <= 0 ||
                    entry.carrier_value.value <= 100 ||
                    WITH_LOCK(wallet->cs_wallet, return wallet->IsSpent(entry.outpoint))) {
                    continue;
                }
                if (plan.source_carriers.size() >= 64 ||
                    total_excess >
                        std::numeric_limits<int64_t>::max() -
                            (entry.carrier_value.value - 100)) {
                    throw ApplicationError(
                        ErrorKind::WALLET,
                        "PAYMASTER_CARRIER_WITHDRAWAL_TOO_LARGE");
                }
                const auto destination = wallet->GetNewDestination(
                    OutputType::BECH32M,
                    "Paymaster retained carrier");
                if (!destination) {
                    throw ApplicationError(
                        ErrorKind::WALLET,
                        "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                }
                ProviderMaintenanceOutput replacement;
                replacement.purpose = entry.purpose;
                replacement.asset = PoolAsset::DD_CARRIER;
                replacement.script_pub_key =
                    GetScriptForDestination(*destination);
                replacement.carrier_value = DDCents{100};
                plan.source_carriers.push_back(entry.outpoint);
                plan.replacement_carriers.push_back(
                    std::move(replacement));
                total_excess += entry.carrier_value.value - 100;
            }
            if (plan.source_carriers.empty() || total_excess <= 0) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_NO_WITHDRAWABLE_CARRIER_EXCESS");
            }
            const auto excess_destination = wallet->GetNewDestination(
                OutputType::BECH32M,
                "Paymaster carrier fee withdrawal");
            if (!excess_destination) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
            }
            plan.excess_script_pub_key =
                GetScriptForDestination(*excess_destination);
            plan.excess_amount = DDCents{total_excess};

            std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
            const auto append_recipient = [&](const CScript& script,
                                              CAmount amount) {
                CTxDestination destination;
                if (!ExtractDestination(script, destination)) return false;
                const std::string encoded =
                    EncodeDigiDollarAddress(destination);
                CDigiDollarAddress address{encoded};
                if (encoded.empty() || !address.IsValid()) return false;
                recipients.emplace_back(std::move(address), amount);
                return true;
            };
            for (const ProviderMaintenanceOutput& replacement :
                 plan.replacement_carriers) {
                if (!append_recipient(
                        replacement.script_pub_key,
                        replacement.carrier_value.value)) {
                    throw ApplicationError(
                        ErrorKind::WALLET,
                        "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                }
            }
            if (!append_recipient(
                    plan.excess_script_pub_key,
                    plan.excess_amount.value)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
            }
            DDTransferPlan transfer_plan;
            std::string plan_error;
            if (!dd_wallet->PlanFundedDigiDollarTransfer(
                    recipients, transfer_plan, plan_error,
                    &plan.source_carriers,
                    /*allow_paymaster_pool_inputs=*/true)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: " +
                        plan_error);
            }
            plan.estimated_fee = DGBSatoshis{
                transfer_plan.estimated_fee};
            if (plan.estimated_fee.value <= 0 ||
                plan.estimated_fee.value >
                    liquidity_policy.maximum_maintenance_fee_per_transaction.value) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
            }
        }
        plan.plan_id = CarrierWithdrawalPlanId(plan);
        std::string plan_error;
        if (!ValidateProviderCarrierWithdrawalPlan(plan, plan_error) ||
            !SaveCarrierWithdrawalPreview(*wallet, plan, plan_error)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                plan_error.empty()
                    ? "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED"
                    : plan_error);
        }
        CarrierWithdrawalResult result;
        result.executed = false;
        result.plan_id = plan.plan_id;
        result.mode = mode;
        result.source_carriers = plan.source_carriers.size();
        result.expires_at = plan.expires_at;
        if (mode == CarrierWithdrawalMode::ALL_EXCESS) {
            result.withdrawable_excess_cents = plan.excess_amount;
            result.retained_carrier_cents = DDCents{static_cast<int64_t>(
                    plan.replacement_carriers.size()) * 100};
            result.estimated_network_fee_satoshis = plan.estimated_fee.value;
        } else {
            result.operational_carrier_target = plan.target_operational_carriers_after_release;
        }
        return result;
    }

    const auto& plan_id_value = parameters.plan_id;
    if (!plan_id_value) {
        throw ApplicationError(
            ErrorKind::INVALID_INPUT,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_ID_REQUIRED");
    }
    const uint256 supplied_plan_id =
        *plan_id_value;
    ProviderCarrierWithdrawalPlan plan;
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        if (!batch.ReadPaymasterCarrierWithdrawalPlan(plan)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND");
        }
    }
    if (plan.plan_id != supplied_plan_id || plan.mode != mode ||
        plan.plan_id != CarrierWithdrawalPlanId(plan)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
    }

    // A repeated execution after a successful response loss must
    // return the same terminal result. Check the durable operation
    // before applying preview expiry or policy-revision guards.
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        if (mode == CarrierWithdrawalMode::ALL_EXCESS) {
            ProviderMaintenanceLedger terminal_ledger;
            if (batch.ReadPaymasterMaintenanceLedger(
                    terminal_ledger)) {
                const auto existing = std::find_if(
                    terminal_ledger.records.begin(),
                    terminal_ledger.records.end(),
                    [&](const ProviderMaintenanceRecord& record) {
                        return record.operation_id ==
                                   plan.operation_id &&
                               record.plan_id == plan.plan_id;
                    });
                if (existing != terminal_ledger.records.end() &&
                    (existing->state ==
                         ProviderMaintenanceState::BROADCAST ||
                     existing->state ==
                         ProviderMaintenanceState::CONFIRMED)) {
                    CarrierWithdrawalResult result;
                    result.executed = true;
                    result.plan_id = plan.plan_id;
                    result.mode = mode;
                    result.txid = existing->transaction_id;
                    result.withdrawable_excess_cents = plan.excess_amount;
                    result.retained_carrier_cents = DDCents{static_cast<int64_t>(
                            plan.replacement_carriers.size()) *
                            100};
                    result.estimated_network_fee_satoshis = existing->actual_fee.value;
                    return result;
                }
            } else if (batch.HasPaymasterMaintenanceLedger()) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            }
        } else {
            ProviderLiquidityPolicy terminal_policy;
            std::vector<ProviderPoolEntry> terminal_pool;
            if (batch.ReadPaymasterLiquidityPolicy(
                    terminal_policy) &&
                batch.ReadPaymasterProviderPool(terminal_pool)) {
                const auto source = std::find_if(
                    terminal_pool.begin(), terminal_pool.end(),
                    [&](const ProviderPoolEntry& entry) {
                        return entry.outpoint ==
                                   plan.source_carriers.front() &&
                               entry.state ==
                                   PoolEntryState::RELEASED;
                    });
                if (source != terminal_pool.end() &&
                    terminal_policy.target_operational_carriers ==
                        plan.target_operational_carriers_after_release) {
                    CarrierWithdrawalResult result;
                    result.executed = true;
                    result.plan_id = plan.plan_id;
                    result.mode = mode;
                    result.operational_carrier_target = terminal_policy
                            .target_operational_carriers;
                    return result;
                }
            }
        }
    }
    if (plan.expires_at < now ||
        plan.liquidity_policy_updated_at !=
            liquidity_policy.updated_at) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
    }

    if (mode == CarrierWithdrawalMode::RELEASE_SLOT) {
        if (provider_is_running()) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_PROVIDER_MUST_BE_STOPPED");
        }
        {
            LOCK(wallet->cs_wallet);
            WalletBatch batch{wallet->GetDatabase()};
            ProviderLiquidityPolicy current_policy;
            ProviderCarrierWithdrawalPlan current_plan;
            std::vector<ProviderPoolEntry> pool;
            if (!batch.ReadPaymasterLiquidityPolicy(current_policy) ||
                !batch.ReadPaymasterCarrierWithdrawalPlan(current_plan) ||
                !batch.ReadPaymasterProviderPool(pool) ||
                current_plan.plan_id != plan.plan_id ||
                current_policy.updated_at !=
                    plan.liquidity_policy_updated_at ||
                current_policy.target_operational_carriers !=
                    plan.target_operational_carriers_after_release + 1) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
            }
            const auto source = std::find_if(
                pool.begin(), pool.end(),
                [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint ==
                           plan.source_carriers.front();
                });
            if (source == pool.end() ||
                source->purpose != PoolPurpose::OPERATIONAL ||
                source->asset != PoolAsset::DD_CARRIER ||
                source->state != PoolEntryState::AVAILABLE ||
                source->confirmation_height <= 0 ||
                wallet->IsSpent(source->outpoint)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
            }
            source->state = PoolEntryState::RELEASED;
            source->reservation_id.SetNull();
            source->updated_at = now;
            current_policy.target_operational_carriers =
                plan.target_operational_carriers_after_release;
            if (current_policy.updated_at ==
                std::numeric_limits<int64_t>::max()) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_LIQUIDITY_POLICY_TIME_EXHAUSTED");
            }
            current_policy.updated_at = std::max(
                now, current_policy.updated_at + 1);
            std::string policy_error;
            if (!ValidateProviderLiquidityPolicy(
                    current_policy, policy_error)) {
                throw ApplicationError(
                    ErrorKind::WALLET, policy_error);
            }
            if (!batch.TxnBegin()) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_DATABASE_BEGIN");
            }
            if (!batch.WritePaymasterProviderPool(pool) ||
                !batch.WritePaymasterLiquidityPolicy(
                    current_policy)) {
                batch.TxnAbort();
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_DATABASE_WRITE");
            }
            if (!batch.TxnCommit()) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_DATABASE_COMMIT");
            }
            liquidity_policy = current_policy;
        }
        CarrierWithdrawalResult result;
        result.executed = true;
        result.plan_id = plan.plan_id;
        result.mode = mode;
        result.operational_carrier_target = liquidity_policy.target_operational_carriers;
        return result;
    }

    if (wallet->IsLocked()) throw ApplicationError(ErrorKind::UNLOCK, "PAYMASTER_WALLET_LOCKED", ErrorDisposition::USER_DECISION);
    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    if (!dd_wallet) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_DD_WALLET_UNAVAILABLE");
    }
    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        ProviderLiquidityPolicy current_policy;
        ProviderCarrierWithdrawalPlan current_plan;
        if (!batch.ReadPaymasterLiquidityPolicy(current_policy) ||
            !batch.ReadPaymasterCarrierWithdrawalPlan(current_plan) ||
            !batch.ReadPaymasterProviderPool(pool) ||
            current_plan.plan_id != plan.plan_id ||
            current_policy.updated_at !=
                plan.liquidity_policy_updated_at) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
        }
        liquidity_policy = current_policy;
        if (!batch.ReadPaymasterMaintenanceLedger(ledger)) {
            if (batch.HasPaymasterMaintenanceLedger()) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            }
            ledger.accounting_time_high_water = now;
        }
        const auto existing = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [&](const ProviderMaintenanceRecord& record) {
                return record.operation_id == plan.operation_id;
            });
        if (existing != ledger.records.end() &&
            (existing->state == ProviderMaintenanceState::BROADCAST ||
             existing->state == ProviderMaintenanceState::CONFIRMED)) {
            CarrierWithdrawalResult result;
            result.executed = true;
            result.plan_id = plan.plan_id;
            result.mode = mode;
            result.txid = existing->transaction_id;
            result.withdrawable_excess_cents = plan.excess_amount;
            result.estimated_network_fee_satoshis = existing->actual_fee.value;
            return result;
        }
        std::string lock_error;
        if (!CheckPaymasterNewSignatureInputs(*wallet, plan.source_carriers, lock_error, InputRole::PROVIDER_CARRIER)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_WITHDRAWAL_FAILED: " + lock_error);
        }
    }

    std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
    const auto append_recipient = [&](const CScript& script,
                                      CAmount amount) {
        CTxDestination destination;
        if (!ExtractDestination(script, destination)) return false;
        const std::string encoded =
            EncodeDigiDollarAddress(destination);
        CDigiDollarAddress address{encoded};
        if (encoded.empty() || !address.IsValid()) return false;
        recipients.emplace_back(std::move(address), amount);
        return true;
    };
    for (const ProviderMaintenanceOutput& replacement :
         plan.replacement_carriers) {
        if (!append_recipient(
                replacement.script_pub_key,
                replacement.carrier_value.value)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
        }
    }
    if (!append_recipient(
            plan.excess_script_pub_key,
            plan.excess_amount.value)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
    }
    DDTransferPlan transfer_plan;
    std::string transfer_error;
    if (!dd_wallet->PlanFundedDigiDollarTransfer(
            recipients, transfer_plan, transfer_error,
            &plan.source_carriers,
            /*allow_paymaster_pool_inputs=*/true) ||
        transfer_plan.estimated_fee != plan.estimated_fee.value) {
        throw ApplicationError(
            ErrorKind::WALLET,
            transfer_error.empty()
                ? "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED"
                : "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: " +
                      transfer_error);
    }

    ProviderMaintenanceRecord operation;
    operation.operation_id = plan.operation_id;
    operation.plan_id = plan.plan_id;
    operation.kind =
        ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS;
    operation.outputs = plan.replacement_carriers;
    operation.source_inputs = plan.source_carriers;
    operation.withdrawal_excess_script_pub_key =
        plan.excess_script_pub_key;
    operation.withdrawal_excess_amount = plan.excess_amount;
    operation.maximum_fee =
        liquidity_policy.maximum_maintenance_fee_per_transaction;
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        ProviderLiquidityPolicy current_policy;
        ProviderCarrierWithdrawalPlan current_plan;
        ProviderMaintenanceLedger current_ledger;
        std::vector<ProviderPoolEntry> current_pool;
        if (!batch.ReadPaymasterLiquidityPolicy(current_policy) ||
            !batch.ReadPaymasterCarrierWithdrawalPlan(current_plan) ||
            !batch.ReadPaymasterProviderPool(current_pool) ||
            current_plan.plan_id != plan.plan_id ||
            current_policy.updated_at !=
                plan.liquidity_policy_updated_at) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
        }
        if (!batch.ReadPaymasterMaintenanceLedger(current_ledger)) {
            if (batch.HasPaymasterMaintenanceLedger()) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            }
            current_ledger.accounting_time_high_water = now;
        }
        operation.maximum_fee =
            current_policy
                .maximum_maintenance_fee_per_transaction;
        if (!CheckPaymasterNewSignatureInputs(*wallet, plan.source_carriers, transfer_error, InputRole::PROVIDER_CARRIER)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_WITHDRAWAL_FAILED: " + transfer_error);
        }
        if (!ReserveProviderMaintenanceBudget(
                current_ledger, current_policy, operation, now,
                transfer_error)) {
            throw ApplicationError(ErrorKind::WALLET, transfer_error);
        }
        for (const COutPoint& source_outpoint :
             plan.source_carriers) {
            const auto source = std::find_if(
                current_pool.begin(), current_pool.end(),
                [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint == source_outpoint;
                });
            const bool already_reserved =
                source != current_pool.end() &&
                source->state == PoolEntryState::RESERVED &&
                source->reservation_id == plan.operation_id;
            if (source == current_pool.end() ||
                source->asset != PoolAsset::DD_CARRIER ||
                source->confirmation_height <= 0 ||
                wallet->IsSpent(source_outpoint) ||
                (source->state != PoolEntryState::AVAILABLE &&
                 !already_reserved)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
            }
            source->state = PoolEntryState::RESERVED;
            source->reservation_id = plan.operation_id;
            source->updated_at = now;
        }
        if (!paymaster_store::internal::MakeProviderPoolRoom(
                current_pool,
                plan.replacement_carriers.size(),
                transfer_error)) {
            throw ApplicationError(
                ErrorKind::WALLET, transfer_error);
        }
        if (!batch.TxnBegin()) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_DATABASE_BEGIN");
        }
        if (!batch.WritePaymasterMaintenanceLedger(
                current_ledger) ||
            !batch.WritePaymasterProviderPool(current_pool)) {
            batch.TxnAbort();
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.TxnCommit()) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_DATABASE_COMMIT");
        }
        liquidity_policy = current_policy;
        ledger = std::move(current_ledger);
        pool = std::move(current_pool);
    }

    std::vector<ProviderMaintenanceTransaction> transactions;
    const DDMaintenanceTransfer transfer{transfer_plan, plan.excess_script_pub_key,
        "Paymaster carrier excess withdrawal", recipients.size() > 1};
    if (!ExecuteProviderMaintenanceTransactions(*wallet,
            CarrierWithdrawalMaintenanceAuthority{operation, plan, liquidity_policy},
            {transfer}, transactions, transfer_error)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_WITHDRAWAL_FAILED: " + transfer_error);
    }
    CarrierWithdrawalResult result;
    result.executed = true;
    result.plan_id = plan.plan_id;
    result.mode = mode;
    result.txid = transactions.front().transaction->GetHash();
    result.withdrawable_excess_cents = plan.excess_amount;
    result.retained_carrier_cents = DDCents{static_cast<int64_t>(plan.replacement_carriers.size()) * 100};
    result.estimated_network_fee_satoshis = transactions.front().fee.value;
    return result;
}

PoolPreparationResult PreparePaymasterPool(WalletContext& context, CWallet& wallet_ref, const PoolPreparationParameters& parameters)
{
    auto* wallet = &wallet_ref;
    EnsureProviderRestoreSafe(*wallet);
    wallet->BlockUntilSyncedToCurrentChain();
    ProviderIdentityRecord identity;
    std::optional<ProviderWorkGuard> pool_guard;
    if (context.paymaster && context.paymaster->Enabled() &&
        GetPaymasterIdentity(*wallet, identity)) {
        // Manual previews and executions share the scheduler's
        // wallet-local exclusion slot. This prevents two callers, or
        // automatic replenishment and an expert action, from deriving
        // and committing changes from the same stale pool snapshot.
        pool_guard.emplace(
            *context.paymaster, wallet->GetName(),
            identity.provider_id, /*require_running=*/false);
        if (!pool_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
        }
    }

    const int admission = parameters.admission;
    const int operational = parameters.operational;
    const int admission_carriers = parameters.admission_carriers;
    const int operational_carriers = parameters.operational_carriers;
    const bool execute = parameters.execute;
    if (admission < static_cast<int>(REQUIRED_ADMISSION_SLOTS) || admission > 16 ||
        operational < 1 || operational > 16 || admission_carriers < 0 ||
        admission_carriers > 16 || operational_carriers < 0 || operational_carriers > 16) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_POOL_TARGET");
    }
    const int64_t fee_limit = parameters.maximum_fee;
    if (fee_limit <= 0 || !MoneyRange(fee_limit) || fee_limit > MAX_MONEY / 2) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_INVALID_FEE_LIMIT");
    }
    const bool proposed_policy = parameters.preview_policy.has_value();
    if (proposed_policy && (execute || parameters.plan_id.has_value() || parameters.cancel)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_SETUP_PREVIEW_ONLY");
    }
    ProviderPolicy policy;
    if (proposed_policy) {
        policy = *parameters.preview_policy;
        std::string policy_error;
        if (!ValidateProviderPolicy(policy, policy_error)) throw ApplicationError(ErrorKind::INVALID_INPUT, policy_error);
    } else if (!GetPaymasterProviderPolicy(*wallet, policy)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POLICY_NOT_FOUND");
    }
    const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
    if (user_paid && (admission_carriers < static_cast<int>(REQUIRED_ADMISSION_SLOTS) ||
                      operational_carriers < 1)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
    }
    if (!user_paid && (admission_carriers != 0 || operational_carriers != 0)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
    }
    std::vector<ProviderPoolEntry> existing;
    std::string refresh_error;
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        if (!ReadOptionalProviderPool(batch, existing, refresh_error)) {
            throw ApplicationError(ErrorKind::WALLET, refresh_error);
        }
    }
    if (!existing.empty() &&
        !RefreshPoolConfirmationHeights(*wallet, existing, refresh_error, /*persist=*/execute)) {
        throw ApplicationError(ErrorKind::WALLET, refresh_error);
    }
    constexpr int64_t admission_value{MIN_ADMISSION_DGB_SATOSHIS};
    constexpr int64_t carrier_value{100};
    const int64_t operational_value = std::max<int64_t>(
        MIN_ADMISSION_DGB_SATOSHIS, policy.maximum_network_fee.value);
    const auto live_count = [&](PoolPurpose purpose, PoolAsset asset,
                                int64_t minimum_dgb_value = 0) {
        return static_cast<int>(std::count_if(existing.begin(), existing.end(),
                                              [&](const ProviderPoolEntry& entry) {
                                                  return entry.purpose == purpose && entry.asset == asset &&
                                                         (asset != PoolAsset::DGB ||
                                                          entry.dgb_value.value >= minimum_dgb_value) &&
                                                         (purpose != PoolPurpose::OPERATIONAL || asset != PoolAsset::DD_CARRIER ||
                                                          IsProviderCarrierUsable(entry.carrier_value, &policy)) &&
                                                         (entry.state == PoolEntryState::AVAILABLE ||
                                                          entry.state == PoolEntryState::RESERVED ||
                                                          entry.state == PoolEntryState::PENDING_SUCCESSOR);
                                              }));
    };
    const int missing_admission = std::max(
        0, admission - live_count(PoolPurpose::ADMISSION,
                                  PoolAsset::DGB, admission_value));
    const int missing_operational = std::max(
        0, operational - live_count(PoolPurpose::OPERATIONAL,
                                    PoolAsset::DGB,
                                    operational_value));
    const int missing_admission_carriers = std::max(0, admission_carriers - live_count(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER));
    const int missing_operational_carriers = std::max(0, operational_carriers - live_count(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER));
    const auto checked_product = [](int64_t value, int count) {
        if (value < 0 || count < 0 ||
            (count != 0 &&
             value > std::numeric_limits<int64_t>::max() / count)) {
            return std::optional<int64_t>{};
        }
        return std::optional<int64_t>{value * count};
    };
    const auto admission_total = checked_product(
        admission_value, missing_admission);
    const auto operational_total = checked_product(
        operational_value, missing_operational);
    std::optional<int64_t> total;
    if (admission_total && operational_total) {
        total = CheckedAdd(*admission_total, *operational_total);
    }
    // This preview feeds one wallet transaction. Reject arithmetic
    // overflow and aggregate values outside DigiByte's monetary range
    // before deriving destinations or unlocking the wallet.
    if (!total || !MoneyRange(*total)) {
        throw ApplicationError(
            ErrorKind::INVALID_INPUT,
            "PAYMASTER_POOL_VALUE_OUT_OF_RANGE");
    }
    // Index synchronization may drain wallet validation callbacks.
    // Never wait for that queue while holding cs_wallet.
    const bool txindex_ready = !execute || ProviderTxIndexIsReady(*wallet, /*wait_for_sync=*/true);
    LOCK(wallet->cs_wallet);
    auto ledger = ReadPreparationLedger(*wallet);
    auto setup_authorization = ReadPoolSetupAuthorization(*wallet);
    const uint256 authorization = PoolPreparationAuthorization(*wallet);
    if (authorization.IsNull() && !proposed_policy) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_IDENTITY_NOT_FOUND");
    const bool pending = std::any_of(ledger.records.begin(), ledger.records.end(), [](const auto& record) {
        return record.IsPreparation() && record.state != ProviderMaintenanceState::CONFIRMED &&
               record.state != ProviderMaintenanceState::RELEASED;
    });
    // Version 2 binds typed values instead of JSON formatting/key order.
    HashWriter request_hasher = TaggedHash("DigiByte Paymaster Pool Request v2");
    request_hasher << admission << operational << admission_carriers << operational_carriers << fee_limit
                   << parameters.recovery_transaction.value_or(uint256{});
    request_hasher << bool(parameters.recovery_outputs);
    if (parameters.recovery_outputs) {
        request_hasher << uint64_t(parameters.recovery_outputs->size());
        for (const auto& output : *parameters.recovery_outputs) request_hasher << output.index << uint8_t(output.purpose);
    }
    const uint256 request_hash = request_hasher.GetSHA256();
    const uint256 requested_plan = parameters.plan_id.value_or(uint256{});
    if (execute && !parameters.cancel && setup_authorization.plan_id == requested_plan &&
        setup_authorization.revoked) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_PLAN_CANCELLED");
    }
    const bool replay = execute && std::any_of(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
        return record.IsPreparation() && record.plan_id == requested_plan && record.preparation_request == request_hash &&
               record.preparation_authorization == authorization;
    });
    if (execute && !parameters.cancel && !replay && !requested_plan.IsNull() &&
        setup_authorization.plan_id == requested_plan) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_PLAN_NOT_RETAINED");
    }
    CTransactionRef recovery_tx;
    std::vector<ProviderMaintenanceOutput> recovery_outputs;
    const auto& recovery_id = parameters.recovery_transaction;
    const auto& recovery_mapping = parameters.recovery_outputs;
    if (recovery_id.has_value() != recovery_mapping.has_value()) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_MAPPING_REQUIRED");
    }
    if (recovery_id.has_value() && !replay) {
        LOCK(wallet->cs_wallet);
        const auto found = wallet->mapWallet.find(*recovery_id);
        if (found == wallet->mapWallet.end() || !found->second.tx ||
            found->second.isAbandoned() || found->second.isConflicted() ||
            IsDigiDollarTransaction(*found->second.tx)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_TRANSACTION_INVALID");
        }
        recovery_tx = found->second.tx;
        // Adoption authorizes only a wallet-owned self-transfer. It must
        // not turn a foreign, client, or provider payment into setup.
        for (const auto& input : recovery_tx->vin) {
            if (!(wallet->IsMine(input.prevout) & ISMINE_SPENDABLE) || IsPaymasterInputReserved(*wallet, input.prevout) ||
                std::any_of(existing.begin(), existing.end(), [&](const auto& entry) { return entry.outpoint == input.prevout; })) {
                throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_INPUT_INVALID");
            }
        }
        for (const auto& output : recovery_tx->vout) {
            if (!(wallet->IsMine(output) & ISMINE_SPENDABLE)) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_OUTPUT_INVALID");
        }
        int recovered_admission{0}, recovered_operational{0};
        std::set<uint32_t> indices;
        for (const auto& item : *recovery_mapping) {
            const auto index = item.index;
            const auto purpose = item.purpose;
            if (index >= recovery_tx->vout.size() || !indices.insert(index).second ||
                (purpose != PoolPurpose::ADMISSION && purpose != PoolPurpose::OPERATIONAL) ||
                wallet->IsSpent(COutPoint{recovery_tx->GetHash(), index})) {
                throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_OUTPUT_INVALID");
            }
            const auto& output = recovery_tx->vout[index];
            const bool admission_output = purpose == PoolPurpose::ADMISSION;
            if (output.nValue != (admission_output ? admission_value : operational_value)) {
                throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_VALUE_MISMATCH");
            }
            ProviderMaintenanceOutput mapped;
            mapped.purpose = admission_output ? PoolPurpose::ADMISSION : PoolPurpose::OPERATIONAL;
            mapped.asset = PoolAsset::DGB;
            mapped.script_pub_key = output.scriptPubKey;
            mapped.dgb_value = DGBSatoshis{output.nValue};
            recovery_outputs.push_back(std::move(mapped));
            admission_output ? ++recovered_admission : ++recovered_operational;
        }
        if (recovered_admission != missing_admission || recovered_operational != missing_operational || recovery_outputs.empty()) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_RECOVERY_TARGET_MISMATCH");
        }
        const auto fee = GetProviderFinanceTransactionFee(*wallet, recovery_tx);
        if (!fee || fee->value > fee_limit) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_FEE_LIMIT");
    }
    const int64_t total_carriers = carrier_value *
                                   (missing_admission_carriers + missing_operational_carriers);
    HashWriter plan_hasher = TaggedHash(
        "DigiByte Paymaster Pool Preparation Plan v2");
    plan_hasher << Params().GenesisBlock().GetHash()
                << GetProviderPolicyHash(policy)
                << setup_authorization.generation
                << admission << operational
                << admission_carriers << operational_carriers
                << missing_admission << missing_operational
                << missing_admission_carriers
                << missing_operational_carriers
                << admission_value << operational_value
                << carrier_value << *total << total_carriers;
    // Bind the existing inventory as well as deficit counts. After
    // slots are spent, a new explicit setup must not alias an old,
    // completed command with the same targets and numeric deficits.
    for (const auto& entry : existing) {
        plan_hasher << entry.outpoint << static_cast<uint8_t>(entry.state)
                    << static_cast<uint8_t>(entry.purpose) << static_cast<uint8_t>(entry.asset)
                    << entry.dgb_value.value << entry.carrier_value.value;
    }
    // The permanent generation, rather than removable history, prevents a
    // cancelled/completed plan from becoming a new approval after pruning.
    plan_hasher << fee_limit << authorization << recovery_outputs;
    if (recovery_tx) plan_hasher << recovery_tx->GetWitnessHash();
    if (proposed_policy) plan_hasher << std::string{"setup preview only"};
    // A lost execute response must be recoverable by another preview.
    // Reuse only an outstanding plan with the same request and current
    // authorization; changed targets/policy never inherit its approval.
    const auto pending_match = std::find_if(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
        return record.IsPreparation() && record.state != ProviderMaintenanceState::CONFIRMED &&
               record.state != ProviderMaintenanceState::RELEASED &&
               record.preparation_request == request_hash && record.preparation_authorization == authorization;
    });
    const bool resume_preview = !execute && !proposed_policy && pending_match != ledger.records.end();
    const uint256 plan_id = resume_preview ? pending_match->plan_id : plan_hasher.GetSHA256();
    PoolPreparationResult result;
    result.plan_id = plan_id;
    result.preview_only = proposed_policy;
    result.admission_dgb_slots = admission;
    result.operational_dgb_slots = operational;
    result.admission_carrier_slots = admission_carriers;
    result.operational_carrier_slots = operational_carriers;
    result.missing_admission_dgb_slots = missing_admission;
    result.missing_operational_dgb_slots = missing_operational;
    result.missing_admission_carrier_slots = missing_admission_carriers;
    result.missing_operational_carrier_slots = missing_operational_carriers;
    result.admission_dgb_satoshis_each = admission_value;
    result.operational_dgb_satoshis_each = operational_value;
    result.carrier_cents_each = DDCents{carrier_value};
    result.total_output_satoshis = *total;
    result.total_carrier_cents = DDCents{total_carriers};
    result.maximum_fee_satoshis = fee_limit;
    int64_t total_fee_limit = fee_limit *
        ((missing_admission + missing_operational > 0) +
         (missing_admission_carriers + missing_operational_carriers > 0));
    if (replay || resume_preview) {
        // Report the original authorization, even after one or both
        // asset steps no longer appear in the current deficit counts.
        total_fee_limit = 0;
        for (const auto& record : ledger.records) {
            if (!record.IsPreparation() || record.plan_id != (replay ? requested_plan : plan_id)) continue;
            const auto combined = CheckedAdd(total_fee_limit, record.maximum_fee.value);
            if (!combined || !MoneyRange(*combined)) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            total_fee_limit = *combined;
        }
    }
    result.maximum_total_fee_satoshis = total_fee_limit;
    result.accepted = false;
    result.cancelled = false;
    result.preparation = CapturePoolPreparation(*wallet);
    if (!execute) {
        result.executed = false;
        return result;
    }

    const auto& supplied_plan = parameters.plan_id;
    if (!supplied_plan) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "PAYMASTER_POOL_PLAN_REQUIRED");
    }
    const uint256 supplied_id = *supplied_plan;
    if (parameters.cancel) {
        size_t recovered{0};
        std::string error;
        if (!ReconcileProviderMaintenance(*wallet, recovered, error)) throw ApplicationError(ErrorKind::WALLET, error);
        ledger = ReadPreparationLedger(*wallet);
        bool found{false};
        std::vector<uint256> operation_ids;
        for (const auto& record : ledger.records) {
            if (!record.IsPreparation() || record.plan_id != supplied_id) continue;
            found = true;
            if (record.state == ProviderMaintenanceState::PLANNED && record.transaction_id.IsNull())
                operation_ids.push_back(record.operation_id);
        }
        if (!found) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_PLAN_NOT_FOUND");
        const bool cancelled = !operation_ids.empty();
        if (cancelled) {
            ProviderMaintenanceLedger proposed;
            int64_t released_fee{0};
            if (!PreparePaymasterMaintenanceCancellation(*wallet, ledger, operation_ids, GetTime(),
                    proposed, released_fee, error)) throw ApplicationError(ErrorKind::WALLET, error);
            WritePreparationLedger(*wallet, proposed);
        }
        result.cancelled = cancelled;
        result.executed = false;
        result.plan_id = supplied_id;
        result.preparation = CapturePoolPreparation(*wallet);
        return result;
    }
    const bool already_authorized = replay;
    if (already_authorized && std::any_of(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
            return record.IsPreparation() && record.plan_id == supplied_id && record.state == ProviderMaintenanceState::RELEASED;
        })) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_PLAN_CANCELLED");
    if (!already_authorized && supplied_id != plan_id) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_POOL_PLAN_CHANGED");
    if (pending && !already_authorized) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOL_PREPARATION_PENDING");
    if (!context.paymaster || !context.paymaster->Enabled() || !pool_guard || !pool_guard->Acquired()) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DISABLED");
    }
    ProviderSettings settings;
    if (!GetPaymasterProviderSettings(*wallet, settings)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_SETTINGS_UNAVAILABLE");
    }
    // Approval is durable while disabled; execution still checks the
    // enabled flag. The setup wizard saves this approval before it
    // applies the operator's final runtime and enabled settings.
    if (!already_authorized) {
        const size_t new_steps = (missing_admission + missing_operational > 0) +
                                 (missing_admission_carriers + missing_operational_carriers > 0);
        std::string admission_error;
        if (!PrepareProviderMaintenanceAdmission(ledger, new_steps, GetTime(), admission_error)) {
            throw ApplicationError(ErrorKind::WALLET, admission_error);
        }
        if (setup_authorization.generation == std::numeric_limits<uint64_t>::max()) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SETUP_GENERATION_EXHAUSTED");
        }
        if (!GetPaymasterIdentity(*wallet, identity)) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_IDENTITY_NOT_FOUND");
        ++setup_authorization.generation;
        setup_authorization.provider_id = identity.provider_id;
        setup_authorization.plan_id = supplied_id;
        setup_authorization.revoked = false;
        if (wallet->IsLocked()) throw ApplicationError(ErrorKind::UNLOCK, "PAYMASTER_WALLET_LOCKED", ErrorDisposition::USER_DECISION);
        const auto append = [&](PoolAsset asset, int admission_count, int operational_count) {
            if (admission_count + operational_count == 0) return;
            ProviderMaintenanceRecord record;
            do { record.operation_id = GetRandHash(); } while (record.operation_id.IsNull());
            record.plan_id = plan_id;
            record.kind = asset == PoolAsset::DGB ? ProviderMaintenanceKind::PREPARE_DGB : ProviderMaintenanceKind::PREPARE_CARRIER;
            record.maximum_fee = DGBSatoshis{fee_limit};
            record.preparation_authorization = authorization;
            record.preparation_request = request_hash;
            if (!settings.enabled) record.preparation_error = "PAYMASTER_PROVIDER_DISABLED";
            record.created_at = record.updated_at = GetTime();
            if (asset == PoolAsset::DGB && recovery_tx) {
                record.outputs = recovery_outputs;
                record.transaction_id = recovery_tx->GetHash();
                record.actual_fee = *GetProviderFinanceTransactionFee(*wallet, recovery_tx);
                record.state = ProviderMaintenanceState::BROADCAST;
            } else {
                for (int i = 0; i < admission_count + operational_count; ++i) {
                    const bool admission_output = i < admission_count;
                    auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                        asset == PoolAsset::DGB ? "Paymaster pool setup" : "Paymaster carrier pool setup");
                    if (!destination) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                    ProviderMaintenanceOutput output;
                    output.asset = asset;
                    output.purpose = admission_output ? PoolPurpose::ADMISSION : PoolPurpose::OPERATIONAL;
                    output.script_pub_key = GetScriptForDestination(*destination);
                    if (asset == PoolAsset::DGB) output.dgb_value = DGBSatoshis{admission_output ? admission_value : operational_value};
                    else output.carrier_value = DDCents{carrier_value};
                    record.outputs.push_back(std::move(output));
                }
            }
            ledger.records.push_back(std::move(record));
        };
        // Save both exact output plans before either transaction can be
        // committed. Only the existing wallet stores signed bytes.
        append(PoolAsset::DD_CARRIER, missing_admission_carriers, missing_operational_carriers);
        append(PoolAsset::DGB, missing_admission, missing_operational);
        CommitPoolPreparation(*wallet, setup_authorization, ledger);
    }
    std::set<uint256> prior_transactions;
    for (const auto& record : ledger.records) {
        if (!record.transaction_id.IsNull()) prior_transactions.insert(record.transaction_id);
    }
    if (settings.enabled && txindex_ready && AutomaticProviderStateIsSynchronized(*wallet)) ContinuePoolPreparation(*wallet);
    bool executed{false};
    for (const auto& record : ReadPreparationLedger(*wallet).records) {
        if (!record.IsPreparation() || record.plan_id != supplied_id || record.transaction_id.IsNull()) continue;
        executed |= prior_transactions.count(record.transaction_id) == 0;
        if (record.kind == ProviderMaintenanceKind::PREPARE_DGB) {
            result.dgb_txid = record.transaction_id;
            result.txid = record.transaction_id;
            result.network_fee_satoshis = record.actual_fee.value;
        } else {
            result.dd_txid = record.transaction_id;
        }
    }
    if (!result.txid && result.dd_txid) result.txid = result.dd_txid;
    result.accepted = true;
    result.preparation = CapturePoolPreparation(*wallet);
    result.executed = executed;
    result.plan_id = supplied_id;
    existing.clear();
    GetPaymasterProviderPoolEntries(*wallet, existing);
    auto pool = std::move(existing);
    result.pool = std::move(pool);
    return result;
}

PoolRebalanceResult RebalancePaymasterPool(WalletContext& context, CWallet& wallet_ref, const PoolRebalanceParameters& parameters)
{
    auto* wallet = &wallet_ref;
    EnsureProviderRestoreSafe(*wallet);
    wallet->BlockUntilSyncedToCurrentChain();
    ProviderIdentityRecord identity;
    std::optional<ProviderWorkGuard> pool_guard;
    if (context.paymaster && context.paymaster->Enabled() &&
        GetPaymasterIdentity(*wallet, identity)) {
        pool_guard.emplace(
            *context.paymaster, wallet->GetName(),
            identity.provider_id, /*require_running=*/false);
        if (!pool_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
        }
    }

    const bool dgb_only = parameters.dgb_only;
    const auto& fee_option = parameters.maximum_fee;
    const bool recommend_fee = parameters.recommend_fee;
    CAmount maximum_fee = !fee_option ? wallet->m_default_max_tx_fee : *fee_option;
    const bool bounded = recommend_fee || dgb_only || fee_option.has_value();
    if (bounded && (maximum_fee <= 0 || !MoneyRange(maximum_fee)))
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_RETIREMENT_FEE_LIMIT");
    const int admission = parameters.admission;
    const int operational = parameters.operational;
    const int admission_carriers = parameters.admission_carriers;
    const int operational_carriers = parameters.operational_carriers;
    const bool execute = parameters.execute;
    if (recommend_fee && execute)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_RETIREMENT_FEE_PREVIEW_ONLY");
    const auto& expected_revision = parameters.expected_revision;
    if (expected_revision.has_value()) {
        ProviderLiquidityPolicy saved;
        if (!GetPaymasterProviderLiquidityPolicy(*wallet, saved) ||
            saved.updated_at != *expected_revision ||
            saved.target_admission_dgb != admission || saved.target_operational_dgb != operational ||
            saved.target_admission_carriers != admission_carriers || saved.target_operational_carriers != operational_carriers)
            throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_LIQUIDITY_POLICY_CHANGED");
    }
    if (admission < static_cast<int>(REQUIRED_ADMISSION_SLOTS) || admission > 16 ||
        operational < 1 || operational > 16 || admission_carriers < 0 ||
        admission_carriers > 16 || operational_carriers < 0 || operational_carriers > 16) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_POOL_TARGET");
    }
    ProviderPolicy policy;
    if (!GetPaymasterProviderPolicy(*wallet, policy)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POLICY_NOT_FOUND");
    }
    const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
    // An exact saved target may deliberately retain fewer carriers
    // after a slot release/model change. Releasing excess neither
    // enables the provider nor establishes payment readiness.
    if (!expected_revision && !dgb_only && user_paid && (admission_carriers < static_cast<int>(REQUIRED_ADMISSION_SLOTS) ||
                      operational_carriers < 1)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
    }
    if (!expected_revision && !dgb_only && !user_paid && (admission_carriers != 0 || operational_carriers != 0)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
    }
    std::vector<ProviderPoolEntry> entries;
    if (!GetPaymasterProviderPoolEntries(*wallet, entries)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOLS_NOT_PREPARED");
    }
    std::string refresh_error;
    if (!RefreshPoolConfirmationHeights(*wallet, entries, refresh_error, /*persist=*/execute)) {
        throw ApplicationError(ErrorKind::WALLET, refresh_error);
    }
    if (std::any_of(entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
            return entry.state == PoolEntryState::RESERVED ||
                   entry.state == PoolEntryState::PENDING_SUCCESSOR ||
                   entry.state == PoolEntryState::COMMITTED;
        })) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_REBALANCE_ACTIVE_RESERVATIONS");
    }

    const auto candidates = [&](PoolPurpose purpose, PoolAsset asset) {
        std::vector<const ProviderPoolEntry*> result;
        for (const ProviderPoolEntry& entry : entries) {
            if (entry.purpose == purpose && entry.asset == asset &&
                entry.state == PoolEntryState::AVAILABLE) {
                result.push_back(&entry);
            }
        }
        // Retain the most useful entries when reducing a target. In
        // particular, a policy increase can leave old operational DGB
        // outputs below the new advertised fee ceiling; those must be
        // retired before larger, still-ready slots.
        std::sort(result.begin(), result.end(), [asset](const auto* a, const auto* b) {
            const int64_t a_value = asset == PoolAsset::DGB
                ? a->dgb_value.value : a->carrier_value.value;
            const int64_t b_value = asset == PoolAsset::DGB
                ? b->dgb_value.value : b->carrier_value.value;
            if (a_value != b_value) return a_value > b_value;
            return a->outpoint < b->outpoint;
        });
        return result;
    };
    const auto admission_dgb = candidates(PoolPurpose::ADMISSION, PoolAsset::DGB);
    const auto operational_dgb = candidates(PoolPurpose::OPERATIONAL, PoolAsset::DGB);
    const auto admission_dd = candidates(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER);
    const auto operational_dd = candidates(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER);
    if (!expected_revision && (admission > static_cast<int>(admission_dgb.size()) ||
        operational > static_cast<int>(operational_dgb.size()) ||
        (!dgb_only && (admission_carriers > static_cast<int>(admission_dd.size()) ||
        operational_carriers > static_cast<int>(operational_dd.size()))))) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_REBALANCE_INCREASE_USE_PREPARE");
    }

    std::vector<const ProviderPoolEntry*> retire_dgb;
    std::vector<const ProviderPoolEntry*> retire_dd;
    const auto retire_tail = [](const auto& available, int keep, auto& retired) {
        // A reviewed saved target is a retention floor, not a request
        // to create missing slots. Another category can have excess
        // even while this one is empty or below its target.
        if (available.size() > static_cast<size_t>(keep))
            retired.insert(retired.end(), available.begin() + keep, available.end());
    };
    retire_tail(admission_dgb, admission, retire_dgb);
    retire_tail(operational_dgb, operational, retire_dgb);
    if (!dgb_only) {
        retire_tail(admission_dd, admission_carriers, retire_dd);
        retire_tail(operational_dd, operational_carriers, retire_dd);
    }
    if (std::any_of(retire_dgb.begin(), retire_dgb.end(), [](const auto* entry) {
            return entry->confirmation_height <= 0;
        }) ||
        std::any_of(retire_dd.begin(), retire_dd.end(), [](const auto* entry) {
            return entry->confirmation_height <= 0;
        })) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_REBALANCE_REQUIRES_CONFIRMED_INPUTS");
    }
    const auto count_retired = [](const auto& retired, PoolPurpose purpose) {
        return static_cast<int>(std::count_if(retired.begin(), retired.end(),
                                              [&](const auto* entry) { return entry->purpose == purpose; }));
    };
    const int retired_admission_dgb = count_retired(retire_dgb, PoolPurpose::ADMISSION);
    const int retired_operational_dgb = count_retired(retire_dgb, PoolPurpose::OPERATIONAL);
    const int retired_admission_dd = count_retired(retire_dd, PoolPurpose::ADMISSION);
    const int retired_operational_dd = count_retired(retire_dd, PoolPurpose::OPERATIONAL);
    const auto checked_pool_sum = [](const auto& selected,
                                     const auto& amount) {
        std::optional<int64_t> total{int64_t{0}};
        for (const ProviderPoolEntry* entry : selected) {
            total = CheckedAdd(*total, amount(*entry));
            if (!total) break;
        }
        return total;
    };
    const auto retired_dgb_value = checked_pool_sum(
        retire_dgb,
        [](const ProviderPoolEntry& entry) {
            return entry.dgb_value.value;
        });
    const auto retired_dd_value = checked_pool_sum(
        retire_dd,
        [](const ProviderPoolEntry& entry) {
            return entry.carrier_value.value;
        });
    if (!retired_dgb_value || !MoneyRange(*retired_dgb_value) ||
        !retired_dd_value || *retired_dd_value < 0) {
        throw ApplicationError(
            ErrorKind::INVALID_INPUT,
            "PAYMASTER_REBALANCE_VALUE_OUT_OF_RANGE");
    }

    CAmount estimated_dgb_fee{0}, estimated_dd_fee{0};
    if (recommend_fee) {
        // Estimate the same output shapes without reserving destinations,
        // signing, or changing either the pool or a spending approval.
        if (!retire_dgb.empty()) {
            CCoinControl coin_control;
            coin_control.m_allow_other_inputs = false;
            coin_control.m_allow_paymaster_pool_inputs = true;
            coin_control.m_min_depth = 1;
            CMutableTransaction transaction;
            for (const auto* entry : retire_dgb) {
                coin_control.Select(entry->outpoint);
                transaction.vin.emplace_back(entry->outpoint);
            }
            transaction.vout.emplace_back(*retired_dgb_value, GetScriptForDestination(WitnessV1Taproot{}));
            LOCK(wallet->cs_wallet);
            const auto size = CalculateMaximumSignedTxSize(CTransaction{transaction}, wallet, &coin_control);
            if (size.vsize <= 0 || size.vsize > std::numeric_limits<unsigned int>::max())
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RETIREMENT_FEE_ESTIMATE_FAILED");
            estimated_dgb_fee = GetMinimumFee(*wallet, static_cast<unsigned int>(size.vsize), coin_control, nullptr);
        }
        if (!retire_dd.empty()) {
            auto* dd_wallet = wallet->GetDDWallet();
            if (!dd_wallet) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DD_WALLET_UNAVAILABLE");
            std::vector<COutPoint> inputs;
            std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
            for (const auto* entry : retire_dd) {
                CTxDestination destination;
                if (!ExtractDestination(entry->script_pub_key, destination))
                    throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                CDigiDollarAddress address{EncodeDigiDollarAddress(destination)};
                if (!address.IsValid()) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                inputs.push_back(entry->outpoint);
                recipients.emplace_back(std::move(address), entry->carrier_value.value);
            }
            DDTransferPlan plan;
            std::string error;
            if (!dd_wallet->PlanFundedDigiDollarTransfer(recipients, plan, error, &inputs, /*allow_paymaster_pool_inputs=*/true))
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RETIREMENT_FEE_ESTIMATE_FAILED: " + error);
            estimated_dd_fee = plan.estimated_fee;
        }
        if (!MoneyRange(estimated_dgb_fee) || !MoneyRange(estimated_dd_fee) ||
            (!retire_dgb.empty() && estimated_dgb_fee == 0) || (!retire_dd.empty() && estimated_dd_fee == 0))
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RETIREMENT_FEE_ESTIMATE_FAILED");
        const CAmount estimate = std::max(estimated_dgb_fee, estimated_dd_fee);
        const CAmount wallet_limit = wallet->m_default_max_tx_fee;
        if (wallet_limit <= 0 || !MoneyRange(wallet_limit) || estimate > wallet_limit)
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RETIREMENT_WALLET_FEE_LIMIT");
        // Ten percent headroom, rounded up to 0.01 DGB. This proposed
        // bound still requires a separate exact-plan execution approval.
        constexpr CAmount increment{COIN / 100};
        const CAmount proposed = ((estimate + (estimate + 9) / 10 + increment - 1) / increment) * increment;
        maximum_fee = std::min(wallet_limit, std::max(!fee_option ? CAmount{0} : maximum_fee, std::max(increment, proposed)));
    }

    HashWriter plan_hasher = TaggedHash(
        "DigiByte Paymaster Pool Rebalance Plan v1");
    plan_hasher << Params().GenesisBlock().GetHash()
                << GetProviderPolicyHash(policy)
                << admission << operational
                << admission_carriers << operational_carriers;
    for (const ProviderPoolEntry* entry : retire_dgb) {
        plan_hasher << entry->outpoint
                    << static_cast<uint8_t>(entry->purpose)
                    << entry->dgb_value.value;
    }
    for (const ProviderPoolEntry* entry : retire_dd) {
        plan_hasher << entry->outpoint
                    << static_cast<uint8_t>(entry->purpose)
                    << entry->carrier_value.value;
    }
    if (dgb_only) plan_hasher << std::string{"dgb_only"} << maximum_fee;
    else if (bounded) plan_hasher << std::string{"bounded_retirement"} << maximum_fee;
    if (expected_revision.has_value()) plan_hasher << std::string{"liquidity_revision"} << *expected_revision;
    const uint256 plan_id = plan_hasher.GetSHA256();

    PoolRebalanceResult result;
    result.plan_id = plan_id;
    result.retired_admission_dgb_slots = retired_admission_dgb;
    result.retired_operational_dgb_slots = retired_operational_dgb;
    result.retired_admission_carrier_slots = retired_admission_dd;
    result.retired_operational_carrier_slots = retired_operational_dd;
    result.retired_dgb_satoshis = *retired_dgb_value;
    result.retired_carrier_cents = DDCents{*retired_dd_value};
    if (recommend_fee) {
        result.estimated_dgb_fee_satoshis = estimated_dgb_fee;
        result.estimated_dd_fee_satoshis = estimated_dd_fee;
        result.wallet_maximum_fee_satoshis = wallet->m_default_max_tx_fee;
    }
    if (bounded) {
        const int transactions = int(!retire_dgb.empty()) + int(!retire_dd.empty());
        if (transactions > 0 && maximum_fee > MAX_MONEY / transactions)
            throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_RETIREMENT_FEE_LIMIT");
        result.maximum_network_fee_satoshis = maximum_fee;
        result.maximum_total_fee_satoshis = maximum_fee * transactions;
    }
    if (!execute) {
        result.executed = false;
        return result;
    }

    const auto& supplied_plan = parameters.plan_id;
    if (!supplied_plan) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "PAYMASTER_POOL_PLAN_REQUIRED");
    }
    if (*supplied_plan != plan_id) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "PAYMASTER_POOL_PLAN_CHANGED");
    }
    if (retire_dgb.empty() && retire_dd.empty()) {
        result.executed = false;
        return result;
    }

    if (!GetPaymasterIdentity(*wallet, identity)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_IDENTITY_NOT_FOUND");
    }
    // Reconcile confirmed setup transactions before checking the pending
    // journal; pool confirmation refresh alone does not advance it.
    size_t recovered{0};
    std::string reconcile_error;
    if (!ReconcileProviderMaintenance(*wallet, recovered, reconcile_error)) {
        throw ApplicationError(ErrorKind::WALLET, reconcile_error);
    }
    // A later refill from an older approved plan must not undo this
    // explicit pool reduction. Cancel unsigned setup work first.
    if (HasPendingPoolPreparation(*wallet)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOL_PREPARATION_PENDING");
    }
    if (wallet->IsLocked()) throw ApplicationError(ErrorKind::UNLOCK, "PAYMASTER_WALLET_LOCKED", ErrorDisposition::USER_DECISION);
    {
        std::vector<COutPoint> sources;
        for (const auto* entry : retire_dgb) sources.push_back(entry->outpoint);
        std::string error;
        if (!CheckPaymasterNewSignatureInputs(*wallet, sources, error)) throw ApplicationError(ErrorKind::WALLET, error);
        sources.clear();
        for (const auto* entry : retire_dd) sources.push_back(entry->outpoint);
        if (!CheckPaymasterNewSignatureInputs(*wallet, sources, error, InputRole::PROVIDER_CARRIER)) throw ApplicationError(ErrorKind::WALLET, error);
    }
    PoolRetirementMaintenanceAuthority authority;
    authority.provider_id = identity.provider_id;
    authority.reviewed_plan = plan_id;
    authority.provider_policy_hash = GetProviderPolicyHash(policy);
    authority.liquidity_revision = expected_revision;
    authority.maximum_fee = DGBSatoshis{bounded ? maximum_fee : wallet->m_default_max_tx_fee};
    for (const auto* entry : retire_dgb) authority.sources.push_back(*entry);
    for (const auto* entry : retire_dd) authority.sources.push_back(*entry);
    std::vector<ProviderMaintenanceTransfer> transfers;
    if (!retire_dd.empty()) {
        DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
        if (!dd_wallet) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DD_WALLET_UNAVAILABLE");
        std::vector<COutPoint> inputs;
        std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
        for (const auto* entry : retire_dd) {
            auto destination = wallet->GetNewDestination(OutputType::BECH32M, "Retired Paymaster carrier");
            if (!destination) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
            CDigiDollarAddress address{EncodeDigiDollarAddress(*destination)};
            if (!address.IsValid()) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
            inputs.push_back(entry->outpoint);
            recipients.emplace_back(std::move(address), entry->carrier_value.value);
        }
        DDTransferPlan plan;
        std::string error;
        if (!dd_wallet->PlanFundedDigiDollarTransfer(recipients, plan, error, &inputs, true))
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CARRIER_REBALANCE_FAILED: " + error);
        transfers.push_back(DDMaintenanceTransfer{plan,
            GetScriptForDestination(recipients.front().first.GetDigiDollarDestination()),
            "Paymaster carrier retirement", recipients.size() > 1});
    }
    if (!retire_dgb.empty()) {
        auto destination = wallet->GetNewDestination(OutputType::BECH32M, "Retired Paymaster DGB");
        if (!destination) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
        DGBMaintenanceTransfer transfer;
        transfer.coin_control.m_allow_other_inputs = false;
        transfer.coin_control.m_allow_paymaster_pool_inputs = true;
        transfer.coin_control.m_min_depth = 1;
        for (const auto* entry : retire_dgb) transfer.coin_control.Select(entry->outpoint);
        transfer.recipients.push_back({*destination, *retired_dgb_value, true});
        transfers.push_back(std::move(transfer));
    }
    std::vector<ProviderMaintenanceTransaction> transactions;
    std::string execution_error;
    if (!ExecuteProviderMaintenanceTransactions(*wallet, authority, transfers, transactions, execution_error))
        throw ApplicationError(ErrorKind::WALLET, execution_error);
    result.executed = true;
    for (size_t index{0}; index < transactions.size(); ++index) {
        if (std::holds_alternative<DDMaintenanceTransfer>(transfers[index])) {
            result.dd_txid = transactions[index].transaction->GetHash();
            result.dd_network_fee_satoshis = transactions[index].fee.value;
        } else {
            result.dgb_txid = transactions[index].transaction->GetHash();
            result.network_fee_satoshis = transactions[index].fee.value;
        }
    }
    std::vector<ProviderPoolEntry> current_pool;
    if (!GetPaymasterProviderPoolEntries(*wallet, current_pool))
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOLS_NOT_PREPARED");
    result.pool = std::move(current_pool);
    return result;
}

} // namespace wallet::paymaster_app
