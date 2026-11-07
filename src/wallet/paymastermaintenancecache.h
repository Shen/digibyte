// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERMAINTENANCECACHE_H
#define DIGIBYTE_WALLET_PAYMASTERMAINTENANCECACHE_H

namespace wallet {
class WalletDatabase;
/** Discard derived maintenance history on wallet destruction. No reservations
 * or financial authority are stored in this process-local cache. */
void ForgetPaymasterMaintenanceCache(WalletDatabase& database);
} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERMAINTENANCECACHE_H
