// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/paymasterfinance.h>
#include <wallet/paymasterfinancecache.h>

#include <chainparams.h>
#include <hash.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <logging/timer.h>
#include <paymaster/finance.h>
#include <util/time.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <boost/signals2/connection.hpp>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>

namespace wallet {
using paymaster_store::internal::PersistedReadError;
using paymaster_store::internal::ProviderPoolReadError;
using namespace DigiDollar::Paymaster;

namespace {
enum class FinanceWorkKind { TRANSACTION, EVENT };
using FinanceWorkKey = std::pair<FinanceWorkKind, uint256>;
struct FinanceWork {
    uint64_t sequence{0};
    uint256 after;
    bool required{false};
    bool source_observed{false};
};
struct FinanceState {
    std::mutex mutex;
    boost::signals2::scoped_connection transactions;
    bool initialized{false};
    PaymasterId provider;
    uint64_t sequence{0};
    std::map<FinanceWorkKey, FinanceWork> pending;
    std::map<uint256, ProviderMaintenanceRecord> maintenance;
    std::set<uint256> setup_transactions;
    std::map<uint256, uint256> attempts_by_commit;
    std::map<uint256, uint256> commit_transactions;

    void Mark(FinanceWorkKind kind, const uint256& id, bool required = false)
    {
        if (id.IsNull()) return;
        auto& work = pending[{kind, id}];
        work.sequence = ++sequence;
        work.after.SetNull();
        work.source_observed = false;
        work.required |= required;
    }
};
struct PendingFinanceChanges {
    WalletDatabase* database{nullptr};
    std::optional<ProviderMaintenanceLedger> maintenance;
    std::optional<std::vector<ProviderPoolEntry>> pool;
    std::map<uint256, std::pair<PaymasterId, uint256>> attempts;
    std::map<uint256, std::pair<PaymasterId, uint256>> commits;
    std::set<uint256> events;
    bool rebuild{false};
};
// No registry or state lock spans wallet/database I/O or signal disconnection.
// Wallet operations hold cs_wallet before obtaining the short-lived state lock.
std::mutex finance_registry_mutex;
std::map<WalletDatabase*, std::shared_ptr<FinanceState>> finance_states;
std::map<const WalletBatch*, PendingFinanceChanges> pending_finance_changes;

std::shared_ptr<FinanceState> FindFinanceState(WalletDatabase& database)
{
    std::lock_guard<std::mutex> lock{finance_registry_mutex};
    const auto it = finance_states.find(&database);
    return it == finance_states.end() ? nullptr : it->second;
}

std::shared_ptr<FinanceState> GetFinanceState(CWallet& wallet)
{
    if (auto state = FindFinanceState(wallet.GetDatabase())) return state;
    auto state = std::make_shared<FinanceState>();
    const std::weak_ptr<FinanceState> weak = state;
    state->transactions = wallet.NotifyTransactionChanged.connect([weak](const uint256& txid, ChangeType) {
        if (auto observed = weak.lock()) {
            std::lock_guard<std::mutex> lock{observed->mutex};
            observed->Mark(FinanceWorkKind::TRANSACTION, txid);
        }
    });
    std::lock_guard<std::mutex> lock{finance_registry_mutex};
    finance_states.emplace(&wallet.GetDatabase(), state);
    return state;
}

template <typename Update>
void QueueFinanceChange(const WalletBatch* owner, WalletDatabase& database, Update update)
{
    std::lock_guard<std::mutex> lock{finance_registry_mutex};
    // Before initialization the full reconstruction reads the durable records.
    if (finance_states.count(&database) == 0) return;
    auto& changes = pending_finance_changes[owner];
    changes.database = &database;
    update(changes);
}

auto MaintenanceContribution(const ProviderMaintenanceRecord& record)
{
    return std::tie(record.kind, record.state, record.transaction_id,
                    record.actual_fee.value, record.created_at);
}

void ObserveTransaction(CWallet& wallet, const uint256& txid, int64_t created_at,
                        ProviderFinanceEventState& state, int64_t& confirmed_at,
                        std::map<uint256, int64_t>& confirmation_times)
{
    state = ProviderFinanceEventState::PENDING;
    confirmed_at = 0;
    const auto transaction = wallet.mapWallet.find(txid);
    if (transaction == wallet.mapWallet.end()) return;
    if (transaction->second.isAbandoned() || transaction->second.isConflicted()) {
        state = ProviderFinanceEventState::INVALIDATED;
        return;
    }
    if (wallet.GetTxDepthInMainChain(transaction->second) <= 0) return;
    state = ProviderFinanceEventState::CONFIRMED;
    int64_t block_time{0};
    if (const auto* confirmed = transaction->second.state<TxStateConfirmed>()) {
        const auto [entry, inserted] = confirmation_times.emplace(confirmed->confirmed_block_hash, 0);
        if (inserted) wallet.chain().findBlock(confirmed->confirmed_block_hash, interfaces::FoundBlock().time(entry->second));
        block_time = entry->second;
    }
    confirmed_at = std::max(created_at, block_time > 0 ? block_time : transaction->second.GetTxTime());
}

bool PrepareObservation(ProviderFinanceEvent& event, const ProviderFinanceEvent* previous,
                        int64_t now, bool& changed, std::string& error)
{
    if (previous && !CheckProviderFinanceEventBinding(*previous, event, error)) return false;
    changed = !previous || previous->state != event.state || previous->confirmed_at != event.confirmed_at;
    if (!changed) event.updated_at = previous->updated_at;
    else {
        if (previous && previous->updated_at == std::numeric_limits<int64_t>::max()) {
            error = "PAYMASTER_FINANCE_REVISION_EXHAUSTED";
            return false;
        }
        event.updated_at = std::max({event.created_at, event.confirmed_at, now,
                                     previous ? previous->updated_at + 1 : int64_t{0}});
    }
    return ValidateProviderFinanceEvent(event, error);
}

ProviderFinanceEvent MaintenanceEvent(CWallet& wallet, const ProviderIdentityRecord& identity,
                                       const ProviderMaintenanceRecord& record)
{
    ProviderFinanceEvent event;
    event.event_id = record.operation_id;
    event.genesis_hash = Params().GenesisBlock().GetHash();
    event.provider_id = identity.provider_id;
    event.kind = record.kind == ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS
        ? ProviderFinanceEventKind::CARRIER_WITHDRAWAL
        : record.IsPreparation() ? ProviderFinanceEventKind::POOL_SETUP : ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT;
    event.transaction_id = record.transaction_id;
    event.dgb_cost = record.actual_fee;
    event.created_at = record.created_at;
    if (record.IsPreparation()) {
        const auto transaction = wallet.mapWallet.find(record.transaction_id);
        if (transaction != wallet.mapWallet.end()) event.created_at = std::max(identity.created_at, transaction->second.GetTxTime());
    }
    return event;
}

ProviderFinanceEvent TransferEvent(const ProviderIdentityRecord& identity,
                                    const ProviderAttempt& attempt, const ProviderCommitRecord& commit)
{
    ProviderFinanceEvent event;
    event.event_id = commit.commit_key;
    event.genesis_hash = Params().GenesisBlock().GetHash();
    event.provider_id = identity.provider_id;
    event.kind = ProviderFinanceEventKind::TRANSFER;
    event.funding_model = attempt.provider_manifest.funding_model;
    event.sponsorship_scope = attempt.provider_manifest.sponsorship_scope;
    event.transaction_id = commit.final_txid;
    event.dd_income = attempt.provider_manifest.service_fee;
    event.dgb_cost = attempt.provider_manifest.network_fee;
    event.created_at = commit.committed_at;
    event.updated_at = commit.committed_at;
    return event;
}

uint256 NativeFinanceEventId(const PaymasterId& provider, ProviderFinanceEventKind kind, const uint256& txid)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Finance Event v1");
    hasher << provider << static_cast<uint8_t>(kind) << txid;
    return hasher.GetSHA256();
}
} // namespace

void ForgetPaymasterFinanceCache(WalletDatabase& database)
{
    std::shared_ptr<FinanceState> retired;
    {
        std::lock_guard<std::mutex> lock{finance_registry_mutex};
        const auto it = finance_states.find(&database);
        if (it == finance_states.end()) return;
        retired = std::move(it->second);
        finance_states.erase(it);
    }
}

void QueuePaymasterFinanceRecord(const WalletBatch* owner, WalletDatabase& database, const ProviderAttempt& attempt)
{
    if (attempt.commit_key.IsNull()) return;
    QueueFinanceChange(owner, database, [&](auto& changes) {
        changes.attempts[attempt.commit_key] = {attempt.provider_id, attempt.attempt_id};
    });
}

void QueuePaymasterFinanceRecord(const WalletBatch* owner, WalletDatabase& database, const ProviderCommitRecord& commit)
{
    QueueFinanceChange(owner, database, [&](auto& changes) {
        changes.commits[commit.commit_key] = {commit.provider_id, commit.final_txid};
    });
}

void QueuePaymasterFinanceRecord(const WalletBatch* owner, WalletDatabase& database, const ProviderMaintenanceLedger& ledger)
{
    QueueFinanceChange(owner, database, [&](auto& changes) { changes.maintenance = ledger; });
}

void QueuePaymasterFinanceRecord(const WalletBatch* owner, WalletDatabase& database, const std::vector<ProviderPoolEntry>& pool)
{
    QueueFinanceChange(owner, database, [&](auto& changes) { changes.pool = pool; });
}

void QueuePaymasterFinanceEvent(const WalletBatch* owner, WalletDatabase& database, const uint256& event_id)
{
    QueueFinanceChange(owner, database, [&](auto& changes) { changes.events.insert(event_id); });
}

void QueuePaymasterFinanceRebuild(const WalletBatch* owner, WalletDatabase& database)
{
    QueueFinanceChange(owner, database, [](auto& changes) { changes.rebuild = true; });
}

void PaymasterFinanceTransactionEnd(const WalletBatch* owner, bool committed)
{
    PendingFinanceChanges changes;
    std::shared_ptr<FinanceState> state;
    {
        std::lock_guard<std::mutex> lock{finance_registry_mutex};
        const auto pending = pending_finance_changes.find(owner);
        if (pending == pending_finance_changes.end()) return;
        changes = std::move(pending->second);
        pending_finance_changes.erase(pending);
        if (!committed) return;
        const auto found = finance_states.find(changes.database);
        if (found == finance_states.end()) return;
        state = found->second;
    }
    std::lock_guard<std::mutex> lock{state->mutex};
    if (changes.rebuild) state->initialized = false;
    if (changes.maintenance) {
        std::map<uint256, ProviderMaintenanceRecord> next;
        for (const auto& record : changes.maintenance->records) {
            if (record.transaction_id.IsNull()) continue;
            const auto old = state->maintenance.find(record.operation_id);
            if (old == state->maintenance.end() || MaintenanceContribution(old->second) != MaintenanceContribution(record)) {
                state->Mark(FinanceWorkKind::EVENT, record.operation_id);
            }
            next.emplace(record.operation_id, record);
        }
        for (const auto& [id, record] : state->maintenance) {
            if (next.count(id) == 0) state->Mark(FinanceWorkKind::EVENT, id);
        }
        state->maintenance = std::move(next);
    }
    if (changes.pool) {
        std::set<uint256> next;
        for (const auto& entry : *changes.pool) {
            if (entry.origin_commit_key.IsNull() && !entry.outpoint.hash.IsNull()) next.insert(entry.outpoint.hash);
        }
        for (const auto& txid : next) {
            if (state->setup_transactions.count(txid) == 0) state->Mark(FinanceWorkKind::TRANSACTION, txid);
        }
        state->setup_transactions = std::move(next);
    }
    for (const auto& [commit, attempt] : changes.attempts) {
        if (attempt.first != state->provider) continue;
        state->attempts_by_commit[commit] = attempt.second;
        if (state->commit_transactions.count(commit)) state->Mark(FinanceWorkKind::EVENT, commit);
    }
    for (const auto& [commit, transaction] : changes.commits) {
        if (transaction.first != state->provider) continue;
        state->commit_transactions[commit] = transaction.second;
        state->Mark(FinanceWorkKind::EVENT, commit);
    }
    for (const auto& id : changes.events) state->Mark(FinanceWorkKind::EVENT, id, true);
}

bool RebuildProviderFinances(CWallet& wallet,
                               size_t& changed_events,
                               std::string& error)
{
    LOG_TIME_MILLIS_WITH_CATEGORY("Paymaster reconciliation", BCLog::BENCH);
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

    auto observed = GetFinanceState(wallet);
    {
        std::lock_guard<std::mutex> lock{observed->mutex};
        observed->initialized = false;
        observed->provider = identity.provider_id;
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

    std::map<uint256, ProviderFinanceEvent> updates;
    std::map<uint256, size_t> event_positions;
    for (size_t i = 0; i < finance.events.size(); ++i) {
        event_positions.emplace(finance.events[i].event_id, i);
    }
    std::map<uint256, int64_t> confirmation_times;
    const int64_t now = GetTime();
    const auto transaction_state = [&](const uint256& txid, int64_t created_at,
                                       ProviderFinanceEventState& state, int64_t& confirmed_at) {
        ObserveTransaction(wallet, txid, created_at, state, confirmed_at, confirmation_times);
    };
    const auto apply_event = [&](ProviderFinanceEvent event) {
        const auto position = event_positions.find(event.event_id);
        auto existing = position == event_positions.end()
            ? finance.events.end() : finance.events.begin() + position->second;
        bool changed{false};
        if (!PrepareObservation(event, existing == finance.events.end() ? nullptr : &*existing,
                                now, changed, error)) return false;
        if (event.genesis_hash != finance.genesis_hash || event.provider_id != finance.provider_id) {
            error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
            return false;
        }
        if (changed) {
            updates[event.event_id] = event;
            ledger_changed = true;
            ++changed_events;
        }
        if (existing == finance.events.end()) {
            // Validate above, then rebuild all daily totals once after the
            // batch, including newly reconstructed historical events.
            if (finance.events.size() >= MAX_PROVIDER_FINANCE_EVENTS) {
                error = "PAYMASTER_FINANCE_LEDGER_FULL";
                return false;
            }
            finance.events.push_back(std::move(event));
            event_positions.emplace(finance.events.back().event_id, finance.events.size() - 1);
            return true;
        }
        *existing = std::move(event);
        return true;
    };

    // Events created directly by pool-management RPCs are not represented by
    // a provider commit or maintenance reservation. Reconcile their chain
    // state here before adding any newly discoverable records below.
    for (const ProviderFinanceEvent& saved : finance.events) {
        auto event = saved;
        transaction_state(event.transaction_id, event.created_at, event.state, event.confirmed_at);
        if (!apply_event(std::move(event))) return false;
    }

    // Wallet transaction metadata is persisted before broadcast, so finance
    // failure or a crash after either retirement step cannot lose its fee.
    const auto recover_cost = [&](ProviderFinanceEventKind kind, const uint256& txid) {
        const uint256 event_id = NativeFinanceEventId(identity.provider_id, kind, txid);
        if (event_positions.count(event_id) != 0) {
            return true;
        }
        const auto transaction = wallet.mapWallet.find(txid);
        if (transaction == wallet.mapWallet.end() ||
            !transaction->second.tx) {
            if (!finance.earlier_history_partial) {
                finance.earlier_history_partial = true;
                ledger_changed = true;
            }
            return true;
        }
        const auto cost = GetProviderFinanceTransactionFee(wallet, transaction->second.tx);
        if (!cost) {
            if (!finance.earlier_history_partial) {
                finance.earlier_history_partial = true;
                ledger_changed = true;
            }
            return true;
        }
        ProviderFinanceEvent event;
        event.event_id = event_id;
        event.genesis_hash = genesis_hash;
        event.provider_id = identity.provider_id;
        event.kind = kind;
        event.transaction_id = txid;
        event.dgb_cost = *cost;
        event.created_at = std::max(
            identity.created_at, transaction->second.GetTxTime());
        transaction_state(event.transaction_id, event.created_at,
                          event.state, event.confirmed_at);
        return apply_event(std::move(event));
    };
    for (const auto& [txid, transaction] : wallet.mapWallet) {
        const auto marker = transaction.mapValue.find(PAYMASTER_RETIREMENT_PROVIDER_KEY);
        if (marker == transaction.mapValue.end()) continue;
        if (marker->second != identity.provider_id.GetHex()) {
            error = "PAYMASTER_RETIREMENT_IDENTITY_MISMATCH";
            return false;
        }
        if (!recover_cost(ProviderFinanceEventKind::POOL_RETIREMENT, txid)) return false;
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
            if (!recover_cost(ProviderFinanceEventKind::POOL_SETUP, txid)) return false;
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
        auto event = TransferEvent(identity, attempt->second, commit);
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
            auto event = MaintenanceEvent(wallet, identity, record);
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

    finance.updated_at = std::max(finance.updated_at, now);
    for (const ProviderFinanceEvent& event : finance.events) {
        finance.updated_at = std::max(finance.updated_at, event.updated_at);
    }
    ProviderFinanceMetadata metadata;
    metadata.genesis_hash = finance.genesis_hash;
    metadata.provider_id = finance.provider_id;
    metadata.history_complete_from = finance.history_complete_from;
    metadata.earlier_history_partial = finance.earlier_history_partial;
    metadata.updated_at = finance.updated_at;
    std::vector<ProviderFinanceEvent> changed;
    changed.reserve(updates.size());
    for (const auto& [id, event] : updates) changed.push_back(event);
    if (ledger_changed && !batch.WritePaymasterFinanceEvents(metadata, changed, error, false)) {
        if (error.empty()) error = "PAYMASTER_FINANCE_DATABASE_WRITE";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock{observed->mutex};
        observed->maintenance.clear();
        for (const auto& record : maintenance.records) {
            if (!record.transaction_id.IsNull()) observed->maintenance.emplace(record.operation_id, record);
        }
        observed->setup_transactions.clear();
        for (const auto& entry : provider_pool) {
            if (entry.origin_commit_key.IsNull() && !entry.outpoint.hash.IsNull()) observed->setup_transactions.insert(entry.outpoint.hash);
        }
        observed->attempts_by_commit.clear();
        for (const auto& [commit, attempt] : attempts_by_commit) observed->attempts_by_commit.emplace(commit, attempt.attempt_id);
        observed->commit_transactions.clear();
        for (const auto& commit : commits) {
            if (commit.provider_id == identity.provider_id) observed->commit_transactions.emplace(commit.commit_key, commit.final_txid);
        }
        observed->pending.clear();
        observed->initialized = true;
    }
    return true;
}

bool ReconcileProviderFinances(CWallet& wallet, size_t& changed_events, std::string& error)
{
    LOG_TIME_MILLIS_WITH_CATEGORY("Paymaster incremental finance reconciliation", BCLog::BENCH);
    LOCK(wallet.cs_wallet);
    changed_events = 0;
    error.clear();
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    const auto identity_status = batch.ReadPaymasterIdentityWithStatus(identity);
    if (identity_status == DatabaseReadStatus::NOT_FOUND) {
        ForgetPaymasterFinanceCache(wallet.GetDatabase());
        return true;
    }
    if (identity_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(identity_status, "ProviderIdentityRecord", identity,
                                  "PAYMASTER_IDENTITY_NOT_FOUND", "PAYMASTER_INVALID_IDENTITY");
        return false;
    }
    auto state = GetFinanceState(wallet);
    bool rebuild;
    std::vector<std::pair<FinanceWorkKey, FinanceWork>> selected;
    {
        std::lock_guard<std::mutex> lock{state->mutex};
        rebuild = !state->initialized || state->provider != identity.provider_id;
        if (!rebuild) {
            for (const auto& work : state->pending) {
                if (selected.size() == MAX_FINANCE_RECONCILIATION_WORK) break;
                selected.push_back(work);
            }
        }
    }
    if (rebuild) return RebuildProviderFinances(wallet, changed_events, error);

    ProviderFinanceMetadata metadata;
    const auto read = batch.ReadPaymasterFinanceMetadata(metadata);
    if (read != DatabaseReadStatus::FOUND) {
        error = read == DatabaseReadStatus::UNSUPPORTED_VERSION
            ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (metadata.provider_id != identity.provider_id || metadata.genesis_hash != Params().GenesisBlock().GetHash()) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    if (selected.empty()) return true;

    const int64_t now = GetTime();
    const bool was_partial = metadata.earlier_history_partial;
    std::map<uint256, ProviderFinanceEvent> updates;
    std::map<uint256, int64_t> confirmation_times;
    struct CompletedWork {
        FinanceWorkKey key;
        uint64_t sequence;
        std::optional<FinanceWork> continuation;
    };
    std::vector<CompletedWork> completed;
    size_t processed{0};
    const auto read_event = [&](const uint256& id, ProviderFinanceEvent& event) {
        const auto pending = updates.find(id);
        if (pending == updates.end()) return batch.ReadPaymasterFinanceEvent(id, event);
        event = pending->second;
        return DatabaseReadStatus::FOUND;
    };
    const auto apply = [&](ProviderFinanceEvent event) {
        ProviderFinanceEvent previous;
        const auto status = read_event(event.event_id, previous);
        if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
            error = "PAYMASTER_INVALID_FINANCE_EVENT";
            return false;
        }
        ObserveTransaction(wallet, event.transaction_id, event.created_at, event.state, event.confirmed_at, confirmation_times);
        {
            std::lock_guard<std::mutex> lock{state->mutex};
            const auto maintenance = state->maintenance.find(event.event_id);
            if (maintenance != state->maintenance.end() &&
                (maintenance->second.state == ProviderMaintenanceState::FAILED ||
                 maintenance->second.state == ProviderMaintenanceState::RELEASED)) {
                event.state = ProviderFinanceEventState::INVALIDATED;
                event.confirmed_at = 0;
            }
        }
        bool changed{false};
        if (!PrepareObservation(event, status == DatabaseReadStatus::FOUND ? &previous : nullptr, now, changed, error)) return false;
        if (event.genesis_hash != metadata.genesis_hash || event.provider_id != metadata.provider_id) {
            error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
            return false;
        }
        if (changed) updates[event.event_id] = std::move(event);
        return true;
    };
    const auto recover_native = [&](ProviderFinanceEventKind kind, const uint256& txid) {
        const auto event_id = NativeFinanceEventId(identity.provider_id, kind, txid);
        ProviderFinanceEvent existing;
        const auto status = read_event(event_id, existing);
        if (status == DatabaseReadStatus::FOUND) return true; // Observed through the transaction index below.
        if (status != DatabaseReadStatus::NOT_FOUND) { error = "PAYMASTER_INVALID_FINANCE_EVENT"; return false; }
        const auto transaction = wallet.mapWallet.find(txid);
        const auto cost = transaction == wallet.mapWallet.end()
            ? std::nullopt : GetProviderFinanceTransactionFee(wallet, transaction->second.tx);
        if (!cost) { metadata.earlier_history_partial = true; return true; }
        if (processed == MAX_FINANCE_RECONCILIATION_WORK) return true;
        ProviderFinanceEvent event;
        event.event_id = event_id;
        event.genesis_hash = metadata.genesis_hash;
        event.provider_id = identity.provider_id;
        event.kind = kind;
        event.transaction_id = txid;
        event.dgb_cost = *cost;
        event.created_at = std::max(identity.created_at, transaction->second.GetTxTime());
        ++processed;
        return apply(std::move(event));
    };

    for (const auto& [key, work] : selected) {
        if (processed == MAX_FINANCE_RECONCILIATION_WORK) break;
        if (key.first == FinanceWorkKind::EVENT) {
            ++processed;
            ProviderFinanceEvent event;
            const auto status = read_event(key.second, event);
            if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
                error = "PAYMASTER_INVALID_FINANCE_EVENT";
                return false;
            }
            std::optional<ProviderMaintenanceRecord> maintenance;
            bool have_commit{false};
            uint256 attempt_id;
            {
                std::lock_guard<std::mutex> lock{state->mutex};
                const auto found = state->maintenance.find(key.second);
                if (found != state->maintenance.end()) maintenance = found->second;
                have_commit = state->commit_transactions.count(key.second) != 0;
                const auto attempt = state->attempts_by_commit.find(key.second);
                if (attempt != state->attempts_by_commit.end()) attempt_id = attempt->second;
            }
            bool have_event = status == DatabaseReadStatus::FOUND;
            if (maintenance) {
                event = MaintenanceEvent(wallet, identity, *maintenance);
                have_event = true;
            } else if (have_commit) {
                ProviderCommitRecord commit;
                const auto commit_status = batch.ReadPaymasterProviderCommitWithStatus(key.second, commit);
                if (commit_status != DatabaseReadStatus::FOUND) {
                    error = "PAYMASTER_FINANCE_COMMIT_READ_FAILED";
                    return false;
                }
                ProviderAttempt attempt;
                const auto attempt_status = attempt_id.IsNull() ? DatabaseReadStatus::NOT_FOUND
                    : batch.ReadPaymasterAttemptWithStatus(attempt_id, attempt);
                if (attempt_status != DatabaseReadStatus::FOUND && attempt_status != DatabaseReadStatus::NOT_FOUND) {
                    error = "PAYMASTER_FINANCE_ATTEMPT_READ_FAILED";
                    return false;
                }
                if (attempt_status == DatabaseReadStatus::FOUND && !attempt.provider_manifest.manifest_id.IsNull()) {
                    event = TransferEvent(identity, attempt, commit);
                    have_event = true;
                } else metadata.earlier_history_partial = true;
            }
            if (have_event) {
                if (!apply(std::move(event))) return false;
            } else if (work.required) {
                error = "PAYMASTER_INVALID_FINANCE_EVENT";
                return false;
            }
            completed.push_back({key, work.sequence, std::nullopt});
            continue;
        }

        auto continuation = work;
        if (!work.source_observed) {
            bool setup;
            {
                std::lock_guard<std::mutex> lock{state->mutex};
                setup = state->setup_transactions.count(key.second) != 0;
            }
            if (setup && !recover_native(ProviderFinanceEventKind::POOL_SETUP, key.second)) return false;
            if (processed == MAX_FINANCE_RECONCILIATION_WORK) {
                // Repeat source inspection: a transaction can carry both markers.
                completed.push_back({key, work.sequence, continuation});
                break;
            }
            const auto transaction = wallet.mapWallet.find(key.second);
            if (transaction != wallet.mapWallet.end()) {
                const auto marker = transaction->second.mapValue.find(PAYMASTER_RETIREMENT_PROVIDER_KEY);
                if (marker != transaction->second.mapValue.end()) {
                    if (marker->second != identity.provider_id.GetHex()) {
                        error = "PAYMASTER_RETIREMENT_IDENTITY_MISMATCH";
                        return false;
                    }
                    if (!recover_native(ProviderFinanceEventKind::POOL_RETIREMENT, key.second)) return false;
                }
            }
            continuation.source_observed = true;
        }
        if (processed == MAX_FINANCE_RECONCILIATION_WORK) {
            completed.push_back({key, work.sequence, continuation});
            break;
        }
        std::vector<ProviderFinanceEvent> events;
        bool more{false};
        if (!batch.ReadPaymasterFinanceTransactionEvents(key.second, events,
                MAX_FINANCE_RECONCILIATION_WORK - processed, work.after, &more)) {
            error = "PAYMASTER_FINANCE_TRANSACTION_INDEX_READ_FAILED";
            return false;
        }
        processed += std::max(size_t{1}, events.size());
        const auto last_id = events.empty() ? uint256{} : events.back().event_id;
        for (auto& event : events) {
            if (!apply(std::move(event))) return false;
        }
        if (more) {
            continuation.after = last_id;
            completed.push_back({key, work.sequence, continuation});
        } else completed.push_back({key, work.sequence, std::nullopt});
    }

    std::vector<ProviderFinanceEvent> changed;
    for (const auto& [id, event] : updates) changed.push_back(event);
    if (!changed.empty() || was_partial != metadata.earlier_history_partial) {
        metadata.updated_at = std::max(metadata.updated_at, now);
        // These records already reflect the wallet observation just performed.
        // Do not turn an observation update into another reconciliation request.
        if (!batch.WritePaymasterFinanceEvents(metadata, changed, error, false)) return false;
    }
    {
        std::lock_guard<std::mutex> lock{state->mutex};
        for (const auto& done : completed) {
            const auto current = state->pending.find(done.key);
            // A notification during commit supersedes the processed snapshot.
            if (current == state->pending.end() || current->second.sequence != done.sequence) continue;
            if (done.continuation) current->second = *done.continuation;
            else state->pending.erase(current);
        }
    }
    changed_events = changed.size();
    return true;
}

bool ReconcileProviderFinanceReport(CWallet& wallet, bool /*continuation*/, std::string& error)
{
    LOCK(wallet.cs_wallet);
    size_t changed{0};
    if (!ReconcileProviderFinances(wallet, changed, error)) return false;
    if (const auto state = FindFinanceState(wallet.GetDatabase())) {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (!state->initialized || !state->pending.empty()) {
            error = "PAYMASTER_FINANCE_RECONCILING";
            return false;
        }
    }
    return true;
}

bool AreProviderFinancesCurrent(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    const auto state = FindFinanceState(wallet.GetDatabase());
    if (!state) return false;
    std::lock_guard<std::mutex> lock{state->mutex};
    return state->initialized && state->pending.empty();
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
    if (!batch.MigratePaymasterFinanceLedger(error)) return false;
    ProviderFinanceMetadata ledger;
    if (batch.ReadPaymasterFinanceMetadata(ledger) != DatabaseReadStatus::FOUND) {
        if (batch.HasPaymasterFinanceLedger()) {
            error = "PAYMASTER_INVALID_FINANCE_LEDGER";
            return false;
        }
        ledger.genesis_hash = genesis_hash;
        ledger.provider_id = identity.provider_id;
        ledger.history_complete_from = now;
        ledger.earlier_history_partial = identity.created_at < now;
        ledger.updated_at = now;
    }
    if (ledger.genesis_hash != genesis_hash ||
        ledger.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    ProviderFinanceEvent event;
    event.event_id = NativeFinanceEventId(identity.provider_id, kind, transaction->GetHash());
    event.genesis_hash = genesis_hash;
    event.provider_id = identity.provider_id;
    event.kind = kind;
    event.state = ProviderFinanceEventState::PENDING;
    event.transaction_id = transaction->GetHash();
    event.dgb_cost = dgb_cost;
    event.created_at = now;
    event.updated_at = now;
    ProviderFinanceEvent existing;
    const auto previous = batch.ReadPaymasterFinanceEvent(event.event_id, existing);
    if (previous == DatabaseReadStatus::FOUND) {
        // The identity of a retry is the transaction, not the wall clock at retry.
        event.created_at = existing.created_at;
        if (!CheckProviderFinanceEventBinding(existing, event, error)) return false;
        event = existing;
    } else if (previous != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_INVALID_FINANCE_EVENT";
        return false;
    }
    if (!batch.WritePaymasterFinanceEvents(ledger, {event}, error)) {
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

namespace paymaster_store::internal {
/** Prepare the accounting side of a provider commit. The exact service fee
 * and network fee are bound in the provider authorization manifest, so this
 * event can share the same database transaction as the durable commit. */
bool PrepareProviderTransferFinanceEvent(
    WalletBatch& batch,
    const ProviderIdentityRecord& identity,
    const ProviderAttempt& attempt,
    const ProviderCommitRecord& commit,
    const uint256& expected_genesis,
    ProviderFinanceMetadata& ledger,
    ProviderFinanceEvent& event,
    bool& changed,
    std::string& error)
{
    changed = false;
    if (!batch.MigratePaymasterFinanceLedger(error)) return false;
    const auto status = batch.ReadPaymasterFinanceMetadata(ledger);
    const bool have_ledger = status == DatabaseReadStatus::FOUND;
    if (!have_ledger && status != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (!have_ledger) {
        ledger.genesis_hash = expected_genesis;
        ledger.provider_id = identity.provider_id;
        ledger.history_complete_from = commit.committed_at;
        ledger.earlier_history_partial =
            identity.created_at < commit.committed_at;
        ledger.updated_at = commit.committed_at;
        changed = true;
    }
    if (ledger.genesis_hash != expected_genesis ||
        ledger.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }

    const ProviderAuthorizationManifest& manifest = attempt.provider_manifest;
    if (manifest.manifest_id.IsNull()) {
        error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        return false;
    }
    event = TransferEvent(identity, attempt, commit);
    event.genesis_hash = expected_genesis;

    ProviderFinanceEvent existing;
    const auto existing_status = batch.ReadPaymasterFinanceEvent(event.event_id, existing);
    if (existing_status == DatabaseReadStatus::FOUND) {
        // Exact replay preserves the latest chain observation.
        if (!CheckProviderFinanceEventBinding(existing, event, error)) return false;
        event = existing;
    } else if (existing_status == DatabaseReadStatus::NOT_FOUND) {
        changed = true;
    } else {
        error = "PAYMASTER_INVALID_FINANCE_EVENT";
        return false;
    }
    return ValidateProviderFinanceEvent(event, error);
}

} // namespace paymaster_store::internal

} // namespace wallet
