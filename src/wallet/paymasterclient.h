// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERCLIENT_H
#define DIGIBYTE_WALLET_PAYMASTERCLIENT_H
#include <paymaster/client.h>
#include <wallet/paymasterresults.h>
#include <optional>
namespace wallet {
class CWallet;
struct WalletContext;
namespace paymaster_app {
struct ClientPaymentOptions {
    std::string request_id;
    std::string address;
    int64_t fee_cap{0};
    bool subtract_fee{false};
    bool send_all{false};
    bool retry_transport{false};
    DigiDollar::Paymaster::FeeMode mode{DigiDollar::Paymaster::FeeMode::PAYMASTER};
    DigiDollar::Paymaster::PrivacyProfile privacy{DigiDollar::Paymaster::PrivacyProfile::STANDARD};
    DigiDollar::Paymaster::SelectionMode selection{DigiDollar::Paymaster::SelectionMode::LOWEST_TOTAL_COST};
    int maximum_attempts{3};
    std::optional<std::vector<unsigned char>> identity_key;
    std::optional<std::vector<unsigned char>> descriptor;
    std::optional<std::vector<unsigned char>> capability;
};
struct QuoteParameters : ClientPaymentOptions {
    DigiDollar::Paymaster::PaymasterId provider_id;
    int64_t payment{0};
    int64_t requested_amount{0};
    std::vector<COutPoint> inputs;
    std::optional<uint256> offer_id;
};
struct AutomaticPaymentParameters : ClientPaymentOptions {
    int64_t amount{0};
    bool prepare_only{false};
    std::optional<uint256> authorization_commitment;
    std::optional<DigiDollar::Paymaster::PaymasterId> preferred_provider;
    std::optional<uint256> preferred_offer;
};
PaymentResult RequestAutomaticPaymasterPayment(WalletContext& context, CWallet& wallet,
    const AutomaticPaymentParameters& parameters, const std::vector<COutPoint>* preset_inputs = nullptr);
PaymentResult RequestPaymasterQuote(WalletContext& context, CWallet& wallet, const QuoteParameters& parameters);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERCLIENT_H
