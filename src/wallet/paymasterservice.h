// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_PAYMASTERSERVICE_H
#define DIGIBYTE_WALLET_PAYMASTERSERVICE_H
#include <psbt.h>
#include <wallet/paymasterresults.h>
#include <functional>
#include <optional>
namespace wallet {
class CWallet;
struct WalletContext;
/** Contain one wallet's background failure; false forbids further work in this cycle. */
bool RunPaymasterBackgroundWork(WalletContext& context, CWallet& wallet,
                               const std::function<void()>& work);
void RunPaymasterProviderServiceCycle(WalletContext& context, CWallet& wallet);
namespace paymaster_app {
enum class ExecutionMode { MANUAL, AUTOMATIC };
ProviderStartResult StartPaymaster(WalletContext& context, CWallet& wallet, bool wait_for_readiness, ExecutionMode mode);
ProviderStopResult StopPaymaster(WalletContext& context, CWallet& wallet, bool persistent, bool pause_setup);
/** Serialize direct submission with provider stop, policy and maintenance work.
 * Exact durable replay remains available without starting the provider. */
PaymentResult SubmitPaymasterTransaction(WalletContext& context, CWallet& wallet, PartiallySignedTransaction psbt, ExecutionMode mode);
ProviderProcessingResult ProcessPaymasterSubmits(WalletContext& context, CWallet& wallet, ExecutionMode mode);
ProviderProcessingResult ProcessPaymasterRequests(WalletContext& context, CWallet& wallet, ExecutionMode mode);
PaymentResult ProcessPaymasterResult(WalletContext& context, CWallet& wallet, const std::string& request_id);
PaymentResult AuthorizePaymasterPSBT(WalletContext& context, CWallet& wallet, PartiallySignedTransaction candidate,
                              const std::optional<uint256>& authorization_commitment);
PaymentResult AuthorizePaymasterPSBT(WalletContext& context, CWallet& wallet, const std::vector<unsigned char>& encoded_psbt,
                              const std::optional<uint256>& authorization_commitment);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERSERVICE_H
