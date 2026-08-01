// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Restricted-sponsorship capabilities and single-use authorization. */

#ifndef DIGIBYTE_PAYMASTER_SPONSORSHIP_H
#define DIGIBYTE_PAYMASTER_SPONSORSHIP_H

#include <netaddress.h>
#include <paymaster/directory.h>
#include <paymaster/types.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

static constexpr uint16_t SPONSOR_AUTHORIZATION_SCHEME_SCHNORR_V1{1};

/** A provider-approved, non-gossiped endpoint and restricted offer. */
struct RestrictedServiceDescriptor {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    CService p2p_endpoint;
    uint256 offer_id;
    uint256 policy_hash;
    XOnlyPubKey sponsor_authorization_key;
    std::string sponsor_display_name;
    uint16_t authorization_scheme{SPONSOR_AUTHORIZATION_SCHEME_SCHNORR_V1};
    int64_t expires_at{0};
    uint64_t admission_sequence{0};
    uint16_t min_confirmations{1};
    std::vector<AdmissionSlotProof> minimum_liquidity_proof;
    std::vector<unsigned char> provider_identity_signature;

    SERIALIZE_METHODS(RestrictedServiceDescriptor, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id,
                  WithParams(CNetAddr::V2, obj.p2p_endpoint), obj.offer_id,
                  obj.policy_hash, obj.sponsor_authorization_key,
                  obj.sponsor_display_name, obj.authorization_scheme,
                  obj.expires_at, obj.admission_sequence,
                  obj.min_confirmations, obj.minimum_liquidity_proof,
                  obj.provider_identity_signature);
    }
};

/** The payment-specific secret supplied only over the authenticated direct session. */
struct SponsorshipCapability {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    uint256 offer_id;
    uint256 policy_hash;
    CScript recipient_script;
    DDCents amount;
    uint256 payment_request_nonce;
    int64_t expires_at{0};
    std::vector<unsigned char> sponsor_signature;

    SERIALIZE_METHODS(SponsorshipCapability, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.offer_id,
                  obj.policy_hash, obj.recipient_script, obj.amount,
                  obj.payment_request_nonce, obj.expires_at,
                  obj.sponsor_signature);
    }
};

enum class SponsorshipAuthorizationState : uint8_t {
    RESERVED,
    CONSUMED,
};

/** Durable replay protection. The secret capability is deliberately absent. */
struct SponsorshipAuthorizationRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    SponsorshipAuthorizationState state{SponsorshipAuthorizationState::RESERVED};
    uint256 capability_hash;
    uint256 payment_binding_hash;
    uint256 reservation_id;
    int64_t reserved_at{0};
    int64_t expires_at{0};
    int64_t consumed_at{0};

    SERIALIZE_METHODS(SponsorshipAuthorizationRecord, obj)
    {
        READWRITE(obj.version,
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipAuthorizationState::CONSUMED)>>(obj.state),
                  obj.capability_hash, obj.payment_binding_hash,
                  obj.reservation_id, obj.reserved_at, obj.expires_at,
                  obj.consumed_at);
    }
};

uint256 GetRestrictedDescriptorSignatureHash(const RestrictedServiceDescriptor& descriptor);
uint256 GetSponsorshipCapabilitySignatureHash(const SponsorshipCapability& capability);
uint256 GetSponsorshipCapabilityHash(const SponsorshipCapability& capability);

bool ValidateRestrictedServiceDescriptor(const RestrictedServiceDescriptor& descriptor,
                                         const XOnlyPubKey& provider_identity_key,
                                         const uint256& expected_genesis,
                                         int64_t now,
                                         std::string& error,
                                         bool allow_local_endpoint = false);
bool ValidateSponsorshipCapability(const SponsorshipCapability& capability,
                                   const RestrictedServiceDescriptor& descriptor,
                                   const CScript& expected_recipient_script,
                                   DDCents expected_amount,
                                   const uint256& expected_payment_request_nonce,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error);
bool ValidateSponsorshipAuthorizationRecord(const SponsorshipAuthorizationRecord& record,
                                            std::string& error);

/** Validate the scope and authorization-hash rules shared by intents and quotes. */
bool ValidateSponsorshipBinding(FundingModel funding_model,
                                SponsorshipScope scope,
                                uint32_t fee_rate_bps,
                                DDCents service_fee,
                                const uint256& authorization_hash,
                                std::string& error);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_SPONSORSHIP_H
