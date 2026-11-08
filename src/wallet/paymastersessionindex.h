// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERSESSIONINDEX_H
#define DIGIBYTE_WALLET_PAYMASTERSESSIONINDEX_H

#include <paymaster/reservation.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wallet {
class WalletBatch;
class WalletDatabase;

/** Disposable scheduling progress, never financial authority. Independent
 * scans must not advance one another. Wallet unload discards all progress. */
enum class PaymasterScan { QUOTE_EXPIRY, RECONCILIATION, CLIENT_FINALS, CAPACITY_SNAPSHOTS, RECOVERIES, PROVIDER_COMMITS, SIGNED_ATTEMPTS, POOL_SUCCESSORS };
struct PaymasterScanPosition {
    std::string request;
    uint256 record;
};
PaymasterScanPosition GetPaymasterScanPosition(WalletDatabase& database, PaymasterScan scan);
void SetPaymasterScanPosition(WalletDatabase& database, PaymasterScan scan, const PaymasterScanPosition& position);
/** Take and reset progress before reading. Publish a continuation only after
 * the entire page succeeds; any early return restarts failed work. */
class PaymasterScanPage {
    WalletDatabase& m_database;
    PaymasterScan m_scan;
public:
    const PaymasterScanPosition after;
    PaymasterScanPage(WalletDatabase& database, PaymasterScan scan);
    void Complete(const std::string& next);
    void Complete(const uint256& next);
};
constexpr size_t PAYMASTER_BACKGROUND_SESSION_ROWS{64};
constexpr size_t PAYMASTER_BACKGROUND_SESSION_BYTES{64 * 8192};

/** Derived lookup only. Caller holds the wallet lock; returned records are
 * freshly read and still require exact-artifact/authorization validation. */
bool ListPaymasterFinalSessions(WalletBatch& batch, WalletDatabase& database,
                                const uint256& txid,
                                std::vector<DigiDollar::Paymaster::PaymentSession>& sessions);
/** Derived candidate selection for generic rebroadcast exclusion. Each hit is
 * freshly read; a miss is never signing, payment or release authority. */
bool ListPaymasterProviderCommitsForTransaction(WalletBatch& batch, WalletDatabase& database,
    const uint256& txid, std::vector<DigiDollar::Paymaster::ProviderCommitRecord>& commits);
void PrimePaymasterSessionIndex(WalletDatabase& database,
                                const std::vector<DigiDollar::Paymaster::PaymentSession>& sessions);
void ForgetPaymasterSessionIndex(WalletDatabase& database);
/** Non-authoritative progress through the bounded provider replay-cache scan.
 * Reset on read/commit failure and wallet unload; never grants a release. */
uint256 GetPaymasterCapacityCleanupCursor(WalletDatabase& database);
void SetPaymasterCapacityCleanupCursor(WalletDatabase& database, const uint256& after);
/** Apply queued index deltas only after the database commit has succeeded.
 * Abort/destruction discards them and invalidates the derived lookup. */
void PaymasterSessionIndexEnd(const WalletBatch* owner, bool committed = false);
} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERSESSIONINDEX_H
