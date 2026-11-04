// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_PAYMASTERSERVICE_H
#define DIGIBYTE_WALLET_PAYMASTERSERVICE_H
#include <psbt.h>
#include <univalue.h>
#include <optional>
namespace wallet {
class CWallet;
struct WalletContext;
void RunPaymasterProviderServiceCycle(WalletContext& context, CWallet& wallet);
namespace paymaster_app {
enum class ExecutionMode { MANUAL, AUTOMATIC };
UniValue StartPaymaster(WalletContext& context, CWallet& wallet, bool wait_for_readiness, ExecutionMode mode);
UniValue StopPaymaster(WalletContext& context, CWallet& wallet, bool persistent, bool pause_setup);
UniValue SubmitPaymasterTransaction(CWallet& wallet, PartiallySignedTransaction psbt, ExecutionMode mode);
UniValue ProcessPaymasterSubmits(WalletContext& context, CWallet& wallet, ExecutionMode mode);
UniValue ProcessPaymasterRequests(WalletContext& context, CWallet& wallet, ExecutionMode mode);
UniValue ProcessPaymasterResult(WalletContext& context, CWallet& wallet, const std::string& request_id);
UniValue AuthorizePaymasterPSBT(WalletContext& context, CWallet& wallet, PartiallySignedTransaction candidate,
                              const std::optional<uint256>& authorization_commitment);
UniValue AuthorizePaymasterPSBT(WalletContext& context, CWallet& wallet, const std::string& encoded_psbt,
                              const std::optional<uint256>& authorization_commitment);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERSERVICE_H
