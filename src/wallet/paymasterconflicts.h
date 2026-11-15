// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_PAYMASTERCONFLICTS_H
#define DIGIBYTE_WALLET_PAYMASTERCONFLICTS_H

#include <paymaster/conflict.h>

#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class COutPoint;
namespace DigiDollar::Paymaster { struct ProviderPoolEntry; struct ProviderMaintenanceLedger; }
namespace wallet {
class CWallet;
class WalletBatch;
struct PaymasterMaintenanceExecution;
struct ExactFinalTransactionPreflight;
using PaymasterConflictClosure = DigiDollar::Paymaster::ProviderConflictClosure;

/** Only the canonical verifier constructs this observation. Callers recheck
 * while holding cs_main through the accounting commit. */
class PaymasterInputConflictProof {
    PaymasterConflictClosure m_record;
    uint256 m_tip, m_block;
    uint32_t m_transactions;
    explicit PaymasterInputConflictProof(PaymasterConflictClosure record, uint256 tip, const DigiDollar::Paymaster::ConflictInclusion& inclusion)
        : m_record(std::move(record)), m_tip(tip), m_block(inclusion.block_hash), m_transactions(inclusion.transactions) {}
    friend std::optional<PaymasterInputConflictProof> VerifyPaymasterInputConflict(
        CWallet&, const CTransaction&, const uint256&, const uint256&,
        const PaymasterConflictClosure&, std::string&);
public:
    const PaymasterConflictClosure& Record() const { return m_record; }
    bool Recheck(CWallet& wallet, std::string& error) const;
    bool Recheck(CWallet& wallet, const CTransaction& original, const uint256& provider,
                 const uint256& authority, std::string& error) const;
};
std::optional<PaymasterInputConflictProof> VerifyPaymasterInputConflict(
    CWallet& wallet, const CTransaction& original, const uint256& provider,
    const uint256& authority, const PaymasterConflictClosure& record, std::string& error);
/** Bounded observation also covers commits never inserted into mapWallet.
 * Missing evidence, blocks and disposable progress never free funds. */
std::optional<PaymasterInputConflictProof> ObservePaymasterInputConflict(
    CWallet& wallet, const CTransaction& original, const uint256& provider,
    const uint256& authority, int64_t now, std::string& error);
bool PaymasterConflictClosureIsAbsent(WalletBatch& batch, const uint256& txid, std::string& error);
bool ReadSettledPaymasterConflict(CWallet& wallet, WalletBatch& batch, const uint256& txid,
    const uint256& provider, const uint256& authority, bool& settled, std::string& error,
    std::optional<PaymasterInputConflictProof>* snapshot = nullptr);
/** Prepare an accounting after-image without writes. Settled sources remain
 * present until their new dispositions commit, including at a full pool. */
bool PreparePaymasterConflictPool(CWallet& wallet, WalletBatch& batch,
    const CTransaction& original, const std::vector<COutPoint>& sources, const uint256& owner,
    int64_t now, bool pending_native, std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& pool,
    std::string& error);
bool ReconcilePaymasterProviderConflicts(CWallet& wallet, int64_t now, size_t& closed, std::string& error);

/** Alternate native accounting is constructible only after every immutable
 * leg is proved confirmed or defeated by a mature input conflict. */
class ValidatedPaymasterConflictCompletion {
    friend class PaymasterCheckpointOperation;
    friend std::optional<ValidatedPaymasterConflictCompletion> PreparePaymasterNativeConflictCompletion(
        CWallet&, const PaymasterMaintenanceExecution&, std::string&);
    ValidatedPaymasterConflictCompletion() = default;
    std::shared_ptr<const PaymasterMaintenanceExecution> m_execution;
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry> m_pool;
    std::shared_ptr<DigiDollar::Paymaster::ProviderMaintenanceLedger> m_ledger;
    std::vector<std::pair<std::shared_ptr<const CTransaction>, PaymasterInputConflictProof>> m_conflicts;
    std::vector<std::pair<std::shared_ptr<const CTransaction>, std::shared_ptr<const ExactFinalTransactionPreflight>>> m_confirmed;
    bool Recheck(CWallet& wallet, std::string& error) const;
    bool PersistConfirmed(CWallet& wallet, std::string& error) const;
};
std::optional<ValidatedPaymasterConflictCompletion> PreparePaymasterNativeConflictCompletion(
    CWallet& wallet, const PaymasterMaintenanceExecution& execution, std::string& error);

} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERCONFLICTS_H
