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
enum class PaymasterScan { QUOTE_EXPIRY, RECONCILIATION, CLIENT_FINALS, CAPACITY_SNAPSHOTS, RECOVERIES, PROVIDER_COMMITS, SIGNED_ATTEMPTS, POOL_SUCCESSORS,
                          RELAY_PROVIDER_COMMITS, RELAY_SIGNED_ATTEMPTS, RELAY_CLIENT_FINALS };
struct PaymasterScanPosition {
    std::string request;
    uint256 record;
};
PaymasterScanPosition GetPaymasterScanPosition(WalletDatabase& database, PaymasterScan scan);
void SetPaymasterScanPosition(WalletDatabase& database, PaymasterScan scan, const PaymasterScanPosition& position);
/** One randomized relay opportunity drains bounded pages across later ticks.
 * Observations have separate cursors; this disposable progress grants no
 * payment authority and is discarded on wallet unload or policy cancellation. */
struct PaymasterRelayRound {
    bool active{false};
    bool provider_commits_complete{false};
    bool signed_attempts_complete{false};
    bool client_finals_complete{false};
    bool Complete() const { return active && provider_commits_complete && signed_attempts_complete && client_finals_complete; }
};
PaymasterRelayRound GetPaymasterRelayRound(WalletDatabase& database);
bool BeginPaymasterRelayRound(WalletDatabase& database);
void SetPaymasterRelayRound(WalletDatabase& database, const PaymasterRelayRound& round);
void ResetPaymasterRelayRound(WalletDatabase& database);
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

struct PaymasterProviderHistory {
    size_t retained{0};
    bool complete{false};
    bool full{false};
};
/** Bounded disposable observation. Caller holds cs_wallet. Counts constrain
 * resource admission only; locators never grant signing/payment authority.
 * Committed typed writes update the observation; abort/unload discards it. */
bool ObservePaymasterProviderHistory(WalletBatch& batch, WalletDatabase& database,
    size_t limit, PaymasterProviderHistory& history, std::string& error);
struct PaymasterProviderDrainCandidates {
    std::vector<std::string> sessions;
    std::vector<uint256> recoveries;
    bool complete{false};
};
PaymasterProviderDrainCandidates GetPaymasterProviderDrainCandidates(
    WalletDatabase& database, const uint256& provider_id);
void CompletePaymasterProviderDrainCandidate(WalletDatabase& database, const std::string& request);
void CompletePaymasterProviderDrainCandidate(WalletDatabase& database, const uint256& recovery);

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
