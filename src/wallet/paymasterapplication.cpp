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
#include <wallet/paymasterstore_internal.h>
#include <wallet/spend.h>

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
                recovered_successors, reconciliation_error)) {
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
    if (result.have_policy && result.have_safety_policy && result.have_budget_ledger) {
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
                       result.policy.maximum_network_fee, GetTime())
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
        result.errors.push_back("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE");
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
        result.endpoint = LookupNumeric(context.args->GetArg("-paymasterendpoint", ""));
        const bool local_regtest = Params().GetChainType() == ChainType::REGTEST &&
                                   result.endpoint.IsLocal();
        if (!result.endpoint.IsValid() ||
            (!result.endpoint.IsRoutable() && !local_regtest)) {
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

bool MakeProviderPoolRoomForMaintenance(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    size_t additional,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    while (entries.size() + additional > 256) {
        const auto removable = std::min_element(
            entries.begin(), entries.end(),
            [](const ProviderPoolEntry& lhs, const ProviderPoolEntry& rhs) {
                const bool lhs_active = IsActiveProviderPoolState(lhs.state);
                const bool rhs_active = IsActiveProviderPoolState(rhs.state);
                if (lhs_active != rhs_active) return !lhs_active;
                return lhs.updated_at < rhs.updated_at;
            });
        if (removable == entries.end() ||
            IsActiveProviderPoolState(removable->state)) {
            error = "PAYMASTER_POOL_TOO_LARGE";
            return false;
        }
        entries.erase(removable);
    }
    return true;
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
            if (!ReleaseProviderMaintenanceBudget(
                    ledger, previous.operation_id, GetTime(), error)) {
                return false;
            }
            ledger_changed = true;
            for (ProviderPoolEntry& entry : pool) {
                if (entry.state == PoolEntryState::RESERVED &&
                    entry.reservation_id == previous.operation_id) {
                    entry.state = PoolEntryState::AVAILABLE;
                    entry.reservation_id.SetNull();
                    entry.updated_at = GetTime();
                    pool_changed = true;
                }
            }
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

bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error)
{
    LOG_TIME_MILLIS_WITH_CATEGORY("Paymaster reconciliation", BCLog::BENCH);
    using namespace DigiDollar::Paymaster;
    recovered = 0;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderMaintenanceLedger ledger;
    const DatabaseReadStatus ledger_status{
        batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
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
    for (ProviderMaintenanceRecord& record : ledger.records) {
        if (record.state != ProviderMaintenanceState::PLANNED &&
            record.state != ProviderMaintenanceState::CONFIRMED &&
            !record.HasUnresolvedTransaction()) {
            continue;
        }
        // A conflict/abandon observation is reversible. Keep following the
        // exact saved maintenance transaction; never construct a replacement here.
        const auto matches = [&](const CWalletTx& wallet_tx) {
            if (!wallet_tx.tx) return false;
            for (const COutPoint& source : record.source_inputs) {
                if (std::none_of(
                        wallet_tx.tx->vin.begin(), wallet_tx.tx->vin.end(),
                        [&](const CTxIn& input) {
                            return input.prevout == source;
                        })) {
                    return false;
                }
            }
            for (const ProviderMaintenanceOutput& output : record.outputs) {
                if (!FindMaintenanceOutputIndex(*wallet_tx.tx, output)) return false;
            }
            if (record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS &&
                !FindExactDigiDollarOutputIndex(
                    *wallet_tx.tx,
                    record.withdrawal_excess_script_pub_key,
                    record.withdrawal_excess_amount)) {
                return false;
            }
            return true;
        };
        auto transaction = wallet.mapWallet.end();
        if (!record.transaction_id.IsNull()) {
            transaction = wallet.mapWallet.find(record.transaction_id);
            if (transaction != wallet.mapWallet.end() && !matches(transaction->second)) {
                error = "PAYMASTER_MAINTENANCE_TRANSACTION_CONFLICT";
                return false;
            }
        } else {
            for (auto candidate = wallet.mapWallet.begin(); candidate != wallet.mapWallet.end(); ++candidate) {
                if (!matches(candidate->second)) continue;
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
                        if (!ReleaseProviderMaintenanceBudget(
                                ledger, record.operation_id, now, error)) {
                            return false;
                        }
                        ledger_changed = true;
                        for (ProviderPoolEntry* source : sources) {
                            source->state = PoolEntryState::AVAILABLE;
                            source->reservation_id.SetNull();
                            source->updated_at = now;
                            pool_changed = true;
                        }
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
                const auto source = std::find_if(
                    pool.begin(), pool.end(),
                    [&](const ProviderPoolEntry& entry) {
                        return entry.outpoint == source_input;
                    });
                if (source == pool.end()) {
                    continue;
                }
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
            const auto source = std::find_if(
                pool.begin(), pool.end(),
                [&](const ProviderPoolEntry& entry) {
                    return entry.outpoint == source_input;
                });
            if (source == pool.end()) {
                error = "PAYMASTER_MAINTENANCE_SOURCE_MISSING";
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
                if (!MakeProviderPoolRoomForMaintenance(pool, 1, error)) {
                    return false;
                }
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
    if (!ledger_changed && !pool_changed) return true;
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
    return true;
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
    bool changed{false};
    for (const auto& record : ledger.records) {
        const bool unneeded = (dgb_satisfied && record.kind == ProviderMaintenanceKind::REPLENISH_DGB) ||
                              (carriers_satisfied && record.kind == ProviderMaintenanceKind::REPLENISH_CARRIER);
        if (!unneeded || record.state != ProviderMaintenanceState::PLANNED || !record.transaction_id.IsNull()) continue;
        if (!ReleaseProviderMaintenanceBudget(ledger, record.operation_id, GetTime(), error)) return false;
        changed = true;
    }
    if (changed && !batch.WritePaymasterMaintenanceLedger(ledger)) {
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
    if (!ReleaseProviderMaintenanceBudget(ledger, operation.operation_id, GetTime(), error) ||
        !ReserveProviderMaintenanceBudget(ledger, policy, smaller, GetTime(), error)) return false;
    if (!batch.WritePaymasterMaintenanceLedger(ledger)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    operation = std::move(smaller);
    return true;
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
    if (missing_admission == 0 && missing_operational == 0) return true;
    if (!policy.automatic_replenishment || !policy.paid_maintenance_approved) {
        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
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
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status == DatabaseReadStatus::NOT_FOUND) {
            ledger.accounting_time_high_water = GetTime();
        } else if (ledger_status != DatabaseReadStatus::FOUND) {
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
                if (!ReleaseProviderMaintenanceBudget(
                        ledger, operation->operation_id, GetTime(), error)) {
                    return false;
                }
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
    // Run the normal signed construction again against current wallet inputs;
    // all ordinary transaction, fee, signature and size checks remain in force.
    created = CreateTransaction(wallet, recipients, /*change_pos=*/-1, coin_control, /*sign=*/true);
    if (!created) {
        error = "PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED";
        return false;
    }
    if (created->fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }
    LOCK(wallet.cs_wallet);
    PaymasterCheckpointOperation checkpoint{wallet};
    if (!checkpoint.Begin(error)) return false;
    std::string commit_error;
    if (!wallet.CommitTransaction(created->tx, {}, {}, &commit_error)) {
        error = "PAYMASTER_POOL_TRANSACTION_REJECTED: " + commit_error;
        return false;
    }

    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        if (!ReadOptionalProviderPool(batch, pool, error) ||
            !SpendProviderMaintenanceBudget(
                ledger, operation->operation_id, created->tx->GetHash(),
                DGBSatoshis{created->fee}, GetTime(), error)) {
            if (error.empty()) {
                error = "PAYMASTER_MAINTENANCE_LEDGER_UPDATE_FAILED";
            }
            return false;
        }
        const size_t missing_pool_entries = std::count_if(
            operation->outputs.begin(), operation->outputs.end(),
            [&](const ProviderMaintenanceOutput& output) {
                const auto output_index = FindMaintenanceOutputIndex(
                    *created->tx, output);
                return output_index &&
                       std::none_of(pool.begin(), pool.end(),
                                    [&](const ProviderPoolEntry& existing) {
                                        return existing.outpoint ==
                                               COutPoint{created->tx->GetHash(),
                                                         *output_index};
                                    });
            });
        if (!MakeProviderPoolRoomForMaintenance(
                pool, missing_pool_entries, error)) {
            return false;
        }
        for (const ProviderMaintenanceOutput& output : operation->outputs) {
            const auto output_index = FindMaintenanceOutputIndex(
                *created->tx, output);
            if (!output_index) {
                error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING";
                return false;
            }
            ProviderPoolEntry entry;
            entry.outpoint = COutPoint{created->tx->GetHash(), *output_index};
            entry.purpose = output.purpose;
            entry.asset = PoolAsset::DGB;
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.script_pub_key = output.script_pub_key;
            entry.dgb_value = output.dgb_value;
            entry.reservation_id = operation->operation_id;
            entry.origin_commit_key = operation->operation_id;
            entry.updated_at = GetTime();
            if (std::none_of(pool.begin(), pool.end(), [&](const ProviderPoolEntry& existing) {
                    return existing.outpoint == entry.outpoint;
                })) pool.push_back(std::move(entry));
        }
        if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterMaintenanceLedger(ledger) ||
            !batch.WritePaymasterProviderPool(pool)) {
            batch.TxnAbort();
            error = "PAYMASTER_DATABASE_WRITE";
            return false;
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
    }
    return checkpoint.Complete(error);
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
    if (missing_admission == 0 && missing_operational == 0) return true;
    if (!policy.automatic_replenishment || !policy.paid_maintenance_approved) {
        error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
        return false;
    }
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
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status == DatabaseReadStatus::NOT_FOUND) {
            ledger.accounting_time_high_water = GetTime();
        } else if (ledger_status != DatabaseReadStatus::FOUND) {
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
                if (!ReleaseProviderMaintenanceBudget(
                        ledger, operation->operation_id, GetTime(), error)) {
                    return false;
                }
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

    LOCK(wallet.cs_wallet);
    PaymasterCheckpointOperation checkpoint{wallet};
    if (!checkpoint.Begin(error)) return false;
    std::string txid_string;
    std::string transfer_error;
    if (!dd_wallet->TransferDigiDollarMany(
            recipients, txid_string, transfer_error,
            /*dd_change_out=*/nullptr,
            /*preset_dd_inputs=*/nullptr,
            "Paymaster automatic carrier replenishment",
            /*allow_paymaster_pool_inputs=*/false,
            /*exact_plan=*/&plan)) {
        error = "PAYMASTER_CARRIER_REPLENISHMENT_FAILED: " + transfer_error;
        return false;
    }
    const uint256 txid = uint256S(txid_string);
    CTransactionRef transaction;
    {
        LOCK(wallet.cs_wallet);
        const auto found = wallet.mapWallet.find(txid);
        if (found != wallet.mapWallet.end()) transaction = found->second.tx;
    }
    if (!transaction) {
        error = "PAYMASTER_MAINTENANCE_TRANSACTION_NOT_IN_WALLET";
        return false;
    }
    CAmount actual_fee{0};
    {
        LOCK(wallet.cs_wallet);
        const CAmount debit = wallet.GetDebit(*transaction, ISMINE_ALL);
        const CAmount value_out = transaction->GetValueOut();
        if (debit < value_out) {
            error = "PAYMASTER_MAINTENANCE_FEE_INVALID";
            return false;
        }
        actual_fee = debit - value_out;
    }
    // The DD planner returns a conservative signed-size fee ceiling.  The
    // finalized transaction may pay less when its exact witness/change shape
    // is smaller, but it must never exceed the previewed ceiling or the
    // operator's absolute maintenance limit.
    if (actual_fee <= 0 || actual_fee > plan.estimated_fee ||
        actual_fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_CHANGED";
        return false;
    }

    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        const DatabaseReadStatus ledger_status{
            batch.ReadPaymasterMaintenanceLedgerWithStatus(ledger)};
        if (ledger_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                ledger_status, "ProviderMaintenanceLedger", ledger,
                "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
            return false;
        }
        if (!ReadOptionalProviderPool(batch, pool, error) ||
            !SpendProviderMaintenanceBudget(
                ledger, operation->operation_id, txid,
                DGBSatoshis{actual_fee}, GetTime(), error)) {
            if (error.empty()) {
                error = "PAYMASTER_MAINTENANCE_LEDGER_UPDATE_FAILED";
            }
            return false;
        }
        const size_t missing_pool_entries = std::count_if(
            operation->outputs.begin(), operation->outputs.end(),
            [&](const ProviderMaintenanceOutput& output) {
                const auto output_index = FindMaintenanceOutputIndex(
                    *transaction, output);
                return output_index &&
                       std::none_of(pool.begin(), pool.end(),
                                    [&](const ProviderPoolEntry& existing) {
                                        return existing.outpoint ==
                                               COutPoint{txid, *output_index};
                                    });
            });
        if (!MakeProviderPoolRoomForMaintenance(
                pool, missing_pool_entries, error)) {
            return false;
        }
        for (const ProviderMaintenanceOutput& output : operation->outputs) {
            const auto output_index = FindMaintenanceOutputIndex(
                *transaction, output);
            if (!output_index) {
                error = "PAYMASTER_MAINTENANCE_OUTPUT_MISSING";
                return false;
            }
            ProviderPoolEntry entry;
            entry.outpoint = COutPoint{txid, *output_index};
            entry.purpose = output.purpose;
            entry.asset = PoolAsset::DD_CARRIER;
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.script_pub_key = output.script_pub_key;
            entry.carrier_value = output.carrier_value;
            entry.reservation_id = operation->operation_id;
            entry.origin_commit_key = operation->operation_id;
            entry.updated_at = GetTime();
            if (std::none_of(pool.begin(), pool.end(),
                             [&](const ProviderPoolEntry& existing) {
                                 return existing.outpoint == entry.outpoint;
                             })) {
                pool.push_back(std::move(entry));
            }
        }
        if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterMaintenanceLedger(ledger) ||
            !batch.WritePaymasterProviderPool(pool)) {
            batch.TxnAbort();
            error = "PAYMASTER_DATABASE_WRITE";
            return false;
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
    }
    return checkpoint.Complete(error);
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
        return status.last_error == "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED";
    default:
        return false;
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

ProviderAutomationSnapshot CaptureProviderAutomationStatus(const CWallet& wallet, const WalletContext& context,
                                        const ProviderReadiness& readiness, int64_t now, bool synchronized)
{
    using namespace DigiDollar::Paymaster;
    LOCK(wallet.cs_wallet);
    const auto& policy = readiness.liquidity_policy;
    const bool configured = readiness.have_liquidity_policy;
    const bool enabled = configured && policy.automatic_replenishment;
    const bool approved = configured && policy.paid_maintenance_approved;
    const bool running = readiness.have_identity && context.paymaster &&
        context.paymaster->IsProviderRunning(wallet.GetName(), readiness.identity.provider_id);
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
        const auto service = ProviderServiceStatusForObservation(
            context.paymaster->GetProviderServiceStatus(wallet.GetName()), readiness.ready, synchronized);
        reason = service.last_error;
        const bool faulted = service.state == ProviderServiceState::FAULT;
        if (faulted) {
            state = ProviderAutomationState::BLOCKED;
            if (reason.empty()) reason = "PAYMASTER_AUTOMATIC_SERVICE_ERROR";
        } else if (service.state == ProviderServiceState::DRAIN_ONLY) {
            state = ProviderAutomationState::PAUSED;
            if (reason.empty()) reason = "PAYMASTER_PROVIDER_DRAIN_ONLY";
        } else if (reason == "PAYMASTER_PROVIDER_SYNCING") state = ProviderAutomationState::PAUSED;
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
