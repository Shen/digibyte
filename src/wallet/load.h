// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_LOAD_H
#define DIGIBYTE_WALLET_LOAD_H

#include <cstdint>
#include <string>
#include <vector>

class ArgsManager;
class CScheduler;

namespace interfaces {
class Chain;
} // namespace interfaces

namespace wallet {
struct WalletContext;

//! Responsible for reading and validating the -wallet arguments and verifying the wallet database.
bool VerifyWallets(WalletContext& context);

//! Load wallet databases.
bool LoadWallets(WalletContext& context);

//! Complete startup of wallets.
void StartWallets(WalletContext& context, CScheduler& scheduler);

//! Reconcile expired Paymaster authority and durable final state for every
//! loaded wallet. Individual quote/capacity releases are atomic in
//! PaymasterStore; one wallet failure does not prevent maintenance of another.
void RunPeriodicPaymasterMaintenance(WalletContext& context, int64_t now);

//! Register the recurring Paymaster wallet-maintenance task.
void SchedulePeriodicPaymasterMaintenance(WalletContext& context,
                                          CScheduler& scheduler);

//! Run one bounded automatic provider-service cycle for every loaded wallet.
void RunPaymasterProviderServices(WalletContext& context);

//! Register the short, bounded automatic Paymaster provider worker.
void SchedulePaymasterProviderServices(WalletContext& context,
                                       CScheduler& scheduler);

//! Flush all wallets in preparation for shutdown.
void FlushWallets(WalletContext& context);

//! Stop all wallets. Wallets will be flushed first.
void StopWallets(WalletContext& context);

//! Close all wallets.
void UnloadWallets(WalletContext& context);
} // namespace wallet

#endif // DIGIBYTE_WALLET_LOAD_H
