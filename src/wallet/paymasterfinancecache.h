// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERFINANCECACHE_H
#define DIGIBYTE_WALLET_PAYMASTERFINANCECACHE_H
#include <uint256.h>
#include <string>
#include <utility>
#include <vector>
namespace DigiDollar::Paymaster {
struct ProviderAttempt;
struct ProviderCommitRecord;
struct ProviderMaintenanceLedger;
struct ProviderPoolEntry;
struct AlternativeRecoveryRecord;
struct ProviderFinanceEvent;
}
namespace wallet {
class WalletBatch;
class WalletDatabase;
// Lifecycle only; cached reconciliation never authorizes spending.
void ForgetPaymasterFinanceCache(WalletDatabase& database);
/** Queue source observations inside the owning WalletBatch transaction. They
 * become visible only after commit; rollback/destruction discards them. */
template <typename T>
void QueuePaymasterFinanceRecord(const WalletBatch*, WalletDatabase&, const T&) {}
void QueuePaymasterFinanceRecord(const WalletBatch*, WalletDatabase&,
                                 const DigiDollar::Paymaster::ProviderAttempt&);
void QueuePaymasterFinanceRecord(const WalletBatch*, WalletDatabase&,
                                 const DigiDollar::Paymaster::ProviderCommitRecord&);
void QueuePaymasterFinanceRecord(const WalletBatch*, WalletDatabase&,
                                 const DigiDollar::Paymaster::AlternativeRecoveryRecord&);
void QueuePaymasterFinanceRecord(const WalletBatch*, WalletDatabase&,
                                 const DigiDollar::Paymaster::ProviderMaintenanceLedger&);
void QueuePaymasterFinanceRecord(const WalletBatch*, WalletDatabase&,
                                 const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>&);
void QueuePaymasterFinanceEvent(const WalletBatch*, WalletDatabase&,
                               const DigiDollar::Paymaster::ProviderFinanceEvent&,
                               const DigiDollar::Paymaster::ProviderFinanceEvent* previous, bool reconcile);
void QueuePaymasterFinanceRebuild(const WalletBatch*, WalletDatabase&);
template <typename K>
void QueuePaymasterFinanceErase(const WalletBatch*, WalletDatabase&, const K&) {}
/** Publish removed source bindings only after the authoritative erase commits. */
void QueuePaymasterFinanceErase(const WalletBatch*, WalletDatabase&,
                               const std::pair<std::string, uint256>&);
void PaymasterFinanceTransactionEnd(const WalletBatch*, bool committed = false);
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERFINANCECACHE_H
