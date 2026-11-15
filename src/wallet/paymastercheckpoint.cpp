// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/paymastercheckpoint.h>

#include <chainparams.h>
#include <hash.h>
#include <logging.h>
#include <random.h>
#include <streams.h>
#include <support/allocators/zeroafterfree.h>
#include <primitives/transaction.h>
#include <version.h>
#include <util/fs_helpers.h>
#include <util/system.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/paymastersessionindex.h>
#include <wallet/paymasterfinancecache.h>
#include <wallet/paymastermaintenancecache.h>

#include <array>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <vector>

#ifdef WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace wallet {
namespace {
const std::string CHECKPOINT_KEY{"pmcheckpoint"};
constexpr uint64_t CHECKPOINT_MAGIC{0x3150435042474444};
constexpr size_t CHECKPOINT_FILE_SIZE{149};
const std::string JOURNAL_KEY{"pmcheckpointjournal"};
// A snapshot may replace the full accepted finance history: an old and a new
// event/time/tx/day row per event. Preserve that domain limit, including a legacy
// vector ledger, plus separate headroom for payment/native accounting records.
constexpr size_t MAX_JOURNAL_BYTES{1792 * DigiDollar::Paymaster::MAX_PROVIDER_FINANCE_EVENTS + 64 * 1024 * 1024};
constexpr size_t MAX_JOURNAL_ROWS{8 * DigiDollar::Paymaster::MAX_PROVIDER_FINANCE_EVENTS + 65536};
constexpr size_t MAX_JOURNAL_KEY_BYTES{4096};
constexpr size_t MAX_JOURNAL_VALUE_BYTES{256 * DigiDollar::Paymaster::MAX_PROVIDER_FINANCE_EVENTS + 64 * 1024 * 1024};
// Transactions can contain encrypted-key or legacy private-key rows before a
// Paymaster write. Match the wallet stream's zero-after-free lifetime for every
// captured byte, parser temporary and journal copy, including ordinary aborts.
using RawBytes = std::vector<unsigned char, zero_after_free_allocator<unsigned char>>;

struct RawValue {
    RawBytes bytes;
    template <typename Stream> void Unserialize(Stream& stream)
    {
        if (stream.size() > MAX_JOURNAL_VALUE_BYTES) throw std::ios_base::failure("checkpoint journal row too large");
        bytes.resize(stream.size());
        stream.read(MakeWritableByteSpan(bytes));
    }
};

struct Mutation {
    RawBytes key;
    std::optional<RawBytes> before, after;
    template <typename Stream> void Serialize(Stream& stream) const
    {
        stream << key << bool(before);
        if (before) stream << *before;
        stream << bool(after);
        if (after) stream << *after;
    }
};

/** Wallet-only intent. Its complete contents are authenticated by the next
 * external checkpoint token; no key, transaction or metadata leaves the wallet. */
struct MutationJournal {
    static constexpr uint16_t CURRENT_VERSION{1};
    uint16_t version{CURRENT_VERSION};
    uint256 nonce;
    std::optional<PaymasterCheckpoint> before;
    PaymasterCheckpoint next;
    std::optional<RawBytes> restore_guard;
    std::vector<Mutation> rows;
    template <typename Stream> void Serialize(Stream& stream) const
    {
        stream << version << nonce << bool(before);
        if (before) stream << *before;
        stream << next << bool(restore_guard);
        if (restore_guard) stream << *restore_guard;
        stream << rows;
    }
    template <typename Stream> void Unserialize(Stream& stream)
    {
        if (stream.size() > MAX_JOURNAL_BYTES) throw std::ios_base::failure("checkpoint journal too large");
        stream >> version;
        if (version != CURRENT_VERSION) throw std::ios_base::failure("unsupported checkpoint journal");
        stream >> nonce;
        uint8_t present;
        stream >> present;
        if (present > 1) throw std::ios_base::failure("checkpoint journal invalid optional");
        if (present) { before.emplace(); stream >> *before; }
        else before.reset();
        stream >> next;
        const auto bytes = [&](size_t maximum) {
            const auto size = ReadCompactSize(stream, /*range_check=*/false);
            if (size > maximum || size > stream.size()) throw std::ios_base::failure("checkpoint journal invalid row");
            RawBytes value(size);
            stream.read(MakeWritableByteSpan(value));
            return value;
        };
        stream >> present;
        if (present > 1) throw std::ios_base::failure("checkpoint journal invalid optional");
        if (present) restore_guard = bytes(MAX_JOURNAL_VALUE_BYTES);
        else restore_guard.reset();
        const auto count = ReadCompactSize(stream, /*range_check=*/false);
        if (count > MAX_JOURNAL_ROWS || count > stream.size() / 3)
            throw std::ios_base::failure("checkpoint journal too many rows");
        rows.clear();
        rows.reserve(count);
        for (uint64_t index{0}; index < count; ++index) {
            Mutation row;
            row.key = bytes(MAX_JOURNAL_KEY_BYTES);
            for (auto* value : {&row.before, &row.after}) {
                uint8_t present;
                stream >> present;
                if (present > 1) throw std::ios_base::failure("checkpoint journal invalid optional");
                if (present) *value = bytes(MAX_JOURNAL_VALUE_BYTES);
            }
            rows.push_back(std::move(row));
        }
        if (!stream.empty()) throw std::ios_base::failure("checkpoint journal trailing bytes");
    }
};

template <typename T> RawBytes Bytes(const T& value)
{
    DataStream stream;
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

DatabaseReadStatus ReadRaw(DatabaseBatch& batch, Span<const std::byte> key, std::optional<RawBytes>& value)
{
    RawValue raw;
    const auto status = batch.ReadWithStatus(key, raw);
    if (status == DatabaseReadStatus::FOUND) value = std::move(raw.bytes);
    else value.reset();
    return status;
}

uint256 JournalHash(const MutationJournal& journal)
{
    auto target = journal.next;
    target.token.SetNull();
    HashWriter hasher;
    hasher << std::string{"DigiByte/PaymasterCheckpointMutation/v1"}
           << journal.version << journal.nonce << bool(journal.before);
    if (journal.before) hasher << *journal.before;
    hasher << target << bool(journal.restore_guard);
    if (journal.restore_guard) hasher << *journal.restore_guard;
    return (hasher << journal.rows).GetHash();
}

struct CheckpointFileState {
    std::recursive_mutex mutex;
    bool transaction_active{false};
};

struct TransactionState {
    std::recursive_mutex mutex;
    std::shared_ptr<CheckpointFileState> file_state;
    bool owns_file{false};
    std::unique_ptr<fsbridge::FileLock> file_lock;
    fs::path path;
    PaymasterCheckpoint next;
    bool dirty{false};
    bool failed{false};
    bool capture_failed{false};
    DatabaseBatch* batch{nullptr};
    WalletDatabase* database{nullptr};
    std::optional<PaymasterCheckpoint> before;
    std::optional<RawBytes> restore_guard;
    std::map<RawBytes, Mutation> mutations;
    size_t mutation_bytes{0};

    ~TransactionState()
    {
        if (!owns_file) return;
        std::lock_guard<std::recursive_mutex> lock{file_state->mutex};
        // POSIX locks are process-owned. Close this descriptor before allowing
        // another transaction to open/close a descriptor for the same file.
        file_lock.reset();
        file_state->transaction_active = false;
    }
};
// Registry lock is never held during file/database I/O or while acquiring a
// transaction/file lock. Order: wallet -> transaction -> checkpoint file.
std::mutex registry_mutex;
std::map<const WalletBatch*, std::shared_ptr<TransactionState>> transactions;
std::map<fs::path, std::weak_ptr<CheckpointFileState>> checkpoint_files;
thread_local std::set<WalletDatabase*> pending_operations;
thread_local std::map<WalletDatabase*, uint256> pending_executions;

std::shared_ptr<TransactionState> FindTransaction(const WalletBatch* owner)
{
    std::lock_guard<std::mutex> lock{registry_mutex};
    const auto found = transactions.find(owner);
    return found == transactions.end() ? nullptr : found->second;
}

std::shared_ptr<CheckpointFileState> FileState(const fs::path& path)
{
    // Every caller uses PaymasterCheckpointPath: one cached network data
    // directory and a genesis/provider-derived filename. Normalize that path
    // without probing a not-yet-created file (including Windows long paths).
    const auto key = path.lexically_normal();
    std::lock_guard<std::mutex> lock{registry_mutex};
    auto& weak = checkpoint_files[key];
    if (auto shared = weak.lock()) return shared;
    // Reclaim expired identities without retaining wallet/provider lifetimes.
    for (auto it = checkpoint_files.begin(); it != checkpoint_files.end();) {
        if (it->first != key && it->second.expired()) it = checkpoint_files.erase(it);
        else ++it;
    }
    auto shared = std::make_shared<CheckpointFileState>();
    weak = shared;
    return shared;
}

bool Valid(const PaymasterCheckpoint& record, const uint256& provider)
{
    return (record.version == 1 || record.version == PaymasterCheckpoint::CURRENT_VERSION) &&
           record.genesis_hash == Params().GenesisBlock().GetHash() &&
           record.provider_id == provider && !provider.IsNull() &&
           record.generation > 0 && !record.token.IsNull() &&
           record.pending <= (record.version == 1 ? 1 : PaymasterCheckpoint::RETIRED);
}

struct StrictRecord : PaymasterCheckpoint {
    template <typename Stream>
    void Unserialize(Stream& stream)
    {
        stream >> static_cast<PaymasterCheckpoint&>(*this);
        if (!stream.empty()) throw std::ios_base::failure("checkpoint trailing bytes");
    }
};

DatabaseReadStatus ReadCheckpoint(DatabaseBatch& batch, PaymasterCheckpoint& record)
{
    StrictRecord strict;
    const auto status = batch.ReadWithStatus(CHECKPOINT_KEY, strict);
    record = strict;
    return status;
}

uint256 Checksum(const PaymasterCheckpoint& record)
{
    return (HashWriter{} << std::string{"DigiByte/PaymasterCheckpoint/v1"} << record).GetHash();
}

// The file is fixed-size, contains no attacker-selected paths and has no
// extensible parser. Future layouts, extra bytes and corruption fail closed.
DatabaseReadStatus ReadFile(const fs::path& path, PaymasterCheckpoint& record)
{
    // CRT read handles on Windows can prevent replacement of the same file.
    // Keep their entire lifetime inside this file's checkpoint mutex. This
    // takes no database locks: callers read the wallet row before entering.
    const auto file_state = FileState(path);
    if (!file_state) return DatabaseReadStatus::READ_ERROR;
    std::lock_guard<std::recursive_mutex> lock{file_state->mutex};
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found) return DatabaseReadStatus::NOT_FOUND;
    if (ec || !fs::is_regular_file(status)) return DatabaseReadStatus::READ_ERROR;
    FILE* file = fsbridge::fopen(path, "rb");
    if (!file) return DatabaseReadStatus::READ_ERROR;
    std::array<unsigned char, CHECKPOINT_FILE_SIZE> bytes{};
    const bool complete = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size() &&
                          std::fgetc(file) == EOF && !std::ferror(file);
    const bool closed = std::fclose(file) == 0;
    if (!complete || !closed) return DatabaseReadStatus::READ_ERROR;
    try {
        CDataStream stream{bytes, SER_DISK, CLIENT_VERSION};
        uint64_t magic;
        uint256 checksum;
        stream >> magic >> record >> checksum;
        if (!stream.empty() || magic != CHECKPOINT_MAGIC || checksum != Checksum(record)) return DatabaseReadStatus::READ_ERROR;
        return DatabaseReadStatus::FOUND;
    } catch (const std::exception&) {
        return DatabaseReadStatus::READ_ERROR;
    }
}

bool SyncDirectory(const fs::path& path)
{
#ifdef WIN32
    (void)path;
    // MoveFileExW below uses WRITE_THROUGH. File contents were flushed first.
    return true;
#else
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    return synced && closed;
#endif
}

bool CommitCheckpointFile(FILE* file)
{
#if defined(WIN32) || (defined(MAC_OSX) && defined(F_FULLFSYNC))
    return FileCommit(file);
#else
    // The generic FileCommit intentionally tolerates EINVAL on some file
    // systems. A financial checkpoint must reject unsupported synchronization.
    return std::fflush(file) == 0 && fsync(fileno(file)) == 0;
#endif
}

#ifdef WIN32
std::wstring ExtendedPath(const fs::path& path)
{
    auto native = fs::absolute(path).lexically_normal().make_preferred().wstring();
    if (native.rfind(L"\\\\?\\", 0) == 0) return native;
    if (native.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + native.substr(2);
    return L"\\\\?\\" + native;
}
#endif

bool WriteFile(const fs::path& path, const PaymasterCheckpoint& record)
{
    const auto temporary = fs::path{path.parent_path()} / fs::PathFromString(record.token.GetHex() + ".tmp");
    std::error_code ec;
    if (fs::symlink_status(temporary, ec).type() != fs::file_type::not_found) { LogPrintf("Paymaster checkpoint temporary path unavailable: %s\n", ec.message()); return false; }
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << CHECKPOINT_MAGIC << record << Checksum(record);
    if (stream.size() != CHECKPOINT_FILE_SIZE) { LogPrintf("Paymaster checkpoint unexpected encoded size: %u\n", stream.size()); return false; }
    FILE* file = fsbridge::fopen(temporary, "wb");
    if (!file) { LogPrintf("Paymaster checkpoint file open failed\n"); return false; }
    const bool written = std::fwrite(stream.data(), 1, stream.size(), file) == stream.size() && CommitCheckpointFile(file);
    const bool closed = std::fclose(file) == 0;
    bool replaced{false};
    if (written && closed) {
#ifdef WIN32
        replaced = MoveFileExW(ExtendedPath(temporary).c_str(), ExtendedPath(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!replaced) LogPrintf("Paymaster checkpoint replace failed: Windows error %u\n", GetLastError());
#else
        replaced = RenameOver(temporary, path);
#endif
    }
    if (!replaced) fs::remove(temporary, ec);
    // Never roll an advanced external checkpoint back after an uncertain
    // flush/commit. A mismatch is a reason to stop creating new authority.
    if (!written || !closed || !replaced) LogPrintf("Paymaster checkpoint file commit failed: write %d, close %d, replace %d\n", written, closed, replaced);
    return replaced && SyncDirectory(path.parent_path());
}

bool ReadCurrent(DatabaseBatch& batch, const uint256& provider,
                 PaymasterCheckpoint& current, bool& enrolled)
{
    PaymasterCheckpoint external;
    const auto wallet_status = ReadCheckpoint(batch, current);
    const auto external_status = ReadFile(PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), provider), external);
    enrolled = wallet_status == DatabaseReadStatus::FOUND;
    if (wallet_status == DatabaseReadStatus::NOT_FOUND && external_status == DatabaseReadStatus::NOT_FOUND) return true;
    const bool matches = wallet_status == DatabaseReadStatus::FOUND && external_status == DatabaseReadStatus::FOUND &&
                         Valid(current, provider) && Valid(external, provider) && current == external;
    return matches;
}

bool ReadResumableCurrent(DatabaseBatch& batch, const uint256& provider,
                          const uint256& execution_hash, PaymasterCheckpoint& current)
{
    PaymasterCheckpoint external;
    if (execution_hash.IsNull() || ReadCheckpoint(batch, current) != DatabaseReadStatus::FOUND ||
        ReadFile(PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), provider), external) != DatabaseReadStatus::FOUND ||
        !Valid(current, provider) || !Valid(external, provider) || current.pending != 1 ||
        current.token != execution_hash) return false;
    if (current == external) return true;
    // The external high-water mark is written before its wallet commit. Only
    // that immediately adjacent interrupted write may be reconciled. Never
    // accept an arbitrary stale backup, retirement or another operation.
    if (current.generation == std::numeric_limits<uint64_t>::max() ||
        external.generation != current.generation + 1 || external.pending == PaymasterCheckpoint::RETIRED ||
        (external.pending == 1 && external.token != execution_hash)) return false;
    current = external;
    return true;
}
} // namespace

bool PaymasterCheckpoint::operator==(const PaymasterCheckpoint& other) const
{
    return version == other.version && genesis_hash == other.genesis_hash &&
           provider_id == other.provider_id && generation == other.generation &&
           token == other.token && pending == other.pending;
}

fs::path PaymasterCheckpointPath(const uint256& genesis, const uint256& provider)
{
    const auto name = (HashWriter{} << std::string{"DigiByte/PaymasterCheckpoint/path/v1"} << genesis << provider).GetHash();
    const auto path = gArgs.GetDataDirNet() / "paymaster-checkpoints" / fs::PathFromString(name.GetHex() + ".checkpoint");
#ifdef WIN32
    return fs::path{std::filesystem::path{ExtendedPath(path)}};
#else
    return path;
#endif
}

bool CheckPaymasterCheckpoint(WalletDatabase& database, std::string& error)
{
    error.clear();
    uint16_t journal_version{0};
    if (database.MakeBatch()->ReadWithStatus(JOURNAL_KEY, journal_version) != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED";
        return false;
    }
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    const auto status = WalletBatch{database}.ReadPaymasterIdentityWithStatus(identity);
    if (status == DatabaseReadStatus::NOT_FOUND) return true;
    auto batch = database.MakeBatch();
    PaymasterCheckpoint current;
    bool enrolled{false};
    if (status != DatabaseReadStatus::FOUND || !ReadCurrent(*batch, identity.provider_id, current, enrolled)) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED";
        return false;
    }
    if (enrolled && current.generation == (std::numeric_limits<uint64_t>::max)()) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED";
        return false;
    }
    if (enrolled && current.pending == PaymasterCheckpoint::RETIRED) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_RETIRED";
        return false;
    }
    PaymasterMaintenanceExecution execution;
    const auto execution_status = WalletBatch{database}.ReadPaymasterMaintenanceExecutionWithStatus(execution);
    if (execution_status != DatabaseReadStatus::NOT_FOUND &&
        (execution_status != DatabaseReadStatus::FOUND || !enrolled || current.pending != 1 ||
         current.token != GetPaymasterMaintenanceExecutionHash(execution))) {
        error = "PAYMASTER_MAINTENANCE_RECOVERY_REVIEW_REQUIRED";
        return false;
    }
    if (enrolled && current.pending && !pending_operations.count(&database)) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION";
        return false;
    }
    return true;
}

bool CheckPaymasterCapitalRecoveryCheckpoint(WalletDatabase& database,
                                            uint256& revision, std::string& error)
{
    error.clear();
    revision.SetNull();
    uint16_t journal_version{0};
    if (database.MakeBatch()->ReadWithStatus(JOURNAL_KEY, journal_version) != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED";
        return false;
    }
    WalletBatch batch{database};
    DigiDollar::Paymaster::ProviderIdentityRecord identity, guard;
    if (batch.ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND ||
        batch.ReadPaymasterRestoreGuardWithStatus(guard) != DatabaseReadStatus::FOUND ||
        guard.provider_id != identity.provider_id) {
        error = "PAYMASTER_CAPITAL_RECOVERY_REQUIRES_RESTORED_WALLET";
        return false;
    }
    auto raw = database.MakeBatch();
    PaymasterCheckpoint current;
    bool enrolled{false};
    if (!ReadCurrent(*raw, identity.provider_id, current, enrolled) || !enrolled ||
        current.generation == (std::numeric_limits<uint64_t>::max)()) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED";
        return false;
    }
    if (current.pending == 1) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION";
        return false;
    }
    revision = Checksum(current);
    return true;
}

void PaymasterCheckpointBegin(const WalletBatch* owner)
{
    auto state = std::make_shared<TransactionState>();
    std::lock_guard<std::mutex> lock{registry_mutex};
    transactions.try_emplace(owner, std::move(state));
}

bool PaymasterCheckpointInTransaction(const WalletBatch* owner)
{
    return static_cast<bool>(FindTransaction(owner));
}

namespace {
bool CaptureMutation(TransactionState& state, DatabaseBatch& batch,
    Span<const std::byte> key, const std::optional<RawBytes>& after)
{
    if (state.batch && state.batch != &batch) { state.capture_failed = true; return false; }
    state.batch = &batch;
    const RawBytes encoded_key{UCharCast(key.data()), UCharCast(key.data()) + key.size()};
    if (encoded_key == Bytes(CHECKPOINT_KEY)) return true;
    if (encoded_key == Bytes(JOURNAL_KEY)) { state.capture_failed = true; return false; }
    auto found = state.mutations.find(encoded_key);
    Mutation row;
    size_t previous_bytes{0};
    if (found == state.mutations.end()) {
        row.key = encoded_key;
        const auto status = ReadRaw(batch, key, row.before);
        if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) state.capture_failed = true;
    } else {
        row = found->second;
        previous_bytes = row.key.size() + (row.before ? row.before->size() : 0) + (row.after ? row.after->size() : 0);
    }
    row.after = after;
    const size_t bytes = row.key.size() + (row.before ? row.before->size() : 0) + (row.after ? row.after->size() : 0);
    if (row.key.empty() || row.key.size() > MAX_JOURNAL_KEY_BYTES ||
        (row.before && row.before->size() > MAX_JOURNAL_VALUE_BYTES) ||
        (row.after && row.after->size() > MAX_JOURNAL_VALUE_BYTES) ||
        (found == state.mutations.end() && state.mutations.size() >= MAX_JOURNAL_ROWS) ||
        bytes > MAX_JOURNAL_BYTES - state.mutation_bytes + previous_bytes) state.capture_failed = true;
    if (state.capture_failed) {
        state.mutations.clear();
        state.mutation_bytes = 0;
        return false;
    }
    state.mutation_bytes = state.mutation_bytes - previous_bytes + bytes;
    state.mutations[encoded_key] = std::move(row);
    return true;
}
} // namespace

bool WritePaymasterCheckpointRecord(const WalletBatch* owner, DatabaseBatch& batch,
    Span<const std::byte> key, Span<const std::byte> value, bool overwrite)
{
    const auto transaction = FindTransaction(owner);
    if (!transaction) return batch.Write(key, value, overwrite);
    std::lock_guard<std::recursive_mutex> lock{transaction->mutex};
    auto& state = *transaction;
    if (state.capture_failed && !state.dirty) return batch.Write(key, value, overwrite);
    // A failed write is never part of the intended after-image. A checkpointed
    // financial transaction cannot commit after an unrecordable mutation.
    const RawBytes after{UCharCast(value.data()), UCharCast(value.data()) + value.size()};
    if (!CaptureMutation(state, batch, key, after) && state.dirty) { state.failed = true; return false; }
    if (batch.Write(key, value, overwrite)) return true;
    state.capture_failed = true;
    if (state.dirty) state.failed = true;
    return false;
}

bool ErasePaymasterCheckpointRecord(const WalletBatch* owner, DatabaseBatch& batch, Span<const std::byte> key)
{
    const auto transaction = FindTransaction(owner);
    if (!transaction) return batch.Erase(key);
    std::lock_guard<std::recursive_mutex> lock{transaction->mutex};
    auto& state = *transaction;
    if (state.capture_failed && !state.dirty) return batch.Erase(key);
    if (!CaptureMutation(state, batch, key, std::nullopt) && state.dirty) { state.failed = true; return false; }
    if (batch.Erase(key)) return true;
    state.capture_failed = true;
    if (state.dirty) state.failed = true;
    return false;
}

bool ErasePaymasterCheckpointPrefix(const WalletBatch* owner, DatabaseBatch& batch, Span<const std::byte> prefix)
{
    if (!FindTransaction(owner)) return batch.ErasePrefix(prefix);
    auto cursor = batch.GetNewPrefixCursor(prefix);
    if (!cursor) return false;
    std::vector<RawBytes> keys;
    size_t bytes{0};
    while (true) {
        DataStream key, value;
        const auto status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        if (status != DatabaseCursor::Status::MORE || key.size() > MAX_JOURNAL_KEY_BYTES ||
            value.size() > MAX_JOURNAL_VALUE_BYTES || keys.size() >= MAX_JOURNAL_ROWS ||
            key.size() + value.size() > MAX_JOURNAL_BYTES - bytes) return false;
        bytes += key.size() + value.size();
        const auto raw = MakeUCharSpan(key);
        keys.emplace_back(raw.begin(), raw.end());
    }
    cursor.reset();
    for (const auto& key : keys) if (!ErasePaymasterCheckpointRecord(owner, batch, MakeByteSpan(key))) return false;
    return true;
}

namespace {
bool TrackCheckpoint(const WalletBatch* owner, DatabaseBatch& batch, WalletDatabase& database,
                     bool retirement, const uint256& expected_revision, bool adoption = false)
{
    const auto transaction = FindTransaction(owner);
    if (!transaction) return false;
    std::lock_guard<std::recursive_mutex> lock{transaction->mutex};
    auto& state = *transaction;
    if (state.failed) return false;
    if (state.dirty) return true;
    // Quarantined wallets cannot advance the original provider's high-water
    // mark through signed recovery/accounting. Only an explicit reviewed
    // retirement or current-backup adoption may advance that checkpoint.
    if (!retirement && !adoption) {
        DigiDollar::Paymaster::ProviderIdentityRecord guard;
        const auto guard_status = batch.ReadWithStatus(DBKeys::PAYMASTER_RESTORE_GUARD, guard);
        if (guard_status == DatabaseReadStatus::FOUND &&
            guard.version == DigiDollar::Paymaster::ProviderIdentityRecord::CURRENT_VERSION && !guard.provider_id.IsNull()) return true;
        if (guard_status != DatabaseReadStatus::NOT_FOUND) { state.failed = true; return false; }
    }
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    const auto identity_status = batch.ReadWithStatus(DBKeys::PAYMASTER_IDENTITY, identity);
    if (identity_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (identity_status != DatabaseReadStatus::FOUND || identity.version != DigiDollar::Paymaster::ProviderIdentityRecord::CURRENT_VERSION || identity.provider_id.IsNull()) {
        state.failed = true;
        return false;
    }
    // A client-only or ordinary transaction has no independent provider
    // checkpoint and keeps its existing backend semantics. Complete capture
    // becomes mandatory only once this transaction would advance one.
    if (state.capture_failed) { state.failed = true; return false; }
    state.path = PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), identity.provider_id);
    try {
        uint16_t journal_version{0};
        if (batch.ReadWithStatus(JOURNAL_KEY, journal_version) != DatabaseReadStatus::NOT_FOUND)
            throw std::runtime_error("unfinished checkpoint mutation");
        state.batch = &batch;
        state.database = &database;
        const auto guard_status = ReadRaw(batch, MakeByteSpan(Bytes(DBKeys::PAYMASTER_RESTORE_GUARD)), state.restore_guard);
        if (guard_status != DatabaseReadStatus::FOUND && guard_status != DatabaseReadStatus::NOT_FOUND)
            throw std::runtime_error("invalid restore guard");
        state.file_state = FileState(state.path);
        if (!state.file_state) throw std::runtime_error("invalid checkpoint path");
        std::lock_guard<std::recursive_mutex> file_guard{state.file_state->mutex};
        if (state.file_state->transaction_active) throw std::runtime_error("checkpoint busy");
        state.file_state->transaction_active = true;
        state.owns_file = true;
        fs::create_directories(state.path.parent_path());
        if (!SyncDirectory(state.path.parent_path().parent_path())) throw std::runtime_error("directory sync failed");
        const auto lock_path = fs::path{state.path.parent_path()} / fs::PathFromString(identity.provider_id.GetHex() + ".lock");
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(lock_path, ec))) throw std::runtime_error("invalid lock file");
        FILE* file = fsbridge::fopen(lock_path, "ab");
        if (!file || std::fclose(file) != 0) throw std::runtime_error("lock file unavailable");
        state.file_lock = std::make_unique<fsbridge::FileLock>(lock_path);
        if (!state.file_lock->TryLock()) throw std::runtime_error("checkpoint busy");
        bool enrolled{false};
        const auto execution = pending_executions.find(&database);
        const uint256 execution_hash = execution == pending_executions.end() ? uint256{} : execution->second;
        if (!ReadCurrent(batch, identity.provider_id, state.next, enrolled)) {
            if (!ReadResumableCurrent(batch, identity.provider_id, execution_hash, state.next))
                throw std::runtime_error("checkpoint not current");
            enrolled = true;
        }
        if (enrolled) state.before = state.next;
        if (state.next.generation == (std::numeric_limits<uint64_t>::max)()) throw std::runtime_error("checkpoint exhausted");
        if (retirement || adoption) {
            DigiDollar::Paymaster::ProviderIdentityRecord guard;
            if (batch.ReadWithStatus(DBKeys::PAYMASTER_RESTORE_GUARD, guard) != DatabaseReadStatus::FOUND ||
                guard.version != DigiDollar::Paymaster::ProviderIdentityRecord::CURRENT_VERSION ||
                guard.provider_id != identity.provider_id || !enrolled ||
                expected_revision.IsNull() || Checksum(state.next) != expected_revision ||
                state.next.pending == 1 ||
                (adoption && state.next.pending != 0)) throw std::runtime_error("recovery checkpoint changed");
            if (retirement) state.next.pending = PaymasterCheckpoint::RETIRED;
        } else if (enrolled && (state.next.pending == PaymasterCheckpoint::RETIRED ||
                   (state.next.pending && !pending_operations.count(&database)))) {
            throw std::runtime_error("checkpoint prohibits new authority");
        }
        state.next.version = PaymasterCheckpoint::CURRENT_VERSION;
        state.next.genesis_hash = Params().GenesisBlock().GetHash();
        state.next.provider_id = identity.provider_id;
        ++state.next.generation;
        state.next.token = state.next.pending == 1 && !execution_hash.IsNull() ? execution_hash : GetRandHash();
        if (!batch.Write(CHECKPOINT_KEY, state.next)) throw std::runtime_error("checkpoint write failed");
        state.dirty = true;
        return true;
    } catch (const std::exception& exception) {
        LogPrint(BCLog::DIGIDOLLAR, "Paymaster checkpoint mutation refused: %s\n", exception.what());
        state.failed = true;
        return false;
    }
}
} // namespace

bool PaymasterCheckpointWrite(const WalletBatch* owner, DatabaseBatch& batch, WalletDatabase& database)
{
    return TrackCheckpoint(owner, batch, database, false, {});
}

bool WalletBatch::RetirePaymasterCheckpoint(const uint256& expected_revision)
{
    const auto state = FindTransaction(this);
    if (!state) return false;
    std::lock_guard<std::recursive_mutex> lock{state->mutex};
    if (state->dirty) return false;
    return TrackCheckpoint(this, *m_batch, m_database, true, expected_revision);
}

bool WalletBatch::AdoptPaymasterCheckpoint(const uint256& expected_revision)
{
    const auto state = FindTransaction(this);
    if (!state) return false;
    std::lock_guard<std::recursive_mutex> lock{state->mutex};
    if (state->dirty ||
        !TrackCheckpoint(this, *m_batch, m_database, false, expected_revision, true)) return false;
    // The external checkpoint is advanced by PrepareCommit before this erase
    // commits. A failed commit leaves the guard and a conservative mismatch.
    if (!ErasePaymasterCheckpointRecord(this, *m_batch, MakeByteSpan(Bytes(DBKeys::PAYMASTER_RESTORE_GUARD)))) {
        state->failed = true;
        return false;
    }
    return true;
}

namespace {
bool ValidJournal(const MutationJournal& journal)
{
    if (journal.version != MutationJournal::CURRENT_VERSION || journal.nonce.IsNull() ||
        !Valid(journal.next, journal.next.provider_id) ||
        journal.next.token != JournalHash(journal) || journal.rows.size() > MAX_JOURNAL_ROWS) return false;
    if (journal.before) {
        if (!Valid(*journal.before, journal.next.provider_id) ||
            journal.before->generation == std::numeric_limits<uint64_t>::max() ||
            journal.next.generation != journal.before->generation + 1) return false;
    } else if (journal.next.generation != 1) return false;
    RawBytes previous;
    size_t bytes{0};
    for (const auto& row : journal.rows) {
        if (row.key.empty() || row.key.size() > MAX_JOURNAL_KEY_BYTES ||
            (!previous.empty() && row.key <= previous) || row.key == Bytes(CHECKPOINT_KEY) || row.key == Bytes(JOURNAL_KEY) ||
            (row.before && row.before->size() > MAX_JOURNAL_VALUE_BYTES) ||
            (row.after && row.after->size() > MAX_JOURNAL_VALUE_BYTES)) return false;
        const size_t size = row.key.size() + (row.before ? row.before->size() : 0) + (row.after ? row.after->size() : 0);
        if (size > MAX_JOURNAL_BYTES - bytes) return false;
        bytes += size;
        previous = row.key;
    }
    return GetSerializeSize(journal) <= MAX_JOURNAL_BYTES;
}

bool MatchesJournalBefore(DatabaseBatch& batch, const MutationJournal& journal)
{
    PaymasterCheckpoint current;
    const auto status = ReadCheckpoint(batch, current);
    if (journal.before ? status != DatabaseReadStatus::FOUND || !(current == *journal.before)
                       : status != DatabaseReadStatus::NOT_FOUND) return false;
    std::optional<RawBytes> guard;
    const auto guard_status = ReadRaw(batch, MakeByteSpan(Bytes(DBKeys::PAYMASTER_RESTORE_GUARD)), guard);
    if ((guard_status != DatabaseReadStatus::FOUND && guard_status != DatabaseReadStatus::NOT_FOUND) ||
        guard != journal.restore_guard) return false;
    for (const auto& row : journal.rows) {
        std::optional<RawBytes> current;
        const auto row_status = ReadRaw(batch, MakeByteSpan(row.key), current);
        if ((row_status != DatabaseReadStatus::FOUND && row_status != DatabaseReadStatus::NOT_FOUND) || current != row.before) return false;
    }
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    const auto identity_row = std::find_if(journal.rows.begin(), journal.rows.end(), [](const auto& row) {
        return row.key == Bytes(DBKeys::PAYMASTER_IDENTITY);
    });
    if (identity_row == journal.rows.end()) {
        if (batch.ReadWithStatus(DBKeys::PAYMASTER_IDENTITY, identity) != DatabaseReadStatus::FOUND) return false;
    } else {
        if (!identity_row->after) return false;
        try {
            SpanReader stream{CLIENT_VERSION, *identity_row->after};
            stream >> identity;
            if (!stream.empty()) return false;
        } catch (const std::exception&) { return false; }
    }
    if (identity.provider_id != journal.next.provider_id || !DigiDollar::Paymaster::ValidateProviderIdentityRecord(identity)) return false;
    return true;
}

bool ApplyJournalAfter(DatabaseBatch& batch, const MutationJournal& journal)
{
    for (const auto& row : journal.rows) {
        const auto key = MakeByteSpan(row.key);
        if (row.after ? !batch.Write(key, MakeByteSpan(*row.after), true) : !batch.Erase(key)) return false;
    }
    return batch.Write(CHECKPOINT_KEY, journal.next) && batch.Erase(JOURNAL_KEY);
}

bool HasBoundNativeJournal(const TransactionState& state, bool& bound)
{
    bound = false;
    if (!state.batch || !state.database) return false;
    PaymasterMaintenanceExecution execution;
    const auto status = state.batch->ReadWithStatus(std::string{"pmnativeexec"}, execution);
    if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) return false;
    if (status == DatabaseReadStatus::NOT_FOUND) {
        const auto original = state.mutations.find(Bytes(std::string{"pmnativeexec"}));
        if (original == state.mutations.end() || !original->second.before) return true;
        try {
            SpanReader stream{CLIENT_VERSION, *original->second.before};
            stream >> execution;
            if (!stream.empty()) return false;
        } catch (const std::exception&) { return false; }
    }
    if (!ValidatePaymasterMaintenanceExecution(execution)) return false;
    const auto hash = GetPaymasterMaintenanceExecutionHash(execution);
    const auto active = pending_executions.find(state.database);
    const bool target_bound = state.next.pending == 1 && state.next.token == hash;
    const bool active_bound = active != pending_executions.end() && active->second == hash &&
        state.before && state.before->pending == 1 && state.before->token == hash;
    bound = target_bound || active_bound;
    return bound;
}

bool PrepareMutationJournal(TransactionState& state)
{
    if (!state.batch || state.capture_failed) return false;
    MutationJournal journal;
    do { journal.nonce = GetRandHash(); } while (journal.nonce.IsNull());
    journal.before = state.before;
    journal.next = state.next;
    journal.restore_guard = state.restore_guard;
    journal.rows.reserve(state.mutations.size());
    for (auto& [key, mutation] : state.mutations) journal.rows.push_back(std::move(mutation));
    state.mutations.clear();
    journal.next.token = JournalHash(journal);
    if (!ValidJournal(journal)) return false;
    auto& batch = *state.batch;
    // No external authority has advanced yet. Persist just the immutable intent
    // in a separate wallet transaction; failure here leaves the old checkpoint.
    if (!batch.TxnAbort() || !batch.TxnBegin()) return false;
    if (!MatchesJournalBefore(batch, journal) || !batch.Write(JOURNAL_KEY, journal, false) || !batch.TxnCommit()) {
        batch.TxnAbort();
        return false;
    }
    if (!batch.TxnBegin()) return false;
    if (!MatchesJournalBefore(batch, journal) || !ApplyJournalAfter(batch, journal)) {
        batch.TxnAbort();
        return false;
    }
    state.next = journal.next;
    return true;
}
} // namespace

bool RecoverPaymasterCheckpointJournal(WalletDatabase& database, std::string& error)
{
    error.clear();
    auto batch = database.MakeBatch();
    MutationJournal journal;
    const auto status = batch->ReadWithStatus(JOURNAL_KEY, journal);
    if (status == DatabaseReadStatus::NOT_FOUND) return true;
    const auto fail = [&](const char* code) { error = code; return false; };
    if (status != DatabaseReadStatus::FOUND || !ValidJournal(journal))
        return fail("PAYMASTER_CHECKPOINT_JOURNAL_INVALID");
    TransactionState lock_owner;
    lock_owner.path = PaymasterCheckpointPath(journal.next.genesis_hash, journal.next.provider_id);
    lock_owner.file_state = FileState(lock_owner.path);
    if (!lock_owner.file_state) return fail("PAYMASTER_CHECKPOINT_JOURNAL_UNAVAILABLE");
    std::lock_guard<std::recursive_mutex> file_guard{lock_owner.file_state->mutex};
    if (lock_owner.file_state->transaction_active) return fail("PAYMASTER_CHECKPOINT_JOURNAL_BUSY");
    lock_owner.file_state->transaction_active = true;
    lock_owner.owns_file = true;
    try {
        const auto lock_path = fs::path{lock_owner.path.parent_path()} / fs::PathFromString(journal.next.provider_id.GetHex() + ".lock");
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(lock_path, ec))) return fail("PAYMASTER_CHECKPOINT_JOURNAL_UNAVAILABLE");
        lock_owner.file_lock = std::make_unique<fsbridge::FileLock>(lock_path);
        if (!lock_owner.file_lock->TryLock()) return fail("PAYMASTER_CHECKPOINT_JOURNAL_BUSY");
        PaymasterCheckpoint external;
        const auto external_status = ReadFile(lock_owner.path, external);
        const bool committed_intent = external_status == DatabaseReadStatus::FOUND && external == journal.next;
        const bool uncommitted_intent = journal.before
            ? external_status == DatabaseReadStatus::FOUND && external == *journal.before
            : external_status == DatabaseReadStatus::NOT_FOUND;
        if ((!committed_intent && !uncommitted_intent) || !batch->TxnBegin())
            return fail("PAYMASTER_CHECKPOINT_JOURNAL_CONFLICT");
        // Re-read inside the transaction and under the same file/OS lock. An
        // unbound preparation-only intent is discarded, never spending authority.
        MutationJournal current;
        if (batch->ReadWithStatus(JOURNAL_KEY, current) != DatabaseReadStatus::FOUND ||
            !ValidJournal(current) || current.next.token != journal.next.token || !(current.next == journal.next) ||
            !MatchesJournalBefore(*batch, journal) ||
            (committed_intent ? !ApplyJournalAfter(*batch, journal) : !batch->Erase(JOURNAL_KEY)) || !batch->TxnCommit()) {
            batch->TxnAbort();
            return fail("PAYMASTER_CHECKPOINT_JOURNAL_RECOVERY_FAILED");
        }
        ForgetPaymasterSessionIndex(database);
        ForgetPaymasterFinanceCache(database);
        ForgetPaymasterMaintenanceCache(database);
        return true;
    } catch (const std::exception&) {
        batch->TxnAbort();
        return fail("PAYMASTER_CHECKPOINT_JOURNAL_UNAVAILABLE");
    }
}

bool PaymasterCheckpointPrepareCommit(const WalletBatch* owner)
{
    const auto transaction = FindTransaction(owner);
    if (!transaction) return true;
    std::lock_guard<std::recursive_mutex> lock{transaction->mutex};
    auto& state = *transaction;
    if (state.failed) return false;
    if (!state.dirty) return true;
    try {
        std::lock_guard<std::recursive_mutex> file_guard{state.file_state->mutex};
        bool native{false};
        if (!HasBoundNativeJournal(state, native) || (!native && !PrepareMutationJournal(state))) return false;
        return WriteFile(state.path, state.next);
    } catch (const std::exception&) {
        return false;
    }
}

void PaymasterCheckpointEnd(const WalletBatch* owner)
{
    // Destroy (and release its OS lock) outside the registry critical section.
    std::shared_ptr<TransactionState> retired;
    {
        std::lock_guard<std::mutex> lock{registry_mutex};
        const auto found = transactions.find(owner);
        if (found == transactions.end()) return;
        retired = std::move(found->second);
        transactions.erase(found);
    }
}

bool PaymasterCheckpointTracked(const WalletBatch* owner)
{
    const auto state = FindTransaction(owner);
    if (!state) return false;
    std::lock_guard<std::recursive_mutex> lock{state->mutex};
    return state->dirty || state->failed;
}

void PaymasterCheckpointFailed(const WalletBatch* owner)
{
    const auto state = FindTransaction(owner);
    if (!state) return;
    std::lock_guard<std::recursive_mutex> lock{state->mutex};
    state->failed = true;
}

bool WalletBatch::SetPaymasterCheckpointPending(bool pending, const uint256& execution_hash)
{
    const bool own = !PaymasterCheckpointInTransaction(this);
    if (own && !TxnBegin()) return false;
    if (!PaymasterCheckpointWrite(this, *m_batch, m_database)) {
        TxnAbort();
        return false;
    }
    {
        const auto transaction = FindTransaction(this);
        if (!transaction) { TxnAbort(); return false; }
        std::lock_guard<std::recursive_mutex> lock{transaction->mutex};
        auto& state = *transaction;
        if (!state.dirty) {
            TxnAbort();
            return false;
        }
        state.next.pending = pending ? 1 : 0;
        if (pending && !execution_hash.IsNull()) state.next.token = execution_hash;
        if (!pending) state.next.token = GetRandHash();
        if (!m_batch->Write(CHECKPOINT_KEY, state.next)) {
            TxnAbort();
            return false;
        }
    }
    return !own || TxnCommit();
}

PaymasterCheckpointOperation::~PaymasterCheckpointOperation()
{
    if (m_active) pending_operations.erase(&m_wallet.GetDatabase());
    if (m_active) pending_executions.erase(&m_wallet.GetDatabase());
}

bool PaymasterCheckpointOperation::Begin(std::string& error)
{
    AssertLockHeld(m_wallet.cs_wallet);
    if (m_active || pending_operations.count(&m_wallet.GetDatabase()) ||
        !CheckPaymasterCheckpoint(m_wallet.GetDatabase(), error)) return false;
    if (!WalletBatch{m_wallet.GetDatabase()}.SetPaymasterCheckpointPending(true)) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_WRITE";
        return false;
    }
    pending_operations.insert(&m_wallet.GetDatabase());
    m_active = true;
    return true;
}

bool PaymasterCheckpointOperation::Complete(std::string& error)
{
    AssertLockHeld(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (!m_execution_hash.IsNull()) {
        PaymasterMaintenanceExecution saved;
        if (batch.ReadPaymasterMaintenanceExecutionWithStatus(saved) != DatabaseReadStatus::FOUND ||
            GetPaymasterMaintenanceExecutionHash(saved) != m_execution_hash) {
            error = "PAYMASTER_MAINTENANCE_RECOVERY_REVIEW_REQUIRED";
            return false;
        }
    }
    if (!m_active || !batch.TxnBegin() ||
        (!m_execution_hash.IsNull() && !batch.ErasePaymasterMaintenanceExecution()) ||
        !batch.SetPaymasterCheckpointPending(false) || !batch.TxnCommit()) {
        batch.TxnAbort();
        error = "PAYMASTER_PROVIDER_CHECKPOINT_WRITE";
        return false;
    }
    pending_operations.erase(&m_wallet.GetDatabase());
    pending_executions.erase(&m_wallet.GetDatabase());
    m_active = false;
    return true;
}

bool PaymasterCheckpointOperation::Begin(const PaymasterMaintenanceExecution& execution, std::string& error)
{
    AssertLockHeld(m_wallet.cs_wallet);
    DigiDollar::Paymaster::ProviderIdentityRecord guard;
    if (m_active || pending_operations.count(&m_wallet.GetDatabase()) ||
        WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterRestoreGuardWithStatus(guard) != DatabaseReadStatus::NOT_FOUND ||
        !ValidatePaymasterMaintenanceExecution(execution) || !CheckPaymasterCheckpoint(m_wallet.GetDatabase(), error)) return false;
    const auto commitment = GetPaymasterMaintenanceExecutionHash(execution);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (!batch.TxnBegin() || !batch.WritePaymasterMaintenanceExecution(execution) ||
        !batch.SetPaymasterCheckpointPending(true, commitment) || !batch.TxnCommit()) {
        batch.TxnAbort();
        error = "PAYMASTER_PROVIDER_CHECKPOINT_WRITE";
        return false;
    }
    pending_operations.insert(&m_wallet.GetDatabase());
    pending_executions[&m_wallet.GetDatabase()] = commitment;
    m_execution_hash = commitment;
    m_active = true;
    return true;
}

bool PaymasterCheckpointOperation::Resume(const PaymasterMaintenanceExecution& execution, std::string& error)
{
    AssertLockHeld(m_wallet.cs_wallet);
    error.clear();
    WalletBatch records{m_wallet.GetDatabase()};
    DigiDollar::Paymaster::ProviderIdentityRecord identity, guard;
    PaymasterMaintenanceExecution saved;
    if (m_active || pending_operations.count(&m_wallet.GetDatabase()) || !ValidatePaymasterMaintenanceExecution(execution) ||
        records.ReadPaymasterIdentityWithStatus(identity) != DatabaseReadStatus::FOUND || identity.provider_id != execution.provider_id ||
        records.ReadPaymasterRestoreGuardWithStatus(guard) != DatabaseReadStatus::NOT_FOUND ||
        records.ReadPaymasterMaintenanceExecutionWithStatus(saved) != DatabaseReadStatus::FOUND ||
        GetPaymasterMaintenanceExecutionHash(saved) != GetPaymasterMaintenanceExecutionHash(execution)) {
        error = "PAYMASTER_MAINTENANCE_RECOVERY_REVIEW_REQUIRED";
        return false;
    }
    const auto commitment = GetPaymasterMaintenanceExecutionHash(execution);
    auto raw = m_wallet.GetDatabase().MakeBatch();
    PaymasterCheckpoint current;
    if (!ReadResumableCurrent(*raw, identity.provider_id, commitment, current)) {
        error = "PAYMASTER_MAINTENANCE_RECOVERY_CHECKPOINT_CHANGED";
        return false;
    }
    pending_operations.insert(&m_wallet.GetDatabase());
    pending_executions[&m_wallet.GetDatabase()] = commitment;
    m_active = true;
    m_execution_hash = commitment;
    if (!WalletBatch{m_wallet.GetDatabase()}.SetPaymasterCheckpointPending(true, commitment)) {
        pending_operations.erase(&m_wallet.GetDatabase());
        pending_executions.erase(&m_wallet.GetDatabase());
        m_active = false;
        error = "PAYMASTER_PROVIDER_CHECKPOINT_WRITE";
        return false;
    }
    return true;
}
} // namespace wallet
