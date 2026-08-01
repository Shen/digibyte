// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Role-aware PSBT templates and the client/provider spending firewalls.
 * Manifests bind the complete authorized transaction, and validation rejects
 * unknown fields, output roles, inputs, or sighash modes before either party
 * is allowed to sign its own inputs.
 */

#include <paymaster/psbt.h>

#include <hash.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/wire.h>
#include <script/interpreter.h>
#include <streams.h>
#include <version.h>

#include <algorithm>
#include <limits>
#include <map>

namespace DigiDollar::Paymaster {
namespace {

template <typename T>
std::vector<unsigned char> SerializeExact(const T& value)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool ValidatePersistedProviderSignatureEnvelope(
    const ProviderAttempt& attempt,
    std::string& error)
{
    if (attempt.provider_signed_at <= 0 ||
        attempt.provider_signed_at > attempt.retry_until ||
        attempt.provider_signed_at > attempt.updated_at ||
        attempt.provider_signed_result.empty() ||
        !attempt.provider_identity_key.IsFullyValid()) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        return false;
    }
    PaymasterResult result;
    try {
        SpanReader stream{::PROTOCOL_VERSION,
                          attempt.provider_signed_result};
        stream >> result;
        if (!stream.empty() ||
            SerializeExact(result) != attempt.provider_signed_result) {
            throw std::ios_base::failure(
                "non-canonical provider-signed result");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        return false;
    }
    if (!ValidatePaymasterResult(
            result, result.genesis_hash, attempt.provider_id,
            attempt.commit_key, attempt.provider_identity_key, 1, error) ||
        result.result_sequence != 1 ||
        result.status != PaymasterResultStatus::FINAL_COMMITTED ||
        result.updated_at != attempt.provider_signed_at || !result.txid ||
        *result.txid != attempt.final_txid ||
        !result.raw_transaction_hash ||
        *result.raw_transaction_hash != Hash(attempt.final_transaction) ||
        !result.final_transaction ||
        SerializeExact(*result.final_transaction) !=
            attempt.final_transaction) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        }
        return false;
    }
    return true;
}

void ClearSignatures(PSBTInput& input)
{
    input.partial_sigs.clear();
    input.final_script_sig.clear();
    input.final_script_witness.SetNull();
    input.m_tap_key_sig.clear();
    input.m_tap_script_sigs.clear();
}

bool HasNonFinalSignatures(const PSBTInput& input)
{
    return !input.partial_sigs.empty() || !input.m_tap_key_sig.empty() ||
           !input.m_tap_script_sigs.empty();
}

bool HasFinalTaprootKeyPathDefaultSighash(const PSBTInput& input, const CTxOut& prevout)
{
    int witness_version{-1};
    std::vector<unsigned char> witness_program;
    return prevout.scriptPubKey.IsWitnessProgram(witness_version, witness_program) &&
           witness_version == 1 && witness_program.size() == WITNESS_V1_TAPROOT_SIZE &&
           input.final_script_sig.empty() && input.final_script_witness.stack.size() == 1 &&
           input.final_script_witness.stack.front().size() == 64;
}

bool SameUnsignedTransaction(const PartiallySignedTransaction& lhs,
                             const PartiallySignedTransaction& rhs)
{
    return lhs.tx.has_value() && rhs.tx.has_value() &&
           SerializeExact(*lhs.tx) == SerializeExact(*rhs.tx);
}

bool SameGlobalMap(const PartiallySignedTransaction& lhs,
                   const PartiallySignedTransaction& rhs)
{
    PartiallySignedTransaction normalized{lhs};
    normalized.tx = rhs.tx;
    normalized.inputs = rhs.inputs;
    normalized.outputs = rhs.outputs;
    return SerializeExact(normalized) == SerializeExact(rhs);
}

bool RoleMustBeSigned(InputRole role, CollaborativeSignatureStage stage)
{
    if (stage == CollaborativeSignatureStage::FULLY_SIGNED) return true;
    return stage == CollaborativeSignatureStage::USER_SIGNED &&
           IsInputOwnedBy(role, SigningParty::USER);
}

template <typename T>
std::vector<T> OptionalVector(const std::optional<T>& value)
{
    return value ? std::vector<T>{*value} : std::vector<T>{};
}

bool ValidateRoleLayout(const PaymentIntent& intent,
                        const PaymasterQuote& quote,
                        const CollaborativePSBTTemplate& trusted,
                        std::string& error)
{
    if (!trusted.psbt.tx || trusted.input_roles.size() != trusted.psbt.inputs.size() ||
        trusted.input_roles.size() != trusted.psbt.tx->vin.size() ||
        CTransaction{*trusted.psbt.tx} != CTransaction{quote.unsigned_transaction}) {
        error = "PAYMASTER_AUTH_TEMPLATE_MISMATCH";
        return false;
    }
    size_t index{0};
    for (const COutPoint& outpoint : intent.user_dd_inputs) {
        if (index >= trusted.input_roles.size() ||
            trusted.input_roles[index] != InputRole::USER_DD ||
            trusted.psbt.tx->vin[index].prevout != outpoint) {
            error = "PAYMASTER_AUTH_USER_INPUT_MISMATCH";
            return false;
        }
        ++index;
    }
    if (quote.reserved_carrier) {
        if (index >= trusted.input_roles.size() ||
            trusted.input_roles[index] != InputRole::PROVIDER_CARRIER ||
            trusted.psbt.tx->vin[index].prevout != quote.reserved_carrier->outpoint) {
            error = "PAYMASTER_AUTH_CARRIER_INPUT_MISMATCH";
            return false;
        }
        ++index;
    }
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        if (index >= trusted.input_roles.size() ||
            trusted.input_roles[index] != InputRole::PROVIDER_DGB ||
            trusted.psbt.tx->vin[index].prevout != input.outpoint) {
            error = "PAYMASTER_AUTH_PROVIDER_INPUT_MISMATCH";
            return false;
        }
        ++index;
    }
    if (index != trusted.input_roles.size() ||
        std::find(trusted.input_roles.begin(), trusted.input_roles.end(),
                  InputRole::USER_DGB) != trusted.input_roles.end()) {
        error = "PAYMASTER_AUTH_UNKNOWN_INPUT_ROLE";
        return false;
    }
    return true;
}

bool ToInputRole(ReservationRole role, InputRole& result)
{
    switch (role) {
    case ReservationRole::USER_DD: result = InputRole::USER_DD; return true;
    case ReservationRole::USER_DGB: result = InputRole::USER_DGB; return true;
    case ReservationRole::PROVIDER_CARRIER: result = InputRole::PROVIDER_CARRIER; return true;
    case ReservationRole::PROVIDER_DGB: result = InputRole::PROVIDER_DGB; return true;
    }
    return false;
}

bool DecodeProviderAuthorizationArtifacts(
    const ProviderAttempt& attempt,
    PaymentIntent& intent,
    PaymasterQuote& quote,
    CollaborativePSBTTemplate& trusted,
    bool allow_legacy_protocol,
    std::string& error)
{
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    CMutableTransaction persisted_unsigned;
    PartiallySignedTransaction psbt;
    try {
        CDataStream request_stream{attempt.quote_request, SER_NETWORK,
                                   ::PROTOCOL_VERSION};
        request_stream >> request;
        CDataStream response_stream{attempt.signed_quote, SER_NETWORK,
                                    ::PROTOCOL_VERSION};
        response_stream >> response;
        SpanReader transaction_stream{::PROTOCOL_VERSION, attempt.unsigned_transaction};
        transaction_stream >> persisted_unsigned;
        if (!request_stream.empty() || !response_stream.empty() ||
            !transaction_stream.empty()) {
            throw std::ios_base::failure("trailing provider authorization data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_ENCODING";
        return false;
    }
    std::string decode_error;
    if (!DecodeRawPSBT(psbt, MakeByteSpan(attempt.unsigned_psbt), decode_error)) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_TEMPLATE_ENCODING";
        return false;
    }
    if (SerializeExact(request) != attempt.quote_request ||
        SerializeExact(response) != attempt.signed_quote ||
        SerializeExact(persisted_unsigned) != attempt.unsigned_transaction ||
        SerializeExact(psbt) != attempt.unsigned_psbt) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_NONCANONICAL";
        return false;
    }

    trusted = {};
    trusted.psbt = std::move(psbt);
    trusted.input_roles.reserve(attempt.input_roles.size());
    for (const ReservationRole role : attempt.input_roles) {
        InputRole input_role;
        if (!ToInputRole(role, input_role)) {
            error = "PAYMASTER_PROVIDER_AUTHORIZATION_INPUT_ROLE";
            return false;
        }
        trusted.input_roles.push_back(input_role);
    }

    intent = std::move(request.intent);
    quote = std::move(response.quote);
    const uint256 intent_hash{GetPaymentIntentHash(intent)};
    // Versions 1-4 are accepted only by the exact durable-commit recovery
    // caller. New provider signatures and all client finalization stay pinned
    // to the current protocol with no downgrade path.
    const bool supported_protocol{
        request.version == response.version &&
        (request.version == DigiDollar::Paymaster::PROTOCOL_VERSION ||
         (allow_legacy_protocol &&
          request.version >= 1 &&
          request.version < DigiDollar::Paymaster::PROTOCOL_VERSION))};
    const bool supported_intent{
        intent.version == PaymentIntent::CURRENT_VERSION ||
        (allow_legacy_protocol &&
         intent.version == PaymentIntent::LEGACY_VERSION)};
    if (!supported_protocol ||
        !supported_intent ||
        quote.version != PaymasterQuote::CURRENT_VERSION ||
        response.request_id != intent.request_id ||
        response.session_id != intent.session_id ||
        quote.genesis_hash != intent.genesis_hash ||
        attempt.session_id != intent.session_id ||
        attempt.provider_id != intent.provider_id ||
        attempt.client_nonce != intent.client_nonce ||
        attempt.intent_hash != intent_hash || quote.intent_hash != intent_hash ||
        quote.provider_id != attempt.provider_id ||
        quote.offer_id != intent.offer_id ||
        quote.policy_hash != intent.policy_hash ||
        quote.funding_model != intent.funding_model ||
        quote.sponsorship_scope != intent.sponsorship_scope ||
        quote.sponsorship_authorization_hash !=
            intent.sponsorship_authorization_hash ||
        quote.quote_id != attempt.quote_id ||
        quote.template_commitment != attempt.template_commitment ||
        quote.unsigned_txid != attempt.unsigned_txid ||
        quote.created_at <= 0 || quote.expires_at <= quote.created_at ||
        quote.retry_until < quote.expires_at ||
        quote.expires_at != attempt.quote_expires_at ||
        quote.retry_until != attempt.retry_until ||
        CTransaction{persisted_unsigned}.GetHash() != attempt.unsigned_txid ||
        CTransaction{quote.unsigned_transaction}.GetHash() != attempt.unsigned_txid ||
        CTransaction{persisted_unsigned} != CTransaction{quote.unsigned_transaction} ||
        !trusted.psbt.tx ||
        CTransaction{*trusted.psbt.tx} != CTransaction{quote.unsigned_transaction} ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != attempt.provider_id ||
        quote.identity_signature.size() != 64 ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(quote), quote.identity_signature) ||
        attempt.commit_key != GetPaymasterCommitKey(
                                  attempt.provider_id, attempt.client_nonce,
                                  attempt.intent_hash, attempt.quote_id,
                                  attempt.template_commitment) ||
        attempt.template_commitment !=
            GetCollaborativeTemplateCommitment(intent, quote, trusted)) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT";
        return false;
    }
    if (!ValidateCollaborativePSBT(
            trusted.psbt, trusted, CollaborativeSignatureStage::UNSIGNED, error)) {
        return false;
    }
    return true;
}

bool ValidateProviderManifestForExecution(
    const ProviderAttempt& attempt,
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted,
    bool allow_legacy_commit,
    std::string& error)
{
    if (!attempt.provider_manifest.manifest_id.IsNull()) {
        if (allow_legacy_commit &&
            attempt.provider_manifest.version ==
                ProviderAuthorizationManifest::LEGACY_VERSION) {
            // V1 did not bind its wallet-local budget record. It can never
            // authorize a new signature, but an already durable exact commit
            // remains recoverable after first verifying the original V1
            // manifest id and then projecting the missing fields from the
            // immutable quote/commit artifacts.
            if (attempt.provider_manifest.manifest_id !=
                GetProviderAuthorizationManifestId(
                    attempt.provider_manifest)) {
                error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID";
                return false;
            }
            ProviderAuthorizationManifest upgraded{
                attempt.provider_manifest};
            upgraded.version =
                ProviderAuthorizationManifest::CURRENT_VERSION;
            upgraded.budget_reservation_id = attempt.commit_key;
            upgraded.maximum_network_fee = quote.network_fee;
            upgraded.manifest_id =
                GetProviderAuthorizationManifestId(upgraded);
            return ValidateProviderAuthorizationManifest(
                upgraded, intent, quote, trusted, error);
        }
        return ValidateProviderAuthorizationManifest(
            attempt.provider_manifest, intent, quote, trusted, error);
    }
    if (!allow_legacy_commit) {
        error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        return false;
    }

    // V9/V10 records are upgraded in memory by WalletBatch and therefore no
    // longer expose their original version. They remain recoverable only as
    // an already durable, byte-exact commit. Reconstructing and validating an
    // ephemeral manifest gives that migration path the same structural spend
    // firewall without permitting a new signature.
    HashWriter marker_hasher = TaggedHash("DigiByte Paymaster Legacy Recovery Authority v1");
    marker_hasher << attempt.attempt_id << attempt.commit_key;
    const uint256 safety_marker{marker_hasher.GetSHA256()};
    ProviderAuthorizationManifest legacy_manifest;
    return !attempt.attempt_id.IsNull() && !attempt.commit_key.IsNull() &&
           BuildProviderAuthorizationManifest(
               intent, quote, trusted, safety_marker, legacy_manifest, error);
}

} // namespace

bool IsInputOwnedBy(InputRole role, SigningParty party)
{
    switch (role) {
    case InputRole::USER_DD:
    case InputRole::USER_DGB:
        return party == SigningParty::USER;
    case InputRole::PROVIDER_CARRIER:
    case InputRole::PROVIDER_DGB:
        return party == SigningParty::PROVIDER;
    }
    return false;
}

uint256 GetCollaborativeTemplateCommitment(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted_template)
{
    PaymasterQuote projection{quote};
    projection.template_commitment.SetNull();
    projection.identity_signature.clear();
    std::vector<uint8_t> roles;
    roles.reserve(trusted_template.input_roles.size());
    for (const InputRole role : trusted_template.input_roles) {
        roles.push_back(static_cast<uint8_t>(role));
    }
    const std::vector<unsigned char> canonical_quote = SerializeExact(projection);
    const std::vector<unsigned char> canonical_psbt = SerializeExact(trusted_template.psbt);
    HashWriter hasher = TaggedHash("DigiByte Paymaster Template v1");
    hasher << GetPaymentIntentHash(intent) << canonical_quote << canonical_psbt << roles;
    return hasher.GetSHA256();
}

uint256 GetClientAuthorizationManifestId(const ClientAuthorizationManifest& manifest)
{
    ClientAuthorizationManifest projection{manifest};
    projection.manifest_id.SetNull();
    HashWriter hasher = TaggedHash("DigiByte Paymaster Client Authority v1");
    hasher << projection;
    return hasher.GetSHA256();
}

uint256 GetProviderAuthorizationManifestId(const ProviderAuthorizationManifest& manifest)
{
    ProviderAuthorizationManifest projection{manifest};
    projection.manifest_id.SetNull();
    HashWriter hasher = TaggedHash("DigiByte Paymaster Provider Authority v1");
    hasher << projection;
    return hasher.GetSHA256();
}

bool ValidateQuoteAgainstCapacitySnapshot(
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    std::string& error)
{
    PaymasterCapacityProof proof;
    try {
        CDataStream stream{capacity.capacity_proof, SER_NETWORK,
                           ::PROTOCOL_VERSION};
        stream >> proof;
        if (!stream.empty()) throw std::ios_base::failure("trailing capacity proof");
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
        return false;
    }
    if (SerializeExact(proof) != capacity.capacity_proof ||
        proof.snapshot_id != capacity.snapshot_id ||
        proof.provider_id != capacity.provider_id ||
        proof.client_nonce != capacity.client_nonce ||
        proof.created_at != capacity.created_at ||
        proof.expires_at != capacity.expires_at ||
        proof.funding_model != capacity.funding_model ||
        proof.requires_carrier != capacity.requires_carrier ||
        GetCapacityResourceCommitment(proof) != capacity.resource_commitment ||
        proof.liquidity_slots.size() != 1 || quote.provider_id != proof.provider_id ||
        quote.funding_model != proof.funding_model) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
        return false;
    }

    const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
    if (quote.reserved_dgb_inputs.size() != slot.dgb_inputs.size()) {
        error = "PAYMASTER_CAPACITY_QUOTE_MISMATCH";
        return false;
    }
    std::map<COutPoint, std::pair<uint256, DGBSatoshis>> capacity_dgb;
    for (const CapacityDGBInput& input : slot.dgb_inputs) {
        if (!capacity_dgb.emplace(
                             input.input.outpoint,
                             std::make_pair(CTransaction{input.input.creating_tx}.GetHash(),
                                            input.input.value))
                 .second) {
            error = "PAYMASTER_CAPACITY_QUOTE_MISMATCH";
            return false;
        }
    }
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        const auto it = capacity_dgb.find(input.outpoint);
        if (it == capacity_dgb.end() ||
            it->second.first != CTransaction{input.creating_tx}.GetHash() ||
            !(it->second.second == input.value)) {
            error = "PAYMASTER_CAPACITY_QUOTE_MISMATCH";
            return false;
        }
    }
    if (quote.reserved_carrier.has_value() != proof.requires_carrier ||
        slot.carrier.has_value() != proof.requires_carrier ||
        (quote.reserved_carrier &&
        (!slot.carrier ||
         quote.reserved_carrier->outpoint != slot.carrier->carrier.outpoint ||
         CTransaction{quote.reserved_carrier->creating_tx}.GetHash() !=
             CTransaction{slot.carrier->carrier.creating_tx}.GetHash() ||
         !(quote.reserved_carrier->value == slot.carrier->carrier.value)))) {
        error = "PAYMASTER_CAPACITY_QUOTE_MISMATCH";
        return false;
    }
    error.clear();
    return true;
}

bool BuildClientAuthorizationManifest(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    const CollaborativePSBTTemplate& trusted_template,
    DDCents maximum_service_fee,
    ClientAuthorizationManifest& manifest,
    std::string& error)
{
    return BuildClientAuthorizationManifest(
        intent, quote, capacity, trusted_template, maximum_service_fee,
        intent.recipient_amount, /*subtract_paymaster_fee_from_amount=*/false,
        /*send_all_spendable_dd=*/false, manifest, error);
}

bool BuildClientAuthorizationManifest(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    const CollaborativePSBTTemplate& trusted_template,
    DDCents maximum_service_fee,
    DDCents requested_amount,
    bool subtract_paymaster_fee_from_amount,
    bool send_all_spendable_dd,
    ClientAuthorizationManifest& manifest,
    std::string& error)
{
    manifest = {};
    manifest.request_id = intent.request_id;
    manifest.session_id = intent.session_id;
    manifest.provider_id = intent.provider_id;
    manifest.offer_id = intent.offer_id;
    manifest.policy_hash = intent.policy_hash;
    manifest.capacity_snapshot_id = capacity.snapshot_id;
    manifest.capacity_resource_commitment = capacity.resource_commitment;
    manifest.capacity_client_nonce = capacity.client_nonce;
    manifest.capacity_request_hash = capacity.request_hash;
    manifest.capacity_proof_hash = Hash(capacity.capacity_proof);
    manifest.canonical_request_hash = intent.canonical_request_hash;
    manifest.requested_fee_mode = intent.requested_fee_mode;
    manifest.privacy_profile = intent.privacy_profile;
    manifest.selection_mode = intent.selection_mode;
    manifest.requested_amount = requested_amount;
    manifest.subtract_paymaster_fee_from_amount =
        subtract_paymaster_fee_from_amount;
    manifest.send_all_spendable_dd = send_all_spendable_dd;
    manifest.funding_model = intent.funding_model;
    manifest.sponsorship_scope = intent.sponsorship_scope;
    manifest.user_dd_inputs = intent.user_dd_inputs;
    manifest.recipient_script = intent.recipient_script;
    manifest.recipient_amount = intent.recipient_amount;
    manifest.user_dd_change_script = intent.user_dd_change_script;
    manifest.maximum_service_fee = maximum_service_fee;
    manifest.service_fee = quote.service_fee;
    // The initial signature path separately enforces quote.expires_at. Once
    // that exact signature exists, idempotent submit/recovery remains valid
    // for the explicitly signed retry window.
    manifest.expires_at = quote.retry_until;
    manifest.unsigned_txid = quote.unsigned_txid;
    manifest.template_commitment = quote.template_commitment;
    manifest.manifest_id = GetClientAuthorizationManifestId(manifest);
    return ValidateClientAuthorizationManifest(
        manifest, intent, quote, capacity, trusted_template, error);
}

bool ValidateClientAuthorizationManifest(
    const ClientAuthorizationManifest& manifest,
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    const CollaborativePSBTTemplate& trusted_template,
    std::string& error)
{
    if (!IsSupportedClientAuthorizationManifestVersion(manifest.version) ||
        manifest.manifest_id.IsNull() ||
        manifest.manifest_id != GetClientAuthorizationManifestId(manifest)) {
        error = "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID";
        return false;
    }
    if (manifest.version >= 4) {
        if (manifest.requested_amount.value <= 0 ||
            manifest.requested_amount.value > MAX_DD_OUTPUT_CENTS ||
            (manifest.send_all_spendable_dd &&
             !manifest.subtract_paymaster_fee_from_amount)) {
            error = "PAYMASTER_CLIENT_AUTH_AMOUNT_INVALID";
            return false;
        }
        if (manifest.subtract_paymaster_fee_from_amount) {
            if (manifest.recipient_amount.value >
                    std::numeric_limits<int64_t>::max() -
                        manifest.service_fee.value ||
                manifest.recipient_amount.value + manifest.service_fee.value !=
                    manifest.requested_amount.value) {
                error = "PAYMASTER_CLIENT_AUTH_GROSS_AMOUNT_MISMATCH";
                return false;
            }
        } else if (manifest.recipient_amount != manifest.requested_amount) {
            error = "PAYMASTER_CLIENT_AUTH_RECIPIENT_AMOUNT_MISMATCH";
            return false;
        }
    }
    const int64_t expected_expiry = quote.retry_until;
    if (manifest.request_id != intent.request_id ||
        manifest.session_id != intent.session_id ||
        manifest.provider_id != intent.provider_id ||
        manifest.provider_id != quote.provider_id ||
        manifest.offer_id != intent.offer_id || manifest.offer_id != quote.offer_id ||
        manifest.policy_hash != intent.policy_hash || manifest.policy_hash != quote.policy_hash ||
        manifest.capacity_snapshot_id != capacity.snapshot_id ||
        manifest.capacity_resource_commitment != capacity.resource_commitment ||
        manifest.capacity_client_nonce != intent.client_nonce ||
        manifest.capacity_client_nonce != capacity.client_nonce ||
        manifest.capacity_request_hash.IsNull() ||
        manifest.capacity_request_hash != capacity.request_hash ||
        manifest.capacity_proof_hash.IsNull() ||
        manifest.capacity_proof_hash != Hash(capacity.capacity_proof) ||
        manifest.canonical_request_hash.IsNull() ||
        manifest.canonical_request_hash != intent.canonical_request_hash ||
        manifest.requested_fee_mode != intent.requested_fee_mode ||
        manifest.privacy_profile != intent.privacy_profile ||
        manifest.selection_mode != intent.selection_mode ||
        capacity.session_id != intent.session_id || capacity.provider_id != intent.provider_id ||
        manifest.funding_model != intent.funding_model ||
        manifest.funding_model != quote.funding_model ||
        manifest.sponsorship_scope != intent.sponsorship_scope ||
        manifest.sponsorship_scope != quote.sponsorship_scope ||
        manifest.user_dd_inputs != intent.user_dd_inputs ||
        manifest.recipient_script != intent.recipient_script ||
        !(manifest.recipient_amount == intent.recipient_amount) ||
        manifest.user_dd_change_script != intent.user_dd_change_script ||
        manifest.maximum_service_fee.value < 0 ||
        !(manifest.service_fee == quote.service_fee) ||
        manifest.service_fee.value > manifest.maximum_service_fee.value ||
        manifest.expires_at != expected_expiry || manifest.expires_at <= 0 ||
        manifest.unsigned_txid != quote.unsigned_txid ||
        manifest.template_commitment != quote.template_commitment ||
        quote.template_commitment !=
            GetCollaborativeTemplateCommitment(intent, quote, trusted_template)) {
        error = "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH";
        return false;
    }
    return ValidateQuoteAgainstCapacitySnapshot(quote, capacity, error) &&
           ValidateRoleLayout(intent, quote, trusted_template, error);
}

bool BuildProviderAuthorizationManifest(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted_template,
    const uint256& safety_policy_hash,
    ProviderAuthorizationManifest& manifest,
    std::string& error)
{
    manifest = {};
    manifest.request_id = intent.request_id;
    manifest.session_id = intent.session_id;
    manifest.provider_id = quote.provider_id;
    manifest.intent_hash = quote.intent_hash;
    manifest.funding_model = quote.funding_model;
    manifest.sponsorship_scope = quote.sponsorship_scope;
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        manifest.provider_dgb_inputs.push_back(input.outpoint);
    }
    if (quote.reserved_carrier) {
        manifest.provider_carrier_inputs.push_back(quote.reserved_carrier->outpoint);
    }
    manifest.carrier_return_scripts = OptionalVector(quote.carrier_return_script);
    manifest.provider_fee_scripts = OptionalVector(quote.provider_fee_script);
    manifest.dgb_change_scripts = OptionalVector(quote.dgb_change_script);
    manifest.service_fee = quote.service_fee;
    manifest.network_fee = quote.network_fee;
    manifest.safety_policy_hash = safety_policy_hash;
    manifest.budget_reservation_id = GetPaymasterCommitKey(
        quote.provider_id, intent.client_nonce, quote.intent_hash,
        quote.quote_id, quote.template_commitment);
    manifest.maximum_network_fee = quote.network_fee;
    manifest.expires_at = quote.retry_until;
    manifest.unsigned_txid = quote.unsigned_txid;
    manifest.template_commitment = quote.template_commitment;
    manifest.manifest_id = GetProviderAuthorizationManifestId(manifest);
    return ValidateProviderAuthorizationManifest(
        manifest, intent, quote, trusted_template, error);
}

bool ValidateProviderAuthorizationManifest(
    const ProviderAuthorizationManifest& manifest,
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted_template,
    std::string& error)
{
    if (manifest.version != ProviderAuthorizationManifest::CURRENT_VERSION ||
        manifest.manifest_id.IsNull() || manifest.safety_policy_hash.IsNull() ||
        manifest.budget_reservation_id.IsNull() ||
        manifest.maximum_network_fee.value < 0 ||
        manifest.manifest_id != GetProviderAuthorizationManifestId(manifest)) {
        error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID";
        return false;
    }
    std::vector<COutPoint> dgb_inputs;
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        dgb_inputs.push_back(input.outpoint);
    }
    const std::vector<COutPoint> carrier_inputs = quote.reserved_carrier ? std::vector<COutPoint>{quote.reserved_carrier->outpoint} : std::vector<COutPoint>{};
    if (manifest.request_id != intent.request_id ||
        manifest.session_id != intent.session_id ||
        manifest.provider_id != intent.provider_id ||
        manifest.provider_id != quote.provider_id ||
        manifest.intent_hash != GetPaymentIntentHash(intent) ||
        manifest.intent_hash != quote.intent_hash ||
        manifest.funding_model != intent.funding_model ||
        manifest.funding_model != quote.funding_model ||
        manifest.sponsorship_scope != intent.sponsorship_scope ||
        manifest.sponsorship_scope != quote.sponsorship_scope ||
        manifest.provider_dgb_inputs != dgb_inputs ||
        manifest.provider_carrier_inputs != carrier_inputs ||
        manifest.carrier_return_scripts != OptionalVector(quote.carrier_return_script) ||
        manifest.provider_fee_scripts != OptionalVector(quote.provider_fee_script) ||
        manifest.dgb_change_scripts != OptionalVector(quote.dgb_change_script) ||
        !(manifest.service_fee == quote.service_fee) ||
        !(manifest.network_fee == quote.network_fee) ||
        manifest.budget_reservation_id != GetPaymasterCommitKey(
            quote.provider_id, intent.client_nonce, quote.intent_hash,
            quote.quote_id, quote.template_commitment) ||
        !(manifest.maximum_network_fee == quote.network_fee) ||
        manifest.network_fee.value > manifest.maximum_network_fee.value ||
        manifest.expires_at != quote.retry_until || manifest.expires_at <= 0 ||
        manifest.unsigned_txid != quote.unsigned_txid ||
        manifest.template_commitment != quote.template_commitment ||
        quote.template_commitment !=
            GetCollaborativeTemplateCommitment(intent, quote, trusted_template)) {
        error = "PAYMASTER_PROVIDER_AUTH_BINDING_MISMATCH";
        return false;
    }
    return ValidateRoleLayout(intent, quote, trusted_template, error);
}

bool ValidateProviderAuthorizationForExecution(
    const ProviderAttempt& attempt,
    int64_t now,
    CollaborativePSBTTemplate& trusted_template,
    std::string& error)
{
    error.clear();
    trusted_template = {};
    if (now <= 0 || attempt.created_at <= 0 || now < attempt.created_at ||
        attempt.quote_expires_at <= 0 ||
        attempt.retry_until < attempt.quote_expires_at ||
        now > attempt.retry_until) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_EXPIRED";
        return false;
    }
    if (attempt.state != AttemptState::USER_PSBT_ACCEPTED) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_STATE";
        return false;
    }
    PaymentIntent intent;
    PaymasterQuote quote;
    if (!DecodeProviderAuthorizationArtifacts(
            attempt, intent, quote, trusted_template,
            /*allow_legacy_protocol=*/false, error) ||
        !ValidateProviderManifestForExecution(
            attempt, intent, quote, trusted_template,
            /*allow_legacy_commit=*/false, error)) {
        return false;
    }
    return true;
}

bool ValidateProviderCommitForExecution(
    const ProviderAttempt& attempt,
    const ProviderCommitRecord& commit,
    int64_t now,
    CMutableTransaction& final_transaction,
    std::string& error)
{
    error.clear();
    final_transaction = CMutableTransaction{};
    const bool exact_persisted_provider_signature =
        attempt.state == AttemptState::PROVIDER_SIGNED &&
        !attempt.final_txid.IsNull() &&
        !attempt.final_transaction.empty() &&
        ValidatePersistedProviderSignatureEnvelope(attempt, error);
    const bool exact_durable_commit_state =
        attempt.state >= AttemptState::FINAL_COMMITTED &&
        attempt.state <= AttemptState::MEMPOOL;
    if (!exact_persisted_provider_signature && !exact_durable_commit_state) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_STATE";
        }
        return false;
    }
    if (now <= 0 || commit.version != ProviderCommitRecord::CURRENT_VERSION ||
        commit.commit_key.IsNull() || commit.provider_id.IsNull() ||
        commit.quote_id.IsNull() || commit.template_commitment.IsNull() ||
        commit.final_txid.IsNull() || commit.raw_transaction_hash.IsNull() ||
        commit.final_transaction.empty() || commit.provider_inputs.empty() ||
        commit.committed_at <= 0 || now < commit.committed_at ||
        (exact_persisted_provider_signature &&
         commit.committed_at < attempt.provider_signed_at)) {
        error = "PAYMASTER_PROVIDER_COMMIT_NOT_EXECUTABLE";
        return false;
    }

    // retry_until limits creation of a new provider signature and network
    // resubmission of a merely user-signed PSBT. It cannot revoke a complete
    // transaction that was already persisted in PROVIDER_SIGNED before that
    // boundary. Such an exact attempt may be promoted to its first atomic
    // commit after a crash; later states are accepted only by callers that
    // first loaded the byte-exact durable ProviderCommitRecord.

    PaymentIntent intent;
    PaymasterQuote quote;
    CollaborativePSBTTemplate trusted;
    if (!DecodeProviderAuthorizationArtifacts(
            attempt, intent, quote, trusted,
            /*allow_legacy_protocol=*/true, error) ||
        !ValidateProviderManifestForExecution(
            attempt, intent, quote, trusted,
            /*allow_legacy_commit=*/true, error)) {
        return false;
    }
    if (attempt.commit_key != commit.commit_key ||
        attempt.provider_id != commit.provider_id ||
        attempt.quote_id != commit.quote_id ||
        attempt.template_commitment != commit.template_commitment ||
        attempt.unsigned_txid != commit.final_txid ||
        attempt.final_txid != commit.final_txid ||
        attempt.final_transaction != commit.final_transaction ||
        attempt.retry_until != commit.retry_until ||
        Hash(commit.final_transaction) != commit.raw_transaction_hash) {
        error = "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_MISMATCH";
        return false;
    }

    CMutableTransaction decoded;
    try {
        SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
        stream >> decoded;
        if (!stream.empty()) {
            throw std::ios_base::failure("trailing provider commit transaction data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_COMMIT_TRANSACTION_ENCODING";
        return false;
    }
    const CTransaction transaction{decoded};
    if (transaction.GetHash() != commit.final_txid ||
        transaction.GetWitnessHash() != commit.raw_transaction_hash) {
        error = "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_MISMATCH";
        return false;
    }

    std::vector<COutPoint> expected_provider_inputs;
    if (decoded.vin.size() != trusted.input_roles.size()) {
        error = "PAYMASTER_PROVIDER_COMMIT_INPUT_MISMATCH";
        return false;
    }
    for (size_t index = 0; index < trusted.input_roles.size(); ++index) {
        const InputRole role{trusted.input_roles[index]};
        if (role == InputRole::PROVIDER_CARRIER || role == InputRole::PROVIDER_DGB) {
            expected_provider_inputs.push_back(decoded.vin[index].prevout);
        } else if (role != InputRole::USER_DD) {
            error = "PAYMASTER_PROVIDER_COMMIT_INPUT_ROLE";
            return false;
        }
    }
    if (expected_provider_inputs.empty() ||
        expected_provider_inputs != commit.provider_inputs) {
        error = "PAYMASTER_PROVIDER_COMMIT_INPUT_MISMATCH";
        return false;
    }
    if (!ValidateFinalCollaborativeTransaction(
            decoded, commit.final_txid, commit.raw_transaction_hash,
            trusted, error)) {
        return false;
    }
    final_transaction = std::move(decoded);
    return true;
}

bool ValidateClientFinalForExecution(
    const ProviderAttempt& attempt,
    const uint256& expected_wtxid,
    CMutableTransaction& final_transaction,
    std::string& error)
{
    error.clear();
    final_transaction = CMutableTransaction{};
    if (attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.final_txid.IsNull() || expected_wtxid.IsNull() ||
        attempt.final_transaction.empty() || attempt.user_signed_psbt.empty() ||
        attempt.client_manifest.manifest_id.IsNull() ||
        attempt.accepted_client_manifest_id !=
            attempt.client_manifest.manifest_id ||
        attempt.client_manifest_accepted_at <= 0 ||
        attempt.client_manifest_accepted_at < attempt.created_at ||
        (attempt.state != AttemptState::PROVIDER_SIGNED &&
         attempt.state != AttemptState::FINAL_COMMITTED &&
         attempt.state != AttemptState::BROADCAST &&
         attempt.state != AttemptState::STEMPOOL &&
         attempt.state != AttemptState::MEMPOOL)) {
        error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED";
        return false;
    }

    PaymentIntent intent;
    PaymasterQuote quote;
    CollaborativePSBTTemplate trusted;
    if (!DecodeProviderAuthorizationArtifacts(
            attempt, intent, quote, trusted,
            /*allow_legacy_protocol=*/false, error) ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, intent, quote,
            attempt.capacity_snapshot, trusted, error)) {
        return false;
    }

    PartiallySignedTransaction user_psbt;
    std::string decode_error;
    if (!DecodeRawPSBT(user_psbt, MakeByteSpan(attempt.user_signed_psbt),
                       decode_error) ||
        SerializeExact(user_psbt) != attempt.user_signed_psbt) {
        error = "PAYMASTER_CLIENT_FINAL_USER_PSBT_ENCODING";
        return false;
    }
    if (!ValidateCollaborativePSBT(
            user_psbt, trusted, CollaborativeSignatureStage::USER_SIGNED,
            error)) {
        return false;
    }

    CMutableTransaction decoded;
    try {
        SpanReader stream{::PROTOCOL_VERSION, attempt.final_transaction};
        stream >> decoded;
        if (!stream.empty()) {
            throw std::ios_base::failure(
                "trailing client final transaction data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CLIENT_FINAL_TRANSACTION_ENCODING";
        return false;
    }
    const CTransaction transaction{decoded};
    if (SerializeExact(transaction) != attempt.final_transaction ||
        transaction.GetHash() != attempt.final_txid ||
        transaction.GetWitnessHash() != expected_wtxid) {
        error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_MISMATCH";
        return false;
    }
    if (!ValidateFinalCollaborativeTransaction(
            decoded, attempt.final_txid, expected_wtxid, trusted, error)) {
        return false;
    }
    final_transaction = std::move(decoded);
    return true;
}

bool CreateCollaborativePSBTTemplate(
    const CMutableTransaction& transaction,
    const std::vector<CollaborativeInput>& inputs,
    CollaborativePSBTTemplate& result,
    std::string& error)
{
    error.clear();
    result = {};
    if (transaction.vin.empty() || transaction.vin.size() != inputs.size()) {
        error = "PAYMASTER_PSBT_INPUT_COUNT";
        return false;
    }

    result.psbt = PartiallySignedTransaction{transaction};
    result.input_roles.reserve(inputs.size());
    for (size_t index = 0; index < inputs.size(); ++index) {
        const CollaborativeInput& input = inputs[index];
        if (!input.creating_tx || input.outpoint.IsNull() ||
            transaction.vin[index].prevout != input.outpoint ||
            input.creating_tx->GetHash() != input.outpoint.hash ||
            input.outpoint.n >= input.creating_tx->vout.size()) {
            error = "PAYMASTER_PSBT_PREVOUT_MISMATCH";
            result = {};
            return false;
        }
        result.psbt.inputs[index].non_witness_utxo = input.creating_tx;
        result.psbt.inputs[index].sighash_type = SIGHASH_DEFAULT;
        result.input_roles.push_back(input.role);
    }
    return true;
}

bool ValidateCollaborativePSBT(
    const PartiallySignedTransaction& candidate,
    const CollaborativePSBTTemplate& trusted_template,
    CollaborativeSignatureStage expected_stage,
    std::string& error)
{
    error.clear();
    const PartiallySignedTransaction& expected = trusted_template.psbt;
    if (!candidate.tx || !expected.tx || candidate.inputs.size() != expected.inputs.size() ||
        candidate.outputs.size() != expected.outputs.size() ||
        trusted_template.input_roles.size() != expected.inputs.size()) {
        error = "PAYMASTER_PSBT_SHAPE_MISMATCH";
        return false;
    }
    if (!SameUnsignedTransaction(candidate, expected)) {
        error = "PAYMASTER_PSBT_TRANSACTION_MISMATCH";
        return false;
    }
    if (!SameGlobalMap(candidate, expected)) {
        error = "PAYMASTER_PSBT_GLOBAL_FIELDS";
        return false;
    }
    for (size_t index = 0; index < candidate.outputs.size(); ++index) {
        if (SerializeExact(candidate.outputs[index]) != SerializeExact(expected.outputs[index])) {
            error = "PAYMASTER_PSBT_OUTPUT_FIELDS";
            return false;
        }
    }

    const PrecomputedTransactionData txdata = PrecomputePSBTData(candidate);
    for (size_t index = 0; index < candidate.inputs.size(); ++index) {
        const InputRole role = trusted_template.input_roles[index];
        if (role != InputRole::USER_DD && role != InputRole::USER_DGB &&
            role != InputRole::PROVIDER_CARRIER &&
            role != InputRole::PROVIDER_DGB) {
            error = "PAYMASTER_PSBT_INPUT_ROLE";
            return false;
        }
        const PSBTInput& input = candidate.inputs[index];
        CTxOut actual_prevout;
        CTxOut expected_prevout;
        if (!candidate.GetInputUTXO(actual_prevout, index) ||
            !expected.GetInputUTXO(expected_prevout, index) || actual_prevout != expected_prevout) {
            error = "PAYMASTER_PSBT_PREVOUT_MISMATCH";
            return false;
        }

        // Finalized PSBT inputs do not serialize PSBT_IN_SIGHASH. For BIP86
        // key-path spends, a canonical 64-byte Schnorr signature itself
        // commits to SIGHASH_DEFAULT; a 65th byte would encode another mode.
        const bool implicit_final_default = !input.sighash_type &&
                                            HasFinalTaprootKeyPathDefaultSighash(input, expected_prevout);
        if ((!input.sighash_type && !implicit_final_default) ||
            (input.sighash_type && *input.sighash_type != SIGHASH_DEFAULT)) {
            error = "PAYMASTER_PSBT_SIGHASH";
            return false;
        }

        PSBTInput input_metadata{input};
        PSBTInput expected_metadata{expected.inputs[index]};
        ClearSignatures(input_metadata);
        ClearSignatures(expected_metadata);
        if (implicit_final_default) input_metadata.sighash_type = SIGHASH_DEFAULT;
        if (SerializeExact(input_metadata) != SerializeExact(expected_metadata)) {
            error = "PAYMASTER_PSBT_INPUT_FIELDS";
            return false;
        }

        const bool must_be_signed = RoleMustBeSigned(trusted_template.input_roles[index], expected_stage);
        if (!must_be_signed) {
            PSBTInput unsigned_input{input};
            ClearSignatures(unsigned_input);
            if (SerializeExact(unsigned_input) != SerializeExact(input)) {
                error = "PAYMASTER_PSBT_UNEXPECTED_SIGNATURE";
                return false;
            }
            continue;
        }
        if (HasNonFinalSignatures(input) || !PSBTInputSigned(input) ||
            !PSBTInputSignedAndVerified(candidate, index, &txdata)) {
            error = "PAYMASTER_PSBT_SIGNATURE_INVALID";
            return false;
        }
    }
    return true;
}

bool ValidateFinalCollaborativeTransaction(
    const CMutableTransaction& final_transaction,
    const uint256& expected_txid,
    const uint256& expected_wtxid,
    const CollaborativePSBTTemplate& trusted_template,
    std::string& error)
{
    error.clear();
    if (!trusted_template.psbt.tx ||
        final_transaction.vin.size() != trusted_template.psbt.inputs.size()) {
        error = "PAYMASTER_FINAL_TRANSACTION_SHAPE";
        return false;
    }

    const CTransaction final{final_transaction};
    if (final.GetHash() != expected_txid ||
        CTransaction{*trusted_template.psbt.tx}.GetHash() != expected_txid) {
        error = "PAYMASTER_FINAL_TXID_MISMATCH";
        return false;
    }
    if (final.GetWitnessHash() != expected_wtxid) {
        error = "PAYMASTER_FINAL_WTXID_MISMATCH";
        return false;
    }

    CMutableTransaction non_witness{final_transaction};
    for (CTxIn& input : non_witness.vin) {
        input.scriptWitness.SetNull();
    }
    if (SerializeExact(non_witness) != SerializeExact(*trusted_template.psbt.tx)) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISMATCH";
        return false;
    }

    PartiallySignedTransaction reconstructed{trusted_template.psbt};
    for (size_t index = 0; index < reconstructed.inputs.size(); ++index) {
        reconstructed.inputs[index].final_script_sig = final_transaction.vin[index].scriptSig;
        reconstructed.inputs[index].final_script_witness = final_transaction.vin[index].scriptWitness;
    }
    if (!ValidateCollaborativePSBT(reconstructed, trusted_template,
                                   CollaborativeSignatureStage::FULLY_SIGNED, error)) {
        return false;
    }

    CMutableTransaction extracted;
    if (!FinalizeAndExtractPSBT(reconstructed, extracted) ||
        SerializeExact(extracted) != SerializeExact(final_transaction)) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISMATCH";
        return false;
    }
    return true;
}

} // namespace DigiDollar::Paymaster
