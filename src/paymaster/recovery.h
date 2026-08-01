// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Exact retry and wallet-owned-output recovery authorization. */

#ifndef DIGIBYTE_PAYMASTER_RECOVERY_H
#define DIGIBYTE_PAYMASTER_RECOVERY_H

#include <paymaster/psbt.h>
#include <paymaster/reservation.h>
#include <paymaster/wire.h>

#include <consensus/amount.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class CChainParams;
class CCoinsViewCache;

namespace DigiDollar::Paymaster {

/** One DD output that returns value from the original user inputs to a fresh,
 * wallet-owned destination. The wallet integration must independently derive
 * and reserve the matching script before calling this module. */
struct AlternativeRecoveryReturn {
    CScript script_pub_key;
    DDCents amount;

    SERIALIZE_METHODS(AlternativeRecoveryReturn, obj)
    {
        READWRITE(obj.script_pub_key, obj.amount);
    }
};

/** Immutable authority for one alternative-provider cancel-to-self variant.
 *
 * The manifest deliberately has no client DGB input or arbitrary destination.
 * Provider scripts and all wallet-return scripts are exact, and the original
 * commit/template binding prevents a recovery artifact from being detached
 * from the ambiguous authorization it is intended to conflict with. */
struct AlternativeRecoveryManifest {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    uint256 manifest_id;
    std::string request_id;
    uint256 session_id;
    PaymasterId original_provider_id;
    PaymasterId recovery_provider_id;
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    uint256 offer_id;
    uint256 policy_hash;
    uint256 original_commit_key;
    uint256 original_template_commitment;
    uint256 capacity_snapshot_id;
    uint256 capacity_resource_commitment;
    std::vector<COutPoint> user_dd_inputs;
    std::vector<AlternativeRecoveryReturn> wallet_returns;
    std::vector<COutPoint> recovery_provider_carrier_inputs;
    std::vector<COutPoint> recovery_provider_dgb_inputs;
    std::vector<CScript> recovery_provider_dd_scripts;
    std::vector<CScript> recovery_provider_dgb_change_scripts;
    DDCents maximum_service_fee;
    DDCents service_fee;
    DGBSatoshis network_fee;
    int64_t expires_at{0};
    uint256 unsigned_txid;
    uint256 template_commitment;

    SERIALIZE_METHODS(AlternativeRecoveryManifest, obj)
    {
        READWRITE(obj.version, obj.manifest_id, obj.request_id, obj.session_id,
                  obj.original_provider_id, obj.recovery_provider_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                  obj.offer_id, obj.policy_hash,
                  obj.original_commit_key, obj.original_template_commitment,
                  obj.capacity_snapshot_id, obj.capacity_resource_commitment,
                  obj.user_dd_inputs, obj.wallet_returns,
                  obj.recovery_provider_carrier_inputs,
                  obj.recovery_provider_dgb_inputs,
                  obj.recovery_provider_dd_scripts,
                  obj.recovery_provider_dgb_change_scripts,
                  obj.maximum_service_fee, obj.service_fee, obj.network_fee,
                  obj.expires_at, obj.unsigned_txid,
                  obj.template_commitment);
    }
};

/** Fully explicit inputs to the pure alternative-recovery builder.
 *
 * wallet_verified_fresh_return_scripts is intentionally separate from
 * wallet_returns. The wallet layer populates it only after proving ownership
 * and freshness, while this core validator requires an exact ordered match.
 * The provider DD and DGB change scripts must likewise come from the
 * authenticated recovery-provider quote and be checked for provider ownership
 * by that provider before it signs. */
struct AlternativeRecoveryParameters {
    uint256 genesis_hash;
    std::string request_id;
    uint256 session_id;
    PaymasterId original_provider_id;
    PaymasterId recovery_provider_id;
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    uint256 offer_id;
    uint256 policy_hash;
    uint256 original_commit_key;
    uint256 original_template_commitment;
    PaymasterCapacityRequest capacity_request;
    ValidatedCapacitySnapshot capacity_snapshot;
    XOnlyPubKey recovery_provider_identity_key;
    std::vector<COutPoint> persisted_user_dd_inputs;
    std::vector<CollaborativeInput> user_dd_inputs;
    std::vector<AlternativeRecoveryReturn> wallet_returns;
    std::vector<CScript> wallet_verified_fresh_return_scripts;
    std::optional<CScript> recovery_provider_dd_script;
    std::optional<CScript> recovery_provider_dgb_change_script;
    DDCents maximum_service_fee;
    DDCents service_fee;
    DGBSatoshis network_fee;
    CAmount fee_rate{0};
    int64_t expires_at{0};
};

struct AlternativeRecoveryTemplate {
    AlternativeRecoveryManifest manifest;
    CollaborativePSBTTemplate trusted_template;
};

/** Wallet-local authority presented to the user before any USER input is
 * signed. This is deliberately separate from the provider-authenticated
 * AlternativeRecoveryManifest: accepting a valid provider template is a
 * local spending decision, not an implication of validating that template.
 *
 * The commitment is stable across restart and binds every economically or
 * operationally relevant recovery choice. The acceptance timestamp and the
 * accepted commitment are stored separately on AlternativeRecoveryRecord so
 * merely receiving this manifest can never authorize a signature. */
struct RecoveryAuthorizationManifest {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 authorization_commitment;
    std::string request_id;
    uint256 session_id;
    uint256 recovery_id;
    PaymasterId original_provider_id;
    PaymasterId recovery_provider_id;
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    uint256 offer_id;
    uint256 policy_hash;
    uint256 original_commit_key;
    uint256 original_template_commitment;
    uint256 capacity_snapshot_id;
    uint256 capacity_resource_commitment;
    uint256 recovery_request_hash;
    uint256 recovery_commit_key;
    uint256 recovery_manifest_id;
    uint256 recovery_template_commitment;
    std::vector<COutPoint> user_dd_inputs;
    std::vector<AlternativeRecoveryReturn> wallet_returns;
    DDCents maximum_service_fee;
    DDCents service_fee;
    DGBSatoshis network_fee;
    int64_t expires_at{0};

    SERIALIZE_METHODS(RecoveryAuthorizationManifest, obj)
    {
        READWRITE(obj.version, obj.authorization_commitment,
                  obj.request_id, obj.session_id, obj.recovery_id,
                  obj.original_provider_id, obj.recovery_provider_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                  obj.offer_id, obj.policy_hash, obj.original_commit_key,
                  obj.original_template_commitment,
                  obj.capacity_snapshot_id,
                  obj.capacity_resource_commitment,
                  obj.recovery_request_hash, obj.recovery_commit_key,
                  obj.recovery_manifest_id,
                  obj.recovery_template_commitment, obj.user_dd_inputs,
                  obj.wallet_returns, obj.maximum_service_fee,
                  obj.service_fee, obj.network_fee, obj.expires_at);
    }
};

/** Proof that the owner of one USER_DD prevout authorized this exact recovery
 * request. The domain-separated hash includes every return script, both
 * provider identities, the original commit/template and the capacity proof. */
struct AlternativeRecoveryInputProof {
    COutPoint outpoint;
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(AlternativeRecoveryInputProof, obj)
    {
        READWRITE(obj.outpoint, obj.signature);
    }
};

/** First payment-revealing recovery message. It may be sent only after the
 * client has validated the named capacity snapshot. */
struct AlternativeRecoveryRequest {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    std::string request_id;
    uint256 session_id;
    PaymasterId original_provider_id;
    PaymasterId recovery_provider_id;
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    uint256 offer_id;
    uint256 policy_hash;
    uint256 original_commit_key;
    uint256 original_template_commitment;
    uint256 client_nonce;
    PaymasterCapacityRequest capacity_request;
    uint256 capacity_snapshot_id;
    uint256 capacity_resource_commitment;
    std::vector<COutPoint> user_dd_inputs;
    std::vector<AlternativeRecoveryReturn> wallet_returns;
    DDCents maximum_service_fee;
    DDCents service_fee;
    int64_t created_at{0};
    int64_t expires_at{0};
    std::vector<AlternativeRecoveryInputProof> user_input_proofs;

    SERIALIZE_METHODS(AlternativeRecoveryRequest, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.request_id,
                  obj.session_id, obj.original_provider_id,
                  obj.recovery_provider_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                  obj.offer_id, obj.policy_hash,
                  obj.original_commit_key,
                  obj.original_template_commitment, obj.client_nonce,
                  obj.capacity_request,
                  obj.capacity_snapshot_id,
                  obj.capacity_resource_commitment, obj.user_dd_inputs,
                  obj.wallet_returns, obj.maximum_service_fee,
                  obj.service_fee, obj.created_at, obj.expires_at,
                  obj.user_input_proofs);
    }
};

/** Provider-authenticated recovery template. The client never signs the PSBT
 * carried here until it has independently rebuilt both manifest and PSBT. */
struct AlternativeRecoveryResponse {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    std::string request_id;
    uint256 session_id;
    uint256 recovery_id;
    PaymasterId recovery_provider_id;
    uint256 recovery_request_hash;
    uint256 recovery_commit_key;
    AlternativeRecoveryManifest manifest;
    std::vector<unsigned char> unsigned_psbt;
    int64_t created_at{0};
    int64_t expires_at{0};
    std::vector<unsigned char> identity_signature;

    SERIALIZE_METHODS(AlternativeRecoveryResponse, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.request_id,
                  obj.session_id, obj.recovery_id,
                  obj.recovery_provider_id,
                  obj.recovery_request_hash, obj.recovery_commit_key,
                  obj.manifest, obj.unsigned_psbt, obj.created_at,
                  obj.expires_at, obj.identity_signature);
    }
};

/** Exact user-authorized recovery PSBT returned to the recovery provider. */
struct AlternativeRecoverySubmit {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    std::string request_id;
    uint256 session_id;
    uint256 recovery_id;
    PaymasterId recovery_provider_id;
    uint256 recovery_request_hash;
    uint256 recovery_commit_key;
    uint256 template_commitment;
    std::vector<unsigned char> user_psbt;

    SERIALIZE_METHODS(AlternativeRecoverySubmit, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.request_id,
                  obj.session_id, obj.recovery_id,
                  obj.recovery_provider_id,
                  obj.recovery_request_hash, obj.recovery_commit_key,
                  obj.template_commitment, obj.user_psbt);
    }
};

/** Provider-signed final recovery result. PaymasterResult already supplies a
 * monotonic sequence, commit binding and identity signature. */
struct AlternativeRecoveryResultMessage {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    std::string request_id;
    uint256 session_id;
    uint256 recovery_id;
    uint256 recovery_request_hash;
    PaymasterResult result;

    SERIALIZE_METHODS(AlternativeRecoveryResultMessage, obj)
    {
        READWRITE(obj.version, obj.request_id, obj.session_id,
                  obj.recovery_id,
                  obj.recovery_request_hash, obj.result);
    }
};

enum class AlternativeRecoveryPhase : uint8_t {
    CAPACITY_PENDING,
    REQUEST_READY,
    RESPONSE_VALIDATED,
    USER_SIGNED,
    FINAL_COMMITTED,
};

/** Durable client/provider state for one alternative recovery. Raw protocol
 * messages are retained so restart retries are byte-identical. Client records
 * are indexed by the original request id; provider records are addressed only
 * by recovery_request_hash to avoid treating an untrusted UUID as authority. */
struct AlternativeRecoveryRecord {
    static constexpr uint16_t CURRENT_VERSION{4};
    static constexpr uint16_t LEGACY_VERSION{2};

    static constexpr bool IsSupportedVersion(uint16_t version)
    {
        return version == CURRENT_VERSION || version == 3 ||
               version == LEGACY_VERSION;
    }

    uint16_t version{CURRENT_VERSION};
    bool provider_side{false};
    std::string request_id;
    uint256 session_id;
    uint256 recovery_id;
    uint256 recovery_request_hash;
    PaymasterId original_provider_id;
    PaymasterId recovery_provider_id;
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    /** Persist the locally selected recovery offer before any payment detail is
     * disclosed. These fields remain immutable through every later phase. */
    uint256 offer_id;
    uint256 policy_hash;
    XOnlyPubKey recovery_provider_identity_key;
    std::string recovery_provider_endpoint;
    uint256 original_commit_key;
    uint256 original_template_commitment;
    /** The locally selected economic ceiling and quoted fee are persisted
     * before the capacity request is sent. This makes restart retries
     * independent of directory-cache eviction without disclosing user
     * outpoints before the capacity proof is validated. */
    DDCents selected_maximum_service_fee;
    DDCents selected_service_fee;
    uint256 client_nonce;
    PaymasterCapacityRequest capacity_request;
    ValidatedCapacitySnapshot capacity_snapshot;
    AlternativeRecoveryRequest recovery_request;
    AlternativeRecoveryResponse recovery_response;
    /** A validated response creates this manifest, but only an exact explicit
     * accepted_recovery_authorization_commitment permits USER_SIGNED. */
    RecoveryAuthorizationManifest recovery_authorization;
    uint256 accepted_recovery_authorization_commitment;
    int64_t recovery_authorization_accepted_at{0};
    AlternativeRecoveryPhase phase{AlternativeRecoveryPhase::CAPACITY_PENDING};
    /** Terminal local expiry is permitted only before a provider signature
     * exists. The record remains as a replay barrier. */
    bool expired{false};
    std::vector<unsigned char> user_signed_psbt;
    std::vector<unsigned char> final_transaction;
    uint256 expected_wtxid;
    PaymasterResult signed_result;
    int64_t created_at{0};
    int64_t updated_at{0};
    /** Provider-wallet-local authority for consuming the recovery fee budget.
     * These fields are never supplied by the peer. They are committed with the
     * provider's recovery quote and remain immutable through every later phase.
     * A v2 record has no such binding and may only drain work for which a user
     * authorization or exact final artifact was already durable. */
    uint256 provider_safety_policy_hash;
    uint256 provider_budget_reservation_id;
    uint256 provider_netgroup_bucket;
    DGBSatoshis provider_maximum_network_fee;
    /** First canonical provider-signed Capacity claim observed for this exact
     * client-side recovery handshake. It is evidence-only and never acts as a
     * validated snapshot or recovery authorization. */
    std::vector<unsigned char> capacity_proof_claim_candidate;

    SERIALIZE_METHODS(AlternativeRecoveryRecord, obj)
    {
        READWRITE(obj.version, obj.provider_side, obj.request_id,
                  obj.session_id, obj.recovery_id,
                  obj.recovery_request_hash,
                  obj.original_provider_id, obj.recovery_provider_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                  obj.offer_id, obj.policy_hash,
                  obj.recovery_provider_identity_key,
                  obj.recovery_provider_endpoint, obj.original_commit_key,
                  obj.original_template_commitment,
                  obj.selected_maximum_service_fee,
                  obj.selected_service_fee, obj.client_nonce,
                  obj.capacity_request, obj.capacity_snapshot,
                  obj.recovery_request, obj.recovery_response,
                  obj.recovery_authorization,
                  obj.accepted_recovery_authorization_commitment,
                  obj.recovery_authorization_accepted_at,
                  Using<EnumByteFormatter<static_cast<uint8_t>(AlternativeRecoveryPhase::FINAL_COMMITTED)>>(obj.phase),
                  obj.expired,
                  obj.user_signed_psbt, obj.final_transaction,
                  obj.expected_wtxid, obj.signed_result,
                  obj.created_at, obj.updated_at);
        if (obj.version >= 3) {
            READWRITE(obj.provider_safety_policy_hash,
                      obj.provider_budget_reservation_id,
                      obj.provider_netgroup_bucket,
                      obj.provider_maximum_network_fee);
        }
        if (obj.version >= 4) {
            READWRITE(obj.capacity_proof_claim_candidate);
        }
    }
};

uint256 GetAlternativeRecoveryRequestHash(
    const AlternativeRecoveryRequest& request);
/** Stable local identity allocated before payment details are disclosed. */
uint256 GetAlternativeRecoveryId(
    const std::string& request_id,
    const uint256& session_id,
    const PaymasterId& recovery_provider_id,
    const uint256& client_nonce);
uint256 GetAlternativeRecoveryInputControlHash(
    const AlternativeRecoveryRequest& request,
    const COutPoint& outpoint);
uint256 GetAlternativeRecoveryResponseSignatureHash(
    const AlternativeRecoveryResponse& response);
uint256 GetAlternativeRecoveryCommitKey(
    const AlternativeRecoveryResponse& response);

bool ValidateAlternativeRecoveryRequestEnvelope(
    const AlternativeRecoveryRequest& request,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error);
bool ValidateAlternativeRecoveryRequestInputs(
    const AlternativeRecoveryRequest& request,
    const std::vector<XOnlyPubKey>& user_output_keys,
    std::string& error);
bool ValidateAlternativeRecoveryResponseEnvelope(
    const AlternativeRecoveryResponse& response,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error);
bool ValidateAlternativeRecoveryResponse(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    int64_t now,
    std::string& error);
/** Validate the provider-authenticated response and independently rebuild the
 * exact manifest and unsigned PSBT from local authority. The response PSBT must
 * use one canonical serialization and contain no signature or unknown field. */
bool ValidateAlternativeRecoveryResponseTemplate(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    AlternativeRecoveryTemplate& trusted,
    PartiallySignedTransaction& unsigned_psbt,
    std::string& error);
bool ValidateAlternativeRecoverySubmitEnvelope(
    const AlternativeRecoverySubmit& submit,
    const uint256& expected_genesis,
    std::string& error);
/** Validate an exact user-signed submit against the persisted response and a
 * freshly rebuilt local recovery authority. */
bool ValidateAlternativeRecoverySubmit(
    const AlternativeRecoverySubmit& submit,
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    PartiallySignedTransaction& user_psbt,
    std::string& error);
bool ValidateAlternativeRecoveryResultEnvelope(
    const AlternativeRecoveryResultMessage& message,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error);
/** Bind a signed result to the exact recovery response and monotonically
 * increasing result sequence. Final transaction validation remains a separate
 * mandatory step before a final state transition. */
bool ValidateAlternativeRecoveryResult(
    const AlternativeRecoveryResultMessage& message,
    const AlternativeRecoveryResponse& response,
    const XOnlyPubKey& provider_identity_key,
    const uint256& expected_genesis,
    uint64_t minimum_sequence,
    int64_t now,
    std::string& error);

uint256 GetAlternativeRecoveryManifestId(const AlternativeRecoveryManifest& manifest);
uint256 GetAlternativeRecoveryTemplateCommitment(
    const AlternativeRecoveryManifest& manifest,
    const CollaborativePSBTTemplate& trusted_template);
uint256 GetRecoveryAuthorizationCommitment(
    const RecoveryAuthorizationManifest& manifest);
bool BuildRecoveryAuthorizationManifest(
    const AlternativeRecoveryRequest& request,
    const AlternativeRecoveryResponse& response,
    RecoveryAuthorizationManifest& authorization,
    std::string& error);
bool ValidateRecoveryAuthorizationManifest(
    const RecoveryAuthorizationManifest& authorization,
    const AlternativeRecoveryRequest& request,
    const AlternativeRecoveryResponse& response,
    int64_t now,
    std::string& error);

/** Build a deterministic same-user-input cancel-to-self PSBT after rechecking
 * the recovery provider's complete Capacity proof. No key, wallet mutation,
 * signing, commit, or broadcast occurs here. */
bool BuildAlternativeRecoveryTemplate(
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    AlternativeRecoveryTemplate& result,
    std::string& error);

/** Rebuild and compare the entire trusted template and manifest. Call this
 * immediately before every user/provider signature, retry, and broadcast. */
bool ValidateAlternativeRecoveryTemplate(
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    std::string& error);

/** Validate an untrusted recovery PSBT against a freshly rebuilt local
 * authority. Unknown global/input/output fields, roles, signatures, or
 * non-default sighashes are rejected by the common collaborative firewall. */
bool ValidateAlternativeRecoveryPSBT(
    const PartiallySignedTransaction& candidate,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    CollaborativeSignatureStage expected_stage,
    std::string& error);

/** Final-transaction counterpart used before recovery broadcast or final
 * state transition. It checks current capacity, the complete local manifest,
 * txid/wtxid, every witness, and all signatures against actual prevouts. */
bool ValidateFinalAlternativeRecoveryTransaction(
    const CMutableTransaction& final_transaction,
    const uint256& expected_wtxid,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    bool exact_final_already_known,
    std::string& error);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_RECOVERY_H
