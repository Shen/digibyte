// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Alternative-provider recovery for an ambiguous authorized payment.
 * Recovery may reuse the client's original DD inputs only to return value to
 * fresh wallet-owned scripts, plus a locally capped fee; it cannot mutate or
 * cancel an already valid original transaction on chain.
 */

#include <paymaster/recovery.h>

#include <hash.h>
#include <kernel/chainparams.h>
#include <script/interpreter.h>
#include <streams.h>
#include <util/overflow.h>
#include <version.h>

#include <ios>
#include <map>
#include <set>

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

bool IsCanonicalP2TR(const CScript& script)
{
    int witness_version{-1};
    std::vector<unsigned char> witness_program;
    return script.IsWitnessProgram(witness_version, witness_program) &&
           witness_version == 1 &&
           witness_program.size() == WITNESS_V1_TAPROOT_SIZE;
}

bool SameTemplate(const CollaborativePSBTTemplate& lhs,
                  const CollaborativePSBTTemplate& rhs)
{
    return lhs.input_roles == rhs.input_roles &&
           SerializeExact(lhs.psbt) == SerializeExact(rhs.psbt);
}

bool SameManifest(const AlternativeRecoveryManifest& lhs,
                  const AlternativeRecoveryManifest& rhs)
{
    return SerializeExact(lhs) == SerializeExact(rhs);
}

bool DecodeCanonicalRecoveryPSBT(const std::vector<unsigned char>& encoded,
                                 PartiallySignedTransaction& psbt,
                                 const char* error_code,
                                 std::string& error)
{
    psbt = {};
    std::string decode_error;
    if (!DecodeRawPSBT(psbt, MakeByteSpan(encoded), decode_error) ||
        SerializeExact(psbt) != encoded) {
        error = error_code;
        psbt = {};
        return false;
    }
    return true;
}

bool DecodeCapacitySnapshot(const AlternativeRecoveryParameters& parameters,
                            int64_t now,
                            PaymasterCapacityProof& proof,
                            std::string& error)
{
    const ValidatedCapacitySnapshot& snapshot = parameters.capacity_snapshot;
    if (snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
        snapshot.snapshot_id.IsNull() || snapshot.resource_commitment.IsNull() ||
        snapshot.session_id != parameters.session_id ||
        snapshot.provider_id != parameters.recovery_provider_id ||
        snapshot.attempt_id.IsNull() || snapshot.client_nonce.IsNull() ||
        snapshot.request_hash.IsNull() || snapshot.capacity_proof.empty() ||
        snapshot.funding_model != FundingModel::USER_PAID ||
        snapshot.requires_carrier !=
            (parameters.service_fee.value > 0 &&
             parameters.service_fee.value < 100) ||
        snapshot.created_at <= 0 || snapshot.validated_at < snapshot.created_at ||
        snapshot.validated_at > now || snapshot.expires_at <= now) {
        error = "PAYMASTER_RECOVERY_INVALID_CAPACITY_SNAPSHOT";
        return false;
    }

    try {
        CDataStream stream{snapshot.capacity_proof, SER_NETWORK,
                           ::PROTOCOL_VERSION};
        stream >> proof;
        if (!stream.empty() || SerializeExact(proof) != snapshot.capacity_proof) {
            throw std::ios_base::failure("non-canonical capacity proof");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_RECOVERY_INVALID_CAPACITY_SNAPSHOT";
        return false;
    }

    const std::vector<unsigned char> request_bytes{
        SerializeExact(parameters.capacity_request)};
    if (snapshot.snapshot_id != proof.snapshot_id ||
        snapshot.resource_commitment != GetCapacityResourceCommitment(proof) ||
        snapshot.client_nonce != proof.client_nonce ||
        snapshot.funding_model != proof.funding_model ||
        snapshot.requires_carrier != proof.requires_carrier ||
        snapshot.request_hash != Hash(request_bytes) ||
        snapshot.created_at != proof.created_at ||
        snapshot.expires_at != proof.expires_at) {
        error = "PAYMASTER_RECOVERY_CAPACITY_BINDING_MISMATCH";
        return false;
    }
    return true;
}

bool ValidateRecoveryBindings(const AlternativeRecoveryParameters& parameters,
                              const CapacityChainstateCallbacks& chainstate,
                              int64_t now,
                              PaymasterCapacityProof& proof,
                              std::string& error)
{
    if (now <= 0 || parameters.genesis_hash.IsNull() ||
        !IsCanonicalRequestId(parameters.request_id) ||
        parameters.session_id.IsNull() || parameters.original_provider_id.IsNull() ||
        parameters.recovery_provider_id.IsNull() ||
        parameters.original_provider_id == parameters.recovery_provider_id ||
        (parameters.privacy_profile != PrivacyProfile::STANDARD &&
         parameters.privacy_profile != PrivacyProfile::HIGH) ||
        parameters.offer_id.IsNull() || parameters.policy_hash.IsNull() ||
        parameters.original_commit_key.IsNull() ||
        parameters.original_template_commitment.IsNull()) {
        error = parameters.original_provider_id == parameters.recovery_provider_id ? "PAYMASTER_RECOVERY_PROVIDER_MUST_DIFFER" : "PAYMASTER_RECOVERY_INVALID_BINDING";
        return false;
    }
    if (parameters.capacity_request.genesis_hash != parameters.genesis_hash ||
        parameters.capacity_request.provider_id != parameters.recovery_provider_id ||
        parameters.capacity_request.request_id != parameters.request_id ||
        parameters.capacity_request.session_id != parameters.session_id ||
        parameters.capacity_request.funding_model != FundingModel::USER_PAID ||
        parameters.capacity_request.requires_carrier !=
            (parameters.service_fee.value > 0 &&
             parameters.service_fee.value < 100) ||
        parameters.expires_at <= now ||
        parameters.expires_at > parameters.capacity_request.expires_at ||
        TimeDeltaExceeds(parameters.expires_at, now,
                         MAX_DIRECT_MESSAGE_TTL_SECONDS)) {
        error = "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH";
        return false;
    }
    if (!DecodeCapacitySnapshot(parameters, now, proof, error)) return false;
    if (parameters.expires_at > proof.expires_at) {
        error = "PAYMASTER_RECOVERY_CAPACITY_EXPIRED";
        return false;
    }
    if (!ValidateCapacityProof(
            proof, parameters.capacity_request, parameters.genesis_hash,
            parameters.recovery_provider_identity_key, chainstate, now, error)) {
        return false;
    }
    return true;
}

bool ValidateUserReturns(const AlternativeRecoveryParameters& parameters,
                         std::string& error)
{
    if (parameters.persisted_user_dd_inputs.empty() ||
        parameters.persisted_user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        parameters.user_dd_inputs.size() !=
            parameters.persisted_user_dd_inputs.size()) {
        error = "PAYMASTER_RECOVERY_USER_INPUT_MISMATCH";
        return false;
    }
    std::set<COutPoint> unique_inputs;
    for (size_t index = 0; index < parameters.user_dd_inputs.size(); ++index) {
        const CollaborativeInput& input = parameters.user_dd_inputs[index];
        if (input.role != InputRole::USER_DD || input.outpoint.IsNull() ||
            input.outpoint != parameters.persisted_user_dd_inputs[index] ||
            !unique_inputs.insert(input.outpoint).second) {
            error = "PAYMASTER_RECOVERY_USER_INPUT_MISMATCH";
            return false;
        }
    }

    if (parameters.wallet_returns.empty() ||
        parameters.wallet_returns.size() > MAX_PAYMENT_INTENT_INPUTS ||
        parameters.wallet_returns.size() !=
            parameters.wallet_verified_fresh_return_scripts.size()) {
        error = "PAYMASTER_RECOVERY_RETURN_SCRIPT_MISMATCH";
        return false;
    }
    std::set<CScript> unique_scripts;
    for (size_t index = 0; index < parameters.wallet_returns.size(); ++index) {
        const AlternativeRecoveryReturn& output = parameters.wallet_returns[index];
        if (output.script_pub_key !=
                parameters.wallet_verified_fresh_return_scripts[index] ||
            !IsCanonicalP2TR(output.script_pub_key) ||
            output.amount.value < 100 || output.amount.value > MAX_DD_OUTPUT_CENTS ||
            !unique_scripts.insert(output.script_pub_key).second) {
            error = "PAYMASTER_RECOVERY_RETURN_SCRIPT_MISMATCH";
            return false;
        }
    }
    return true;
}

bool BuildExpectedRecovery(const AlternativeRecoveryParameters& parameters,
                           const CChainParams& chain_params,
                           const CCoinsViewCache& coins,
                           const CapacityChainstateCallbacks& chainstate,
                           int64_t now,
                           AlternativeRecoveryTemplate& result,
                           std::string& error)
{
    result = {};
    PaymasterCapacityProof capacity_proof;
    if (!ValidateRecoveryBindings(
            parameters, chainstate, now, capacity_proof, error) ||
        !ValidateUserReturns(parameters, error)) {
        return false;
    }
    if (parameters.maximum_service_fee.value < 0 ||
        parameters.maximum_service_fee.value > MAX_DD_OUTPUT_CENTS ||
        parameters.service_fee.value < 0 ||
        parameters.service_fee.value > parameters.maximum_service_fee.value) {
        error = "PAYMASTER_RECOVERY_SERVICE_FEE_LIMIT";
        return false;
    }
    if (parameters.network_fee.value <= 0 || parameters.fee_rate <= 0) {
        error = "PAYMASTER_RECOVERY_INVALID_NETWORK_FEE";
        return false;
    }

    const PaymasterLiquiditySlot& slot = capacity_proof.liquidity_slots.front();
    const bool needs_provider_dd_script{
        slot.carrier.has_value() || parameters.service_fee.value > 0};
    if (needs_provider_dd_script !=
            parameters.recovery_provider_dd_script.has_value() ||
        (parameters.recovery_provider_dd_script &&
         !IsCanonicalP2TR(*parameters.recovery_provider_dd_script))) {
        error = "PAYMASTER_RECOVERY_PROVIDER_DD_SCRIPT";
        return false;
    }
    if (parameters.service_fee.value > 0 &&
        parameters.service_fee.value < 100 && !slot.carrier) {
        error = "PAYMASTER_RECOVERY_CARRIER_REQUIRED";
        return false;
    }

    CollaborativeTransferParams transfer;
    transfer.inputs = parameters.user_dd_inputs;
    for (const AlternativeRecoveryReturn& output : parameters.wallet_returns) {
        transfer.dd_outputs.push_back(
            {output.script_pub_key, output.amount.value,
             transfer.dd_outputs.empty() ? DDOutputRole::RECIPIENT : DDOutputRole::USER_CHANGE});
    }

    if (slot.carrier) {
        const VerifiedDDCarrier& carrier = slot.carrier->carrier;
        transfer.inputs.push_back(
            {carrier.outpoint,
             MakeTransactionRef(CMutableTransaction{carrier.creating_tx}),
             InputRole::PROVIDER_CARRIER});
        int64_t successor_amount{carrier.value.value};
        if (parameters.service_fee.value > 0 &&
            parameters.service_fee.value < 100) {
            const auto sum = CheckedAdd(successor_amount,
                                        parameters.service_fee.value);
            if (!sum || *sum > MAX_DD_OUTPUT_CENTS) {
                error = "PAYMASTER_RECOVERY_INVALID_CARRIER_SUCCESSOR";
                return false;
            }
            successor_amount = *sum;
        }
        transfer.dd_outputs.push_back(
            {*parameters.recovery_provider_dd_script, successor_amount,
             DDOutputRole::CARRIER_SUCCESSOR});
    }
    if (parameters.service_fee.value >= 100) {
        transfer.dd_outputs.push_back(
            {*parameters.recovery_provider_dd_script,
             parameters.service_fee.value, DDOutputRole::PROVIDER_FEE});
    }

    int64_t total_dgb{0};
    for (const CapacityDGBInput& capacity_input : slot.dgb_inputs) {
        const VerifiedDGBInput& input = capacity_input.input;
        const auto sum = CheckedAdd(total_dgb, input.value.value);
        if (!sum) {
            error = "PAYMASTER_RECOVERY_DGB_AMOUNT_OVERFLOW";
            return false;
        }
        total_dgb = *sum;
        transfer.inputs.push_back(
            {input.outpoint,
             MakeTransactionRef(CMutableTransaction{input.creating_tx}),
             InputRole::PROVIDER_DGB});
    }
    if (parameters.network_fee.value > total_dgb) {
        error = "PAYMASTER_RECOVERY_DGB_FUNDING";
        return false;
    }
    const int64_t dgb_change{total_dgb - parameters.network_fee.value};
    if ((dgb_change > 0) !=
            parameters.recovery_provider_dgb_change_script.has_value() ||
        (parameters.recovery_provider_dgb_change_script &&
         !IsCanonicalP2TR(*parameters.recovery_provider_dgb_change_script))) {
        error = "PAYMASTER_RECOVERY_PROVIDER_DGB_CHANGE";
        return false;
    }
    if (dgb_change > 0) {
        transfer.dgb_outputs.emplace_back(
            dgb_change, *parameters.recovery_provider_dgb_change_script);
    }
    transfer.fee_rate = parameters.fee_rate;

    const CollaborativeTransferResult built =
        BuildUnsignedCollaborativeTransfer(transfer, chain_params, coins);
    if (!built.success) {
        error = built.error;
        return false;
    }
    if (built.miner_fee != parameters.network_fee.value) {
        error = "PAYMASTER_RECOVERY_NETWORK_FEE_MISMATCH";
        return false;
    }
    if (!CreateCollaborativePSBTTemplate(
            built.tx, transfer.inputs, result.trusted_template, error)) {
        result = {};
        return false;
    }

    AlternativeRecoveryManifest& manifest = result.manifest;
    manifest.request_id = parameters.request_id;
    manifest.session_id = parameters.session_id;
    manifest.original_provider_id = parameters.original_provider_id;
    manifest.recovery_provider_id = parameters.recovery_provider_id;
    manifest.privacy_profile = parameters.privacy_profile;
    manifest.offer_id = parameters.offer_id;
    manifest.policy_hash = parameters.policy_hash;
    manifest.original_commit_key = parameters.original_commit_key;
    manifest.original_template_commitment =
        parameters.original_template_commitment;
    manifest.capacity_snapshot_id = parameters.capacity_snapshot.snapshot_id;
    manifest.capacity_resource_commitment =
        parameters.capacity_snapshot.resource_commitment;
    manifest.user_dd_inputs = parameters.persisted_user_dd_inputs;
    manifest.wallet_returns = parameters.wallet_returns;
    if (slot.carrier) {
        manifest.recovery_provider_carrier_inputs.push_back(
            slot.carrier->carrier.outpoint);
    }
    for (const CapacityDGBInput& input : slot.dgb_inputs) {
        manifest.recovery_provider_dgb_inputs.push_back(input.input.outpoint);
    }
    if (parameters.recovery_provider_dd_script) {
        manifest.recovery_provider_dd_scripts.push_back(
            *parameters.recovery_provider_dd_script);
    }
    if (parameters.recovery_provider_dgb_change_script) {
        manifest.recovery_provider_dgb_change_scripts.push_back(
            *parameters.recovery_provider_dgb_change_script);
    }
    manifest.maximum_service_fee = parameters.maximum_service_fee;
    manifest.service_fee = parameters.service_fee;
    manifest.network_fee = parameters.network_fee;
    manifest.expires_at = parameters.expires_at;
    manifest.unsigned_txid = CTransaction{built.tx}.GetHash();
    manifest.template_commitment =
        GetAlternativeRecoveryTemplateCommitment(
            manifest, result.trusted_template);
    manifest.manifest_id = GetAlternativeRecoveryManifestId(manifest);
    return true;
}

} // namespace

uint256 GetAlternativeRecoveryRequestHash(
    const AlternativeRecoveryRequest& request)
{
    AlternativeRecoveryRequest projection{request};
    projection.user_input_proofs.clear();
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Request v2");
    hasher << projection;
    return hasher.GetSHA256();
}

uint256 GetAlternativeRecoveryId(
    const std::string& request_id,
    const uint256& session_id,
    const PaymasterId& recovery_provider_id,
    const uint256& client_nonce)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Identity v2");
    hasher << request_id << session_id << recovery_provider_id << client_nonce;
    return hasher.GetSHA256();
}

uint256 GetAlternativeRecoveryInputControlHash(
    const AlternativeRecoveryRequest& request,
    const COutPoint& outpoint)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Input v2");
    hasher << GetAlternativeRecoveryRequestHash(request) << outpoint;
    return hasher.GetSHA256();
}

uint256 GetAlternativeRecoveryResponseSignatureHash(
    const AlternativeRecoveryResponse& response)
{
    AlternativeRecoveryResponse projection{response};
    projection.identity_signature.clear();
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Response v2");
    hasher << projection;
    return hasher.GetSHA256();
}

uint256 GetAlternativeRecoveryCommitKey(
    const AlternativeRecoveryResponse& response)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Commit v2");
    hasher << response.recovery_provider_id << response.recovery_request_hash
           << response.manifest.manifest_id
           << response.manifest.template_commitment;
    return hasher.GetSHA256();
}

bool ValidateAlternativeRecoveryRequestEnvelope(
    const AlternativeRecoveryRequest& request,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error)
{
    if (request.version != AlternativeRecoveryRequest::CURRENT_VERSION ||
        expected_genesis.IsNull() || request.genesis_hash != expected_genesis ||
        !IsCanonicalRequestId(request.request_id) || request.session_id.IsNull() ||
        request.original_provider_id.IsNull() ||
        request.recovery_provider_id.IsNull() ||
        request.original_provider_id == request.recovery_provider_id ||
        (request.privacy_profile != PrivacyProfile::STANDARD &&
         request.privacy_profile != PrivacyProfile::HIGH) ||
        request.offer_id.IsNull() || request.policy_hash.IsNull() ||
        request.original_commit_key.IsNull() ||
        request.original_template_commitment.IsNull() ||
        request.client_nonce.IsNull() || request.capacity_snapshot_id.IsNull() ||
        request.capacity_resource_commitment.IsNull()) {
        error = request.original_provider_id == request.recovery_provider_id
                    ? "PAYMASTER_RECOVERY_PROVIDER_MUST_DIFFER"
                    : "PAYMASTER_INVALID_RECOVERY_REQUEST_BINDING";
        return false;
    }
    if (!ValidateCapacityRequestEnvelope(
            request.capacity_request, request.genesis_hash, now, error)) {
        return false;
    }
    if (request.capacity_request.genesis_hash != request.genesis_hash ||
        request.capacity_request.provider_id != request.recovery_provider_id ||
        request.capacity_request.request_id != request.request_id ||
        request.capacity_request.session_id != request.session_id ||
        request.capacity_request.client_nonce != request.client_nonce ||
        request.capacity_request.funding_model != FundingModel::USER_PAID ||
        request.capacity_request.requires_carrier !=
            (request.service_fee.value > 0 && request.service_fee.value < 100) ||
        request.capacity_request.expires_at < request.expires_at) {
        error = "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH";
        return false;
    }
    if (now <= 0 || request.created_at <= 0 || request.created_at > now ||
        request.expires_at <= now || request.expires_at <= request.created_at ||
        TimeDeltaExceeds(request.expires_at, request.created_at,
                         MAX_DIRECT_MESSAGE_TTL_SECONDS)) {
        error = "PAYMASTER_RECOVERY_REQUEST_EXPIRED";
        return false;
    }
    if (request.user_dd_inputs.empty() ||
        request.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        request.user_input_proofs.size() != request.user_dd_inputs.size() ||
        request.wallet_returns.empty() ||
        request.wallet_returns.size() > MAX_PAYMENT_INTENT_INPUTS ||
        request.maximum_service_fee.value < 0 ||
        request.maximum_service_fee.value > MAX_DD_OUTPUT_CENTS ||
        request.service_fee.value < 0 ||
        request.service_fee.value > request.maximum_service_fee.value) {
        error = "PAYMASTER_INVALID_RECOVERY_REQUEST_SHAPE";
        return false;
    }

    std::set<COutPoint> unique_inputs;
    for (size_t index = 0; index < request.user_dd_inputs.size(); ++index) {
        const COutPoint& outpoint = request.user_dd_inputs[index];
        const AlternativeRecoveryInputProof& proof =
            request.user_input_proofs[index];
        if (outpoint.IsNull() || !unique_inputs.insert(outpoint).second ||
            proof.outpoint != outpoint || proof.signature.size() != 64) {
            error = "PAYMASTER_INVALID_RECOVERY_INPUT_PROOF";
            return false;
        }
    }
    std::set<CScript> unique_scripts;
    for (const AlternativeRecoveryReturn& output : request.wallet_returns) {
        if (!IsCanonicalP2TR(output.script_pub_key) || output.amount.value < 100 ||
            output.amount.value > MAX_DD_OUTPUT_CENTS ||
            !unique_scripts.insert(output.script_pub_key).second) {
            error = "PAYMASTER_RECOVERY_RETURN_SCRIPT_MISMATCH";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ValidateAlternativeRecoveryRequestInputs(
    const AlternativeRecoveryRequest& request,
    const std::vector<XOnlyPubKey>& user_output_keys,
    std::string& error)
{
    if (user_output_keys.size() != request.user_dd_inputs.size() ||
        request.user_input_proofs.size() != request.user_dd_inputs.size()) {
        error = "PAYMASTER_RECOVERY_USER_INPUT_MISMATCH";
        return false;
    }
    for (size_t index = 0; index < user_output_keys.size(); ++index) {
        if (!user_output_keys[index].IsFullyValid() ||
            request.user_input_proofs[index].outpoint !=
                request.user_dd_inputs[index] ||
            !user_output_keys[index].VerifySchnorr(
                GetAlternativeRecoveryInputControlHash(
                    request, request.user_dd_inputs[index]),
                request.user_input_proofs[index].signature)) {
            error = "PAYMASTER_INVALID_RECOVERY_INPUT_PROOF";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ValidateAlternativeRecoveryResponseEnvelope(
    const AlternativeRecoveryResponse& response,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error)
{
    if (response.version != AlternativeRecoveryResponse::CURRENT_VERSION ||
        expected_genesis.IsNull() || response.genesis_hash != expected_genesis ||
        !IsCanonicalRequestId(response.request_id) ||
        response.session_id.IsNull() || response.recovery_id.IsNull() ||
        response.recovery_provider_id.IsNull() ||
        response.recovery_request_hash.IsNull() ||
        response.recovery_commit_key.IsNull() ||
        response.manifest.version != AlternativeRecoveryManifest::CURRENT_VERSION ||
        response.manifest.manifest_id.IsNull() ||
        response.manifest.offer_id.IsNull() ||
        response.manifest.policy_hash.IsNull() ||
        response.manifest.template_commitment.IsNull() ||
        response.unsigned_psbt.empty() ||
        response.unsigned_psbt.size() > MAX_DIRECT_PSBT_BYTES ||
        response.identity_signature.size() != 64) {
        error = "PAYMASTER_INVALID_RECOVERY_RESPONSE_SHAPE";
        return false;
    }
    if (now <= 0 || response.created_at <= 0 || response.created_at > now ||
        response.expires_at <= now || response.expires_at <= response.created_at ||
        TimeDeltaExceeds(response.expires_at, response.created_at,
                         MAX_DIRECT_MESSAGE_TTL_SECONDS) ||
        response.manifest.expires_at != response.expires_at ||
        response.manifest.manifest_id !=
            GetAlternativeRecoveryManifestId(response.manifest) ||
        response.recovery_commit_key != GetAlternativeRecoveryCommitKey(response)) {
        error = "PAYMASTER_INVALID_RECOVERY_RESPONSE_BINDING";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateAlternativeRecoveryResponse(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    int64_t now,
    std::string& error)
{
    if (!ValidateAlternativeRecoveryRequestEnvelope(
            request, response.genesis_hash, now, error) ||
        !ValidateAlternativeRecoveryResponseEnvelope(
            response, request.genesis_hash, now, error)) {
        return false;
    }
    const AlternativeRecoveryManifest& manifest = response.manifest;
    if (!provider_identity_key.IsFullyValid() ||
        response.request_id != request.request_id ||
        response.session_id != request.session_id ||
        response.recovery_id != GetAlternativeRecoveryId(
            request.request_id, request.session_id,
            request.recovery_provider_id, request.client_nonce) ||
        response.recovery_provider_id != request.recovery_provider_id ||
        response.recovery_request_hash !=
            GetAlternativeRecoveryRequestHash(request) ||
        manifest.request_id != request.request_id ||
        manifest.session_id != request.session_id ||
        manifest.original_provider_id != request.original_provider_id ||
        manifest.recovery_provider_id != request.recovery_provider_id ||
        manifest.privacy_profile != request.privacy_profile ||
        manifest.offer_id != request.offer_id ||
        manifest.policy_hash != request.policy_hash ||
        manifest.original_commit_key != request.original_commit_key ||
        manifest.original_template_commitment !=
            request.original_template_commitment ||
        manifest.capacity_snapshot_id != request.capacity_snapshot_id ||
        manifest.capacity_resource_commitment !=
            request.capacity_resource_commitment ||
        manifest.user_dd_inputs != request.user_dd_inputs ||
        SerializeExact(manifest.wallet_returns) !=
            SerializeExact(request.wallet_returns) ||
        !(manifest.maximum_service_fee == request.maximum_service_fee) ||
        !(manifest.service_fee == request.service_fee) ||
        response.expires_at > request.expires_at ||
        !provider_identity_key.VerifySchnorr(
            GetAlternativeRecoveryResponseSignatureHash(response),
            response.identity_signature)) {
        error = "PAYMASTER_RECOVERY_RESPONSE_MISMATCH";
        return false;
    }
    error.clear();
    return true;
}

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
    std::string& error)
{
    trusted = {};
    unsigned_psbt = {};
    if (!ValidateAlternativeRecoveryResponse(
            response, request, provider_identity_key, now, error)) {
        return false;
    }

    if (parameters.genesis_hash != request.genesis_hash ||
        parameters.request_id != request.request_id ||
        parameters.session_id != request.session_id ||
        parameters.original_provider_id != request.original_provider_id ||
        parameters.recovery_provider_id != request.recovery_provider_id ||
        parameters.privacy_profile != request.privacy_profile ||
        parameters.offer_id != request.offer_id ||
        parameters.policy_hash != request.policy_hash ||
        parameters.original_commit_key != request.original_commit_key ||
        parameters.original_template_commitment !=
            request.original_template_commitment ||
        SerializeExact(parameters.capacity_request) !=
            SerializeExact(request.capacity_request) ||
        parameters.capacity_snapshot.snapshot_id !=
            request.capacity_snapshot_id ||
        parameters.capacity_snapshot.resource_commitment !=
            request.capacity_resource_commitment ||
        parameters.persisted_user_dd_inputs != request.user_dd_inputs ||
        SerializeExact(parameters.wallet_returns) !=
            SerializeExact(request.wallet_returns) ||
        !(parameters.maximum_service_fee == request.maximum_service_fee) ||
        !(parameters.service_fee == request.service_fee) ||
        parameters.expires_at != response.expires_at) {
        error = "PAYMASTER_RECOVERY_LOCAL_AUTHORITY_MISMATCH";
        return false;
    }

    AlternativeRecoveryTemplate rebuilt;
    if (!BuildExpectedRecovery(parameters, chain_params, coins, chainstate, now,
                               rebuilt, error)) {
        return false;
    }
    if (!SameManifest(response.manifest, rebuilt.manifest)) {
        error = "PAYMASTER_RECOVERY_TEMPLATE_MISMATCH";
        return false;
    }

    PartiallySignedTransaction decoded;
    if (!DecodeCanonicalRecoveryPSBT(
            response.unsigned_psbt, decoded,
            "PAYMASTER_RECOVERY_RESPONSE_PSBT_NONCANONICAL", error) ||
        !ValidateCollaborativePSBT(
            decoded, rebuilt.trusted_template,
            CollaborativeSignatureStage::UNSIGNED, error)) {
        return false;
    }
    trusted = std::move(rebuilt);
    unsigned_psbt = std::move(decoded);
    error.clear();
    return true;
}

bool ValidateAlternativeRecoverySubmitEnvelope(
    const AlternativeRecoverySubmit& submit,
    const uint256& expected_genesis,
    std::string& error)
{
    if (submit.version != AlternativeRecoverySubmit::CURRENT_VERSION ||
        expected_genesis.IsNull() || submit.genesis_hash != expected_genesis ||
        !IsCanonicalRequestId(submit.request_id) || submit.session_id.IsNull() ||
        submit.recovery_id.IsNull() ||
        submit.recovery_provider_id.IsNull() ||
        submit.recovery_request_hash.IsNull() ||
        submit.recovery_commit_key.IsNull() ||
        submit.template_commitment.IsNull() || submit.user_psbt.empty() ||
        submit.user_psbt.size() > MAX_DIRECT_PSBT_BYTES) {
        error = "PAYMASTER_INVALID_RECOVERY_SUBMIT";
        return false;
    }
    error.clear();
    return true;
}

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
    std::string& error)
{
    user_psbt = {};
    if (!ValidateAlternativeRecoverySubmitEnvelope(
            submit, parameters.genesis_hash, error) ||
        !ValidateAlternativeRecoveryResponseEnvelope(
            response, parameters.genesis_hash, now, error)) {
        return false;
    }
    if (submit.request_id != response.request_id ||
        submit.session_id != response.session_id ||
        submit.recovery_id != response.recovery_id ||
        submit.recovery_provider_id != response.recovery_provider_id ||
        submit.recovery_request_hash != response.recovery_request_hash ||
        submit.recovery_commit_key != response.recovery_commit_key ||
        submit.template_commitment !=
            response.manifest.template_commitment ||
        !SameManifest(response.manifest, trusted.manifest)) {
        error = "PAYMASTER_RECOVERY_SUBMIT_MISMATCH";
        return false;
    }

    PartiallySignedTransaction decoded;
    if (!DecodeCanonicalRecoveryPSBT(
            submit.user_psbt, decoded,
            "PAYMASTER_RECOVERY_SUBMIT_PSBT_NONCANONICAL", error) ||
        !ValidateAlternativeRecoveryPSBT(
            decoded, trusted, parameters, chain_params, coins, chainstate, now,
            CollaborativeSignatureStage::USER_SIGNED, error)) {
        return false;
    }
    user_psbt = std::move(decoded);
    error.clear();
    return true;
}

bool ValidateAlternativeRecoveryResultEnvelope(
    const AlternativeRecoveryResultMessage& message,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error)
{
    if (message.version != AlternativeRecoveryResultMessage::CURRENT_VERSION ||
        !IsCanonicalRequestId(message.request_id) || message.session_id.IsNull() ||
        message.recovery_id.IsNull() ||
        message.recovery_request_hash.IsNull() ||
        message.result.genesis_hash != expected_genesis || now <= 0 ||
        message.result.updated_at > now ||
        !ValidatePaymasterResultShape(message.result, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_RECOVERY_RESULT";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateAlternativeRecoveryResult(
    const AlternativeRecoveryResultMessage& message,
    const AlternativeRecoveryResponse& response,
    const XOnlyPubKey& provider_identity_key,
    const uint256& expected_genesis,
    uint64_t minimum_sequence,
    int64_t now,
    std::string& error)
{
    if (!ValidateAlternativeRecoveryResultEnvelope(
            message, expected_genesis, now, error)) {
        return false;
    }
    if (message.request_id != response.request_id ||
        message.session_id != response.session_id ||
        message.recovery_id != response.recovery_id ||
        message.recovery_request_hash != response.recovery_request_hash ||
        message.result.updated_at < response.created_at) {
        error = "PAYMASTER_RECOVERY_RESULT_MISMATCH";
        return false;
    }
    return ValidatePaymasterResult(
        message.result, expected_genesis, response.recovery_provider_id,
        response.recovery_commit_key, provider_identity_key, minimum_sequence,
        error);
}

uint256 GetAlternativeRecoveryTemplateCommitment(
    const AlternativeRecoveryManifest& manifest,
    const CollaborativePSBTTemplate& trusted_template)
{
    AlternativeRecoveryManifest projection{manifest};
    projection.manifest_id.SetNull();
    projection.template_commitment.SetNull();
    std::vector<uint8_t> roles;
    roles.reserve(trusted_template.input_roles.size());
    for (const InputRole role : trusted_template.input_roles) {
        roles.push_back(static_cast<uint8_t>(role));
    }
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Template v2");
    hasher << projection << SerializeExact(trusted_template.psbt) << roles;
    return hasher.GetSHA256();
}

uint256 GetAlternativeRecoveryManifestId(
    const AlternativeRecoveryManifest& manifest)
{
    AlternativeRecoveryManifest projection{manifest};
    projection.manifest_id.SetNull();
    HashWriter hasher = TaggedHash("DigiByte Paymaster Recovery Authority v2");
    hasher << projection;
    return hasher.GetSHA256();
}

uint256 GetRecoveryAuthorizationCommitment(
    const RecoveryAuthorizationManifest& manifest)
{
    RecoveryAuthorizationManifest projection{manifest};
    projection.authorization_commitment.SetNull();
    HashWriter hasher =
        TaggedHash("DigiByte Paymaster Recovery Authorization v1");
    hasher << projection;
    return hasher.GetSHA256();
}

bool BuildRecoveryAuthorizationManifest(
    const AlternativeRecoveryRequest& request,
    const AlternativeRecoveryResponse& response,
    RecoveryAuthorizationManifest& authorization,
    std::string& error)
{
    authorization = {};
    const AlternativeRecoveryManifest& manifest = response.manifest;
    if (request.version != AlternativeRecoveryRequest::CURRENT_VERSION ||
        response.version != AlternativeRecoveryResponse::CURRENT_VERSION ||
        response.request_id != request.request_id ||
        response.session_id != request.session_id ||
        response.recovery_id.IsNull() ||
        response.recovery_provider_id != request.recovery_provider_id ||
        response.recovery_request_hash !=
            GetAlternativeRecoveryRequestHash(request) ||
        response.recovery_commit_key != GetAlternativeRecoveryCommitKey(response) ||
        manifest.manifest_id != GetAlternativeRecoveryManifestId(manifest) ||
        manifest.request_id != request.request_id ||
        manifest.session_id != request.session_id ||
        manifest.original_provider_id != request.original_provider_id ||
        manifest.recovery_provider_id != request.recovery_provider_id ||
        manifest.privacy_profile != request.privacy_profile ||
        manifest.offer_id != request.offer_id ||
        manifest.policy_hash != request.policy_hash ||
        manifest.original_commit_key != request.original_commit_key ||
        manifest.original_template_commitment !=
            request.original_template_commitment ||
        manifest.capacity_snapshot_id != request.capacity_snapshot_id ||
        manifest.capacity_resource_commitment !=
            request.capacity_resource_commitment ||
        manifest.user_dd_inputs != request.user_dd_inputs ||
        SerializeExact(manifest.wallet_returns) !=
            SerializeExact(request.wallet_returns) ||
        !(manifest.maximum_service_fee == request.maximum_service_fee) ||
        !(manifest.service_fee == request.service_fee) ||
        manifest.expires_at != response.expires_at) {
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_BINDING_MISMATCH";
        return false;
    }

    authorization.request_id = request.request_id;
    authorization.session_id = request.session_id;
    authorization.recovery_id = response.recovery_id;
    authorization.original_provider_id = request.original_provider_id;
    authorization.recovery_provider_id = request.recovery_provider_id;
    authorization.privacy_profile = request.privacy_profile;
    authorization.offer_id = request.offer_id;
    authorization.policy_hash = request.policy_hash;
    authorization.original_commit_key = request.original_commit_key;
    authorization.original_template_commitment =
        request.original_template_commitment;
    authorization.capacity_snapshot_id = request.capacity_snapshot_id;
    authorization.capacity_resource_commitment =
        request.capacity_resource_commitment;
    authorization.recovery_request_hash = response.recovery_request_hash;
    authorization.recovery_commit_key = response.recovery_commit_key;
    authorization.recovery_manifest_id = manifest.manifest_id;
    authorization.recovery_template_commitment =
        manifest.template_commitment;
    authorization.user_dd_inputs = manifest.user_dd_inputs;
    authorization.wallet_returns = manifest.wallet_returns;
    authorization.maximum_service_fee = manifest.maximum_service_fee;
    authorization.service_fee = manifest.service_fee;
    authorization.network_fee = manifest.network_fee;
    authorization.expires_at = response.expires_at;
    authorization.authorization_commitment =
        GetRecoveryAuthorizationCommitment(authorization);
    if (authorization.authorization_commitment.IsNull()) {
        authorization = {};
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_INVALID";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateRecoveryAuthorizationManifest(
    const RecoveryAuthorizationManifest& authorization,
    const AlternativeRecoveryRequest& request,
    const AlternativeRecoveryResponse& response,
    int64_t now,
    std::string& error)
{
    RecoveryAuthorizationManifest expected;
    if (!BuildRecoveryAuthorizationManifest(request, response, expected,
                                             error)) {
        return false;
    }
    if (authorization.version !=
            RecoveryAuthorizationManifest::CURRENT_VERSION ||
        authorization.authorization_commitment.IsNull() || now <= 0 ||
        authorization.expires_at <= now ||
        SerializeExact(authorization) != SerializeExact(expected)) {
        error = authorization.expires_at <= now
                    ? "PAYMASTER_RECOVERY_AUTHORIZATION_EXPIRED"
                    : "PAYMASTER_RECOVERY_AUTHORIZATION_MISMATCH";
        return false;
    }
    error.clear();
    return true;
}

bool BuildAlternativeRecoveryTemplate(
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    AlternativeRecoveryTemplate& result,
    std::string& error)
{
    error.clear();
    return BuildExpectedRecovery(parameters, chain_params, coins, chainstate,
                                 now, result, error);
}

bool ValidateAlternativeRecoveryTemplate(
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    std::string& error)
{
    AlternativeRecoveryTemplate expected;
    if (!BuildExpectedRecovery(parameters, chain_params, coins, chainstate,
                               now, expected, error)) {
        return false;
    }
    if (trusted.manifest.version !=
            AlternativeRecoveryManifest::CURRENT_VERSION ||
        trusted.manifest.manifest_id.IsNull() ||
        trusted.manifest.template_commitment.IsNull() ||
        trusted.manifest.manifest_id !=
            GetAlternativeRecoveryManifestId(trusted.manifest) ||
        trusted.manifest.template_commitment !=
            GetAlternativeRecoveryTemplateCommitment(
                trusted.manifest, trusted.trusted_template) ||
        !SameManifest(trusted.manifest, expected.manifest) ||
        !SameTemplate(trusted.trusted_template, expected.trusted_template)) {
        error = "PAYMASTER_RECOVERY_TEMPLATE_MISMATCH";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateAlternativeRecoveryPSBT(
    const PartiallySignedTransaction& candidate,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    const CapacityChainstateCallbacks& chainstate,
    int64_t now,
    CollaborativeSignatureStage expected_stage,
    std::string& error)
{
    if (!ValidateAlternativeRecoveryTemplate(
            trusted, parameters, chain_params, coins, chainstate, now,
            error)) {
        return false;
    }
    return ValidateCollaborativePSBT(
        candidate, trusted.trusted_template, expected_stage, error);
}

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
    std::string& error)
{
    if (expected_wtxid.IsNull()) {
        error = "PAYMASTER_RECOVERY_FINAL_WTXID_MISSING";
        return false;
    }

    // Establish the exact transaction/witness binding before considering the
    // narrowly scoped post-expiry path. This prevents an expired capacity
    // proof from becoming a generic authorization to validate different final
    // bytes, outputs, roles or sighash modes.
    if (!ValidateFinalCollaborativeTransaction(
            final_transaction, trusted.manifest.unsigned_txid,
            expected_wtxid, trusted.trusted_template, error)) {
        return false;
    }

    int64_t authorization_time{now};
    if (parameters.expires_at <= now) {
        if (!exact_final_already_known || parameters.expires_at <= 1) {
            error = "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH";
            return false;
        }
        // USER_SIGNED/provider-signed artifacts are deliberately not released
        // on timeout. For an exact transaction already observed in the local
        // mempool or active chain, re-run the complete cryptographic authority
        // check at the final instant in which that authority was valid. The
        // caller still supplies current chainstate callbacks, so foreign
        // conflicts and all creating-output facts remain enforced.
        authorization_time = parameters.expires_at - 1;
    }
    return ValidateAlternativeRecoveryTemplate(
        trusted, parameters, chain_params, coins, chainstate,
        authorization_time, error);
}

} // namespace DigiDollar::Paymaster
