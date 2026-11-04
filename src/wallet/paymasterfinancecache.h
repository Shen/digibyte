// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERFINANCECACHE_H
#define DIGIBYTE_WALLET_PAYMASTERFINANCECACHE_H
namespace wallet {
class WalletDatabase;
// Lifecycle only; cached reconciliation never authorizes spending.
void ForgetPaymasterFinanceCache(WalletDatabase& database);
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERFINANCECACHE_H
