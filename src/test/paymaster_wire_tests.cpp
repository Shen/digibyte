// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Canonical Paymaster message decoding and envelope-bound tests. */

#include <boost/test/unit_test.hpp>

#include <coins.h>
#include <consensus/digidollar.h>
#include <hash.h>
#include <key.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/validation.h>
#include <paymaster/wire.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <validation.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using namespace DigiDollar::Paymaster;

namespace {

CScript P2TRScript()
{
    CKey key;
    key.MakeNewKey(true);
    return GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
}

PaymentIntent Intent(const uint256& genesis, int64_t now, SponsorshipScope scope)
{
    PaymentIntent intent;
    intent.genesis_hash = genesis;
    intent.provider_id = uint256S("01");
    intent.request_id = "550e8400-e29b-41d4-a716-446655440020";
    intent.session_id = uint256S("02");
    intent.client_nonce = uint256S("03");
    intent.canonical_request_hash = uint256S("0301");
    intent.user_dd_inputs = {COutPoint{uint256S("04"), 0}};
    intent.recipient_script = P2TRScript();
    intent.recipient_amount = DDCents{10000};
    intent.user_dd_change_script = P2TRScript();
    intent.offer_id = uint256S("05");
    intent.funding_model = scope == SponsorshipScope::RESTRICTED ? FundingModel::SPONSORED : FundingModel::USER_PAID;
    intent.sponsorship_scope = scope;
    intent.policy_hash = uint256S("06");
    intent.expires_at = now + 30;
    intent.user_input_proofs.push_back({intent.user_dd_inputs.front(), std::vector<unsigned char>(64)});
    return intent;
}

XOnlyPubKey BIP86OutputKey(const CKey& internal_key)
{
    const auto tweaked = XOnlyPubKey{internal_key.GetPubKey()}.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    return tweaked->first;
}

void SignBIP86ControlProof(const CKey& internal_key,
                           const uint256& hash,
                           std::vector<unsigned char>& signature)
{
    signature.resize(64);
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(internal_key.SignSchnorr(hash, signature, &empty_merkle_root, uint256{}));
}

void SignCapacityIdentity(const CKey& identity_key, PaymasterCapacityProof& proof)
{
    proof.identity_signature.resize(64);
    BOOST_REQUIRE(identity_key.SignSchnorr(GetCapacityProofSignatureHash(proof),
                                           proof.identity_signature, nullptr, uint256{}));
}

PaymasterCapacityProof SignedCapacityProof(const PaymasterCapacityRequest& request,
                                           const CKey& identity_key,
                                           const CKey& dgb_internal_key,
                                           const CKey& carrier_internal_key,
                                           int64_t now)
{
    PaymasterCapacityProof proof;
    proof.genesis_hash = request.genesis_hash;
    proof.provider_id = request.provider_id;
    proof.request_id = request.request_id;
    proof.session_id = request.session_id;
    proof.client_nonce = request.client_nonce;
    proof.funding_model = request.funding_model;
    proof.requires_carrier = request.requires_carrier;
    proof.snapshot_id = uint256S("13");
    proof.created_at = now;
    proof.expires_at = now + 30;

    const XOnlyPubKey dgb_output_key{BIP86OutputKey(dgb_internal_key)};
    CMutableTransaction dgb_tx;
    dgb_tx.vout.emplace_back(10000000,
                             GetScriptForDestination(WitnessV1Taproot{dgb_output_key}));
    CapacityDGBInput dgb;
    dgb.input.creating_tx = dgb_tx;
    dgb.input.outpoint = COutPoint{CTransaction{dgb_tx}.GetHash(), 0};
    dgb.input.value = DGBSatoshis{dgb_tx.vout.front().nValue};
    dgb.control_proof.reference_block = uint256S("15");
    dgb.control_proof.expires_at = proof.expires_at;
    SignBIP86ControlProof(dgb_internal_key,
                          GetCapacityControlHash(proof, dgb.input.outpoint,
                                                 dgb.control_proof.expires_at),
                          dgb.control_proof.signature);

    PaymasterLiquiditySlot slot;
    if (proof.requires_carrier) {
        const XOnlyPubKey carrier_output_key{
            BIP86OutputKey(carrier_internal_key)};
        CMutableTransaction carrier_tx;
        carrier_tx.vout.emplace_back(
            0, GetScriptForDestination(
                   WitnessV1Taproot{carrier_output_key}));
        CapacityDDCarrier carrier;
        carrier.carrier.creating_tx = carrier_tx;
        carrier.carrier.outpoint =
            COutPoint{CTransaction{carrier_tx}.GetHash(), 0};
        carrier.carrier.value = DDCents{100};
        carrier.control_proof.reference_block =
            dgb.control_proof.reference_block;
        carrier.control_proof.expires_at = proof.expires_at;
        SignBIP86ControlProof(
            carrier_internal_key,
            GetCapacityControlHash(proof, carrier.carrier.outpoint,
                                   carrier.control_proof.expires_at),
            carrier.control_proof.signature);
        slot.carrier = std::move(carrier);
    }
    slot.dgb_inputs.push_back(std::move(dgb));
    proof.liquidity_slots.push_back(std::move(slot));
    SignCapacityIdentity(identity_key, proof);
    return proof;
}

struct RecoveryWireMessages {
    AlternativeRecoveryRequest request;
    AlternativeRecoveryResponse response;
    AlternativeRecoverySubmit submit;
    AlternativeRecoveryResultMessage result;
};

RecoveryWireMessages RecoveryMessages(
    const uint256& genesis, int64_t now, const std::string& request_id,
    const uint256& session_id, const PaymasterId& recovery_provider_id)
{
    RecoveryWireMessages messages;
    AlternativeRecoveryRequest& request = messages.request;
    request.genesis_hash = genesis;
    request.request_id = request_id;
    request.session_id = session_id;
    request.original_provider_id = uint256S("a101");
    request.recovery_provider_id = recovery_provider_id;
    request.original_commit_key = uint256S("a102");
    request.original_template_commitment = uint256S("a103");
    request.offer_id = uint256S("a10a");
    request.policy_hash = uint256S("a10b");
    request.client_nonce = session_id;
    request.capacity_request.genesis_hash = genesis;
    request.capacity_request.provider_id = recovery_provider_id;
    request.capacity_request.request_id = request_id;
    request.capacity_request.session_id = session_id;
    request.capacity_request.client_nonce = request.client_nonce;
    request.capacity_request.funding_model = FundingModel::USER_PAID;
    request.capacity_request.requires_carrier = false;
    request.capacity_request.created_at = now;
    request.capacity_request.expires_at = now + 60;
    request.capacity_snapshot_id = uint256S("a104");
    request.capacity_resource_commitment = uint256S("a105");
    request.user_dd_inputs = {COutPoint{uint256S("a106"), 0}};
    request.wallet_returns = {{P2TRScript(), DDCents{10000}}};
    request.maximum_service_fee = DDCents{500};
    request.service_fee = DDCents{100};
    request.created_at = now;
    request.expires_at = now + 60;
    request.user_input_proofs = {
        {request.user_dd_inputs.front(), std::vector<unsigned char>(64)}};

    AlternativeRecoveryResponse& response = messages.response;
    response.genesis_hash = genesis;
    response.request_id = request_id;
    response.session_id = session_id;
    response.recovery_id = GetAlternativeRecoveryId(
        request_id, session_id, recovery_provider_id, request.client_nonce);
    response.recovery_provider_id = recovery_provider_id;
    response.recovery_request_hash = GetAlternativeRecoveryRequestHash(request);
    response.manifest.request_id = request_id;
    response.manifest.session_id = session_id;
    response.manifest.original_provider_id = request.original_provider_id;
    response.manifest.recovery_provider_id = recovery_provider_id;
    response.manifest.original_commit_key = request.original_commit_key;
    response.manifest.original_template_commitment =
        request.original_template_commitment;
    response.manifest.offer_id = request.offer_id;
    response.manifest.policy_hash = request.policy_hash;
    response.manifest.capacity_snapshot_id = request.capacity_snapshot_id;
    response.manifest.capacity_resource_commitment =
        request.capacity_resource_commitment;
    response.manifest.user_dd_inputs = request.user_dd_inputs;
    response.manifest.wallet_returns = request.wallet_returns;
    response.manifest.recovery_provider_dgb_inputs = {
        COutPoint{uint256S("a107"), 0}};
    response.manifest.recovery_provider_dgb_change_scripts = {P2TRScript()};
    response.manifest.maximum_service_fee = request.maximum_service_fee;
    response.manifest.service_fee = request.service_fee;
    response.manifest.network_fee = DGBSatoshis{1000};
    response.manifest.expires_at = now + 30;
    response.manifest.unsigned_txid = uint256S("a108");
    response.manifest.template_commitment = uint256S("a109");
    response.manifest.manifest_id =
        GetAlternativeRecoveryManifestId(response.manifest);
    response.recovery_commit_key = GetAlternativeRecoveryCommitKey(response);
    response.unsigned_psbt = {0x70, 0x73, 0x62, 0x74};
    response.created_at = now;
    response.expires_at = response.manifest.expires_at;
    response.identity_signature.resize(64);

    AlternativeRecoverySubmit& submit = messages.submit;
    submit.genesis_hash = genesis;
    submit.request_id = request_id;
    submit.session_id = session_id;
    submit.recovery_id = response.recovery_id;
    submit.recovery_provider_id = recovery_provider_id;
    submit.recovery_request_hash = response.recovery_request_hash;
    submit.recovery_commit_key = response.recovery_commit_key;
    submit.template_commitment = response.manifest.template_commitment;
    submit.user_psbt = response.unsigned_psbt;

    AlternativeRecoveryResultMessage& result = messages.result;
    result.request_id = request_id;
    result.session_id = session_id;
    result.recovery_id = response.recovery_id;
    result.recovery_request_hash = response.recovery_request_hash;
    result.result.genesis_hash = genesis;
    result.result.provider_id = recovery_provider_id;
    result.result.commit_key = response.recovery_commit_key;
    result.result.result_sequence = 1;
    result.result.status = PaymasterResultStatus::SLOT_UNAVAILABLE;
    result.result.updated_at = now;
    result.result.identity_signature.resize(64);
    return messages;
}

template <typename Message>
Message NetworkRoundTrip(const Message& message)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << message;
    Message decoded;
    stream >> decoded;
    BOOST_REQUIRE(stream.empty());
    return decoded;
}

uint256 DirectSessionIdForTest(const DirectMessage& direct)
{
    return std::visit(
        [](const auto& message) {
            using Message = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<Message, PaymasterQuoteRequest>) {
                return message.intent.session_id;
            } else {
                return message.session_id;
            }
        },
        direct.payload);
}

void CheckTwoSessionRoundRobin(const std::vector<DirectMessage>& selected,
                               uint64_t keyed_netgroup)
{
    BOOST_REQUIRE_EQUAL(selected.size(), 4U);
    for (const DirectMessage& direct : selected) {
        BOOST_REQUIRE(direct.keyed_netgroup.has_value());
        BOOST_CHECK_EQUAL(*direct.keyed_netgroup, keyed_netgroup);
    }
    const uint256 first_session{DirectSessionIdForTest(selected[0])};
    const uint256 second_session{DirectSessionIdForTest(selected[1])};
    BOOST_REQUIRE(first_session != second_session);
    BOOST_CHECK(DirectSessionIdForTest(selected[2]) == first_session);
    BOOST_CHECK(DirectSessionIdForTest(selected[3]) == second_session);
    BOOST_CHECK_EQUAL(selected[0].peer_id + 100, selected[2].peer_id);
    BOOST_CHECK_EQUAL(selected[1].peer_id + 100, selected[3].peer_id);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wire_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(capacity_is_exactly_one_bounded_slot)
{
    const int64_t now{100000};
    const uint256 genesis{uint256S("10")};
    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id = uint256S("11");
    request.request_id = "550e8400-e29b-41d4-a716-446655440011";
    request.session_id = uint256S("16");
    request.client_nonce = uint256S("12");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = false;
    request.created_at = now;
    request.expires_at = now + 60;
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateCapacityRequestEnvelope(request, genesis, now, error), error);

    PaymasterCapacityProof proof;
    proof.genesis_hash = genesis;
    proof.provider_id = request.provider_id;
    proof.request_id = request.request_id;
    proof.session_id = request.session_id;
    proof.client_nonce = request.client_nonce;
    proof.funding_model = request.funding_model;
    proof.requires_carrier = request.requires_carrier;
    proof.snapshot_id = uint256S("13");
    proof.created_at = now;
    proof.expires_at = now + 30;
    PaymasterLiquiditySlot slot;
    CapacityDGBInput dgb;
    dgb.input.outpoint = COutPoint{uint256S("14"), 0};
    dgb.input.value = DGBSatoshis{1000};
    dgb.control_proof.reference_block = uint256S("15");
    dgb.control_proof.expires_at = proof.expires_at;
    dgb.control_proof.signature.resize(64);
    slot.dgb_inputs.push_back(dgb);
    proof.liquidity_slots.push_back(slot);
    proof.identity_signature.resize(64);
    BOOST_REQUIRE_MESSAGE(ValidateCapacityProofEnvelope(proof, request, now, error), error);

    proof.liquidity_slots.push_back(slot);
    BOOST_CHECK(!ValidateCapacityProofEnvelope(proof, request, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_LIMITS");
}

BOOST_AUTO_TEST_CASE(capacity_v5_binds_funding_model_and_carrier_role)
{
    constexpr int64_t now{100000};
    const uint256 genesis{uint256S("17")};
    CKey identity_key;
    CKey dgb_internal_key;
    CKey carrier_internal_key;
    identity_key.MakeNewKey(true);
    dgb_internal_key.MakeNewKey(true);
    carrier_internal_key.MakeNewKey(true);

    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id =
        GetPaymasterId(XOnlyPubKey{identity_key.GetPubKey()});
    request.request_id = "550e8400-e29b-41d4-a716-446655440017";
    request.session_id = uint256S("18");
    request.client_nonce = uint256S("19");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = true;
    request.created_at = now;
    request.expires_at = now + 60;
    const PaymasterCapacityProof proof = SignedCapacityProof(
        request, identity_key, dgb_internal_key, carrier_internal_key, now);

    std::string error;
    BOOST_REQUIRE_MESSAGE(
        ValidateCapacityProofEnvelope(proof, request, now, error), error);

    PaymasterCapacityProof wrong_model{proof};
    wrong_model.funding_model = FundingModel::SPONSORED;
    BOOST_CHECK(!ValidateCapacityProofEnvelope(
        wrong_model, request, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_BINDING_MISMATCH");

    PaymasterCapacityRequest carrierless_request{request};
    carrierless_request.requires_carrier = false;
    PaymasterCapacityProof unexpected_carrier{proof};
    unexpected_carrier.requires_carrier = false;
    BOOST_CHECK(!ValidateCapacityProofEnvelope(
        unexpected_carrier, carrierless_request, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_SLOT");

    PaymasterCapacityRequest sponsored_carrier{request};
    sponsored_carrier.funding_model = FundingModel::SPONSORED;
    BOOST_CHECK(!ValidateCapacityRequestEnvelope(
        sponsored_carrier, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_REQUEST");

    for (const uint16_t legacy_version : {uint16_t{1}, uint16_t{2},
                                          uint16_t{3}, uint16_t{4}}) {
        PaymasterCapacityRequest legacy_request{request};
        legacy_request.version = legacy_version;
        BOOST_CHECK(!ValidateCapacityRequestEnvelope(
            legacy_request, genesis, now, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN");

        PaymasterCapacityProof legacy_proof{proof};
        legacy_proof.version = legacy_version;
        BOOST_CHECK(!ValidateCapacityProofEnvelope(
            legacy_proof, request, now, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN");
    }
}

BOOST_AUTO_TEST_CASE(capacity_full_validation_checks_identity_bip86_and_chainstate)
{
    const int64_t now{100000};
    const uint256 genesis{uint256S("20")};
    CKey identity_key;
    CKey dgb_internal_key;
    CKey carrier_internal_key;
    identity_key.MakeNewKey(true);
    dgb_internal_key.MakeNewKey(true);
    carrier_internal_key.MakeNewKey(true);

    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id = GetPaymasterId(XOnlyPubKey{identity_key.GetPubKey()});
    request.request_id = "550e8400-e29b-41d4-a716-446655440021";
    request.session_id = uint256S("22");
    request.client_nonce = uint256S("21");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = true;
    request.created_at = now;
    request.expires_at = now + 60;
    const PaymasterCapacityProof proof = SignedCapacityProof(
        request, identity_key, dgb_internal_key, carrier_internal_key, now);

    size_t reference_calls{0};
    size_t dgb_calls{0};
    size_t carrier_calls{0};
    CapacityChainstateCallbacks chainstate;
    chainstate.validate_reference_block = [&](const uint256& reference_block,
                                              std::string&) {
        ++reference_calls;
        return reference_block == uint256S("15");
    };
    chainstate.validate_dgb_input = [&](const VerifiedDGBInput& input,
                                        XOnlyPubKey& output_key, std::string&) {
        ++dgb_calls;
        BOOST_CHECK(input.outpoint == proof.liquidity_slots.front().dgb_inputs.front().input.outpoint);
        BOOST_CHECK(CTransaction{input.creating_tx}.GetHash() == input.outpoint.hash);
        BOOST_CHECK_EQUAL(input.value.value, 10000000);
        output_key = BIP86OutputKey(dgb_internal_key);
        return true;
    };
    chainstate.validate_dd_carrier = [&](const VerifiedDDCarrier& carrier,
                                         XOnlyPubKey& output_key, std::string&) {
        ++carrier_calls;
        BOOST_CHECK(carrier.outpoint == proof.liquidity_slots.front().carrier->carrier.outpoint);
        BOOST_CHECK(CTransaction{carrier.creating_tx}.GetHash() == carrier.outpoint.hash);
        BOOST_CHECK_EQUAL(carrier.value.value, 100);
        output_key = BIP86OutputKey(carrier_internal_key);
        return true;
    };

    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateCapacityProof(
                              proof, request, genesis,
                              XOnlyPubKey{identity_key.GetPubKey()}, chainstate, now, error),
                          error);
    BOOST_CHECK_EQUAL(reference_calls, 1U);
    BOOST_CHECK_EQUAL(dgb_calls, 1U);
    BOOST_CHECK_EQUAL(carrier_calls, 1U);
}

BOOST_AUTO_TEST_CASE(capacity_full_validation_rejects_signature_and_request_mutations)
{
    const int64_t now{100000};
    const uint256 genesis{uint256S("30")};
    CKey identity_key;
    CKey dgb_internal_key;
    CKey carrier_internal_key;
    identity_key.MakeNewKey(true);
    dgb_internal_key.MakeNewKey(true);
    carrier_internal_key.MakeNewKey(true);

    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id = GetPaymasterId(XOnlyPubKey{identity_key.GetPubKey()});
    request.request_id = "550e8400-e29b-41d4-a716-446655440031";
    request.session_id = uint256S("34");
    request.client_nonce = uint256S("31");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = true;
    request.created_at = now;
    request.expires_at = now + 60;
    const PaymasterCapacityProof valid = SignedCapacityProof(
        request, identity_key, dgb_internal_key, carrier_internal_key, now);

    CapacityChainstateCallbacks chainstate;
    chainstate.validate_reference_block = [](const uint256&, std::string&) { return true; };
    chainstate.validate_dgb_input = [&](const VerifiedDGBInput&, XOnlyPubKey& output_key,
                                        std::string&) {
        output_key = BIP86OutputKey(dgb_internal_key);
        return true;
    };
    chainstate.validate_dd_carrier = [&](const VerifiedDDCarrier&, XOnlyPubKey& output_key,
                                         std::string&) {
        output_key = BIP86OutputKey(carrier_internal_key);
        return true;
    };
    const XOnlyPubKey identity_public_key{identity_key.GetPubKey()};
    std::string error;

    PaymasterCapacityProof mutated{valid};
    mutated.identity_signature.front() ^= 1;
    BOOST_CHECK(!ValidateCapacityProof(mutated, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_IDENTITY_SIGNATURE");

    CKey wrong_identity_key;
    wrong_identity_key.MakeNewKey(true);
    BOOST_CHECK(!ValidateCapacityProof(valid, request, genesis,
                                       XOnlyPubKey{wrong_identity_key.GetPubKey()},
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_IDENTITY");

    mutated = valid;
    mutated.liquidity_slots.front().dgb_inputs.front().control_proof.signature.front() ^= 1;
    SignCapacityIdentity(identity_key, mutated);
    BOOST_CHECK(!ValidateCapacityProof(mutated, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_DGB_CONTROL_SIGNATURE");

    mutated = valid;
    mutated.liquidity_slots.front().carrier->control_proof.signature.front() ^= 1;
    SignCapacityIdentity(identity_key, mutated);
    BOOST_CHECK(!ValidateCapacityProof(mutated, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_CARRIER_CONTROL_SIGNATURE");

    mutated = valid;
    mutated.created_at = request.created_at - 1;
    BOOST_CHECK(!ValidateCapacityProof(mutated, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_BINDING_MISMATCH");

    mutated = valid;
    ++mutated.liquidity_slots.front().dgb_inputs.front().control_proof.expires_at;
    BOOST_CHECK(!ValidateCapacityProof(mutated, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_DGB_INPUT");

    PaymasterCapacityRequest wrong_nonce{request};
    wrong_nonce.client_nonce = uint256S("32");
    BOOST_CHECK(!ValidateCapacityProof(valid, wrong_nonce, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_BINDING_MISMATCH");

    BOOST_CHECK(!ValidateCapacityProof(valid, request, uint256S("33"), identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN");
}

BOOST_AUTO_TEST_CASE(capacity_full_validation_requires_successful_chainstate_callbacks)
{
    const int64_t now{100000};
    const uint256 genesis{uint256S("40")};
    CKey identity_key;
    CKey dgb_internal_key;
    CKey carrier_internal_key;
    identity_key.MakeNewKey(true);
    dgb_internal_key.MakeNewKey(true);
    carrier_internal_key.MakeNewKey(true);

    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id = GetPaymasterId(XOnlyPubKey{identity_key.GetPubKey()});
    request.request_id = "550e8400-e29b-41d4-a716-446655440041";
    request.session_id = uint256S("42");
    request.client_nonce = uint256S("41");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = true;
    request.created_at = now;
    request.expires_at = now + 60;
    const PaymasterCapacityProof proof = SignedCapacityProof(
        request, identity_key, dgb_internal_key, carrier_internal_key, now);
    const XOnlyPubKey identity_public_key{identity_key.GetPubKey()};
    std::string error;

    CapacityChainstateCallbacks chainstate;
    BOOST_CHECK(!ValidateCapacityProof(proof, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_CHAINSTATE_VALIDATOR_MISSING");

    chainstate.validate_reference_block = [](const uint256&, std::string& callback_error) {
        callback_error = "PAYMASTER_TEST_STALE_REFERENCE_BLOCK";
        return false;
    };
    chainstate.validate_dgb_input = [&](const VerifiedDGBInput&, XOnlyPubKey& output_key,
                                        std::string&) {
        output_key = BIP86OutputKey(dgb_internal_key);
        return true;
    };
    BOOST_CHECK(!ValidateCapacityProof(proof, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_TEST_STALE_REFERENCE_BLOCK");

    chainstate.validate_reference_block = [](const uint256&, std::string&) { return true; };
    chainstate.validate_dgb_input = [](const VerifiedDGBInput&, XOnlyPubKey&,
                                       std::string& callback_error) {
        callback_error = "PAYMASTER_TEST_SPENT_DGB_INPUT";
        return false;
    };
    BOOST_CHECK(!ValidateCapacityProof(proof, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_TEST_SPENT_DGB_INPUT");

    chainstate.validate_dgb_input = [&](const VerifiedDGBInput&, XOnlyPubKey& output_key,
                                        std::string&) {
        output_key = BIP86OutputKey(dgb_internal_key);
        return true;
    };
    BOOST_CHECK(!ValidateCapacityProof(proof, request, genesis, identity_public_key,
                                       chainstate, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_CHAINSTATE_VALIDATOR_MISSING");
}

BOOST_FIXTURE_TEST_CASE(admission_validation_rejects_mempool_conflicts,
                        TestChain100Setup)
{
    CKey identity_key;
    identity_key.MakeNewKey(true);
    Announcement announcement;
    announcement.identity_key = XOnlyPubKey{identity_key.GetPubKey()};
    announcement.sequence = 1;
    announcement.min_confirmations = 1;
    announcement.offers.push_back(
        {uint256S("51"), uint256S("52"), FundingModel::USER_PAID,
         SponsorshipScope::PUBLIC, 50, DDCents{100}, DDCents{1000000}});

    uint256 reference_block;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(m_node.chainman);
        BOOST_REQUIRE(m_node.chainman->ActiveChain().Tip());
        reference_block =
            m_node.chainman->ActiveChain().Tip()->GetBlockHash();
    }

    std::vector<CKey> dgb_keys(REQUIRED_ADMISSION_SLOTS);
    std::vector<CKey> carrier_keys(REQUIRED_ADMISSION_SLOTS);
    for (size_t index = 0; index < REQUIRED_ADMISSION_SLOTS; ++index) {
        dgb_keys[index].MakeNewKey(true);
        carrier_keys[index].MakeNewKey(true);

        CMutableTransaction dgb_tx;
        dgb_tx.vin.emplace_back(COutPoint{uint256::ONE,
                                          static_cast<uint32_t>(100 + index)});
        dgb_tx.vout.emplace_back(
            MIN_ADMISSION_DGB_SATOSHIS,
            GetScriptForDestination(
                WitnessV1Taproot{BIP86OutputKey(dgb_keys[index])}));

        CMutableTransaction carrier_tx;
        carrier_tx.SetDigiDollarType(::DD_TX_TRANSFER);
        carrier_tx.vin.emplace_back(COutPoint{
            uint256::ONE, static_cast<uint32_t>(200 + index)});
        carrier_tx.vout.emplace_back(
            0, GetScriptForDestination(
                   WitnessV1Taproot{BIP86OutputKey(carrier_keys[index])}));
        carrier_tx.vout.emplace_back(
            0, CScript{} << OP_RETURN
                         << std::vector<unsigned char>{'D', 'D'}
                         << CScriptNum(DD_TX_TRANSFER) << CScriptNum(100));

        AdmissionSlotProof slot;
        slot.dgb_creating_tx = dgb_tx;
        slot.dgb_outpoint = COutPoint{CTransaction{dgb_tx}.GetHash(), 0};
        slot.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        slot.carrier_creating_tx = carrier_tx;
        slot.carrier_outpoint =
            COutPoint{CTransaction{carrier_tx}.GetHash(), 0};
        slot.carrier_value = DDCents{100};
        slot.reference_block = reference_block;
        slot.expires_at = 100600;
        SignBIP86ControlProof(
            dgb_keys[index],
            GetAdmissionControlHash(
                GetPaymasterId(announcement.identity_key),
                announcement.sequence, reference_block, slot.dgb_outpoint,
                slot.expires_at, /*carrier=*/false),
            slot.dgb_control_signature);
        SignBIP86ControlProof(
            carrier_keys[index],
            GetAdmissionControlHash(
                GetPaymasterId(announcement.identity_key),
                announcement.sequence, reference_block,
                slot.carrier_outpoint, slot.expires_at, /*carrier=*/true),
            slot.carrier_control_signature);

        {
            LOCK(cs_main);
            auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
            coins.AddCoin(slot.dgb_outpoint,
                          Coin{dgb_tx.vout.at(0), /*nHeightIn=*/100,
                               /*fCoinBaseIn=*/false},
                          /*possible_overwrite=*/false);
            coins.AddCoin(slot.carrier_outpoint,
                          Coin{carrier_tx.vout.at(0), /*nHeightIn=*/100,
                               /*fCoinBaseIn=*/false},
                          /*possible_overwrite=*/false);
        }
        announcement.admission_slots.push_back(std::move(slot));
    }

    std::string error;
    BOOST_REQUIRE_MESSAGE(
        ValidateAdmissionProofs(announcement, *m_node.chainman, error), error);

    const auto add_conflict = [&](const COutPoint& outpoint) {
        CMutableTransaction spending_tx;
        spending_tx.vin.emplace_back(outpoint);
        spending_tx.vout.emplace_back(1, CScript{} << OP_TRUE);
        const CTransactionRef spending_ref{
            MakeTransactionRef(std::move(spending_tx))};
        {
            LOCK2(cs_main, m_node.mempool->cs);
            TestMemPoolEntryHelper entry;
            m_node.mempool->addUnchecked(entry.Fee(1).FromTx(spending_ref));
        }
        return spending_ref;
    };
    const auto remove_conflict = [&](const CTransactionRef& transaction) {
        WITH_LOCK(m_node.mempool->cs,
                  m_node.mempool->removeRecursive(
                      *transaction, MemPoolRemovalReason::CONFLICT));
    };

    const CTransactionRef dgb_conflict{
        add_conflict(announcement.admission_slots.front().dgb_outpoint)};
    BOOST_CHECK(!ValidateAdmissionProofs(
        announcement, *m_node.chainman, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_DGB_ADMISSION_PROOF");
    remove_conflict(dgb_conflict);

    BOOST_REQUIRE_MESSAGE(
        ValidateAdmissionProofs(announcement, *m_node.chainman, error), error);
    const CTransactionRef carrier_conflict{
        add_conflict(announcement.admission_slots.front().carrier_outpoint)};
    BOOST_CHECK(!ValidateAdmissionProofs(
        announcement, *m_node.chainman, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CARRIER_ADMISSION_PROOF");
    remove_conflict(carrier_conflict);
}

BOOST_FIXTURE_TEST_CASE(
    authorized_capacity_retry_uses_historical_envelope_but_current_resources,
    TestChain100Setup)
{
    constexpr int64_t authorization_time{100000};
    constexpr int64_t observation_time{100100};
    const uint256 genesis{uint256S("48")};
    CKey identity_key;
    CKey dgb_internal_key;
    CKey unused_carrier_key;
    identity_key.MakeNewKey(true);
    dgb_internal_key.MakeNewKey(true);
    unused_carrier_key.MakeNewKey(true);

    PaymasterCapacityRequest request;
    request.genesis_hash = genesis;
    request.provider_id =
        GetPaymasterId(XOnlyPubKey{identity_key.GetPubKey()});
    request.request_id = "550e8400-e29b-41d4-a716-446655440048";
    request.session_id = uint256S("49");
    request.client_nonce = uint256S("4a");
    request.funding_model = FundingModel::USER_PAID;
    request.requires_carrier = false;
    request.created_at = authorization_time;
    request.expires_at = authorization_time + 60;
    PaymasterCapacityProof proof = SignedCapacityProof(
        request, identity_key, dgb_internal_key, unused_carrier_key,
        authorization_time);

    uint256 old_active_reference;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(m_node.chainman);
        BOOST_REQUIRE(m_node.chainman->ActiveChain().Height() >= 50);
        old_active_reference =
            m_node.chainman->ActiveChain()[50]->GetBlockHash();
    }
    CapacityDGBInput& dgb =
        proof.liquidity_slots.front().dgb_inputs.front();
    dgb.control_proof.reference_block = old_active_reference;
    SignBIP86ControlProof(
        dgb_internal_key,
        GetCapacityControlHash(proof, dgb.input.outpoint,
                               dgb.control_proof.expires_at),
        dgb.control_proof.signature);
    SignCapacityIdentity(identity_key, proof);

    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
            dgb.input.outpoint,
            Coin{dgb.input.creating_tx.vout.at(dgb.input.outpoint.n),
                 /*nHeightIn=*/100, /*fCoinBaseIn=*/false},
            /*possible_overwrite=*/false);
    }

    std::string error;
    // Fresh authorization still requires a recent reference block.
    BOOST_CHECK(!ValidateCapacityProofAgainstChainstate(
        proof, request, XOnlyPubKey{identity_key.GetPubKey()},
        *m_node.chainman, authorization_time, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_STALE_CAPACITY_REFERENCE_BLOCK");

    // The already authorized retry rechecks the signed envelope at its
    // acceptance time but all resource facts against today's chainstate.
    BOOST_REQUIRE_MESSAGE(
        ValidateAuthorizedCapacityRetryAgainstChainstate(
            proof, request, XOnlyPubKey{identity_key.GetPubKey()},
            *m_node.chainman, authorization_time, observation_time,
            AuthorizedCapacityResourceMode::REQUIRE_UNSPENT, error),
        error);

    // A new authorization/signature attempt after the Capacity TTL remains
    // forbidden even though an exact authorized retry is still possible.
    BOOST_CHECK(!ValidateCapacityProofAgainstChainstate(
        proof, request, XOnlyPubKey{identity_key.GetPubKey()},
        *m_node.chainman, observation_time, error));
    BOOST_CHECK(error == "PAYMASTER_INVALID_CAPACITY_TIME" ||
                error == "PAYMASTER_INVALID_CAPACITY_LIMITS");

    CMutableTransaction foreign_spender;
    foreign_spender.vin.emplace_back(dgb.input.outpoint);
    foreign_spender.vout.emplace_back(1, CScript{} << OP_TRUE);
    const CTransactionRef foreign_ref{
        MakeTransactionRef(std::move(foreign_spender))};
    {
        LOCK2(cs_main, m_node.mempool->cs);
        TestMemPoolEntryHelper entry;
        m_node.mempool->addUnchecked(entry.Fee(1).FromTx(foreign_ref));
    }
    BOOST_CHECK(!ValidateAuthorizedCapacityRetryAgainstChainstate(
        proof, request, XOnlyPubKey{identity_key.GetPubKey()},
        *m_node.chainman, authorization_time, observation_time,
        AuthorizedCapacityResourceMode::REQUIRE_UNSPENT, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE");
    WITH_LOCK(m_node.mempool->cs,
              m_node.mempool->removeRecursive(
                  *foreign_ref, MemPoolRemovalReason::CONFLICT));

    {
        LOCK(cs_main);
        BOOST_REQUIRE(
            m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(
                dgb.input.outpoint));
    }
    BOOST_CHECK(!ValidateAuthorizedCapacityRetryAgainstChainstate(
        proof, request, XOnlyPubKey{identity_key.GetPubKey()},
        *m_node.chainman, authorization_time, observation_time,
        AuthorizedCapacityResourceMode::REQUIRE_UNSPENT, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE");
}

BOOST_AUTO_TEST_CASE(restricted_capability_is_bound_but_not_signature_checked_on_network_thread)
{
    const int64_t now{100000};
    const uint256 genesis{uint256S("20")};
    PaymasterQuoteRequest request;
    request.intent = Intent(genesis, now, SponsorshipScope::RESTRICTED);
    RestrictedServiceDescriptor descriptor;
    descriptor.genesis_hash = genesis;
    descriptor.provider_id = request.intent.provider_id;
    descriptor.offer_id = request.intent.offer_id;
    descriptor.policy_hash = request.intent.policy_hash;
    descriptor.expires_at = request.intent.expires_at;
    descriptor.provider_identity_signature.resize(64);
    SponsorshipCapability capability;
    capability.genesis_hash = genesis;
    capability.provider_id = request.intent.provider_id;
    capability.offer_id = request.intent.offer_id;
    capability.policy_hash = request.intent.policy_hash;
    capability.recipient_script = request.intent.recipient_script;
    capability.amount = request.intent.recipient_amount;
    capability.payment_request_nonce = uint256S("21");
    capability.expires_at = request.intent.expires_at;
    capability.sponsor_signature.resize(64);
    request.intent.sponsorship_authorization_hash = GetSponsorshipCapabilityHash(capability);
    request.restricted_descriptor = descriptor;
    request.restricted_capability = capability;
    std::string error;
    BOOST_REQUIRE_EQUAL(request.version, DigiDollar::Paymaster::PROTOCOL_VERSION);
    BOOST_REQUIRE_EQUAL(request.intent.version, PaymentIntent::CURRENT_VERSION);
    BOOST_REQUIRE(IsCanonicalRequestId(request.intent.request_id));
    BOOST_REQUIRE(!request.intent.user_dd_inputs.front().IsNull());
    int witness_version{-1};
    std::vector<unsigned char> witness_program;
    BOOST_REQUIRE(request.intent.recipient_script.IsWitnessProgram(
        witness_version, witness_program));
    BOOST_REQUIRE_EQUAL(witness_version, 1);
    BOOST_REQUIRE_EQUAL(witness_program.size(), 32U);
    BOOST_REQUIRE(XOnlyPubKey{witness_program}.IsFullyValid());
    BOOST_REQUIRE_MESSAGE(
        ValidateSponsorshipBinding(request.intent.funding_model,
                                   request.intent.sponsorship_scope, 0,
                                   DDCents{0},
                                   request.intent.sponsorship_authorization_hash,
                                   error),
        error);
    BOOST_REQUIRE_MESSAGE(ValidateQuoteRequestEnvelope(request, genesis, now, error), error);
    BOOST_CHECK(!ValidateRedactedQuoteRequestEnvelope(request, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_UNREDACTED_CAPABILITY");

    PaymasterQuoteRequest redacted{request};
    redacted.restricted_descriptor.reset();
    redacted.restricted_capability.reset();
    BOOST_REQUIRE_MESSAGE(
        ValidateRedactedQuoteRequestEnvelope(redacted, genesis, now, error), error);

    request.restricted_capability->amount.value++;
    BOOST_CHECK(!ValidateQuoteRequestEnvelope(request, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPABILITY_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_CASE(submit_has_hard_psbt_limit_and_round_trips_canonically)
{
    PaymasterSubmit submit;
    submit.genesis_hash = uint256S("30");
    submit.provider_id = uint256S("31");
    submit.request_id = "550e8400-e29b-41d4-a716-446655440030";
    submit.session_id = uint256S("32");
    submit.quote_id = uint256S("33");
    submit.commit_key = uint256S("34");
    submit.template_commitment = uint256S("35");
    submit.user_psbt = {0x70, 0x73, 0x62, 0x74};
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateSubmitEnvelope(submit, submit.genesis_hash, error), error);

    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << submit;
    PaymasterSubmit decoded;
    stream >> decoded;
    BOOST_CHECK(stream.empty());
    BOOST_CHECK(decoded.commit_key == submit.commit_key);
    BOOST_CHECK(decoded.user_psbt == submit.user_psbt);

    submit.user_psbt.resize(MAX_DIRECT_PSBT_BYTES + 1);
    BOOST_CHECK(!ValidateSubmitEnvelope(submit, submit.genesis_hash, error));
}

BOOST_AUTO_TEST_CASE(result_message_rejects_future_and_partial_final_artifacts)
{
    const int64_t now{100000};
    PaymasterResultMessage message;
    message.request_id = "550e8400-e29b-41d4-a716-446655440040";
    message.session_id = uint256S("40");
    message.result.genesis_hash = uint256S("41");
    message.result.provider_id = uint256S("42");
    message.result.commit_key = uint256S("43");
    message.result.result_sequence = 1;
    message.result.status = PaymasterResultStatus::SLOT_UNAVAILABLE;
    message.result.updated_at = now;
    message.result.identity_signature.resize(64);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateResultMessageEnvelope(message, message.result.genesis_hash,
                                                        now, error),
                          error);
    message.result.updated_at = now + 61;
    BOOST_CHECK(!ValidateResultMessageEnvelope(message, message.result.genesis_hash, now, error));
}

BOOST_AUTO_TEST_CASE(recovery_direct_envelopes_are_canonical_bounded_and_mutation_safe)
{
    constexpr int64_t now{100000};
    const uint256 genesis{uint256S("a200")};
    const RecoveryWireMessages valid = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440201",
        uint256S("a202"), uint256S("a203"));
    std::string error;

    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryRequestEnvelope(
                              valid.request, genesis, now, error),
                          error);
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryResponseEnvelope(
                              valid.response, genesis, now, error),
                          error);
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoverySubmitEnvelope(
                              valid.submit, genesis, error),
                          error);
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryResultEnvelope(
                              valid.result, genesis, now, error),
                          error);

    const AlternativeRecoveryRequest decoded_request =
        NetworkRoundTrip(valid.request);
    const AlternativeRecoveryResponse decoded_response =
        NetworkRoundTrip(valid.response);
    const AlternativeRecoverySubmit decoded_submit =
        NetworkRoundTrip(valid.submit);
    const AlternativeRecoveryResultMessage decoded_result =
        NetworkRoundTrip(valid.result);
    BOOST_CHECK(GetAlternativeRecoveryRequestHash(decoded_request) ==
                GetAlternativeRecoveryRequestHash(valid.request));
    BOOST_CHECK(GetAlternativeRecoveryResponseSignatureHash(decoded_response) ==
                GetAlternativeRecoveryResponseSignatureHash(valid.response));
    BOOST_CHECK(decoded_submit.recovery_commit_key ==
                valid.submit.recovery_commit_key);
    BOOST_CHECK(decoded_submit.user_psbt == valid.submit.user_psbt);
    BOOST_CHECK(decoded_result.recovery_request_hash ==
                valid.result.recovery_request_hash);
    BOOST_CHECK_EQUAL(decoded_result.result.result_sequence,
                      valid.result.result.result_sequence);

    AlternativeRecoveryRequest bad_request{valid.request};
    bad_request.capacity_request.client_nonce = uint256S("a204");
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestEnvelope(
        bad_request, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH");

    bad_request = valid.request;
    bad_request.user_dd_inputs.resize(MAX_PAYMENT_INTENT_INPUTS + 1);
    bad_request.user_input_proofs.resize(MAX_PAYMENT_INTENT_INPUTS + 1);
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestEnvelope(
        bad_request, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_REQUEST_SHAPE");

    AlternativeRecoveryResponse bad_response{valid.response};
    ++bad_response.manifest.network_fee.value;
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoveryResponseEnvelope(
        bad_response, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_RESPONSE_BINDING");

    bad_response = valid.response;
    bad_response.unsigned_psbt.resize(MAX_DIRECT_PSBT_BYTES + 1);
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoveryResponseEnvelope(
        bad_response, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_RESPONSE_SHAPE");

    AlternativeRecoverySubmit bad_submit{valid.submit};
    bad_submit.genesis_hash = uint256S("a205");
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoverySubmitEnvelope(
        bad_submit, genesis, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_SUBMIT");

    bad_submit = valid.submit;
    bad_submit.user_psbt.resize(MAX_DIRECT_PSBT_BYTES + 1);
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoverySubmitEnvelope(
        bad_submit, genesis, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_SUBMIT");

    AlternativeRecoveryResultMessage bad_result{valid.result};
    bad_result.recovery_request_hash.SetNull();
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoveryResultEnvelope(
        bad_result, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_RESULT");

    bad_result = valid.result;
    bad_result.result.updated_at = now + 1;
    error.clear();
    BOOST_CHECK(!ValidateAlternativeRecoveryResultEnvelope(
        bad_result, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_RESULT");
}

BOOST_AUTO_TEST_CASE(manager_inbox_is_bounded_ephemeral_and_replay_protected)
{
    Manager manager{true};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.genesis_hash = uint256S("50");
    request.provider_id = uint256S("51");
    request.request_id = "550e8400-e29b-41d4-a716-446655440052";
    request.session_id = uint256S("54");
    request.client_nonce = uint256S("52");
    request.created_at = 100000;
    request.expires_at = 100060;

    const uint256 first_id{uint256S("53")};
    const std::vector<unsigned char> canonical_netgroup{1, 10, 20};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, first_id, 100, DirectPayload{request}, 100000,
                    uint64_t{7}, canonical_netgroup) ==
                DirectEnqueueResult::ACCEPTED);
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, first_id, 100, DirectPayload{request}, 100000) ==
                DirectEnqueueResult::DUPLICATE);
    BOOST_CHECK(manager.EnqueueDirectMessage(1, first_id, 100,
                                             DirectPayload{request}, 100000));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    auto received = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(received.size(), 1U);
    BOOST_CHECK(std::holds_alternative<PaymasterCapacityRequest>(received.front().payload));
    BOOST_REQUIRE(received.front().keyed_netgroup.has_value());
    BOOST_CHECK_EQUAL(*received.front().keyed_netgroup, 7U);
    BOOST_CHECK(received.front().canonical_netgroup == canonical_netgroup);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);

    // Reconnecting does not bypass semantic replay protection. Once the first
    // copy was consumed, an exact retry is delivered once more so the durable
    // handler can reproduce the same response for the replacement peer.
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, first_id, 100, DirectPayload{request}, 100001) ==
                DirectEnqueueResult::DUPLICATE);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    const auto retried = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(retried.size(), 1U);
    BOOST_CHECK_EQUAL(retried.front().peer_id, 2);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);

    PaymasterCapacityRequest conflicting{request};
    conflicting.expires_at++;
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, uint256S("54"), 100, DirectPayload{conflicting}, 100002) ==
                DirectEnqueueResult::CONFLICT);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    // The bool adapter still reports a non-benign network outcome, but the
    // same conflict artifact is not multiplied in the bounded inbox.
    BOOST_CHECK(!manager.EnqueueDirectMessage(
        2, uint256S("54"), 100, DirectPayload{conflicting}, 100002));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    PaymasterCapacityRequest nonce_conflict{request};
    nonce_conflict.client_nonce = uint256S("55");
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, uint256S("55"), 100, DirectPayload{nonce_conflict}, 100002) ==
                DirectEnqueueResult::CONFLICT);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(),
                      MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);

    PaymasterCapacityRequest third_conflict{request};
    third_conflict.created_at++;
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, uint256S("56"), 100, DirectPayload{third_conflict}, 100002) ==
                DirectEnqueueResult::FULL);

    // A retained conflict must not make the original message id look queued.
    // Once one slot is consumed, the original exact retry is delivered again.
    BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(1).size(), 1U);
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, first_id, 100, DirectPayload{request}, 100003) ==
                DirectEnqueueResult::DUPLICATE);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(),
                      MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
    BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(
                                   MAX_DIRECT_INBOX_MESSAGES_PER_SESSION)
                            .size(),
                        MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);

    BOOST_CHECK(manager.EnqueueDirectMessage(
        2, first_id, 100, DirectPayload{request},
        100000 + DIRECT_REPLAY_TTL_SECONDS));

    manager.SetEnabled(false);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_CHECK(!manager.EnqueueDirectMessage(1, uint256S("55"), 100,
                                              DirectPayload{request}, 101000));
}

BOOST_AUTO_TEST_CASE(manager_semantic_replay_is_idempotent_for_every_direct_message)
{
    const PaymasterId provider_id{uint256S("5601")};
    const std::string request_id{"550e8400-e29b-41d4-a716-446655440560"};
    const uint256 session_id{uint256S("5602")};

    PaymasterCapacityRequest capacity_request;
    capacity_request.funding_model = FundingModel::SPONSORED;
    capacity_request.provider_id = provider_id;
    capacity_request.request_id = request_id;
    capacity_request.session_id = session_id;
    capacity_request.client_nonce = uint256S("5603");
    capacity_request.expires_at = 100060;

    PaymasterCapacityProof capacity_proof;
    capacity_proof.funding_model = capacity_request.funding_model;
    capacity_proof.provider_id = provider_id;
    capacity_proof.request_id = request_id;
    capacity_proof.session_id = session_id;
    capacity_proof.client_nonce = capacity_request.client_nonce;
    capacity_proof.expires_at = 100060;

    PaymasterQuoteRequest quote_request;
    quote_request.intent.provider_id = provider_id;
    quote_request.intent.request_id = request_id;
    quote_request.intent.session_id = session_id;
    quote_request.intent.expires_at = 100060;

    PaymasterQuoteResponse quote_response;
    quote_response.request_id = request_id;
    quote_response.session_id = session_id;
    quote_response.quote.provider_id = provider_id;
    quote_response.quote.expires_at = 100060;

    PaymasterSubmit submit;
    submit.provider_id = provider_id;
    submit.request_id = request_id;
    submit.session_id = session_id;
    submit.user_psbt = {0x01};

    PaymasterResultMessage result;
    result.request_id = request_id;
    result.session_id = session_id;
    result.result.provider_id = provider_id;
    result.result.result_sequence = 1;
    result.result.updated_at = 100000;

    const RecoveryWireMessages recovery = RecoveryMessages(
        uint256S("56a0"), 100000,
        "550e8400-e29b-41d4-a716-446655440561", uint256S("56a1"),
        uint256S("56a2"));

    std::vector<DirectPayload> payloads{
        capacity_request, capacity_proof, quote_request,
        quote_response, submit, result, recovery.request,
        recovery.response, recovery.submit, recovery.result};

    for (size_t index{0}; index < payloads.size(); ++index) {
        Manager manager{true};
        uint256 message_id;
        message_id.begin()[0] = static_cast<unsigned char>(index + 1);

        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          1, message_id, 100, payloads[index], 100000) ==
                      DirectEnqueueResult::ACCEPTED);
        // An in-flight same-peer retry is benign and does not duplicate the
        // queue entry that is already waiting for its durable handler.
        BOOST_CHECK(manager.EnqueueDirectMessageResult(
                        1, message_id, 100, payloads[index], 100000) ==
                    DirectEnqueueResult::DUPLICATE);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
        BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(1).size(), 1U);

        // Once consumed, both a same-connection retry and a reconnect retry
        // are delivered again so the handler can reproduce its cached result.
        BOOST_CHECK(manager.EnqueueDirectMessageResult(
                        1, message_id, 100, payloads[index], 100001) ==
                    DirectEnqueueResult::DUPLICATE);
        auto same_peer_retry = manager.TakeDirectMessages(1);
        BOOST_REQUIRE_EQUAL(same_peer_retry.size(), 1U);
        BOOST_CHECK_EQUAL(same_peer_retry.front().peer_id, 1);

        BOOST_CHECK(manager.EnqueueDirectMessageResult(
                        2, message_id, 100, payloads[index], 100002) ==
                    DirectEnqueueResult::DUPLICATE);
        auto reconnect_retry = manager.TakeDirectMessages(1);
        BOOST_REQUIRE_EQUAL(reconnect_retry.size(), 1U);
        BOOST_CHECK_EQUAL(reconnect_retry.front().peer_id, 2);

        DirectPayload conflicting{payloads[index]};
        std::visit([](auto& message) {
            using Message = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<Message, PaymasterCapacityRequest> ||
                          std::is_same_v<Message, PaymasterCapacityProof>) {
                ++message.expires_at;
            } else if constexpr (std::is_same_v<Message, PaymasterQuoteRequest>) {
                ++message.intent.expires_at;
            } else if constexpr (std::is_same_v<Message, PaymasterQuoteResponse>) {
                ++message.quote.expires_at;
            } else if constexpr (std::is_same_v<Message, PaymasterSubmit>) {
                message.user_psbt.push_back(0x02);
            } else if constexpr (std::is_same_v<Message, AlternativeRecoveryRequest>) {
                message.capacity_snapshot_id = uint256S("56ff");
            } else if constexpr (std::is_same_v<Message, AlternativeRecoveryResponse>) {
                message.unsigned_psbt.push_back(0x02);
            } else if constexpr (std::is_same_v<Message, AlternativeRecoverySubmit>) {
                message.user_psbt.push_back(0x02);
            } else {
                ++message.result.updated_at;
            }
        },
                   conflicting);
        uint256 conflicting_message_id;
        conflicting_message_id.begin()[0] = static_cast<unsigned char>(index + 0x40);
        BOOST_CHECK(manager.EnqueueDirectMessageResult(
                        2, conflicting_message_id, 100, conflicting, 100003) ==
                    DirectEnqueueResult::CONFLICT);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
        BOOST_CHECK(manager.EnqueueDirectMessageResult(
                        3, conflicting_message_id, 100, conflicting, 100004) ==
                    DirectEnqueueResult::CONFLICT);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
        const auto retained_conflict = manager.TakeDirectMessages(1);
        BOOST_REQUIRE_EQUAL(retained_conflict.size(), 1U);
        BOOST_CHECK(retained_conflict.front().message_id == conflicting_message_id);
    }
}

BOOST_AUTO_TEST_CASE(manager_semantic_conflicts_consume_direct_rate_buckets)
{
    Manager manager{true};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = uint256S("56c0");
    request.request_id = "550e8400-e29b-41d4-a716-446655440563";
    request.session_id = uint256S("56c1");
    request.client_nonce = uint256S("56c2");
    request.expires_at = 100060;

    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, uint256S("56c3"), 100, DirectPayload{request}, 100000) ==
                  DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(1).size(), 1U);

    for (size_t index{1}; index < MAX_DIRECT_MESSAGES_PER_SESSION_PER_WINDOW; ++index) {
        PaymasterCapacityRequest conflicting{request};
        conflicting.expires_at += static_cast<int64_t>(index);
        uint256 message_id;
        message_id.begin()[0] = static_cast<unsigned char>(index + 1);
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          1, message_id, 100, DirectPayload{conflicting}, 100000) ==
                      DirectEnqueueResult::CONFLICT);
        BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(1).size(), 1U);
    }

    PaymasterCapacityRequest rate_limited_conflict{request};
    rate_limited_conflict.expires_at +=
        static_cast<int64_t>(MAX_DIRECT_MESSAGES_PER_SESSION_PER_WINDOW);
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, uint256S("56ff"), 100,
                    DirectPayload{rate_limited_conflict}, 100000) ==
                DirectEnqueueResult::RATE_LIMITED);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
}

BOOST_AUTO_TEST_CASE(manager_prunes_expired_unconsumed_inbound_messages)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = uint256S("5700");
    for (size_t index{0}; index < MAX_DIRECT_INBOX_MESSAGES; ++index) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(index + 1);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          static_cast<int64_t>(index + 1), id, 100,
                          DirectPayload{request}, now,
                          static_cast<uint64_t>(index + 1)) ==
                      DirectEnqueueResult::ACCEPTED);
    }
    BOOST_REQUIRE_EQUAL(manager.DirectMessageCount(),
                        MAX_DIRECT_INBOX_MESSAGES);

    const uint256 fresh_id{uint256S("57ff")};
    request.request_id = fresh_id.GetHex();
    request.session_id = fresh_id;
    request.client_nonce = fresh_id;
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      100, fresh_id, 100, DirectPayload{request},
                      SaturatingAddSeconds(now,
                                           MAX_DIRECT_MESSAGE_TTL_SECONDS),
                      uint64_t{100}) == DirectEnqueueResult::ACCEPTED);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
}

BOOST_AUTO_TEST_CASE(manager_explicit_prune_expires_direct_messages_without_enqueue)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    const int64_t expires_at{
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS)};

    PaymasterCapacityProof first;
    first.provider_id = uint256S("5710");
    first.request_id = "550e8400-e29b-41d4-a716-446655445710";
    first.session_id = uint256S("5711");
    first.client_nonce = uint256S("5712");
    PaymasterCapacityProof second{first};
    second.request_id = "550e8400-e29b-41d4-a716-446655445711";
    second.session_id = uint256S("5713");
    second.client_nonce = uint256S("5714");
    const uint256 first_id{uint256S("5715")};
    const uint256 second_id{uint256S("5716")};
    const uint256 outbound_id{uint256S("5717")};

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        1, first_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{first}, now,
        uint64_t{10}));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        2, second_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{second}, now,
        uint64_t{20}));
    BOOST_REQUIRE(manager.QueueOutboundDirectMessage(
        3, outbound_id, 100, DirectPayload{first}, now));
    BOOST_CHECK(manager.HasEquivocationCandidates());
    const auto selected = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(selected.size(), 1U);
    BOOST_CHECK(selected.front().message_id == first_id);

    // No enqueue or take operation drives expiry here. Explicit pruning must
    // release the remaining inbound/outbound bytes and reset the fairness
    // cursor after the inbound queue becomes empty.
    manager.PruneDirectMessages(expires_at);
    BOOST_CHECK(!manager.HasDirectMessages());
    BOOST_CHECK(!manager.HasEquivocationCandidates());
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0U);
    BOOST_CHECK(!manager.HasOutboundDirectMessage(3, outbound_id));

    PaymasterCapacityRequest lower_group;
    lower_group.provider_id = uint256S("5718");
    lower_group.request_id = "550e8400-e29b-41d4-a716-446655445712";
    lower_group.session_id = uint256S("5719");
    lower_group.client_nonce = uint256S("571a");
    PaymasterCapacityRequest higher_group{lower_group};
    higher_group.request_id = "550e8400-e29b-41d4-a716-446655445713";
    higher_group.session_id = uint256S("571b");
    higher_group.client_nonce = uint256S("571c");
    const uint256 lower_id{uint256S("571d")};
    const uint256 higher_id{uint256S("571e")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        4, lower_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{lower_group},
        expires_at, uint64_t{5}));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        5, higher_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{higher_group},
        expires_at, uint64_t{20}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    const auto after_prune = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(after_prune.size(), 1U);
    BOOST_CHECK(after_prune.front().message_id == lower_id);
}

BOOST_AUTO_TEST_CASE(manager_enqueue_retains_expired_signed_evidence_candidates)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    const int64_t expires_at{
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS)};

    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("5720");
    proof.request_id = "550e8400-e29b-41d4-a716-446655445720";
    proof.session_id = uint256S("5721");
    proof.client_nonce = uint256S("5722");
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, uint256S("5723"), 100, DirectPayload{proof}, now,
                      uint64_t{1}) == DirectEnqueueResult::ACCEPTED);

    PaymasterQuoteResponse quote;
    quote.request_id = "550e8400-e29b-41d4-a716-446655445721";
    quote.session_id = uint256S("5724");
    quote.quote.provider_id = uint256S("5725");
    quote.quote.intent_hash = uint256S("5726");
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      2, uint256S("5727"), 100, DirectPayload{quote}, now,
                      uint64_t{2}) == DirectEnqueueResult::ACCEPTED);

    PaymasterCapacityRequest fresh;
    fresh.provider_id = uint256S("5728");
    fresh.request_id = "550e8400-e29b-41d4-a716-446655445722";
    fresh.session_id = uint256S("5729");
    fresh.client_nonce = uint256S("572a");
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      3, uint256S("572b"), 100, DirectPayload{fresh},
                      expires_at, uint64_t{3}) ==
                  DirectEnqueueResult::ACCEPTED);

    // Enqueue-side cleanup may discard stale ordinary traffic, but signed
    // evidence candidates remain until all wallets have scanned them.
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 3U);
    manager.PruneDirectMessages(expires_at);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    const auto remaining = manager.TakeCapacityRequests(fresh.provider_id, 1);
    BOOST_REQUIRE_EQUAL(remaining.size(), 1U);
    BOOST_CHECK(remaining.front().message_id == uint256S("572b"));
}

BOOST_AUTO_TEST_CASE(manager_peek_lease_survives_prune_until_explicit_release)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("5738");
    proof.request_id = "550e8400-e29b-41d4-a716-446655445725";
    proof.session_id = uint256S("5739");
    proof.client_nonce = uint256S("573a");
    const uint256 message_id{uint256S("573b")};
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, message_id, 100, DirectPayload{proof}, now) ==
                  DirectEnqueueResult::ACCEPTED);

    auto lease = manager.LeaseCapacityProofs(
        proof.provider_id, proof.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
    manager.PruneDirectMessages(
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    lease.Release();
    manager.PruneDirectMessages(
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_scoped_lease_blocks_generic_and_specific_take)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityProof leased_proof;
    leased_proof.provider_id = uint256S("5730");
    leased_proof.request_id = "550e8400-e29b-41d4-a716-446655445724";
    leased_proof.session_id = uint256S("5731");
    leased_proof.client_nonce = uint256S("5732");
    PaymasterCapacityProof available_proof{leased_proof};
    available_proof.provider_id = uint256S("5733");
    available_proof.request_id = "550e8400-e29b-41d4-a716-44665544572a";
    available_proof.session_id = uint256S("5734");
    available_proof.client_nonce = uint256S("5735");
    const uint256 leased_id{uint256S("5736")};
    const uint256 available_id{uint256S("5737")};
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, leased_id, 100, DirectPayload{leased_proof}, now,
                      uint64_t{1}) == DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      2, available_id, 100, DirectPayload{available_proof}, now,
                      uint64_t{2}) == DirectEnqueueResult::ACCEPTED);

    auto lease = manager.LeaseCapacityProofs(
        leased_proof.provider_id, leased_proof.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
    const auto generic = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(generic.size(), 1U);
    BOOST_CHECK(generic.front().message_id == available_id);
    BOOST_CHECK(manager.TakeCapacityProofs(
                           leased_proof.provider_id,
                           leased_proof.client_nonce, 1)
                    .empty());

    lease.Release();
    const auto specific = manager.TakeCapacityProofs(
        leased_proof.provider_id, leased_proof.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(specific.size(), 1U);
    BOOST_CHECK(specific.front().message_id == leased_id);
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_scoped_capacity_lease_releases_on_exception)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("5740");
    proof.request_id = "550e8400-e29b-41d4-a716-446655445727";
    proof.session_id = uint256S("5741");
    proof.client_nonce = uint256S("5742");
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, uint256S("5743"), 100, DirectPayload{proof}, now) ==
                  DirectEnqueueResult::ACCEPTED);

    const auto fail_after_peek = [&] {
        auto lease = manager.LeaseCapacityProofs(
            proof.provider_id, proof.client_nonce, 1);
        BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
        manager.PruneDirectMessages(
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
        throw std::runtime_error{"simulated durable-store failure"};
    };
    BOOST_CHECK_THROW(fail_after_peek(), std::runtime_error);

    manager.PruneDirectMessages(
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_overlapping_scoped_leases_are_reference_counted)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterQuoteResponse response;
    response.request_id = "550e8400-e29b-41d4-a716-446655445728";
    response.session_id = uint256S("5744");
    response.quote.provider_id = uint256S("5745");
    response.quote.intent_hash = uint256S("5746");
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, uint256S("5747"), 100, DirectPayload{response}, now) ==
                  DirectEnqueueResult::ACCEPTED);

    auto first = manager.LeaseQuoteResponses(
        response.request_id, response.session_id,
        response.quote.provider_id, response.quote.intent_hash, 1);
    BOOST_REQUIRE_EQUAL(first.Messages().size(), 1U);
    {
        auto second = manager.LeaseQuoteResponses(
            response.request_id, response.session_id,
            response.quote.provider_id, response.quote.intent_hash, 1);
        BOOST_REQUIRE_EQUAL(second.Messages().size(), 1U);
        first.Release();
        manager.PruneDirectMessages(
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    }

    manager.PruneDirectMessages(
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_old_scope_cannot_release_new_exact_replay)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("5748");
    proof.request_id = "550e8400-e29b-41d4-a716-446655445729";
    proof.session_id = uint256S("5749");
    proof.client_nonce = uint256S("574a");
    const uint256 message_id{uint256S("574b")};
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, message_id, 100, DirectPayload{proof}, now) ==
                  DirectEnqueueResult::ACCEPTED);

    auto old_scope = manager.LeaseCapacityProofs(
        proof.provider_id, proof.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(old_scope.Messages().size(), 1U);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({message_id}));
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      2, message_id, 100, DirectPayload{proof}, now + 1) ==
                  DirectEnqueueResult::DUPLICATE);
    auto replay_scope = manager.LeaseCapacityProofs(
        proof.provider_id, proof.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(replay_scope.Messages().size(), 1U);

    old_scope.Release();
    manager.PruneDirectMessages(SaturatingAddSeconds(
        now + 1, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    replay_scope.Release();
    manager.PruneDirectMessages(SaturatingAddSeconds(
        now + 1, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_maintenance_peek_does_not_create_a_prune_lease)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("573c");
    proof.request_id = "550e8400-e29b-41d4-a716-446655445726";
    proof.session_id = uint256S("573d");
    proof.client_nonce = uint256S("573e");
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, uint256S("573f"), 100, DirectPayload{proof}, now) ==
                  DirectEnqueueResult::ACCEPTED);

    const auto observed = manager.PeekCapacityProofs(
        proof.provider_id, proof.client_nonce, 1, /*lease=*/false);
    BOOST_REQUIRE_EQUAL(observed.size(), 1U);
    manager.PruneDirectMessages(
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_retained_candidate_rejects_exact_replay_after_replay_ttl)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    const uint256 message_id{uint256S("572c")};
    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("572d");
    proof.request_id = "550e8400-e29b-41d4-a716-446655445723";
    proof.session_id = uint256S("572e");
    proof.client_nonce = uint256S("572f");

    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, message_id, 100, DirectPayload{proof}, now,
                      uint64_t{1}) == DirectEnqueueResult::ACCEPTED);
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, message_id, 100, DirectPayload{proof},
                    SaturatingAddSeconds(now, DIRECT_REPLAY_TTL_SECONDS),
                    uint64_t{2}) == DirectEnqueueResult::DUPLICATE);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    PaymasterCapacityProof conflicting{proof};
    conflicting.snapshot_id = uint256S("5730");
    const uint256 conflict_id{uint256S("5731")};
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, conflict_id, 100, DirectPayload{conflicting},
                    SaturatingAddSeconds(now, DIRECT_REPLAY_TTL_SECONDS),
                    uint64_t{2}) == DirectEnqueueResult::CONFLICT);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    BOOST_CHECK(manager.AcknowledgeDirectMessagesIfPresent(
        {message_id, conflict_id}));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_replayed_retained_conflict_remains_conflict)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    PaymasterCapacityProof baseline;
    baseline.provider_id = uint256S("5732");
    baseline.request_id = "550e8400-e29b-41d4-a716-446655445724";
    baseline.session_id = uint256S("5733");
    baseline.client_nonce = uint256S("5734");
    PaymasterCapacityProof conflict{baseline};
    conflict.snapshot_id = uint256S("5735");
    const uint256 baseline_id{uint256S("5736")};
    const uint256 conflict_id{uint256S("5737")};

    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, baseline_id, 100, DirectPayload{baseline}, now) ==
                  DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, conflict_id, 100, DirectPayload{conflict}, now) ==
                  DirectEnqueueResult::CONFLICT);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent({baseline_id}));
    BOOST_REQUIRE_EQUAL(manager.DirectMessageCount(), 1U);

    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, conflict_id, 100, DirectPayload{conflict}, now + 1) ==
                DirectEnqueueResult::CONFLICT);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
}

BOOST_AUTO_TEST_CASE(manager_prunes_orphaned_inbound_fairness_cursors)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{5701};

    PaymasterCapacityRequest first;
    first.funding_model = FundingModel::SPONSORED;
    first.provider_id = uint256S("5702");
    first.request_id = "550e8400-e29b-41d4-a716-446655445701";
    first.session_id = uint256S("5703");
    first.client_nonce = uint256S("5704");
    PaymasterCapacityRequest second{first};
    second.request_id = "550e8400-e29b-41d4-a716-446655445702";
    second.session_id = uint256S("5705");
    second.client_nonce = uint256S("5706");

    const uint256 first_id{uint256S("5707")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        1, first_id, 100, DirectPayload{first}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        2, uint256S("5708"), 100, DirectPayload{second}, now, netgroup));
    const auto selected = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(selected.size(), 1U);
    BOOST_CHECK(selected.front().message_id == first_id);

    // The second message expires while the fairness cursor still names the
    // first session. Pruning the expired message must also remove that now
    // orphaned cursor before this netgroup is reused.
    const int64_t expires_at{
        SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS)};
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      3, first_id, 100, DirectPayload{first}, expires_at,
                      netgroup) == DirectEnqueueResult::DUPLICATE);
    PaymasterCapacityRequest third{first};
    third.request_id = "550e8400-e29b-41d4-a716-446655445703";
    third.session_id = uint256S("5709");
    third.client_nonce = uint256S("570a");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        4, uint256S("570b"), 100, DirectPayload{third}, expires_at,
        netgroup));

    const auto reused = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(reused.size(), 1U);
    BOOST_CHECK(reused.front().message_id == first_id);
}

BOOST_AUTO_TEST_CASE(manager_replay_keys_do_not_let_stale_artifacts_poison_new_commits)
{
    const PaymasterId provider_id{uint256S("56b0")};
    const std::string request_id{"550e8400-e29b-41d4-a716-446655440562"};
    const uint256 session_id{uint256S("56b1")};
    Manager manager{true};
    unsigned char message_number{1};
    auto accept_and_consume = [&](DirectPayload payload) {
        uint256 message_id;
        message_id.begin()[0] = message_number++;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          1, message_id, 100, std::move(payload), 100000) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(1).size(), 1U);
    };

    PaymasterQuoteResponse stale_quote;
    stale_quote.request_id = request_id;
    stale_quote.session_id = session_id;
    stale_quote.quote.provider_id = provider_id;
    stale_quote.quote.quote_id = uint256S("56b2");
    stale_quote.quote.template_commitment = uint256S("56b3");
    PaymasterQuoteResponse fresh_quote{stale_quote};
    fresh_quote.quote.quote_id = uint256S("56b4");
    fresh_quote.quote.template_commitment = uint256S("56b5");
    accept_and_consume(stale_quote);
    accept_and_consume(fresh_quote);

    PaymasterSubmit stale_submit;
    stale_submit.provider_id = provider_id;
    stale_submit.request_id = request_id;
    stale_submit.session_id = session_id;
    stale_submit.quote_id = uint256S("56b2");
    stale_submit.commit_key = uint256S("56b6");
    stale_submit.template_commitment = uint256S("56b3");
    PaymasterSubmit fresh_submit{stale_submit};
    fresh_submit.quote_id = uint256S("56b4");
    fresh_submit.commit_key = uint256S("56b7");
    fresh_submit.template_commitment = uint256S("56b5");
    accept_and_consume(stale_submit);
    accept_and_consume(fresh_submit);

    PaymasterResultMessage stale_result;
    stale_result.request_id = request_id;
    stale_result.session_id = session_id;
    stale_result.result.provider_id = provider_id;
    stale_result.result.commit_key = uint256S("56b6");
    stale_result.result.result_sequence = 1;
    PaymasterResultMessage fresh_result{stale_result};
    fresh_result.result.commit_key = uint256S("56b7");
    accept_and_consume(stale_result);
    accept_and_consume(fresh_result);

    RecoveryWireMessages stale_recovery = RecoveryMessages(
        uint256S("56b8"), 100000, request_id, session_id, provider_id);
    RecoveryWireMessages fresh_recovery{stale_recovery};
    fresh_recovery.response.recovery_id = uint256S("56b9");
    fresh_recovery.response.recovery_commit_key = uint256S("56ba");
    fresh_recovery.submit.recovery_id = fresh_recovery.response.recovery_id;
    fresh_recovery.submit.recovery_commit_key =
        fresh_recovery.response.recovery_commit_key;
    fresh_recovery.result.recovery_id = fresh_recovery.response.recovery_id;
    fresh_recovery.result.result.commit_key =
        fresh_recovery.response.recovery_commit_key;
    accept_and_consume(stale_recovery.response);
    accept_and_consume(fresh_recovery.response);
    accept_and_consume(stale_recovery.submit);
    accept_and_consume(fresh_recovery.submit);
    accept_and_consume(stale_recovery.result);
    accept_and_consume(fresh_recovery.result);
}

BOOST_AUTO_TEST_CASE(recovery_direct_messages_obey_size_and_fair_queue_boundaries)
{
    constexpr int64_t now{100000};
    const uint256 genesis{uint256S("a300")};
    const PaymasterId provider_id{uint256S("a301")};
    const RecoveryWireMessages messages = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440302",
        uint256S("a302"), provider_id);
    const std::vector<DirectPayload> payloads{
        messages.request, messages.response, messages.submit, messages.result};

    for (size_t index{0}; index < payloads.size(); ++index) {
        Manager manager{true};
        uint256 message_id;
        message_id.begin()[0] = static_cast<unsigned char>(index + 1);
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          1, message_id, MAX_DIRECT_MESSAGE_BYTES,
                          payloads[index], now) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_CHECK(manager.EnqueueDirectMessageResult(
                        2, uint256S("a303"), MAX_DIRECT_MESSAGE_BYTES + 1,
                        payloads[index], now) ==
                    DirectEnqueueResult::INVALID);
        BOOST_REQUIRE_EQUAL(manager.TakeDirectMessages(1).size(), 1U);
    }

    const RecoveryWireMessages second = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440304",
        uint256S("a304"), provider_id);
    const RecoveryWireMessages third = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440305",
        uint256S("a305"), provider_id);
    const RecoveryWireMessages fourth = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440306",
        uint256S("a306"), provider_id);
    Manager peer_fair{true};
    BOOST_REQUIRE(peer_fair.EnqueueDirectMessage(
        1, uint256S("a310"), 1, DirectPayload{messages.request}, now));
    BOOST_REQUIRE(peer_fair.EnqueueDirectMessage(
        1, uint256S("a311"), 1, DirectPayload{second.response}, now));
    BOOST_REQUIRE(peer_fair.EnqueueDirectMessage(
        1, uint256S("a312"), 1, DirectPayload{third.submit}, now));
    BOOST_REQUIRE(peer_fair.EnqueueDirectMessage(
        2, uint256S("a313"), 1, DirectPayload{fourth.result}, now));
    const auto peer_order = peer_fair.TakeDirectMessages(4);
    BOOST_REQUIRE_EQUAL(peer_order.size(), 4U);
    BOOST_CHECK_EQUAL(peer_order[0].peer_id, 1);
    BOOST_CHECK_EQUAL(peer_order[1].peer_id, 2);
    BOOST_CHECK_EQUAL(peer_order[2].peer_id, 1);
    BOOST_CHECK_EQUAL(peer_order[3].peer_id, 1);
    BOOST_CHECK(std::holds_alternative<AlternativeRecoveryRequest>(
        peer_order[0].payload));
    BOOST_CHECK(std::holds_alternative<AlternativeRecoveryResultMessage>(
        peer_order[1].payload));

    Manager netgroup_fair{true};
    constexpr uint64_t first_netgroup{10};
    constexpr uint64_t second_netgroup{20};
    const RecoveryWireMessages fifth = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440321",
        uint256S("a321"), provider_id);
    const RecoveryWireMessages sixth = RecoveryMessages(
        genesis, now, "550e8400-e29b-41d4-a716-446655440322",
        uint256S("a322"), provider_id);
    const std::vector<DirectPayload> netgroup_payloads{
        messages.request, second.response, third.submit, fourth.result};
    BOOST_REQUIRE_EQUAL(netgroup_payloads.size(),
                        MAX_DIRECT_INBOX_MESSAGES_PER_NETGROUP);
    for (size_t index{0};
         index < MAX_DIRECT_INBOX_MESSAGES_PER_NETGROUP; ++index) {
        uint256 message_id;
        message_id.begin()[0] = static_cast<unsigned char>(index + 0x20);
        BOOST_REQUIRE(netgroup_fair.EnqueueDirectMessage(
            static_cast<int64_t>(index + 10), message_id, 1,
            netgroup_payloads[index], now, first_netgroup));
    }
    BOOST_CHECK(netgroup_fair.EnqueueDirectMessageResult(
                    50, uint256S("a323"), 1, DirectPayload{fifth.request},
                    now, first_netgroup) ==
                DirectEnqueueResult::FULL);
    BOOST_REQUIRE(netgroup_fair.EnqueueDirectMessage(
        51, uint256S("a324"), 1, DirectPayload{sixth.request},
        now, second_netgroup));

    const auto first = netgroup_fair.TakeDirectMessages(1);
    const auto second_group = netgroup_fair.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_REQUIRE_EQUAL(second_group.size(), 1U);
    BOOST_REQUIRE(first.front().keyed_netgroup.has_value());
    BOOST_REQUIRE(second_group.front().keyed_netgroup.has_value());
    BOOST_CHECK_EQUAL(*first.front().keyed_netgroup, first_netgroup);
    BOOST_CHECK_EQUAL(*second_group.front().keyed_netgroup, second_netgroup);
}

BOOST_AUTO_TEST_CASE(manager_rejects_inbox_count_and_byte_overflow)
{
    Manager manager{true};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    for (size_t i = 0; i < MAX_DIRECT_INBOX_MESSAGES_PER_PEER; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessage(1, id, 1, DirectPayload{request}, 100000));
    }
    request.request_id = uint256S("f0").GetHex();
    request.session_id = uint256S("f0");
    request.client_nonce = uint256S("f0");
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, uint256S("f0"), 1, DirectPayload{request}, 100000) ==
                DirectEnqueueResult::FULL);
    for (size_t i = MAX_DIRECT_INBOX_MESSAGES_PER_PEER;
         i < MAX_DIRECT_INBOX_MESSAGES; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessage(2, id, 1,
                                                   DirectPayload{request}, 100000));
    }
    request.request_id = uint256S("ff").GetHex();
    request.session_id = uint256S("ff");
    request.client_nonce = uint256S("ff");
    BOOST_CHECK(!manager.EnqueueDirectMessage(3, uint256S("ff"), 1,
                                              DirectPayload{request}, 100000));

    manager.ClearDirectMessages();
    request.request_id = uint256S("fd").GetHex();
    request.session_id = uint256S("fd");
    request.client_nonce = uint256S("fd");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(1, uint256S("fd"), MAX_DIRECT_MESSAGE_BYTES,
                                               DirectPayload{request}, 100000));
    request.request_id = uint256S("fe").GetHex();
    request.session_id = uint256S("fe");
    request.client_nonce = uint256S("fe");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(2, uint256S("fe"), MAX_DIRECT_MESSAGE_BYTES,
                                               DirectPayload{request}, 100000));
    request.request_id = uint256S("fc").GetHex();
    request.session_id = uint256S("fc");
    request.client_nonce = uint256S("fc");
    BOOST_CHECK(!manager.EnqueueDirectMessage(1, uint256S("fc"), 1,
                                              DirectPayload{request}, 100000));
}

BOOST_AUTO_TEST_CASE(manager_inbox_preserves_capacity_for_unrelated_sessions)
{
    Manager manager{true};
    PaymasterResultMessage result;
    result.request_id = "550e8400-e29b-41d4-a716-446655440056";
    result.session_id = uint256S("56");
    result.result.provider_id = uint256S("57");

    result.result.result_sequence = 1;
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        1, uint256S("58"), 100, DirectPayload{result}, 100000));
    result.result.result_sequence = 2;
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        2, uint256S("59"), 100, DirectPayload{result}, 100000));
    result.result.result_sequence = 3;
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    3, uint256S("5a"), 100, DirectPayload{result}, 100000) ==
                DirectEnqueueResult::FULL);

    result.request_id = "550e8400-e29b-41d4-a716-44665544005b";
    result.session_id = uint256S("5b");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        3, uint256S("5c"), 100, DirectPayload{result}, 100000));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 3U);
}

BOOST_AUTO_TEST_CASE(manager_quote_response_inbox_is_session_selective)
{
    Manager manager{true};
    PaymasterQuoteResponse first;
    first.request_id = "550e8400-e29b-41d4-a716-446655440061";
    first.session_id = uint256S("61");
    first.quote.provider_id = uint256S("62");
    first.quote.intent_hash = uint256S("67");
    PaymasterQuoteResponse second = first;
    second.request_id = "550e8400-e29b-41d4-a716-446655440063";
    second.session_id = uint256S("63");
    second.quote.provider_id = uint256S("64");
    PaymasterQuoteResponse other_attempt = first;
    other_attempt.quote.intent_hash = uint256S("68");

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        11, uint256S("65"), 100, DirectPayload{first}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        12, uint256S("66"), 120, DirectPayload{second}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      13, uint256S("69"), 110,
                      DirectPayload{other_attempt}, 100000) ==
                  DirectEnqueueResult::CONFLICT);

    const auto selected = manager.TakeQuoteResponses(
        first.request_id, first.session_id, first.quote.provider_id,
        first.quote.intent_hash, 1);
    BOOST_REQUIRE_EQUAL(selected.size(), 1U);
    BOOST_CHECK_EQUAL(selected.front().peer_id, 11);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    BOOST_CHECK(manager.TakeQuoteResponses(
                           first.request_id, first.session_id,
                           first.quote.provider_id,
                           first.quote.intent_hash, 1)
                    .empty());

    const auto selected_other_attempt = manager.TakeQuoteResponses(
        other_attempt.request_id, other_attempt.session_id,
        other_attempt.quote.provider_id, other_attempt.quote.intent_hash, 1);
    BOOST_REQUIRE_EQUAL(selected_other_attempt.size(), 1U);
    BOOST_CHECK_EQUAL(selected_other_attempt.front().peer_id, 13);

    const auto remaining = manager.TakeDirectMessages(1);
    BOOST_REQUIRE_EQUAL(remaining.size(), 1U);
    const auto* response = std::get_if<PaymasterQuoteResponse>(&remaining.front().payload);
    BOOST_REQUIRE(response != nullptr);
    BOOST_CHECK_EQUAL(response->request_id, second.request_id);
}

BOOST_AUTO_TEST_CASE(manager_quote_request_inbox_is_provider_wallet_selective)
{
    Manager manager{true};
    PaymasterQuoteRequest first;
    first.intent = Intent(uint256S("67"), 100000, SponsorshipScope::PUBLIC);
    first.intent.provider_id = uint256S("68");
    PaymasterQuoteRequest second = first;
    second.intent.provider_id = uint256S("69");

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        21, uint256S("6a"), 100, DirectPayload{first}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        22, uint256S("6b"), 100, DirectPayload{second}, 100000));

    const auto selected = manager.TakeQuoteRequests(first.intent.provider_id, 1);
    BOOST_REQUIRE_EQUAL(selected.size(), 1U);
    BOOST_CHECK_EQUAL(selected.front().peer_id, 21);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_CHECK(manager.TakeQuoteRequests(first.intent.provider_id, 1).empty());

    const auto remaining = manager.TakeQuoteRequests(second.intent.provider_id, 1);
    BOOST_REQUIRE_EQUAL(remaining.size(), 1U);
    BOOST_CHECK_EQUAL(remaining.front().peer_id, 22);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
}

BOOST_AUTO_TEST_CASE(manager_capacity_queues_are_handshake_selective)
{
    Manager manager{true};
    PaymasterCapacityRequest first_request;
    first_request.funding_model = FundingModel::SPONSORED;
    first_request.genesis_hash = uint256S("70");
    first_request.provider_id = uint256S("71");
    first_request.request_id = "550e8400-e29b-41d4-a716-446655440072";
    first_request.session_id = uint256S("72");
    first_request.client_nonce = uint256S("72");
    first_request.created_at = 100000;
    first_request.expires_at = 100060;
    PaymasterCapacityRequest second_request{first_request};
    second_request.provider_id = uint256S("73");
    second_request.request_id = "550e8400-e29b-41d4-a716-446655440074";
    second_request.session_id = uint256S("74");
    second_request.client_nonce = uint256S("74");

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        21, uint256S("75"), 100, DirectPayload{first_request}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        22, uint256S("76"), 100, DirectPayload{second_request}, 100000));
    const auto requests = manager.TakeCapacityRequests(first_request.provider_id, 1);
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_CHECK_EQUAL(requests.front().peer_id, 21);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    PaymasterCapacityProof first_proof;
    first_proof.funding_model = first_request.funding_model;
    first_proof.provider_id = first_request.provider_id;
    first_proof.request_id = first_request.request_id;
    first_proof.session_id = first_request.session_id;
    first_proof.client_nonce = first_request.client_nonce;
    PaymasterCapacityProof second_proof;
    second_proof.funding_model = second_request.funding_model;
    second_proof.provider_id = second_request.provider_id;
    second_proof.request_id = second_request.request_id;
    second_proof.session_id = second_request.session_id;
    second_proof.client_nonce = second_request.client_nonce;
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        21, uint256S("77"), 100, DirectPayload{first_proof}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        22, uint256S("78"), 100, DirectPayload{second_proof}, 100000));

    const auto proofs = manager.TakeCapacityProofs(
        first_request.provider_id, first_request.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(proofs.size(), 1U);
    BOOST_CHECK_EQUAL(proofs.front().peer_id, 21);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    BOOST_CHECK(manager.TakeCapacityProofs(
                           first_request.provider_id, second_request.client_nonce, 1)
                    .empty());

    BOOST_REQUIRE(manager.QueueCapacityRequest(
        31, uint256S("79"), 100, first_request, 100000));
    BOOST_REQUIRE(manager.QueueCapacityProof(
        31, uint256S("7a"), 100, first_proof, 100000));
    const auto outbound = manager.TakeOutboundDirectMessages(31, 2, 100001);
    BOOST_REQUIRE_EQUAL(outbound.size(), 2U);
    BOOST_CHECK(std::holds_alternative<PaymasterCapacityRequest>(outbound[0].payload));
    BOOST_CHECK(std::holds_alternative<PaymasterCapacityProof>(outbound[1].payload));
}

BOOST_AUTO_TEST_CASE(manager_capacity_proof_peek_requires_exact_ack)
{
    Manager manager{true};
    const PaymasterId provider_id{uint256S("7b01")};
    const uint256 client_nonce{uint256S("7b02")};
    PaymasterCapacityProof first;
    first.provider_id = provider_id;
    first.request_id = "550e8400-e29b-41d4-a716-446655447b01";
    first.session_id = uint256S("7b03");
    first.client_nonce = client_nonce;
    PaymasterCapacityProof second{first};
    second.request_id = "550e8400-e29b-41d4-a716-446655447b02";
    second.session_id = uint256S("7b04");
    PaymasterCapacityProof third{first};
    third.request_id = "550e8400-e29b-41d4-a716-446655447b03";
    third.session_id = uint256S("7b05");
    const uint256 first_id{uint256S("7b11")};
    const uint256 second_id{uint256S("7b12")};
    const uint256 third_id{uint256S("7b13")};

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        11, first_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{first},
        100000, uint64_t{10}));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        22, second_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{second},
        100000, uint64_t{20}));
    BOOST_CHECK(manager.HasDirectMessages());

    const auto first_peek =
        manager.PeekCapacityProofs(provider_id, client_nonce, 1);
    const auto repeated_peek =
        manager.PeekCapacityProofs(provider_id, client_nonce, 1);
    BOOST_REQUIRE_EQUAL(first_peek.size(), 1U);
    BOOST_REQUIRE_EQUAL(repeated_peek.size(), 1U);
    BOOST_CHECK(first_peek.front().message_id == first_id);
    BOOST_CHECK(repeated_peek.front().message_id == first_id);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);

    // A mixed valid/invalid acknowledgement is atomic: the valid message must
    // remain queued when any requested content hash is unknown.
    BOOST_CHECK(!manager.AcknowledgeDirectMessages(
        {first_id, uint256S("7bff")}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    const auto retained =
        manager.PeekCapacityProofs(provider_id, client_nonce, 1);
    BOOST_REQUIRE_EQUAL(retained.size(), 1U);
    BOOST_CHECK(retained.front().message_id == first_id);

    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({first_id}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    // Releasing the first maximum-sized entry must restore the global byte
    // allowance without weakening per-peer, per-group, or per-session limits.
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        33, third_id, MAX_DIRECT_MESSAGE_BYTES, DirectPayload{third},
        100000, uint64_t{30}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);

    const auto next_group =
        manager.PeekCapacityProofs(provider_id, client_nonce, 1);
    BOOST_REQUIRE_EQUAL(next_group.size(), 1U);
    BOOST_CHECK(next_group.front().message_id == second_id);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({second_id}));
    const auto final_group =
        manager.PeekCapacityProofs(provider_id, client_nonce, 1);
    BOOST_REQUIRE_EQUAL(final_group.size(), 1U);
    BOOST_CHECK(final_group.front().message_id == third_id);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({third_id}));
    BOOST_CHECK(!manager.HasDirectMessages());
    BOOST_CHECK(!manager.AcknowledgeDirectMessages({third_id}));
}

BOOST_AUTO_TEST_CASE(manager_quote_response_peek_requires_exact_ack)
{
    Manager manager{true};
    PaymasterQuoteResponse response;
    response.request_id = "550e8400-e29b-41d4-a716-446655447c01";
    response.session_id = uint256S("7c01");
    response.quote.provider_id = uint256S("7c02");
    response.quote.intent_hash = uint256S("7c03");
    const uint256 message_id{uint256S("7c04")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        41, message_id, 100, DirectPayload{response}, 100000,
        uint64_t{40}));

    const auto first = manager.PeekQuoteResponses(
        response.request_id, response.session_id,
        response.quote.provider_id, response.quote.intent_hash, 1);
    const auto repeated = manager.PeekQuoteResponses(
        response.request_id, response.session_id,
        response.quote.provider_id, response.quote.intent_hash, 1);
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_REQUIRE_EQUAL(repeated.size(), 1U);
    BOOST_CHECK(first.front().message_id == message_id);
    BOOST_CHECK(repeated.front().message_id == message_id);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    BOOST_CHECK(!manager.AcknowledgeDirectMessages({uint256S("7cff")}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({message_id}));
    BOOST_CHECK(manager.PeekQuoteResponses(
                           response.request_id, response.session_id,
                           response.quote.provider_id,
                           response.quote.intent_hash, 1)
                    .empty());
}

BOOST_AUTO_TEST_CASE(manager_idempotent_ack_tolerates_concurrent_release)
{
    Manager manager{true};
    PaymasterCapacityProof first;
    first.provider_id = uint256S("7c10");
    first.request_id = "550e8400-e29b-41d4-a716-446655447c10";
    first.session_id = uint256S("7c11");
    first.client_nonce = uint256S("7c12");
    PaymasterCapacityProof second{first};
    second.request_id = "550e8400-e29b-41d4-a716-446655447c13";
    second.session_id = uint256S("7c13");
    const uint256 first_id{uint256S("7c14")};
    const uint256 second_id{uint256S("7c15")};
    const uint256 missing_id{uint256S("7cff")};

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        51, first_id, 100, DirectPayload{first}, 100000,
        uint64_t{50}));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        52, second_id, 100, DirectPayload{second}, 100000,
        uint64_t{60}));

    // The strict API remains atomic for callers that require an exact lease.
    BOOST_CHECK(!manager.AcknowledgeDirectMessages({first_id, missing_id}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);

    // Durable consumers may race with another consumer or TTL pruning. The
    // idempotent API removes what remains and treats already absent ids as a
    // successful acknowledgement of the durable work.
    BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent(
        {first_id, missing_id}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent(
        {first_id, missing_id}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_CHECK(!manager.AcknowledgeDirectMessagesIfPresent(
        {second_id, second_id}));
    BOOST_CHECK(!manager.AcknowledgeDirectMessagesIfPresent(
        {second_id, uint256{}}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent({second_id}));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_recovery_capacity_peek_requires_exact_ack)
{
    Manager manager{true};
    const RecoveryWireMessages recovery = RecoveryMessages(
        uint256S("7d01"), 100000,
        "550e8400-e29b-41d4-a716-446655447d01", uint256S("7d02"),
        uint256S("7d03"));
    PaymasterCapacityProof proof;
    proof.genesis_hash = recovery.request.capacity_request.genesis_hash;
    proof.provider_id = recovery.request.recovery_provider_id;
    proof.request_id = recovery.request.request_id;
    proof.session_id = recovery.request.session_id;
    proof.client_nonce = recovery.request.client_nonce;
    proof.funding_model = recovery.request.capacity_request.funding_model;
    const uint256 message_id{uint256S("7d04")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        51, message_id, 100, DirectPayload{proof}, 100000,
        uint64_t{50}));

    const auto first = manager.PeekCapacityProofs(
        recovery.request.recovery_provider_id,
        recovery.request.client_nonce, 1);
    const auto repeated = manager.PeekCapacityProofs(
        recovery.request.recovery_provider_id,
        recovery.request.client_nonce, 1);
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_REQUIRE_EQUAL(repeated.size(), 1U);
    BOOST_CHECK(first.front().message_id == message_id);
    BOOST_CHECK(repeated.front().message_id == message_id);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    BOOST_CHECK(!manager.AcknowledgeDirectMessages({uint256S("7dff")}));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({message_id}));
    BOOST_CHECK(!manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_equivocation_candidate_fast_path_is_selective)
{
    Manager manager{true};
    BOOST_CHECK(!manager.HasDirectMessages());
    BOOST_CHECK(!manager.HasEquivocationCandidates());

    PaymasterCapacityRequest request;
    request.provider_id = uint256S("7e01");
    request.request_id = "550e8400-e29b-41d4-a716-446655447e01";
    request.session_id = uint256S("7e02");
    request.client_nonce = uint256S("7e03");
    const uint256 request_id{uint256S("7e11")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        61, request_id, 100, DirectPayload{request}, 100000));

    PaymasterSubmit submit;
    submit.provider_id = uint256S("7e04");
    submit.request_id = "550e8400-e29b-41d4-a716-446655447e02";
    submit.session_id = uint256S("7e05");
    const uint256 submit_id{uint256S("7e12")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        62, submit_id, 100, DirectPayload{submit}, 100000));
    BOOST_CHECK(manager.HasDirectMessages());
    BOOST_CHECK(!manager.HasEquivocationCandidates());

    PaymasterCapacityProof proof;
    proof.provider_id = uint256S("7e06");
    proof.request_id = "550e8400-e29b-41d4-a716-446655447e03";
    proof.session_id = uint256S("7e07");
    proof.client_nonce = uint256S("7e08");
    const uint256 proof_id{uint256S("7e13")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        63, proof_id, 100, DirectPayload{proof}, 100000));
    BOOST_CHECK(manager.HasEquivocationCandidates());
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({proof_id}));
    BOOST_CHECK(!manager.HasEquivocationCandidates());

    PaymasterQuoteResponse quote;
    quote.request_id = "550e8400-e29b-41d4-a716-446655447e04";
    quote.session_id = uint256S("7e09");
    quote.quote.provider_id = uint256S("7e0a");
    quote.quote.intent_hash = uint256S("7e0b");
    const uint256 quote_id{uint256S("7e14")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        64, quote_id, 100, DirectPayload{quote}, 100000));
    BOOST_CHECK(manager.HasEquivocationCandidates());
    BOOST_REQUIRE(manager.AcknowledgeDirectMessages({quote_id}));
    BOOST_CHECK(!manager.HasEquivocationCandidates());
    BOOST_CHECK(manager.HasDirectMessages());
}

BOOST_AUTO_TEST_CASE(manager_submit_and_result_inboxes_are_owner_selective)
{
    Manager manager{true};
    PaymasterSubmit first_submit;
    first_submit.provider_id = uint256S("81");
    PaymasterSubmit second_submit;
    second_submit.provider_id = uint256S("82");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        31, uint256S("83"), 100, DirectPayload{first_submit}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        32, uint256S("84"), 100, DirectPayload{second_submit}, 100000));
    const auto submits = manager.TakeSubmits(first_submit.provider_id, 1);
    BOOST_REQUIRE_EQUAL(submits.size(), 1U);
    BOOST_CHECK_EQUAL(submits.front().peer_id, 31);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);

    PaymasterResultMessage first_result;
    first_result.request_id = "550e8400-e29b-41d4-a716-446655440085";
    first_result.session_id = uint256S("85");
    first_result.result.provider_id = uint256S("86");
    PaymasterResultMessage second_result = first_result;
    second_result.request_id = "550e8400-e29b-41d4-a716-446655440087";
    second_result.session_id = uint256S("87");
    second_result.result.provider_id = uint256S("88");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        33, uint256S("89"), 100, DirectPayload{first_result}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        34, uint256S("8a"), 100, DirectPayload{second_result}, 100000));
    const auto results = manager.TakeResults(
        first_result.request_id, first_result.session_id,
        first_result.result.provider_id, 1);
    BOOST_REQUIRE_EQUAL(results.size(), 1U);
    BOOST_CHECK_EQUAL(results.front().peer_id, 33);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);

    BOOST_CHECK(manager.TakeResults(
                           first_result.request_id, first_result.session_id,
                           first_result.result.provider_id, 1)
                    .empty());
    const auto remaining = manager.TakeDirectMessages(2);
    BOOST_REQUIRE_EQUAL(remaining.size(), 2U);
    // Fair queueing rotates keyed netgroups, so cross-group delivery order is
    // deliberately unspecified. Both owner-selective leftovers must survive.
    const bool slot0_submit = std::holds_alternative<PaymasterSubmit>(remaining[0].payload);
    const bool slot1_submit = std::holds_alternative<PaymasterSubmit>(remaining[1].payload);
    const bool slot0_result = std::holds_alternative<PaymasterResultMessage>(remaining[0].payload);
    const bool slot1_result = std::holds_alternative<PaymasterResultMessage>(remaining[1].payload);
    BOOST_CHECK(slot0_submit != slot1_submit);
    BOOST_CHECK(slot0_result != slot1_result);
}

BOOST_AUTO_TEST_CASE(manager_outbox_is_peer_scoped_bounded_and_expires)
{
    Manager manager{true};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.genesis_hash = uint256S("61");
    request.provider_id = uint256S("62");
    request.request_id = "550e8400-e29b-41d4-a716-446655440063";
    request.session_id = uint256S("63");
    request.client_nonce = uint256S("63");
    request.created_at = 100000;
    request.expires_at = 100060;

    BOOST_REQUIRE(manager.QueueOutboundDirectMessage(
        11, uint256S("64"), 100, DirectPayload{request}, 100000));
    BOOST_CHECK(!manager.QueueOutboundDirectMessage(
        11, uint256S("64"), 100, DirectPayload{request}, 100000));
    // The same durable payload may be retried on a replacement direct peer.
    BOOST_REQUIRE(manager.QueueOutboundDirectMessage(
        12, uint256S("64"), 100, DirectPayload{request}, 100000));
    BOOST_CHECK(!manager.HasOutboundDirectMessage(11, uint256S("64")));
    BOOST_CHECK(manager.HasOutboundDirectMessage(12, uint256S("64")));
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1U);

    auto first = manager.TakeOutboundDirectMessages(11, 2, 100001);
    BOOST_CHECK(first.empty());
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1U);
    BOOST_CHECK(!manager.HasOutboundDirectMessage(11, uint256S("64")));
    BOOST_CHECK(manager.HasOutboundDirectMessage(12, uint256S("64")));
    BOOST_CHECK(manager.TakeOutboundDirectMessages(11, 2, 100002).empty());

    auto dispatched = manager.TakeOutboundDirectMessages(12, 1, 100002);
    BOOST_REQUIRE_EQUAL(dispatched.size(), 1U);
    // Exact status-poll retries on the same live peer report success while the
    // first copy is in flight, but do not consume another outbox slot.
    BOOST_CHECK(manager.QueueOutboundDirectMessage(
        12, uint256S("64"), 100, DirectPayload{request}, 100002));
    BOOST_CHECK(!manager.HasOutboundDirectMessage(12, uint256S("64")));
    // A replacement peer is never delayed, and the same peer may retry once
    // the bounded in-flight interval elapsed.
    BOOST_CHECK(manager.QueueOutboundDirectMessage(
        14, uint256S("64"), 100, DirectPayload{request}, 100002));
    BOOST_CHECK(manager.HasOutboundDirectMessage(14, uint256S("64")));
    BOOST_CHECK(manager.QueueOutboundDirectMessage(
        12, uint256S("64"), 100, DirectPayload{request}, 100003));
    BOOST_CHECK(manager.HasOutboundDirectMessage(12, uint256S("64")));

    auto expired = manager.TakeOutboundDirectMessages(
        12, 2, 100003 + MAX_DIRECT_MESSAGE_TTL_SECONDS);
    BOOST_CHECK(expired.empty());
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0U);

    const int64_t near_max = std::numeric_limits<int64_t>::max() - 10;
    BOOST_REQUIRE(manager.QueueOutboundDirectMessage(
        13, uint256S("65"), 100, DirectPayload{request}, near_max));
    const auto saturated = manager.TakeOutboundDirectMessages(13, 1, near_max);
    BOOST_REQUIRE_EQUAL(saturated.size(), 1U);

    for (size_t i = 0; i < MAX_DIRECT_INBOX_MESSAGES_PER_PEER; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(
            11, id, 1, DirectPayload{request}, 101000));
    }
    BOOST_CHECK(!manager.QueueOutboundDirectMessage(11, uint256S("f0"), 1,
                                                    DirectPayload{request}, 101000));
    for (size_t i = MAX_DIRECT_INBOX_MESSAGES_PER_PEER;
         i < MAX_DIRECT_INBOX_MESSAGES; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(
            12, id, 1, DirectPayload{request}, 101000));
    }
    BOOST_CHECK(!manager.QueueOutboundDirectMessage(13, uint256S("ff"), 1,
                                                    DirectPayload{request}, 101000));
    manager.SetEnabled(false);
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0U);
}

BOOST_AUTO_TEST_CASE(manager_transport_limits_survive_peer_reconnects_by_netgroup)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{12345};

    for (size_t i = 0; i < MAX_DIRECT_TRANSPORT_MESSAGES_PER_PEER; ++i) {
        BOOST_REQUIRE(manager.AdmitDirectTransport(1, netgroup, now));
    }
    BOOST_CHECK(!manager.AdmitDirectTransport(1, netgroup, now));
    for (size_t i = MAX_DIRECT_TRANSPORT_MESSAGES_PER_PEER;
         i < MAX_DIRECT_TRANSPORT_MESSAGES_PER_NETGROUP; ++i) {
        BOOST_REQUIRE(manager.AdmitDirectTransport(2, netgroup, now));
    }
    // A replacement peer id cannot reset the keyed-netgroup budget.
    BOOST_CHECK(!manager.AdmitDirectTransport(3, netgroup, now));
    BOOST_REQUIRE(manager.AdmitDirectTransport(
        3, netgroup, SaturatingAddSeconds(now, PAYMASTER_RATE_WINDOW_SECONDS)));

    // A backwards wall-clock adjustment cannot reopen an exhausted window.
    for (size_t i = 1; i < MAX_DIRECT_TRANSPORT_MESSAGES_PER_PEER; ++i) {
        BOOST_REQUIRE(manager.AdmitDirectTransport(
            3, netgroup, SaturatingAddSeconds(now, PAYMASTER_RATE_WINDOW_SECONDS)));
    }
    BOOST_CHECK(!manager.AdmitDirectTransport(3, netgroup, now));
}

BOOST_AUTO_TEST_CASE(manager_decoded_token_buckets_burst_refill_and_clock_safety)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{24680};
    const PaymasterId provider_id{uint256S("a101")};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = provider_id;
    PaymasterCapacityRequest first_request;
    uint256 first_id;

    for (size_t i = 0; i < MAX_DIRECT_PAYLOAD_MESSAGES_PER_PEER; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          1, id, 1, DirectPayload{request}, now, netgroup) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(provider_id, 1).size(), 1U);
        if (i == 0) {
            first_id = id;
            first_request = request;
        }
    }

    // A consumed exact semantic retry remains idempotent, but cannot bypass
    // an exhausted decoded-message bucket after reconnecting.
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, first_id, 1, DirectPayload{first_request}, now, netgroup) ==
                DirectEnqueueResult::RATE_LIMITED);
    BOOST_CHECK(manager.TakeCapacityRequests(provider_id, 1).empty());

    const auto enqueue_new = [&](const uint256& id, int64_t at) {
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        return manager.EnqueueDirectMessageResult(
            1, id, 1, DirectPayload{request}, at, netgroup);
    };
    BOOST_CHECK(enqueue_new(uint256S("a102"), now) == DirectEnqueueResult::RATE_LIMITED);
    BOOST_CHECK(enqueue_new(uint256S("a102"), now + 3) == DirectEnqueueResult::RATE_LIMITED);
    BOOST_REQUIRE(enqueue_new(uint256S("a102"), now + 4) == DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(provider_id, 1).size(), 1U);

    // Moving wall time backwards cannot mint tokens past the monotonic high
    // water mark. A normal refill interval restores exactly one peer token.
    BOOST_CHECK(enqueue_new(uint256S("a103"), now) == DirectEnqueueResult::RATE_LIMITED);
    BOOST_REQUIRE(enqueue_new(uint256S("a103"), now + 8) == DirectEnqueueResult::ACCEPTED);
}

BOOST_AUTO_TEST_CASE(manager_token_bucket_maps_fail_closed_and_prune_full_lru_entries)
{
    Manager manager{true};
    constexpr int64_t now{200000};

    for (size_t i = 0; i < MAX_DIRECT_PEER_TOKEN_BUCKETS; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>((i % 255) + 1);
        id.begin()[1] = static_cast<unsigned char>((i / 255) + 1);
        PaymasterCapacityRequest request;
        request.funding_model = FundingModel::SPONSORED;
        request.provider_id = id;
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          static_cast<int64_t>(i), id, 1, DirectPayload{request}, now,
                          static_cast<uint64_t>(i + 1)) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(request.provider_id, 1).size(), 1U);
    }

    PaymasterCapacityRequest overflow;
    overflow.funding_model = FundingModel::SPONSORED;
    overflow.provider_id = uint256S("b101");
    overflow.request_id = uint256S("b102").GetHex();
    overflow.session_id = uint256S("b102");
    overflow.client_nonce = uint256S("b102");
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    10000, uint256S("b102"), 1, DirectPayload{overflow}, now,
                    uint64_t{10000}) == DirectEnqueueResult::RATE_LIMITED);

    // After a complete refill, a full least-recently-used bucket can be
    // discarded safely; partially exhausted buckets are never evicted early.
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      10000, uint256S("b102"), 1, DirectPayload{overflow},
                      now + DIRECT_TOKEN_BUCKET_PERIOD_SECONDS, uint64_t{10000}) ==
                  DirectEnqueueResult::ACCEPTED);
}

BOOST_AUTO_TEST_CASE(manager_token_bucket_refill_saturates_near_time_maximum)
{
    Manager manager{true};
    const int64_t near_max{std::numeric_limits<int64_t>::max() - 10};
    constexpr uint64_t netgroup{35791};

    for (size_t i = 0; i < MAX_DIRECT_PAYLOAD_MESSAGES_PER_PEER; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        PaymasterCapacityRequest request;
        request.funding_model = FundingModel::SPONSORED;
        request.provider_id = uint256S("c101");
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          7, id, 1, DirectPayload{request}, near_max, netgroup) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(request.provider_id, 1).size(), 1U);
    }

    const auto enqueue_at_max = [&](const uint256& id, int64_t at) {
        PaymasterCapacityRequest request;
        request.funding_model = FundingModel::SPONSORED;
        request.provider_id = uint256S("c101");
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        return manager.EnqueueDirectMessageResult(
            7, id, 1, DirectPayload{request}, at, netgroup);
    };
    BOOST_REQUIRE(enqueue_at_max(uint256S("c102"), std::numeric_limits<int64_t>::max()) ==
                  DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(uint256S("c101"), 1).size(), 1U);
    BOOST_REQUIRE(enqueue_at_max(uint256S("c103"), std::numeric_limits<int64_t>::max()) ==
                  DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(uint256S("c101"), 1).size(), 1U);
    BOOST_CHECK(enqueue_at_max(uint256S("c104"), std::numeric_limits<int64_t>::max()) ==
                DirectEnqueueResult::RATE_LIMITED);
    BOOST_CHECK(enqueue_at_max(uint256S("c104"), near_max) ==
                DirectEnqueueResult::RATE_LIMITED);
}

BOOST_AUTO_TEST_CASE(manager_announcement_limits_apply_before_expensive_validation)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{67890};
    size_t admitted{0};
    int64_t peer_id{1};
    while (admitted < MAX_ANNOUNCEMENTS_PER_NETGROUP_PER_WINDOW) {
        const size_t peer_allowance = std::min(
            MAX_ANNOUNCEMENTS_PER_PEER_PER_WINDOW,
            MAX_ANNOUNCEMENTS_PER_NETGROUP_PER_WINDOW - admitted);
        for (size_t i = 0; i < peer_allowance; ++i) {
            BOOST_REQUIRE(manager.AdmitAnnouncementTransport(peer_id, netgroup, now));
        }
        admitted += peer_allowance;
        ++peer_id;
    }
    BOOST_CHECK(!manager.AdmitAnnouncementTransport(peer_id, netgroup, now));

    const PaymasterId provider_id{uint256S("91")};
    for (size_t i = 0; i < MAX_ANNOUNCEMENTS_PER_PROVIDER_PER_WINDOW; ++i) {
        BOOST_REQUIRE(manager.AdmitAnnouncementProvider(provider_id, now));
    }
    BOOST_CHECK(!manager.AdmitAnnouncementProvider(provider_id, now));
    BOOST_REQUIRE(manager.AdmitAnnouncementProvider(
        provider_id, SaturatingAddSeconds(now, PAYMASTER_RATE_WINDOW_SECONDS)));
}

BOOST_AUTO_TEST_CASE(manager_enforces_wallet_quote_rate_per_provider_and_netgroup)
{
    Manager manager{true};
    const PaymasterId provider{uint256S("f101")};
    constexpr uint64_t first_group{41};
    constexpr uint64_t second_group{42};
    constexpr uint32_t limit{2};

    BOOST_CHECK(manager.AdmitProviderQuoteRequest(provider, first_group, limit, 100000));
    BOOST_CHECK(manager.AdmitProviderQuoteRequest(provider, first_group, limit, 100001));
    BOOST_CHECK(!manager.AdmitProviderQuoteRequest(provider, first_group, limit, 100002));
    BOOST_CHECK(manager.AdmitProviderQuoteRequest(provider, second_group, limit, 100002));
    BOOST_CHECK(manager.AdmitProviderQuoteRequest(uint256S("f102"), first_group, limit, 100002));
    BOOST_CHECK(!manager.AdmitProviderQuoteRequest(provider, first_group, 0, 100002));

    // The monotonic window cannot be bypassed by moving wall time backwards,
    // but expires normally after one minute.
    BOOST_CHECK(!manager.AdmitProviderQuoteRequest(provider, first_group, limit, 99999));
    BOOST_CHECK(manager.AdmitProviderQuoteRequest(provider, first_group, limit, 100061));
}

BOOST_AUTO_TEST_CASE(manager_provider_and_session_rates_bound_drained_queues)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    const PaymasterId provider_id{uint256S("92")};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = provider_id;
    for (size_t i = 0; i < MAX_DIRECT_MESSAGES_PER_PROVIDER_PER_WINDOW; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          static_cast<int64_t>(i + 1), id, 1,
                          DirectPayload{request}, now,
                          static_cast<uint64_t>(i + 1)) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(provider_id, 1).size(), 1U);
    }
    uint256 exact_retry_id;
    exact_retry_id.begin()[0] = 1;
    request.request_id = exact_retry_id.GetHex();
    request.session_id = exact_retry_id;
    request.client_nonce = exact_retry_id;
    // A consumed exact replay remains idempotent, but it is still remote work
    // and must not bypass the provider token bucket after reconnecting.
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, exact_retry_id, 1, DirectPayload{request}, now) ==
                DirectEnqueueResult::RATE_LIMITED);
    BOOST_CHECK(manager.TakeCapacityRequests(provider_id, 1).empty());

    request.request_id = uint256S("f1").GetHex();
    request.session_id = uint256S("f1");
    request.client_nonce = uint256S("f1");
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    2, uint256S("f1"), 1, DirectPayload{request}, now) ==
                DirectEnqueueResult::RATE_LIMITED);
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      2, uint256S("f1"), 1, DirectPayload{request},
                      SaturatingAddSeconds(now, PAYMASTER_RATE_WINDOW_SECONDS)) ==
                  DirectEnqueueResult::ACCEPTED);

    Manager session_manager{true};
    PaymasterResultMessage result;
    result.request_id = "550e8400-e29b-41d4-a716-446655440093";
    result.session_id = uint256S("93");
    result.result.provider_id = uint256S("94");
    for (size_t i = 0; i < MAX_DIRECT_MESSAGES_PER_SESSION_PER_WINDOW; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        result.result.result_sequence = i + 1;
        BOOST_REQUIRE(session_manager.EnqueueDirectMessageResult(
                          static_cast<int64_t>(i + 1), id, 1,
                          DirectPayload{result}, now,
                          static_cast<uint64_t>(i + 1)) ==
                      DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(session_manager.TakeResults(
                                               result.request_id, result.session_id,
                                               result.result.provider_id, 1)
                                .size(),
                            1U);
    }
    uint256 exact_session_retry_id;
    exact_session_retry_id.begin()[0] = 1;
    result.result.result_sequence = 1;
    BOOST_CHECK(session_manager.EnqueueDirectMessageResult(
                    100, exact_session_retry_id, 1, DirectPayload{result}, now,
                    uint64_t{100}) == DirectEnqueueResult::RATE_LIMITED);
    BOOST_CHECK(session_manager.TakeResults(
                                   result.request_id, result.session_id,
                                   result.result.provider_id, 1)
                    .empty());
    result.result.result_sequence =
        MAX_DIRECT_MESSAGES_PER_SESSION_PER_WINDOW + 1;
    BOOST_CHECK(session_manager.EnqueueDirectMessageResult(
                    2, uint256S("f2"), 1, DirectPayload{result}, now) ==
                DirectEnqueueResult::RATE_LIMITED);
}

BOOST_AUTO_TEST_CASE(manager_selective_inbox_uses_peer_fair_queueing)
{
    Manager manager{true};
    const PaymasterId provider_id{uint256S("95")};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = provider_id;
    for (unsigned char value = 1; value <= 3; ++value) {
        uint256 id;
        id.begin()[0] = value;
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessage(
            1, id, 1, DirectPayload{request}, 100000));
    }
    request.request_id = uint256S("96").GetHex();
    request.session_id = uint256S("96");
    request.client_nonce = uint256S("96");
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        2, uint256S("96"), 1, DirectPayload{request}, 100000));

    const auto first = manager.TakeCapacityRequests(provider_id, 1);
    const auto second = manager.TakeCapacityRequests(provider_id, 1);
    const auto third = manager.TakeCapacityRequests(provider_id, 1);
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_REQUIRE_EQUAL(second.size(), 1U);
    BOOST_REQUIRE_EQUAL(third.size(), 1U);
    BOOST_CHECK_EQUAL(first.front().peer_id, 1);
    BOOST_CHECK_EQUAL(second.front().peer_id, 2);
    BOOST_CHECK_EQUAL(third.front().peer_id, 1);
}

BOOST_AUTO_TEST_CASE(manager_direct_inbox_rotates_sessions_within_one_netgroup)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{701};
    const PaymasterId provider_id{uint256S("b701")};

    PaymasterCapacityRequest request_a;
    request_a.provider_id = provider_id;
    request_a.request_id = "550e8400-e29b-41d4-a716-446655440701";
    request_a.session_id = uint256S("b702");
    request_a.client_nonce = uint256S("b703");
    PaymasterCapacityProof proof_a;
    proof_a.provider_id = provider_id;
    proof_a.request_id = request_a.request_id;
    proof_a.session_id = request_a.session_id;
    proof_a.client_nonce = request_a.client_nonce;

    PaymasterCapacityRequest request_b{request_a};
    request_b.request_id = "550e8400-e29b-41d4-a716-446655440704";
    request_b.session_id = uint256S("b704");
    request_b.client_nonce = uint256S("b705");
    PaymasterCapacityProof proof_b{proof_a};
    proof_b.request_id = request_b.request_id;
    proof_b.session_id = request_b.session_id;
    proof_b.client_nonce = request_b.client_nonce;

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        11, uint256S("b711"), 1, DirectPayload{request_a}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        111, uint256S("b712"), 1, DirectPayload{proof_a}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        22, uint256S("b713"), 1, DirectPayload{request_b}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        122, uint256S("b714"), 1, DirectPayload{proof_b}, now, netgroup));

    CheckTwoSessionRoundRobin(manager.TakeDirectMessages(4), netgroup);
}

BOOST_AUTO_TEST_CASE(manager_submit_inbox_rotates_sessions_within_one_netgroup)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{702};
    const PaymasterId provider_id{uint256S("b721")};

    PaymasterSubmit submit_a;
    submit_a.provider_id = provider_id;
    submit_a.request_id = "550e8400-e29b-41d4-a716-446655440721";
    submit_a.session_id = uint256S("b722");
    submit_a.quote_id = uint256S("b723");
    submit_a.commit_key = uint256S("b724");
    submit_a.template_commitment = uint256S("b725");
    PaymasterSubmit retry_a{submit_a};
    retry_a.quote_id = uint256S("b726");
    retry_a.commit_key = uint256S("b727");
    retry_a.template_commitment = uint256S("b728");

    PaymasterSubmit submit_b{submit_a};
    submit_b.request_id = "550e8400-e29b-41d4-a716-446655440729";
    submit_b.session_id = uint256S("b729");
    submit_b.quote_id = uint256S("b72a");
    submit_b.commit_key = uint256S("b72b");
    submit_b.template_commitment = uint256S("b72c");
    PaymasterSubmit retry_b{submit_b};
    retry_b.quote_id = uint256S("b72d");
    retry_b.commit_key = uint256S("b72e");
    retry_b.template_commitment = uint256S("b72f");

    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        11, uint256S("b731"), 1, DirectPayload{submit_a}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        111, uint256S("b732"), 1, DirectPayload{retry_a}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        22, uint256S("b733"), 1, DirectPayload{submit_b}, now, netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        122, uint256S("b734"), 1, DirectPayload{retry_b}, now, netgroup));

    CheckTwoSessionRoundRobin(manager.TakeSubmits(provider_id, 4), netgroup);
}

BOOST_AUTO_TEST_CASE(manager_result_inbox_rotates_sessions_within_one_netgroup)
{
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{703};
    const PaymasterId provider_id{uint256S("b741")};

    PaymasterResultMessage result_a;
    result_a.request_id = "550e8400-e29b-41d4-a716-446655440741";
    result_a.session_id = uint256S("b742");
    result_a.result.provider_id = provider_id;
    result_a.result.commit_key = uint256S("b743");
    result_a.result.result_sequence = 1;
    PaymasterResultMessage update_a{result_a};
    update_a.result.result_sequence = 2;

    PaymasterResultMessage result_b{result_a};
    result_b.request_id = "550e8400-e29b-41d4-a716-446655440744";
    result_b.session_id = uint256S("b744");
    result_b.result.commit_key = uint256S("b745");
    PaymasterResultMessage update_b{result_b};
    update_b.result.result_sequence = 2;

    const auto enqueue_results = [&](Manager& manager) {
        BOOST_REQUIRE(manager.EnqueueDirectMessage(
            11, uint256S("b751"), 1, DirectPayload{result_a}, now, netgroup));
        BOOST_REQUIRE(manager.EnqueueDirectMessage(
            111, uint256S("b752"), 1, DirectPayload{update_a}, now, netgroup));
        BOOST_REQUIRE(manager.EnqueueDirectMessage(
            22, uint256S("b753"), 1, DirectPayload{result_b}, now, netgroup));
        BOOST_REQUIRE(manager.EnqueueDirectMessage(
            122, uint256S("b754"), 1, DirectPayload{update_b}, now, netgroup));
    };

    Manager fair_manager{true};
    enqueue_results(fair_manager);
    CheckTwoSessionRoundRobin(fair_manager.TakeDirectMessages(4), netgroup);

    // Exact result consumers remain session-selective even though the shared
    // inbox cursor is now independent of the reconnecting peer id.
    Manager selective_manager{true};
    enqueue_results(selective_manager);
    const auto first_session = selective_manager.TakeResults(
        result_a.request_id, result_a.session_id, provider_id, 2);
    const auto second_session = selective_manager.TakeResults(
        result_b.request_id, result_b.session_id, provider_id, 2);
    BOOST_REQUIRE_EQUAL(first_session.size(), 2U);
    BOOST_REQUIRE_EQUAL(second_session.size(), 2U);
    BOOST_CHECK_EQUAL(first_session[0].peer_id, 11);
    BOOST_CHECK_EQUAL(first_session[1].peer_id, 111);
    BOOST_CHECK_EQUAL(second_session[0].peer_id, 22);
    BOOST_CHECK_EQUAL(second_session[1].peer_id, 122);
}

BOOST_AUTO_TEST_CASE(manager_inbox_is_bounded_and_fair_by_keyed_netgroup)
{
    Manager manager{true};
    const PaymasterId provider_id{uint256S("97")};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = provider_id;
    constexpr uint64_t first_netgroup{10};
    constexpr uint64_t second_netgroup{20};
    for (size_t i = 0; i < MAX_DIRECT_INBOX_MESSAGES_PER_NETGROUP; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 1);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessage(
            static_cast<int64_t>(i + 1), id, 1, DirectPayload{request},
            100000, first_netgroup));
    }
    request.request_id = uint256S("98").GetHex();
    request.session_id = uint256S("98");
    request.client_nonce = uint256S("98");
    BOOST_CHECK(!manager.EnqueueDirectMessage(
        50, uint256S("98"), 1, DirectPayload{request}, 100000, first_netgroup));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        51, uint256S("99"), 1, DirectPayload{request}, 100000, second_netgroup));

    const auto first = manager.TakeCapacityRequests(provider_id, 1);
    const auto second = manager.TakeCapacityRequests(provider_id, 1);
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_REQUIRE_EQUAL(second.size(), 1U);
    BOOST_REQUIRE(first.front().keyed_netgroup.has_value());
    BOOST_REQUIRE(second.front().keyed_netgroup.has_value());
    BOOST_CHECK_EQUAL(*first.front().keyed_netgroup, first_netgroup);
    BOOST_CHECK_EQUAL(*second.front().keyed_netgroup, second_netgroup);
}

BOOST_AUTO_TEST_CASE(manager_provider_runtime_is_wallet_scoped_and_ephemeral)
{
    Manager manager{true};
    const PaymasterId first{uint256S("71")};
    const PaymasterId second{uint256S("72")};
    BOOST_REQUIRE(manager.StartProvider("provider-a", first));
    BOOST_CHECK(manager.StartProvider("provider-a", first));
    BOOST_CHECK(!manager.StartProvider("provider-a", second));
    BOOST_REQUIRE(manager.StartProvider("provider-b", second));
    BOOST_CHECK_EQUAL(manager.RunningProviderCount(), 2U);
    BOOST_CHECK(manager.IsProviderRunning("provider-a", first));
    manager.StopProvider("provider-a");
    BOOST_CHECK(!manager.IsProviderRunning("provider-a", first));
    BOOST_CHECK_EQUAL(manager.RunningProviderCount(), 1U);
    manager.SetEnabled(false);
    BOOST_CHECK_EQUAL(manager.RunningProviderCount(), 0U);
    BOOST_CHECK(!manager.StartProvider("provider-a", first));
}

BOOST_AUTO_TEST_CASE(manager_provider_work_is_serialized_and_status_is_ephemeral)
{
    Manager manager{true};
    const PaymasterId provider_id{uint256S("73")};
    const PaymasterId other_provider{uint256S("74")};

    const ProviderServiceStatus initial =
        manager.GetProviderServiceStatus("provider-wallet");
    BOOST_CHECK(initial.state == ProviderServiceState::STOPPED);
    BOOST_CHECK(initial.last_error.empty());
    BOOST_CHECK(!manager.TryBeginProviderWork("provider-wallet", provider_id));

    BOOST_REQUIRE(manager.StartProvider("provider-wallet", provider_id));
    BOOST_REQUIRE(manager.StartProvider("other-wallet", other_provider));

    PaymasterCapacityRequest capacity_request;
    capacity_request.genesis_hash = uint256S("7301");
    capacity_request.provider_id = provider_id;
    capacity_request.request_id =
        "550e8400-e29b-41d4-a716-446655447301";
    capacity_request.session_id = uint256S("7302");
    capacity_request.client_nonce = uint256S("7303");
    capacity_request.funding_model = FundingModel::USER_PAID;
    capacity_request.created_at = 100000;
    capacity_request.expires_at = 100060;
    const RecoveryWireMessages recovery = RecoveryMessages(
        capacity_request.genesis_hash, 100000,
        "550e8400-e29b-41d4-a716-446655447304", uint256S("7304"),
        provider_id);
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        31, uint256S("7305"), 100, DirectPayload{capacity_request}, 100000));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        32, uint256S("7306"), 100, DirectPayload{recovery.submit}, 100000));
    ProviderQueueStatus queue = manager.GetProviderQueueStatus(provider_id);
    BOOST_CHECK_EQUAL(queue.waiting_requests, 1U);
    BOOST_CHECK_EQUAL(queue.waiting_submits, 1U);
    BOOST_CHECK_EQUAL(
        manager.GetProviderQueueStatus(other_provider).waiting_requests, 0U);

    BOOST_REQUIRE(manager.TryBeginProviderWork("provider-wallet", provider_id));
    BOOST_CHECK(!manager.TryBeginProviderWork("provider-wallet", provider_id));
    BOOST_CHECK(!manager.TryBeginProviderWork("provider-wallet", other_provider));
    BOOST_REQUIRE(manager.TryBeginProviderWork("other-wallet", other_provider));
    manager.EndProviderWork("other-wallet");

    manager.SetProviderServiceStatus("provider-wallet",
                                     ProviderServiceState::ACTIVE, {});
    ProviderServiceStatus status =
        manager.GetProviderServiceStatus("provider-wallet");
    BOOST_CHECK(status.state == ProviderServiceState::ACTIVE);
    BOOST_CHECK(status.last_error.empty());

    manager.EndProviderWork("provider-wallet");
    BOOST_REQUIRE(manager.TryBeginProviderWork("provider-wallet", provider_id));

    // Stopping prevents new work even while an in-flight owner still holds
    // the wallet lock. Releasing that owner is harmless and allows a later
    // explicit restart to acquire the lock normally.
    manager.StopProvider("provider-wallet");
    BOOST_CHECK(!manager.TryBeginProviderWork("provider-wallet", provider_id));
    status = manager.GetProviderServiceStatus("provider-wallet");
    BOOST_CHECK(status.state == ProviderServiceState::STOPPED);
    BOOST_CHECK(status.last_error.empty());
    manager.EndProviderWork("provider-wallet");

    // Wallet-local maintenance such as release_slot is intentionally allowed
    // while the runtime is stopped, but it remains exclusive and prevents a
    // concurrent provider start until the operation finishes.
    BOOST_REQUIRE(manager.TryBeginProviderWork(
        "provider-wallet", provider_id, /*require_running=*/false));
    BOOST_CHECK(!manager.TryBeginProviderWork(
        "provider-wallet", provider_id, /*require_running=*/false));
    BOOST_CHECK(!manager.StartProvider("provider-wallet", provider_id));
    manager.EndProviderWork("provider-wallet");

    // A readiness owner can atomically consume its work slot to start the
    // provider. No unguarded start can interleave between those two states.
    BOOST_REQUIRE(manager.TryBeginProviderWork(
        "provider-wallet", provider_id, /*require_running=*/false));
    BOOST_CHECK(!manager.CompleteProviderStart(
        "provider-wallet", other_provider));
    BOOST_CHECK(!manager.IsProviderRunning("provider-wallet", provider_id));
    BOOST_REQUIRE(manager.CompleteProviderStart(
        "provider-wallet", provider_id));
    BOOST_CHECK(manager.IsProviderRunning("provider-wallet", provider_id));
    BOOST_CHECK(!manager.TryBeginProviderWork(
        "provider-wallet", provider_id, /*require_running=*/false));
    manager.EndProviderWork("provider-wallet");

    BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(provider_id, 1).size(),
                        1U);
    BOOST_REQUIRE_EQUAL(manager.TakeRecoverySubmits(provider_id, 1).size(),
                        1U);
    queue = manager.GetProviderQueueStatus(provider_id);
    BOOST_CHECK_EQUAL(queue.waiting_requests, 0U);
    BOOST_CHECK_EQUAL(queue.waiting_submits, 0U);

    BOOST_REQUIRE(manager.StartProvider("provider-wallet", provider_id));
    BOOST_REQUIRE(manager.TryBeginProviderWork("provider-wallet", provider_id));
    manager.EndProviderWork("provider-wallet");
    manager.SetProviderServiceStatus("provider-wallet",
                                     ProviderServiceState::FAULT,
                                     "PAYMASTER_PROVIDER_SERVICE_FAILED");
    status = manager.GetProviderServiceStatus("provider-wallet");
    BOOST_CHECK(status.state == ProviderServiceState::FAULT);
    BOOST_CHECK_EQUAL(status.last_error,
                      "PAYMASTER_PROVIDER_SERVICE_FAILED");

    manager.SetEnabled(false);
    status = manager.GetProviderServiceStatus("provider-wallet");
    BOOST_CHECK(status.state == ProviderServiceState::STOPPED);
    BOOST_CHECK(status.last_error.empty());
    BOOST_CHECK(!manager.TryBeginProviderWork("provider-wallet", provider_id));
}

BOOST_AUTO_TEST_CASE(manager_provider_inbox_leases_retry_then_acknowledge)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    const PaymasterId provider_id{uint256S("7501")};

    PaymasterCapacityRequest capacity;
    capacity.provider_id = provider_id;
    capacity.request_id = "550e8400-e29b-41d4-a716-446655447501";
    capacity.session_id = uint256S("7502");
    capacity.client_nonce = uint256S("7503");

    PaymasterQuoteRequest quote;
    quote.intent.provider_id = provider_id;
    quote.intent.request_id = capacity.request_id;
    quote.intent.session_id = capacity.session_id;

    PaymasterSubmit submit;
    submit.provider_id = provider_id;
    submit.request_id = capacity.request_id;
    submit.session_id = capacity.session_id;

    const RecoveryWireMessages recovery = RecoveryMessages(
        uint256S("7504"), now,
        "550e8400-e29b-41d4-a716-446655447505", uint256S("7506"),
        provider_id);

    const uint256 capacity_id{uint256S("7511")};
    const uint256 quote_id{uint256S("7512")};
    const uint256 submit_id{uint256S("7513")};
    const uint256 recovery_request_id{uint256S("7514")};
    const uint256 recovery_submit_id{uint256S("7515")};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        1, capacity_id, 100, DirectPayload{capacity}, now));

    // Releasing a scope without acknowledgement models a temporary durable
    // store failure. The same message remains available for an exact retry.
    {
        auto lease = manager.LeaseCapacityRequests(provider_id, 1);
        BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
        BOOST_CHECK(lease.Messages().front().message_id == capacity_id);
        BOOST_CHECK(manager.TakeCapacityRequests(provider_id, 1).empty());
    }
    {
        auto retry = manager.LeaseCapacityRequests(provider_id, 1);
        BOOST_REQUIRE_EQUAL(retry.Messages().size(), 1U);
        BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent(
            {retry.Messages().front().message_id}));
    }

    const auto acknowledge_lease = [&](DirectMessageLease lease,
                                       const uint256& expected_id) {
        BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
        BOOST_CHECK(lease.Messages().front().message_id == expected_id);
        BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent(
            {expected_id}));
    };
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        2, quote_id, 100, DirectPayload{quote}, now + 61));
    acknowledge_lease(manager.LeaseQuoteRequests(provider_id, 1), quote_id);
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        3, submit_id, 100, DirectPayload{submit}, now + 122));
    acknowledge_lease(manager.LeaseSubmits(provider_id, 1), submit_id);
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        4, recovery_request_id, 100, DirectPayload{recovery.request},
        now + 183));
    acknowledge_lease(manager.LeaseRecoveryRequests(provider_id, 1),
                      recovery_request_id);
    BOOST_REQUIRE(manager.EnqueueDirectMessage(
        5, recovery_submit_id, 100, DirectPayload{recovery.submit},
        now + 244));
    acknowledge_lease(manager.LeaseRecoverySubmits(provider_id, 1),
                      recovery_submit_id);

    BOOST_CHECK(!manager.HasDirectMessages());
    const ProviderQueueStatus queue =
        manager.GetProviderQueueStatus(provider_id);
    BOOST_CHECK_EQUAL(queue.waiting_requests, 0U);
    BOOST_CHECK_EQUAL(queue.waiting_submits, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
