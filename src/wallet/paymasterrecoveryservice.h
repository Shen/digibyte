// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERRECOVERYSERVICE_H
#define DIGIBYTE_WALLET_PAYMASTERRECOVERYSERVICE_H
#include <wallet/paymasterclientview.h>
namespace wallet {
class CWallet;
struct WalletContext;
namespace paymaster_app {
struct ClientRecoveryOptions {
    std::optional<uint256> attempt_id;
    std::optional<uint256> provider_id;
    std::optional<uint256> offer_id;
    std::optional<int64_t> maximum_fee;
    std::optional<uint256> authorization_commitment;
    bool prepare_only{false};
    bool retry_transport{false};
};
UniValue ResolvePaymasterSession(WalletContext& context, CWallet& wallet,
    const ClientSessionLookup& lookup, const std::string& action, const ClientRecoveryOptions& options);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERRECOVERYSERVICE_H
