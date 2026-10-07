// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_PAYMASTER_CLI_H
#define DIGIBYTE_PAYMASTER_CLI_H

#include <functional>
#include <optional>
#include <string>
#include <univalue.h>

class ArgsManager;

namespace DigiDollar::Paymaster {
/** Reuse the CLI's existing transport, authentication and argument conversion. */
using CliRpc = std::function<UniValue(const std::string&, const UniValue&, const std::optional<std::string>&)>;
int RunCli(bool setup, const ArgsManager& args, const std::string& rpc_host, const CliRpc& rpc);
} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_CLI_H
