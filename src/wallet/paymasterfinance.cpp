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
#include <chrono>
#include <limits>
#include <map>
#include <mutex>

namespace wallet {
using paymaster_store::internal::PersistedReadError;
using paymaster_store::internal::ProviderPoolReadError;

namespace {
struct FinanceSnapshot {
    unsigned int database_updates;
    uint256 tip;
    uint64_t revision;
    std::chrono::steady_clock::time_point reconciled;
};
std::mutex finance_snapshots_mutex;
std::map<WalletDatabase*, FinanceSnapshot> finance_snapshots;
} // namespace

void ForgetPaymasterFinanceCache(WalletDatabase& database)
{
    std::lock_guard<std::mutex> lock{finance_snapshots_mutex};
    finance_snapshots.erase(&database);
}

bool ReconcileProviderFinanceReport(CWallet& wallet, bool continuation, std::string& error)
{
    LOCK(wallet.cs_wallet);
    auto& database = wallet.GetDatabase();
    const auto now = std::chrono::steady_clock::now();
    DigiDollar::Paymaster::ProviderFinanceMetadata metadata;
    // A point read still validates the persisted version/header on every page.
    // No registry lock spans database I/O, wallet callbacks or reconciliation.
    if (continuation && WalletBatch{database}.ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND) {
        std::lock_guard<std::mutex> lock{finance_snapshots_mutex};
        const auto found = finance_snapshots.find(&database);
        if (found != finance_snapshots.end()) {
            const auto& snapshot = found->second;
            if (snapshot.database_updates == database.nUpdateCounter.load() &&
                snapshot.tip == wallet.GetLastBlockHash() && snapshot.revision == metadata.revision &&
                now - snapshot.reconciled < std::chrono::seconds{60}) {
                error.clear();
                return true;
            }
        }
    }
    ForgetPaymasterFinanceCache(database);
    size_t changed{0};
    if (!ReconcileProviderFinances(wallet, changed, error)) return false;
    if (WalletBatch{database}.ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND) {
        std::lock_guard<std::mutex> lock{finance_snapshots_mutex};
        finance_snapshots[&database] = {database.nUpdateCounter.load(), wallet.GetLastBlockHash(), metadata.revision, now};
    }
    return true;
}

bool ReconcileProviderFinances(CWallet& wallet,
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
                const auto [entry, inserted] = confirmation_times.emplace(confirmed->confirmed_block_hash, 0);
                if (inserted) {
                    wallet.chain().findBlock(confirmed->confirmed_block_hash,
                                            interfaces::FoundBlock().time(entry->second));
                }
                block_time = entry->second;
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
        const auto position = event_positions.find(event.event_id);
        auto existing = position == event_positions.end()
            ? finance.events.end() : finance.events.begin() + position->second;
        if (existing != finance.events.end() &&
            !CheckProviderFinanceEventBinding(*existing, event, error)) return false;
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
            if (existing != finance.events.end()) {
                if (existing->updated_at == std::numeric_limits<int64_t>::max()) {
                    error = "PAYMASTER_FINANCE_REVISION_EXHAUSTED";
                    return false;
                }
                event.updated_at = std::max(event.updated_at, existing->updated_at + 1);
            }
            updates[event.event_id] = event;
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
        if (event.updated_at == std::numeric_limits<int64_t>::max()) {
            error = "PAYMASTER_FINANCE_REVISION_EXHAUSTED";
            return false;
        }
        event.updated_at = std::max({event.created_at, event.confirmed_at, now, event.updated_at + 1});
        updates[event.event_id] = event;
        finance.updated_at = std::max(
            finance.updated_at, event.updated_at);
        ledger_changed = true;
        ++changed_events;
    }

    // Wallet transaction metadata is persisted before broadcast, so finance
    // failure or a crash after either retirement step cannot lose its fee.
    const auto recover_cost = [&](ProviderFinanceEventKind kind, const uint256& txid) {
        HashWriter event_hasher = TaggedHash(
            "DigiByte Paymaster Finance Event v1");
        event_hasher << identity.provider_id << static_cast<uint8_t>(kind) << txid;
        const uint256 event_id = event_hasher.GetSHA256();
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
        const CAmount debit = wallet.GetDebit(
            *transaction->second.tx, ISMINE_ALL);
        const CAmount value_out = transaction->second.tx->GetValueOut();
        if (debit < value_out || !MoneyRange(debit - value_out)) {
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
        event.dgb_cost = DGBSatoshis{debit - value_out};
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
                : record.IsPreparation() ? ProviderFinanceEventKind::POOL_SETUP
                                         : ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT;
            event.transaction_id = record.transaction_id;
            event.dgb_cost = record.actual_fee;
            event.created_at = record.created_at;
            if (record.IsPreparation()) {
                const auto transaction = wallet.mapWallet.find(record.transaction_id);
                if (transaction != wallet.mapWallet.end()) {
                    event.created_at = std::max(identity.created_at, transaction->second.GetTxTime());
                }
            }
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
    ProviderFinanceMetadata metadata;
    metadata.genesis_hash = finance.genesis_hash;
    metadata.provider_id = finance.provider_id;
    metadata.history_complete_from = finance.history_complete_from;
    metadata.earlier_history_partial = finance.earlier_history_partial;
    metadata.updated_at = finance.updated_at;
    std::vector<ProviderFinanceEvent> changed;
    changed.reserve(updates.size());
    for (const auto& [id, event] : updates) changed.push_back(event);
    if (!batch.WritePaymasterFinanceEvents(metadata, changed, error)) {
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
    event = {};
    event.event_id = commit.commit_key;
    event.genesis_hash = expected_genesis;
    event.provider_id = identity.provider_id;
    event.kind = ProviderFinanceEventKind::TRANSFER;
    event.state = ProviderFinanceEventState::PENDING;
    event.funding_model = manifest.funding_model;
    event.sponsorship_scope = manifest.sponsorship_scope;
    event.transaction_id = commit.final_txid;
    event.dd_income = manifest.service_fee;
    event.dgb_cost = manifest.network_fee;
    event.created_at = commit.committed_at;
    event.updated_at = commit.committed_at;

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
