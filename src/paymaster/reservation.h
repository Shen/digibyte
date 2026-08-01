// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Reservation state machine for client and provider wallet inputs. */

#ifndef DIGIBYTE_PAYMASTER_RESERVATION_H
#define DIGIBYTE_PAYMASTER_RESERVATION_H

#include <paymaster/types.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DigiDollar::Paymaster {

enum class SessionState : uint8_t {
    CREATED,
    INPUTS_RESERVED,
    AWAITING_WALLET_UNLOCK,
    AWAITING_USER_SIGNATURE,
    AUTHORIZED,
    DGB_COMMITTING,
    STEMPOOL,
    MEMPOOL,
    CONFIRMED,
    PENDING_PROVIDER,
    FAILED,
    CANCELED_SAFE,
    CONFLICTED,
};

enum class PendingPhase : uint8_t {
    NONE,
    USER_SIGNATURE_SENT,
    PROVIDER_SIGNED_KNOWN,
    PENDING_NETWORK,
    CANCEL_MEMPOOL,
};

enum class AttemptState : uint8_t {
    CANDIDATE,
    QUOTED,
    USER_SIGNED,
    USER_PSBT_ACCEPTED,
    PROVIDER_SIGNED,
    FINAL_COMMITTED,
    BROADCAST,
    STEMPOOL,
    MEMPOOL,
    REJECTED,
    QUOTE_EXPIRED,
    AMBIGUOUS,
    CONFLICTED,
};

enum class ReservationRole : uint8_t {
    USER_DD,
    USER_DGB,
    PROVIDER_CARRIER,
    PROVIDER_DGB,
};

struct PaymentSession {
    static constexpr uint16_t CURRENT_VERSION{4};

    uint16_t version{CURRENT_VERSION};
    std::string request_id;
    uint256 session_id;
    uint256 canonical_request_hash;
    FeeMode fee_mode_requested{FeeMode::DGB};
    FeeMode fee_mode_used{FeeMode::DGB};
    SessionState state{SessionState::CREATED};
    PendingPhase pending_phase{PendingPhase::NONE};
    std::vector<COutPoint> user_inputs;
    std::vector<uint256> attempt_ids;
    int64_t created_at{0};
    int64_t updated_at{0};
    uint256 final_txid;
    bool provider_side{false};
    uint256 recovery_txid;
    /** Original amount entered by the client. In the default mode this is
     * the recipient amount; in subtract mode it is the exact total DD
     * outflow authorized by the client. */
    DDCents requested_amount;
    bool subtract_paymaster_fee_from_amount{false};
    bool send_all_spendable_dd{false};

    SERIALIZE_METHODS(PaymentSession, obj)
    {
        READWRITE(obj.version, obj.request_id, obj.session_id, obj.canonical_request_hash,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FeeMode::AUTO)>>(obj.fee_mode_requested),
                  Using<EnumByteFormatter<static_cast<uint8_t>(FeeMode::AUTO)>>(obj.fee_mode_used),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SessionState::CONFLICTED)>>(obj.state),
                  Using<EnumByteFormatter<static_cast<uint8_t>(PendingPhase::CANCEL_MEMPOOL)>>(obj.pending_phase),
                  obj.user_inputs, obj.attempt_ids, obj.created_at, obj.updated_at, obj.final_txid,
                  obj.provider_side);
        if (obj.version >= 3) READWRITE(obj.recovery_txid);
        if (obj.version >= 4) {
            READWRITE(obj.requested_amount,
                      obj.subtract_paymaster_fee_from_amount,
                      obj.send_all_spendable_dd);
        }
    }
};

struct SelfRecoveryRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    std::string request_id;
    uint256 session_id;
    std::vector<COutPoint> user_inputs;
    uint256 recovery_txid;
    uint256 raw_transaction_hash;
    std::vector<unsigned char> final_transaction;
    int64_t created_at{0};

    SERIALIZE_METHODS(SelfRecoveryRecord, obj)
    {
        READWRITE(obj.version, obj.request_id, obj.session_id, obj.user_inputs,
                  obj.recovery_txid, obj.raw_transaction_hash,
                  obj.final_transaction, obj.created_at);
    }
};

struct InputReservation {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    COutPoint outpoint;
    std::string request_id;
    uint256 session_id;
    ReservationRole role{ReservationRole::USER_DD};
    bool authorization_may_exist{false};
    int64_t created_at{0};

    SERIALIZE_METHODS(InputReservation, obj)
    {
        READWRITE(obj.version, obj.outpoint, obj.request_id, obj.session_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(ReservationRole::PROVIDER_DGB)>>(obj.role),
                  obj.authorization_may_exist, obj.created_at);
    }
};

struct IdempotencyTombstone {
    static constexpr uint16_t CURRENT_VERSION{2};

    uint16_t version{CURRENT_VERSION};
    std::string request_id;
    uint256 session_id;
    uint256 canonical_request_hash;
    FeeMode fee_mode_requested{FeeMode::DGB};
    FeeMode fee_mode_used{FeeMode::DGB};
    SessionState final_state{SessionState::FAILED};
    uint256 final_txid;
    DDCents requested_amount;
    bool subtract_paymaster_fee_from_amount{false};
    bool send_all_spendable_dd{false};

    SERIALIZE_METHODS(IdempotencyTombstone, obj)
    {
        READWRITE(obj.version, obj.request_id, obj.session_id, obj.canonical_request_hash,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FeeMode::AUTO)>>(obj.fee_mode_requested),
                  Using<EnumByteFormatter<static_cast<uint8_t>(FeeMode::AUTO)>>(obj.fee_mode_used),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SessionState::CONFLICTED)>>(obj.final_state),
                  obj.final_txid);
        if (obj.version >= 2) {
            READWRITE(obj.requested_amount,
                      obj.subtract_paymaster_fee_from_amount,
                      obj.send_all_spendable_dd);
        }
    }
};

/** A fully client-validated, short-lived provider capacity snapshot. The
 * canonical request is retained on ProviderAttempt so no payment details need
 * to exist in this record. resource_commitment deliberately excludes the
 * provider-chosen snapshot id and binds the actual advertised outpoints,
 * preventing one wallet from accepting the same operational slot for two
 * conflicting live sessions. */
struct ValidatedCapacitySnapshot {
    static constexpr uint16_t CURRENT_VERSION{2};
    static constexpr uint16_t LEGACY_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 snapshot_id;
    uint256 resource_commitment;
    uint256 session_id;
    uint256 attempt_id;
    PaymasterId provider_id;
    uint256 client_nonce;
    uint256 request_hash;
    std::vector<unsigned char> capacity_proof;
    int64_t created_at{0};
    int64_t expires_at{0};
    int64_t validated_at{0};
    /** Null snapshot/resource identifiers still mean "absent". Use a
     * canonical enum default so parent records can durably represent their
     * pre-capacity state; deserialization continues to reject unknown bytes
     * and executable snapshots require the full signed binding. */
    FundingModel funding_model{FundingModel::USER_PAID};
    bool requires_carrier{false};

    SERIALIZE_METHODS(ValidatedCapacitySnapshot, obj)
    {
        READWRITE(obj.version, obj.snapshot_id, obj.resource_commitment,
                  obj.session_id, obj.attempt_id, obj.provider_id,
                  obj.client_nonce, obj.request_hash, obj.capacity_proof,
                  obj.created_at, obj.expires_at, obj.validated_at);
        if (obj.version >= 2) {
            READWRITE(
                Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                obj.requires_carrier);
        }
    }
};

/** Per-outpoint binding for one live validated capacity snapshot.
 *
 * The database key is (provider_id, outpoint), deliberately independent of
 * asset/role. A provider therefore cannot bind the same output concurrently
 * as DGB in one proof and as a DD carrier in another. The remaining fields
 * are evidence metadata and make replacement/corruption checks fail closed. */
struct CapacityResourceBinding {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    PaymasterId provider_id;
    COutPoint outpoint;
    uint256 snapshot_id;
    uint256 resource_commitment;
    uint256 session_id;
    uint256 attempt_id;
    uint256 proof_hash;
    bool carrier{false};
    uint256 creating_txid;
    int64_t value{0};
    int64_t expires_at{0};

    SERIALIZE_METHODS(CapacityResourceBinding, obj)
    {
        READWRITE(obj.version, obj.provider_id, obj.outpoint,
                  obj.snapshot_id, obj.resource_commitment, obj.session_id,
                  obj.attempt_id, obj.proof_hash, obj.carrier,
                  obj.creating_txid, obj.value, obj.expires_at);
    }
};

/** Durable evidence that a provider-side PMCAPREQ reservation was released
 * only after its signed capacity proof expired. Response and semantic replay
 * indexes remain through the bounded replay horizon and are then compacted
 * atomically; the expired signed envelope is no longer executable. */
struct ProviderCapacityReleaseRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 request_hash;
    uint256 client_nonce;
    int64_t released_at{0};

    SERIALIZE_METHODS(ProviderCapacityReleaseRecord, obj)
    {
        READWRITE(obj.version, obj.request_hash, obj.client_nonce,
                  obj.released_at);
    }
};

/** The complete local authority granted by a client wallet. This record is
 * built from the user's original request and the fully validated quote before
 * any transaction signature is produced. It intentionally has no field for a
 * client DGB input: such an input is categorically outside this authority. */
struct ClientAuthorizationManifest {
    static constexpr uint16_t CURRENT_VERSION{4};
    static constexpr uint16_t LEGACY_VERSION{3};

    uint16_t version{CURRENT_VERSION};
    uint256 manifest_id;
    std::string request_id;
    uint256 session_id;
    PaymasterId provider_id;
    uint256 offer_id;
    uint256 policy_hash;
    uint256 capacity_snapshot_id;
    uint256 capacity_resource_commitment;
    FundingModel funding_model{FundingModel::SPONSORED};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    std::vector<COutPoint> user_dd_inputs;
    CScript recipient_script;
    DDCents recipient_amount;
    CScript user_dd_change_script;
    DDCents maximum_service_fee;
    DDCents service_fee;
    int64_t expires_at{0};
    uint256 unsigned_txid;
    uint256 template_commitment;
    /** Exact redacted PMCAPREQ and signed PMCAPRESP bindings. These are
     * appended in v2 so v1 records remain decodable for explicit migration,
     * but v1 is never accepted for a new signature. */
    uint256 capacity_client_nonce;
    uint256 capacity_request_hash;
    uint256 capacity_proof_hash;
    /** Original local RPC/Qt order, already covered by every user input proof
     * through PaymentIntent v2, repeated here for the immediate spend
     * firewall and durable audit trail. */
    uint256 canonical_request_hash;
    FeeMode requested_fee_mode{FeeMode::PAYMASTER};
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    SelectionMode selection_mode{SelectionMode::LOWEST_TOTAL_COST};
    /** Wallet-local gross/net authorization. These fields never cross the
     * Paymaster wire protocol, but their manifest commitment is checked
     * immediately before every client signature and recovery action. */
    DDCents requested_amount;
    bool subtract_paymaster_fee_from_amount{false};
    bool send_all_spendable_dd{false};

    SERIALIZE_METHODS(ClientAuthorizationManifest, obj)
    {
        READWRITE(obj.version, obj.manifest_id, obj.request_id, obj.session_id,
                  obj.provider_id, obj.offer_id, obj.policy_hash,
                  obj.capacity_snapshot_id, obj.capacity_resource_commitment,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
                  obj.user_dd_inputs, obj.recipient_script, obj.recipient_amount,
                  obj.user_dd_change_script, obj.maximum_service_fee,
                  obj.service_fee, obj.expires_at, obj.unsigned_txid,
                  obj.template_commitment);
        if (obj.version >= 2) {
            READWRITE(obj.capacity_client_nonce,
                      obj.capacity_request_hash,
                      obj.capacity_proof_hash);
        }
        if (obj.version >= 3) {
            READWRITE(obj.canonical_request_hash,
                      Using<EnumByteFormatter<static_cast<uint8_t>(FeeMode::AUTO)>>(obj.requested_fee_mode),
                      Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                      Using<EnumByteFormatter<static_cast<uint8_t>(SelectionMode::PRIVACY_WEIGHTED)>>(obj.selection_mode));
        }
        if (obj.version >= 4) {
            READWRITE(obj.requested_amount,
                      obj.subtract_paymaster_fee_from_amount,
                      obj.send_all_spendable_dd);
        }
    }
};

constexpr bool IsSupportedClientAuthorizationManifestVersion(uint16_t version)
{
    return version == ClientAuthorizationManifest::LEGACY_VERSION ||
           version == ClientAuthorizationManifest::CURRENT_VERSION;
}

/** The complete local authority granted by a provider wallet. Scripts in
 * this record are generated by that wallet before quote signing. Empty
 * vectors mean the corresponding optional role is forbidden; otherwise they
 * contain exactly one value. */
struct ProviderAuthorizationManifest {
    static constexpr uint16_t CURRENT_VERSION{2};
    static constexpr uint16_t LEGACY_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 manifest_id;
    std::string request_id;
    uint256 session_id;
    PaymasterId provider_id;
    uint256 intent_hash;
    FundingModel funding_model{FundingModel::SPONSORED};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    std::vector<COutPoint> provider_dgb_inputs;
    std::vector<COutPoint> provider_carrier_inputs;
    std::vector<CScript> carrier_return_scripts;
    std::vector<CScript> provider_fee_scripts;
    std::vector<CScript> dgb_change_scripts;
    DDCents service_fee;
    DGBSatoshis network_fee;
    uint256 safety_policy_hash;
    /** Exact wallet-local safety-ledger reservation that must exist before
     * this authority can create a provider signature.  The reservation id is
     * the protocol commit key, while maximum_network_fee is the worst-case
     * DGB debit atomically reserved for that key. */
    uint256 budget_reservation_id;
    DGBSatoshis maximum_network_fee;
    int64_t expires_at{0};
    uint256 unsigned_txid;
    uint256 template_commitment;

    SERIALIZE_METHODS(ProviderAuthorizationManifest, obj)
    {
        READWRITE(obj.version, obj.manifest_id, obj.request_id, obj.session_id,
                  obj.provider_id, obj.intent_hash,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
                  obj.provider_dgb_inputs, obj.provider_carrier_inputs,
                  obj.carrier_return_scripts, obj.provider_fee_scripts,
                  obj.dgb_change_scripts, obj.service_fee, obj.network_fee,
                  obj.safety_policy_hash);
        if (obj.version >= 2) {
            READWRITE(obj.budget_reservation_id,
                      obj.maximum_network_fee);
        }
        READWRITE(obj.expires_at, obj.unsigned_txid,
                  obj.template_commitment);
    }
};

struct ProviderAttempt {
    static constexpr uint16_t CURRENT_VERSION{15};
    static constexpr uint16_t LEGACY_VERSION{9};

    uint16_t version{CURRENT_VERSION};
    uint256 session_id;
    uint256 attempt_id;
    PaymasterId provider_id;
    XOnlyPubKey provider_identity_key;
    std::string provider_endpoint;
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    AttemptState state{AttemptState::CANDIDATE};
    uint256 commit_key;
    uint256 client_nonce;
    uint256 intent_hash;
    uint256 quote_id;
    uint256 unsigned_txid;
    uint256 template_commitment;
    uint256 sponsorship_capability_hash;
    std::vector<unsigned char> capacity_request;
    ValidatedCapacitySnapshot capacity_snapshot;
    ClientAuthorizationManifest client_manifest;
    ProviderAuthorizationManifest provider_manifest;
    /** Wallet-HMAC pseudonym of the process-local origin netgroup. Never a
     * raw IP address; required only for newly created provider-side quotes. */
    uint256 provider_netgroup_bucket;
    std::vector<unsigned char> unsigned_intent;
    std::vector<unsigned char> quote_request;
    std::vector<unsigned char> signed_quote;
    std::vector<unsigned char> unsigned_transaction;
    std::vector<unsigned char> unsigned_psbt;
    std::vector<ReservationRole> input_roles;
    std::vector<unsigned char> user_signed_psbt;
    std::vector<unsigned char> final_transaction;
    uint256 final_txid;
    int64_t quote_expires_at{0};
    int64_t retry_until{0};
    int64_t created_at{0};
    int64_t updated_at{0};
    /** Explicit local approval of the exact client authorization manifest.
     * These fields are deliberately last so V9--V12 records remain readable
     * with an empty (therefore non-authorizing) approval. */
    uint256 accepted_client_manifest_id;
    int64_t client_manifest_accepted_at{0};
    /** Time at which the provider input signature was created. This is a
     * durable authorization boundary, not the later database-commit time. */
    int64_t provider_signed_at{0};
    /** Canonical, identity-signed FINAL_COMMITTED PaymasterResult prepared at
     * provider_signed_at. It stays private inside the attempt until the exact
     * ProviderCommit is atomically persisted. */
    std::vector<unsigned char> provider_signed_result;
    /** First canonical provider-signed capacity claim observed for this exact
     * client attempt. This is an equivocation/replay barrier only: it is never
     * a validated Capacity snapshot and grants no spending authority. */
    std::vector<unsigned char> capacity_proof_claim_candidate;
    /** First canonical provider-signed quote claim observed for this exact
     * client attempt. This is evidence-only and must never substitute for the
     * fully validated signed_quote or an authorization manifest. */
    std::vector<unsigned char> quote_response_claim_candidate;

    SERIALIZE_METHODS(ProviderAttempt, obj)
    {
        READWRITE(obj.version, obj.session_id, obj.attempt_id, obj.provider_id,
                  obj.provider_identity_key,
                  obj.provider_endpoint,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                  Using<EnumByteFormatter<static_cast<uint8_t>(AttemptState::CONFLICTED)>>(obj.state),
                  obj.commit_key, obj.client_nonce, obj.intent_hash, obj.quote_id,
                  obj.unsigned_txid, obj.template_commitment);
        if (obj.version >= 9) READWRITE(obj.sponsorship_capability_hash);
        if (obj.version >= 10) READWRITE(obj.capacity_request, obj.capacity_snapshot);
        if (obj.version >= 11) READWRITE(obj.client_manifest, obj.provider_manifest);
        if (obj.version >= 12) READWRITE(obj.provider_netgroup_bucket);
        READWRITE(obj.unsigned_intent,
                  obj.quote_request,
                  obj.signed_quote, obj.unsigned_transaction,
                  obj.unsigned_psbt,
                  Using<VectorFormatter<EnumByteFormatter<static_cast<uint8_t>(ReservationRole::PROVIDER_DGB)>>>(obj.input_roles),
                  obj.user_signed_psbt, obj.final_transaction, obj.final_txid,
                  obj.quote_expires_at, obj.retry_until, obj.created_at, obj.updated_at);
        if (obj.version >= 13) {
            READWRITE(obj.accepted_client_manifest_id,
                      obj.client_manifest_accepted_at);
        }
        if (obj.version >= 14) {
            READWRITE(obj.provider_signed_at,
                      obj.provider_signed_result);
        }
        if (obj.version >= 15) {
            READWRITE(obj.capacity_proof_claim_candidate,
                      obj.quote_response_claim_candidate);
        }
    }
};

struct ProviderCommitRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 commit_key;
    PaymasterId provider_id;
    uint256 quote_id;
    uint256 template_commitment;
    uint256 final_txid;
    uint256 raw_transaction_hash;
    std::vector<unsigned char> final_transaction;
    std::vector<COutPoint> provider_inputs;
    int64_t committed_at{0};
    int64_t retry_until{0};

    SERIALIZE_METHODS(ProviderCommitRecord, obj)
    {
        READWRITE(obj.version, obj.commit_key, obj.provider_id, obj.quote_id,
                  obj.template_commitment, obj.final_txid, obj.raw_transaction_hash,
                  obj.final_transaction, obj.provider_inputs, obj.committed_at, obj.retry_until);
    }
};

struct UserAuthorizationRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 commit_key;
    uint256 attempt_id;
    uint256 canonical_psbt_hash;
    int64_t accepted_at{0};
    int64_t retry_until{0};

    SERIALIZE_METHODS(UserAuthorizationRecord, obj)
    {
        READWRITE(obj.version, obj.commit_key, obj.attempt_id,
                  obj.canonical_psbt_hash, obj.accepted_at, obj.retry_until);
    }
};

bool IsTerminal(SessionState state);
bool CanTransition(SessionState from, SessionState to);
bool CanTransition(AttemptState from, AttemptState to);
std::string_view SessionStateName(SessionState state);
std::string_view PendingPhaseName(PendingPhase phase);
std::string_view AttemptStateName(AttemptState state);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_RESERVATION_H
