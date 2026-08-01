// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Exact retry and wallet-owned recovery-output invariants. */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <coins.h>
#include <hash.h>
#include <key.h>
#include <paymaster/directory.h>
#include <paymaster/recovery.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <version.h>

using namespace DigiDollar::Paymaster;

namespace {

template <typename T>
std::vector<unsigned char> SerializeExact(const T& value)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

XOnlyPubKey BIP86OutputKey(const CKey& internal_key)
{
    const auto tweaked =
        XOnlyPubKey{internal_key.GetPubKey()}.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    return tweaked->first;
}

CScript BIP86Script(const CKey& internal_key)
{
    return GetScriptForDestination(WitnessV1Taproot{
        BIP86OutputKey(internal_key)});
}

CTransactionRef MakeDDSource(CAmount amount, uint32_t nonce,
                             const CScript& script)
{
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_TRANSFER);
    tx.vin.emplace_back(COutPoint{uint256::ONE, nonce});
    tx.vout.emplace_back(0, script);
    tx.vout.emplace_back(
        0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                     << CScriptNum(DD_TX_TRANSFER) << CScriptNum(amount));
    return MakeTransactionRef(std::move(tx));
}

CMutableTransaction MakeDGBSource(CAmount amount, uint32_t nonce,
                                  const CScript& script)
{
    CMutableTransaction tx;
    tx.nVersion = 2;
    tx.vin.emplace_back(COutPoint{uint256::ONE, nonce});
    tx.vout.emplace_back(amount, script);
    return tx;
}

void SignBIP86ControlProof(const CKey& internal_key, const uint256& hash,
                           std::vector<unsigned char>& signature)
{
    signature.resize(64);
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(internal_key.SignSchnorr(
        hash, signature, &empty_merkle_root, uint256{}));
}

struct RecoveryFixture : BasicTestingSetup {
    static constexpr int64_t NOW{100000};

    CCoinsView base;
    CCoinsViewCache coins{&base};
    CKey identity_key;
    CKey dgb_internal_key;
    CKey carrier_internal_key;
    CKey user_internal_key;
    CKey return_internal_key;
    CKey provider_dd_internal_key;
    CKey provider_dgb_change_internal_key;
    CTransactionRef user_dd;
    CTransactionRef carrier;
    CMutableTransaction provider_dgb;

    RecoveryFixture()
    {
        identity_key.MakeNewKey(true);
        dgb_internal_key.MakeNewKey(true);
        carrier_internal_key.MakeNewKey(true);
        user_internal_key.MakeNewKey(true);
        return_internal_key.MakeNewKey(true);
        provider_dd_internal_key.MakeNewKey(true);
        provider_dgb_change_internal_key.MakeNewKey(true);

        user_dd = MakeDDSource(1000, 1, BIP86Script(user_internal_key));
        carrier = MakeDDSource(100, 2, BIP86Script(carrier_internal_key));
        provider_dgb =
            MakeDGBSource(2 * COIN / 10, 3, BIP86Script(dgb_internal_key));
        coins.AddCoin(COutPoint{user_dd->GetHash(), 0},
                      Coin{user_dd->vout[0], 500, false}, false);
        coins.AddCoin(COutPoint{carrier->GetHash(), 0},
                      Coin{carrier->vout[0], 500, false}, false);
        coins.AddCoin(COutPoint{CTransaction{provider_dgb}.GetHash(), 0},
                      Coin{provider_dgb.vout[0], 500, false}, false);
    }

    PaymasterCapacityProof CapacityProof(
        const PaymasterCapacityRequest& request, bool include_carrier) const
    {
        PaymasterCapacityProof proof;
        proof.genesis_hash = request.genesis_hash;
        proof.provider_id = request.provider_id;
        proof.request_id = request.request_id;
        proof.session_id = request.session_id;
        proof.client_nonce = request.client_nonce;
        proof.funding_model = request.funding_model;
        proof.requires_carrier = request.requires_carrier;
        proof.snapshot_id = uint256S("a1");
        proof.created_at = NOW;
        proof.expires_at = NOW + 30;

        PaymasterLiquiditySlot slot;
        CapacityDGBInput dgb;
        dgb.input.outpoint =
            COutPoint{CTransaction{provider_dgb}.GetHash(), 0};
        dgb.input.creating_tx = provider_dgb;
        dgb.input.value = DGBSatoshis{provider_dgb.vout[0].nValue};
        dgb.control_proof.reference_block = uint256S("a2");
        dgb.control_proof.expires_at = proof.expires_at;
        SignBIP86ControlProof(
            dgb_internal_key,
            GetCapacityControlHash(proof, dgb.input.outpoint,
                                   dgb.control_proof.expires_at),
            dgb.control_proof.signature);
        slot.dgb_inputs.push_back(dgb);

        if (include_carrier) {
            CapacityDDCarrier capacity_carrier;
            capacity_carrier.carrier.outpoint =
                COutPoint{carrier->GetHash(), 0};
            capacity_carrier.carrier.creating_tx =
                CMutableTransaction{*carrier};
            capacity_carrier.carrier.value = DDCents{100};
            capacity_carrier.control_proof.reference_block = uint256S("a2");
            capacity_carrier.control_proof.expires_at = proof.expires_at;
            SignBIP86ControlProof(
                carrier_internal_key,
                GetCapacityControlHash(
                    proof, capacity_carrier.carrier.outpoint,
                    capacity_carrier.control_proof.expires_at),
                capacity_carrier.control_proof.signature);
            slot.carrier = capacity_carrier;
        }
        proof.liquidity_slots.push_back(slot);
        proof.identity_signature.resize(64);
        BOOST_REQUIRE(identity_key.SignSchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature,
            nullptr, uint256{}));
        return proof;
    }

    AlternativeRecoveryParameters Parameters(bool include_carrier = false,
                                             int64_t service_fee = 100) const
    {
        AlternativeRecoveryParameters parameters;
        parameters.genesis_hash = uint256S("a3");
        parameters.request_id =
            "550e8400-e29b-41d4-a716-4466554400a3";
        parameters.session_id = uint256S("a4");
        parameters.original_provider_id = uint256S("a5");
        parameters.recovery_provider_identity_key =
            XOnlyPubKey{identity_key.GetPubKey()};
        parameters.recovery_provider_id =
            GetPaymasterId(parameters.recovery_provider_identity_key);
        parameters.offer_id = uint256S("aa");
        parameters.policy_hash = uint256S("ab");
        parameters.original_commit_key = uint256S("a6");
        parameters.original_template_commitment = uint256S("a7");

        parameters.capacity_request.genesis_hash = parameters.genesis_hash;
        parameters.capacity_request.provider_id =
            parameters.recovery_provider_id;
        parameters.capacity_request.request_id = parameters.request_id;
        parameters.capacity_request.session_id = parameters.session_id;
        parameters.capacity_request.client_nonce = uint256S("a8");
        parameters.capacity_request.funding_model = FundingModel::USER_PAID;
        parameters.capacity_request.requires_carrier =
            service_fee > 0 && service_fee < 100;
        parameters.capacity_request.created_at = NOW;
        parameters.capacity_request.expires_at = NOW + 60;
        const PaymasterCapacityProof proof{
            CapacityProof(parameters.capacity_request, include_carrier)};

        parameters.capacity_snapshot.snapshot_id = proof.snapshot_id;
        parameters.capacity_snapshot.resource_commitment =
            GetCapacityResourceCommitment(proof);
        parameters.capacity_snapshot.session_id = parameters.session_id;
        parameters.capacity_snapshot.attempt_id = uint256S("a9");
        parameters.capacity_snapshot.provider_id =
            parameters.recovery_provider_id;
        parameters.capacity_snapshot.client_nonce = proof.client_nonce;
        const std::vector<unsigned char> request_bytes{
            SerializeExact(parameters.capacity_request)};
        parameters.capacity_snapshot.request_hash = Hash(request_bytes);
        parameters.capacity_snapshot.capacity_proof = SerializeExact(proof);
        parameters.capacity_snapshot.created_at = proof.created_at;
        parameters.capacity_snapshot.expires_at = proof.expires_at;
        parameters.capacity_snapshot.validated_at = NOW;
        parameters.capacity_snapshot.funding_model = proof.funding_model;
        parameters.capacity_snapshot.requires_carrier =
            proof.requires_carrier;

        const COutPoint user_outpoint{user_dd->GetHash(), 0};
        parameters.persisted_user_dd_inputs = {user_outpoint};
        parameters.user_dd_inputs = {
            {user_outpoint, user_dd, InputRole::USER_DD}};
        const CScript return_script{BIP86Script(return_internal_key)};
        parameters.wallet_returns = {
            {return_script, DDCents{1000 - service_fee}}};
        parameters.wallet_verified_fresh_return_scripts = {return_script};
        if (include_carrier || service_fee > 0) {
            parameters.recovery_provider_dd_script =
                BIP86Script(provider_dd_internal_key);
        }
        parameters.recovery_provider_dgb_change_script =
            BIP86Script(provider_dgb_change_internal_key);
        parameters.maximum_service_fee = DDCents{100};
        parameters.service_fee = DDCents{service_fee};
        parameters.network_fee = DGBSatoshis{COIN / 10};
        parameters.fee_rate = 100000;
        parameters.expires_at = NOW + 20;
        return parameters;
    }

    AlternativeRecoveryRequest Request(
        const AlternativeRecoveryParameters& parameters) const
    {
        AlternativeRecoveryRequest request;
        request.genesis_hash = parameters.genesis_hash;
        request.request_id = parameters.request_id;
        request.session_id = parameters.session_id;
        request.original_provider_id = parameters.original_provider_id;
        request.recovery_provider_id = parameters.recovery_provider_id;
        request.offer_id = parameters.offer_id;
        request.policy_hash = parameters.policy_hash;
        request.original_commit_key = parameters.original_commit_key;
        request.original_template_commitment =
            parameters.original_template_commitment;
        request.client_nonce = parameters.capacity_request.client_nonce;
        request.capacity_request = parameters.capacity_request;
        request.capacity_snapshot_id =
            parameters.capacity_snapshot.snapshot_id;
        request.capacity_resource_commitment =
            parameters.capacity_snapshot.resource_commitment;
        request.user_dd_inputs = parameters.persisted_user_dd_inputs;
        request.wallet_returns = parameters.wallet_returns;
        request.maximum_service_fee = parameters.maximum_service_fee;
        request.service_fee = parameters.service_fee;
        request.created_at = NOW;
        request.expires_at = parameters.expires_at;

        AlternativeRecoveryInputProof proof;
        proof.outpoint = request.user_dd_inputs.front();
        SignBIP86ControlProof(
            user_internal_key,
            GetAlternativeRecoveryInputControlHash(request, proof.outpoint),
            proof.signature);
        request.user_input_proofs.push_back(std::move(proof));
        return request;
    }

    AlternativeRecoveryResponse Response(
        const AlternativeRecoveryParameters& parameters,
        const AlternativeRecoveryRequest& request,
        const AlternativeRecoveryTemplate& recovery) const
    {
        AlternativeRecoveryResponse response;
        response.genesis_hash = parameters.genesis_hash;
        response.request_id = request.request_id;
        response.session_id = request.session_id;
        response.recovery_id = GetAlternativeRecoveryId(
            request.request_id, request.session_id,
            request.recovery_provider_id, request.client_nonce);
        response.recovery_provider_id = request.recovery_provider_id;
        response.recovery_request_hash =
            GetAlternativeRecoveryRequestHash(request);
        response.manifest = recovery.manifest;
        response.unsigned_psbt =
            SerializeExact(recovery.trusted_template.psbt);
        response.created_at = NOW;
        response.expires_at = parameters.expires_at;
        response.recovery_commit_key =
            GetAlternativeRecoveryCommitKey(response);
        response.identity_signature.resize(64);
        BOOST_REQUIRE(identity_key.SignSchnorr(
            GetAlternativeRecoveryResponseSignatureHash(response),
            response.identity_signature, nullptr, uint256{}));
        return response;
    }

    void ResignResponse(AlternativeRecoveryResponse& response) const
    {
        response.identity_signature.assign(64, 0);
        BOOST_REQUIRE(identity_key.SignSchnorr(
            GetAlternativeRecoveryResponseSignatureHash(response),
            response.identity_signature, nullptr, uint256{}));
    }

    std::vector<CTxOut> TemplatePrevouts(
        const CollaborativePSBTTemplate& trusted) const
    {
        std::vector<CTxOut> prevouts;
        prevouts.reserve(trusted.psbt.inputs.size());
        for (size_t index = 0; index < trusted.psbt.inputs.size(); ++index) {
            CTxOut prevout;
            BOOST_REQUIRE(trusted.psbt.GetInputUTXO(prevout, index));
            prevouts.push_back(std::move(prevout));
        }
        return prevouts;
    }

    std::vector<unsigned char> InputSignature(
        const CMutableTransaction& transaction,
        size_t input_index,
        const CKey& internal_key,
        const PrecomputedTransactionData& txdata) const
    {
        uint256 sighash;
        ScriptExecutionData execdata;
        execdata.m_annex_init = true;
        execdata.m_annex_present = false;
        execdata.m_tapleaf_hash_init = false;
        BOOST_REQUIRE(SignatureHashSchnorr(
            sighash, execdata, transaction, input_index, SIGHASH_DEFAULT,
            SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL));
        std::vector<unsigned char> signature(64);
        const uint256 empty_merkle_root;
        BOOST_REQUIRE(internal_key.SignSchnorr(
            sighash, signature, &empty_merkle_root, uint256{}));
        return signature;
    }

    PartiallySignedTransaction UserSignedPSBT(
        const AlternativeRecoveryTemplate& recovery) const
    {
        PartiallySignedTransaction psbt{recovery.trusted_template.psbt};
        CMutableTransaction transaction{*psbt.tx};
        PrecomputedTransactionData txdata;
        txdata.Init(transaction,
                    TemplatePrevouts(recovery.trusted_template),
                    /*force=*/true);
        for (size_t index = 0; index < psbt.inputs.size(); ++index) {
            if (recovery.trusted_template.input_roles[index] !=
                InputRole::USER_DD) {
                continue;
            }
            psbt.inputs[index].final_script_witness.stack = {
                InputSignature(transaction, index, user_internal_key, txdata)};
        }
        return psbt;
    }

    CMutableTransaction FullySignedTransaction(
        const AlternativeRecoveryTemplate& recovery) const
    {
        CMutableTransaction transaction{*recovery.trusted_template.psbt.tx};
        PrecomputedTransactionData txdata;
        txdata.Init(transaction,
                    TemplatePrevouts(recovery.trusted_template),
                    /*force=*/true);
        for (size_t index = 0;
             index < recovery.trusted_template.input_roles.size(); ++index) {
            const InputRole role =
                recovery.trusted_template.input_roles[index];
            const CKey* signing_key{nullptr};
            if (role == InputRole::USER_DD) {
                signing_key = &user_internal_key;
            } else if (role == InputRole::PROVIDER_CARRIER) {
                signing_key = &carrier_internal_key;
            } else if (role == InputRole::PROVIDER_DGB) {
                signing_key = &dgb_internal_key;
            }
            BOOST_REQUIRE(signing_key != nullptr);
            transaction.vin[index].scriptWitness.stack = {
                InputSignature(transaction, index, *signing_key, txdata)};
        }
        return transaction;
    }

    AlternativeRecoveryResultMessage ResultMessage(
        const AlternativeRecoveryResponse& response,
        const CMutableTransaction& final_transaction) const
    {
        AlternativeRecoveryResultMessage message;
        message.request_id = response.request_id;
        message.session_id = response.session_id;
        message.recovery_id = response.recovery_id;
        message.recovery_request_hash = response.recovery_request_hash;
        message.result.genesis_hash = response.genesis_hash;
        message.result.provider_id = response.recovery_provider_id;
        message.result.commit_key = response.recovery_commit_key;
        message.result.result_sequence = 1;
        message.result.status = PaymasterResultStatus::FINAL_COMMITTED;
        message.result.txid = CTransaction{final_transaction}.GetHash();
        message.result.raw_transaction_hash =
            CTransaction{final_transaction}.GetWitnessHash();
        message.result.final_transaction = final_transaction;
        message.result.updated_at = NOW;
        message.result.identity_signature.resize(64);
        BOOST_REQUIRE(identity_key.SignSchnorr(
            GetPaymasterResultSignatureHash(message.result),
            message.result.identity_signature, nullptr, uint256{}));
        return message;
    }

    CapacityChainstateCallbacks Chainstate() const
    {
        CapacityChainstateCallbacks chainstate;
        chainstate.validate_reference_block = [](const uint256& block,
                                                 std::string&) {
            return block == uint256S("a2");
        };
        chainstate.validate_dgb_input =
            [&](const VerifiedDGBInput& input, XOnlyPubKey& output_key,
                std::string&) {
                if (input.outpoint !=
                    COutPoint{CTransaction{provider_dgb}.GetHash(), 0}) {
                    return false;
                }
                output_key = BIP86OutputKey(dgb_internal_key);
                return true;
            };
        chainstate.validate_dd_carrier =
            [&](const VerifiedDDCarrier& input, XOnlyPubKey& output_key,
                std::string&) {
                if (input.outpoint != COutPoint{carrier->GetHash(), 0}) {
                    return false;
                }
                output_key = BIP86OutputKey(carrier_internal_key);
                return true;
            };
        return chainstate;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_recovery_tests, RecoveryFixture)

BOOST_AUTO_TEST_CASE(builds_and_revalidates_exact_alternative_provider_template)
{
    const AlternativeRecoveryParameters parameters{Parameters()};
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildAlternativeRecoveryTemplate(
                              parameters, Params(), coins, chainstate, NOW,
                              recovery, error),
                          error);
    BOOST_CHECK(!recovery.manifest.manifest_id.IsNull());
    BOOST_CHECK(!recovery.manifest.template_commitment.IsNull());
    BOOST_CHECK_EQUAL(recovery.manifest.service_fee.value, 100);
    BOOST_CHECK_EQUAL(recovery.manifest.network_fee.value, COIN / 10);
    BOOST_REQUIRE_EQUAL(recovery.trusted_template.input_roles.size(), 2U);
    BOOST_CHECK(recovery.trusted_template.input_roles[0] == InputRole::USER_DD);
    BOOST_CHECK(recovery.trusted_template.input_roles[1] ==
                InputRole::PROVIDER_DGB);
    for (const PSBTInput& input : recovery.trusted_template.psbt.inputs) {
        BOOST_REQUIRE(input.sighash_type.has_value());
        BOOST_CHECK_EQUAL(*input.sighash_type, SIGHASH_DEFAULT);
    }
    BOOST_CHECK(ValidateAlternativeRecoveryTemplate(
        recovery, parameters, Params(), coins, chainstate, NOW, error));
    BOOST_CHECK(ValidateAlternativeRecoveryPSBT(
        recovery.trusted_template.psbt, recovery, parameters, Params(), coins,
        chainstate, NOW, CollaborativeSignatureStage::UNSIGNED, error));
}

BOOST_AUTO_TEST_CASE(small_fee_uses_the_exact_capacity_carrier)
{
    const AlternativeRecoveryParameters parameters{Parameters(true, 5)};
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildAlternativeRecoveryTemplate(
                              parameters, Params(), coins, chainstate, NOW,
                              recovery, error),
                          error);
    BOOST_REQUIRE_EQUAL(recovery.trusted_template.input_roles.size(), 3U);
    BOOST_CHECK(recovery.trusted_template.input_roles[1] ==
                InputRole::PROVIDER_CARRIER);
    BOOST_REQUIRE_EQUAL(
        recovery.manifest.recovery_provider_carrier_inputs.size(), 1U);
    BOOST_CHECK_EQUAL(recovery.manifest.wallet_returns.front().amount.value,
                      995);

    AlternativeRecoveryParameters without_carrier{Parameters(false, 5)};
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        without_carrier, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPACITY_SLOT");
}

BOOST_AUTO_TEST_CASE(rejects_same_provider_fee_overrun_and_nonfresh_return)
{
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;

    AlternativeRecoveryParameters parameters{Parameters()};
    parameters.original_provider_id = parameters.recovery_provider_id;
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_PROVIDER_MUST_DIFFER");

    parameters = Parameters();
    parameters.maximum_service_fee = DDCents{99};
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_SERVICE_FEE_LIMIT");

    parameters = Parameters();
    parameters.capacity_request.funding_model = FundingModel::SPONSORED;
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH");

    parameters = Parameters();
    CKey different_return_key;
    different_return_key.MakeNewKey(true);
    parameters.wallet_verified_fresh_return_scripts.front() =
        BIP86Script(different_return_key);
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_RETURN_SCRIPT_MISMATCH");
}

BOOST_AUTO_TEST_CASE(rejects_different_user_inputs_and_invalid_capacity_proof)
{
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;

    AlternativeRecoveryParameters parameters{Parameters()};
    parameters.persisted_user_dd_inputs.front().n++;
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_USER_INPUT_MISMATCH");

    parameters = Parameters();
    PaymasterCapacityProof proof;
    CDataStream stream{parameters.capacity_snapshot.capacity_proof, SER_NETWORK,
                       ::PROTOCOL_VERSION};
    stream >> proof;
    proof.identity_signature.front() ^= 1;
    parameters.capacity_snapshot.capacity_proof = SerializeExact(proof);
    BOOST_CHECK(!BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_INVALID_CAPACITY_IDENTITY_SIGNATURE");
}

BOOST_AUTO_TEST_CASE(rejects_psbt_fields_roles_and_manifest_mutations)
{
    const AlternativeRecoveryParameters parameters{Parameters()};
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;
    BOOST_REQUIRE(BuildAlternativeRecoveryTemplate(
        parameters, Params(), coins, chainstate, NOW, recovery, error));

    PartiallySignedTransaction candidate{recovery.trusted_template.psbt};
    candidate.inputs.front().sighash_type = SIGHASH_ALL;
    BOOST_CHECK(!ValidateAlternativeRecoveryPSBT(
        candidate, recovery, parameters, Params(), coins, chainstate, NOW,
        CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGHASH");

    candidate = recovery.trusted_template.psbt;
    candidate.outputs.front().unknown[{0x42}] = {0x01};
    BOOST_CHECK(!ValidateAlternativeRecoveryPSBT(
        candidate, recovery, parameters, Params(), coins, chainstate, NOW,
        CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_OUTPUT_FIELDS");

    AlternativeRecoveryTemplate wrong_role{recovery};
    wrong_role.trusted_template.input_roles.front() = InputRole::USER_DGB;
    BOOST_CHECK(!ValidateAlternativeRecoveryTemplate(
        wrong_role, parameters, Params(), coins, chainstate, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_TEMPLATE_MISMATCH");

    AlternativeRecoveryTemplate wrong_fee{recovery};
    wrong_fee.manifest.network_fee.value++;
    BOOST_CHECK(!ValidateAlternativeRecoveryTemplate(
        wrong_fee, parameters, Params(), coins, chainstate, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_TEMPLATE_MISMATCH");

    AlternativeRecoveryTemplate wrong_offer{recovery};
    wrong_offer.manifest.offer_id = uint256S("ff");
    BOOST_CHECK(!ValidateAlternativeRecoveryTemplate(
        wrong_offer, parameters, Params(), coins, chainstate, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_TEMPLATE_MISMATCH");
}

BOOST_AUTO_TEST_CASE(recovery_v2_envelope_binds_offer_policy_and_capacity_mode)
{
    const AlternativeRecoveryParameters parameters{Parameters()};
    const AlternativeRecoveryRequest request{Request(parameters)};
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryRequestEnvelope(
                              request, parameters.genesis_hash, NOW, error),
                          error);
    BOOST_CHECK(ValidateAlternativeRecoveryRequestInputs(
        request, {BIP86OutputKey(user_internal_key)}, error));

    AlternativeRecoveryRequest legacy{request};
    legacy.version = 1;
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestEnvelope(
        legacy, parameters.genesis_hash, NOW, error));

    AlternativeRecoveryRequest sponsored{request};
    sponsored.capacity_request.funding_model = FundingModel::SPONSORED;
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestEnvelope(
        sponsored, parameters.genesis_hash, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH");

    AlternativeRecoveryRequest wrong_carrier{request};
    wrong_carrier.capacity_request.requires_carrier = true;
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestEnvelope(
        wrong_carrier, parameters.genesis_hash, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH");

    AlternativeRecoveryRequest changed_offer{request};
    changed_offer.offer_id = uint256S("ac");
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestInputs(
        changed_offer, {BIP86OutputKey(user_internal_key)}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RECOVERY_INPUT_PROOF");

    AlternativeRecoveryRequest missing_policy{request};
    missing_policy.policy_hash.SetNull();
    BOOST_CHECK(!ValidateAlternativeRecoveryRequestEnvelope(
        missing_policy, parameters.genesis_hash, NOW, error));
}

BOOST_AUTO_TEST_CASE(response_firewall_rebuilds_manifest_and_canonical_psbt)
{
    const AlternativeRecoveryParameters parameters{Parameters()};
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildAlternativeRecoveryTemplate(
                              parameters, Params(), coins, chainstate, NOW,
                              recovery, error),
                          error);
    const AlternativeRecoveryRequest request{Request(parameters)};
    const AlternativeRecoveryResponse response{
        Response(parameters, request, recovery)};

    AlternativeRecoveryTemplate validated;
    PartiallySignedTransaction unsigned_psbt;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryResponseTemplate(
                              response, request,
                              parameters.recovery_provider_identity_key,
                              parameters, Params(), coins, chainstate, NOW,
                              validated, unsigned_psbt, error),
                          error);
    BOOST_CHECK(SerializeExact(unsigned_psbt) == response.unsigned_psbt);
    BOOST_CHECK(validated.manifest.manifest_id ==
                recovery.manifest.manifest_id);

    AlternativeRecoveryResponse trailing{response};
    trailing.unsigned_psbt.push_back(0);
    ResignResponse(trailing);
    BOOST_CHECK(!ValidateAlternativeRecoveryResponseTemplate(
        trailing, request, parameters.recovery_provider_identity_key,
        parameters, Params(), coins, chainstate, NOW, validated,
        unsigned_psbt, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_RECOVERY_RESPONSE_PSBT_NONCANONICAL");

    AlternativeRecoveryResponse legacy{response};
    legacy.version = 1;
    BOOST_CHECK(!ValidateAlternativeRecoveryResponseEnvelope(
        legacy, parameters.genesis_hash, NOW, error));

    AlternativeRecoveryResponse unknown_field{response};
    PartiallySignedTransaction mutated{recovery.trusted_template.psbt};
    mutated.outputs.front().unknown[{0x42}] = {0x01};
    unknown_field.unsigned_psbt = SerializeExact(mutated);
    ResignResponse(unknown_field);
    BOOST_CHECK(!ValidateAlternativeRecoveryResponseTemplate(
        unknown_field, request, parameters.recovery_provider_identity_key,
        parameters, Params(), coins, chainstate, NOW, validated,
        unsigned_psbt, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_OUTPUT_FIELDS");

    AlternativeRecoveryParameters wrong_policy{parameters};
    wrong_policy.policy_hash = uint256S("ad");
    BOOST_CHECK(!ValidateAlternativeRecoveryResponseTemplate(
        response, request, parameters.recovery_provider_identity_key,
        wrong_policy, Params(), coins, chainstate, NOW, validated,
        unsigned_psbt, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_LOCAL_AUTHORITY_MISMATCH");
}

BOOST_AUTO_TEST_CASE(submit_and_result_firewalls_reject_cross_session_replay)
{
    const AlternativeRecoveryParameters parameters{Parameters()};
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildAlternativeRecoveryTemplate(
                              parameters, Params(), coins, chainstate, NOW,
                              recovery, error),
                          error);
    const AlternativeRecoveryRequest request{Request(parameters)};
    const AlternativeRecoveryResponse response{
        Response(parameters, request, recovery)};

    AlternativeRecoverySubmit submit;
    submit.genesis_hash = parameters.genesis_hash;
    submit.request_id = response.request_id;
    submit.session_id = response.session_id;
    submit.recovery_id = response.recovery_id;
    submit.recovery_provider_id = response.recovery_provider_id;
    submit.recovery_request_hash = response.recovery_request_hash;
    submit.recovery_commit_key = response.recovery_commit_key;
    submit.template_commitment = response.manifest.template_commitment;
    submit.user_psbt = SerializeExact(UserSignedPSBT(recovery));
    PartiallySignedTransaction decoded_submit;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoverySubmit(
                              submit, response, recovery, parameters, Params(),
                              coins, chainstate, NOW, decoded_submit, error),
                          error);

    AlternativeRecoverySubmit cross_session{submit};
    cross_session.session_id = uint256S("ae");
    BOOST_CHECK(!ValidateAlternativeRecoverySubmit(
        cross_session, response, recovery, parameters, Params(), coins,
        chainstate, NOW, decoded_submit, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_SUBMIT_MISMATCH");

    AlternativeRecoverySubmit legacy_submit{submit};
    legacy_submit.version = 1;
    BOOST_CHECK(!ValidateAlternativeRecoverySubmitEnvelope(
        legacy_submit, parameters.genesis_hash, error));

    AlternativeRecoverySubmit trailing{submit};
    trailing.user_psbt.push_back(0);
    BOOST_CHECK(!ValidateAlternativeRecoverySubmit(
        trailing, response, recovery, parameters, Params(), coins, chainstate,
        NOW, decoded_submit, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_RECOVERY_SUBMIT_PSBT_NONCANONICAL");

    const CMutableTransaction final_transaction{
        FullySignedTransaction(recovery)};
    const AlternativeRecoveryResultMessage result{
        ResultMessage(response, final_transaction)};
    BOOST_CHECK(ValidateAlternativeRecoveryResult(
        result, response, parameters.recovery_provider_identity_key,
        parameters.genesis_hash, 1, NOW, error));

    AlternativeRecoveryResultMessage replay{result};
    replay.session_id = uint256S("af");
    BOOST_CHECK(!ValidateAlternativeRecoveryResult(
        replay, response, parameters.recovery_provider_identity_key,
        parameters.genesis_hash, 1, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_RESULT_MISMATCH");

    AlternativeRecoveryResultMessage legacy_result{result};
    legacy_result.version = 1;
    BOOST_CHECK(!ValidateAlternativeRecoveryResultEnvelope(
        legacy_result, parameters.genesis_hash, NOW, error));

    BOOST_CHECK(!ValidateAlternativeRecoveryResult(
        result, response, parameters.recovery_provider_identity_key,
        parameters.genesis_hash, 2, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_RESULT_SEQUENCE");

    AlternativeRecoveryResultMessage wrong_commit{result};
    wrong_commit.result.commit_key = uint256S("b0");
    wrong_commit.result.identity_signature.assign(64, 0);
    BOOST_REQUIRE(identity_key.SignSchnorr(
        GetPaymasterResultSignatureHash(wrong_commit.result),
        wrong_commit.result.identity_signature, nullptr, uint256{}));
    BOOST_CHECK(!ValidateAlternativeRecoveryResult(
        wrong_commit, response, parameters.recovery_provider_identity_key,
        parameters.genesis_hash, 1, NOW, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RESULT_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_CASE(final_witness_and_post_expiry_authority_are_exact)
{
    const AlternativeRecoveryParameters parameters{Parameters()};
    const CapacityChainstateCallbacks chainstate{Chainstate()};
    AlternativeRecoveryTemplate recovery;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildAlternativeRecoveryTemplate(
                              parameters, Params(), coins, chainstate, NOW,
                              recovery, error),
                          error);
    const CMutableTransaction final_transaction{
        FullySignedTransaction(recovery)};
    const uint256 wtxid{CTransaction{final_transaction}.GetWitnessHash()};
    BOOST_REQUIRE_MESSAGE(ValidateFinalAlternativeRecoveryTransaction(
                              final_transaction, wtxid, recovery, parameters,
                              Params(), coins, chainstate, NOW,
                              /*exact_final_already_known=*/false, error),
                          error);

    CMutableTransaction bad_witness{final_transaction};
    BOOST_REQUIRE(!bad_witness.vin.front().scriptWitness.stack.empty());
    bad_witness.vin.front().scriptWitness.stack.front().front() ^= 1;
    const uint256 bad_wtxid{CTransaction{bad_witness}.GetWitnessHash()};
    BOOST_CHECK(!ValidateFinalAlternativeRecoveryTransaction(
        bad_witness, bad_wtxid, recovery, parameters, Params(), coins,
        chainstate, NOW, /*exact_final_already_known=*/false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");

    const int64_t after_expiry{parameters.expires_at + 1};
    BOOST_CHECK(!ValidateFinalAlternativeRecoveryTransaction(
        final_transaction, wtxid, recovery, parameters, Params(), coins,
        chainstate, after_expiry, /*exact_final_already_known=*/false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_RECOVERY_CAPACITY_REQUEST_MISMATCH");
    BOOST_CHECK(ValidateFinalAlternativeRecoveryTransaction(
        final_transaction, wtxid, recovery, parameters, Params(), coins,
        chainstate, after_expiry, /*exact_final_already_known=*/true, error));

    BOOST_CHECK(!ValidateFinalAlternativeRecoveryTransaction(
        bad_witness, bad_wtxid, recovery, parameters, Params(), coins,
        chainstate, after_expiry, /*exact_final_already_known=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");
}

BOOST_AUTO_TEST_SUITE_END()
