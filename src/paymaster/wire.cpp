// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Cheap, bounded validation of Paymaster messages at the network boundary.
 * Envelope checks intentionally precede chainstate and signature work so a
 * remote peer cannot force expensive processing with oversized, stale, or
 * structurally impossible messages.
 */

#include <paymaster/wire.h>

#include <hash.h>
#include <paymaster/directory.h>

#include <set>
#include <utility>

namespace DigiDollar::Paymaster {
namespace {

bool IsP2TR(const CScript& script)
{
    int version{-1};
    std::vector<unsigned char> program;
    return script.IsWitnessProgram(version, program) && version == 1 && program.size() == 32 &&
           XOnlyPubKey{program}.IsFullyValid();
}

bool ValidWindow(int64_t created_at, int64_t expires_at, int64_t now)
{
    return created_at > 0 && !TimeDeltaExceeds(created_at, now, 60) && expires_at > now &&
           expires_at > created_at &&
           !TimeDeltaExceeds(expires_at, created_at, MAX_DIRECT_MESSAGE_TTL_SECONDS);
}

bool ValidIntentShape(const PaymentIntent& intent, const uint256& expected_genesis, int64_t now)
{
    if (intent.version != PaymentIntent::CURRENT_VERSION || intent.genesis_hash != expected_genesis ||
        intent.provider_id.IsNull() || !IsCanonicalRequestId(intent.request_id) ||
        intent.session_id.IsNull() || intent.client_nonce.IsNull() || intent.offer_id.IsNull() ||
        intent.policy_hash.IsNull() || intent.canonical_request_hash.IsNull() ||
        intent.user_dd_inputs.empty() ||
        intent.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        intent.user_input_proofs.size() != intent.user_dd_inputs.size() ||
        (intent.funding_model != FundingModel::USER_PAID && intent.funding_model != FundingModel::SPONSORED) ||
        (intent.sponsorship_scope != SponsorshipScope::PUBLIC &&
         intent.sponsorship_scope != SponsorshipScope::RESTRICTED) ||
        (intent.requested_fee_mode != FeeMode::PAYMASTER &&
         intent.requested_fee_mode != FeeMode::AUTO) ||
        (intent.privacy_profile != PrivacyProfile::STANDARD &&
         intent.privacy_profile != PrivacyProfile::HIGH) ||
        (intent.selection_mode != SelectionMode::LOWEST_TOTAL_COST &&
         intent.selection_mode != SelectionMode::PRIVACY_WEIGHTED) ||
        !IsP2TR(intent.recipient_script) ||
        (!intent.user_dd_change_script.empty() && !IsP2TR(intent.user_dd_change_script)) ||
        intent.recipient_amount.value < 100 ||
        intent.recipient_amount.value > MAX_DD_OUTPUT_CENTS ||
        intent.expires_at <= now ||
        TimeDeltaExceeds(intent.expires_at, now, MAX_DIRECT_MESSAGE_TTL_SECONDS)) {
        return false;
    }
    std::string binding_error;
    if (!ValidateSponsorshipBinding(intent.funding_model, intent.sponsorship_scope,
                                    0, DDCents{0}, intent.sponsorship_authorization_hash,
                                    binding_error)) return false;
    std::set<COutPoint> inputs;
    for (size_t i = 0; i < intent.user_dd_inputs.size(); ++i) {
        if (intent.user_dd_inputs[i].IsNull() || !inputs.insert(intent.user_dd_inputs[i]).second ||
            intent.user_input_proofs[i].outpoint != intent.user_dd_inputs[i] ||
            intent.user_input_proofs[i].signature.size() != 64) return false;
    }
    return true;
}

bool ValidQuoteShape(const PaymasterQuote& quote, const uint256& expected_genesis, int64_t now)
{
    return quote.version == PaymasterQuote::CURRENT_VERSION && quote.genesis_hash == expected_genesis &&
           !quote.provider_id.IsNull() && !quote.quote_id.IsNull() && !quote.intent_hash.IsNull() &&
           !quote.offer_id.IsNull() && !quote.policy_hash.IsNull() && quote.fee_rate_bps <= MAX_RATE_BPS &&
           quote.fee_rate_bps % 10 == 0 && quote.service_fee.value >= 0 &&
           quote.service_fee.value <= MAX_DD_OUTPUT_CENTS && quote.network_fee.value > 0 &&
           !TimeDeltaExceeds(quote.created_at, now, 60) && quote.expires_at > now &&
           quote.expires_at > quote.created_at &&
           !TimeDeltaExceeds(quote.expires_at, quote.created_at,
                             MAX_DIRECT_MESSAGE_TTL_SECONDS) &&
           quote.retry_until >= quote.expires_at &&
           !TimeDeltaExceeds(quote.retry_until, quote.created_at, DEFAULT_RETRY_SECONDS) &&
           !quote.reserved_dgb_inputs.empty() &&
           quote.reserved_dgb_inputs.size() <= MAX_CAPACITY_DGB_INPUTS &&
           !quote.unsigned_txid.IsNull() && !quote.template_commitment.IsNull() &&
           quote.identity_signature.size() == 64;
}

} // namespace

uint256 GetCapacityControlHash(const PaymasterCapacityProof& proof,
                               const COutPoint& outpoint,
                               int64_t expires_at)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Capacity Control v1");
    hasher << proof.genesis_hash << proof.provider_id << proof.request_id
           << proof.session_id << proof.client_nonce << proof.snapshot_id
           << static_cast<uint8_t>(proof.funding_model)
           << static_cast<uint8_t>(proof.requires_carrier ? 1U : 0U)
           << outpoint << expires_at;
    return hasher.GetSHA256();
}

uint256 GetCapacityProofSignatureHash(const PaymasterCapacityProof& proof)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Capacity v1");
    hasher << proof.version << proof.genesis_hash << proof.provider_id
           << proof.request_id << proof.session_id << proof.client_nonce
           << static_cast<uint8_t>(proof.funding_model)
           << static_cast<uint8_t>(proof.requires_carrier ? 1U : 0U)
           << proof.snapshot_id << proof.created_at << proof.expires_at
           << static_cast<uint64_t>(proof.liquidity_slots.size());
    for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
        hasher << static_cast<uint8_t>(slot.carrier.has_value() ? 1U : 0U);
        if (slot.carrier) {
            hasher << slot.carrier->carrier.outpoint
                   << CTransaction{slot.carrier->carrier.creating_tx}.GetHash()
                   << slot.carrier->carrier.value
                   << slot.carrier->control_proof.reference_block
                   << slot.carrier->control_proof.expires_at
                   << slot.carrier->control_proof.signature;
        }
        hasher << static_cast<uint64_t>(slot.dgb_inputs.size());
        for (const CapacityDGBInput& dgb : slot.dgb_inputs) {
            hasher << dgb.input.outpoint << CTransaction{dgb.input.creating_tx}.GetHash()
                   << dgb.input.value << dgb.control_proof.reference_block
                   << dgb.control_proof.expires_at << dgb.control_proof.signature;
        }
    }
    return hasher.GetSHA256();
}

uint256 GetCapacityResourceCommitment(const PaymasterCapacityProof& proof)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Capacity Resources v1");
    hasher << proof.genesis_hash << proof.provider_id
           << static_cast<uint8_t>(proof.funding_model)
           << static_cast<uint8_t>(proof.requires_carrier ? 1U : 0U)
           << static_cast<uint64_t>(proof.liquidity_slots.size());
    for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
        hasher << static_cast<uint8_t>(slot.carrier.has_value() ? 1U : 0U);
        if (slot.carrier) {
            hasher << slot.carrier->carrier.outpoint
                   << CTransaction{slot.carrier->carrier.creating_tx}.GetHash()
                   << slot.carrier->carrier.value;
        }
        hasher << static_cast<uint64_t>(slot.dgb_inputs.size());
        for (const CapacityDGBInput& dgb : slot.dgb_inputs) {
            hasher << dgb.input.outpoint
                   << CTransaction{dgb.input.creating_tx}.GetHash()
                   << dgb.input.value;
        }
    }
    return hasher.GetSHA256();
}

bool ValidateCapacityRequestEnvelope(const PaymasterCapacityRequest& request,
                                     const uint256& expected_genesis,
                                     int64_t now,
                                     std::string& error)
{
    if (request.version != PROTOCOL_VERSION || request.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (request.provider_id.IsNull() || !IsCanonicalRequestId(request.request_id) ||
        request.session_id.IsNull() || request.client_nonce.IsNull() ||
        (request.funding_model != FundingModel::USER_PAID &&
         request.funding_model != FundingModel::SPONSORED) ||
        (request.requires_carrier &&
         request.funding_model != FundingModel::USER_PAID) ||
        request.requested_slots != 1) {
        error = "PAYMASTER_INVALID_CAPACITY_REQUEST";
        return false;
    }
    if (!ValidWindow(request.created_at, request.expires_at, now)) {
        error = "PAYMASTER_INVALID_CAPACITY_TIME";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateCapacityProofEnvelope(const PaymasterCapacityProof& proof,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error)
{
    if (proof.version != PROTOCOL_VERSION || proof.genesis_hash != expected_genesis ||
        proof.provider_id.IsNull() || !IsCanonicalRequestId(proof.request_id) ||
        proof.session_id.IsNull() || proof.client_nonce.IsNull() ||
        (proof.funding_model != FundingModel::USER_PAID &&
         proof.funding_model != FundingModel::SPONSORED) ||
        (proof.requires_carrier &&
         proof.funding_model != FundingModel::USER_PAID) ||
        proof.snapshot_id.IsNull()) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (!ValidWindow(proof.created_at, proof.expires_at, now) ||
        proof.liquidity_slots.size() != 1) {
        error = "PAYMASTER_INVALID_CAPACITY_LIMITS";
        return false;
    }
    const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
    if (slot.dgb_inputs.empty() || slot.dgb_inputs.size() > MAX_CAPACITY_DGB_INPUTS ||
        slot.carrier.has_value() != proof.requires_carrier ||
        proof.identity_signature.size() != 64) {
        error = "PAYMASTER_INVALID_CAPACITY_SLOT";
        return false;
    }
    std::set<COutPoint> outpoints;
    for (const CapacityDGBInput& dgb : slot.dgb_inputs) {
        if (dgb.input.outpoint.IsNull() || dgb.input.value.value <= 0 ||
            !outpoints.insert(dgb.input.outpoint).second || dgb.control_proof.reference_block.IsNull() ||
            dgb.control_proof.expires_at != proof.expires_at ||
            dgb.control_proof.signature.size() != 64) {
            error = "PAYMASTER_INVALID_CAPACITY_DGB_INPUT";
            return false;
        }
    }
    if (slot.carrier && (slot.carrier->carrier.outpoint.IsNull() ||
                         slot.carrier->carrier.value.value < 100 ||
                         slot.carrier->carrier.value.value > MAX_DD_OUTPUT_CENTS ||
                         !outpoints.insert(slot.carrier->carrier.outpoint).second ||
                         slot.carrier->control_proof.reference_block.IsNull() ||
                         slot.carrier->control_proof.expires_at != proof.expires_at ||
                         slot.carrier->control_proof.signature.size() != 64)) {
        error = "PAYMASTER_INVALID_CAPACITY_CARRIER";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateCapacityProofEnvelope(const PaymasterCapacityProof& proof,
                                   const PaymasterCapacityRequest& request,
                                   int64_t now,
                                   std::string& error)
{
    if (proof.provider_id != request.provider_id || proof.request_id != request.request_id ||
        proof.session_id != request.session_id ||
        proof.client_nonce != request.client_nonce ||
        proof.funding_model != request.funding_model ||
        proof.requires_carrier != request.requires_carrier ||
        proof.created_at < request.created_at || proof.expires_at > request.expires_at) {
        error = "PAYMASTER_CAPACITY_BINDING_MISMATCH";
        return false;
    }
    return ValidateCapacityProofEnvelope(proof, request.genesis_hash, now, error);
}

bool ValidateCapacityProof(const PaymasterCapacityProof& proof,
                           const PaymasterCapacityRequest& request,
                           const uint256& expected_genesis,
                           const XOnlyPubKey& provider_identity_key,
                           const CapacityChainstateCallbacks& chainstate,
                           int64_t now,
                           std::string& error)
{
    if (!ValidateCapacityRequestEnvelope(request, expected_genesis, now, error) ||
        !ValidateCapacityProofEnvelope(proof, request, now, error)) {
        return false;
    }
    if (!provider_identity_key.IsFullyValid() ||
        GetPaymasterId(provider_identity_key) != proof.provider_id) {
        error = "PAYMASTER_INVALID_CAPACITY_IDENTITY";
        return false;
    }
    if (!provider_identity_key.VerifySchnorr(GetCapacityProofSignatureHash(proof),
                                             proof.identity_signature)) {
        error = "PAYMASTER_INVALID_CAPACITY_IDENTITY_SIGNATURE";
        return false;
    }
    if (!chainstate.validate_reference_block || !chainstate.validate_dgb_input) {
        error = "PAYMASTER_CAPACITY_CHAINSTATE_VALIDATOR_MISSING";
        return false;
    }

    std::set<uint256> validated_reference_blocks;
    const auto validate_reference_block = [&](const uint256& reference_block) {
        if (!validated_reference_blocks.insert(reference_block).second) return true;
        std::string callback_error;
        if (!chainstate.validate_reference_block(reference_block, callback_error)) {
            error = callback_error.empty() ? "PAYMASTER_INVALID_CAPACITY_REFERENCE_BLOCK" : std::move(callback_error);
            return false;
        }
        return true;
    };

    const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
    for (const CapacityDGBInput& dgb : slot.dgb_inputs) {
        if (!validate_reference_block(dgb.control_proof.reference_block)) return false;
        XOnlyPubKey output_key;
        std::string callback_error;
        if (!chainstate.validate_dgb_input(dgb.input, output_key, callback_error) ||
            !output_key.IsFullyValid()) {
            error = callback_error.empty() ? "PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE" : std::move(callback_error);
            return false;
        }
        if (!output_key.VerifySchnorr(
                GetCapacityControlHash(proof, dgb.input.outpoint,
                                       dgb.control_proof.expires_at),
                dgb.control_proof.signature)) {
            error = "PAYMASTER_INVALID_CAPACITY_DGB_CONTROL_SIGNATURE";
            return false;
        }
    }

    if (slot.carrier) {
        if (!chainstate.validate_dd_carrier) {
            error = "PAYMASTER_CAPACITY_CHAINSTATE_VALIDATOR_MISSING";
            return false;
        }
        if (!validate_reference_block(slot.carrier->control_proof.reference_block)) return false;
        XOnlyPubKey output_key;
        std::string callback_error;
        if (!chainstate.validate_dd_carrier(slot.carrier->carrier, output_key,
                                            callback_error) ||
            !output_key.IsFullyValid()) {
            error = callback_error.empty() ? "PAYMASTER_INVALID_CAPACITY_CARRIER_CHAINSTATE" : std::move(callback_error);
            return false;
        }
        if (!output_key.VerifySchnorr(
                GetCapacityControlHash(proof, slot.carrier->carrier.outpoint,
                                       slot.carrier->control_proof.expires_at),
                slot.carrier->control_proof.signature)) {
            error = "PAYMASTER_INVALID_CAPACITY_CARRIER_CONTROL_SIGNATURE";
            return false;
        }
    }

    error.clear();
    return true;
}

bool ValidateQuoteRequestEnvelope(const PaymasterQuoteRequest& request,
                                  const uint256& expected_genesis,
                                  int64_t now,
                                  std::string& error)
{
    if (request.version != PROTOCOL_VERSION || !ValidIntentShape(request.intent, expected_genesis, now)) {
        error = "PAYMASTER_INVALID_QUOTE_REQUEST";
        return false;
    }
    const bool restricted = request.intent.sponsorship_scope == SponsorshipScope::RESTRICTED;
    if (restricted != request.restricted_descriptor.has_value() ||
        restricted != request.restricted_capability.has_value()) {
        error = "PAYMASTER_CAPABILITY_PRESENCE_MISMATCH";
        return false;
    }
    if (request.restricted_capability) {
        const RestrictedServiceDescriptor& descriptor = *request.restricted_descriptor;
        const SponsorshipCapability& capability = *request.restricted_capability;
        if (descriptor.version != RestrictedServiceDescriptor::CURRENT_VERSION ||
            descriptor.genesis_hash != expected_genesis ||
            descriptor.provider_id != request.intent.provider_id ||
            descriptor.offer_id != request.intent.offer_id ||
            descriptor.policy_hash != request.intent.policy_hash) {
            error = "PAYMASTER_DESCRIPTOR_BINDING_MISMATCH";
            return false;
        }
        if (descriptor.expires_at < request.intent.expires_at ||
            descriptor.provider_identity_signature.size() != 64) {
            error = "PAYMASTER_INVALID_DESCRIPTOR_ENVELOPE";
            return false;
        }
        if (capability.version != SponsorshipCapability::CURRENT_VERSION ||
            capability.genesis_hash != expected_genesis ||
            capability.provider_id != request.intent.provider_id ||
            capability.offer_id != request.intent.offer_id ||
            capability.policy_hash != request.intent.policy_hash ||
            capability.recipient_script != request.intent.recipient_script ||
            !(capability.amount == request.intent.recipient_amount)) {
            error = "PAYMASTER_CAPABILITY_BINDING_MISMATCH";
            return false;
        }
        if (capability.payment_request_nonce.IsNull() || capability.expires_at <= now ||
            capability.expires_at < request.intent.expires_at ||
            capability.sponsor_signature.size() != 64) {
            error = "PAYMASTER_INVALID_CAPABILITY_ENVELOPE";
            return false;
        }
        if (GetSponsorshipCapabilityHash(capability) !=
            request.intent.sponsorship_authorization_hash) {
            error = "PAYMASTER_CAPABILITY_HASH_MISMATCH";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ValidateRedactedQuoteRequestEnvelope(const PaymasterQuoteRequest& request,
                                          const uint256& expected_genesis,
                                          int64_t now,
                                          std::string& error)
{
    if (request.version != PROTOCOL_VERSION ||
        !ValidIntentShape(request.intent, expected_genesis, now)) {
        error = "PAYMASTER_INVALID_QUOTE_REQUEST";
        return false;
    }
    if (request.restricted_descriptor || request.restricted_capability) {
        error = "PAYMASTER_UNREDACTED_CAPABILITY";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateQuoteResponseEnvelope(const PaymasterQuoteResponse& response,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error)
{
    if (response.version != PROTOCOL_VERSION || !IsCanonicalRequestId(response.request_id) ||
        response.session_id.IsNull() || !ValidQuoteShape(response.quote, expected_genesis, now)) {
        error = "PAYMASTER_INVALID_QUOTE_RESPONSE";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateSubmitEnvelope(const PaymasterSubmit& submit,
                            const uint256& expected_genesis,
                            std::string& error)
{
    if (submit.version != PROTOCOL_VERSION || submit.genesis_hash != expected_genesis ||
        submit.provider_id.IsNull() || !IsCanonicalRequestId(submit.request_id) ||
        submit.session_id.IsNull() || submit.quote_id.IsNull() || submit.commit_key.IsNull() ||
        submit.template_commitment.IsNull() || submit.user_psbt.empty() ||
        submit.user_psbt.size() > MAX_DIRECT_PSBT_BYTES) {
        error = "PAYMASTER_INVALID_SUBMIT";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateResultMessageEnvelope(const PaymasterResultMessage& message,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error)
{
    if (message.version != PROTOCOL_VERSION || !IsCanonicalRequestId(message.request_id) ||
        message.session_id.IsNull() || message.result.genesis_hash != expected_genesis ||
        TimeDeltaExceeds(message.result.updated_at, now, 60) ||
        !ValidatePaymasterResultShape(message.result, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_RESULT_MESSAGE";
        return false;
    }
    error.clear();
    return true;
}

} // namespace DigiDollar::Paymaster
