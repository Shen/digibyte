// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERASYNC_H
#define DIGIBYTE_QT_PAYMASTERASYNC_H
#include <qt/walletasync.h>
namespace PaymasterQt {
using RpcError = WalletAsync::RpcError;
using RpcReply = WalletAsync::RpcReply;
using WorkQueue = WalletAsync::WorkQueue;
QString StableCode(const RpcError& error);
QString Disposition(const RpcError& error);
} // namespace PaymasterQt
#endif // DIGIBYTE_QT_PAYMASTERASYNC_H
