// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERCLIENTVIEW_H
#define DIGIBYTE_WALLET_PAYMASTERCLIENTVIEW_H
#include <paymaster/recovery.h>
#include <paymaster/reservation.h>
#include <wallet/paymasterresults.h>
#include <optional>
#include <string_view>
#include <variant>
namespace wallet {
class PaymasterStore;
namespace paymaster_app {
using ClientSessionLookup = std::variant<std::string, uint256>;
/** False means confirmed absence; unreadable/unsupported records throw. */
bool FindClientSession(const ClientSessionLookup& lookup, PaymasterStore& store,
                       DigiDollar::Paymaster::PaymentSession& session);
/** Current derived capabilities, never authority to skip a fresh execution
 * check. An unreadable artifact fails the whole view instead of enabling an
 * unsigned cancellation or retry. */
struct ClientSessionView {
    std::optional<DigiDollar::Paymaster::ProviderAttempt> attempt;
    std::optional<DigiDollar::Paymaster::AlternativeRecoveryRecord> recovery;
    std::optional<DigiDollar::Paymaster::PaymasterResult> provider_result;
    std::optional<std::string> recipient_address;
    std::string artifact{"none"};
    int64_t observed_at{0};
    bool has_retriable_final{false};
    bool authorization_may_exist{false};
    bool requires_attention{false};
    std::vector<std::string> allowed_actions;
};
bool LoadClientSessionView(PaymasterStore& store, const DigiDollar::Paymaster::PaymentSession& session,
                           int64_t now, ClientSessionView& view, std::string& error);
struct ClientRecoveryResult {
    std::string action;
    PaymentResult session;
    ClientSessionView view;
    std::optional<bool> queued;
    std::optional<bool> connection_pending;
    std::optional<bool> route_available;
    std::optional<bool> broadcast;
    std::optional<std::string> broadcast_error;
    std::optional<std::vector<unsigned char>> raw_transaction;
    std::optional<std::vector<unsigned char>> psbt;
    std::optional<uint256> txid;
};
ClientRecoveryResult CaptureClientSession(PaymasterStore& store, const DigiDollar::Paymaster::PaymentSession& session, std::string_view action);
bool IsActiveClientSession(const DigiDollar::Paymaster::PaymentSession& session, const ClientSessionView& view);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERCLIENTVIEW_H
