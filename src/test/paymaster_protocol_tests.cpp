// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Cryptographic commitment and protocol state-machine invariants. */

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/setup_common.h>

using namespace DigiDollar::Paymaster;

namespace {

CScript P2TRScript(const CKey& key)
{
    return GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
}

PaymentIntent SignedIntent(const CKey& user_key,
                           const PaymasterId& provider_id,
                           const uint256& genesis,
                           const uint256& policy_hash,
                           int64_t now)
{
    PaymentIntent intent;
    intent.genesis_hash = genesis;
    intent.provider_id = provider_id;
    intent.request_id = "550e8400-e29b-41d4-a716-446655440010";
    intent.session_id = uint256S("10");
    intent.client_nonce = uint256S("11");
    intent.canonical_request_hash = uint256S("1101");
    intent.user_dd_inputs = {COutPoint{uint256S("12"), 0}};
    intent.recipient_script = P2TRScript(user_key);
    intent.recipient_amount = DDCents{10000};
    intent.user_dd_change_script = P2TRScript(user_key);
    intent.offer_id = uint256S("13");
    intent.funding_model = FundingModel::USER_PAID;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = policy_hash;
    intent.expires_at = now + 60;
    UserInputControlProof proof;
    proof.outpoint = intent.user_dd_inputs.front();
    proof.signature.resize(64);
    BOOST_REQUIRE(user_key.SignSchnorr(GetUserInputControlHash(intent, proof.outpoint),
                                       proof.signature, nullptr, uint256{}));
    intent.user_input_proofs.push_back(std::move(proof));
    return intent;
}

ProviderPolicy Policy()
{
    ProviderPolicy policy;
    policy.funding_models = FUNDING_MODEL_USER_PAID;
    policy.fee_rate_bps = 100;
    policy.min_payment = DDCents{100};
    policy.max_payment = DDCents{1000000};
    policy.maximum_network_fee = DGBSatoshis{20000000};
    return policy;
}

PaymasterQuote SignedQuote(const CKey& provider_key,
                           const PaymentIntent& intent,
                           const ProviderPolicy& policy,
                           int64_t now)
{
    CKey provider_input_key;
    provider_input_key.MakeNewKey(true);
    CMutableTransaction creating_tx;
    creating_tx.vin.emplace_back(COutPoint{uint256S("20"), 0});
    creating_tx.vout.emplace_back(policy.maximum_network_fee.value, P2TRScript(provider_input_key));

    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = uint256S("21");
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.funding_model = intent.funding_model;
    quote.sponsorship_scope = intent.sponsorship_scope;
    quote.fee_rate_bps = policy.fee_rate_bps;
    quote.service_fee = DDCents{100};
    quote.reserved_dgb_inputs.push_back(
        {COutPoint{CTransaction{creating_tx}.GetHash(), 0}, creating_tx, policy.maximum_network_fee});
    quote.provider_fee_script = P2TRScript(provider_key);
    quote.network_fee = DGBSatoshis{1000};
    quote.created_at = now;
    quote.expires_at = now + policy.quote_ttl;
    quote.retry_until = now + DEFAULT_RETRY_SECONDS;
    quote.unsigned_transaction.vin.emplace_back(quote.reserved_dgb_inputs.front().outpoint);
    quote.unsigned_transaction.vout.emplace_back(policy.maximum_network_fee.value - quote.network_fee.value,
                                                  P2TRScript(provider_key));
    quote.unsigned_txid = CTransaction{quote.unsigned_transaction}.GetHash();
    quote.template_commitment = uint256S("22");
    quote.identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(GetPaymasterQuoteSignatureHash(quote),
                                           quote.identity_signature, nullptr, uint256{}));
    return quote;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_protocol_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(manager_semantic_conflicts_are_retained_and_spend_tokens)
{
    Manager manager{true};
    constexpr int64_t now{100000};
    constexpr uint64_t netgroup{303};
    PaymasterCapacityRequest request;
    request.funding_model = FundingModel::SPONSORED;
    request.provider_id = uint256S("e101");
    request.request_id = uint256S("e102").GetHex();
    request.session_id = uint256S("e102");
    request.client_nonce = uint256S("e103");
    const PaymasterCapacityRequest original{request};
    BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                      1, uint256S("e104"), 1, DirectPayload{request}, now,
                      netgroup) == DirectEnqueueResult::ACCEPTED);
    BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(request.provider_id, 1).size(), 1U);

    request.client_nonce = uint256S("e105");
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, uint256S("e106"), 1, DirectPayload{request}, now,
                    netgroup) == DirectEnqueueResult::CONFLICT);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    const auto retained_conflict = manager.TakeCapacityRequests(request.provider_id, 1);
    BOOST_REQUIRE_EQUAL(retained_conflict.size(), 1U);
    BOOST_CHECK(retained_conflict.front().message_id == uint256S("e106"));

    for (size_t i = 1; i + 1 < MAX_DIRECT_PAYLOAD_MESSAGES_PER_PEER; ++i) {
        uint256 id;
        id.begin()[0] = static_cast<unsigned char>(i + 16);
        request.request_id = id.GetHex();
        request.session_id = id;
        request.client_nonce = id;
        BOOST_REQUIRE(manager.EnqueueDirectMessageResult(
                          1, id, 1, DirectPayload{request}, now,
                          netgroup) == DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(request.provider_id, 1).size(), 1U);
    }

    // The original, retained conflict, and legitimate burst fill the decoded
    // peer bucket exactly. A consumed exact replay remains idempotent but is
    // still rate-limited before expensive redelivery.
    BOOST_CHECK(manager.EnqueueDirectMessageResult(
                    1, uint256S("e104"), 1, DirectPayload{original}, now,
                    netgroup) == DirectEnqueueResult::RATE_LIMITED);
}

BOOST_AUTO_TEST_CASE(intent_control_proof_binds_provider_session_and_payment)
{
    CKey user_key;
    CKey provider_key;
    user_key.MakeNewKey(true);
    provider_key.MakeNewKey(true);
    const int64_t now{100000};
    const uint256 genesis = uint256S("30");
    const ProviderPolicy policy = Policy();
    auto intent = SignedIntent(user_key, GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()}),
                               genesis, GetProviderPolicyHash(policy), now);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidatePaymentIntent(intent, genesis, now,
                                                {XOnlyPubKey{user_key.GetPubKey()}}, error),
                          error);
    const PaymentIntent authorized{intent};

    for (const uint16_t unsupported_version :
         {PaymentIntent::LEGACY_VERSION,
          uint16_t{PaymentIntent::CURRENT_VERSION + 1}}) {
        PaymentIntent wrong_protocol{authorized};
        wrong_protocol.version = unsupported_version;
        BOOST_CHECK(!ValidatePaymentIntent(
            wrong_protocol, genesis, now,
            {XOnlyPubKey{user_key.GetPubKey()}}, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN");
    }

    intent.recipient_amount.value += 1;
    BOOST_CHECK(!ValidatePaymentIntent(intent, genesis, now,
                                       {XOnlyPubKey{user_key.GetPubKey()}}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");

    intent = authorized;
    intent.canonical_request_hash = uint256S("1102");
    BOOST_CHECK(!ValidatePaymentIntent(intent, genesis, now,
                                       {XOnlyPubKey{user_key.GetPubKey()}}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");
    intent = authorized;
    intent.requested_fee_mode = FeeMode::AUTO;
    BOOST_CHECK(!ValidatePaymentIntent(intent, genesis, now,
                                       {XOnlyPubKey{user_key.GetPubKey()}}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");
    intent = authorized;
    intent.privacy_profile = PrivacyProfile::HIGH;
    BOOST_CHECK(!ValidatePaymentIntent(intent, genesis, now,
                                       {XOnlyPubKey{user_key.GetPubKey()}}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");
    intent = authorized;
    intent.selection_mode = SelectionMode::PRIVACY_WEIGHTED;
    BOOST_CHECK(!ValidatePaymentIntent(intent, genesis, now,
                                       {XOnlyPubKey{user_key.GetPubKey()}}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");
}

BOOST_AUTO_TEST_CASE(quote_binds_exact_fee_policy_prevouts_and_unsigned_transaction)
{
    CKey user_key;
    CKey provider_key;
    user_key.MakeNewKey(true);
    provider_key.MakeNewKey(true);
    const int64_t now{100000};
    const uint256 genesis = uint256S("30");
    const ProviderPolicy policy = Policy();
    const auto intent = SignedIntent(user_key, GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()}),
                                     genesis, GetProviderPolicyHash(policy), now);
    auto quote = SignedQuote(provider_key, intent, policy, now);
    const OfferTerms offer{intent.offer_id, intent.policy_hash, FundingModel::USER_PAID,
                           SponsorshipScope::PUBLIC, policy.fee_rate_bps,
                           policy.min_payment, policy.max_payment};
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidatePaymasterQuote(quote, intent, policy,
                                                 XOnlyPubKey{provider_key.GetPubKey()},
                                                 DDCents{100}, now, error), error);
    BOOST_REQUIRE_MESSAGE(ValidatePaymasterQuoteForClient(
                              quote, intent, offer, XOnlyPubKey{provider_key.GetPubKey()},
                              DDCents{100}, now, error), error);

    OfferTerms wrong_offer = offer;
    wrong_offer.policy_hash = uint256S("32");
    BOOST_CHECK(!ValidatePaymasterQuoteForClient(
        quote, intent, wrong_offer, XOnlyPubKey{provider_key.GetPubKey()},
        DDCents{100}, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_BINDING_MISMATCH");

    PaymasterQuote excessive = quote;
    excessive.service_fee = DDCents{101};
    BOOST_CHECK(!ValidatePaymasterQuote(excessive, intent, policy,
                                        XOnlyPubKey{provider_key.GetPubKey()},
                                        DDCents{100}, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_FEE_MISMATCH");
    quote.unsigned_txid = uint256S("31");
    BOOST_CHECK(!ValidatePaymasterQuote(quote, intent, policy,
                                        XOnlyPubKey{provider_key.GetPubKey()},
                                        DDCents{100}, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_QUOTE_TRANSACTION");
}

BOOST_AUTO_TEST_CASE(result_requires_complete_consistent_final_artifact_and_monotonic_sequence)
{
    CKey provider_key;
    provider_key.MakeNewKey(true);
    const uint256 provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    PaymasterResult result;
    result.genesis_hash = uint256S("30");
    result.provider_id = provider_id;
    result.commit_key = uint256S("40");
    result.result_sequence = 2;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{uint256S("41"), 0});
    transaction.vout.emplace_back(1, CScript{} << OP_TRUE);
    result.final_transaction = transaction;
    result.txid = CTransaction{transaction}.GetHash();
    result.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
    result.updated_at = 100;
    result.identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                           result.identity_signature, nullptr, uint256{}));
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidatePaymasterResult(result, result.genesis_hash, provider_id,
                                                  result.commit_key,
                                                  XOnlyPubKey{provider_key.GetPubKey()},
                                                  2, error), error);
    BOOST_CHECK(!ValidatePaymasterResult(result, result.genesis_hash, provider_id,
                                         result.commit_key,
                                         XOnlyPubKey{provider_key.GetPubKey()},
                                         3, error));
    result.raw_transaction_hash.reset();
    BOOST_CHECK(!ValidatePaymasterResult(result, result.genesis_hash, provider_id,
                                         result.commit_key,
                                         XOnlyPubKey{provider_key.GetPubKey()},
                                         2, error));
}

BOOST_AUTO_TEST_CASE(optional_fields_round_trip_canonically)
{
    PaymasterResult original;
    original.genesis_hash = uint256S("50");
    original.provider_id = uint256S("51");
    original.commit_key = uint256S("52");
    original.result_sequence = 1;
    original.status = PaymasterResultStatus::SLOT_UNAVAILABLE;
    original.updated_at = 100;
    original.identity_signature.resize(64);
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << original;
    PaymasterResult decoded;
    stream >> decoded;
    BOOST_CHECK(!decoded.txid);
    BOOST_CHECK(!decoded.raw_transaction_hash);
    BOOST_CHECK(!decoded.final_transaction);
    BOOST_CHECK(decoded.status == original.status);
}

BOOST_AUTO_TEST_SUITE_END()
