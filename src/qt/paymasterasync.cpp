// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterasync.h>
namespace PaymasterQt {
QString StableCode(const RpcError& error)
{
    const auto& value = error.data.find_value("paymaster_code");
    return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
}
QString Disposition(const RpcError& error)
{
    const auto& value = error.data.find_value("disposition");
    return value.isStr() ? QString::fromStdString(value.get_str()) : QStringLiteral("reconcile");
}
} // namespace PaymasterQt
