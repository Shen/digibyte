// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Wallet signing-firewall tests for client and provider manifests. */

#include <boost/test/unit_test.hpp>

#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/reservation.h>
#include <paymaster/wire.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <version.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

namespace wallet {
using namespace DigiDollar::Paymaster;
namespace {

template <typename T>
std::vector<unsigned char> SerializePaymasterTestArtifact(const T& value)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<COutPoint> InputsOwnedBy(
    const PartiallySignedTransaction& psbt,
    const CollaborativePSBTTemplate& trusted,
    SigningParty party)
{
    std::vector<COutPoint> inputs;
    if (!psbt.tx || psbt.tx->vin.size() != trusted.input_roles.size()) {
        return inputs;
    }
    for (size_t index = 0; index < trusted.input_roles.size(); ++index) {
        if (IsInputOwnedBy(trusted.input_roles[index], party)) {
            inputs.push_back(psbt.tx->vin[index].prevout);
        }
    }
    return inputs;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_psbt_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(wallet_with_both_keys_signs_only_requested_role)
{
    CKey key;
    key.MakeNewKey(true);
    FlatSigningProvider provider;
    std::string error;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeSecret(key) + ")", provider, error, /*require_checksum=*/false);
    BOOST_REQUIRE(descriptor);
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        BOOST_REQUIRE(m_wallet.AddWalletDescriptor(wallet_descriptor, provider, "", false));
    }

    TaprootBuilder taproot;
    taproot.Finalize(XOnlyPubKey{key.GetPubKey()});
    const CScript script = GetScriptForDestination(taproot.GetOutput());
    CMutableTransaction user_previous;
    user_previous.vin.emplace_back(COutPoint{uint256::ONE, 1});
    user_previous.vout.emplace_back(2000, script);
    const CTransactionRef user_tx = MakeTransactionRef(std::move(user_previous));
    CMutableTransaction provider_previous;
    provider_previous.vin.emplace_back(COutPoint{uint256::ONE, 2});
    provider_previous.vout.emplace_back(2000, script);
    const CTransactionRef provider_tx = MakeTransactionRef(std::move(provider_previous));

    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{user_tx->GetHash(), 0});
    transaction.vin.emplace_back(COutPoint{provider_tx->GetHash(), 0});
    transaction.vout.emplace_back(3000, CScript{} << OP_TRUE);
    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE(CreateCollaborativePSBTTemplate(
        transaction,
        {{transaction.vin[0].prevout, user_tx, InputRole::USER_DGB},
         {transaction.vin[1].prevout, provider_tx, InputRole::PROVIDER_DGB}},
        trusted, error));

    PartiallySignedTransaction psbt{trusted.psbt};
    BOOST_REQUIRE(SignCollaborativePSBTForParty(m_wallet, psbt, trusted, SigningParty::USER, error));
    BOOST_CHECK(PSBTInputSigned(psbt.inputs[0]));
    BOOST_CHECK(!PSBTInputSigned(psbt.inputs[1]));
    BOOST_CHECK(ValidateCollaborativePSBT(psbt, trusted,
                                          CollaborativeSignatureStage::USER_SIGNED, error));

    CDataStream serialized{SER_NETWORK, ::PROTOCOL_VERSION};
    serialized << psbt;
    PartiallySignedTransaction roundtripped;
    serialized >> roundtripped;
    BOOST_CHECK(!roundtripped.inputs[0].sighash_type);
    BOOST_REQUIRE(roundtripped.inputs[1].sighash_type);
    BOOST_CHECK_EQUAL(*roundtripped.inputs[1].sighash_type, SIGHASH_DEFAULT);
    BOOST_CHECK(ValidateCollaborativePSBT(roundtripped, trusted,
                                          CollaborativeSignatureStage::USER_SIGNED, error));
    psbt = std::move(roundtripped);

    BOOST_REQUIRE(SignCollaborativePSBTForParty(m_wallet, psbt, trusted, SigningParty::PROVIDER, error));
    BOOST_CHECK(PSBTInputSigned(psbt.inputs[0]));
    BOOST_CHECK(PSBTInputSigned(psbt.inputs[1]));
    BOOST_CHECK(ValidateCollaborativePSBT(psbt, trusted,
                                          CollaborativeSignatureStage::FULLY_SIGNED, error));

    CMutableTransaction final_transaction;
    PartiallySignedTransaction extraction_source{psbt};
    BOOST_REQUIRE(FinalizeAndExtractPSBT(extraction_source, final_transaction));
    const CTransaction final{final_transaction};
    BOOST_REQUIRE(ValidateFinalCollaborativeTransaction(
        final_transaction, final.GetHash(), final.GetWitnessHash(), trusted, error));

    BOOST_CHECK(!ValidateFinalCollaborativeTransaction(
        final_transaction, final.GetHash(), uint256::ONE, trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_WTXID_MISMATCH");

    CMutableTransaction changed_output{final_transaction};
    changed_output.vout.front().nValue++;
    const CTransaction changed_output_tx{changed_output};
    BOOST_CHECK(!ValidateFinalCollaborativeTransaction(
        changed_output, changed_output_tx.GetHash(), changed_output_tx.GetWitnessHash(),
        trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_TXID_MISMATCH");

    CMutableTransaction invalid_witness{final_transaction};
    BOOST_REQUIRE(!invalid_witness.vin.front().scriptWitness.stack.empty());
    BOOST_REQUIRE(!invalid_witness.vin.front().scriptWitness.stack.front().empty());
    invalid_witness.vin.front().scriptWitness.stack.front().front() ^= 1;
    const CTransaction invalid_witness_tx{invalid_witness};
    BOOST_CHECK(!ValidateFinalCollaborativeTransaction(
        invalid_witness, invalid_witness_tx.GetHash(), invalid_witness_tx.GetWitnessHash(),
        trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");

    CMutableTransaction invalid_provider_witness{final_transaction};
    BOOST_REQUIRE(!invalid_provider_witness.vin[1].scriptWitness.stack.empty());
    BOOST_REQUIRE(!invalid_provider_witness.vin[1].scriptWitness.stack.front().empty());
    invalid_provider_witness.vin[1].scriptWitness.stack.front().front() ^= 1;
    const CTransaction invalid_provider_witness_tx{invalid_provider_witness};
    BOOST_CHECK(!ValidateFinalCollaborativeTransaction(
        invalid_provider_witness, invalid_provider_witness_tx.GetHash(),
        invalid_provider_witness_tx.GetWitnessHash(), trusted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");
}

BOOST_AUTO_TEST_CASE(payment_intent_control_proofs_use_only_wallet_owned_bip86_inputs)
{
    CKey key;
    key.MakeNewKey(true);
    FlatSigningProvider provider;
    std::string error;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeSecret(key) + ")", provider, error, /*require_checksum=*/false);
    BOOST_REQUIRE(descriptor);
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        BOOST_REQUIRE(m_wallet.AddWalletDescriptor(wallet_descriptor, provider, "", false));
    }

    TaprootBuilder taproot;
    taproot.Finalize(XOnlyPubKey{key.GetPubKey()});
    const CScript script = GetScriptForDestination(taproot.GetOutput());
    CMutableTransaction previous;
    previous.vin.emplace_back(COutPoint{uint256::ONE, 3});
    previous.vout.emplace_back(2000, script);
    const CTransactionRef previous_tx = MakeTransactionRef(std::move(previous));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(previous_tx, TxStateInMempool{}));
    }

    constexpr int64_t now{100000};
    PaymentIntent intent;
    intent.genesis_hash = uint256S("81");
    intent.provider_id = uint256S("82");
    intent.request_id = "550e8400-e29b-41d4-a716-446655440080";
    intent.session_id = uint256S("83");
    intent.client_nonce = uint256S("84");
    intent.canonical_request_hash = uint256S("8401");
    intent.user_dd_inputs = {COutPoint{previous_tx->GetHash(), 0}};
    intent.recipient_script = script;
    intent.recipient_amount = DDCents{1000};
    intent.offer_id = uint256S("85");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("86");
    intent.expires_at = now + 30;

    PaymentIntent partial_failure{intent};
    partial_failure.user_dd_inputs.push_back(COutPoint{uint256S("87"), 0});
    std::vector<XOnlyPubKey> output_keys;
    BOOST_CHECK(!SignPaymentIntentInputs(m_wallet, partial_failure, now, output_keys, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_USER_PREVOUT_NOT_FOUND");
    BOOST_CHECK(partial_failure.user_input_proofs.empty());
    BOOST_CHECK(output_keys.empty());

    BOOST_REQUIRE_MESSAGE(SignPaymentIntentInputs(m_wallet, intent, now, output_keys, error), error);
    BOOST_REQUIRE_EQUAL(intent.user_input_proofs.size(), 1U);
    BOOST_REQUIRE_EQUAL(output_keys.size(), 1U);
    BOOST_CHECK(output_keys.front() == XOnlyPubKey{taproot.GetOutput()});
    BOOST_CHECK(output_keys.front().VerifySchnorr(
        GetUserInputControlHash(intent, intent.user_dd_inputs.front()),
        intent.user_input_proofs.front().signature));
    BOOST_CHECK(ValidatePaymentIntent(intent, intent.genesis_hash, now, output_keys, error));
}

BOOST_AUTO_TEST_CASE(client_change_ownership_is_rechecked_immediately_before_signing)
{
    CKey wallet_key;
    wallet_key.MakeNewKey(true);
    FlatSigningProvider provider;
    std::string error;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeSecret(wallet_key) + ")", provider, error,
        /*require_checksum=*/false);
    BOOST_REQUIRE(descriptor);
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        BOOST_REQUIRE(m_wallet.AddWalletDescriptor(
            wallet_descriptor, provider, "", false));
    }
    TaprootBuilder owned_builder;
    owned_builder.Finalize(XOnlyPubKey{wallet_key.GetPubKey()});
    const CScript owned_script =
        GetScriptForDestination(owned_builder.GetOutput());
    CMutableTransaction previous;
    previous.vin.emplace_back(COutPoint{uint256S("901"), 0});
    previous.vout.emplace_back(1, owned_script);
    const CTransactionRef previous_tx = MakeTransactionRef(std::move(previous));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(previous_tx, TxStateInMempool{}));
    }

    ClientAuthorizationManifest manifest;
    manifest.user_dd_inputs = {COutPoint{previous_tx->GetHash(), 0}};
    manifest.user_dd_change_script = owned_script;
    manifest.manifest_id = GetClientAuthorizationManifestId(manifest);
    BOOST_CHECK(ValidateClientAuthorizationOwnership(
        m_wallet, manifest, error));

    ClientAuthorizationManifest foreign_input{manifest};
    foreign_input.user_dd_inputs = {COutPoint{uint256S("902"), 0}};
    foreign_input.manifest_id =
        GetClientAuthorizationManifestId(foreign_input);
    BOOST_CHECK(!ValidateClientAuthorizationOwnership(
        m_wallet, foreign_input, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_CLIENT_INPUT_NOT_WALLET_OWNED");

    CKey foreign_key;
    foreign_key.MakeNewKey(true);
    TaprootBuilder foreign_builder;
    foreign_builder.Finalize(XOnlyPubKey{foreign_key.GetPubKey()});
    manifest.user_dd_change_script =
        GetScriptForDestination(foreign_builder.GetOutput());
    manifest.manifest_id = GetClientAuthorizationManifestId(manifest);
    BOOST_CHECK(!ValidateClientAuthorizationOwnership(
        m_wallet, manifest, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_CLIENT_CHANGE_NOT_WALLET_OWNED");
    manifest.user_dd_change_script.clear();
    manifest.manifest_id = GetClientAuthorizationManifestId(manifest);
    BOOST_CHECK(ValidateClientAuthorizationOwnership(
        m_wallet, manifest, error));
}

BOOST_AUTO_TEST_CASE(provider_execution_firewall_revalidates_durable_authority)
{
    CKey key;
    key.MakeNewKey(true);
    FlatSigningProvider signing_provider;
    std::string error;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeSecret(key) + ")", signing_provider, error,
        /*require_checksum=*/false);
    BOOST_REQUIRE(descriptor);
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        BOOST_REQUIRE(m_wallet.AddWalletDescriptor(
            wallet_descriptor, signing_provider, "", false));
    }

    TaprootBuilder taproot;
    taproot.Finalize(XOnlyPubKey{key.GetPubKey()});
    const CScript script = GetScriptForDestination(taproot.GetOutput());
    CMutableTransaction user_previous;
    user_previous.vin.emplace_back(COutPoint{uint256::ONE, 11});
    user_previous.vout.emplace_back(4000, script);
    const CTransactionRef user_tx = MakeTransactionRef(std::move(user_previous));
    CMutableTransaction provider_previous;
    provider_previous.vin.emplace_back(COutPoint{uint256::ONE, 12});
    provider_previous.vout.emplace_back(2000, script);
    const CTransactionRef provider_tx =
        MakeTransactionRef(std::move(provider_previous));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(user_tx, TxStateInMempool{}));
        BOOST_REQUIRE(m_wallet.AddToWallet(provider_tx, TxStateInMempool{}));
    }

    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{user_tx->GetHash(), 0});
    transaction.vin.emplace_back(COutPoint{provider_tx->GetHash(), 0});
    transaction.vout.emplace_back(5500, script);
    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE(CreateCollaborativePSBTTemplate(
        transaction,
        {{transaction.vin[0].prevout, user_tx, InputRole::USER_DD},
         {transaction.vin[1].prevout, provider_tx, InputRole::PROVIDER_DGB}},
        trusted, error));

    PartiallySignedTransaction malicious{trusted.psbt};
    malicious.tx->vin[0].prevout = COutPoint{uint256S("90"), 0};
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_TRANSACTION_MISMATCH");
    BOOST_CHECK(!PSBTInputSigned(malicious.inputs[0]));

    malicious = trusted.psbt;
    malicious.tx->vin.emplace_back(COutPoint{uint256S("90"), 1});
    malicious.inputs.emplace_back();
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SHAPE_MISMATCH");

    malicious = trusted.psbt;
    malicious.tx->vout.emplace_back(1, script);
    malicious.outputs.emplace_back();
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SHAPE_MISMATCH");

    malicious = trusted.psbt;
    malicious.inputs[0].unknown[{0x42}] = {0x01};
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_INPUT_FIELDS");

    malicious = trusted.psbt;
    malicious.unknown[{0x42}] = {0x01};
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_GLOBAL_FIELDS");

    malicious = trusted.psbt;
    malicious.outputs[0].unknown[{0x42}] = {0x01};
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_OUTPUT_FIELDS");

    malicious = trusted.psbt;
    malicious.inputs[0].sighash_type = SIGHASH_ALL;
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGHASH");

    CollaborativePSBTTemplate unknown_role{trusted};
    unknown_role.input_roles[0] = static_cast<InputRole>(0xff);
    malicious = trusted.psbt;
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, malicious, unknown_role, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_INPUT_ROLE");

    constexpr int64_t now{100000};
    PaymentIntent intent;
    intent.genesis_hash = uint256S("91");
    intent.provider_id = GetPaymasterId(XOnlyPubKey{key.GetPubKey()});
    intent.request_id = "550e8400-e29b-41d4-a716-446655440091";
    intent.session_id = uint256S("93");
    intent.client_nonce = uint256S("94");
    intent.canonical_request_hash = uint256S("9401");
    intent.user_dd_inputs = {transaction.vin[0].prevout};
    intent.recipient_script = script;
    intent.recipient_amount = DDCents{1000};
    intent.user_dd_change_script = script;
    intent.offer_id = uint256S("95");
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = uint256S("96");
    intent.expires_at = now + 60;

    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = uint256S("97");
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.funding_model = intent.funding_model;
    quote.sponsorship_scope = intent.sponsorship_scope;
    quote.service_fee = DDCents{0};
    quote.reserved_dgb_inputs = {
        {transaction.vin[1].prevout, CMutableTransaction{*provider_tx},
         DGBSatoshis{2000}}};
    quote.dgb_change_script = script;
    quote.network_fee = DGBSatoshis{500};
    quote.created_at = now;
    quote.expires_at = now + 60;
    quote.retry_until = now + 600;
    quote.unsigned_transaction = transaction;
    quote.unsigned_txid = CTransaction{transaction}.GetHash();
    quote.template_commitment =
        GetCollaborativeTemplateCommitment(intent, quote, trusted);
    quote.identity_signature.resize(64);
    BOOST_REQUIRE(key.SignSchnorr(
        GetPaymasterQuoteSignatureHash(quote), quote.identity_signature,
        nullptr, uint256{}));

    PaymasterCapacityProof capacity_proof;
    capacity_proof.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    capacity_proof.genesis_hash = intent.genesis_hash;
    capacity_proof.provider_id = intent.provider_id;
    capacity_proof.request_id = intent.request_id;
    capacity_proof.session_id = intent.session_id;
    capacity_proof.client_nonce = intent.client_nonce;
    capacity_proof.funding_model = intent.funding_model;
    capacity_proof.requires_carrier = false;
    capacity_proof.snapshot_id = uint256S("9a");
    capacity_proof.created_at = now;
    capacity_proof.expires_at = quote.expires_at;
    CapacityDGBInput capacity_input;
    capacity_input.input = quote.reserved_dgb_inputs.front();
    capacity_input.control_proof.reference_block = uint256S("9b");
    capacity_input.control_proof.expires_at = capacity_proof.expires_at;
    capacity_input.control_proof.signature.assign(64, 1);
    PaymasterLiquiditySlot capacity_slot;
    capacity_slot.dgb_inputs = {capacity_input};
    capacity_proof.liquidity_slots = {capacity_slot};
    capacity_proof.identity_signature.assign(64, 0);
    BOOST_REQUIRE(key.SignSchnorr(
        GetCapacityProofSignatureHash(capacity_proof),
        capacity_proof.identity_signature, nullptr, uint256{}));

    PaymasterCapacityRequest capacity_request;
    capacity_request.version = capacity_proof.version;
    capacity_request.genesis_hash = capacity_proof.genesis_hash;
    capacity_request.provider_id = capacity_proof.provider_id;
    capacity_request.request_id = capacity_proof.request_id;
    capacity_request.session_id = capacity_proof.session_id;
    capacity_request.client_nonce = capacity_proof.client_nonce;
    capacity_request.funding_model = capacity_proof.funding_model;
    capacity_request.requires_carrier = capacity_proof.requires_carrier;
    capacity_request.requested_slots = 1;
    capacity_request.created_at = capacity_proof.created_at;
    capacity_request.expires_at = capacity_proof.expires_at;
    const std::vector<unsigned char> capacity_request_bytes =
        SerializePaymasterTestArtifact(capacity_request);

    ValidatedCapacitySnapshot capacity;
    capacity.snapshot_id = capacity_proof.snapshot_id;
    capacity.resource_commitment =
        GetCapacityResourceCommitment(capacity_proof);
    capacity.session_id = intent.session_id;
    capacity.attempt_id = uint256S("98");
    capacity.provider_id = intent.provider_id;
    capacity.client_nonce = intent.client_nonce;
    capacity.request_hash = Hash(capacity_request_bytes);
    capacity.capacity_proof =
        SerializePaymasterTestArtifact(capacity_proof);
    capacity.created_at = capacity_proof.created_at;
    capacity.expires_at = capacity_proof.expires_at;
    capacity.validated_at = now;
    capacity.funding_model = capacity_proof.funding_model;
    capacity.requires_carrier = capacity_proof.requires_carrier;

    ClientAuthorizationManifest client_manifest;
    BOOST_REQUIRE(BuildClientAuthorizationManifest(
        intent, quote, capacity, trusted, DDCents{0}, client_manifest, error));
    BOOST_REQUIRE(ValidateClientAuthorizationManifest(
        client_manifest, intent, quote, capacity, trusted, error));
    BOOST_REQUIRE_MESSAGE(ValidateClientAuthorizationOwnership(
                              m_wallet, client_manifest, error),
                          error);

    PaymasterQuoteRequest quote_request;
    quote_request.intent = intent;
    PaymasterQuoteResponse quote_response;
    quote_response.request_id = intent.request_id;
    quote_response.session_id = intent.session_id;
    quote_response.quote = quote;

    ProviderAttempt attempt;
    attempt.session_id = intent.session_id;
    attempt.attempt_id = uint256S("98");
    attempt.provider_id = intent.provider_id;
    attempt.provider_identity_key = XOnlyPubKey{key.GetPubKey()};
    attempt.state = AttemptState::USER_PSBT_ACCEPTED;
    attempt.client_nonce = intent.client_nonce;
    attempt.intent_hash = quote.intent_hash;
    attempt.quote_id = quote.quote_id;
    attempt.unsigned_txid = quote.unsigned_txid;
    attempt.template_commitment = quote.template_commitment;
    attempt.commit_key = GetPaymasterCommitKey(
        attempt.provider_id, attempt.client_nonce, attempt.intent_hash,
        attempt.quote_id, attempt.template_commitment);
    attempt.capacity_request = capacity_request_bytes;
    attempt.quote_request = SerializePaymasterTestArtifact(quote_request);
    attempt.signed_quote = SerializePaymasterTestArtifact(quote_response);
    attempt.unsigned_transaction = SerializePaymasterTestArtifact(transaction);
    attempt.unsigned_psbt = SerializePaymasterTestArtifact(trusted.psbt);
    attempt.input_roles = {
        ReservationRole::USER_DD, ReservationRole::PROVIDER_DGB};
    attempt.quote_expires_at = quote.expires_at;
    attempt.retry_until = quote.retry_until;
    attempt.created_at = now;
    attempt.updated_at = now;
    attempt.capacity_snapshot = capacity;
    attempt.client_manifest = client_manifest;
    attempt.accepted_client_manifest_id = client_manifest.manifest_id;
    attempt.client_manifest_accepted_at = now + 1;
    BOOST_REQUIRE(BuildProviderAuthorizationManifest(
        intent, quote, trusted, uint256S("99"),
        attempt.provider_manifest, error));
    BOOST_REQUIRE(ValidateProviderAuthorizationManifest(
        attempt.provider_manifest, intent, quote, trusted, error));
    BOOST_REQUIRE_MESSAGE(ValidateProviderAuthorizationOwnership(
                              m_wallet, attempt.provider_manifest, error),
                          error);

    ProviderAuthorizationManifest outdated_provider_manifest{
        attempt.provider_manifest};
    --outdated_provider_manifest.version;
    outdated_provider_manifest.manifest_id =
        GetProviderAuthorizationManifestId(outdated_provider_manifest);
    BOOST_CHECK(!ValidateProviderAuthorizationOwnership(
        m_wallet, outdated_provider_manifest, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID");

    ProviderAuthorizationManifest foreign_provider_input{
        attempt.provider_manifest};
    foreign_provider_input.provider_dgb_inputs = {
        COutPoint{uint256S("9901"), 0}};
    foreign_provider_input.manifest_id =
        GetProviderAuthorizationManifestId(foreign_provider_input);
    BOOST_CHECK(!ValidateProviderAuthorizationOwnership(
        m_wallet, foreign_provider_input, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PROVIDER_INPUT_NOT_WALLET_OWNED");

    CKey foreign_provider_key;
    foreign_provider_key.MakeNewKey(true);
    TaprootBuilder foreign_provider_builder;
    foreign_provider_builder.Finalize(
        XOnlyPubKey{foreign_provider_key.GetPubKey()});
    ProviderAuthorizationManifest foreign_provider_change{
        attempt.provider_manifest};
    foreign_provider_change.dgb_change_scripts = {
        GetScriptForDestination(foreign_provider_builder.GetOutput())};
    foreign_provider_change.manifest_id =
        GetProviderAuthorizationManifestId(foreign_provider_change);
    BOOST_CHECK(!ValidateProviderAuthorizationOwnership(
        m_wallet, foreign_provider_change, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PROVIDER_CHANGE_NOT_WALLET_OWNED");

    CollaborativePSBTTemplate signing_template;
    BOOST_REQUIRE_MESSAGE(ValidateProviderAuthorizationForExecution(
                              attempt, now + 1, signing_template, error),
                          error);

    PartiallySignedTransaction signed_psbt{trusted.psbt};
    BOOST_REQUIRE(SignCollaborativePSBTForParty(
        m_wallet, signed_psbt, trusted, SigningParty::USER, error));
    // Core property after the client signature: the signed transaction still
    // consumes exactly the DD inputs and authorizes exactly the asset outflows
    // captured by the local client manifest.
    BOOST_REQUIRE(signed_psbt.tx);
    BOOST_CHECK(PSBTInputSigned(signed_psbt.inputs[0]));
    BOOST_CHECK(!PSBTInputSigned(signed_psbt.inputs[1]));
    BOOST_CHECK(CTransaction{*signed_psbt.tx} ==
                CTransaction{quote.unsigned_transaction});
    BOOST_CHECK(InputsOwnedBy(signed_psbt, trusted, SigningParty::USER) ==
                client_manifest.user_dd_inputs);
    BOOST_CHECK(client_manifest.recipient_script == intent.recipient_script);
    BOOST_CHECK(client_manifest.recipient_amount == intent.recipient_amount);
    BOOST_CHECK(client_manifest.user_dd_change_script ==
                intent.user_dd_change_script);
    BOOST_CHECK(client_manifest.service_fee == quote.service_fee);
    for (const InputRole role : trusted.input_roles) {
        if (IsInputOwnedBy(role, SigningParty::USER)) {
            BOOST_CHECK(role == InputRole::USER_DD);
        }
    }

    PartiallySignedTransaction redirected_after_user{signed_psbt};
    redirected_after_user.tx->vout.front().scriptPubKey =
        CScript{} << OP_FALSE;
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, redirected_after_user, trusted,
        SigningParty::PROVIDER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_TRANSACTION_MISMATCH");
    BOOST_CHECK(!PSBTInputSigned(redirected_after_user.inputs[1]));

    PartiallySignedTransaction extra_after_user{signed_psbt};
    extra_after_user.tx->vout.emplace_back(1, script);
    extra_after_user.outputs.emplace_back();
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, extra_after_user, trusted, SigningParty::PROVIDER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SHAPE_MISMATCH");
    BOOST_CHECK(!PSBTInputSigned(extra_after_user.inputs[1]));

    attempt.user_signed_psbt = SerializePaymasterTestArtifact(signed_psbt);
    BOOST_REQUIRE(SignCollaborativePSBTForParty(
        m_wallet, signed_psbt, trusted, SigningParty::PROVIDER, error));
    // Symmetric property after the provider signature: no client-controlled
    // PSBT can add a provider input or redirect its exact fee/change authority.
    BOOST_CHECK(CTransaction{*signed_psbt.tx} ==
                CTransaction{quote.unsigned_transaction});
    BOOST_CHECK(PSBTInputSigned(signed_psbt.inputs[0]));
    BOOST_CHECK(PSBTInputSigned(signed_psbt.inputs[1]));
    BOOST_CHECK(InputsOwnedBy(signed_psbt, trusted, SigningParty::PROVIDER) ==
                attempt.provider_manifest.provider_dgb_inputs);
    BOOST_CHECK(attempt.provider_manifest.network_fee == quote.network_fee);
    BOOST_CHECK(attempt.provider_manifest.dgb_change_scripts ==
                std::vector<CScript>{*quote.dgb_change_script});
    CMutableTransaction final_transaction;
    BOOST_REQUIRE(FinalizeAndExtractPSBT(signed_psbt, final_transaction));
    const std::vector<unsigned char> final_bytes =
        SerializePaymasterTestArtifact(final_transaction);
    const CTransaction final{final_transaction};
    attempt.state = AttemptState::FINAL_COMMITTED;
    attempt.final_transaction = final_bytes;
    attempt.final_txid = final.GetHash();

    ProviderCommitRecord commit;
    commit.commit_key = attempt.commit_key;
    commit.provider_id = attempt.provider_id;
    commit.quote_id = attempt.quote_id;
    commit.template_commitment = attempt.template_commitment;
    commit.final_txid = final.GetHash();
    commit.raw_transaction_hash = final.GetWitnessHash();
    commit.final_transaction = final_bytes;
    commit.provider_inputs = {transaction.vin[1].prevout};
    commit.committed_at = now + 2;
    commit.retry_until = attempt.retry_until;

    CMutableTransaction validated_final;
    BOOST_REQUIRE_MESSAGE(ValidateProviderCommitForExecution(
                              attempt, commit, now + 3, validated_final, error),
                          error);
    BOOST_CHECK(CTransaction{validated_final}.GetWitnessHash() ==
                final.GetWitnessHash());

    ProviderAttempt trailing_request{attempt};
    trailing_request.quote_request.push_back(0);
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        trailing_request, commit, now + 3, validated_final, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_ENCODING");

    ProviderAttempt trailing_response{attempt};
    trailing_response.signed_quote.push_back(0);
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        trailing_response, commit, now + 3, validated_final, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_ENCODING");

    ProviderAttempt trailing_transaction{attempt};
    trailing_transaction.unsigned_transaction.push_back(0);
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        trailing_transaction, commit, now + 3, validated_final, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_ENCODING");

    ProviderAttempt trailing_psbt{attempt};
    trailing_psbt.unsigned_psbt.push_back(0);
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        trailing_psbt, commit, now + 3, validated_final, error));
    BOOST_CHECK(error ==
                    "PAYMASTER_PROVIDER_AUTHORIZATION_TEMPLATE_ENCODING" ||
                error ==
                    "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_NONCANONICAL");

    CMutableTransaction client_validated_final;
    BOOST_REQUIRE_MESSAGE(ValidateClientFinalForExecution(
                              attempt, final.GetWitnessHash(),
                              client_validated_final, error),
                          error);
    BOOST_CHECK(CTransaction{client_validated_final} == final);

    ProviderAttempt outdated_client_manifest{attempt};
    --outdated_client_manifest.client_manifest.version;
    outdated_client_manifest.client_manifest.manifest_id =
        GetClientAuthorizationManifestId(
            outdated_client_manifest.client_manifest);
    outdated_client_manifest.accepted_client_manifest_id =
        outdated_client_manifest.client_manifest.manifest_id;
    BOOST_CHECK(!ValidateClientFinalForExecution(
        outdated_client_manifest, final.GetWitnessHash(),
        client_validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID");

    ProviderAttempt outdated_capacity_snapshot{attempt};
    --outdated_capacity_snapshot.capacity_snapshot.version;
    BOOST_CHECK(!ValidateClientFinalForExecution(
        outdated_capacity_snapshot, final.GetWitnessHash(),
        client_validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_SNAPSHOT_VERSION");

    ProviderAttempt missing_client_acceptance{attempt};
    missing_client_acceptance.accepted_client_manifest_id.SetNull();
    missing_client_acceptance.client_manifest_accepted_at = 0;
    BOOST_CHECK(!ValidateClientFinalForExecution(
        missing_client_acceptance, final.GetWitnessHash(),
        client_validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED");

    ProviderAttempt legacy_client_final{attempt};
    --legacy_client_final.version;
    BOOST_CHECK(!ValidateClientFinalForExecution(
        legacy_client_final, final.GetWitnessHash(), client_validated_final,
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED");

    ProviderAttempt changed_client_manifest{attempt};
    changed_client_manifest.client_manifest.service_fee.value++;
    BOOST_CHECK(!ValidateClientFinalForExecution(
        changed_client_manifest, final.GetWitnessHash(),
        client_validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID");

    ProviderAttempt invalid_client_witness{attempt};
    CMutableTransaction client_witness_mutation{final_transaction};
    BOOST_REQUIRE(!client_witness_mutation.vin.front()
                       .scriptWitness.stack.empty());
    BOOST_REQUIRE(!client_witness_mutation.vin.front()
                       .scriptWitness.stack.front()
                       .empty());
    client_witness_mutation.vin.front()
        .scriptWitness.stack.front()
        .front() ^= 1;
    invalid_client_witness.final_transaction =
        SerializePaymasterTestArtifact(client_witness_mutation);
    BOOST_CHECK(!ValidateClientFinalForExecution(
        invalid_client_witness,
        CTransaction{client_witness_mutation}.GetWitnessHash(),
        client_validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");

    ProviderAttempt expired_signing{attempt};
    expired_signing.state = AttemptState::USER_PSBT_ACCEPTED;
    BOOST_CHECK(!ValidateProviderAuthorizationForExecution(
        expired_signing, expired_signing.retry_until + 1,
        signing_template, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTHORIZATION_EXPIRED");

    ProviderAttempt changed_manifest{expired_signing};
    changed_manifest.provider_manifest.network_fee.value++;
    BOOST_CHECK(!ValidateProviderAuthorizationForExecution(
        changed_manifest, now + 1, signing_template, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID");

    ProviderAttempt missing_manifest{expired_signing};
    missing_manifest.provider_manifest = {};
    BOOST_CHECK(!ValidateProviderAuthorizationForExecution(
        missing_manifest, now + 1, signing_template, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED");

    PaymasterQuoteResponse changed_quote_response{quote_response};
    changed_quote_response.quote.identity_signature.front() ^= 1;
    ProviderAttempt changed_quote_signature{expired_signing};
    changed_quote_signature.signed_quote =
        SerializePaymasterTestArtifact(changed_quote_response);
    BOOST_CHECK(!ValidateProviderAuthorizationForExecution(
        changed_quote_signature, now + 1, signing_template, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT");

    ProviderCommitRecord changed_inputs{commit};
    changed_inputs.provider_inputs = {transaction.vin[0].prevout};
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        attempt, changed_inputs, now + 3, validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_COMMIT_INPUT_MISMATCH");

    ProviderAttempt invalid_witness_attempt{attempt};
    ProviderCommitRecord invalid_witness_commit{commit};
    CMutableTransaction invalid_witness{final_transaction};
    BOOST_REQUIRE(!invalid_witness.vin.front().scriptWitness.stack.empty());
    BOOST_REQUIRE(!invalid_witness.vin.front().scriptWitness.stack.front().empty());
    invalid_witness.vin.front().scriptWitness.stack.front().front() ^= 1;
    invalid_witness_commit.final_transaction =
        SerializePaymasterTestArtifact(invalid_witness);
    invalid_witness_commit.raw_transaction_hash =
        CTransaction{invalid_witness}.GetWitnessHash();
    invalid_witness_attempt.final_transaction =
        invalid_witness_commit.final_transaction;
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        invalid_witness_attempt, invalid_witness_commit, now + 3,
        validated_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PSBT_SIGNATURE_INVALID");

    // Expiry forbids creating another signature, but cannot revoke an exact
    // already signed commit. It must remain executable for safe recovery.
    BOOST_CHECK(ValidateProviderCommitForExecution(
        attempt, commit, commit.retry_until + 1, validated_final, error));
    BOOST_CHECK(CTransaction{validated_final}.GetWitnessHash() ==
                final.GetWitnessHash());

    // Protocols 1-4 cannot authorize either a new signature or execution of a
    // previously persisted commit.
    for (uint16_t protocol_version{1};
         protocol_version < DigiDollar::Paymaster::PROTOCOL_VERSION;
         ++protocol_version) {
        PaymasterQuoteRequest legacy_request{quote_request};
        PaymasterQuoteResponse legacy_response{quote_response};
        legacy_request.version = protocol_version;
        legacy_response.version = protocol_version;
        ProviderAttempt legacy_attempt{attempt};
        legacy_attempt.quote_request =
            SerializePaymasterTestArtifact(legacy_request);
        legacy_attempt.signed_quote =
            SerializePaymasterTestArtifact(legacy_response);

        ProviderAttempt legacy_signing{legacy_attempt};
        legacy_signing.state = AttemptState::USER_PSBT_ACCEPTED;
        BOOST_CHECK(!ValidateProviderAuthorizationForExecution(
            legacy_signing, now + 1, signing_template, error));
        BOOST_CHECK_EQUAL(
            error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT");

        BOOST_CHECK(!ValidateProviderCommitForExecution(
            legacy_attempt, commit, now + 3, validated_final, error));
        BOOST_CHECK_EQUAL(
            error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT");
    }

    for (const uint16_t unsupported_protocol :
         {uint16_t{0}, static_cast<uint16_t>(
                           DigiDollar::Paymaster::PROTOCOL_VERSION + 1)}) {
        PaymasterQuoteRequest unsupported_request{quote_request};
        PaymasterQuoteResponse unsupported_response{quote_response};
        unsupported_request.version = unsupported_protocol;
        unsupported_response.version = unsupported_protocol;
        ProviderAttempt unsupported_attempt{attempt};
        unsupported_attempt.quote_request =
            SerializePaymasterTestArtifact(unsupported_request);
        unsupported_attempt.signed_quote =
            SerializePaymasterTestArtifact(unsupported_response);
        BOOST_CHECK(!ValidateProviderCommitForExecution(
            unsupported_attempt, commit, now + 3, validated_final, error));
        BOOST_CHECK_EQUAL(
            error, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT");
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
