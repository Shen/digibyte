// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Wallet-scoped Paymaster RPC orchestration and automatic provider service.
 *
 * RPC parameters are untrusted input. This layer coordinates pure protocol
 * validation with PaymasterStore transactions and wallet signing, but does not
 * bypass either authorization manifest or persisted safety accounting. Every
 * retry reuses the exact durable request binding or fails closed.
 */

#include <wallet/rpc/paymaster.h>

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
namespace {

constexpr const char* INTERNAL_PAYMASTER_SERVICE_METHOD{
    "__paymaster_automatic_service"};
constexpr const char* INTERNAL_PAYMASTER_AUTOSTART_METHOD{
    "__paymaster_automatic_start"};
constexpr const char* INTERNAL_PAYMASTER_MAINTENANCE_METHOD{
    "__paymaster_automatic_maintenance"};

std::string PersistedVersionError(std::string_view record_type,
                                  uint16_t found,
                                  uint16_t expected,
                                  std::string_view invalid_error)
{
    if (found == expected) return std::string{invalid_error};
    return strprintf(
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=%s found=%u expected=%u",
        std::string{record_type}, found, expected);
}

template <typename T>
std::string PersistedReadError(DatabaseReadStatus status,
                               std::string_view record_type,
                               const T& record,
                               std::string_view missing_error,
                               std::string_view invalid_error)
{
    if (status == DatabaseReadStatus::NOT_FOUND) {
        return std::string{missing_error};
    }
    if (status == DatabaseReadStatus::UNSUPPORTED_VERSION) {
        return PersistedVersionError(record_type, record.version,
                                     T::CURRENT_VERSION, invalid_error);
    }
    return std::string{invalid_error};
}

std::string ProviderPoolReadError(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries)
{
    using DigiDollar::Paymaster::ProviderPoolEntry;
    const auto outdated = std::find_if(
        entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
            return entry.version != ProviderPoolEntry::CURRENT_VERSION;
        });
    if (outdated != entries.end()) {
        return PersistedVersionError(
            "ProviderPoolEntry", outdated->version,
            ProviderPoolEntry::CURRENT_VERSION,
            "PAYMASTER_INVALID_PROVIDER_POOL");
    }
    return "PAYMASTER_INVALID_PROVIDER_POOL";
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

// -------------------------------------------------------------------------
// Automatic provider runtime and readiness
// -------------------------------------------------------------------------
// Internal method names are never exposed as user authorization shortcuts.
// They let the scheduler reuse the same RPC orchestration while the store,
// manifests, wallet locks, and budget checks remain authoritative.

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

std::string WalletEndpointURI(const std::string& wallet_name)
{
    static constexpr char HEX[]{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(wallet_name.size());
    for (const unsigned char ch : wallet_name) {
        const bool unreserved =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(HEX[ch >> 4]);
            encoded.push_back(HEX[ch & 0x0f]);
        }
    }
    return "/wallet/" + encoded;
}

class ProviderWorkGuard final
{
public:
    ProviderWorkGuard(DigiDollar::Paymaster::Manager& manager,
                      const std::string& wallet_name,
                      const DigiDollar::Paymaster::PaymasterId& provider_id,
                      bool require_running = true)
        : m_manager{manager}, m_wallet_name{wallet_name},
          m_acquired{manager.TryBeginProviderWork(
              wallet_name, provider_id, require_running)}
    {
    }

    ProviderWorkGuard(const ProviderWorkGuard&) = delete;
    ProviderWorkGuard& operator=(const ProviderWorkGuard&) = delete;
    ~ProviderWorkGuard()
    {
        if (m_acquired) m_manager.EndProviderWork(m_wallet_name);
    }
    bool Acquired() const noexcept { return m_acquired; }

private:
    DigiDollar::Paymaster::Manager& m_manager;
    const std::string m_wallet_name;
    const bool m_acquired;
};

// Serialize automatic cycles, manual processing RPCs, and maintenance for one
// provider wallet. Without this guard two callers could lease the same logical
// work or independently decide that the same finite budget is available.

struct ProviderReadiness {
    bool have_settings{false};
    bool have_identity{false};
    bool have_policy{false};
    bool have_safety_policy{false};
    bool have_budget_ledger{false};
    bool have_liquidity_policy{false};
    bool have_maintenance_ledger{false};
    bool have_pool{false};
    bool wallet_eligible{false};
    bool pool_ready{false};
    bool ready{false};
    uint8_t available_funding_models{0};
    DigiDollar::Paymaster::ProviderSettings settings;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    DigiDollar::Paymaster::ProviderPolicy policy;
    DigiDollar::Paymaster::ProviderSafetyPolicy safety_policy;
    DigiDollar::Paymaster::ProviderBudgetLedger budget_ledger;
    DigiDollar::Paymaster::ProviderLiquidityPolicy liquidity_policy;
    DigiDollar::Paymaster::ProviderMaintenanceLedger maintenance_ledger;
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry> pool_entries;
    DigiDollar::Paymaster::ProviderPoolReadiness pool;
    CService endpoint;
    std::vector<std::string> errors;
};

// -------------------------------------------------------------------------
// Liquidity reconciliation and bounded maintenance
// -------------------------------------------------------------------------
// Reconciliation is free and idempotent: it recognizes confirmed wallet-owned
// successors. Replenishment is a distinct spending operation and is permitted
// only by a persisted liquidity policy with finite transaction/hour/day caps.
bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error);

/** The automatic provider service runs on the validation scheduler. It must
 * never call SyncWithValidationInterfaceQueue (directly or through a wallet
 * or index BlockUntilSyncedToCurrentChain call), because that queue is driven
 * by the same scheduler thread. Check the already-published wallet and index
 * tips instead and retry on the next bounded service cycle. */
bool AutomaticProviderStateIsSynchronized(CWallet& wallet)
{
    const std::optional<int> chain_height = wallet.chain().getHeight();
    if (!chain_height) return false;
    const uint256 chain_tip = wallet.chain().getBlockHash(*chain_height);

    int wallet_height{-1};
    uint256 wallet_tip;
    {
        LOCK(wallet.cs_wallet);
        wallet_height = wallet.GetLastBlockHeight();
        wallet_tip = wallet.GetLastBlockHash();
    }
    if (wallet_height != *chain_height || wallet_tip != chain_tip) return false;

    if (!g_txindex) return false;
    const IndexSummary txindex = g_txindex->GetSummary();
    if (!txindex.synced || txindex.best_block_height != *chain_height ||
        txindex.best_block_hash != chain_tip) {
        return false;
    }

    const std::optional<int> current_height = wallet.chain().getHeight();
    return current_height && *current_height == *chain_height &&
           wallet.chain().getBlockHash(*current_height) == chain_tip;
}

bool ProviderTxIndexIsReady(CWallet& wallet, bool wait_for_sync)
{
    if (!g_txindex) return false;
    if (wait_for_sync) return g_txindex->BlockUntilSyncedToCurrentChain();

    const std::optional<int> chain_height = wallet.chain().getHeight();
    if (!chain_height) return false;
    const uint256 chain_tip = wallet.chain().getBlockHash(*chain_height);
    const IndexSummary txindex = g_txindex->GetSummary();
    if (!txindex.synced || txindex.best_block_height != *chain_height ||
        txindex.best_block_hash != chain_tip) {
        return false;
    }

    const std::optional<int> current_height = wallet.chain().getHeight();
    return current_height && *current_height == *chain_height &&
           wallet.chain().getBlockHash(*current_height) == chain_tip;
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
                                    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
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
            if (record.state != ProviderMaintenanceState::PLANNED &&
                record.state != ProviderMaintenanceState::BROADCAST) {
                continue;
            }
            active_maintenance_sources.insert(
                record.source_inputs.begin(), record.source_inputs.end());
            if (record.source_inputs.empty() &&
                record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS) {
                // V1 withdrawal records did not serialize source_inputs. The
                // pool reservation remains an unambiguous binding; exclude
                // outputs of the maintenance transaction itself.
                for (const ProviderPoolEntry& entry : entries) {
                    if (entry.reservation_id == record.operation_id &&
                        (record.transaction_id.IsNull() ||
                         entry.outpoint.hash != record.transaction_id)) {
                        active_maintenance_sources.insert(entry.outpoint);
                    }
                }
            }
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
    if (changed && !batch.WritePaymasterProviderPool(entries)) {
        error = "PAYMASTER_POOL_CONFIRMATION_UPDATE_FAILED";
        return false;
    }
    return true;
}

ProviderReadiness GetProviderReadiness(CWallet& wallet, WalletContext& context,
                                       bool wait_for_sync = true)
{
    ProviderReadiness result;
    {
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
    if (!RefreshPoolConfirmationHeights(wallet, result.pool_entries, refresh_error)) {
        result.errors.push_back(refresh_error);
    }
    if (result.have_policy && result.have_pool) {
        result.pool = DigiDollar::Paymaster::EvaluateProviderPoolReadiness(
            result.pool_entries, result.policy, tip_height, 1);
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
    if (!result.wallet_eligible) result.errors.push_back(eligibility_error);
    if (!result.have_identity) result.errors.push_back("PAYMASTER_IDENTITY_NOT_FOUND");
    if (!result.have_policy) result.errors.push_back("PAYMASTER_POLICY_NOT_FOUND");
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

// JSON conversion helpers below are presentation only. They must not be used to
// make authorization decisions; callers operate on the typed records above.
UniValue ReadinessErrorsToJSON(const std::vector<std::string>& errors)
{
    UniValue result{UniValue::VARR};
    for (const std::string& error : errors)
        result.push_back(error);
    return result;
}

std::string PoolPurposeName(DigiDollar::Paymaster::PoolPurpose purpose)
{
    return purpose == DigiDollar::Paymaster::PoolPurpose::ADMISSION ? "admission" : "operational";
}

std::string PoolAssetName(DigiDollar::Paymaster::PoolAsset asset)
{
    return asset == DigiDollar::Paymaster::PoolAsset::DGB ? "dgb" : "dd_carrier";
}

std::string PoolEntryStateName(DigiDollar::Paymaster::PoolEntryState state)
{
    using DigiDollar::Paymaster::PoolEntryState;
    switch (state) {
    case PoolEntryState::AVAILABLE: return "available";
    case PoolEntryState::RESERVED: return "reserved";
    case PoolEntryState::PENDING_SUCCESSOR: return "pending_successor";
    case PoolEntryState::SPENT: return "spent";
    case PoolEntryState::COMMITTED: return "committed";
    case PoolEntryState::RELEASED: return "released";
    case PoolEntryState::INVALIDATED: return "invalidated";
    }
    return "unknown";
}

UniValue PoolEntryToJSON(const DigiDollar::Paymaster::ProviderPoolEntry& entry)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("txid", entry.outpoint.hash.GetHex());
    result.pushKV("vout", entry.outpoint.n);
    result.pushKV("purpose", PoolPurposeName(entry.purpose));
    result.pushKV("asset", PoolAssetName(entry.asset));
    result.pushKV("state", PoolEntryStateName(entry.state));
    result.pushKV("dgb_satoshis", entry.dgb_value.value);
    result.pushKV("dd_cents", entry.carrier_value.value);
    result.pushKV("confirmation_height", entry.confirmation_height);
    if (!entry.reservation_id.IsNull()) result.pushKV("reservation_id", entry.reservation_id.GetHex());
    if (!entry.origin_commit_key.IsNull()) {
        result.pushKV("origin_commit_key", entry.origin_commit_key.GetHex());
    }
    result.pushKV("updated_at", entry.updated_at);
    return result;
}

// Announcements describe currently admissible service but grant no spending
// authority. Every later capacity proof, quote, and final transaction is still
// independently authenticated and validated.
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

struct LiquiditySlotCounts {
    size_t target_counted{0};
    size_t ready{0};
    size_t pending{0};
    size_t missing{0};
};

LiquiditySlotCounts CountLiquiditySlots(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    DigiDollar::Paymaster::PoolPurpose purpose,
    DigiDollar::Paymaster::PoolAsset asset,
    size_t target,
    int64_t minimum_dgb_value = 0)
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

UniValue LiquiditySlotCountsToJSON(const LiquiditySlotCounts& counts,
                                   size_t target)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("target", target);
    result.pushKV("ready", counts.ready);
    result.pushKV("pending", counts.pending);
    result.pushKV("counted_toward_target", counts.target_counted);
    result.pushKV("missing", counts.missing);
    return result;
}

UniValue LiquidityPolicyToJSON(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("automatic_replenishment", policy.automatic_replenishment);
    result.pushKV("paid_maintenance_approved", policy.paid_maintenance_approved);
    result.pushKV("target_admission_dgb", policy.target_admission_dgb);
    result.pushKV("target_operational_dgb", policy.target_operational_dgb);
    result.pushKV("target_admission_carriers", policy.target_admission_carriers);
    result.pushKV("target_operational_carriers", policy.target_operational_carriers);
    result.pushKV("maximum_maintenance_fee_per_transaction_satoshis",
                  policy.maximum_maintenance_fee_per_transaction.value);
    result.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                  policy.maximum_maintenance_fee_per_hour.value);
    result.pushKV("maximum_maintenance_fee_per_day_satoshis",
                  policy.maximum_maintenance_fee_per_day.value);
    result.pushKV("updated_at", policy.updated_at);
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

    const auto include_limits = [&](const FundingSafetyLimits& limits) {
        const auto lower_positive = [](int64_t current, int64_t candidate) {
            if (candidate <= 0) return current;
            return current <= 0 ? candidate : std::min(current, candidate);
        };
        result.maximum_maintenance_fee_per_transaction.value = lower_positive(
            result.maximum_maintenance_fee_per_transaction.value,
            limits.maximum_network_fee_per_transaction.value);
        result.maximum_maintenance_fee_per_hour.value = lower_positive(
            result.maximum_maintenance_fee_per_hour.value,
            limits.maximum_network_fee_per_hour.value);
        result.maximum_maintenance_fee_per_day.value = lower_positive(
            result.maximum_maintenance_fee_per_day.value,
            limits.maximum_network_fee_per_day.value);
    };
    if (readiness.have_policy && readiness.have_safety_policy) {
        if (PolicyAllowsFundingModel(readiness.policy, FundingModel::USER_PAID)) {
            include_limits(GetFundingSafetyLimits(
                readiness.safety_policy, FundingModel::USER_PAID,
                SponsorshipScope::PUBLIC));
        }
        if (PolicyAllowsFundingModel(readiness.policy, FundingModel::SPONSORED)) {
            include_limits(GetFundingSafetyLimits(
                readiness.safety_policy, FundingModel::SPONSORED,
                readiness.policy.sponsorship_scope));
        }
    }
    result.updated_at = now;
    return result;
}

UniValue ProviderLiquidityStatusToJSON(const ProviderReadiness& readiness,
                                       int64_t now)
{
    using namespace DigiDollar::Paymaster;
    const ProviderLiquidityPolicy policy = readiness.have_liquidity_policy
        ? readiness.liquidity_policy
        : SuggestedLiquidityPolicy(readiness, now);
    const bool targets_satisfy_provider_policy =
        !readiness.have_policy ||
        ProviderLiquidityTargetsSatisfyPolicy(policy, readiness.policy);
    const auto admission_dgb = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB,
        policy.target_admission_dgb);
    const int64_t operational_dgb_minimum = readiness.have_policy
        ? readiness.policy.maximum_network_fee.value
        : std::numeric_limits<int64_t>::max();
    const auto operational_dgb = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
        policy.target_operational_dgb, operational_dgb_minimum);
    const auto admission_carriers = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::ADMISSION,
        PoolAsset::DD_CARRIER, policy.target_admission_carriers);
    const auto operational_carriers = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::OPERATIONAL,
        PoolAsset::DD_CARRIER, policy.target_operational_carriers);
    const size_t missing = admission_dgb.missing + operational_dgb.missing +
                           admission_carriers.missing +
                           operational_carriers.missing;
    const size_t pending = admission_dgb.pending + operational_dgb.pending +
                           admission_carriers.pending +
                           operational_carriers.pending;
    std::string state{"ready"};
    if (!targets_satisfy_provider_policy) {
        state = "waiting_for_target_configuration";
    } else if (!readiness.have_liquidity_policy ||
        (missing > 0 && !policy.paid_maintenance_approved)) {
        state = "waiting_for_maintenance_approval";
    } else if (pending > 0) {
        state = "waiting_for_liquidity_confirmation";
    } else if (missing > 0) {
        state = "replenishing_liquidity";
    }

    const auto checked_add = [](int64_t& total, int64_t value) {
        if (value < 0 ||
            total > std::numeric_limits<int64_t>::max() - value) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_LIQUIDITY_STATUS_OVERFLOW");
        }
        total += value;
    };
    int64_t carrier_base{0};
    int64_t carrier_excess{0};
    for (const ProviderPoolEntry& entry : readiness.pool_entries) {
        if (entry.asset != PoolAsset::DD_CARRIER ||
            entry.state != PoolEntryState::AVAILABLE ||
            entry.confirmation_height <= 0) {
            continue;
        }
        checked_add(
            carrier_base,
            std::min<int64_t>(100, entry.carrier_value.value));
        checked_add(
            carrier_excess,
            std::max<int64_t>(0, entry.carrier_value.value - 100));
    }

    int64_t reserved_fee{0};
    int64_t spent_hour{0};
    int64_t spent_day{0};
    if (readiness.have_maintenance_ledger) {
        for (const ProviderMaintenanceRecord& record :
             readiness.maintenance_ledger.records) {
            if (record.state == ProviderMaintenanceState::PLANNED) {
                checked_add(reserved_fee, record.maximum_fee.value);
            } else if (record.state ==
                       ProviderMaintenanceState::BROADCAST) {
                // A live transaction remains economic exposure until it is
                // confirmed or fails, irrespective of its wall-clock age.
                checked_add(reserved_fee, record.actual_fee.value);
            } else if (record.state ==
                       ProviderMaintenanceState::CONFIRMED) {
                if (record.updated_at >= now - 60 * 60) {
                    checked_add(spent_hour, record.actual_fee.value);
                }
                if (record.updated_at >= now - 24 * 60 * 60) {
                    checked_add(spent_day, record.actual_fee.value);
                }
            }
        }
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("policy_configured", readiness.have_liquidity_policy);
    result.pushKV("targets_satisfy_provider_policy",
                  targets_satisfy_provider_policy);
    result.pushKV("maintenance_state", state);
    result.pushKV("policy", LiquidityPolicyToJSON(policy));
    result.pushKV("admission_dgb", LiquiditySlotCountsToJSON(
        admission_dgb, policy.target_admission_dgb));
    result.pushKV("operational_dgb", LiquiditySlotCountsToJSON(
        operational_dgb, policy.target_operational_dgb));
    result.pushKV("admission_carriers", LiquiditySlotCountsToJSON(
        admission_carriers, policy.target_admission_carriers));
    result.pushKV("operational_carriers", LiquiditySlotCountsToJSON(
        operational_carriers, policy.target_operational_carriers));
    result.pushKV("maintenance_fee_reserved_satoshis", reserved_fee);
    result.pushKV("maintenance_fee_spent_last_hour_satoshis", spent_hour);
    result.pushKV("maintenance_fee_spent_last_day_satoshis", spent_day);
    result.pushKV("carrier_base_cents", carrier_base);
    result.pushKV("carrier_withdrawable_excess_cents", carrier_excess);
    result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
    return result;
}

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
            record.state != ProviderMaintenanceState::BROADCAST &&
            record.state != ProviderMaintenanceState::CONFIRMED) {
            continue;
        }
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
            transaction = std::find_if(wallet.mapWallet.begin(), wallet.mapWallet.end(),
                                       [&](const auto& item) {
                                           return matches(item.second);
                                       });
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
        if (record.transaction_id.IsNull()) {
            record.transaction_id = txid;
            // Conservatively charge the entire reservation after recovering a
            // crash window; this can only reduce later maintenance capacity.
            record.actual_fee = record.maximum_fee;
            record.state = ProviderMaintenanceState::BROADCAST;
            record.updated_at = now;
            ledger_changed = true;
            ++recovered;
        }

        if (transaction->second.isAbandoned() ||
            transaction->second.isConflicted()) {
            if (record.state != ProviderMaintenanceState::FAILED) {
                record.state = ProviderMaintenanceState::FAILED;
                record.updated_at = now;
                ledger_changed = true;
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
                // A source may already be SPENT after a previously confirmed
                // maintenance transaction, or INVALIDATED by an older generic
                // refresh. Once the exact maintenance transaction is known to
                // have failed, chainstate is authoritative: restore an
                // unspent source, otherwise keep it unavailable. Never steal a
                // reservation that is already bound to another operation.
                if (!source->reservation_id.IsNull() &&
                    source->reservation_id != record.operation_id) {
                    continue;
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
                const PoolEntryState restored_state = wallet.IsSpent(source_input)
                    ? PoolEntryState::INVALIDATED
                    : PoolEntryState::AVAILABLE;
                if (source->state != restored_state ||
                    !source->reservation_id.IsNull()) {
                    source->state = restored_state;
                    source->reservation_id.SetNull();
                    source->updated_at = now;
                    pool_changed = true;
                }
            }
            continue;
        }

        const auto* confirmed = transaction->second.state<TxStateConfirmed>();
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
            record.updated_at = now;
            ledger_changed = true;
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
                if (confirmed &&
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

bool ReconcileProviderFinances(CWallet& wallet,
                               size_t& changed_events,
                               std::string& error)
{
    using namespace DigiDollar::Paymaster;
    changed_events = 0;
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    const DatabaseReadStatus identity_status =
        batch.ReadPaymasterIdentityWithStatus(identity);
    if (identity_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (identity_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            identity_status, "ProviderIdentityRecord", identity,
            "PAYMASTER_IDENTITY_NOT_FOUND", "PAYMASTER_INVALID_IDENTITY");
        return false;
    }

    const uint256 genesis_hash = Params().GenesisBlock().GetHash();
    ProviderFinanceLedger finance;
    bool ledger_changed{false};
    const DatabaseReadStatus finance_status =
        batch.ReadPaymasterFinanceLedgerWithStatus(finance);
    if (finance_status != DatabaseReadStatus::FOUND) {
        if (finance_status != DatabaseReadStatus::NOT_FOUND) {
            error = PersistedReadError(
                finance_status, "ProviderFinanceLedger", finance,
                "PAYMASTER_FINANCE_LEDGER_NOT_FOUND",
                "PAYMASTER_INVALID_FINANCE_LEDGER");
            return false;
        }
        // Pre-feature wallets may have durable records that no longer carry
        // enough context for exact reconstruction. Never estimate them.
        finance.genesis_hash = genesis_hash;
        finance.provider_id = identity.provider_id;
        finance.history_complete_from = GetTime();
        finance.earlier_history_partial = true;
        finance.updated_at = finance.history_complete_from;
        if (!RebuildProviderFinanceDailyTotals(finance, error)) return false;
        ledger_changed = true;
    }
    if (finance.genesis_hash != genesis_hash ||
        finance.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }

    const int64_t now = GetTime();
    const auto transaction_state = [&](const uint256& txid,
                                       int64_t created_at,
                                       ProviderFinanceEventState& state,
                                       int64_t& confirmed_at) {
        state = ProviderFinanceEventState::PENDING;
        confirmed_at = 0;
        const auto transaction = wallet.mapWallet.find(txid);
        if (transaction == wallet.mapWallet.end()) return;
        if (transaction->second.isAbandoned() ||
            transaction->second.isConflicted()) {
            state = ProviderFinanceEventState::INVALIDATED;
            return;
        }
        if (wallet.GetTxDepthInMainChain(transaction->second) > 0) {
            state = ProviderFinanceEventState::CONFIRMED;
            int64_t block_time{0};
            if (const auto* confirmed =
                    transaction->second.state<TxStateConfirmed>()) {
                wallet.chain().findBlock(
                    confirmed->confirmed_block_hash,
                    interfaces::FoundBlock().time(block_time));
            }
            // UTC totals follow the active-chain confirmation day. The wallet
            // observation time remains a conservative fallback for old or
            // temporarily unavailable block metadata.
            confirmed_at = std::max(
                created_at, block_time > 0
                    ? block_time : transaction->second.GetTxTime());
        }
    };
    const auto apply_event = [&](ProviderFinanceEvent event) {
        auto existing = std::find_if(
            finance.events.begin(), finance.events.end(),
            [&](const ProviderFinanceEvent& candidate) {
                return candidate.event_id == event.event_id;
            });
        if (existing != finance.events.end()) {
            const bool same_economic_event =
                existing->genesis_hash == event.genesis_hash &&
                existing->provider_id == event.provider_id &&
                existing->kind == event.kind &&
                existing->funding_model == event.funding_model &&
                existing->sponsorship_scope == event.sponsorship_scope &&
                existing->transaction_id == event.transaction_id &&
                existing->dd_income == event.dd_income &&
                existing->dgb_cost == event.dgb_cost &&
                existing->created_at == event.created_at;
            if (!same_economic_event) {
                error = "PAYMASTER_FINANCE_EVENT_CONFLICT";
                return false;
            }
        }
        if (existing != finance.events.end() &&
            existing->state == event.state &&
            existing->confirmed_at == event.confirmed_at) {
            event.updated_at = existing->updated_at;
        } else {
            // Regtest mock time and imported historical blocks can place the
            // active-chain confirmation after the current wall-clock value.
            // Keep the event chronology valid without replacing the actual
            // block confirmation time with a local observation time.
            event.updated_at = std::max(
                {event.created_at, event.confirmed_at, now});
            ledger_changed = true;
            ++changed_events;
        }
        if (!ValidateProviderFinanceEvent(event, error) ||
            event.genesis_hash != finance.genesis_hash ||
            event.provider_id != finance.provider_id) {
            if (error.empty()) {
                error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
            }
            return false;
        }
        if (existing == finance.events.end()) {
            // The shared upsert enforces the durable event bound. Existing
            // entries are updated directly below so reconciliation can rebuild
            // daily totals once, rather than once per historical event.
            return UpsertProviderFinanceEvent(finance, event, error);
        }
        *existing = std::move(event);
        return true;
    };

    // Events created directly by pool-management RPCs are not represented by
    // a provider commit or maintenance reservation. Reconcile their chain
    // state here before adding any newly discoverable records below.
    for (ProviderFinanceEvent& event : finance.events) {
        ProviderFinanceEventState state;
        int64_t confirmed_at;
        transaction_state(event.transaction_id, event.created_at,
                          state, confirmed_at);
        if (event.state == state && event.confirmed_at == confirmed_at) {
            continue;
        }
        event.state = state;
        event.confirmed_at = confirmed_at;
        event.updated_at = std::max(
            {event.created_at, event.confirmed_at, now});
        finance.updated_at = std::max(
            finance.updated_at, event.updated_at);
        ledger_changed = true;
        ++changed_events;
    }

    // Initial pool preparation predates maintenance records and therefore has
    // no separate operation object to replay. The persisted pool still binds
    // each original slot to its creating transaction. When that transaction
    // remains in this wallet, its native DGB fee can be reconstructed exactly;
    // never infer a cost when either the transaction or its wallet debit is
    // unavailable.
    std::vector<ProviderPoolEntry> provider_pool;
    const DatabaseReadStatus provider_pool_status =
        batch.ReadPaymasterProviderPoolWithStatus(provider_pool);
    if (provider_pool_status == DatabaseReadStatus::FOUND) {
        std::set<uint256> setup_transactions;
        for (const ProviderPoolEntry& entry : provider_pool) {
            if (entry.origin_commit_key.IsNull() &&
                !entry.outpoint.hash.IsNull()) {
                setup_transactions.insert(entry.outpoint.hash);
            }
        }
        for (const uint256& txid : setup_transactions) {
            HashWriter event_hasher = TaggedHash(
                "DigiByte Paymaster Finance Event v1");
            event_hasher << identity.provider_id
                         << static_cast<uint8_t>(
                                ProviderFinanceEventKind::POOL_SETUP)
                         << txid;
            const uint256 event_id = event_hasher.GetSHA256();
            if (std::any_of(finance.events.begin(), finance.events.end(),
                            [&](const ProviderFinanceEvent& event) {
                                return event.event_id == event_id;
                            })) {
                continue;
            }
            const auto transaction = wallet.mapWallet.find(txid);
            if (transaction == wallet.mapWallet.end() ||
                !transaction->second.tx) {
                if (!finance.earlier_history_partial) {
                    finance.earlier_history_partial = true;
                    ledger_changed = true;
                }
                continue;
            }
            const CAmount debit = wallet.GetDebit(
                *transaction->second.tx, ISMINE_ALL);
            const CAmount value_out = transaction->second.tx->GetValueOut();
            if (debit < value_out || !MoneyRange(debit - value_out)) {
                if (!finance.earlier_history_partial) {
                    finance.earlier_history_partial = true;
                    ledger_changed = true;
                }
                continue;
            }
            ProviderFinanceEvent event;
            event.event_id = event_id;
            event.genesis_hash = genesis_hash;
            event.provider_id = identity.provider_id;
            event.kind = ProviderFinanceEventKind::POOL_SETUP;
            event.transaction_id = txid;
            event.dgb_cost = DGBSatoshis{debit - value_out};
            event.created_at = std::max(
                identity.created_at, transaction->second.GetTxTime());
            transaction_state(event.transaction_id, event.created_at,
                              event.state, event.confirmed_at);
            if (!apply_event(std::move(event))) return false;
        }
    } else if (provider_pool_status != DatabaseReadStatus::NOT_FOUND) {
        error = ProviderPoolReadError(provider_pool);
        return false;
    }

    std::vector<PaymentSession> sessions;
    std::map<uint256, ProviderAttempt> attempts_by_commit;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_FINANCE_SESSION_SCAN_FAILED";
        return false;
    }
    for (const PaymentSession& session : sessions) {
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            const DatabaseReadStatus attempt_status =
                batch.ReadPaymasterAttemptWithStatus(attempt_id, attempt);
            if (attempt_status != DatabaseReadStatus::FOUND) {
                error = PersistedReadError(
                    attempt_status, "ProviderAttempt", attempt,
                    "PAYMASTER_FINANCE_ATTEMPT_MISSING",
                    "PAYMASTER_FINANCE_ATTEMPT_READ_FAILED");
                return false;
            }
            if (attempt.provider_id != identity.provider_id ||
                attempt.commit_key.IsNull()) {
                continue;
            }
            attempts_by_commit.emplace(attempt.commit_key, std::move(attempt));
        }
    }
    std::vector<ProviderCommitRecord> commits;
    if (!batch.ListPaymasterProviderCommits(commits)) {
        error = "PAYMASTER_FINANCE_COMMIT_SCAN_FAILED";
        return false;
    }
    for (const ProviderCommitRecord& commit : commits) {
        if (commit.provider_id != identity.provider_id) continue;
        const auto attempt = attempts_by_commit.find(commit.commit_key);
        if (attempt == attempts_by_commit.end() ||
            attempt->second.provider_manifest.manifest_id.IsNull()) {
            if (!finance.earlier_history_partial) {
                finance.earlier_history_partial = true;
                ledger_changed = true;
            }
            continue;
        }
        const ProviderAuthorizationManifest& manifest =
            attempt->second.provider_manifest;
        ProviderFinanceEvent event;
        event.event_id = commit.commit_key;
        event.genesis_hash = genesis_hash;
        event.provider_id = identity.provider_id;
        event.kind = ProviderFinanceEventKind::TRANSFER;
        event.funding_model = manifest.funding_model;
        event.sponsorship_scope = manifest.sponsorship_scope;
        event.transaction_id = commit.final_txid;
        event.dd_income = manifest.service_fee;
        event.dgb_cost = manifest.network_fee;
        event.created_at = commit.committed_at;
        transaction_state(event.transaction_id, event.created_at,
                          event.state, event.confirmed_at);
        if (!apply_event(std::move(event))) return false;
    }

    ProviderMaintenanceLedger maintenance;
    const DatabaseReadStatus maintenance_status =
        batch.ReadPaymasterMaintenanceLedgerWithStatus(maintenance);
    if (maintenance_status == DatabaseReadStatus::FOUND) {
        for (const ProviderMaintenanceRecord& record : maintenance.records) {
            if (record.transaction_id.IsNull()) continue;
            ProviderFinanceEvent event;
            event.event_id = record.operation_id;
            event.genesis_hash = genesis_hash;
            event.provider_id = identity.provider_id;
            event.kind = record.kind ==
                    ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS
                ? ProviderFinanceEventKind::CARRIER_WITHDRAWAL
                : ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT;
            event.transaction_id = record.transaction_id;
            event.dgb_cost = record.actual_fee;
            event.created_at = record.created_at;
            transaction_state(event.transaction_id, event.created_at,
                              event.state, event.confirmed_at);
            if (record.state == ProviderMaintenanceState::FAILED ||
                record.state == ProviderMaintenanceState::RELEASED) {
                event.state = ProviderFinanceEventState::INVALIDATED;
                event.confirmed_at = 0;
            }
            if (!apply_event(std::move(event))) return false;
        }
    } else if (maintenance_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            maintenance_status, "ProviderMaintenanceLedger", maintenance,
            "PAYMASTER_MAINTENANCE_LEDGER_NOT_FOUND",
            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
        return false;
    }

    if (!ledger_changed) return true;
    finance.updated_at = std::max(finance.updated_at, now);
    for (const ProviderFinanceEvent& event : finance.events) {
        finance.updated_at = std::max(finance.updated_at, event.updated_at);
    }
    if (!RebuildProviderFinanceDailyTotals(finance, error) ||
        !batch.WritePaymasterFinanceLedger(finance)) {
        if (error.empty()) error = "PAYMASTER_FINANCE_DATABASE_WRITE";
        return false;
    }
    return true;
}

/** Persist the exact native DGB cost of a wallet-created provider finance
 * transaction. The event id is derived from provider, category and txid, so a
 * retry is idempotent while two categories can never silently share an id. */
bool RecordProviderFinanceTransaction(
    CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceEventKind kind,
    const CTransactionRef& transaction,
    DigiDollar::Paymaster::DGBSatoshis dgb_cost,
    int64_t now,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!transaction || dgb_cost.value < 0 || now <= 0) {
        error = "PAYMASTER_INVALID_FINANCE_TRANSACTION";
        return false;
    }
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_IDENTITY_NOT_FOUND";
        return false;
    }
    const uint256 genesis_hash = Params().GenesisBlock().GetHash();
    ProviderFinanceLedger ledger;
    if (!batch.ReadPaymasterFinanceLedger(ledger)) {
        if (batch.HasPaymasterFinanceLedger()) {
            error = "PAYMASTER_INVALID_FINANCE_LEDGER";
            return false;
        }
        ledger.genesis_hash = genesis_hash;
        ledger.provider_id = identity.provider_id;
        ledger.history_complete_from = now;
        ledger.earlier_history_partial = identity.created_at < now;
        ledger.updated_at = now;
        if (!RebuildProviderFinanceDailyTotals(ledger, error)) return false;
    }
    if (ledger.genesis_hash != genesis_hash ||
        ledger.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    HashWriter event_hasher = TaggedHash(
        "DigiByte Paymaster Finance Event v1");
    event_hasher << identity.provider_id << static_cast<uint8_t>(kind)
                 << transaction->GetHash();
    ProviderFinanceEvent event;
    event.event_id = event_hasher.GetSHA256();
    event.genesis_hash = genesis_hash;
    event.provider_id = identity.provider_id;
    event.kind = kind;
    event.state = ProviderFinanceEventState::PENDING;
    event.transaction_id = transaction->GetHash();
    event.dgb_cost = dgb_cost;
    event.created_at = now;
    event.updated_at = now;
    if (!UpsertProviderFinanceEvent(ledger, event, error) ||
        !batch.WritePaymasterFinanceLedger(ledger)) {
        if (error.empty()) error = "PAYMASTER_FINANCE_DATABASE_WRITE";
        return false;
    }
    return true;
}

/** Calculate the actual base-coin fee of a transaction already present in
 * this wallet. DigiDollar inputs carry zero CTxOut value, so this remains an
 * exact DGB fee calculation for carrier-management transactions as well. */
std::optional<DigiDollar::Paymaster::DGBSatoshis>
GetProviderFinanceTransactionFee(CWallet& wallet,
                                 const CTransactionRef& transaction)
{
    if (!transaction) return std::nullopt;
    LOCK(wallet.cs_wallet);
    const CAmount debit = wallet.GetDebit(*transaction, ISMINE_ALL);
    const CAmount value_out = transaction->GetValueOut();
    if (debit < value_out || !MoneyRange(debit - value_out)) {
        return std::nullopt;
    }
    return DigiDollar::Paymaster::DGBSatoshis{debit - value_out};
}

// Paid DGB replenishment reserves its worst-case maintenance fee before wallet
// construction. The durable plan record makes a crash after broadcast
// distinguishable from a request to create a second replacement transaction.
bool RunAutomaticDGBReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
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
    }

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
            outputs_match && planned_admission == missing_admission &&
            planned_operational == missing_operational &&
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

    std::vector<CRecipient> recipients;
    for (const ProviderMaintenanceOutput& output : operation->outputs) {
        CTxDestination destination;
        if (output.asset != PoolAsset::DGB ||
            !ExtractDestination(output.script_pub_key, destination)) {
            error = "PAYMASTER_INVALID_MAINTENANCE_OUTPUT";
            return false;
        }
        recipients.push_back({destination, output.dgb_value.value, false});
    }
    CCoinControl coin_control;
    auto created = CreateTransaction(wallet, recipients, /*change_pos=*/-1,
                                     coin_control, /*sign=*/true);
    if (!created) {
        error = util::ErrorString(created).original;
        return false;
    }
    if (created->fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }
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
    return true;
}

// DD carriers are ordinary wallet value with a dedicated provider role. Never
// select reserved/pending carrier value, and never silently turn normal client
// DD into provider liquidity without the configured target and preview binding.
bool RunAutomaticCarrierReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
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
    }

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
            outputs_match && planned_admission == missing_admission &&
            planned_operational == missing_operational &&
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
    if (!dd_wallet->PlanDigiDollarTransfer(
            recipients, plan, plan_error,
            /*preset_dd_inputs=*/nullptr,
            /*allow_paymaster_pool_inputs=*/false)) {
        error = "PAYMASTER_CARRIER_REPLENISHMENT_FAILED: " + plan_error;
        return false;
    }
    if (plan.estimated_fee <= 0 ||
        plan.estimated_fee > operation->maximum_fee.value) {
        error = "PAYMASTER_MAINTENANCE_FEE_EXCEEDED";
        return false;
    }

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
    return true;
}

UniValue ReliabilityToJSON(const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
                           int64_t now)
{
    const auto summary = DigiDollar::Paymaster::SummarizeReliability(record, now);
    UniValue result{UniValue::VOBJ};
    result.pushKV("provider_id", record.provider_id.GetHex());
    result.pushKV("successful_attempts", summary.successful_attempts);
    result.pushKV("provider_failures", summary.provider_failures);
    result.pushKV("neutral_failures", summary.neutral_failures);
    result.pushKV("availability_timeouts", summary.availability_timeouts);
    result.pushKV("sufficient_data", summary.sufficient_data);
    if (summary.sufficient_data) result.pushKV("success_rate_basis_points", summary.success_rate_basis_points);
    result.pushKV("latency_ewma_ms", record.latency_ewma_ms);
    result.pushKV("consecutive_provider_failures", record.consecutive_provider_failures);
    result.pushKV("last_observation_at", record.last_observation_at);
    result.pushKV("cooldown_until", record.cooldown_until);
    result.pushKV("cooldown_active", record.cooldown_until > now);
    return result;
}

std::string FeeModeName(DigiDollar::Paymaster::FeeMode mode)
{
    using DigiDollar::Paymaster::FeeMode;
    switch (mode) {
    case FeeMode::DGB: return "dgb";
    case FeeMode::PAYMASTER: return "paymaster";
    case FeeMode::AUTO: return "auto";
    }
    return "unknown";
}

std::string ResultStatusName(DigiDollar::Paymaster::PaymasterResultStatus status)
{
    using DigiDollar::Paymaster::PaymasterResultStatus;
    switch (status) {
    case PaymasterResultStatus::NO_FINAL_COMMIT: return "no_final_commit";
    case PaymasterResultStatus::USER_PSBT_ACCEPTED: return "user_psbt_accepted";
    case PaymasterResultStatus::FINAL_COMMITTED: return "final_committed";
    case PaymasterResultStatus::BROADCAST_ATTEMPTED: return "broadcast_attempted";
    case PaymasterResultStatus::SLOT_UNAVAILABLE: return "slot_unavailable";
    case PaymasterResultStatus::REJECTED: return "rejected";
    }
    return "unknown";
}

// -------------------------------------------------------------------------
// Client session serialization, replay, and equivocation handling
// -------------------------------------------------------------------------
UniValue SessionToJSON(
    const DigiDollar::Paymaster::PaymentSession& session,
    const PaymasterStore* store = nullptr)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", session.request_id);
    result.pushKV("session_id", session.session_id.GetHex());
    result.pushKV("canonical_request_hash", session.canonical_request_hash.GetHex());
    if (session.requested_amount.value > 0) {
        result.pushKV("requested_amount_cents", session.requested_amount.value);
        result.pushKV("subtract_paymaster_fee_from_amount",
                      session.subtract_paymaster_fee_from_amount);
        result.pushKV("send_all_spendable_dd",
                      session.send_all_spendable_dd);
    }
    result.pushKV("requested_fee_mode", FeeModeName(session.fee_mode_requested));
    result.pushKV("fee_mode_used", FeeModeName(session.fee_mode_used));
    result.pushKV("session_state", std::string{SessionStateName(session.state)});
    if (session.pending_phase != PendingPhase::NONE) {
        result.pushKV("pending_phase", std::string{PendingPhaseName(session.pending_phase)});
    }
    result.pushKV("final", IsTerminal(session.state));
    const bool has_final_transaction = !session.final_txid.IsNull();
    std::string broadcast_state{"not_attempted"};
    if (session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE)
        broadcast_state = "confirmed";
    else if (session.pending_phase == PendingPhase::CANCEL_MEMPOOL)
        broadcast_state = "accepted_mempool";
    else if (session.state == SessionState::MEMPOOL)
        broadcast_state = "accepted_mempool";
    else if (session.state == SessionState::STEMPOOL)
        broadcast_state = "accepted_stempool";
    else if (session.state == SessionState::PENDING_PROVIDER || has_final_transaction)
        broadcast_state = "unknown";
    result.pushKV("broadcast_state", broadcast_state);
    const std::string confirmation_state = session.state == SessionState::CONFIRMED ? "payment_confirmed" : (session.state == SessionState::CANCELED_SAFE ? "recovery_confirmed" : (session.state == SessionState::CONFLICTED ? "conflicted" : "unconfirmed"));
    result.pushKV("confirmation_state", confirmation_state);
    result.pushKV("created_at", session.created_at);
    result.pushKV("updated_at", session.updated_at);
    if (!session.final_txid.IsNull()) result.pushKV("txid", session.final_txid.GetHex());
    if (!session.recovery_txid.IsNull()) {
        result.pushKV("recovery_txid", session.recovery_txid.GetHex());
    }

    UniValue inputs{UniValue::VARR};
    for (const auto& input : session.user_inputs) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("txid", input.hash.GetHex());
        entry.pushKV("vout", input.n);
        inputs.push_back(std::move(entry));
    }
    result.pushKV("reserved_user_inputs", std::move(inputs));
    result.pushKV("provider_attempts", static_cast<uint64_t>(session.attempt_ids.size()));

    // The session owns the provider-independent gross order while its latest
    // client authorization manifest owns the exact provider-dependent split.
    // Surface both together when the durable attempt is still available so
    // status queries and exact retries never report the gross amount as the
    // recipient amount in subtract-fee mode.
    if (store && !session.provider_side) {
        for (auto it = session.attempt_ids.rbegin();
             it != session.attempt_ids.rend(); ++it) {
            ProviderAttempt attempt;
            if (!store->GetAttempt(*it, attempt)) continue;
            const ClientAuthorizationManifest& manifest =
                attempt.client_manifest;
            if (manifest.manifest_id.IsNull() ||
                manifest.version !=
                    ClientAuthorizationManifest::CURRENT_VERSION ||
                manifest.recipient_amount.value < 0 ||
                manifest.service_fee.value < 0 ||
                manifest.recipient_amount.value >
                    std::numeric_limits<int64_t>::max() -
                        manifest.service_fee.value) {
                continue;
            }
            result.pushKV("provider_id", manifest.provider_id.GetHex());
            result.pushKV("offer_id", manifest.offer_id.GetHex());
            result.pushKV("policy_hash", manifest.policy_hash.GetHex());
            result.pushKV(
                "funding_model",
                manifest.funding_model == FundingModel::SPONSORED
                    ? "sponsored"
                    : "user_paid");
            result.pushKV("payment_cents",
                          manifest.recipient_amount.value);
            result.pushKV("service_fee_cents",
                          manifest.service_fee.value);
            result.pushKV(
                "user_total_cents",
                manifest.recipient_amount.value +
                    manifest.service_fee.value);
            break;
        }
    }
    return result;
}

bool FindSession(const UniValue& lookup, PaymasterStore& store,
                 DigiDollar::Paymaster::PaymentSession& session)
{
    RPCTypeCheckObj(lookup,
                    {{"request_id", UniValueType(UniValue::VSTR)},
                     {"session_id", UniValueType(UniValue::VSTR)}},
                    /*fAllowNull=*/true, /*fStrict=*/true);
    const bool by_request = !lookup.find_value("request_id").isNull();
    const bool by_session = !lookup.find_value("session_id").isNull();
    if (by_request == by_session) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "lookup must contain exactly one of request_id or session_id");
    }
    if (by_request) {
        const std::string request_id = lookup.find_value("request_id").get_str();
        if (!DigiDollar::Paymaster::IsCanonicalRequestId(request_id)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "request_id must be a canonical lowercase UUID");
        }
        return store.GetSessionByRequestId(request_id, session);
    }
    return store.GetSessionBySessionId(ParseHashO(lookup, "session_id"), session);
}

UniValue AttemptToJSON(const DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("attempt_id", attempt.attempt_id.GetHex());
    result.pushKV("provider_id", attempt.provider_id.GetHex());
    if (!attempt.provider_endpoint.empty()) {
        result.pushKV("provider_endpoint", attempt.provider_endpoint);
    }
    result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
    if (!attempt.quote_id.IsNull()) result.pushKV("quote_id", attempt.quote_id.GetHex());
    if (!attempt.template_commitment.IsNull()) {
        result.pushKV("template_commitment", attempt.template_commitment.GetHex());
    }
    if (!attempt.unsigned_txid.IsNull()) result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
    if (!attempt.commit_key.IsNull()) result.pushKV("commit_key", attempt.commit_key.GetHex());
    if (attempt.quote_expires_at > 0) result.pushKV("quote_expires_at", attempt.quote_expires_at);
    if (attempt.retry_until > 0) result.pushKV("retry_until", attempt.retry_until);
    if (!attempt.client_manifest.manifest_id.IsNull()) {
        result.pushKV("authorization_commitment",
                      attempt.client_manifest.manifest_id.GetHex());
        result.pushKV("authorization_accepted",
                      attempt.accepted_client_manifest_id ==
                              attempt.client_manifest.manifest_id &&
                          attempt.client_manifest_accepted_at > 0);
    }
    if (attempt.client_manifest_accepted_at > 0) {
        result.pushKV("authorization_accepted_at",
                      attempt.client_manifest_accepted_at);
    }
    return result;
}

std::vector<COutPoint> ParsePaymasterInputs(const UniValue& value)
{
    std::vector<COutPoint> inputs;
    std::set<COutPoint> unique;
    for (const UniValue& item : value.getValues()) {
        RPCTypeCheckObj(item,
                        {{"txid", UniValueType(UniValue::VSTR)},
                         {"vout", UniValueType(UniValue::VNUM)}},
                        /*fAllowNull=*/false, /*fStrict=*/true);
        const int64_t output_index = item.find_value("vout").getInt<int64_t>();
        if (output_index < 0 || output_index > std::numeric_limits<uint32_t>::max()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "selected input vout is out of range");
        }
        COutPoint outpoint{ParseHashO(item, "txid"), static_cast<uint32_t>(output_index)};
        if (outpoint.IsNull() || !unique.insert(outpoint).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "selected inputs contain an invalid duplicate");
        }
        inputs.push_back(outpoint);
    }
    if (inputs.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "selected_inputs must not be empty");
    return inputs;
}

std::vector<unsigned char> SerializeQuoteRequest(
    const DigiDollar::Paymaster::PaymasterQuoteRequest& quote_request)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << quote_request;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeCapacityRequest(
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << request;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeCapacityProof(
    const DigiDollar::Paymaster::PaymasterCapacityProof& proof)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << proof;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<DigiDollar::Paymaster::DirectMessage>
SelectCapacityProofMessages(
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce)
{
    using namespace DigiDollar::Paymaster;
    std::vector<DirectMessage> selected;
    selected.reserve(messages.size());
    for (const DirectMessage& message : messages) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (proof && proof->provider_id == provider_id &&
            proof->request_id == request_id &&
            proof->session_id == session_id &&
            proof->client_nonce == client_nonce) {
            selected.push_back(message);
        }
    }
    return selected;
}

template <typename T>
bool DeserializePaymasterHex(const UniValue& value, const char* field, T& decoded,
                             std::string& error)
{
    try {
        const std::vector<unsigned char> bytes = ParseHexV(value, field);
        CDataStream stream{bytes, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> decoded;
        if (!stream.empty()) throw std::ios_base::failure("trailing paymaster data");
    } catch (const std::ios_base::failure&) {
        error = strprintf("PAYMASTER_INVALID_%s_ENCODING", field);
        return false;
    }
    error.clear();
    return true;
}

std::vector<unsigned char> SerializeRedactedQuoteRequest(
    DigiDollar::Paymaster::PaymasterQuoteRequest quote_request)
{
    quote_request.restricted_descriptor.reset();
    quote_request.restricted_capability.reset();
    return SerializeQuoteRequest(quote_request);
}

std::vector<unsigned char> SerializeQuoteResponse(
    const DigiDollar::Paymaster::PaymasterQuoteResponse& quote_response)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << quote_response;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

enum class EquivocationBlockPolicy : uint8_t {
    OBSERVE_ONLY,
    ENFORCE,
};

// Direct-message acknowledgement happens only after durable evidence or a
// durable rejection has been written. Removing queue entries first would lose
// the only proof of a conflicting response on crash.
bool AcknowledgeEquivocationMessages(
    DigiDollar::Paymaster::Manager& manager,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    std::string& error)
{
    if (messages.empty()) return true;
    std::vector<uint256> message_ids;
    message_ids.reserve(messages.size());
    for (const auto& message : messages) {
        message_ids.push_back(message.message_id);
    }
    if (!manager.AcknowledgeDirectMessagesIfPresent(message_ids)) {
        error = "PAYMASTER_EQUIVOCATION_INBOX_ACK_CONFLICT";
        return false;
    }
    return true;
}

bool AcknowledgeRejectedDirectMessage(
    DigiDollar::Paymaster::Manager& manager,
    const DigiDollar::Paymaster::DirectMessage& message,
    std::string& error)
{
    if (!manager.AcknowledgeDirectMessagesIfPresent({message.message_id})) {
        error = "PAYMASTER_DIRECT_INBOX_ACK_CONFLICT";
        return false;
    }
    return true;
}

/** Return whether an immutable quote continuation can never become valid on a
 * later scheduler pass. Capacity is durably reserved before its proof is sent,
 * so a quote that cannot reload that exact reservation is not a transient
 * liquidity or synchronization failure. Keeping such a message leased would
 * let one stale or malicious peer repeatedly fault an otherwise healthy
 * automatic provider service.
 */
bool IsPermanentCapacityContinuationError(const std::string& error)
{
    return error == "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RESERVATION_MISSING" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_MISSING" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_EXPIRED" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_CONFLICT" ||
           error == "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RESPONSE_ENCODING" ||
           error == "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
}

/** Keep one provider-addressed inbound message leased until the durable
 * handler finishes. A normal return acknowledges the message; unwinding from
 * a local failure releases only the lease so the scheduler can retry it. The
 * caller must explicitly acknowledge permanently invalid messages before
 * throwing.
 */
class ProviderInboundMessageGuard final
{
public:
    ProviderInboundMessageGuard(
        DigiDollar::Paymaster::Manager& manager,
        const DigiDollar::Paymaster::DirectMessage& message) noexcept
        : m_manager{manager},
          m_message_id{message.message_id},
          m_uncaught_exceptions{std::uncaught_exceptions()}
    {
    }

    ProviderInboundMessageGuard(const ProviderInboundMessageGuard&) = delete;
    ProviderInboundMessageGuard& operator=(
        const ProviderInboundMessageGuard&) = delete;

    ~ProviderInboundMessageGuard()
    {
        if (std::uncaught_exceptions() != m_uncaught_exceptions) return;
        if (!m_manager.AcknowledgeDirectMessagesIfPresent({m_message_id})) {
            LogPrintf("Paymaster provider inbox acknowledgement failed\n");
        }
    }

private:
    DigiDollar::Paymaster::Manager& m_manager;
    const uint256 m_message_id;
    const int m_uncaught_exceptions;
};

bool PersistPendingCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < proofs.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterCapacityProof>(&proofs[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < proofs.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterCapacityProof>(
                &proofs[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingCapacityEquivocation(
                    attempt_id, SerializeCapacityProof(*first),
                    SerializeCapacityProof(*second),
                    std::max(proofs[first_index].received_at,
                             proofs[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

bool PersistPendingQuoteEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& quotes,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < quotes.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterQuoteResponse>(&quotes[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < quotes.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterQuoteResponse>(
                &quotes[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingQuoteEquivocation(
                    attempt_id, SerializeQuoteResponse(*first),
                    SerializeQuoteResponse(*second),
                    std::max(quotes[first_index].received_at,
                             quotes[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_QUOTE_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

bool PersistPendingAlternativeRecoveryCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& recovery_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < proofs.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterCapacityProof>(&proofs[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < proofs.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterCapacityProof>(
                &proofs[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingAlternativeRecoveryCapacityEquivocation(
                    recovery_id, SerializeCapacityProof(*first),
                    SerializeCapacityProof(*second),
                    std::max(proofs[first_index].received_at,
                             proofs[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

/** Drain provider-signed conflicts for one exact client attempt. Before the
 * first capacity snapshot or quote is accepted, every bounded pair is checked
 * and the first individually valid signed claim is durably staged. This keeps
 * sequential reconnect/restart conflicts provable even when neither artifact
 * passes the deeper local spend firewall. Staged claims are evidence-only and
 * never become authorization baselines. */
bool DrainClientAttemptEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error,
    EquivocationBlockPolicy block_policy = EquivocationBlockPolicy::ENFORCE,
    bool lease_messages = true)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (session.provider_side || attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt.attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(store, attempt.provider_id,
                                              error)) {
        return false;
    }

    std::optional<DirectMessageLease> proof_lease;
    std::vector<DirectMessage> unleased_proofs;
    if (lease_messages) {
        proof_lease.emplace(manager.LeaseCapacityProofs(
            attempt.provider_id, attempt.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_proofs = manager.PeekCapacityProofs(
            attempt.provider_id, attempt.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    }
    const std::vector<DirectMessage>& raw_proofs =
        proof_lease ? proof_lease->Messages() : unleased_proofs;
    const std::vector<DirectMessage> proofs = SelectCapacityProofMessages(
        raw_proofs, attempt.provider_id, session.request_id,
        session.session_id, attempt.client_nonce);
    if (attempt.capacity_snapshot.capacity_proof.empty()) {
        bool capacity_equivocation{false};
        if (!PersistPendingCapacityEquivocationPair(
                store, attempt.attempt_id, proofs, capacity_equivocation,
                error)) {
            return false;
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                return false;
            }
        } else {
            for (const DirectMessage& message : proofs) {
                const auto* proof =
                    std::get_if<PaymasterCapacityProof>(&message.payload);
                if (!proof) continue;
                bool claim_equivocation{false};
                if (!store.StageCapacityProofClaimCandidate(
                        attempt.attempt_id, SerializeCapacityProof(*proof),
                        message.received_at, claim_equivocation, error)) {
                    if (error !=
                        "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                        return false;
                    }
                    if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                          error)) {
                        return false;
                    }
                    continue;
                }
                if (claim_equivocation) {
                    if (!AcknowledgeEquivocationMessages(manager, proofs,
                                                         error)) {
                        return false;
                    }
                    if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                        return false;
                    }
                    break;
                }
                // Maintenance has no in-flight acceptance scope. Once the
                // signed candidate is durable it can consume the inbox copy;
                // active RPCs retain it for Chainstate validation below.
                if (!lease_messages &&
                    !AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
            }
        }
    } else {
        bool capacity_equivocation{false};
        for (const DirectMessage& message : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            if (!proof) continue;
            const std::vector<unsigned char> encoded =
                SerializeCapacityProof(*proof);
            if (encoded == attempt.capacity_snapshot.capacity_proof) continue;

            std::string evidence_error;
            if (store.RecordCapacityEquivocation(
                    attempt.attempt_id, encoded, message.received_at,
                    evidence_error)) {
                capacity_equivocation = true;
                continue;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
        if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
            return false;
        }
        if (capacity_equivocation &&
            block_policy == EquivocationBlockPolicy::ENFORCE) {
            error = "PAYMASTER_CAPACITY_EQUIVOCATION";
            return false;
        }
    }

    std::optional<DirectMessageLease> quote_lease;
    std::vector<DirectMessage> unleased_quotes;
    if (lease_messages) {
        quote_lease.emplace(manager.LeaseQuoteResponses(
            session.request_id, session.session_id, attempt.provider_id,
            attempt.intent_hash, MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_quotes = manager.PeekQuoteResponses(
            session.request_id, session.session_id, attempt.provider_id,
            attempt.intent_hash, MAX_DIRECT_INBOX_MESSAGES_PER_SESSION,
            /*lease=*/false);
    }
    const std::vector<DirectMessage>& quotes =
        quote_lease ? quote_lease->Messages() : unleased_quotes;
    if (attempt.signed_quote.empty()) {
        bool quote_equivocation{false};
        if (!PersistPendingQuoteEquivocationPair(
                store, attempt.attempt_id, quotes, quote_equivocation, error)) {
            return false;
        }
        if (quote_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, quotes, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_QUOTE_EQUIVOCATION";
                return false;
            }
        } else {
            for (const DirectMessage& message : quotes) {
                const auto* response =
                    std::get_if<PaymasterQuoteResponse>(&message.payload);
                if (!response) continue;
                bool claim_equivocation{false};
                if (!store.StageQuoteResponseClaimCandidate(
                        attempt.attempt_id,
                        SerializeQuoteResponse(*response), message.received_at,
                        claim_equivocation, error)) {
                    if (error != "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE") {
                        return false;
                    }
                    if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                          error)) {
                        return false;
                    }
                    continue;
                }
                if (claim_equivocation) {
                    if (!AcknowledgeEquivocationMessages(manager, quotes,
                                                         error)) {
                        return false;
                    }
                    if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                        error = "PAYMASTER_QUOTE_EQUIVOCATION";
                        return false;
                    }
                    break;
                }
                if (!lease_messages &&
                    !AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
            }
        }
    } else {
        bool quote_equivocation{false};
        for (const DirectMessage& message : quotes) {
            const auto* response =
                std::get_if<PaymasterQuoteResponse>(&message.payload);
            if (!response) continue;
            const std::vector<unsigned char> encoded =
                SerializeQuoteResponse(*response);
            if (encoded == attempt.signed_quote) continue;

            std::string evidence_error;
            if (store.RecordQuoteEquivocation(
                    attempt.attempt_id, encoded, message.received_at,
                    evidence_error)) {
                quote_equivocation = true;
                continue;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_QUOTE_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
        if (!AcknowledgeEquivocationMessages(manager, quotes, error)) {
            return false;
        }
        if (quote_equivocation &&
            block_policy == EquivocationBlockPolicy::ENFORCE) {
            error = "PAYMASTER_QUOTE_EQUIVOCATION";
            return false;
        }
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(store, attempt.provider_id,
                                              error)) {
        return false;
    }
    return true;
}

bool DrainClientSessionEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    std::string& error,
    EquivocationBlockPolicy active_block_policy =
        EquivocationBlockPolicy::OBSERVE_ONLY)
{
    using namespace DigiDollar::Paymaster;
    if (session.provider_side) return true;
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!store.GetAttempt(attempt_id, attempt)) {
            error = "PAYMASTER_ATTEMPT_NOT_FOUND";
            return false;
        }
        const EquivocationBlockPolicy attempt_policy =
            attempt_id == session.attempt_ids.back() ? active_block_policy : EquivocationBlockPolicy::OBSERVE_ONLY;
        if (!DrainClientAttemptEquivocations(
                manager, store, session, attempt, error, attempt_policy)) {
            return false;
        }
    }
    error.clear();
    return true;
}

bool DrainAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    std::string& error,
    EquivocationBlockPolicy block_policy =
        EquivocationBlockPolicy::ENFORCE,
    bool lease_messages = true)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (recovery.provider_side) {
        return true;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    std::optional<DirectMessageLease> proof_lease;
    std::vector<DirectMessage> unleased_proofs;
    if (lease_messages) {
        proof_lease.emplace(manager.LeaseCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_proofs = manager.PeekCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    }
    const std::vector<DirectMessage>& raw_proofs =
        proof_lease ? proof_lease->Messages() : unleased_proofs;
    const std::vector<DirectMessage> proofs = SelectCapacityProofMessages(
        raw_proofs, recovery.recovery_provider_id,
        recovery.capacity_request.request_id,
        recovery.capacity_request.session_id, recovery.client_nonce);
    if (recovery.capacity_snapshot.capacity_proof.empty()) {
        if (recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING) {
            return true;
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
            return false;
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                return false;
            }
            return true;
        }
        for (const DirectMessage& message : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            if (!proof) continue;
            bool claim_equivocation{false};
            if (!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
                    recovery.recovery_id, SerializeCapacityProof(*proof),
                    message.received_at, claim_equivocation, error)) {
                if (error != "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                    return false;
                }
                if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                    return false;
                }
                if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                    error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                    return false;
                }
                return true;
            }
            if (!lease_messages &&
                !AcknowledgeRejectedDirectMessage(manager, message, error)) {
                return false;
            }
        }
        return true;
    }
    bool capacity_equivocation{false};
    for (const DirectMessage& message : proofs) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (!proof) continue;
        const std::vector<unsigned char> encoded =
            SerializeCapacityProof(*proof);
        if (encoded == recovery.capacity_snapshot.capacity_proof) continue;

        std::string evidence_error;
        if (store.RecordAlternativeRecoveryCapacityEquivocation(
                recovery.recovery_id, encoded, message.received_at,
                evidence_error)) {
            capacity_equivocation = true;
            continue;
        }
        if (!evidence_error.empty() &&
            evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
            error = std::move(evidence_error);
            return false;
        }
    }
    if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
        return false;
    }
    if (capacity_equivocation &&
        block_policy == EquivocationBlockPolicy::ENFORCE) {
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    return true;
}

bool DrainPendingAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const std::vector<unsigned char>& first_capacity_proof,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (recovery.provider_side || first_capacity_proof.empty() ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING) {
        error = "PAYMASTER_INVALID_RECOVERY_CAPACITY_STATE";
        return false;
    }
    if (!EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    const auto raw_proofs = manager.PeekCapacityProofs(
        recovery.recovery_provider_id, recovery.client_nonce,
        MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    const auto proofs = SelectCapacityProofMessages(
        raw_proofs, recovery.recovery_provider_id,
        recovery.capacity_request.request_id,
        recovery.capacity_request.session_id, recovery.client_nonce);
    bool capacity_equivocation{false};
    for (const DirectMessage& message : proofs) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (!proof) continue;
        const std::vector<unsigned char> encoded =
            SerializeCapacityProof(*proof);
        if (encoded == first_capacity_proof) continue;
        std::string evidence_error;
        if (store.RecordPendingAlternativeRecoveryCapacityEquivocation(
                recovery.recovery_id, first_capacity_proof, encoded,
                message.received_at, evidence_error)) {
            capacity_equivocation = true;
            continue;
        }
        if (!evidence_error.empty() &&
            evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
            error = std::move(evidence_error);
            return false;
        }
    }
    if (capacity_equivocation) {
        if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    // The calling acceptance scope owns the prune lease until the recovery
    // snapshot and signed request commit. This comparison must not acquire a
    // second, unscoped reference that could survive an early return.
    return EnsureProviderNotEquivocationBlocked(
        store, recovery.recovery_provider_id, error);
}

std::vector<unsigned char> SerializeSubmit(
    const DigiDollar::Paymaster::PaymasterSubmit& submit)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << submit;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename T>
std::vector<unsigned char> SerializeRecoveryMessage(const T& message)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << message;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename T>
bool DeserializeCanonicalRecoveryMessage(
    const std::vector<unsigned char>& encoded,
    T& message,
    std::string& error)
{
    try {
        SpanReader stream{::PROTOCOL_VERSION, encoded};
        stream >> message;
        if (!stream.empty() || SerializeRecoveryMessage(message) != encoded) {
            throw std::ios_base::failure("non-canonical recovery message");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PERSISTED_RECOVERY_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

struct RecoveryQueueState {
    bool queued{false};
    bool connection_pending{false};
    bool route_available{false};
};

template <typename T>
// Recovery messages use the same bounded direct-message transport as normal
// Paymaster traffic, but their semantic replay key is tied to the durable
// session rather than the transient peer connection.
bool QueueRecoveryMessage(WalletContext& context,
                          CWallet& wallet,
                          const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
                          const T& message,
                          int64_t now,
                          RecoveryQueueState& state,
                          std::string& error)
{
    using namespace DigiDollar::Paymaster;
    state = {};
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "PAYMASTER_DISABLED";
        return false;
    }
    const bool allow_local_endpoint{
        Params().GetChainType() == ChainType::REGTEST};
    CService endpoint = LookupNumeric(recovery.recovery_provider_endpoint);
    state.route_available = IsEndpointAllowedForPrivacyProfile(
        endpoint, recovery.privacy_profile, allow_local_endpoint);
    if (!state.route_available) {
        for (const Announcement& announcement :
             context.paymaster->GetDirectory().List(now)) {
            if (GetPaymasterId(announcement.identity_key) ==
                    recovery.recovery_provider_id &&
                IsEndpointAllowedForPrivacyProfile(
                    announcement.endpoint, recovery.privacy_profile,
                    allow_local_endpoint)) {
                endpoint = announcement.endpoint;
                state.route_available = true;
                break;
            }
        }
    }
    if (!state.route_available) return true;
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    NodeId peer_id{-1};
    node->connman->ForEachNode([&](CNode* peer) {
        if (peer_id == -1 && peer->IsPaymasterConn() &&
            peer->addr == endpoint) {
            peer_id = peer->GetId();
        }
    });
    if (peer_id == -1) {
        state.connection_pending = node->connman->AddConnection(
            endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
            /*paymaster_high_privacy=*/
            recovery.privacy_profile == PrivacyProfile::HIGH);
        return true;
    }
    const std::vector<unsigned char> encoded =
        SerializeRecoveryMessage(message);
    const uint256 message_id{Hash(encoded)};
    state.queued =
        context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
        context.paymaster->QueueOutboundDirectMessage(
            peer_id, message_id, encoded.size(), DirectPayload{message}, now);
    if (!state.queued) {
        error = "PAYMASTER_DIRECT_QUEUE_FULL";
        return false;
    }
    node->connman->WakeMessageHandler();
    return true;
}

std::string AlternativeRecoveryPhaseName(
    DigiDollar::Paymaster::AlternativeRecoveryPhase phase)
{
    using DigiDollar::Paymaster::AlternativeRecoveryPhase;
    switch (phase) {
    case AlternativeRecoveryPhase::CAPACITY_PENDING: return "capacity_pending";
    case AlternativeRecoveryPhase::REQUEST_READY: return "request_ready";
    case AlternativeRecoveryPhase::RESPONSE_VALIDATED: return "response_validated";
    case AlternativeRecoveryPhase::USER_SIGNED: return "user_signed";
    case AlternativeRecoveryPhase::FINAL_COMMITTED: return "final_committed";
    }
    return "unknown";
}

UniValue AlternativeRecoveryToJSON(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("recovery_id", recovery.recovery_id.GetHex());
    result.pushKV("recovery_provider_id",
                  recovery.recovery_provider_id.GetHex());
    result.pushKV("offer_id", recovery.offer_id.GetHex());
    result.pushKV("policy_hash", recovery.policy_hash.GetHex());
    result.pushKV("original_commit_key",
                  recovery.original_commit_key.GetHex());
    result.pushKV("original_template_commitment",
                  recovery.original_template_commitment.GetHex());
    result.pushKV("privacy_profile",
                  recovery.privacy_profile == PrivacyProfile::HIGH ? "high" : "standard");
    result.pushKV("phase", AlternativeRecoveryPhaseName(recovery.phase));
    result.pushKV("expired", recovery.expired);
    UniValue user_inputs{UniValue::VARR};
    for (const COutPoint& outpoint : recovery.recovery_request.user_dd_inputs) {
        UniValue input{UniValue::VOBJ};
        input.pushKV("txid", outpoint.hash.GetHex());
        input.pushKV("vout", outpoint.n);
        user_inputs.push_back(std::move(input));
    }
    result.pushKV("user_dd_inputs", std::move(user_inputs));
    UniValue wallet_returns{UniValue::VARR};
    for (const AlternativeRecoveryReturn& wallet_return :
         recovery.recovery_request.wallet_returns) {
        UniValue output{UniValue::VOBJ};
        output.pushKV("script_pub_key", HexStr(wallet_return.script_pub_key));
        CTxDestination destination;
        if (ExtractDestination(wallet_return.script_pub_key, destination)) {
            output.pushKV("address", EncodeDestination(destination));
        }
        output.pushKV("amount_cents", wallet_return.amount.value);
        wallet_returns.push_back(std::move(output));
    }
    result.pushKV("wallet_returns", std::move(wallet_returns));
    if (!recovery.capacity_snapshot.snapshot_id.IsNull()) {
        result.pushKV("capacity_snapshot_id",
                      recovery.capacity_snapshot.snapshot_id.GetHex());
        result.pushKV("capacity_resource_commitment",
                      recovery.capacity_snapshot.resource_commitment.GetHex());
    }
    if (!recovery.recovery_authorization.authorization_commitment.IsNull()) {
        result.pushKV(
            "authorization_commitment",
            recovery.recovery_authorization.authorization_commitment.GetHex());
        result.pushKV(
            "authorization_accepted",
            !recovery.provider_side &&
                recovery.accepted_recovery_authorization_commitment ==
                    recovery.recovery_authorization.authorization_commitment &&
                recovery.recovery_authorization_accepted_at > 0);
        result.pushKV("maximum_service_fee_cents",
                      recovery.recovery_authorization.maximum_service_fee.value);
        result.pushKV("service_fee_cents",
                      recovery.recovery_authorization.service_fee.value);
        result.pushKV("network_fee_satoshis",
                      recovery.recovery_authorization.network_fee.value);
    } else {
        result.pushKV("maximum_service_fee_cents",
                      recovery.selected_maximum_service_fee.value);
        result.pushKV("service_fee_cents",
                      recovery.selected_service_fee.value);
    }
    if (recovery.recovery_authorization_accepted_at > 0) {
        result.pushKV("authorization_accepted_at",
                      recovery.recovery_authorization_accepted_at);
    }
    if (!recovery.final_transaction.empty()) {
        result.pushKV("raw_transaction", HexStr(recovery.final_transaction));
        if (recovery.signed_result.txid) {
            result.pushKV("txid", recovery.signed_result.txid->GetHex());
        }
        result.pushKV("wtxid", recovery.expected_wtxid.GetHex());
    }
    result.pushKV("expires_at",
                  recovery.phase >= AlternativeRecoveryPhase::RESPONSE_VALIDATED ? recovery.recovery_response.expires_at : recovery.capacity_request.expires_at);
    return result;
}

bool BuildRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error)
{
    // Keep RPC, background recovery and restart recovery on the same ownership
    // and immutable-template firewall. In particular, the shared builder also
    // proves that every locally supplied provider input and every wallet-return
    // script belongs to the wallet before either side can sign.
    return BuildAlternativeRecoveryParametersFromRecord(
        wallet, recovery, parameters, error);
}

struct PaymasterSubmitQueueState {
    bool queued{false};
    bool connection_pending{false};
    bool route_available{false};
};

bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);

bool HasDurableClientAuthorization(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t observation_time)
{
    using namespace DigiDollar::Paymaster;
    // A persisted signature or later attempt state is never a substitute for
    // the wallet's durable acceptance of this exact, current manifest.
    return attempt.client_manifest.version ==
               ClientAuthorizationManifest::CURRENT_VERSION &&
           !attempt.client_manifest.manifest_id.IsNull() &&
           !attempt.accepted_client_manifest_id.IsNull() &&
           attempt.accepted_client_manifest_id ==
               attempt.client_manifest.manifest_id &&
           attempt.created_at > 0 &&
           attempt.client_manifest_accepted_at > 0 &&
           attempt.client_manifest_accepted_at >= attempt.created_at &&
           attempt.client_manifest_accepted_at <= observation_time &&
           attempt.client_manifest_accepted_at <= attempt.updated_at &&
           attempt.retry_until > 0 &&
           attempt.client_manifest_accepted_at <= attempt.retry_until &&
           attempt.client_manifest_accepted_at <=
               attempt.client_manifest.expires_at &&
           attempt.capacity_snapshot.version ==
               ValidatedCapacitySnapshot::CURRENT_VERSION &&
           attempt.capacity_snapshot.validated_at > 0 &&
           attempt.capacity_snapshot.expires_at > 0 &&
           attempt.client_manifest_accepted_at >=
               attempt.capacity_snapshot.validated_at &&
           attempt.client_manifest_accepted_at <=
               attempt.capacity_snapshot.expires_at;
}

bool QueuePersistedPaymasterSubmit(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    PaymasterSubmitQueueState& state,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    state = {};
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "PAYMASTER_DISABLED";
        return false;
    }
    if (attempt.user_signed_psbt.empty() || attempt.retry_until <= 0 ||
        now > attempt.retry_until) {
        error = "PAYMASTER_RETRY_UNAVAILABLE";
        return false;
    }
    PaymentIntent authorized_intent;
    PaymasterQuote authorized_quote;
    CollaborativePSBTTemplate authorized_template;
    if (!HasDurableClientAuthorization(attempt, now) ||
        attempt.client_manifest_accepted_at > session.updated_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
        return false;
    }
    if (!LoadAttemptAuthorizationArtifacts(
            attempt, authorized_intent, authorized_quote, authorized_template, error) ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, authorized_intent, authorized_quote,
            attempt.capacity_snapshot, authorized_template, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }

    // A persisted USER signature is not authority to reuse stale or replaced
    // provider liquidity. Reconstruct the exact capacity artifacts and apply
    // the same identity/control-proof/chainstate firewall used immediately
    // before the first signature on every retry.
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    try {
        SpanReader request_stream{::PROTOCOL_VERSION,
                                  attempt.capacity_request};
        request_stream >> capacity_request;
        SpanReader proof_stream{
            ::PROTOCOL_VERSION,
            attempt.capacity_snapshot.capacity_proof};
        proof_stream >> capacity_proof;
        if (!request_stream.empty() || !proof_stream.empty()) {
            throw std::ios_base::failure(
                "trailing persisted capacity artifact data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_ARTIFACT_ENCODING";
        return false;
    }
    CDataStream canonical_request{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_request << capacity_request;
    CDataStream canonical_proof{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_proof << capacity_proof;
    const auto canonical_request_span = MakeUCharSpan(canonical_request);
    const auto canonical_proof_span = MakeUCharSpan(canonical_proof);
    if (std::vector<unsigned char>{canonical_request_span.begin(),
                                   canonical_request_span.end()} !=
            attempt.capacity_request ||
        std::vector<unsigned char>{canonical_proof_span.begin(),
                                   canonical_proof_span.end()} !=
            attempt.capacity_snapshot.capacity_proof ||
        Hash(attempt.capacity_request) !=
            attempt.capacity_snapshot.request_hash) {
        error = "PAYMASTER_CAPACITY_ARTIFACT_NONCANONICAL";
        return false;
    }
    node::NodeContext* node = wallet.chain().context();
    const int64_t observation_time =
        std::max(now, attempt.client_manifest_accepted_at);
    if (!node || !node->chainman ||
        !ValidateAuthorizedCapacityRetryAgainstChainstate(
            capacity_proof, capacity_request,
            attempt.provider_identity_key, *node->chainman,
            attempt.client_manifest_accepted_at, observation_time,
            AuthorizedCapacityResourceMode::REQUIRE_UNSPENT, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_NOT_CURRENT";
        return false;
    }
    PartiallySignedTransaction persisted_user_psbt;
    if (!DecodeRawPSBT(persisted_user_psbt,
                       MakeByteSpan(attempt.user_signed_psbt), error) ||
        !ValidateCollaborativePSBT(
            persisted_user_psbt, authorized_template,
            CollaborativeSignatureStage::USER_SIGNED, error)) {
        if (error.empty()) error = "PAYMASTER_PERSISTED_USER_PSBT_INVALID";
        return false;
    }
    CDataStream canonical_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_stream << persisted_user_psbt;
    const auto canonical_span = MakeUCharSpan(canonical_stream);
    if (std::vector<unsigned char>{canonical_span.begin(), canonical_span.end()} !=
        attempt.user_signed_psbt) {
        error = "PAYMASTER_PERSISTED_USER_PSBT_NONCANONICAL";
        return false;
    }
    if (!ValidateClientAuthorizationOwnership(
            wallet, attempt.client_manifest, error)) {
        return false;
    }

    PaymasterSubmit submit;
    submit.genesis_hash = Params().GenesisBlock().GetHash();
    submit.provider_id = attempt.provider_id;
    submit.request_id = session.request_id;
    submit.session_id = session.session_id;
    submit.quote_id = attempt.quote_id;
    submit.commit_key = attempt.commit_key;
    submit.template_commitment = attempt.template_commitment;
    submit.user_psbt = attempt.user_signed_psbt;
    if (!ValidateSubmitEnvelope(submit, submit.genesis_hash, error)) return false;

    CService endpoint;
    const bool allow_local_endpoint{
        Params().GetChainType() == ChainType::REGTEST};
    if (!attempt.provider_endpoint.empty()) {
        endpoint = LookupNumeric(attempt.provider_endpoint);
        state.route_available = IsEndpointAllowedForPrivacyProfile(
            endpoint, attempt.privacy_profile, allow_local_endpoint);
    }
    if (!state.route_available) {
        for (const Announcement& announcement : context.paymaster->GetDirectory().List(now)) {
            if (GetPaymasterId(announcement.identity_key) == attempt.provider_id &&
                IsEndpointAllowedForPrivacyProfile(
                    announcement.endpoint, attempt.privacy_profile,
                    allow_local_endpoint)) {
                endpoint = announcement.endpoint;
                state.route_available = true;
                break;
            }
        }
    }
    if (!state.route_available) return true;

    if (!node || !node->connman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    NodeId peer_id{-1};
    node->connman->ForEachNode([&](CNode* peer) {
        if (peer_id == -1 && peer->IsPaymasterConn() && peer->addr == endpoint) {
            peer_id = peer->GetId();
        }
    });
    if (peer_id == -1) {
        state.connection_pending = node->connman->AddConnection(
            endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
            /*paymaster_high_privacy=*/attempt.privacy_profile == PrivacyProfile::HIGH);
        return true;
    }

    const std::vector<unsigned char> submit_bytes = SerializeSubmit(submit);
    const uint256 message_id = Hash(submit_bytes);
    state.queued = context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                   context.paymaster->QueueOutboundDirectMessage(
                       peer_id, message_id, submit_bytes.size(), DirectPayload{submit}, now);
    if (!state.queued) {
        error = "PAYMASTER_DIRECT_QUEUE_FULL";
        return false;
    }
    node->connman->WakeMessageHandler();
    return true;
}

std::vector<unsigned char> SerializePaymentIntent(
    const DigiDollar::Paymaster::PaymentIntent& intent)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << intent;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

// -------------------------------------------------------------------------
// Final transaction validation and broadcast
// -------------------------------------------------------------------------
// The persisted PSBT/template is the trust anchor. Peer-returned raw
// transactions are inserted into that template and fully checked before the
// wallet records a terminal state or broadcasts anything.
bool LoadTrustedTemplate(const DigiDollar::Paymaster::ProviderAttempt& attempt,
                         DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
                         std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (!DecodeRawPSBT(trusted.psbt, MakeByteSpan(attempt.unsigned_psbt), error)) return false;
    if (!trusted.psbt.tx || trusted.psbt.inputs.size() != attempt.input_roles.size()) {
        error = "PAYMASTER_PERSISTED_TEMPLATE_SHAPE";
        return false;
    }
    static_assert(static_cast<uint8_t>(ReservationRole::USER_DD) == static_cast<uint8_t>(InputRole::USER_DD));
    static_assert(static_cast<uint8_t>(ReservationRole::USER_DGB) == static_cast<uint8_t>(InputRole::USER_DGB));
    static_assert(static_cast<uint8_t>(ReservationRole::PROVIDER_CARRIER) == static_cast<uint8_t>(InputRole::PROVIDER_CARRIER));
    static_assert(static_cast<uint8_t>(ReservationRole::PROVIDER_DGB) == static_cast<uint8_t>(InputRole::PROVIDER_DGB));
    trusted.input_roles.reserve(attempt.input_roles.size());
    for (const ReservationRole role : attempt.input_roles) {
        trusted.input_roles.push_back(static_cast<InputRole>(role));
    }
    return true;
}

bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    try {
        CDataStream request_stream{attempt.quote_request, SER_NETWORK,
                                   ::PROTOCOL_VERSION};
        request_stream >> request;
        SpanReader response_stream{::PROTOCOL_VERSION, attempt.signed_quote};
        response_stream >> response;
        if (!request_stream.empty() || !response_stream.empty()) {
            throw std::ios_base::failure("trailing authorization artifact data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_AUTHORIZATION_ARTIFACT_ENCODING";
        return false;
    }
    if (request.intent.request_id != response.request_id ||
        request.intent.session_id != response.session_id ||
        request.intent.provider_id != response.quote.provider_id ||
        GetPaymentIntentHash(request.intent) != response.quote.intent_hash ||
        response.quote.quote_id != attempt.quote_id ||
        response.quote.template_commitment != attempt.template_commitment ||
        response.quote.unsigned_txid != attempt.unsigned_txid) {
        error = "PAYMASTER_AUTHORIZATION_ARTIFACT_CONFLICT";
        return false;
    }
    if (!LoadTrustedTemplate(attempt, trusted, error)) return false;
    intent = std::move(request.intent);
    quote = std::move(response.quote);
    return true;
}

bool ValidateDurableProviderUserAuthorization(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    UserAuthorizationRecord authorization;
    if (attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
        attempt.commit_key.IsNull() || attempt.user_signed_psbt.empty() ||
        !store.GetUserAuthorization(attempt.commit_key, authorization) ||
        authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key != attempt.commit_key ||
        authorization.attempt_id != attempt.attempt_id ||
        authorization.canonical_psbt_hash !=
            Hash(attempt.user_signed_psbt) ||
        authorization.accepted_at <= 0 ||
        authorization.retry_until != attempt.retry_until) {
        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
        return false;
    }
    return true;
}

bool FindDurableClientAuthorizationAttempt(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    int64_t observation_time,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (session.provider_side) return false;
    for (auto it = session.attempt_ids.rbegin();
         it != session.attempt_ids.rend(); ++it) {
        ProviderAttempt candidate;
        if (!store.GetAttempt(*it, candidate)) {
            error = "PAYMASTER_ATTEMPT_NOT_FOUND";
            return false;
        }
        if (!HasDurableClientAuthorization(candidate, observation_time) ||
            candidate.client_manifest_accepted_at > session.updated_at) {
            const bool has_authorization_evidence =
                !candidate.accepted_client_manifest_id.IsNull() ||
                candidate.client_manifest_accepted_at != 0 ||
                !candidate.user_signed_psbt.empty() ||
                (candidate.state >= AttemptState::USER_SIGNED &&
                 candidate.state <= AttemptState::MEMPOOL) ||
                candidate.state == AttemptState::AMBIGUOUS;
            if (has_authorization_evidence) {
                error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
                return false;
            }
            continue;
        }
        CollaborativePSBTTemplate trusted;
        if (!LoadAttemptAuthorizationArtifacts(
                candidate, intent, quote, trusted, error) ||
            !ValidateClientAuthorizationManifest(
                candidate.client_manifest, intent, quote,
                candidate.capacity_snapshot, trusted, error) ||
            !ValidateClientAuthorizationOwnership(
                wallet, candidate.client_manifest, error)) {
            if (error.empty()) {
                error = "PAYMASTER_CLIENT_AUTHORIZATION_INVALID";
            }
            return false;
        }
        attempt = std::move(candidate);
        return true;
    }
    error.clear();
    return false;
}

UniValue DurableClientAuthorizationToJSON(
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote)
{
    using namespace DigiDollar::Paymaster;
    UniValue result = SessionToJSON(session);
    result.pushKV("provider_id", attempt.provider_id.GetHex());
    result.pushKV("offer_id", intent.offer_id.GetHex());
    result.pushKV("policy_hash", intent.policy_hash.GetHex());
    result.pushKV("funding_model",
                  intent.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
    result.pushKV("payment_cents", intent.recipient_amount.value);
    result.pushKV("service_fee_cents", quote.service_fee.value);
    result.pushKV("user_total_cents",
                  intent.recipient_amount.value + quote.service_fee.value);
    result.pushKV("expires_at", intent.expires_at);
    result.pushKV("queued", false);
    result.pushKV("connection_pending", false);
    result.pushKV("capacity_pending", false);
    result.pushKV("capacity_snapshot_id",
                  attempt.capacity_snapshot.snapshot_id.GetHex());
    result.pushKV("quote_id", attempt.quote_id.GetHex());
    result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
    result.pushKV("template_commitment",
                  attempt.template_commitment.GetHex());
    result.pushKV("authorization_commitment",
                  attempt.client_manifest.manifest_id.GetHex());
    const bool authorization_accepted =
        attempt.accepted_client_manifest_id ==
            attempt.client_manifest.manifest_id &&
        attempt.client_manifest_accepted_at > 0;
    result.pushKV("authorization_accepted", authorization_accepted);
    if (authorization_accepted) {
        result.pushKV("authorization_accepted_at",
                      attempt.client_manifest_accepted_at);
    }
    result.pushKV("psbt", EncodeBase64(attempt.unsigned_psbt));
    return result;
}

bool GetValidatedClientAuthorizationCommitment(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    uint256& commitment,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymentIntent intent;
    PaymasterQuote quote;
    CollaborativePSBTTemplate trusted;
    if (!LoadAttemptAuthorizationArtifacts(attempt, intent, quote, trusted, error) ||
        attempt.client_manifest.manifest_id.IsNull() ||
        (attempt.state == AttemptState::QUOTED && now > attempt.quote_expires_at) ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, intent, quote, attempt.capacity_snapshot,
            trusted, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }
    commitment = attempt.client_manifest.manifest_id;
    return true;
}

std::vector<unsigned char> SerializePSBT(const PartiallySignedTransaction& psbt)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << psbt;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeTransaction(const CMutableTransaction& transaction)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << transaction;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

using FinalTransactionPresence = ExactFinalTransactionPresence;

bool PreflightFinalPaymasterTransaction(CWallet& wallet,
                                        const CTransactionRef& transaction,
                                        FinalTransactionPresence& presence,
                                        std::string& error,
                                        ExactFinalTxIndexMode txindex_mode =
                                            ExactFinalTxIndexMode::WAIT_FOR_SYNC)
{
    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, txindex_mode,
            preflight, error)) {
        presence = FinalTransactionPresence::NONE;
        return false;
    }
    presence = preflight.presence;
    return true;
}

bool MarkFinalValidationFailureRecoverable(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    std::string& error)
{
    return store.MarkClientFinalValidationFailureRecoverable(
        session.request_id, attempt.attempt_id, now, error);
}

std::vector<COutPoint> ProviderInputs(const CMutableTransaction& transaction,
                                      const std::vector<DigiDollar::Paymaster::ReservationRole>& roles)
{
    using namespace DigiDollar::Paymaster;
    std::vector<COutPoint> result;
    if (transaction.vin.size() != roles.size()) return result;
    for (size_t index = 0; index < roles.size(); ++index) {
        if (roles[index] == ReservationRole::PROVIDER_CARRIER ||
            roles[index] == ReservationRole::PROVIDER_DGB) {
            result.push_back(transaction.vin[index].prevout);
        }
    }
    return result;
}

bool TemplateInputsAvailable(CWallet& wallet, const CMutableTransaction& transaction)
{
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& input : transaction.vin)
        coins.emplace(input.prevout, Coin{});
    wallet.chain().findCoins(coins);
    return std::all_of(coins.begin(), coins.end(), [](const auto& entry) {
        return !entry.second.IsSpent();
    });
}

bool RejectUnavailableTemplateInputs(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    const CMutableTransaction& transaction,
    int64_t now,
    bool& rejected,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    rejected = false;
    if (attempt.state == AttemptState::REJECTED) {
        if (!store.GetProviderResult(attempt.commit_key, result) ||
            result.status != PaymasterResultStatus::REJECTED) {
            error = "PAYMASTER_REJECTED_RESULT_MISSING";
            return false;
        }
        rejected = true;
        return true;
    }
    if (attempt.state != AttemptState::QUOTED &&
        attempt.state != AttemptState::USER_SIGNED &&
        attempt.state != AttemptState::USER_PSBT_ACCEPTED) {
        return true;
    }
    if (TemplateInputsAvailable(wallet, transaction)) return true;

    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != attempt.provider_id ||
        identity.identity_key != attempt.provider_identity_key) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::REJECTED;
    result.updated_at = now;
    result.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    if (!store.RejectUnavailableProviderSubmit(
            attempt.attempt_id, result, result.genesis_hash, attempt, error)) {
        return false;
    }
    rejected = true;
    return true;
}

bool BuildFinalProviderResult(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    result = PaymasterResult{};
    CMutableTransaction transaction;
    try {
        SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
        stream >> transaction;
        if (!stream.empty()) {
            error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != commit.provider_id) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = commit.provider_id;
    result.commit_key = commit.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = CTransaction{transaction}.GetHash();
    result.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
    result.final_transaction = transaction;
    result.updated_at = now;
    result.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    error.clear();
    return true;
}

bool GetOrCreateFinalProviderResult(CWallet& wallet,
                                    PaymasterStore& store,
                                    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
                                    int64_t now,
                                    DigiDollar::Paymaster::PaymasterResult& result,
                                    std::string& error)
{
    if (store.GetProviderResult(commit.commit_key, result)) return true;
    if (!BuildFinalProviderResult(wallet, commit, now, result, error)) return false;
    return store.StoreProviderResult(result, result.genesis_hash, error);
}

bool InsertAndBroadcastPaymasterTransaction(CWallet& wallet,
                                            const CTransactionRef& transaction,
                                            bool& already_confirmed,
                                            std::string& error)
{
    return InsertAndBroadcastExactPaymasterTransaction(
        wallet, transaction, already_confirmed, error);
}

// Wallet insertion precedes network broadcast so restart recovery can find the
// exact authorized transaction even if the process exits between the two.
bool InsertAndBroadcastProviderCommit(CWallet& wallet,
                                      PaymasterStore& store,
                                      const DigiDollar::Paymaster::ProviderCommitRecord& commit,
                                      int64_t now,
                                      bool& already_confirmed,
                                      std::string& error,
                                      CTransactionRef* exact_transaction = nullptr)
{
    using namespace DigiDollar::Paymaster;
    ProviderAttempt attempt;
    if (!store.GetAttemptByTemplateCommitment(commit.template_commitment, attempt)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    CMutableTransaction decoded;
    if (!ValidateProviderCommitForExecution(attempt, commit, now, decoded, error) ||
        !store.ValidateProviderBudgetAuthorization(
            attempt, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }
    if (attempt.provider_manifest.manifest_id.IsNull() ||
        !ValidateProviderAuthorizationOwnership(
            wallet, attempt.provider_manifest, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        }
        return false;
    }
    const CTransactionRef transaction = MakeTransactionRef(decoded);
    if (exact_transaction) *exact_transaction = transaction;
    FinalTransactionPresence presence{FinalTransactionPresence::NONE};
    if (!PreflightFinalPaymasterTransaction(wallet, transaction, presence, error)) {
        return false;
    }
    return InsertAndBroadcastPaymasterTransaction(
        wallet, transaction, already_confirmed, error);
}

bool MarkProviderResultBroadcastAttempted(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymasterResult& result,
    int64_t now,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (result.status == PaymasterResultStatus::BROADCAST_ATTEMPTED) return true;
    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != result.provider_id) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.status = PaymasterResultStatus::BROADCAST_ATTEMPTED;
    ++result.result_sequence;
    result.updated_at = std::max(result.updated_at, now);
    result.identity_signature.assign(64, 0);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    return store.StoreProviderResult(result, result.genesis_hash, error);
}

struct ProviderCommitRecoveryResult {
    bool broadcast{false};
    bool already_confirmed{false};
    DigiDollar::Paymaster::PaymasterResult provider_result;
    std::string error;
};

bool IsProviderNetworkStateAtLeast(
    DigiDollar::Paymaster::AttemptState state,
    DigiDollar::Paymaster::AttemptState minimum)
{
    using DigiDollar::Paymaster::AttemptState;
    switch (minimum) {
    case AttemptState::BROADCAST:
        return state == AttemptState::BROADCAST ||
               state == AttemptState::STEMPOOL ||
               state == AttemptState::MEMPOOL;
    case AttemptState::STEMPOOL:
        return state == AttemptState::STEMPOOL ||
               state == AttemptState::MEMPOOL;
    case AttemptState::MEMPOOL:
        return state == AttemptState::MEMPOOL;
    default:
        return false;
    }
}

bool IsProviderSessionNetworkStateAtLeast(
    DigiDollar::Paymaster::SessionState state,
    DigiDollar::Paymaster::SessionState minimum)
{
    using DigiDollar::Paymaster::SessionState;
    switch (minimum) {
    case SessionState::STEMPOOL:
        return state == SessionState::STEMPOOL ||
               state == SessionState::MEMPOOL ||
               state == SessionState::CONFIRMED;
    case SessionState::MEMPOOL:
        return state == SessionState::MEMPOOL ||
               state == SessionState::CONFIRMED;
    case SessionState::CONFIRMED:
        return state == SessionState::CONFIRMED;
    default:
        return false;
    }
}

bool ReloadExactProviderCommitAttempt(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    const uint256& expected_attempt_id,
    const uint256& expected_session_id,
    int64_t now,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    ProviderAttempt reloaded;
    CMutableTransaction validated;
    if (!store.GetAttemptByTemplateCommitment(
            commit.template_commitment, reloaded)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (reloaded.attempt_id != expected_attempt_id ||
        reloaded.session_id != expected_session_id ||
        !ValidateProviderCommitForExecution(
            reloaded, commit, now, validated, error) ||
        !store.ValidateProviderBudgetAuthorization(
            reloaded, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return false;
    }
    attempt = std::move(reloaded);
    return true;
}

bool ReloadExactProviderCommitSession(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    const std::string& expected_request_id,
    const uint256& expected_session_id,
    const uint256& expected_attempt_id,
    DigiDollar::Paymaster::PaymentSession& session,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymentSession reloaded;
    if (!store.GetSessionBySessionId(expected_session_id, reloaded)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (reloaded.request_id != expected_request_id ||
        reloaded.session_id != expected_session_id ||
        !reloaded.provider_side ||
        (!reloaded.final_txid.IsNull() &&
         reloaded.final_txid != commit.final_txid) ||
        std::find(reloaded.attempt_ids.begin(), reloaded.attempt_ids.end(),
                  expected_attempt_id) == reloaded.attempt_ids.end()) {
        error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return false;
    }
    session = std::move(reloaded);
    return true;
}

ProviderCommitRecoveryResult RecoverProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    ProviderCommitRecoveryResult result;
    ProviderCommitRecord persisted_commit;
    if (commit.commit_key.IsNull() ||
        !store.GetProviderCommit(commit.commit_key, persisted_commit) ||
        persisted_commit.version != commit.version ||
        persisted_commit.commit_key != commit.commit_key ||
        persisted_commit.provider_id != commit.provider_id ||
        persisted_commit.quote_id != commit.quote_id ||
        persisted_commit.template_commitment != commit.template_commitment ||
        persisted_commit.final_txid != commit.final_txid ||
        persisted_commit.raw_transaction_hash != commit.raw_transaction_hash ||
        persisted_commit.final_transaction != commit.final_transaction ||
        persisted_commit.provider_inputs != commit.provider_inputs ||
        persisted_commit.committed_at != commit.committed_at ||
        persisted_commit.retry_until != commit.retry_until) {
        result.error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return result;
    }
    ProviderAttempt attempt;
    if (!store.GetAttemptByTemplateCommitment(commit.template_commitment, attempt)) {
        result.error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return result;
    }
    PaymentSession session;
    if (!store.GetSessionBySessionId(attempt.session_id, session)) {
        result.error = "PAYMASTER_SESSION_NOT_FOUND";
        return result;
    }
    // The caller supplied the exact durable ProviderCommitRecord. The
    // broadcast helper reloads its current attempt and applies the complete
    // commit, manifest, ownership, and budget firewall.
    result.broadcast = InsertAndBroadcastProviderCommit(
        wallet, store, commit, now, result.already_confirmed, result.error);
    if (!result.broadcast) return result;

    const uint256 expected_attempt_id{attempt.attempt_id};
    const uint256 expected_session_id{attempt.session_id};
    if (!ReloadExactProviderCommitAttempt(
            store, commit, expected_attempt_id, expected_session_id, now,
            attempt, result.error)) {
        return result;
    }
    const auto advance_attempt = [&](AttemptState target) {
        if (IsProviderNetworkStateAtLeast(attempt.state, target)) return true;

        ProviderAttempt update{attempt};
        update.state = target;
        update.updated_at = std::max(update.updated_at, now);
        std::string update_error;
        if (store.UpdateAttempt(session.request_id, update, update_error)) {
            attempt = std::move(update);
            return true;
        }
        if (update_error != "PAYMASTER_INVALID_ATTEMPT_TRANSITION") {
            result.error = std::move(update_error);
            return false;
        }

        // Broadcast can synchronously notify the wallet. That callback may
        // atomically advance the durable attempt to STEMPOOL/MEMPOOL while
        // this RPC still holds its pre-callback copy. Treat the stale update
        // as idempotent only after reloading and revalidating the exact commit
        // binding, and only when the callback reached this target or a later
        // network-observation state.
        ProviderAttempt raced;
        std::string reload_error;
        if (!ReloadExactProviderCommitAttempt(
                store, commit, expected_attempt_id, expected_session_id, now,
                raced, reload_error)) {
            result.error = std::move(reload_error);
            return false;
        }
        if (!IsProviderNetworkStateAtLeast(raced.state, target)) {
            result.error = std::move(update_error);
            return false;
        }
        attempt = std::move(raced);
        return true;
    };
    if (!advance_attempt(AttemptState::BROADCAST)) return result;
    if (!result.already_confirmed &&
        !advance_attempt(gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) ? AttemptState::STEMPOOL : AttemptState::MEMPOOL)) {
        return result;
    }
    const SessionState session_state = result.already_confirmed ? SessionState::CONFIRMED : (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) ? SessionState::STEMPOOL : SessionState::MEMPOOL);
    if (!ReloadExactProviderCommitSession(
            store, commit, session.request_id, expected_session_id,
            expected_attempt_id, session, result.error)) {
        return result;
    }
    if (session.final_txid != commit.final_txid ||
        !IsProviderSessionNetworkStateAtLeast(session.state, session_state)) {
        std::string transition_error;
        if (!store.TransitionSession(
                session.request_id, session_state, PendingPhase::NONE,
                commit.final_txid, now, transition_error)) {
            if (transition_error != "PAYMASTER_INVALID_SESSION_TRANSITION") {
                result.error = std::move(transition_error);
                return result;
            }
            PaymentSession raced;
            std::string reload_error;
            if (!ReloadExactProviderCommitSession(
                    store, commit, session.request_id, expected_session_id,
                    expected_attempt_id, raced, reload_error)) {
                result.error = std::move(reload_error);
                return result;
            }
            if (raced.final_txid != commit.final_txid ||
                !IsProviderSessionNetworkStateAtLeast(
                    raced.state, session_state)) {
                result.error = std::move(transition_error);
                return result;
            }
            session = std::move(raced);
        }
    }
    if (!GetOrCreateFinalProviderResult(
            wallet, store, commit, now, result.provider_result, result.error)) {
        return result;
    }
    std::string result_error;
    if (!MarkProviderResultBroadcastAttempted(
            wallet, store, result.provider_result, now, result_error)) {
        result.error = result_error;
    }
    return result;
}

// -------------------------------------------------------------------------
// Strict policy parsing
// -------------------------------------------------------------------------
// Parsers reject unknown or nonsensical values rather than inheriting permissive
// defaults. In particular, zero safety budgets mean disabled, never unlimited.
UniValue ProviderPolicyToJSON(const DigiDollar::Paymaster::ProviderPolicy& policy)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    UniValue models{UniValue::VARR};
    if (PolicyAllowsFundingModel(policy, FundingModel::SPONSORED)) models.push_back("sponsored");
    if (PolicyAllowsFundingModel(policy, FundingModel::USER_PAID)) models.push_back("user_paid");
    result.pushKV("funding_models", std::move(models));
    result.pushKV("sponsorship_scope",
                  policy.sponsorship_scope == SponsorshipScope::PUBLIC ? "public" : "restricted");
    result.pushKV("fee_rate_bps", policy.fee_rate_bps);
    result.pushKV("min_amount_cents", policy.min_payment.value);
    result.pushKV("max_amount_cents", policy.max_payment.value);
    result.pushKV("quote_ttl", policy.quote_ttl);
    result.pushKV("maximum_network_fee_dgb_satoshis", policy.maximum_network_fee.value);
    result.pushKV("policy_hash", GetProviderPolicyHash(policy).GetHex());
    return result;
}

DigiDollar::Paymaster::ProviderPolicy ParseProviderPolicy(const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(value,
                    {{"funding_models", UniValueType(UniValue::VARR)},
                     {"sponsorship_scope", UniValueType(UniValue::VSTR)},
                     {"fee_rate_bps", UniValueType(UniValue::VNUM)},
                     {"min_amount_cents", UniValueType(UniValue::VNUM)},
                     {"max_amount_cents", UniValueType(UniValue::VNUM)},
                     {"quote_ttl", UniValueType(UniValue::VNUM)},
                     {"maximum_network_fee_dgb_satoshis", UniValueType(UniValue::VNUM)}},
                    /*fAllowNull=*/false, /*fStrict=*/true);
    ProviderPolicy policy;
    for (const UniValue& model : value.find_value("funding_models").get_array().getValues()) {
        if (!model.isStr()) throw JSONRPCError(RPC_INVALID_PARAMETER, "funding_models entries must be strings");
        uint8_t bit{0};
        if (model.get_str() == "sponsored")
            bit = FUNDING_MODEL_SPONSORED;
        else if (model.get_str() == "user_paid")
            bit = FUNDING_MODEL_USER_PAID;
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown Paymaster funding model");
        if ((policy.funding_models & bit) != 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Duplicate Paymaster funding model");
        }
        policy.funding_models |= bit;
    }
    const std::string scope = value.find_value("sponsorship_scope").get_str();
    if (scope == "public")
        policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    else if (scope == "restricted")
        policy.sponsorship_scope = SponsorshipScope::RESTRICTED;
    else
        throw JSONRPCError(RPC_INVALID_PARAMETER, "sponsorship_scope must be public or restricted");
    policy.fee_rate_bps = value.find_value("fee_rate_bps").getInt<uint32_t>();
    policy.min_payment = DDCents{value.find_value("min_amount_cents").getInt<int64_t>()};
    policy.max_payment = DDCents{value.find_value("max_amount_cents").getInt<int64_t>()};
    policy.quote_ttl = value.find_value("quote_ttl").getInt<int64_t>();
    policy.maximum_network_fee = DGBSatoshis{
        value.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>()};
    std::string error;
    if (!ValidateProviderPolicy(policy, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    return policy;
}

UniValue FundingSafetyLimitsToJSON(
    const DigiDollar::Paymaster::FundingSafetyLimits& limits)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_network_fee_per_transaction_satoshis",
                  limits.maximum_network_fee_per_transaction.value);
    result.pushKV("maximum_reserved_network_fee_satoshis",
                  limits.maximum_reserved_network_fee.value);
    result.pushKV("maximum_network_fee_per_hour_satoshis",
                  limits.maximum_network_fee_per_hour.value);
    result.pushKV("maximum_network_fee_per_day_satoshis",
                  limits.maximum_network_fee_per_day.value);
    result.pushKV("maximum_completed_per_hour", limits.maximum_completed_per_hour);
    result.pushKV("maximum_completed_per_day", limits.maximum_completed_per_day);
    return result;
}

DigiDollar::Paymaster::FundingSafetyLimits ParseFundingSafetyLimits(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"maximum_network_fee_per_transaction_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_reserved_network_fee_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_network_fee_per_hour_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_network_fee_per_day_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_completed_per_hour", UniValueType(UniValue::VNUM)},
         {"maximum_completed_per_day", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{
        value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>()};
    limits.maximum_reserved_network_fee = DGBSatoshis{
        value.find_value("maximum_reserved_network_fee_satoshis").getInt<int64_t>()};
    limits.maximum_network_fee_per_hour = DGBSatoshis{
        value.find_value("maximum_network_fee_per_hour_satoshis").getInt<int64_t>()};
    limits.maximum_network_fee_per_day = DGBSatoshis{
        value.find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>()};
    limits.maximum_completed_per_hour =
        value.find_value("maximum_completed_per_hour").getInt<uint32_t>();
    limits.maximum_completed_per_day =
        value.find_value("maximum_completed_per_day").getInt<uint32_t>();
    return limits;
}

UniValue ProviderSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("user_paid", FundingSafetyLimitsToJSON(policy.user_paid));
    result.pushKV("public_sponsored", FundingSafetyLimitsToJSON(policy.public_sponsored));
    result.pushKV("restricted_sponsored", FundingSafetyLimitsToJSON(policy.restricted_sponsored));
    result.pushKV("maximum_active_quotes_total", policy.maximum_active_quotes_total);
    result.pushKV("maximum_active_quotes_per_netgroup",
                  policy.maximum_active_quotes_per_netgroup);
    result.pushKV("maximum_active_quotes_per_recipient",
                  policy.maximum_active_quotes_per_recipient);
    result.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                  policy.maximum_quote_requests_per_netgroup_per_minute);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

DigiDollar::Paymaster::ProviderSafetyPolicy ParseProviderSafetyPolicy(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"user_paid", UniValueType(UniValue::VOBJ)},
         {"public_sponsored", UniValueType(UniValue::VOBJ)},
         {"restricted_sponsored", UniValueType(UniValue::VOBJ)},
         {"maximum_active_quotes_total", UniValueType(UniValue::VNUM)},
         {"maximum_active_quotes_per_netgroup", UniValueType(UniValue::VNUM)},
         {"maximum_active_quotes_per_recipient", UniValueType(UniValue::VNUM)},
         {"maximum_quote_requests_per_netgroup_per_minute", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    ProviderSafetyPolicy policy;
    policy.user_paid = ParseFundingSafetyLimits(value.find_value("user_paid"));
    policy.public_sponsored = ParseFundingSafetyLimits(value.find_value("public_sponsored"));
    policy.restricted_sponsored = ParseFundingSafetyLimits(value.find_value("restricted_sponsored"));
    policy.maximum_active_quotes_total =
        value.find_value("maximum_active_quotes_total").getInt<uint32_t>();
    policy.maximum_active_quotes_per_netgroup =
        value.find_value("maximum_active_quotes_per_netgroup").getInt<uint32_t>();
    policy.maximum_active_quotes_per_recipient =
        value.find_value("maximum_active_quotes_per_recipient").getInt<uint32_t>();
    policy.maximum_quote_requests_per_netgroup_per_minute =
        value.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<uint32_t>();
    return policy;
}

UniValue ProviderSafetyClassStatusToJSON(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
    DigiDollar::Paymaster::FundingModel model,
    DigiDollar::Paymaster::SponsorshipScope scope,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    CScript status_script;
    status_script << OP_TRUE;
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        ledger, policy, model, scope, GetRecipientBudgetBucket(ledger, status_script),
        DGBSatoshis{1}, now);
    UniValue result{UniValue::VOBJ};
    result.pushKV("reserved_network_fee_satoshis", status.reserved_network_fee.value);
    result.pushKV("spent_network_fee_last_hour_satoshis",
                  status.spent_network_fee_last_hour.value);
    result.pushKV("spent_network_fee_last_day_satoshis",
                  status.spent_network_fee_last_day.value);
    result.pushKV("active_quotes", status.active_quotes);
    result.pushKV("completed_last_hour", status.completed_last_hour);
    result.pushKV("completed_last_day", status.completed_last_day);
    result.pushKV("can_accept_minimum_quote", status.can_accept_quote);
    result.pushKV("errors", ReadinessErrorsToJSON(status.errors));
    return result;
}

UniValue ClientSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_service_fee_per_transaction_cents",
                  policy.maximum_service_fee_per_transaction.value);
    result.pushKV("maximum_service_fee_per_day_cents",
                  policy.maximum_service_fee_per_day.value);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

DigiDollar::Paymaster::ClientSafetyPolicy ParseClientSafetyPolicy(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"maximum_service_fee_per_transaction_cents", UniValueType(UniValue::VNUM)},
         {"maximum_service_fee_per_day_cents", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    ClientSafetyPolicy policy;
    policy.maximum_service_fee_per_transaction = DDCents{
        value.find_value("maximum_service_fee_per_transaction_cents").getInt<int64_t>()};
    policy.maximum_service_fee_per_day = DDCents{
        value.find_value("maximum_service_fee_per_day_cents").getInt<int64_t>()};
    return policy;
}

std::vector<RPCArg> FundingSafetyLimitArgs()
{
    return {
        {"maximum_network_fee_per_transaction_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Maximum miner fee for one transaction; zero only disables an unused model"},
        {"maximum_reserved_network_fee_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Maximum miner fee reserved by concurrent quotes"},
        {"maximum_network_fee_per_hour_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-hour miner-fee ceiling"},
        {"maximum_network_fee_per_day_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-day miner-fee ceiling"},
        {"maximum_completed_per_hour", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-hour transaction ceiling"},
        {"maximum_completed_per_day", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-day transaction ceiling"},
    };
}

std::vector<RPCResult> FundingSafetyLimitResults()
{
    return {
        {RPCResult::Type::NUM, "maximum_network_fee_per_transaction_satoshis", "Per-transaction miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_reserved_network_fee_satoshis", "Concurrent reserved miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_network_fee_per_hour_satoshis", "Rolling-hour miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_network_fee_per_day_satoshis", "Rolling-day miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_completed_per_hour", "Rolling-hour transaction ceiling"},
        {RPCResult::Type::NUM, "maximum_completed_per_day", "Rolling-day transaction ceiling"},
    };
}

std::vector<RPCResult> ProviderSafetyClassStatusResults()
{
    return {
        {RPCResult::Type::NUM, "reserved_network_fee_satoshis", "Miner fee held by active quotes"},
        {RPCResult::Type::NUM, "spent_network_fee_last_hour_satoshis", "Miner fee committed during the rolling hour"},
        {RPCResult::Type::NUM, "spent_network_fee_last_day_satoshis", "Miner fee committed during the rolling day"},
        {RPCResult::Type::NUM, "active_quotes", "All active provider quote reservations"},
        {RPCResult::Type::NUM, "completed_last_hour", "Completed transactions during the rolling hour"},
        {RPCResult::Type::NUM, "completed_last_day", "Completed transactions during the rolling day"},
        {RPCResult::Type::BOOL, "can_accept_minimum_quote", "Whether a one-satoshi hypothetical quote fits the limits"},
        {RPCResult::Type::ARR, "errors", "Safety failures", {{RPCResult::Type::STR, "", "Stable Paymaster safety error"}}},
    };
}

std::vector<RPCResult> ProviderSafetyPolicyResults()
{
    return {
        {RPCResult::Type::OBJ, "user_paid", "USER_PAID loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::OBJ, "public_sponsored", "Public SPONSORED loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::OBJ, "restricted_sponsored", "Restricted SPONSORED loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::NUM, "maximum_active_quotes_total", "Maximum concurrent active quotes"},
        {RPCResult::Type::NUM, "maximum_active_quotes_per_netgroup", "Maximum concurrent quotes per network group"},
        {RPCResult::Type::NUM, "maximum_active_quotes_per_recipient", "Maximum concurrent quotes per pseudonymous recipient bucket"},
        {RPCResult::Type::NUM, "maximum_quote_requests_per_netgroup_per_minute", "Quote request rate per network group"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
    };
}

std::vector<RPCResult> ProviderLiquidityPolicyResults()
{
    return {
        {RPCResult::Type::BOOL, "automatic_replenishment", "Whether automatic maintenance is enabled"},
        {RPCResult::Type::BOOL, "paid_maintenance_approved", "Whether finite paid maintenance was approved"},
        {RPCResult::Type::NUM, "target_admission_dgb", "Target number of admission DGB slots"},
        {RPCResult::Type::NUM, "target_operational_dgb", "Target number of operational DGB slots"},
        {RPCResult::Type::NUM, "target_admission_carriers", "Target number of admission DigiDollar carriers"},
        {RPCResult::Type::NUM, "target_operational_carriers", "Target number of operational DigiDollar carriers"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_transaction_satoshis", "Per-transaction maintenance-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_hour_satoshis", "Rolling-hour maintenance-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_day_satoshis", "Rolling-day maintenance-fee ceiling"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last liquidity-policy update"},
    };
}

std::vector<RPCResult> ProviderLiquiditySlotResults()
{
    return {
        {RPCResult::Type::NUM, "target", "Configured slot target"},
        {RPCResult::Type::NUM, "ready", "Confirmed available slots"},
        {RPCResult::Type::NUM, "pending", "Unconfirmed successor or maintenance slots"},
        {RPCResult::Type::NUM, "counted_toward_target", "Available, reserved, or pending slots counted toward the target"},
        {RPCResult::Type::NUM, "missing", "Slots still missing from the target"},
    };
}

std::vector<RPCResult> ProviderLiquidityStatusResults()
{
    return {
        {RPCResult::Type::BOOL, "policy_configured", "Whether the operator saved the policy"},
        {RPCResult::Type::BOOL, "targets_satisfy_provider_policy", "Whether the saved targets can satisfy the active provider offer"},
        {RPCResult::Type::STR, "maintenance_state", "Current automatic liquidity state"},
        {RPCResult::Type::OBJ, "policy", "Saved or suggested liquidity policy",
         ProviderLiquidityPolicyResults()},
        {RPCResult::Type::OBJ, "admission_dgb", "Admission DGB slot status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "operational_dgb", "Operational DGB slot status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "admission_carriers", "Admission DigiDollar carrier status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "operational_carriers", "Operational DigiDollar carrier status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::NUM, "maintenance_fee_reserved_satoshis", "Maintenance fee exposed by planned or broadcast transactions"},
        {RPCResult::Type::NUM, "maintenance_fee_spent_last_hour_satoshis", "Confirmed maintenance fees in the rolling hour"},
        {RPCResult::Type::NUM, "maintenance_fee_spent_last_day_satoshis", "Confirmed maintenance fees in the rolling day"},
        {RPCResult::Type::NUM, "carrier_base_cents", "DigiDollar base value retained in available carriers"},
        {RPCResult::Type::NUM, "carrier_withdrawable_excess_cents", "DigiDollar carrier value available for excess withdrawal"},
        {RPCResult::Type::ARR, "readiness_errors", "Outstanding liquidity readiness gates",
         {{RPCResult::Type::STR, "", "Stable Paymaster readiness error"}}},
    };
}

} // namespace

bool ReconcilePaymasterProviderMaintenance(CWallet& wallet,
                                           size_t& recovered,
                                           std::string& error)
{
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) return false;
    size_t finance_changes{0};
    return ReconcileProviderFinances(wallet, finance_changes, error);
}

bool ReconcilePaymasterProviderFinances(CWallet& wallet,
                                        size_t& changed_events,
                                        std::string& error)
{
    return ReconcileProviderFinances(wallet, changed_events, error);
}

bool ReconcilePaymasterClientHistory(CWallet& wallet, std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    DigiDollarWallet* const dd_wallet = wallet.GetDDWallet();
    if (!dd_wallet) return true;

    PaymasterStore store{wallet};
    std::vector<PaymentSession> sessions;
    if (!store.ListClientSessions(sessions, error)) return false;

    for (const PaymentSession& session : sessions) {
        // Only a transaction already accepted by the wallet/node is a
        // completed send. Signed, ambiguous, failed, or safely canceled
        // sessions must not acquire an outgoing history row merely because
        // they contain durable final bytes.
        if (session.state != SessionState::STEMPOOL &&
            session.state != SessionState::MEMPOOL &&
            session.state != SessionState::CONFIRMED) {
            continue;
        }
        if (session.final_txid.IsNull()) continue;

        bool found_final_attempt{false};
        for (auto it = session.attempt_ids.rbegin();
             it != session.attempt_ids.rend(); ++it) {
            ProviderAttempt attempt;
            if (!store.GetAttempt(*it, attempt) ||
                attempt.final_txid != session.final_txid) {
                continue;
            }

            PaymentIntent intent;
            PaymasterQuote quote;
            CollaborativePSBTTemplate trusted_template;
            std::string artifact_error;
            if (!LoadAttemptAuthorizationArtifacts(
                    attempt, intent, quote, trusted_template,
                    artifact_error)) {
                error = artifact_error.empty()
                    ? "PAYMASTER_CLIENT_HISTORY_ARTIFACTS_INVALID"
                    : artifact_error;
                return false;
            }
            if (intent.recipient_amount.value <= 0 ||
                quote.service_fee.value < 0 ||
                intent.recipient_amount.value >
                    std::numeric_limits<CAmount>::max() -
                        quote.service_fee.value) {
                error = "PAYMASTER_CLIENT_HISTORY_AMOUNT_INVALID";
                return false;
            }

            if (!dd_wallet->RecordPaymasterSendHistory(
                    session.final_txid, intent.recipient_script,
                    intent.recipient_amount.value + quote.service_fee.value,
                    error)) {
                return false;
            }
            found_final_attempt = true;
            break;
        }

        if (!found_final_attempt) {
            error = "PAYMASTER_CLIENT_HISTORY_ATTEMPT_NOT_FOUND";
            return false;
        }
    }
    return true;
}

bool EnsureProviderNotEquivocationBlocked(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    std::string& error)
{
    DigiDollar::Paymaster::PaymasterProviderBlock block;
    const DatabaseReadStatus status =
        store.GetProviderBlock(provider_id, block, error);
    if (status == DatabaseReadStatus::READ_ERROR) return false;
    if (status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_PROVIDER_EQUIVOCATION_BLOCKED";
        return false;
    }

    DigiDollar::Paymaster::PaymasterEquivocationEvidence pending;
    const DatabaseReadStatus pending_status =
        store.GetPendingEquivocation(provider_id, pending, error);
    if (pending_status == DatabaseReadStatus::READ_ERROR) return false;
    if (pending_status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_PROVIDER_EQUIVOCATION_PENDING";
        return false;
    }
    return true;
}

bool DrainPaymasterEquivocationInbox(WalletContext& context,
                                     CWallet& wallet,
                                     std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    PaymasterStore store{wallet};
    if (!store.PromotePendingEquivocations(error)) return false;
    if (!context.paymaster) return true;
    if (!context.paymaster->HasEquivocationCandidates()) {
        return true;
    }

    std::vector<PaymentSession> sessions;
    if (!store.ListClientSessions(sessions, error)) return false;
    std::string first_error;
    const auto remember_error = [&first_error](const std::string& candidate) {
        if (first_error.empty() && !candidate.empty()) {
            first_error = candidate;
        }
    };
    for (const PaymentSession& session : sessions) {
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!store.GetAttempt(attempt_id, attempt)) {
                remember_error("PAYMASTER_ATTEMPT_NOT_FOUND");
                continue;
            }
            std::string attempt_error;
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt, attempt_error,
                    EquivocationBlockPolicy::OBSERVE_ONLY,
                    /*lease_messages=*/false)) {
                remember_error(attempt_error);
            }
        }
    }

    std::vector<AlternativeRecoveryRecord> recoveries;
    if (!store.ListClientAlternativeRecoveries(recoveries, error)) {
        return false;
    }
    for (const AlternativeRecoveryRecord& recovery : recoveries) {
        std::string recovery_error;
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, recovery_error,
                EquivocationBlockPolicy::OBSERVE_ONLY,
                /*lease_messages=*/false)) {
            remember_error(recovery_error);
        }
    }
    if (!first_error.empty()) {
        error = std::move(first_error);
        return false;
    }
    error.clear();
    return true;
}

RPCHelpMan getdigidollarsendsession()
{
    return RPCHelpMan{
        "getdigidollarsendsession",
        "Return one persistent DigiDollar send session without creating a new payment attempt.\n",
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent 256-bit session identifier"},
                                                                                                             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The authoritative persistent session", {
                                                                                         {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                         {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                         {RPCResult::Type::STR_HEX, "canonical_request_hash", "Hash of the canonical authorized request"},
                                                                                         {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount; exact total outflow in subtract mode"},
                                                                                         {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the service fee is deducted from requested_amount_cents"},
                                                                                         {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the session is bound to all ordinary spendable DD"},
                                                                                         {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
                                                                                         {RPCResult::Type::STR, "fee_mode_used", "Persisted effective fee mode"},
                                                                                         {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider bound by the latest durable client authorization"},
                                                                                         {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Offer bound by the latest durable client authorization"},
                                                                                         {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Provider policy bound by the latest durable client authorization"},
                                                                                         {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
                                                                                         {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Exact recipient amount from the latest durable client authorization"},
                                                                                         {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact rounded provider service fee"},
                                                                                         {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Exact recipient amount plus service fee"},
                                                                                         {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                        {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Persisted phase for PENDING_PROVIDER"},
                                                                                        {RPCResult::Type::BOOL, "final", "Whether the state is terminal"},
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
                                                                                    }},
        RPCExamples{HelpExampleCli("getdigidollarsendsession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            return SessionToJSON(session, &store);
        },
    };
}

UniValue ResolveAlternativePaymasterRecovery(
    WalletContext& context,
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& original_attempt,
    const UniValue& options)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    const int64_t now{GetTime()};
    if (!CheckPaymasterClientReadiness(wallet, context, error) ||
        !CheckPaymasterPrivacyReadiness(
            original_attempt.privacy_profile, context, error)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman || !node->chainman || !context.paymaster) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
    }

    const UniValue& provider_value =
        options.find_value("recovery_provider_id");
    const UniValue& offer_value = options.find_value("recovery_offer_id");
    const std::optional<PaymasterId> requested_provider =
        provider_value.isNull() ? std::nullopt : std::optional<PaymasterId>{ParseHashO(options, "recovery_provider_id")};
    const std::optional<uint256> requested_offer =
        offer_value.isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "recovery_offer_id")};
    const bool prepare_only =
        !options.find_value("prepare_only").isNull() &&
        options.find_value("prepare_only").get_bool();
    const UniValue& commitment_value =
        options.find_value("recovery_authorization_commitment");
    const std::optional<uint256> accepted_commitment =
        commitment_value.isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "recovery_authorization_commitment")};

    AlternativeRecoveryRecord recovery;
    if (!store.GetAlternativeRecovery(session.request_id, recovery)) {
        if (session.state != SessionState::PENDING_PROVIDER ||
            original_attempt.provider_id.IsNull() ||
            original_attempt.commit_key.IsNull() ||
            original_attempt.template_commitment.IsNull() ||
            !((original_attempt.state >= AttemptState::USER_SIGNED &&
               original_attempt.state <= AttemptState::MEMPOOL) ||
              original_attempt.state == AttemptState::AMBIGUOUS)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_ALLOWED");
        }
        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        if (!dd_wallet) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "DigiDollar wallet not initialized");
        }
        CAmount total_dd{0};
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        const UniValue& maximum_value =
            options.find_value("maximum_recovery_service_fee_cents");
        const DDCents requested_maximum{
            maximum_value.isNull() ? MAX_DD_OUTPUT_CENTS : maximum_value.getInt<int64_t>()};
        const DDCents effective_maximum =
            EffectiveClientServiceFeeCap(wallet, requested_maximum, error);
        if (effective_maximum.value < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
        }

        std::vector<PaymasterReliabilityRecord> reliability_records;
        if (!store.ListProviderReliability(reliability_records)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_REPUTATION_DATABASE_READ");
        }
        std::map<PaymasterId, PaymasterReliabilityRecord> reliability;
        for (auto& record : reliability_records) {
            reliability.emplace(record.provider_id, std::move(record));
        }
        const auto candidates = BuildOfferCandidates(
            context.paymaster->GetDirectory().List(now), DDCents{total_dd},
            FUNDING_MODEL_USER_PAID, effective_maximum, reliability, now,
            error);
        if (!error.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
        }
        const auto selected = std::find_if(
            candidates.begin(), candidates.end(),
            [&](const OfferCandidate& candidate) {
                return candidate.provider_id !=
                           original_attempt.provider_id &&
                       candidate.terms.funding_model ==
                           FundingModel::USER_PAID &&
                       (!requested_provider ||
                        candidate.provider_id == *requested_provider) &&
                       (!requested_offer ||
                        candidate.terms.offer_id == *requested_offer) &&
                       (original_attempt.privacy_profile !=
                            PrivacyProfile::HIGH ||
                        candidate.endpoint.IsTor()) &&
                       total_dd - candidate.service_fee.value >= 100;
            });
        if (selected == candidates.end()) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_NO_ELIGIBLE_RECOVERY_PROVIDER");
        }

        do {
            recovery.client_nonce = GetRandHash();
        } while (recovery.client_nonce.IsNull());
        recovery.provider_side = false;
        recovery.request_id = session.request_id;
        recovery.session_id = session.session_id;
        recovery.original_provider_id = original_attempt.provider_id;
        recovery.recovery_provider_id = selected->provider_id;
        recovery.privacy_profile = original_attempt.privacy_profile;
        recovery.offer_id = selected->terms.offer_id;
        recovery.policy_hash = selected->terms.policy_hash;
        recovery.recovery_provider_identity_key = selected->identity_key;
        recovery.recovery_provider_endpoint =
            selected->endpoint.ToStringAddrPort();
        recovery.original_commit_key = original_attempt.commit_key;
        recovery.original_template_commitment =
            original_attempt.template_commitment;
        recovery.selected_maximum_service_fee = effective_maximum;
        recovery.selected_service_fee = selected->service_fee;
        recovery.recovery_id = GetAlternativeRecoveryId(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, recovery.client_nonce);
        recovery.capacity_request.genesis_hash =
            Params().GenesisBlock().GetHash();
        recovery.capacity_request.provider_id =
            recovery.recovery_provider_id;
        recovery.capacity_request.request_id = recovery.request_id;
        recovery.capacity_request.session_id = recovery.session_id;
        recovery.capacity_request.client_nonce = recovery.client_nonce;
        recovery.capacity_request.funding_model = FundingModel::USER_PAID;
        recovery.capacity_request.requires_carrier =
            recovery.selected_service_fee.value > 0 &&
            recovery.selected_service_fee.value < 100;
        recovery.capacity_request.requested_slots = 1;
        recovery.capacity_request.created_at = now;
        recovery.capacity_request.expires_at = std::min(
            selected->announcement_expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        recovery.phase = AlternativeRecoveryPhase::CAPACITY_PENDING;
        recovery.created_at = recovery.updated_at = now;
        if (recovery.capacity_request.expires_at <= now ||
            !store.PrepareAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_CAPACITY_REQUEST_EXPIRED" : error);
        }
    } else {
        if (recovery.expired || recovery.provider_side ||
            recovery.original_provider_id != original_attempt.provider_id ||
            recovery.original_commit_key != original_attempt.commit_key ||
            recovery.original_template_commitment !=
                original_attempt.template_commitment ||
            (requested_provider &&
             recovery.recovery_provider_id != *requested_provider) ||
            (requested_offer && recovery.offer_id != *requested_offer)) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
        }
        const UniValue& maximum_value =
            options.find_value("maximum_recovery_service_fee_cents");
        const bool durable_authorization =
            !recovery.accepted_recovery_authorization_commitment.IsNull() &&
            recovery.accepted_recovery_authorization_commitment ==
                recovery.recovery_authorization.authorization_commitment &&
            recovery.recovery_authorization_accepted_at > 0;
        const bool inconsistent_authorization =
            recovery.accepted_recovery_authorization_commitment.IsNull() !=
                (recovery.recovery_authorization_accepted_at == 0) ||
            (!recovery.accepted_recovery_authorization_commitment.IsNull() &&
             recovery.accepted_recovery_authorization_commitment !=
                 recovery.recovery_authorization.authorization_commitment);
        if (inconsistent_authorization) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_RECOVERY_AUTHORIZATION_CONFLICT");
        }
        if (!maximum_value.isNull() && !durable_authorization) {
            const DDCents effective = EffectiveClientServiceFeeCap(
                wallet, DDCents{maximum_value.getInt<int64_t>()}, error);
            if (effective.value < 0 ||
                !(effective == recovery.selected_maximum_service_fee)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_RECOVERY_FEE_LIMIT_CHANGED");
            }
        }
    }

    const EquivocationBlockPolicy recovery_block_policy =
        static_cast<uint8_t>(recovery.phase) <
                static_cast<uint8_t>(AlternativeRecoveryPhase::USER_SIGNED) ?
            EquivocationBlockPolicy::ENFORCE :
            EquivocationBlockPolicy::OBSERVE_ONLY;
    if (!DrainAlternativeRecoveryCapacityEquivocations(
            *context.paymaster, store, recovery, error,
            recovery_block_policy)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }

    const auto make_result = [&](const AlternativeRecoveryRecord& record,
                                 const RecoveryQueueState& queue,
                                 std::optional<bool> broadcast = std::nullopt,
                                 const std::string& broadcast_error = {}) {
        PaymentSession current_session;
        if (!store.GetSessionByRequestId(record.request_id,
                                         current_session)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_SESSION_NOT_FOUND");
        }
        UniValue result{UniValue::VOBJ};
        result.pushKV("action", "cancel_to_self");
        result.pushKV("session", SessionToJSON(current_session));
        result.pushKV("attempt", AttemptToJSON(original_attempt));
        result.pushKV("artifact", "alternative_recovery");
        result.pushKV("recovery", AlternativeRecoveryToJSON(record));
        result.pushKV("queued", queue.queued);
        result.pushKV("connection_pending", queue.connection_pending);
        result.pushKV("route_available", queue.route_available);
        if (broadcast) result.pushKV("broadcast", *broadcast);
        if (!broadcast_error.empty()) {
            result.pushKV("broadcast_error", broadcast_error);
        }
        return result;
    };
    const auto fail_after_recovery_store =
        [&](const AlternativeRecoveryRecord& durable_recovery,
            const CTransaction& exact_final, int rpc_code,
            const std::string& failure) -> void {
        std::string persistence_error;
        if (!store.RecordClientAlternativeRecoveryConflict(
                durable_recovery.request_id, durable_recovery.recovery_id,
                exact_final.GetHash(), exact_final.GetWitnessHash(), now,
                persistence_error)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                persistence_error.empty() ? "PAYMASTER_RECOVERY_FINAL_CONFLICT_NOT_PERSISTED" : persistence_error);
        }
        throw JSONRPCError(
            rpc_code,
            failure.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : failure);
    };

    if (recovery.phase == AlternativeRecoveryPhase::CAPACITY_PENDING) {
        auto proof_lease = context.paymaster->LeaseCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
        const auto proofs = SelectCapacityProofMessages(
            proof_lease.Messages(), recovery.recovery_provider_id,
            recovery.capacity_request.request_id,
            recovery.capacity_request.session_id, recovery.client_nonce);
        if (proofs.empty()) {
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.capacity_request, now,
                    queue, error)) {
                throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
            }
            return make_result(recovery, queue);
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(
                    *context.paymaster, proofs, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_CAPACITY_EQUIVOCATION");
        }
        std::optional<PaymasterCapacityProof> accepted_proof;
        std::string rejected_proof_error;
        for (const DirectMessage& direct : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&direct.payload);
            if (!proof) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error = "PAYMASTER_INVALID_CAPACITY_PROOF";
                continue;
            }

            bool claim_equivocation{false};
            const std::vector<unsigned char> encoded_proof =
                SerializeCapacityProof(*proof);
            if (!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
                    recovery.recovery_id, encoded_proof, direct.received_at,
                    claim_equivocation, error)) {
                if (error != "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error =
                    "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(
                        *context.paymaster, proofs, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_CAPACITY_EQUIVOCATION");
            }
            if (!store.GetAlternativeRecoveryById(recovery.recovery_id,
                                                  recovery)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND");
            }

            std::string proof_error;
            if (!ValidateCapacityProofAgainstChainstate(
                    *proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key, *node->chainman,
                    now, proof_error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error =
                    proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
                continue;
            }
            accepted_proof = *proof;
            break;
        }
        if (!accepted_proof) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                rejected_proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : rejected_proof_error);
        }
        const PaymasterCapacityProof& proof = *accepted_proof;
        const std::vector<unsigned char> first_capacity_proof =
            SerializeCapacityProof(proof);
        recovery.capacity_snapshot.snapshot_id = proof.snapshot_id;
        recovery.capacity_snapshot.resource_commitment =
            GetCapacityResourceCommitment(proof);
        recovery.capacity_snapshot.session_id = recovery.session_id;
        recovery.capacity_snapshot.attempt_id = recovery.recovery_id;
        recovery.capacity_snapshot.provider_id = proof.provider_id;
        recovery.capacity_snapshot.client_nonce = proof.client_nonce;
        recovery.capacity_snapshot.funding_model = proof.funding_model;
        recovery.capacity_snapshot.requires_carrier =
            proof.requires_carrier;
        recovery.capacity_snapshot.request_hash =
            Hash(SerializeCapacityRequest(recovery.capacity_request));
        recovery.capacity_snapshot.capacity_proof = first_capacity_proof;
        recovery.capacity_snapshot.created_at = proof.created_at;
        recovery.capacity_snapshot.expires_at = proof.expires_at;
        recovery.capacity_snapshot.validated_at = now;

        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        CAmount total_dd{0};
        if (!dd_wallet) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "DigiDollar wallet not initialized");
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        CAmount remaining = total_dd - recovery.selected_service_fee.value;
        if (remaining < 100) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_RECOVERY_RETURN_TOO_SMALL");
        }
        std::vector<AlternativeRecoveryReturn> returns;
        while (remaining > 0) {
            CAmount chunk = std::min<CAmount>(remaining,
                                              MAX_DD_OUTPUT_CENTS);
            if (remaining > MAX_DD_OUTPUT_CENTS &&
                remaining - chunk < 100) {
                chunk = remaining - 100;
            }
            const auto destination = wallet.GetNewDestination(
                OutputType::BECH32M, "Paymaster alternative recovery");
            if (!destination || chunk < 100 ||
                chunk > MAX_DD_OUTPUT_CENTS) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RECOVERY_DESTINATION_UNAVAILABLE");
            }
            returns.push_back(
                {GetScriptForDestination(*destination), DDCents{chunk}});
            remaining -= chunk;
        }
        if (returns.size() > MAX_PAYMENT_INTENT_INPUTS) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_RECOVERY_TOO_MANY_RETURNS");
        }

        AlternativeRecoveryRequest recovery_request;
        recovery_request.genesis_hash =
            Params().GenesisBlock().GetHash();
        recovery_request.request_id = recovery.request_id;
        recovery_request.session_id = recovery.session_id;
        recovery_request.original_provider_id =
            recovery.original_provider_id;
        recovery_request.recovery_provider_id =
            recovery.recovery_provider_id;
        recovery_request.privacy_profile = recovery.privacy_profile;
        recovery_request.offer_id = recovery.offer_id;
        recovery_request.policy_hash = recovery.policy_hash;
        recovery_request.original_commit_key =
            recovery.original_commit_key;
        recovery_request.original_template_commitment =
            recovery.original_template_commitment;
        recovery_request.client_nonce = recovery.client_nonce;
        recovery_request.capacity_request = recovery.capacity_request;
        recovery_request.capacity_snapshot_id = proof.snapshot_id;
        recovery_request.capacity_resource_commitment =
            recovery.capacity_snapshot.resource_commitment;
        recovery_request.user_dd_inputs = session.user_inputs;
        recovery_request.wallet_returns = std::move(returns);
        recovery_request.maximum_service_fee =
            recovery.selected_maximum_service_fee;
        recovery_request.service_fee = recovery.selected_service_fee;
        recovery_request.created_at = now;
        recovery_request.expires_at = std::min(
            proof.expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        // Recheck the capacity proof at the last possible point before the
        // first payment-revealing signatures are produced.
        if (wallet.IsLocked()) {
            throw JSONRPCError(
                RPC_WALLET_UNLOCK_NEEDED,
                "Wallet unlock is required for alternative recovery");
        }
        if (!DrainPendingAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, first_capacity_proof,
                error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        std::vector<XOnlyPubKey> output_keys;
        if (!ValidateCapacityProofAgainstChainstate(
                proof, recovery.capacity_request,
                recovery.recovery_provider_identity_key,
                *node->chainman, now, error) ||
            !SignAlternativeRecoveryRequestInputs(
                wallet, recovery_request, now, output_keys, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        recovery.recovery_request = std::move(recovery_request);
        recovery.recovery_request_hash =
            GetAlternativeRecoveryRequestHash(recovery.recovery_request);
        recovery.phase = AlternativeRecoveryPhase::REQUEST_READY;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::REQUEST_READY) {
        const auto responses = context.paymaster->TakeRecoveryResponses(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        if (responses.empty()) {
            PaymasterCapacityProof proof;
            if (!DeserializeCanonicalRecoveryMessage(
                    recovery.capacity_snapshot.capacity_proof, proof, error) ||
                !ValidateCapacityProofAgainstChainstate(
                    proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key,
                    *node->chainman, now, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.recovery_request, now,
                    queue, error)) {
                throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
            }
            return make_result(recovery, queue);
        }
        const auto* response =
            std::get_if<AlternativeRecoveryResponse>(
                &responses.front().payload);
        AlternativeRecoveryRecord candidate{recovery};
        if (!response ||
            !ValidateAlternativeRecoveryResponse(
                *response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, now, error)) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESPONSE" : error);
        }
        candidate.recovery_response = *response;
        candidate.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
        candidate.updated_at = std::max(candidate.updated_at, now);
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, candidate, parameters, error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                *response, candidate.recovery_request,
                candidate.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error) ||
            !BuildRecoveryAuthorizationManifest(
                candidate.recovery_request, *response,
                candidate.recovery_authorization, error) ||
            !store.UpdateAlternativeRecovery(candidate, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response, now,
                error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error)) {
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
        }
        if (prepare_only) {
            return make_result(recovery, RecoveryQueueState{});
        }
        if (!accepted_commitment) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_REQUIRED");
        }
        if (*accepted_commitment !=
            recovery.recovery_authorization.authorization_commitment) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH");
        }
        if (wallet.IsLocked()) {
            throw JSONRPCError(
                RPC_WALLET_UNLOCK_NEEDED,
                "Wallet unlock is required for alternative recovery");
        }
        // Persist the exact user authorization and reserve its client fee
        // before invoking any wallet signer. Reload and revalidate the
        // immutable recovery template after that transaction commits.
        if (!store.AcceptAlternativeRecoveryAuthorization(
                session.request_id, *accepted_commitment, now, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery) ||
            !BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response, now,
                error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_RECOVERY_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!SignCollaborativePSBTForParty(
                wallet, unsigned_psbt, trusted.trusted_template,
                SigningParty::USER, error) ||
            !ValidateAlternativeRecoveryPSBTAgainstChainstate(
                unsigned_psbt, trusted, parameters, Params(),
                *node->chainman, now,
                CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        recovery.user_signed_psbt = SerializePSBT(unsigned_psbt);
        recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::USER_SIGNED) {
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        if (authorization_time <= 0) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_RECOVERY_AUTHORIZATION_REQUIRED");
        }
        const int64_t observation_time{
            std::max(now, authorization_time)};
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        AlternativeRecoverySubmit submit;
        submit.genesis_hash = Params().GenesisBlock().GetHash();
        submit.request_id = recovery.request_id;
        submit.session_id = recovery.session_id;
        submit.recovery_id = recovery.recovery_id;
        submit.recovery_provider_id = recovery.recovery_provider_id;
        submit.recovery_request_hash = recovery.recovery_request_hash;
        submit.recovery_commit_key =
            recovery.recovery_response.recovery_commit_key;
        submit.template_commitment =
            recovery.recovery_response.manifest.template_commitment;
        submit.user_psbt = recovery.user_signed_psbt;
        const auto messages = context.paymaster->TakeRecoveryResults(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        const AlternativeRecoveryResultMessage* message{nullptr};
        CTransactionRef observed_final;
        FinalTransactionPresence observed_presence{
            FinalTransactionPresence::NONE};
        if (!messages.empty()) {
            message = std::get_if<AlternativeRecoveryResultMessage>(
                &messages.front().payload);
            if (!message || !ValidateAlternativeRecoveryResult(*message, recovery.recovery_response, recovery.recovery_provider_identity_key, Params().GenesisBlock().GetHash(), 1, now, error) ||
                !message->result.final_transaction ||
                !message->result.raw_transaction_hash) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESULT" : error);
            }
            observed_final = MakeTransactionRef(
                *message->result.final_transaction);
            if (!PreflightFinalPaymasterTransaction(
                    wallet, observed_final, observed_presence, error)) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
            }
        }
        const AuthorizedCapacityResourceMode resource_mode{
            observed_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT};
        const CTransaction* const known_final{
            observed_presence != FinalTransactionPresence::NONE ? observed_final.get() : nullptr};
        PartiallySignedTransaction user_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                resource_mode, trusted, unsigned_psbt, error, known_final) ||
            !ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
                submit, recovery.recovery_response, trusted, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                resource_mode, user_psbt, error, known_final)) {
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
        }

        if (message) {
            const CMutableTransaction& final_transaction =
                *message->result.final_transaction;
            const CTransactionRef& final_ref = observed_final;
            const FinalTransactionPresence presence{observed_presence};
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, *message->result.raw_transaction_hash,
                    trusted, parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    resource_mode, error)) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
            }
            recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
            recovery.final_transaction =
                SerializeTransaction(final_transaction);
            recovery.expected_wtxid =
                CTransaction{final_transaction}.GetWitnessHash();
            recovery.signed_result = message->result;
            recovery.updated_at = std::max(recovery.updated_at, now);
            SelfRecoveryRecord final_recovery;
            final_recovery.request_id = recovery.request_id;
            final_recovery.session_id = recovery.session_id;
            final_recovery.user_inputs =
                recovery.recovery_request.user_dd_inputs;
            final_recovery.recovery_txid =
                CTransaction{final_transaction}.GetHash();
            final_recovery.raw_transaction_hash =
                Hash(recovery.final_transaction);
            final_recovery.final_transaction = recovery.final_transaction;
            final_recovery.created_at = now;
            if (!store.CommitClientAlternativeRecoveryFinal(
                    recovery, final_recovery, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }

            // The final commit above performs database work and spends the
            // client fee reservation. Rebuild every authority from the exact
            // durable bytes and repeat the chainstate/mempool firewall before
            // handing the transaction to the node.
            AlternativeRecoveryRecord broadcast_recovery;
            AlternativeRecoveryParameters broadcast_parameters;
            AlternativeRecoveryTemplate broadcast_trusted;
            PartiallySignedTransaction broadcast_unsigned_psbt;
            FinalTransactionPresence broadcast_presence{
                FinalTransactionPresence::NONE};
            if (!store.GetAlternativeRecovery(
                    recovery.request_id, broadcast_recovery)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RECOVERY_FINAL_STATE_RELOAD_FAILED");
            }
            if (broadcast_recovery.phase !=
                    AlternativeRecoveryPhase::FINAL_COMMITTED ||
                broadcast_recovery.recovery_id != recovery.recovery_id ||
                broadcast_recovery.final_transaction !=
                    SerializeTransaction(final_transaction) ||
                broadcast_recovery.expected_wtxid !=
                    CTransaction{final_transaction}.GetWitnessHash()) {
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID");
            }
            if (!PreflightFinalPaymasterTransaction(
                    wallet, final_ref, broadcast_presence, error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED, error);
            }
            if (!BuildRecoveryParametersFromRecord(
                    wallet, broadcast_recovery, broadcast_parameters, error) ||
                !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                    broadcast_recovery.recovery_response,
                    broadcast_recovery.recovery_request,
                    broadcast_recovery.recovery_provider_identity_key,
                    broadcast_parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    broadcast_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                    broadcast_trusted,
                    broadcast_unsigned_psbt, error,
                    broadcast_presence != FinalTransactionPresence::NONE ? final_ref.get() : nullptr)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    error.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, broadcast_recovery.expected_wtxid,
                    broadcast_trusted, broadcast_parameters, Params(),
                    *node->chainman, authorization_time, observation_time,
                    broadcast_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                    error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    error.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            recovery = std::move(broadcast_recovery);
            bool already_confirmed{false};
            std::string broadcast_error;
            const bool broadcast = InsertAndBroadcastPaymasterTransaction(
                wallet, final_ref, already_confirmed, broadcast_error);
            if (!broadcast) {
                if (IsTransientPaymasterFinalizationError(broadcast_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
            }
            if (broadcast && !store.ReconcileFinalTransaction(
                                 *final_ref, already_confirmed ? 1 : 0,
                                 !already_confirmed, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return make_result(recovery, RecoveryQueueState{}, broadcast,
                               broadcast_error);
        }
        RecoveryQueueState queue;
        if (!QueueRecoveryMessage(context, wallet, recovery, submit, now,
                                  queue, error)) {
            throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
        }
        return make_result(recovery, queue);
    }

    if (recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED) {
        CMutableTransaction final_transaction;
        try {
            SpanReader stream{::PROTOCOL_VERSION,
                              recovery.final_transaction};
            stream >> final_transaction;
            if (!stream.empty()) {
                throw std::ios_base::failure("trailing recovery transaction");
            }
        } catch (const std::ios_base::failure&) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_PERSISTED_RECOVERY_CORRUPT");
        }
        const CTransactionRef final_ref =
            MakeTransactionRef(final_transaction);
        FinalTransactionPresence presence{FinalTransactionPresence::NONE};
        if (!PreflightFinalPaymasterTransaction(
                wallet, final_ref, presence, error)) {
            if (IsTransientPaymasterFinalizationError(error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED, error);
        }
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        const int64_t observation_time{
            std::max(now, authorization_time)};
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            authorization_time <= 0 ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                trusted,
                unsigned_psbt, error,
                presence != FinalTransactionPresence::NONE ? final_ref.get() : nullptr) ||
            !ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                final_transaction, recovery.expected_wtxid, trusted,
                parameters, Params(), *node->chainman,
                authorization_time, observation_time,
                presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                error)) {
            if (IsTransientPaymasterFinalizationError(error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED, error);
        }
        bool already_confirmed{false};
        std::string broadcast_error;
        const bool broadcast = InsertAndBroadcastPaymasterTransaction(
            wallet, final_ref, already_confirmed, broadcast_error);
        if (!broadcast) {
            if (IsTransientPaymasterFinalizationError(broadcast_error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED,
                broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
        }
        if (broadcast && !store.ReconcileFinalTransaction(
                             *final_ref, already_confirmed ? 1 : 0,
                             !already_confirmed, now, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        return make_result(recovery, RecoveryQueueState{}, broadcast,
                           broadcast_error);
    }
    throw JSONRPCError(RPC_WALLET_ERROR,
                       "PAYMASTER_ALTERNATIVE_RECOVERY_STATE_INVALID");
}

RPCHelpMan resolvepaymastersession()
{
    return RPCHelpMan{
        "resolvepaymastersession",
        "Inspect or recover an existing Paymaster session.\n"
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
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Exact recovery selection and authorization", {
                                                                                                                        {"attempt_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Existing original attempt identifier"},
                                                                                                                        {"recovery_provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact distinct recovery provider"},
                                                                                                                        {"recovery_offer_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact USER_PAID recovery offer"},
                                                                                                                        {"maximum_recovery_service_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Local recovery service-fee ceiling"},
                                                                                                                        {"prepare_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stop after validating and persisting the exact authorization commitment"},
                                                                                                                        {"recovery_authorization_commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Exact commitment required before signing user inputs"},
                                                                                                                    }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative local recovery state", {
                                                                                      {RPCResult::Type::STR, "action", "Performed local action"},
                                                                                      {RPCResult::Type::OBJ, "session", /*optional=*/false, "Persistent session", {
                                                                                                                                                                      {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                                                                                                      {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                                                                                                       {RPCResult::Type::STR_HEX, "canonical_request_hash", "Canonical request hash"},
                                                                                                                                                                       {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
                                                                                                                                                                       {RPCResult::Type::STR, "fee_mode_used", "Effective fee mode"},
                                                                                                                                                                       {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original recipient amount or exact total DD outflow"},
                                                                                                                                                                       {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the Paymaster fee is deducted from the requested amount"},
                                                                                                                                                                       {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the requested amount was bound to all spendable confirmed DD"},
                                                                                                                                                                       {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                                                                                                      {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Pending provider phase"},
                                                                                                                                                                      {RPCResult::Type::BOOL, "final", "Whether the session is terminal"},
                                                                                                                                                                      {RPCResult::Type::STR, "broadcast_state", "Authoritative broadcast state"},
                                                                                                                                                                      {RPCResult::Type::STR, "confirmation_state", "Authoritative confirmation state"},
                                                                                                                                                                      {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
                                                                                                                                                                      {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
                                                                                                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known payment transaction"},
                                                                                                                                                                      {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known self-recovery transaction"},
                                                                                                                                                                      {RPCResult::Type::ARR, "reserved_user_inputs", "Immutable user input set", {
                                                                                                                                                                                                                                                     {RPCResult::Type::OBJ, "", "A reserved user input", {
                                                                                                                                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "txid", "Creating transaction id"},
                                                                                                                                                                                                                                                                                                             {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                                                                         }},
                                                                                                                                                                                                                                                 }},
                                                                                                                                                                      {RPCResult::Type::NUM, "provider_attempts", "Persistent provider attempt count"},
                                                                                                                                                                  }},
                                                                                      {RPCResult::Type::OBJ, "attempt", /*optional=*/true, "Selected persistent attempt", {
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "attempt_id", "Persistent attempt identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "provider_id", "Provider identifier"},
                                                                                                                                                                              {RPCResult::Type::STR, "provider_endpoint", /*optional=*/true, "Direct provider endpoint"},
                                                                                                                                                                              {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Provider quote identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Bound transaction template"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "unsigned_txid", /*optional=*/true, "Unsigned transaction identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Provider commit key"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "quote_expires_at", /*optional=*/true, "Quote expiration time"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "retry_until", /*optional=*/true, "Exact retry deadline"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact client authorization"},
                                                                                                                                                                              {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                          }},
                                                                                      {RPCResult::Type::STR, "artifact", /*optional=*/true, "none, user_psbt, final_transaction, or alternative_recovery"},
                                                                                      {RPCResult::Type::OBJ, "recovery", /*optional=*/true, "Persistent alternative-recovery authority and state", {
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
                                                                                                                                                                                                   }},
                                                                                      {RPCResult::Type::STR, "psbt", /*optional=*/true, "Exact persisted user-signed PSBT"},
                                                                                      {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact durably committed transaction"},
                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Committed transaction id"},
                                                                                      {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Idempotent self-recovery transaction id"},
                                                                                      {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether local broadcast succeeded"},
                                                                                      {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Local broadcast error; exact retry remains available"},
                                                                                      {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the exact persisted submit is queued"},
                                                                                      {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a dedicated provider reconnection was requested"},
                                                                                      {RPCResult::Type::BOOL, "route_available", /*optional=*/true, "Whether the persisted provider route is available"},
                                                                                      {RPCResult::Type::STR, "result_status", /*optional=*/true, "Latest signed provider result"},
                                                                                      {RPCResult::Type::NUM, "result_sequence", /*optional=*/true, "Latest monotonic result sequence"},
                                                                                  }},
        RPCExamples{HelpExampleCli("resolvepaymastersession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}' retry_same")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
                             {"recovery_authorization_commitment", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);

            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            if (context.paymaster && context.paymaster->Enabled()) {
                std::string equivocation_error;
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session,
                        equivocation_error,
                        EquivocationBlockPolicy::OBSERVE_ONLY)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       equivocation_error);
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("action", action);
            result.pushKV("session", SessionToJSON(session));
            if (action == "refresh") return result;

            if (action == "abandon_unsigned") {
                std::string error;
                if (!store.AbandonUnsignedClientSession(
                        session.request_id, GetTime(), error) ||
                    !store.GetSessionByRequestId(session.request_id, session)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_UNSIGNED_ABANDON_FAILED" : error);
                }
                result.pushKV("session", SessionToJSON(session));
                result.pushKV("artifact", "none");
                return result;
            }

            if (session.attempt_ids.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session has no provider attempt");
            }
            uint256 attempt_id = session.attempt_ids.back();
            if (!options.find_value("attempt_id").isNull()) {
                attempt_id = ParseHashO(options, "attempt_id");
                if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
                    session.attempt_ids.end()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "attempt_id does not belong to this session");
                }
            }
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttempt(attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster attempt not found");
            }
            if (action == "cancel_to_self") {
                return ResolveAlternativePaymasterRecovery(
                    context, *wallet, store, session, attempt, options);
            }
            if (action == "fallback") {
                std::string error;
                if (!store.AbandonClientAttemptForFallback(session.request_id, attempt_id,
                                                           GetTime(), error) ||
                    !store.GetSessionByRequestId(session.request_id, session) ||
                    !store.GetAttempt(attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       error.empty() ? "PAYMASTER_FALLBACK_FAILED" : error);
                }
                result.pushKV("session", SessionToJSON(session));
                result.pushKV("attempt", AttemptToJSON(attempt));
                result.pushKV("artifact", "none");
                return result;
            }
            result.pushKV("attempt", AttemptToJSON(attempt));

            DigiDollar::Paymaster::ProviderCommitRecord commit;
            if (!attempt.commit_key.IsNull() && store.GetProviderCommit(attempt.commit_key, commit)) {
                result.pushKV("artifact", "final_transaction");
                result.pushKV("raw_transaction", HexStr(commit.final_transaction));
                result.pushKV("txid", commit.final_txid.GetHex());
                if (action == "retry_same") {
                    const int64_t now{GetTime()};
                    bool already_confirmed{false};
                    std::string broadcast_error;
                    CTransactionRef exact_transaction;
                    const bool broadcast = InsertAndBroadcastProviderCommit(
                        *wallet, store, commit, now, already_confirmed,
                        broadcast_error, &exact_transaction);
                    result.pushKV("broadcast", broadcast);
                    if (!broadcast_error.empty()) {
                        result.pushKV("broadcast_error", broadcast_error);
                    }
                    std::string reconcile_error;
                    if (broadcast && !store.ReconcileFinalTransaction(
                                         *exact_transaction, already_confirmed ? 1 : 0,
                                         !already_confirmed, now, reconcile_error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, reconcile_error);
                    }
                }
                DigiDollar::Paymaster::PaymasterResult provider_result;
                if (store.GetProviderResult(commit.commit_key, provider_result)) {
                    result.pushKV("result_status", ResultStatusName(provider_result.status));
                    result.pushKV("result_sequence", provider_result.result_sequence);
                }
                return result;
            }
            if (attempt.retry_until > 0 && GetTime() > attempt.retry_until) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
            }
            if (!attempt.user_signed_psbt.empty()) {
                result.pushKV("artifact", "user_psbt");
                result.pushKV("psbt", EncodeBase64(attempt.user_signed_psbt));
                if (action == "retry_same") {
                    PaymasterSubmitQueueState queue_state;
                    std::string error;
                    if (!QueuePersistedPaymasterSubmit(context, *wallet, session, attempt,
                                                       GetTime(), queue_state, error)) {
                        throw JSONRPCError(
                            error == "PAYMASTER_DIRECT_QUEUE_FULL" ? RPC_CLIENT_NODE_CAPACITY_REACHED : RPC_WALLET_ERROR,
                            error);
                    }
                    result.pushKV("queued", queue_state.queued);
                    result.pushKV("connection_pending", queue_state.connection_pending);
                    result.pushKV("route_available", queue_state.route_available);
                }
            } else {
                result.pushKV("artifact", "none");
            }
            return result;
        },
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
                                                                                           {RPCResult::Type::NUM_TIME, "expires_at", "Provider retry deadline"},
                                                                                           {RPCResult::Type::BOOL, "queued", "Whether PMSUBMIT is queued to a connected v2 peer"},
                                                                                           {RPCResult::Type::BOOL, "connection_pending", "Whether reconnecting to the provider was requested"},
                                                                                           {RPCResult::Type::BOOL, "route_available", "Whether a current provider announcement supplies the retry route"},
                                                                                       }},
        RPCExamples{HelpExampleCli("walletprocesspaymasterpsbt", "\"cHNidP8...\" \"0123...\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
            const uint256 unsigned_txid = CTransaction{*candidate.tx}.GetHash();
            std::optional<uint256> supplied_authorization_commitment;
            if (request.params.size() > 1 && !request.params[1].isNull()) {
                supplied_authorization_commitment =
                    ParseHashV(request.params[1], "authorization_commitment");
            }
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "No persistent Paymaster template matches this PSBT");
            }
            DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error) ||
                !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                    candidate, trusted,
                    DigiDollar::Paymaster::CollaborativeSignatureStage::UNSIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            const int64_t now = GetTime();
            PaymentIntent authorized_intent;
            PaymasterQuote authorized_quote;
            CollaborativePSBTTemplate authorized_template;
            if (!LoadAttemptAuthorizationArtifacts(
                    attempt, authorized_intent, authorized_quote,
                    authorized_template, error) ||
                attempt.client_manifest.manifest_id.IsNull() ||
                now > attempt.client_manifest.expires_at ||
                !ValidateClientAuthorizationManifest(
                    attempt.client_manifest, authorized_intent, authorized_quote,
                    attempt.capacity_snapshot, authorized_template, error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
            }
            if (attempt.state == AttemptState::QUOTED) {
                if (!supplied_authorization_commitment) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED");
                }
                if (*supplied_authorization_commitment !=
                    attempt.client_manifest.manifest_id) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
                }
                PaymasterCapacityRequest capacity_request;
                PaymasterCapacityProof capacity_proof;
                try {
                    SpanReader request_stream{::PROTOCOL_VERSION,
                                              attempt.capacity_request};
                    request_stream >> capacity_request;
                    SpanReader proof_stream{
                        ::PROTOCOL_VERSION,
                        attempt.capacity_snapshot.capacity_proof};
                    proof_stream >> capacity_proof;
                    if (!request_stream.empty() || !proof_stream.empty()) {
                        throw std::ios_base::failure("trailing capacity artifact data");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
                }
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof, capacity_request,
                        attempt.provider_identity_key, *node->chainman, now,
                        error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
                }
                if (!store.AcceptClientAuthorization(
                        session.request_id, attempt.attempt_id,
                        *supplied_authorization_commitment, now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
                }
            }
            const int64_t operation_now = std::max(
                now, std::max(session.updated_at, attempt.updated_at));
            if (attempt.state != DigiDollar::Paymaster::AttemptState::QUOTED) {
                if (attempt.state >= DigiDollar::Paymaster::AttemptState::USER_SIGNED &&
                    attempt.state <= DigiDollar::Paymaster::AttemptState::MEMPOOL &&
                    !attempt.user_signed_psbt.empty()) {
                    if (operation_now > attempt.retry_until) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
                    }
                    candidate = PartiallySignedTransaction{};
                } else {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster attempt is not awaiting a user signature");
                }
            } else if (operation_now > attempt.quote_expires_at) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster quote expired");
            }

            if (candidate.tx &&
                session.state != DigiDollar::Paymaster::SessionState::INPUTS_RESERVED &&
                session.state != DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK &&
                session.state != DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE &&
                session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Paymaster session is not awaiting a user authorization");
            }

            if (!candidate.tx) {
                if (!DecodeRawPSBT(candidate, MakeByteSpan(attempt.user_signed_psbt), error) ||
                    !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                        candidate, trusted,
                        DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Persisted Paymaster PSBT is corrupt");
                }
            } else {
                if (wallet->IsLocked()) {
                    if ((session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                         session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                         session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) &&
                        !store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                       "Wallet unlock is required to sign Paymaster USER inputs");
                }
                if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK) {
                    if (!store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                }
                // Re-read the immutable authorization immediately before the
                // only operation that can sign client inputs. Earlier RPC
                // validation is deliberately not treated as signing authority.
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                ProviderAttempt signing_attempt;
                PaymentIntent signing_intent;
                PaymasterQuote signing_quote;
                CollaborativePSBTTemplate signing_template;
                if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
                    signing_attempt.state != AttemptState::QUOTED ||
                    !supplied_authorization_commitment ||
                    signing_attempt.accepted_client_manifest_id !=
                        *supplied_authorization_commitment ||
                    signing_attempt.accepted_client_manifest_id !=
                        signing_attempt.client_manifest.manifest_id ||
                    signing_attempt.client_manifest_accepted_at <= 0 ||
                    std::max(operation_now, signing_attempt.updated_at) >
                        signing_attempt.client_manifest.expires_at ||
                    !LoadAttemptAuthorizationArtifacts(
                        signing_attempt, signing_intent, signing_quote,
                        signing_template, error) ||
                    !ValidateClientAuthorizationManifest(
                        signing_attempt.client_manifest, signing_intent,
                        signing_quote, signing_attempt.capacity_snapshot,
                        signing_template, error) ||
                    !ValidateCollaborativePSBT(
                        candidate, signing_template,
                        CollaborativeSignatureStage::UNSIGNED, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
                }
                if (signing_intent.canonical_request_hash !=
                        session.canonical_request_hash ||
                    signing_intent.requested_fee_mode !=
                        session.fee_mode_requested ||
                    signing_intent.privacy_profile !=
                        signing_attempt.privacy_profile) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH");
                }
                PaymasterCapacityRequest signing_capacity_request;
                PaymasterCapacityProof signing_capacity_proof;
                try {
                    SpanReader request_stream{::PROTOCOL_VERSION,
                                              signing_attempt.capacity_request};
                    request_stream >> signing_capacity_request;
                    SpanReader proof_stream{
                        ::PROTOCOL_VERSION,
                        signing_attempt.capacity_snapshot.capacity_proof};
                    proof_stream >> signing_capacity_proof;
                    if (!request_stream.empty() || !proof_stream.empty()) {
                        throw std::ios_base::failure(
                            "trailing capacity artifact data");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
                }
                node::NodeContext* signing_node = wallet->chain().context();
                if (!signing_node || !signing_node->chainman ||
                    !ValidateCapacityProofAgainstChainstate(
                        signing_capacity_proof, signing_capacity_request,
                        signing_attempt.provider_identity_key,
                        *signing_node->chainman,
                        std::max(operation_now, signing_attempt.updated_at), error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
                }
                if (!ValidateClientAuthorizationOwnership(
                        *wallet, signing_attempt.client_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, signing_attempt,
                        error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                attempt = std::move(signing_attempt);
                if (!SignCollaborativePSBTForParty(*wallet, candidate,
                                                   signing_template,
                                                   DigiDollar::Paymaster::SigningParty::USER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                const std::vector<unsigned char> signed_psbt = SerializePSBT(candidate);
                if (!store.TransitionSession(session.request_id,
                                             DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                             DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
                attempt.user_signed_psbt = signed_psbt;
                attempt.updated_at = std::max(attempt.updated_at, operation_now);
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            DigiDollar::Paymaster::PaymentSession authoritative;
            if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
                !store.GetAttempt(attempt.attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload durable Paymaster authorization");
            }

            PaymasterSubmitQueueState queue_state;
            if (!QueuePersistedPaymasterSubmit(context, *wallet, authoritative,
                                               attempt, operation_now, queue_state, error)) {
                throw JSONRPCError(
                    error == "PAYMASTER_DIRECT_QUEUE_FULL" ? RPC_CLIENT_NODE_CAPACITY_REACHED : RPC_WALLET_ERROR,
                    error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("psbt", EncodeBase64(attempt.user_signed_psbt));
            result.pushKV("request_id", authoritative.request_id);
            result.pushKV("session_id", authoritative.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", attempt.quote_id.GetHex());
            result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
            result.pushKV("template_commitment", attempt.template_commitment.GetHex());
            if (!attempt.client_manifest.manifest_id.IsNull()) {
                result.pushKV("authorization_commitment",
                              attempt.client_manifest.manifest_id.GetHex());
            }
            const bool authorization_accepted =
                attempt.accepted_client_manifest_id ==
                    attempt.client_manifest.manifest_id &&
                !attempt.accepted_client_manifest_id.IsNull() &&
                attempt.client_manifest_accepted_at > 0;
            result.pushKV("authorization_accepted",
                          authorization_accepted);
            if (authorization_accepted) {
                result.pushKV("authorization_accepted_at",
                              attempt.client_manifest_accepted_at);
            }
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(authoritative.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(attempt.state)});
            result.pushKV("expires_at", attempt.retry_until);
            result.pushKV("queued", queue_state.queued);
            result.pushKV("connection_pending", queue_state.connection_pending);
            result.pushKV("route_available", queue_state.route_available);
            return result;
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            PartiallySignedTransaction user_psbt;
            std::string error;
            if (!DecodeBase64PSBT(user_psbt, request.params[0].get_str(), error) || !user_psbt.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   strprintf("Paymaster PSBT decode failed: %s", error));
            }
            const uint256 unsigned_txid = CTransaction{*user_psbt.tx}.GetHash();
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "No persistent Paymaster template matches this PSBT");
            }
            DigiDollar::Paymaster::ProviderIdentityRecord provider_identity;
            if (!CheckPaymasterProviderWallet(*wallet, error) ||
                !GetPaymasterIdentity(*wallet, provider_identity) ||
                provider_identity.provider_id != attempt.provider_id) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "Paymaster provider identity does not match the quote" : error);
            }
            DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error) ||
                !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                    user_psbt, trusted,
                    DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            const std::vector<unsigned char> canonical_user_psbt = SerializePSBT(user_psbt);
            if (!attempt.user_signed_psbt.empty() && attempt.user_signed_psbt != canonical_user_psbt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PSBT_CONFLICT");
            }

            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            const int64_t now = GetTime();
            DigiDollar::Paymaster::ProviderCommitRecord committed;
            const bool already_committed = !attempt.commit_key.IsNull() &&
                                           store.GetProviderCommit(attempt.commit_key, committed);
            const bool exact_persisted_provider_signature =
                attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
                !attempt.final_txid.IsNull() &&
                !attempt.final_transaction.empty() &&
                attempt.provider_signed_at > 0 &&
                attempt.provider_signed_at <= attempt.retry_until &&
                !attempt.provider_signed_result.empty();
            if (!already_committed && !exact_persisted_provider_signature &&
                now > attempt.retry_until) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
            }
            if (!already_committed && !exact_persisted_provider_signature) {
                PaymentIntent authorized_intent;
                PaymasterQuote authorized_quote;
                CollaborativePSBTTemplate authorized_template;
                if (!LoadAttemptAuthorizationArtifacts(
                        attempt, authorized_intent, authorized_quote,
                        authorized_template, error) ||
                    attempt.provider_manifest.manifest_id.IsNull() ||
                    now > attempt.provider_manifest.expires_at ||
                    !ValidateProviderAuthorizationManifest(
                        attempt.provider_manifest, authorized_intent,
                        authorized_quote, authorized_template, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_EXPIRED" : error);
                }
                if (!ValidateProviderAuthorizationOwnership(
                        *wallet, attempt.provider_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (!already_committed &&
                attempt.state != DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
                wallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                   "Provider wallet unlock is required for a new Paymaster signature");
            }

            if (!already_committed) {
                bool rejected{false};
                DigiDollar::Paymaster::PaymasterResult rejection;
                if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *user_psbt.tx,
                                                     now, rejected, rejection, error)) {
                    throw JSONRPCError(
                        error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                        error);
                }
                if (rejected) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE");
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::QUOTED) {
                if (now > attempt.quote_expires_at) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster quote expired");
                }
                if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) {
                    if (!store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else if (session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED &&
                           session.state != DigiDollar::Paymaster::SessionState::PENDING_PROVIDER) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "Paymaster session cannot accept a user authorization");
                }
                attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
                attempt.user_signed_psbt = canonical_user_psbt;
                attempt.updated_at = std::max(attempt.updated_at, now);
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_SIGNED) {
                if (!store.AcceptUserAuthorization(session.request_id, attempt.attempt_id,
                                                   Hash(canonical_user_psbt), now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            PartiallySignedTransaction full_psbt{user_psbt};
            bool signed_now{false};
            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_PSBT_ACCEPTED) {
                ProviderAttempt signing_attempt;
                CollaborativePSBTTemplate signing_template;
                if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
                    signing_attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
                    !ValidateDurableProviderUserAuthorization(
                        store, signing_attempt, error) ||
                    !ValidateProviderAuthorizationForExecution(
                        signing_attempt, now, signing_template, error) ||
                    !ValidateCollaborativePSBT(
                        full_psbt, signing_template,
                        CollaborativeSignatureStage::USER_SIGNED, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_STATE" : error);
                }
                attempt = std::move(signing_attempt);
                node::NodeContext* node_ctx = wallet->chain().context();
                PaymasterCapacityRequest capacity_request;
                PaymasterCapacityProof capacity_proof;
                if (!node_ctx || !node_ctx->chainman) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
                }
                if (!store.ValidateProviderPreSignatureAuthorization(
                        attempt, Params().GenesisBlock().GetHash(), now,
                        capacity_request, capacity_proof, error) ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof, capacity_request,
                        attempt.provider_identity_key, *node_ctx->chainman,
                        now, error) ||
                    !TemplateInputsAvailable(*wallet, *full_psbt.tx)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE" : error);
                }
                if (!ValidateProviderAuthorizationOwnership(
                        *wallet, attempt.provider_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!SignCollaborativePSBTForParty(*wallet, full_psbt,
                                                   signing_template,
                                                   DigiDollar::Paymaster::SigningParty::PROVIDER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                CMutableTransaction final_transaction;
                if (!FinalizeAndExtractPSBT(full_psbt, final_transaction)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_FINALIZATION_FAILED");
                }
                const CTransaction signed_transaction{final_transaction};
                if (!ValidateFinalCollaborativeTransaction(
                        final_transaction, signed_transaction.GetHash(),
                        signed_transaction.GetWitnessHash(), signing_template,
                        error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                // The signed result below is private crash-recovery authority.
                // Do not persist even that envelope until the exact transaction
                // has passed the shared script, witness, chain and mempool
                // firewall. The second preflight before the atomic commit stays
                // in place to close the race after this observation.
                FinalTransactionPresence signed_presence{
                    FinalTransactionPresence::NONE};
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, MakeTransactionRef(final_transaction),
                        signed_presence, error)) {
                    const bool transient =
                        error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
                        error == "PAYMASTER_TXINDEX_NOT_READY";
                    throw JSONRPCError(
                        transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error);
                }
                signed_now = true;
                attempt.state = DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED;
                attempt.final_transaction = SerializeTransaction(final_transaction);
                attempt.final_txid = CTransaction{final_transaction}.GetHash();
                const int64_t provider_signed_at =
                    std::max(attempt.updated_at, now);
                DigiDollar::Paymaster::ProviderCommitRecord signed_commit;
                signed_commit.commit_key = attempt.commit_key;
                signed_commit.provider_id = attempt.provider_id;
                signed_commit.quote_id = attempt.quote_id;
                signed_commit.template_commitment =
                    attempt.template_commitment;
                signed_commit.final_txid = attempt.final_txid;
                signed_commit.raw_transaction_hash =
                    Hash(attempt.final_transaction);
                signed_commit.final_transaction =
                    attempt.final_transaction;
                signed_commit.provider_inputs = ProviderInputs(
                    *full_psbt.tx, attempt.input_roles);
                signed_commit.committed_at = provider_signed_at;
                signed_commit.retry_until = attempt.retry_until;
                DigiDollar::Paymaster::PaymasterResult signed_result;
                if (!BuildFinalProviderResult(
                        *wallet, signed_commit, provider_signed_at,
                        signed_result, error)) {
                    throw JSONRPCError(
                        error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                        error);
                }
                attempt.provider_signed_at = provider_signed_at;
                attempt.provider_signed_result =
                    SerializeRecoveryMessage(signed_result);
                attempt.updated_at = provider_signed_at;
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED) {
                DigiDollar::Paymaster::ProviderCommitRecord commit;
                commit.commit_key = attempt.commit_key;
                commit.provider_id = attempt.provider_id;
                commit.quote_id = attempt.quote_id;
                commit.template_commitment = attempt.template_commitment;
                commit.final_txid = attempt.final_txid;
                commit.raw_transaction_hash = Hash(attempt.final_transaction);
                commit.final_transaction = attempt.final_transaction;
                commit.provider_inputs = ProviderInputs(*user_psbt.tx, attempt.input_roles);
                // A crash may leave the complete provider-signed transaction
                // durable without its atomic ProviderCommit. Preserve the
                // signature boundary while recording the actual (monotonic)
                // commit time, even when the retry window has since elapsed.
                const int64_t commit_time =
                    std::max(now, attempt.provider_signed_at);
                commit.committed_at = commit_time;
                commit.retry_until = attempt.retry_until;
                const std::optional<uint256> sponsorship_hash =
                    attempt.sponsorship_capability_hash.IsNull() ? std::nullopt : std::optional<uint256>{attempt.sponsorship_capability_hash};
                DigiDollar::Paymaster::PaymasterResult final_result;
                if (!DeserializeCanonicalRecoveryMessage(
                        attempt.provider_signed_result, final_result,
                        error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID");
                }

                // Reload the durable PROVIDER_SIGNED attempt and re-run both
                // the provider authorization firewall and the full final
                // transaction verifier at the last possible point before the
                // atomic FINAL_COMMITTED transition.
                ProviderAttempt committing_attempt;
                CMutableTransaction validated_final;
                FinalTransactionPresence final_presence{
                    FinalTransactionPresence::NONE};
                if (!store.GetAttempt(attempt.attempt_id, committing_attempt) ||
                    committing_attempt.state != AttemptState::PROVIDER_SIGNED ||
                    !ValidateProviderCommitForExecution(
                        committing_attempt, commit, commit_time, validated_final,
                        error) ||
                    !store.ValidateProviderBudgetAuthorization(
                        committing_attempt,
                        BudgetReservationState::RESERVED,
                        /*allow_historical_policy=*/true, error)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_INVALID" : error);
                }
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, MakeTransactionRef(validated_final),
                        final_presence, error)) {
                    const bool transient =
                        error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
                        error == "PAYMASTER_TXINDEX_NOT_READY";
                    throw JSONRPCError(
                        transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error);
                }
                attempt = std::move(committing_attempt);
                if (!store.CommitProviderFinalTransaction(session.request_id, attempt.attempt_id,
                                                          commit, sponsorship_hash,
                                                          final_result,
                                                          Params().GenesisBlock().GetHash(),
                                                          error) ||
                    !store.GetProviderCommit(attempt.commit_key, committed)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (!already_committed && committed.final_transaction.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Paymaster attempt is not recoverable by exact provider retry");
            }

            ProviderCommitRecoveryResult recovery =
                RecoverProviderCommit(
                    *wallet, store, committed,
                    std::max(now, committed.committed_at));
            if (recovery.provider_result.commit_key.IsNull()) {
                const int code = recovery.error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR;
                throw JSONRPCError(code, recovery.error);
            }

            DigiDollar::Paymaster::PaymentSession authoritative;
            if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
                !store.GetAttempt(attempt.attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload durable provider commit");
            }
            UniValue result{UniValue::VOBJ};
            if (signed_now) result.pushKV("psbt", EncodeBase64(SerializePSBT(full_psbt)));
            result.pushKV("hex", HexStr(committed.final_transaction));
            result.pushKV("txid", committed.final_txid.GetHex());
            result.pushKV("broadcast", recovery.broadcast);
            if (!recovery.error.empty()) result.pushKV("broadcast_error", recovery.error);
            result.pushKV("request_id", authoritative.request_id);
            result.pushKV("session_id", authoritative.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", attempt.quote_id.GetHex());
            result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
            result.pushKV("template_commitment", attempt.template_commitment.GetHex());
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(authoritative.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(attempt.state)});
            result.pushKV("result_status", ResultStatusName(recovery.provider_result.status));
            result.pushKV("result_sequence", recovery.provider_result.result_sequence);
            result.pushKV("expires_at", attempt.retry_until);
            return result;
        },
    };
}

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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            std::string error;
            if (!CreatePaymasterIdentity(*wallet, request.params[0].get_str(), GetTime(), identity, error)) {
                const int code = error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR;
                throw JSONRPCError(code, error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("provider_id", identity.provider_id.GetHex());
            result.pushKV("identity_key", HexStr(identity.identity_key));
            result.pushKV("display_name", identity.display_name);
            result.pushKV("created_at", identity.created_at);
            return result;
        },
    };
}

RPCHelpMan setpaymasterpolicy()
{
    return RPCHelpMan{
        "setpaymasterpolicy",
        "Validate and atomically persist this wallet's Paymaster operating policy.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete provider policy", {
                                                                                                {"funding_models", RPCArg::Type::ARR, RPCArg::Optional::NO, "Non-empty funding model allowlist", {{"model", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "sponsored or user_paid"}}},
                                                                                                {"sponsorship_scope", RPCArg::Type::STR, RPCArg::Optional::NO, "public or restricted"},
                                                                                                {"fee_rate_bps", RPCArg::Type::NUM, RPCArg::Optional::NO, "User-paid rate in basis points"},
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
                                                                                                  {RPCResult::Type::NUM, "min_amount_cents", "Minimum recipient amount"},
                                                                                                  {RPCResult::Type::NUM, "max_amount_cents", "Maximum recipient amount"},
                                                                                                  {RPCResult::Type::NUM, "quote_ttl", "Quote lifetime in seconds"},
                                                                                                  {RPCResult::Type::NUM, "maximum_network_fee_dgb_satoshis", "Absolute provider network-fee cap"},
                                                                                                  {RPCResult::Type::STR_HEX, "policy_hash", "Canonical provider policy hash"},
                                                                                              }},
        RPCExamples{HelpExampleCli("setpaymasterpolicy", "'{\"funding_models\":[\"sponsored\",\"user_paid\"],\"sponsorship_scope\":\"public\",\"fee_rate_bps\":50,\"min_amount_cents\":100,\"max_amount_cents\":100000,\"quote_ttl\":60,\"maximum_network_fee_dgb_satoshis\":20000000}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const DigiDollar::Paymaster::ProviderPolicy policy = ParseProviderPolicy(request.params[0].get_obj());
            std::string error;
            if (!SetPaymasterProviderPolicy(*wallet, policy, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return ProviderPolicyToJSON(policy);
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto policy = ParseProviderSafetyPolicy(request.params[0].get_obj());
            std::string error;
            if (!SetPaymasterProviderSafetyPolicy(*wallet, policy, GetTime(), error) ||
                !GetPaymasterProviderSafetyPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "PAYMASTER_SAFETY_POLICY_RELOAD_FAILED" : error);
            }
            return ProviderSafetyPolicyToJSON(policy);
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
        },
    };
}

RPCHelpMan setpaymasterclientsafetypolicy()
{
    return RPCHelpMan{
        "setpaymasterclientsafetypolicy",
        "Persist wallet-local per-transfer and rolling-day Paymaster service-fee ceilings.\n",
        {
            {"policy", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Complete local client safety policy", {
                                                                                                           {"maximum_service_fee_per_transaction_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum service fee for one transfer"},
                                                                                                           {"maximum_service_fee_per_day_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Rolling-day service-fee ceiling"},
                                                                                                       }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persisted client safety policy", {
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_per_transaction_cents", "Per-transfer service-fee ceiling"},
                                                                                  {RPCResult::Type::NUM, "maximum_service_fee_per_day_cents", "Rolling-day service-fee ceiling"},
                                                                                  {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
                                                                              }},
        RPCExamples{HelpExampleCli("setpaymasterclientsafetypolicy", "'{\"maximum_service_fee_per_transaction_cents\":100,\"maximum_service_fee_per_day_cents\":1000}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            auto policy = ParseClientSafetyPolicy(request.params[0].get_obj());
            std::string error;
            if (!SetPaymasterClientSafetyPolicy(*wallet, policy, GetTime(), error) ||
                !GetPaymasterClientSafetyPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "PAYMASTER_CLIENT_SAFETY_POLICY_RELOAD_FAILED" : error);
            }
            return ClientSafetyPolicyToJSON(policy);
        },
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
                                                                                                                                                        {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
                                                                                                                                                    }},
                                                                        {RPCResult::Type::NUM_TIME, "accounting_time_high_water", /*optional=*/true, "Monotonic accounting time"},
                                                                        {RPCResult::Type::NUM, "active_reservations", /*optional=*/true, "Active fee reservations"},
                                                                        {RPCResult::Type::NUM, "reserved_service_fee_cents", /*optional=*/true, "Service fee held by active authorizations"},
                                                                        {RPCResult::Type::NUM, "spent_service_fee_last_day_cents", /*optional=*/true, "Committed service fees in the rolling day"},
                                                                        {RPCResult::Type::NUM, "available_service_fee_today_cents", /*optional=*/true, "Remaining rolling-day allowance after reservations"},
                                                                    }},
        RPCExamples{HelpExampleCli("getpaymasterclientsafetystatus", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
        },
    };
}

RPCHelpMan createrestrictedpaymasterdescriptor()
{
    return RPCHelpMan{
        "createrestrictedpaymasterdescriptor",
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const std::vector<unsigned char> key_bytes =
                ParseHexV(request.params[0], "sponsor_authorization_key");
            if (key_bytes.size() != 32) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "sponsor_authorization_key must be exactly 32 bytes");
            }
            const XOnlyPubKey sponsor_key{MakeUCharSpan(key_bytes)};
            const int64_t ttl = request.params[2].isNull() ? 300 : request.params[2].getInt<int64_t>();
            if (!sponsor_key.IsFullyValid() || ttl < 1 || ttl > ANNOUNCEMENT_TTL_SECONDS) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Invalid sponsor key or expires_in outside 1..600");
            }
            const ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            if (!readiness.ready || readiness.policy.sponsorship_scope != SponsorshipScope::RESTRICTED) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   readiness.ready ? "PAYMASTER_POLICY_NOT_RESTRICTED" : "PAYMASTER_PROVIDER_NOT_READY");
            }
            node::NodeContext* node = wallet->chain().context();
            if (!node || !node->chainman) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            }
            uint256 reference_block;
            {
                LOCK(cs_main);
                const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
                if (!tip) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_NODE_NOT_READY");
                reference_block = tip->GetBlockHash();
            }
            const int64_t now = GetTime();
            RestrictedServiceDescriptor descriptor;
            std::string error;
            if (!BuildRestrictedServiceDescriptor(
                    *wallet, readiness.identity, readiness.policy, readiness.pool_entries,
                    readiness.endpoint, sponsor_key, request.params[1].get_str(),
                    Params().GenesisBlock().GetHash(), reference_block, now,
                    SaturatingAddSeconds(now, ttl),
                    descriptor, error) ||
                !ValidateRestrictedServiceDescriptor(
                    descriptor, readiness.identity.identity_key,
                    Params().GenesisBlock().GetHash(), now, error,
                    Params().GetChainType() == ChainType::REGTEST) ||
                !ValidateRestrictedAdmissionProofs(descriptor, readiness.identity.identity_key,
                                                   *node->chainman, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
            stream << descriptor;
            UniValue result{UniValue::VOBJ};
            result.pushKV("descriptor", HexStr(MakeUCharSpan(stream)));
            result.pushKV("provider_id", descriptor.provider_id.GetHex());
            result.pushKV("provider_identity_key", HexStr(readiness.identity.identity_key));
            result.pushKV("offer_id", descriptor.offer_id.GetHex());
            result.pushKV("policy_hash", descriptor.policy_hash.GetHex());
            result.pushKV("endpoint", descriptor.p2p_endpoint.ToStringAddrPort());
            result.pushKV("expires_at", descriptor.expires_at);
            return result;
        },
    };
}

RPCHelpMan setpaymasterenabled()
{
    return RPCHelpMan{
        "setpaymasterenabled",
        "Persistently enable or disable Paymaster provider operation for this wallet.\n"
        "Enabling does not start the provider until startpaymaster passes readiness checks.\n",
        {
            {"enabled", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Provider enablement"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Persistent provider configuration", {
                                                                                     {RPCResult::Type::BOOL, "enabled", "Configured provider enablement"},
                                                                                     {RPCResult::Type::BOOL, "running", "Whether the runtime provider is online"},
                                                                                     {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Bound provider policy"},
                                                                                 }},
        RPCExamples{HelpExampleCli("setpaymasterenabled", "true")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::string error;
            if (!SetPaymasterProviderEnabled(*wallet, request.params[0].get_bool(), GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            DigiDollar::Paymaster::ProviderSettings settings;
            if (!GetPaymasterProviderSettings(*wallet, settings)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload Paymaster settings");
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("enabled", settings.enabled);
            if (!settings.enabled && context.paymaster) context.paymaster->StopProvider(wallet->GetName());
            DigiDollar::Paymaster::ProviderIdentityRecord identity;
            const bool running = settings.enabled && context.paymaster &&
                                 GetPaymasterIdentity(*wallet, identity) &&
                                 context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id);
            result.pushKV("running", running);
            if (!settings.policy_hash.IsNull()) result.pushKV("policy_hash", settings.policy_hash.GetHex());
            return result;
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            const UniValue& value = request.params[0].get_obj();
            RPCTypeCheckObj(
                value,
                {{"operation_mode", UniValueType(UniValue::VSTR)},
                 {"autostart", UniValueType(UniValue::VBOOL)}},
                /*fAllowNull=*/true, /*fStrict=*/true);
            if (value.find_value("operation_mode").isNull() &&
                value.find_value("autostart").isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "At least one runtime setting is required");
            }

            ProviderSettings current;
            if (!GetPaymasterProviderSettings(*wallet, current)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND");
            }
            ProviderOperationMode mode = current.operation_mode;
            const UniValue& mode_value = value.find_value("operation_mode");
            if (!mode_value.isNull()) {
                const std::string mode_name = mode_value.get_str();
                if (mode_name == "automatic") {
                    mode = ProviderOperationMode::AUTOMATIC;
                } else if (mode_name == "manual") {
                    mode = ProviderOperationMode::MANUAL;
                } else {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "operation_mode must be automatic or manual");
                }
            }
            const UniValue& autostart_value = value.find_value("autostart");
            const bool autostart = autostart_value.isNull()
                                       ? current.autostart
                                       : autostart_value.get_bool();

            ProviderIdentityRecord identity;
            const bool running = context.paymaster &&
                                 GetPaymasterIdentity(*wallet, identity) &&
                                 context.paymaster->IsProviderRunning(
                                     wallet->GetName(), identity.provider_id);
            if (running && mode != current.operation_mode) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_STOP_PROVIDER_BEFORE_MODE_CHANGE");
            }
            std::string error;
            if (!SetPaymasterProviderRuntimeSettings(
                    *wallet, mode, autostart, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("operation_mode", ProviderOperationModeName(mode));
            result.pushKV("autostart", autostart);
            result.pushKV("running", running);
            return result;
        },
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
        },
        RPCResult{RPCResult::Type::OBJ, "", "Saved policy",
                  ProviderLiquidityPolicyResults()},
        RPCExamples{HelpExampleCli("setpaymasterliquiditypolicy", "'{\"automatic_replenishment\":true,\"paid_maintenance_approved\":true,\"target_admission_dgb\":3,\"target_operational_dgb\":1,\"target_admission_carriers\":3,\"target_operational_carriers\":1,\"maximum_maintenance_fee_per_transaction_satoshis\":1000000,\"maximum_maintenance_fee_per_hour_satoshis\":5000000,\"maximum_maintenance_fee_per_day_satoshis\":20000000}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderIdentityRecord identity;
            if (!context.paymaster ||
                !GetPaymasterIdentity(*wallet, identity)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
            }
            ProviderWorkGuard work_guard{
                *context.paymaster, wallet->GetName(), identity.provider_id,
                /*require_running=*/false};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_BUSY");
            }
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
            int64_t policy_time = GetTime();
            ProviderLiquidityPolicy current_policy;
            if (GetPaymasterProviderLiquidityPolicy(
                    *wallet, current_policy)) {
                if (current_policy.updated_at ==
                    std::numeric_limits<int64_t>::max()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_LIQUIDITY_POLICY_TIME_EXHAUSTED");
                }
                policy_time = std::max(
                    policy_time, current_policy.updated_at + 1);
            }
            policy.updated_at = policy_time;
            std::string error;
            if (!SetPaymasterProviderLiquidityPolicy(
                    *wallet, policy, policy.updated_at, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return LiquidityPolicyToJSON(policy);
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            return ProviderLiquidityStatusToJSON(readiness, GetTime());
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
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
            const std::string mode_name = options.find_value("mode").get_str();
            CarrierWithdrawalMode mode;
            if (mode_name == "all_excess") {
                mode = CarrierWithdrawalMode::ALL_EXCESS;
            } else if (mode_name == "release_slot") {
                mode = CarrierWithdrawalMode::RELEASE_SLOT;
            } else {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_INVALID_CARRIER_WITHDRAWAL_MODE");
            }
            const UniValue& execute_value = options.find_value("execute");
            const bool execute = !execute_value.isNull() && execute_value.get_bool();
            ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
            if (!context.paymaster || !readiness.have_identity) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
            }
            ProviderWorkGuard work_guard{
                *context.paymaster, wallet->GetName(),
                readiness.identity.provider_id,
                /*require_running=*/false};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PROVIDER_BUSY");
            }
            if (!readiness.have_liquidity_policy) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
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
                    if (provider_is_running()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PROVIDER_MUST_BE_STOPPED");
                    }
                    const UniValue& txid_value = options.find_value("txid");
                    const UniValue& vout_value = options.find_value("vout");
                    if (txid_value.isNull() || vout_value.isNull()) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RELEASE_SLOT_OUTPOINT_REQUIRED");
                    }
                    const int64_t vout = vout_value.getInt<int64_t>();
                    if (vout < 0 ||
                        vout > std::numeric_limits<uint32_t>::max()) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_INVALID_CARRIER_OUTPOINT");
                    }
                    const COutPoint source{
                        ParseHashV(txid_value, "txid"),
                        static_cast<uint32_t>(vout)};
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
                        wallet->IsSpent(found->outpoint)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
                    }
                    if (liquidity_policy.target_operational_carriers == 0) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO");
                    }
                    plan.source_carriers.push_back(source);
                    plan.target_operational_carriers_after_release =
                        liquidity_policy.target_operational_carriers - 1;
                } else {
                    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                    if (!dd_wallet) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DD_WALLET_UNAVAILABLE");
                    }
                    int64_t total_excess{0};
                    for (const ProviderPoolEntry& entry :
                         readiness.pool_entries) {
                        if (entry.asset != PoolAsset::DD_CARRIER ||
                            entry.state != PoolEntryState::AVAILABLE ||
                            entry.confirmation_height <= 0 ||
                            entry.carrier_value.value <= 100 ||
                            wallet->IsSpent(entry.outpoint)) {
                            continue;
                        }
                        if (plan.source_carriers.size() >= 64 ||
                            total_excess >
                                std::numeric_limits<int64_t>::max() -
                                    (entry.carrier_value.value - 100)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                "PAYMASTER_CARRIER_WITHDRAWAL_TOO_LARGE");
                        }
                        const auto destination = wallet->GetNewDestination(
                            OutputType::BECH32M,
                            "Paymaster retained carrier");
                        if (!destination) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
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
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_NO_WITHDRAWABLE_CARRIER_EXCESS");
                    }
                    const auto excess_destination = wallet->GetNewDestination(
                        OutputType::BECH32M,
                        "Paymaster carrier fee withdrawal");
                    if (!excess_destination) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
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
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                        }
                    }
                    if (!append_recipient(
                            plan.excess_script_pub_key,
                            plan.excess_amount.value)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                    }
                    DDTransferPlan transfer_plan;
                    std::string plan_error;
                    if (!dd_wallet->PlanDigiDollarTransfer(
                            recipients, transfer_plan, plan_error,
                            &plan.source_carriers,
                            /*allow_paymaster_pool_inputs=*/true)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: " +
                                plan_error);
                    }
                    plan.estimated_fee = DGBSatoshis{
                        transfer_plan.estimated_fee};
                    if (plan.estimated_fee.value <= 0 ||
                        plan.estimated_fee.value >
                            liquidity_policy.maximum_maintenance_fee_per_transaction.value) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_MAINTENANCE_FEE_EXCEEDED");
                    }
                }
                plan.plan_id = CarrierWithdrawalPlanId(plan);
                std::string plan_error;
                if (!ValidateProviderCarrierWithdrawalPlan(plan, plan_error) ||
                    !SaveCarrierWithdrawalPreview(*wallet, plan, plan_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        plan_error.empty()
                            ? "PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED"
                            : plan_error);
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("executed", false);
                result.pushKV("plan_id", plan.plan_id.GetHex());
                result.pushKV("mode", mode_name);
                result.pushKV("source_carriers", plan.source_carriers.size());
                result.pushKV("expires_at", plan.expires_at);
                if (mode == CarrierWithdrawalMode::ALL_EXCESS) {
                    result.pushKV(
                        "withdrawable_excess_cents",
                        plan.excess_amount.value);
                    result.pushKV(
                        "retained_carrier_cents",
                        static_cast<int64_t>(
                            plan.replacement_carriers.size()) * 100);
                    result.pushKV(
                        "estimated_network_fee_satoshis",
                        plan.estimated_fee.value);
                } else {
                    result.pushKV(
                        "operational_carrier_target",
                        plan.target_operational_carriers_after_release);
                }
                return result;
            }

            const UniValue& plan_id_value = options.find_value("plan_id");
            if (plan_id_value.isNull()) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_ID_REQUIRED");
            }
            const uint256 supplied_plan_id =
                ParseHashV(plan_id_value, "plan_id");
            ProviderCarrierWithdrawalPlan plan;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!batch.ReadPaymasterCarrierWithdrawalPlan(plan)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_NOT_FOUND");
                }
            }
            if (plan.plan_id != supplied_plan_id || plan.mode != mode ||
                plan.plan_id != CarrierWithdrawalPlanId(plan)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
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
                            UniValue result{UniValue::VOBJ};
                            result.pushKV("executed", true);
                            result.pushKV("plan_id", plan.plan_id.GetHex());
                            result.pushKV("mode", mode_name);
                            result.pushKV(
                                "txid",
                                existing->transaction_id.GetHex());
                            result.pushKV(
                                "withdrawable_excess_cents",
                                plan.excess_amount.value);
                            result.pushKV(
                                "retained_carrier_cents",
                                static_cast<int64_t>(
                                    plan.replacement_carriers.size()) *
                                    100);
                            result.pushKV(
                                "estimated_network_fee_satoshis",
                                existing->actual_fee.value);
                            return result;
                        }
                    } else if (batch.HasPaymasterMaintenanceLedger()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
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
                            UniValue result{UniValue::VOBJ};
                            result.pushKV("executed", true);
                            result.pushKV("plan_id", plan.plan_id.GetHex());
                            result.pushKV("mode", mode_name);
                            result.pushKV(
                                "operational_carrier_target",
                                terminal_policy
                                    .target_operational_carriers);
                            return result;
                        }
                    }
                }
            }
            if (plan.expires_at < now ||
                plan.liquidity_policy_updated_at !=
                    liquidity_policy.updated_at) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
            }

            if (mode == CarrierWithdrawalMode::RELEASE_SLOT) {
                if (provider_is_running()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
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
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
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
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE");
                    }
                    source->state = PoolEntryState::RELEASED;
                    source->reservation_id.SetNull();
                    source->updated_at = now;
                    current_policy.target_operational_carriers =
                        plan.target_operational_carriers_after_release;
                    if (current_policy.updated_at ==
                        std::numeric_limits<int64_t>::max()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_LIQUIDITY_POLICY_TIME_EXHAUSTED");
                    }
                    current_policy.updated_at = std::max(
                        now, current_policy.updated_at + 1);
                    std::string policy_error;
                    if (!ValidateProviderLiquidityPolicy(
                            current_policy, policy_error)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR, policy_error);
                    }
                    if (!batch.TxnBegin()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DATABASE_BEGIN");
                    }
                    if (!batch.WritePaymasterProviderPool(pool) ||
                        !batch.WritePaymasterLiquidityPolicy(
                            current_policy)) {
                        batch.TxnAbort();
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DATABASE_WRITE");
                    }
                    if (!batch.TxnCommit()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_DATABASE_COMMIT");
                    }
                    liquidity_policy = current_policy;
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("executed", true);
                result.pushKV("plan_id", plan.plan_id.GetHex());
                result.pushKV("mode", mode_name);
                result.pushKV(
                    "operational_carrier_target",
                    liquidity_policy.target_operational_carriers);
                return result;
            }

            EnsureWalletIsUnlocked(*wallet);
            DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
            if (!dd_wallet) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
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
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                }
                liquidity_policy = current_policy;
                if (!batch.ReadPaymasterMaintenanceLedger(ledger)) {
                    if (batch.HasPaymasterMaintenanceLedger()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
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
                    UniValue result{UniValue::VOBJ};
                    result.pushKV("executed", true);
                    result.pushKV("plan_id", plan.plan_id.GetHex());
                    result.pushKV("mode", mode_name);
                    result.pushKV(
                        "txid", existing->transaction_id.GetHex());
                    result.pushKV(
                        "withdrawable_excess_cents",
                        plan.excess_amount.value);
                    result.pushKV(
                        "estimated_network_fee_satoshis",
                        existing->actual_fee.value);
                    return result;
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
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                }
            }
            if (!append_recipient(
                    plan.excess_script_pub_key,
                    plan.excess_amount.value)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
            }
            DDTransferPlan transfer_plan;
            std::string transfer_error;
            if (!dd_wallet->PlanDigiDollarTransfer(
                    recipients, transfer_plan, transfer_error,
                    &plan.source_carriers,
                    /*allow_paymaster_pool_inputs=*/true) ||
                transfer_plan.estimated_fee != plan.estimated_fee.value) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
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
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                }
                if (!batch.ReadPaymasterMaintenanceLedger(current_ledger)) {
                    if (batch.HasPaymasterMaintenanceLedger()) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                    }
                    current_ledger.accounting_time_high_water = now;
                }
                operation.maximum_fee =
                    current_policy
                        .maximum_maintenance_fee_per_transaction;
                if (!ReserveProviderMaintenanceBudget(
                        current_ledger, current_policy, operation, now,
                        transfer_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, transfer_error);
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
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED");
                    }
                    source->state = PoolEntryState::RESERVED;
                    source->reservation_id = plan.operation_id;
                    source->updated_at = now;
                }
                if (!MakeProviderPoolRoomForMaintenance(
                        current_pool,
                        plan.replacement_carriers.size(),
                        transfer_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR, transfer_error);
                }
                if (!batch.TxnBegin()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.WritePaymasterMaintenanceLedger(
                        current_ledger) ||
                    !batch.WritePaymasterProviderPool(current_pool)) {
                    batch.TxnAbort();
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_WRITE");
                }
                if (!batch.TxnCommit()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_COMMIT");
                }
                liquidity_policy = current_policy;
                ledger = std::move(current_ledger);
                pool = std::move(current_pool);
            }

            std::string txid_string;
            if (!dd_wallet->TransferDigiDollarMany(
                    recipients, txid_string, transfer_error,
                    /*dd_change_out=*/nullptr,
                    &plan.source_carriers,
                    "Paymaster carrier excess withdrawal",
                    /*allow_paymaster_pool_inputs=*/true,
                    /*exact_plan=*/&transfer_plan)) {
                // The exact sources stay reserved. A retry first reconciles
                // any transaction that may have crossed the commit boundary.
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_CARRIER_WITHDRAWAL_FAILED: " +
                        transfer_error);
            }
            const uint256 txid = uint256S(txid_string);
            CTransactionRef transaction;
            {
                LOCK(wallet->cs_wallet);
                const auto found = wallet->mapWallet.find(txid);
                if (found != wallet->mapWallet.end()) {
                    transaction = found->second.tx;
                }
            }
            if (!transaction) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_MAINTENANCE_TRANSACTION_NOT_IN_WALLET");
            }
            CAmount actual_fee{0};
            {
                LOCK(wallet->cs_wallet);
                const CAmount debit =
                    wallet->GetDebit(*transaction, ISMINE_ALL);
                const CAmount value_out = transaction->GetValueOut();
                if (debit < value_out) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_MAINTENANCE_FEE_INVALID");
                }
                actual_fee = debit - value_out;
            }
            // Treat the reviewed estimate as a maximum.  Exact signing can
            // reduce the serialized size and therefore the actual fee; an
            // increase beyond the preview remains a hard failure.
            if (actual_fee <= 0 ||
                actual_fee > transfer_plan.estimated_fee ||
                actual_fee >
                    liquidity_policy.maximum_maintenance_fee_per_transaction.value) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_MAINTENANCE_FEE_CHANGED");
            }
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!batch.ReadPaymasterMaintenanceLedger(ledger) ||
                    !batch.ReadPaymasterProviderPool(pool) ||
                    !SpendProviderMaintenanceBudget(
                        ledger, plan.operation_id, txid,
                        DGBSatoshis{actual_fee}, now, transfer_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        transfer_error.empty()
                            ? "PAYMASTER_MAINTENANCE_LEDGER_UPDATE_FAILED"
                            : transfer_error);
                }
                for (const COutPoint& source_outpoint :
                     plan.source_carriers) {
                    const auto source = std::find_if(
                        pool.begin(), pool.end(),
                        [&](const ProviderPoolEntry& entry) {
                            return entry.outpoint == source_outpoint;
                        });
                    if (source == pool.end() ||
                        source->state != PoolEntryState::RESERVED ||
                        source->reservation_id != plan.operation_id) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_CARRIER_WITHDRAWAL_POOL_CONFLICT");
                    }
                    source->state = PoolEntryState::COMMITTED;
                    source->updated_at = now;
                }
                for (const ProviderMaintenanceOutput& replacement :
                     plan.replacement_carriers) {
                    const auto output_index = FindMaintenanceOutputIndex(
                        *transaction, replacement);
                    if (!output_index) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_MAINTENANCE_OUTPUT_MISSING");
                    }
                    ProviderPoolEntry entry;
                    entry.outpoint = COutPoint{txid, *output_index};
                    entry.purpose = replacement.purpose;
                    entry.asset = PoolAsset::DD_CARRIER;
                    entry.state = PoolEntryState::PENDING_SUCCESSOR;
                    entry.script_pub_key = replacement.script_pub_key;
                    entry.carrier_value = replacement.carrier_value;
                    entry.reservation_id = plan.operation_id;
                    entry.origin_commit_key = plan.operation_id;
                    entry.updated_at = now;
                    if (std::none_of(
                            pool.begin(), pool.end(),
                            [&](const ProviderPoolEntry& existing) {
                                return existing.outpoint == entry.outpoint;
                            })) {
                        pool.push_back(std::move(entry));
                    }
                }
                if (!batch.TxnBegin()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.WritePaymasterMaintenanceLedger(ledger) ||
                    !batch.WritePaymasterProviderPool(pool)) {
                    batch.TxnAbort();
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_WRITE");
                }
                if (!batch.TxnCommit()) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_DATABASE_COMMIT");
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("executed", true);
            result.pushKV("plan_id", plan.plan_id.GetHex());
            result.pushKV("mode", mode_name);
            result.pushKV("txid", txid.GetHex());
            result.pushKV(
                "withdrawable_excess_cents",
                plan.excess_amount.value);
            result.pushKV(
                "retained_carrier_cents",
                static_cast<int64_t>(
                    plan.replacement_carriers.size()) * 100);
            result.pushKV(
                "estimated_network_fee_satoshis",
                actual_fee);
            return result;
        },
    };
}

RPCHelpMan preparepaymasterpool()
{
    return RPCHelpMan{
        "preparepaymasterpool",
        "Preview or create dedicated P2TR DGB and DD-carrier outputs for the Paymaster admission and operational pools.\n"
        "The default is a side-effect-free preview. Set execute=true only after reviewing the returned amounts.\n"
        "Existing live entries count toward the requested totals, so an interrupted preparation can be resumed safely.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Pool target and explicit execution approval", {
                                                                                                                    {"admission_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Three to sixteen admission DGB slots"},
                                                                                                                    {"operational_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "One to sixteen operational DGB slots"},
                                                                                                                    {"admission_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Three to sixteen admission carriers for USER_PAID"},
                                                                                                                    {"operational_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "One to sixteen operational carriers for USER_PAID"},
                                                                                                                    {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Create and broadcast the reviewed pool transaction"},
                                                                                                                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Pool preparation preview or transaction", {
                                                                                           {RPCResult::Type::BOOL, "executed", "Whether wallet funds were committed"},
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"admission_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"operational_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"admission_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"operational_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"execute", UniValueType(UniValue::VBOOL)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const int admission = options.find_value("admission_dgb_slots").getInt<int>();
            const int operational = options.find_value("operational_dgb_slots").getInt<int>();
            const int admission_carriers = options.find_value("admission_carrier_slots").isNull() ? 0 : options.find_value("admission_carrier_slots").getInt<int>();
            const int operational_carriers = options.find_value("operational_carrier_slots").isNull() ? 0 : options.find_value("operational_carrier_slots").getInt<int>();
            const bool execute = !options.find_value("execute").isNull() &&
                                 options.find_value("execute").get_bool();
            if (admission < static_cast<int>(REQUIRED_ADMISSION_SLOTS) || admission > 16 ||
                operational < 1 || operational > 16 || admission_carriers < 0 ||
                admission_carriers > 16 || operational_carriers < 0 || operational_carriers > 16) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_INVALID_POOL_TARGET");
            }
            ProviderPolicy policy;
            if (!GetPaymasterProviderPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POLICY_NOT_FOUND");
            }
            const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
            if (user_paid && (admission_carriers < static_cast<int>(REQUIRED_ADMISSION_SLOTS) ||
                              operational_carriers < 1)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
            }
            if (!user_paid && (admission_carriers != 0 || operational_carriers != 0)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
            }
            std::vector<ProviderPoolEntry> existing;
            const bool have_existing = GetPaymasterProviderPoolEntries(*wallet, existing);
            std::string refresh_error;
            if (have_existing &&
                !RefreshPoolConfirmationHeights(*wallet, existing, refresh_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
            }
            const auto live_count = [&](PoolPurpose purpose, PoolAsset asset) {
                return static_cast<int>(std::count_if(existing.begin(), existing.end(),
                                                      [&](const ProviderPoolEntry& entry) {
                                                          return entry.purpose == purpose && entry.asset == asset &&
                                                                 (entry.state == PoolEntryState::AVAILABLE ||
                                                                  entry.state == PoolEntryState::RESERVED ||
                                                                  entry.state == PoolEntryState::PENDING_SUCCESSOR);
                                                      }));
            };
            const int missing_admission = std::max(0, admission - live_count(PoolPurpose::ADMISSION, PoolAsset::DGB));
            const int missing_operational = std::max(0, operational - live_count(PoolPurpose::OPERATIONAL, PoolAsset::DGB));
            const int missing_admission_carriers = std::max(0, admission_carriers - live_count(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER));
            const int missing_operational_carriers = std::max(0, operational_carriers - live_count(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER));
            constexpr int64_t admission_value{MIN_ADMISSION_DGB_SATOSHIS};
            constexpr int64_t carrier_value{100};
            const int64_t operational_value = std::max<int64_t>(
                MIN_ADMISSION_DGB_SATOSHIS, policy.maximum_network_fee.value);
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
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "PAYMASTER_POOL_VALUE_OUT_OF_RANGE");
            }
            const int64_t total_carriers = carrier_value *
                                           (missing_admission_carriers + missing_operational_carriers);
            UniValue result{UniValue::VOBJ};
            result.pushKV("admission_dgb_slots", admission);
            result.pushKV("operational_dgb_slots", operational);
            result.pushKV("admission_carrier_slots", admission_carriers);
            result.pushKV("operational_carrier_slots", operational_carriers);
            result.pushKV("missing_admission_dgb_slots", missing_admission);
            result.pushKV("missing_operational_dgb_slots", missing_operational);
            result.pushKV("missing_admission_carrier_slots", missing_admission_carriers);
            result.pushKV("missing_operational_carrier_slots", missing_operational_carriers);
            result.pushKV("admission_dgb_satoshis_each", admission_value);
            result.pushKV("operational_dgb_satoshis_each", operational_value);
            result.pushKV("carrier_cents_each", carrier_value);
            result.pushKV("total_output_satoshis", *total);
            result.pushKV("total_carrier_cents", total_carriers);
            if (!execute) {
                result.pushKV("executed", false);
                return result;
            }

            EnsureWalletIsUnlocked(*wallet);
            bool executed{false};
            std::string dd_txid;
            CTransactionRef dd_tx;
            if (missing_admission_carriers + missing_operational_carriers > 0) {
                DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_DD_WALLET_UNAVAILABLE");
                struct CarrierOutput {
                    CScript script;
                    PoolPurpose purpose;
                };
                std::vector<CarrierOutput> carrier_outputs;
                std::vector<std::pair<CDigiDollarAddress, CAmount>> carrier_recipients;
                const auto append_carriers = [&](PoolPurpose purpose, int count) {
                    for (int i = 0; i < count; ++i) {
                        auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                     purpose == PoolPurpose::ADMISSION ? "Paymaster admission carrier" : "Paymaster operational carrier");
                        if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                        const std::string encoded = EncodeDigiDollarAddress(*destination);
                        CDigiDollarAddress address{encoded};
                        if (encoded.empty() || !address.IsValid()) {
                            throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                        }
                        carrier_outputs.push_back({GetScriptForDestination(*destination), purpose});
                        carrier_recipients.emplace_back(std::move(address), carrier_value);
                    }
                };
                append_carriers(PoolPurpose::ADMISSION, missing_admission_carriers);
                append_carriers(PoolPurpose::OPERATIONAL, missing_operational_carriers);
                std::string transfer_error;
                if (!dd_wallet->TransferDigiDollarMany(carrier_recipients, dd_txid, transfer_error,
                                                       nullptr, nullptr, "Paymaster carrier pool")) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_TRANSACTION_FAILED: " + transfer_error);
                }
                const uint256 carrier_hash = uint256S(dd_txid);
                CTransactionRef carrier_tx;
                {
                    LOCK(wallet->cs_wallet);
                    const auto it = wallet->mapWallet.find(carrier_hash);
                    if (it != wallet->mapWallet.end()) carrier_tx = it->second.tx;
                }
                if (!carrier_tx) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_TRANSACTION_NOT_IN_WALLET");
                }
                dd_tx = carrier_tx;
                const int64_t now = GetTime();
                for (const CarrierOutput& output : carrier_outputs) {
                    const auto match = std::find_if(carrier_tx->vout.begin(), carrier_tx->vout.end(),
                                                    [&](const CTxOut& txout) {
                                                        return txout.nValue == 0 && txout.scriptPubKey == output.script;
                                                    });
                    if (match == carrier_tx->vout.end()) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_OUTPUT_NOT_FOUND_AFTER_COMMIT");
                    }
                    ProviderPoolEntry entry;
                    entry.outpoint = COutPoint{carrier_hash,
                                               static_cast<uint32_t>(std::distance(carrier_tx->vout.begin(), match))};
                    entry.purpose = output.purpose;
                    entry.asset = PoolAsset::DD_CARRIER;
                    entry.script_pub_key = output.script;
                    entry.carrier_value = DDCents{carrier_value};
                    entry.updated_at = now;
                    existing.push_back(std::move(entry));
                }
                std::string persist_error;
                if (!SetPaymasterProviderPoolEntries(*wallet, existing, persist_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       persist_error + "; carrier outputs remain controlled by this wallet");
                }
                executed = true;
            }

            struct PlannedOutput {
                CTxDestination destination;
                PoolPurpose purpose;
                int64_t value;
            };
            std::vector<PlannedOutput> planned;
            std::vector<CRecipient> recipients;
            const auto append = [&](PoolPurpose purpose, int count, int64_t value) {
                for (int i = 0; i < count; ++i) {
                    auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                 purpose == PoolPurpose::ADMISSION ? "Paymaster admission" : "Paymaster operational");
                    if (!destination) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                    }
                    planned.push_back({*destination, purpose, value});
                    recipients.push_back({*destination, value, false});
                }
            };
            append(PoolPurpose::ADMISSION, missing_admission, admission_value);
            append(PoolPurpose::OPERATIONAL, missing_operational, operational_value);
            CTransactionRef dgb_tx;
            CAmount dgb_fee{0};
            if (!recipients.empty()) {
                CCoinControl coin_control;
                auto created = CreateTransaction(*wallet, recipients, /*change_pos=*/-1,
                                                 coin_control, /*sign=*/true);
                if (!created) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                                       util::ErrorString(created).original);
                }
                std::string commit_error;
                if (!wallet->CommitTransaction(created->tx, {}, {}, &commit_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_POOL_TRANSACTION_REJECTED: " + commit_error);
                }
                const int64_t now = GetTime();
                for (const auto& output : planned) {
                    const CScript script = GetScriptForDestination(output.destination);
                    const auto match = std::find_if(created->tx->vout.begin(), created->tx->vout.end(),
                                                    [&](const CTxOut& txout) {
                                                        return txout.nValue == output.value && txout.scriptPubKey == script;
                                                    });
                    if (match == created->tx->vout.end()) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_OUTPUT_NOT_FOUND_AFTER_COMMIT");
                    }
                    ProviderPoolEntry entry;
                    entry.outpoint = COutPoint{created->tx->GetHash(),
                                               static_cast<uint32_t>(std::distance(created->tx->vout.begin(), match))};
                    entry.purpose = output.purpose;
                    entry.asset = PoolAsset::DGB;
                    entry.script_pub_key = script;
                    entry.dgb_value = DGBSatoshis{output.value};
                    entry.updated_at = now;
                    existing.push_back(std::move(entry));
                }
                std::string error;
                if (!SetPaymasterProviderPoolEntries(*wallet, existing, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       error + "; pool outputs remain controlled by this wallet");
                }
                dgb_tx = created->tx;
                dgb_fee = created->fee;
                executed = true;
            }
            const int64_t finance_time = GetTime();
            std::string finance_error;
            if (dd_tx) {
                const auto fee = GetProviderFinanceTransactionFee(
                    *wallet, dd_tx);
                if (!fee || !RecordProviderFinanceTransaction(
                                *wallet,
                                ProviderFinanceEventKind::POOL_SETUP,
                                dd_tx, *fee, finance_time,
                                finance_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        finance_error.empty()
                            ? "PAYMASTER_POOL_FINANCE_FEE_UNAVAILABLE"
                            : finance_error);
                }
            }
            if (dgb_tx && !RecordProviderFinanceTransaction(
                              *wallet,
                              ProviderFinanceEventKind::POOL_SETUP,
                              dgb_tx, DGBSatoshis{dgb_fee}, finance_time,
                              finance_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, finance_error);
            }
            UniValue pool{UniValue::VARR};
            for (const auto& entry : existing)
                pool.push_back(PoolEntryToJSON(entry));
            result.pushKV("executed", executed);
            if (!dd_txid.empty()) result.pushKV("dd_txid", dd_txid);
            if (dgb_tx) {
                result.pushKV("txid", dgb_tx->GetHash().GetHex());
                result.pushKV("dgb_txid", dgb_tx->GetHash().GetHex());
                result.pushKV("network_fee_satoshis", dgb_fee);
            } else if (!dd_txid.empty()) {
                result.pushKV("txid", dd_txid);
            }
            result.pushKV("pool", std::move(pool));
            return result;
        },
    };
}

RPCHelpMan rebalancepaymasterpool()
{
    return RPCHelpMan{
        "rebalancepaymasterpool",
        "Preview or retire excess available Paymaster pool entries.\n"
        "The default is side-effect-free. Set execute=true only after reviewing the exact retired values.\n"
        "Active reservations are never touched. Increasing a target remains an additive preparepaymasterpool operation.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Pool target and explicit execution approval", {
                                                                                                                    {"admission_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Remaining admission DGB slots"},
                                                                                                                    {"operational_dgb_slots", RPCArg::Type::NUM, RPCArg::Optional::NO, "Remaining operational DGB slots"},
                                                                                                                    {"admission_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Remaining admission carriers"},
                                                                                                                    {"operational_carrier_slots", RPCArg::Type::NUM, RPCArg::Default{0}, "Remaining operational carriers"},
                                                                                                                    {"execute", RPCArg::Type::BOOL, RPCArg::Default{false}, "Commit the reviewed retirement transactions"},
                                                                                                                }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Pool rebalance preview or result", {
                                                                                    {RPCResult::Type::BOOL, "executed", "Whether a retirement transaction was committed"},
                                                                                    {RPCResult::Type::NUM, "retired_admission_dgb_slots", "Admission DGB entries selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_operational_dgb_slots", "Operational DGB entries selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_admission_carrier_slots", "Admission carriers selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_operational_carrier_slots", "Operational carriers selected for retirement"},
                                                                                    {RPCResult::Type::NUM, "retired_dgb_satoshis", "DGB returned to ordinary wallet liquidity before network fee"},
                                                                                    {RPCResult::Type::NUM, "retired_carrier_cents", "DD returned to ordinary wallet liquidity"},
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"admission_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"operational_dgb_slots", UniValueType(UniValue::VNUM)},
                             {"admission_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"operational_carrier_slots", UniValueType(UniValue::VNUM)},
                             {"execute", UniValueType(UniValue::VBOOL)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const int admission = options.find_value("admission_dgb_slots").getInt<int>();
            const int operational = options.find_value("operational_dgb_slots").getInt<int>();
            const int admission_carriers = options.find_value("admission_carrier_slots").isNull() ? 0 : options.find_value("admission_carrier_slots").getInt<int>();
            const int operational_carriers = options.find_value("operational_carrier_slots").isNull() ? 0 : options.find_value("operational_carrier_slots").getInt<int>();
            const bool execute = !options.find_value("execute").isNull() &&
                                 options.find_value("execute").get_bool();
            if (admission < static_cast<int>(REQUIRED_ADMISSION_SLOTS) || admission > 16 ||
                operational < 1 || operational > 16 || admission_carriers < 0 ||
                admission_carriers > 16 || operational_carriers < 0 || operational_carriers > 16) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_INVALID_POOL_TARGET");
            }
            ProviderPolicy policy;
            if (!GetPaymasterProviderPolicy(*wallet, policy)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POLICY_NOT_FOUND");
            }
            const bool user_paid = PolicyAllowsFundingModel(policy, FundingModel::USER_PAID);
            if (user_paid && (admission_carriers < static_cast<int>(REQUIRED_ADMISSION_SLOTS) ||
                              operational_carriers < 1)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
            }
            if (!user_paid && (admission_carriers != 0 || operational_carriers != 0)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
            }
            std::vector<ProviderPoolEntry> entries;
            if (!GetPaymasterProviderPoolEntries(*wallet, entries)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOLS_NOT_PREPARED");
            }
            std::string refresh_error;
            if (!RefreshPoolConfirmationHeights(*wallet, entries, refresh_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, refresh_error);
            }
            if (std::any_of(entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
                    return entry.state == PoolEntryState::RESERVED ||
                           entry.state == PoolEntryState::PENDING_SUCCESSOR ||
                           entry.state == PoolEntryState::COMMITTED;
                })) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REBALANCE_ACTIVE_RESERVATIONS");
            }

            const auto candidates = [&](PoolPurpose purpose, PoolAsset asset) {
                std::vector<const ProviderPoolEntry*> result;
                for (const ProviderPoolEntry& entry : entries) {
                    if (entry.purpose == purpose && entry.asset == asset &&
                        entry.state == PoolEntryState::AVAILABLE) {
                        result.push_back(&entry);
                    }
                }
                std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) {
                    return a->outpoint < b->outpoint;
                });
                return result;
            };
            const auto admission_dgb = candidates(PoolPurpose::ADMISSION, PoolAsset::DGB);
            const auto operational_dgb = candidates(PoolPurpose::OPERATIONAL, PoolAsset::DGB);
            const auto admission_dd = candidates(PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER);
            const auto operational_dd = candidates(PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER);
            if (admission > static_cast<int>(admission_dgb.size()) ||
                operational > static_cast<int>(operational_dgb.size()) ||
                admission_carriers > static_cast<int>(admission_dd.size()) ||
                operational_carriers > static_cast<int>(operational_dd.size())) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_REBALANCE_INCREASE_USE_PREPARE");
            }

            std::vector<const ProviderPoolEntry*> retire_dgb;
            std::vector<const ProviderPoolEntry*> retire_dd;
            const auto retire_tail = [](const auto& available, int keep, auto& retired) {
                retired.insert(retired.end(), available.begin() + keep, available.end());
            };
            retire_tail(admission_dgb, admission, retire_dgb);
            retire_tail(operational_dgb, operational, retire_dgb);
            retire_tail(admission_dd, admission_carriers, retire_dd);
            retire_tail(operational_dd, operational_carriers, retire_dd);
            if (std::any_of(retire_dgb.begin(), retire_dgb.end(), [](const auto* entry) {
                    return entry->confirmation_height <= 0;
                }) ||
                std::any_of(retire_dd.begin(), retire_dd.end(), [](const auto* entry) {
                    return entry->confirmation_height <= 0;
                })) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REBALANCE_REQUIRES_CONFIRMED_INPUTS");
            }
            const auto count_retired = [](const auto& retired, PoolPurpose purpose) {
                return static_cast<int>(std::count_if(retired.begin(), retired.end(),
                                                      [&](const auto* entry) { return entry->purpose == purpose; }));
            };
            const int retired_admission_dgb = count_retired(retire_dgb, PoolPurpose::ADMISSION);
            const int retired_operational_dgb = count_retired(retire_dgb, PoolPurpose::OPERATIONAL);
            const int retired_admission_dd = count_retired(retire_dd, PoolPurpose::ADMISSION);
            const int retired_operational_dd = count_retired(retire_dd, PoolPurpose::OPERATIONAL);
            const int64_t retired_dgb_value = std::accumulate(retire_dgb.begin(), retire_dgb.end(), int64_t{0},
                                                              [](int64_t total, const auto* entry) { return total + entry->dgb_value.value; });
            const int64_t retired_dd_value = std::accumulate(retire_dd.begin(), retire_dd.end(), int64_t{0},
                                                             [](int64_t total, const auto* entry) { return total + entry->carrier_value.value; });

            UniValue result{UniValue::VOBJ};
            result.pushKV("retired_admission_dgb_slots", retired_admission_dgb);
            result.pushKV("retired_operational_dgb_slots", retired_operational_dgb);
            result.pushKV("retired_admission_carrier_slots", retired_admission_dd);
            result.pushKV("retired_operational_carrier_slots", retired_operational_dd);
            result.pushKV("retired_dgb_satoshis", retired_dgb_value);
            result.pushKV("retired_carrier_cents", retired_dd_value);
            if (!execute) {
                result.pushKV("executed", false);
                return result;
            }

            EnsureWalletIsUnlocked(*wallet);
            bool executed{false};
            std::string dd_txid;
            CTransactionRef dd_tx;
            if (!retire_dd.empty()) {
                DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_DD_WALLET_UNAVAILABLE");
                std::vector<COutPoint> inputs;
                std::vector<std::pair<CDigiDollarAddress, CAmount>> recipients;
                for (const ProviderPoolEntry* entry : retire_dd) {
                    auto destination = wallet->GetNewDestination(OutputType::BECH32M, "Retired Paymaster carrier");
                    if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                    CDigiDollarAddress address{EncodeDigiDollarAddress(*destination)};
                    if (!address.IsValid()) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_ADDRESS_UNAVAILABLE");
                    inputs.push_back(entry->outpoint);
                    recipients.emplace_back(std::move(address), entry->carrier_value.value);
                }
                std::string transfer_error;
                if (!dd_wallet->TransferDigiDollarMany(recipients, dd_txid, transfer_error,
                                                       nullptr, &inputs, "Paymaster carrier retirement",
                                                       /*allow_paymaster_pool_inputs=*/true)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CARRIER_REBALANCE_FAILED: " + transfer_error);
                }
                {
                    LOCK(wallet->cs_wallet);
                    const auto transaction = wallet->mapWallet.find(
                        uint256S(dd_txid));
                    if (transaction != wallet->mapWallet.end()) {
                        dd_tx = transaction->second.tx;
                    }
                }
                if (!dd_tx) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CARRIER_RETIREMENT_TRANSACTION_NOT_IN_WALLET");
                }
                const int64_t now = GetTime();
                for (const COutPoint& input : inputs) {
                    auto entry = std::find_if(entries.begin(), entries.end(),
                                              [&](const ProviderPoolEntry& candidate) { return candidate.outpoint == input; });
                    if (entry != entries.end()) {
                        entry->state = PoolEntryState::SPENT;
                        entry->updated_at = now;
                    }
                }
                std::string persist_error;
                if (!SetPaymasterProviderPoolEntries(*wallet, entries, persist_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       persist_error + "; retired carrier inputs are recoverable from wallet history");
                }
                executed = true;
            }

            CTransactionRef dgb_tx;
            CAmount dgb_fee{0};
            if (!retire_dgb.empty()) {
                auto destination = wallet->GetNewDestination(OutputType::BECH32M, "Retired Paymaster DGB");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_DESTINATION_UNAVAILABLE");
                CCoinControl coin_control;
                coin_control.m_allow_other_inputs = false;
                coin_control.m_allow_paymaster_pool_inputs = true;
                coin_control.m_min_depth = 1;
                for (const ProviderPoolEntry* entry : retire_dgb)
                    coin_control.Select(entry->outpoint);
                std::vector<CRecipient> recipients{{*destination, retired_dgb_value, /*subtract_fee=*/true}};
                auto created = CreateTransaction(*wallet, recipients, /*change_pos=*/-1,
                                                 coin_control, /*sign=*/true);
                if (!created) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_DGB_REBALANCE_FAILED: " + util::ErrorString(created).original);
                }
                std::string commit_error;
                if (!wallet->CommitTransaction(created->tx, {}, {}, &commit_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_DGB_REBALANCE_REJECTED: " + commit_error);
                }
                const int64_t now = GetTime();
                for (const ProviderPoolEntry* selected : retire_dgb) {
                    auto entry = std::find_if(entries.begin(), entries.end(),
                                              [&](const ProviderPoolEntry& candidate) { return candidate.outpoint == selected->outpoint; });
                    if (entry != entries.end()) {
                        entry->state = PoolEntryState::SPENT;
                        entry->updated_at = now;
                    }
                }
                std::string persist_error;
                if (!SetPaymasterProviderPoolEntries(*wallet, entries, persist_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       persist_error + "; retired DGB inputs are recoverable from wallet history");
                }
                dgb_tx = created->tx;
                dgb_fee = created->fee;
                executed = true;
            }

            const int64_t finance_time = GetTime();
            std::string finance_error;
            if (dd_tx) {
                const auto fee = GetProviderFinanceTransactionFee(
                    *wallet, dd_tx);
                if (!fee || !RecordProviderFinanceTransaction(
                                *wallet,
                                ProviderFinanceEventKind::POOL_RETIREMENT,
                                dd_tx, *fee, finance_time,
                                finance_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        finance_error.empty()
                            ? "PAYMASTER_RETIREMENT_FINANCE_FEE_UNAVAILABLE"
                            : finance_error);
                }
            }
            if (dgb_tx && !RecordProviderFinanceTransaction(
                              *wallet,
                              ProviderFinanceEventKind::POOL_RETIREMENT,
                              dgb_tx, DGBSatoshis{dgb_fee}, finance_time,
                              finance_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, finance_error);
            }

            UniValue pool{UniValue::VARR};
            for (const auto& entry : entries)
                pool.push_back(PoolEntryToJSON(entry));
            result.pushKV("executed", executed);
            if (!dd_txid.empty()) result.pushKV("dd_txid", dd_txid);
            if (dgb_tx) {
                result.pushKV("dgb_txid", dgb_tx->GetHash().GetHex());
                result.pushKV("network_fee_satoshis", dgb_fee);
            }
            result.pushKV("pool", std::move(pool));
            return result;
        },
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
                {"period", RPCArg::Type::STR, RPCArg::Default{"30d"}, "today, 7d, 30d, or all"},
                {"include_events", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include paginated accounting events"},
                {"limit", RPCArg::Type::NUM, RPCArg::Default{100}, "Maximum events returned (1-10000)"},
                {"cursor", RPCArg::Type::STR, RPCArg::Default{""}, "Last event id from the previous page"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Provider finance status", {
            {RPCResult::Type::STR_HEX, "provider_id", "Provider identity owning this ledger"},
            {RPCResult::Type::STR, "period", "Selected accounting period"},
            {RPCResult::Type::NUM, "service_fee_income_cents", "Confirmed DD service-fee income"},
            {RPCResult::Type::NUM, "dgb_operating_cost_satoshis", "Confirmed DGB operating costs"},
            {RPCResult::Type::NUM, "successful_transfers", "Confirmed Paymaster transfers"},
            {RPCResult::Type::NUM, "average_service_fee_cents", "Average DD income per confirmed transfer"},
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
                    {RPCResult::Type::NUM, "average_service_fee_cents", "Average DD income per confirmed transfer"},
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
                    {RPCResult::Type::STR, "kind", "transfer, setup, replenishment, retirement, or withdrawal"},
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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

            // Finance reconciliation classifies wallet transactions against
            // the active chain. Wait for the wallet notification queue first
            // so a just-confirmed payment cannot be reported as pending while
            // the rest of this RPC already observes the newer chain tip.
            wallet->BlockUntilSyncedToCurrentChain();
            size_t reconciled{0};
            std::string error;
            if (!ReconcilePaymasterProviderFinances(*wallet, reconciled, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            ProviderFinanceLedger ledger;
            ProviderIdentityRecord identity;
            ProviderBackupStatus backup;
            std::vector<ProviderPoolEntry> pool;
            ProviderMaintenanceLedger maintenance;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (!batch.ReadPaymasterIdentity(identity) ||
                    !batch.ReadPaymasterFinanceLedger(ledger)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FINANCE_LEDGER_NOT_FOUND");
                }
                if (!batch.ReadPaymasterProviderPool(pool) &&
                    batch.HasPaymasterProviderPool()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_INVALID_PROVIDER_POOL");
                }
                if (!batch.ReadPaymasterMaintenanceLedger(maintenance) &&
                    batch.HasPaymasterMaintenanceLedger()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
                }
            }
            if (!GetPaymasterProviderBackupStatus(
                    *wallet, backup, GetTime(), error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }

            const int64_t now = GetTime();
            const int64_t utc_day = (now / (24 * 60 * 60)) * (24 * 60 * 60);
            int64_t cutoff{0};
            if (period == "today") cutoff = utc_day;
            if (period == "7d") cutoff = now - 7 * 24 * 60 * 60;
            if (period == "30d") cutoff = now - 30 * 24 * 60 * 60;
            int64_t dd_income{0};
            int64_t dgb_cost{0};
            uint64_t transfers{0};
            uint64_t user_paid{0};
            uint64_t public_sponsored{0};
            uint64_t restricted_sponsored{0};
            int64_t user_paid_income{0};
            int64_t user_paid_cost{0};
            int64_t public_sponsored_income{0};
            int64_t public_sponsored_cost{0};
            int64_t restricted_sponsored_income{0};
            int64_t restricted_sponsored_cost{0};
            const auto add_amount = [&](int64_t& total, int64_t amount) {
                if (amount < 0 || total > std::numeric_limits<int64_t>::max() - amount) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FINANCE_TOTAL_OVERFLOW");
                }
                total += amount;
            };
            std::vector<const ProviderFinanceEvent*> matching_events;
            for (const ProviderFinanceEvent& event : ledger.events) {
                // A confirmed booking belongs to the UTC period in which it
                // actually became part of the active chain. Pending and
                // invalidated records have no confirmation time, so their
                // creation time remains the only useful ordering key.
                const int64_t accounting_time =
                    event.state == ProviderFinanceEventState::CONFIRMED
                    ? event.confirmed_at
                    : event.created_at;
                if (cutoff > 0 && accounting_time < cutoff) continue;
                matching_events.push_back(&event);
                if (event.state != ProviderFinanceEventState::CONFIRMED) continue;
                add_amount(dd_income, event.dd_income.value);
                add_amount(dgb_cost, event.dgb_cost.value);
                if (event.kind != ProviderFinanceEventKind::TRANSFER) continue;
                ++transfers;
                if (event.funding_model == FundingModel::USER_PAID) {
                    ++user_paid;
                    add_amount(user_paid_income, event.dd_income.value);
                    add_amount(user_paid_cost, event.dgb_cost.value);
                } else if (event.sponsorship_scope == SponsorshipScope::PUBLIC) {
                    ++public_sponsored;
                    add_amount(public_sponsored_income, event.dd_income.value);
                    add_amount(public_sponsored_cost, event.dgb_cost.value);
                } else {
                    ++restricted_sponsored;
                    add_amount(restricted_sponsored_income,
                               event.dd_income.value);
                    add_amount(restricted_sponsored_cost,
                               event.dgb_cost.value);
                }
            }

            int64_t dgb_available{0};
            int64_t dgb_reserved{0};
            int64_t dgb_pending{0};
            int64_t carrier_base{0};
            int64_t carrier_earned{0};
            int64_t carrier_withdrawable{0};
            for (const ProviderPoolEntry& entry : pool) {
                if (!IsActiveProviderPoolState(entry.state)) continue;
                if (entry.asset == PoolAsset::DGB) {
                    if (entry.state == PoolEntryState::AVAILABLE) {
                        add_amount(dgb_available, entry.dgb_value.value);
                    } else if (entry.state == PoolEntryState::PENDING_SUCCESSOR) {
                        add_amount(dgb_pending, entry.dgb_value.value);
                    } else {
                        add_amount(dgb_reserved, entry.dgb_value.value);
                    }
                    continue;
                }
                const int64_t base = std::min<int64_t>(100, entry.carrier_value.value);
                const int64_t excess = std::max<int64_t>(0, entry.carrier_value.value - base);
                add_amount(carrier_base, base);
                add_amount(carrier_earned, excess);
                if (entry.state == PoolEntryState::AVAILABLE &&
                    entry.confirmation_height > 0) {
                    add_amount(carrier_withdrawable, excess);
                }
            }
            const int64_t pending_maintenance = std::count_if(
                maintenance.records.begin(), maintenance.records.end(),
                [](const ProviderMaintenanceRecord& record) {
                    return record.state == ProviderMaintenanceState::PLANNED ||
                           record.state == ProviderMaintenanceState::BROADCAST;
                });

            UniValue result{UniValue::VOBJ};
            result.pushKV("provider_id", identity.provider_id.GetHex());
            result.pushKV("period", period);
            result.pushKV("service_fee_income_cents", dd_income);
            result.pushKV("dgb_operating_cost_satoshis", dgb_cost);
            result.pushKV("successful_transfers", transfers);
            result.pushKV("average_service_fee_cents",
                          transfers == 0 ? 0 : dd_income / static_cast<int64_t>(transfers));
            result.pushKV("user_paid_transfers", user_paid);
            result.pushKV("public_sponsored_transfers", public_sponsored);
            result.pushKV("restricted_sponsored_transfers", restricted_sponsored);
            const auto make_model_summary = [](uint64_t model_transfers,
                                               int64_t model_income,
                                               int64_t model_cost) {
                UniValue summary{UniValue::VOBJ};
                summary.pushKV("successful_transfers", model_transfers);
                summary.pushKV("service_fee_income_cents", model_income);
                summary.pushKV("dgb_operating_cost_satoshis", model_cost);
                return summary;
            };
            UniValue model_breakdown{UniValue::VOBJ};
            model_breakdown.pushKV(
                "user_paid",
                make_model_summary(user_paid, user_paid_income,
                                   user_paid_cost));
            model_breakdown.pushKV(
                "public_sponsored",
                make_model_summary(public_sponsored,
                                   public_sponsored_income,
                                   public_sponsored_cost));
            model_breakdown.pushKV(
                "restricted_sponsored",
                make_model_summary(restricted_sponsored,
                                   restricted_sponsored_income,
                                   restricted_sponsored_cost));
            result.pushKV("model_breakdown", std::move(model_breakdown));

            // The operator dashboard displays all standard periods together.
            // Compute them from the authoritative event stream in one wallet
            // snapshot so the four cards cannot disagree because of separate
            // RPC calls crossing a new block or day boundary.
            const auto make_period_summary = [&](int64_t summary_cutoff) {
                int64_t summary_income{0};
                int64_t summary_cost{0};
                uint64_t summary_transfers{0};
                uint64_t summary_user_paid{0};
                uint64_t summary_public_sponsored{0};
                uint64_t summary_restricted_sponsored{0};
                for (const ProviderFinanceEvent& event : ledger.events) {
                    if (event.state != ProviderFinanceEventState::CONFIRMED) continue;
                    if (summary_cutoff > 0 &&
                        event.confirmed_at < summary_cutoff) continue;
                    add_amount(summary_income, event.dd_income.value);
                    add_amount(summary_cost, event.dgb_cost.value);
                    if (event.kind != ProviderFinanceEventKind::TRANSFER) continue;
                    ++summary_transfers;
                    if (event.funding_model == FundingModel::USER_PAID) {
                        ++summary_user_paid;
                    } else if (event.sponsorship_scope == SponsorshipScope::PUBLIC) {
                        ++summary_public_sponsored;
                    } else {
                        ++summary_restricted_sponsored;
                    }
                }
                UniValue summary{UniValue::VOBJ};
                summary.pushKV("service_fee_income_cents", summary_income);
                summary.pushKV("dgb_operating_cost_satoshis", summary_cost);
                summary.pushKV("successful_transfers", summary_transfers);
                summary.pushKV(
                    "average_service_fee_cents",
                    summary_transfers == 0
                        ? 0
                        : summary_income / static_cast<int64_t>(summary_transfers));
                summary.pushKV("user_paid_transfers", summary_user_paid);
                summary.pushKV("public_sponsored_transfers", summary_public_sponsored);
                summary.pushKV("restricted_sponsored_transfers", summary_restricted_sponsored);
                return summary;
            };
            UniValue period_summaries{UniValue::VOBJ};
            period_summaries.pushKV("today", make_period_summary(utc_day));
            period_summaries.pushKV("7d", make_period_summary(now - 7 * 24 * 60 * 60));
            period_summaries.pushKV("30d", make_period_summary(now - 30 * 24 * 60 * 60));
            period_summaries.pushKV("all", make_period_summary(0));
            result.pushKV("period_summaries", std::move(period_summaries));
            result.pushKV("history_partially_reconstructable",
                          ledger.earlier_history_partial);
            result.pushKV("history_complete_from", ledger.history_complete_from);
            result.pushKV("backup_required", ProviderBackupRequired(backup));
            result.pushKV("last_successful_backup_at",
                          backup.last_successful_backup_at);
            result.pushKV("external_backup_acknowledged_at",
                          backup.external_backup_acknowledged_at);

            const CAmount oracle_price =
                OracleIntegration::GetCurrentOraclePriceMicroUSD();
            if (oracle_price > 0) {
                const long double income_usd =
                    static_cast<long double>(dd_income) / 100.0L;
                const long double cost_usd =
                    static_cast<long double>(dgb_cost) *
                    static_cast<long double>(oracle_price) /
                    static_cast<long double>(COIN) / 1000000.0L;
                result.pushKV("oracle_price_micro_usd", oracle_price);
                result.pushKV("valuation_time", now);
                result.pushKV("estimated_result_usd",
                              static_cast<double>(income_usd - cost_usd));
            }

            UniValue pool_capital{UniValue::VOBJ};
            pool_capital.pushKV("dgb_available_satoshis", dgb_available);
            pool_capital.pushKV("dgb_reserved_satoshis", dgb_reserved);
            pool_capital.pushKV("dgb_pending_satoshis", dgb_pending);
            pool_capital.pushKV("carrier_base_cents", carrier_base);
            pool_capital.pushKV("carrier_earned_cents", carrier_earned);
            pool_capital.pushKV("carrier_withdrawable_cents", carrier_withdrawable);
            pool_capital.pushKV("pending_maintenance_transactions", pending_maintenance);
            result.pushKV("pool_capital", std::move(pool_capital));

            if (include_events) {
                // A compact daily progression lets operator UIs graph a year
                // without exposing transaction identifiers or loading the
                // complete event ledger. The selected-period filter is still
                // respected; an all-history request deliberately returns only
                // the most recent 366 daily materializations.
                std::vector<const ProviderFinanceDailyTotals*> daily_totals;
                for (const ProviderFinanceDailyTotals& total :
                     ledger.daily_totals) {
                    if (cutoff > 0 && total.day_start < cutoff) continue;
                    daily_totals.push_back(&total);
                }
                std::sort(
                    daily_totals.begin(), daily_totals.end(),
                    [](const ProviderFinanceDailyTotals* lhs,
                       const ProviderFinanceDailyTotals* rhs) {
                        return lhs->day_start > rhs->day_start;
                    });
                if (daily_totals.size() > 366) daily_totals.resize(366);
                UniValue days{UniValue::VARR};
                for (const ProviderFinanceDailyTotals* total : daily_totals) {
                    UniValue day{UniValue::VOBJ};
                    day.pushKV("day_start", total->day_start);
                    day.pushKV("service_fee_income_cents",
                               total->dd_income.value);
                    day.pushKV("dgb_operating_cost_satoshis",
                               total->dgb_cost.value);
                    day.pushKV("successful_transfers",
                               total->successful_transfers);
                    day.pushKV("maintenance_transactions",
                               total->maintenance_transactions);
                    days.push_back(std::move(day));
                }
                result.pushKV("daily_totals", std::move(days));

                std::sort(matching_events.begin(), matching_events.end(),
                          [](const ProviderFinanceEvent* lhs,
                             const ProviderFinanceEvent* rhs) {
                              if (lhs->created_at != rhs->created_at) {
                                  return lhs->created_at > rhs->created_at;
                              }
                              return rhs->event_id < lhs->event_id;
                          });
                size_t start{0};
                if (!cursor.empty()) {
                    const auto previous = std::find_if(
                        matching_events.begin(), matching_events.end(),
                        [&](const ProviderFinanceEvent* event) {
                            return event->event_id.GetHex() == cursor;
                        });
                    if (previous == matching_events.end()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER,
                                           "PAYMASTER_INVALID_FINANCE_CURSOR");
                    }
                    start = static_cast<size_t>(
                        std::distance(matching_events.begin(), previous)) + 1;
                }
                UniValue events{UniValue::VARR};
                const size_t end = std::min(
                    matching_events.size(), start + static_cast<size_t>(limit));
                const auto kind_name = [](ProviderFinanceEventKind kind) {
                    switch (kind) {
                    case ProviderFinanceEventKind::TRANSFER: return "transfer";
                    case ProviderFinanceEventKind::POOL_SETUP: return "setup";
                    case ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT: return "replenishment";
                    case ProviderFinanceEventKind::POOL_RETIREMENT: return "retirement";
                    case ProviderFinanceEventKind::CARRIER_WITHDRAWAL: return "withdrawal";
                    }
                    return "invalid";
                };
                const auto state_name = [](ProviderFinanceEventState state) {
                    switch (state) {
                    case ProviderFinanceEventState::PENDING: return "pending";
                    case ProviderFinanceEventState::CONFIRMED: return "confirmed";
                    case ProviderFinanceEventState::INVALIDATED: return "invalidated";
                    }
                    return "invalid";
                };
                for (size_t index = start; index < end; ++index) {
                    const ProviderFinanceEvent& event = *matching_events[index];
                    UniValue item{UniValue::VOBJ};
                    item.pushKV("event_id", event.event_id.GetHex());
                    item.pushKV("kind", kind_name(event.kind));
                    item.pushKV("state", state_name(event.state));
                    item.pushKV("dd_income_cents", event.dd_income.value);
                    item.pushKV("dgb_cost_satoshis", event.dgb_cost.value);
                    item.pushKV("created_at", event.created_at);
                    if (event.confirmed_at > 0) {
                        item.pushKV("confirmed_at", event.confirmed_at);
                    }
                    if (event.kind == ProviderFinanceEventKind::TRANSFER) {
                        item.pushKV("funding_model",
                                    event.funding_model == FundingModel::USER_PAID
                                        ? "user_paid" : "sponsored");
                        if (event.funding_model == FundingModel::SPONSORED) {
                            item.pushKV("sponsorship_scope",
                                        event.sponsorship_scope == SponsorshipScope::PUBLIC
                                            ? "public" : "restricted");
                        }
                    }
                    item.pushKV("transaction_id", event.transaction_id.GetHex());
                    events.push_back(std::move(item));
                }
                result.pushKV("events", std::move(events));
                if (end < matching_events.size() && end > start) {
                    result.pushKV("next_cursor",
                                  matching_events[end - 1]->event_id.GetHex());
                }
            }
            return result;
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const UniValue& options = request.params[0];
            RPCTypeCheckObj(options,
                            {{"external_backup", UniValueType(UniValue::VBOOL)}},
                            /*fAllowNull=*/false, /*fStrict=*/true);
            if (!options.find_value("external_backup").get_bool()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_EXTERNAL_BACKUP_NOT_CONFIRMED");
            }
            const int64_t now = GetTime();
            std::string error;
            if (!AcknowledgePaymasterProviderExternalBackup(
                    *wallet, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            DigiDollar::Paymaster::ProviderBackupStatus status;
            if (!GetPaymasterProviderBackupStatus(
                    *wallet, status, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("acknowledged", true);
            result.pushKV("backup_required",
                          DigiDollar::Paymaster::ProviderBackupRequired(status));
            result.pushKV("acknowledged_at",
                          status.external_backup_acknowledged_at);
            return result;
        },
    };
}

RPCHelpMan getpaymasterinfo()
{
    return RPCHelpMan{
        "getpaymasterinfo",
        "Return this wallet's authoritative Paymaster provider configuration and staged readiness.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider information", {
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
                                                                                                                                                    {RPCResult::Type::NUM, "min_amount_cents", "Minimum recipient amount"},
                                                                                                                                                    {RPCResult::Type::NUM, "max_amount_cents", "Maximum recipient amount"},
                                                                                                                                                    {RPCResult::Type::NUM, "quote_ttl", "Quote lifetime in seconds"},
                                                                                                                                                    {RPCResult::Type::NUM, "maximum_network_fee_dgb_satoshis", "Absolute provider network-fee cap"},
                                                                                                                                                    {RPCResult::Type::STR_HEX, "policy_hash", "Canonical provider policy hash"},
                                                                                                                                                }},
                                                                        {RPCResult::Type::ARR, "readiness_errors", "Outstanding readiness gates", {{RPCResult::Type::STR, "", "Stable readiness error"}}},
                                                                    }},
        RPCExamples{HelpExampleCli("getpaymasterinfo", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            size_t finance_changes{0};
            std::string finance_error;
            if (!ReconcilePaymasterProviderFinances(
                    *wallet, finance_changes, finance_error)) {
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
                service_status = context.paymaster->GetProviderServiceStatus(
                    wallet->GetName());
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
                ProviderLiquidityStatusToJSON(readiness, GetTime()));
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

                ProviderFinanceLedger finance;
                if (WalletBatch{wallet->GetDatabase()}
                        .ReadPaymasterFinanceLedger(finance)) {
                    int64_t income{0};
                    int64_t cost{0};
                    uint64_t transfers{0};
                    bool overflow{false};
                    for (const ProviderFinanceDailyTotals& daily :
                         finance.daily_totals) {
                        if (daily.dd_income.value < 0 || daily.dgb_cost.value < 0 ||
                            income > std::numeric_limits<int64_t>::max() -
                                         daily.dd_income.value ||
                            cost > std::numeric_limits<int64_t>::max() -
                                       daily.dgb_cost.value ||
                            transfers > std::numeric_limits<uint64_t>::max() -
                                            daily.successful_transfers) {
                            overflow = true;
                            break;
                        }
                        income += daily.dd_income.value;
                        cost += daily.dgb_cost.value;
                        transfers += daily.successful_transfers;
                    }
                    if (!overflow) {
                        UniValue summary{UniValue::VOBJ};
                        summary.pushKV("service_fee_income_cents", income);
                        summary.pushKV("dgb_operating_cost_satoshis", cost);
                        summary.pushKV("successful_transfers", transfers);
                        summary.pushKV("history_partially_reconstructable",
                                       finance.earlier_history_partial);
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
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            return result;
        },
    };
}

RPCHelpMan getpaymasteroffers()
{
    return RPCHelpMan{
        "getpaymasteroffers",
        "Return a locally filtered and deterministically sorted snapshot of public Paymaster offers.\n"
        "This does not contact a provider or reserve liquidity.\n",
        {
            {"amount_cents", RPCArg::Type::NUM, RPCArg::Optional::NO, "Recipient amount, or exact total DD outflow when subtract_paymaster_fee_from_amount is true"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Optional local offer-preview semantics", {
                {"subtract_paymaster_fee_from_amount", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Treat amount_cents as exact total DD outflow and derive the recipient amount"},
            }},
        },
        RPCResult{RPCResult::Type::ARR, "", "Eligible public offers", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Sorted offer", {
                                                                                                                                          {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                                                                          {RPCResult::Type::STR, "display_name", "Untrusted display label"},
                                                                                                                                          {RPCResult::Type::STR, "endpoint", "Authenticated P2P endpoint"},
                                                                                                                                          {RPCResult::Type::STR_HEX, "offer_id", "Public offer identifier"},
                                                                                                                                          {RPCResult::Type::STR_HEX, "policy_hash", "Bound provider policy"},
                                                                                                                                          {RPCResult::Type::STR, "funding_model", "sponsored or user_paid"},
                                                                                                                                          {RPCResult::Type::NUM, "fee_rate_bps", "Service-fee rate in basis points"},
                                                                                                                                          {RPCResult::Type::NUM, "payment_cents", "Requested recipient amount"},
                                                                                                                                          {RPCResult::Type::NUM, "service_fee_cents", "Exact rounded service fee"},
                                                                                                                                          {RPCResult::Type::NUM, "user_total_cents", "Exact total charged to the user"},
                                                                                                                                          {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", "Whether amount_cents was treated as an exact total outflow"},
                                                                                                                                          {RPCResult::Type::NUM_TIME, "expires_at", "Announcement expiration"},
                                                                                                                                          {RPCResult::Type::BOOL, "reputation_sufficient_data", "Whether enough local observations exist"},
                                                                                                                                          {RPCResult::Type::NUM, "success_rate_basis_points", /*optional=*/true, "Observed success rate when statistically meaningful"},
                                                                                                                                          {RPCResult::Type::NUM, "latency_ewma_ms", "Locally observed latency estimate"},
                                                                                                                                      }}}},
        RPCExamples{HelpExampleCli("getpaymasteroffers", "1000") +
                    HelpExampleCli("getpaymasteroffers", "5000 '{\"subtract_paymaster_fee_from_amount\":true}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
            if (!request.params[1].isNull()) {
                const UniValue& options = request.params[1].get_obj();
                RPCTypeCheckObj(
                    options,
                    {{"subtract_paymaster_fee_from_amount",
                      UniValueType(UniValue::VBOOL)}},
                    /*fAllowNull=*/true, /*fStrict=*/true);
                if (!options.find_value(
                        "subtract_paymaster_fee_from_amount").isNull()) {
                    subtract_fee = options.find_value(
                        "subtract_paymaster_fee_from_amount").get_bool();
                }
            }
            const DigiDollar::Paymaster::DDCents requested_amount{
                request.params[0].getInt<int64_t>()};
            const auto candidates = subtract_fee
                ? DigiDollar::Paymaster::BuildGrossOfferCandidates(
                      context.paymaster->GetDirectory().List(now),
                      requested_amount,
                      DigiDollar::Paymaster::FUNDING_MODEL_ALL,
                      DigiDollar::Paymaster::DDCents{
                          DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS},
                      reputation, now, error)
                : DigiDollar::Paymaster::BuildOfferCandidates(
                      context.paymaster->GetDirectory().List(now),
                      requested_amount,
                      DigiDollar::Paymaster::FUNDING_MODEL_ALL,
                      DigiDollar::Paymaster::DDCents{
                          DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS},
                      reputation, now, error);
            if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            UniValue result{UniValue::VARR};
            for (const auto& candidate : candidates) {
                UniValue offer{UniValue::VOBJ};
                offer.pushKV("provider_id", candidate.provider_id.GetHex());
                offer.pushKV("display_name", candidate.display_name);
                offer.pushKV("endpoint", candidate.endpoint.ToStringAddrPort());
                offer.pushKV("offer_id", candidate.terms.offer_id.GetHex());
                offer.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
                offer.pushKV("funding_model", candidate.terms.funding_model == DigiDollar::Paymaster::FundingModel::SPONSORED ? "sponsored" : "user_paid");
                offer.pushKV("fee_rate_bps", candidate.terms.fee_rate_bps);
                offer.pushKV("payment_cents", candidate.payment.value);
                offer.pushKV("service_fee_cents", candidate.service_fee.value);
                offer.pushKV("user_total_cents", candidate.user_total.value);
                offer.pushKV("subtract_paymaster_fee_from_amount", subtract_fee);
                offer.pushKV("expires_at", candidate.announcement_expires_at);
                offer.pushKV("reputation_sufficient_data", candidate.reliability.sufficient_data);
                if (candidate.reliability.sufficient_data) {
                    offer.pushKV("success_rate_basis_points", candidate.reliability.success_rate_basis_points);
                }
                offer.pushKV("latency_ewma_ms", candidate.latency_ewma_ms);
                result.push_back(std::move(offer));
            }
            return result;
        },
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
                                                                                            {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Selected provider; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Selected offer; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Selected provider policy; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
                                                                                            {RPCResult::Type::STR, "session_state", "Authoritative wallet session state"},
                                                                                            {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Outstanding asynchronous phase"},
                                                                                            {RPCResult::Type::BOOL, "final", "Whether the session is terminal"},
                                                                                            {RPCResult::Type::STR, "broadcast_state", "not_attempted, unknown, accepted_mempool, accepted_stempool, or confirmed"},
                                                                                            {RPCResult::Type::STR, "confirmation_state", "unconfirmed, payment_confirmed, recovery_confirmed, or conflicted"},
                                                                                            {RPCResult::Type::NUM_TIME, "created_at", "Persistent session creation time"},
                                                                                            {RPCResult::Type::NUM_TIME, "updated_at", "Last durable session update"},
                                                                                            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final payment transaction id"},
                                                                                            {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known idempotent self-recovery transaction id"},
                                                                                            {RPCResult::Type::ARR, "reserved_user_inputs", "Hard-reserved USER_DD inputs", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reserved input", {
                                                                                                                                                                                                                                                 {RPCResult::Type::STR_HEX, "txid", "Input transaction"},
                                                                                                                                                                                                                                                 {RPCResult::Type::NUM, "vout", "Input output index"},
                                                                                                                                                                                                                                             }}}},
                                                                                            {RPCResult::Type::NUM, "provider_attempts", "Number of persistent provider attempts"},
                                                                                            {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Recipient amount; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact rounded service fee; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Total DD selected for payment and fee; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::NUM_TIME, "expires_at", /*optional=*/true, "Intent expiration; omitted for a terminal tombstone"},
                                                                                            {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the request is queued to a connected v2 peer"},
                                                                                            {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a short-lived connection was requested"},
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
                             {"selected_inputs", UniValueType(UniValue::VARR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);
            const PaymasterId provider_id = ParseHashV(request.params[0], "provider_id");
            const std::string request_id = options.find_value("request_id").get_str();
            if (!IsCanonicalRequestId(request_id)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "request_id must be a canonical lowercase UUID");
            }
            const int64_t payment = options.find_value("amount_cents").getInt<int64_t>();
            const bool subtract_fee =
                !options.find_value("subtract_paymaster_fee_from_amount").isNull() &&
                options.find_value("subtract_paymaster_fee_from_amount").get_bool();
            const bool send_all =
                !options.find_value("send_all_spendable_dd").isNull() &&
                options.find_value("send_all_spendable_dd").get_bool();
            const int64_t requested_amount =
                options.find_value("requested_amount_cents").isNull()
                    ? payment
                    : options.find_value("requested_amount_cents").getInt<int64_t>();
            const int64_t fee_cap = options.find_value("maximum_paymaster_fee_cents").getInt<int64_t>();
            if (payment < 100 || payment > MAX_DD_OUTPUT_CENTS ||
                requested_amount < 100 || requested_amount > MAX_DD_OUTPUT_CENTS ||
                fee_cap < 0 || fee_cap > MAX_DD_OUTPUT_CENTS) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Paymaster amount or fee cap is out of range");
            }
            if (send_all && !subtract_fee) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
            }
            if ((!subtract_fee && requested_amount != payment) ||
                (subtract_fee && requested_amount < payment)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_INVALID_CLIENT_PAYMENT_ORDER");
            }
            FeeMode requested_mode{FeeMode::PAYMASTER};
            if (!options.find_value("fee_mode").isNull()) {
                const std::string mode = options.find_value("fee_mode").get_str();
                if (mode == "auto")
                    requested_mode = FeeMode::AUTO;
                else if (mode != "paymaster") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_mode must be paymaster or auto");
                }
            }
            PrivacyProfile privacy{PrivacyProfile::STANDARD};
            if (!options.find_value("privacy").isNull()) {
                const std::string profile = options.find_value("privacy").get_str();
                if (profile == "high")
                    privacy = PrivacyProfile::HIGH;
                else if (profile != "standard") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "privacy must be standard or high");
                }
            }
            SelectionMode selection{SelectionMode::LOWEST_TOTAL_COST};
            if (!options.find_value("selection").isNull()) {
                const std::string policy = options.find_value("selection").get_str();
                if (policy == "privacy_weighted")
                    selection = SelectionMode::PRIVACY_WEIGHTED;
                else if (policy != "lowest_total_cost") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "selection must be lowest_total_cost or privacy_weighted");
                }
            }
            const int maximum_attempts = options.find_value("maximum_provider_attempts").isNull() ? ((privacy == PrivacyProfile::HIGH ||
                                                                                                      !options.find_value("sponsorship_capability").isNull()) ?
                                                                                                         1 :
                                                                                                         3) :
                                                                                                    options.find_value("maximum_provider_attempts").getInt<int>();
            if (maximum_attempts < 1 || maximum_attempts > 16 ||
                (privacy == PrivacyProfile::HIGH && maximum_attempts != 1)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
            }
            if (!CheckPaymasterPrivacyReadiness(privacy, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }
            const std::vector<COutPoint> requested_inputs =
                ParsePaymasterInputs(options.find_value("selected_inputs"));
            const std::string address = options.find_value("address").get_str();
            const CDigiDollarAddress dd_address{address};
            if (!dd_address.IsValidForCurrentNetwork()) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigiDollar address for this network");
            }
            const CScript recipient_script = GetScriptForDestination(dd_address.GetDigiDollarDestination());
            const std::optional<uint256> requested_offer = options.find_value("offer_id").isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "offer_id")};

            const bool have_restricted_key = !options.find_value("provider_identity_key").isNull();
            const bool have_restricted_descriptor =
                !options.find_value("restricted_service_descriptor").isNull();
            const bool have_restricted_capability =
                !options.find_value("sponsorship_capability").isNull();
            const bool restricted = have_restricted_key || have_restricted_descriptor ||
                                    have_restricted_capability;
            if (restricted && !(have_restricted_key && have_restricted_descriptor &&
                                have_restricted_capability)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "provider_identity_key, restricted_service_descriptor, and sponsorship_capability "
                                   "must be supplied together");
            }
            if (restricted && maximum_attempts != 1) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Restricted sponsorship requires maximum_provider_attempts=1");
            }
            std::optional<XOnlyPubKey> restricted_identity_key;
            std::optional<RestrictedServiceDescriptor> restricted_descriptor;
            std::optional<SponsorshipCapability> restricted_capability;
            std::string error;
            PaymasterStore store{*wallet};
            const int64_t now = GetTime();
            PaymentSession durable_session;
            const bool have_durable_session =
                store.GetSessionByRequestId(request_id, durable_session);
            if (have_durable_session && !IsTerminal(durable_session.state)) {
                ProviderAttempt durable_attempt;
                PaymentIntent durable_intent;
                PaymasterQuote durable_quote;
                if (FindDurableClientAuthorizationAttempt(
                        *wallet, store, durable_session, GetTime(), durable_attempt,
                        durable_intent, durable_quote, error)) {
                    if (durable_attempt.provider_id != provider_id ||
                        durable_intent.request_id != request_id ||
                        durable_intent.session_id != durable_session.session_id ||
                        durable_intent.canonical_request_hash !=
                            durable_session.canonical_request_hash ||
                        durable_intent.requested_fee_mode != requested_mode ||
                        durable_intent.privacy_profile != privacy ||
                        durable_intent.selection_mode != selection ||
                        durable_attempt.privacy_profile != privacy ||
                        durable_intent.recipient_script != recipient_script ||
                        durable_intent.recipient_amount != DDCents{payment} ||
                        (durable_session.requested_amount.value == 0
                             ? (subtract_fee || send_all || requested_amount != payment)
                             : (durable_session.requested_amount != DDCents{requested_amount} ||
                                durable_session.subtract_paymaster_fee_from_amount != subtract_fee ||
                                durable_session.send_all_spendable_dd != send_all)) ||
                        durable_intent.user_dd_inputs != requested_inputs ||
                        durable_quote.service_fee.value > fee_cap ||
                        (requested_offer &&
                         durable_intent.offer_id != *requested_offer)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
                    }
                    if (!DrainClientSessionEquivocations(
                            *context.paymaster, store, durable_session,
                            error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    return DurableClientAuthorizationToJSON(
                        durable_session, durable_attempt, durable_intent,
                        durable_quote);
                }
                if (!error.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, durable_session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (restricted) {
                const std::vector<unsigned char> key_bytes =
                    ParseHexV(options.find_value("provider_identity_key"), "provider_identity_key");
                if (key_bytes.size() != 32) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "provider_identity_key must be exactly 32 bytes");
                }
                restricted_identity_key.emplace(MakeUCharSpan(key_bytes));
                RestrictedServiceDescriptor descriptor;
                SponsorshipCapability capability;
                if (!restricted_identity_key->IsFullyValid() ||
                    !DeserializePaymasterHex(options.find_value("restricted_service_descriptor"),
                                             "RESTRICTED_DESCRIPTOR", descriptor, error) ||
                    !DeserializePaymasterHex(options.find_value("sponsorship_capability"),
                                             "SPONSORSHIP_CAPABILITY", capability, error)) {
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                       error.empty() ? "PAYMASTER_INVALID_RESTRICTED_IDENTITY" : error);
                }
                if (provider_id != descriptor.provider_id ||
                    (requested_offer && *requested_offer != descriptor.offer_id) ||
                    !ValidateRestrictedServiceDescriptor(descriptor, *restricted_identity_key,
                                                         Params().GenesisBlock().GetHash(), GetTime(), error,
                                                         Params().GetChainType() == ChainType::REGTEST) ||
                    !ValidateSponsorshipCapability(capability, descriptor, recipient_script,
                                                   DDCents{payment}, capability.payment_request_nonce,
                                                   Params().GenesisBlock().GetHash(), GetTime(), error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       error.empty() ? "PAYMASTER_RESTRICTED_BINDING_MISMATCH" : error);
                }
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman ||
                    !ValidateRestrictedAdmissionProofs(descriptor, *restricted_identity_key,
                                                       *node->chainman, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       error.empty() ? "PAYMASTER_RESTRICTED_ADMISSION_UNAVAILABLE" : error);
                }
                if (privacy == PrivacyProfile::HIGH && !descriptor.p2p_endpoint.IsTor()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "High privacy requires an onion restricted provider");
                }
                restricted_descriptor = std::move(descriptor);
                restricted_capability = std::move(capability);
            }

            // Resolve an exact terminal retry from local durable authority
            // before consulting the live directory. The provider resources
            // used by the completed transaction are expected to be spent, so
            // requiring a fresh announcement/capacity check here would make
            // the idempotent tombstone dependent on remote state. All fields
            // which define the local order are still committed below; a hash
            // mismatch remains a hard conflict in CreateOrJoinSession().
            HashWriter request_hasher = TaggedHash("DigiByte Paymaster RPC Request v1");
            // The session hash commits the provider-independent local order.
            // In subtract mode the exact provider-dependent recipient and fee
            // remain bound by each signed intent/quote/authorization manifest,
            // allowing a safe pre-signature provider fallback to produce a
            // new exact confirmation under the same gross order.
            request_hasher << request_id << recipient_script
                           << DDCents{subtract_fee ? requested_amount : payment}
                           << DDCents{fee_cap} << requested_inputs
                           << static_cast<uint8_t>(requested_mode)
                           << static_cast<uint8_t>(privacy)
                           << static_cast<uint8_t>(selection) << maximum_attempts;
            if (subtract_fee || send_all) {
                request_hasher << std::string{"gross-amount-v1"}
                               << DDCents{requested_amount}
                               << subtract_fee << send_all;
            }
            if (restricted) {
                request_hasher << *restricted_identity_key
                               << GetRestrictedDescriptorSignatureHash(*restricted_descriptor)
                               << GetSponsorshipCapabilityHash(*restricted_capability);
            }
            const uint256 canonical_request_hash = request_hasher.GetSHA256();
            if (have_durable_session && IsTerminal(durable_session.state)) {
                if (durable_session.canonical_request_hash !=
                        canonical_request_hash ||
                    durable_session.fee_mode_requested != requested_mode) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_REQUEST_ID_CONFLICT");
                }
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, durable_session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                return SessionToJSON(durable_session, &store);
            }

            const DDCents effective_fee_cap =
                EffectiveClientServiceFeeCap(*wallet, DDCents{fee_cap}, error);
            if (effective_fee_cap.value < 0) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            std::vector<PaymasterReliabilityRecord> records;
            if (!store.ListProviderReliability(records)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
            }
            std::map<PaymasterId, PaymasterReliabilityRecord> reputation;
            for (auto& record : records)
                reputation.emplace(record.provider_id, std::move(record));
            OfferCandidate candidate;
            if (restricted) {
                const auto& descriptor = *restricted_descriptor;
                candidate.provider_id = descriptor.provider_id;
                candidate.identity_key = *restricted_identity_key;
                candidate.display_name = descriptor.sponsor_display_name;
                candidate.endpoint = descriptor.p2p_endpoint;
                candidate.announcement_sequence = descriptor.admission_sequence;
                candidate.announcement_expires_at = descriptor.expires_at;
                candidate.terms = OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                             FundingModel::SPONSORED,
                                             SponsorshipScope::RESTRICTED, 0,
                                             DDCents{payment}, DDCents{payment}};
                candidate.payment = DDCents{payment};
                candidate.service_fee = DDCents{0};
                candidate.user_total = DDCents{payment};
            } else {
                const auto candidates = BuildOfferCandidates(
                    context.paymaster->GetDirectory().List(now), DDCents{payment}, FUNDING_MODEL_ALL,
                    effective_fee_cap, reputation, now, error);
                if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                const auto candidate_it = std::find_if(candidates.begin(), candidates.end(),
                                                       [&](const OfferCandidate& entry) {
                                                           return entry.provider_id == provider_id &&
                                                                  (privacy != PrivacyProfile::HIGH || entry.endpoint.IsTor()) &&
                                                                  (!requested_offer || entry.terms.offer_id == *requested_offer);
                                                       });
                if (candidate_it == candidates.end()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_REQUESTED_OFFER_UNAVAILABLE");
                }
                candidate = *candidate_it;
            }
            if ((subtract_fee &&
                 candidate.user_total != DDCents{requested_amount}) ||
                (!subtract_fee && candidate.payment != DDCents{requested_amount})) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "PAYMASTER_NO_EXACT_GROSS_OFFER");
            }

            PaymentSession session;
            const auto create_result = store.CreateOrJoinSession(
                request_id, canonical_request_hash, requested_mode, now, session, error);
            if (create_result == CreatePaymasterSessionResult::CONFLICT ||
                create_result == CreatePaymasterSessionResult::DATABASE_ERROR) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (create_result == CreatePaymasterSessionResult::FINAL_TOMBSTONE) {
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                return SessionToJSON(session, &store);
            }
            if (!store.BindClientPaymentOrder(
                    request_id, DDCents{requested_amount}, subtract_fee,
                    send_all, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            session.requested_amount = DDCents{requested_amount};
            session.subtract_paymaster_fee_from_amount = subtract_fee;
            session.send_all_spendable_dd = send_all;

            ProviderAttempt attempt;
            PaymentIntent intent;
            bool resume_attempt{false};
            for (auto it = session.attempt_ids.rbegin(); it != session.attempt_ids.rend(); ++it) {
                ProviderAttempt existing;
                if (!store.GetAttempt(*it, existing) || existing.unsigned_intent.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                PaymentIntent existing_intent;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, existing.unsigned_intent};
                    stream >> existing_intent;
                    if (!stream.empty()) throw std::ios_base::failure("trailing intent data");
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                if (existing.provider_id == provider_id &&
                    existing_intent.offer_id == candidate.terms.offer_id &&
                    existing.state != AttemptState::REJECTED &&
                    existing.state != AttemptState::QUOTE_EXPIRED) {
                    if (existing_intent.canonical_request_hash != canonical_request_hash ||
                        existing_intent.requested_fee_mode != requested_mode ||
                        existing_intent.privacy_profile != privacy ||
                        existing_intent.selection_mode != selection ||
                        existing.privacy_profile != privacy) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
                    }
                    attempt = std::move(existing);
                    intent = std::move(existing_intent);
                    resume_attempt = true;
                    break;
                }
            }
            if (!resume_attempt) {
                if (session.attempt_ids.size() >= static_cast<size_t>(maximum_attempts)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_MAXIMUM_PROVIDER_ATTEMPTS_REACHED");
                }
                if (!session.attempt_ids.empty() &&
                    session.state != SessionState::INPUTS_RESERVED) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FALLBACK_REQUIRES_EXPLICIT_ABANDON");
                }
                DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
                if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
                std::vector<COutPoint> selected_inputs;
                std::vector<CAmount> selected_amounts;
                CAmount selected_total{0};
                if (!session.attempt_ids.empty()) {
                    if (requested_inputs != session.user_inputs) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_FALLBACK_INPUT_CONFLICT");
                    }
                    selected_inputs = session.user_inputs;
                    for (const COutPoint& outpoint : selected_inputs) {
                        const CAmount input_amount = dd_wallet->GetDDFromUTXO(outpoint);
                        if (input_amount <= 0 ||
                            input_amount > std::numeric_limits<CAmount>::max() - selected_total) {
                            throw JSONRPCError(RPC_WALLET_ERROR,
                                               "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
                        }
                        selected_amounts.push_back(input_amount);
                        selected_total += input_amount;
                    }
                    if (selected_total < candidate.user_total.value) {
                        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                                           "PAYMASTER_FALLBACK_FEE_EXCEEDS_RESERVED_INPUTS");
                    }
                } else if (!dd_wallet->SelectDDCoins(candidate.user_total.value, requested_inputs,
                                                     selected_inputs, selected_total,
                                                     &selected_amounts, &error)) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error);
                }
                CScript change_script;
                if (selected_total > candidate.user_total.value) {
                    const auto change = wallet->GetNewDestination(OutputType::BECH32M,
                                                                  "Paymaster DD change");
                    if (!change) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                    change_script = GetScriptForDestination(*change);
                }
                PaymentIntentParameters parameters;
                parameters.genesis_hash = Params().GenesisBlock().GetHash();
                parameters.request_id = request_id;
                parameters.session_id = session.session_id;
                parameters.canonical_request_hash = canonical_request_hash;
                parameters.requested_fee_mode = requested_mode;
                parameters.privacy_profile = privacy;
                parameters.selection_mode = selection;
                if (restricted) {
                    parameters.client_nonce = restricted_capability->payment_request_nonce;
                    parameters.sponsorship_authorization_hash =
                        GetSponsorshipCapabilityHash(*restricted_capability);
                } else {
                    do {
                        parameters.client_nonce = GetRandHash();
                    } while (parameters.client_nonce.IsNull());
                }
                parameters.user_dd_inputs = selected_inputs;
                parameters.recipient_script = recipient_script;
                parameters.user_dd_change_script = change_script;
                parameters.expires_at = std::min(
                    SaturatingAddSeconds(now, DEFAULT_QUOTE_TTL_SECONDS),
                    candidate.announcement_expires_at);
                auto unsigned_intent = BuildUnsignedPaymentIntent(candidate, parameters, now, error);
                if (!unsigned_intent) throw JSONRPCError(RPC_WALLET_ERROR, error);
                intent = std::move(*unsigned_intent);
                do {
                    attempt.attempt_id = GetRandHash();
                } while (attempt.attempt_id.IsNull());
                attempt.provider_id = provider_id;
                attempt.provider_identity_key = candidate.identity_key;
                attempt.provider_endpoint = candidate.endpoint.ToStringAddrPort();
                attempt.privacy_profile = privacy;
                attempt.client_nonce = intent.client_nonce;
                attempt.sponsorship_capability_hash = intent.sponsorship_authorization_hash;
                attempt.unsigned_intent = SerializePaymentIntent(intent);
                attempt.created_at = attempt.updated_at = now;
                if (!store.PreparePaymentIntent(request_id, attempt, now, error) ||
                    !store.GetSessionByRequestId(request_id, session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            node::NodeContext* node = wallet->chain().context();
            if (!node || !node->connman || !node->chainman) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            }
            PaymasterCapacityRequest capacity_request;
            if (attempt.capacity_request.empty()) {
                capacity_request.genesis_hash = Params().GenesisBlock().GetHash();
                capacity_request.provider_id = provider_id;
                capacity_request.request_id = request_id;
                capacity_request.session_id = session.session_id;
                capacity_request.client_nonce = attempt.client_nonce;
                capacity_request.funding_model = intent.funding_model;
                capacity_request.requires_carrier =
                    intent.funding_model == FundingModel::USER_PAID &&
                    candidate.service_fee.value > 0 &&
                    candidate.service_fee.value < 100;
                capacity_request.requested_slots = 1;
                capacity_request.created_at = now;
                capacity_request.expires_at = std::min(
                    intent.expires_at,
                    SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
                if (capacity_request.expires_at <= now) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_REQUEST_EXPIRED");
                }
                const std::vector<unsigned char> capacity_bytes =
                    SerializeCapacityRequest(capacity_request);
                if (!store.PrepareCapacityRequest(request_id, attempt.attempt_id,
                                                  capacity_bytes, now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       error.empty() ? "PAYMASTER_ATTEMPT_NOT_FOUND" : error);
                }
            } else {
                try {
                    CDataStream stream{attempt.capacity_request, SER_NETWORK,
                                       ::PROTOCOL_VERSION};
                    stream >> capacity_request;
                    if (!stream.empty() ||
                        SerializeCapacityRequest(capacity_request) !=
                            attempt.capacity_request) {
                        throw std::ios_base::failure("non-canonical capacity request");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT");
                }
            }

            PaymasterCapacityProof capacity_proof;
            bool capacity_ready = !attempt.capacity_snapshot.snapshot_id.IsNull();
            if (!capacity_ready) {
                // Persist a simultaneously queued signed conflict before the
                // acceptance loop can discard either locally invalid proof.
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                auto proof_lease = context.paymaster->LeaseCapacityProofs(
                    provider_id, attempt.client_nonce,
                    MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
                const auto proofs = SelectCapacityProofMessages(
                    proof_lease.Messages(), provider_id, request_id,
                    session.session_id, attempt.client_nonce);
                bool capacity_equivocation{false};
                if (!PersistPendingCapacityEquivocationPair(
                        store, attempt.attempt_id, proofs,
                        capacity_equivocation, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (capacity_equivocation) {
                    if (!AcknowledgeEquivocationMessages(
                            *context.paymaster, proofs, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_EQUIVOCATION");
                }
                std::string rejected_proof_error;
                for (const DirectMessage& direct : proofs) {
                    const auto* received =
                        std::get_if<PaymasterCapacityProof>(&direct.payload);
                    if (!received) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_proof_error =
                            "PAYMASTER_INVALID_CAPACITY_PROOF";
                        continue;
                    }

                    bool claim_equivocation{false};
                    const std::vector<unsigned char> encoded_proof =
                        SerializeCapacityProof(*received);
                    if (!store.StageCapacityProofClaimCandidate(
                            attempt.attempt_id, encoded_proof,
                            direct.received_at, claim_equivocation, error)) {
                        if (error !=
                            "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_proof_error =
                            "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
                        continue;
                    }
                    if (claim_equivocation) {
                        if (!AcknowledgeEquivocationMessages(
                                *context.paymaster, proofs, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_CAPACITY_EQUIVOCATION");
                    }
                    if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_ATTEMPT_NOT_FOUND");
                    }

                    std::string proof_error;
                    if (!ValidateCapacityProofAgainstChainstate(
                            *received, capacity_request,
                            attempt.provider_identity_key, *node->chainman,
                            now, proof_error)) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_proof_error =
                            proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
                        continue;
                    }
                    capacity_proof = *received;
                    ValidatedCapacitySnapshot snapshot;
                    snapshot.snapshot_id = capacity_proof.snapshot_id;
                    snapshot.resource_commitment =
                        GetCapacityResourceCommitment(capacity_proof);
                    snapshot.provider_id = capacity_proof.provider_id;
                    snapshot.client_nonce = capacity_proof.client_nonce;
                    snapshot.funding_model = capacity_proof.funding_model;
                    snapshot.requires_carrier = capacity_proof.requires_carrier;
                    snapshot.request_hash = Hash(attempt.capacity_request);
                    snapshot.capacity_proof = SerializeCapacityProof(capacity_proof);
                    snapshot.created_at = capacity_proof.created_at;
                    snapshot.expires_at = capacity_proof.expires_at;
                    snapshot.validated_at = now;
                    if (!store.CommitValidatedCapacitySnapshot(
                            request_id, attempt.attempt_id, snapshot, now, error) ||
                        !store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    capacity_ready = true;
                    break;
                }
                if (!capacity_ready && !rejected_proof_error.empty()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       rejected_proof_error);
                }
            }
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (!capacity_ready) {
                NodeId peer_id{-1};
                node->connman->ForEachNode([&](CNode* peer) {
                    if (peer_id == -1 && peer->IsPaymasterConn() &&
                        peer->addr == candidate.endpoint) {
                        peer_id = peer->GetId();
                    }
                });
                bool capacity_queued{false};
                bool capacity_connection_pending{false};
                if (peer_id == -1) {
                    capacity_connection_pending = node->connman->AddConnection(
                        candidate.endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
                        /*paymaster_high_privacy=*/privacy == PrivacyProfile::HIGH);
                } else {
                    const uint256 message_id = Hash(attempt.capacity_request);
                    capacity_queued =
                        context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                        context.paymaster->QueueCapacityRequest(
                            peer_id, message_id, attempt.capacity_request.size(),
                            capacity_request, now);
                    if (!capacity_queued) {
                        throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED,
                                           "PAYMASTER_DIRECT_QUEUE_FULL");
                    }
                    node->connman->WakeMessageHandler();
                }
                if (!store.GetSessionByRequestId(request_id, session)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_SESSION_NOT_FOUND");
                }
                UniValue result = SessionToJSON(session);
                result.pushKV("provider_id", provider_id.GetHex());
                result.pushKV("offer_id", candidate.terms.offer_id.GetHex());
                result.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
                result.pushKV("funding_model",
                              intent.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
                result.pushKV("payment_cents", candidate.payment.value);
                result.pushKV("service_fee_cents", candidate.service_fee.value);
                result.pushKV("user_total_cents", candidate.user_total.value);
                result.pushKV("expires_at", intent.expires_at);
                result.pushKV("queued", capacity_queued);
                result.pushKV("connection_pending", capacity_connection_pending);
                result.pushKV("capacity_pending", true);
                return result;
            }
            try {
                CDataStream stream{attempt.capacity_snapshot.capacity_proof,
                                   SER_NETWORK, ::PROTOCOL_VERSION};
                stream >> capacity_proof;
                if (!stream.empty() ||
                    SerializeCapacityProof(capacity_proof) !=
                        attempt.capacity_snapshot.capacity_proof) {
                    throw std::ios_base::failure("non-canonical capacity proof");
                }
            } catch (const std::ios_base::failure&) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PERSISTED_CAPACITY_PROOF_CORRUPT");
            }
            // Re-run the same full firewall immediately before any intent
            // signature or network disclosure. A now-spent capacity outpoint
            // invalidates the attempt without exposing the user's intent.
            if (!ValidateCapacityProofAgainstChainstate(
                    capacity_proof, capacity_request, attempt.provider_identity_key,
                    *node->chainman, now, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            bool queued{false};
            bool connection_pending{false};
            bool quote_received{false};
            PaymasterQuoteRequest quote_request;
            std::vector<unsigned char> request_bytes;
            if (attempt.quote_request.empty()) {
                if (wallet->IsLocked()) {
                    if (session.state == SessionState::INPUTS_RESERVED &&
                        !store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                                 PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else {
                    if ((session.state == SessionState::INPUTS_RESERVED ||
                         session.state == SessionState::AWAITING_WALLET_UNLOCK) &&
                        !store.TransitionSession(request_id, SessionState::AWAITING_USER_SIGNATURE,
                                                 PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    if (!DrainClientAttemptEquivocations(
                            *context.paymaster, store, session, attempt,
                            error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    std::vector<XOnlyPubKey> output_keys;
                    if (!SignPaymentIntentInputs(*wallet, intent, now, output_keys, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    std::vector<std::vector<unsigned char>> signatures;
                    for (const auto& proof : intent.user_input_proofs)
                        signatures.push_back(proof.signature);
                    intent.user_input_proofs.clear();
                    auto finalized = FinalizeQuoteRequest(
                        intent, output_keys, signatures, restricted_descriptor,
                        restricted_capability, now, error);
                    if (!finalized) throw JSONRPCError(RPC_WALLET_ERROR, error);
                    quote_request = std::move(*finalized);
                    request_bytes = SerializeQuoteRequest(quote_request);
                    if (!store.FinalizePaymentIntent(request_id, attempt.attempt_id,
                                                     SerializeRedactedQuoteRequest(quote_request),
                                                     now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_ATTEMPT_NOT_FOUND");
                    }
                }
            } else {
                try {
                    CDataStream stream{attempt.quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
                    stream >> quote_request;
                    if (!stream.empty()) throw std::ios_base::failure("trailing request data");
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PERSISTED_REQUEST_CORRUPT");
                }
                quote_request.restricted_descriptor = restricted_descriptor;
                quote_request.restricted_capability = restricted_capability;
                if (!ValidateQuoteRequestEnvelope(quote_request, Params().GenesisBlock().GetHash(),
                                                  now, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                request_bytes = SerializeQuoteRequest(quote_request);
            }

            if (!request_bytes.empty()) {
                // Pairwise equivocation is signature/binding evidence, not an
                // authorization decision. Record it before any rejected quote
                // is acknowledged by the deeper local spend firewall.
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                auto response_lease = context.paymaster->LeaseQuoteResponses(
                    request_id, session.session_id, provider_id,
                    GetPaymentIntentHash(quote_request.intent),
                    MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
                const auto& responses = response_lease.Messages();
                bool quote_equivocation{false};
                if (!PersistPendingQuoteEquivocationPair(
                        store, attempt.attempt_id, responses,
                        quote_equivocation, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (quote_equivocation) {
                    if (!AcknowledgeEquivocationMessages(
                            *context.paymaster, responses, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_QUOTE_EQUIVOCATION");
                }
                std::string rejected_quote_error;
                int rejected_quote_code{RPC_INVALID_PARAMETER};
                for (const DirectMessage& direct : responses) {
                    const auto* response =
                        std::get_if<PaymasterQuoteResponse>(&direct.payload);
                    if (!response) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_quote_error =
                            "PAYMASTER_INVALID_QUOTE_RESPONSE";
                        continue;
                    }

                    bool claim_equivocation{false};
                    const std::vector<unsigned char> encoded_quote =
                        SerializeQuoteResponse(*response);
                    if (!store.StageQuoteResponseClaimCandidate(
                            attempt.attempt_id, encoded_quote,
                            direct.received_at, claim_equivocation, error)) {
                        if (error !=
                            "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE") {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_quote_error =
                            "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
                        continue;
                    }
                    if (claim_equivocation) {
                        if (!AcknowledgeEquivocationMessages(
                                *context.paymaster, responses, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_QUOTE_EQUIVOCATION");
                    }
                    if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_ATTEMPT_NOT_FOUND");
                    }

                    CollaborativePSBTTemplate trusted;
                    ClientAuthorizationManifest client_manifest;
                    std::string response_error;
                    int response_error_code{RPC_INVALID_PARAMETER};
                    bool valid_response{
                        ValidateQuoteResponseForRequest(*response, quote_request, candidate.terms, candidate.identity_key, effective_fee_cap, now, response_error) &&
                        ValidateQuoteAgainstCapacitySnapshot(
                            response->quote, attempt.capacity_snapshot,
                            response_error) &&
                        BuildTrustedQuoteTemplate(
                            *wallet, quote_request.intent, response->quote,
                            Params(), *node->chainman, trusted,
                            response_error) &&
                        BuildClientAuthorizationManifest(
                            quote_request.intent, response->quote,
                            attempt.capacity_snapshot, trusted,
                            effective_fee_cap,
                            session.requested_amount.value > 0
                                ? session.requested_amount
                                : quote_request.intent.recipient_amount,
                            session.subtract_paymaster_fee_from_amount,
                            session.send_all_spendable_dd, client_manifest,
                            response_error)};
                    if (valid_response &&
                        !ValidateClientAuthorizationOwnership(
                            *wallet, client_manifest, response_error)) {
                        valid_response = false;
                        response_error_code = RPC_WALLET_ERROR;
                    }
                    if (!valid_response) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        rejected_quote_error =
                            response_error.empty() ? "PAYMASTER_INVALID_QUOTE_RESPONSE" : std::move(response_error);
                        rejected_quote_code = response_error_code;
                        continue;
                    }
                    attempt.state = AttemptState::QUOTED;
                    attempt.intent_hash = response->quote.intent_hash;
                    attempt.quote_id = response->quote.quote_id;
                    attempt.unsigned_txid = response->quote.unsigned_txid;
                    attempt.template_commitment = response->quote.template_commitment;
                    attempt.commit_key = GetPaymasterCommitKey(
                        provider_id, attempt.client_nonce, attempt.intent_hash,
                        attempt.quote_id, attempt.template_commitment);
                    attempt.signed_quote = SerializeQuoteResponse(*response);
                    attempt.client_manifest = std::move(client_manifest);
                    attempt.unsigned_transaction = SerializeTransaction(response->quote.unsigned_transaction);
                    attempt.unsigned_psbt = SerializePSBT(trusted.psbt);
                    attempt.input_roles.clear();
                    attempt.input_roles.reserve(trusted.input_roles.size());
                    static_assert(static_cast<uint8_t>(InputRole::USER_DD) ==
                                  static_cast<uint8_t>(ReservationRole::USER_DD));
                    static_assert(static_cast<uint8_t>(InputRole::PROVIDER_DGB) ==
                                  static_cast<uint8_t>(ReservationRole::PROVIDER_DGB));
                    for (const InputRole role : trusted.input_roles) {
                        attempt.input_roles.push_back(static_cast<ReservationRole>(role));
                    }
                    attempt.quote_expires_at = response->quote.expires_at;
                    attempt.retry_until = response->quote.retry_until;
                    attempt.updated_at = std::max(attempt.updated_at, now);
                    if (!store.UpdateAttempt(request_id, attempt, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    quote_received = true;
                    if (!DrainClientAttemptEquivocations(
                            *context.paymaster, store, session, attempt,
                            error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    break;
                }
                if (!quote_received && !rejected_quote_error.empty()) {
                    throw JSONRPCError(rejected_quote_code,
                                       rejected_quote_error);
                }

                NodeId peer_id{-1};
                if (!quote_received && attempt.state == AttemptState::CANDIDATE) {
                    node->connman->ForEachNode([&](CNode* peer) {
                        if (peer_id == -1 && peer->IsPaymasterConn() && peer->addr == candidate.endpoint) {
                            peer_id = peer->GetId();
                        }
                    });
                    if (peer_id == -1) {
                        connection_pending = node->connman->AddConnection(
                            candidate.endpoint.ToStringAddrPort(), ConnectionType::PAYMASTER,
                            /*paymaster_high_privacy=*/privacy == PrivacyProfile::HIGH);
                    } else {
                        const uint256 message_id = Hash(request_bytes);
                        queued = context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                                 context.paymaster->QueueOutboundDirectMessage(
                                     peer_id, message_id, request_bytes.size(), DirectPayload{quote_request}, now);
                        if (!queued) throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED,
                                                        "PAYMASTER_DIRECT_QUEUE_FULL");
                        node->connman->WakeMessageHandler();
                    }
                }
            }
            if (!store.GetSessionByRequestId(request_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
            }
            UniValue result = SessionToJSON(session);
            result.pushKV("provider_id", provider_id.GetHex());
            result.pushKV("offer_id", candidate.terms.offer_id.GetHex());
            result.pushKV("policy_hash", candidate.terms.policy_hash.GetHex());
            result.pushKV("funding_model",
                          intent.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
            result.pushKV("payment_cents", candidate.payment.value);
            result.pushKV("service_fee_cents", candidate.service_fee.value);
            result.pushKV("user_total_cents", candidate.user_total.value);
            result.pushKV("expires_at", intent.expires_at);
            result.pushKV("queued", queued);
            result.pushKV("connection_pending", connection_pending);
            result.pushKV("capacity_pending", false);
            result.pushKV("capacity_snapshot_id",
                          attempt.capacity_snapshot.snapshot_id.GetHex());
            if (!attempt.quote_id.IsNull()) {
                result.pushKV("quote_id", attempt.quote_id.GetHex());
                result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
                result.pushKV("template_commitment", attempt.template_commitment.GetHex());
                result.pushKV("authorization_commitment",
                              attempt.client_manifest.manifest_id.GetHex());
                const bool authorization_accepted =
                    attempt.accepted_client_manifest_id ==
                        attempt.client_manifest.manifest_id &&
                    attempt.client_manifest_accepted_at > 0;
                result.pushKV("authorization_accepted",
                              authorization_accepted);
                if (authorization_accepted) {
                    result.pushKV("authorization_accepted_at",
                                  attempt.client_manifest_accepted_at);
                }
                result.pushKV("psbt", EncodeBase64(attempt.unsigned_psbt));
            }
            return result;
        },
    };
}

UniValue RequestAutomaticPaymasterQuote(const JSONRPCRequest& request,
                                        const std::string& address,
                                        CAmount amount,
                                        const UniValue& options,
                                        const std::vector<COutPoint>* preset_inputs)
{
    using namespace DigiDollar::Paymaster;
    WalletContext& context = EnsureWalletContext(request.context);
    std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
    if (!wallet) return UniValue::VNULL;
    std::string readiness_error;
    if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
        throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
    }

    const std::string request_id = options.find_value("request_id").get_str();
    const int64_t fee_cap = options.find_value("maximum_paymaster_fee_cents").getInt<int64_t>();
    const std::string fee_mode = options.find_value("fee_mode").get_str();
    const FeeMode requested_mode =
        fee_mode == "auto" ? FeeMode::AUTO : FeeMode::PAYMASTER;
    const bool subtract_fee =
        !options.find_value("subtract_paymaster_fee_from_amount").isNull() &&
        options.find_value("subtract_paymaster_fee_from_amount").get_bool();
    const bool send_all =
        !options.find_value("send_all_spendable_dd").isNull() &&
        options.find_value("send_all_spendable_dd").get_bool();
    if (send_all && !subtract_fee) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
    }
    const bool prepare_only = !options.find_value("prepare_only").isNull() &&
                              options.find_value("prepare_only").get_bool();
    std::optional<uint256> supplied_authorization_commitment;
    if (!options.find_value("authorization_commitment").isNull()) {
        supplied_authorization_commitment = ParseHashV(
            options.find_value("authorization_commitment"),
            "authorization_commitment");
    }
    if (prepare_only && supplied_authorization_commitment) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "prepare_only and authorization_commitment are mutually exclusive");
    }
    const std::string privacy_name = options.find_value("privacy").isNull() ? "standard" : options.find_value("privacy").get_str();
    const std::string selection_name = options.find_value("selection").isNull() ? "lowest_total_cost" : options.find_value("selection").get_str();
    const PrivacyProfile privacy = privacy_name == "high" ? PrivacyProfile::HIGH : PrivacyProfile::STANDARD;
    const SelectionMode selection = selection_name == "privacy_weighted" ? SelectionMode::PRIVACY_WEIGHTED : SelectionMode::LOWEST_TOTAL_COST;
    const int maximum_attempts = options.find_value("maximum_provider_attempts").isNull() ? (privacy == PrivacyProfile::HIGH ? 1 : 3) : options.find_value("maximum_provider_attempts").getInt<int>();
    if (maximum_attempts < 1 || maximum_attempts > 16 ||
        (privacy == PrivacyProfile::HIGH && maximum_attempts != 1)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
    }
    if (!CheckPaymasterPrivacyReadiness(privacy, context, readiness_error)) {
        throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
    }

    const bool have_restricted_key = !options.find_value("provider_identity_key").isNull();
    const bool have_restricted_descriptor =
        !options.find_value("restricted_service_descriptor").isNull();
    const bool have_restricted_capability =
        !options.find_value("sponsorship_capability").isNull();
    const bool restricted = have_restricted_key || have_restricted_descriptor ||
                            have_restricted_capability;
    if (restricted && !(have_restricted_key && have_restricted_descriptor &&
                        have_restricted_capability)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "provider_identity_key, restricted_service_descriptor, and sponsorship_capability "
                           "must be supplied together");
    }
    if (restricted && maximum_attempts != 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "Restricted sponsorship requires maximum_provider_attempts=1");
    }
    std::optional<XOnlyPubKey> restricted_identity_key;
    std::optional<RestrictedServiceDescriptor> restricted_descriptor;
    std::optional<SponsorshipCapability> restricted_capability;
    OfferCandidate restricted_candidate;
    std::string error;
    PaymasterStore store{*wallet};
    const int64_t now = GetTime();
    PaymentSession durable_session;
    if (store.GetSessionByRequestId(request_id, durable_session) &&
        !IsTerminal(durable_session.state)) {
        ProviderAttempt durable_attempt;
        PaymentIntent durable_intent;
        PaymasterQuote durable_quote;
        if (FindDurableClientAuthorizationAttempt(
                *wallet, store, durable_session, now, durable_attempt,
                durable_intent, durable_quote, error)) {
            const CDigiDollarAddress dd_address{address};
            const CScript recipient_script = GetScriptForDestination(
                dd_address.GetDigiDollarDestination());
            if (durable_intent.request_id != request_id ||
                durable_intent.session_id != durable_session.session_id ||
                durable_intent.canonical_request_hash !=
                    durable_session.canonical_request_hash ||
                durable_intent.requested_fee_mode !=
                    durable_session.fee_mode_requested ||
                durable_intent.requested_fee_mode != requested_mode ||
                durable_intent.privacy_profile != privacy ||
                durable_intent.selection_mode != selection ||
                durable_attempt.privacy_profile != privacy ||
                durable_intent.recipient_script != recipient_script ||
                (subtract_fee
                     ? (durable_intent.recipient_amount.value >
                            std::numeric_limits<int64_t>::max() -
                                durable_quote.service_fee.value ||
                        durable_intent.recipient_amount.value +
                                durable_quote.service_fee.value !=
                            amount)
                     : durable_intent.recipient_amount != DDCents{amount}) ||
                (durable_session.requested_amount.value == 0
                     ? (subtract_fee || send_all)
                     : (durable_session.requested_amount != DDCents{amount} ||
                        durable_session.subtract_paymaster_fee_from_amount !=
                            subtract_fee ||
                        durable_session.send_all_spendable_dd != send_all)) ||
                durable_quote.service_fee.value > fee_cap ||
                (preset_inputs &&
                 *preset_inputs != durable_session.user_inputs)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, durable_session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue resume = DurableClientAuthorizationToJSON(
                durable_session, durable_attempt, durable_intent,
                durable_quote);
            resume.pushKV("authorization_required",
                          !supplied_authorization_commitment.has_value());
            if (!supplied_authorization_commitment) return resume;
            if (*supplied_authorization_commitment !=
                durable_attempt.client_manifest.manifest_id) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
            }

            static constexpr const char* RESUME_FIELDS[]{
                "requested_fee_mode", "fee_mode_used", "provider_id",
                "offer_id", "policy_hash", "funding_model",
                "payment_cents", "service_fee_cents", "user_total_cents",
                "requested_amount_cents",
                "subtract_paymaster_fee_from_amount",
                "send_all_spendable_dd",
                "authorization_commitment", "authorization_accepted",
                "authorization_accepted_at"};
            const auto copy_resume_fields = [&resume](UniValue& target) {
                for (const char* field : RESUME_FIELDS) {
                    const UniValue& value = resume.find_value(field);
                    if (!value.isNull() && target.find_value(field).isNull()) {
                        target.pushKV(field, value);
                    }
                }
            };

            // A provider can commit and broadcast the exact transaction
            // between two client polls. Consume and validate that signed
            // result before attempting to queue the already-authorized USER
            // PSBT again. Otherwise the retry firewall correctly observes
            // the provider Capacity outpoint as spent, but reports a false
            // failure even though it was spent by the exact final transaction
            // that is waiting in the result inbox.
            JSONRPCRequest nested{request};
            nested.params = UniValue{UniValue::VARR};
            nested.params.push_back(request_id);
            UniValue provider_result =
                processpaymasterresult().HandleRequest(nested);
            if (provider_result.find_value("processed").isTrue()) {
                copy_resume_fields(provider_result);
                return provider_result;
            }

            // The exact final transaction may already be in stem relay or
            // the mempool even when there is no unread result envelope left
            // (for example, immediately after the first successful automatic
            // poll or after a restart).  Its Capacity inputs are necessarily
            // spent by that transaction.  Return the durable status instead
            // of re-running walletprocesspaymasterpsbt, which would otherwise
            // misclassify the known exact spend as a foreign Capacity spend.
            if (durable_session.state == SessionState::STEMPOOL ||
                durable_session.state == SessionState::MEMPOOL ||
                !durable_session.final_txid.IsNull()) {
                UniValue committed = SessionToJSON(durable_session, &store);
                copy_resume_fields(committed);
                return committed;
            }

            if (wallet->IsLocked() &&
                durable_attempt.state == AttemptState::QUOTED) {
                return resume;
            }

            nested.params = UniValue{UniValue::VARR};
            nested.params.push_back(EncodeBase64(durable_attempt.unsigned_psbt));
            nested.params.push_back(
                supplied_authorization_commitment->GetHex());
            UniValue authorization =
                walletprocesspaymasterpsbt().HandleRequest(nested);
            copy_resume_fields(authorization);
            nested.params = UniValue{UniValue::VARR};
            nested.params.push_back(request_id);
            provider_result =
                processpaymasterresult().HandleRequest(nested);
            if (provider_result.find_value("processed").isTrue()) {
                copy_resume_fields(provider_result);
                return provider_result;
            }
            return authorization;
        }
        if (!error.empty()) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, durable_session, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }
    const DDCents effective_fee_cap =
        EffectiveClientServiceFeeCap(*wallet, DDCents{fee_cap}, error);
    if (effective_fee_cap.value < 0) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }
    if (restricted) {
        const std::vector<unsigned char> key_bytes =
            ParseHexV(options.find_value("provider_identity_key"), "provider_identity_key");
        if (key_bytes.size() != 32) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "provider_identity_key must be exactly 32 bytes");
        }
        restricted_identity_key.emplace(MakeUCharSpan(key_bytes));
        RestrictedServiceDescriptor descriptor;
        SponsorshipCapability capability;
        const CDigiDollarAddress dd_address{address};
        const CScript recipient_script = GetScriptForDestination(dd_address.GetDigiDollarDestination());
        if (!restricted_identity_key->IsFullyValid() ||
            !DeserializePaymasterHex(options.find_value("restricted_service_descriptor"),
                                     "RESTRICTED_DESCRIPTOR", descriptor, error) ||
            !DeserializePaymasterHex(options.find_value("sponsorship_capability"),
                                     "SPONSORSHIP_CAPABILITY", capability, error) ||
            !ValidateRestrictedServiceDescriptor(
                descriptor, *restricted_identity_key, Params().GenesisBlock().GetHash(),
                GetTime(), error, Params().GetChainType() == ChainType::REGTEST) ||
            !ValidateSponsorshipCapability(
                capability, descriptor, recipient_script, DDCents{amount},
                capability.payment_request_nonce, Params().GenesisBlock().GetHash(),
                GetTime(), error)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               error.empty() ? "PAYMASTER_INVALID_RESTRICTED_CONTEXT" : error);
        }
        node::NodeContext* node = wallet->chain().context();
        if (!node || !node->chainman ||
            !ValidateRestrictedAdmissionProofs(descriptor, *restricted_identity_key,
                                               *node->chainman, error)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               error.empty() ? "PAYMASTER_RESTRICTED_ADMISSION_UNAVAILABLE" : error);
        }
        if (privacy == PrivacyProfile::HIGH && !descriptor.p2p_endpoint.IsTor()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "High privacy requires an onion restricted provider");
        }
        restricted_candidate.provider_id = descriptor.provider_id;
        restricted_candidate.identity_key = *restricted_identity_key;
        restricted_candidate.display_name = descriptor.sponsor_display_name;
        restricted_candidate.endpoint = descriptor.p2p_endpoint;
        restricted_candidate.announcement_sequence = descriptor.admission_sequence;
        restricted_candidate.announcement_expires_at = descriptor.expires_at;
        restricted_candidate.terms = OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                                FundingModel::SPONSORED,
                                                SponsorshipScope::RESTRICTED, 0,
                                                DDCents{amount}, DDCents{amount}};
        restricted_candidate.payment = DDCents{amount};
        restricted_candidate.service_fee = DDCents{0};
        restricted_candidate.user_total = DDCents{amount};
        restricted_descriptor = std::move(descriptor);
        restricted_capability = std::move(capability);
    }

    std::optional<std::pair<PaymasterId, uint256>> persisted_selection;
    std::optional<std::vector<COutPoint>> persisted_inputs;
    std::set<PaymasterId> attempted_providers;
    std::optional<uint256> active_attempt_id;
    PaymentSession persisted_session;
    const bool have_persisted_session =
        store.GetSessionByRequestId(request_id, persisted_session);
    if (have_persisted_session) {
        if (IsTerminal(persisted_session.state)) {
            if (persisted_session.requested_amount.value == 0
                    ? (subtract_fee || send_all)
                    : (persisted_session.requested_amount != DDCents{amount} ||
                       persisted_session.subtract_paymaster_fee_from_amount !=
                           subtract_fee ||
                       persisted_session.send_all_spendable_dd != send_all)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_REQUEST_ID_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, persisted_session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return SessionToJSON(persisted_session, &store);
        }
        persisted_inputs = persisted_session.user_inputs;
        if (!persisted_session.attempt_ids.empty()) {
            for (const uint256& attempt_id : persisted_session.attempt_ids) {
                ProviderAttempt persisted_attempt;
                if (!store.GetAttempt(attempt_id, persisted_attempt) ||
                    persisted_attempt.unsigned_intent.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                attempted_providers.insert(persisted_attempt.provider_id);
                if (attempt_id != persisted_session.attempt_ids.back() ||
                    persisted_attempt.state == AttemptState::REJECTED ||
                    persisted_attempt.state == AttemptState::QUOTE_EXPIRED) {
                    continue;
                }
                PaymentIntent persisted_intent;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, persisted_attempt.unsigned_intent};
                    stream >> persisted_intent;
                    if (!stream.empty()) throw std::ios_base::failure("trailing intent data");
                    if (persisted_intent.user_dd_inputs != persisted_session.user_inputs) {
                        throw std::ios_base::failure("input binding mismatch");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                if (persisted_intent.expires_at <= now) {
                    std::string abandon_error;
                    if (!store.AbandonClientAttemptForFallback(
                            request_id, attempt_id, now, abandon_error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, abandon_error);
                    }
                } else {
                    persisted_selection = std::pair{persisted_attempt.provider_id,
                                                    persisted_intent.offer_id};
                    active_attempt_id = attempt_id;
                }
            }
        }
    }
    std::vector<PaymasterReliabilityRecord> records;
    if (!store.ListProviderReliability(records)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REPUTATION_DATABASE_READ");
    }
    std::map<PaymasterId, PaymasterReliabilityRecord> reputation;
    for (auto& record : records)
        reputation.emplace(record.provider_id, std::move(record));
    std::vector<OfferCandidate> candidates;
    if (restricted) {
        if (!EnsureProviderNotEquivocationBlocked(
                store, restricted_candidate.provider_id, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        candidates.push_back(restricted_candidate);
    } else {
        candidates = subtract_fee
            ? BuildGrossOfferCandidates(
                  context.paymaster->GetDirectory().List(now),
                  DDCents{amount}, FUNDING_MODEL_ALL, effective_fee_cap,
                  reputation, now, error)
            : BuildOfferCandidates(
                  context.paymaster->GetDirectory().List(now),
                  DDCents{amount}, FUNDING_MODEL_ALL, effective_fee_cap,
                  reputation, now, error);
        if (!error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    }
    if (privacy == PrivacyProfile::HIGH) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [](const OfferCandidate& candidate) { return !candidate.endpoint.IsTor(); }),
                         candidates.end());
    }
    std::optional<OfferCandidate> selected;
    if (persisted_selection) {
        const auto candidate = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const OfferCandidate& entry) {
                                                return entry.provider_id == persisted_selection->first &&
                                                       entry.terms.offer_id == persisted_selection->second;
                                            });
        if (candidate != candidates.end())
            selected = *candidate;
        else if (active_attempt_id) {
            if (!store.AbandonClientAttemptForFallback(
                    request_id, *active_attempt_id, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            persisted_selection.reset();
        }
    }
    if (!persisted_selection) {
        if (persisted_session.attempt_ids.size() >= static_cast<size_t>(maximum_attempts)) {
            if (!persisted_session.attempt_ids.empty()) {
                std::string abandon_error;
                if (!store.AbandonUnsignedClientSession(
                        request_id, now, abandon_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, abandon_error);
                }
            }
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_MAXIMUM_PROVIDER_ATTEMPTS_REACHED");
        }
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const OfferCandidate& candidate) {
                                            return attempted_providers.count(candidate.provider_id) != 0;
                                        }),
                         candidates.end());
        FastRandomContext rng;
        selected = SelectOfferCandidate(candidates, selection,
                                        effective_fee_cap, rng, error,
                                        subtract_fee);
    }
    if (!selected) {
        const std::string selection_error =
            subtract_fee && error == "PAYMASTER_NO_ELIGIBLE_OFFER"
                ? "PAYMASTER_NO_EXACT_GROSS_OFFER"
                : error;
        if (have_persisted_session) {
            std::string abandon_error;
            if (!store.AbandonUnsignedClientSession(
                    request_id, now, abandon_error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, abandon_error);
            }
        }
        throw JSONRPCError(RPC_WALLET_ERROR, selection_error);
    }

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    if (!dd_wallet) throw JSONRPCError(RPC_WALLET_ERROR, "DigiDollar wallet not initialized");
    std::vector<COutPoint> selected_inputs;
    std::vector<CAmount> selected_amounts;
    CAmount selected_total{0};
    bool inputs_ok{false};
    if (persisted_inputs && !persisted_inputs->empty()) {
        // This request already owns hard reservations for these exact inputs.
        // Reusing the persisted binding is the only safe automatic resume;
        // ordinary coin selection must continue to hide every reservation.
        if (preset_inputs && *preset_inputs != *persisted_inputs) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_PERSISTED_INPUT_CONFLICT");
        }
        selected_inputs = *persisted_inputs;
        inputs_ok = !selected_inputs.empty();
    } else if (send_all) {
        if (dd_wallet->GetSpendableDDBalance() != amount) {
            error = "PAYMASTER_SWEEP_BALANCE_CHANGED";
        } else if (preset_inputs) {
            inputs_ok = dd_wallet->SelectDDCoins(
                selected->user_total.value, *preset_inputs, selected_inputs,
                selected_total, &selected_amounts, &error);
            if (inputs_ok && selected_total != amount) {
                inputs_ok = false;
                error = "PAYMASTER_SWEEP_BALANCE_CHANGED";
            }
        } else {
            inputs_ok = dd_wallet->SelectAllSpendableDDCoins(
                amount, selected_inputs, selected_total, &selected_amounts,
                error);
        }
    } else if (preset_inputs) {
        inputs_ok = dd_wallet->SelectDDCoins(selected->user_total.value, *preset_inputs,
                                             selected_inputs, selected_total,
                                             &selected_amounts, &error);
    } else {
        inputs_ok = dd_wallet->SelectDDCoins(selected->user_total.value, selected_inputs,
                                             selected_total, &selected_amounts);
    }
    if (!inputs_ok) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                           error.empty() ? "Insufficient confirmed DigiDollar inputs" : error);
    }

    UniValue inputs{UniValue::VARR};
    for (const COutPoint& outpoint : selected_inputs) {
        UniValue input{UniValue::VOBJ};
        input.pushKV("txid", outpoint.hash.GetHex());
        input.pushKV("vout", outpoint.n);
        inputs.push_back(std::move(input));
    }
    UniValue intent{UniValue::VOBJ};
    intent.pushKV("request_id", request_id);
    intent.pushKV("address", address);
    intent.pushKV("amount_cents", selected->payment.value);
    intent.pushKV("requested_amount_cents", amount);
    intent.pushKV("subtract_paymaster_fee_from_amount", subtract_fee);
    intent.pushKV("send_all_spendable_dd", send_all);
    intent.pushKV("maximum_paymaster_fee_cents", fee_cap);
    intent.pushKV("fee_mode", fee_mode);
    intent.pushKV("privacy", privacy_name);
    intent.pushKV("selection", selection_name);
    intent.pushKV("maximum_provider_attempts", maximum_attempts);
    intent.pushKV("offer_id", selected->terms.offer_id.GetHex());
    if (restricted) {
        intent.pushKV("provider_identity_key", options.find_value("provider_identity_key"));
        intent.pushKV("restricted_service_descriptor",
                      options.find_value("restricted_service_descriptor"));
        intent.pushKV("sponsorship_capability", options.find_value("sponsorship_capability"));
    }
    intent.pushKV("selected_inputs", std::move(inputs));

    JSONRPCRequest nested{request};
    nested.params = UniValue{UniValue::VARR};
    nested.params.push_back(selected->provider_id.GetHex());
    nested.params.push_back(std::move(intent));
    UniValue quote_result = requestpaymasterquote().HandleRequest(nested);
    if (quote_result.find_value("funding_model").isNull()) {
        quote_result.pushKV(
            "funding_model",
            selected->terms.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
    }

    const auto copy_quote_fields = [&quote_result](UniValue& target) {
        static constexpr const char* FIELDS[]{
            "requested_fee_mode", "fee_mode_used", "provider_id", "offer_id",
            "policy_hash", "funding_model",
            "payment_cents", "service_fee_cents", "user_total_cents", "expires_at",
            "requested_amount_cents",
            "subtract_paymaster_fee_from_amount", "send_all_spendable_dd",
            "quote_id", "unsigned_txid", "template_commitment",
            "authorization_required", "authorization_commitment"};
        for (const char* field : FIELDS) {
            const UniValue& value = quote_result.find_value(field);
            if (!value.isNull() && target.find_value(field).isNull()) target.pushKV(field, value);
        }
    };

    const UniValue& psbt = quote_result.find_value("psbt");
    if (psbt.isNull()) {
        if (supplied_authorization_commitment) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_AUTHORIZATION_NOT_READY");
        }
        return quote_result;
    }

    const UniValue& unsigned_txid_value = quote_result.find_value("unsigned_txid");
    if (!unsigned_txid_value.isStr()) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "PAYMASTER_AUTHORIZATION_TEMPLATE_MISSING");
    }
    ProviderAttempt authorization_attempt;
    const uint256 unsigned_txid = ParseHashV(unsigned_txid_value, "unsigned_txid");
    if (!store.GetAttemptByUnsignedTxid(unsigned_txid, authorization_attempt)) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "PAYMASTER_AUTHORIZATION_ATTEMPT_MISSING");
    }
    uint256 commitment;
    if (!GetValidatedClientAuthorizationCommitment(
            authorization_attempt, GetTime(), commitment, error)) {
        throw JSONRPCError(
            RPC_WALLET_ERROR,
            error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_INVALID" : error);
    }
    if (supplied_authorization_commitment &&
        *supplied_authorization_commitment != commitment) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
    }
    quote_result.pushKV("authorization_required",
                        !supplied_authorization_commitment.has_value());
    if (quote_result.find_value("authorization_commitment").isNull()) {
        quote_result.pushKV("authorization_commitment", commitment.GetHex());
    }
    // A high-level call can never silently turn a freshly received quote into
    // a USER signature. The caller must round-trip the exact manifest hash.
    if (!supplied_authorization_commitment) return quote_result;

    if (authorization_attempt.state == AttemptState::QUOTED) {
        if (!store.AcceptClientAuthorization(
                request_id, authorization_attempt.attempt_id, commitment,
                GetTime(), error) ||
            !store.GetAttempt(authorization_attempt.attempt_id,
                              authorization_attempt)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
    }
    if (wallet->IsLocked()) {
        PaymentSession session;
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
        }
        if (session.state == SessionState::AWAITING_USER_SIGNATURE &&
            !store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                     PendingPhase::NONE, {},
                                     std::max(GetTime(),
                                              authorization_attempt.updated_at),
                                     error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
        }
        UniValue paused = SessionToJSON(session);
        copy_quote_fields(paused);
        const bool authorization_accepted =
            authorization_attempt.accepted_client_manifest_id == commitment &&
            authorization_attempt.client_manifest_accepted_at > 0;
        paused.pushKV("authorization_accepted", authorization_accepted);
        if (authorization_accepted) {
            paused.pushKV("authorization_accepted_at",
                          authorization_attempt.client_manifest_accepted_at);
        }
        return paused;
    }

    nested.params = UniValue{UniValue::VARR};
    nested.params.push_back(psbt);
    nested.params.push_back(supplied_authorization_commitment->GetHex());
    UniValue authorization = walletprocesspaymasterpsbt().HandleRequest(nested);
    copy_quote_fields(authorization);

    nested.params = UniValue{UniValue::VARR};
    nested.params.push_back(request_id);
    UniValue provider_result = processpaymasterresult().HandleRequest(nested);
    if (provider_result.find_value("processed").isTrue()) {
        copy_quote_fields(provider_result);
        return provider_result;
    }
    return authorization;
}

RPCHelpMan getpaymasterreputation()
{
    return RPCHelpMan{
        "getpaymasterreputation",
        "Return privacy-preserving local reliability aggregates. No payment details are stored.\n",
        {
            {"provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional provider identity; omit to list all local records"},
        },
        RPCResult{RPCResult::Type::ARR, "", "Local provider reliability records", {{RPCResult::Type::OBJ, "", /*optional=*/false, "Reliability aggregate", {}}}},
        RPCExamples{HelpExampleCli("getpaymasterreputation", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
        },
    };
}

RPCHelpMan getpaymasterpoolinfo()
{
    return RPCHelpMan{
        "getpaymasterpoolinfo",
        "Return the persisted admission and operational Paymaster pool without modifying it.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider pool information", {
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
                readiness = DigiDollar::Paymaster::EvaluateProviderPoolReadiness(entries, policy, tip_height, 1);
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
            result.pushKV("ready", prepared && have_policy && readiness.ready);
            result.pushKV("entries", static_cast<uint64_t>(entries.size()));
            result.pushKV("complete_operational_slots", static_cast<uint64_t>(readiness.complete_operational_slots));
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            result.pushKV("pool", std::move(pool));
            return result;
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
        },
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
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
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
        },
    };
}

RPCHelpMan processpaymasterresult()
{
    return RPCHelpMan{
        "processpaymasterresult",
        "Validate and persist at most one PMRESULT for an existing client session.\n",
        {{"request_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Canonical request UUID"}},
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative result processing state", {
                                                                                         {RPCResult::Type::BOOL, "processed", "Whether a matching result was available"},
                                                                                         {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                         {RPCResult::Type::STR_HEX, "session_id", "Persistent session"},
                                                                                         {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                         {RPCResult::Type::STR_HEX, "offer_id", "Exact authorized offer"},
                                                                                         {RPCResult::Type::STR_HEX, "policy_hash", "Exact authorized provider policy"},
                                                                                         {RPCResult::Type::STR, "funding_model", "Exact authorized funding model"},
                                                                                         {RPCResult::Type::NUM, "payment_cents", "Exact recipient amount"},
                                                                                         {RPCResult::Type::NUM, "service_fee_cents", "Exact provider service fee"},
                                                                                         {RPCResult::Type::NUM, "user_total_cents", "Exact maximum wallet outflow"},
                                                                                         {RPCResult::Type::STR_HEX, "authorization_commitment", "Accepted client authorization manifest"},
                                                                                         {RPCResult::Type::BOOL, "authorization_accepted", "Whether the exact manifest remains accepted"},
                                                                                         {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                         {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                         {RPCResult::Type::STR, "result_status", /*optional=*/true, "Signed provider status"},
                                                                                         {RPCResult::Type::NUM, "result_sequence", /*optional=*/true, "Monotonic sequence"},
                                                                                         {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction"},
                                                                                     }},
        RPCExamples{HelpExampleCli("processpaymasterresult", "\"550e8400-e29b-41d4-a716-446655440000\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            if (!context.paymaster || !context.paymaster->Enabled()) {
                throw JSONRPCError(RPC_MISC_ERROR, "DigiDollar Paymaster support is disabled");
            }
            const std::string request_id = request.params[0].get_str();
            if (!IsCanonicalRequestId(request_id)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "request_id must be a canonical lowercase UUID");
            }
            PaymasterStore store{*wallet};
            PaymentSession session;
            if (!store.GetSessionByRequestId(request_id, session) || session.attempt_ids.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_NOT_FOUND");
            }
            ProviderAttempt attempt;
            if (!store.GetAttempt(session.attempt_ids.back(), attempt) ||
                !attempt.provider_identity_key.IsFullyValid()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_IDENTITY_NOT_PERSISTED");
            }
            const int64_t operation_now{GetTime()};
            std::string error;
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt, error,
                    EquivocationBlockPolicy::OBSERVE_ONLY)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            PaymentIntent authorized_intent;
            PaymasterQuote authorized_quote;
            CollaborativePSBTTemplate authorized_template;
            if (!HasDurableClientAuthorization(attempt, operation_now) ||
                attempt.client_manifest_accepted_at > session.updated_at ||
                !LoadAttemptAuthorizationArtifacts(
                    attempt, authorized_intent, authorized_quote,
                    authorized_template, error) ||
                !ValidateClientAuthorizationManifest(
                    attempt.client_manifest, authorized_intent,
                    authorized_quote, attempt.capacity_snapshot,
                    authorized_template, error) ||
                !ValidateClientAuthorizationOwnership(
                    *wallet, attempt.client_manifest, error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED" : error);
            }
            const auto messages = context.paymaster->TakeResults(
                request_id, session.session_id, attempt.provider_id, 1);
            std::optional<PaymasterResult> accepted_result;
            FinalTransactionPresence final_presence{FinalTransactionPresence::NONE};
            if (!messages.empty()) {
                const auto* message = std::get_if<PaymasterResultMessage>(&messages.front().payload);
                if (!message) throw JSONRPCError(RPC_INTERNAL_ERROR, "PAYMASTER_RESULT_QUEUE_CORRUPT");
                PaymasterResult previous;
                const uint64_t minimum_sequence = store.GetProviderResult(attempt.commit_key, previous) ? previous.result_sequence : 1;
                if (!ValidatePaymasterResult(message->result, Params().GenesisBlock().GetHash(),
                                             attempt.provider_id, attempt.commit_key,
                                             attempt.provider_identity_key, minimum_sequence, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                if (message->result.final_transaction) {
                    CollaborativePSBTTemplate trusted;
                    if (!LoadTrustedTemplate(attempt, trusted, error) ||
                        !ValidateFinalCollaborativeTransaction(
                            *message->result.final_transaction, *message->result.txid,
                            *message->result.raw_transaction_hash, trusted, error)) {
                        const std::string validation_error{error};
                        std::string persistence_error;
                        if (!MarkFinalValidationFailureRecoverable(
                                store, session, attempt, GetTime(), persistence_error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, persistence_error);
                        }
                        throw JSONRPCError(RPC_INVALID_PARAMETER, validation_error);
                    }
                    if (!PreflightFinalPaymasterTransaction(
                            *wallet, MakeTransactionRef(*message->result.final_transaction),
                            final_presence, error)) {
                        const std::string preflight_error{error};
                        const bool transient =
                            IsTransientPaymasterFinalizationError(preflight_error);
                        if (!transient) {
                            std::string persistence_error;
                            if (!MarkFinalValidationFailureRecoverable(
                                    store, session, attempt, GetTime(), persistence_error)) {
                                throw JSONRPCError(RPC_WALLET_ERROR, persistence_error);
                            }
                        }
                        throw JSONRPCError(
                            transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                            preflight_error);
                    }
                }
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, attempt, error,
                        EquivocationBlockPolicy::OBSERVE_ONLY)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!store.StoreClientResult(message->result, Params().GenesisBlock().GetHash(),
                                             attempt.attempt_id, GetTime(), error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                accepted_result = message->result;
                const bool final = message->result.final_transaction.has_value();
                if (final) {
                    const CMutableTransaction& transaction = *message->result.final_transaction;
                    const CTransaction exact_final{transaction};
                    const auto fail_after_final_store =
                        [&](int rpc_code, const std::string& failure) -> void {
                        std::string persistence_error;
                        if (!store.RecordClientFinalConflict(
                                request_id, attempt.attempt_id,
                                exact_final.GetHash(), exact_final.GetWitnessHash(),
                                GetTime(), persistence_error)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                persistence_error.empty() ? "PAYMASTER_FINAL_CONFLICT_NOT_PERSISTED" : persistence_error);
                        }
                        throw JSONRPCError(
                            rpc_code,
                            failure.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : failure);
                    };
                    // StoreClientResult commits the signed result, final
                    // artifacts, user-authorization evidence, attempt and
                    // session transition in one wallet transaction.
                    if (!store.GetAttempt(attempt.attempt_id, attempt) ||
                        !store.GetSessionByRequestId(request_id, session)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
                    }

                    // Result persistence and state reconciliation can perform
                    // database work. Reload and revalidate the exact authority
                    // and final bytes at the last possible point before the
                    // transaction is handed to the node for broadcast.
                    ProviderAttempt broadcast_attempt;
                    PaymentIntent broadcast_intent;
                    PaymasterQuote broadcast_quote;
                    CollaborativePSBTTemplate broadcast_template;
                    FinalTransactionPresence broadcast_presence{
                        FinalTransactionPresence::NONE};
                    if (!store.GetAttempt(attempt.attempt_id, broadcast_attempt)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
                    }
                    if (!HasDurableClientAuthorization(
                            broadcast_attempt, GetTime()) ||
                        broadcast_attempt.client_manifest_accepted_at >
                            session.updated_at ||
                        !LoadAttemptAuthorizationArtifacts(
                            broadcast_attempt, broadcast_intent, broadcast_quote,
                            broadcast_template, error) ||
                        !ValidateClientAuthorizationManifest(
                            broadcast_attempt.client_manifest, broadcast_intent,
                            broadcast_quote, broadcast_attempt.capacity_snapshot,
                            broadcast_template, error) ||
                        !ValidateClientAuthorizationOwnership(
                            *wallet, broadcast_attempt.client_manifest, error) ||
                        !ValidateFinalCollaborativeTransaction(
                            transaction, CTransaction{transaction}.GetHash(),
                            CTransaction{transaction}.GetWitnessHash(),
                            broadcast_template, error) ||
                        broadcast_attempt.final_txid != CTransaction{transaction}.GetHash() ||
                        broadcast_attempt.final_transaction !=
                            SerializeTransaction(transaction)) {
                        fail_after_final_store(
                            RPC_TRANSACTION_REJECTED,
                            error.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : error);
                    }
                    if (!PreflightFinalPaymasterTransaction(
                            *wallet, MakeTransactionRef(transaction),
                            broadcast_presence, error)) {
                        if (IsTransientPaymasterFinalizationError(error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        fail_after_final_store(RPC_TRANSACTION_REJECTED, error);
                    }
                    if (!store.ValidateClientDurableFinalForBroadcast(
                            CTransaction{transaction}, GetTime(),
                            broadcast_presence != FinalTransactionPresence::NONE,
                            error)) {
                        if (IsTransientPaymasterFinalizationError(error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        fail_after_final_store(
                            RPC_TRANSACTION_REJECTED,
                            error.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : error);
                    }
                    if (!DrainClientAttemptEquivocations(
                            *context.paymaster, store, session,
                            broadcast_attempt, error,
                            EquivocationBlockPolicy::OBSERVE_ONLY)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    bool already_confirmed{false};
                    std::string broadcast_error;
                    if (InsertAndBroadcastPaymasterTransaction(
                            *wallet, MakeTransactionRef(transaction), already_confirmed,
                            broadcast_error)) {
                        if (!store.RecordClientFinalObservation(
                                request_id, attempt.attempt_id,
                                already_confirmed, GetTime(), error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                    } else {
                        if (IsTransientPaymasterFinalizationError(
                                broadcast_error)) {
                            throw JSONRPCError(
                                RPC_WALLET_ERROR,
                                broadcast_error.empty() ? "PAYMASTER_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
                        }
                        fail_after_final_store(
                            RPC_TRANSACTION_REJECTED,
                            broadcast_error.empty() ? "PAYMASTER_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
                    }
                } else if (message->result.status == PaymasterResultStatus::REJECTED ||
                           message->result.status == PaymasterResultStatus::SLOT_UNAVAILABLE) {
                    if (!store.GetAttempt(attempt.attempt_id, attempt) ||
                        !store.GetSessionByRequestId(request_id, session)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                }
            }
            store.GetSessionByRequestId(request_id, session);
            store.GetAttempt(session.attempt_ids.back(), attempt);

            // Collaborative Paymaster transactions bypass the ordinary
            // TransferDigiDollarMany() path that records an outgoing DD row.
            // Once exact final bytes are durable, repair that display-only
            // history idempotently. A history-write failure must never turn a
            // valid, possibly already broadcast payment into an RPC failure;
            // every later result poll will retry the same deterministic row.
            if (!attempt.final_txid.IsNull()) {
                if (DigiDollarWallet* dd_wallet = wallet->GetDDWallet()) {
                    std::string history_error;
                    const CAmount user_total =
                        authorized_intent.recipient_amount.value +
                        authorized_quote.service_fee.value;
                    if (!dd_wallet->RecordPaymasterSendHistory(
                            attempt.final_txid,
                            authorized_intent.recipient_script,
                            user_total,
                            history_error)) {
                        LogPrintf(
                            "Paymaster: unable to record client DD history: %s\n",
                            history_error);
                    }
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("processed", accepted_result.has_value());
            result.pushKV("request_id", request_id);
            result.pushKV("session_id", session.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            // Always echo the exact durable authorization, including on the
            // final result response. Callers must never reconstruct a funding
            // model or fee from a stale directory preview, and Qt needs to
            // distinguish a completed payment from a changed pre-signing
            // authorization without guessing missing fields.
            result.pushKV("offer_id", authorized_intent.offer_id.GetHex());
            result.pushKV("policy_hash", authorized_intent.policy_hash.GetHex());
            result.pushKV(
                "funding_model",
                authorized_intent.funding_model == FundingModel::SPONSORED
                    ? "sponsored"
                    : "user_paid");
            result.pushKV("payment_cents",
                          authorized_intent.recipient_amount.value);
            result.pushKV("service_fee_cents",
                          authorized_quote.service_fee.value);
            result.pushKV("user_total_cents",
                          authorized_intent.recipient_amount.value +
                              authorized_quote.service_fee.value);
            result.pushKV("authorization_commitment",
                          attempt.client_manifest.manifest_id.GetHex());
            result.pushKV(
                "authorization_accepted",
                attempt.accepted_client_manifest_id ==
                        attempt.client_manifest.manifest_id &&
                    attempt.client_manifest_accepted_at > 0);
            result.pushKV("session_state", std::string{SessionStateName(session.state)});
            result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
            if (accepted_result) {
                result.pushKV("result_status", ResultStatusName(accepted_result->status));
                result.pushKV("result_sequence", accepted_result->result_sequence);
                if (accepted_result->txid) result.pushKV("txid", accepted_result->txid->GetHex());
            }
            return result;
        },
    };
}

RPCHelpMan processpaymastersubmits()
{
    return RPCHelpMan{
        "processpaymastersubmits",
        "Process at most one addressed PMSUBMIT through the durable provider commit path.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider submit processing result", {
                                                                                     {RPCResult::Type::BOOL, "processed", "Whether a submit was available"},
                                                                                     {RPCResult::Type::BOOL, "queued", "Whether PMRESULT was queued"},
                                                                                     {RPCResult::Type::STR, "message_type", /*optional=*/true, "submit or recovery_submit"},
                                                                                     {RPCResult::Type::STR, "request_id", /*optional=*/true, "Canonical request UUID"},
                                                                                     {RPCResult::Type::STR_HEX, "session_id", /*optional=*/true, "Client session"},
                                                                                     {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                                     {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Durable commit key"},
                                                                                     {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final transaction"},
                                                                                     {RPCResult::Type::STR, "result_status", /*optional=*/true, "Signed provider result status"},
                                                                                     {RPCResult::Type::STR, "attempt_state", /*optional=*/true, "Authoritative provider attempt state"},
                                                                                     {RPCResult::Type::STR, "rejection_code", /*optional=*/true, "Stable local reason for a rejected stale submit"},
                                                                                     {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether an alternative-recovery transaction was broadcast"},
                                                                                     {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable alternative-recovery broadcast error"},
                                                                                     {RPCResult::Type::OBJ, "recovery", /*optional=*/true, "Persistent alternative-recovery authority and state", {
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
                                                                                                                                                                                                  }},
                                                                                     {RPCResult::Type::OBJ, "commit", /*optional=*/true, "Durable provider commit and broadcast result", {
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
                                                                                 }},
        RPCExamples{HelpExampleCli("processpaymastersubmits", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool automatic_service =
                request.strMethod == INTERNAL_PAYMASTER_SERVICE_METHOD;
            const ExactFinalTxIndexMode service_txindex_mode =
                automatic_service ? ExactFinalTxIndexMode::NONBLOCKING
                                  : ExactFinalTxIndexMode::WAIT_FOR_SYNC;
            if (automatic_service) {
                if (!AutomaticProviderStateIsSynchronized(*wallet)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_SYNCING");
                }
            } else {
                wallet->BlockUntilSyncedToCurrentChain();
            }
            ProviderIdentityRecord identity;
            if (!GetPaymasterIdentity(*wallet, identity) || !context.paymaster ||
                !context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_NOT_RUNNING");
            }
            ProviderSettings runtime_settings;
            if (!GetPaymasterProviderSettings(*wallet, runtime_settings)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND");
            }
            if (runtime_settings.operation_mode ==
                    ProviderOperationMode::AUTOMATIC &&
                !automatic_service) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE");
            }
            ProviderWorkGuard work_guard{*context.paymaster,
                                         wallet->GetName(),
                                         identity.provider_id};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SERVICE_BUSY");
            }
            std::string error;
            const int64_t now = GetTime();
            const auto recovery_lease =
                context.paymaster->LeaseRecoverySubmits(identity.provider_id, 1);
            const auto& recovery_messages = recovery_lease.Messages();
            if (!recovery_messages.empty()) {
                const DirectMessage& direct = recovery_messages.front();
                ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                          direct};
                const auto* submit =
                    std::get_if<AlternativeRecoverySubmit>(&direct.payload);
                if (!submit || !ValidateAlternativeRecoverySubmitEnvelope(
                                   *submit, Params().GenesisBlock().GetHash(),
                                   error)) {
                    if (!AcknowledgeRejectedDirectMessage(
                            *context.paymaster, direct, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error);
                }
                PaymasterStore store{*wallet};
                AlternativeRecoveryRecord recovery;
                if (!store.GetAlternativeRecoveryById(submit->recovery_id,
                                                      recovery) ||
                    !recovery.provider_side || recovery.expired ||
                    recovery.request_id != submit->request_id ||
                    recovery.session_id != submit->session_id ||
                    recovery.recovery_provider_id !=
                        submit->recovery_provider_id ||
                    recovery.recovery_request_hash !=
                        submit->recovery_request_hash ||
                    recovery.recovery_response.recovery_commit_key !=
                        submit->recovery_commit_key ||
                    recovery.recovery_response.manifest.template_commitment !=
                        submit->template_commitment) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH");
                }

                const auto queue_result = [&](const AlternativeRecoveryRecord& record) {
                    AlternativeRecoveryResultMessage message;
                    message.request_id = record.request_id;
                    message.session_id = record.session_id;
                    message.recovery_id = record.recovery_id;
                    message.recovery_request_hash =
                        record.recovery_request_hash;
                    message.result = record.signed_result;
                    const std::vector<unsigned char> encoded =
                        SerializeRecoveryMessage(message);
                    const uint256 message_id{Hash(encoded)};
                    const bool queued =
                        context.paymaster->HasOutboundDirectMessage(
                            direct.peer_id, message_id) ||
                        context.paymaster->QueueOutboundDirectMessage(
                            direct.peer_id, message_id, encoded.size(),
                            DirectPayload{message}, now);
                    node::NodeContext* node_ctx = wallet->chain().context();
                    if (queued && node_ctx && node_ctx->connman) {
                        node_ctx->connman->WakeMessageHandler();
                    }
                    return queued;
                };

                if (recovery.phase ==
                    AlternativeRecoveryPhase::FINAL_COMMITTED) {
                    if (recovery.user_signed_psbt != submit->user_psbt ||
                        recovery.signed_result.status !=
                            PaymasterResultStatus::FINAL_COMMITTED) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
                    }
                    UniValue result{UniValue::VOBJ};
                    result.pushKV("processed", true);
                    result.pushKV("queued", queue_result(recovery));
                    result.pushKV("message_type", "recovery_submit");
                    result.pushKV("request_id", recovery.request_id);
                    result.pushKV("session_id", recovery.session_id.GetHex());
                    result.pushKV("provider_id",
                                  recovery.recovery_provider_id.GetHex());
                    result.pushKV("commit_key",
                                  recovery.recovery_response.recovery_commit_key
                                      .GetHex());
                    result.pushKV("txid",
                                  recovery.signed_result.txid->GetHex());
                    result.pushKV("result_status", "final_committed");
                    result.pushKV("recovery",
                                  AlternativeRecoveryToJSON(recovery));
                    return result;
                }

                node::NodeContext* node_ctx = wallet->chain().context();
                if (!node_ctx || !node_ctx->chainman) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                                       "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
                }
                // Receiving an exact USER-signed replay is not authority to
                // create a new provider signature after the response or
                // Capacity window expired. Only an already durable final may
                // use the separately named authorized-retry firewall.
                const int64_t authorization_time{now};
                AlternativeRecoveryParameters parameters;
                AlternativeRecoveryTemplate trusted;
                PartiallySignedTransaction unsigned_psbt;
                PartiallySignedTransaction user_psbt;
                if (!BuildRecoveryParametersFromRecord(
                        *wallet, recovery, parameters, error) ||
                    !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                        recovery.recovery_response,
                        recovery.recovery_request,
                        recovery.recovery_provider_identity_key, parameters,
                        Params(), *node_ctx->chainman, authorization_time, trusted,
                        unsigned_psbt, error) ||
                    !ValidateAlternativeRecoverySubmitAgainstChainstate(
                        *submit, recovery.recovery_response, trusted,
                        parameters, Params(), *node_ctx->chainman,
                        authorization_time,
                        user_psbt, error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error);
                }

                if (recovery.phase ==
                    AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
                    recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
                    recovery.user_signed_psbt = submit->user_psbt;
                    recovery.updated_at = std::max(recovery.updated_at, now);
                    if (!store.UpdateAlternativeRecovery(recovery, error) ||
                        !store.GetAlternativeRecoveryById(
                            submit->recovery_id, recovery)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else if (recovery.phase !=
                               AlternativeRecoveryPhase::USER_SIGNED ||
                           recovery.user_signed_psbt != submit->user_psbt) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
                }
                if (wallet->IsLocked()) {
                    throw JSONRPCError(
                        RPC_WALLET_UNLOCK_NEEDED,
                        "Provider wallet unlock is required for a recovery signature");
                }

                // Reload and rebuild every local authority immediately before
                // exposing a provider signature. The peer-supplied PSBT is
                // never itself trusted as the signing policy.
                parameters = {};
                trusted = {};
                unsigned_psbt = {};
                user_psbt = {};
                AlternativeRecoverySubmit persisted_submit{*submit};
                persisted_submit.user_psbt = recovery.user_signed_psbt;
                if (!store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
                        recovery, Params().GenesisBlock().GetHash(), now,
                        error) ||
                    !BuildRecoveryParametersFromRecord(
                        *wallet, recovery, parameters, error) ||
                    !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                        recovery.recovery_response,
                        recovery.recovery_request,
                        recovery.recovery_provider_identity_key, parameters,
                        Params(), *node_ctx->chainman, authorization_time,
                        trusted,
                        unsigned_psbt, error) ||
                    !ValidateAlternativeRecoverySubmitAgainstChainstate(
                        persisted_submit, recovery.recovery_response, trusted,
                        parameters, Params(), *node_ctx->chainman,
                        authorization_time,
                        user_psbt, error)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_AUTHORIZATION_INVALID" : error);
                }
                const AlternativeRecoveryManifest& manifest =
                    recovery.recovery_response.manifest;
                if (!SignCollaborativePSBTForParty(
                        *wallet, user_psbt, trusted.trusted_template,
                        SigningParty::PROVIDER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                CMutableTransaction final_transaction;
                if (!FinalizeAndExtractPSBT(user_psbt, final_transaction)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_FINALIZATION_FAILED");
                }
                const CTransaction final_tx{final_transaction};
                if (!ValidateFinalAlternativeRecoveryAgainstChainstate(
                        final_transaction, final_tx.GetWitnessHash(), trusted,
                        parameters, Params(), *node_ctx->chainman,
                        authorization_time,
                        /*exact_final_already_known=*/false, error)) {
                    throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
                }
                FinalTransactionPresence presence{FinalTransactionPresence::NONE};
                const CTransactionRef final_ref =
                    MakeTransactionRef(final_transaction);
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, final_ref, presence, error,
                        service_txindex_mode)) {
                    throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
                }

                ProviderCommitRecord commit;
                commit.commit_key =
                    recovery.recovery_response.recovery_commit_key;
                commit.provider_id = recovery.recovery_provider_id;
                commit.quote_id = recovery.recovery_id;
                commit.template_commitment = manifest.template_commitment;
                commit.final_txid = final_tx.GetHash();
                commit.final_transaction =
                    SerializeTransaction(final_transaction);
                commit.raw_transaction_hash =
                    Hash(commit.final_transaction);
                commit.provider_inputs =
                    manifest.recovery_provider_carrier_inputs;
                commit.provider_inputs.insert(
                    commit.provider_inputs.end(),
                    manifest.recovery_provider_dgb_inputs.begin(),
                    manifest.recovery_provider_dgb_inputs.end());
                commit.committed_at = now;
                commit.retry_until =
                    recovery.recovery_response.expires_at;
                PaymasterResult final_result;
                if (!BuildFinalProviderResult(
                        *wallet, commit, now, final_result, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
                recovery.final_transaction = commit.final_transaction;
                recovery.expected_wtxid = final_tx.GetWitnessHash();
                recovery.signed_result = final_result;
                recovery.updated_at = std::max(recovery.updated_at, now);
                if (!store.CommitProviderAlternativeRecoveryFinal(
                        recovery, commit, final_result,
                        Params().GenesisBlock().GetHash(), error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }

                // The commit above irrevocably consumes the provider budget
                // and pool slots because a valid provider signature now
                // exists. Reload those exact durable artifacts and repeat the
                // complete authority and mempool firewall. A failure leaves
                // FINAL_COMMITTED intact for safe retry; it must never roll
                // the economic reservation back to AVAILABLE.
                ProviderCommitRecord broadcast_commit;
                PaymasterResult broadcast_result;
                AlternativeRecoveryRecord broadcast_recovery;
                if (!store.GetProviderCommit(
                        commit.commit_key, broadcast_commit) ||
                    !store.GetProviderResult(
                        commit.commit_key, broadcast_result) ||
                    !store.GetProviderAlternativeRecoveryByCommit(
                        broadcast_commit, broadcast_recovery, error) ||
                    broadcast_commit.final_txid != commit.final_txid ||
                    broadcast_commit.raw_transaction_hash !=
                        commit.raw_transaction_hash ||
                    broadcast_commit.final_transaction !=
                        commit.final_transaction ||
                    !broadcast_result.txid ||
                    *broadcast_result.txid != commit.final_txid ||
                    !broadcast_result.raw_transaction_hash ||
                    *broadcast_result.raw_transaction_hash !=
                        final_tx.GetWitnessHash() ||
                    !broadcast_result.final_transaction ||
                    SerializeTransaction(
                        *broadcast_result.final_transaction) !=
                        commit.final_transaction) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_FINAL_RELOAD_FAILED" : error);
                }
                AlternativeRecoveryParameters broadcast_parameters;
                AlternativeRecoveryTemplate broadcast_trusted;
                PartiallySignedTransaction broadcast_unsigned_psbt;
                PartiallySignedTransaction broadcast_user_psbt;
                AlternativeRecoverySubmit broadcast_submit{*submit};
                broadcast_submit.user_psbt =
                    broadcast_recovery.user_signed_psbt;
                FinalTransactionPresence broadcast_presence{
                    FinalTransactionPresence::NONE};
                if (!BuildRecoveryParametersFromRecord(
                        *wallet, broadcast_recovery, broadcast_parameters,
                        error) ||
                    !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                        broadcast_recovery.recovery_response,
                        broadcast_recovery.recovery_request,
                        broadcast_recovery.recovery_provider_identity_key,
                        broadcast_parameters, Params(), *node_ctx->chainman,
                        authorization_time, broadcast_trusted,
                        broadcast_unsigned_psbt, error) ||
                    !ValidateAlternativeRecoverySubmitAgainstChainstate(
                        broadcast_submit,
                        broadcast_recovery.recovery_response,
                        broadcast_trusted, broadcast_parameters, Params(),
                        *node_ctx->chainman, authorization_time,
                        broadcast_user_psbt, error)) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_CONFLICT" : error);
                }
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, final_ref, broadcast_presence, error,
                        service_txindex_mode)) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_PREFLIGHT_FAILED" : error);
                }
                if (!ValidateFinalAlternativeRecoveryAgainstChainstate(
                        final_transaction,
                        broadcast_recovery.expected_wtxid,
                        broadcast_trusted, broadcast_parameters, Params(),
                        *node_ctx->chainman, authorization_time,
                        broadcast_presence != FinalTransactionPresence::NONE,
                        error)) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_CONFLICT" : error);
                }
                recovery = std::move(broadcast_recovery);
                commit = std::move(broadcast_commit);
                bool already_confirmed{false};
                std::string broadcast_error;
                const bool broadcast = InsertAndBroadcastPaymasterTransaction(
                    *wallet, final_ref, already_confirmed, broadcast_error);
                if (!broadcast) {
                    throw JSONRPCError(
                        IsTransientPaymasterFinalizationError(broadcast_error) ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        broadcast_error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_PROVIDER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
                }
                UniValue result{UniValue::VOBJ};
                result.pushKV("processed", true);
                result.pushKV("queued", queue_result(recovery));
                result.pushKV("message_type", "recovery_submit");
                result.pushKV("request_id", recovery.request_id);
                result.pushKV("session_id", recovery.session_id.GetHex());
                result.pushKV("provider_id",
                              recovery.recovery_provider_id.GetHex());
                result.pushKV("commit_key", commit.commit_key.GetHex());
                result.pushKV("txid", commit.final_txid.GetHex());
                result.pushKV("result_status", "final_committed");
                result.pushKV("broadcast", broadcast);
                if (!broadcast_error.empty()) {
                    result.pushKV("broadcast_error", broadcast_error);
                }
                result.pushKV("recovery",
                              AlternativeRecoveryToJSON(recovery));
                return result;
            }
            const auto submit_lease =
                context.paymaster->LeaseSubmits(identity.provider_id, 1);
            const auto& messages = submit_lease.Messages();
            UniValue result{UniValue::VOBJ};
            result.pushKV("processed", !messages.empty());
            result.pushKV("queued", false);
            if (messages.empty()) return result;
            const DirectMessage& direct = messages.front();
            ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                      direct};
            const auto* submit = std::get_if<PaymasterSubmit>(&direct.payload);
            if (!submit) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "PAYMASTER_SUBMIT_QUEUE_CORRUPT");
            }
            if (!ValidateSubmitEnvelope(*submit, Params().GenesisBlock().GetHash(), error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            PaymasterStore store{*wallet};
            ProviderAttempt attempt;
            if (!store.GetAttemptByTemplateCommitment(submit->template_commitment, attempt) ||
                attempt.provider_id != submit->provider_id || attempt.session_id != submit->session_id ||
                attempt.quote_id != submit->quote_id || attempt.commit_key != submit->commit_key) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SUBMIT_BINDING_MISMATCH");
            }
            PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session) ||
                session.request_id != submit->request_id) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_SUBMIT_SESSION_MISMATCH");
            }

            PartiallySignedTransaction submitted_psbt;
            CollaborativePSBTTemplate trusted;
            if (!DecodeRawPSBT(submitted_psbt, MakeByteSpan(submit->user_psbt), error) ||
                !submitted_psbt.tx || !LoadTrustedTemplate(attempt, trusted, error) ||
                !ValidateCollaborativePSBT(submitted_psbt, trusted,
                                           CollaborativeSignatureStage::USER_SIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            PaymasterResult provider_result;
            bool rejected{false};
            if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *submitted_psbt.tx,
                                                 GetTime(), rejected, provider_result, error)) {
                throw JSONRPCError(
                    error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                    error);
            }

            UniValue committed;
            if (!rejected) {
                JSONRPCRequest nested{request};
                nested.params = UniValue{UniValue::VARR};
                nested.params.push_back(EncodeBase64(submit->user_psbt));
                committed = submitpaymasterdigidollar().HandleRequest(nested);
                if (!store.GetProviderResult(submit->commit_key, provider_result) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_RESULT_NOT_DURABLE");
                }
            }
            PaymasterResultMessage message;
            message.request_id = submit->request_id;
            message.session_id = submit->session_id;
            message.result = provider_result;
            CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
            stream << message;
            const auto bytes = MakeUCharSpan(stream);
            const std::vector<unsigned char> encoded{bytes.begin(), bytes.end()};
            const bool queued = context.paymaster->QueueOutboundDirectMessage(
                messages.front().peer_id, Hash(encoded), encoded.size(), DirectPayload{message}, GetTime());
            node::NodeContext* node = wallet->chain().context();
            if (queued && node && node->connman) node->connman->WakeMessageHandler();
            result.pushKV("queued", queued);
            result.pushKV("request_id", message.request_id);
            result.pushKV("session_id", message.session_id.GetHex());
            result.pushKV("provider_id", provider_result.provider_id.GetHex());
            result.pushKV("commit_key", provider_result.commit_key.GetHex());
            if (provider_result.txid) result.pushKV("txid", provider_result.txid->GetHex());
            result.pushKV("result_status", ResultStatusName(provider_result.status));
            result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
            if (rejected) {
                result.pushKV("rejection_code", "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE");
            } else {
                result.pushKV("commit", committed);
            }
            return result;
        },
    };
}

RPCHelpMan processpaymasterrequests()
{
    return RPCHelpMan{
        "processpaymasterrequests",
        "Process at most one addressed PMCAPREQ or PMQUOTEREQ for this running provider wallet. "
        "Capacity is proven before any payment intent is accepted. New proofs and quotes require "
        "an unlocked wallet; exact durable retries do not.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider quote processing result", {
                                                                                    {RPCResult::Type::BOOL, "processed", "Whether a request was available"},
                                                                                    {RPCResult::Type::BOOL, "queued", "Whether the signed response was queued"},
                                                                                    {RPCResult::Type::BOOL, "rejected", /*optional=*/true, "Whether a permanently invalid inbound continuation was discarded"},
                                                                                    {RPCResult::Type::STR, "rejection_reason", /*optional=*/true, "Stable reason for discarding a permanently invalid continuation"},
                                                                                    {RPCResult::Type::STR, "message_type", /*optional=*/true, "capacity, recovery_request, or quote"},
                                                                                    {RPCResult::Type::STR_HEX, "client_nonce", /*optional=*/true, "Capacity-bound client nonce"},
                                                                                    {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Signed capacity snapshot"},
                                                                                    {RPCResult::Type::STR, "request_id", /*optional=*/true, "Canonical request UUID"},
                                                                                    {RPCResult::Type::STR_HEX, "session_id", /*optional=*/true, "Client session"},
                                                                                    {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                                    {RPCResult::Type::STR_HEX, "attempt_id", /*optional=*/true, "Durable provider attempt"},
                                                                                    {RPCResult::Type::STR_HEX, "recovery_id", /*optional=*/true, "Stable alternative-recovery identifier"},
                                                                                    {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Signed quote"},
                                                                                    {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Exact template"},
                                                                                }},
        RPCExamples{HelpExampleCli("processpaymasterrequests", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool automatic_service =
                request.strMethod == INTERNAL_PAYMASTER_SERVICE_METHOD;
            if (automatic_service) {
                if (!AutomaticProviderStateIsSynchronized(*wallet)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_SYNCING");
                }
            } else {
                wallet->BlockUntilSyncedToCurrentChain();
            }
            const ProviderReadiness readiness =
                GetProviderReadiness(*wallet, context, !automatic_service);
            if (automatic_service &&
                std::find(readiness.errors.begin(), readiness.errors.end(),
                          "PAYMASTER_REQUIRES_READY_TXINDEX") !=
                    readiness.errors.end()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SYNCING");
            }
            if (!context.paymaster || !readiness.have_identity ||
                !context.paymaster->IsProviderRunning(wallet->GetName(), readiness.identity.provider_id)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_NOT_RUNNING");
            }
            if (readiness.settings.operation_mode ==
                    ProviderOperationMode::AUTOMATIC &&
                !automatic_service) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE");
            }
            ProviderWorkGuard work_guard{*context.paymaster,
                                         wallet->GetName(),
                                         readiness.identity.provider_id};
            if (!work_guard.Acquired()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_SERVICE_BUSY");
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("queued", false);
            const int64_t now = GetTime();
            std::string error;
            node::NodeContext* node = wallet->chain().context();
            if (!node || !node->chainman) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
            }

            // A reserved operational slot intentionally makes staged pool
            // readiness false for *new* clients. The same automatic service
            // must nevertheless continue the already capacity-bound quote
            // request behind it. Skip fresh capacity work while liquidity is
            // unavailable, then fall through to recovery/quote continuations.
            if (!automatic_service || readiness.ready) {
                const auto capacity_lease =
                    context.paymaster->LeaseCapacityRequests(
                        readiness.identity.provider_id, 1);
                const auto& capacity_messages = capacity_lease.Messages();
                if (!capacity_messages.empty()) {
                    result.pushKV("processed", true);
                    result.pushKV("message_type", "capacity");
                    const DirectMessage& direct = capacity_messages.front();
                    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                              direct};
                    const auto* capacity_request = std::get_if<PaymasterCapacityRequest>(
                        &direct.payload);
                    if (!capacity_request || !ValidateCapacityRequestEnvelope(
                                                 *capacity_request,
                                                 Params().GenesisBlock().GetHash(),
                                                 now, error)) {
                        if (!AcknowledgeRejectedDirectMessage(
                                *context.paymaster, direct, error)) {
                            throw JSONRPCError(RPC_WALLET_ERROR, error);
                        }
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            error.empty() ? "PAYMASTER_INVALID_CAPACITY_REQUEST" : error);
                    }
                    if (wallet->IsLocked()) {
                        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                           "Provider wallet unlock is required for a capacity proof");
                    }
                    if (!readiness.ready) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_PROVIDER_NOT_RUNNING");
                    }
                    uint256 reference_block;
                    int tip_height{-1};
                    {
                        LOCK(cs_main);
                        const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
                        if (tip) {
                            reference_block = tip->GetBlockHash();
                            tip_height = tip->nHeight;
                        }
                    }
                    if (reference_block.IsNull() || tip_height < 0) {
                        throw JSONRPCError(RPC_MISC_ERROR,
                                           "PAYMASTER_CAPACITY_CHAINSTATE_UNAVAILABLE");
                    }
                    if (capacity_messages.front().canonical_netgroup.empty()) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
                    }
                    PaymasterCapacityProof proof;
                    std::vector<ProviderPoolEntry> reserved;
                    if (!ReserveAndBuildPaymasterCapacityProof(
                            *wallet, readiness.identity, *capacity_request,
                            Params().GenesisBlock().GetHash(), reference_block,
                            tip_height, /*minimum_confirmations=*/1,
                            capacity_messages.front().canonical_netgroup, now,
                            proof, reserved, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    const std::vector<unsigned char> proof_bytes =
                        SerializeCapacityProof(proof);
                    const uint256 message_id = Hash(proof_bytes);
                    const bool queued = context.paymaster->QueueCapacityProof(
                        capacity_messages.front().peer_id, message_id,
                        proof_bytes.size(), proof, now);
                    if (queued && node->connman) node->connman->WakeMessageHandler();
                    result.pushKV("queued", queued);
                    result.pushKV("provider_id", proof.provider_id.GetHex());
                    result.pushKV("client_nonce", proof.client_nonce.GetHex());
                    result.pushKV("capacity_snapshot_id", proof.snapshot_id.GetHex());
                    return result;
                }
            }

            const auto recovery_lease =
                context.paymaster->LeaseRecoveryRequests(
                    readiness.identity.provider_id, 1);
            const auto& recovery_messages = recovery_lease.Messages();
            if (!recovery_messages.empty()) {
                result.pushKV("processed", true);
                result.pushKV("message_type", "recovery_request");
                const DirectMessage& direct = recovery_messages.front();
                ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                          direct};
                const auto* recovery_request =
                    std::get_if<AlternativeRecoveryRequest>(&direct.payload);
                if (!recovery_request ||
                    !ValidateAlternativeRecoveryRequestEnvelope(
                        *recovery_request, Params().GenesisBlock().GetHash(),
                        now, error)) {
                    if (!AcknowledgeRejectedDirectMessage(
                            *context.paymaster, direct, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_INVALID_RECOVERY_REQUEST" : error);
                }
                if (direct.canonical_netgroup.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
                }
                if (recovery_request->privacy_profile == PrivacyProfile::HIGH) {
                    bool onion_peer{false};
                    if (node->connman) {
                        node->connman->ForEachNode([&](CNode* peer) {
                            if (peer->GetId() == direct.peer_id &&
                                peer->addr.IsTor()) {
                                onion_peer = true;
                            }
                        });
                    }
                    if (!onion_peer) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RECOVERY_HIGH_PRIVACY_REQUIRES_ONION");
                    }
                }

                PaymasterStore store{*wallet};
                uint256 netgroup_bucket;
                const uint256 recovery_id = GetAlternativeRecoveryId(
                    recovery_request->request_id,
                    recovery_request->session_id,
                    recovery_request->recovery_provider_id,
                    recovery_request->client_nonce);
                AlternativeRecoveryRecord persisted;
                if (store.GetAlternativeRecoveryById(recovery_id, persisted)) {
                    if (!persisted.provider_side || persisted.expired ||
                        persisted.recovery_request_hash !=
                            GetAlternativeRecoveryRequestHash(*recovery_request) ||
                        SerializeRecoveryMessage(persisted.recovery_request) !=
                            SerializeRecoveryMessage(*recovery_request) ||
                        persisted.phase <
                            AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
                    }
                    const std::vector<unsigned char> encoded =
                        SerializeRecoveryMessage(persisted.recovery_response);
                    const bool queued =
                        context.paymaster->HasOutboundDirectMessage(
                            direct.peer_id, Hash(encoded)) ||
                        context.paymaster->QueueOutboundDirectMessage(
                            direct.peer_id, Hash(encoded), encoded.size(),
                            DirectPayload{persisted.recovery_response}, now);
                    if (queued && node->connman) {
                        node->connman->WakeMessageHandler();
                    }
                    result.pushKV("queued", queued);
                    result.pushKV("request_id", persisted.request_id);
                    result.pushKV("session_id",
                                  persisted.session_id.GetHex());
                    result.pushKV("provider_id",
                                  persisted.recovery_provider_id.GetHex());
                    result.pushKV("recovery_id",
                                  persisted.recovery_id.GetHex());
                    result.pushKV(
                        "template_commitment",
                        persisted.recovery_response.manifest
                            .template_commitment.GetHex());
                    return result;
                }

                if (wallet->IsLocked()) {
                    throw JSONRPCError(
                        RPC_WALLET_UNLOCK_NEEDED,
                        "Provider wallet unlock is required for a recovery quote");
                }
                const bool continuation_ready =
                    CapacityContinuationMayProceed(readiness);
                if (!continuation_ready ||
                    !ProviderTxIndexIsReady(*wallet, !automatic_service)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        !continuation_ready ? "PAYMASTER_PROVIDER_DRAIN_ONLY" : "PAYMASTER_REQUIRES_READY_TXINDEX");
                }
                HashWriter offer_hasher =
                    TaggedHash("DigiByte Paymaster Offer v1");
                offer_hasher << readiness.identity.provider_id
                             << recovery_request->policy_hash
                             << static_cast<uint8_t>(FundingModel::USER_PAID);
                const uint256 expected_offer_id = offer_hasher.GetSHA256();
                if (recovery_request->policy_hash !=
                        GetProviderPolicyHash(readiness.policy) ||
                    recovery_request->offer_id !=
                        expected_offer_id ||
                    !PolicyAllowsFundingModel(readiness.policy,
                                              FundingModel::USER_PAID)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "PAYMASTER_RECOVERY_POLICY_MISMATCH");
                }

                PaymasterCapacityProof capacity_proof;
                if (!store.GetProviderCapacityProof(
                        recovery_request->capacity_request, now,
                        capacity_proof, error) ||
                    capacity_proof.snapshot_id !=
                        recovery_request->capacity_snapshot_id ||
                    GetCapacityResourceCommitment(capacity_proof) !=
                        recovery_request->capacity_resource_commitment ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof,
                        recovery_request->capacity_request,
                        readiness.identity.identity_key, *node->chainman,
                        now, error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_RECOVERY_CAPACITY_BINDING_MISMATCH" : error);
                }

                std::vector<CollaborativeInput> user_inputs;
                for (const COutPoint& outpoint :
                     recovery_request->user_dd_inputs) {
                    uint256 block_hash;
                    CTransactionRef creating_tx;
                    if (!g_txindex->FindTx(outpoint.hash, block_hash,
                                           creating_tx) ||
                        !creating_tx) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_FOUND");
                    }
                    user_inputs.push_back(
                        {outpoint, creating_tx, InputRole::USER_DD});
                }
                ValidatedUserDDInputs validated_user_inputs;
                {
                    LOCK(cs_main);
                    if (!ValidateCollaborativeUserDDInputs(
                            user_inputs,
                            node->chainman->ActiveChainstate().CoinsTip(),
                            validated_user_inputs, error)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                    }
                }
                if (!ValidateAlternativeRecoveryRequestInputs(
                        *recovery_request, validated_user_inputs.output_keys,
                        error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                const CAmount total_dd{
                    validated_user_inputs.total_dd_amount};
                CAmount returned_dd{0};
                for (const AlternativeRecoveryReturn& output :
                     recovery_request->wallet_returns) {
                    if (output.amount.value >
                        std::numeric_limits<CAmount>::max() - returned_dd) {
                        throw JSONRPCError(
                            RPC_INVALID_PARAMETER,
                            "PAYMASTER_RECOVERY_AMOUNT_OVERFLOW");
                    }
                    returned_dd += output.amount.value;
                }
                const auto fee_plan = EvaluateServiceFee(
                    readiness.policy, FundingModel::USER_PAID,
                    DDCents{total_dd}, error);
                if (!fee_plan ||
                    !(fee_plan->service_fee ==
                      recovery_request->service_fee) ||
                    total_dd - returned_dd !=
                        recovery_request->service_fee.value) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_RECOVERY_SERVICE_FEE_MISMATCH" : error);
                }

                ValidatedCapacitySnapshot snapshot;
                snapshot.snapshot_id = capacity_proof.snapshot_id;
                snapshot.resource_commitment =
                    GetCapacityResourceCommitment(capacity_proof);
                snapshot.session_id = recovery_request->session_id;
                snapshot.attempt_id = recovery_id;
                snapshot.provider_id = capacity_proof.provider_id;
                snapshot.client_nonce = capacity_proof.client_nonce;
                snapshot.request_hash = Hash(
                    SerializeCapacityRequest(
                        recovery_request->capacity_request));
                snapshot.capacity_proof =
                    SerializeCapacityProof(capacity_proof);
                snapshot.created_at = capacity_proof.created_at;
                snapshot.expires_at = capacity_proof.expires_at;
                snapshot.validated_at = now;
                snapshot.funding_model = capacity_proof.funding_model;
                snapshot.requires_carrier = capacity_proof.requires_carrier;

                AlternativeRecoveryParameters parameters;
                parameters.genesis_hash = recovery_request->genesis_hash;
                parameters.request_id = recovery_request->request_id;
                parameters.session_id = recovery_request->session_id;
                parameters.original_provider_id =
                    recovery_request->original_provider_id;
                parameters.recovery_provider_id =
                    recovery_request->recovery_provider_id;
                parameters.privacy_profile =
                    recovery_request->privacy_profile;
                parameters.offer_id = recovery_request->offer_id;
                parameters.policy_hash = recovery_request->policy_hash;
                parameters.original_commit_key =
                    recovery_request->original_commit_key;
                parameters.original_template_commitment =
                    recovery_request->original_template_commitment;
                parameters.capacity_request =
                    recovery_request->capacity_request;
                parameters.capacity_snapshot = snapshot;
                parameters.recovery_provider_identity_key =
                    readiness.identity.identity_key;
                parameters.persisted_user_dd_inputs =
                    recovery_request->user_dd_inputs;
                parameters.user_dd_inputs = std::move(user_inputs);
                parameters.wallet_returns =
                    recovery_request->wallet_returns;
                for (const AlternativeRecoveryReturn& output :
                     parameters.wallet_returns) {
                    parameters.wallet_verified_fresh_return_scripts.push_back(
                        output.script_pub_key);
                }
                const PaymasterLiquiditySlot& slot =
                    capacity_proof.liquidity_slots.front();
                const CAmount network_fee{COIN / 10};
                CAmount provider_dgb{0};
                for (const CapacityDGBInput& input : slot.dgb_inputs) {
                    if (input.input.value.value >
                        std::numeric_limits<CAmount>::max() - provider_dgb) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_RECOVERY_AMOUNT_OVERFLOW");
                    }
                    provider_dgb += input.input.value.value;
                }
                if (network_fee > readiness.policy.maximum_network_fee.value ||
                    provider_dgb < network_fee) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
                }
                if (!store.CheckProviderRecoveryAdmission(
                        *recovery_request,
                        GetAlternativeRecoveryRequestHash(*recovery_request),
                        direct.canonical_netgroup, slot.carrier.has_value(),
                        DGBSatoshis{network_fee}, now, netgroup_bucket,
                        error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : error);
                }
                const bool need_dd_script =
                    slot.carrier.has_value() ||
                    recovery_request->service_fee.value > 0;
                if (need_dd_script) {
                    const auto destination = wallet->GetNewDestination(
                        OutputType::BECH32M,
                        "Paymaster recovery DD return");
                    if (!destination) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_RECOVERY_CHANGE_UNAVAILABLE");
                    }
                    parameters.recovery_provider_dd_script =
                        GetScriptForDestination(*destination);
                }
                if (provider_dgb > network_fee) {
                    const auto destination = wallet->GetNewDestination(
                        OutputType::BECH32M,
                        "Paymaster recovery DGB change");
                    if (!destination) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "PAYMASTER_RECOVERY_CHANGE_UNAVAILABLE");
                    }
                    parameters.recovery_provider_dgb_change_script =
                        GetScriptForDestination(*destination);
                }
                parameters.maximum_service_fee =
                    recovery_request->maximum_service_fee;
                parameters.service_fee = recovery_request->service_fee;
                parameters.network_fee = DGBSatoshis{network_fee};
                parameters.fee_rate = 1;
                parameters.expires_at = std::min(
                    recovery_request->expires_at,
                    SaturatingAddSeconds(now, readiness.policy.quote_ttl));

                AlternativeRecoveryTemplate built;
                if (!BuildAlternativeRecoveryTemplateAgainstChainstate(
                        parameters, Params(), *node->chainman, now, built,
                        error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
                AlternativeRecoveryResponse response;
                response.genesis_hash = recovery_request->genesis_hash;
                response.request_id = recovery_request->request_id;
                response.session_id = recovery_request->session_id;
                response.recovery_id = recovery_id;
                response.recovery_provider_id =
                    recovery_request->recovery_provider_id;
                response.recovery_request_hash =
                    GetAlternativeRecoveryRequestHash(*recovery_request);
                response.manifest = built.manifest;
                response.unsigned_psbt =
                    SerializePSBT(built.trusted_template.psbt);
                response.created_at = now;
                response.expires_at = parameters.expires_at;
                response.recovery_commit_key =
                    GetAlternativeRecoveryCommitKey(response);
                CKey identity_key;
                ProviderIdentityRecord identity;
                response.identity_signature.resize(64);
                if (!GetPaymasterIdentityKey(*wallet, identity_key, identity,
                                             error) ||
                    identity.provider_id !=
                        response.recovery_provider_id ||
                    !identity_key.SignSchnorr(
                        GetAlternativeRecoveryResponseSignatureHash(response),
                        response.identity_signature, nullptr, GetRandHash())) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_RECOVERY_RESPONSE_SIGNING_FAILED" : error);
                }
                if (!ValidateAlternativeRecoveryResponse(
                        response, *recovery_request, identity.identity_key,
                        now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }

                AlternativeRecoveryRecord recovery;
                recovery.provider_side = true;
                recovery.request_id = recovery_request->request_id;
                recovery.session_id = recovery_request->session_id;
                recovery.recovery_id = recovery_id;
                recovery.recovery_request_hash =
                    response.recovery_request_hash;
                recovery.original_provider_id =
                    recovery_request->original_provider_id;
                recovery.recovery_provider_id =
                    recovery_request->recovery_provider_id;
                recovery.privacy_profile =
                    recovery_request->privacy_profile;
                recovery.offer_id = recovery_request->offer_id;
                recovery.policy_hash = recovery_request->policy_hash;
                recovery.recovery_provider_identity_key =
                    identity.identity_key;
                recovery.recovery_provider_endpoint =
                    readiness.endpoint.ToStringAddrPort();
                recovery.original_commit_key =
                    recovery_request->original_commit_key;
                recovery.original_template_commitment =
                    recovery_request->original_template_commitment;
                recovery.selected_maximum_service_fee =
                    recovery_request->maximum_service_fee;
                recovery.selected_service_fee =
                    recovery_request->service_fee;
                recovery.client_nonce = recovery_request->client_nonce;
                recovery.capacity_request =
                    recovery_request->capacity_request;
                recovery.capacity_snapshot = snapshot;
                recovery.recovery_request = *recovery_request;
                recovery.recovery_response = response;
                recovery.provider_safety_policy_hash =
                    GetProviderSafetyPolicyHash(readiness.safety_policy);
                recovery.provider_budget_reservation_id =
                    response.recovery_commit_key;
                recovery.provider_netgroup_bucket = netgroup_bucket;
                recovery.provider_maximum_network_fee =
                    response.manifest.network_fee;
                if (!BuildRecoveryAuthorizationManifest(
                        *recovery_request, response,
                        recovery.recovery_authorization, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                recovery.phase =
                    AlternativeRecoveryPhase::RESPONSE_VALIDATED;
                recovery.created_at = recovery.updated_at = now;
                if (!store.CommitProviderAlternativeRecoveryQuote(
                        recovery, netgroup_bucket,
                        Params().GenesisBlock().GetHash(), now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                const std::vector<unsigned char> encoded =
                    SerializeRecoveryMessage(response);
                const bool queued =
                    context.paymaster->QueueOutboundDirectMessage(
                        direct.peer_id, Hash(encoded), encoded.size(),
                        DirectPayload{response}, now);
                if (queued && node->connman) {
                    node->connman->WakeMessageHandler();
                }
                result.pushKV("queued", queued);
                result.pushKV("request_id", response.request_id);
                result.pushKV("session_id", response.session_id.GetHex());
                result.pushKV("provider_id",
                              response.recovery_provider_id.GetHex());
                result.pushKV("recovery_id", recovery_id.GetHex());
                result.pushKV("template_commitment",
                              response.manifest.template_commitment.GetHex());
                return result;
            }

            const auto quote_lease = context.paymaster->LeaseQuoteRequests(
                readiness.identity.provider_id, 1);
            const auto& messages = quote_lease.Messages();
            result.pushKV("processed", !messages.empty());
            if (messages.empty()) return result;
            result.pushKV("message_type", "quote");
            const DirectMessage& direct = messages.front();
            ProviderInboundMessageGuard message_guard{*context.paymaster,
                                                      direct};
            const auto* quote_request =
                std::get_if<PaymasterQuoteRequest>(&direct.payload);
            if (!quote_request) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "PAYMASTER_REQUEST_QUEUE_CORRUPT");
            }

            if (!ValidateQuoteRequestEnvelope(*quote_request, Params().GenesisBlock().GetHash(),
                                              now, error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            std::optional<SponsorshipAuthorizationRecord> sponsorship;
            const bool restricted = quote_request->intent.funding_model == FundingModel::SPONSORED &&
                                    quote_request->intent.sponsorship_scope ==
                                        SponsorshipScope::RESTRICTED;
            const auto validate_restricted_request = [&] {
                if (!restricted) return;
                const auto& descriptor = *quote_request->restricted_descriptor;
                const auto& capability = *quote_request->restricted_capability;
                if (readiness.policy.sponsorship_scope != SponsorshipScope::RESTRICTED ||
                    readiness.policy.funding_models != FUNDING_MODEL_SPONSORED ||
                    descriptor.policy_hash != GetProviderPolicyHash(readiness.policy) ||
                    !ValidateRestrictedServiceDescriptor(descriptor, readiness.identity.identity_key,
                                                         Params().GenesisBlock().GetHash(), now, error,
                                                         Params().GetChainType() == ChainType::REGTEST) ||
                    !ValidateSponsorshipCapability(capability, descriptor,
                                                   quote_request->intent.recipient_script,
                                                   quote_request->intent.recipient_amount,
                                                   quote_request->intent.client_nonce,
                                                   Params().GenesisBlock().GetHash(), now, error) ||
                    !ValidateRestrictedAdmissionProofs(descriptor, readiness.identity.identity_key,
                                                       *node->chainman, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       error.empty() ? "PAYMASTER_RESTRICTED_POLICY_MISMATCH" : error);
                }
            };

            PaymasterStore store{*wallet};
            const std::vector<unsigned char> redacted_request =
                SerializeRedactedQuoteRequest(*quote_request);
            PaymentSession persisted_session;
            if (store.GetSessionByRequestId(quote_request->intent.request_id,
                                            persisted_session)) {
                ProviderAttempt persisted_attempt;
                const uint256 canonical_request_hash = Hash(redacted_request);
                if (!persisted_session.provider_side ||
                    persisted_session.session_id != quote_request->intent.session_id ||
                    persisted_session.canonical_request_hash != canonical_request_hash ||
                    persisted_session.attempt_ids.size() != 1 ||
                    !store.GetAttempt(persisted_session.attempt_ids.front(), persisted_attempt) ||
                    persisted_attempt.session_id != quote_request->intent.session_id ||
                    persisted_attempt.provider_id != readiness.identity.provider_id ||
                    persisted_attempt.client_nonce != quote_request->intent.client_nonce ||
                    persisted_attempt.intent_hash != GetPaymentIntentHash(quote_request->intent) ||
                    persisted_attempt.quote_request != redacted_request ||
                    persisted_attempt.signed_quote.empty()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_REQUEST_ID_CONFLICT");
                }
                if (persisted_attempt.quote_expires_at <= now ||
                    persisted_attempt.retry_until < now) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_RETRY_EXPIRED");
                }
                validate_restricted_request();

                PaymasterQuoteResponse response;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, persisted_attempt.signed_quote};
                    stream >> response;
                    if (!stream.empty() || SerializeQuoteResponse(response) !=
                                               persisted_attempt.signed_quote) {
                        throw std::ios_base::failure("non-canonical persisted quote");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
                }
                if (response.request_id != quote_request->intent.request_id ||
                    response.session_id != quote_request->intent.session_id ||
                    response.quote.provider_id != readiness.identity.provider_id ||
                    response.quote.quote_id != persisted_attempt.quote_id ||
                    response.quote.template_commitment !=
                        persisted_attempt.template_commitment) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
                }

                const uint256 message_id = Hash(persisted_attempt.signed_quote);
                const bool queued = context.paymaster->QueueOutboundDirectMessage(
                    messages.front().peer_id, message_id,
                    persisted_attempt.signed_quote.size(), DirectPayload{response}, now);
                if (queued && node->connman) node->connman->WakeMessageHandler();
                result.pushKV("queued", queued);
                result.pushKV("request_id", response.request_id);
                result.pushKV("session_id", response.session_id.GetHex());
                result.pushKV("provider_id", response.quote.provider_id.GetHex());
                result.pushKV("attempt_id", persisted_attempt.attempt_id.GetHex());
                result.pushKV("quote_id", response.quote.quote_id.GetHex());
                result.pushKV("template_commitment",
                              response.quote.template_commitment.GetHex());
                return result;
            }

            if (messages.front().canonical_netgroup.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
            }
            if (!CapacityContinuationMayProceed(readiness)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_PROVIDER_DRAIN_ONLY");
            }
            uint256 provider_netgroup_bucket;

            if (wallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                   "Provider wallet unlock is required for a new quote");
            }
            if (!ProviderTxIndexIsReady(*wallet, !automatic_service)) {
                throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_REQUIRES_READY_TXINDEX");
            }
            validate_restricted_request();

            std::vector<CollaborativeInput> user_inputs;
            for (const COutPoint& outpoint : quote_request->intent.user_dd_inputs) {
                uint256 block_hash;
                CTransactionRef creating_tx;
                if (!g_txindex->FindTx(outpoint.hash, block_hash, creating_tx) || !creating_tx) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PREVOUT_NOT_FOUND");
                }
                user_inputs.push_back({outpoint, creating_tx, InputRole::USER_DD});
            }
            ProviderQuotePreflightResult quote_preflight;
            {
                LOCK(cs_main);
                if (!PreflightProviderQuoteRequest(
                        *quote_request, readiness.policy, user_inputs, Params(),
                        node->chainman->ActiveChainstate().CoinsTip(), now,
                        quote_preflight, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
            }
            const ServiceFeePlan* const fee_plan{&quote_preflight.fee_plan};
            const bool needs_carrier = fee_plan->output_kind == FeeOutputKind::CARRIER_SUCCESSOR;
            std::vector<ProviderPoolEntry> available;
            if (!GetPaymasterProviderPoolEntries(*wallet, available)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOLS_NOT_PREPARED");
            }
            std::sort(available.begin(), available.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.outpoint < rhs.outpoint;
            });
            const auto usable = [&](const ProviderPoolEntry& entry, PoolAsset asset) {
                return entry.purpose == PoolPurpose::OPERATIONAL && entry.asset == asset &&
                       entry.state == PoolEntryState::RESERVED &&
                       entry.reservation_id == quote_request->intent.client_nonce &&
                       entry.confirmation_height > 0;
            };
            const auto dgb_it = std::find_if(available.begin(), available.end(), [&](const auto& entry) {
                return usable(entry, PoolAsset::DGB) &&
                       entry.dgb_value.value >= readiness.policy.maximum_network_fee.value;
            });
            const auto carrier_it = std::find_if(available.begin(), available.end(), [&](const auto& entry) {
                return usable(entry, PoolAsset::DD_CARRIER);
            });
            if (dgb_it == available.end() || (needs_carrier && carrier_it == available.end())) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_OPERATIONAL_SLOT_MISSING");
            }
            const CAmount network_fee{COIN / 10};
            if (readiness.policy.maximum_network_fee.value < network_fee ||
                dgb_it->dgb_value.value < network_fee) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
            }
            if (!store.CheckProviderQuoteAdmission(
                    *quote_request, Hash(redacted_request),
                    messages.front().canonical_netgroup, needs_carrier,
                    DGBSatoshis{network_fee}, now,
                    provider_netgroup_bucket, error)) {
                if (IsPermanentCapacityContinuationError(error)) {
                    // The exact quote can no longer be authorized because its
                    // preceding Capacity reservation is absent, expired, or
                    // conflicts with the persisted proof. Consume only this
                    // invalid inbox entry and leave the provider's maintenance
                    // state intact; a remote peer must not be able to turn a
                    // safely rejected continuation into a provider fault.
                    const std::string rejection_reason{error};
                    if (!AcknowledgeRejectedDirectMessage(
                            *context.paymaster, direct, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    result.pushKV("rejected", true);
                    result.pushKV("rejection_reason", rejection_reason);
                    return result;
                }
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : error);
            }
            CTransactionRef dgb_creating_tx;
            CTransactionRef carrier_creating_tx;
            {
                LOCK(wallet->cs_wallet);
                const CWalletTx* dgb_wallet_tx = wallet->GetWalletTx(dgb_it->outpoint.hash);
                if (dgb_wallet_tx) dgb_creating_tx = dgb_wallet_tx->tx;
                if (needs_carrier) {
                    const CWalletTx* carrier_wallet_tx = wallet->GetWalletTx(carrier_it->outpoint.hash);
                    if (carrier_wallet_tx) carrier_creating_tx = carrier_wallet_tx->tx;
                }
            }
            if (!dgb_creating_tx || (needs_carrier && !carrier_creating_tx)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_POOL_PREVOUT_NOT_FOUND");
            }

            ProviderQuoteBuildParameters parameters;
            do {
                parameters.quote_id = GetRandHash();
            } while (parameters.quote_id.IsNull());
            parameters.user_inputs = std::move(user_inputs);
            parameters.reserved_dgb_inputs.push_back(
                {dgb_it->outpoint, CMutableTransaction{*dgb_creating_tx}, dgb_it->dgb_value});
            if (needs_carrier) {
                parameters.reserved_carrier = VerifiedDDCarrier{
                    carrier_it->outpoint, CMutableTransaction{*carrier_creating_tx},
                    carrier_it->carrier_value};
                const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                   "Paymaster carrier successor");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                parameters.carrier_return_script = GetScriptForDestination(*destination);
            } else if (fee_plan->output_kind == FeeOutputKind::PROVIDER_OUTPUT) {
                const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                   "Paymaster service fee");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                parameters.provider_fee_script = GetScriptForDestination(*destination);
            }
            if (dgb_it->dgb_value.value > network_fee) {
                const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                                   "Paymaster DGB change");
                if (!destination) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_CHANGE_UNAVAILABLE");
                parameters.dgb_change_script = GetScriptForDestination(*destination);
            }
            parameters.network_fee = DGBSatoshis{network_fee};
            parameters.created_at = now;
            parameters.expires_at = SaturatingAddSeconds(now, readiness.policy.quote_ttl);
            parameters.retry_until = SaturatingAddSeconds(now, DEFAULT_RETRY_SECONDS);
            ProviderQuoteBuildResult built;
            {
                LOCK(cs_main);
                if (!BuildUnsignedProviderQuote(*quote_request, readiness.policy, parameters,
                                                Params(), node->chainman->ActiveChainstate().CoinsTip(),
                                                built, error)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, error);
                }
            }
            ProviderAuthorizationManifest provider_manifest;
            if (!BuildProviderAuthorizationManifest(
                    quote_request->intent, built.quote, built.trusted_template,
                    GetProviderSafetyPolicyHash(readiness.safety_policy),
                    provider_manifest, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (!ValidateProviderAuthorizationOwnership(
                    *wallet, provider_manifest, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            CKey identity_key;
            ProviderIdentityRecord identity;
            built.quote.identity_signature.resize(64);
            if (!GetPaymasterIdentityKey(*wallet, identity_key, identity, error) ||
                !identity_key.SignSchnorr(GetPaymasterQuoteSignatureHash(built.quote),
                                          built.quote.identity_signature, nullptr,
                                          GetRandHash())) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "PAYMASTER_QUOTE_SIGNING_FAILED" : error);
            }
            PaymasterQuoteResponse response;
            response.request_id = quote_request->intent.request_id;
            response.session_id = quote_request->intent.session_id;
            response.quote = built.quote;
            ProviderAttempt attempt;
            do {
                attempt.attempt_id = GetRandHash();
            } while (attempt.attempt_id.IsNull());
            attempt.session_id = response.session_id;
            attempt.provider_id = response.quote.provider_id;
            attempt.provider_identity_key = identity.identity_key;
            attempt.state = AttemptState::QUOTED;
            attempt.client_nonce = quote_request->intent.client_nonce;
            attempt.intent_hash = response.quote.intent_hash;
            attempt.quote_id = response.quote.quote_id;
            attempt.unsigned_txid = response.quote.unsigned_txid;
            attempt.template_commitment = response.quote.template_commitment;
            attempt.commit_key = GetPaymasterCommitKey(attempt.provider_id, attempt.client_nonce,
                                                       attempt.intent_hash, attempt.quote_id,
                                                       attempt.template_commitment);
            if (restricted) {
                attempt.sponsorship_capability_hash =
                    GetSponsorshipCapabilityHash(*quote_request->restricted_capability);
                SponsorshipAuthorizationRecord authorization;
                authorization.capability_hash = attempt.sponsorship_capability_hash;
                authorization.payment_binding_hash = attempt.intent_hash;
                authorization.reservation_id = attempt.commit_key;
                authorization.reserved_at = now;
                authorization.expires_at = quote_request->restricted_capability->expires_at;
                sponsorship = std::move(authorization);
            }
            attempt.quote_request = redacted_request;
            attempt.signed_quote = SerializeQuoteResponse(response);
            attempt.provider_manifest = std::move(provider_manifest);
            attempt.provider_netgroup_bucket = provider_netgroup_bucket;
            attempt.unsigned_transaction = SerializeTransaction(response.quote.unsigned_transaction);
            attempt.unsigned_psbt = SerializePSBT(built.trusted_template.psbt);
            for (const InputRole role : built.trusted_template.input_roles) {
                attempt.input_roles.push_back(static_cast<ReservationRole>(role));
            }
            attempt.quote_expires_at = response.quote.expires_at;
            attempt.retry_until = response.quote.retry_until;
            attempt.created_at = attempt.updated_at = now;
            if (!store.CommitProviderQuote(
                    attempt, sponsorship, Params().GenesisBlock().GetHash(),
                    now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            const std::vector<unsigned char> response_bytes = SerializeQuoteResponse(response);
            const uint256 message_id = Hash(response_bytes);
            const bool queued = context.paymaster->QueueOutboundDirectMessage(
                messages.front().peer_id, message_id, response_bytes.size(), DirectPayload{response}, now);
            if (queued && node->connman) node->connman->WakeMessageHandler();
            result.pushKV("queued", queued);
            result.pushKV("request_id", response.request_id);
            result.pushKV("session_id", response.session_id.GetHex());
            result.pushKV("provider_id", response.quote.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", response.quote.quote_id.GetHex());
            result.pushKV("template_commitment", response.quote.template_commitment.GetHex());
            return result;
        },
    };
}

RPCHelpMan startpaymaster()
{
    return RPCHelpMan{
        "startpaymaster",
        "Start normal Paymaster provider operation after all readiness gates pass. "
        "When safety limits are exhausted, a non-announcing drain-only runtime "
        "is started only if durable quotes or signatures still require exact completion.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider runtime result", {
                                                                           {RPCResult::Type::BOOL, "running", "Whether the provider is now online"},
                                                                           {RPCResult::Type::BOOL, "ready", "Whether every readiness gate passed"},
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
                                                                       }},
        RPCExamples{HelpExampleCli("startpaymaster", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const bool automatic_autostart =
                request.strMethod == INTERNAL_PAYMASTER_AUTOSTART_METHOD;
            if (automatic_autostart) {
                if (!AutomaticProviderStateIsSynchronized(*wallet)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_PROVIDER_SYNCING");
                }
            } else {
                wallet->BlockUntilSyncedToCurrentChain();
            }
            const int64_t now = GetTime();
            const DurablePaymasterRecoveryReport recovery =
                RecoverDurablePaymasterCommits(*wallet, now);
            UniValue recovery_errors{UniValue::VARR};
            for (const std::string& recovery_error : recovery.errors) {
                recovery_errors.push_back(recovery_error);
            }
            const ProviderReadiness readiness =
                GetProviderReadiness(*wallet, context, !automatic_autostart);
            bool running{false};
            bool drain_work{false};
            if (!readiness.identity.provider_id.IsNull()) {
                std::string drain_error;
                PaymasterStore store{*wallet};
                if (!store.HasProviderDrainWork(
                        readiness.identity.provider_id, drain_work,
                        drain_error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, drain_error);
                }
            }
            const bool drain_only{!readiness.ready && drain_work};
            const auto is_liquidity_error = [](const std::string& value) {
                return value == "PAYMASTER_POOLS_NOT_PREPARED" ||
                       value == "PAYMASTER_ADMISSION_DGB_MISSING" ||
                       value == "PAYMASTER_ADMISSION_CARRIERS_MISSING" ||
                       value == "PAYMASTER_OPERATIONAL_SLOT_MISSING";
            };
            const bool maintenance_start =
                !readiness.ready && !readiness.errors.empty() &&
                readiness.settings.operation_mode == ProviderOperationMode::AUTOMATIC &&
                readiness.have_policy && readiness.have_liquidity_policy &&
                readiness.liquidity_policy.automatic_replenishment &&
                ProviderLiquidityTargetsSatisfyPolicy(
                    readiness.liquidity_policy, readiness.policy) &&
                std::all_of(readiness.errors.begin(), readiness.errors.end(),
                            is_liquidity_error);
            Announcement announcement;
            if (readiness.ready || drain_only || maintenance_start) {
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "Node context unavailable");
                }
                std::string error;
                const bool restricted = readiness.policy.sponsorship_scope ==
                                        SponsorshipScope::RESTRICTED;
                if (readiness.ready && !restricted) {
                    uint256 reference_block;
                    {
                        LOCK(cs_main);
                        const CBlockIndex* tip =
                            node->chainman->ActiveChain().Tip();
                        if (!tip) {
                            throw JSONRPCError(RPC_MISC_ERROR,
                                               "PAYMASTER_NODE_NOT_READY");
                        }
                        reference_block = tip->GetBlockHash();
                    }
                    if (!BuildPaymasterAnnouncement(
                            *wallet, readiness.identity, readiness.policy,
                            readiness.pool_entries, readiness.endpoint,
                            Params().GenesisBlock().GetHash(), reference_block,
                            now, readiness.available_funding_models,
                            announcement, error) ||
                        !ValidateAnnouncementEnvelope(
                            announcement, Params().GenesisBlock().GetHash(),
                            now, error,
                            Params().GetChainType() == ChainType::REGTEST) ||
                        !ValidateAdmissionProofs(
                            announcement, *node->chainman, error) ||
                        !context.paymaster->GetDirectory().AddValidated(
                            announcement, now)) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            error.empty() ? "PAYMASTER_ANNOUNCEMENT_REJECTED" : error);
                    }
                }
                running = context.paymaster->StartProvider(wallet->GetName(), readiness.identity.provider_id);
                if (!running) throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_PROVIDER_RUNTIME_CONFLICT");
                ProviderServiceState initial_state{ProviderServiceState::ACTIVE};
                if (drain_only) {
                    initial_state = ProviderServiceState::DRAIN_ONLY;
                } else if (maintenance_start) {
                    initial_state = !readiness.have_liquidity_policy ||
                                            !readiness.liquidity_policy.paid_maintenance_approved
                        ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                        : ProviderServiceState::REPLENISHING_LIQUIDITY;
                } else if (readiness.settings.operation_mode ==
                           ProviderOperationMode::MANUAL) {
                    initial_state = ProviderServiceState::MANUAL;
                }
                context.paymaster->SetProviderServiceStatus(
                    wallet->GetName(), initial_state);
                if (readiness.ready && !restricted && node->connman) {
                    node->connman->ForEachNode([&announcement, node](CNode* peer) {
                        if (!peer->fSuccessfullyConnected || peer->fDisconnect) return;
                        const CNetMsgMaker maker{peer->GetCommonVersion()};
                        node->connman->PushMessage(peer, maker.Make(NetMsgType::PMANNOUNCE,
                                                                    announcement));
                    });
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("running", running);
            result.pushKV("ready", readiness.ready);
            result.pushKV("drain_only", drain_only && running);
            result.pushKV("operation_mode", ProviderOperationModeName(
                                                readiness.settings.operation_mode));
            result.pushKV("autostart", readiness.settings.autostart);
            ProviderServiceState service_state{ProviderServiceState::STOPPED};
            if (running) {
                if (drain_only) {
                    service_state = ProviderServiceState::DRAIN_ONLY;
                } else if (maintenance_start) {
                    service_state = !readiness.have_liquidity_policy ||
                                            !readiness.liquidity_policy.paid_maintenance_approved
                        ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                        : ProviderServiceState::REPLENISHING_LIQUIDITY;
                } else if (readiness.settings.operation_mode ==
                           ProviderOperationMode::MANUAL) {
                    service_state = ProviderServiceState::MANUAL;
                } else {
                    service_state = ProviderServiceState::ACTIVE;
                }
            } else if (readiness.settings.autostart && wallet->IsLocked()) {
                service_state = ProviderServiceState::WAITING_FOR_UNLOCK;
            } else if (readiness.settings.autostart && !readiness.ready) {
                service_state = ProviderServiceState::WAITING_FOR_READINESS;
            }
            if (!running && context.paymaster) {
                context.paymaster->SetProviderServiceStatus(
                    wallet->GetName(), service_state,
                    readiness.errors.empty() ||
                            service_state == ProviderServiceState::STOPPED
                        ? std::string{}
                        : StableProviderServiceError(
                              readiness.errors.front()));
            }
            result.pushKV("service_state",
                          ProviderServiceStateName(service_state));
            if (readiness.have_safety_policy) {
                result.pushKV("safety_policy",
                              ProviderSafetyPolicyToJSON(
                                  readiness.safety_policy));
            }
            if (!readiness.identity.provider_id.IsNull()) result.pushKV("provider_id", readiness.identity.provider_id.GetHex());
            if (running) {
                result.pushKV("endpoint", readiness.endpoint.ToStringAddrPort());
                if (announcement.sequence != 0) {
                    result.pushKV("announcement_sequence", announcement.sequence);
                }
            }
            result.pushKV("recovery_candidates", recovery.candidates);
            result.pushKV("recovered_broadcasts", recovery.recovered);
            result.pushKV("expired_commits", 0);
            result.pushKV("recovery_errors", std::move(recovery_errors));
            result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
            return result;
        },
    };
}

RPCHelpMan stoppaymaster()
{
    return RPCHelpMan{
        "stoppaymaster",
        "Stop the ephemeral Paymaster provider runtime for this wallet. Durable recovery records are retained.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider runtime result", {{RPCResult::Type::BOOL, "running", "Always false after a successful stop"}}},
        RPCExamples{HelpExampleCli("stoppaymaster", "")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            if (context.paymaster) context.paymaster->StopProvider(wallet->GetName());
            UniValue result{UniValue::VOBJ};
            result.pushKV("running", false);
            return result;
        },
    };
}

// One bounded unit of work per queue keeps the validation scheduler responsive
// and preserves fairness across wallets/providers. Temporary local failures
// retain their lease for retry; permanent protocol failures are durably rejected
// before the message is acknowledged.
void RunPaymasterProviderServiceCycle(WalletContext& context, CWallet& wallet)
{
    using namespace DigiDollar::Paymaster;
    Manager* const manager = context.paymaster;
    if (!manager || !manager->Enabled()) return;

    ProviderSettings settings;
    ProviderIdentityRecord identity;
    if (!GetPaymasterProviderSettings(wallet, settings) ||
        !GetPaymasterIdentity(wallet, identity) || !settings.enabled) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::STOPPED);
        return;
    }

    bool running = manager->IsProviderRunning(wallet.GetName(),
                                              identity.provider_id);
    if (!running) {
        if (!settings.autostart) {
            manager->SetProviderServiceStatus(wallet.GetName(),
                                              ProviderServiceState::STOPPED);
            return;
        }
        if (wallet.IsLocked()) {
            manager->SetProviderServiceStatus(
                wallet.GetName(), ProviderServiceState::WAITING_FOR_UNLOCK,
                "PAYMASTER_WALLET_LOCKED");
            return;
        }

        JSONRPCRequest start_request;
        start_request.strMethod = std::string{INTERNAL_PAYMASTER_AUTOSTART_METHOD};
        start_request.params = UniValue{UniValue::VARR};
        start_request.URI = WalletEndpointURI(wallet.GetName());
        start_request.context = &context;
        try {
            const UniValue result = startpaymaster().HandleRequest(start_request);
            running = result.find_value("running").isTrue();
            if (!running) {
                std::string error{"PAYMASTER_PROVIDER_NOT_READY"};
                const UniValue& readiness_errors =
                    result.find_value("readiness_errors");
                if (readiness_errors.isArray() &&
                    !readiness_errors.getValues().empty() &&
                    readiness_errors[0].isStr()) {
                    error = StableProviderServiceError(
                        readiness_errors[0].get_str());
                }
                manager->SetProviderServiceStatus(
                    wallet.GetName(),
                    wallet.IsLocked()
                        ? ProviderServiceState::WAITING_FOR_UNLOCK
                        : ProviderServiceState::WAITING_FOR_READINESS,
                    std::move(error));
                return;
            }
        } catch (const UniValue& rpc_error) {
            const UniValue& message = rpc_error.find_value("message");
            const std::string error = StableProviderServiceError(
                message.isStr() ? message.get_str()
                                : "PAYMASTER_AUTOSTART_FAILED");
            manager->SetProviderServiceStatus(
                wallet.GetName(),
                error == "PAYMASTER_WALLET_LOCKED"
                    ? ProviderServiceState::WAITING_FOR_UNLOCK
                    : ProviderServiceState::WAITING_FOR_READINESS,
                error);
            return;
        } catch (const std::exception&) {
            manager->SetProviderServiceStatus(
                wallet.GetName(), ProviderServiceState::FAULT,
                "PAYMASTER_AUTOSTART_FAILED");
            return;
        }
    }

    if (settings.operation_mode == ProviderOperationMode::MANUAL) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::MANUAL);
        return;
    }
    if (wallet.IsLocked()) {
        // Keep the explicitly started runtime registered, but never consume or
        // sign queued work until the operator unlocks the wallet again.
        manager->SetProviderServiceStatus(
            wallet.GetName(), ProviderServiceState::WAITING_FOR_UNLOCK,
            "PAYMASTER_WALLET_LOCKED");
        return;
    }

    const ProviderServiceStatus previous =
        manager->GetProviderServiceStatus(wallet.GetName());
    ProviderServiceState state =
        previous.state == ProviderServiceState::DRAIN_ONLY
            ? ProviderServiceState::DRAIN_ONLY
            : ProviderServiceState::ACTIVE;
    std::string first_error;
    bool accept_new_requests{state != ProviderServiceState::DRAIN_ONLY};

    if (accept_new_requests) {
        ProviderWorkGuard maintenance_guard{
            *manager, wallet.GetName(), identity.provider_id};
        if (!maintenance_guard.Acquired()) {
            // A manual expert action or another bounded provider step owns
            // the wallet. Leave its service state untouched and retry next
            // scheduler tick.
            return;
        }
        ProviderReadiness liquidity = GetProviderReadiness(
            wallet, context, /*wait_for_sync=*/false);
        const ProviderLiquidityPolicy liquidity_policy =
            liquidity.have_liquidity_policy
                ? liquidity.liquidity_policy
                : SuggestedLiquidityPolicy(liquidity, GetTime());
        const bool targets_satisfy_provider_policy =
            !liquidity.have_policy ||
            ProviderLiquidityTargetsSatisfyPolicy(
                liquidity_policy, liquidity.policy);
        const auto admission_dgb = CountLiquiditySlots(
            liquidity.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB,
            liquidity_policy.target_admission_dgb);
        const int64_t operational_dgb_minimum = liquidity.have_policy
            ? liquidity.policy.maximum_network_fee.value
            : std::numeric_limits<int64_t>::max();
        const auto operational_dgb = CountLiquiditySlots(
            liquidity.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
            liquidity_policy.target_operational_dgb,
            operational_dgb_minimum);
        const auto admission_carriers = CountLiquiditySlots(
            liquidity.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER,
            liquidity_policy.target_admission_carriers);
        const auto operational_carriers = CountLiquiditySlots(
            liquidity.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER,
            liquidity_policy.target_operational_carriers);
        const size_t missing_dgb = admission_dgb.missing + operational_dgb.missing;
        const size_t missing_carriers = admission_carriers.missing +
                                        operational_carriers.missing;
        const bool targets_confirmed =
            admission_dgb.ready >= liquidity_policy.target_admission_dgb &&
            operational_dgb.ready >= liquidity_policy.target_operational_dgb &&
            admission_carriers.ready >= liquidity_policy.target_admission_carriers &&
            operational_carriers.ready >= liquidity_policy.target_operational_carriers;
        if (!targets_satisfy_provider_policy) {
            // A zero operational carrier target can be a deliberate result of
            // release_slot. It is valid to persist, but automatic maintenance
            // must not claim success or silently recreate capital the operator
            // explicitly released. Require an explicit target change first.
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_READINESS;
            first_error = "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE";
        } else if (missing_dgb + missing_carriers > 0) {
            accept_new_requests = false;
            if (!liquidity.have_liquidity_policy ||
                !liquidity_policy.paid_maintenance_approved) {
                state = ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL;
                first_error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
            } else if (!liquidity_policy.automatic_replenishment) {
                state = ProviderServiceState::WAITING_FOR_READINESS;
                first_error = "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED";
            } else {
                std::string maintenance_error;
                const bool replenished = missing_carriers > 0
                    ? RunAutomaticCarrierReplenishment(
                          wallet, liquidity_policy,
                          admission_carriers.missing,
                          operational_carriers.missing,
                          maintenance_error)
                    : RunAutomaticDGBReplenishment(
                          wallet, liquidity_policy,
                          admission_dgb.missing,
                          operational_dgb.missing,
                          maintenance_error);
                if (!replenished) {
                    first_error = StableProviderServiceError(
                        maintenance_error.empty()
                            ? "PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"
                            : maintenance_error);
                    state = first_error == "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED" ||
                                    first_error == "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"
                        ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                        : ProviderServiceState::REPLENISHING_LIQUIDITY;
                } else {
                    state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
                    first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
                }
            }
        } else if (!targets_confirmed) {
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
            first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
        }
    }

    const bool announcement_refresh_required =
        previous.state == ProviderServiceState::WAITING_FOR_UNLOCK ||
        previous.state == ProviderServiceState::WAITING_FOR_READINESS ||
        previous.state == ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL ||
        previous.state == ProviderServiceState::REPLENISHING_LIQUIDITY ||
        previous.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION ||
        previous.state == ProviderServiceState::FAULT;
    if (accept_new_requests && announcement_refresh_required) {
        ProviderReadiness resumed = GetProviderReadiness(
            wallet, context, /*wait_for_sync=*/false);
        std::string announcement_error;
        if (!PublishCurrentProviderAnnouncement(
                wallet, context, resumed, GetTime(), nullptr,
                announcement_error)) {
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_READINESS;
            first_error = StableProviderServiceError(
                announcement_error.empty()
                    ? "PAYMASTER_ANNOUNCEMENT_REJECTED"
                    : announcement_error);
        }
    }

    JSONRPCRequest service_request;
    service_request.strMethod = std::string{INTERNAL_PAYMASTER_SERVICE_METHOD};
    service_request.params = UniValue{UniValue::VARR};
    service_request.URI = WalletEndpointURI(wallet.GetName());
    service_request.context = &context;

    const auto run_step = [&](const RPCHelpMan& handler) {
        try {
            handler.HandleRequest(service_request);
        } catch (const UniValue& rpc_error) {
            const UniValue& message = rpc_error.find_value("message");
            const std::string error = StableProviderServiceError(
                message.isStr() ? message.get_str()
                                : "PAYMASTER_AUTOMATIC_SERVICE_ERROR");
            if (error == "PAYMASTER_PROVIDER_SERVICE_BUSY") return;
            if (first_error.empty()) first_error = error;
            if (error == "PAYMASTER_WALLET_LOCKED") {
                state = ProviderServiceState::WAITING_FOR_UNLOCK;
            } else if (error == "PAYMASTER_PROVIDER_SYNCING" ||
                       error == "PAYMASTER_REQUIRES_READY_TXINDEX" ||
                       error == "PAYMASTER_TXINDEX_NOT_READY" ||
                       error == "PAYMASTER_NODE_NOT_READY") {
                state = ProviderServiceState::WAITING_FOR_READINESS;
            } else if (error == "PAYMASTER_PROVIDER_DRAIN_ONLY" ||
                       error == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED") {
                state = ProviderServiceState::DRAIN_ONLY;
            } else if (error == "PAYMASTER_PROVIDER_NOT_RUNNING") {
                state = ProviderServiceState::STOPPED;
            } else {
                state = ProviderServiceState::FAULT;
            }
        } catch (const std::exception&) {
            if (first_error.empty()) {
                first_error = "PAYMASTER_AUTOMATIC_SERVICE_ERROR";
            }
            state = ProviderServiceState::FAULT;
        }
    };

    // A liquidity pause blocks new capacity proofs but must not strand the
    // quote/recovery continuation that caused the reservation. The request
    // handler itself skips fresh capacity messages unless readiness is fully
    // restored. Drain-only providers still finish only already-authorized
    // submissions and never create a new quote.
    const bool process_request_continuations =
        state == ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL ||
        state == ProviderServiceState::REPLENISHING_LIQUIDITY ||
        state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
    if (accept_new_requests || process_request_continuations) {
        run_step(processpaymasterrequests());
    }
    if (state != ProviderServiceState::WAITING_FOR_UNLOCK &&
        state != ProviderServiceState::WAITING_FOR_READINESS &&
        state != ProviderServiceState::FAULT &&
        state != ProviderServiceState::STOPPED) {
        run_step(processpaymastersubmits());
    }
    if (!first_error.empty() &&
        (previous.state != state || previous.last_error != first_error)) {
        wallet.WalletLogPrintf(
            "Paymaster automatic provider service paused: %s\n",
            first_error);
    }
    manager->SetProviderServiceStatus(wallet.GetName(), state,
                                      std::move(first_error));
}

} // namespace wallet
