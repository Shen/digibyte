// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Deterministic transaction layout and value-conservation tests. */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <coins.h>
#include <key.h>
#include <paymaster/protocol.h>
#include <paymaster/txbuilder.h>
#include <paymaster/wire.h>
#include <test/util/setup_common.h>

namespace {

using namespace DigiDollar::Paymaster;

CScript NewP2TRScript()
{
    CKey key;
    key.MakeNewKey(true);
    const auto tweaked = XOnlyPubKey{key.GetPubKey()}.CreateTapTweak(nullptr);
    BOOST_REQUIRE(tweaked.has_value());
    return CScript{} << OP_1 << ToByteVector(tweaked->first);
}

CTransactionRef MakeDDSource(CAmount amount, uint32_t nonce,
                             const CScript& script)
{
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_TRANSFER);
    tx.vin.emplace_back(COutPoint{uint256::ONE, nonce});
    tx.vout.emplace_back(0, script);
    tx.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(DD_TX_TRANSFER) << CScriptNum(amount));
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef MakeDDSource(CAmount amount, uint32_t nonce)
{
    return MakeDDSource(amount, nonce, NewP2TRScript());
}

CTransactionRef MakeDGBSource(CAmount amount, uint32_t nonce)
{
    CMutableTransaction tx;
    tx.nVersion = 2;
    tx.vin.emplace_back(COutPoint{uint256::ONE, nonce});
    tx.vout.emplace_back(amount, NewP2TRScript());
    return MakeTransactionRef(std::move(tx));
}

struct CollaborativeFixture : BasicTestingSetup {
    CCoinsView base;
    CCoinsViewCache coins{&base};
    CTransactionRef user_dd{MakeDDSource(2000, 1)};
    CTransactionRef carrier{MakeDDSource(100, 2)};
    CTransactionRef provider_dgb{MakeDGBSource(COIN, 3)};

    CollaborativeFixture()
    {
        coins.AddCoin(COutPoint{user_dd->GetHash(), 0}, Coin{user_dd->vout[0], 500, false}, false);
        coins.AddCoin(COutPoint{carrier->GetHash(), 0}, Coin{carrier->vout[0], 500, false}, false);
        coins.AddCoin(COutPoint{provider_dgb->GetHash(), 0}, Coin{provider_dgb->vout[0], 500, false}, false);
    }

    CollaborativeTransferParams ValidParams() const
    {
        CollaborativeTransferParams params;
        params.inputs = {
            {COutPoint{user_dd->GetHash(), 0}, user_dd, InputRole::USER_DD},
            {COutPoint{carrier->GetHash(), 0}, carrier, InputRole::PROVIDER_CARRIER},
            {COutPoint{provider_dgb->GetHash(), 0}, provider_dgb, InputRole::PROVIDER_DGB},
        };
        params.dd_outputs = {
            {NewP2TRScript(), 1000, DDOutputRole::RECIPIENT},
            {NewP2TRScript(), 995, DDOutputRole::USER_CHANGE},
            {NewP2TRScript(), 105, DDOutputRole::CARRIER_SUCCESSOR},
        };
        params.dgb_outputs = {CTxOut{9 * COIN / 10, CScript{} << OP_TRUE}};
        params.fee_rate = 100000;
        return params;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_txbuilder_tests, CollaborativeFixture)

BOOST_AUTO_TEST_CASE(builds_fixed_unsigned_carrier_transfer)
{
    const auto params = ValidParams();
    const auto first = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    const auto second = BuildUnsignedCollaborativeTransfer(params, Params(), coins);

    BOOST_REQUIRE_MESSAGE(first.success, first.error);
    BOOST_REQUIRE_MESSAGE(second.success, second.error);
    BOOST_CHECK(CTransaction{first.tx} == CTransaction{second.tx});
    BOOST_CHECK_EQUAL(first.miner_fee, COIN / 10);
    BOOST_CHECK_EQUAL(first.tx.vin.size(), 3U);
    BOOST_CHECK_EQUAL(first.tx.vout.size(), 5U);
    BOOST_CHECK(first.tx.IsDigiDollar());
}

BOOST_AUTO_TEST_CASE(rejects_prevout_not_matching_creating_transaction)
{
    auto params = ValidParams();
    params.inputs[0].creating_tx = carrier;
    const auto result = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "PAYMASTER_INVALID_CREATING_TRANSACTION");
}

BOOST_AUTO_TEST_CASE(rejects_dd_conservation_failure)
{
    auto params = ValidParams();
    params.dd_outputs[1].amount = 994;
    const auto result = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "PAYMASTER_DD_CONSERVATION");
}

BOOST_AUTO_TEST_CASE(rejects_sub_minimum_user_change)
{
    auto params = ValidParams();
    params.dd_outputs[1].amount = 99;
    params.dd_outputs[0].amount = 1896;
    const auto result = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "PAYMASTER_INVALID_DD_OUTPUT");
}

BOOST_AUTO_TEST_CASE(rejects_underfunded_miner_fee)
{
    auto params = ValidParams();
    params.dgb_outputs[0].nValue += 1;
    const auto result = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "PAYMASTER_INSUFFICIENT_MINER_FEE");
}

BOOST_AUTO_TEST_CASE(rejects_unknown_input_and_output_roles)
{
    auto params = ValidParams();
    params.inputs[0].role = static_cast<InputRole>(0xff);
    auto result = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "PAYMASTER_INVALID_INPUT_ROLE");

    params = ValidParams();
    params.dd_outputs[0].role = static_cast<DDOutputRole>(0xff);
    result = BuildUnsignedCollaborativeTransfer(params, Params(), coins);
    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "PAYMASTER_INVALID_DD_OUTPUT_ROLE");
}

BOOST_AUTO_TEST_CASE(validates_exact_user_dd_inputs_against_chainstate)
{
    const CollaborativeInput valid{
        COutPoint{user_dd->GetHash(), 0}, user_dd, InputRole::USER_DD};
    ValidatedUserDDInputs validated;
    std::string error;
    BOOST_REQUIRE_MESSAGE(
        ValidateCollaborativeUserDDInputs({valid}, coins, validated, error),
        error);
    BOOST_CHECK_EQUAL(validated.total_dd_amount, 2000);
    BOOST_REQUIRE_EQUAL(validated.output_keys.size(), 1U);
    BOOST_CHECK(validated.output_keys.front().IsFullyValid());

    BOOST_CHECK(!ValidateCollaborativeUserDDInputs({}, coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_NO_INPUTS");

    auto wrong_role = valid;
    wrong_role.role = InputRole::PROVIDER_CARRIER;
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {wrong_role}, coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_INPUT_ROLE");

    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {valid, valid}, coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DUPLICATE_INPUT");

    auto missing_tx = valid;
    missing_tx.creating_tx.reset();
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {missing_tx}, coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CREATING_TRANSACTION");

    auto wrong_hash = valid;
    wrong_hash.creating_tx = carrier;
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {wrong_hash}, coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CREATING_TRANSACTION");

    auto bad_index = valid;
    bad_index.outpoint.n = 99;
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {bad_index}, coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CREATING_TRANSACTION");

    const CTransactionRef absent = MakeDDSource(300, 101);
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{COutPoint{absent->GetHash(), 0}, absent, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PREVOUT_MISMATCH");
}

BOOST_AUTO_TEST_CASE(rejects_nonexact_coin_coinbase_amount_and_p2tr)
{
    ValidatedUserDDInputs validated;
    std::string error;

    const CTransactionRef mismatched = MakeDDSource(300, 102);
    const COutPoint mismatched_outpoint{mismatched->GetHash(), 0};
    coins.AddCoin(mismatched_outpoint,
                  Coin{CTxOut{1, mismatched->vout[0].scriptPubKey}, 500, false}, false);
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{mismatched_outpoint, mismatched, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PREVOUT_MISMATCH");

    const CTransactionRef coinbase_coin = MakeDDSource(300, 103);
    const COutPoint coinbase_outpoint{coinbase_coin->GetHash(), 0};
    coins.AddCoin(coinbase_outpoint,
                  Coin{coinbase_coin->vout[0], 500, true}, false);
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{coinbase_outpoint, coinbase_coin, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PREVOUT_MISMATCH");

    CMutableTransaction coinbase_source;
    coinbase_source.SetDigiDollarType(::DD_TX_TRANSFER);
    coinbase_source.vin.emplace_back(COutPoint{});
    coinbase_source.vout.emplace_back(0, NewP2TRScript());
    coinbase_source.vout.emplace_back(
        0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                     << CScriptNum(DD_TX_TRANSFER) << CScriptNum(300));
    const CTransactionRef coinbase_tx =
        MakeTransactionRef(std::move(coinbase_source));
    const COutPoint coinbase_tx_outpoint{coinbase_tx->GetHash(), 0};
    coins.AddCoin(coinbase_tx_outpoint,
                  Coin{coinbase_tx->vout[0], 500, false}, false);
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{coinbase_tx_outpoint, coinbase_tx, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PREVOUT_MISMATCH");

    const CTransactionRef zero_dd = MakeDDSource(0, 104);
    const COutPoint zero_outpoint{zero_dd->GetHash(), 0};
    coins.AddCoin(zero_outpoint, Coin{zero_dd->vout[0], 500, false}, false);
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{zero_outpoint, zero_dd, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_DD_INPUT");

    const CScript invalid_p2tr =
        CScript{} << OP_1 << std::vector<unsigned char>(32, 0xff);
    const CTransactionRef bad_key = MakeDDSource(300, 105, invalid_p2tr);
    const COutPoint bad_key_outpoint{bad_key->GetHash(), 0};
    coins.AddCoin(bad_key_outpoint,
                  Coin{bad_key->vout[0], 500, false}, false);
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{bad_key_outpoint, bad_key, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_DD_INPUT");

    const CTransactionRef spent = MakeDDSource(300, 106);
    const COutPoint spent_outpoint{spent->GetHash(), 0};
    coins.AddCoin(spent_outpoint, Coin{spent->vout[0], 500, false}, false);
    BOOST_REQUIRE(coins.SpendCoin(spent_outpoint));
    BOOST_CHECK(!ValidateCollaborativeUserDDInputs(
        {{spent_outpoint, spent, InputRole::USER_DD}},
        coins, validated, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PREVOUT_MISMATCH");
}

BOOST_AUTO_TEST_CASE(provider_quote_builder_reconstructs_signed_client_template)
{
    CKey user_key;
    CKey provider_identity_key;
    user_key.MakeNewKey(true);
    provider_identity_key.MakeNewKey(true);
    const auto user_tweak = XOnlyPubKey{user_key.GetPubKey()}.CreateTapTweak(nullptr);
    BOOST_REQUIRE(user_tweak);
    const CScript user_script = CScript{} << OP_1 << ToByteVector(user_tweak->first);
    CMutableTransaction user_source;
    user_source.SetDigiDollarType(::DD_TX_TRANSFER);
    user_source.vin.emplace_back(COutPoint{uint256::ONE, 90});
    user_source.vout.emplace_back(0, user_script);
    user_source.vout.emplace_back(0, CScript{} << OP_RETURN
                                               << std::vector<unsigned char>{'D', 'D'}
                                               << CScriptNum(DD_TX_TRANSFER)
                                               << CScriptNum(2000));
    const CTransactionRef user_prev = MakeTransactionRef(std::move(user_source));
    const COutPoint user_outpoint{user_prev->GetHash(), 0};
    coins.AddCoin(user_outpoint, Coin{user_prev->vout[0], 500, false}, false);

    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_USER_PAID;
    policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    policy.fee_rate_bps = 50;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{1000000};
    policy.maximum_network_fee = DGBSatoshis{COIN / 10};
    const int64_t now{100000};
    PaymasterQuoteRequest request;
    request.intent.genesis_hash = Params().GenesisBlock().GetHash();
    request.intent.provider_id = GetPaymasterId(XOnlyPubKey{provider_identity_key.GetPubKey()});
    request.intent.request_id = "550e8400-e29b-41d4-a716-446655440090";
    request.intent.session_id = uint256S("91");
    request.intent.client_nonce = uint256S("92");
    request.intent.canonical_request_hash = uint256S("9201");
    request.intent.user_dd_inputs = {user_outpoint};
    request.intent.recipient_script = NewP2TRScript();
    request.intent.recipient_amount = DDCents{1000};
    request.intent.user_dd_change_script = NewP2TRScript();
    request.intent.offer_id = uint256S("93");
    request.intent.funding_model = FundingModel::USER_PAID;
    request.intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    request.intent.policy_hash = GetProviderPolicyHash(policy);
    request.intent.expires_at = now + 60;
    UserInputControlProof proof;
    proof.outpoint = user_outpoint;
    proof.signature.resize(64);
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(user_key.SignSchnorr(GetUserInputControlHash(request.intent, user_outpoint),
                                       proof.signature, &empty_merkle_root, uint256{}));
    request.intent.user_input_proofs.push_back(proof);

    const std::vector<CollaborativeInput> user_inputs{
        {user_outpoint, user_prev, InputRole::USER_DD}};
    ProviderQuotePreflightResult preflight;
    std::string error;
    BOOST_REQUIRE_MESSAGE(PreflightProviderQuoteRequest(
                              request, policy, user_inputs, Params(), coins,
                              now, preflight, error),
                          error);
    BOOST_CHECK_EQUAL(preflight.user_inputs.total_dd_amount, 2000);
    BOOST_CHECK_EQUAL(preflight.user_inputs.output_keys.size(), 1U);
    BOOST_CHECK_EQUAL(preflight.fee_plan.service_fee.value, 5);
    BOOST_CHECK_EQUAL(preflight.user_change, 995);

    ProviderQuoteBuildParameters parameters;
    parameters.quote_id = uint256S("94");
    parameters.user_inputs = user_inputs;
    parameters.reserved_carrier = VerifiedDDCarrier{
        COutPoint{carrier->GetHash(), 0}, CMutableTransaction{*carrier}, DDCents{100}};
    parameters.reserved_dgb_inputs = {VerifiedDGBInput{
        COutPoint{provider_dgb->GetHash(), 0}, CMutableTransaction{*provider_dgb},
        DGBSatoshis{COIN}}};
    parameters.carrier_return_script = NewP2TRScript();
    parameters.dgb_change_script = NewP2TRScript();
    parameters.network_fee = DGBSatoshis{COIN / 10};
    parameters.created_at = now;
    parameters.expires_at = now + policy.quote_ttl;
    parameters.retry_until = now + DEFAULT_RETRY_SECONDS;

    ProviderQuoteBuildResult built;
    BOOST_REQUIRE_MESSAGE(BuildUnsignedProviderQuote(
                              request, policy, parameters, Params(), coins, built, error),
                          error);
    BOOST_CHECK_EQUAL(built.quote.service_fee.value, 5);
    BOOST_CHECK_EQUAL(built.trusted_template.input_roles.size(), 3U);
    BOOST_CHECK(built.quote.unsigned_txid == CTransaction{built.quote.unsigned_transaction}.GetHash());
    built.quote.identity_signature.resize(64);
    BOOST_REQUIRE(provider_identity_key.SignSchnorr(
        GetPaymasterQuoteSignatureHash(built.quote), built.quote.identity_signature,
        nullptr, uint256{}));
    BOOST_REQUIRE_MESSAGE(ValidatePaymasterQuote(
                              built.quote, request.intent, policy,
                              XOnlyPubKey{provider_identity_key.GetPubKey()},
                              DDCents{5}, now, error),
                          error);

    parameters.dgb_change_script.reset();
    BOOST_CHECK(!BuildUnsignedProviderQuote(
        request, policy, parameters, Params(), coins, built, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DGB_CHANGE_MISMATCH");

    auto invalid_request = request;
    invalid_request.version = 0;
    BOOST_CHECK(!PreflightProviderQuoteRequest(
        invalid_request, policy, user_inputs, Params(), coins, now,
        preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_REQUEST");

    invalid_request = request;
    invalid_request.intent.genesis_hash = uint256S("ff");
    BOOST_CHECK(!PreflightProviderQuoteRequest(
        invalid_request, policy, user_inputs, Params(), coins, now,
        preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_REQUEST");

    invalid_request = request;
    invalid_request.intent.user_input_proofs[0].signature[0] ^= 1;
    BOOST_CHECK(!PreflightProviderQuoteRequest(
        invalid_request, policy, user_inputs, Params(), coins, now,
        preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");

    invalid_request = request;
    invalid_request.intent.recipient_amount = DDCents{1991};
    invalid_request.intent.user_input_proofs[0].signature.resize(64);
    BOOST_REQUIRE(user_key.SignSchnorr(
        GetUserInputControlHash(invalid_request.intent, user_outpoint),
        invalid_request.intent.user_input_proofs[0].signature,
        &empty_merkle_root, uint256{}));
    BOOST_CHECK(!PreflightProviderQuoteRequest(
        invalid_request, policy, user_inputs, Params(), coins, now,
        preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INSUFFICIENT_USER_DD");

    invalid_request = request;
    invalid_request.intent.user_dd_change_script.clear();
    invalid_request.intent.user_input_proofs[0].signature.resize(64);
    BOOST_REQUIRE(user_key.SignSchnorr(
        GetUserInputControlHash(invalid_request.intent, user_outpoint),
        invalid_request.intent.user_input_proofs[0].signature,
        &empty_merkle_root, uint256{}));
    BOOST_CHECK(!PreflightProviderQuoteRequest(
        invalid_request, policy, user_inputs, Params(), coins, now,
        preflight, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_USER_CHANGE_MISMATCH");
}

BOOST_AUTO_TEST_SUITE_END()
