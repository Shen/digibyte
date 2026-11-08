// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERFINANCE_H
#define DIGIBYTE_WALLET_PAYMASTERFINANCE_H

#include <paymaster/error.h>
#include <paymaster/finance.h>
#include <primitives/transaction.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace wallet {
class CWallet;

/** A captured accounting observation, never payment or release authority. */
struct ProviderFinancePoolCapital {
    DigiDollar::Paymaster::DGBSatoshis dgb_available;
    DigiDollar::Paymaster::DGBSatoshis dgb_reserved;
    DigiDollar::Paymaster::DGBSatoshis dgb_pending;
    DigiDollar::Paymaster::DDCents carrier_base;
    DigiDollar::Paymaster::DDCents carrier_earned;
    DigiDollar::Paymaster::DDCents carrier_withdrawable;
    uint64_t pending_maintenance_transactions{0};
};

struct ProviderFinanceValuation {
    CAmount oracle_price_micro_usd{0};
    int64_t time{0};
    double estimated_result_usd{0};
};

struct ProviderFinanceStatus {
    DigiDollar::Paymaster::ProviderFinanceQuery query;
    DigiDollar::Paymaster::PaymasterId provider_id;
    DigiDollar::Paymaster::ProviderFinanceReport report;
    DigiDollar::Paymaster::ProviderBackupStatus backup;
    bool backup_required{false};
    std::array<DigiDollar::Paymaster::DDCents, 4> average_service_fee{};
    ProviderFinancePoolCapital pool_capital;
    std::optional<ProviderFinanceValuation> valuation;
};

struct ProviderFinanceStatusFailure {
    DigiDollar::Paymaster::ErrorKind kind{DigiDollar::Paymaster::ErrorKind::WALLET};
    std::string code;
    // Some existing callers expose only the category/code. A present
    // disposition retains the established structured reconciliation contract.
    std::optional<DigiDollar::Paymaster::ErrorDisposition> disposition;
};

using ProviderFinanceStatusResult = std::variant<ProviderFinanceStatus, ProviderFinanceStatusFailure>;
/** Wait for wallet notifications before taking the wallet-locked accounting
 * snapshot. A zero query time captures the clock after that wait; an explicit
 * time supports deterministic callers. All displayed values use that time. */
ProviderFinanceStatusResult CaptureProviderFinanceStatus(
    CWallet& wallet, DigiDollar::Paymaster::ProviderFinanceQuery query);

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
/** Read a source-checked summary without scanning history or granting authority.
 * Reconciliation must have completed; damaged derivations invalidate the cache. */
bool ReadProviderFinanceSummary(CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceMetadata& summary, std::string& error);
std::optional<DigiDollar::Paymaster::DGBSatoshis> GetProviderFinanceTransactionFee(
    CWallet& wallet, const CTransactionRef& transaction);
bool RecordProviderFinanceTransaction(CWallet& wallet,
    DigiDollar::Paymaster::ProviderFinanceEventKind kind,
    const CTransactionRef& transaction, DigiDollar::Paymaster::DGBSatoshis dgb_cost,
    int64_t now, std::string& error);
} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERFINANCE_H
