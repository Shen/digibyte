// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERFINANCE_H
#define DIGIBYTE_WALLET_PAYMASTERFINANCE_H

#include <paymaster/provider.h>
#include <primitives/transaction.h>

#include <optional>
#include <string>

namespace wallet {
class CWallet;

// Durable wallet-native retirement marker, written before broadcast.
inline constexpr const char* PAYMASTER_RETIREMENT_PROVIDER_KEY{"paymaster_retirement_provider"};

bool ReconcileProviderFinances(CWallet& wallet, size_t& changed_events, std::string& error);
/** Full reconstruction for wallet load, restore, migration or explicit repair.
 * Normal reconciliation uses transaction notifications and committed source
 * changes, with at most this many jobs/events in one wallet-locked step. */
inline constexpr size_t MAX_FINANCE_RECONCILIATION_WORK{256};
bool RebuildProviderFinances(CWallet& wallet, size_t& changed_events, std::string& error);
/** Both first and continuation pages use bounded incremental reconciliation.
 * A backlog returns PAYMASTER_FINANCE_RECONCILING, never stale current totals. */
bool ReconcileProviderFinanceReport(CWallet& wallet, bool continuation, std::string& error);
/** Requires cs_wallet; only observes the derived work state. It performs no
 * reconciliation or database reads and never grants payment authority. */
bool AreProviderFinancesCurrent(const CWallet& wallet);
std::optional<DigiDollar::Paymaster::DGBSatoshis> GetProviderFinanceTransactionFee(
    CWallet& wallet, const CTransactionRef& transaction);
bool RecordProviderFinanceTransaction(CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceEventKind kind,
    const CTransactionRef& transaction, DigiDollar::Paymaster::DGBSatoshis dgb_cost,
    int64_t now, std::string& error);
} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERFINANCE_H
