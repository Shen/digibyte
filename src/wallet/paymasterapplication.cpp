// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/paymasterapplication.h>
#include <paymaster/error.h>

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
#include <logging/timer.h>
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
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterfinance.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/paymastermaintenancecache.h>
#include <wallet/spend.h>

#include <util/overflow.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <boost/signals2/connection.hpp>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>

namespace wallet::paymaster_app {
using paymaster_store::internal::PersistedReadError;
using paymaster_store::internal::ProviderPoolReadError;

uint256 MaintenancePlanId(
    DigiDollar::Paymaster::ProviderMaintenanceKind kind,
    const std::vector<DigiDollar::Paymaster::ProviderMaintenanceOutput>& outputs,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Maintenance v1");
    hasher << static_cast<uint8_t>(kind) << outputs
           << policy.version
           << policy.automatic_replenishment
           << policy.paid_maintenance_approved
           << policy.target_admission_dgb
           << policy.target_operational_dgb
           << policy.target_admission_carriers
           << policy.target_operational_carriers
           << policy.maximum_maintenance_fee_per_transaction
           << policy.maximum_maintenance_fee_per_hour
           << policy.maximum_maintenance_fee_per_day
           << policy.updated_at;
    return hasher.GetSHA256();
}

std::optional<uint32_t> FindMaintenanceOutputIndex(
    const CTransaction& transaction,
    const DigiDollar::Paymaster::ProviderMaintenanceOutput& output)
{
    using namespace DigiDollar::Paymaster;
    std::optional<uint32_t> result;
    for (uint32_t index{0}; index < transaction.vout.size(); ++index) {
        const CTxOut& txout = transaction.vout[index];
        const CAmount expected_value = output.asset == PoolAsset::DGB
            ? output.dgb_value.value : 0;
        if (txout.nValue != expected_value ||
            txout.scriptPubKey != output.script_pub_key) {
            continue;
        }
        if (output.asset == PoolAsset::DD_CARRIER) {
            CAmount dd_amount{0};
            if (!DigiDollar::ExtractDDAmountFromTransaction(
                    transaction,
                    COutPoint{transaction.GetHash(), index}, dd_amount) ||
                dd_amount != output.carrier_value.value) {
                continue;
            }
        }
        // Exact maintenance scripts are fresh and may occur only once. A
        // duplicate is ambiguous and must never be registered as a pool slot.
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}

std::optional<uint32_t> FindExactDigiDollarOutputIndex(
    const CTransaction& transaction,
    const CScript& script_pub_key,
    DigiDollar::Paymaster::DDCents amount)
{
    std::optional<uint32_t> result;
    for (uint32_t index{0}; index < transaction.vout.size(); ++index) {
        const CTxOut& txout = transaction.vout[index];
        if (txout.nValue != 0 || txout.scriptPubKey != script_pub_key) {
            continue;
        }
        CAmount dd_amount{0};
        if (!DigiDollar::ExtractDDAmountFromTransaction(
                transaction,
                COutPoint{transaction.GetHash(), index}, dd_amount) ||
            dd_amount != amount.value) {
            continue;
        }
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}
bool ReadOptionalProviderPool(
    WalletBatch& batch,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error)
{
    const DatabaseReadStatus status{
        batch.ReadPaymasterProviderPoolWithStatus(entries)};
    if (status == DatabaseReadStatus::FOUND) return true;
    if (status == DatabaseReadStatus::NOT_FOUND) {
        entries.clear();
        return true;
    }
    error = ProviderPoolReadError(entries);
    return false;
}

const char* ProviderOperationModeName(
    DigiDollar::Paymaster::ProviderOperationMode mode)
{
    using DigiDollar::Paymaster::ProviderOperationMode;
    switch (mode) {
    case ProviderOperationMode::AUTOMATIC:
        return "automatic";
    case ProviderOperationMode::MANUAL:
        return "manual";
    }
    return "invalid";
}

const char* ProviderServiceStateName(
    DigiDollar::Paymaster::ProviderServiceState state)
{
    using DigiDollar::Paymaster::ProviderServiceState;
    switch (state) {
    case ProviderServiceState::STOPPED:
        return "stopped";
    case ProviderServiceState::WAITING_FOR_UNLOCK:
        return "waiting_for_unlock";
    case ProviderServiceState::WAITING_FOR_READINESS:
        return "waiting_for_readiness";
    case ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL:
        return "waiting_for_maintenance_approval";
    case ProviderServiceState::REPLENISHING_LIQUIDITY:
        return "replenishing_liquidity";
    case ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION:
        return "waiting_for_liquidity_confirmation";
    case ProviderServiceState::ACTIVE:
        return "active";
    case ProviderServiceState::MANUAL:
        return "manual";
    case ProviderServiceState::DRAIN_ONLY:
        return "drain_only";
    case ProviderServiceState::FAULT:
        return "error";
    }
    return "error";
}

std::string StableProviderServiceError(std::string error)
{
    if (error.rfind("PAYMASTER_", 0) != 0) {
        return "PAYMASTER_AUTOMATIC_SERVICE_ERROR";
    }
    const size_t separator = error.find_first_of(": \t\r\n");
    if (separator != std::string::npos) error.resize(separator);
    return error;
}

std::optional<interfaces::BlockKey> ResolvePaymasterActiveTip(
    interfaces::Chain& chain, const CBlockLocator& locator)
{
    if (locator.vHave.empty()) return std::nullopt;
    interfaces::BlockKey tip{locator.vHave.front()};
    bool active{false};
    // Resolve the captured hash, never a height that a reorg can invalidate.
    // An inactive first entry must not fall back to an older locator ancestor.
    if (!chain.findBlock(tip.hash, interfaces::FoundBlock().height(tip.height).inActiveChain(active)) ||
        !active || tip.height < 0) {
        return std::nullopt;
    }
    return tip;
}

bool AutomaticProviderStateIsSynchronized(CWallet& wallet)
{
    const auto chain_tip = ResolvePaymasterActiveTip(wallet.chain(), wallet.chain().getTipLocator());
    if (!chain_tip) return false;

    int wallet_height{-1};
    uint256 wallet_tip;
    {
        LOCK(wallet.cs_wallet);
        wallet_height = wallet.GetLastBlockHeight();
        wallet_tip = wallet.GetLastBlockHash();
    }
    if (wallet_height != chain_tip->height || wallet_tip != chain_tip->hash) return false;

    if (!g_txindex) return false;
    const IndexSummary txindex = g_txindex->GetSummary();
    if (!txindex.synced || txindex.best_block_height != chain_tip->height ||
        txindex.best_block_hash != chain_tip->hash) {
        return false;
    }

    const CBlockLocator current_tip = wallet.chain().getTipLocator();
    return !current_tip.vHave.empty() && current_tip.vHave.front() == chain_tip->hash;
}

bool ProviderTxIndexIsReady(CWallet& wallet, bool wait_for_sync)
{
    if (!g_txindex) return false;
    if (wait_for_sync) return g_txindex->BlockUntilSyncedToCurrentChain();

    const auto chain_tip = ResolvePaymasterActiveTip(wallet.chain(), wallet.chain().getTipLocator());
    if (!chain_tip) return false;
    const IndexSummary txindex = g_txindex->GetSummary();
    if (!txindex.synced || txindex.best_block_height != chain_tip->height ||
        txindex.best_block_hash != chain_tip->hash) {
        return false;
    }

    const CBlockLocator current_tip = wallet.chain().getTipLocator();
    return !current_tip.vHave.empty() && current_tip.vHave.front() == chain_tip->hash;
}

bool CapacityContinuationMayProceed(const ProviderReadiness& readiness)
{
    if (readiness.ready) return true;

    // A successful Capacity-v5 proof atomically reserves the exact pool slots
    // and active-quote admission needed by its one follow-up request. Global
    // readiness consequently reports those resources as unavailable (and can
    // report the active-quote ceiling) even though no additional capacity is
    // consumed by promoting that same admission. All other readiness failures
    // remain fatal. The read-only admission preflight and atomic quote commit
    // then enforce the exact unexpired admission and re-evaluate the budget
    // after its transition from RESERVED to PROMOTED.
    return !readiness.errors.empty() &&
           std::all_of(readiness.errors.begin(), readiness.errors.end(),
                       [](const std::string& error) {
                           return error == "PAYMASTER_OPERATIONAL_SLOT_MISSING" ||
                                  error == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED";
                       });
}

bool CapacityRequestMayProceed(const ProviderReadiness& readiness,
                               bool have_stored_response)
{
    // Only an exact stored proof can continue without fresh capacity. Its
    // signature, request indexes, expiry and reservation are checked again by
    // ReserveAndBuildPaymasterCapacityProof before anything is returned.
    return readiness.ready ||
           (have_stored_response && CapacityContinuationMayProceed(readiness));
}

bool CheckPaymasterClientReadiness(CWallet& wallet, WalletContext& context,
                                   std::string& error)
{
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "DigiDollar Paymaster support is disabled";
        return false;
    }
    if (!context.args || context.args->GetIntArg("-prune", 0) != 0) {
        error = "PAYMASTER_REQUIRES_PRUNE_0";
        return false;
    }
    if (!context.args || !context.args->GetBoolArg("-txindex", DEFAULT_TXINDEX)) {
        error = "PAYMASTER_REQUIRES_TXINDEX";
        return false;
    }
    if (context.args && Params().GetChainType() == ChainType::MAIN &&
        context.args->GetBoolArg("-capturemessages", false)) {
        error = "PAYMASTER_MESSAGE_CAPTURE_ENABLED";
        return false;
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman ||
        !(node->connman->GetLocalServices() & NODE_P2P_V2)) {
        error = "PAYMASTER_REQUIRES_V2_TRANSPORT";
        return false;
    }
    if (!context.chain || context.chain->isInitialBlockDownload() ||
        !context.chain->isReadyToBroadcast()) {
        error = "PAYMASTER_NODE_NOT_READY";
        return false;
    }

    if (node->connman->GetPaymasterBudget().outbound == 0) {
        error = "PAYMASTER_NO_LOCAL_DIRECT_CAPACITY";
        return false;
    }
    wallet.BlockUntilSyncedToCurrentChain();
    if (!g_txindex || !g_txindex->BlockUntilSyncedToCurrentChain()) {
        error = "PAYMASTER_REQUIRES_READY_TXINDEX";
        return false;
    }

    int32_t tip_height;
    {
        LOCK(wallet.cs_wallet);
        tip_height = wallet.GetLastBlockHeight();
    }
    const int activation_height =
        Params().GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
    if (tip_height + 1 < activation_height) {
        error = "PAYMASTER_DIGIDOLLAR_NOT_ACTIVE";
        return false;
    }
    return true;
}

DigiDollar::Paymaster::DDCents EffectiveClientServiceFeeCap(
    CWallet& wallet,
    DigiDollar::Paymaster::DDCents requested,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    ClientSafetyPolicy policy;
    if (!GetPaymasterClientSafetyPolicy(wallet, policy)) {
        error = "PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED";
        return DDCents{-1};
    }
    if (!ValidateClientSafetyPolicy(policy, error)) return DDCents{-1};
    if (requested.value < 0) {
        error = "PAYMASTER_CLIENT_FEE_LIMIT_INVALID";
        return DDCents{-1};
    }
    error.clear();
    return DDCents{std::min(requested.value,
                            policy.maximum_service_fee_per_transaction.value)};
}

bool CheckPaymasterPrivacyReadiness(DigiDollar::Paymaster::PrivacyProfile privacy,
                                    WalletContext& context, std::string& error)
{
    using DigiDollar::Paymaster::PrivacyProfile;
    error.clear();
    if (privacy != PrivacyProfile::HIGH) return true;
    if (context.args && context.args->GetBoolArg("-capturemessages", false)) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_CAPTUREMESSAGES_0";
        return false;
    }
    if (fLogIPs) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_LOGIPS_0";
        return false;
    }
    Proxy onion_proxy;
    if (!GetProxy(NET_ONION, onion_proxy)) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_ONION_PROXY";
        return false;
    }
    if (!onion_proxy.randomize_credentials) {
        error = "PAYMASTER_HIGH_PRIVACY_REQUIRES_PROXY_ISOLATION";
        return false;
    }
    return true;
}

bool RefreshPoolConfirmationHeights(CWallet& wallet,
                                    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
                                    std::string& error, bool persist)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    // A caller's observation may predate a concurrent reservation or commit.
    // Only mutate the current authoritative pool under this same wallet lock.
    if (persist && !ReadOptionalProviderPool(batch, entries, error)) return false;
    ProviderMaintenanceLedger maintenance_ledger;
    std::set<COutPoint> active_maintenance_sources;
    if (batch.ReadPaymasterMaintenanceLedger(maintenance_ledger)) {
        std::string validation_error;
        if (!ValidateProviderMaintenanceLedger(
                maintenance_ledger, validation_error)) {
            error = validation_error.empty()
                ? "PAYMASTER_INVALID_MAINTENANCE_LEDGER"
                : validation_error;
            return false;
        }
        for (const ProviderMaintenanceRecord& record :
             maintenance_ledger.records) {
            if (!record.ProtectsSourceInputs()) {
                continue;
            }
            active_maintenance_sources.insert(record.source_inputs.begin(), record.source_inputs.end());
        }
    } else if (batch.HasPaymasterMaintenanceLedger()) {
        error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
        return false;
    }
    bool changed{false};
    for (auto& entry : entries) {
        const auto tx = wallet.mapWallet.find(entry.outpoint.hash);
        const bool active_maintenance_source =
            active_maintenance_sources.count(entry.outpoint) != 0;
        const bool origin_invalid =
            !entry.origin_commit_key.IsNull() &&
            tx != wallet.mapWallet.end() &&
            (tx->second.isAbandoned() || tx->second.isConflicted());
        if (origin_invalid &&
            IsActiveProviderPoolState(entry.state) &&
            !active_maintenance_source) {
            entry.state = PoolEntryState::INVALIDATED;
            entry.confirmation_height = 0;
            entry.reservation_id.SetNull();
            entry.updated_at = GetTime();
            changed = true;
            continue;
        }
        const bool spent = wallet.IsSpent(entry.outpoint);
        if (IsActiveProviderPoolState(entry.state) && spent &&
            !active_maintenance_source) {
            entry.state = entry.state == PoolEntryState::COMMITTED
                ? PoolEntryState::SPENT
                : PoolEntryState::INVALIDATED;
            entry.reservation_id.SetNull();
            entry.updated_at = GetTime();
            changed = true;
        }
        const auto* confirmed = tx == wallet.mapWallet.end() ? nullptr : tx->second.state<TxStateConfirmed>();
        const int32_t confirmation_height = confirmed ? confirmed->confirmed_block_height : 0;
        if (entry.confirmation_height != confirmation_height) {
            entry.confirmation_height = confirmation_height;
            changed = true;
        }
        if (!spent && entry.state == PoolEntryState::PENDING_SUCCESSOR &&
            confirmation_height > 0) {
            entry.state = PoolEntryState::AVAILABLE;
            entry.reservation_id.SetNull();
            entry.updated_at = GetTime();
            changed = true;
        } else if (!spent && entry.state == PoolEntryState::AVAILABLE &&
                   confirmation_height == 0 &&
                   !entry.origin_commit_key.IsNull()) {
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.reservation_id = entry.origin_commit_key;
            entry.updated_at = GetTime();
            changed = true;
        }
    }
    if (persist && changed && !batch.WritePaymasterProviderPool(entries)) {
        error = "PAYMASTER_POOL_CONFIRMATION_UPDATE_FAILED";
        return false;
    }
    return true;
}

ProviderReadiness GetProviderReadiness(CWallet& wallet, WalletContext& context,
                                       bool wait_for_sync, bool reconcile)
{
    ProviderReadiness result;
    std::string checkpoint_error;
    {
        LOCK(wallet.cs_wallet);
        if (!CheckPaymasterCheckpoint(wallet.GetDatabase(), checkpoint_error)) reconcile = false;
    }
    if (reconcile) {
        PaymasterStore store{wallet};
        size_t recovered_successors{0};
        std::string reconciliation_error;
        if (!store.ReconcileProviderPoolSuccessors(
                recovered_successors, reconciliation_error, /*bounded=*/true)) {
            result.errors.push_back(
                reconciliation_error.empty()
                    ? "PAYMASTER_POOL_RECONCILIATION_FAILED"
                    : reconciliation_error);
        }
        size_t recovered_maintenance{0};
        reconciliation_error.clear();
        if (!ReconcileProviderMaintenance(
                wallet, recovered_maintenance, reconciliation_error)) {
            result.errors.push_back(
                reconciliation_error.empty()
                    ? "PAYMASTER_MAINTENANCE_RECONCILIATION_FAILED"
                    : reconciliation_error);
        }
        size_t expired_quotes{0};
        std::string expiry_error;
        if (!store.ExpireProviderQuotes(GetTime(), expired_quotes, expiry_error)) {
            result.errors.push_back(expiry_error.empty() ? "PAYMASTER_QUOTE_EXPIRY_FAILED" : expiry_error);
        }
        size_t expired_capacity_reservations{0};
        expiry_error.clear();
        if (!store.ExpireProviderCapacityReservations(
                GetTime(), expired_capacity_reservations, expiry_error)) {
            result.errors.push_back(
                expiry_error.empty() ? "PAYMASTER_CAPACITY_EXPIRY_FAILED" : expiry_error);
        }
        size_t expired_recoveries{0};
        expiry_error.clear();
        if (!store.ExpireAlternativeRecoveries(
                GetTime(), expired_recoveries, expiry_error)) {
            result.errors.push_back(
                expiry_error.empty() ? "PAYMASTER_RECOVERY_EXPIRY_FAILED" : expiry_error);
        }
    }
    std::string eligibility_error;
    result.wallet_eligible = CheckPaymasterProviderWallet(wallet, eligibility_error);
    std::string restore_error;
    const bool restore_safe = CheckPaymasterProviderRestoreGuard(wallet, restore_error);
    result.have_settings = GetPaymasterProviderSettings(wallet, result.settings);
    result.have_identity = GetPaymasterIdentity(wallet, result.identity);
    result.have_policy = GetPaymasterProviderPolicy(wallet, result.policy);
    result.have_safety_policy = GetPaymasterProviderSafetyPolicy(wallet, result.safety_policy);
    result.have_budget_ledger = GetPaymasterProviderBudgetLedger(wallet, result.budget_ledger);
    result.have_liquidity_policy = GetPaymasterProviderLiquidityPolicy(
        wallet, result.liquidity_policy);
    result.have_maintenance_ledger = GetPaymasterProviderMaintenanceLedger(
        wallet, result.maintenance_ledger);
    result.have_pool = GetPaymasterProviderPoolEntries(wallet, result.pool_entries);
    int32_t tip_height;
    {
        LOCK(wallet.cs_wallet);
        tip_height = wallet.GetLastBlockHeight();
    }
    std::string refresh_error;
    if (!RefreshPoolConfirmationHeights(wallet, result.pool_entries, refresh_error, reconcile)) {
        result.errors.push_back(refresh_error);
    }
    if (result.have_policy && result.have_pool) {
        result.pool = GetPaymasterProviderPoolReadiness(
            wallet, result.pool_entries, result.policy, tip_height, 1);
        result.pool_ready = result.pool.ready;
    }
    std::optional<DigiDollar::Paymaster::DGBSatoshis> network_fee;
    if (result.have_policy) {
        std::string fee_error;
        network_fee = DigiDollar::Paymaster::GetProviderNetworkFee(result.policy, fee_error);
        if (!network_fee) result.errors.push_back(fee_error);
    }
    if (network_fee && result.have_safety_policy && result.have_budget_ledger) {
        CScript status_recipient;
        status_recipient << OP_TRUE;
        const uint256 recipient_bucket =
            DigiDollar::Paymaster::GetRecipientBudgetBucket(
                result.budget_ledger, status_recipient);
        const auto model_available = [&](DigiDollar::Paymaster::FundingModel model,
                                         DigiDollar::Paymaster::SponsorshipScope scope) {
            return DigiDollar::Paymaster::EvaluateProviderSafetyStatus(
                       result.budget_ledger, result.safety_policy, model, scope,
                       recipient_bucket,
                       *network_fee, GetTime())
                .can_accept_quote;
        };
        if (DigiDollar::Paymaster::PolicyAllowsFundingModel(
                result.policy, DigiDollar::Paymaster::FundingModel::USER_PAID) &&
            model_available(DigiDollar::Paymaster::FundingModel::USER_PAID,
                            DigiDollar::Paymaster::SponsorshipScope::PUBLIC)) {
            result.available_funding_models |=
                DigiDollar::Paymaster::FUNDING_MODEL_USER_PAID;
        }
        if (DigiDollar::Paymaster::PolicyAllowsFundingModel(
                result.policy, DigiDollar::Paymaster::FundingModel::SPONSORED) &&
            model_available(DigiDollar::Paymaster::FundingModel::SPONSORED,
                            result.policy.sponsorship_scope)) {
            result.available_funding_models |=
                DigiDollar::Paymaster::FUNDING_MODEL_SPONSORED;
        }
        if (result.available_funding_models == 0) {
            result.errors.push_back("PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
        }
    }

    result.available_funding_models &= DigiDollar::Paymaster::SupportedProviderPoolFundingModels(result.pool, result.policy);
    if (result.pool_ready && network_fee && result.have_safety_policy &&
        result.have_budget_ledger && result.available_funding_models == 0 && result.errors.empty()) {
        result.errors.push_back("PAYMASTER_SAFETY_LIMIT_EXHAUSTED");
    }

    if (!context.paymaster || !context.paymaster->Enabled()) result.errors.push_back("PAYMASTER_DISABLED");
    if (!result.have_settings || !result.settings.enabled) result.errors.push_back("PAYMASTER_PROVIDER_NOT_ENABLED");
    if (!result.wallet_eligible) {
        result.errors.push_back(eligibility_error);
        result.available_funding_models = 0;
    }
    if (!restore_safe) {
        result.errors.push_back(restore_error);
        result.available_funding_models = 0;
    }
    if (!result.have_identity) result.errors.push_back("PAYMASTER_IDENTITY_NOT_FOUND");
    if (!result.have_policy) result.errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
    if (result.have_policy) {
        std::string scope_error;
        if (!DigiDollar::Paymaster::CheckNewSponsorshipScope(result.policy.sponsorship_scope, scope_error)) {
            result.errors.push_back(scope_error);
            result.available_funding_models = 0;
        }
    }
    if (!result.have_safety_policy) result.errors.push_back("PAYMASTER_SAFETY_POLICY_NOT_FOUND");
    if (!result.have_budget_ledger) result.errors.push_back("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND");
    if (result.have_liquidity_policy && !result.have_maintenance_ledger)
        result.errors.push_back("PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND");
    if (result.have_settings && result.have_policy && result.settings.policy_hash != DigiDollar::Paymaster::GetProviderPolicyHash(result.policy)) {
        result.errors.push_back("PAYMASTER_POLICY_BINDING_MISMATCH");
    }
    if (result.have_policy && result.have_liquidity_policy &&
        !DigiDollar::Paymaster::ProviderLiquidityTargetsSatisfyPolicy(
            result.liquidity_policy, result.policy)) {
        // A carrier target may intentionally be reduced to zero by
        // release_slot. Keep that durable configuration valid, but surface a
        // distinct readiness gate instead of pretending that an automatic
        // start can restore capacity that the saved target does not request.
        result.available_funding_models &= ~DigiDollar::Paymaster::FUNDING_MODEL_USER_PAID;
        if (result.available_funding_models == 0) result.errors.push_back("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE");
    }
    if (wallet.IsLocked()) result.errors.push_back("PAYMASTER_WALLET_LOCKED");
    if (!result.have_pool) {
        result.errors.push_back("PAYMASTER_POOLS_NOT_PREPARED");
    } else if (result.have_policy) {
        result.errors.insert(result.errors.end(), result.pool.errors.begin(), result.pool.errors.end());
    }
    if (!context.args || context.args->GetIntArg("-prune", 0) != 0) {
        result.errors.push_back("PAYMASTER_REQUIRES_PRUNE_0");
    }
    const bool txindex_configured =
        context.args && context.args->GetBoolArg("-txindex", DEFAULT_TXINDEX);
    if (!txindex_configured) {
        result.errors.push_back("PAYMASTER_REQUIRES_TXINDEX");
    } else if (!ProviderTxIndexIsReady(wallet, wait_for_sync)) {
        result.errors.push_back("PAYMASTER_REQUIRES_READY_TXINDEX");
    }
    if (context.args && Params().GetChainType() == ChainType::MAIN &&
        context.args->GetBoolArg("-capturemessages", false)) {
        result.errors.push_back("PAYMASTER_MESSAGE_CAPTURE_ENABLED");
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman ||
        !(node->connman->GetLocalServices() & NODE_P2P_V2)) {
        result.errors.push_back("PAYMASTER_REQUIRES_V2_TRANSPORT");
    }
    if (node && node->connman) {
        if (node->connman->GetPaymasterBudget().inbound == 0) result.errors.push_back("PAYMASTER_NO_LOCAL_INBOUND_CAPACITY");
        if (!node->connman->PaymasterListenerReady()) result.errors.push_back("PAYMASTER_DIRECT_LISTENER_NOT_READY");
    }
    if (!context.args || !context.args->IsArgSet("-paymasterendpoint")) {
        result.errors.push_back("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED");
    } else {
        result.endpoint = LookupNumeric(context.args->GetArg("-paymasterendpoint", ""), 0);
        if (!DigiDollar::Paymaster::IsSupportedPaymasterEndpoint(result.endpoint, Params().GetChainType() == ChainType::REGTEST)) {
            result.errors.push_back("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE");
        }
    }
    if (!context.chain || context.chain->isInitialBlockDownload() || !context.chain->isReadyToBroadcast()) {
        result.errors.push_back("PAYMASTER_NODE_NOT_READY");
    }
    const int activation_height = Params().GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_DIGIDOLLAR);
    if (tip_height + 1 < activation_height) result.errors.push_back("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE");
    result.ready = result.errors.empty();
    return result;
}

bool PublishCurrentProviderAnnouncement(
    CWallet& wallet,
    WalletContext& context,
    const ProviderReadiness& readiness,
    int64_t now,
    DigiDollar::Paymaster::Announcement* published,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!readiness.ready) {
        error = readiness.errors.empty()
            ? "PAYMASTER_PROVIDER_NOT_READY"
            : readiness.errors.front();
        return false;
    }
    if (readiness.policy.sponsorship_scope == SponsorshipScope::RESTRICTED) {
        if (published) *published = Announcement{};
        return true;
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->chainman || !context.paymaster) {
        error = "PAYMASTER_NODE_NOT_READY";
        return false;
    }
    uint256 reference_block;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
        if (!tip) {
            error = "PAYMASTER_NODE_NOT_READY";
            return false;
        }
        reference_block = tip->GetBlockHash();
    }
    Announcement announcement;
    if (!BuildPaymasterAnnouncement(
            wallet, readiness.identity, readiness.policy,
            readiness.pool_entries, readiness.endpoint,
            Params().GenesisBlock().GetHash(), reference_block,
            now, readiness.available_funding_models,
            announcement, error) ||
        !ValidateAnnouncementEnvelope(
            announcement, Params().GenesisBlock().GetHash(), now, error,
            Params().GetChainType() == ChainType::REGTEST) ||
        !ValidateAdmissionProofs(announcement, *node->chainman, error) ||
        !context.paymaster->GetDirectory().AddValidated(announcement, now)) {
        if (error.empty()) error = "PAYMASTER_ANNOUNCEMENT_REJECTED";
        return false;
    }
    if (node->connman) {
        node->connman->ForEachNode([&announcement, node](CNode* peer) {
            if (!peer->fSuccessfullyConnected || peer->fDisconnect) return;
            const CNetMsgMaker maker{peer->GetCommonVersion()};
            node->connman->PushMessage(
                peer, maker.Make(NetMsgType::PMANNOUNCE, announcement));
        });
    }
    if (published) *published = std::move(announcement);
    return true;
}

LiquiditySlotCounts CountLiquiditySlots(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    DigiDollar::Paymaster::PoolPurpose purpose,
    DigiDollar::Paymaster::PoolAsset asset,
    size_t target,
    int64_t minimum_dgb_value)
{
    using namespace DigiDollar::Paymaster;
    LiquiditySlotCounts result;
    for (const ProviderPoolEntry& entry : entries) {
        const bool counts_toward_target =
            entry.state == PoolEntryState::AVAILABLE ||
            entry.state == PoolEntryState::RESERVED ||
            entry.state == PoolEntryState::PENDING_SUCCESSOR;
        if (entry.purpose != purpose || entry.asset != asset ||
            !counts_toward_target) continue;
        // An operational DGB slot that cannot cover the provider's current
        // advertised fee ceiling is not usable capacity. Do not let such a
        // stale or undersized output suppress automatic replenishment.
        if (asset == PoolAsset::DGB &&
            entry.dgb_value.value < minimum_dgb_value) {
            continue;
        }
        ++result.target_counted;
        if (entry.state == PoolEntryState::AVAILABLE &&
            entry.confirmation_height > 0) {
            ++result.ready;
        } else {
            ++result.pending;
        }
    }
    result.missing = target > result.target_counted
        ? target - result.target_counted
        : 0;
    return result;
}

DigiDollar::Paymaster::ProviderLiquidityPolicy SuggestedLiquidityPolicy(
    const ProviderReadiness& readiness,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    ProviderLiquidityPolicy result;
    const auto active_count = [&](PoolPurpose purpose, PoolAsset asset) {
        return static_cast<uint16_t>(std::min<size_t>(
            16, std::count_if(
                    readiness.pool_entries.begin(), readiness.pool_entries.end(),
                    [&](const ProviderPoolEntry& entry) {
                        return entry.purpose == purpose && entry.asset == asset &&
                               IsActiveProviderPoolState(entry.state);
                    })));
    };
    result.target_admission_dgb = std::max<uint16_t>(
        3, active_count(PoolPurpose::ADMISSION, PoolAsset::DGB));
    result.target_operational_dgb = std::max<uint16_t>(
        1, active_count(PoolPurpose::OPERATIONAL, PoolAsset::DGB));
    if (readiness.have_policy && PolicyAllowsFundingModel(
            readiness.policy, FundingModel::USER_PAID)) {
        result.target_admission_carriers = std::max<uint16_t>(
            3, active_count(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER));
        result.target_operational_carriers = std::max<uint16_t>(
            1, active_count(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER));
    }

    // Read-only proposals for an unconfigured wallet. Refill transactions
    // create several reserves, so their limits are independent of customer
    // payment budgets. Paid maintenance remains unapproved until explicitly saved.
    const int size = std::max({int(result.target_operational_dgb),
                               int(result.target_operational_carriers),
                               int(result.target_admission_dgb) - 2,
                               int(result.target_admission_carriers) - 2});
    const auto proposed = SetupLiquidityPreset(result.target_admission_carriers > 0, size);
    result.maximum_maintenance_fee_per_transaction.value = proposed.find_value(
        "maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>();
    result.maximum_maintenance_fee_per_hour.value = proposed.find_value(
        "maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>();
    result.maximum_maintenance_fee_per_day.value = proposed.find_value(
        "maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>();
    result.updated_at = now;
    return result;
}

DigiDollar::Paymaster::ProviderServiceStatus ProviderServiceStatusForObservation(
    DigiDollar::Paymaster::ProviderServiceStatus status, bool ready, bool synchronized)
{
    using namespace DigiDollar::Paymaster;
    // A scheduler tick can precede wallet/index notifications for a new block.
    // Retain every genuine fault and unresolved gate. Only this exact, known
    // transient wait may be superseded by a synchronized, ready observation.
    // Payment handlers independently recheck synchronization before execution.
    if (ready && synchronized && status.state == ProviderServiceState::WAITING_FOR_READINESS &&
        status.last_error == "PAYMASTER_PROVIDER_SYNCING") {
        status.state = ProviderServiceState::ACTIVE;
        status.last_error.clear();
    }
    return status;
}

bool SaveCarrierWithdrawalPreview(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    const DatabaseReadStatus ledger_status{
        batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
    if (ledger_status != DatabaseReadStatus::FOUND &&
        ledger_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            ledger_status, "ProviderMaintenanceLedger", ledger,
            "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        return false;
    }
    const bool have_ledger{ledger_status == DatabaseReadStatus::FOUND};
    if (!ReadOptionalProviderPool(batch, pool, error)) return false;

    bool ledger_changed{false};
    bool pool_changed{false};
    ProviderCarrierWithdrawalPlan previous;
    const DatabaseReadStatus previous_status{
        batch.ReadPaymasterCarrierWithdrawalPlanWithStatus(previous)};
    if (previous_status == DatabaseReadStatus::FOUND &&
        previous.operation_id != plan.operation_id && have_ledger) {
        const auto record = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [&](const ProviderMaintenanceRecord& candidate) {
                return candidate.operation_id == previous.operation_id;
            });
        if (record != ledger.records.end() &&
            record->state == ProviderMaintenanceState::PLANNED) {
            PaymasterMaintenanceRelease released;
            if (!PreparePaymasterMaintenanceRelease(wallet, ledger, pool, {previous.operation_id},
                    PaymasterMaintenanceReleaseKind::CARRIER_WITHDRAWAL, GetTime(), released, error)) return false;
            ledger = std::move(released.ledger);
            pool = std::move(released.pool);
            ledger_changed = true;
            pool_changed = true;
        }
    } else if (previous_status != DatabaseReadStatus::FOUND &&
               previous_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            previous_status, "ProviderCarrierWithdrawalPlan", previous,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND",
            "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN");
        return false;
    }

    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((ledger_changed && !batch.WritePaymasterMaintenanceLedger(ledger)) ||
        (pool_changed && !batch.WritePaymasterProviderPool(pool)) ||
        !batch.WritePaymasterCarrierWithdrawalPlan(plan)) {
        batch.TxnAbort();
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

namespace {
constexpr size_t MAINTENANCE_HISTORY_WORK{256};
constexpr size_t MAINTENANCE_DIRTY_LIMIT{4096};

bool MatchesMaintenanceTransaction(const DigiDollar::Paymaster::ProviderMaintenanceRecord& record,
                                   const CWalletTx& wallet_tx)
{
    if (!wallet_tx.tx) return false;
    for (const auto& source : record.source_inputs) {
        if (std::none_of(wallet_tx.tx->vin.begin(), wallet_tx.tx->vin.end(),
                         [&](const CTxIn& input) { return input.prevout == source; })) return false;
    }
    for (const auto& output : record.outputs) {
        if (!FindMaintenanceOutputIndex(*wallet_tx.tx, output)) return false;
    }
    return record.kind != DigiDollar::Paymaster::ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS ||
           FindExactDigiDollarOutputIndex(*wallet_tx.tx, record.withdrawal_excess_script_pub_key,
                                          record.withdrawal_excess_amount).has_value();
}

struct MaintenanceHistory {
    std::mutex mutex;
    boost::signals2::scoped_connection notifications;
    uint256 binding;
    std::optional<uint256> position;
    size_t buckets{0};
    size_t wallet_size{0};
    bool complete{false};
    bool reset{false};
    std::set<uint256> dirty;
    // Two exact candidates already prove ambiguity. Retaining more cannot
    // authorize recovery, and would let repeated deposits grow this cache.
    std::map<uint256, std::set<uint256>> candidates;
};
std::mutex maintenance_history_mutex;
std::map<WalletDatabase*, std::shared_ptr<MaintenanceHistory>> maintenance_histories;

std::shared_ptr<MaintenanceHistory> GetMaintenanceHistory(CWallet& wallet)
{
    {
        std::lock_guard<std::mutex> lock{maintenance_history_mutex};
        const auto existing = maintenance_histories.find(&wallet.GetDatabase());
        if (existing != maintenance_histories.end()) return existing->second;
    }
    auto state = std::make_shared<MaintenanceHistory>();
    state->wallet_size = wallet.mapWallet.size();
    const std::weak_ptr<MaintenanceHistory> weak{state};
    state->notifications = wallet.NotifyTransactionChanged.connect([weak](const uint256& txid, ChangeType change) {
        if (auto history = weak.lock()) {
            std::lock_guard<std::mutex> lock{history->mutex};
            if (change == CT_NEW) {
                if (history->wallet_size == std::numeric_limits<size_t>::max()) history->reset = true;
                else ++history->wallet_size;
            } else if (change == CT_DELETED) {
                if (history->wallet_size == 0) history->reset = true;
                else --history->wallet_size;
            }
            if (history->dirty.size() >= MAINTENANCE_DIRTY_LIMIT) {
                history->reset = true;
                history->dirty.clear();
            }
            history->dirty.insert(txid);
        }
    });
    std::lock_guard<std::mutex> lock{maintenance_history_mutex};
    return maintenance_histories.emplace(&wallet.GetDatabase(), state).first->second;
}

bool ReadMaintenanceCandidates(CWallet& wallet,
                               const DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger,
                               std::map<uint256, std::set<uint256>>& candidates)
{
    using namespace DigiDollar::Paymaster;
    AssertLockHeld(wallet.cs_wallet);
    std::vector<const ProviderMaintenanceRecord*> records;
    HashWriter hash = TaggedHash("DigiByte Paymaster Maintenance History v1");
    for (const auto& record : ledger.records) {
        if (record.state != ProviderMaintenanceState::PLANNED || !record.transaction_id.IsNull()) continue;
        records.push_back(&record);
        hash << record.operation_id << static_cast<uint8_t>(record.kind) << record.outputs << record.source_inputs
             << record.withdrawal_excess_script_pub_key << record.withdrawal_excess_amount;
    }
    if (records.empty()) return true;
    const auto state = GetMaintenanceHistory(wallet);
    std::lock_guard<std::mutex> lock{state->mutex};
    const uint256 binding = hash.GetSHA256();
    const auto restart = [&] {
        state->binding = binding;
        state->position.reset();
        state->buckets = wallet.mapWallet.bucket_count();
        state->wallet_size = wallet.mapWallet.size();
        state->complete = false;
        state->reset = false;
        state->candidates.clear();
        state->dirty.clear();
    };
    // A failed WriteTx can leave an in-memory insertion without a wallet
    // notification. Detect it independently before trusting a negative lookup.
    if (state->binding != binding || state->reset || state->buckets != wallet.mapWallet.bucket_count() ||
        state->wallet_size != wallet.mapWallet.size()) restart();
    // An unordered-map rehash or deletion must never leave a saved iterator.
    // Newly inserted/updated transactions are independently queued by the
    // wallet notification, including insertions before the scan position.
    auto next = wallet.mapWallet.begin();
    if (state->position && !state->complete) {
        next = wallet.mapWallet.find(*state->position);
        if (next == wallet.mapWallet.end()) restart();
        else ++next;
        if (!state->position) next = wallet.mapWallet.begin();
    }
    const auto observe = [&](const uint256& txid) {
        const auto transaction = wallet.mapWallet.find(txid);
        for (const auto* record : records) {
            auto& found = state->candidates[record->operation_id];
            if (transaction != wallet.mapWallet.end() && MatchesMaintenanceTransaction(*record, transaction->second)) {
                if (found.size() < 2) found.insert(txid);
            } else {
                const bool ambiguous = found.size() == 2;
                if (found.erase(txid) && ambiguous) state->reset = true;
            }
        }
    };
    size_t work{0};
    while (!state->complete && work < MAINTENANCE_HISTORY_WORK) {
        if (next == wallet.mapWallet.end()) { state->complete = true; break; }
        const uint256 txid = next->first;
        ++next;
        state->position = txid;
        observe(txid);
        ++work;
    }
    while (!state->dirty.empty() && work < MAINTENANCE_HISTORY_WORK) {
        const uint256 txid = *state->dirty.begin();
        state->dirty.erase(state->dirty.begin());
        observe(txid);
        ++work;
    }
    if (!state->complete || !state->dirty.empty() || state->reset) return false;
    candidates = state->candidates;
    return true;
}
} // namespace

bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error)
{
    recovered = 0;
    error.clear();
    LOCK(wallet.cs_wallet);
    if (!ResumeProviderMaintenanceTransactions(wallet, error)) return false;
    LOG_TIME_MILLIS_WITH_CATEGORY("Paymaster reconciliation", BCLog::BENCH);
    using namespace DigiDollar::Paymaster;
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    ProviderLiquidityPolicy policy;
    const DatabaseReadStatus ledger_status{
        ReadPaymasterMaintenanceState(batch, policy, ledger)};
    if (ledger_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (ledger_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            ledger_status, "ProviderMaintenanceLedger", ledger,
            "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        return false;
    }
    std::vector<ProviderPoolEntry> pool;
    if (!ReadOptionalProviderPool(batch, pool, error)) return false;
    const auto original_pool = pool;
    bool ledger_changed{false};
    bool pool_changed{false};
    const int64_t now = GetTime();
    const auto update_record_time = [&](ProviderMaintenanceRecord& record) {
        // Policy revisions and a clock rollback can put durable accounting
        // ahead of wall time. Reconciliation must not invalidate an existing
        // record or move confirmed spend into an earlier budget window.
        ledger.accounting_time_high_water = std::max({
            now, ledger.accounting_time_high_water, record.updated_at});
        record.updated_at = ledger.accounting_time_high_water;
        ledger_changed = true;
    };
    ProviderCarrierWithdrawalPlan withdrawal_plan;
    const DatabaseReadStatus withdrawal_status{
        batch.ReadPaymasterCarrierWithdrawalPlanWithStatus(withdrawal_plan)};
    if (withdrawal_status != DatabaseReadStatus::FOUND &&
        withdrawal_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            withdrawal_status, "ProviderCarrierWithdrawalPlan",
            withdrawal_plan,
            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND",
            "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_PLAN");
        return false;
    }
    const bool have_withdrawal_plan{
        withdrawal_status == DatabaseReadStatus::FOUND};
    std::map<uint256, std::set<uint256>> history_candidates;
    const bool history_ready = ReadMaintenanceCandidates(wallet, ledger, history_candidates);
    const auto find_source = [&](const COutPoint& outpoint, bool restore) {
        auto source = std::find_if(pool.begin(), pool.end(), [&](const auto& entry) { return entry.outpoint == outpoint; });
        if (source != pool.end() || !restore) return source;
        ProviderPoolEntry retired;
        if (paymaster_store::internal::ReadProviderPoolDisposition(batch, original_pool, outpoint, retired) != DatabaseReadStatus::FOUND ||
            retired.state == PoolEntryState::RELEASED ||
            !paymaster_store::internal::MakeProviderPoolRoom(pool, 1, error)) {
            if (error.empty()) error = "PAYMASTER_MAINTENANCE_SOURCE_MISSING";
            return pool.end();
        }
        pool.push_back(std::move(retired));
        pool_changed = true;
        return std::prev(pool.end());
    };
    for (ProviderMaintenanceRecord& record : ledger.records) {
        const bool previously_confirmed = record.state == ProviderMaintenanceState::CONFIRMED;
        if (record.state != ProviderMaintenanceState::PLANNED &&
            record.state != ProviderMaintenanceState::CONFIRMED &&
            !record.HasUnresolvedTransaction()) {
            continue;
        }
        // A conflict/abandon observation is reversible. Keep following the
        // exact saved maintenance transaction; never construct a replacement here.
        auto transaction = wallet.mapWallet.end();
        if (!record.transaction_id.IsNull()) {
            transaction = wallet.mapWallet.find(record.transaction_id);
            if (transaction != wallet.mapWallet.end() && !MatchesMaintenanceTransaction(record, transaction->second)) {
                error = "PAYMASTER_MAINTENANCE_TRANSACTION_CONFLICT";
                return false;
            }
        } else {
            if (!history_ready) continue;
            for (const uint256& txid : history_candidates[record.operation_id]) {
                auto candidate = wallet.mapWallet.find(txid);
                // The derived lookup only selects candidates. Revalidate the
                // entire journal binding before recording any transaction.
                if (candidate == wallet.mapWallet.end() || !MatchesMaintenanceTransaction(record, candidate->second)) continue;
                if (transaction != wallet.mapWallet.end()) {
                    error = "PAYMASTER_MAINTENANCE_TRANSACTION_AMBIGUOUS";
                    return false;
                }
                transaction = candidate;
            }
        }
        if (transaction == wallet.mapWallet.end()) {
            if (record.state == ProviderMaintenanceState::PLANNED &&
                record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS &&
                record.transaction_id.IsNull()) {
                int64_t release_after =
                    record.updated_at >
                            std::numeric_limits<int64_t>::max() - 15 * 60
                        ? std::numeric_limits<int64_t>::max()
                        : record.updated_at + 15 * 60;
                if (have_withdrawal_plan &&
                    withdrawal_plan.operation_id == record.operation_id) {
                    release_after = withdrawal_plan.expires_at;
                }
                if (now > release_after) {
                    std::vector<ProviderPoolEntry*> sources;
                    for (ProviderPoolEntry& entry : pool) {
                        const bool explicitly_bound = std::find(
                            record.source_inputs.begin(),
                            record.source_inputs.end(), entry.outpoint) !=
                            record.source_inputs.end();
                        if (explicitly_bound) {
                            sources.push_back(&entry);
                        }
                    }
                    const bool can_release = !sources.empty() &&
                        std::all_of(
                            sources.begin(), sources.end(),
                            [&](const ProviderPoolEntry* source) {
                                return source != nullptr &&
                                       source->reservation_id ==
                                           record.operation_id &&
                                       (source->state ==
                                            PoolEntryState::RESERVED ||
                                        source->state ==
                                            PoolEntryState::COMMITTED) &&
                                       !wallet.IsSpent(source->outpoint);
                            }) &&
                        sources.size() == record.source_inputs.size();
                    if (can_release) {
                        PaymasterMaintenanceRelease released;
                        if (!PreparePaymasterMaintenanceRelease(wallet, ledger, pool, {record.operation_id},
                                PaymasterMaintenanceReleaseKind::CARRIER_WITHDRAWAL, now, released, error)) return false;
                        // Keep references into ledger.records valid for the surrounding pass.
                        record = *std::find_if(released.ledger.records.begin(), released.ledger.records.end(),
                            [&](const auto& candidate) { return candidate.operation_id == record.operation_id; });
                        ledger.accounting_time_high_water = released.ledger.accounting_time_high_water;
                        pool = std::move(released.pool);
                        ledger_changed = true;
                        pool_changed = true;
                    }
                }
            }
            continue;
        }
        const uint256 txid = transaction->first;
        if (record.IsPreparation()) {
            const auto fee = GetProviderFinanceTransactionFee(wallet, transaction->second.tx);
            if (!fee || fee->value > record.maximum_fee.value) {
                error = "PAYMASTER_POOL_FEE_LIMIT";
                return false;
            }
            if (record.actual_fee != *fee) {
                record.actual_fee = *fee;
                ledger_changed = true;
            }
        }
        if (record.transaction_id.IsNull()) {
            record.transaction_id = txid;
            // Conservatively charge the entire reservation after recovering a
            // crash window; this can only reduce later maintenance capacity.
            if (!record.IsPreparation()) record.actual_fee = record.maximum_fee;
            record.state = ProviderMaintenanceState::BROADCAST;
            update_record_time(record);
            ++recovered;
        }

        if (transaction->second.isAbandoned() ||
            transaction->second.isConflicted()) {
            if (record.state != ProviderMaintenanceState::FAILED) {
                record.state = ProviderMaintenanceState::FAILED;
                update_record_time(record);
            }
            for (ProviderPoolEntry& entry : pool) {
                if (entry.outpoint.hash != txid ||
                    entry.origin_commit_key != record.operation_id ||
                    !IsActiveProviderPoolState(entry.state)) {
                    continue;
                }
                entry.state = PoolEntryState::INVALIDATED;
                entry.confirmation_height = 0;
                entry.reservation_id.SetNull();
                entry.updated_at = now;
                pool_changed = true;
            }
            for (const COutPoint& source_input : record.source_inputs) {
                const auto source = find_source(source_input, /*restore=*/true);
                if (source == pool.end()) return false;
                // Abandonment or a conflict can reverse. Retain the source for
                // the exact signed transaction, including records released by
                // older versions. Never steal another operation's reservation.
                if (!source->reservation_id.IsNull() &&
                    source->reservation_id != record.operation_id) {
                    error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                    return false;
                }
                const bool recoverable_state =
                    source->state == PoolEntryState::AVAILABLE ||
                    source->state == PoolEntryState::RESERVED ||
                    source->state == PoolEntryState::COMMITTED ||
                    source->state == PoolEntryState::SPENT ||
                    source->state == PoolEntryState::INVALIDATED;
                if (!recoverable_state) {
                    error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                    return false;
                }
                const PoolEntryState restored_state = PoolEntryState::COMMITTED;
                if (source->state != restored_state ||
                    source->reservation_id != record.operation_id) {
                    source->state = restored_state;
                    source->reservation_id = record.operation_id;
                    source->updated_at = now;
                    pool_changed = true;
                }
            }
            continue;
        }

        const auto* confirmed = transaction->second.state<TxStateConfirmed>();
        if (record.kind == ProviderMaintenanceKind::PREPARE_CARRIER &&
            (confirmed || transaction->second.InMempool())) {
            CAmount carrier_total{0};
            for (const auto& output : record.outputs) carrier_total += output.carrier_value.value;
            auto* dd_wallet = wallet.GetDDWallet();
            if (!dd_wallet || !dd_wallet->RecordPaymasterSendHistory(
                    txid, record.outputs.front().script_pub_key, carrier_total, error)) {
                if (error.empty()) error = "PAYMASTER_DD_WALLET_UNAVAILABLE";
                return false;
            }
        }
        const PoolEntryState expected_source_state = confirmed
            ? PoolEntryState::SPENT
            : PoolEntryState::COMMITTED;
        for (const COutPoint& source_input : record.source_inputs) {
            auto source = find_source(source_input, /*restore=*/!confirmed);
            // Confirmation proves consumption of this journal-bound source.
            if (source == pool.end()) {
                if (confirmed) continue;
                return false;
            }
            if (!source->reservation_id.IsNull() &&
                source->reservation_id != record.operation_id) {
                error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                return false;
            }
            const bool recoverable_state =
                source->state == PoolEntryState::AVAILABLE ||
                source->state == PoolEntryState::RESERVED ||
                source->state == PoolEntryState::COMMITTED ||
                source->state == PoolEntryState::SPENT ||
                source->state == PoolEntryState::INVALIDATED;
            if (!recoverable_state) {
                error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
                return false;
            }
            const uint256 expected_reservation = confirmed
                ? uint256{}
                : record.operation_id;
            if (source->state != expected_source_state ||
                source->reservation_id != expected_reservation) {
                source->state = expected_source_state;
                source->reservation_id = expected_reservation;
                source->updated_at = now;
                pool_changed = true;
            }
        }

        const ProviderMaintenanceState expected_state = confirmed
            ? ProviderMaintenanceState::CONFIRMED
            : ProviderMaintenanceState::BROADCAST;
        if (record.state != expected_state) {
            record.state = expected_state;
            update_record_time(record);
        }
        for (const ProviderMaintenanceOutput& output : record.outputs) {
            const auto output_index = FindMaintenanceOutputIndex(
                *transaction->second.tx, output);
            if (!output_index) {
                error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING";
                return false;
            }
            ProviderPoolEntry expected;
            expected.outpoint = COutPoint{txid, *output_index};
            expected.purpose = output.purpose;
            expected.asset = output.asset;
            expected.state = confirmed ? PoolEntryState::AVAILABLE
                                       : PoolEntryState::PENDING_SUCCESSOR;
            expected.script_pub_key = output.script_pub_key;
            expected.dgb_value = output.dgb_value;
            expected.carrier_value = output.carrier_value;
            expected.confirmation_height = confirmed
                ? confirmed->confirmed_block_height : 0;
            expected.reservation_id = confirmed ? uint256{}
                                                : record.operation_id;
            expected.origin_commit_key = record.operation_id;
            expected.updated_at = record.created_at;
            const auto existing = std::find_if(
                pool.begin(), pool.end(), [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint == expected.outpoint;
                });
            if (existing == pool.end()) {
                if (wallet.IsSpent(expected.outpoint)) continue;
                ProviderPoolEntry retired;
                const auto status = paymaster_store::internal::ReadProviderPoolDisposition(batch, original_pool, expected.outpoint, retired);
                if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
                    error = "PAYMASTER_POOL_HISTORY_READ";
                    return false;
                }
                if (status == DatabaseReadStatus::FOUND &&
                    retired.state != PoolEntryState::INVALIDATED) continue;
                // Older inventories may already have forgotten a release.
                // Absence is not permission to re-enrol an old unspent output.
                if (status == DatabaseReadStatus::NOT_FOUND && previously_confirmed) {
                    error = "PAYMASTER_POOL_HISTORY_REVIEW_REQUIRED";
                    return false;
                }
                if (!paymaster_store::internal::MakeProviderPoolRoom(pool, 1, error)) return false;
                pool.push_back(std::move(expected));
                pool_changed = true;
            } else if (existing->purpose != expected.purpose ||
                       existing->asset != expected.asset ||
                       existing->script_pub_key != expected.script_pub_key ||
                       existing->dgb_value != expected.dgb_value ||
                       existing->carrier_value != expected.carrier_value ||
                       (!existing->origin_commit_key.IsNull() &&
                        existing->origin_commit_key != record.operation_id)) {
                error = "PAYMASTER_MAINTENANCE_POOL_CONFLICT";
                return false;
            } else {
                if (existing->origin_commit_key.IsNull()) {
                    existing->origin_commit_key = record.operation_id;
                    pool_changed = true;
                }
                if (existing->confirmation_height !=
                    expected.confirmation_height) {
                    existing->confirmation_height =
                        expected.confirmation_height;
                    pool_changed = true;
                }
                if (existing->state == PoolEntryState::INVALIDATED &&
                    !wallet.IsSpent(existing->outpoint)) {
                    existing->state = expected.state;
                    existing->reservation_id = expected.reservation_id;
                    existing->updated_at = std::max(existing->updated_at, now);
                    pool_changed = true;
                } else if (confirmed &&
                    existing->state == PoolEntryState::PENDING_SUCCESSOR) {
                    existing->state = PoolEntryState::AVAILABLE;
                    existing->reservation_id.SetNull();
                    existing->updated_at = now;
                    pool_changed = true;
                } else if (!confirmed &&
                           existing->state == PoolEntryState::AVAILABLE) {
                    existing->state = PoolEntryState::PENDING_SUCCESSOR;
                    existing->reservation_id = record.operation_id;
                    existing->updated_at = now;
                    pool_changed = true;
                }
            }
        }
    }
    std::string validation_error;
    if (!ValidateProviderMaintenanceLedger(ledger, validation_error)) {
        error = validation_error;
        return false;
    }
    if (!ledger_changed && !pool_changed) {
        if (!history_ready) error = "PAYMASTER_MAINTENANCE_RECONCILING";
        return history_ready;
    }
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((ledger_changed && !batch.WritePaymasterMaintenanceLedger(ledger)) ||
        (pool_changed && !batch.WritePaymasterProviderPool(pool))) {
        batch.TxnAbort();
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    if (!history_ready) error = "PAYMASTER_MAINTENANCE_RECONCILING";
    return history_ready;
}

bool ReleaseSatisfiedProviderReplenishment(CWallet& wallet, bool dgb_satisfied,
                                          bool carriers_satisfied, std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!dgb_satisfied && !carriers_satisfied) return true;
    LOCK(wallet.cs_wallet);
    // A PLANNED record can still have a wallet transaction after a lost reply.
    size_t recovered{0};
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) return false;
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    const auto status = batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger);
    if (status == DatabaseReadStatus::NOT_FOUND) return true;
    if (status != DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
        return false;
    }
    std::vector<uint256> release_ids;
    for (const auto& record : ledger.records) {
        const bool unneeded = (dgb_satisfied && record.kind == ProviderMaintenanceKind::REPLENISH_DGB) ||
                              (carriers_satisfied && record.kind == ProviderMaintenanceKind::REPLENISH_CARRIER);
        if (!unneeded || record.state != ProviderMaintenanceState::PLANNED || !record.transaction_id.IsNull()) continue;
        release_ids.push_back(record.operation_id);
    }
    if (release_ids.empty()) return true;
    ProviderMaintenanceLedger released;
    int64_t released_fee{0};
    if (!PreparePaymasterMaintenanceCancellation(wallet, ledger, release_ids, GetTime(), released, released_fee, error)) return false;
    if (!batch.WritePaymasterMaintenanceLedger(released)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

static bool ReduceReplenishmentPlan(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    DigiDollar::Paymaster::ProviderMaintenanceRecord& operation,
    size_t output_count,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (output_count == operation.outputs.size()) return true;
    if (output_count == 0 || output_count > operation.outputs.size() ||
        operation.state != ProviderMaintenanceState::PLANNED ||
        !operation.transaction_id.IsNull() || operation.IsPreparation()) {
        error = "PAYMASTER_INVALID_MAINTENANCE_RESERVATION";
        return false;
    }
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    if (!batch.ReadPaymasterMaintenanceLedger(ledger)) {
        error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
        return false;
    }
    const auto original = std::find_if(ledger.records.begin(), ledger.records.end(),
        [&](const ProviderMaintenanceRecord& record) { return record.operation_id == operation.operation_id; });
    if (original == ledger.records.end() || original->state != ProviderMaintenanceState::PLANNED ||
        !original->transaction_id.IsNull() || original->plan_id != operation.plan_id ||
        original->outputs != operation.outputs || original->maximum_fee != operation.maximum_fee) {
        error = "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT";
        return false;
    }
    ProviderMaintenanceRecord smaller{operation};
    do {
        smaller.operation_id = GetRandHash();
    } while (smaller.operation_id.IsNull());
    smaller.outputs.resize(output_count);
    smaller.plan_id = MaintenancePlanId(smaller.kind, smaller.outputs, policy);
    ProviderMaintenanceLedger released;
    int64_t released_fee{0};
    if (!PreparePaymasterMaintenanceCancellation(wallet, ledger, {operation.operation_id}, GetTime(), released, released_fee, error)) return false;
    ledger = std::move(released);
    if (!ReserveProviderMaintenanceBudget(ledger, policy, smaller, GetTime(), error)) return false;
    if (!batch.WritePaymasterMaintenanceLedger(ledger)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    operation = std::move(smaller);
    return true;
}

uint256 PoolPreparationAuthorization(CWallet& wallet)
{
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    DigiDollar::Paymaster::ProviderPolicy policy;
    if (!GetPaymasterIdentity(wallet, identity) || !GetPaymasterProviderPolicy(wallet, policy)) return {};
    HashWriter hash = TaggedHash("DigiByte Paymaster Pool Authorization v1");
    hash << Params().GenesisBlock().GetHash() << identity.provider_id << DigiDollar::Paymaster::GetProviderPolicyHash(policy);
    return hash.GetSHA256();
}

DigiDollar::Paymaster::ProviderSetupAuthorization ReadPoolSetupAuthorization(CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    DigiDollar::Paymaster::ProviderSetupAuthorization authorization;
    const auto status = batch.ReadPaymasterSetupAuthorizationWithStatus(authorization);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
        throw DigiDollar::Paymaster::ApplicationError(DigiDollar::Paymaster::ErrorKind::WALLET, "PAYMASTER_INVALID_SETUP_AUTHORIZATION");
    }
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    if (status == DatabaseReadStatus::FOUND && authorization.generation != 0 &&
        (!GetPaymasterIdentity(wallet, identity) || identity.provider_id != authorization.provider_id)) {
        throw DigiDollar::Paymaster::ApplicationError(DigiDollar::Paymaster::ErrorKind::WALLET, "PAYMASTER_SETUP_IDENTITY_MISMATCH");
    }
    authorization.genesis_hash = Params().GenesisBlock().GetHash();
    return authorization;
}

bool CheckPaymasterPoolFundingBroadcast(CWallet& wallet, std::string& error)
{
    LOCK(wallet.cs_wallet);
    error.clear();
    if (wallet.GetBroadcastTransactions()) return true;
    error = "PAYMASTER_WALLET_BROADCAST_DISABLED";
    return false;
}

namespace {
using namespace DigiDollar::Paymaster;

std::string MaintenanceBuildFailure(const ProviderMaintenanceAuthority& authority,
                                    bool digidollar, const std::string& detail)
{
    if (std::holds_alternative<FiniteSetupMaintenanceAuthority>(authority))
        return std::string{digidollar ? "PAYMASTER_CARRIER_TRANSACTION_FAILED: " : "PAYMASTER_POOL_WAITING_DGB: "} + detail;
    if (std::holds_alternative<RecurringRefillMaintenanceAuthority>(authority))
        return digidollar ? "PAYMASTER_CARRIER_REPLENISHMENT_FAILED: " + detail : "PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED";
    if (std::holds_alternative<PoolRetirementMaintenanceAuthority>(authority))
        return std::string{digidollar ? "PAYMASTER_CARRIER_REBALANCE_FAILED: " : "PAYMASTER_DGB_REBALANCE_FAILED: "} + detail;
    return detail;
}

bool SameMaintenanceOperation(const ProviderMaintenanceRecord& saved,
                              const ProviderMaintenanceRecord& expected)
{
    return saved.operation_id == expected.operation_id && saved.plan_id == expected.plan_id &&
           saved.kind == expected.kind && saved.outputs == expected.outputs &&
           saved.source_inputs == expected.source_inputs && saved.maximum_fee == expected.maximum_fee &&
           saved.preparation_authorization == expected.preparation_authorization &&
           saved.preparation_request == expected.preparation_request &&
           saved.withdrawal_excess_script_pub_key == expected.withdrawal_excess_script_pub_key &&
           saved.withdrawal_excess_amount == expected.withdrawal_excess_amount;
}

bool ValidateMaintenanceExecutionAuthority(CWallet& wallet, const ProviderMaintenanceAuthority& authority,
                                          ProviderMaintenanceRecord& operation,
                                          DGBSatoshis& maximum_fee, std::string& error)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!CheckPaymasterProviderRestoreGuard(wallet, error)) return false;
    WalletBatch batch{wallet.GetDatabase()};
    return std::visit([&](const auto& approval) {
        using Approval = std::decay_t<decltype(approval)>;
        if constexpr (std::is_same_v<Approval, PoolRetirementMaintenanceAuthority>) {
            ProviderIdentityRecord identity;
            ProviderPolicy policy;
            std::vector<ProviderPoolEntry> pool;
            if (approval.reviewed_plan.IsNull() || approval.sources.empty() ||
                !batch.ReadPaymasterIdentity(identity) || identity.provider_id != approval.provider_id ||
                !batch.ReadPaymasterProviderPool(pool)) {
                error = "PAYMASTER_POOL_PLAN_CHANGED";
                return false;
            }
            if (!batch.ReadPaymasterPolicy(policy) || approval.provider_policy_hash.IsNull() ||
                GetProviderPolicyHash(policy) != approval.provider_policy_hash) {
                error = "PAYMASTER_PROVIDER_POLICY_CHANGED";
                return false;
            }
            if (approval.liquidity_revision) {
                ProviderLiquidityPolicy liquidity;
                if (!batch.ReadPaymasterLiquidityPolicy(liquidity) || liquidity.updated_at != *approval.liquidity_revision) {
                    error = "PAYMASTER_LIQUIDITY_POLICY_CHANGED";
                    return false;
                }
            }
            for (const auto& source : approval.sources) {
                const auto current = std::find_if(pool.begin(), pool.end(), [&](const auto& entry) { return entry.outpoint == source.outpoint; });
                if (current == pool.end() || current->state != PoolEntryState::AVAILABLE ||
                    !current->reservation_id.IsNull() || current->asset != source.asset ||
                    current->purpose != source.purpose || current->script_pub_key != source.script_pub_key ||
                    current->dgb_value != source.dgb_value || current->carrier_value != source.carrier_value ||
                    current->confirmation_height <= 0 || wallet.IsSpent(source.outpoint)) {
                    error = "PAYMASTER_POOL_PLAN_CHANGED";
                    return false;
                }
            }
            maximum_fee = approval.maximum_fee;
            return true;
        } else {
            ProviderMaintenanceLedger ledger;
            if (batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger) != DatabaseReadStatus::FOUND) {
                error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
                return false;
            }
            const auto current = std::find_if(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
                return record.operation_id == approval.operation.operation_id;
            });
            if (current == ledger.records.end() || current->state != ProviderMaintenanceState::PLANNED ||
                !current->transaction_id.IsNull() || !SameMaintenanceOperation(*current, approval.operation)) {
                error = "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT";
                return false;
            }
            operation = *current;
            maximum_fee = current->maximum_fee;
            if constexpr (std::is_same_v<Approval, FiniteSetupMaintenanceAuthority>) {
                ProviderSettings settings;
                const auto setup = ReadPoolSetupAuthorization(wallet);
                // Accepted V1 commands survive their atomic journal migration.
                // Generation zero is the migration fence; fresh setup approval
                // always advances it. The exact PLANNED row must still exist.
                // A V2 journal without an intact fence failed the read above.
                const bool legacy_accepted = setup.generation == 0 && setup.plan_id.IsNull();
                if (!operation.IsPreparation() || (!legacy_accepted && setup.plan_id != operation.plan_id) || setup.revoked ||
                    operation.preparation_authorization != PoolPreparationAuthorization(wallet)) {
                    error = "PAYMASTER_POOL_POLICY_CHANGED";
                    return false;
                }
                if (!batch.ReadPaymasterSettings(settings) || !settings.enabled) {
                    error = "PAYMASTER_PROVIDER_DISABLED";
                    return false;
                }
                return CheckPaymasterPoolFundingBroadcast(wallet, error);
            } else {
                ProviderLiquidityPolicy policy;
                if (!batch.ReadPaymasterLiquidityPolicy(policy) ||
                    (HashWriter{} << policy).GetHash() != (HashWriter{} << approval.policy).GetHash()) {
                    error = "PAYMASTER_LIQUIDITY_POLICY_CHANGED";
                    return false;
                }
                if constexpr (std::is_same_v<Approval, RecurringRefillMaintenanceAuthority>) {
                    ProviderSettings settings;
                    if ((operation.kind != ProviderMaintenanceKind::REPLENISH_DGB &&
                         operation.kind != ProviderMaintenanceKind::REPLENISH_CARRIER) ||
                        !policy.automatic_replenishment || !policy.paid_maintenance_approved) {
                        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
                        return false;
                    }
                    if (!batch.ReadPaymasterSettings(settings) || !settings.enabled) {
                        error = "PAYMASTER_PROVIDER_DISABLED";
                        return false;
                    }
                    return CheckPaymasterPoolFundingBroadcast(wallet, error);
                } else {
                    ProviderCarrierWithdrawalPlan plan;
                    if (operation.kind != ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS ||
                        !batch.ReadPaymasterCarrierWithdrawalPlan(plan) ||
                        plan.plan_id != approval.plan.plan_id || plan.operation_id != operation.operation_id ||
                        plan.liquidity_policy_updated_at != policy.updated_at || plan.expires_at < GetTime()) {
                        error = "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED";
                        return false;
                    }
                    return true;
                }
            }
        }
    }, authority);
}

bool ValidateMaintenanceTransferBinding(const CWallet& wallet, const ProviderMaintenanceAuthority& authority,
                                       const ProviderMaintenanceRecord& operation,
                                       const ProviderMaintenanceTransfer& transfer, std::string& error)
{
    AssertLockHeld(wallet.cs_wallet);
    const auto fail = [&] { error = "PAYMASTER_MAINTENANCE_TRANSACTION_BINDING_MISMATCH"; return false; };
    const bool dgb_leg = std::holds_alternative<DGBMaintenanceTransfer>(transfer);
    std::vector<std::pair<CScript, CAmount>> recipients;
    std::vector<COutPoint> selected;
    if (dgb_leg) {
        const auto& dgb = std::get<DGBMaintenanceTransfer>(transfer);
        const auto inputs = dgb.coin_control.ListSelected();
        selected.assign(inputs.begin(), inputs.end());
        for (const auto& recipient : dgb.recipients) recipients.emplace_back(GetScriptForDestination(recipient.dest), recipient.nAmount);
    } else {
        const auto& dd = std::get<DDMaintenanceTransfer>(transfer);
        selected = dd.plan.dd_utxos;
        for (const auto& [address, amount] : dd.plan.recipients) {
            const CDigiDollarAddress destination{address};
            if (!destination.IsValidForCurrentNetwork()) return fail();
            recipients.emplace_back(GetScriptForDestination(destination.GetDigiDollarDestination()), amount);
        }
    }
    if (const auto* retirement = std::get_if<PoolRetirementMaintenanceAuthority>(&authority)) {
        std::set<COutPoint> expected;
        CAmount total{0}, returned{0};
        for (const auto& source : retirement->sources) {
            if ((source.asset == PoolAsset::DGB) != dgb_leg) continue;
            const auto amount = dgb_leg ? source.dgb_value.value : source.carrier_value.value;
            if (amount <= 0 || amount > std::numeric_limits<CAmount>::max() - total) return fail();
            total += amount;
            if (!expected.insert(source.outpoint).second) return fail();
        }
        if (selected.size() != expected.size() || std::set<COutPoint>{selected.begin(), selected.end()} != expected) return fail();
        for (const auto& [script, amount] : recipients) {
            if (!(wallet.IsMine(script) & ISMINE_SPENDABLE) || amount <= 0 || amount > std::numeric_limits<CAmount>::max() - returned) return fail();
            returned += amount;
        }
        if (returned != total) return fail();
        if (dgb_leg) {
            const auto& dgb = std::get<DGBMaintenanceTransfer>(transfer);
            if (dgb.coin_control.m_allow_other_inputs || !dgb.coin_control.m_allow_paymaster_pool_inputs ||
                std::any_of(dgb.recipients.begin(), dgb.recipients.end(), [](const auto& recipient) { return !recipient.fSubtractFeeFromAmount; })) return fail();
        }
        return true;
    }
    std::vector<std::pair<CScript, CAmount>> expected;
    for (const auto& output : operation.outputs) {
        if ((output.asset == PoolAsset::DGB) != dgb_leg || !(wallet.IsMine(output.script_pub_key) & ISMINE_SPENDABLE)) return fail();
        expected.emplace_back(output.script_pub_key, dgb_leg ? output.dgb_value.value : output.carrier_value.value);
    }
    if (!operation.withdrawal_excess_script_pub_key.empty()) {
        if (dgb_leg || !(wallet.IsMine(operation.withdrawal_excess_script_pub_key) & ISMINE_SPENDABLE)) return fail();
        expected.emplace_back(operation.withdrawal_excess_script_pub_key, operation.withdrawal_excess_amount.value);
    }
    if (recipients != expected) return fail();
    if (dgb_leg) {
        const auto& dgb = std::get<DGBMaintenanceTransfer>(transfer);
        if (dgb.coin_control.m_allow_paymaster_pool_inputs ||
            std::any_of(dgb.recipients.begin(), dgb.recipients.end(), [](const auto& recipient) { return recipient.fSubtractFeeFromAmount; })) return fail();
    } else if (!operation.source_inputs.empty() && selected != operation.source_inputs) return fail();
    return true;
}

bool PrepareMaintenanceAccounting(CWallet& wallet, const ProviderMaintenanceAuthority& authority,
    const ProviderMaintenanceRecord& operation, const ProviderMaintenanceTransaction& result,
    std::vector<ProviderPoolEntry>& pool, ProviderMaintenanceLedger& ledger, std::string& error)
{
    AssertLockHeld(wallet.cs_wallet);
    const int64_t now = GetTime();
    if (const auto* retirement = std::get_if<PoolRetirementMaintenanceAuthority>(&authority)) {
        for (const auto& input : result.transaction->vin) {
            const auto source = std::find_if(retirement->sources.begin(), retirement->sources.end(), [&](const auto& entry) { return entry.outpoint == input.prevout; });
            if (source == retirement->sources.end()) continue; // Ordinary DGB fee funding for a DD leg.
            const auto current = std::find_if(pool.begin(), pool.end(), [&](const auto& entry) { return entry.outpoint == input.prevout; });
            if (current == pool.end() || current->state != PoolEntryState::AVAILABLE || !current->reservation_id.IsNull()) {
                error = "PAYMASTER_REBALANCE_ACTIVE_RESERVATIONS";
                return false;
            }
            current->state = PoolEntryState::SPENT;
            current->updated_at = std::max(current->updated_at, now);
        }
        return true;
    }
    const auto saved = std::find_if(ledger.records.begin(), ledger.records.end(), [&](const auto& record) { return record.operation_id == operation.operation_id; });
    if (saved == ledger.records.end() || saved->state != ProviderMaintenanceState::PLANNED ||
        !saved->transaction_id.IsNull() || !SameMaintenanceOperation(*saved, operation)) {
        error = "PAYMASTER_MAINTENANCE_OPERATION_CONFLICT";
        return false;
    }
    if (operation.IsPreparation()) {
        saved->transaction_id = result.transaction->GetHash();
        saved->actual_fee = result.fee;
        saved->state = ProviderMaintenanceState::BROADCAST;
        ledger.accounting_time_high_water = std::max({ledger.accounting_time_high_water, saved->updated_at, now});
        saved->updated_at = ledger.accounting_time_high_water;
    } else if (!SpendProviderMaintenanceBudget(ledger, operation.operation_id, result.transaction->GetHash(), result.fee, now, error)) return false;
    for (const auto& input : operation.source_inputs) {
        const auto source = std::find_if(pool.begin(), pool.end(), [&](const auto& entry) { return entry.outpoint == input; });
        if (source == pool.end() || source->state != PoolEntryState::RESERVED || source->reservation_id != operation.operation_id) {
            error = "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
            return false;
        }
        source->state = PoolEntryState::COMMITTED;
        source->updated_at = std::max(source->updated_at, now);
    }
    for (const auto& output : operation.outputs) {
        const auto index = FindMaintenanceOutputIndex(*result.transaction, output);
        if (!index) { error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING"; return false; }
        const COutPoint outpoint{result.transaction->GetHash(), *index};
        if (std::any_of(pool.begin(), pool.end(), [&](const auto& entry) { return entry.outpoint == outpoint; })) {
            error = "PAYMASTER_MAINTENANCE_POOL_CONFLICT";
            return false;
        }
        if (!paymaster_store::internal::MakeProviderPoolRoom(pool, 1, error)) return false;
        ProviderPoolEntry entry;
        entry.outpoint = outpoint;
        entry.purpose = output.purpose;
        entry.asset = output.asset;
        entry.state = PoolEntryState::PENDING_SUCCESSOR;
        entry.script_pub_key = output.script_pub_key;
        entry.dgb_value = output.dgb_value;
        entry.carrier_value = output.carrier_value;
        entry.reservation_id = operation.operation_id;
        entry.origin_commit_key = operation.operation_id;
        entry.updated_at = now;
        pool.push_back(std::move(entry));
    }
    return true;
}
bool CheckMaintenanceExecutionState(CWallet& wallet,
    const PaymasterMaintenanceExecution& execution, std::string& error, bool* accounted = nullptr)
{
    AssertLockHeld(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    std::vector<ProviderPoolEntry> pool;
    ProviderMaintenanceLedger ledger;
    const auto status = batch.ReadPaymasterProviderPoolWithStatus(pool);
    if (!ValidatePaymasterMaintenanceExecution(execution) ||
        batch.ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND || identity.provider_id != execution.provider_id ||
        (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) ||
        (execution.has_ledger && batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger) != DatabaseReadStatus::FOUND)) {
        error = "PAYMASTER_MAINTENANCE_RECOVERY_REVIEW_REQUIRED";
        return false;
    }
    for (const auto& leg : execution.legs) {
        CMutableTransaction transaction;
        SpanReader stream{::PROTOCOL_VERSION, leg.transaction};
        stream >> transaction;
        const auto known = wallet.mapWallet.find(transaction.GetHash());
        if (known != wallet.mapWallet.end() && (!known->second.tx || known->second.isAbandoned() ||
            known->second.isConflicted() || known->second.state<TxStateUnrecognized>() ||
            paymaster_store::internal::CanonicalBytes(*known->second.tx) != leg.transaction)) {
            error = "PAYMASTER_MAINTENANCE_RECOVERY_TRANSACTION_CONFLICT";
            return false;
        }
    }
    const bool before = (status == DatabaseReadStatus::FOUND) == execution.had_pool &&
        (HashWriter{} << pool).GetHash() == (HashWriter{} << execution.before_pool).GetHash() &&
        (!execution.has_ledger || (HashWriter{} << ledger).GetHash() == (HashWriter{} << execution.before_ledger).GetHash());
    const bool after = status == DatabaseReadStatus::FOUND &&
        (HashWriter{} << pool).GetHash() == (HashWriter{} << execution.after_pool).GetHash() &&
        (!execution.has_ledger || (HashWriter{} << ledger).GetHash() == (HashWriter{} << execution.after_ledger).GetHash());
    if (!before && !after) { error = "PAYMASTER_MAINTENANCE_RECOVERY_ACCOUNTING_CONFLICT"; return false; }
    if (accounted) *accounted = after;
    return true;
}

bool CommitMaintenanceExecution(CWallet& wallet, const PaymasterMaintenanceExecution& execution,
    PaymasterCheckpointOperation& checkpoint, std::string& error)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!CheckMaintenanceExecutionState(wallet, execution, error)) return false;
    std::vector<CTransactionRef> transactions;
    // Validate every leg before the first commit. Recovery never constructs or
    // signs a replacement for these checkpoint-bound bytes.
    for (const auto& leg : execution.legs) {
        CMutableTransaction transaction;
        SpanReader stream{::PROTOCOL_VERSION, leg.transaction};
        stream >> transaction; // Validated by the immutable journal decoder.
        const auto tx = MakeTransactionRef(std::move(transaction));
        const auto fee = GetProviderFinanceTransactionFee(wallet, tx);
        if (!fee || *fee != leg.fee) { error = "PAYMASTER_MAINTENANCE_RECOVERY_FEE_CHANGED"; return false; }
        transactions.push_back(tx);
    }
    for (size_t index{0}; index < execution.legs.size(); ++index) {
        const auto& leg = execution.legs[index];
        const auto& tx = transactions[index];
        const auto existing = wallet.mapWallet.find(tx->GetHash());
        if (existing == wallet.mapWallet.end()) {
            if (!wallet.CommitTransaction(tx, leg.metadata, {}, &error)) return false;
        } else {
            // AddToWallet can leave its in-memory entry behind when WriteTx
            // fails. Re-persist it before accounting or removing the journal.
            auto& saved = existing->second;
            for (const auto& [key, value] : leg.metadata) {
                const auto found = saved.mapValue.find(key);
                if (found != saved.mapValue.end() && found->second != value) {
                    error = "PAYMASTER_MAINTENANCE_RECOVERY_METADATA_CONFLICT";
                    return false;
                }
                saved.mapValue[key] = value;
            }
            if (!WalletBatch{wallet.GetDatabase()}.WriteTx(saved)) {
                error = "PAYMASTER_MAINTENANCE_RECOVERY_TRANSACTION_WRITE";
                return false;
            }
            if (wallet.GetBroadcastTransactions() && wallet.GetTxDepthInMainChain(saved) == 0 &&
                !wallet.SubmitTxMemoryPoolAndRelay(saved, error, true)) return false;
        }
        if (leg.digidollar) {
            auto* dd_wallet = wallet.GetDDWallet();
            const auto comment = leg.metadata.find("comment");
            if (!dd_wallet || !dd_wallet->ProcessIncomingDDTransaction(tx)) { error = "PAYMASTER_MAINTENANCE_DD_STATE_UPDATE_FAILED"; return false; }
            if (!dd_wallet->RecordPaymasterSendHistory(tx->GetHash(), leg.history_recipient, leg.history_amount.value,
                    error, comment == leg.metadata.end() ? std::string{} : comment->second, leg.multiple_recipients)) return false;
        }
    }
    // Pool and budget transitions are one commit, including both retirement
    // legs. Repeating an already committed transition preserves exact records.
    bool accounted{false};
    // Native wallet callbacks can be reentrant under cs_wallet. Never overwrite
    // an unexpected pool/budget transition observed during a commit callback.
    if (!CheckMaintenanceExecutionState(wallet, execution, error, &accounted)) return false;
    WalletBatch batch{wallet.GetDatabase()};
    if (!accounted) {
        if (!batch.TxnBegin() || !batch.WritePaymasterProviderPool(execution.after_pool) ||
            (execution.has_ledger && !batch.WritePaymasterMaintenanceLedger(execution.after_ledger)) || !batch.TxnCommit()) {
            batch.TxnAbort();
            error = "PAYMASTER_MAINTENANCE_RECOVERY_ACCOUNTING_WRITE";
            return false;
        }
    }
    size_t changes{0};
    if (!ReconcileProviderFinances(wallet, changes, error)) return false;
    return checkpoint.Complete(error);
}
} // namespace

bool ExecuteProviderMaintenanceTransactions(
    CWallet& wallet, const ProviderMaintenanceAuthority& authority,
    const std::vector<ProviderMaintenanceTransfer>& transfers,
    std::vector<ProviderMaintenanceTransaction>& transactions, std::string& error)
{
    using namespace DigiDollar::Paymaster;
    transactions.clear();
    error.clear();
    if (transfers.empty() || transfers.size() > 2 ||
        (transfers.size() != 1 && !std::holds_alternative<PoolRetirementMaintenanceAuthority>(authority))) {
        error = "PAYMASTER_INVALID_MAINTENANCE_TRANSACTION";
        return false;
    }
    if (const auto* retirement = std::get_if<PoolRetirementMaintenanceAuthority>(&authority)) {
        for (const bool dgb : {false, true}) {
            const bool have_sources = std::any_of(retirement->sources.begin(), retirement->sources.end(), [&](const auto& source) {
                return (source.asset == PoolAsset::DGB) == dgb;
            });
            const auto legs = std::count_if(transfers.begin(), transfers.end(), [&](const auto& transfer) {
                return std::holds_alternative<DGBMaintenanceTransfer>(transfer) == dgb;
            });
            if (legs != (have_sources ? 1 : 0)) { error = "PAYMASTER_INVALID_MAINTENANCE_TRANSACTION"; return false; }
        }
    }
    LOCK(wallet.cs_wallet);
    if (wallet.IsLocked()) { error = "PAYMASTER_WALLET_LOCKED"; return false; }
    if (!std::holds_alternative<PoolRetirementMaintenanceAuthority>(authority)) {
        size_t recovered{0};
        if (!ReconcileProviderMaintenance(wallet, recovered, error)) return false;
    }
    ProviderMaintenanceRecord operation;
    DGBSatoshis maximum_fee;
    if (!ValidateMaintenanceExecutionAuthority(wallet, authority, operation, maximum_fee, error)) return false;
    if (maximum_fee.value <= 0 || !MoneyRange(maximum_fee.value)) { error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED"; return false; }
    const bool pool_inputs = std::holds_alternative<CarrierWithdrawalMaintenanceAuthority>(authority) ||
                             std::holds_alternative<PoolRetirementMaintenanceAuthority>(authority);
    std::vector<ProviderMaintenanceTransaction> built;
    for (const auto& transfer : transfers) {
        // Keep authority, input-lock checks and each signer in one wallet-lock
        // interval. A saved plan never overrides an operator's newer lock.
        if (!ValidateMaintenanceExecutionAuthority(wallet, authority, operation, maximum_fee, error)) return false;
        if (!ValidateMaintenanceTransferBinding(wallet, authority, operation, transfer, error)) return false;
        ProviderMaintenanceTransaction result;
        if (const auto* dgb = std::get_if<DGBMaintenanceTransfer>(&transfer)) {
            const auto selected = dgb->coin_control.ListSelected();
            if (!CheckPaymasterNewSignatureInputs(wallet, {selected.begin(), selected.end()}, error)) return false;
            if (dgb->coin_control.m_allow_paymaster_pool_inputs && !pool_inputs) { error = "PAYMASTER_INVALID_MAINTENANCE_TRANSACTION"; return false; }
            auto created = CreateTransaction(wallet, dgb->recipients, -1, dgb->coin_control, true);
            if (!created) { error = MaintenanceBuildFailure(authority, false, util::ErrorString(created).original); return false; }
            result.transaction = created->tx;
            result.fee = DGBSatoshis{created->fee};
        } else {
            const auto& dd = std::get<DDMaintenanceTransfer>(transfer);
            auto* dd_wallet = wallet.GetDDWallet();
            if (!dd_wallet) { error = "PAYMASTER_DD_WALLET_UNAVAILABLE"; return false; }
            auto inputs = dd.plan.dd_utxos;
            inputs.insert(inputs.end(), dd.plan.fee_utxos.begin(), dd.plan.fee_utxos.end());
            if (!CheckPaymasterNewSignatureInputs(wallet, inputs, error)) return false;
            CMutableTransaction transaction;
            if (!dd_wallet->BuildDigiDollarTransfer(dd.plan, transaction, error, pool_inputs)) {
                error = MaintenanceBuildFailure(authority, true, error);
                return false;
            }
            result.transaction = MakeTransactionRef(std::move(transaction));
            const auto fee = GetProviderFinanceTransactionFee(wallet, result.transaction);
            if (!fee || fee->value <= 0 || fee->value > dd.plan.estimated_fee) { error = "PAYMASTER_MAINTENANCE_FEE_CHANGED"; return false; }
            result.fee = *fee;
        }
        if (result.fee.value <= 0 || result.fee.value > maximum_fee.value || result.fee.value > wallet.m_default_max_tx_fee) {
            if (std::holds_alternative<FiniteSetupMaintenanceAuthority>(authority))
                error = strprintf("PAYMASTER_POOL_FEE_LIMIT: signed fee %s DGB exceeds approved setup limit %s DGB",
                    FormatMoney(result.fee.value), FormatMoney(maximum_fee.value));
            else if (std::holds_alternative<PoolRetirementMaintenanceAuthority>(authority)) error = "PAYMASTER_RETIREMENT_FEE_LIMIT";
            else error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
            return false;
        }
        built.push_back(std::move(result));
    }
    PaymasterMaintenanceExecution execution;
    ProviderIdentityRecord identity;
    WalletBatch records{wallet.GetDatabase()};
    if (records.ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND) { error = "PAYMASTER_IDENTITY_NOT_FOUND"; return false; }
    execution.genesis_hash = Params().GenesisBlock().GetHash();
    execution.provider_id = identity.provider_id;
    execution.operation_id = GetRandHash();
    execution.created_at = GetTime();
    const auto pool_status = records.ReadPaymasterProviderPoolWithStatus(execution.before_pool);
    if (pool_status != DatabaseReadStatus::FOUND && pool_status != DatabaseReadStatus::NOT_FOUND) { error = "PAYMASTER_INVALID_PROVIDER_POOL"; return false; }
    execution.had_pool = pool_status == DatabaseReadStatus::FOUND;
    execution.after_pool = execution.before_pool;
    execution.has_ledger = !std::holds_alternative<PoolRetirementMaintenanceAuthority>(authority);
    if (execution.has_ledger && records.ReadPaymasterMaintenanceLedgerWithStatus(execution.before_ledger) != DatabaseReadStatus::FOUND) { error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER"; return false; }
    execution.after_ledger = execution.before_ledger;
    for (size_t index{0}; index < built.size(); ++index) {
        const auto& result = built[index];
        PaymasterMaintenanceExecutionLeg leg;
        leg.transaction = paymaster_store::internal::CanonicalBytes(CMutableTransaction{*result.transaction});
        leg.fee = result.fee;
        if (const auto* retirement = std::get_if<PoolRetirementMaintenanceAuthority>(&authority)) leg.metadata[PAYMASTER_RETIREMENT_PROVIDER_KEY] = retirement->provider_id.GetHex();
        if (const auto* dd = std::get_if<DDMaintenanceTransfer>(&transfers[index])) {
            leg.digidollar = true;
            leg.metadata["comment"] = dd->comment;
            leg.history_recipient = dd->history_recipient;
            leg.history_amount = DDCents{dd->plan.total_amount};
            leg.multiple_recipients = dd->multiple_recipients;
        }
        execution.legs.push_back(std::move(leg));
        if (!PrepareMaintenanceAccounting(wallet, authority, operation, result,
                execution.after_pool, execution.after_ledger, error)) return false;
    }
    PaymasterCheckpointOperation checkpoint{wallet};
    if (!checkpoint.Begin(execution, error)) return false;
    if (!CommitMaintenanceExecution(wallet, execution, checkpoint, error)) return false;
    transactions = std::move(built);
    return true;
}

bool ResumeProviderMaintenanceTransactions(CWallet& wallet, std::string& error)
{
    LOCK(wallet.cs_wallet);
    try {
        PaymasterMaintenanceExecution execution;
        const auto status = WalletBatch{wallet.GetDatabase()}.ReadPaymasterMaintenanceExecutionWithStatus(execution);
        if (status == DatabaseReadStatus::NOT_FOUND) return true;
        if (status != DatabaseReadStatus::FOUND) { error = "PAYMASTER_MAINTENANCE_RECOVERY_REVIEW_REQUIRED"; return false; }
        if (!CheckMaintenanceExecutionState(wallet, execution, error)) return false;
        PaymasterCheckpointOperation checkpoint{wallet};
        if (!checkpoint.Resume(execution, error)) return false;
        return CommitMaintenanceExecution(wallet, execution, checkpoint, error);
    } catch (const std::exception& exception) {
        // CommitTransaction throws after a failed wallet database write.
        // Startup must remain available for inspection, with the exact journal
        // and pending fence retained for another recovery attempt.
        wallet.WalletLogPrintf("Paymaster native transaction recovery failed: %s\n", exception.what());
        error = "PAYMASTER_MAINTENANCE_RECOVERY_TRANSACTION_COMMIT";
        return false;
    }
}

bool ProviderMaintenanceBroadcastPending(
    const CWallet& wallet, const DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger)
{
    LOCK(wallet.cs_wallet);
    for (const auto& record : ledger.records) {
        if (!record.HasUnresolvedTransaction()) continue;
        const auto found = wallet.mapWallet.find(record.transaction_id);
        if (found == wallet.mapWallet.end()) continue; // Reconciliation owns missing evidence.
        const auto& transaction = found->second;
        if (!transaction.isAbandoned() && !transaction.isConflicted() &&
            wallet.GetTxDepthInMainChain(transaction) == 0 &&
            !transaction.InMempool() && !wallet.chain().isInStempool(record.transaction_id)) return true;
    }
    return false;
}

bool RunAutomaticDGBReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!CheckPaymasterProviderRestoreGuard(wallet, error)) return false;
    // A bounded history pass may still be locating a transaction from a lost
    // reply. Do not sign another transaction for that plan while it is unknown.
    size_t recovered{0};
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) return false;
    if (recovered != 0) {
        error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
        return false; // Re-observe slot deficits before planning any new work.
    }
    if (missing_admission == 0 && missing_operational == 0) return true;
    if (!policy.automatic_replenishment || !policy.paid_maintenance_approved) {
        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
    if (!CheckPaymasterPoolFundingBroadcast(wallet, error)) return false;
    ProviderPolicy provider_policy;
    if (!GetPaymasterProviderPolicy(wallet, provider_policy)) {
        error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }

    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    std::optional<ProviderMaintenanceRecord> operation;
    {
        LOCK(wallet.cs_wallet);
        // Readiness reconciliation also writes this journal. Keep planning,
        // budget reservation and persistence in one read-modify-write interval.
        WalletBatch batch{wallet.GetDatabase()};
        ProviderLiquidityPolicy saved_policy;
        const DatabaseReadStatus ledger_status{
            ReadPaymasterMaintenanceState(batch, saved_policy, ledger)};
        if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        // Validate the pool before reserving budget, creating destinations, or
        // constructing a transaction. Re-read it after commit to cover a
        // concurrent change without ever overwriting an unreadable record.
        if (!ReadOptionalProviderPool(batch, pool, error)) return false;
        const auto planned = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [](const ProviderMaintenanceRecord& record) {
                return record.kind == ProviderMaintenanceKind::REPLENISH_DGB &&
                       record.state == ProviderMaintenanceState::PLANNED;
            });
        if (planned != ledger.records.end()) operation = *planned;

        const int64_t admission_value = MIN_ADMISSION_DGB_SATOSHIS;
        const int64_t operational_value = std::max<int64_t>(
            MIN_ADMISSION_DGB_SATOSHIS,
            provider_policy.maximum_network_fee.value);
        if (operation) {
            size_t planned_admission{0};
            size_t planned_operational{0};
            const bool outputs_match = std::all_of(
                operation->outputs.begin(), operation->outputs.end(),
                [&](const ProviderMaintenanceOutput& output) {
                    if (output.asset != PoolAsset::DGB ||
                        output.carrier_value.value != 0) {
                        return false;
                    }
                    if (output.purpose == PoolPurpose::ADMISSION &&
                        output.dgb_value.value == admission_value) {
                        ++planned_admission;
                        return true;
                    }
                    if (output.purpose == PoolPurpose::OPERATIONAL &&
                        output.dgb_value.value == operational_value) {
                        ++planned_operational;
                        return true;
                    }
                    return false;
                });
            const bool plan_matches =
                outputs_match && (planned_admission == missing_admission || planned_operational == 0) &&
                planned_admission <= missing_admission &&
                planned_operational <= missing_operational &&
                operation->maximum_fee ==
                    policy.maximum_maintenance_fee_per_transaction &&
                operation->plan_id ==
                    MaintenancePlanId(operation->kind, operation->outputs, policy);
            if (!plan_matches) {
                ProviderMaintenanceLedger released;
                int64_t released_fee{0};
                if (!PreparePaymasterMaintenanceCancellation(wallet, ledger, {operation->operation_id},
                        GetTime(), released, released_fee, error)) return false;
                ledger = std::move(released);
                {
                    LOCK(wallet.cs_wallet);
                    if (!WalletBatch{wallet.GetDatabase()}
                             .WritePaymasterMaintenanceLedger(ledger)) {
                        error = "PAYMASTER_DATABASE_WRITE";
                        return false;
                    }
                }
                operation.reset();
            }
        }
        if (!operation) {
            ProviderMaintenanceRecord record;
            do {
                record.operation_id = GetRandHash();
            } while (record.operation_id.IsNull());
            record.kind = ProviderMaintenanceKind::REPLENISH_DGB;
            const auto append_outputs = [&](PoolPurpose purpose, size_t count,
                                            int64_t value, const char* label) {
                for (size_t index{0}; index < count; ++index) {
                    const auto destination = wallet.GetNewDestination(
                        OutputType::BECH32M, label);
                    if (!destination) return false;
                    ProviderMaintenanceOutput output;
                    output.purpose = purpose;
                    output.asset = PoolAsset::DGB;
                    output.script_pub_key = GetScriptForDestination(*destination);
                    output.dgb_value = DGBSatoshis{value};
                    record.outputs.push_back(std::move(output));
                }
                return true;
            };
            if (!append_outputs(PoolPurpose::ADMISSION, missing_admission,
                                admission_value, "Paymaster admission maintenance") ||
                !append_outputs(PoolPurpose::OPERATIONAL, missing_operational,
                                operational_value, "Paymaster operational maintenance")) {
                error = "PAYMASTER_POOL_DESTINATION_UNAVAILABLE";
                return false;
            }
            record.plan_id = MaintenancePlanId(record.kind, record.outputs, policy);
            record.maximum_fee = policy.maximum_maintenance_fee_per_transaction;
            if (!ReserveProviderMaintenanceBudget(
                    ledger, policy, record, GetTime(), error)) {
                return false;
            }
            {
                LOCK(wallet.cs_wallet);
                if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterMaintenanceLedger(ledger)) {
                    error = "PAYMASTER_DATABASE_WRITE";
                    return false;
                }
            }
            operation = std::move(record);
        }

    }

    std::vector<CRecipient> recipients;
    CAmount required_dgb{0};
    for (const ProviderMaintenanceOutput& output : operation->outputs) {
        CTxDestination destination;
        if (output.asset != PoolAsset::DGB ||
            !MoneyRange(output.dgb_value.value) ||
            output.dgb_value.value > MAX_MONEY - required_dgb ||
            !ExtractDestination(output.script_pub_key, destination)) {
            error = "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            return false;
        }
        recipients.push_back({destination, output.dgb_value.value, false});
        required_dgb += output.dgb_value.value;
    }
    CCoinControl coin_control;
    // As with finite setup, stem-only change is invisible to the ordinary
    // broadcast fee precheck. Wait for confirmed funding before signing.
    coin_control.m_min_depth = 1;
    auto created = CreateTransaction(wallet, recipients, /*change_pos=*/-1,
                                     coin_control, /*sign=*/false);
    if (!created) {
        CAmount available;
        {
            LOCK(wallet.cs_wallet);
            available = AvailableCoins(wallet, &coin_control).GetTotalAmount();
        }
        // Admission outputs come first. Keep their priority, then create as
        // many operational outputs as ordinary funding (including fees) permits.
        // Never change a reserve's value or use the protected pool as funding.
        while (!created && recipients.size() > 1) {
            required_dgb -= recipients.back().nAmount;
            recipients.pop_back();
            if (required_dgb >= available) continue; // A positive fee is also needed.
            created = CreateTransaction(wallet, recipients, /*change_pos=*/-1,
                                        coin_control, /*sign=*/false);
        }
    }
    if (!created) {
        // Diagnose a definite capital shortfall using ordinary coin selection:
        // protected reserves, locked coins and unconfirmed change stay excluded.
        // Other construction failures must not masquerade as missing capital.
        LOCK(wallet.cs_wallet);
        error = AvailableCoins(wallet, &coin_control).GetTotalAmount() <= required_dgb
            ? "PAYMASTER_POOL_WAITING_DGB"
            : "PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED";
        return false;
    }
    if (created->fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }
    if (!ReduceReplenishmentPlan(wallet, policy, *operation, recipients.size(), error)) return false;
    std::vector<ProviderMaintenanceTransaction> transactions;
    const DGBMaintenanceTransfer transfer{recipients, coin_control};
    return ExecuteProviderMaintenanceTransactions(wallet,
        RecurringRefillMaintenanceAuthority{*operation, policy}, {transfer}, transactions, error);
}

bool RunAutomaticCarrierReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!CheckPaymasterProviderRestoreGuard(wallet, error)) return false;
    // A bounded history pass may still be locating a transaction from a lost
    // reply. Do not sign another transaction for that plan while it is unknown.
    size_t recovered{0};
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) return false;
    if (recovered != 0) {
        error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
        return false; // Re-observe slot deficits before planning any new work.
    }
    if (missing_admission == 0 && missing_operational == 0) return true;
    if (!policy.automatic_replenishment || !policy.paid_maintenance_approved) {
        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
    if (!CheckPaymasterPoolFundingBroadcast(wallet, error)) return false;
    DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
    if (!dd_wallet) {
        error = "PAYMASTER_DD_WALLET_UNAVAILABLE";
        return false;
    }

    ProviderMaintenanceLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    std::optional<ProviderMaintenanceRecord> operation;
    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        ProviderLiquidityPolicy saved_policy;
        const DatabaseReadStatus ledger_status{
            ReadPaymasterMaintenanceState(batch, saved_policy, ledger)};
        if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        if (!ReadOptionalProviderPool(batch, pool, error)) return false;
        const auto planned = std::find_if(
            ledger.records.begin(), ledger.records.end(),
            [](const ProviderMaintenanceRecord& record) {
                return record.kind == ProviderMaintenanceKind::REPLENISH_CARRIER &&
                       record.state == ProviderMaintenanceState::PLANNED;
            });
        if (planned != ledger.records.end()) operation = *planned;

        if (operation) {
            size_t planned_admission{0};
            size_t planned_operational{0};
            const bool outputs_match = std::all_of(
                operation->outputs.begin(), operation->outputs.end(),
                [&](const ProviderMaintenanceOutput& output) {
                    if (output.asset != PoolAsset::DD_CARRIER ||
                        output.dgb_value.value != 0 ||
                        output.carrier_value.value != 100) {
                        return false;
                    }
                    if (output.purpose == PoolPurpose::ADMISSION) {
                        ++planned_admission;
                        return true;
                    }
                    if (output.purpose == PoolPurpose::OPERATIONAL) {
                        ++planned_operational;
                        return true;
                    }
                    return false;
                });
            const bool plan_matches =
                outputs_match && (planned_admission == missing_admission || planned_operational == 0) &&
                planned_admission <= missing_admission &&
                planned_operational <= missing_operational &&
                operation->maximum_fee ==
                    policy.maximum_maintenance_fee_per_transaction &&
                operation->plan_id ==
                    MaintenancePlanId(operation->kind, operation->outputs, policy);
            if (!plan_matches) {
                ProviderMaintenanceLedger released;
                int64_t released_fee{0};
                if (!PreparePaymasterMaintenanceCancellation(wallet, ledger, {operation->operation_id},
                        GetTime(), released, released_fee, error)) return false;
                ledger = std::move(released);
                {
                    LOCK(wallet.cs_wallet);
                    if (!WalletBatch{wallet.GetDatabase()}
                             .WritePaymasterMaintenanceLedger(ledger)) {
                        error = "PAYMASTER_DATABASE_WRITE";
                        return false;
                    }
                }
                operation.reset();
            }
        }

        if (!operation) {
            ProviderMaintenanceRecord record;
            do {
                record.operation_id = GetRandHash();
            } while (record.operation_id.IsNull());
            record.kind = ProviderMaintenanceKind::REPLENISH_CARRIER;
            const auto append_outputs = [&](PoolPurpose purpose, size_t count,
                                            const char* label) {
                for (size_t index{0}; index < count; ++index) {
                    const auto destination = wallet.GetNewDestination(
                        OutputType::BECH32M, label);
                    if (!destination) return false;
                    ProviderMaintenanceOutput output;
                    output.purpose = purpose;
                    output.asset = PoolAsset::DD_CARRIER;
                    output.script_pub_key = GetScriptForDestination(*destination);
                    output.carrier_value = DDCents{100};
                    record.outputs.push_back(std::move(output));
                }
                return true;
            };
            if (!append_outputs(PoolPurpose::ADMISSION, missing_admission,
                                "Paymaster admission carrier maintenance") ||
                !append_outputs(PoolPurpose::OPERATIONAL, missing_operational,
                                "Paymaster operational carrier maintenance")) {
                error = "PAYMASTER_POOL_DESTINATION_UNAVAILABLE";
                return false;
            }
            record.plan_id = MaintenancePlanId(record.kind, record.outputs, policy);
            record.maximum_fee = policy.maximum_maintenance_fee_per_transaction;
            if (!ReserveProviderMaintenanceBudget(
                    ledger, policy, record, GetTime(), error)) {
                return false;
            }
            {
                LOCK(wallet.cs_wallet);
                if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterMaintenanceLedger(ledger)) {
                    error = "PAYMASTER_DATABASE_WRITE";
                    return false;
                }
            }
            operation = std::move(record);
        }

    }

    std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
    recipients.reserve(operation->outputs.size());
    for (const ProviderMaintenanceOutput& output : operation->outputs) {
        CTxDestination destination;
        if (output.asset != PoolAsset::DD_CARRIER ||
            !ExtractDestination(output.script_pub_key, destination)) {
            error = "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            return false;
        }
        const std::string encoded = EncodeDigiDollarAddress(destination);
        CDigiDollarAddress address{encoded};
        if (encoded.empty() || !address.IsValid()) {
            error = "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE";
            return false;
        }
        recipients.emplace_back(std::move(address), output.carrier_value.value);
    }

    DDTransferPlan plan;
    std::string plan_error;
    bool funded = dd_wallet->PlanFundedDigiDollarTransfer(
            recipients, plan, plan_error,
            /*preset_dd_inputs=*/nullptr,
            /*allow_paymaster_pool_inputs=*/false);
    while (!funded && recipients.size() > 1 &&
           (plan.error_code == DDTransferPlanError::INSUFFICIENT_DD_INPUTS ||
            plan.error_code == DDTransferPlanError::INSUFFICIENT_DGB_FEE_INPUTS)) {
        recipients.pop_back();
        funded = dd_wallet->PlanFundedDigiDollarTransfer(
            recipients, plan, plan_error, /*preset_dd_inputs=*/nullptr,
            /*allow_paymaster_pool_inputs=*/false);
    }
    if (!funded) {
        // Preserve the ordinary planner's funding diagnosis without parsing
        // display text or changing its protected-pool selection rules.
        error = plan.error_code == DDTransferPlanError::INSUFFICIENT_DD_INPUTS
            ? "PAYMASTER_POOL_WAITING_FUNDS"
            : plan.error_code == DDTransferPlanError::INSUFFICIENT_DGB_FEE_INPUTS
                ? "PAYMASTER_POOL_WAITING_DGB"
                : "PAYMASTER_CARRIER_REPLENISHMENT_FAILED: " + plan_error;
        return false;
    }
    if (plan.estimated_fee <= 0 ||
        plan.estimated_fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }

    if (!ReduceReplenishmentPlan(wallet, policy, *operation, recipients.size(), error)) return false;

    std::vector<ProviderMaintenanceTransaction> transactions;
    const DDMaintenanceTransfer transfer{plan, operation->outputs.front().script_pub_key,
        "Paymaster automatic carrier replenishment", recipients.size() > 1};
    return ExecuteProviderMaintenanceTransactions(wallet,
        RecurringRefillMaintenanceAuthority{*operation, policy}, {transfer}, transactions, error);
}

void EnsureProviderRestoreSafe(const CWallet& wallet)
{
    std::string error;
    if (!CheckPaymasterProviderRestoreGuard(wallet, error)) {
        throw DigiDollar::Paymaster::ApplicationError(DigiDollar::Paymaster::ErrorKind::WALLET, error);
    }
}

bool ProviderCanServeDuringRefill(
    const DigiDollar::Paymaster::ProviderServiceStatus& status, const ProviderReadiness& readiness, bool synchronized)
{
    if (!synchronized || !readiness.ready || !readiness.pool_ready ||
        !readiness.errors.empty() || readiness.pool.complete_operational_slots == 0) return false;
    // These reasons concern additional reserves, not authority to spend the
    // independent confirmed capacity that passed the full readiness checks.
    // Unknown construction, journal, signing or database failures stay closed.
    switch (status.state) {
    case DigiDollar::Paymaster::ProviderServiceState::REPLENISHING_LIQUIDITY:
        return status.last_error == "PAYMASTER_POOL_WAITING_DGB" ||
               status.last_error == "PAYMASTER_POOL_WAITING_FUNDS" ||
               status.last_error == "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
    case DigiDollar::Paymaster::ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL:
        return status.last_error == "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED" ||
               status.last_error == "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED";
    case DigiDollar::Paymaster::ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION:
        return status.last_error == "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
    case DigiDollar::Paymaster::ProviderServiceState::WAITING_FOR_READINESS:
        return status.last_error == "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED" ||
               status.last_error == "PAYMASTER_WALLET_BROADCAST_DISABLED" ||
               status.last_error == "PAYMASTER_MAINTENANCE_BROADCAST_PENDING" ||
               (status.last_error == "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE" &&
                (readiness.available_funding_models & DigiDollar::Paymaster::FUNDING_MODEL_SPONSORED) != 0);
    default:
        return false;
    }
}

ProviderWorkPlan PlanProviderWork(const DigiDollar::Paymaster::ProviderServiceStatus& status,
                                  bool accept_new_requests, bool synchronized)
{
    using namespace DigiDollar::Paymaster;
    if (!synchronized) return {};
    switch (status.state) {
    case ProviderServiceState::ACTIVE:
        return {accept_new_requests, true, true};
    case ProviderServiceState::DRAIN_ONLY:
        return {false, false, true};
    case ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL:
    case ProviderServiceState::REPLENISHING_LIQUIDITY:
    case ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION:
        return {false, true, true};
    case ProviderServiceState::WAITING_FOR_READINESS:
        if (status.last_error == "PAYMASTER_POOL_PREPARATION_PENDING" ||
            status.last_error == "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED" ||
            status.last_error == "PAYMASTER_MAINTENANCE_BROADCAST_PENDING" ||
            status.last_error == "PAYMASTER_WALLET_BROADCAST_DISABLED" ||
            status.last_error == "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") return {false, true, true};
        return {};
    default:
        return {};
    }
}

ProviderActivitySnapshot CaptureProviderActivity(const ProviderReadiness& readiness, int64_t now)
{
    using namespace DigiDollar::Paymaster;
    ProviderActivitySnapshot result;
    result.active_payments = std::count_if(
        readiness.budget_ledger.reservations.begin(), readiness.budget_ledger.reservations.end(),
        [](const auto& reservation) { return reservation.state == BudgetReservationState::RESERVED; });
    result.capacity_requests = std::count_if(
        readiness.budget_ledger.capacity_admissions.begin(), readiness.budget_ledger.capacity_admissions.end(),
        [now](const auto& admission) { return admission.state == CapacityAdmissionState::RESERVED && admission.expires_at > now; });
    result.reserved_outputs = std::count_if(
        readiness.pool_entries.begin(), readiness.pool_entries.end(),
        [](const auto& entry) { return entry.purpose == PoolPurpose::OPERATIONAL && entry.state == PoolEntryState::RESERVED; });
    result.pending_confirmations = std::count_if(
        readiness.pool_entries.begin(), readiness.pool_entries.end(),
        [](const auto& entry) {
            return entry.state == PoolEntryState::PENDING_SUCCESSOR ||
                   (entry.state == PoolEntryState::AVAILABLE && entry.confirmation_height <= 0);
        });
    return result;
}

ProviderLifecycleStatus CaptureProviderLifecycleStatus(
    const CWallet& wallet, const WalletContext& context,
    const ProviderReadiness& readiness, bool synchronized)
{
    using namespace DigiDollar::Paymaster;
    LOCK(wallet.cs_wallet);
    ProviderLifecycleStatus result;
    const auto* manager = context.paymaster;
    if (!manager || !manager->Enabled() || !readiness.have_identity) return result;
    result.running = manager->IsProviderRunning(wallet.GetName(), readiness.identity.provider_id);
    result.start_requested = manager->HasRequestedProviderStart(wallet.GetName(), readiness.identity.provider_id);
    if (!readiness.have_settings || !readiness.settings.enabled) return result;
    result.service = ProviderServiceStatusForObservation(
        manager->GetProviderServiceStatus(wallet.GetName()), readiness.ready, synchronized);
    if (!result.running) {
        result.service = {};
        if (readiness.settings.autostart || result.start_requested) {
            result.service.state = wallet.IsLocked() ? ProviderServiceState::WAITING_FOR_UNLOCK
                                                   : ProviderServiceState::WAITING_FOR_READINESS;
        }
    } else if (readiness.settings.operation_mode == ProviderOperationMode::MANUAL) {
        result.service.state = ProviderServiceState::MANUAL;
    } else if (wallet.IsLocked()) {
        result.service.state = ProviderServiceState::WAITING_FOR_UNLOCK;
        result.service.last_error = "PAYMASTER_WALLET_LOCKED";
    } else if (result.service.state == ProviderServiceState::STOPPED) {
        result.service.state = ProviderServiceState::ACTIVE;
    }
    return result;
}


ProviderAutomationSnapshot CaptureProviderAutomationStatus(const CWallet& wallet, const WalletContext& context,
                                        const ProviderReadiness& readiness, int64_t now, bool synchronized)
{
    using namespace DigiDollar::Paymaster;
    LOCK(wallet.cs_wallet);
    const auto& policy = readiness.liquidity_policy;
    const bool configured = readiness.have_liquidity_policy;
    const bool enabled = configured && policy.automatic_replenishment;
    const bool approved = configured && policy.paid_maintenance_approved;
    const auto lifecycle = CaptureProviderLifecycleStatus(wallet, context, readiness, synchronized);
    const bool running = lifecycle.running;
    ProviderAutomationState state{ProviderAutomationState::READY};
    std::string reason;
    bool pending{false}, planned{false};
    for (const auto& record : readiness.maintenance_ledger.records) {
        if (record.IsPreparation()) continue;
        pending |= record.state == ProviderMaintenanceState::BROADCAST;
        planned |= record.state == ProviderMaintenanceState::PLANNED;
    }
    if (!enabled) {
        state = ProviderAutomationState::OFF;
        reason = configured ? "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED" : "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
    } else if (!approved || policy.maximum_maintenance_fee_per_transaction.value <= 0 ||
               policy.maximum_maintenance_fee_per_hour.value <= 0 ||
               policy.maximum_maintenance_fee_per_day.value <= 0) {
        state = ProviderAutomationState::BLOCKED; reason = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
    } else if (!readiness.settings.enabled || !running) {
        state = ProviderAutomationState::PAUSED; reason = "PAYMASTER_PROVIDER_STOPPED";
    } else if (wallet.IsLocked()) {
        state = ProviderAutomationState::PAUSED; reason = "PAYMASTER_WALLET_LOCKED";
    } else if (readiness.settings.operation_mode != ProviderOperationMode::AUTOMATIC) {
        state = ProviderAutomationState::PAUSED; reason = "PAYMASTER_MANUAL_PROCESSING";
    } else {
        const auto& service = lifecycle.service;
        reason = service.last_error;
        const bool faulted = service.state == ProviderServiceState::FAULT;
        if (faulted) {
            state = ProviderAutomationState::BLOCKED;
            if (reason.empty()) reason = "PAYMASTER_AUTOMATIC_SERVICE_ERROR";
        } else if (service.state == ProviderServiceState::DRAIN_ONLY) {
            state = ProviderAutomationState::PAUSED;
            if (reason.empty()) reason = "PAYMASTER_PROVIDER_DRAIN_ONLY";
        } else if (reason == "PAYMASTER_PROVIDER_SYNCING") state = ProviderAutomationState::PAUSED;
        else if (ProviderMaintenanceBroadcastPending(wallet, readiness.maintenance_ledger)) {
            state = ProviderAutomationState::BLOCKED;
            reason = wallet.GetBroadcastTransactions() ? "PAYMASTER_MAINTENANCE_BROADCAST_PENDING"
                                                       : "PAYMASTER_WALLET_BROADCAST_DISABLED";
        }
        else if (pending) state = ProviderAutomationState::WAITING_CONFIRMATION;
        else if (!reason.empty() && reason != "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING") state = ProviderAutomationState::BLOCKED;
        else if (planned) state = ProviderAutomationState::WORKING;
        else if (!readiness.ready) state = ProviderAutomationState::BLOCKED;
        const auto activity = CaptureProviderActivity(readiness, now);
        const bool payment_in_use = activity.active_payments > 0 ||
            activity.capacity_requests > 0 ||
            activity.reserved_outputs > 0;
        const int64_t minimum_operational_dgb = readiness.have_policy
            ? readiness.policy.maximum_network_fee.value : std::numeric_limits<int64_t>::max();
        const bool targets_accounted_for =
            CountLiquiditySlots(readiness.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB, policy.target_admission_dgb).missing == 0 &&
            CountLiquiditySlots(readiness.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER, policy.target_admission_carriers).missing == 0 &&
            CountLiquiditySlots(readiness.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB, policy.target_operational_dgb, minimum_operational_dgb).missing == 0 &&
            CountLiquiditySlots(readiness.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, policy.target_operational_carriers).missing == 0;
        if (!faulted && !pending && !planned && !readiness.ready && payment_in_use && targets_accounted_for &&
            readiness.errors.size() == 1 && readiness.errors.front() == "PAYMASTER_OPERATIONAL_SLOT_MISSING" &&
            (reason.empty() || reason == "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING")) {
            state = ProviderAutomationState::PAUSED;
            reason = "PAYMASTER_PAYMENT_CAPACITY_IN_USE";
        } else if (!faulted && reason == "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING") {
            if (readiness.ready && !pending) reason.clear();
            else if (activity.pending_confirmations > 0)
                state = ProviderAutomationState::WAITING_CONFIRMATION;
            else if (state == ProviderAutomationState::BLOCKED && !readiness.errors.empty()) reason = readiness.errors.front();
        }
        if (state == ProviderAutomationState::BLOCKED && reason.empty() && !readiness.errors.empty()) reason = readiness.errors.front();
    }
    return {configured, enabled, approved, state, std::move(reason), pending};
}

} // namespace wallet::paymaster_app

namespace wallet {
void ForgetPaymasterMaintenanceCache(WalletDatabase& database)
{
    std::shared_ptr<paymaster_app::MaintenanceHistory> state;
    {
        std::lock_guard<std::mutex> lock{paymaster_app::maintenance_history_mutex};
        const auto existing = paymaster_app::maintenance_histories.find(&database);
        if (existing == paymaster_app::maintenance_histories.end()) return;
        state = std::move(existing->second);
        paymaster_app::maintenance_histories.erase(existing);
    }
    // Disconnect outside the registry lock, like the other derived wallet caches.
}
} // namespace wallet
