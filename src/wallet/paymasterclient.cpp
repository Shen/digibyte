// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <wallet/paymasterclient.h>
#include <wallet/paymasterservice.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>
#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/validation.h>
#include <random.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/spend.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;


// Preserve the existing v1 hash bytes, including the original selected inputs.
// Both entry points must enforce the same binding on terminal retries.
uint256 CanonicalClientRequestHash(
    const std::string& request_id, const CScript& recipient_script,
    DDCents order_amount, DDCents fee_cap, const std::vector<COutPoint>& inputs,
    FeeMode mode, PrivacyProfile privacy, SelectionMode selection,
    int maximum_attempts, bool subtract_fee, bool send_all,
    const std::optional<XOnlyPubKey>& restricted_key,
    const std::optional<RestrictedServiceDescriptor>& descriptor,
    const std::optional<SponsorshipCapability>& capability)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster RPC Request v1");
    hasher << request_id << recipient_script << order_amount << fee_cap << inputs
           << static_cast<uint8_t>(mode) << static_cast<uint8_t>(privacy)
           << static_cast<uint8_t>(selection) << maximum_attempts;
    if (subtract_fee || send_all) {
        hasher << std::string{"gross-amount-v1"} << order_amount << subtract_fee << send_all;
    }
    if (restricted_key) {
        hasher << *restricted_key << GetRestrictedDescriptorSignatureHash(*descriptor)
               << GetSponsorshipCapabilityHash(*capability);
    }
    return hasher.GetSHA256();
}

DDCents PreviewMaximumServiceFee(CWallet& wallet, DDCents requested_limit,
                                int64_t now, std::string& error)
{
    ClientSafetyPolicy policy;
    if (!GetPaymasterClientSafetyPolicy(wallet, policy)) {
        error = "PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED";
        return DDCents{-1};
    }
    if (!ValidateClientSafetyPolicy(policy, error)) return DDCents{-1};
    if (requested_limit.value < 0 || requested_limit.value > MAX_DD_OUTPUT_CENTS) {
        error = "PAYMASTER_CLIENT_FEE_LIMIT_INVALID";
        return DDCents{-1};
    }

    ClientFeeLedger ledger;
    if (!GetPaymasterClientFeeLedger(wallet, ledger)) {
        error = "PAYMASTER_CLIENT_FEE_LEDGER_REQUIRED";
        return DDCents{-1};
    }

    int64_t available_today = policy.maximum_service_fee_per_day.value;
    const int64_t effective_now = std::max(now, ledger.accounting_time_high_water);
    for (const ClientFeeReservation& reservation : ledger.reservations) {
        const bool counts_against_budget =
            reservation.state == BudgetReservationState::RESERVED ||
            (reservation.state == BudgetReservationState::SPENT &&
             !TimeDeltaExceeds(effective_now, reservation.updated_at, 24 * 60 * 60));
        if (!counts_against_budget || reservation.service_fee.value <= 0) continue;
        available_today = reservation.service_fee.value >= available_today
                              ? 0
                              : available_today - reservation.service_fee.value;
    }

    error.clear();
    return DDCents{std::min({requested_limit.value,
                             policy.maximum_service_fee_per_transaction.value,
                             available_today})};
}

// Only a failed provider contact, before transaction authorization, reaches
// this helper. User cancellation, locked wallets and local connection limits
// must not produce negative provider history.
void RecordUnavailableClientProvider(PaymasterStore& store, const PaymentSession& session,
                                     const ProviderAttempt& attempt, int64_t now)
{
    if (session.provider_side || attempt.state != AttemptState::CANDIDATE ||
        !attempt.signed_quote.empty() || !attempt.user_signed_psbt.empty()) return;
    std::string error;
    if (!store.AbandonClientAttemptForFallback(session.request_id, attempt.attempt_id, now, error) ||
        !store.RecordProviderOutcome(attempt.attempt_id, ReliabilityOutcome::AVAILABILITY_TIMEOUT,
                                     now, 0, error)) {
        LogPrint(BCLog::WALLETDB, "Paymaster availability observation could not be saved: %s\n", error);
    }
}


namespace {
void ValidateClientPaymentOptions(const ClientPaymentOptions& input)
{
    if (!CDigiDollarAddress{input.address}.IsValidForCurrentNetwork())
        throw ApplicationError(ErrorKind::INVALID_ADDRESS, "Invalid DigiDollar address for this network");
    if (!IsCanonicalRequestId(input.request_id))
        throw ApplicationError(ErrorKind::INVALID_INPUT, "request_id must be a canonical lowercase UUID");
    if ((input.mode != FeeMode::AUTO && input.mode != FeeMode::PAYMASTER) ||
        (input.privacy != PrivacyProfile::STANDARD && input.privacy != PrivacyProfile::HIGH) ||
        (input.selection != SelectionMode::LOWEST_TOTAL_COST && input.selection != SelectionMode::PRIVACY_WEIGHTED))
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_CLIENT_OPTIONS");
    if (input.fee_cap < 0 || input.fee_cap > MAX_DD_OUTPUT_CENTS)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "Paymaster amount or fee cap is out of range");
    if (input.maximum_attempts < 1 || input.maximum_attempts > 16 ||
        (input.privacy == PrivacyProfile::HIGH && input.maximum_attempts != 1))
        throw ApplicationError(ErrorKind::INVALID_INPUT, "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
    if (input.send_all && !input.subtract_fee)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
    const bool restricted = input.identity_key || input.descriptor || input.capability;
    if (restricted && !(input.identity_key && input.descriptor && input.capability))
        throw ApplicationError(ErrorKind::INVALID_INPUT,
            "provider_identity_key, restricted_service_descriptor, and sponsorship_capability must be supplied together");
    if (restricted && input.maximum_attempts != 1)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "Restricted sponsorship requires maximum_provider_attempts=1");
}

bool TryCommitClientCapacityProof(
    Manager& manager, PaymasterStore& store, const QuoteParameters& input,
    const PaymentSession& session, ProviderAttempt& attempt,
    const PaymasterCapacityRequest& capacity_request, ChainstateManager& chainman,
    int64_t now, std::string& error)
{
    const auto& provider_id = input.provider_id;
    const auto& request_id = input.request_id;
    PaymasterCapacityProof capacity_proof;
    bool capacity_ready{false};
    // Persist a simultaneously queued signed conflict before the
    // acceptance loop can discard either locally invalid proof.
    if (!DrainClientAttemptEquivocations(
            manager, store, session, attempt, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    auto proof_lease = manager.LeaseCapacityProofs(
        provider_id, attempt.client_nonce,
        MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
    const auto proofs = SelectCapacityProofMessages(
        proof_lease.Messages(), provider_id, request_id,
        session.session_id, attempt.client_nonce);
    bool capacity_equivocation{false};
    if (!PersistPendingCapacityEquivocationPair(
            store, attempt.attempt_id, proofs,
            capacity_equivocation, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (capacity_equivocation) {
        if (!AcknowledgeEquivocationMessages(
                manager, proofs, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_CAPACITY_EQUIVOCATION");
    }
    std::string rejected_proof_error;
    for (const DirectMessage& direct : proofs) {
        const auto* received =
            std::get_if<PaymasterCapacityProof>(&direct.payload);
        if (!received) {
            if (!AcknowledgeRejectedDirectMessage(
                    manager, direct, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            rejected_proof_error =
                "PAYMASTER_INVALID_CAPACITY_PROOF";
            continue;
        }

        bool claim_equivocation{false};
        const std::vector<unsigned char> encoded_proof =
            SerializeCapacityProof(*received);
        if (!store.StageCapacityProofClaimCandidate(
                attempt.attempt_id, encoded_proof,
                direct.received_at, claim_equivocation, error)) {
            if (error !=
                "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            if (!AcknowledgeRejectedDirectMessage(
                    manager, direct, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            rejected_proof_error =
                "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
            continue;
        }
        if (claim_equivocation) {
            if (!AcknowledgeEquivocationMessages(
                    manager, proofs, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_CAPACITY_EQUIVOCATION");
        }
        if (!store.GetAttempt(attempt.attempt_id, attempt)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_ATTEMPT_NOT_FOUND");
        }

        std::string proof_error;
        if (!ValidateCapacityProofAgainstChainstate(
                *received, capacity_request,
                attempt.provider_identity_key, chainman,
                now, proof_error)) {
            if (!AcknowledgeRejectedDirectMessage(
                    manager, direct, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            rejected_proof_error =
                proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
            continue;
        }
        capacity_proof = *received;
        ValidatedCapacitySnapshot snapshot;
        snapshot.snapshot_id = capacity_proof.snapshot_id;
        snapshot.resource_commitment =
            GetCapacityResourceCommitment(capacity_proof);
        snapshot.provider_id = capacity_proof.provider_id;
        snapshot.client_nonce = capacity_proof.client_nonce;
        snapshot.funding_model = capacity_proof.funding_model;
        snapshot.requires_carrier = capacity_proof.requires_carrier;
        snapshot.request_hash = Hash(attempt.capacity_request);
        snapshot.capacity_proof = SerializeCapacityProof(capacity_proof);
        snapshot.created_at = capacity_proof.created_at;
        snapshot.expires_at = capacity_proof.expires_at;
        snapshot.validated_at = now;
        if (!store.CommitValidatedCapacitySnapshot(
                request_id, attempt.attempt_id, snapshot, now, error) ||
            !store.GetAttempt(attempt.attempt_id, attempt)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        capacity_ready = true;
        break;
    }
    if (!capacity_ready && !rejected_proof_error.empty()) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           rejected_proof_error);
    }
    return capacity_ready;
}
} // namespace

PaymentResult RequestPaymasterQuote(WalletContext& context, CWallet& target, const QuoteParameters& input)
{
    ValidateClientPaymentOptions(input);
    CWallet* const wallet = &target;
    std::string readiness_error;
    if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error))
        throw ApplicationError(ErrorKind::GENERAL, readiness_error);
    const auto& provider_id = input.provider_id;
    const auto& request_id = input.request_id;
    const int64_t payment = input.payment, requested_amount = input.requested_amount, fee_cap = input.fee_cap;
    const bool subtract_fee = input.subtract_fee, send_all = input.send_all;
    if (payment < 100 || payment > MAX_DD_OUTPUT_CENTS ||
        requested_amount < 100 || requested_amount > MAX_DD_OUTPUT_CENTS) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "Paymaster amount or fee cap is out of range");
    }
    if ((!subtract_fee && requested_amount != payment) ||
        (subtract_fee && requested_amount < payment)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "PAYMASTER_INVALID_CLIENT_PAYMENT_ORDER");
    }
    const auto requested_mode = input.mode;
    const auto privacy = input.privacy;
    const auto selection = input.selection;
    const int maximum_attempts = input.maximum_attempts;
    if (!CheckPaymasterPrivacyReadiness(privacy, context, readiness_error)) {
        throw ApplicationError(ErrorKind::GENERAL, readiness_error);
    }
    const auto& requested_inputs = input.inputs;
    const auto& address = input.address;
    const CDigiDollarAddress dd_address{address};
    const CScript recipient_script = GetScriptForDestination(dd_address.GetDigiDollarDestination());
    const auto& requested_offer = input.offer_id;
    const bool have_restricted_key = input.identity_key.has_value();
    const bool have_restricted_descriptor = input.descriptor.has_value();
    const bool have_restricted_capability = input.capability.has_value();
    const bool restricted = have_restricted_key || have_restricted_descriptor ||
                            have_restricted_capability;
    std::optional<XOnlyPubKey> restricted_identity_key;
    std::optional<RestrictedServiceDescriptor> restricted_descriptor;
    std::optional<SponsorshipCapability> restricted_capability;
    std::string error;
    PaymasterStore store{*wallet};
    const int64_t now = GetTime();
    PaymentSession durable_session;
    const bool have_durable_session =
        store.GetSessionByRequestId(request_id, durable_session);
    if (have_durable_session && !IsTerminal(durable_session.state)) {
        ProviderAttempt durable_attempt;
        PaymentIntent durable_intent;
        PaymasterQuote durable_quote;
        if (FindDurableClientAuthorizationAttempt(
                *wallet, store, durable_session, GetTime(), durable_attempt,
                durable_intent, durable_quote, error)) {
            if (durable_attempt.provider_id != provider_id ||
                durable_intent.request_id != request_id ||
                durable_intent.session_id != durable_session.session_id ||
                durable_intent.canonical_request_hash !=
                    durable_session.canonical_request_hash ||
                durable_intent.requested_fee_mode != requested_mode ||
                durable_intent.privacy_profile != privacy ||
                durable_intent.selection_mode != selection ||
                durable_attempt.privacy_profile != privacy ||
                durable_intent.recipient_script != recipient_script ||
                durable_intent.recipient_amount != DDCents{payment} ||
                (durable_session.requested_amount.value == 0
                     ? (subtract_fee || send_all || requested_amount != payment)
                     : (durable_session.requested_amount != DDCents{requested_amount} ||
                        durable_session.subtract_paymaster_fee_from_amount != subtract_fee ||
                        durable_session.send_all_spendable_dd != send_all)) ||
                durable_intent.user_dd_inputs != requested_inputs ||
                durable_quote.service_fee.value > fee_cap ||
                (requested_offer &&
                 durable_intent.offer_id != *requested_offer)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, durable_session,
                    error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            return CaptureDurableClientAuthorization(
                durable_session, durable_attempt, durable_intent,
                durable_quote);
        }
        if (!error.empty()) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, durable_session, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }
    // Existing durable authorizations above retain their exact recovery
    // path. A new request must respect current feature availability.
    if (restricted && !CheckNewSponsorshipScope(SponsorshipScope::RESTRICTED, error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }
    if (restricted) {
        const std::vector<unsigned char> key_bytes =
            *input.identity_key;
        if (key_bytes.size() != 32) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               "provider_identity_key must be exactly 32 bytes");
        }
        restricted_identity_key.emplace(MakeUCharSpan(key_bytes));
        RestrictedServiceDescriptor descriptor;
        SponsorshipCapability capability;
        if (!restricted_identity_key->IsFullyValid() ||
            !DeserializePaymasterBytes(*input.descriptor,
                                     "RESTRICTED_DESCRIPTOR", descriptor, error) ||
            !DeserializePaymasterBytes(*input.capability,
                                     "SPONSORSHIP_CAPABILITY", capability, error)) {
            throw ApplicationError(ErrorKind::DESERIALIZATION,
                               error.empty() ? "PAYMASTER_INVALID_RESTRICTED_IDENTITY" : error);
        }
        if (provider_id != descriptor.provider_id ||
            (requested_offer && *requested_offer != descriptor.offer_id) ||
            !ValidateRestrictedServiceDescriptor(descriptor, *restricted_identity_key,
                                                 Params().GenesisBlock().GetHash(), GetTime(), error,
                                                 Params().GetChainType() == ChainType::REGTEST) ||
            !ValidateSponsorshipCapability(capability, descriptor, recipient_script,
                                           DDCents{payment}, capability.payment_request_nonce,
                                           Params().GenesisBlock().GetHash(), GetTime(), error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               error.empty() ? "PAYMASTER_RESTRICTED_BINDING_MISMATCH" : error);
        }
        node::NodeContext* node = wallet->chain().context();
        if (!node || !node->chainman ||
            !ValidateRestrictedAdmissionProofs(descriptor, *restricted_identity_key,
                                               *node->chainman, error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               error.empty() ? "PAYMASTER_RESTRICTED_ADMISSION_UNAVAILABLE" : error);
        }
        if (privacy == PrivacyProfile::HIGH && !descriptor.p2p_endpoint.IsTor()) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               "High privacy requires an onion restricted provider");
        }
        restricted_descriptor = std::move(descriptor);
        restricted_capability = std::move(capability);
    }

    // Resolve an exact terminal retry from local durable authority
    // before consulting the live directory. The provider resources
    // used by the completed transaction are expected to be spent, so
    // requiring a fresh announcement/capacity check here would make
    // the idempotent tombstone dependent on remote state. All fields
    // which define the local order are still committed below; a hash
    // mismatch remains a hard conflict in CreateOrJoinSession().
    const uint256 canonical_request_hash = CanonicalClientRequestHash(
        request_id, recipient_script, DDCents{subtract_fee ? requested_amount : payment},
        DDCents{fee_cap}, requested_inputs, requested_mode, privacy, selection,
        maximum_attempts, subtract_fee, send_all, restricted_identity_key,
        restricted_descriptor, restricted_capability);
    if (have_durable_session && IsTerminal(durable_session.state)) {
        if (durable_session.canonical_request_hash !=
                canonical_request_hash ||
            durable_session.fee_mode_requested != requested_mode) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_REQUEST_ID_CONFLICT");
        }
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, durable_session, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        return CaptureSessionPayment(durable_session, &store);
    }

    const DDCents effective_fee_cap =
        EffectiveClientServiceFeeCap(*wallet, DDCents{fee_cap}, error);
    if (effective_fee_cap.value < 0) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    std::vector<PaymasterReliabilityRecord> records;
    if (!store.ListProviderReliability(records)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_REPUTATION_DATABASE_READ");
    }
    std::map<PaymasterId, PaymasterReliabilityRecord> reputation;
    for (auto& record : records)
        reputation.emplace(record.provider_id, std::move(record));
    OfferCandidate candidate;
    if (restricted) {
        const auto& descriptor = *restricted_descriptor;
        candidate.provider_id = descriptor.provider_id;
        candidate.identity_key = *restricted_identity_key;
        candidate.display_name = descriptor.sponsor_display_name;
        candidate.endpoint = descriptor.p2p_endpoint;
        candidate.announcement_sequence = descriptor.admission_sequence;
        candidate.announcement_expires_at = descriptor.expires_at;
        candidate.terms = OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                     FundingModel::SPONSORED,
                                     SponsorshipScope::RESTRICTED, 0,
                                     DDCents{payment}, DDCents{payment}};
        candidate.payment = DDCents{payment};
        candidate.service_fee = DDCents{0};
        candidate.user_total = DDCents{payment};
    } else {
        const auto candidates = BuildOfferCandidates(
            context.paymaster->GetDirectory().List(now), DDCents{payment}, FUNDING_MODEL_ALL,
            effective_fee_cap, reputation, now, error);
        if (!error.empty()) throw ApplicationError(ErrorKind::INVALID_INPUT, error);
        const auto candidate_it = std::find_if(candidates.begin(), candidates.end(),
                                               [&](const OfferCandidate& entry) {
                                                   return entry.provider_id == provider_id &&
                                                          (privacy != PrivacyProfile::HIGH || entry.endpoint.IsTor()) &&
                                                          (!requested_offer || entry.terms.offer_id == *requested_offer);
                                               });
        if (candidate_it == candidates.end()) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_REQUESTED_OFFER_UNAVAILABLE");
        }
        candidate = *candidate_it;
    }
    if ((subtract_fee &&
         candidate.user_total != DDCents{requested_amount}) ||
        (!subtract_fee && candidate.payment != DDCents{requested_amount})) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "PAYMASTER_NO_EXACT_GROSS_OFFER");
    }

    PaymentSession session;
    if (!CheckPaymasterClientServiceFee(*wallet, candidate.payment, candidate.service_fee, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    const auto create_result = store.CreateOrJoinSession(
        request_id, canonical_request_hash, requested_mode, now, session, error);
    if (create_result == CreatePaymasterSessionResult::CONFLICT ||
        create_result == CreatePaymasterSessionResult::DATABASE_ERROR) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (create_result == CreatePaymasterSessionResult::FINAL_TOMBSTONE) {
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, session, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        return CaptureSessionPayment(session, &store);
    }
    if (!store.BindClientPaymentOrder(
            request_id, DDCents{requested_amount}, subtract_fee,
            send_all, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    session.requested_amount = DDCents{requested_amount};
    session.subtract_paymaster_fee_from_amount = subtract_fee;
    session.send_all_spendable_dd = send_all;

    ProviderAttempt attempt;
    PaymentIntent intent;
    bool resume_attempt{false};
    for (auto it = session.attempt_ids.rbegin(); it != session.attempt_ids.rend(); ++it) {
        ProviderAttempt existing;
        if (!store.GetAttempt(*it, existing) || existing.unsigned_intent.empty()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PERSISTED_INTENT_CORRUPT");
        }
        PaymentIntent existing_intent;
        try {
            SpanReader stream{::PROTOCOL_VERSION, existing.unsigned_intent};
            stream >> existing_intent;
            if (!stream.empty()) throw std::ios_base::failure("trailing intent data");
        } catch (const std::ios_base::failure&) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PERSISTED_INTENT_CORRUPT");
        }
        if (existing.provider_id == provider_id &&
            existing_intent.offer_id == candidate.terms.offer_id &&
            existing.state != AttemptState::REJECTED &&
            existing.state != AttemptState::QUOTE_EXPIRED) {
            if (existing_intent.canonical_request_hash != canonical_request_hash ||
                existing_intent.requested_fee_mode != requested_mode ||
                existing_intent.privacy_profile != privacy ||
                existing_intent.selection_mode != selection ||
                existing.privacy_profile != privacy) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
            }
            attempt = std::move(existing);
            intent = std::move(existing_intent);
            resume_attempt = true;
            break;
        }
    }
    if (input.retry_transport) {
        if (const auto* node = wallet->chain().context(); node && node->connman) {
            node->connman->RetryPaymasterConnection(PaymentChannelKey(*wallet, session, provider_id));
        }
    }
    std::shared_ptr<DirectLease> reservation_lease;
    if (!resume_attempt) {
        if (!candidate.HasPreparationWindow(now)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_ANNOUNCEMENT_EXPIRING");
        }
        auto* transport_node = wallet->chain().context();
        if (!transport_node || !transport_node->connman) throw ApplicationError(ErrorKind::INTERNAL, "Node context unavailable");
        const auto transport = transport_node->connman->RequestPaymasterConnection(
            PaymentChannelKey(*wallet, session, provider_id), candidate.endpoint, privacy == PrivacyProfile::HIGH);
        if (transport.state != DirectState::READY || !transport.lease) {
            if (!transport.Pending()) throw ApplicationError(ErrorKind::NODE_CAPACITY, PaymasterTransportError(transport.state));
            PaymentResult result = CaptureSessionPayment(session);
            result.provider_id = provider_id;
            result.offer_id = candidate.terms.offer_id;
            result.policy_hash = candidate.terms.policy_hash;
            result.payment_cents = candidate.payment;
            result.service_fee_cents = candidate.service_fee;
            result.user_total_cents = candidate.user_total;
            result.transport = CapturePaymasterTransport(*transport_node->connman, transport.state);
            result.queued = false;
            result.connection_pending = true;
            result.capacity_pending = true;
            return result;
        }
        reservation_lease = transport.lease;
        if (session.attempt_ids.size() >= static_cast<size_t>(maximum_attempts)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_MAXIMUM_PROVIDER_ATTEMPTS_REACHED");
        }
        if (!session.attempt_ids.empty() &&
            session.state != SessionState::INPUTS_RESERVED) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_FALLBACK_REQUIRES_EXPLICIT_ABANDON");
        }
        DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
        if (!dd_wallet) throw ApplicationError(ErrorKind::WALLET, "DigiDollar wallet not initialized");
        std::vector<COutPoint> selected_inputs;
        std::vector<CAmount> selected_amounts;
        CAmount selected_total{0};
        if (!session.attempt_ids.empty()) {
            if (requested_inputs != session.user_inputs) {
                throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_FALLBACK_INPUT_CONFLICT");
            }
            selected_inputs = session.user_inputs;
            for (const COutPoint& outpoint : selected_inputs) {
                const CAmount input_amount = dd_wallet->GetDDFromUTXO(outpoint);
                if (input_amount <= 0 ||
                    input_amount > std::numeric_limits<CAmount>::max() - selected_total) {
                    throw ApplicationError(ErrorKind::WALLET,
                                       "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
                }
                selected_amounts.push_back(input_amount);
                selected_total += input_amount;
            }
            if (selected_total < candidate.user_total.value) {
                throw ApplicationError(ErrorKind::INSUFFICIENT_FUNDS,
                                   "PAYMASTER_FALLBACK_FEE_EXCEEDS_RESERVED_INPUTS");
            }
        } else if (!dd_wallet->SelectDDCoins(candidate.user_total.value, requested_inputs,
                                             selected_inputs, selected_total,
                                             &selected_amounts, &error)) {
            throw ApplicationError(ErrorKind::INSUFFICIENT_FUNDS, error);
        }
        CScript change_script;
        if (selected_total > candidate.user_total.value) {
            const auto change = wallet->GetNewDestination(OutputType::BECH32M,
                                                          "Paymaster DD change");
            if (!change) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CHANGE_UNAVAILABLE");
            change_script = GetScriptForDestination(*change);
        }
        PaymentIntentParameters parameters;
        parameters.genesis_hash = Params().GenesisBlock().GetHash();
        parameters.request_id = request_id;
        parameters.session_id = session.session_id;
        parameters.canonical_request_hash = canonical_request_hash;
        parameters.requested_fee_mode = requested_mode;
        parameters.privacy_profile = privacy;
        parameters.selection_mode = selection;
        if (restricted) {
            parameters.client_nonce = restricted_capability->payment_request_nonce;
            parameters.sponsorship_authorization_hash =
                GetSponsorshipCapabilityHash(*restricted_capability);
        } else {
            do {
                parameters.client_nonce = GetRandHash();
            } while (parameters.client_nonce.IsNull());
        }
        parameters.user_dd_inputs = selected_inputs;
        parameters.recipient_script = recipient_script;
        parameters.user_dd_change_script = change_script;
        parameters.expires_at = std::min(
            SaturatingAddSeconds(now, DEFAULT_QUOTE_TTL_SECONDS),
            candidate.announcement_expires_at);
        auto unsigned_intent = BuildUnsignedPaymentIntent(candidate, parameters, now, error);
        if (!unsigned_intent) throw ApplicationError(ErrorKind::WALLET, error);
        intent = std::move(*unsigned_intent);
        do {
            attempt.attempt_id = GetRandHash();
        } while (attempt.attempt_id.IsNull());
        attempt.provider_id = provider_id;
        attempt.provider_identity_key = candidate.identity_key;
        attempt.provider_endpoint = candidate.endpoint.ToStringAddrPort();
        attempt.privacy_profile = privacy;
        attempt.client_nonce = intent.client_nonce;
        attempt.sponsorship_capability_hash = intent.sponsorship_authorization_hash;
        attempt.unsigned_intent = SerializePaymentIntent(intent);
        attempt.created_at = attempt.updated_at = now;
        if (!store.PreparePaymentIntent(request_id, attempt, now, error) ||
            !store.GetSessionByRequestId(request_id, session)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    node::NodeContext* node = wallet->chain().context();
    if (!node || !node->connman || !node->chainman) {
        throw ApplicationError(ErrorKind::INTERNAL, "Node context unavailable");
    }
    PaymasterCapacityRequest capacity_request;
    if (attempt.capacity_request.empty()) {
        capacity_request.genesis_hash = Params().GenesisBlock().GetHash();
        capacity_request.provider_id = provider_id;
        capacity_request.request_id = request_id;
        capacity_request.session_id = session.session_id;
        capacity_request.client_nonce = attempt.client_nonce;
        capacity_request.funding_model = intent.funding_model;
        capacity_request.requires_carrier =
            intent.funding_model == FundingModel::USER_PAID &&
            candidate.service_fee.value > 0 &&
            candidate.service_fee.value < 100;
        capacity_request.requested_slots = 1;
        capacity_request.created_at = now;
        capacity_request.expires_at = std::min(
            intent.expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        if (capacity_request.expires_at <= now) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_CAPACITY_REQUEST_EXPIRED");
        }
        const std::vector<unsigned char> capacity_bytes =
            SerializeCapacityRequest(capacity_request);
        if (!store.PrepareCapacityRequest(request_id, attempt.attempt_id,
                                          capacity_bytes, now, error) ||
            !store.GetAttempt(attempt.attempt_id, attempt)) {
            throw ApplicationError(ErrorKind::WALLET,
                               error.empty() ? "PAYMASTER_ATTEMPT_NOT_FOUND" : error);
        }
    } else {
        try {
            CDataStream stream{attempt.capacity_request, SER_NETWORK,
                               ::PROTOCOL_VERSION};
            stream >> capacity_request;
            if (!stream.empty() ||
                SerializeCapacityRequest(capacity_request) !=
                    attempt.capacity_request) {
                throw std::ios_base::failure("non-canonical capacity request");
            }
        } catch (const std::ios_base::failure&) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT");
        }
    }

    if (capacity_request.expires_at <= now && attempt.capacity_snapshot.snapshot_id.IsNull()) {
        RecordUnavailableClientProvider(store, session, attempt, now);
        node->connman->ReleasePaymasterConnection(PaymentChannelKey(*wallet, session, provider_id));
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CAPACITY_REQUEST_EXPIRED");
    }
    PaymasterCapacityProof capacity_proof;
    bool capacity_ready = !attempt.capacity_snapshot.snapshot_id.IsNull();
    if (!capacity_ready) {
        capacity_ready = TryCommitClientCapacityProof(
            *context.paymaster, store, input, session, attempt,
            capacity_request, *node->chainman, now, error);
    }
    if (!DrainClientAttemptEquivocations(
            *context.paymaster, store, session, attempt, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (!capacity_ready) {
        const auto transport = node->connman->RequestPaymasterConnection(
            PaymentChannelKey(*wallet, session, provider_id), candidate.endpoint, privacy == PrivacyProfile::HIGH);
        const NodeId peer_id = transport.lease && transport.state == DirectState::READY ? transport.lease->peer_id.load() : -1;
        bool capacity_queued{false};
        bool capacity_connection_pending{transport.Pending()};
        if (peer_id == -1) {
            if (!capacity_connection_pending) {
                if (transport.state == DirectState::CONNECT_FAILED || transport.state == DirectState::FAILED ||
                    transport.state == DirectState::EXPIRED) {
                    RecordUnavailableClientProvider(store, session, attempt, now);
                }
                throw ApplicationError(ErrorKind::NODE_CAPACITY, PaymasterTransportError(transport.state));
            }
        } else {
            const uint256 message_id = Hash(attempt.capacity_request);
            capacity_queued =
                context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                context.paymaster->QueueCapacityRequest(
                    peer_id, message_id, attempt.capacity_request.size(),
                    capacity_request, now);
            if (!capacity_queued) {
                throw ApplicationError(ErrorKind::NODE_CAPACITY,
                                   "PAYMASTER_DIRECT_QUEUE_FULL");
            }
            node->connman->WakeMessageHandler();
        }
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_SESSION_NOT_FOUND");
        }
        PaymentResult result = CaptureSessionPayment(session);
        result.provider_id = provider_id;
        result.offer_id = candidate.terms.offer_id;
        result.policy_hash = candidate.terms.policy_hash;
        result.funding_model = intent.funding_model;
        result.payment_cents = candidate.payment;
        result.service_fee_cents = candidate.service_fee;
        result.user_total_cents = candidate.user_total;
        result.expires_at = intent.expires_at;
        result.transport = CapturePaymasterTransport(*node->connman, transport.state);
        result.queued = capacity_queued;
        result.connection_pending = capacity_connection_pending;
        result.capacity_pending = true;
        return result;
    }
    try {
        CDataStream stream{attempt.capacity_snapshot.capacity_proof,
                           SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> capacity_proof;
        if (!stream.empty() ||
            SerializeCapacityProof(capacity_proof) !=
                attempt.capacity_snapshot.capacity_proof) {
            throw std::ios_base::failure("non-canonical capacity proof");
        }
    } catch (const std::ios_base::failure&) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PERSISTED_CAPACITY_PROOF_CORRUPT");
    }
    // Re-run the same full firewall immediately before any intent
    // signature or network disclosure. A now-spent capacity outpoint
    // invalidates the attempt without exposing the user's intent.
    if (!ValidateCapacityProofAgainstChainstate(
            capacity_proof, capacity_request, attempt.provider_identity_key,
            *node->chainman, now, error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }

    bool queued{false};
    bool connection_pending{false};
    bool quote_received{false};
    PaymasterQuoteRequest quote_request;
    std::vector<unsigned char> request_bytes;
    if (attempt.quote_request.empty()) {
        if (wallet->IsLocked()) {
            if (session.state == SessionState::INPUTS_RESERVED &&
                !store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                         PendingPhase::NONE, {}, now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
        } else {
            if ((session.state == SessionState::INPUTS_RESERVED ||
                 session.state == SessionState::AWAITING_WALLET_UNLOCK) &&
                !store.TransitionSession(request_id, SessionState::AWAITING_USER_SIGNATURE,
                                         PendingPhase::NONE, {}, now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt,
                    error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            std::vector<XOnlyPubKey> output_keys;
            if (!SignPaymentIntentInputs(*wallet, intent, now, output_keys, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            std::vector<std::vector<unsigned char>> signatures;
            for (const auto& proof : intent.user_input_proofs)
                signatures.push_back(proof.signature);
            intent.user_input_proofs.clear();
            auto finalized = FinalizeQuoteRequest(
                intent, output_keys, signatures, restricted_descriptor,
                restricted_capability, now, error);
            if (!finalized) throw ApplicationError(ErrorKind::WALLET, error);
            quote_request = std::move(*finalized);
            request_bytes = SerializeQuoteRequest(quote_request);
            if (!store.FinalizePaymentIntent(request_id, attempt.attempt_id,
                                             SerializeRedactedQuoteRequest(quote_request),
                                             now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_ATTEMPT_NOT_FOUND");
            }
        }
    } else {
        try {
            CDataStream stream{attempt.quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
            stream >> quote_request;
            if (!stream.empty()) throw std::ios_base::failure("trailing request data");
        } catch (const std::ios_base::failure&) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PERSISTED_REQUEST_CORRUPT");
        }
        quote_request.restricted_descriptor = restricted_descriptor;
        quote_request.restricted_capability = restricted_capability;
        if (quote_request.intent.expires_at <= now) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               "PAYMASTER_QUOTE_REQUEST_EXPIRED");
        }
        if (!ValidateQuoteRequestEnvelope(quote_request, Params().GenesisBlock().GetHash(),
                                          now, error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, error);
        }
        request_bytes = SerializeQuoteRequest(quote_request);
    }

    if (!request_bytes.empty()) {
        // Pairwise equivocation is signature/binding evidence, not an
        // authorization decision. Record it before any rejected quote
        // is acknowledged by the deeper local spend firewall.
        if (!DrainClientAttemptEquivocations(
                *context.paymaster, store, session, attempt, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        auto response_lease = context.paymaster->LeaseQuoteResponses(
            request_id, session.session_id, provider_id,
            GetPaymentIntentHash(quote_request.intent),
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
        const auto& responses = response_lease.Messages();
        bool quote_equivocation{false};
        if (!PersistPendingQuoteEquivocationPair(
                store, attempt.attempt_id, responses,
                quote_equivocation, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (quote_equivocation) {
            if (!AcknowledgeEquivocationMessages(
                    *context.paymaster, responses, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_QUOTE_EQUIVOCATION");
        }
        std::string rejected_quote_error;
        ErrorKind rejected_quote_code{ErrorKind::INVALID_INPUT};
        for (const DirectMessage& direct : responses) {
            const auto* response =
                std::get_if<PaymasterQuoteResponse>(&direct.payload);
            if (!response) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                rejected_quote_error =
                    "PAYMASTER_INVALID_QUOTE_RESPONSE";
                continue;
            }

            bool claim_equivocation{false};
            const std::vector<unsigned char> encoded_quote =
                SerializeQuoteResponse(*response);
            if (!store.StageQuoteResponseClaimCandidate(
                    attempt.attempt_id, encoded_quote,
                    direct.received_at, claim_equivocation, error)) {
                if (error !=
                    "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE") {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                rejected_quote_error =
                    "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(
                        *context.paymaster, responses, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_QUOTE_EQUIVOCATION");
            }
            if (!store.GetAttempt(attempt.attempt_id, attempt)) {
                throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_ATTEMPT_NOT_FOUND");
            }

            CollaborativePSBTTemplate trusted;
            ClientAuthorizationManifest client_manifest;
            std::string response_error;
            ErrorKind response_error_code{ErrorKind::INVALID_INPUT};
            bool valid_response{
                ValidateQuoteResponseForRequest(*response, quote_request, candidate.terms, candidate.identity_key, effective_fee_cap, now, response_error) &&
                ValidateQuoteAgainstCapacitySnapshot(
                    response->quote, attempt.capacity_snapshot,
                    response_error) &&
                BuildTrustedQuoteTemplate(
                    *wallet, quote_request.intent, response->quote,
                    Params(), *node->chainman, trusted,
                    response_error) &&
                BuildClientAuthorizationManifest(
                    quote_request.intent, response->quote,
                    attempt.capacity_snapshot, trusted,
                    effective_fee_cap,
                    session.requested_amount.value > 0
                        ? session.requested_amount
                        : quote_request.intent.recipient_amount,
                    session.subtract_paymaster_fee_from_amount,
                    session.send_all_spendable_dd, client_manifest,
                    response_error)};
            if (valid_response &&
                !ValidateClientAuthorizationOwnership(
                    *wallet, client_manifest, response_error)) {
                valid_response = false;
                response_error_code = ErrorKind::WALLET;
            }
            if (!valid_response) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                rejected_quote_error =
                    response_error.empty() ? "PAYMASTER_INVALID_QUOTE_RESPONSE" : std::move(response_error);
                rejected_quote_code = response_error_code;
                continue;
            }
            attempt.state = AttemptState::QUOTED;
            attempt.intent_hash = response->quote.intent_hash;
            attempt.quote_id = response->quote.quote_id;
            attempt.unsigned_txid = response->quote.unsigned_txid;
            attempt.template_commitment = response->quote.template_commitment;
            attempt.commit_key = GetPaymasterCommitKey(
                provider_id, attempt.client_nonce, attempt.intent_hash,
                attempt.quote_id, attempt.template_commitment);
            attempt.signed_quote = SerializeQuoteResponse(*response);
            attempt.client_manifest = std::move(client_manifest);
            attempt.unsigned_transaction = SerializeTransaction(response->quote.unsigned_transaction);
            attempt.unsigned_psbt = SerializePSBT(trusted.psbt);
            attempt.input_roles.clear();
            attempt.input_roles.reserve(trusted.input_roles.size());
            static_assert(static_cast<uint8_t>(InputRole::USER_DD) ==
                          static_cast<uint8_t>(ReservationRole::USER_DD));
            static_assert(static_cast<uint8_t>(InputRole::PROVIDER_DGB) ==
                          static_cast<uint8_t>(ReservationRole::PROVIDER_DGB));
            for (const InputRole role : trusted.input_roles) {
                attempt.input_roles.push_back(static_cast<ReservationRole>(role));
            }
            attempt.quote_expires_at = response->quote.expires_at;
            attempt.retry_until = response->quote.retry_until;
            attempt.updated_at = std::max(attempt.updated_at, now);
            if (!store.UpdateAttempt(request_id, attempt, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            quote_received = true;
            node->connman->ReleasePaymasterConnection(PaymentChannelKey(*wallet, session, provider_id));
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt,
                    error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            break;
        }
        if (!quote_received && !rejected_quote_error.empty()) {
            throw ApplicationError(rejected_quote_code,
                               rejected_quote_error);
        }

        NodeId peer_id{-1};
        if (!quote_received && attempt.state == AttemptState::CANDIDATE) {
            const auto transport = node->connman->RequestPaymasterConnection(
                PaymentChannelKey(*wallet, session, provider_id), candidate.endpoint, privacy == PrivacyProfile::HIGH);
            peer_id = transport.lease && transport.state == DirectState::READY ? transport.lease->peer_id.load() : -1;
            if (peer_id == -1) {
                connection_pending = transport.Pending();
                if (!connection_pending) {
                    if (transport.state == DirectState::CONNECT_FAILED || transport.state == DirectState::FAILED ||
                        transport.state == DirectState::EXPIRED) {
                        RecordUnavailableClientProvider(store, session, attempt, now);
                    }
                    throw ApplicationError(ErrorKind::NODE_CAPACITY, PaymasterTransportError(transport.state));
                }
            } else {
                const uint256 message_id = Hash(request_bytes);
                queued = context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                         context.paymaster->QueueOutboundDirectMessage(
                             peer_id, message_id, request_bytes.size(), DirectPayload{quote_request}, now);
                if (!queued) throw ApplicationError(ErrorKind::NODE_CAPACITY,
                                                "PAYMASTER_DIRECT_QUEUE_FULL");
                node->connman->WakeMessageHandler();
            }
        }
    }
    if (!store.GetSessionByRequestId(request_id, session)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND");
    }
    PaymentResult result = CaptureSessionPayment(session);
    result.provider_id = provider_id;
    result.offer_id = candidate.terms.offer_id;
    result.policy_hash = candidate.terms.policy_hash;
    result.funding_model = intent.funding_model;
    result.payment_cents = candidate.payment;
    result.service_fee_cents = candidate.service_fee;
    result.user_total_cents = candidate.user_total;
    result.expires_at = intent.expires_at;
    result.queued = queued;
    result.connection_pending = connection_pending;
    result.capacity_pending = false;
    result.capacity_snapshot_id = attempt.capacity_snapshot.snapshot_id;
    if (!attempt.quote_id.IsNull()) {
        result.quote_id = attempt.quote_id;
        result.unsigned_txid = attempt.unsigned_txid;
        result.template_commitment = attempt.template_commitment;
        result.authorization_commitment = attempt.client_manifest.manifest_id;
        const bool authorization_accepted =
            attempt.accepted_client_manifest_id ==
                attempt.client_manifest.manifest_id &&
            attempt.client_manifest_accepted_at > 0;
        result.authorization_accepted = authorization_accepted;
        if (authorization_accepted) {
            result.authorization_accepted_at = attempt.client_manifest_accepted_at;
        }
        result.psbt = attempt.unsigned_psbt;
    }
    return result;
}
PaymentResult RequestAutomaticPaymasterPayment(WalletContext& context, CWallet& target,
    const AutomaticPaymentParameters& input, const std::vector<COutPoint>* preset_inputs)
{
    ValidateClientPaymentOptions(input);
    CWallet* const wallet = &target;
    std::string readiness_error;
    if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
        throw ApplicationError(ErrorKind::GENERAL, readiness_error);
    }

    const auto& request_id = input.request_id;
    const auto& address = input.address;
    const int64_t amount = input.amount, fee_cap = input.fee_cap;
    if (amount < 100 || amount > MAX_DD_OUTPUT_CENTS)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "Paymaster amount or fee cap is out of range");
    const auto requested_mode = input.mode;
    const bool subtract_fee = input.subtract_fee, send_all = input.send_all;
    const bool prepare_only = input.prepare_only;
    const auto& supplied_authorization_commitment = input.authorization_commitment;
    if (prepare_only && supplied_authorization_commitment) {
        throw ApplicationError(
            ErrorKind::INVALID_INPUT,
            "prepare_only and authorization_commitment are mutually exclusive");
    }
    const auto privacy = input.privacy;
    const auto selection = input.selection;
    const int maximum_attempts = input.maximum_attempts;
    if (!CheckPaymasterPrivacyReadiness(privacy, context, readiness_error)) {
        throw ApplicationError(ErrorKind::GENERAL, readiness_error);
    }

    const bool have_restricted_key = input.identity_key.has_value();
    const auto& preferred_provider = input.preferred_provider;
    const auto& preferred_offer = input.preferred_offer;
    const bool have_restricted_descriptor = input.descriptor.has_value();
    const bool have_restricted_capability = input.capability.has_value();
    if (preferred_provider.has_value() != preferred_offer.has_value())
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_PREFERRED_OFFER_PAIR_REQUIRED");
    const bool restricted = have_restricted_key || have_restricted_descriptor ||
                            have_restricted_capability;
    std::optional<XOnlyPubKey> restricted_identity_key;
    std::optional<RestrictedServiceDescriptor> restricted_descriptor;
    std::optional<SponsorshipCapability> restricted_capability;
    OfferCandidate restricted_candidate;
    std::string error;
    PaymasterStore store{*wallet};
    const int64_t now = GetTime();
    PaymentSession durable_session;
    if (store.GetSessionByRequestId(request_id, durable_session) &&
        !IsTerminal(durable_session.state)) {
        ProviderAttempt durable_attempt;
        PaymentIntent durable_intent;
        PaymasterQuote durable_quote;
        if (FindDurableClientAuthorizationAttempt(
                *wallet, store, durable_session, now, durable_attempt,
                durable_intent, durable_quote, error)) {
            const CDigiDollarAddress dd_address{address};
            const CScript recipient_script = GetScriptForDestination(
                dd_address.GetDigiDollarDestination());
            if (durable_intent.request_id != request_id ||
                durable_intent.session_id != durable_session.session_id ||
                durable_intent.canonical_request_hash !=
                    durable_session.canonical_request_hash ||
                durable_intent.requested_fee_mode !=
                    durable_session.fee_mode_requested ||
                durable_intent.requested_fee_mode != requested_mode ||
                durable_intent.privacy_profile != privacy ||
                durable_intent.selection_mode != selection ||
                durable_attempt.privacy_profile != privacy ||
                durable_intent.recipient_script != recipient_script ||
                (subtract_fee
                     ? (durable_intent.recipient_amount.value >
                            std::numeric_limits<int64_t>::max() -
                                durable_quote.service_fee.value ||
                        durable_intent.recipient_amount.value +
                                durable_quote.service_fee.value !=
                            amount)
                     : durable_intent.recipient_amount != DDCents{amount}) ||
                (durable_session.requested_amount.value == 0
                     ? (subtract_fee || send_all)
                     : (durable_session.requested_amount != DDCents{amount} ||
                        durable_session.subtract_paymaster_fee_from_amount !=
                            subtract_fee ||
                        durable_session.send_all_spendable_dd != send_all)) ||
                durable_quote.service_fee.value > fee_cap ||
                (preset_inputs &&
                 *preset_inputs != durable_session.user_inputs)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, durable_session, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            PaymentResult resume = CaptureDurableClientAuthorization(
                durable_session, durable_attempt, durable_intent,
                durable_quote);
            resume.authorization_required = !supplied_authorization_commitment.has_value();
            if (!supplied_authorization_commitment) return resume;
            if (*supplied_authorization_commitment !=
                durable_attempt.client_manifest.manifest_id) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
            }

            const auto copy_resume_fields = [&resume](PaymentResult& target) {
                FillMissingPaymentTerms(target, resume);
                if (!target.authorization_accepted) target.authorization_accepted = resume.authorization_accepted;
                if (!target.authorization_accepted_at) target.authorization_accepted_at = resume.authorization_accepted_at;
            };

            // A provider can commit and broadcast the exact transaction
            // between two client polls. Consume and validate that signed
            // result before attempting to queue the already-authorized USER
            // PSBT again. Otherwise the retry firewall correctly observes
            // the provider Capacity outpoint as spent, but reports a false
            // failure even though it was spent by the exact final transaction
            // that is waiting in the result inbox.
            PaymentResult provider_result = ProcessPaymasterResult(context, *wallet, request_id);
            if (provider_result.processed.value_or(false)) {
                copy_resume_fields(provider_result);
                return provider_result;
            }

            // The exact final transaction may already be in stem relay or
            // the mempool even when there is no unread result envelope left
            // (for example, immediately after the first successful automatic
            // poll or after a restart).  Its Capacity inputs are necessarily
            // spent by that transaction.  Return the durable status instead
            // of re-running walletprocesspaymasterpsbt, which would otherwise
            // misclassify the known exact spend as a foreign Capacity spend.
            if (durable_session.state == SessionState::STEMPOOL ||
                durable_session.state == SessionState::MEMPOOL ||
                !durable_session.final_txid.IsNull()) {
                PaymentResult committed = CaptureSessionPayment(durable_session, &store);
                copy_resume_fields(committed);
                return committed;
            }

            if (wallet->IsLocked() &&
                durable_attempt.state == AttemptState::QUOTED) {
                return resume;
            }

            if (input.retry_transport) {
                if (const auto* node = wallet->chain().context(); node && node->connman) {
                    node->connman->RetryPaymasterConnection(
                        PaymentChannelKey(*wallet, durable_session, durable_attempt.provider_id));
                }
            }
            PaymentResult authorization = AuthorizePaymasterPSBT(context, *wallet,
                durable_attempt.unsigned_psbt, supplied_authorization_commitment);
            copy_resume_fields(authorization);
            provider_result = ProcessPaymasterResult(context, *wallet, request_id);
            if (provider_result.processed.value_or(false)) {
                copy_resume_fields(provider_result);
                return provider_result;
            }
            return authorization;
        }
        if (!error.empty()) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, durable_session, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }
    // Do not apply the new-request gate to the durable authorization resume above.
    if (restricted && !CheckNewSponsorshipScope(SponsorshipScope::RESTRICTED, error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }
    const DDCents effective_fee_cap =
        EffectiveClientServiceFeeCap(*wallet, DDCents{fee_cap}, error);
    if (effective_fee_cap.value < 0) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (restricted) {
        const std::vector<unsigned char> key_bytes =
            *input.identity_key;
        if (key_bytes.size() != 32) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               "provider_identity_key must be exactly 32 bytes");
        }
        restricted_identity_key.emplace(MakeUCharSpan(key_bytes));
        RestrictedServiceDescriptor descriptor;
        SponsorshipCapability capability;
        const CDigiDollarAddress dd_address{address};
        const CScript recipient_script = GetScriptForDestination(dd_address.GetDigiDollarDestination());
        if (!restricted_identity_key->IsFullyValid() ||
            !DeserializePaymasterBytes(*input.descriptor,
                                     "RESTRICTED_DESCRIPTOR", descriptor, error) ||
            !DeserializePaymasterBytes(*input.capability,
                                     "SPONSORSHIP_CAPABILITY", capability, error) ||
            !ValidateRestrictedServiceDescriptor(
                descriptor, *restricted_identity_key, Params().GenesisBlock().GetHash(),
                GetTime(), error, Params().GetChainType() == ChainType::REGTEST) ||
            !ValidateSponsorshipCapability(
                capability, descriptor, recipient_script, DDCents{amount},
                capability.payment_request_nonce, Params().GenesisBlock().GetHash(),
                GetTime(), error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               error.empty() ? "PAYMASTER_INVALID_RESTRICTED_CONTEXT" : error);
        }
        node::NodeContext* node = wallet->chain().context();
        if (!node || !node->chainman ||
            !ValidateRestrictedAdmissionProofs(descriptor, *restricted_identity_key,
                                               *node->chainman, error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               error.empty() ? "PAYMASTER_RESTRICTED_ADMISSION_UNAVAILABLE" : error);
        }
        if (privacy == PrivacyProfile::HIGH && !descriptor.p2p_endpoint.IsTor()) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               "High privacy requires an onion restricted provider");
        }
        restricted_candidate.provider_id = descriptor.provider_id;
        restricted_candidate.identity_key = *restricted_identity_key;
        restricted_candidate.display_name = descriptor.sponsor_display_name;
        restricted_candidate.endpoint = descriptor.p2p_endpoint;
        restricted_candidate.announcement_sequence = descriptor.admission_sequence;
        restricted_candidate.announcement_expires_at = descriptor.expires_at;
        restricted_candidate.terms = OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                                FundingModel::SPONSORED,
                                                SponsorshipScope::RESTRICTED, 0,
                                                DDCents{amount}, DDCents{amount}};
        restricted_candidate.payment = DDCents{amount};
        restricted_candidate.service_fee = DDCents{0};
        restricted_candidate.user_total = DDCents{amount};
        restricted_descriptor = std::move(descriptor);
        restricted_capability = std::move(capability);
    }

    std::optional<std::pair<PaymasterId, uint256>> persisted_selection;
    std::optional<std::vector<COutPoint>> persisted_inputs;
    std::set<PaymasterId> attempted_providers;
    std::optional<uint256> active_attempt_id;
    PaymentSession persisted_session;
    const bool have_persisted_session =
        store.GetSessionByRequestId(request_id, persisted_session);
    if (preferred_provider && have_persisted_session && !IsTerminal(persisted_session.state) &&
        !persisted_session.attempt_ids.empty()) {
        // The initial choice is durably bound by the first attempt. A changed
        // preference cannot retarget an existing order, including a replay.
        ProviderAttempt first_attempt;
        if (!store.GetAttempt(persisted_session.attempt_ids.front(), first_attempt)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DETAILS_UNAVAILABLE");
        }
        PaymentIntent first_intent;
        try {
            SpanReader stream{::PROTOCOL_VERSION, first_attempt.unsigned_intent};
            stream >> first_intent;
            if (!stream.empty()) throw std::runtime_error("Trailing intent data");
        } catch (const std::exception&) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DETAILS_UNAVAILABLE");
        }
        if (first_attempt.provider_id != *preferred_provider ||
            first_intent.offer_id != *preferred_offer) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_REQUEST_ID_CONFLICT");
        }
    }
    if (have_persisted_session) {
        if (IsTerminal(persisted_session.state)) {
            // After pruning, the current format retains the order hash but
            // not its original inputs. Require the caller's saved input set
            // to verify a send replay; identity-only status reads still work.
            const auto& original_inputs = preset_inputs ? *preset_inputs : persisted_session.user_inputs;
            if (original_inputs.empty()) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DETAILS_UNAVAILABLE");
            }
            const CDigiDollarAddress dd_address{address};
            const uint256 request_hash = CanonicalClientRequestHash(
                request_id, GetScriptForDestination(dd_address.GetDigiDollarDestination()),
                DDCents{amount}, DDCents{fee_cap}, original_inputs, requested_mode,
                privacy, selection, maximum_attempts, subtract_fee, send_all,
                restricted_identity_key, restricted_descriptor, restricted_capability);
            if (request_hash != persisted_session.canonical_request_hash ||
                requested_mode != persisted_session.fee_mode_requested) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_REQUEST_ID_CONFLICT");
            }
            if (persisted_session.requested_amount.value == 0
                    ? (subtract_fee || send_all)
                    : (persisted_session.requested_amount != DDCents{amount} ||
                       persisted_session.subtract_paymaster_fee_from_amount !=
                           subtract_fee ||
                       persisted_session.send_all_spendable_dd != send_all)) {
                throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_REQUEST_ID_CONFLICT");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, persisted_session, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            return CaptureSessionPayment(persisted_session, &store);
        }
        persisted_inputs = persisted_session.user_inputs;
        if (!persisted_session.attempt_ids.empty()) {
            for (const uint256& attempt_id : persisted_session.attempt_ids) {
                ProviderAttempt persisted_attempt;
                if (!store.GetAttempt(attempt_id, persisted_attempt) ||
                    persisted_attempt.unsigned_intent.empty()) {
                    throw ApplicationError(ErrorKind::WALLET,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                attempted_providers.insert(persisted_attempt.provider_id);
                if (attempt_id != persisted_session.attempt_ids.back() ||
                    persisted_attempt.state == AttemptState::REJECTED ||
                    persisted_attempt.state == AttemptState::QUOTE_EXPIRED) {
                    continue;
                }
                PaymentIntent persisted_intent;
                try {
                    SpanReader stream{::PROTOCOL_VERSION, persisted_attempt.unsigned_intent};
                    stream >> persisted_intent;
                    if (!stream.empty()) throw std::ios_base::failure("trailing intent data");
                    if (persisted_intent.user_dd_inputs != persisted_session.user_inputs) {
                        throw std::ios_base::failure("input binding mismatch");
                    }
                } catch (const std::ios_base::failure&) {
                    throw ApplicationError(ErrorKind::WALLET,
                                       "PAYMASTER_PERSISTED_INTENT_CORRUPT");
                }
                if (persisted_intent.expires_at <= now) {
                    if (persisted_attempt.signed_quote.empty() &&
                        (!persisted_attempt.quote_request.empty() ||
                         (!persisted_attempt.capacity_request.empty() && persisted_attempt.capacity_snapshot.snapshot_id.IsNull()))) {
                        RecordUnavailableClientProvider(store, persisted_session, persisted_attempt, now);
                    }
                    std::string abandon_error;
                    if (!store.AbandonClientAttemptForFallback(
                            request_id, attempt_id, now, abandon_error)) {
                        throw ApplicationError(ErrorKind::WALLET, abandon_error);
                    }
                } else {
                    persisted_selection = std::pair{persisted_attempt.provider_id,
                                                    persisted_intent.offer_id};
                    active_attempt_id = attempt_id;
                }
            }
        }
    }
    std::vector<PaymasterReliabilityRecord> records;
    if (!store.ListProviderReliability(records)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_REPUTATION_DATABASE_READ");
    }
    std::map<PaymasterId, PaymasterReliabilityRecord> reputation;
    for (auto& record : records)
        reputation.emplace(record.provider_id, std::move(record));
    std::vector<OfferCandidate> candidates;
    if (restricted) {
        if (!EnsureProviderNotEquivocationBlocked(
                store, restricted_candidate.provider_id, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        candidates.push_back(restricted_candidate);
    } else {
        candidates = subtract_fee
            ? BuildGrossOfferCandidates(
                  context.paymaster->GetDirectory().List(now),
                  DDCents{amount}, FUNDING_MODEL_ALL, effective_fee_cap,
                  reputation, now, error)
            : BuildOfferCandidates(
                  context.paymaster->GetDirectory().List(now),
                  DDCents{amount}, FUNDING_MODEL_ALL, effective_fee_cap,
                  reputation, now, error);
        if (!error.empty()) throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }
    ClientSafetyPolicy selection_policy;
    if (!GetPaymasterClientSafetyPolicy(*wallet, selection_policy)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED");
    }
    std::string fee_error;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const OfferCandidate& candidate) {
            std::string candidate_error;
            if (CheckClientServiceFee(selection_policy, candidate.payment, candidate.service_fee, candidate_error)) return false;
            fee_error = candidate_error;
            return true;
        }), candidates.end());
    if (candidates.empty() && !fee_error.empty()) throw ApplicationError(ErrorKind::WALLET, fee_error);
    if (privacy == PrivacyProfile::HIGH) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [](const OfferCandidate& candidate) { return !candidate.endpoint.IsTor(); }),
                         candidates.end());
    }
    if (preferred_provider && persisted_session.attempt_ids.empty()) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [&](const OfferCandidate& candidate) {
                return candidate.provider_id != *preferred_provider || candidate.terms.offer_id != *preferred_offer;
            }), candidates.end());
        if (candidates.empty()) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SELECTED_OFFER_UNAVAILABLE",
                have_persisted_session ? ErrorDisposition::RECONCILE : ErrorDisposition::RETRY_BEFORE_EXECUTION, !have_persisted_session);
        }
    }
    std::optional<OfferCandidate> selected;
    if (persisted_selection) {
        const auto candidate = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const OfferCandidate& entry) {
                                                return entry.provider_id == persisted_selection->first &&
                                                       entry.terms.offer_id == persisted_selection->second;
                                            });
        if (candidate != candidates.end())
            selected = *candidate;
        else if (active_attempt_id) {
            if (!store.AbandonClientAttemptForFallback(
                    request_id, *active_attempt_id, now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            persisted_selection.reset();
        }
    }
    if (!persisted_selection) {
        if (persisted_session.attempt_ids.size() >= static_cast<size_t>(maximum_attempts)) {
            if (!persisted_session.attempt_ids.empty()) {
                std::string abandon_error;
                if (!store.AbandonUnsignedClientSession(
                        request_id, now, abandon_error)) {
                    throw ApplicationError(ErrorKind::WALLET, abandon_error);
                }
            }
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_MAXIMUM_PROVIDER_ATTEMPTS_REACHED");
        }
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const OfferCandidate& candidate) {
                                            return attempted_providers.count(candidate.provider_id) != 0;
                                        }),
                         candidates.end());
        FastRandomContext rng;
        selected = SelectOfferCandidate(candidates, selection,
                                        effective_fee_cap, rng, error,
                                        subtract_fee);
    }
    if (!selected) {
        const std::string selection_error =
            subtract_fee && error == "PAYMASTER_NO_ELIGIBLE_OFFER"
                ? "PAYMASTER_NO_EXACT_GROSS_OFFER"
                : error;
        if (have_persisted_session) {
            std::string abandon_error;
            if (!store.AbandonUnsignedClientSession(
                    request_id, now, abandon_error)) {
                throw ApplicationError(ErrorKind::WALLET, abandon_error);
            }
        }
        throw ApplicationError(ErrorKind::WALLET, selection_error,
            have_persisted_session ? ErrorDisposition::RECONCILE : ErrorDisposition::RETRY_BEFORE_EXECUTION, !have_persisted_session);
    }

    DigiDollarWallet* dd_wallet = wallet->GetDDWallet();
    if (!dd_wallet) throw ApplicationError(ErrorKind::WALLET, "DigiDollar wallet not initialized");
    std::vector<COutPoint> selected_inputs;
    std::vector<CAmount> selected_amounts;
    CAmount selected_total{0};
    bool inputs_ok{false};
    if (persisted_inputs && !persisted_inputs->empty()) {
        // This request already owns hard reservations for these exact inputs.
        // Reusing the persisted binding is the only safe automatic resume;
        // ordinary coin selection must continue to hide every reservation.
        if (preset_inputs && *preset_inputs != *persisted_inputs) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PERSISTED_INPUT_CONFLICT");
        }
        selected_inputs = *persisted_inputs;
        inputs_ok = !selected_inputs.empty();
    } else if (send_all) {
        if (dd_wallet->GetSpendableDDBalance() != amount) {
            error = "PAYMASTER_SWEEP_BALANCE_CHANGED";
        } else if (preset_inputs) {
            inputs_ok = dd_wallet->SelectDDCoins(
                selected->user_total.value, *preset_inputs, selected_inputs,
                selected_total, &selected_amounts, &error);
            if (inputs_ok && selected_total != amount) {
                inputs_ok = false;
                error = "PAYMASTER_SWEEP_BALANCE_CHANGED";
            }
        } else {
            inputs_ok = dd_wallet->SelectAllSpendableDDCoins(
                amount, selected_inputs, selected_total, &selected_amounts,
                error);
        }
    } else if (preset_inputs) {
        inputs_ok = dd_wallet->SelectDDCoins(selected->user_total.value, *preset_inputs,
                                             selected_inputs, selected_total,
                                             &selected_amounts, &error);
    } else {
        inputs_ok = dd_wallet->SelectDDCoins(selected->user_total.value, selected_inputs,
                                             selected_total, &selected_amounts, &error);
    }
    if (!inputs_ok) {
        // Preserve the upstream selection reason and give GUI/CLI clients a
        // stable category. Session existence still requires a separate lookup.
        throw ApplicationError(ErrorKind::INSUFFICIENT_FUNDS,
                           "PAYMASTER_DD_INPUT_SELECTION_FAILED: " +
                               (error.empty() ? "Insufficient confirmed DigiDollar inputs" : error));
    }

    QuoteParameters quote;
    static_cast<ClientPaymentOptions&>(quote) = input;
    quote.provider_id = selected->provider_id;
    quote.offer_id = selected->terms.offer_id;
    quote.payment = selected->payment.value;
    quote.requested_amount = amount;
    quote.inputs = std::move(selected_inputs);
    PaymentResult quote_result = RequestPaymasterQuote(context, *wallet, quote);
    if (!quote_result.funding_model) {
        quote_result.funding_model = selected->terms.funding_model;
    }

    const auto copy_quote_fields = [&quote_result](PaymentResult& target) {
        FillMissingPaymentTerms(target, quote_result);
        if (!target.expires_at) target.expires_at = quote_result.expires_at;
        if (!target.quote_id) target.quote_id = quote_result.quote_id;
        if (!target.unsigned_txid) target.unsigned_txid = quote_result.unsigned_txid;
        if (!target.template_commitment) target.template_commitment = quote_result.template_commitment;
        if (!target.authorization_required) target.authorization_required = quote_result.authorization_required;
    };

    const auto& psbt = quote_result.psbt;
    if (!psbt) {
        if (supplied_authorization_commitment) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_AUTHORIZATION_NOT_READY");
        }
        return quote_result;
    }

    if (!quote_result.unsigned_txid) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_AUTHORIZATION_TEMPLATE_MISSING");
    }
    ProviderAttempt authorization_attempt;
    const uint256& unsigned_txid = *quote_result.unsigned_txid;
    if (!store.GetAttemptByUnsignedTxid(unsigned_txid, authorization_attempt)) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_AUTHORIZATION_ATTEMPT_MISSING");
    }
    uint256 commitment;
    if (!GetValidatedClientAuthorizationCommitment(
            authorization_attempt, GetTime(), commitment, error)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_INVALID" : error);
    }
    if (supplied_authorization_commitment &&
        *supplied_authorization_commitment != commitment) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
    }
    quote_result.authorization_required = !supplied_authorization_commitment.has_value();
    if (!quote_result.authorization_commitment) {
        quote_result.authorization_commitment = commitment;
    }
    // A high-level call can never silently turn a freshly received quote into
    // a USER signature. The caller must round-trip the exact manifest hash.
    if (!supplied_authorization_commitment) return quote_result;

    if (authorization_attempt.state == AttemptState::QUOTED) {
        if (!store.AcceptClientAuthorization(
                request_id, authorization_attempt.attempt_id, commitment,
                GetTime(), error) ||
            !store.GetAttempt(authorization_attempt.attempt_id,
                              authorization_attempt)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
    }
    if (wallet->IsLocked()) {
        PaymentSession session;
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND");
        }
        if (session.state == SessionState::AWAITING_USER_SIGNATURE &&
            !store.TransitionSession(request_id, SessionState::AWAITING_WALLET_UNLOCK,
                                     PendingPhase::NONE, {},
                                     std::max(GetTime(),
                                              authorization_attempt.updated_at),
                                     error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!store.GetSessionByRequestId(request_id, session)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND");
        }
        PaymentResult paused = CaptureSessionPayment(session);
        copy_quote_fields(paused);
        const bool authorization_accepted =
            authorization_attempt.accepted_client_manifest_id == commitment &&
            authorization_attempt.client_manifest_accepted_at > 0;
        paused.authorization_accepted = authorization_accepted;
        if (authorization_accepted) {
            paused.authorization_accepted_at = authorization_attempt.client_manifest_accepted_at;
        }
        return paused;
    }

    PaymentResult authorization = AuthorizePaymasterPSBT(context, *wallet, *psbt, supplied_authorization_commitment);
    copy_quote_fields(authorization);

    PaymentResult provider_result = ProcessPaymasterResult(context, *wallet, request_id);
    if (provider_result.processed.value_or(false)) {
        copy_quote_fields(provider_result);
        return provider_result;
    }
    return authorization;
}

} // namespace wallet::paymaster_app
