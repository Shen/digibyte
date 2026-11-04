// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERSESSIONINDEX_H
#define DIGIBYTE_WALLET_PAYMASTERSESSIONINDEX_H

#include <paymaster/reservation.h>

#include <vector>

namespace wallet {
class WalletBatch;
class WalletDatabase;

/** Derived lookup only. Caller holds the wallet lock; returned records are
 * freshly read and still require exact-artifact/authorization validation. */
bool ListPaymasterFinalSessions(WalletBatch& batch, WalletDatabase& database,
                                const uint256& txid,
                                std::vector<DigiDollar::Paymaster::PaymentSession>& sessions);
void PrimePaymasterSessionIndex(WalletDatabase& database,
                                const std::vector<DigiDollar::Paymaster::PaymentSession>& sessions);
void ForgetPaymasterSessionIndex(WalletDatabase& database);
/** Apply queued index deltas only after the database commit has succeeded.
 * Abort/destruction discards them and invalidates the derived lookup. */
void PaymasterSessionIndexEnd(const WalletBatch* owner, bool committed = false);
} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERSESSIONINDEX_H
