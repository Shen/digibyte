// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/paymastercheckpoint.h>

#include <chainparams.h>
#include <hash.h>
#include <logging.h>
#include <random.h>
#include <streams.h>
#include <util/fs_helpers.h>
#include <util/system.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <array>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>

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

struct TransactionState {
    std::unique_ptr<fsbridge::FileLock> file_lock;
    fs::path path;
    PaymasterCheckpoint next;
    bool dirty{false};
    bool failed{false};
};
std::recursive_mutex checkpoint_mutex;
std::map<const WalletBatch*, TransactionState> transactions;
thread_local std::set<WalletDatabase*> pending_operations;

bool Valid(const PaymasterCheckpoint& record, const uint256& provider)
{
    return record.version == PaymasterCheckpoint::CURRENT_VERSION &&
           record.genesis_hash == Params().GenesisBlock().GetHash() &&
           record.provider_id == provider && !provider.IsNull() &&
           record.generation > 0 && !record.token.IsNull() && record.pending <= 1;
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
    if (enrolled && current.pending && !pending_operations.count(&database)) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION";
        return false;
    }
    return true;
}

void PaymasterCheckpointBegin(const WalletBatch* owner)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    transactions.try_emplace(owner);
}

bool PaymasterCheckpointInTransaction(const WalletBatch* owner)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    return transactions.count(owner) != 0;
}

bool PaymasterCheckpointWrite(const WalletBatch* owner, DatabaseBatch& batch, WalletDatabase& database)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    const auto found = transactions.find(owner);
    if (found == transactions.end()) return false;
    auto& state = found->second;
    if (state.failed) return false;
    if (state.dirty) return true;
    // Explicitly restored wallets remain permanently quarantined by the
    // append-only guard. Their exact signed recovery/accounting must not
    // overwrite the original provider's independent high-water mark.
    if (batch.Exists(DBKeys::PAYMASTER_RESTORE_GUARD)) return true;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    const auto identity_status = batch.ReadWithStatus(DBKeys::PAYMASTER_IDENTITY, identity);
    if (identity_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (identity_status != DatabaseReadStatus::FOUND || identity.version != DigiDollar::Paymaster::ProviderIdentityRecord::CURRENT_VERSION || identity.provider_id.IsNull()) {
        state.failed = true;
        return false;
    }
    state.path = PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), identity.provider_id);
    try {
        // POSIX fcntl locks belong to the process, not to individual threads.
        // Refuse a second in-process transaction before opening its lock file
        // (closing that second descriptor would also release the first lock).
        for (const auto& other : transactions) {
            if (other.first != owner && other.second.dirty && other.second.path == state.path) throw std::runtime_error("checkpoint busy");
        }
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
        if (!ReadCurrent(batch, identity.provider_id, state.next, enrolled) ||
            (enrolled && state.next.pending && !pending_operations.count(&database)) ||
            state.next.generation == (std::numeric_limits<uint64_t>::max)()) throw std::runtime_error("checkpoint not current");
        state.next.version = PaymasterCheckpoint::CURRENT_VERSION;
        state.next.genesis_hash = Params().GenesisBlock().GetHash();
        state.next.provider_id = identity.provider_id;
        ++state.next.generation;
        state.next.token = GetRandHash();
        if (!batch.Write(CHECKPOINT_KEY, state.next)) throw std::runtime_error("checkpoint write failed");
        state.dirty = true;
        return true;
    } catch (const std::exception& exception) {
        LogPrint(BCLog::DIGIDOLLAR, "Paymaster checkpoint mutation refused: %s\n", exception.what());
        state.failed = true;
        return false;
    }
}

bool PaymasterCheckpointPrepareCommit(const WalletBatch* owner)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    const auto found = transactions.find(owner);
    if (found == transactions.end()) return true;
    auto& state = found->second;
    if (state.failed) return false;
    if (!state.dirty) return true;
    try {
        return WriteFile(state.path, state.next);
    } catch (const std::exception&) {
        return false;
    }
}

void PaymasterCheckpointEnd(const WalletBatch* owner)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    transactions.erase(owner);
}

bool PaymasterCheckpointTracked(const WalletBatch* owner)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    const auto found = transactions.find(owner);
    return found != transactions.end() && (found->second.dirty || found->second.failed);
}

void PaymasterCheckpointFailed(const WalletBatch* owner)
{
    std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
    const auto found = transactions.find(owner);
    if (found != transactions.end()) found->second.failed = true;
}

bool WalletBatch::SetPaymasterCheckpointPending(bool pending)
{
    if (!TxnBegin()) return false;
    if (!PaymasterCheckpointWrite(this, *m_batch, m_database)) {
        TxnAbort();
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lock{checkpoint_mutex};
        auto& state = transactions.at(this);
        if (!state.dirty) {
            TxnAbort();
            return false;
        }
        state.next.pending = pending ? 1 : 0;
        if (!m_batch->Write(CHECKPOINT_KEY, state.next)) {
            TxnAbort();
            return false;
        }
    }
    return TxnCommit();
}

PaymasterCheckpointOperation::~PaymasterCheckpointOperation()
{
    if (m_active) pending_operations.erase(&m_wallet.GetDatabase());
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
    if (!m_active || !WalletBatch{m_wallet.GetDatabase()}.SetPaymasterCheckpointPending(false)) {
        error = "PAYMASTER_PROVIDER_CHECKPOINT_WRITE";
        return false;
    }
    pending_operations.erase(&m_wallet.GetDatabase());
    m_active = false;
    return true;
}
} // namespace wallet
