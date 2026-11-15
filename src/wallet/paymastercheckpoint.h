// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERCHECKPOINT_H
#define DIGIBYTE_WALLET_PAYMASTERCHECKPOINT_H

#include <serialize.h>
#include <span.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {
struct ProviderPoolEntry;
struct ProviderMaintenanceLedger;
}

namespace wallet {
class CWallet;
class DatabaseBatch;
class WalletBatch;
class WalletDatabase;
struct PaymasterMaintenanceExecution;

enum class PaymasterMaintenanceAccounting { MISMATCH, BEFORE, AFTER };
/** Classify already validated immutable accounting snapshots. This grants no
 * signing or execution authority; callers still validate records and identity. */
PaymasterMaintenanceAccounting ClassifyPaymasterMaintenanceAccounting(
    const PaymasterMaintenanceExecution& execution, bool has_pool,
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& pool,
    const DigiDollar::Paymaster::ProviderMaintenanceLedger* ledger);

/** No keys or transaction contents leave the wallet. Tokens distinguish
 * independently advanced copies; a pending native token binds its exact
 * execution and predecessor through wallet-only checkpoint metadata. */
struct PaymasterCheckpoint {
    // Version 2 retains the layout and reads version 1, adding permanent
    // retirement. Older binaries reject the new version rather than resume.
    static constexpr uint32_t CURRENT_VERSION{2};
    static constexpr uint8_t RETIRED{2};
    uint32_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    uint256 provider_id;
    uint64_t generation{0};
    uint256 token;
    uint8_t pending{0};
    SERIALIZE_METHODS(PaymasterCheckpoint, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.generation, obj.token, obj.pending);
    }
    bool operator==(const PaymasterCheckpoint& other) const;
};

fs::path PaymasterCheckpointPath(const uint256& genesis, const uint256& provider);
bool CheckPaymasterCheckpoint(WalletDatabase& database, std::string& error);

/** Finish only an independently committed exact database intent, before wallet
 * records/flags/coin-lock owners are loaded into memory. Never a live repair or
 * a stale-backup adoption operation. Unknown or inconsistent intent is retained. */
bool RecoverPaymasterCheckpointJournal(WalletDatabase& database, std::string& error);
/** Only a proven absent intent permits interpreting missing native execution
 * as unused capacity. Unknown/corrupt/unreadable intent retains the fence. */
bool PaymasterCheckpointJournalIsAbsent(DatabaseBatch& batch);

// Record the complete raw mutation of a WalletBatch transaction, including
// ordinary flags and coin-lock rows sharing a Paymaster financial transition.
// Transactions without a Paymaster checkpoint keep the ordinary backend flow.
bool WritePaymasterCheckpointRecord(const WalletBatch* owner, DatabaseBatch& batch,
    Span<const std::byte> key, Span<const std::byte> value, bool overwrite);
bool ErasePaymasterCheckpointRecord(const WalletBatch* owner, DatabaseBatch& batch,
    Span<const std::byte> key);
bool ErasePaymasterCheckpointPrefix(const WalletBatch* owner, DatabaseBatch& batch,
    Span<const std::byte> prefix);

/** Read-only recovery proof: an explicitly restored wallet must match the
 * independently retained, enrolled checkpoint and have no unfinished fence.
 * The revision must be rechecked under the checkpoint lock at execution. */
bool CheckPaymasterCapitalRecoveryCheckpoint(WalletDatabase& database,
                                            uint256& revision, std::string& error);

// WalletBatch transaction notifications. Ordinary wallet writes do not perform
// checkpoint I/O. State belongs to this Paymaster module, not to WalletBatch.
void PaymasterCheckpointBegin(const WalletBatch* owner);
bool PaymasterCheckpointWrite(const WalletBatch* owner, DatabaseBatch& batch, WalletDatabase& database);
bool PaymasterCheckpointPrepareCommit(const WalletBatch* owner);
bool PaymasterCheckpointTracked(const WalletBatch* owner);
void PaymasterCheckpointFailed(const WalletBatch* owner);
/** Release only checkpoint state and its locks. WalletBatch owns the common
 * transaction completion, including derived finance/session notifications. */
void PaymasterCheckpointEnd(const WalletBatch* owner);
bool PaymasterCheckpointInTransaction(const WalletBatch* owner);

/** Fence wallet-native broadcasts whose transaction and Paymaster accounting
 * cannot share a database transaction. An unfinished fence survives restart;
 * only its exact immutable journal can resume through the pending marker. */
class PaymasterCheckpointOperation {
public:
    explicit PaymasterCheckpointOperation(CWallet& wallet) : m_wallet(wallet) {}
    ~PaymasterCheckpointOperation();
    PaymasterCheckpointOperation(const PaymasterCheckpointOperation&) = delete;
    PaymasterCheckpointOperation& operator=(const PaymasterCheckpointOperation&) = delete;
    bool Active() const { return m_active; }
    bool Begin(std::string& error);
    bool Begin(const PaymasterMaintenanceExecution& execution, std::string& error);
    /** Resume only this exact immutable journal and predecessor binding against its external token.
     * Repair an adjacent DB gap without advancing the file. Success with
     * Active()==false means an interrupted Complete was already externalized;
     * its exact cleanup was committed and financial work must not be repeated. */
    bool Resume(const PaymasterMaintenanceExecution& execution, std::string& error);
    bool Complete(std::string& error);
private:
    CWallet& m_wallet;
    bool m_active{false};
    uint256 m_execution_hash;
};
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERCHECKPOINT_H
