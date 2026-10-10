// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERCHECKPOINT_H
#define DIGIBYTE_WALLET_PAYMASTERCHECKPOINT_H

#include <serialize.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <string>

namespace wallet {
class CWallet;
class DatabaseBatch;
class WalletBatch;
class WalletDatabase;

/** No keys or transaction contents leave the wallet. The random token also
 * distinguishes independently advanced copies at the same generation. */
struct PaymasterCheckpoint {
    static constexpr uint32_t CURRENT_VERSION{1};
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

// WalletBatch transaction notifications. Ordinary wallet writes do not perform
// checkpoint I/O. State belongs to this Paymaster module, not to WalletBatch.
void PaymasterCheckpointBegin(const WalletBatch* owner);
bool PaymasterCheckpointWrite(const WalletBatch* owner, DatabaseBatch& batch, WalletDatabase& database);
bool PaymasterCheckpointPrepareCommit(const WalletBatch* owner);
bool PaymasterCheckpointTracked(const WalletBatch* owner);
void PaymasterCheckpointFailed(const WalletBatch* owner);
void PaymasterCheckpointEnd(const WalletBatch* owner);
bool PaymasterCheckpointInTransaction(const WalletBatch* owner);

/** Fence wallet-native broadcasts whose transaction and Paymaster accounting
 * cannot share a database transaction. An unfinished fence survives restart;
 * only this live operation may continue writing through its pending marker. */
class PaymasterCheckpointOperation {
public:
    explicit PaymasterCheckpointOperation(CWallet& wallet) : m_wallet(wallet) {}
    ~PaymasterCheckpointOperation();
    PaymasterCheckpointOperation(const PaymasterCheckpointOperation&) = delete;
    PaymasterCheckpointOperation& operator=(const PaymasterCheckpointOperation&) = delete;
    bool Active() const { return m_active; }
    bool Begin(std::string& error);
    bool Complete(std::string& error);
private:
    CWallet& m_wallet;
    bool m_active{false};
};
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERCHECKPOINT_H
