// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Canonical wire envelopes, message bounds, and transport validation. */

#ifndef DIGIBYTE_PAYMASTER_WIRE_H
#define DIGIBYTE_PAYMASTER_WIRE_H

#include <paymaster/protocol.h>
#include <paymaster/sponsorship.h>
#include <pubkey.h>
#include <serialize.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

static constexpr size_t MAX_DIRECT_MESSAGE_BYTES{1024 * 1024};
static constexpr size_t MAX_DIRECT_PSBT_BYTES{900 * 1024};
static constexpr size_t MAX_CAPACITY_DGB_INPUTS{64};
static constexpr int64_t MAX_DIRECT_MESSAGE_TTL_SECONDS{60};

struct CapacityControlProof {
    uint256 reference_block;
    int64_t expires_at{0};
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(CapacityControlProof, obj)
    {
        READWRITE(obj.reference_block, obj.expires_at, obj.signature);
    }
};

struct CapacityDGBInput {
    VerifiedDGBInput input;
    CapacityControlProof control_proof;

    SERIALIZE_METHODS(CapacityDGBInput, obj) { READWRITE(obj.input, obj.control_proof); }
};

struct CapacityDDCarrier {
    VerifiedDDCarrier carrier;
    CapacityControlProof control_proof;

    SERIALIZE_METHODS(CapacityDDCarrier, obj) { READWRITE(obj.carrier, obj.control_proof); }
};

struct PaymasterLiquiditySlot {
    std::optional<CapacityDDCarrier> carrier;
    std::vector<CapacityDGBInput> dgb_inputs;

    SERIALIZE_METHODS(PaymasterLiquiditySlot, obj)
    {
        READWRITE(Using<OptionalFieldFormatter<CapacityDDCarrier>>(obj.carrier), obj.dgb_inputs);
    }
};

struct PaymasterCapacityRequest {
    uint16_t version{PROTOCOL_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    std::string request_id;
    uint256 session_id;
    uint256 client_nonce;
    /** Null provider/session/request bindings make the default request
     * non-executable. Keep the pre-capacity persistent shape serializable;
     * unknown bytes received from disk or the wire are still rejected by the
     * enum formatter. */
    FundingModel funding_model{FundingModel::USER_PAID};
    bool requires_carrier{false};
    uint16_t requested_slots{1};
    int64_t created_at{0};
    int64_t expires_at{0};

    SERIALIZE_METHODS(PaymasterCapacityRequest, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.request_id,
                  obj.session_id, obj.client_nonce);
        if (obj.version >= 4) {
            READWRITE(
                Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                obj.requires_carrier);
        }
        READWRITE(obj.requested_slots,
                  obj.created_at, obj.expires_at);
    }
};

struct PaymasterCapacityProof {
    uint16_t version{PROTOCOL_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    std::string request_id;
    uint256 session_id;
    uint256 client_nonce;
    FundingModel funding_model{static_cast<FundingModel>(0xff)};
    bool requires_carrier{false};
    uint256 snapshot_id;
    int64_t created_at{0};
    int64_t expires_at{0};
    std::vector<PaymasterLiquiditySlot> liquidity_slots;
    std::vector<unsigned char> identity_signature;

    SERIALIZE_METHODS(PaymasterCapacityProof, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.request_id,
                  obj.session_id, obj.client_nonce);
        if (obj.version >= 4) {
            READWRITE(
                Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                obj.requires_carrier);
        }
        READWRITE(obj.snapshot_id,
                  obj.created_at, obj.expires_at, obj.liquidity_slots,
                  obj.identity_signature);
    }
};

/** Chain-dependent validation hooks for a capacity snapshot. The caller must
 * evaluate all callbacks against one coherent active-chain view.
 *
 * validate_reference_block must reject a block that is unknown, outside the
 * active chain, or older than the caller's freshness policy.
 *
 * validate_dgb_input must reject a chain- or mempool-spent or coinbase
 * outpoint, a creating transaction/output/value mismatch, an input below the
 * local confirmation policy, or a non-P2TR output. It returns the x-only key
 * from the actual chainstate output.
 *
 * validate_dd_carrier applies the same rules and must additionally derive the
 * DigiDollar amount from the creating transaction rather than trusting the
 * advertised value. It likewise returns the actual P2TR output key.
 */
struct CapacityChainstateCallbacks {
    std::function<bool(const uint256& reference_block, std::string& error)> validate_reference_block;
    std::function<bool(const VerifiedDGBInput& input, XOnlyPubKey& output_key,
                       std::string& error)>
        validate_dgb_input;
    std::function<bool(const VerifiedDDCarrier& carrier, XOnlyPubKey& output_key,
                       std::string& error)>
        validate_dd_carrier;
};

struct PaymasterQuoteRequest {
    uint16_t version{PROTOCOL_VERSION};
    PaymentIntent intent;
    std::optional<RestrictedServiceDescriptor> restricted_descriptor;
    std::optional<SponsorshipCapability> restricted_capability;

    SERIALIZE_METHODS(PaymasterQuoteRequest, obj)
    {
        READWRITE(obj.version, obj.intent,
                  Using<OptionalFieldFormatter<RestrictedServiceDescriptor>>(obj.restricted_descriptor),
                  Using<OptionalFieldFormatter<SponsorshipCapability>>(obj.restricted_capability));
    }
};

struct PaymasterQuoteResponse {
    uint16_t version{PROTOCOL_VERSION};
    std::string request_id;
    uint256 session_id;
    PaymasterQuote quote;

    SERIALIZE_METHODS(PaymasterQuoteResponse, obj)
    {
        READWRITE(obj.version, obj.request_id, obj.session_id, obj.quote);
    }
};

struct PaymasterSubmit {
    uint16_t version{PROTOCOL_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    std::string request_id;
    uint256 session_id;
    uint256 quote_id;
    uint256 commit_key;
    uint256 template_commitment;
    std::vector<unsigned char> user_psbt;

    SERIALIZE_METHODS(PaymasterSubmit, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.request_id,
                  obj.session_id, obj.quote_id, obj.commit_key,
                  obj.template_commitment, obj.user_psbt);
    }
};

struct PaymasterResultMessage {
    uint16_t version{PROTOCOL_VERSION};
    std::string request_id;
    uint256 session_id;
    PaymasterResult result;

    SERIALIZE_METHODS(PaymasterResultMessage, obj)
    {
        READWRITE(obj.version, obj.request_id, obj.session_id, obj.result);
    }
};

bool ValidateCapacityRequestEnvelope(const PaymasterCapacityRequest& request,
                                     const uint256& expected_genesis,
                                     int64_t now,
                                     std::string& error);
bool ValidateCapacityProofEnvelope(const PaymasterCapacityProof& proof,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error);
bool ValidateCapacityProofEnvelope(const PaymasterCapacityProof& proof,
                                   const PaymasterCapacityRequest& request,
                                   int64_t now,
                                   std::string& error);
/** Perform complete client-side capacity validation after the cheap network
 * envelope check. This verifies request/chain/nonce/TTL binding, the provider
 * identity signature, every BIP86 control proof, and chain-dependent input
 * facts through callbacks supplied by the node integration layer. */
bool ValidateCapacityProof(const PaymasterCapacityProof& proof,
                           const PaymasterCapacityRequest& request,
                           const uint256& expected_genesis,
                           const XOnlyPubKey& provider_identity_key,
                           const CapacityChainstateCallbacks& chainstate,
                           int64_t now,
                           std::string& error);
bool ValidateQuoteRequestEnvelope(const PaymasterQuoteRequest& request,
                                  const uint256& expected_genesis,
                                  int64_t now,
                                  std::string& error);
/** Validate the canonical secret-free form retained in a wallet. Restricted
 * descriptor and capability fields must have been removed; their binding is
 * retained by intent.sponsorship_authorization_hash. */
bool ValidateRedactedQuoteRequestEnvelope(const PaymasterQuoteRequest& request,
                                          const uint256& expected_genesis,
                                          int64_t now,
                                          std::string& error);
bool ValidateQuoteResponseEnvelope(const PaymasterQuoteResponse& response,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error);
bool ValidateSubmitEnvelope(const PaymasterSubmit& submit,
                            const uint256& expected_genesis,
                            std::string& error);
bool ValidateResultMessageEnvelope(const PaymasterResultMessage& message,
                                   const uint256& expected_genesis,
                                   int64_t now,
                                   std::string& error);

uint256 GetCapacityControlHash(const PaymasterCapacityProof& proof,
                               const COutPoint& outpoint,
                               int64_t expires_at);
uint256 GetCapacityProofSignatureHash(const PaymasterCapacityProof& proof);
/** Bind the actual provider/outpoint/value resources while deliberately
 * excluding snapshot_id and timestamps. This detects one provider promising
 * the same live slot to conflicting local sessions under different proof ids. */
uint256 GetCapacityResourceCommitment(const PaymasterCapacityProof& proof);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_WIRE_H
