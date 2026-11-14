// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Wallet signing-firewall tests for client and provider manifests. */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <coins.h>
#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/reservation.h>
#include <paymaster/txbuilder.h>
#include <paymaster/types.h>
#include <paymaster/wire.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <atomic>
#include <thread>

namespace wallet {
using namespace DigiDollar::Paymaster;
namespace {

struct PaymasterWalletSetup : WalletTestingSetup {
    PaymasterWalletSetup() : WalletTestingSetup{ChainType::REGTEST} {}
};

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

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_psbt_tests, PaymasterWalletSetup)

BOOST_AUTO_TEST_CASE(shared_bip86_control_signer_binds_prevout_script_and_digest)
{
    CKey key;
    key.MakeNewKey(true);
    FlatSigningProvider provider;
    std::string error;
    auto descriptor = Parse("tr(" + EncodeSecret(key) + ")", provider, error, false);
    BOOST_REQUIRE(descriptor);
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    LOCK(m_wallet.cs_wallet);
    m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    BOOST_REQUIRE(m_wallet.AddWalletDescriptor(wallet_descriptor, provider, "", false));
    TaprootBuilder taproot;
    taproot.Finalize(XOnlyPubKey{key.GetPubKey()});
    const auto script = GetScriptForDestination(taproot.GetOutput());
    CMutableTransaction creating;
    creating.vin.emplace_back(COutPoint{uint256::ONE, 0});
    creating.vout.emplace_back(0, script);
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(creating), TxStateInactive{}));
    const COutPoint input{creating.GetHash(), 0};
    const uint256 digest = (HashWriter{} << std::string{"Paymaster control regression"}).GetHash();
    Bip86ControlSignature signed_control;
    BOOST_REQUIRE(SignPaymasterBip86Control(m_wallet, input, script, digest, signed_control) == Bip86ControlSigningError::NONE);
    BOOST_CHECK(signed_control.output_key.VerifySchnorr(digest, signed_control.signature));
    BOOST_CHECK(!signed_control.output_key.VerifySchnorr(uint256::ONE, signed_control.signature));
    // A successful previous result cannot survive a failed ownership lookup.
    BOOST_CHECK(SignPaymasterBip86Control(m_wallet, {input.hash, 1}, script, digest, signed_control) == Bip86ControlSigningError::PREVOUT_NOT_FOUND);
    BOOST_CHECK(signed_control.signature.empty());
    BOOST_CHECK(SignPaymasterBip86Control(m_wallet, input, CScript{} << OP_TRUE, digest, signed_control) == Bip86ControlSigningError::PREVOUT_NOT_FOUND);
    BOOST_CHECK(signed_control.signature.empty());
}

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

    // An authenticated provider remains untrusted for transaction contents.
    // Even a wallet containing both parties' keys must refuse each mutation
    // before creating either signature, independently of network validation.
    for (const auto party : {SigningParty::USER, SigningParty::PROVIDER}) {
        for (int attack = 0; attack < 10; ++attack) {
            BOOST_TEST_CONTEXT("signing party " << static_cast<int>(party) << ", mutation " << attack) {
                PartiallySignedTransaction malicious{trusted.psbt};
                switch (attack) {
                case 0: malicious.tx->vout[0].scriptPubKey = script; break;
                case 1: --malicious.tx->vout[0].nValue; break;
                case 2: malicious.tx->vin[0].prevout = transaction.vin[1].prevout; break;
                case 3: malicious.tx->vin[1].prevout = transaction.vin[0].prevout; break;
                case 4: --malicious.tx->vin[0].nSequence; break;
                case 5: ++malicious.tx->nLockTime; break;
                case 6: ++malicious.tx->nVersion; break;
                case 7: ++malicious.inputs[0].witness_utxo.nValue; break;
                case 8: ++malicious.inputs[1].witness_utxo.nValue; break;
                case 9: malicious.inputs[0].sighash_type = SIGHASH_ALL | SIGHASH_ANYONECANPAY; break;
                }
                BOOST_CHECK(!SignCollaborativePSBTForParty(m_wallet, malicious, trusted, party, error));
                BOOST_CHECK(!PSBTInputSigned(malicious.inputs[0]));
                BOOST_CHECK(!PSBTInputSigned(malicious.inputs[1]));
            }
        }
    }

    PartiallySignedTransaction psbt{trusted.psbt};
    {
        const auto unchanged = SerializePaymasterTestArtifact(psbt);
        BOOST_CHECK(!SignCollaborativePSBTForParty(m_wallet, psbt, trusted,
            SigningParty::USER, GetTime() - 1, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_SIGNING_AUTHORIZATION_EXPIRED");
        BOOST_CHECK(SerializePaymasterTestArtifact(psbt) == unchanged);
    }
    {
        const auto old_time = GetMockTime();
        const int64_t deadline = GetTime() + 10;
        std::atomic<bool> waiting{false};
        bool signed_after_wait{false};
        std::string signing_error;
        std::thread signer;
        {
            LOCK(m_wallet.cs_wallet);
            signer = std::thread{[&] {
                waiting = true;
                signed_after_wait = SignCollaborativePSBTForParty(m_wallet, psbt, trusted,
                    SigningParty::USER, deadline, signing_error);
            }};
            while (!waiting.load()) std::this_thread::yield();
            SetMockTime(deadline + 1);
        }
        signer.join();
        SetMockTime(old_time);
        BOOST_CHECK(!signed_after_wait);
        BOOST_CHECK_EQUAL(signing_error, "PAYMASTER_SIGNING_AUTHORIZATION_EXPIRED");
        BOOST_CHECK(SerializePaymasterTestArtifact(psbt) == SerializePaymasterTestArtifact(trusted.psbt));
    }
    // A manual lock installed after preparation is a veto for every new USER
    // signature, including this generic USER_DGB fixture.
    const auto user_input = psbt.tx->vin.front().prevout;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.LockCoin(user_input));
    }
    const auto before_user_veto = SerializePaymasterTestArtifact(psbt);
    BOOST_CHECK(!SignCollaborativePSBTForParty(m_wallet, psbt, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_INPUT_LOCKED");
    BOOST_CHECK(SerializePaymasterTestArtifact(psbt) == before_user_veto);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.UnlockCoin(user_input));
        BOOST_CHECK(!m_wallet.IsLockedCoin(user_input));
        BOOST_CHECK(!m_wallet.IsManuallyLockedCoin(user_input));
    }
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

    const COutPoint provider_input{psbt.tx->vin[1].prevout};
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.LockPaymasterCoin(provider_input);
        BOOST_REQUIRE(m_wallet.LockCoin(provider_input));
        BOOST_CHECK(m_wallet.IsOrdinaryLockedCoin(provider_input));
    }
    const auto before_locked_signing = SerializePaymasterTestArtifact(psbt);
    BOOST_CHECK(!SignCollaborativePSBTForParty(m_wallet, psbt, trusted, SigningParty::PROVIDER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_INPUT_LOCKED");
    BOOST_CHECK(SerializePaymasterTestArtifact(psbt) == before_locked_signing);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.UnlockCoin(provider_input));
        BOOST_CHECK(!m_wallet.IsOrdinaryLockedCoin(provider_input));
        BOOST_CHECK(m_wallet.IsLockedCoin(provider_input));
    }
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

BOOST_AUTO_TEST_CASE(authentic_provider_cannot_replace_locally_reconstructed_payment)
{
    CKey user_key, provider_key, impostor;
    user_key.MakeNewKey(true);
    provider_key.MakeNewKey(true);
    impostor.MakeNewKey(true);
    FlatSigningProvider signing_provider;
    std::string error;
    auto descriptor = Parse("tr(" + EncodeSecret(user_key) + ")", signing_provider,
        error, /*require_checksum=*/false);
    BOOST_REQUIRE(descriptor);
    WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 0};
    TaprootBuilder user_builder, provider_builder;
    user_builder.Finalize(XOnlyPubKey{user_key.GetPubKey()});
    provider_builder.Finalize(XOnlyPubKey{provider_key.GetPubKey()});
    const CScript user_script = GetScriptForDestination(user_builder.GetOutput());
    const CScript provider_script = GetScriptForDestination(provider_builder.GetOutput());
    CMutableTransaction user_source;
    user_source.SetDigiDollarType(::DD_TX_TRANSFER);
    user_source.vin.emplace_back(COutPoint{uint256::ONE, 80});
    user_source.vout.emplace_back(0, user_script);
    user_source.vout.emplace_back(0, CScript{} << OP_RETURN
        << std::vector<unsigned char>{'D', 'D'} << CScriptNum(DD_TX_TRANSFER) << CScriptNum(1000));
    const auto user_tx = MakeTransactionRef(std::move(user_source));
    CMutableTransaction provider_source;
    provider_source.vin.emplace_back(COutPoint{uint256::ONE, 81});
    provider_source.vout.emplace_back(COIN, provider_script);
    const auto provider_tx = MakeTransactionRef(std::move(provider_source));
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        BOOST_REQUIRE(m_wallet.AddWalletDescriptor(wallet_descriptor, signing_provider, "", false));
        BOOST_REQUIRE(m_wallet.AddToWallet(user_tx, TxStateInMempool{}));
    }
    {
        LOCK(cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        coins.AddCoin({user_tx->GetHash(), 0}, Coin{user_tx->vout[0], 0, false}, false);
        coins.AddCoin({provider_tx->GetHash(), 0}, Coin{provider_tx->vout[0], 0, false}, false);
    }
    CollaborativeTransferParams params;
    params.inputs = {{COutPoint{user_tx->GetHash(), 0}, user_tx, InputRole::USER_DD},
        {COutPoint{provider_tx->GetHash(), 0}, provider_tx, InputRole::PROVIDER_DGB}};
    params.dd_outputs = {{provider_script, 100, DDOutputRole::RECIPIENT},
        {user_script, 900, DDOutputRole::USER_CHANGE}};
    params.dgb_outputs = {CTxOut{9 * COIN / 10, provider_script}};
    params.fee_rate = 1;
    const auto built = BuildUnsignedCollaborativeTransfer(params, Params(), *m_node.chainman);
    BOOST_REQUIRE_MESSAGE(built.success, built.error);
    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE(CreateCollaborativePSBTTemplate(built.tx, params.inputs, trusted, error));
    constexpr int64_t now{100000};
    PaymentIntent intent;
    intent.genesis_hash = Params().GenesisBlock().GetHash();
    intent.provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    intent.request_id = "550e8400-e29b-41d4-a716-446655440080";
    intent.session_id = uint256S("80");
    intent.client_nonce = uint256S("81");
    intent.canonical_request_hash = uint256S("82");
    intent.user_dd_inputs = {params.inputs[0].outpoint};
    intent.recipient_script = provider_script;
    intent.recipient_amount = DDCents{100};
    intent.user_dd_change_script = user_script;
    intent.offer_id = uint256S("83");
    intent.policy_hash = uint256S("84");
    intent.funding_model = FundingModel::USER_PAID;
    intent.expires_at = now + 60;
    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = uint256S("85");
    quote.funding_model = intent.funding_model;
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.reserved_dgb_inputs = {{params.inputs[1].outpoint,
        CMutableTransaction{*provider_tx}, DGBSatoshis{COIN}}};
    quote.dgb_change_script = provider_script;
    quote.network_fee = DGBSatoshis{COIN / 10};
    quote.created_at = now;
    quote.expires_at = now + 60;
    quote.retry_until = now + DEFAULT_RETRY_SECONDS;
    quote.unsigned_transaction = built.tx;
    quote.unsigned_txid = CTransaction{built.tx}.GetHash();
    quote.template_commitment = GetCollaborativeTemplateCommitment(intent, quote, trusted);
    const OfferTerms offer{intent.offer_id, intent.policy_hash, FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, 0, DDCents{100}, DDCents{1000000}};
    const auto sign_quote = [](PaymasterQuote& candidate, const CKey& key) {
        candidate.identity_signature.resize(64);
        BOOST_REQUIRE(key.SignSchnorr(GetPaymasterQuoteSignatureHash(candidate),
            candidate.identity_signature, nullptr, uint256{}));
    };
    sign_quote(quote, provider_key);
    CollaborativePSBTTemplate reconstructed;
    BOOST_REQUIRE_MESSAGE(ValidatePaymasterQuoteForClient(quote, intent, offer,
        XOnlyPubKey{provider_key.GetPubKey()}, DDCents{0}, now, error), error);
    BOOST_REQUIRE_MESSAGE(BuildTrustedQuoteTemplate(m_wallet, intent, quote, Params(),
        *m_node.chainman, reconstructed, error), error);
    for (int attack = 0; attack < 8; ++attack) {
        BOOST_TEST_CONTEXT("correctly signed malicious quote " << attack) {
            auto malicious = quote;
            switch (attack) {
            case 0: malicious.unsigned_transaction.vout[0].scriptPubKey = user_script; break;
            case 1:
                malicious.unsigned_transaction.vout.back().scriptPubKey = CScript{} << OP_RETURN
                    << std::vector<unsigned char>{'D', 'D'} << CScriptNum(DD_TX_TRANSFER)
                    << CScriptNum(101) << CScriptNum(899);
                break;
            case 2: malicious.unsigned_transaction.vout[1].scriptPubKey = provider_script; break;
            case 3: malicious.unsigned_transaction.vout.emplace_back(0, provider_script); break;
            case 4: malicious.unsigned_transaction.vin[0].prevout = params.inputs[1].outpoint; break;
            case 5: ++malicious.unsigned_transaction.nLockTime; break;
            case 6: ++malicious.unsigned_transaction.nVersion; break;
            case 7: --malicious.unsigned_transaction.vout[2].nValue; break;
            }
            malicious.unsigned_txid = CTransaction{malicious.unsigned_transaction}.GetHash();
            sign_quote(malicious, provider_key);
            // A real provider signature and consistent txid alone are not the
            // firewall. Reconstruction from our wallet/intent must still fail.
            BOOST_REQUIRE(ValidatePaymasterQuoteForClient(malicious, intent, offer,
                XOnlyPubKey{provider_key.GetPubKey()}, DDCents{0}, now, error));
            BOOST_CHECK(!BuildTrustedQuoteTemplate(m_wallet, intent, malicious, Params(),
                *m_node.chainman, reconstructed, error));
            BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_TRANSACTION_MISMATCH");
            BOOST_CHECK(!reconstructed.psbt.tx);
        }
    }
    auto exchanged_identity = quote;
    exchanged_identity.provider_id = GetPaymasterId(XOnlyPubKey{impostor.GetPubKey()});
    sign_quote(exchanged_identity, impostor);
    BOOST_CHECK(!ValidatePaymasterQuoteForClient(exchanged_identity, intent, offer,
        XOnlyPubKey{impostor.GetPubKey()}, DDCents{0}, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_CHAIN_OR_PROVIDER_MISMATCH");

    // Reconstruct the provider's exact fee output at the client boundary.
    // The expected output amounts are independent of the shared calculator.
    struct CarrierCase {
        CAmount carrier;
        CAmount fee;
        CAmount output; // -1 denotes an invalid carrier successor.
        FundingModel funding_model{FundingModel::USER_PAID};
        CAmount recipient{100};
    };
    const CarrierCase carrier_cases[]{
        {100, 0, 0},
        {99, 1, -1},
        {100, 1, 101},
        {100, 99, 199},
        {MAX_DD_OUTPUT_CENTS - 99, 99, MAX_DD_OUTPUT_CENTS},
        {MAX_DD_OUTPUT_CENTS - 98, 99, -1},
        {MAX_DD_OUTPUT_CENTS - 1, 1, MAX_DD_OUTPUT_CENTS},
        {MAX_DD_OUTPUT_CENTS, 1, -1},
        {100, 100, 100},
        {100, 0, 0, FundingModel::SPONSORED},
        {100, 0, 0, FundingModel::SPONSORED, 1000},
        {100, 0, 0, FundingModel::USER_PAID, 900},
        {100, 1, 101, FundingModel::USER_PAID, 999},
    };
    for (const auto& row : carrier_cases) {
        BOOST_TEST_CONTEXT("carrier=" << row.carrier << " fee=" << row.fee << " recipient=" << row.recipient
                                     << " model=" << static_cast<int>(row.funding_model)) {
            auto fee_intent = intent;
            fee_intent.recipient_amount = DDCents{row.recipient};
            fee_intent.funding_model = row.funding_model;
            const CAmount user_change = 1000 - row.recipient - row.fee;
            if (user_change == 0) fee_intent.user_dd_change_script.clear();
            ProviderPolicy fee_policy;
            fee_policy.funding_models = row.funding_model == FundingModel::USER_PAID
                ? FUNDING_MODEL_USER_PAID : FUNDING_MODEL_SPONSORED;
            fee_policy.min_payment = DDCents{100};
            fee_policy.max_payment = DDCents{1000000};
            fee_policy.fee_rate_bps = row.fee > 0 ? MAX_RATE_BPS : 0;
            fee_policy.maximum_user_paid_service_fee = DDCents{row.fee};
            fee_policy.maximum_network_fee = DGBSatoshis{COIN / 10};
            fee_intent.policy_hash = GetProviderPolicyHash(fee_policy);
            fee_intent.user_input_proofs = {{fee_intent.user_dd_inputs.front(), std::vector<unsigned char>(64)}};
            const uint256 empty_merkle_root;
            BOOST_REQUIRE(user_key.SignSchnorr(
                GetUserInputControlHash(fee_intent, fee_intent.user_dd_inputs.front()),
                fee_intent.user_input_proofs.front().signature, &empty_merkle_root, uint256{}));
            auto fee_quote = quote;
            fee_quote.policy_hash = fee_intent.policy_hash;
            fee_quote.intent_hash = GetPaymentIntentHash(fee_intent);
            fee_quote.funding_model = row.funding_model;
            fee_quote.fee_rate_bps = fee_policy.fee_rate_bps;
            fee_quote.service_fee = DDCents{row.fee};
            const auto build_provider = [&](ProviderQuoteBuildResult& result) {
                PaymasterQuoteRequest fee_request;
                fee_request.intent = fee_intent;
                ProviderQuoteBuildParameters provider_parameters;
                provider_parameters.quote_id = fee_quote.quote_id;
                provider_parameters.user_inputs = {params.inputs.front()};
                provider_parameters.reserved_carrier = fee_quote.reserved_carrier;
                provider_parameters.reserved_dgb_inputs = fee_quote.reserved_dgb_inputs;
                provider_parameters.carrier_return_script = fee_quote.carrier_return_script;
                provider_parameters.provider_fee_script = fee_quote.provider_fee_script;
                provider_parameters.dgb_change_script = fee_quote.dgb_change_script;
                provider_parameters.network_fee = fee_quote.network_fee;
                provider_parameters.created_at = fee_quote.created_at;
                provider_parameters.expires_at = fee_quote.expires_at;
                provider_parameters.retry_until = fee_quote.retry_until;
                LOCK(cs_main);
                return BuildUnsignedProviderQuote(
                    fee_request, fee_policy, provider_parameters, Params(),
                    m_node.chainman->ActiveChainstate().CoinsTip(), result, error);
            };
            auto fee_params = params;
            fee_params.dd_outputs[0].amount = row.recipient;
            if (user_change == 0) fee_params.dd_outputs.erase(fee_params.dd_outputs.begin() + 1);
            else fee_params.dd_outputs[1].amount = user_change;
            if (row.fee > 0 && row.fee < 100) {
                CMutableTransaction carrier_source;
                carrier_source.SetDigiDollarType(::DD_TX_TRANSFER);
                carrier_source.vin.emplace_back(COutPoint{uint256::ONE, 82});
                carrier_source.vout.emplace_back(0, provider_script);
                carrier_source.vout.emplace_back(0, CScript{} << OP_RETURN
                    << std::vector<unsigned char>{'D', 'D'} << CScriptNum(DD_TX_TRANSFER)
                    << CScriptNum(row.carrier));
                const auto carrier_tx = MakeTransactionRef(std::move(carrier_source));
                const COutPoint carrier_input{carrier_tx->GetHash(), 0};
                fee_quote.reserved_carrier = VerifiedDDCarrier{
                    carrier_input, CMutableTransaction{*carrier_tx}, DDCents{row.carrier}};
                fee_quote.carrier_return_script = provider_script;
                const auto successor = ComputeCarrierSuccessor(
                    DDCents{row.carrier}, DDCents{row.fee}, error);
                BOOST_CHECK_EQUAL(successor.has_value(), row.output >= 0);
                if (row.output < 0) {
                    ProviderQuoteBuildResult rejected;
                    BOOST_CHECK(!build_provider(rejected));
                    BOOST_CHECK_EQUAL(error, row.carrier < 100
                        ? "PAYMASTER_INVALID_QUOTE_FEE_SLOT" : "PAYMASTER_INVALID_CARRIER_SUCCESSOR");
                    BOOST_CHECK(!BuildTrustedQuoteTemplate(m_wallet, fee_intent, fee_quote,
                        Params(), *m_node.chainman, reconstructed, error));
                    BOOST_CHECK_EQUAL(error, "PAYMASTER_CARRIER_OUTPUT_MISMATCH");
                    continue;
                }
                BOOST_REQUIRE(successor);
                BOOST_CHECK_EQUAL(successor->value, row.output);
                {
                    LOCK(cs_main);
                    m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
                        carrier_input, Coin{carrier_tx->vout[0], 0, false}, true);
                }
                fee_params.inputs.insert(fee_params.inputs.begin() + 1,
                    {carrier_input, carrier_tx, InputRole::PROVIDER_CARRIER});
                fee_params.dd_outputs.push_back(
                    {provider_script, row.output, DDOutputRole::CARRIER_SUCCESSOR});
            } else if (row.fee >= 100) {
                fee_quote.provider_fee_script = provider_script;
                fee_params.dd_outputs.push_back(
                    {provider_script, row.output, DDOutputRole::PROVIDER_FEE});
            }
            const auto fee_transaction = BuildUnsignedCollaborativeTransfer(
                fee_params, Params(), *m_node.chainman);
            BOOST_REQUIRE_MESSAGE(fee_transaction.success, fee_transaction.error);
            fee_quote.unsigned_transaction = fee_transaction.tx;
            fee_quote.unsigned_txid = CTransaction{fee_transaction.tx}.GetHash();
            CollaborativePSBTTemplate fee_template;
            BOOST_REQUIRE(CreateCollaborativePSBTTemplate(
                fee_transaction.tx, fee_params.inputs, fee_template, error));
            fee_quote.template_commitment = GetCollaborativeTemplateCommitment(
                fee_intent, fee_quote, fee_template);

            // Compare both production boundaries with the hand-arranged
            // expected transaction above, including role and PSBT bytes.
            ProviderQuoteBuildResult provider_built;
            BOOST_REQUIRE_MESSAGE(build_provider(provider_built), error);
            BOOST_CHECK(SerializePaymasterTestArtifact(provider_built.quote.unsigned_transaction) ==
                        SerializePaymasterTestArtifact(fee_transaction.tx));
            BOOST_CHECK(SerializePaymasterTestArtifact(provider_built.trusted_template.psbt) ==
                        SerializePaymasterTestArtifact(fee_template.psbt));
            BOOST_CHECK(provider_built.trusted_template.input_roles == fee_template.input_roles);
            BOOST_CHECK(provider_built.quote.template_commitment == fee_quote.template_commitment);
            sign_quote(fee_quote, provider_key);
            BOOST_REQUIRE_MESSAGE(BuildTrustedQuoteTemplate(m_wallet, fee_intent, fee_quote,
                Params(), *m_node.chainman, reconstructed, error), error);
            BOOST_REQUIRE(reconstructed.psbt.tx);
            BOOST_CHECK(CTransaction{*reconstructed.psbt.tx} == CTransaction{fee_transaction.tx});
            BOOST_CHECK(SerializePaymasterTestArtifact(reconstructed.psbt) ==
                        SerializePaymasterTestArtifact(fee_template.psbt));
            BOOST_CHECK(reconstructed.input_roles == fee_template.input_roles);
        }
    }
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

namespace {
void CheckProviderExecutionFirewall(WalletTestingSetup& fixture, uint16_t version)
{
    auto& m_wallet = fixture.m_wallet;
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
    capacity_proof.version = version;
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
    quote_request.version = version;
    quote_request.intent = intent;
    PaymasterQuoteResponse quote_response;
    quote_response.version = version;
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
    const auto user_input = signed_psbt.tx->vin.front().prevout;
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.LockDigiDollarCoin(user_input));
        BOOST_REQUIRE(m_wallet.LockCoin(user_input));
    }
    BOOST_CHECK(!SignCollaborativePSBTForParty(
        m_wallet, signed_psbt, trusted, SigningParty::USER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_INPUT_LOCKED");
    BOOST_CHECK(SerializePaymasterTestArtifact(signed_psbt) == SerializePaymasterTestArtifact(trusted.psbt));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.UnlockCoin(user_input));
        BOOST_CHECK(m_wallet.IsDigiDollarLockedCoin(user_input));
        BOOST_CHECK(m_wallet.IsLockedCoin(user_input));
    }
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(
        m_wallet, signed_psbt, trusted, SigningParty::USER, error), error);
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

    ProviderAttempt saved_signature{attempt};
    saved_signature.state = AttemptState::PROVIDER_SIGNED;
    saved_signature.provider_signed_at = now + 1;
    saved_signature.updated_at = now + 2;
    PaymasterResult envelope;
    envelope.genesis_hash = quote_request.intent.genesis_hash;
    envelope.provider_id = attempt.provider_id;
    envelope.commit_key = attempt.commit_key;
    envelope.result_sequence = 1;
    envelope.status = PaymasterResultStatus::FINAL_COMMITTED;
    envelope.txid = final.GetHash();
    envelope.raw_transaction_hash = Hash(final_bytes);
    envelope.final_transaction = final_transaction;
    envelope.updated_at = saved_signature.provider_signed_at;
    envelope.identity_signature.resize(64);
    BOOST_REQUIRE(key.SignSchnorr(GetPaymasterResultSignatureHash(envelope),
        envelope.identity_signature, nullptr, uint256{}));
    saved_signature.provider_signed_result = SerializePaymasterTestArtifact(envelope);
    PaymasterResult decoded;
    error = "stale error";
    BOOST_REQUIRE_MESSAGE(DecodeProviderSignedResultEnvelope(saved_signature, decoded, error), error);
    BOOST_CHECK(error.empty());
    BOOST_CHECK(SerializePaymasterTestArtifact(decoded) == SerializePaymasterTestArtifact(envelope));
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto changed = saved_signature;
        if (mutation == 0) changed.provider_signed_result.push_back(0);
        if (mutation == 1) changed.provider_signed_result.back() ^= 1;
        if (mutation == 2) ++changed.provider_signed_at;
        if (mutation == 3) changed.final_transaction.back() ^= 1;
        decoded = envelope;
        BOOST_CHECK(!DecodeProviderSignedResultEnvelope(changed, decoded, error));
        BOOST_CHECK(!error.empty());
        BOOST_CHECK(SerializePaymasterTestArtifact(decoded) == SerializePaymasterTestArtifact(PaymasterResult{}));
    }

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

    // Both complete direct-payment versions retain the same inner signed
    // quote and exact final transaction, including after quote/retry expiry.
    for (const uint16_t version : {DigiDollar::Paymaster::LEGACY_DIRECT_PROTOCOL_VERSION,
                                   DigiDollar::Paymaster::PROTOCOL_VERSION}) {
        PaymasterQuoteRequest supported_request{quote_request};
        PaymasterQuoteResponse supported_response{quote_response};
        supported_request.version = version;
        supported_response.version = version;
        ProviderAttempt supported_attempt{attempt};
        supported_attempt.quote_request = SerializePaymasterTestArtifact(supported_request);
        supported_attempt.signed_quote = SerializePaymasterTestArtifact(supported_response);
        ProviderAttempt before_signature{supported_attempt};
        before_signature.state = AttemptState::USER_PSBT_ACCEPTED;
        BOOST_REQUIRE_MESSAGE(ValidateProviderAuthorizationForExecution(
            before_signature, now + 1, signing_template, error), error);
        BOOST_REQUIRE_MESSAGE(ValidateProviderCommitForExecution(
            supported_attempt, commit, commit.retry_until + 1, validated_final, error), error);
        BOOST_CHECK(CTransaction{validated_final}.GetWitnessHash() == final.GetWitnessHash());
    }

    // Protocols 1-4 cannot authorize either a new signature or execution of a
    // previously persisted commit.
    for (uint16_t protocol_version{1};
         protocol_version < DigiDollar::Paymaster::LEGACY_DIRECT_PROTOCOL_VERSION;
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

} // namespace

BOOST_AUTO_TEST_CASE(new_signature_locks_follow_validated_asset_roles)
{
    using namespace DigiDollar::Paymaster;
    const COutPoint input{uint256::ONE, 50};
    std::string error;
    LOCK(m_wallet.cs_wallet);
    BOOST_REQUIRE(m_wallet.LockDigiDollarCoin(input));
    m_wallet.LockPaymasterCoin(input);
    for (const auto role : {InputRole::USER_DD, InputRole::PROVIDER_CARRIER}) {
        BOOST_CHECK(CheckPaymasterNewSignatureInputs(m_wallet, {input}, error, role));
    }
    for (const auto role : {InputRole::USER_DGB, InputRole::PROVIDER_DGB}) {
        BOOST_CHECK(!CheckPaymasterNewSignatureInputs(m_wallet, {input}, error, role));
    }
    BOOST_REQUIRE(m_wallet.LockCoin(input));
    for (const auto role : {InputRole::USER_DD, InputRole::USER_DGB, InputRole::PROVIDER_CARRIER, InputRole::PROVIDER_DGB}) {
        BOOST_CHECK(!CheckPaymasterNewSignatureInputs(m_wallet, {input}, error, role));
        BOOST_CHECK_EQUAL(error, role == InputRole::USER_DD || role == InputRole::USER_DGB
            ? "PAYMASTER_CLIENT_INPUT_LOCKED" : "PAYMASTER_PROVIDER_INPUT_LOCKED");
    }
    BOOST_REQUIRE(m_wallet.UnlockCoin(input));
    BOOST_CHECK(CheckPaymasterNewSignatureInputs(m_wallet, {input}, error, InputRole::PROVIDER_CARRIER));
    BOOST_CHECK(!CheckPaymasterNewSignatureInputs(m_wallet, {input}, error));
    BOOST_CHECK(m_wallet.IsDigiDollarLockedCoin(input));
    BOOST_REQUIRE(m_wallet.UnlockDigiDollarCoin(input));
    BOOST_CHECK(m_wallet.IsLockedCoin(input));
}

BOOST_AUTO_TEST_CASE(provider_execution_firewall_revalidates_durable_authority)
{
    CheckProviderExecutionFirewall(*this, DigiDollar::Paymaster::PROTOCOL_VERSION);
}

BOOST_AUTO_TEST_CASE(v5_provider_commit_preserves_exact_signed_authority)
{
    CheckProviderExecutionFirewall(*this, DigiDollar::Paymaster::LEGACY_DIRECT_PROTOCOL_VERSION);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
