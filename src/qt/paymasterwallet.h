// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERWALLET_H
#define DIGIBYTE_QT_PAYMASTERWALLET_H

#include <qt/walletmodel.h>

namespace PaymasterQt {
/** Temporary authority for deterministic injected transports. Production
 * signing uses ExecuteSigningRpcAsync so lock inspection/relocking stay off Qt. */
std::shared_ptr<WalletModel::UnlockContext> RequestUnlock(WalletModel& model);

/** Inspect/relock on workers; retain the native unlock dialog and the shared
 * wallet lease. is_current runs on Qt before the dialog and before dispatch. */
void ExecuteSigningRpcAsync(WalletModel& model, std::string command, UniValue params,
                            std::function<bool()> is_current, WalletModel::RpcCallback callback);
} // namespace PaymasterQt

#endif // DIGIBYTE_QT_PAYMASTERWALLET_H
