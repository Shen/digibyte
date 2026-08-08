// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Client-side offer selection and payment-session state transitions. */

#ifndef DIGIBYTE_PAYMASTER_CLIENT_H
#define DIGIBYTE_PAYMASTER_CLIENT_H

#include <paymaster/directory.h>
#include <paymaster/reputation.h>
#include <paymaster/wire.h>
#include <random.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

struct OfferCandidate {
    PaymasterId provider_id;
    XOnlyPubKey identity_key;
    std::string display_name;
    CService endpoint;
    uint64_t announcement_sequence{0};
    int64_t announcement_expires_at{0};
    OfferTerms terms;
    DDCents payment;
    DDCents service_fee;
    DDCents user_total;
    ReliabilitySummary reliability;
    int64_t latency_ewma_ms{0};
};

/** Public, non-secret fields needed to construct the intent before wallet
 * keys are accessed. The random session and nonce are supplied by the caller
 * so retries can reproduce the same request exactly. */
struct PaymentIntentParameters {
    uint256 genesis_hash;
    std::string request_id;
    uint256 session_id;
    uint256 client_nonce;
    uint256 canonical_request_hash;
    FeeMode requested_fee_mode{FeeMode::PAYMASTER};
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    SelectionMode selection_mode{SelectionMode::LOWEST_TOTAL_COST};
    std::vector<COutPoint> user_dd_inputs;
    CScript recipient_script;
    CScript user_dd_change_script;
    int64_t expires_at{0};
    uint256 sponsorship_authorization_hash;
};

/** Return whether an endpoint is usable for the selected privacy profile.
 * Unknown profiles and invalid endpoints fail closed. */
bool IsEndpointAllowedForPrivacyProfile(
    const CService& endpoint,
    PrivacyProfile privacy_profile,
    bool allow_local_endpoint = false);

std::optional<PaymentIntent> BuildUnsignedPaymentIntent(
    const OfferCandidate& offer,
    const PaymentIntentParameters& parameters,
    int64_t now,
    std::string& error);

/** Attach externally produced input-control signatures and validate the exact
 * request envelope. Private keys never cross this API boundary. */
std::optional<PaymasterQuoteRequest> FinalizeQuoteRequest(
    PaymentIntent intent,
    const std::vector<XOnlyPubKey>& user_output_keys,
    const std::vector<std::vector<unsigned char>>& input_signatures,
    std::optional<RestrictedServiceDescriptor> restricted_descriptor,
    std::optional<SponsorshipCapability> restricted_capability,
    int64_t now,
    std::string& error);

/** Validate all response bindings before any quote is persisted or shown to
 * the user. */
bool ValidateQuoteResponseForRequest(
    const PaymasterQuoteResponse& response,
    const PaymasterQuoteRequest& request,
    const OfferTerms& offer,
    const XOnlyPubKey& provider_identity_key,
    DDCents maximum_service_fee,
    int64_t now,
    std::string& error);

std::vector<OfferCandidate> BuildOfferCandidates(
    const std::vector<Announcement>& announcements,
    DDCents payment,
    uint8_t allowed_funding_models,
    DDCents maximum_service_fee,
    const std::map<PaymasterId, PaymasterReliabilityRecord>& reliability,
    int64_t now,
    std::string& error);

/** Build candidates when amount is the maximum total DD wallet outflow.
 * Only offers with an exact cent-granular payment + rounded-fee solution are
 * returned. */
std::vector<OfferCandidate> BuildGrossOfferCandidates(
    const std::vector<Announcement>& announcements,
    DDCents gross_amount,
    uint8_t allowed_funding_models,
    DDCents maximum_service_fee,
    const std::map<PaymasterId, PaymasterReliabilityRecord>& reliability,
    int64_t now,
    std::string& error);

std::optional<OfferCandidate> SelectOfferCandidate(
    const std::vector<OfferCandidate>& sorted_candidates,
    SelectionMode mode,
    DDCents privacy_fee_tolerance,
    FastRandomContext& rng,
    std::string& error,
    bool fixed_user_total = false);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_CLIENT_H
