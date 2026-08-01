// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Role-aware PSBT manifest and mutation-rejection tests. */

#include <hash.h>
#include <paymaster/psbt.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/wire.h>
#include <streams.h>

#include <boost/test/unit_test.hpp>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_psbt_tests)

namespace {

CTransactionRef MakePrevTx(CAmount value, uint32_t discriminator)
{
    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{uint256::ONE, discriminator});
    transaction.vout.emplace_back(value, CScript{} << OP_DROP << OP_TRUE);
    return MakeTransactionRef(std::move(transaction));
}

CollaborativePSBTTemplate MakeTemplate()
{
    const CTransactionRef user_prev = MakePrevTx(1000, 1);
    const CTransactionRef provider_prev = MakePrevTx(2000, 2);
    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{user_prev->GetHash(), 0});
    transaction.vin.emplace_back(COutPoint{provider_prev->GetHash(), 0});
    transaction.vout.emplace_back(2500, CScript{} << OP_TRUE);

    CollaborativePSBTTemplate result;
    std::string error;
    BOOST_REQUIRE(CreateCollaborativePSBTTemplate(
        transaction,
        {{transaction.vin[0].prevout, user_prev, InputRole::USER_DGB},
         {transaction.vin[1].prevout, provider_prev, InputRole::PROVIDER_DGB}},
        result, error));
    return result;
}

} // namespace

BOOST_AUTO_TEST_CASE(requires_exact_template_and_default_sighash)
{
    const CollaborativePSBTTemplate trusted = MakeTemplate();
    PartiallySignedTransaction candidate{trusted.psbt};
    std::string error;
    BOOST_CHECK(ValidateCollaborativePSBT(candidate, trusted,
                                          CollaborativeSignatureStage::UNSIGNED, error));

    candidate.inputs[0].sighash_type = SIGHASH_ALL;
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGHASH");

    candidate = trusted.psbt;
    candidate.unknown[{0x42}] = {0x01};
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_GLOBAL_FIELDS");

    candidate = trusted.psbt;
    candidate.outputs[0].unknown[{0x42}] = {0x01};
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_OUTPUT_FIELDS");

    candidate = trusted.psbt;
    candidate.inputs[0].unknown[{0x42}] = {0x01};
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_INPUT_FIELDS");

    candidate = trusted.psbt;
    candidate.tx->vin[0].prevout = COutPoint{uint256S("43"), 0};
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_TRANSACTION_MISMATCH");

    candidate = trusted.psbt;
    candidate.tx->vin.emplace_back(COutPoint{uint256S("44"), 0});
    candidate.inputs.emplace_back();
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SHAPE_MISMATCH");

    candidate = trusted.psbt;
    candidate.tx->vout[0].scriptPubKey = CScript{} << OP_FALSE;
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_TRANSACTION_MISMATCH");

    candidate = trusted.psbt;
    candidate.tx->vout.emplace_back(1, CScript{} << OP_TRUE);
    candidate.outputs.emplace_back();
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SHAPE_MISMATCH");

    CollaborativePSBTTemplate unknown_role{trusted};
    unknown_role.input_roles[0] = static_cast<InputRole>(0xff);
    candidate = trusted.psbt;
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, unknown_role,
                                           CollaborativeSignatureStage::UNSIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_INPUT_ROLE");
}

BOOST_AUTO_TEST_CASE(enforces_role_order_and_verifies_final_signatures)
{
    const CollaborativePSBTTemplate trusted = MakeTemplate();
    PartiallySignedTransaction candidate{trusted.psbt};
    std::string error;

    candidate.inputs[0].final_script_sig = CScript{} << OP_0;
    candidate.inputs[1].final_script_sig = CScript{} << OP_0;
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::USER_SIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_UNEXPECTED_SIGNATURE");

    candidate = trusted.psbt;
    candidate.inputs[0].final_script_sig = CScript{} << OP_0;
    BOOST_REQUIRE(ValidateCollaborativePSBT(candidate, trusted,
                                            CollaborativeSignatureStage::USER_SIGNED, error));

    candidate.inputs[1].final_script_sig = CScript{} << OP_0;
    BOOST_CHECK(ValidateCollaborativePSBT(candidate, trusted,
                                          CollaborativeSignatureStage::FULLY_SIGNED, error));

    candidate.inputs[0].final_script_sig = CScript{} << OP_0 << OP_0;
    BOOST_CHECK(!ValidateCollaborativePSBT(candidate, trusted,
                                           CollaborativeSignatureStage::FULLY_SIGNED, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");
}

BOOST_AUTO_TEST_CASE(template_commitment_binds_quote_psbt_prevouts_and_roles_without_recursion)
{
    const CollaborativePSBTTemplate trusted = MakeTemplate();
    PaymentIntent intent;
    intent.genesis_hash = uint256S("71");
    intent.provider_id = uint256S("72");
    intent.request_id = "550e8400-e29b-41d4-a716-446655440071";
    intent.session_id = uint256S("73");
    intent.client_nonce = uint256S("74");
    intent.canonical_request_hash = uint256S("7401");
    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = uint256S("75");
    quote.network_fee = DGBSatoshis{10000000};
    quote.unsigned_transaction = *trusted.psbt.tx;
    quote.unsigned_txid = CTransaction{quote.unsigned_transaction}.GetHash();

    const uint256 commitment = GetCollaborativeTemplateCommitment(intent, quote, trusted);
    BOOST_CHECK(!commitment.IsNull());

    PaymasterQuote recursive_fields = quote;
    recursive_fields.template_commitment = uint256S("76");
    recursive_fields.identity_signature.assign(64, 1);
    BOOST_CHECK(GetCollaborativeTemplateCommitment(intent, recursive_fields, trusted) == commitment);

    PaymasterQuote changed_quote = quote;
    changed_quote.network_fee.value++;
    BOOST_CHECK(GetCollaborativeTemplateCommitment(intent, changed_quote, trusted) != commitment);

    CollaborativePSBTTemplate changed_role = trusted;
    changed_role.input_roles[0] = InputRole::PROVIDER_DGB;
    BOOST_CHECK(GetCollaborativeTemplateCommitment(intent, quote, changed_role) != commitment);

    CollaborativePSBTTemplate changed_prevout = trusted;
    changed_prevout.psbt.inputs[0].non_witness_utxo = MakePrevTx(1001, 1);
    BOOST_CHECK(GetCollaborativeTemplateCommitment(intent, quote, changed_prevout) != commitment);

    CollaborativePSBTTemplate changed_output = trusted;
    changed_output.psbt.tx->vout[0].nValue++;
    BOOST_CHECK(GetCollaborativeTemplateCommitment(intent, quote, changed_output) != commitment);

    CollaborativePSBTTemplate changed_sighash = trusted;
    changed_sighash.psbt.inputs[0].sighash_type = SIGHASH_ALL;
    BOOST_CHECK(GetCollaborativeTemplateCommitment(intent, quote, changed_sighash) != commitment);
}

BOOST_AUTO_TEST_CASE(authorization_manifests_fail_closed_on_asset_or_role_changes)
{
    const CTransactionRef user_prev = MakePrevTx(1000, 11);
    const CTransactionRef provider_prev = MakePrevTx(2000, 12);
    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{user_prev->GetHash(), 0});
    transaction.vin.emplace_back(COutPoint{provider_prev->GetHash(), 0});
    transaction.vout.emplace_back(2990, CScript{} << OP_TRUE);
    CollaborativePSBTTemplate trusted;
    std::string error;
    BOOST_REQUIRE(CreateCollaborativePSBTTemplate(
        transaction,
        {{transaction.vin[0].prevout, user_prev, InputRole::USER_DD},
         {transaction.vin[1].prevout, provider_prev, InputRole::PROVIDER_DGB}},
        trusted, error));

    PaymentIntent intent;
    intent.genesis_hash = uint256S("81");
    intent.provider_id = uint256S("82");
    intent.request_id = "550e8400-e29b-41d4-a716-446655440081";
    intent.session_id = uint256S("83");
    intent.client_nonce = uint256S("84");
    intent.canonical_request_hash = uint256S("8401");
    intent.user_dd_inputs = {transaction.vin[0].prevout};
    intent.recipient_script = CScript{} << OP_TRUE;
    intent.recipient_amount = DDCents{900};
    intent.user_dd_change_script = CScript{} << OP_TRUE;
    intent.offer_id = uint256S("85");
    intent.policy_hash = uint256S("86");
    intent.funding_model = FundingModel::SPONSORED;
    intent.expires_at = 1100;

    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = uint256S("87");
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.funding_model = intent.funding_model;
    quote.service_fee = DDCents{0};
    quote.reserved_dgb_inputs.push_back(
        {transaction.vin[1].prevout, CMutableTransaction{*provider_prev},
         DGBSatoshis{2000}});
    quote.dgb_change_script = CScript{} << OP_TRUE;
    quote.network_fee = DGBSatoshis{10};
    quote.created_at = 1000;
    quote.expires_at = 1060;
    quote.retry_until = 2000;
    quote.unsigned_transaction = transaction;
    quote.unsigned_txid = CTransaction{transaction}.GetHash();
    quote.template_commitment =
        GetCollaborativeTemplateCommitment(intent, quote, trusted);

    ValidatedCapacitySnapshot capacity;
    PaymasterCapacityProof capacity_proof;
    capacity_proof.genesis_hash = intent.genesis_hash;
    capacity_proof.provider_id = intent.provider_id;
    capacity_proof.request_id = intent.request_id;
    capacity_proof.session_id = intent.session_id;
    capacity_proof.client_nonce = intent.client_nonce;
    capacity_proof.funding_model = intent.funding_model;
    capacity_proof.requires_carrier = false;
    capacity_proof.snapshot_id = uint256S("88");
    capacity_proof.created_at = 990;
    capacity_proof.expires_at = 1050;
    CapacityDGBInput capacity_dgb;
    capacity_dgb.input = quote.reserved_dgb_inputs.front();
    capacity_dgb.control_proof.reference_block = uint256S("89");
    capacity_dgb.control_proof.expires_at = capacity_proof.expires_at;
    capacity_dgb.control_proof.signature.assign(64, 1);
    PaymasterLiquiditySlot capacity_slot;
    capacity_slot.dgb_inputs = {capacity_dgb};
    capacity_proof.liquidity_slots = {capacity_slot};
    capacity_proof.identity_signature.assign(64, 1);
    CDataStream capacity_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    capacity_stream << capacity_proof;
    const auto capacity_bytes = MakeUCharSpan(capacity_stream);
    capacity.snapshot_id = capacity_proof.snapshot_id;
    capacity.resource_commitment = GetCapacityResourceCommitment(capacity_proof);
    capacity.session_id = intent.session_id;
    capacity.attempt_id = uint256S("8a");
    capacity.provider_id = intent.provider_id;
    capacity.client_nonce = intent.client_nonce;
    capacity.request_hash = uint256S("8b");
    capacity.capacity_proof.assign(capacity_bytes.begin(), capacity_bytes.end());
    capacity.created_at = capacity_proof.created_at;
    capacity.expires_at = capacity_proof.expires_at;
    capacity.validated_at = 995;
    capacity.funding_model = capacity_proof.funding_model;
    capacity.requires_carrier = capacity_proof.requires_carrier;

    ClientAuthorizationManifest client;
    BOOST_REQUIRE(BuildClientAuthorizationManifest(
        intent, quote, capacity, trusted, DDCents{5}, client, error));
    BOOST_CHECK(ValidateClientAuthorizationManifest(
        client, intent, quote, capacity, trusted, error));
    // Property invariant: all client-controlled asset outflows are exact
    // copies of the locally approved request and quote before any signature.
    BOOST_CHECK(client.user_dd_inputs == intent.user_dd_inputs);
    BOOST_CHECK(client.recipient_script == intent.recipient_script);
    BOOST_CHECK(client.recipient_amount == intent.recipient_amount);
    BOOST_CHECK(client.user_dd_change_script == intent.user_dd_change_script);
    BOOST_CHECK(client.service_fee == quote.service_fee);
    BOOST_CHECK(client.capacity_client_nonce == intent.client_nonce);
    BOOST_CHECK(client.capacity_request_hash == capacity.request_hash);
    BOOST_CHECK(client.capacity_proof_hash == Hash(capacity.capacity_proof));
    BOOST_CHECK(client.canonical_request_hash ==
                intent.canonical_request_hash);
    BOOST_CHECK(client.requested_fee_mode == intent.requested_fee_mode);
    BOOST_CHECK(client.privacy_profile == intent.privacy_profile);
    BOOST_CHECK(client.selection_mode == intent.selection_mode);

    ClientAuthorizationManifest exact_gross;
    BOOST_REQUIRE(BuildClientAuthorizationManifest(
        intent, quote, capacity, trusted, DDCents{5}, DDCents{900},
        /*subtract_paymaster_fee_from_amount=*/true,
        /*send_all_spendable_dd=*/true, exact_gross, error));
    BOOST_CHECK(ValidateClientAuthorizationManifest(
        exact_gross, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(exact_gross.requested_amount.value, 900);
    BOOST_CHECK(exact_gross.subtract_paymaster_fee_from_amount);
    BOOST_CHECK(exact_gross.send_all_spendable_dd);

    ClientAuthorizationManifest wrong_gross{exact_gross};
    wrong_gross.requested_amount = DDCents{901};
    wrong_gross.manifest_id = GetClientAuthorizationManifestId(wrong_gross);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        wrong_gross, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_GROSS_AMOUNT_MISMATCH");

    ClientAuthorizationManifest changed_order{client};
    changed_order.canonical_request_hash = uint256S("8c01");
    changed_order.manifest_id = GetClientAuthorizationManifestId(changed_order);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        changed_order, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    changed_order = client;
    changed_order.requested_fee_mode = FeeMode::AUTO;
    changed_order.manifest_id = GetClientAuthorizationManifestId(changed_order);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        changed_order, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    changed_order = client;
    changed_order.privacy_profile = PrivacyProfile::HIGH;
    changed_order.manifest_id = GetClientAuthorizationManifestId(changed_order);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        changed_order, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    changed_order = client;
    changed_order.selection_mode = SelectionMode::PRIVACY_WEIGHTED;
    changed_order.manifest_id = GetClientAuthorizationManifestId(changed_order);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        changed_order, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    ValidatedCapacitySnapshot rebound_request{capacity};
    rebound_request.request_hash = uint256S("8c");
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        client, intent, quote, rebound_request, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    ValidatedCapacitySnapshot mutated_proof{capacity};
    BOOST_REQUIRE(!mutated_proof.capacity_proof.empty());
    mutated_proof.capacity_proof.back() ^= 1;
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        client, intent, quote, mutated_proof, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    ValidatedCapacitySnapshot rebound_nonce{capacity};
    rebound_nonce.client_nonce = uint256S("8d");
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        client, intent, quote, rebound_nonce, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");
    PaymasterQuote unproven_capacity{quote};
    unproven_capacity.reserved_dgb_inputs.front().value.value++;
    unproven_capacity.template_commitment = GetCollaborativeTemplateCommitment(
        intent, unproven_capacity, trusted);
    ClientAuthorizationManifest unproven_manifest;
    BOOST_CHECK(!BuildClientAuthorizationManifest(
        intent, unproven_capacity, capacity, trusted, DDCents{5},
        unproven_manifest, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_QUOTE_MISMATCH");

    PaymasterQuote wrong_capacity_model{quote};
    wrong_capacity_model.funding_model = FundingModel::USER_PAID;
    BOOST_CHECK(!ValidateQuoteAgainstCapacitySnapshot(
        wrong_capacity_model, capacity, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH");

    PaymasterQuote unexpected_carrier{quote};
    unexpected_carrier.reserved_carrier = VerifiedDDCarrier{
        COutPoint{uint256S("8e"), 0}, CMutableTransaction{}, DDCents{100}};
    BOOST_CHECK(!ValidateQuoteAgainstCapacitySnapshot(
        unexpected_carrier, capacity, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_QUOTE_MISMATCH");

    PaymasterQuote wrong_dgb_outpoint{quote};
    wrong_dgb_outpoint.reserved_dgb_inputs.front().outpoint.n++;
    BOOST_CHECK(!ValidateQuoteAgainstCapacitySnapshot(
        wrong_dgb_outpoint, capacity, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_QUOTE_MISMATCH");

    PaymasterCapacityProof carrier_proof{capacity_proof};
    carrier_proof.funding_model = FundingModel::USER_PAID;
    carrier_proof.requires_carrier = true;
    CapacityDDCarrier proven_carrier;
    proven_carrier.carrier = VerifiedDDCarrier{
        COutPoint{uint256S("8f"), 0}, CMutableTransaction{}, DDCents{100}};
    proven_carrier.control_proof.reference_block = uint256S("90");
    proven_carrier.control_proof.expires_at = carrier_proof.expires_at;
    proven_carrier.control_proof.signature.assign(64, 1);
    carrier_proof.liquidity_slots.front().carrier = proven_carrier;
    ValidatedCapacitySnapshot carrier_capacity{capacity};
    carrier_capacity.resource_commitment =
        GetCapacityResourceCommitment(carrier_proof);
    CDataStream carrier_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    carrier_stream << carrier_proof;
    const auto carrier_bytes = MakeUCharSpan(carrier_stream);
    carrier_capacity.capacity_proof.assign(
        carrier_bytes.begin(), carrier_bytes.end());
    carrier_capacity.funding_model = FundingModel::USER_PAID;
    carrier_capacity.requires_carrier = true;
    PaymasterQuote carrier_quote{quote};
    carrier_quote.funding_model = FundingModel::USER_PAID;
    carrier_quote.service_fee = DDCents{5};
    carrier_quote.reserved_carrier = proven_carrier.carrier;
    BOOST_REQUIRE_MESSAGE(ValidateQuoteAgainstCapacitySnapshot(
                              carrier_quote, carrier_capacity, error),
                          error);
    PaymasterQuote missing_carrier{carrier_quote};
    missing_carrier.reserved_carrier.reset();
    BOOST_CHECK(!ValidateQuoteAgainstCapacitySnapshot(
        missing_carrier, carrier_capacity, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_QUOTE_MISMATCH");
    carrier_quote.reserved_carrier->outpoint.n++;
    BOOST_CHECK(!ValidateQuoteAgainstCapacitySnapshot(
        carrier_quote, carrier_capacity, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_QUOTE_MISMATCH");

    ClientAuthorizationManifest changed_recipient{client};
    changed_recipient.recipient_amount.value++;
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        changed_recipient, intent, quote, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID");

    PaymasterQuote increased_service_fee{quote};
    increased_service_fee.service_fee = DDCents{1};
    increased_service_fee.template_commitment =
        GetCollaborativeTemplateCommitment(intent, increased_service_fee, trusted);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        client, intent, increased_service_fee, capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    PaymentIntent changed_user_change{intent};
    changed_user_change.user_dd_change_script = CScript{} << OP_FALSE;
    PaymasterQuote changed_user_change_quote{quote};
    changed_user_change_quote.intent_hash =
        GetPaymentIntentHash(changed_user_change);
    changed_user_change_quote.template_commitment =
        GetCollaborativeTemplateCommitment(
            changed_user_change, changed_user_change_quote, trusted);
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        client, changed_user_change, changed_user_change_quote,
        capacity, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_BINDING_MISMATCH");

    CollaborativePSBTTemplate user_dgb{trusted};
    user_dgb.input_roles.front() = InputRole::USER_DGB;
    ClientAuthorizationManifest rebound_client{client};
    rebound_client.template_commitment =
        GetCollaborativeTemplateCommitment(intent, quote, user_dgb);
    rebound_client.manifest_id = GetClientAuthorizationManifestId(rebound_client);
    PaymasterQuote rebound_quote{quote};
    rebound_quote.template_commitment = rebound_client.template_commitment;
    BOOST_CHECK(!ValidateClientAuthorizationManifest(
        rebound_client, intent, rebound_quote, capacity, user_dgb, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_AUTH_USER_INPUT_MISMATCH");

    ProviderAuthorizationManifest provider;
    BOOST_REQUIRE(BuildProviderAuthorizationManifest(
        intent, quote, trusted, uint256S("8c"), provider, error));
    BOOST_CHECK(ValidateProviderAuthorizationManifest(
        provider, intent, quote, trusted, error));
    // Property invariant: provider signatures may consume only the locally
    // reserved pool inputs and the exact fee/change authority in the manifest.
    BOOST_REQUIRE_EQUAL(provider.provider_dgb_inputs.size(), 1U);
    BOOST_CHECK(provider.provider_dgb_inputs.front() ==
                quote.reserved_dgb_inputs.front().outpoint);
    BOOST_CHECK(provider.dgb_change_scripts ==
                std::vector<CScript>{*quote.dgb_change_script});
    BOOST_CHECK(provider.network_fee == quote.network_fee);
    BOOST_CHECK(provider.budget_reservation_id == GetPaymasterCommitKey(
        quote.provider_id, intent.client_nonce, quote.intent_hash,
        quote.quote_id, quote.template_commitment));
    BOOST_CHECK(provider.maximum_network_fee == quote.network_fee);
    ProviderAuthorizationManifest changed_fee{provider};
    changed_fee.network_fee.value++;
    BOOST_CHECK(!ValidateProviderAuthorizationManifest(
        changed_fee, intent, quote, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID");

    ProviderAuthorizationManifest changed_budget{provider};
    changed_budget.maximum_network_fee.value++;
    changed_budget.manifest_id =
        GetProviderAuthorizationManifestId(changed_budget);
    BOOST_CHECK(!ValidateProviderAuthorizationManifest(
        changed_budget, intent, quote, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_BINDING_MISMATCH");

    PaymasterQuote increased_network_fee{quote};
    increased_network_fee.network_fee.value++;
    increased_network_fee.template_commitment =
        GetCollaborativeTemplateCommitment(intent, increased_network_fee, trusted);
    BOOST_CHECK(!ValidateProviderAuthorizationManifest(
        provider, intent, increased_network_fee, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_BINDING_MISMATCH");

    PaymasterQuote redirected_change{quote};
    redirected_change.dgb_change_script = CScript{} << OP_FALSE;
    redirected_change.template_commitment =
        GetCollaborativeTemplateCommitment(intent, redirected_change, trusted);
    BOOST_CHECK(!ValidateProviderAuthorizationManifest(
        provider, intent, redirected_change, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_SUITE_END()
