// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Restricted-sponsorship descriptor and capability validation.
 * Capabilities bind one provider, client, recipient, amount, and expiry; they
 * supplement normal Paymaster validation and never replace it.
 */

#include <paymaster/sponsorship.h>

#include <hash.h>

#include <set>

namespace DigiDollar::Paymaster {
namespace {

void HashAdmissionProof(HashWriter& hasher, const AdmissionSlotProof& slot)
{
    hasher << slot.dgb_outpoint << slot.dgb_creating_tx.GetHash() << slot.dgb_value
           << slot.dgb_control_signature << slot.carrier_outpoint
           << slot.carrier_creating_tx.GetHash() << slot.carrier_value
           << slot.carrier_control_signature << slot.reference_block << slot.expires_at;
}

} // namespace

uint256 GetRestrictedDescriptorSignatureHash(const RestrictedServiceDescriptor& descriptor)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Restricted Descriptor v1");
    hasher << descriptor.version << descriptor.genesis_hash << descriptor.provider_id
           << WithParams(CNetAddr::V2, descriptor.p2p_endpoint)
           << descriptor.offer_id << descriptor.policy_hash
           << descriptor.sponsor_authorization_key << descriptor.sponsor_display_name
           << descriptor.authorization_scheme << descriptor.expires_at
           << descriptor.admission_sequence << descriptor.min_confirmations
           << static_cast<uint64_t>(descriptor.minimum_liquidity_proof.size());
    for (const AdmissionSlotProof& slot : descriptor.minimum_liquidity_proof) {
        HashAdmissionProof(hasher, slot);
    }
    return hasher.GetSHA256();
}

uint256 GetSponsorshipCapabilitySignatureHash(const SponsorshipCapability& capability)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Sponsorship Capability v1");
    hasher << capability.version << capability.genesis_hash << capability.provider_id
           << capability.offer_id << capability.policy_hash << capability.recipient_script
           << capability.amount << capability.payment_request_nonce << capability.expires_at;
    return hasher.GetSHA256();
}

uint256 GetSponsorshipCapabilityHash(const SponsorshipCapability& capability)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Sponsorship Capability Id v1");
    hasher << GetSponsorshipCapabilitySignatureHash(capability) << capability.sponsor_signature;
    return hasher.GetSHA256();
}

bool ValidateRestrictedServiceDescriptor(const RestrictedServiceDescriptor& descriptor,
                                         const XOnlyPubKey& provider_identity_key,
                                         const uint256& expected_genesis,
                                         int64_t now,
                                         std::string& error,
                                         bool allow_local_endpoint)
{
    if (descriptor.version != RestrictedServiceDescriptor::CURRENT_VERSION ||
        descriptor.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (!provider_identity_key.IsFullyValid() ||
        descriptor.provider_id != GetPaymasterId(provider_identity_key)) {
        error = "PAYMASTER_INVALID_IDENTITY";
        return false;
    }
    if (!descriptor.p2p_endpoint.IsValid() ||
        (!descriptor.p2p_endpoint.IsRoutable() &&
         !(allow_local_endpoint && descriptor.p2p_endpoint.IsLocal()))) {
        error = "PAYMASTER_UNROUTABLE_ENDPOINT";
        return false;
    }
    if (descriptor.offer_id.IsNull() || descriptor.policy_hash.IsNull() ||
        !descriptor.sponsor_authorization_key.IsFullyValid() ||
        !IsValidPaymasterDisplayName(descriptor.sponsor_display_name) ||
        descriptor.authorization_scheme != SPONSOR_AUTHORIZATION_SCHEME_SCHNORR_V1) {
        error = "PAYMASTER_INVALID_RESTRICTED_DESCRIPTOR";
        return false;
    }
    if (descriptor.expires_at <= now ||
        TimeDeltaExceeds(descriptor.expires_at, now, ANNOUNCEMENT_TTL_SECONDS)) {
        error = "PAYMASTER_INVALID_DESCRIPTOR_TIME";
        return false;
    }
    if (descriptor.admission_sequence == 0 || descriptor.min_confirmations == 0 ||
        descriptor.minimum_liquidity_proof.size() != REQUIRED_ADMISSION_SLOTS) {
        error = "PAYMASTER_INVALID_ADMISSION_SLOT";
        return false;
    }
    std::set<COutPoint> outpoints;
    for (const AdmissionSlotProof& slot : descriptor.minimum_liquidity_proof) {
        if (slot.dgb_outpoint.IsNull() || slot.dgb_value.value < MIN_ADMISSION_DGB_SATOSHIS ||
            slot.reference_block.IsNull() || slot.expires_at < descriptor.expires_at ||
            slot.dgb_control_signature.size() != 64 ||
            !outpoints.insert(slot.dgb_outpoint).second) {
            error = "PAYMASTER_INVALID_ADMISSION_SLOT";
            return false;
        }
    }
    if (descriptor.provider_identity_signature.size() != 64 ||
        !provider_identity_key.VerifySchnorr(GetRestrictedDescriptorSignatureHash(descriptor),
                                             descriptor.provider_identity_signature)) {
        error = "PAYMASTER_INVALID_DESCRIPTOR_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateSponsorshipCapability(const SponsorshipCapability& capability,
                                   const RestrictedServiceDescriptor& descriptor,
                                   const CScript& expected_recipient_script,
                                   DDCents expected_amount,
                                   const uint256& expected_payment_request_nonce,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error)
{
    if (capability.version != SponsorshipCapability::CURRENT_VERSION ||
        capability.genesis_hash != expected_genesis ||
        capability.genesis_hash != descriptor.genesis_hash) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (capability.provider_id != descriptor.provider_id ||
        capability.offer_id != descriptor.offer_id ||
        capability.policy_hash != descriptor.policy_hash ||
        capability.recipient_script != expected_recipient_script ||
        !(capability.amount == expected_amount) ||
        capability.payment_request_nonce.IsNull() ||
        capability.payment_request_nonce != expected_payment_request_nonce) {
        error = "PAYMASTER_CAPABILITY_BINDING_MISMATCH";
        return false;
    }
    if (capability.amount.value < 100 || capability.amount.value > MAX_DD_OUTPUT_CENTS ||
        capability.expires_at <= now || capability.expires_at > descriptor.expires_at) {
        error = "PAYMASTER_INVALID_CAPABILITY_TIME_OR_AMOUNT";
        return false;
    }
    if (capability.sponsor_signature.size() != 64 ||
        !descriptor.sponsor_authorization_key.VerifySchnorr(
            GetSponsorshipCapabilitySignatureHash(capability), capability.sponsor_signature)) {
        error = "PAYMASTER_INVALID_CAPABILITY_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateSponsorshipAuthorizationRecord(const SponsorshipAuthorizationRecord& record,
                                            std::string& error)
{
    if (record.version != SponsorshipAuthorizationRecord::CURRENT_VERSION ||
        record.capability_hash.IsNull() || record.payment_binding_hash.IsNull() ||
        record.reservation_id.IsNull() || record.reserved_at <= 0 ||
        record.expires_at <= record.reserved_at) {
        error = "PAYMASTER_INVALID_SPONSORSHIP_RECORD";
        return false;
    }
    if ((record.state != SponsorshipAuthorizationState::RESERVED &&
         record.state != SponsorshipAuthorizationState::CONSUMED) ||
        (record.state == SponsorshipAuthorizationState::RESERVED && record.consumed_at != 0) ||
        (record.state == SponsorshipAuthorizationState::CONSUMED &&
         (record.consumed_at < record.reserved_at || record.consumed_at > record.expires_at))) {
        error = "PAYMASTER_INVALID_SPONSORSHIP_STATE";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateSponsorshipBinding(FundingModel funding_model,
                                SponsorshipScope scope,
                                uint32_t fee_rate_bps,
                                DDCents service_fee,
                                const uint256& authorization_hash,
                                std::string& error)
{
    if ((funding_model != FundingModel::USER_PAID && funding_model != FundingModel::SPONSORED) ||
        (scope != SponsorshipScope::PUBLIC && scope != SponsorshipScope::RESTRICTED)) {
        error = "PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE";
        return false;
    }
    if (funding_model == FundingModel::USER_PAID) {
        if (scope != SponsorshipScope::PUBLIC || !authorization_hash.IsNull()) {
            error = "PAYMASTER_INVALID_USER_PAID_SCOPE";
            return false;
        }
    } else {
        if (fee_rate_bps != 0 || service_fee.value != 0) {
            error = "PAYMASTER_SPONSORED_FEE";
            return false;
        }
        if ((scope == SponsorshipScope::PUBLIC && !authorization_hash.IsNull()) ||
            (scope == SponsorshipScope::RESTRICTED && authorization_hash.IsNull())) {
            error = "PAYMASTER_INVALID_SPONSORSHIP_AUTHORIZATION";
            return false;
        }
    }
    error.clear();
    return true;
}

} // namespace DigiDollar::Paymaster
