// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Pure client-side offer selection and payment-intent construction.
 * Candidate ranking may choose a provider, but only the later wallet-local
 * authorization manifest can permit DD inputs to be signed.
 */

#include <paymaster/client.h>

#include <algorithm>
#include <limits>
#include <set>

namespace DigiDollar::Paymaster {
namespace {

bool IsP2TR(const CScript& script)
{
    int version{-1};
    std::vector<unsigned char> program;
    return script.IsWitnessProgram(version, program) && version == 1 && program.size() == 32 &&
           XOnlyPubKey{program}.IsFullyValid();
}

} // namespace

std::optional<PaymentIntent> BuildUnsignedPaymentIntent(
    const OfferCandidate& offer,
    const PaymentIntentParameters& parameters,
    int64_t now,
    std::string& error)
{
    const OfferTerms& terms = offer.terms;
    const auto expected_fee = ComputePaymasterFee(offer.payment, terms.fee_rate_bps);
    if (parameters.genesis_hash.IsNull() || offer.provider_id.IsNull() ||
        !IsCanonicalRequestId(parameters.request_id) || parameters.session_id.IsNull() ||
        parameters.client_nonce.IsNull() || parameters.canonical_request_hash.IsNull() ||
        (parameters.requested_fee_mode != FeeMode::PAYMASTER &&
         parameters.requested_fee_mode != FeeMode::AUTO) ||
        (parameters.privacy_profile != PrivacyProfile::STANDARD &&
         parameters.privacy_profile != PrivacyProfile::HIGH) ||
        (parameters.selection_mode != SelectionMode::LOWEST_TOTAL_COST &&
         parameters.selection_mode != SelectionMode::PRIVACY_WEIGHTED) ||
        parameters.user_dd_inputs.empty() ||
        parameters.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        !IsP2TR(parameters.recipient_script) ||
        (!parameters.user_dd_change_script.empty() && !IsP2TR(parameters.user_dd_change_script)) ||
        offer.payment.value < 100 || offer.payment.value > MAX_DD_OUTPUT_CENTS ||
        offer.payment.value < terms.min_payment.value || offer.payment.value > terms.max_payment.value ||
        !expected_fee || !(*expected_fee == offer.service_fee) ||
        terms.offer_id.IsNull() || terms.policy_hash.IsNull() || parameters.expires_at <= now ||
        TimeDeltaExceeds(parameters.expires_at, now, MAX_DIRECT_MESSAGE_TTL_SECONDS) ||
        parameters.expires_at > offer.announcement_expires_at) {
        error = "PAYMASTER_INVALID_INTENT_PARAMETERS";
        return std::nullopt;
    }
    std::set<COutPoint> unique_inputs;
    for (const COutPoint& input : parameters.user_dd_inputs) {
        if (input.IsNull() || !unique_inputs.insert(input).second) {
            error = "PAYMASTER_INVALID_INTENT_INPUTS";
            return std::nullopt;
        }
    }
    if (!ValidateSponsorshipBinding(terms.funding_model, terms.scope, terms.fee_rate_bps,
                                    offer.service_fee,
                                    parameters.sponsorship_authorization_hash, error)) {
        return std::nullopt;
    }

    PaymentIntent intent;
    intent.genesis_hash = parameters.genesis_hash;
    intent.provider_id = offer.provider_id;
    intent.request_id = parameters.request_id;
    intent.session_id = parameters.session_id;
    intent.client_nonce = parameters.client_nonce;
    intent.canonical_request_hash = parameters.canonical_request_hash;
    intent.requested_fee_mode = parameters.requested_fee_mode;
    intent.privacy_profile = parameters.privacy_profile;
    intent.selection_mode = parameters.selection_mode;
    intent.user_dd_inputs = parameters.user_dd_inputs;
    intent.recipient_script = parameters.recipient_script;
    intent.recipient_amount = offer.payment;
    intent.user_dd_change_script = parameters.user_dd_change_script;
    intent.offer_id = terms.offer_id;
    intent.funding_model = terms.funding_model;
    intent.sponsorship_scope = terms.scope;
    intent.policy_hash = terms.policy_hash;
    intent.sponsorship_authorization_hash = parameters.sponsorship_authorization_hash;
    intent.expires_at = parameters.expires_at;
    error.clear();
    return intent;
}

std::optional<PaymasterQuoteRequest> FinalizeQuoteRequest(
    PaymentIntent intent,
    const std::vector<XOnlyPubKey>& user_output_keys,
    const std::vector<std::vector<unsigned char>>& input_signatures,
    std::optional<RestrictedServiceDescriptor> restricted_descriptor,
    std::optional<SponsorshipCapability> restricted_capability,
    int64_t now,
    std::string& error)
{
    if (input_signatures.size() != intent.user_dd_inputs.size()) {
        error = "PAYMASTER_INVALID_INTENT_SIGNATURE_COUNT";
        return std::nullopt;
    }
    intent.user_input_proofs.clear();
    intent.user_input_proofs.reserve(input_signatures.size());
    for (size_t index = 0; index < input_signatures.size(); ++index) {
        intent.user_input_proofs.push_back(
            UserInputControlProof{intent.user_dd_inputs[index], input_signatures[index]});
    }
    if (!ValidatePaymentIntent(intent, intent.genesis_hash, now, user_output_keys, error)) {
        return std::nullopt;
    }
    PaymasterQuoteRequest request;
    request.intent = std::move(intent);
    request.restricted_descriptor = std::move(restricted_descriptor);
    request.restricted_capability = std::move(restricted_capability);
    if (!ValidateQuoteRequestEnvelope(request, request.intent.genesis_hash, now, error)) {
        return std::nullopt;
    }
    error.clear();
    return request;
}

bool ValidateQuoteResponseForRequest(
    const PaymasterQuoteResponse& response,
    const PaymasterQuoteRequest& request,
    const OfferTerms& offer,
    const XOnlyPubKey& provider_identity_key,
    DDCents maximum_service_fee,
    int64_t now,
    std::string& error)
{
    if (!ValidateQuoteResponseEnvelope(response, request.intent.genesis_hash, now, error)) return false;
    if (response.request_id != request.intent.request_id ||
        response.session_id != request.intent.session_id) {
        error = "PAYMASTER_QUOTE_RESPONSE_SESSION_MISMATCH";
        return false;
    }
    return ValidatePaymasterQuoteForClient(response.quote, request.intent, offer,
                                           provider_identity_key, maximum_service_fee,
                                           now, error);
}

std::vector<OfferCandidate> BuildOfferCandidates(
    const std::vector<Announcement>& announcements,
    DDCents payment,
    uint8_t allowed_funding_models,
    DDCents maximum_service_fee,
    const std::map<PaymasterId, PaymasterReliabilityRecord>& reliability,
    int64_t now,
    std::string& error)
{
    std::vector<OfferCandidate> result;
    if (payment.value < 100 || payment.value > MAX_DD_OUTPUT_CENTS ||
        maximum_service_fee.value < 0 || allowed_funding_models == 0 || now <= 0) {
        error = "PAYMASTER_INVALID_OFFER_REQUEST";
        return result;
    }
    for (const Announcement& announcement : announcements) {
        if (announcement.expires_at <= now || !announcement.identity_key.IsFullyValid()) continue;
        const PaymasterId provider_id = GetPaymasterId(announcement.identity_key);
        ReliabilitySummary summary;
        int64_t latency_ewma_ms{0};
        const auto reputation = reliability.find(provider_id);
        if (reputation != reliability.end()) {
            if (!ValidateReliabilityRecord(reputation->second) || reputation->second.cooldown_until > now) continue;
            summary = SummarizeReliability(reputation->second, now);
            latency_ewma_ms = reputation->second.latency_ewma_ms;
        }
        for (const OfferTerms& terms : announcement.offers) {
            const uint8_t model_bit = uint8_t{1} << static_cast<uint8_t>(terms.funding_model);
            if ((allowed_funding_models & model_bit) == 0 || terms.scope != SponsorshipScope::PUBLIC ||
                payment.value < terms.min_payment.value || payment.value > terms.max_payment.value) continue;
            const auto fee = ComputePaymasterFee(payment, terms.fee_rate_bps);
            if (!fee || fee->value > maximum_service_fee.value ||
                fee->value > std::numeric_limits<int64_t>::max() - payment.value) continue;
            if (terms.funding_model == FundingModel::SPONSORED &&
                (terms.fee_rate_bps != 0 || fee->value != 0)) continue;
            result.push_back(OfferCandidate{
                provider_id, announcement.identity_key, announcement.display_name, announcement.endpoint,
                announcement.sequence, announcement.expires_at, terms, payment, *fee,
                DDCents{payment.value + fee->value}, summary, latency_ewma_ms});
        }
    }
    std::sort(result.begin(), result.end(), [](const OfferCandidate& lhs, const OfferCandidate& rhs) {
        if (lhs.user_total.value != rhs.user_total.value) return lhs.user_total.value < rhs.user_total.value;
        if (lhs.reliability.sufficient_data && rhs.reliability.sufficient_data &&
            lhs.reliability.success_rate_basis_points != rhs.reliability.success_rate_basis_points) {
            return lhs.reliability.success_rate_basis_points > rhs.reliability.success_rate_basis_points;
        }
        if (lhs.latency_ewma_ms > 0 && rhs.latency_ewma_ms > 0 &&
            lhs.latency_ewma_ms != rhs.latency_ewma_ms) return lhs.latency_ewma_ms < rhs.latency_ewma_ms;
        if (lhs.provider_id != rhs.provider_id) return lhs.provider_id < rhs.provider_id;
        return lhs.terms.offer_id < rhs.terms.offer_id;
    });
    error.clear();
    return result;
}

std::vector<OfferCandidate> BuildGrossOfferCandidates(
    const std::vector<Announcement>& announcements,
    DDCents gross_amount,
    uint8_t allowed_funding_models,
    DDCents maximum_service_fee,
    const std::map<PaymasterId, PaymasterReliabilityRecord>& reliability,
    int64_t now,
    std::string& error)
{
    std::vector<OfferCandidate> result;
    if (gross_amount.value < 100 || gross_amount.value > MAX_DD_OUTPUT_CENTS ||
        maximum_service_fee.value < 0 || allowed_funding_models == 0 || now <= 0) {
        error = "PAYMASTER_INVALID_OFFER_REQUEST";
        return result;
    }
    for (const Announcement& announcement : announcements) {
        if (announcement.expires_at <= now || !announcement.identity_key.IsFullyValid()) continue;
        const PaymasterId provider_id = GetPaymasterId(announcement.identity_key);
        ReliabilitySummary summary;
        int64_t latency_ewma_ms{0};
        const auto reputation = reliability.find(provider_id);
        if (reputation != reliability.end()) {
            if (!ValidateReliabilityRecord(reputation->second) || reputation->second.cooldown_until > now) continue;
            summary = SummarizeReliability(reputation->second, now);
            latency_ewma_ms = reputation->second.latency_ewma_ms;
        }
        for (const OfferTerms& terms : announcement.offers) {
            const uint8_t model_bit = uint8_t{1} << static_cast<uint8_t>(terms.funding_model);
            if ((allowed_funding_models & model_bit) == 0 || terms.scope != SponsorshipScope::PUBLIC) continue;
            const auto payment = ComputePaymasterPaymentFromGross(gross_amount, terms.fee_rate_bps);
            if (!payment || payment->value < 100 ||
                payment->value < terms.min_payment.value || payment->value > terms.max_payment.value) continue;
            const auto fee = ComputePaymasterFee(*payment, terms.fee_rate_bps);
            if (!fee || fee->value > maximum_service_fee.value ||
                payment->value + fee->value != gross_amount.value) continue;
            if (terms.funding_model == FundingModel::SPONSORED &&
                (terms.fee_rate_bps != 0 || fee->value != 0)) continue;
            result.push_back(OfferCandidate{
                provider_id, announcement.identity_key, announcement.display_name, announcement.endpoint,
                announcement.sequence, announcement.expires_at, terms, *payment, *fee,
                gross_amount, summary, latency_ewma_ms});
        }
    }
    std::sort(result.begin(), result.end(), [](const OfferCandidate& lhs, const OfferCandidate& rhs) {
        if (lhs.service_fee.value != rhs.service_fee.value) return lhs.service_fee.value < rhs.service_fee.value;
        if (lhs.payment.value != rhs.payment.value) return lhs.payment.value > rhs.payment.value;
        if (lhs.reliability.sufficient_data && rhs.reliability.sufficient_data &&
            lhs.reliability.success_rate_basis_points != rhs.reliability.success_rate_basis_points) {
            return lhs.reliability.success_rate_basis_points > rhs.reliability.success_rate_basis_points;
        }
        if (lhs.latency_ewma_ms > 0 && rhs.latency_ewma_ms > 0 &&
            lhs.latency_ewma_ms != rhs.latency_ewma_ms) return lhs.latency_ewma_ms < rhs.latency_ewma_ms;
        if (lhs.provider_id != rhs.provider_id) return lhs.provider_id < rhs.provider_id;
        return lhs.terms.offer_id < rhs.terms.offer_id;
    });
    error.clear();
    return result;
}

std::optional<OfferCandidate> SelectOfferCandidate(
    const std::vector<OfferCandidate>& sorted_candidates,
    SelectionMode mode,
    DDCents privacy_fee_tolerance,
    FastRandomContext& rng,
    std::string& error,
    bool fixed_user_total)
{
    if (privacy_fee_tolerance.value < 0 ||
        static_cast<uint8_t>(mode) > static_cast<uint8_t>(SelectionMode::PRIVACY_WEIGHTED)) {
        error = "PAYMASTER_INVALID_SELECTION_POLICY";
        return std::nullopt;
    }
    if (sorted_candidates.empty()) {
        error = "PAYMASTER_NO_ELIGIBLE_OFFER";
        return std::nullopt;
    }
    if (mode == SelectionMode::LOWEST_TOTAL_COST || privacy_fee_tolerance.value == 0) {
        error.clear();
        return sorted_candidates.front();
    }
    const int64_t lowest = fixed_user_total
                               ? sorted_candidates.front().service_fee.value
                               : sorted_candidates.front().user_total.value;
    const int64_t ceiling = privacy_fee_tolerance.value > std::numeric_limits<int64_t>::max() - lowest
                                ? std::numeric_limits<int64_t>::max()
                                : lowest + privacy_fee_tolerance.value;
    size_t eligible{0};
    while (eligible < sorted_candidates.size()) {
        const int64_t cost = fixed_user_total
                                 ? sorted_candidates[eligible].service_fee.value
                                 : sorted_candidates[eligible].user_total.value;
        if (cost > ceiling) break;
        ++eligible;
    }
    error.clear();
    return sorted_candidates[static_cast<size_t>(rng.randrange(eligible))];
}

} // namespace DigiDollar::Paymaster
