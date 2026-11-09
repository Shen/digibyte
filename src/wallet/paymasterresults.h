// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERRESULTS_H
#define DIGIBYTE_WALLET_PAYMASTERRESULTS_H

#include <paymaster/manager.h>
#include <paymaster/transport.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterstore.h>

#include <optional>
#include <string>
#include <vector>

namespace wallet::paymaster_app {

struct TransportSnapshot {
    std::optional<DigiDollar::Paymaster::DirectState> state;
    DigiDollar::Paymaster::DirectBudget budget;
    DigiDollar::Paymaster::DirectPermits::Usage usage{};
    uint64_t queued{0};
    bool listener_ready{false};
};

struct ProviderStartResult {
    bool start_requested{false};
    bool running{false};
    bool ready{false};
    bool drain_only{false};
    DigiDollar::Paymaster::ProviderOperationMode operation_mode{DigiDollar::Paymaster::ProviderOperationMode::AUTOMATIC};
    bool autostart{false};
    DigiDollar::Paymaster::ProviderServiceState service_state{DigiDollar::Paymaster::ProviderServiceState::STOPPED};
    std::optional<DigiDollar::Paymaster::ProviderSafetyPolicy> safety_policy;
    std::optional<DigiDollar::Paymaster::PaymasterId> provider_id;
    std::optional<CService> endpoint;
    std::optional<uint64_t> announcement_sequence;
    DurablePaymasterRecoveryReport recovery;
    std::vector<std::string> readiness_errors;
    std::optional<TransportSnapshot> transport;
};

struct ProviderStopResult {
    bool running{false};
};

struct ProviderEnabledResult {
    bool enabled{false};
    bool running{false};
    std::optional<uint256> policy_hash;
};

struct RestrictedDescriptorResult {
    DigiDollar::Paymaster::RestrictedServiceDescriptor descriptor;
    XOnlyPubKey provider_identity_key;
};

struct ProviderRuntimeResult {
    DigiDollar::Paymaster::ProviderOperationMode operation_mode{DigiDollar::Paymaster::ProviderOperationMode::AUTOMATIC};
    bool autostart{false};
    bool running{false};
};

struct MaintenanceCancellationResult {
    bool executed{false};
    PaymasterMaintenanceCancellation review;
    std::vector<uint256> operation_ids;
};

struct CapitalReleaseResult {
    bool executed{false};
    PaymasterCapitalRelease release;
};

struct BackupAdoptionResult {
    bool executed{false};
    PaymasterCapitalRelease review;
    DigiDollar::Paymaster::PaymasterId provider_id;
};

struct BackupAcknowledgementResult {
    bool backup_required{false};
    int64_t acknowledged_at{0};
};


/** Captured under the wallet's authoritative read path; serialization never re-reads it. */
struct SessionSnapshot {
    DigiDollar::Paymaster::PaymentSession session;
    std::optional<DigiDollar::Paymaster::ClientPaymentOrder> resume_order;
    std::string resume_order_error;
    PaymasterSessionObservation observation;
    bool observation_requested{false};
    std::string observation_error;
};

/** Optional values distinguish an unavailable artifact from a known false/zero value. */
struct PaymentResult {
    std::optional<SessionSnapshot> session;
    std::optional<std::string> request_id;
    std::optional<std::string> to_address;
    std::optional<std::string> broadcast_error;
    std::optional<uint256> session_id;
    std::optional<DigiDollar::Paymaster::PaymasterId> provider_id;
    std::optional<uint256> attempt_id;
    std::optional<uint256> offer_id;
    std::optional<uint256> policy_hash;
    std::optional<uint256> txid;
    std::optional<uint256> quote_id;
    std::optional<uint256> unsigned_txid;
    std::optional<uint256> template_commitment;
    std::optional<uint256> authorization_commitment;
    std::optional<uint256> capacity_snapshot_id;
    std::optional<DigiDollar::Paymaster::DDCents> payment_cents;
    std::optional<DigiDollar::Paymaster::DDCents> service_fee_cents;
    std::optional<DigiDollar::Paymaster::DDCents> user_total_cents;
    std::optional<DigiDollar::Paymaster::DDCents> requested_amount_cents;
    std::optional<bool> processed;
    std::optional<bool> broadcast;
    std::optional<bool> queued;
    std::optional<bool> connection_pending;
    std::optional<bool> capacity_pending;
    std::optional<bool> route_available;
    std::optional<bool> authorization_required;
    std::optional<bool> authorization_accepted;
    std::optional<bool> subtract_paymaster_fee_from_amount;
    std::optional<bool> send_all_spendable_dd;
    std::optional<int64_t> expires_at;
    std::optional<int64_t> authorization_accepted_at;
    std::optional<uint64_t> result_sequence;
    std::optional<DigiDollar::Paymaster::FeeMode> requested_fee_mode;
    std::optional<DigiDollar::Paymaster::FeeMode> fee_mode_used;
    std::optional<DigiDollar::Paymaster::PrivacyProfile> privacy_profile;
    std::optional<DigiDollar::Paymaster::FundingModel> funding_model;
    std::optional<DigiDollar::Paymaster::SessionState> session_state;
    std::optional<DigiDollar::Paymaster::AttemptState> attempt_state;
    std::optional<DigiDollar::Paymaster::PaymasterResultStatus> result_status;
    std::optional<TransportSnapshot> transport;
    std::optional<std::vector<unsigned char>> psbt;
    std::optional<std::vector<unsigned char>> final_transaction;
};

/** Complete display terms without replacing present values, including false and zero. */
void FillMissingPaymentTerms(PaymentResult& target, const PaymentResult& source);

enum class ProviderProcessingDisposition {
    NO_MESSAGE,
    PROCESSED,
    REJECTED,
    DEFERRED,
};

/** One processing outcome; delivery and signed payment status are separate observations. */
struct ProviderProcessingResult {
    ProviderProcessingDisposition disposition{ProviderProcessingDisposition::NO_MESSAGE};
    /** A message was observed, including REJECTED/DEFERRED; consult disposition
     * for processing outcome. This never acknowledges a payment or delivery. */
    bool HasMessage() const { return disposition != ProviderProcessingDisposition::NO_MESSAGE; }
    bool IsRejected() const { return disposition == ProviderProcessingDisposition::REJECTED; }
    bool IsDeferred() const { return disposition == ProviderProcessingDisposition::DEFERRED; }
    std::optional<bool> queued;
    std::optional<bool> broadcast;
    std::optional<std::string> broadcast_error;
    std::optional<std::string> request_id;
    std::optional<uint256> session_id;
    std::optional<DigiDollar::Paymaster::PaymasterId> provider_id;
    std::optional<uint256> attempt_id;
    std::optional<uint256> quote_id;
    std::optional<uint256> template_commitment;
    std::optional<uint256> capacity_snapshot_id;
    std::optional<uint256> txid;
    std::optional<DigiDollar::Paymaster::AttemptState> attempt_state;
    std::optional<DigiDollar::Paymaster::PaymasterResultStatus> result_status;
    std::optional<std::string> message_type;
    std::optional<std::string> rejection_reason;
    std::optional<std::string> rejection_code;
    std::optional<std::string> deferral_reason;
    std::optional<uint256> commit_key;
    std::optional<uint256> client_nonce;
    std::optional<uint256> recovery_id;
    std::optional<DigiDollar::Paymaster::AlternativeRecoveryRecord> recovery;
    std::optional<PaymentResult> commit;
};

struct PreparationFunding {
    std::optional<CAmount> required_output_satoshis;
    std::optional<CAmount> confirmed_available_satoshis;
    std::optional<CAmount> available_including_unconfirmed_satoshis;
    std::optional<CAmount> capital_shortfall_satoshis;
    std::optional<CAmount> funding_shortfall_upper_bound_satoshis;
    std::optional<DigiDollar::Paymaster::DDCents> required_output_cents;
    std::optional<DigiDollar::Paymaster::DDCents> confirmed_available_cents;
    std::optional<DigiDollar::Paymaster::DDCents> pending_cents;
    std::optional<DigiDollar::Paymaster::DDCents> capital_shortfall_cents;
};
struct PreparationSnapshot {
    DigiDollar::Paymaster::ProviderMaintenanceRecord record;
    PreparationFunding funding;
};
struct CarrierWithdrawalResult {
    std::optional<bool> executed;
    std::optional<uint256> plan_id;
    std::optional<DigiDollar::Paymaster::CarrierWithdrawalMode> mode;
    std::optional<uint64_t> source_carriers;
    std::optional<int64_t> expires_at;
    std::optional<DigiDollar::Paymaster::DDCents> withdrawable_excess_cents;
    std::optional<DigiDollar::Paymaster::DDCents> retained_carrier_cents;
    std::optional<CAmount> estimated_network_fee_satoshis;
    std::optional<int64_t> operational_carrier_target;
    std::optional<uint256> txid;
};
struct PoolPreparationResult {
    std::optional<uint256> plan_id;
    std::optional<bool> preview_only;
    std::optional<int64_t> admission_dgb_slots;
    std::optional<int64_t> operational_dgb_slots;
    std::optional<int64_t> admission_carrier_slots;
    std::optional<int64_t> operational_carrier_slots;
    std::optional<int64_t> missing_admission_dgb_slots;
    std::optional<int64_t> missing_operational_dgb_slots;
    std::optional<int64_t> missing_admission_carrier_slots;
    std::optional<int64_t> missing_operational_carrier_slots;
    std::optional<CAmount> admission_dgb_satoshis_each;
    std::optional<CAmount> operational_dgb_satoshis_each;
    std::optional<DigiDollar::Paymaster::DDCents> carrier_cents_each;
    std::optional<CAmount> total_output_satoshis;
    std::optional<DigiDollar::Paymaster::DDCents> total_carrier_cents;
    std::optional<CAmount> maximum_fee_satoshis;
    std::optional<CAmount> maximum_total_fee_satoshis;
    std::optional<bool> accepted;
    std::optional<bool> cancelled;
    std::optional<std::vector<PreparationSnapshot>> preparation;
    std::optional<bool> executed;
    std::optional<uint256> dgb_txid;
    std::optional<uint256> txid;
    std::optional<CAmount> network_fee_satoshis;
    std::optional<uint256> dd_txid;
    std::optional<std::vector<DigiDollar::Paymaster::ProviderPoolEntry>> pool;
};
struct PoolRebalanceResult {
    std::optional<uint256> plan_id;
    std::optional<int64_t> retired_admission_dgb_slots;
    std::optional<int64_t> retired_operational_dgb_slots;
    std::optional<int64_t> retired_admission_carrier_slots;
    std::optional<int64_t> retired_operational_carrier_slots;
    std::optional<CAmount> retired_dgb_satoshis;
    std::optional<DigiDollar::Paymaster::DDCents> retired_carrier_cents;
    std::optional<CAmount> estimated_dgb_fee_satoshis;
    std::optional<CAmount> estimated_dd_fee_satoshis;
    std::optional<CAmount> wallet_maximum_fee_satoshis;
    std::optional<CAmount> maximum_network_fee_satoshis;
    std::optional<CAmount> maximum_total_fee_satoshis;
    std::optional<bool> executed;
    std::optional<uint256> dd_txid;
    std::optional<CAmount> dd_network_fee_satoshis;
    std::optional<uint256> dgb_txid;
    std::optional<CAmount> network_fee_satoshis;
    std::optional<std::vector<DigiDollar::Paymaster::ProviderPoolEntry>> pool;
};

} // namespace wallet::paymaster_app
#endif // DIGIBYTE_WALLET_PAYMASTERRESULTS_H
