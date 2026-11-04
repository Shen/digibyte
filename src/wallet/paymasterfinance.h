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
/** Subsequent report pages may reuse a successful reconciliation while the
 * wallet database and processed chain tip are unchanged. The first page always
 * reconciles. Errors are never cached; reopening a wallet starts cold. */
bool ReconcileProviderFinanceReport(CWallet& wallet, bool continuation, std::string& error);
std::optional<DigiDollar::Paymaster::DGBSatoshis> GetProviderFinanceTransactionFee(
    CWallet& wallet, const CTransactionRef& transaction);
bool RecordProviderFinanceTransaction(CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceEventKind kind,
    const CTransactionRef& transaction, DigiDollar::Paymaster::DGBSatoshis dgb_cost,
    int64_t now, std::string& error);
} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERFINANCE_H
