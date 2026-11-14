// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/walletdb.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymastersessionindex.h>
#include <wallet/paymasterfinancecache.h>
#include <wallet/wallet.h>
#include <paymaster/finance.h>

#include <chainparams.h>
#include <crypto/common.h>
#include <hash.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <ios>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
namespace {
using FinalIds = std::pair<uint256, uint256>;
struct SessionIndex {
    bool valid{false};
    bool commits_valid{false};
    bool pool_history_valid{false};
    std::set<COutPoint> pool_history_candidates;
    uint64_t revision{0};
    uint256 capacity_cleanup_after;
    std::map<PaymasterScan, PaymasterScanPosition> scans;
    std::map<uint256, std::set<std::string>> by_transaction;
    std::map<std::string, FinalIds> by_request;
    std::map<uint256, uint256> commit_transactions;
    std::map<uint256, std::set<uint256>> commits_by_transaction;

    void UpdateCommit(const uint256& key, const std::optional<uint256>& txid)
    {
        const auto old = commit_transactions.find(key);
        if (old != commit_transactions.end()) {
            auto& keys = commits_by_transaction[old->second];
            keys.erase(key);
            if (keys.empty()) commits_by_transaction.erase(old->second);
            commit_transactions.erase(old);
        }
        if (txid) {
            commit_transactions[key] = *txid;
            commits_by_transaction[*txid].insert(key);
        }
    }

    void Update(const std::string& request, const std::optional<FinalIds>& ids)
    {
        const auto old = by_request.find(request);
        if (old != by_request.end()) {
            for (const auto& txid : {old->second.first, old->second.second}) {
                auto entry = by_transaction.find(txid);
                if (entry == by_transaction.end()) continue;
                entry->second.erase(request);
                if (entry->second.empty()) by_transaction.erase(entry);
            }
            by_request.erase(old);
        }
        if (!ids || (ids->first.IsNull() && ids->second.IsNull())) return;
        by_request.emplace(request, *ids);
        for (const auto& txid : {ids->first, ids->second}) {
            if (!txid.IsNull()) by_transaction[txid].insert(request);
        }
    }
};
struct PendingSessionIndex {
    WalletDatabase* database{nullptr};
    std::map<std::string, std::optional<FinalIds>> updates;
    std::map<uint256, std::optional<uint256>> commits;
    bool pool_history_changed{false};
};

SessionIndex BuildSessionIndex(const std::vector<DigiDollar::Paymaster::PaymentSession>& sessions)
{
    SessionIndex index;
    for (const auto& session : sessions) {
        index.Update(session.request_id, FinalIds{session.final_txid, session.recovery_txid});
    }
    index.valid = true;
    return index;
}
// Lock order: wallet/database transaction -> index mutex. Never perform database
// I/O or invoke wallet callbacks while holding this registry lock.
std::mutex session_index_mutex;
std::map<WalletDatabase*, SessionIndex> session_indexes;
std::map<const WalletBatch*, PendingSessionIndex> pending_session_indexes;

void QueueSessionIndex(const WalletBatch* owner, WalletDatabase& database,
                       const std::string& request, std::optional<FinalIds> ids)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    auto& pending = pending_session_indexes[owner];
    pending.database = &database;
    pending.updates[request] = std::move(ids);
}
void QueueCommitIndex(const WalletBatch* owner, WalletDatabase& database,
                      const uint256& key, std::optional<uint256> txid)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    auto& pending = pending_session_indexes[owner];
    pending.database = &database;
    pending.commits[key] = txid;
}
} // namespace

void PaymasterSessionIndexEnd(const WalletBatch* owner, bool committed)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    const auto pending = pending_session_indexes.find(owner);
    if (pending == pending_session_indexes.end()) return;
    const auto index = session_indexes.find(pending->second.database);
    if (index != session_indexes.end()) {
        ++index->second.revision;
        if (pending->second.pool_history_changed) index->second.pool_history_valid = false;
        if (!committed) {
            index->second.valid = false;
            index->second.commits_valid = false;
            index->second.scans.clear();
        } else {
            if (index->second.valid)
                for (const auto& [request, ids] : pending->second.updates) index->second.Update(request, ids);
            if (index->second.commits_valid)
                for (const auto& [key, txid] : pending->second.commits) index->second.UpdateCommit(key, txid);
        }
    }
    pending_session_indexes.erase(pending);
}

void ForgetPaymasterSessionIndex(WalletDatabase& database)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    session_indexes.erase(&database);
}

PaymasterScanPosition GetPaymasterScanPosition(WalletDatabase& database, PaymasterScan scan)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    return session_indexes[&database].scans[scan];
}

void SetPaymasterScanPosition(WalletDatabase& database, PaymasterScan scan, const PaymasterScanPosition& position)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    session_indexes[&database].scans[scan] = position;
}

PaymasterScanPage::PaymasterScanPage(WalletDatabase& database, PaymasterScan scan)
    : m_database(database), m_scan(scan), after(GetPaymasterScanPosition(database, scan))
{
    SetPaymasterScanPosition(database, scan, {});
}
void PaymasterScanPage::Complete(const std::string& next)
{
    SetPaymasterScanPosition(m_database, m_scan, {next, {}});
}
void PaymasterScanPage::Complete(const uint256& next)
{
    SetPaymasterScanPosition(m_database, m_scan, {{}, next});
}

uint256 GetPaymasterCapacityCleanupCursor(WalletDatabase& database)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    return session_indexes[&database].capacity_cleanup_after;
}

void SetPaymasterCapacityCleanupCursor(WalletDatabase& database, const uint256& after)
{
    std::lock_guard<std::mutex> lock{session_index_mutex};
    session_indexes[&database].capacity_cleanup_after = after;
}

void PrimePaymasterSessionIndex(WalletDatabase& database,
                                const std::vector<DigiDollar::Paymaster::PaymentSession>& sessions)
{
    auto rebuilt = BuildSessionIndex(sessions);
    std::lock_guard<std::mutex> lock{session_index_mutex};
    rebuilt.revision = session_indexes[&database].revision + 1;
    rebuilt.capacity_cleanup_after = session_indexes[&database].capacity_cleanup_after;
    rebuilt.scans = std::move(session_indexes[&database].scans);
    rebuilt.commits_valid = session_indexes[&database].commits_valid;
    rebuilt.commit_transactions = std::move(session_indexes[&database].commit_transactions);
    rebuilt.commits_by_transaction = std::move(session_indexes[&database].commits_by_transaction);
    rebuilt.pool_history_valid = session_indexes[&database].pool_history_valid;
    rebuilt.pool_history_candidates = std::move(session_indexes[&database].pool_history_candidates);
    session_indexes[&database] = std::move(rebuilt);
}

bool ListPaymasterFinalSessions(WalletBatch& batch, WalletDatabase& database,
                                const uint256& txid,
                                std::vector<DigiDollar::Paymaster::PaymentSession>& sessions)
{
    sessions.clear();
    std::set<std::string> requests;
    bool rebuild;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock{session_index_mutex};
        const auto& index = session_indexes[&database];
        rebuild = !index.valid;
        revision = index.revision;
        const auto found = index.by_transaction.find(txid);
        if (!rebuild && found != index.by_transaction.end()) requests = found->second;
    }
    if (rebuild) {
        std::vector<DigiDollar::Paymaster::PaymentSession> all;
        if (!batch.ListPaymasterSessions(all)) return false;
        auto rebuilt = BuildSessionIndex(all);
        std::lock_guard<std::mutex> lock{session_index_mutex};
        auto& index = session_indexes[&database];
        // A concurrently completed write requires a new snapshot, never a
        // falsely empty lookup. Normal callers are serialized by cs_wallet.
        if (index.revision != revision) return false;
        rebuilt.revision = revision;
        rebuilt.capacity_cleanup_after = index.capacity_cleanup_after;
        rebuilt.scans = std::move(index.scans);
        rebuilt.commits_valid = index.commits_valid;
        rebuilt.commit_transactions = std::move(index.commit_transactions);
        rebuilt.commits_by_transaction = std::move(index.commits_by_transaction);
        index = std::move(rebuilt);
        const auto found = index.by_transaction.find(txid);
        if (found != index.by_transaction.end()) requests = found->second;
    }
    for (const auto& request : requests) {
        DigiDollar::Paymaster::PaymentSession session;
        if (batch.ReadPaymasterSessionWithStatus(request, session) != DatabaseReadStatus::FOUND ||
            (session.final_txid != txid && session.recovery_txid != txid)) {
            ForgetPaymasterSessionIndex(database);
            sessions.clear();
            return false;
        }
        sessions.push_back(std::move(session));
    }
    return true;
}

bool ListPaymasterProviderCommitsForTransaction(WalletBatch& batch, WalletDatabase& database,
    const uint256& txid, std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits)
{
    commits.clear();
    bool rebuild;
    uint64_t revision;
    std::set<uint256> keys;
    {
        std::lock_guard<std::mutex> lock{session_index_mutex};
        const auto& index = session_indexes[&database];
        rebuild = !index.commits_valid;
        revision = index.revision;
        if (!rebuild) {
            const auto found = index.commits_by_transaction.find(txid);
            if (found != index.commits_by_transaction.end()) keys = found->second;
        }
    }
    if (rebuild) {
        std::vector<DigiDollar::Paymaster::ProviderCommitRecord> all;
        if (!batch.ListPaymasterProviderCommits(all)) return false;
        SessionIndex rebuilt;
        for (const auto& commit : all) rebuilt.UpdateCommit(commit.commit_key, commit.final_txid);
        std::lock_guard<std::mutex> lock{session_index_mutex};
        auto& index = session_indexes[&database];
        if (index.revision != revision) return false;
        index.commit_transactions = std::move(rebuilt.commit_transactions);
        index.commits_by_transaction = std::move(rebuilt.commits_by_transaction);
        index.commits_valid = true;
        const auto found = index.commits_by_transaction.find(txid);
        if (found != index.commits_by_transaction.end()) keys = found->second;
    }
    for (const auto& key : keys) {
        DigiDollar::Paymaster::ProviderCommitRecord commit;
        if (batch.ReadPaymasterProviderCommitWithStatus(key, commit) != DatabaseReadStatus::FOUND || commit.final_txid != txid) {
            ForgetPaymasterSessionIndex(database);
            commits.clear();
            return false;
        }
        commits.push_back(std::move(commit));
    }
    return true;
}

namespace DBKeys {
const std::string PAYMASTER_SESSION{"pmsession"};
const std::string PAYMASTER_CLIENT_ORDER{"pmclientorder"};
const std::string PAYMASTER_RECOVERY{"pmrecovery"};
const std::string PAYMASTER_ALT_RECOVERY{"pmaltrecovery"};
const std::string PAYMASTER_ALT_RECOVERY_ARCHIVE{"pmaltarchive"};
const std::string PAYMASTER_ALT_RECOVERY_LOCATOR{"pmaltlocator"};
const std::string PAYMASTER_ALT_RECOVERY_REQUEST{"pmaltrecoveryreq"};
const std::string PAYMASTER_ATTEMPT{"pmattempt"};
const std::string PAYMASTER_CAPACITY{"pmcapacity"};
const std::string PAYMASTER_CAPACITY_SLOT{"pmcapacityslot"};
const std::string PAYMASTER_CAPACITY_RESOURCE{"pmcapacityresource"};
const std::string PAYMASTER_CAPACITY_RESPONSE{"pmcapresponse"};
const std::string PAYMASTER_CAPACITY_NONCE{"pmcapnonce"};
const std::string PAYMASTER_CAPACITY_SESSION{"pmcapsession"};
const std::string PAYMASTER_CAPACITY_RELEASE{"pmcaprelease"};
const std::string PAYMASTER_TEMPLATE{"pmtemplate"};
const std::string PAYMASTER_UNSIGNED_TX{"pmtxid"};
const std::string PAYMASTER_SESSION_ID{"pmsessionid"};
const std::string PAYMASTER_RESERVATION{"pmreserve"};
const std::string PAYMASTER_TOMBSTONE{"pmtombstone"};
const std::string PAYMASTER_PROVIDER_COMMIT{"pmcommit"};
const std::string PAYMASTER_USER_AUTH{"pmauth"};
const std::string PAYMASTER_IDENTITY{"pmidentity"};
const std::string PAYMASTER_POLICY{"pmpolicy"};
const std::string PAYMASTER_SETTINGS{"pmsettings"};
const std::string PAYMASTER_PROVIDER_SAFETY{"pmprovidersafety"};
const std::string PAYMASTER_CLIENT_SAFETY{"pmclientsafety"};
const std::string PAYMASTER_PROVIDER_BUDGET{"pmproviderbudget"};
const std::string PAYMASTER_CLIENT_FEES{"pmclientfees"};
const std::string PAYMASTER_SPONSOR_AUTH{"pmsponsor"};
const std::string PAYMASTER_PROVIDER_POOL{"pmpool"};
const std::string PAYMASTER_LIQUIDITY_POLICY{"pmliquidity"};
const std::string PAYMASTER_MAINTENANCE_LEDGER{"pmmaintenance"};
const std::string PAYMASTER_SETUP_AUTHORIZATION{"pmsetupauth"};
const std::string PAYMASTER_CARRIER_WITHDRAWAL{"pmcarrierwithdraw"};
const std::string PAYMASTER_FINANCE_LEDGER{"pmfinance"};
const std::string PAYMASTER_BACKUP_STATUS{"pmbackup"};
const std::string PAYMASTER_RESTORE_GUARD{"pmrestoreguard"};
const std::string PAYMASTER_ANNOUNCE_SEQ{"pmannounceseq"};
const std::string PAYMASTER_RESULT{"pmresult"};
const std::string PAYMASTER_RELIABILITY{"pmreliability"};
const std::string PAYMASTER_OUTCOME{"pmoutcome"};
const std::string PAYMASTER_EQUIVOCATION_PENDING{"pmequivocationpending"};
const std::string PAYMASTER_EQUIVOCATION{"pmequivocation"};
const std::string PAYMASTER_PROVIDER_BLOCK{"pmproviderblock"};
} // namespace DBKeys

// Each Paymaster mutation and its checkpoint share the same DB transaction.
// Standalone codecs get an implicit transaction; multi-record transitions use
// their caller's existing transaction and advance the external file once.
namespace {
template <typename T> void QueueCommitRecord(const WalletBatch*, WalletDatabase&, const T&) {}
void QueueCommitRecord(const WalletBatch* owner, WalletDatabase& database,
                       const DigiDollar::Paymaster::ProviderCommitRecord& commit)
{
    QueueCommitIndex(owner, database, commit.commit_key, commit.final_txid);
}
template <typename K> void QueueCommitErase(const WalletBatch*, WalletDatabase&, const K&) {}
void QueueCommitErase(const WalletBatch* owner, WalletDatabase& database,
                      const std::pair<std::string, uint256>& key)
{
    if (key.first == DBKeys::PAYMASTER_PROVIDER_COMMIT) QueueCommitIndex(owner, database, key.second, std::nullopt);
}
} // namespace
template <typename K, typename T>
bool WalletBatch::WritePaymasterIC(const K& key, const T& value, bool overwrite)
{
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    const bool written = PaymasterCheckpointWrite(this, *m_batch, m_database) &&
                         WriteIC(key, value, overwrite) &&
                         PaymasterCheckpointWrite(this, *m_batch, m_database);
    if (!written) {
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return false;
    }
    QueuePaymasterFinanceRecord(this, m_database, value);
    QueueCommitRecord(this, m_database, value);
    if (!own || TxnCommit()) return true;
    TxnAbort();
    return false;
}

template <typename K>
bool WalletBatch::ErasePaymasterIC(const K& key)
{
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    const bool erased = PaymasterCheckpointWrite(this, *m_batch, m_database) && EraseIC(key);
    if (!erased) {
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return false;
    }
    QueuePaymasterFinanceErase(this, m_database, key);
    QueueCommitErase(this, m_database, key);
    if (!own || TxnCommit()) return true;
    TxnAbort();
    return false;
}

// -------------------------------------------------------------------------
// Paymaster database codecs
// -------------------------------------------------------------------------
// WalletBatch provides record-level encoding and shape validation. Multi-record
// security transitions belong to PaymasterStore and must use one explicit DB
// transaction; callers must not compose a signing transition from independent
// WritePaymaster* calls.
uint256 GetPaymasterMaintenanceExecutionHash(const PaymasterMaintenanceExecution& execution)
{
    return (HashWriter{} << std::string{"DigiByte/PaymasterNativeMaintenance/v1"} << execution).GetHash();
}

bool ValidatePaymasterMaintenanceExecution(const PaymasterMaintenanceExecution& execution)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    if (execution.version != PaymasterMaintenanceExecution::CURRENT_VERSION ||
        execution.genesis_hash != Params().GenesisBlock().GetHash() || execution.provider_id.IsNull() ||
        execution.operation_id.IsNull() || execution.created_at <= 0 || execution.legs.empty() || execution.legs.size() > 2 ||
        !ValidateProviderPoolEntries(execution.before_pool, error) || !ValidateProviderPoolEntries(execution.after_pool, error) ||
        (!execution.had_pool && !execution.before_pool.empty()) ||
        (execution.has_ledger && (!ValidateProviderMaintenanceLedger(execution.before_ledger, error) ||
                                  !ValidateProviderMaintenanceLedger(execution.after_ledger, error)))) return false;
    std::set<COutPoint> inputs;
    for (const auto& leg : execution.legs) {
        if (leg.transaction.empty() || leg.transaction.size() > 4 * 1024 * 1024 ||
            leg.fee.value <= 0 || !MoneyRange(leg.fee.value) || leg.metadata.size() > 2 ||
            (leg.digidollar && (leg.history_amount.value <= 0 || leg.history_recipient.empty()))) return false;
        for (const auto& [key, value] : leg.metadata) {
            if ((key != "comment" && key != "paymaster_retirement_provider") || value.size() > 4096) return false;
        }
        try {
            CDataStream stream{leg.transaction, SER_NETWORK, ::PROTOCOL_VERSION};
            CMutableTransaction transaction;
            stream >> transaction;
            CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
            canonical << transaction;
            const auto bytes = MakeUCharSpan(canonical);
            if (!stream.empty() || std::vector<unsigned char>(bytes.begin(), bytes.end()) != leg.transaction ||
                transaction.vin.empty() || transaction.vout.empty()) return false;
            for (const auto& input : transaction.vin)
                if (input.prevout.IsNull() || !inputs.insert(input.prevout).second ||
                    (input.scriptSig.empty() && input.scriptWitness.IsNull())) return false;
        } catch (const std::exception&) { return false; }
    }
    return true;
}

bool WalletBatch::WritePaymasterMaintenanceExecution(const PaymasterMaintenanceExecution& execution)
{
    PaymasterMaintenanceExecution previous;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    if (!ValidatePaymasterMaintenanceExecution(execution) ||
        ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND || identity.provider_id != execution.provider_id ||
        ReadPaymasterMaintenanceExecutionWithStatus(previous) != DatabaseReadStatus::NOT_FOUND) return false;
    return WritePaymasterIC(std::string{"pmnativeexec"}, execution, false);
}

DatabaseReadStatus WalletBatch::ReadPaymasterMaintenanceExecutionWithStatus(PaymasterMaintenanceExecution& execution)
{
    struct StrictExecution : PaymasterMaintenanceExecution {
        void Unserialize(DataStream& stream)
        {
            stream >> static_cast<PaymasterMaintenanceExecution&>(*this);
            if (!stream.empty()) throw std::ios_base::failure("maintenance execution trailing bytes");
        }
    } strict;
    execution = {};
    const auto status = m_batch->ReadWithStatus(std::string{"pmnativeexec"}, strict);
    execution = strict;
    if ((status == DatabaseReadStatus::FOUND || status == DatabaseReadStatus::READ_ERROR) &&
        execution.version != PaymasterMaintenanceExecution::CURRENT_VERSION) return DatabaseReadStatus::UNSUPPORTED_VERSION;
    if (status != DatabaseReadStatus::FOUND) return status;
    return ValidatePaymasterMaintenanceExecution(execution) ? DatabaseReadStatus::FOUND : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ErasePaymasterMaintenanceExecution()
{
    return ErasePaymasterIC(std::string{"pmnativeexec"});
}

template <typename K, typename T>
DatabaseReadStatus WalletBatch::ReadPaymasterVersionedRecord(const K& key,
                                                              T& value)
{
    value = T{};
    const DatabaseReadStatus status = m_batch->ReadWithStatus(key, value);
    // Deserialization of an authentic shortened old layout may stop after its
    // leading version field. Preserve that distinction instead of treating the
    // record as absent or as an unspecified storage failure.
    if ((status == DatabaseReadStatus::FOUND ||
         status == DatabaseReadStatus::READ_ERROR) &&
        value.version != T::CURRENT_VERSION) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    return status;
}

bool WalletBatch::WritePaymasterClientOrder(const DigiDollar::Paymaster::ClientPaymentOrder& order, bool overwrite)
{
    using namespace DigiDollar::Paymaster;
    if (order.version != ClientPaymentOrder::CURRENT_VERSION ||
        !IsCanonicalRequestId(order.request_id) || order.session_id.IsNull() ||
        order.genesis_hash.IsNull() || order.canonical_request_hash.IsNull()) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CLIENT_ORDER, order.request_id), order, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterClientOrderWithStatus(
    const std::string& request_id, DigiDollar::Paymaster::ClientPaymentOrder& order)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    order = {};
    const auto status = m_batch->ReadWithStatus(std::make_pair(DBKeys::PAYMASTER_CLIENT_ORDER, request_id), order);
    if ((status == DatabaseReadStatus::FOUND || status == DatabaseReadStatus::READ_ERROR) &&
        order.version != 1 && order.version != ClientPaymentOrder::CURRENT_VERSION) return DatabaseReadStatus::UNSUPPORTED_VERSION;
    if (status != DatabaseReadStatus::FOUND) return status;
    if (order.request_id != request_id || order.session_id.IsNull() ||
        order.genesis_hash.IsNull() || order.canonical_request_hash.IsNull() ||
        order.preferred_provider.IsNull() != order.preferred_offer.IsNull()) return DatabaseReadStatus::READ_ERROR;
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterClientOrder(const std::string& request_id)
{
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id) &&
        ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CLIENT_ORDER, request_id));
}

bool WalletBatch::WritePaymasterSession(const DigiDollar::Paymaster::PaymentSession& session, bool overwrite)
{
    if (!DigiDollar::Paymaster::IsCanonicalRequestId(session.request_id) ||
        session.version != DigiDollar::Paymaster::PaymentSession::CURRENT_VERSION) {
        return false;
    }
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    if (!WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_SESSION, session.request_id), session, overwrite)) {
        if (own) TxnAbort();
        return false;
    }
    QueueSessionIndex(this, m_database, session.request_id,
                      FinalIds{session.final_txid, session.recovery_txid});
    if (!own || TxnCommit()) return true;
    TxnAbort();
    return false;
}

DatabaseReadStatus WalletBatch::ReadPaymasterSessionWithStatus(
    const std::string& request_id,
    DigiDollar::Paymaster::PaymentSession& session)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_SESSION, request_id), session);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (session.request_id != request_id || session.session_id.IsNull() ||
        !CanTransition(session.state, session.state)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterSession(
    const std::string& request_id,
    DigiDollar::Paymaster::PaymentSession& session)
{
    return ReadPaymasterSessionWithStatus(request_id, session) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterSession(const std::string& request_id)
{
    return m_batch->Exists(
        std::make_pair(DBKeys::PAYMASTER_SESSION, request_id));
}

bool WalletBatch::ListPaymasterSessions(
    std::vector<DigiDollar::Paymaster::PaymentSession>& sessions)
{
    sessions.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_SESSION;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            std::string request_id;
            DigiDollar::Paymaster::PaymentSession session;
            key >> type >> request_id;
            value >> session;
            if (type != DBKeys::PAYMASTER_SESSION ||
                session.version !=
                    DigiDollar::Paymaster::PaymentSession::CURRENT_VERSION ||
                session.request_id != request_id ||
                !DigiDollar::Paymaster::IsCanonicalRequestId(request_id) ||
                session.session_id.IsNull()) {
                return false;
            }
            sessions.push_back(std::move(session));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ListPaymasterSessionsPage(const std::string& after, size_t maximum_rows,
    size_t maximum_bytes, std::vector<DigiDollar::Paymaster::PaymentSession>& sessions, std::string& next)
{
    using namespace DigiDollar::Paymaster;
    constexpr size_t maximum_row_bytes{8192};
    sessions.clear();
    next.clear();
    if ((!after.empty() && !IsCanonicalRequestId(after)) || maximum_rows == 0 || maximum_rows > 100 ||
        maximum_bytes < maximum_row_bytes || maximum_bytes > 100 * maximum_row_bytes) return false;
    DataStream begin;
    begin << DBKeys::PAYMASTER_SESSION;
    SerializeData end{begin.begin(), begin.end()};
    if (end.empty() || end.back() == std::byte{0xff}) return false;
    end.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(end.back()) + 1)};
    if (!after.empty()) begin << after << uint8_t{0};
    auto cursor = m_batch->GetNewRangeCursor(begin, end);
    if (!cursor) return false;
    size_t scanned{0}, bytes{0};
    while (scanned < maximum_rows && bytes <= maximum_bytes - maximum_row_bytes) {
        DataStream key, value;
        const auto status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) { next.clear(); return true; }
        if (status != DatabaseCursor::Status::MORE || key.size() > maximum_row_bytes ||
            value.size() > maximum_row_bytes - key.size()) return false;
        ++scanned;
        bytes += key.size() + value.size();
        try {
            std::string type, request;
            PaymentSession session;
            key >> type >> request;
            value >> session;
            if (!key.empty() || !value.empty() || type != DBKeys::PAYMASTER_SESSION ||
                !IsCanonicalRequestId(request) || (!after.empty() && request <= after) ||
                session.request_id != request || session.version != PaymentSession::CURRENT_VERSION ||
                session.session_id.IsNull() || !CanTransition(session.state, session.state) ||
                session.user_inputs.size() > MAX_PAYMENT_INTENT_INPUTS || session.attempt_ids.size() > 16) return false;
            next = request;
            sessions.push_back(std::move(session));
        } catch (const std::ios_base::failure&) { return false; }
    }
    return true;
}

bool WalletBatch::ErasePaymasterSession(const std::string& request_id)
{
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    if (!ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_SESSION, request_id))) {
        if (own) TxnAbort();
        return false;
    }
    QueueSessionIndex(this, m_database, request_id, std::nullopt);
    if (!own || TxnCommit()) return true;
    TxnAbort();
    return false;
}

bool WalletBatch::WritePaymasterRecovery(
    const DigiDollar::Paymaster::SelfRecoveryRecord& recovery,
    bool overwrite)
{
    if (recovery.version != DigiDollar::Paymaster::SelfRecoveryRecord::CURRENT_VERSION ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(recovery.request_id) ||
        recovery.session_id.IsNull() || recovery.user_inputs.empty() ||
        recovery.recovery_txid.IsNull() || recovery.raw_transaction_hash.IsNull() ||
        recovery.final_transaction.empty() || recovery.created_at <= 0) {
        return false;
    }
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RECOVERY, recovery.request_id),
                   recovery, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterRecoveryWithStatus(
    const std::string& request_id,
    DigiDollar::Paymaster::SelfRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RECOVERY, request_id), recovery);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (recovery.request_id != request_id || recovery.session_id.IsNull() ||
        recovery.user_inputs.empty() || recovery.recovery_txid.IsNull() ||
        recovery.raw_transaction_hash.IsNull() ||
        recovery.final_transaction.empty() || recovery.created_at <= 0) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterRecovery(
    const std::string& request_id,
    DigiDollar::Paymaster::SelfRecoveryRecord& recovery)
{
    return ReadPaymasterRecoveryWithStatus(request_id, recovery) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterRecovery(const std::string& request_id)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RECOVERY, request_id));
}

bool WalletBatch::WritePaymasterAlternativeRecovery(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    bool overwrite)
{
    using namespace DigiDollar::Paymaster;
    AlternativeRecoveryRecord archived;
    if (ReadPaymasterVersionedRecord(
            std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE, recovery.recovery_id),
            archived) != DatabaseReadStatus::NOT_FOUND) return false;
    uint256 archived_commit;
    if (m_batch->ReadWithStatus(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_LOCATOR,
            recovery.recovery_id), archived_commit) != DatabaseReadStatus::NOT_FOUND) return false;
    if (recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        !IsCanonicalRequestId(recovery.request_id) ||
        recovery.session_id.IsNull() || recovery.recovery_id.IsNull() ||
        recovery.original_provider_id.IsNull() ||
        recovery.recovery_provider_id.IsNull() ||
        recovery.offer_id.IsNull() || recovery.policy_hash.IsNull() ||
        recovery.original_provider_id == recovery.recovery_provider_id ||
        !recovery.recovery_provider_identity_key.IsFullyValid() ||
        GetPaymasterId(recovery.recovery_provider_identity_key) !=
            recovery.recovery_provider_id ||
        recovery.original_commit_key.IsNull() ||
        recovery.original_template_commitment.IsNull() ||
        recovery.client_nonce.IsNull() || recovery.created_at <= 0 ||
        recovery.updated_at < recovery.created_at ||
        recovery.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        recovery.recovery_id != GetAlternativeRecoveryId(
                                    recovery.request_id, recovery.session_id,
                                    recovery.recovery_provider_id, recovery.client_nonce)) {
        return false;
    }
    return WritePaymasterIC(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id),
        recovery, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterAlternativeRecoveryWithStatus(
    const uint256& recovery_id,
    DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    if (recovery_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus active_status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery_id), recovery);
    AlternativeRecoveryRecord archived;
    const DatabaseReadStatus archive_status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE, recovery_id), archived);
    uint256 archived_commit;
    const auto locator_status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_LOCATOR, recovery_id), archived_commit);
    if (locator_status != DatabaseReadStatus::FOUND && locator_status != DatabaseReadStatus::NOT_FOUND)
        return locator_status;
    if ((archive_status == DatabaseReadStatus::NOT_FOUND) != (locator_status == DatabaseReadStatus::NOT_FOUND))
        return DatabaseReadStatus::READ_ERROR;
    if (active_status != DatabaseReadStatus::NOT_FOUND &&
        archive_status != DatabaseReadStatus::NOT_FOUND) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = active_status == DatabaseReadStatus::NOT_FOUND
        ? archive_status : active_status;
    if (active_status == DatabaseReadStatus::NOT_FOUND) recovery = std::move(archived);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (active_status == DatabaseReadStatus::NOT_FOUND) {
        if (!recovery.provider_side || recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
            archived_commit.IsNull() || archived_commit != recovery.recovery_response.recovery_commit_key)
            return DatabaseReadStatus::READ_ERROR;
    }
    if (recovery.recovery_id != recovery_id ||
        !IsCanonicalRequestId(recovery.request_id) ||
        recovery.session_id.IsNull() || recovery.original_provider_id.IsNull() ||
        recovery.recovery_provider_id.IsNull() || recovery.offer_id.IsNull() ||
        recovery.policy_hash.IsNull() ||
        recovery.original_provider_id == recovery.recovery_provider_id ||
        !recovery.recovery_provider_identity_key.IsFullyValid() ||
        GetPaymasterId(recovery.recovery_provider_identity_key) !=
            recovery.recovery_provider_id ||
        recovery.original_commit_key.IsNull() ||
        recovery.original_template_commitment.IsNull() ||
        recovery.client_nonce.IsNull() ||
        recovery.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        recovery.recovery_id != GetAlternativeRecoveryId(
                                    recovery.request_id, recovery.session_id,
                                    recovery.recovery_provider_id,
                                    recovery.client_nonce) ||
        recovery.created_at <= 0 || recovery.updated_at < recovery.created_at) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterAlternativeRecovery(
    const uint256& recovery_id,
    DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    return ReadPaymasterAlternativeRecoveryWithStatus(recovery_id, recovery) ==
           DatabaseReadStatus::FOUND;
}

namespace {
/** Bound raw rows and bytes before filtering. Point reads reuse the canonical
 * version/binding checks, including active/archive conflicts for recoveries.
 * The caller holds cs_wallet, so the cursor and point read see the same row. */
template <typename Record, typename Read>
bool ReadPaymasterRecordPage(DatabaseBatch& batch, const std::string& prefix,
    const uint256& after, size_t maximum_row_bytes, std::vector<Record>& records,
    uint256& next, Read read)
{
    records.clear();
    next.SetNull();
    DataStream begin;
    begin << prefix;
    SerializeData end{begin.begin(), begin.end()};
    if (end.empty() || end.back() == std::byte{0xff}) return false;
    end.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(end.back()) + 1)};
    if (!after.IsNull()) begin << after << uint8_t{0};
    auto cursor = batch.GetNewRangeCursor(begin, end);
    if (!cursor) return false;
    size_t bytes{0};
    constexpr size_t maximum_rows{64};
    const size_t maximum_bytes{2 * maximum_row_bytes};
    while (records.size() < maximum_rows && bytes <= maximum_bytes - maximum_row_bytes) {
        DataStream key, value;
        const auto status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) { next.SetNull(); return true; }
        if (status != DatabaseCursor::Status::MORE || key.size() > maximum_row_bytes ||
            value.size() > maximum_row_bytes - key.size()) return false;
        bytes += key.size() + value.size();
        try {
            std::string type;
            uint256 id;
            key >> type >> id;
            Record record;
            if (!key.empty() || type != prefix || id.IsNull() ||
                (!after.IsNull() && !(after < id)) || !read(id, record)) return false;
            records.push_back(std::move(record));
            next = id;
        } catch (const std::ios_base::failure&) { return false; }
    }
    return true;
}
} // namespace

bool WalletBatch::ListPaymasterAlternativeRecoveriesPage(const uint256& after,
    std::vector<DigiDollar::Paymaster::AlternativeRecoveryRecord>& recoveries, uint256& next)
{
    return ReadPaymasterRecordPage(*m_batch, DBKeys::PAYMASTER_ALT_RECOVERY, after,
        16 * DigiDollar::Paymaster::MAX_DIRECT_MESSAGE_BYTES, recoveries, next,
        [this](const auto& id, auto& record) { return ReadPaymasterAlternativeRecovery(id, record); });
}

bool WalletBatch::ListPaymasterCapacitySnapshotsPage(const uint256& after,
    std::vector<DigiDollar::Paymaster::ValidatedCapacitySnapshot>& snapshots, uint256& next)
{
    return ReadPaymasterRecordPage(*m_batch, DBKeys::PAYMASTER_CAPACITY, after,
        DigiDollar::Paymaster::MAX_DIRECT_MESSAGE_BYTES + 8192, snapshots, next,
        [this](const auto& id, auto& record) {
            return ReadPaymasterCapacitySnapshot(id, record) && !record.provider_id.IsNull();
        });
}

bool WalletBatch::ListPaymasterProviderCommitsPage(const uint256& after,
    std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits, uint256& next)
{
    return ReadPaymasterRecordPage(*m_batch, DBKeys::PAYMASTER_PROVIDER_COMMIT, after,
        4 * DigiDollar::Paymaster::MAX_DIRECT_MESSAGE_BYTES + 8192, commits, next,
        [this](const auto& id, auto& record) { return ReadPaymasterProviderCommit(id, record); });
}

bool WalletBatch::ListPaymasterAlternativeRecoveries(
    std::vector<DigiDollar::Paymaster::AlternativeRecoveryRecord>& recoveries)
{
    using DigiDollar::Paymaster::AlternativeRecoveryRecord;
    recoveries.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_ALT_RECOVERY;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 recovery_id;
            AlternativeRecoveryRecord recovery;
            key >> type >> recovery_id;
            CDataStream recovery_stream{MakeUCharSpan(value), SER_DISK,
                                        CLIENT_VERSION};
            recovery_stream >> recovery;
            if (type != DBKeys::PAYMASTER_ALT_RECOVERY ||
                !recovery_stream.empty() ||
                recovery_id.IsNull() || recovery.recovery_id != recovery_id ||
                recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
                recovery.capacity_proof_claim_candidate.size() >
                    DigiDollar::Paymaster::MAX_EQUIVOCATION_ARTIFACT_BYTES) {
                return false;
            }
            AlternativeRecoveryRecord checked;
            if (ReadPaymasterAlternativeRecoveryWithStatus(recovery_id, checked) != DatabaseReadStatus::FOUND)
                return false;
            recoveries.push_back(std::move(recovery));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterAlternativeRecovery(
    const uint256& recovery_id)
{
    return !recovery_id.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY,
                                  recovery_id));
}

bool WalletBatch::ArchivePaymasterAlternativeRecovery(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    AlternativeRecoveryRecord current;
    AlternativeRecoveryRecord archived;
    const auto canonical_bytes = [](const AlternativeRecoveryRecord& value) {
        CDataStream stream{SER_DISK, CLIENT_VERSION};
        stream << value;
        return SerializeData{stream.begin(), stream.end()};
    };
    const auto& commit_key = recovery.recovery_response.recovery_commit_key;
    if (!PaymasterCheckpointInTransaction(this) || !recovery.provider_side ||
        recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
        commit_key.IsNull() ||
        ReadPaymasterAlternativeRecoveryWithStatus(recovery.recovery_id, current) != DatabaseReadStatus::FOUND ||
        canonical_bytes(current) != canonical_bytes(recovery) ||
        ReadPaymasterVersionedRecord(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE,
            recovery.recovery_id), archived) != DatabaseReadStatus::NOT_FOUND) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE,
               recovery.recovery_id), recovery, false) &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_LOCATOR,
               recovery.recovery_id), commit_key, false) &&
           ErasePaymasterAlternativeRecovery(recovery.recovery_id);
}

bool WalletBatch::WritePaymasterAlternativeRecoveryRequest(
    const std::string& request_id, const uint256& recovery_id,
    bool overwrite)
{
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id) &&
           !recovery_id.IsNull() &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST,
                                  request_id),
                   recovery_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterAlternativeRecoveryRequestWithStatus(
    const std::string& request_id, uint256& recovery_id)
{
    recovery_id.SetNull();
    if (!DigiDollar::Paymaster::IsCanonicalRequestId(request_id)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST, request_id),
        recovery_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return recovery_id.IsNull() ? DatabaseReadStatus::READ_ERROR
                                : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterAlternativeRecoveryRequest(
    const std::string& request_id, uint256& recovery_id)
{
    return ReadPaymasterAlternativeRecoveryRequestWithStatus(
               request_id, recovery_id) == DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterAlternativeRecoveryRequest(
    const std::string& request_id)
{
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id) &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST,
                                  request_id));
}

bool WalletBatch::WritePaymasterAttempt(const DigiDollar::Paymaster::ProviderAttempt& attempt, bool overwrite)
{
    if (attempt.version != DigiDollar::Paymaster::ProviderAttempt::CURRENT_VERSION || attempt.attempt_id.IsNull() ||
        attempt.capacity_proof_claim_candidate.size() >
            DigiDollar::Paymaster::MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        attempt.quote_response_claim_candidate.size() >
            DigiDollar::Paymaster::MAX_EQUIVOCATION_ARTIFACT_BYTES) {
        return false;
    }
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt.attempt_id), attempt, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterAttemptWithStatus(
    const uint256& attempt_id,
    DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    using namespace DigiDollar::Paymaster;
    if (attempt_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt_id), attempt);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (attempt.attempt_id != attempt_id ||
        !CanTransition(attempt.state, attempt.state) ||
        attempt.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        attempt.quote_response_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterAttempt(
    const uint256& attempt_id,
    DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    return ReadPaymasterAttemptWithStatus(attempt_id, attempt) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterAttempt(const uint256& attempt_id)
{
    return !attempt_id.IsNull() &&
           m_batch->Exists(
               std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt_id));
}

bool WalletBatch::ErasePaymasterAttempt(const uint256& attempt_id)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, attempt_id));
}

bool WalletBatch::WritePaymasterCapacitySnapshot(
    const DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot,
    bool overwrite)
{
    if (snapshot.version != DigiDollar::Paymaster::ValidatedCapacitySnapshot::CURRENT_VERSION ||
        snapshot.snapshot_id.IsNull() || snapshot.resource_commitment.IsNull() ||
        snapshot.session_id.IsNull() || snapshot.attempt_id.IsNull()) {
        return false;
    }
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY, snapshot.snapshot_id),
                   snapshot, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacitySnapshotWithStatus(
    const uint256& snapshot_id,
    DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot)
{
    using DigiDollar::Paymaster::ValidatedCapacitySnapshot;
    if (snapshot_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY, snapshot_id), snapshot);
    if (status != DatabaseReadStatus::FOUND) return status;
    return snapshot.snapshot_id == snapshot_id &&
                   !snapshot.resource_commitment.IsNull() &&
                   !snapshot.session_id.IsNull() && !snapshot.attempt_id.IsNull()
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCapacitySnapshot(
    const uint256& snapshot_id,
    DigiDollar::Paymaster::ValidatedCapacitySnapshot& snapshot)
{
    return ReadPaymasterCapacitySnapshotWithStatus(snapshot_id, snapshot) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterCapacitySnapshots(
    std::vector<DigiDollar::Paymaster::ValidatedCapacitySnapshot>& snapshots)
{
    using DigiDollar::Paymaster::ValidatedCapacitySnapshot;
    snapshots.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_CAPACITY;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 snapshot_id;
            ValidatedCapacitySnapshot snapshot;
            key >> type >> snapshot_id;
            value >> snapshot;
            if (type != DBKeys::PAYMASTER_CAPACITY || snapshot_id.IsNull() ||
                snapshot.snapshot_id != snapshot_id ||
                snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
                snapshot.resource_commitment.IsNull() ||
                snapshot.provider_id.IsNull() || snapshot.session_id.IsNull() ||
                snapshot.attempt_id.IsNull()) {
                return false;
            }
            snapshots.push_back(std::move(snapshot));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterCapacitySnapshot(const uint256& snapshot_id)
{
    return !snapshot_id.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY, snapshot_id));
}

bool WalletBatch::WritePaymasterCapacitySlot(const uint256& resource_commitment,
                                             const uint256& snapshot_id,
                                             bool overwrite)
{
    return !resource_commitment.IsNull() && !snapshot_id.IsNull() &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SLOT, resource_commitment),
                   snapshot_id, overwrite);
}

bool WalletBatch::ReadPaymasterCapacitySlot(const uint256& resource_commitment,
                                            uint256& snapshot_id)
{
    return !resource_commitment.IsNull() &&
           m_batch->Read(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SLOT, resource_commitment),
                         snapshot_id) &&
           !snapshot_id.IsNull();
}

bool WalletBatch::ErasePaymasterCapacitySlot(
    const uint256& resource_commitment)
{
    return !resource_commitment.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SLOT,
                                  resource_commitment));
}

bool WalletBatch::WritePaymasterCapacityResource(
    const DigiDollar::Paymaster::CapacityResourceBinding& binding,
    bool overwrite)
{
    using DigiDollar::Paymaster::CapacityResourceBinding;
    if (binding.version != CapacityResourceBinding::CURRENT_VERSION ||
        binding.provider_id.IsNull() || binding.outpoint.IsNull() ||
        binding.snapshot_id.IsNull() || binding.resource_commitment.IsNull() ||
        binding.session_id.IsNull() || binding.attempt_id.IsNull() ||
        binding.proof_hash.IsNull() || binding.creating_txid.IsNull() ||
        binding.value <= 0 || binding.expires_at <= 0) {
        return false;
    }
    return WritePaymasterIC(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESOURCE,
                       std::make_pair(binding.provider_id, binding.outpoint)),
        binding, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacityResourceWithStatus(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const COutPoint& outpoint,
    DigiDollar::Paymaster::CapacityResourceBinding& binding)
{
    using DigiDollar::Paymaster::CapacityResourceBinding;
    if (provider_id.IsNull() || outpoint.IsNull()) {
        return DatabaseReadStatus::READ_ERROR;
    }
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESOURCE,
                       std::make_pair(provider_id, outpoint)),
        binding);
    if (status != DatabaseReadStatus::FOUND) return status;
    return binding.provider_id == provider_id && binding.outpoint == outpoint &&
                   !binding.snapshot_id.IsNull() &&
                   !binding.resource_commitment.IsNull() &&
                   !binding.session_id.IsNull() && !binding.attempt_id.IsNull() &&
                   !binding.proof_hash.IsNull() &&
                   !binding.creating_txid.IsNull() && binding.value > 0 &&
                   binding.expires_at > 0
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCapacityResource(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const COutPoint& outpoint,
    DigiDollar::Paymaster::CapacityResourceBinding& binding)
{
    return ReadPaymasterCapacityResourceWithStatus(provider_id, outpoint,
                                                    binding) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterCapacityResource(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const COutPoint& outpoint)
{
    return !provider_id.IsNull() && !outpoint.IsNull() &&
           ErasePaymasterIC(std::make_pair(
               DBKeys::PAYMASTER_CAPACITY_RESOURCE,
               std::make_pair(provider_id, outpoint)));
}

bool WalletBatch::WritePaymasterCapacityResponse(
    const uint256& request_hash,
    const std::vector<unsigned char>& response,
    bool overwrite)
{
    return !request_hash.IsNull() && !response.empty() &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, request_hash),
                   response, overwrite);
}

bool WalletBatch::ReadPaymasterCapacityResponse(
    const uint256& request_hash,
    std::vector<unsigned char>& response)
{
    return ReadPaymasterCapacityResponseWithStatus(request_hash, response) == DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacityResponseWithStatus(
    const uint256& request_hash, std::vector<unsigned char>& response)
{
    response.clear();
    if (request_hash.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const auto status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, request_hash), response);
    if (status != DatabaseReadStatus::FOUND) return status;
    return !response.empty() ? DatabaseReadStatus::FOUND : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ListPaymasterCapacityResponses(
    std::vector<std::pair<uint256, std::vector<unsigned char>>>& responses)
{
    responses.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_CAPACITY_RESPONSE;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 request_hash;
            std::vector<unsigned char> response;
            key >> type >> request_hash;
            value >> response;
            if (type != DBKeys::PAYMASTER_CAPACITY_RESPONSE ||
                request_hash.IsNull() || response.empty()) {
                return false;
            }
            responses.emplace_back(request_hash, std::move(response));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ListPaymasterCapacityResponsesPage(
    const uint256& after, size_t maximum_responses, size_t maximum_bytes,
    std::vector<std::pair<uint256, std::vector<unsigned char>>>& responses,
    uint256& next)
{
    using namespace DigiDollar::Paymaster;
    // Reserve room before each cursor read for a complete permitted row,
    // including its key and CompactSize prefix. Thus a successful page bounds
    // bytes actually scanned, not merely the returned payload lengths.
    constexpr size_t maximum_row_bytes{MAX_DIRECT_MESSAGE_BYTES + 128};
    responses.clear();
    next.SetNull();
    if (maximum_responses == 0 || maximum_responses > 64 ||
        maximum_bytes < maximum_row_bytes || maximum_bytes > 4 * maximum_row_bytes) return false;
    DataStream start;
    start << DBKeys::PAYMASTER_CAPACITY_RESPONSE;
    SerializeData end{start.begin(), start.end()};
    if (end.empty() || end.back() == std::byte{0xff}) return false;
    end.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(end.back()) + 1)};
    if (!after.IsNull()) start << after << uint8_t{0};
    auto cursor = m_batch->GetNewRangeCursor(start, end);
    if (!cursor) return false;
    size_t scanned_bytes{0};
    while (responses.size() < maximum_responses &&
           scanned_bytes <= maximum_bytes - maximum_row_bytes) {
        DataStream key, value;
        const auto status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) {
            next.SetNull();
            return true;
        }
        if (status != DatabaseCursor::Status::MORE ||
            key.size() > maximum_row_bytes || value.size() > maximum_row_bytes - key.size()) return false;
        scanned_bytes += key.size() + value.size();
        try {
            std::string type;
            uint256 request_hash;
            std::vector<unsigned char> response;
            key >> type >> request_hash;
            value >> response;
            if (!key.empty() || !value.empty() ||
                type != DBKeys::PAYMASTER_CAPACITY_RESPONSE || request_hash.IsNull() ||
                (!after.IsNull() && !(after < request_hash)) || response.empty() ||
                response.size() > MAX_DIRECT_MESSAGE_BYTES) return false;
            responses.emplace_back(request_hash, std::move(response));
            next = request_hash;
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterCapacityResponse(const uint256& request_hash)
{
    return !request_hash.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE,
                                  request_hash));
}

bool WalletBatch::WritePaymasterCapacityNonce(const uint256& client_nonce,
                                              const uint256& request_hash,
                                              bool overwrite)
{
    return !client_nonce.IsNull() && !request_hash.IsNull() &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, client_nonce),
                   request_hash, overwrite);
}

bool WalletBatch::ReadPaymasterCapacityNonce(const uint256& client_nonce,
                                             uint256& request_hash)
{
    return ReadPaymasterCapacityNonceWithStatus(client_nonce, request_hash) == DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacityNonceWithStatus(
    const uint256& client_nonce, uint256& request_hash)
{
    request_hash.SetNull();
    if (client_nonce.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const auto status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, client_nonce), request_hash);
    if (status != DatabaseReadStatus::FOUND) return status;
    return !request_hash.IsNull() ? DatabaseReadStatus::FOUND : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ErasePaymasterCapacityNonce(const uint256& client_nonce)
{
    return !client_nonce.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE,
                                  client_nonce));
}

bool WalletBatch::WritePaymasterCapacitySession(const uint256& session_key,
                                                const uint256& request_hash,
                                                bool overwrite)
{
    return !session_key.IsNull() && !request_hash.IsNull() &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION, session_key),
                   request_hash, overwrite);
}

bool WalletBatch::ReadPaymasterCapacitySession(const uint256& session_key,
                                               uint256& request_hash)
{
    return ReadPaymasterCapacitySessionWithStatus(session_key, request_hash) == DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacitySessionWithStatus(
    const uint256& session_key, uint256& request_hash)
{
    request_hash.SetNull();
    if (session_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const auto status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION, session_key), request_hash);
    if (status != DatabaseReadStatus::FOUND) return status;
    return !request_hash.IsNull() ? DatabaseReadStatus::FOUND : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ErasePaymasterCapacitySession(const uint256& session_key)
{
    return !session_key.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION,
                                  session_key));
}

bool WalletBatch::WritePaymasterCapacityRelease(
    const DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release,
    bool overwrite)
{
    return release.version == DigiDollar::Paymaster::ProviderCapacityReleaseRecord::CURRENT_VERSION &&
           !release.request_hash.IsNull() && !release.client_nonce.IsNull() &&
           release.released_at > 0 &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE,
                                  release.request_hash),
                   release, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCapacityReleaseWithStatus(
    const uint256& request_hash,
    DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release)
{
    if (request_hash.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE, request_hash),
        release);
    if (status != DatabaseReadStatus::FOUND) return status;
    return release.request_hash == request_hash &&
                   !release.client_nonce.IsNull() && release.released_at > 0
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCapacityRelease(
    const uint256& request_hash,
    DigiDollar::Paymaster::ProviderCapacityReleaseRecord& release)
{
    return ReadPaymasterCapacityReleaseWithStatus(request_hash, release) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterCapacityRelease(const uint256& request_hash)
{
    return !request_hash.IsNull() &&
           m_batch->Exists(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE,
                                          request_hash));
}

bool WalletBatch::ErasePaymasterCapacityRelease(const uint256& request_hash)
{
    return !request_hash.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE,
                                  request_hash));
}

bool WalletBatch::WritePaymasterTemplate(const uint256& template_commitment,
                                         const uint256& attempt_id,
                                         bool overwrite)
{
    if (template_commitment.IsNull() || attempt_id.IsNull()) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_TEMPLATE, template_commitment), attempt_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterTemplateWithStatus(
    const uint256& template_commitment, uint256& attempt_id)
{
    attempt_id.SetNull();
    if (template_commitment.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_TEMPLATE, template_commitment),
        attempt_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return attempt_id.IsNull() ? DatabaseReadStatus::READ_ERROR
                               : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterTemplate(const uint256& template_commitment,
                                        uint256& attempt_id)
{
    return ReadPaymasterTemplateWithStatus(template_commitment, attempt_id) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterTemplate(const uint256& template_commitment)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_TEMPLATE, template_commitment));
}

bool WalletBatch::WritePaymasterUnsignedTx(const uint256& unsigned_txid,
                                           const uint256& attempt_id,
                                           bool overwrite)
{
    if (unsigned_txid.IsNull() || attempt_id.IsNull()) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, unsigned_txid), attempt_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterUnsignedTxWithStatus(
    const uint256& unsigned_txid, uint256& attempt_id)
{
    attempt_id.SetNull();
    if (unsigned_txid.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, unsigned_txid),
        attempt_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return attempt_id.IsNull() ? DatabaseReadStatus::READ_ERROR
                               : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterUnsignedTx(const uint256& unsigned_txid,
                                          uint256& attempt_id)
{
    return ReadPaymasterUnsignedTxWithStatus(unsigned_txid, attempt_id) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterUnsignedTx(const uint256& unsigned_txid)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, unsigned_txid));
}

bool WalletBatch::WritePaymasterSessionId(const uint256& session_id, const std::string& request_id, bool overwrite)
{
    if (session_id.IsNull() || !DigiDollar::Paymaster::IsCanonicalRequestId(request_id)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_SESSION_ID, session_id), request_id, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSessionIdWithStatus(
    const uint256& session_id, std::string& request_id)
{
    request_id.clear();
    if (session_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_SESSION_ID, session_id), request_id);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::IsCanonicalRequestId(request_id)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterSessionId(const uint256& session_id,
                                         std::string& request_id)
{
    return ReadPaymasterSessionIdWithStatus(session_id, request_id) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterSessionId(const uint256& session_id)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_SESSION_ID, session_id));
}

bool WalletBatch::WritePaymasterReservation(const DigiDollar::Paymaster::InputReservation& reservation, bool overwrite)
{
    if (reservation.version != DigiDollar::Paymaster::InputReservation::CURRENT_VERSION ||
        reservation.outpoint.IsNull() || reservation.session_id.IsNull() ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(reservation.request_id)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RESERVATION, reservation.outpoint), reservation, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterReservationWithStatus(
    const COutPoint& outpoint,
    DigiDollar::Paymaster::InputReservation& reservation)
{
    if (outpoint.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RESERVATION, outpoint), reservation);
    if (status != DatabaseReadStatus::FOUND) return status;
    return reservation.outpoint == outpoint &&
                   !reservation.session_id.IsNull() &&
                   DigiDollar::Paymaster::IsCanonicalRequestId(
                       reservation.request_id)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterReservation(
    const COutPoint& outpoint,
    DigiDollar::Paymaster::InputReservation& reservation)
{
    return ReadPaymasterReservationWithStatus(outpoint, reservation) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterReservation(const COutPoint& outpoint)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RESERVATION, outpoint));
}

bool WalletBatch::WritePaymasterTombstone(const DigiDollar::Paymaster::IdempotencyTombstone& tombstone, bool overwrite)
{
    if (tombstone.version != DigiDollar::Paymaster::IdempotencyTombstone::CURRENT_VERSION ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(tombstone.request_id) || tombstone.session_id.IsNull() ||
        !DigiDollar::Paymaster::IsTerminal(tombstone.final_state)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, tombstone.request_id), tombstone, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterTombstoneWithStatus(
    const std::string& request_id,
    DigiDollar::Paymaster::IdempotencyTombstone& tombstone)
{
    using namespace DigiDollar::Paymaster;
    if (!IsCanonicalRequestId(request_id)) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, request_id), tombstone);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (tombstone.request_id != request_id || tombstone.session_id.IsNull() ||
        !IsTerminal(tombstone.final_state)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterTombstone(
    const std::string& request_id,
    DigiDollar::Paymaster::IdempotencyTombstone& tombstone)
{
    return ReadPaymasterTombstoneWithStatus(request_id, tombstone) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterProviderCommit(const DigiDollar::Paymaster::ProviderCommitRecord& commit, bool overwrite)
{
    if (commit.version != DigiDollar::Paymaster::ProviderCommitRecord::CURRENT_VERSION ||
        commit.commit_key.IsNull() || commit.provider_id.IsNull() || commit.quote_id.IsNull() ||
        commit.template_commitment.IsNull() || commit.final_txid.IsNull() ||
        commit.raw_transaction_hash.IsNull() || commit.final_transaction.empty() ||
        commit.provider_inputs.empty()) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit.commit_key), commit, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderCommitWithStatus(
    const uint256& commit_key,
    DigiDollar::Paymaster::ProviderCommitRecord& commit)
{
    if (commit_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit_key), commit);
    if (status != DatabaseReadStatus::FOUND) return status;
    return commit.commit_key == commit_key && !commit.provider_id.IsNull() &&
                   !commit.quote_id.IsNull() &&
                   !commit.template_commitment.IsNull() &&
                   !commit.final_txid.IsNull() &&
                   !commit.raw_transaction_hash.IsNull() &&
                   !commit.final_transaction.empty() &&
                   !commit.provider_inputs.empty()
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterProviderCommit(
    const uint256& commit_key,
    DigiDollar::Paymaster::ProviderCommitRecord& commit)
{
    return ReadPaymasterProviderCommitWithStatus(commit_key, commit) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterProviderCommit(const uint256& commit_key)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit_key));
}

bool WalletBatch::ListPaymasterProviderCommits(
    std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits)
{
    commits.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_PROVIDER_COMMIT;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            uint256 commit_key;
            DigiDollar::Paymaster::ProviderCommitRecord commit;
            key >> type >> commit_key;
            value >> commit;
            if (type != DBKeys::PAYMASTER_PROVIDER_COMMIT ||
                commit.version != DigiDollar::Paymaster::ProviderCommitRecord::CURRENT_VERSION ||
                commit.commit_key != commit_key || commit.provider_id.IsNull() ||
                commit.quote_id.IsNull() || commit.template_commitment.IsNull() ||
                commit.final_txid.IsNull() || commit.raw_transaction_hash.IsNull() ||
                commit.final_transaction.empty() || commit.provider_inputs.empty()) {
                return false;
            }
            commits.push_back(std::move(commit));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::WritePaymasterUserAuthorization(
    const DigiDollar::Paymaster::UserAuthorizationRecord& authorization,
    bool overwrite)
{
    if (authorization.version != DigiDollar::Paymaster::UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key.IsNull() || authorization.attempt_id.IsNull() ||
        authorization.canonical_psbt_hash.IsNull() || authorization.accepted_at <= 0 ||
        authorization.retry_until < authorization.accepted_at) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_USER_AUTH, authorization.commit_key),
                   authorization, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterUserAuthorizationWithStatus(
    const uint256& commit_key,
    DigiDollar::Paymaster::UserAuthorizationRecord& authorization)
{
    if (commit_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_USER_AUTH, commit_key), authorization);
    if (status != DatabaseReadStatus::FOUND) return status;
    return authorization.commit_key == commit_key &&
                   !authorization.attempt_id.IsNull() &&
                   !authorization.canonical_psbt_hash.IsNull() &&
                   authorization.accepted_at > 0 &&
                   authorization.retry_until >= authorization.accepted_at
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterUserAuthorization(
    const uint256& commit_key,
    DigiDollar::Paymaster::UserAuthorizationRecord& authorization)
{
    return ReadPaymasterUserAuthorizationWithStatus(commit_key, authorization) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterUserAuthorization(const uint256& commit_key)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_USER_AUTH, commit_key));
}

bool WalletBatch::WritePaymasterIdentity(
    const DigiDollar::Paymaster::ProviderIdentityRecord& identity,
    bool overwrite)
{
    if (!DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity)) return false;
    return WritePaymasterIC(DBKeys::PAYMASTER_IDENTITY, identity, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterIdentityWithStatus(
    DigiDollar::Paymaster::ProviderIdentityRecord& identity)
{
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_IDENTITY, identity);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterIdentity(
    DigiDollar::Paymaster::ProviderIdentityRecord& identity)
{
    return ReadPaymasterIdentityWithStatus(identity) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterPolicy(const DigiDollar::Paymaster::ProviderPolicy& policy,
                                       bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateProviderPolicy(policy, error)) return false;
    return WritePaymasterIC(DBKeys::PAYMASTER_POLICY, policy, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterPolicyWithStatus(
    DigiDollar::Paymaster::ProviderPolicy& policy)
{
    policy = DigiDollar::Paymaster::ProviderPolicy{};
    std::string error;
    const DatabaseReadStatus status =
        m_batch->ReadWithStatus(DBKeys::PAYMASTER_POLICY, policy);
    if ((status == DatabaseReadStatus::FOUND ||
         status == DatabaseReadStatus::READ_ERROR) &&
        policy.version != DigiDollar::Paymaster::ProviderPolicy::LEGACY_VERSION &&
        policy.version != DigiDollar::Paymaster::ProviderPolicy::CURRENT_VERSION) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderPolicy(policy, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterPolicy(
    DigiDollar::Paymaster::ProviderPolicy& policy)
{
    return ReadPaymasterPolicyWithStatus(policy) == DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterSettings(const DigiDollar::Paymaster::ProviderSettings& settings,
                                         bool overwrite)
{
    if (settings.version != DigiDollar::Paymaster::ProviderSettings::CURRENT_VERSION ||
        settings.updated_at <= 0 || (settings.enabled && settings.policy_hash.IsNull())) return false;
    return WritePaymasterIC(DBKeys::PAYMASTER_SETTINGS, settings, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSettingsWithStatus(
    DigiDollar::Paymaster::ProviderSettings& settings)
{
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_SETTINGS, settings);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (settings.updated_at <= 0 ||
        (settings.enabled && settings.policy_hash.IsNull()) ||
        static_cast<uint8_t>(settings.operation_mode) >
            static_cast<uint8_t>(
                DigiDollar::Paymaster::ProviderOperationMode::MANUAL)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterSettings(
    DigiDollar::Paymaster::ProviderSettings& settings)
{
    return ReadPaymasterSettingsWithStatus(settings) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterTombstone(const std::string& request_id)
{
    return m_batch->Exists(
        std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, request_id));
}

bool WalletBatch::HasPaymasterSettings()
{
    return m_batch->Exists(DBKeys::PAYMASTER_SETTINGS);
}

bool WalletBatch::WritePaymasterProviderSafetyPolicy(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& safety,
    bool overwrite)
{
    DigiDollar::Paymaster::ProviderPolicy advertised;
    std::string error;
    if (!ReadPaymasterPolicy(advertised) ||
        !DigiDollar::Paymaster::ValidateProviderSafetyPolicy(safety, advertised, error)) {
        return false;
    }
    return WritePaymasterIC(DBKeys::PAYMASTER_PROVIDER_SAFETY, safety, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderSafetyPolicyWithStatus(
    DigiDollar::Paymaster::ProviderSafetyPolicy& safety)
{
    DigiDollar::Paymaster::ProviderPolicy advertised;
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_PROVIDER_SAFETY, safety);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (ReadPaymasterPolicyWithStatus(advertised) !=
            DatabaseReadStatus::FOUND ||
        !DigiDollar::Paymaster::ValidateProviderSafetyPolicy(
            safety, advertised, error)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterProviderSafetyPolicy(
    DigiDollar::Paymaster::ProviderSafetyPolicy& safety)
{
    return ReadPaymasterProviderSafetyPolicyWithStatus(safety) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterProviderSafetyPolicy()
{
    return m_batch->Exists(DBKeys::PAYMASTER_PROVIDER_SAFETY);
}

bool WalletBatch::WritePaymasterClientSafetyPolicy(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateClientSafetyPolicy(policy, error) &&
           WritePaymasterIC(DBKeys::PAYMASTER_CLIENT_SAFETY, policy, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterClientSafetyPolicyWithStatus(
    DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    using DigiDollar::Paymaster::ClientSafetyPolicy;
    // Unlike provider/session records, the complete v1 client policy remains
    // supported: it grants only its existing absolute fee limits. Do not route
    // it through the current-version-only reader or silently add a percent cap.
    policy = {};
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        DBKeys::PAYMASTER_CLIENT_SAFETY, policy);
    if ((status == DatabaseReadStatus::FOUND ||
         status == DatabaseReadStatus::READ_ERROR) &&
        policy.version != 1 && policy.version != ClientSafetyPolicy::CURRENT_VERSION) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    if (status != DatabaseReadStatus::FOUND) return status;
    std::string error;
    return DigiDollar::Paymaster::ValidateClientSafetyPolicy(policy, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterClientSafetyPolicy(
    DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    return ReadPaymasterClientSafetyPolicyWithStatus(policy) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterClientSafetyPolicy()
{
    return m_batch->Exists(DBKeys::PAYMASTER_CLIENT_SAFETY);
}

bool WalletBatch::WritePaymasterProviderBudgetLedger(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateProviderBudgetLedger(ledger, error) &&
           WritePaymasterIC(DBKeys::PAYMASTER_PROVIDER_BUDGET, ledger, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderBudgetLedgerWithStatus(
    DigiDollar::Paymaster::ProviderBudgetLedger& ledger)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_PROVIDER_BUDGET, ledger);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderBudgetLedger(ledger, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterProviderBudgetLedger(
    DigiDollar::Paymaster::ProviderBudgetLedger& ledger)
{
    return ReadPaymasterProviderBudgetLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterProviderBudgetLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_PROVIDER_BUDGET);
}

bool WalletBatch::WritePaymasterClientFeeLedger(
    const DigiDollar::Paymaster::ClientFeeLedger& ledger,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateClientFeeLedger(ledger, error) &&
           WritePaymasterIC(DBKeys::PAYMASTER_CLIENT_FEES, ledger, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterClientFeeLedgerWithStatus(
    DigiDollar::Paymaster::ClientFeeLedger& ledger)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_CLIENT_FEES, ledger);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateClientFeeLedger(ledger, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterClientFeeLedger(
    DigiDollar::Paymaster::ClientFeeLedger& ledger)
{
    return ReadPaymasterClientFeeLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterClientFeeLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_CLIENT_FEES);
}

bool WalletBatch::WritePaymasterSponsorshipAuthorization(
    const DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization,
    bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateSponsorshipAuthorizationRecord(authorization, error)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_SPONSOR_AUTH, authorization.capability_hash),
                   authorization, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSponsorshipAuthorizationWithStatus(
    const uint256& capability_hash,
    DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization)
{
    if (capability_hash.IsNull()) return DatabaseReadStatus::READ_ERROR;
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_SPONSOR_AUTH, capability_hash),
        authorization);
    if (status != DatabaseReadStatus::FOUND) return status;
    return authorization.capability_hash == capability_hash &&
                   DigiDollar::Paymaster::ValidateSponsorshipAuthorizationRecord(
                       authorization, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterSponsorshipAuthorization(
    const uint256& capability_hash,
    DigiDollar::Paymaster::SponsorshipAuthorizationRecord& authorization)
{
    return ReadPaymasterSponsorshipAuthorizationWithStatus(
               capability_hash, authorization) == DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterProviderPool(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    bool overwrite)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    if (!ValidateProviderPoolEntries(entries, error)) return false;
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    {
        // Invalidate before any mutation (including reads inside this batch).
        // Abort/destruction invalidates again, so uncommitted candidates never
        // become the next scan's view of committed history.
        std::lock_guard<std::mutex> lock{session_index_mutex};
        session_indexes[&m_database].pool_history_valid = false;
        auto& pending = pending_session_indexes[this];
        pending.database = &m_database;
        pending.pool_history_changed = true;
    }
    const auto fail = [&] {
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return false;
    };
    std::vector<ProviderPoolEntry> previous;
    const auto status = ReadPaymasterProviderPoolWithStatus(previous);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) return fail();
    const auto history_status = ReadPaymasterPoolHistoryMarker();
    if (history_status != DatabaseReadStatus::FOUND && history_status != DatabaseReadStatus::NOT_FOUND) return fail();
    const auto history_readable = [&](const COutPoint& outpoint) {
        ProviderPoolEntry archived;
        const auto read = ReadPaymasterPoolHistoryWithStatus(outpoint, archived);
        return read == DatabaseReadStatus::FOUND || read == DatabaseReadStatus::NOT_FOUND;
    };
    for (const auto& old : previous) {
        if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.outpoint == old.outpoint; })) continue;
        // A source can become SPENT and be pruned in the same working copy.
        // Retain its protected before-image even when the terminal row was
        // never written separately. History grants no capacity authority.
        if (IsActiveProviderPoolState(old.state) && old.state != PoolEntryState::RESERVED &&
            old.state != PoolEntryState::COMMITTED) continue;
        if (!history_readable(old.outpoint)) return fail();
        if (!WritePaymasterIC(std::make_pair(std::string{"pmpoolhistory"}, old.outpoint), old, true) ||
            !WritePaymasterIC(std::string{"pmpoolhistoryversion"}, uint16_t{1}, true)) return fail();
    }
    // Explicit re-enrolment replaces history only in the same transaction as
    // its new authoritative pool row. Automatic reconstruction reads history.
    for (const auto& entry : entries) {
        if (!history_readable(entry.outpoint)) return fail();
        if (!ErasePaymasterIC(std::make_pair(std::string{"pmpoolhistory"}, entry.outpoint))) return fail();
    }
    if (!WritePaymasterIC(DBKeys::PAYMASTER_PROVIDER_POOL, entries, overwrite)) return fail();
    return !own || TxnCommit();
}

DatabaseReadStatus WalletBatch::ReadPaymasterPoolHistoryMarker()
{
    uint16_t version{0};
    const auto status = m_batch->ReadWithStatus(std::string{"pmpoolhistoryversion"}, version);
    if (status != DatabaseReadStatus::FOUND) return status;
    return version == 1 ? status : DatabaseReadStatus::UNSUPPORTED_VERSION;
}

DatabaseReadStatus WalletBatch::ReadPaymasterPoolHistoryInputs(const CWallet& wallet, std::set<COutPoint>& inputs)
{
    AssertLockHeld(wallet.cs_wallet);
    inputs.clear();
    const auto marker = ReadPaymasterPoolHistoryMarker();
    if (marker != DatabaseReadStatus::FOUND) return marker;
    std::set<COutPoint> candidates;
    bool rebuild;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock{session_index_mutex};
        const auto& index = session_indexes[&m_database];
        rebuild = !index.pool_history_valid;
        revision = index.revision;
        if (!rebuild) candidates = index.pool_history_candidates;
    }
    const auto fail = [&] {
        std::lock_guard<std::mutex> lock{session_index_mutex};
        session_indexes[&m_database].pool_history_valid = false;
        inputs.clear();
        return DatabaseReadStatus::READ_ERROR;
    };
    if (rebuild) {
        DataStream prefix;
        prefix << std::string{"pmpoolhistory"};
        auto cursor = m_batch->GetNewPrefixCursor(prefix);
        if (!cursor) return fail();
        while (true) {
            DataStream key, value;
            const auto status = cursor->Next(key, value);
            if (status == DatabaseCursor::Status::DONE) break;
            if (status != DatabaseCursor::Status::MORE) return fail();
            try {
                std::string type;
                COutPoint outpoint;
                key >> type >> outpoint;
                DigiDollar::Paymaster::ProviderPoolEntry entry;
                if (!key.empty() || type != "pmpoolhistory" ||
                    ReadPaymasterPoolHistoryWithStatus(outpoint, entry) != DatabaseReadStatus::FOUND) return fail();
                candidates.insert(outpoint);
            } catch (const std::ios_base::failure&) {
                return fail();
            }
        }
        std::lock_guard<std::mutex> lock{session_index_mutex};
        auto& index = session_indexes[&m_database];
        if (index.revision != revision) return DatabaseReadStatus::READ_ERROR;
        index.pool_history_candidates = candidates;
        index.pool_history_valid = true;
    }
    // The index selects candidates only. A reorg can make an old source live
    // without any pool write; determine that from the current wallet and read
    // each relevant authoritative disposition again before allowing selection.
    for (const auto& outpoint : candidates) {
        const auto* transaction = wallet.GetWalletTx(outpoint.hash);
        if (!transaction || outpoint.n >= transaction->tx->vout.size() || wallet.IsSpent(outpoint)) continue;
        DigiDollar::Paymaster::ProviderPoolEntry entry;
        if (ReadPaymasterPoolHistoryWithStatus(outpoint, entry) != DatabaseReadStatus::FOUND) return fail();
        if (entry.state != DigiDollar::Paymaster::PoolEntryState::RELEASED) inputs.insert(outpoint);
    }
    return DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ReadPaymasterPoolHistoryWithStatus(
    const COutPoint& outpoint, DigiDollar::Paymaster::ProviderPoolEntry& entry)
{
    using namespace DigiDollar::Paymaster;
    const auto marker_status = ReadPaymasterPoolHistoryMarker();
    if (marker_status != DatabaseReadStatus::FOUND && marker_status != DatabaseReadStatus::NOT_FOUND) return marker_status;
    const auto status = ReadPaymasterVersionedRecord(std::make_pair(std::string{"pmpoolhistory"}, outpoint), entry);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (marker_status != DatabaseReadStatus::FOUND) return DatabaseReadStatus::READ_ERROR;
    std::string error;
    if (entry.outpoint != outpoint || entry.state == PoolEntryState::AVAILABLE ||
        entry.state == PoolEntryState::PENDING_SUCCESSOR ||
        !ValidateProviderPoolEntries({entry}, error)) return DatabaseReadStatus::READ_ERROR;
    return status;
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderPoolWithStatus(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries)
{
    using namespace DigiDollar::Paymaster;
    entries.clear();
    std::string error;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        DBKeys::PAYMASTER_PROVIDER_POOL, entries);
    const auto outdated = std::find_if(
        entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
            return entry.version != ProviderPoolEntry::CURRENT_VERSION;
        });
    if (outdated != entries.end()) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    if (status != DatabaseReadStatus::FOUND) return status;
    return ValidateProviderPoolEntries(entries, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterProviderPool(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries)
{
    return ReadPaymasterProviderPoolWithStatus(entries) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterProviderPool()
{
    return m_batch->Exists(DBKeys::PAYMASTER_PROVIDER_POOL);
}

bool WalletBatch::WritePaymasterLiquidityPolicy(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidateProviderLiquidityPolicy(policy, error)) {
        return false;
    }
    return WritePaymasterIC(DBKeys::PAYMASTER_LIQUIDITY_POLICY, policy, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterLiquidityPolicyWithStatus(
    DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_LIQUIDITY_POLICY, policy);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderLiquidityPolicy(policy,
                                                                   error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterLiquidityPolicy(
    DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    return ReadPaymasterLiquidityPolicyWithStatus(policy) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterLiquidityPolicy()
{
    return m_batch->Exists(DBKeys::PAYMASTER_LIQUIDITY_POLICY);
}

bool WalletBatch::WritePaymasterMaintenanceLedger(
    const DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger,
    bool overwrite)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    if (!ValidateProviderMaintenanceLedger(ledger, error)) return false;
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    const auto fail = [&] {
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return false;
    };
    ProviderSetupAuthorization authorization;
    const auto setup_status = ReadPaymasterSetupAuthorizationWithStatus(authorization);
    if (setup_status != DatabaseReadStatus::FOUND && setup_status != DatabaseReadStatus::NOT_FOUND) return fail();
    const bool previously_revoked = authorization.revoked;
    ProviderMaintenanceLedger previous;
    const auto previous_status = m_batch->ReadWithStatus(DBKeys::PAYMASTER_MAINTENANCE_LEDGER, previous);
    if ((previous_status != DatabaseReadStatus::FOUND && previous_status != DatabaseReadStatus::NOT_FOUND) ||
        (previous_status == DatabaseReadStatus::FOUND && !ValidateProviderMaintenanceLedger(previous, error))) return fail();
    if (setup_status == DatabaseReadStatus::NOT_FOUND) {
        if (previous_status == DatabaseReadStatus::FOUND && previous.version != 1) return fail();
        // The permanent fence and V2 journal always commit together. Missing
        // V2 authority must never be mistaken for an unused generation.
        authorization.genesis_hash = Params().GenesisBlock().GetHash();
    }
    if (std::any_of(ledger.records.begin(), ledger.records.end(), [&](const auto& record) {
            return record.IsPreparation() && record.plan_id == authorization.plan_id &&
                   record.state == ProviderMaintenanceState::RELEASED;
        })) {
        authorization.revoked = true;
    }
    ProviderMaintenanceLedger current{ledger};
    current.version = ProviderMaintenanceLedger::CURRENT_VERSION;
    const bool write_authorization = setup_status == DatabaseReadStatus::NOT_FOUND || authorization.revoked != previously_revoked;
    if ((write_authorization && !WritePaymasterSetupAuthorization(authorization)) ||
        !WritePaymasterIC(DBKeys::PAYMASTER_MAINTENANCE_LEDGER, current, overwrite)) return fail();
    if (!own || TxnCommit()) return true;
    TxnAbort();
    return false;
}

DatabaseReadStatus WalletBatch::ReadPaymasterMaintenanceLedgerWithStatus(
    DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    ledger = {};
    const auto status = m_batch->ReadWithStatus(DBKeys::PAYMASTER_MAINTENANCE_LEDGER, ledger);
    if ((status == DatabaseReadStatus::FOUND || status == DatabaseReadStatus::READ_ERROR) &&
        ledger.version != 1 && ledger.version != ProviderMaintenanceLedger::CURRENT_VERSION) {
        return DatabaseReadStatus::UNSUPPORTED_VERSION;
    }
    if (status != DatabaseReadStatus::FOUND) return status;
    ProviderSetupAuthorization authorization;
    const auto setup_status = ReadPaymasterSetupAuthorizationWithStatus(authorization);
    if (setup_status != DatabaseReadStatus::FOUND &&
        (setup_status != DatabaseReadStatus::NOT_FOUND || ledger.version != 1)) {
        return setup_status == DatabaseReadStatus::NOT_FOUND ? DatabaseReadStatus::READ_ERROR : setup_status;
    }
    return ValidateProviderMaintenanceLedger(ledger, error) ? DatabaseReadStatus::FOUND : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::WritePaymasterSetupAuthorization(
    const DigiDollar::Paymaster::ProviderSetupAuthorization& authorization)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    if (!ValidateProviderSetupAuthorization(authorization, error) ||
        authorization.genesis_hash != Params().GenesisBlock().GetHash()) return false;
    ProviderIdentityRecord identity;
    if (authorization.generation != 0 &&
        (ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND ||
         identity.provider_id != authorization.provider_id)) return false;
    ProviderSetupAuthorization previous;
    const auto status = ReadPaymasterSetupAuthorizationWithStatus(previous);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) return false;
    if (status == DatabaseReadStatus::NOT_FOUND && authorization.generation > 1) return false;
    if (status == DatabaseReadStatus::FOUND &&
        (authorization.generation < previous.generation ||
         authorization.generation - previous.generation > 1 ||
         (previous.generation != 0 && authorization.provider_id != previous.provider_id) ||
         (authorization.generation == previous.generation &&
          (authorization.provider_id != previous.provider_id || authorization.plan_id != previous.plan_id ||
           (previous.revoked && !authorization.revoked))))) return false;
    return WritePaymasterIC(DBKeys::PAYMASTER_SETUP_AUTHORIZATION, authorization);
}

DatabaseReadStatus WalletBatch::ReadPaymasterSetupAuthorizationWithStatus(
    DigiDollar::Paymaster::ProviderSetupAuthorization& authorization)
{
    std::string error;
    const auto status = ReadPaymasterVersionedRecord(DBKeys::PAYMASTER_SETUP_AUTHORIZATION, authorization);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (!DigiDollar::Paymaster::ValidateProviderSetupAuthorization(authorization, error) ||
        authorization.genesis_hash != Params().GenesisBlock().GetHash()) return DatabaseReadStatus::READ_ERROR;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    if (authorization.generation != 0 &&
        (ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND ||
         identity.provider_id != authorization.provider_id)) return DatabaseReadStatus::READ_ERROR;
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterMaintenanceLedger(
    DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger)
{
    return ReadPaymasterMaintenanceLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterMaintenanceLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_MAINTENANCE_LEDGER);
}

namespace {
using namespace DigiDollar::Paymaster;
const std::string FINANCE_EVENT{"pmfinevent"};
const std::string FINANCE_TIME{"pmfintime"};
const std::string FINANCE_TX{"pmfintx"};
const std::string FINANCE_DAY{"pmfinday"};
using FinanceOrder = std::array<unsigned char, 40>;
using FinanceDay = std::array<unsigned char, 8>;

template <typename T> struct StrictFinanceRecord : T {
    template <typename Stream> void Unserialize(Stream& stream)
    {
        stream >> static_cast<T&>(*this);
        if (!stream.empty()) throw std::ios_base::failure("finance record trailing bytes");
    }
};

struct FinanceVersion {
    uint16_t version{0};
    SERIALIZE_METHODS(FinanceVersion, obj) { READWRITE(obj.version); }
};

int64_t FinanceEventTime(const ProviderFinanceEvent& event)
{
    return event.state == ProviderFinanceEventState::CONFIRMED ? event.confirmed_at : event.created_at;
}

FinanceOrder FinanceTimeKey(const ProviderFinanceEvent& event)
{
    FinanceOrder key;
    WriteBE64(key.data(), ~uint64_t(FinanceEventTime(event)));
    // uint256 ordering compares its stored bytes. Complement them for the
    // same descending tie-break without relying on little-endian integers.
    for (size_t i = 0; i < 32; ++i) key[8 + i] = ~event.event_id.begin()[i];
    return key;
}

FinanceDay FinanceDayKey(int64_t day)
{
    FinanceDay key;
    WriteBE64(key.data(), uint64_t(day));
    return key;
}

DataStream FinancePrefix(const std::string& type)
{
    DataStream prefix;
    prefix << type;
    return prefix;
}

bool ValidFinanceTotals(const ProviderFinanceTotals& totals)
{
    ProviderFinanceTotals checked;
    std::string error;
    if (totals.total.version != ProviderFinanceDailyTotals::CURRENT_VERSION ||
        totals.total.day_start < 0 || !AddProviderFinanceTotals(checked, totals, error)) return false;
    const uint64_t transfers = uint64_t(totals.total.user_paid_transfers) +
        totals.total.public_sponsored_transfers + totals.total.restricted_sponsored_transfers;
    if (transfers != totals.total.successful_transfers) return false;
    int64_t income{0}, cost{0};
    for (size_t i = 0; i < totals.income.size(); ++i) {
        if (totals.income[i] > totals.total.dd_income.value - income ||
            totals.cost[i] > totals.total.dgb_cost.value - cost) return false;
        income += totals.income[i];
        cost += totals.cost[i];
    }
    return true;
}

bool EmptyFinanceTotals(const ProviderFinanceTotals& totals)
{
    auto copy = totals;
    copy.total.day_start = 0;
    return copy == ProviderFinanceTotals{};
}

struct FinanceCursor {
    uint16_t version{1};
    uint256 genesis;
    PaymasterId provider;
    uint8_t period{0};
    int64_t cutoff{0};
    uint64_t revision{0};
    FinanceOrder position{};
    SERIALIZE_METHODS(FinanceCursor, obj)
    {
        READWRITE(obj.version, obj.genesis, obj.provider, obj.period,
                  obj.cutoff, obj.revision, obj.position);
    }
};
} // namespace

DatabaseReadStatus WalletBatch::ReadPaymasterFinanceMetadata(ProviderFinanceMetadata& metadata)
{
    metadata = {};
    FinanceVersion version;
    auto status = m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, version);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (version.version != ProviderFinanceMetadata::CURRENT_VERSION) return DatabaseReadStatus::UNSUPPORTED_VERSION;
    StrictFinanceRecord<ProviderFinanceMetadata> record;
    status = m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, record);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (record.genesis_hash.IsNull() || record.provider_id.IsNull() ||
        record.history_complete_from <= 0 || record.updated_at < record.history_complete_from ||
        record.revision == 0 || record.event_count > MAX_PROVIDER_FINANCE_EVENTS ||
        record.totals.total.day_start != 0 || !ValidFinanceTotals(record.totals) ||
        uint64_t(record.totals.total.successful_transfers) + record.totals.total.maintenance_transactions > record.event_count) {
        return DatabaseReadStatus::READ_ERROR;
    }
    metadata = record;
    return DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ReadPaymasterFinanceEvent(const uint256& event_id,
                                                         ProviderFinanceEvent& event)
{
    event = {};
    StrictFinanceRecord<ProviderFinanceEvent> record;
    auto status = m_batch->ReadWithStatus(std::make_pair(FINANCE_EVENT, event_id), record);
    if (status != DatabaseReadStatus::FOUND) return status;
    std::string error;
    if (record.event_id != event_id || !ValidateProviderFinanceEvent(record, error)) return DatabaseReadStatus::READ_ERROR;
    event = record;
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::MigratePaymasterFinanceLedger(std::string& error)
{
    error.clear();
    FinanceVersion version;
    const auto status = m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, version);
    if (status == DatabaseReadStatus::NOT_FOUND) return true;
    if (status != DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (version.version == ProviderFinanceMetadata::CURRENT_VERSION) {
        ProviderFinanceMetadata metadata;
        if (ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND) return true;
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (version.version != ProviderFinanceLedger::CURRENT_VERSION) {
        error = "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION";
        return false;
    }
    StrictFinanceRecord<ProviderFinanceLedger> legacy;
    if (m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, legacy) != DatabaseReadStatus::FOUND ||
        !ValidateProviderFinanceLedger(legacy, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (!WritePaymasterFinanceLedger(legacy)) {
        error = "PAYMASTER_FINANCE_MIGRATION_FAILED";
        return false;
    }
    return true;
}

bool WalletBatch::WritePaymasterFinanceEvents(const ProviderFinanceMetadata& initial,
                                             const std::vector<ProviderFinanceEvent>& events,
                                             std::string& error, bool queue_reconciliation)
{
    if (!MigratePaymasterFinanceLedger(error)) return false;
    ProviderFinanceMetadata metadata;
    const auto status = ReadPaymasterFinanceMetadata(metadata);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    const bool create = status == DatabaseReadStatus::NOT_FOUND;
    if (create) {
        metadata = initial;
        metadata.version = ProviderFinanceMetadata::CURRENT_VERSION;
        metadata.revision = 0;
        metadata.event_count = 0;
        metadata.totals = {};
    }
    if (metadata.genesis_hash.IsNull() || metadata.provider_id.IsNull() ||
        metadata.genesis_hash != initial.genesis_hash || metadata.provider_id != initial.provider_id ||
        metadata.history_complete_from <= 0 || metadata.updated_at < metadata.history_complete_from ||
        metadata.revision == std::numeric_limits<uint64_t>::max()) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) { error = "PAYMASTER_DATABASE_BEGIN"; return false; }
    const auto fail = [&](const char* fallback) {
        if (error.empty()) error = fallback;
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return false;
    };
    bool changed = create || (initial.earlier_history_partial && !metadata.earlier_history_partial);
    metadata.earlier_history_partial |= initial.earlier_history_partial;
    if (changed) metadata.updated_at = std::max(metadata.updated_at, initial.updated_at);
    for (const auto& event : events) {
        if (!ValidateProviderFinanceEvent(event, error) || event.genesis_hash != metadata.genesis_hash ||
            event.provider_id != metadata.provider_id) return fail("PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH");
        ProviderFinanceEvent old;
        const auto old_status = ReadPaymasterFinanceEvent(event.event_id, old);
        if (old_status != DatabaseReadStatus::FOUND && old_status != DatabaseReadStatus::NOT_FOUND) {
            return fail("PAYMASTER_INVALID_FINANCE_EVENT");
        }
        const bool exists = old_status == DatabaseReadStatus::FOUND;
        if (exists) {
            if (!CheckProviderFinanceEventBinding(old, event, error)) return fail("PAYMASTER_FINANCE_EVENT_CONFLICT");
            if (event.updated_at < old.updated_at) continue;
            if (event.updated_at == old.updated_at) {
                if (event.state != old.state || event.confirmed_at != old.confirmed_at) {
                    return fail("PAYMASTER_FINANCE_EVENT_STATE_CONFLICT");
                }
                continue;
            }
        } else if (metadata.event_count >= MAX_PROVIDER_FINANCE_EVENTS) {
            return fail("PAYMASTER_FINANCE_LEDGER_FULL");
        }
        const auto contribute = [&](const ProviderFinanceEvent& contribution, int sign) {
            if (!ApplyProviderFinanceContribution(metadata.totals, contribution, sign, error)) return false;
            if (contribution.state != ProviderFinanceEventState::CONFIRMED) return true;
            const int64_t day = (contribution.confirmed_at / PROVIDER_BUDGET_WINDOW_SECONDS) * PROVIDER_BUDGET_WINDOW_SECONDS;
            const auto key = std::make_pair(FINANCE_DAY, FinanceDayKey(day));
            StrictFinanceRecord<ProviderFinanceTotals> daily;
            const auto read = m_batch->ReadWithStatus(key, daily);
            if (read != DatabaseReadStatus::FOUND && read != DatabaseReadStatus::NOT_FOUND) return false;
            if (read == DatabaseReadStatus::FOUND &&
                (daily.total.day_start != day || !ValidFinanceTotals(daily))) return false;
            daily.total.day_start = day;
            if (!ApplyProviderFinanceContribution(daily, contribution, sign, error)) return false;
            if (EmptyFinanceTotals(daily)) return ErasePaymasterIC(key);
            return WritePaymasterIC(key, static_cast<const ProviderFinanceTotals&>(daily));
        };
        if ((exists && !contribute(old, -1)) || !contribute(event, 1)) {
            return fail("PAYMASTER_INVALID_FINANCE_TOTALS");
        }
        if (exists && !ErasePaymasterIC(std::make_pair(FINANCE_TIME, FinanceTimeKey(old)))) {
            return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
        }
        if (!WritePaymasterIC(std::make_pair(FINANCE_EVENT, event.event_id), event) ||
            !WritePaymasterIC(std::make_pair(FINANCE_TIME, FinanceTimeKey(event)), event.event_id) ||
            !WritePaymasterIC(std::make_pair(FINANCE_TX, std::make_pair(event.transaction_id, event.event_id)), event.event_id)) {
            return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
        }
        if (!exists) ++metadata.event_count;
        metadata.updated_at = std::max(metadata.updated_at, event.updated_at);
        QueuePaymasterFinanceEvent(this, m_database, event, exists ? &old : nullptr, queue_reconciliation);
        changed = true;
    }
    if (changed) {
        ++metadata.revision;
        if (!WritePaymasterIC(DBKeys::PAYMASTER_FINANCE_LEDGER, metadata)) return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
    }
    if (own && !TxnCommit()) return fail("PAYMASTER_DATABASE_COMMIT");
    return true;
}

bool WalletBatch::WritePaymasterFinanceLedger(
    const DigiDollar::Paymaster::ProviderFinanceLedger& ledger,
    bool overwrite)
{
    std::string error;
    if (!ValidateProviderFinanceLedger(ledger, error) ||
        (!overwrite && HasPaymasterFinanceLedger())) return false;
    ProviderFinanceMetadata metadata;
    FinanceVersion version;
    const auto previous = m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, version);
    if (previous != DatabaseReadStatus::FOUND && previous != DatabaseReadStatus::NOT_FOUND) return false;
    if (previous == DatabaseReadStatus::FOUND) {
        if (version.version == ProviderFinanceMetadata::CURRENT_VERSION) {
            if (ReadPaymasterFinanceMetadata(metadata) != DatabaseReadStatus::FOUND ||
                metadata.genesis_hash != ledger.genesis_hash || metadata.provider_id != ledger.provider_id ||
                metadata.revision == std::numeric_limits<uint64_t>::max()) return false;
        } else if (version.version != ProviderFinanceLedger::CURRENT_VERSION) {
            return false;
        }
    }
    metadata.genesis_hash = ledger.genesis_hash;
    metadata.provider_id = ledger.provider_id;
    metadata.history_complete_from = ledger.history_complete_from;
    metadata.earlier_history_partial = ledger.earlier_history_partial;
    metadata.updated_at = ledger.updated_at;
    metadata.event_count = ledger.events.size();
    metadata.totals = {};
    ++metadata.revision;
    std::map<int64_t, ProviderFinanceTotals> days;
    for (const auto& event : ledger.events) {
        if (!ApplyProviderFinanceContribution(metadata.totals, event, 1, error)) return false;
        if (event.state == ProviderFinanceEventState::CONFIRMED) {
            const int64_t day = (event.confirmed_at / PROVIDER_BUDGET_WINDOW_SECONDS) * PROVIDER_BUDGET_WINDOW_SECONDS;
            auto& totals = days[day];
            totals.total.day_start = day;
            if (!ApplyProviderFinanceContribution(totals, event, 1, error)) return false;
        }
    }
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    const auto fail = [&] {
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return false;
    };
    // Snapshot replacement is reserved for migration/rebuild and legacy
    // internal callers. Normal event updates use WritePaymasterFinanceEvents.
    if (!PaymasterCheckpointWrite(this, *m_batch, m_database)) return fail();
    for (const auto& prefix : {FINANCE_EVENT, FINANCE_TIME, FINANCE_TX, FINANCE_DAY}) {
        if (!m_batch->ErasePrefix(FinancePrefix(prefix))) return fail();
    }
    for (const auto& event : ledger.events) {
        if (!WritePaymasterIC(std::make_pair(FINANCE_EVENT, event.event_id), event) ||
            !WritePaymasterIC(std::make_pair(FINANCE_TIME, FinanceTimeKey(event)), event.event_id) ||
            !WritePaymasterIC(std::make_pair(FINANCE_TX, std::make_pair(event.transaction_id, event.event_id)), event.event_id)) return fail();
    }
    for (const auto& [day, totals] : days) {
        if (!WritePaymasterIC(std::make_pair(FINANCE_DAY, FinanceDayKey(day)), totals)) return fail();
    }
    if (!WritePaymasterIC(DBKeys::PAYMASTER_FINANCE_LEDGER, metadata)) return fail();
    QueuePaymasterFinanceRebuild(this, m_database);
    if (own && !TxnCommit()) return fail();
    return true;
}

bool WalletBatch::RepairPaymasterFinanceDerivations(
    const uint256& genesis, const uint256& provider, ProviderFinanceLedger& ledger, std::string& error)
{
    error.clear();
    ledger = {};
    const auto invalid = [&](const char* code) { ledger = {}; error = code; return false; };
    FinanceVersion version;
    const auto header = m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, version);
    if (header == DatabaseReadStatus::NOT_FOUND) return true;
    if (header != DatabaseReadStatus::FOUND) return invalid("PAYMASTER_INVALID_FINANCE_LEDGER");
    if (version.version == ProviderFinanceLedger::CURRENT_VERSION) {
        if (!MigratePaymasterFinanceLedger(error)) return false;
    } else if (version.version != ProviderFinanceMetadata::CURRENT_VERSION) {
        return invalid("PAYMASTER_UNSUPPORTED_PERSISTED_VERSION");
    }
    // Identity, provenance, version and source completeness remain mandatory.
    // The totals are derived and deliberately not trusted as read prerequisites.
    StrictFinanceRecord<ProviderFinanceMetadata> metadata;
    if (m_batch->ReadWithStatus(DBKeys::PAYMASTER_FINANCE_LEDGER, metadata) != DatabaseReadStatus::FOUND ||
        metadata.version != ProviderFinanceMetadata::CURRENT_VERSION ||
        metadata.genesis_hash != genesis || metadata.provider_id != provider || genesis.IsNull() || provider.IsNull() ||
        metadata.history_complete_from <= 0 || metadata.updated_at < metadata.history_complete_from ||
        metadata.revision == 0 || metadata.event_count > MAX_PROVIDER_FINANCE_EVENTS ||
        metadata.totals.total.version != ProviderFinanceDailyTotals::CURRENT_VERSION) {
        return invalid("PAYMASTER_INVALID_FINANCE_LEDGER");
    }
    ledger.genesis_hash = genesis;
    ledger.provider_id = provider;
    ledger.history_complete_from = metadata.history_complete_from;
    ledger.earlier_history_partial = metadata.earlier_history_partial;
    ledger.updated_at = metadata.updated_at;
    ProviderFinanceTotals totals;
    std::map<int64_t, ProviderFinanceTotals> days;
    auto cursor = m_batch->GetNewPrefixCursor(FinancePrefix(FINANCE_EVENT));
    if (!cursor) return invalid("PAYMASTER_FINANCE_READ_FAILED");
    while (true) {
        DataStream key, value;
        const auto next = cursor->Next(key, value);
        if (next == DatabaseCursor::Status::DONE) break;
        if (next != DatabaseCursor::Status::MORE || ledger.events.size() >= metadata.event_count) {
            return invalid("PAYMASTER_INVALID_FINANCE_EVENT");
        }
        try {
            std::string type;
            uint256 id;
            ProviderFinanceEvent event;
            key >> type >> id;
            value >> event;
            if (!key.empty() || !value.empty() || type != FINANCE_EVENT || event.event_id != id ||
                event.genesis_hash != genesis || event.provider_id != provider || event.updated_at > metadata.updated_at ||
                !ApplyProviderFinanceContribution(totals, event, 1, error)) {
                return invalid("PAYMASTER_INVALID_FINANCE_EVENT");
            }
            if (event.state == ProviderFinanceEventState::CONFIRMED) {
                const int64_t day = (event.confirmed_at / PROVIDER_BUDGET_WINDOW_SECONDS) * PROVIDER_BUDGET_WINDOW_SECONDS;
                auto& daily = days[day];
                daily.total.day_start = day;
                if (!ApplyProviderFinanceContribution(daily, event, 1, error)) return invalid("PAYMASTER_INVALID_FINANCE_EVENT");
            }
            ledger.events.push_back(std::move(event));
        } catch (const std::exception&) { return invalid("PAYMASTER_INVALID_FINANCE_EVENT"); }
    }
    cursor.reset();
    if (ledger.events.size() != metadata.event_count) return invalid("PAYMASTER_INVALID_FINANCE_EVENT");
    for (const auto& [day, daily] : days) ledger.daily_totals.push_back(daily.total);

    using EncodedRecords = std::map<SerializeData, SerializeData>;
    EncodedRecords expected;
    const auto add = [&](const auto& key, const auto& value) {
        DataStream encoded_key, encoded_value;
        encoded_key << key;
        encoded_value << value;
        expected.emplace(SerializeData{encoded_key.begin(), encoded_key.end()},
                         SerializeData{encoded_value.begin(), encoded_value.end()});
    };
    for (const auto& event : ledger.events) {
        add(std::make_pair(FINANCE_TIME, FinanceTimeKey(event)), event.event_id);
        add(std::make_pair(FINANCE_TX, std::make_pair(event.transaction_id, event.event_id)), event.event_id);
    }
    for (const auto& [day, daily] : days) add(std::make_pair(FINANCE_DAY, FinanceDayKey(day)), daily);
    bool repair = !(metadata.totals == totals);
    auto& remaining = expected;
    for (const auto& prefix : {FINANCE_TIME, FINANCE_TX, FINANCE_DAY}) {
        cursor = m_batch->GetNewPrefixCursor(FinancePrefix(prefix));
        if (!cursor) return invalid("PAYMASTER_FINANCE_READ_FAILED");
        while (true) {
            DataStream key, value;
            const auto next = cursor->Next(key, value);
            if (next == DatabaseCursor::Status::DONE) break;
            if (next != DatabaseCursor::Status::MORE) return invalid("PAYMASTER_FINANCE_READ_FAILED");
            if (prefix == FINANCE_DAY && value.size() >= sizeof(uint16_t)) {
                DataStream encoded;
                encoded.write(value);
                FinanceVersion daily;
                encoded >> daily;
                if (daily.version != ProviderFinanceDailyTotals::CURRENT_VERSION) {
                    return invalid("PAYMASTER_UNSUPPORTED_PERSISTED_VERSION");
                }
            }
            const auto found = remaining.find(SerializeData{key.begin(), key.end()});
            if (found == remaining.end() || found->second != SerializeData{value.begin(), value.end()}) repair = true;
            if (found != remaining.end()) remaining.erase(found);
        }
        cursor.reset();
    }
    repair |= !remaining.empty();
    if (!repair) return true;
    if (metadata.revision == std::numeric_limits<uint64_t>::max()) return invalid("PAYMASTER_FINANCE_REVISION_EXHAUSTED");
    metadata.totals = totals;
    ++metadata.revision;
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return invalid("PAYMASTER_DATABASE_BEGIN");
    const auto fail = [&](const char* code) {
        PaymasterCheckpointFailed(this);
        if (own) TxnAbort();
        return invalid(code);
    };
    if (!PaymasterCheckpointWrite(this, *m_batch, m_database)) return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
    for (const auto& prefix : {FINANCE_TIME, FINANCE_TX, FINANCE_DAY}) {
        if (!m_batch->ErasePrefix(FinancePrefix(prefix))) return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
    }
    // Preserve every immutable event byte. Only disposable derivations change.
    for (const auto& event : ledger.events) {
        if (!WritePaymasterIC(std::make_pair(FINANCE_TIME, FinanceTimeKey(event)), event.event_id) ||
            !WritePaymasterIC(std::make_pair(FINANCE_TX, std::make_pair(event.transaction_id, event.event_id)), event.event_id)) {
            return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
        }
    }
    for (const auto& [day, daily] : days) {
        if (!WritePaymasterIC(std::make_pair(FINANCE_DAY, FinanceDayKey(day)), daily)) return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
    }
    if (!WritePaymasterIC(DBKeys::PAYMASTER_FINANCE_LEDGER, static_cast<const ProviderFinanceMetadata&>(metadata))) {
        return fail("PAYMASTER_FINANCE_DATABASE_WRITE");
    }
    QueuePaymasterFinanceRebuild(this, m_database);
    if (own && !TxnCommit()) return fail("PAYMASTER_DATABASE_COMMIT");
    return true;
}

DatabaseReadStatus WalletBatch::ReadPaymasterFinanceLedgerWithStatus(
    DigiDollar::Paymaster::ProviderFinanceLedger& ledger)
{
    std::string error;
    ledger = {};
    if (!MigratePaymasterFinanceLedger(error)) {
        return error == "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" ?
            DatabaseReadStatus::UNSUPPORTED_VERSION : DatabaseReadStatus::READ_ERROR;
    }
    ProviderFinanceMetadata metadata;
    const auto status = ReadPaymasterFinanceMetadata(metadata);
    if (status != DatabaseReadStatus::FOUND) return status;
    ledger.genesis_hash = metadata.genesis_hash;
    ledger.provider_id = metadata.provider_id;
    ledger.history_complete_from = metadata.history_complete_from;
    ledger.earlier_history_partial = metadata.earlier_history_partial;
    ledger.updated_at = metadata.updated_at;
    auto cursor = m_batch->GetNewPrefixCursor(FinancePrefix(FINANCE_EVENT));
    if (!cursor) return DatabaseReadStatus::READ_ERROR;
    ProviderFinanceTotals all;
    std::map<int64_t, ProviderFinanceTotals> days;
    while (true) {
        DataStream key, value;
        const auto next = cursor->Next(key, value);
        if (next == DatabaseCursor::Status::DONE) break;
        if (next != DatabaseCursor::Status::MORE || ledger.events.size() >= metadata.event_count) return DatabaseReadStatus::READ_ERROR;
        try {
            std::string type;
            uint256 id;
            ProviderFinanceEvent event;
            key >> type >> id;
            value >> event;
            if (!key.empty() || !value.empty() || type != FINANCE_EVENT || event.event_id != id ||
                event.genesis_hash != metadata.genesis_hash || event.provider_id != metadata.provider_id ||
                !ApplyProviderFinanceContribution(all, event, 1, error)) return DatabaseReadStatus::READ_ERROR;
            if (event.state == ProviderFinanceEventState::CONFIRMED) {
                const int64_t day = (event.confirmed_at / PROVIDER_BUDGET_WINDOW_SECONDS) * PROVIDER_BUDGET_WINDOW_SECONDS;
                auto& totals = days[day];
                totals.total.day_start = day;
                if (!ApplyProviderFinanceContribution(totals, event, 1, error)) return DatabaseReadStatus::READ_ERROR;
            }
            ledger.events.push_back(std::move(event));
        } catch (const std::exception&) { return DatabaseReadStatus::READ_ERROR; }
    }
    if (ledger.events.size() != metadata.event_count || !(all == metadata.totals)) return DatabaseReadStatus::READ_ERROR;
    cursor = m_batch->GetNewPrefixCursor(FinancePrefix(FINANCE_DAY));
    if (!cursor) return DatabaseReadStatus::READ_ERROR;
    while (true) {
        DataStream key, value;
        const auto next = cursor->Next(key, value);
        if (next == DatabaseCursor::Status::DONE) break;
        if (next != DatabaseCursor::Status::MORE) return DatabaseReadStatus::READ_ERROR;
        try {
            std::string type;
            FinanceDay day_key;
            ProviderFinanceTotals totals;
            key >> type >> day_key;
            value >> totals;
            const auto expected = days.find(totals.total.day_start);
            if (!key.empty() || !value.empty() || type != FINANCE_DAY ||
                day_key != FinanceDayKey(totals.total.day_start) || expected == days.end() ||
                !(totals == expected->second)) return DatabaseReadStatus::READ_ERROR;
            ledger.daily_totals.push_back(totals.total);
            days.erase(expected);
        } catch (const std::exception&) { return DatabaseReadStatus::READ_ERROR; }
    }
    if (!days.empty()) return DatabaseReadStatus::READ_ERROR;
    return ValidateProviderFinanceLedger(ledger, error) ? DatabaseReadStatus::FOUND : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterFinanceTransactionEvents(const uint256& txid,
                                                        std::vector<ProviderFinanceEvent>& events,
                                                        size_t limit, const uint256& after, bool* has_more)
{
    events.clear();
    if (has_more) *has_more = false;
    if (txid.IsNull() || limit == 0 || limit > MAX_PROVIDER_FINANCE_EVENTS) return false;
    DataStream prefix;
    prefix << FINANCE_TX << txid;
    SerializeData end{prefix.begin(), prefix.end()};
    while (!end.empty() && end.back() == std::byte{0xff}) end.pop_back();
    if (!end.empty()) end.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(end.back()) + 1)};
    DataStream start;
    start << FINANCE_TX << txid;
    if (!after.IsNull()) start << after;
    auto cursor = m_batch->GetNewRangeCursor(start, end);
    if (!cursor) return false;
    while (true) {
        DataStream key, value;
        const auto next = cursor->Next(key, value);
        if (next == DatabaseCursor::Status::DONE) return true;
        if (next != DatabaseCursor::Status::MORE) return false;
        try {
            std::string type;
            uint256 transaction, id;
            uint256 indexed_id;
            key >> type >> transaction >> id;
            value >> indexed_id;
            if (!key.empty() || !value.empty() || indexed_id != id || type != FINANCE_TX || transaction != txid) return false;
            if (!after.IsNull() && !(after < id)) continue;
            if (events.size() == limit) {
                if (!has_more) return false;
                *has_more = true;
                return true;
            }
            ProviderFinanceEvent event;
            if (ReadPaymasterFinanceEvent(id, event) != DatabaseReadStatus::FOUND || event.transaction_id != txid) return false;
            events.push_back(std::move(event));
        } catch (const std::exception&) { return false; }
    }
}

bool WalletBatch::ReadPaymasterFinanceReport(const ProviderFinanceQuery& query,
                                            ProviderFinanceReport& report, std::string& error)
{
    std::optional<std::pair<int64_t, uint256>> position;
    return ReadPaymasterFinanceReport(query, report, error, position);
}

bool WalletBatch::ReadPaymasterFinanceReport(const ProviderFinanceQuery& query,
                                            ProviderFinanceReport& report, std::string& error,
                                            std::optional<std::pair<int64_t, uint256>>& cursor_position)
{
    report = {};
    cursor_position.reset();
    error.clear();
    auto fail = [&](const char* reason) { report = {}; error = reason; return false; };
    if (query.now <= 0 || query.now > std::numeric_limits<int64_t>::max() - PROVIDER_BUDGET_WINDOW_SECONDS || query.limit == 0 || query.limit > 10000 ||
        uint8_t(query.period) > uint8_t(ProviderFinancePeriod::ALL) ||
        (!query.include_events && !query.cursor.empty())) return fail("PAYMASTER_INVALID_FINANCE_QUERY");
    if (!MigratePaymasterFinanceLedger(error)) return false;
    if (ReadPaymasterFinanceMetadata(report.metadata) != DatabaseReadStatus::FOUND) return fail("PAYMASTER_FINANCE_READ_FAILED");
    const int64_t today = (query.now / PROVIDER_BUDGET_WINDOW_SECONDS) * PROVIDER_BUDGET_WINDOW_SECONDS;
    const std::array<int64_t, 4> cutoffs{today, std::max<int64_t>(0, today - 6 * PROVIDER_BUDGET_WINDOW_SECONDS),
                                      std::max<int64_t>(0, today - 29 * PROVIDER_BUDGET_WINDOW_SECONDS), 0};
    const int64_t cutoff = cutoffs[size_t(query.period)];
    report.periods[size_t(ProviderFinancePeriod::ALL)] = report.metadata.totals;
    // At most 366 calendar buckets are needed for all summaries and the chart.
    DataStream start = FinancePrefix(FINANCE_DAY);
    start << FinanceDayKey(std::max<int64_t>(0, today - 365 * PROVIDER_BUDGET_WINDOW_SECONDS));
    DataStream end = FinancePrefix(FINANCE_DAY);
    end << FinanceDayKey(today + PROVIDER_BUDGET_WINDOW_SECONDS);
    auto cursor = m_batch->GetNewRangeCursor(start, end);
    if (!cursor) return fail("PAYMASTER_FINANCE_READ_FAILED");
    while (true) {
        DataStream key, value;
        const auto next = cursor->Next(key, value);
        if (next == DatabaseCursor::Status::DONE) break;
        if (next != DatabaseCursor::Status::MORE || report.days.size() >= 366) return fail("PAYMASTER_FINANCE_INDEX_INVALID");
        try {
            std::string type;
            FinanceDay day;
            ProviderFinanceTotals totals;
            key >> type >> day;
            value >> totals;
            if (!key.empty() || !value.empty() || type != FINANCE_DAY ||
                !ValidFinanceTotals(totals) || day != FinanceDayKey(totals.total.day_start) ||
                totals.total.day_start % PROVIDER_BUDGET_WINDOW_SECONDS != 0) return fail("PAYMASTER_FINANCE_INDEX_INVALID");
            for (size_t i = 0; i < 3; ++i) {
                if (totals.total.day_start >= cutoffs[i] &&
                    !AddProviderFinanceTotals(report.periods[i], totals, error)) return fail("PAYMASTER_FINANCE_INDEX_INVALID");
            }
            report.days.push_back(totals);
        } catch (const std::exception&) { return fail("PAYMASTER_FINANCE_INDEX_INVALID"); }
    }
    if (!query.include_events) return true;
    FinanceCursor page;
    page.genesis = report.metadata.genesis_hash;
    page.provider = report.metadata.provider_id;
    page.period = uint8_t(query.period);
    page.cutoff = cutoff;
    page.revision = report.metadata.revision;
    start = FinancePrefix(FINANCE_TIME);
    if (!query.cursor.empty()) {
        if (query.cursor.size() > 512 || !IsHex(query.cursor)) return fail("PAYMASTER_FINANCE_CURSOR_INVALID");
        try {
            DataStream bytes{ParseHex(query.cursor)};
            FinanceCursor previous;
            bytes >> previous;
            if (!bytes.empty() || previous.version != page.version || previous.genesis != page.genesis ||
                previous.provider != page.provider || previous.period != page.period) return fail("PAYMASTER_FINANCE_CURSOR_INVALID");
            if (previous.cutoff != page.cutoff || previous.revision != page.revision) return fail("PAYMASTER_FINANCE_CURSOR_RESTART_REQUIRED");
            const uint64_t time = ~ReadBE64(previous.position.data());
            if (time > uint64_t(std::numeric_limits<int64_t>::max())) return fail("PAYMASTER_FINANCE_CURSOR_INVALID");
            uint256 event;
            for (size_t i = 0; i < 32; ++i) event.begin()[i] = ~previous.position[8 + i];
            cursor_position = std::make_pair(int64_t(time), event);
            start << previous.position << uint8_t{0}; // strict successor of the fixed-length key
        } catch (const std::exception&) { return fail("PAYMASTER_FINANCE_CURSOR_INVALID"); }
    }
    end = FinancePrefix(FINANCE_TIME);
    FinanceOrder last;
    last.fill(255);
    WriteBE64(last.data(), ~uint64_t(cutoff));
    end << last << uint8_t{0};
    cursor = m_batch->GetNewRangeCursor(start, end);
    if (!cursor) return fail("PAYMASTER_FINANCE_READ_FAILED");
    while (true) {
        DataStream key, value;
        const auto next = cursor->Next(key, value);
        if (next == DatabaseCursor::Status::DONE) break;
        if (next != DatabaseCursor::Status::MORE) return fail("PAYMASTER_FINANCE_INDEX_INVALID");
        try {
            std::string type;
            FinanceOrder position;
            uint256 id;
            ProviderFinanceEvent event;
            key >> type >> position;
            value >> id;
            if (!key.empty() || !value.empty() || type != FINANCE_TIME ||
                ReadPaymasterFinanceEvent(id, event) != DatabaseReadStatus::FOUND || position != FinanceTimeKey(event) ||
                FinanceEventTime(event) < cutoff || event.genesis_hash != page.genesis || event.provider_id != page.provider) return fail("PAYMASTER_FINANCE_INDEX_INVALID");
            if (report.events.size() == query.limit) {
                DataStream bytes;
                bytes << page;
                report.next_cursor = HexStr(bytes);
                break;
            }
            page.position = position;
            report.events.push_back(std::move(event));
        } catch (const std::exception&) { return fail("PAYMASTER_FINANCE_INDEX_INVALID"); }
    }
    return true;
}

bool WalletBatch::ReadPaymasterFinanceLedger(
    DigiDollar::Paymaster::ProviderFinanceLedger& ledger)
{
    return ReadPaymasterFinanceLedgerWithStatus(ledger) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterFinanceLedger()
{
    return m_batch->Exists(DBKeys::PAYMASTER_FINANCE_LEDGER);
}

bool WalletBatch::WritePaymasterBackupStatus(
    const DigiDollar::Paymaster::ProviderBackupStatus& status,
    bool overwrite)
{
    std::string error;
    // This is a non-blocking reminder, never financial authority. In
    // particular, recording success AFTER BackupWallet copied the database
    // must not make that otherwise current backup's checkpoint obsolete.
    // Financial transitions which also update the reminder still track their
    // other writes in the caller's atomic transaction.
    return DigiDollar::Paymaster::ValidateProviderBackupStatus(status, error) &&
           WriteIC(DBKeys::PAYMASTER_BACKUP_STATUS, status, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterBackupStatusWithStatus(
    DigiDollar::Paymaster::ProviderBackupStatus& status)
{
    std::string error;
    const DatabaseReadStatus read_status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_BACKUP_STATUS, status);
    if (read_status != DatabaseReadStatus::FOUND) return read_status;
    return DigiDollar::Paymaster::ValidateProviderBackupStatus(status, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterBackupStatus(
    DigiDollar::Paymaster::ProviderBackupStatus& status)
{
    return ReadPaymasterBackupStatusWithStatus(status) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterBackupStatus()
{
    return m_batch->Exists(DBKeys::PAYMASTER_BACKUP_STATUS);
}

bool WalletBatch::WritePaymasterRestoreGuard(const DigiDollar::Paymaster::ProviderIdentityRecord& identity)
{
    return DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity) &&
           WriteIC(DBKeys::PAYMASTER_RESTORE_GUARD, identity, /*overwrite=*/false);
}

DatabaseReadStatus WalletBatch::ReadPaymasterRestoreGuardWithStatus(DigiDollar::Paymaster::ProviderIdentityRecord& identity)
{
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(DBKeys::PAYMASTER_RESTORE_GUARD, identity);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::WritePaymasterCarrierWithdrawalPlan(
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
    bool overwrite)
{
    std::string error;
    return DigiDollar::Paymaster::ValidateProviderCarrierWithdrawalPlan(
               plan, error) &&
           WritePaymasterIC(DBKeys::PAYMASTER_CARRIER_WITHDRAWAL, plan, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterCarrierWithdrawalPlanWithStatus(
    DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan)
{
    std::string error;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        DBKeys::PAYMASTER_CARRIER_WITHDRAWAL, plan);
    if (status != DatabaseReadStatus::FOUND) return status;
    return DigiDollar::Paymaster::ValidateProviderCarrierWithdrawalPlan(
               plan, error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterCarrierWithdrawalPlan(
    DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan)
{
    return ReadPaymasterCarrierWithdrawalPlanWithStatus(plan) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterCarrierWithdrawalPlan()
{
    return m_batch->Exists(DBKeys::PAYMASTER_CARRIER_WITHDRAWAL);
}

bool WalletBatch::ErasePaymasterCarrierWithdrawalPlan()
{
    return ErasePaymasterIC(DBKeys::PAYMASTER_CARRIER_WITHDRAWAL);
}

bool WalletBatch::WritePaymasterAnnouncementSequence(uint64_t sequence, bool overwrite)
{
    if (sequence == 0) return false;
    return WritePaymasterIC(DBKeys::PAYMASTER_ANNOUNCE_SEQ, sequence, overwrite);
}

bool WalletBatch::ReadPaymasterAnnouncementSequence(uint64_t& sequence)
{
    return ReadPaymasterAnnouncementSequenceWithStatus(sequence) == DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ReadPaymasterAnnouncementSequenceWithStatus(uint64_t& sequence)
{
    const auto status = m_batch->ReadWithStatus(DBKeys::PAYMASTER_ANNOUNCE_SEQ, sequence);
    return status == DatabaseReadStatus::FOUND && sequence == 0 ? DatabaseReadStatus::READ_ERROR : status;
}

bool WalletBatch::WritePaymasterResult(const DigiDollar::Paymaster::PaymasterResult& result,
                                       bool overwrite)
{
    std::string error;
    if (!DigiDollar::Paymaster::ValidatePaymasterResultShape(result, error)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RESULT, result.commit_key), result, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterResultWithStatus(
    const uint256& commit_key,
    DigiDollar::Paymaster::PaymasterResult& result)
{
    std::string error;
    if (commit_key.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RESULT, commit_key), result);
    if (status != DatabaseReadStatus::FOUND) return status;
    return result.commit_key == commit_key &&
                   DigiDollar::Paymaster::ValidatePaymasterResultShape(result,
                                                                       error)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterResult(
    const uint256& commit_key,
    DigiDollar::Paymaster::PaymasterResult& result)
{
    return ReadPaymasterResultWithStatus(commit_key, result) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::HasPaymasterResult(const uint256& commit_key)
{
    return !commit_key.IsNull() &&
           m_batch->Exists(std::make_pair(DBKeys::PAYMASTER_RESULT, commit_key));
}

bool WalletBatch::ErasePaymasterResult(const uint256& commit_key)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RESULT, commit_key));
}

bool WalletBatch::WritePaymasterReliability(
    const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
    bool overwrite)
{
    if (!DigiDollar::Paymaster::ValidateReliabilityRecord(record)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RELIABILITY, record.provider_id), record, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterReliabilityWithStatus(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterReliabilityRecord& record)
{
    if (provider_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_RELIABILITY, provider_id), record);
    if (status != DatabaseReadStatus::FOUND) return status;
    return record.provider_id == provider_id &&
                   DigiDollar::Paymaster::ValidateReliabilityRecord(record)
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool WalletBatch::ReadPaymasterReliability(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterReliabilityRecord& record)
{
    return ReadPaymasterReliabilityWithStatus(provider_id, record) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterReliability(
    std::vector<DigiDollar::Paymaster::PaymasterReliabilityRecord>& records)
{
    records.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_RELIABILITY;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            DigiDollar::Paymaster::PaymasterId provider_id;
            DigiDollar::Paymaster::PaymasterReliabilityRecord record;
            key >> type >> provider_id;
            value >> record;
            if (type != DBKeys::PAYMASTER_RELIABILITY || record.provider_id != provider_id ||
                !DigiDollar::Paymaster::ValidateReliabilityRecord(record)) return false;
            records.push_back(std::move(record));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::ErasePaymasterReliability(const DigiDollar::Paymaster::PaymasterId& provider_id)
{
    return !provider_id.IsNull() && ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_RELIABILITY, provider_id));
}

bool WalletBatch::WritePaymasterEquivocationEvidence(
    const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
    bool overwrite)
{
    return DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence) &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION, evidence.evidence_id),
                   evidence, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterEquivocationEvidence(
    const uint256& evidence_id,
    DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence)
{
    if (evidence_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION, evidence_id), evidence);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (evidence.evidence_id != evidence_id ||
        !DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::WritePaymasterPendingEquivocation(
    const DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence,
    bool overwrite)
{
    return DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence) &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING,
                                  evidence.provider_id),
                   evidence, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterPendingEquivocation(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterEquivocationEvidence& evidence)
{
    if (provider_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING, provider_id),
        evidence);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (evidence.provider_id != provider_id ||
        !DigiDollar::Paymaster::ValidateEquivocationEvidence(evidence)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

DatabaseReadStatus WalletBatch::ListPaymasterPendingEquivocations(
    std::vector<DigiDollar::Paymaster::PaymasterEquivocationEvidence>& evidence)
{
    using DigiDollar::Paymaster::PaymasterEquivocationEvidence;
    evidence.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_EQUIVOCATION_PENDING;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return DatabaseReadStatus::READ_ERROR;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) {
            evidence.clear();
            return DatabaseReadStatus::READ_ERROR;
        }
        try {
            std::string type;
            DigiDollar::Paymaster::PaymasterId provider_id;
            PaymasterEquivocationEvidence candidate;
            key >> type >> provider_id;
            value >> candidate;
            if (!key.empty() || !value.empty() ||
                type != DBKeys::PAYMASTER_EQUIVOCATION_PENDING ||
                provider_id.IsNull() || candidate.provider_id != provider_id ||
                !DigiDollar::Paymaster::ValidateEquivocationEvidence(candidate)) {
                evidence.clear();
                return DatabaseReadStatus::READ_ERROR;
            }
            evidence.push_back(std::move(candidate));
        } catch (const std::ios_base::failure&) {
            evidence.clear();
            return DatabaseReadStatus::READ_ERROR;
        }
    }
    return evidence.empty() ? DatabaseReadStatus::NOT_FOUND : DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterPendingEquivocation(
    const DigiDollar::Paymaster::PaymasterId& provider_id)
{
    return !provider_id.IsNull() &&
           ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING,
                                  provider_id));
}

bool WalletBatch::WritePaymasterProviderBlock(
    const DigiDollar::Paymaster::PaymasterProviderBlock& block,
    bool overwrite)
{
    return DigiDollar::Paymaster::ValidateProviderBlock(block) &&
           WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_PROVIDER_BLOCK, block.provider_id),
                   block, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterProviderBlock(
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    DigiDollar::Paymaster::PaymasterProviderBlock& block)
{
    if (provider_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = m_batch->ReadWithStatus(
        std::make_pair(DBKeys::PAYMASTER_PROVIDER_BLOCK, provider_id), block);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (block.provider_id != provider_id ||
        !DigiDollar::Paymaster::ValidateProviderBlock(block)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ListPaymasterProviderBlocks(
    std::vector<DigiDollar::Paymaster::PaymasterProviderBlock>& blocks)
{
    blocks.clear();
    DataStream prefix;
    prefix << DBKeys::PAYMASTER_PROVIDER_BLOCK;
    std::unique_ptr<DatabaseCursor> cursor = m_batch->GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    while (true) {
        DataStream key;
        DataStream value;
        const DatabaseCursor::Status status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status == DatabaseCursor::Status::FAIL) return false;
        try {
            std::string type;
            DigiDollar::Paymaster::PaymasterId provider_id;
            DigiDollar::Paymaster::PaymasterProviderBlock block;
            key >> type >> provider_id;
            value >> block;
            if (type != DBKeys::PAYMASTER_PROVIDER_BLOCK || block.provider_id != provider_id ||
                !DigiDollar::Paymaster::ValidateProviderBlock(block)) return false;
            blocks.push_back(std::move(block));
        } catch (const std::ios_base::failure&) {
            return false;
        }
    }
    return true;
}

bool WalletBatch::WritePaymasterOutcomeMarker(
    const DigiDollar::Paymaster::PaymasterOutcomeMarker& marker,
    bool overwrite)
{
    if (marker.version != DigiDollar::Paymaster::PaymasterOutcomeMarker::CURRENT_VERSION ||
        marker.attempt_id.IsNull() || marker.provider_id.IsNull() || marker.observed_at <= 0 ||
        static_cast<uint8_t>(marker.outcome) >
            static_cast<uint8_t>(DigiDollar::Paymaster::ReliabilityOutcome::AVAILABILITY_TIMEOUT)) return false;
    return WritePaymasterIC(std::make_pair(DBKeys::PAYMASTER_OUTCOME, marker.attempt_id), marker, overwrite);
}

DatabaseReadStatus WalletBatch::ReadPaymasterOutcomeMarkerWithStatus(
    const uint256& attempt_id,
    DigiDollar::Paymaster::PaymasterOutcomeMarker& marker)
{
    using namespace DigiDollar::Paymaster;
    if (attempt_id.IsNull()) return DatabaseReadStatus::READ_ERROR;
    const DatabaseReadStatus status = ReadPaymasterVersionedRecord(
        std::make_pair(DBKeys::PAYMASTER_OUTCOME, attempt_id), marker);
    if (status != DatabaseReadStatus::FOUND) return status;
    if (marker.attempt_id != attempt_id || marker.provider_id.IsNull() ||
        marker.observed_at <= 0 ||
        static_cast<uint8_t>(marker.outcome) >
            static_cast<uint8_t>(ReliabilityOutcome::AVAILABILITY_TIMEOUT)) {
        return DatabaseReadStatus::READ_ERROR;
    }
    return DatabaseReadStatus::FOUND;
}

bool WalletBatch::ReadPaymasterOutcomeMarker(
    const uint256& attempt_id,
    DigiDollar::Paymaster::PaymasterOutcomeMarker& marker)
{
    return ReadPaymasterOutcomeMarkerWithStatus(attempt_id, marker) ==
           DatabaseReadStatus::FOUND;
}

bool WalletBatch::ErasePaymasterOutcomeMarker(const uint256& attempt_id)
{
    return ErasePaymasterIC(std::make_pair(DBKeys::PAYMASTER_OUTCOME, attempt_id));
}

} // namespace wallet
