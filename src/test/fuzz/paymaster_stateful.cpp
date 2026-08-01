// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Stateful Paymaster protocol fuzzer.
 *
 * One input drives a sequence of capacity, directory, quote, submit, result,
 * recovery, expiry, and replay operations. The important oracle is not whether
 * arbitrary operations succeed, but that every reachable state preserves
 * terminal-state monotonicity, bounded arithmetic, and exact authorization
 * bindings across adversarial ordering.
 */

#include <chainparams.h>
#include <coins.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <key.h>
#include <paymaster/directory.h>
#include <paymaster/provider.h>
#include <paymaster/recovery.h>
#include <paymaster/reputation.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <util/chaintype.h>
#include <version.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace DigiDollar::Paymaster;

namespace {

constexpr int64_t NOW{100000};
constexpr int64_t NETWORK_FEE{COIN / 10};
constexpr int64_t PROVIDER_DGB_VALUE{2 * NETWORK_FEE};

void initialize_paymaster_stateful()
{
    ECC_Start();
    SelectParams(ChainType::REGTEST);
    assert(DigiDollar::Paymaster::PROTOCOL_VERSION == 5);
    assert(PaymentIntent::CURRENT_VERSION == 2);
    assert(ClientAuthorizationManifest::CURRENT_VERSION == 4);
    assert(AlternativeRecoveryRequest::CURRENT_VERSION == 2);
    assert(ProviderBudgetLedger::CURRENT_VERSION == 4);
    assert(SaturatingAddSeconds(std::numeric_limits<int64_t>::max(), 1) ==
           std::numeric_limits<int64_t>::max());
    assert(SaturatingAddSeconds(std::numeric_limits<int64_t>::min(), -1) ==
           std::numeric_limits<int64_t>::min());
}

template <typename... Ts>
std::vector<unsigned char> SerializeExact(const Ts&... values)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    (stream << ... << values);
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename... Ts>
uint256 CanonicalHash(const Ts&... values)
{
    return Hash(SerializeExact(values...));
}

bool ContainsSubsequence(const std::vector<unsigned char>& haystack,
                         const std::vector<unsigned char>& needle)
{
    return !needle.empty() &&
           std::search(haystack.begin(), haystack.end(), needle.begin(),
                       needle.end()) != haystack.end();
}

CKey DeterministicKey(uint8_t discriminator)
{
    std::array<unsigned char, 32> secret{};
    secret.back() = discriminator;
    CKey key;
    key.Set(secret.begin(), secret.end(), true);
    assert(key.IsValid());
    return key;
}

XOnlyPubKey BIP86OutputKey(const CKey& internal_key)
{
    const auto tweaked =
        XOnlyPubKey{internal_key.GetPubKey()}.CreateTapTweak(nullptr);
    assert(tweaked.has_value());
    return tweaked->first;
}

CScript BIP86Script(const CKey& internal_key)
{
    return GetScriptForDestination(
        WitnessV1Taproot{BIP86OutputKey(internal_key)});
}

void SignBIP86(const CKey& internal_key, const uint256& hash,
               std::vector<unsigned char>& signature)
{
    signature.resize(64);
    const uint256 empty_merkle_root;
    assert(internal_key.SignSchnorr(
        hash, signature, &empty_merkle_root, uint256{}));
}

void SignIdentity(const CKey& identity_key, const uint256& hash,
                  std::vector<unsigned char>& signature)
{
    signature.resize(64);
    assert(identity_key.SignSchnorr(
        hash, signature, nullptr, uint256{}));
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

struct CapacityArtifacts {
    PaymasterCapacityRequest request;
    PaymasterCapacityProof proof;
    ValidatedCapacitySnapshot snapshot;
};

enum class ReplayDisposition { NEW,
                               EXACT,
                               CONFLICT };

class SemanticReplayCache
{
private:
    std::map<std::string, uint256> m_seen;

public:
    ReplayDisposition Observe(const std::string& key, const uint256& content)
    {
        const auto [it, inserted] = m_seen.emplace(key, content);
        if (inserted) return ReplayDisposition::NEW;
        return it->second == content ? ReplayDisposition::EXACT : ReplayDisposition::CONFLICT;
    }
};

/**
 * A deterministic, key-only fixture. Keys are public test constants and are
 * never read from a wallet. Chainstate checks are callbacks over the local
 * in-memory coins view, so this target has no network, disk, or wallet hooks.
 */
struct StatefulArtifacts {
    CCoinsView base;
    CCoinsViewCache coins{&base};

    CKey provider_identity_key{DeterministicKey(1)};
    CKey provider_dgb_key{DeterministicKey(2)};
    CKey recovery_identity_key{DeterministicKey(3)};
    CKey recovery_dgb_key{DeterministicKey(4)};
    CKey user_key{DeterministicKey(5)};
    CKey recipient_key{DeterministicKey(6)};
    CKey user_change_key{DeterministicKey(7)};
    CKey provider_change_key{DeterministicKey(8)};
    CKey recovery_return_key{DeterministicKey(9)};
    CKey recovery_fee_key{DeterministicKey(10)};
    CKey recovery_change_key{DeterministicKey(11)};

    CTransactionRef user_dd;
    CMutableTransaction provider_dgb;
    CMutableTransaction recovery_dgb;
    VerifiedDGBInput verified_provider_dgb;
    VerifiedDGBInput verified_recovery_dgb;

    ProviderPolicy policy;
    OfferTerms offer;
    CapacityArtifacts capacity;
    PaymentIntent intent;
    PaymasterQuoteRequest quote_request;
    PaymasterQuote quote;
    PaymasterQuoteResponse quote_response;
    CollaborativePSBTTemplate trusted_template;
    ClientAuthorizationManifest client_manifest;
    ProviderAuthorizationManifest provider_manifest;
    PaymasterSubmit submit;
    PaymasterResult result;
    PaymasterResultMessage result_message;

    StatefulArtifacts()
    {
        user_dd = MakeDDSource(1000, 1, BIP86Script(user_key));
        provider_dgb = MakeDGBSource(
            PROVIDER_DGB_VALUE, 2, BIP86Script(provider_dgb_key));
        recovery_dgb = MakeDGBSource(
            PROVIDER_DGB_VALUE, 3, BIP86Script(recovery_dgb_key));

        coins.AddCoin(COutPoint{user_dd->GetHash(), 0},
                      Coin{user_dd->vout[0], 500, false}, false);
        coins.AddCoin(COutPoint{CTransaction{provider_dgb}.GetHash(), 0},
                      Coin{provider_dgb.vout[0], 500, false}, false);
        coins.AddCoin(COutPoint{CTransaction{recovery_dgb}.GetHash(), 0},
                      Coin{recovery_dgb.vout[0], 500, false}, false);

        verified_provider_dgb = {
            COutPoint{CTransaction{provider_dgb}.GetHash(), 0},
            provider_dgb,
            DGBSatoshis{PROVIDER_DGB_VALUE}};
        verified_recovery_dgb = {
            COutPoint{CTransaction{recovery_dgb}.GetHash(), 0},
            recovery_dgb,
            DGBSatoshis{PROVIDER_DGB_VALUE}};

        policy.funding_models = FUNDING_MODEL_SPONSORED;
        policy.sponsorship_scope = SponsorshipScope::PUBLIC;
        policy.fee_rate_bps = 0;
        policy.min_payment = DDCents{100};
        policy.max_payment = DDCents{MAX_DD_OUTPUT_CENTS};
        policy.quote_ttl = 60;
        policy.maximum_network_fee = DGBSatoshis{NETWORK_FEE};

        const XOnlyPubKey identity_key{provider_identity_key.GetPubKey()};
        const PaymasterId provider_id{GetPaymasterId(identity_key)};
        capacity = MakeCapacity(
            provider_identity_key, provider_dgb_key, verified_provider_dgb,
            provider_id, uint256S("101"), uint256S("102"),
            FundingModel::SPONSORED, false);

        intent.genesis_hash = Params().GenesisBlock().GetHash();
        intent.provider_id = provider_id;
        intent.request_id = "550e8400-e29b-41d4-a716-446655440100";
        intent.session_id = uint256S("101");
        intent.client_nonce = uint256S("102");
        intent.canonical_request_hash = uint256S("10201");
        intent.user_dd_inputs = {COutPoint{user_dd->GetHash(), 0}};
        intent.recipient_script = BIP86Script(recipient_key);
        intent.recipient_amount = DDCents{900};
        intent.user_dd_change_script = BIP86Script(user_change_key);
        intent.offer_id = uint256S("103");
        intent.funding_model = FundingModel::SPONSORED;
        intent.sponsorship_scope = SponsorshipScope::PUBLIC;
        intent.policy_hash = GetProviderPolicyHash(policy);
        intent.expires_at = NOW + 30;
        intent.user_input_proofs.push_back({intent.user_dd_inputs.front(), {}});
        SignBIP86(user_key,
                  GetUserInputControlHash(intent, intent.user_dd_inputs.front()),
                  intent.user_input_proofs.front().signature);
        quote_request.intent = intent;

        ProviderQuoteBuildParameters parameters;
        parameters.quote_id = uint256S("104");
        parameters.user_inputs = {
            {intent.user_dd_inputs.front(), user_dd, InputRole::USER_DD}};
        parameters.reserved_dgb_inputs = {verified_provider_dgb};
        parameters.dgb_change_script = BIP86Script(provider_change_key);
        parameters.network_fee = DGBSatoshis{NETWORK_FEE};
        parameters.created_at = NOW;
        parameters.expires_at = NOW + 30;
        parameters.retry_until = NOW + 60;
        ProviderQuoteBuildResult built;
        std::string error;
        assert(BuildUnsignedProviderQuote(
            quote_request, policy, parameters, Params(), coins, built, error));
        quote = std::move(built.quote);
        trusted_template = std::move(built.trusted_template);
        SignIdentity(provider_identity_key, GetPaymasterQuoteSignatureHash(quote),
                     quote.identity_signature);

        offer.offer_id = intent.offer_id;
        offer.policy_hash = intent.policy_hash;
        offer.funding_model = FundingModel::SPONSORED;
        offer.scope = SponsorshipScope::PUBLIC;
        offer.fee_rate_bps = 0;
        offer.min_payment = DDCents{100};
        offer.max_payment = DDCents{MAX_DD_OUTPUT_CENTS};

        quote_response.request_id = intent.request_id;
        quote_response.session_id = intent.session_id;
        quote_response.quote = quote;

        assert(BuildClientAuthorizationManifest(
            intent, quote, capacity.snapshot, trusted_template, DDCents{0},
            client_manifest, error));
        assert(BuildProviderAuthorizationManifest(
            intent, quote, trusted_template, uint256S("105"),
            provider_manifest, error));

        submit.genesis_hash = intent.genesis_hash;
        submit.provider_id = intent.provider_id;
        submit.request_id = intent.request_id;
        submit.session_id = intent.session_id;
        submit.quote_id = quote.quote_id;
        submit.template_commitment = quote.template_commitment;
        submit.commit_key = GetPaymasterCommitKey(
            intent.provider_id, intent.client_nonce, quote.intent_hash,
            quote.quote_id, quote.template_commitment);
        submit.user_psbt = SerializeExact(UserSignedProviderPSBT());

        result.genesis_hash = intent.genesis_hash;
        result.provider_id = intent.provider_id;
        result.commit_key = submit.commit_key;
        result.result_sequence = 1;
        result.status = PaymasterResultStatus::NO_FINAL_COMMIT;
        result.updated_at = NOW + 5;
        SignIdentity(provider_identity_key, GetPaymasterResultSignatureHash(result),
                     result.identity_signature);
        result_message.request_id = intent.request_id;
        result_message.session_id = intent.session_id;
        result_message.result = result;

        ValidateBaseline();
    }

    CapacityArtifacts MakeCapacity(
        const CKey& identity, const CKey& dgb_key,
        const VerifiedDGBInput& dgb_input, const PaymasterId& provider_id,
        const uint256& session_id, const uint256& nonce,
        FundingModel funding_model, bool requires_carrier) const
    {
        CapacityArtifacts artifacts;
        artifacts.request.genesis_hash = Params().GenesisBlock().GetHash();
        artifacts.request.provider_id = provider_id;
        artifacts.request.request_id =
            "550e8400-e29b-41d4-a716-446655440100";
        artifacts.request.session_id = session_id;
        artifacts.request.client_nonce = nonce;
        artifacts.request.funding_model = funding_model;
        artifacts.request.requires_carrier = requires_carrier;
        artifacts.request.created_at = NOW;
        artifacts.request.expires_at = NOW + 60;

        PaymasterCapacityProof& proof = artifacts.proof;
        proof.genesis_hash = artifacts.request.genesis_hash;
        proof.provider_id = provider_id;
        proof.request_id = artifacts.request.request_id;
        proof.session_id = session_id;
        proof.client_nonce = nonce;
        proof.funding_model = funding_model;
        proof.requires_carrier = requires_carrier;
        proof.snapshot_id = Hash(session_id, nonce);
        proof.created_at = NOW;
        proof.expires_at = NOW + 30;
        PaymasterLiquiditySlot slot;
        CapacityDGBInput dgb;
        dgb.input = dgb_input;
        dgb.control_proof.reference_block = uint256S("106");
        dgb.control_proof.expires_at = proof.expires_at;
        SignBIP86(dgb_key,
                  GetCapacityControlHash(
                      proof, dgb.input.outpoint,
                      dgb.control_proof.expires_at),
                  dgb.control_proof.signature);
        slot.dgb_inputs.push_back(dgb);
        proof.liquidity_slots.push_back(slot);
        SignIdentity(identity, GetCapacityProofSignatureHash(proof),
                     proof.identity_signature);

        ValidatedCapacitySnapshot& snapshot = artifacts.snapshot;
        snapshot.snapshot_id = proof.snapshot_id;
        snapshot.resource_commitment = GetCapacityResourceCommitment(proof);
        snapshot.session_id = session_id;
        snapshot.attempt_id = Hash(nonce, session_id);
        snapshot.provider_id = provider_id;
        snapshot.client_nonce = nonce;
        snapshot.request_hash = CanonicalHash(artifacts.request);
        snapshot.capacity_proof = SerializeExact(proof);
        snapshot.created_at = proof.created_at;
        snapshot.expires_at = proof.expires_at;
        snapshot.validated_at = NOW;
        snapshot.funding_model = funding_model;
        snapshot.requires_carrier = requires_carrier;
        return artifacts;
    }

    CapacityChainstateCallbacks CapacityCallbacks(
        const VerifiedDGBInput& expected, const CKey& dgb_key) const
    {
        CapacityChainstateCallbacks callbacks;
        callbacks.validate_reference_block = [](const uint256& block,
                                                std::string&) {
            return block == uint256S("106");
        };
        callbacks.validate_dgb_input =
            [expected, dgb_key](const VerifiedDGBInput& input,
                                XOnlyPubKey& output_key, std::string&) {
                if (input.outpoint != expected.outpoint ||
                    !(input.value == expected.value) ||
                    CTransaction{input.creating_tx}.GetHash() !=
                        CTransaction{expected.creating_tx}.GetHash()) {
                    return false;
                }
                output_key = BIP86OutputKey(dgb_key);
                return true;
            };
        return callbacks;
    }

    AlternativeRecoveryParameters RecoveryParameters() const
    {
        AlternativeRecoveryParameters parameters;
        parameters.genesis_hash = intent.genesis_hash;
        parameters.request_id = intent.request_id;
        parameters.session_id = intent.session_id;
        parameters.original_provider_id = intent.provider_id;
        parameters.recovery_provider_identity_key =
            XOnlyPubKey{recovery_identity_key.GetPubKey()};
        parameters.recovery_provider_id =
            GetPaymasterId(parameters.recovery_provider_identity_key);
        parameters.original_commit_key = submit.commit_key;
        parameters.original_template_commitment = quote.template_commitment;
        parameters.offer_id = intent.offer_id;
        parameters.policy_hash = intent.policy_hash;

        const CapacityArtifacts recovery_capacity = MakeCapacity(
            recovery_identity_key, recovery_dgb_key, verified_recovery_dgb,
            parameters.recovery_provider_id, intent.session_id,
            uint256S("107"), FundingModel::USER_PAID, false);
        parameters.capacity_request = recovery_capacity.request;
        parameters.capacity_snapshot = recovery_capacity.snapshot;
        parameters.persisted_user_dd_inputs = intent.user_dd_inputs;
        parameters.user_dd_inputs = {
            {intent.user_dd_inputs.front(), user_dd, InputRole::USER_DD}};
        const CScript return_script{BIP86Script(recovery_return_key)};
        parameters.wallet_returns = {{return_script, DDCents{900}}};
        parameters.wallet_verified_fresh_return_scripts = {return_script};
        parameters.recovery_provider_dd_script =
            BIP86Script(recovery_fee_key);
        parameters.recovery_provider_dgb_change_script =
            BIP86Script(recovery_change_key);
        parameters.maximum_service_fee = DDCents{100};
        parameters.service_fee = DDCents{100};
        parameters.network_fee = DGBSatoshis{NETWORK_FEE};
        parameters.fee_rate = 1;
        parameters.expires_at = NOW + 20;
        return parameters;
    }

    AlternativeRecoveryRequest RecoveryRequest(
        const AlternativeRecoveryParameters& parameters) const
    {
        AlternativeRecoveryRequest request;
        request.genesis_hash = parameters.genesis_hash;
        request.request_id = parameters.request_id;
        request.session_id = parameters.session_id;
        request.original_provider_id = parameters.original_provider_id;
        request.recovery_provider_id = parameters.recovery_provider_id;
        request.privacy_profile = parameters.privacy_profile;
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
        request.user_input_proofs.push_back(
            {request.user_dd_inputs.front(), {}});
        SignBIP86(
            user_key,
            GetAlternativeRecoveryInputControlHash(
                request, request.user_dd_inputs.front()),
            request.user_input_proofs.front().signature);
        return request;
    }

    AlternativeRecoveryResponse RecoveryResponse(
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
        response.unsigned_psbt = SerializeExact(recovery.trusted_template.psbt);
        response.created_at = NOW;
        response.expires_at = parameters.expires_at;
        response.recovery_commit_key =
            GetAlternativeRecoveryCommitKey(response);
        SignIdentity(
            recovery_identity_key,
            GetAlternativeRecoveryResponseSignatureHash(response),
            response.identity_signature);
        return response;
    }

    std::vector<CTxOut> TemplatePrevouts(
        const CollaborativePSBTTemplate& trusted) const
    {
        std::vector<CTxOut> prevouts;
        prevouts.reserve(trusted.psbt.inputs.size());
        for (size_t index = 0; index < trusted.psbt.inputs.size(); ++index) {
            assert(index <= static_cast<size_t>(std::numeric_limits<int>::max()));
            CTxOut prevout;
            assert(trusted.psbt.GetInputUTXO(prevout, static_cast<int>(index)));
            prevouts.push_back(std::move(prevout));
        }
        return prevouts;
    }

    std::vector<unsigned char> InputSignature(
        const CMutableTransaction& transaction, size_t input_index,
        const CKey& internal_key,
        const PrecomputedTransactionData& txdata) const
    {
        uint256 sighash;
        ScriptExecutionData execdata;
        execdata.m_annex_init = true;
        execdata.m_annex_present = false;
        execdata.m_tapleaf_hash_init = false;
        assert(input_index <=
               static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
        assert(SignatureHashSchnorr(
            sighash, execdata, transaction, static_cast<uint32_t>(input_index),
            SIGHASH_DEFAULT,
            SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL));
        std::vector<unsigned char> signature(64);
        const uint256 empty_merkle_root;
        assert(internal_key.SignSchnorr(
            sighash, signature, &empty_merkle_root, uint256{}));
        return signature;
    }

    PartiallySignedTransaction UserSignedProviderPSBT() const
    {
        PartiallySignedTransaction psbt{trusted_template.psbt};
        const CMutableTransaction transaction{*psbt.tx};
        PrecomputedTransactionData txdata;
        txdata.Init(transaction, TemplatePrevouts(trusted_template),
                    /*force=*/true);
        for (size_t index = 0; index < psbt.inputs.size(); ++index) {
            const InputRole role{trusted_template.input_roles[index]};
            if (role != InputRole::USER_DD && role != InputRole::USER_DGB) {
                continue;
            }
            psbt.inputs[index].final_script_witness.stack = {
                InputSignature(transaction, index, user_key, txdata)};
        }
        return psbt;
    }

    CMutableTransaction FullySignedProviderTransaction() const
    {
        CMutableTransaction transaction{*trusted_template.psbt.tx};
        PrecomputedTransactionData txdata;
        txdata.Init(transaction, TemplatePrevouts(trusted_template),
                    /*force=*/true);
        for (size_t index = 0; index < trusted_template.input_roles.size();
             ++index) {
            const InputRole role{trusted_template.input_roles[index]};
            const CKey* signing_key{nullptr};
            if (role == InputRole::USER_DD || role == InputRole::USER_DGB) {
                signing_key = &user_key;
            } else if (role == InputRole::PROVIDER_DGB) {
                signing_key = &provider_dgb_key;
            }
            assert(signing_key != nullptr);
            transaction.vin[index].scriptWitness.stack = {
                InputSignature(transaction, index, *signing_key, txdata)};
        }
        return transaction;
    }

    PaymasterResultMessage FinalResultMessage() const
    {
        const CMutableTransaction final_transaction{
            FullySignedProviderTransaction()};
        const CTransaction final{final_transaction};
        PaymasterResultMessage message;
        message.request_id = intent.request_id;
        message.session_id = intent.session_id;
        message.result.genesis_hash = intent.genesis_hash;
        message.result.provider_id = intent.provider_id;
        message.result.commit_key = submit.commit_key;
        message.result.result_sequence = 1;
        message.result.status = PaymasterResultStatus::FINAL_COMMITTED;
        message.result.txid = final.GetHash();
        message.result.raw_transaction_hash = final.GetWitnessHash();
        message.result.final_transaction = final_transaction;
        message.result.updated_at = NOW + 5;
        SignIdentity(provider_identity_key,
                     GetPaymasterResultSignatureHash(message.result),
                     message.result.identity_signature);
        return message;
    }

    PartiallySignedTransaction UserSignedRecoveryPSBT(
        const AlternativeRecoveryTemplate& recovery) const
    {
        PartiallySignedTransaction psbt{recovery.trusted_template.psbt};
        const CMutableTransaction transaction{*psbt.tx};
        PrecomputedTransactionData txdata;
        txdata.Init(transaction, TemplatePrevouts(recovery.trusted_template),
                    /*force=*/true);
        for (size_t index = 0; index < psbt.inputs.size(); ++index) {
            if (recovery.trusted_template.input_roles[index] !=
                InputRole::USER_DD) {
                continue;
            }
            psbt.inputs[index].final_script_witness.stack = {
                InputSignature(transaction, index, user_key, txdata)};
        }
        return psbt;
    }

    CMutableTransaction FullySignedRecoveryTransaction(
        const AlternativeRecoveryTemplate& recovery) const
    {
        CMutableTransaction transaction{*recovery.trusted_template.psbt.tx};
        PrecomputedTransactionData txdata;
        txdata.Init(transaction, TemplatePrevouts(recovery.trusted_template),
                    /*force=*/true);
        for (size_t index = 0;
             index < recovery.trusted_template.input_roles.size(); ++index) {
            const InputRole role{recovery.trusted_template.input_roles[index]};
            const CKey* signing_key{nullptr};
            if (role == InputRole::USER_DD || role == InputRole::USER_DGB) {
                signing_key = &user_key;
            } else if (role == InputRole::PROVIDER_DGB) {
                signing_key = &recovery_dgb_key;
            }
            assert(signing_key != nullptr);
            transaction.vin[index].scriptWitness.stack = {
                InputSignature(transaction, index, *signing_key, txdata)};
        }
        return transaction;
    }

    AlternativeRecoverySubmit RecoverySubmit(
        const AlternativeRecoveryResponse& response,
        const AlternativeRecoveryTemplate& recovery) const
    {
        AlternativeRecoverySubmit submit;
        submit.genesis_hash = response.genesis_hash;
        submit.request_id = response.request_id;
        submit.session_id = response.session_id;
        submit.recovery_id = response.recovery_id;
        submit.recovery_provider_id = response.recovery_provider_id;
        submit.recovery_request_hash = response.recovery_request_hash;
        submit.recovery_commit_key = response.recovery_commit_key;
        submit.template_commitment = response.manifest.template_commitment;
        submit.user_psbt = SerializeExact(UserSignedRecoveryPSBT(recovery));
        return submit;
    }

    AlternativeRecoveryResultMessage RecoveryResult(
        const AlternativeRecoveryResponse& response,
        const AlternativeRecoveryTemplate& recovery) const
    {
        const CMutableTransaction final_transaction{
            FullySignedRecoveryTransaction(recovery)};
        const CTransaction final{final_transaction};
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
        message.result.txid = final.GetHash();
        message.result.raw_transaction_hash = final.GetWitnessHash();
        message.result.final_transaction = final_transaction;
        message.result.updated_at = NOW;
        SignIdentity(
            recovery_identity_key,
            GetPaymasterResultSignatureHash(message.result),
            message.result.identity_signature);
        return message;
    }

    void ValidateBaseline() const
    {
        std::string error;
        assert(ValidateCapacityProof(
            capacity.proof, capacity.request, intent.genesis_hash,
            XOnlyPubKey{provider_identity_key.GetPubKey()},
            CapacityCallbacks(verified_provider_dgb, provider_dgb_key), NOW,
            error));
        assert(ValidatePaymentIntent(
            intent, intent.genesis_hash, NOW,
            {BIP86OutputKey(user_key)}, error));
        assert(ValidateQuoteRequestEnvelope(
            quote_request, intent.genesis_hash, NOW, error));
        assert(ValidatePaymasterQuoteForClient(
            quote, intent, offer,
            XOnlyPubKey{provider_identity_key.GetPubKey()}, DDCents{0}, NOW,
            error));
        assert(ValidateQuoteResponseEnvelope(
            quote_response, intent.genesis_hash, NOW, error));
        assert(ValidateClientAuthorizationManifest(
            client_manifest, intent, quote, capacity.snapshot,
            trusted_template, error));
        assert(ValidateProviderAuthorizationManifest(
            provider_manifest, intent, quote, trusted_template, error));
        assert(ValidateCollaborativePSBT(
            UserSignedProviderPSBT(), trusted_template,
            CollaborativeSignatureStage::USER_SIGNED, error));
        assert(ValidateSubmitEnvelope(submit, intent.genesis_hash, error));
        assert(ValidatePaymasterResult(
            result, intent.genesis_hash, intent.provider_id,
            submit.commit_key,
            XOnlyPubKey{provider_identity_key.GetPubKey()}, 1, error));
        assert(ValidateResultMessageEnvelope(
            result_message, intent.genesis_hash, NOW, error));
        const PaymasterResultMessage final_message{FinalResultMessage()};
        assert(ValidateResultMessageEnvelope(
            final_message, intent.genesis_hash, NOW, error));
        assert(ValidatePaymasterResult(
            final_message.result, intent.genesis_hash, intent.provider_id,
            submit.commit_key,
            XOnlyPubKey{provider_identity_key.GetPubKey()}, 1, error));
        assert(ValidateFinalCollaborativeTransaction(
            *final_message.result.final_transaction,
            *final_message.result.txid,
            *final_message.result.raw_transaction_hash, trusted_template,
            error));
    }
};

enum class Phase : uint8_t {
    START,
    CAPACITY,
    INTENT,
    QUOTE,
    CLIENT_ACCEPTED,
    SUBMIT,
    RESULT,
    RECOVERY,
};

bool HasExactDDOutput(const CTransaction& tx, const CScript& script,
                      CAmount expected_amount)
{
    size_t matches{0};
    for (uint32_t index = 0; index < tx.vout.size(); ++index) {
        CAmount amount{0};
        if (DigiDollar::ExtractDDAmountFromTransaction(
                tx, COutPoint{tx.GetHash(), index}, amount) &&
            tx.vout[index].scriptPubKey == script &&
            amount == expected_amount) {
            ++matches;
        }
    }
    return matches == 1;
}

bool ClientOutflowsMatchManifest(const StatefulArtifacts& artifacts)
{
    std::string error;
    if (!ValidateClientAuthorizationManifest(
            artifacts.client_manifest, artifacts.intent, artifacts.quote,
            artifacts.capacity.snapshot, artifacts.trusted_template, error) ||
        !ValidateCollaborativePSBT(
            artifacts.trusted_template.psbt, artifacts.trusted_template,
            CollaborativeSignatureStage::UNSIGNED, error) ||
        !artifacts.trusted_template.psbt.tx) {
        return false;
    }
    const auto& roles = artifacts.trusted_template.input_roles;
    if (roles.empty() || roles.front() != InputRole::USER_DD ||
        std::find(roles.begin(), roles.end(), InputRole::USER_DGB) !=
            roles.end()) {
        return false;
    }
    CAmount user_input{0};
    if (!DigiDollar::ExtractDDAmountFromTransaction(
            *artifacts.user_dd, artifacts.intent.user_dd_inputs.front(),
            user_input)) {
        return false;
    }
    const CAmount change =
        user_input - artifacts.client_manifest.recipient_amount.value -
        artifacts.client_manifest.service_fee.value;
    const CTransaction tx{*artifacts.trusted_template.psbt.tx};
    return change >= 0 &&
           HasExactDDOutput(
               tx, artifacts.client_manifest.recipient_script,
               artifacts.client_manifest.recipient_amount.value) &&
           HasExactDDOutput(
               tx, artifacts.client_manifest.user_dd_change_script, change);
}

bool ProviderOutflowsMatchManifest(const StatefulArtifacts& artifacts)
{
    std::string error;
    if (!ValidateProviderAuthorizationManifest(
            artifacts.provider_manifest, artifacts.intent, artifacts.quote,
            artifacts.trusted_template, error) ||
        !artifacts.trusted_template.psbt.tx) {
        return false;
    }
    CAmount dgb_in{0};
    for (const VerifiedDGBInput& input : artifacts.quote.reserved_dgb_inputs) {
        dgb_in += input.value.value;
    }
    CAmount dgb_out{0};
    for (const CTxOut& output : artifacts.trusted_template.psbt.tx->vout) {
        if (output.nValue > 0) dgb_out += output.nValue;
    }
    return dgb_in >= dgb_out &&
           dgb_in - dgb_out == artifacts.provider_manifest.network_fee.value &&
           artifacts.provider_manifest.provider_dgb_inputs ==
               std::vector<COutPoint>{artifacts.verified_provider_dgb.outpoint};
}

bool RecoveryOutflowsMatchManifest(
    const StatefulArtifacts& artifacts,
    const AlternativeRecoveryParameters& parameters,
    const AlternativeRecoveryTemplate& recovery)
{
    std::string error;
    if (!ValidateAlternativeRecoveryTemplate(
            recovery, parameters, Params(), artifacts.coins,
            artifacts.CapacityCallbacks(
                artifacts.verified_recovery_dgb,
                artifacts.recovery_dgb_key),
            NOW, error) ||
        !recovery.trusted_template.psbt.tx ||
        recovery.manifest.original_provider_id ==
            recovery.manifest.recovery_provider_id ||
        recovery.manifest.user_dd_inputs != artifacts.intent.user_dd_inputs ||
        std::find(recovery.trusted_template.input_roles.begin(),
                  recovery.trusted_template.input_roles.end(),
                  InputRole::USER_DGB) !=
            recovery.trusted_template.input_roles.end()) {
        return false;
    }
    CAmount returned{0};
    for (const AlternativeRecoveryReturn& output :
         recovery.manifest.wallet_returns) {
        if (!HasExactDDOutput(
                CTransaction{*recovery.trusted_template.psbt.tx},
                output.script_pub_key, output.amount.value)) {
            return false;
        }
        returned += output.amount.value;
    }
    return returned + recovery.manifest.service_fee.value == 1000;
}

class StatefulMachine
{
private:
    StatefulArtifacts& m_artifacts;
    SemanticReplayCache m_replays;
    SemanticReplayCache m_replay_probes;
    Phase m_phase{Phase::START};
    bool m_final_committed{false};

    bool CanApply(Phase required, Phase completed) const
    {
        return m_phase == required ||
               static_cast<uint8_t>(m_phase) >=
                   static_cast<uint8_t>(completed);
    }

    bool Finish(Phase required, Phase completed, const std::string& key,
                const uint256& content)
    {
        if (!CanApply(required, completed) ||
            m_replays.Observe(key, content) ==
                ReplayDisposition::CONFLICT) {
            return false;
        }
        if (m_phase == required) m_phase = completed;
        return true;
    }

    std::string CapacityKey() const
    {
        return "capacity:" + m_artifacts.intent.provider_id.GetHex() + ":" +
               m_artifacts.intent.request_id + ":" +
               m_artifacts.intent.session_id.GetHex();
    }

    std::string IntentKey() const
    {
        return "intent:" + m_artifacts.intent.provider_id.GetHex() + ":" +
               m_artifacts.intent.request_id + ":" +
               m_artifacts.intent.session_id.GetHex();
    }

    std::string QuoteKey() const
    {
        return "quote:" + m_artifacts.quote.provider_id.GetHex() + ":" +
               m_artifacts.quote.quote_id.GetHex();
    }

    std::string SubmitKey() const
    {
        return "submit:" + m_artifacts.submit.provider_id.GetHex() + ":" +
               m_artifacts.submit.commit_key.GetHex();
    }

    std::string ClientAcceptanceKey() const
    {
        return "client-acceptance:" +
               m_artifacts.client_manifest.provider_id.GetHex() + ":" +
               m_artifacts.submit.commit_key.GetHex() + ":" +
               m_artifacts.client_manifest.manifest_id.GetHex();
    }

    std::string ResultKey() const
    {
        return "result:" + m_artifacts.result.provider_id.GetHex() + ":" +
               m_artifacts.result.commit_key.GetHex() + ":" +
               std::to_string(m_artifacts.result.result_sequence);
    }

    std::string FinalResultKey(const PaymasterResult& result) const
    {
        return "result:" + result.provider_id.GetHex() + ":" +
               result.commit_key.GetHex() + ":" +
               std::to_string(result.result_sequence);
    }

    std::string RecoveryKey(const AlternativeRecoveryManifest& manifest) const
    {
        return "recovery:" + manifest.original_commit_key.GetHex() + ":" +
               manifest.recovery_provider_id.GetHex();
    }

public:
    explicit StatefulMachine(StatefulArtifacts& artifacts)
        : m_artifacts{artifacts}
    {
    }

    Phase CurrentPhase() const { return m_phase; }

    bool Capacity(bool mutate, uint8_t variant)
    {
        if (!CanApply(Phase::START, Phase::CAPACITY)) return false;
        PaymasterCapacityRequest request{m_artifacts.capacity.request};
        PaymasterCapacityProof proof{m_artifacts.capacity.proof};
        if (mutate) {
            switch (variant % 6) {
            case 0: request.client_nonce = uint256S("201"); break;
            case 1: proof.identity_signature.front() ^= 1; break;
            case 2: proof.expires_at = SaturatingAddSeconds(NOW, 61); break;
            case 3:
                proof.liquidity_slots.front().dgb_inputs.front().input.value.value++;
                break;
            case 4: proof.funding_model = FundingModel::USER_PAID; break;
            case 5: proof.requires_carrier = true; break;
            }
        }
        std::string error;
        const bool valid = ValidateCapacityProof(
            proof, request, m_artifacts.intent.genesis_hash,
            XOnlyPubKey{m_artifacts.provider_identity_key.GetPubKey()},
            m_artifacts.CapacityCallbacks(
                m_artifacts.verified_provider_dgb,
                m_artifacts.provider_dgb_key),
            NOW, error);
        if (mutate) assert(!valid);
        return valid && Finish(
                            Phase::START, Phase::CAPACITY, CapacityKey(),
                            CanonicalHash(request, proof));
    }

    bool Intent(bool mutate, uint8_t variant)
    {
        if (!CanApply(Phase::CAPACITY, Phase::INTENT)) return false;
        PaymasterQuoteRequest request{m_artifacts.quote_request};
        if (mutate) {
            switch (variant % 4) {
            case 0: request.intent.user_input_proofs.front().signature.front() ^= 1; break;
            case 1: request.intent.expires_at = NOW; break;
            case 2: request.intent.provider_id = uint256S("202"); break;
            case 3:
                request.intent.funding_model =
                    static_cast<FundingModel>(0xff);
                break;
            }
        }
        std::string error;
        const bool valid = ValidateQuoteRequestEnvelope(
                               request, m_artifacts.intent.genesis_hash, NOW,
                               error) &&
                           ValidatePaymentIntent(
                               request.intent,
                               m_artifacts.intent.genesis_hash, NOW,
                               {BIP86OutputKey(m_artifacts.user_key)}, error);
        if (mutate) assert(!valid);
        return valid && Finish(
                            Phase::CAPACITY, Phase::INTENT, IntentKey(),
                            CanonicalHash(request));
    }

    bool Quote(bool mutate, uint8_t variant)
    {
        if (!CanApply(Phase::INTENT, Phase::QUOTE)) return false;
        PaymasterQuoteResponse response{m_artifacts.quote_response};
        if (mutate) {
            switch (variant % 4) {
            case 0: response.quote.identity_signature.front() ^= 1; break;
            case 1: response.quote.network_fee.value++; break;
            case 2: response.quote.template_commitment = uint256S("203"); break;
            case 3: response.quote.provider_id = uint256S("204"); break;
            }
        }
        std::string error;
        const bool valid = ValidateQuoteResponseEnvelope(
                               response, m_artifacts.intent.genesis_hash, NOW,
                               error) &&
                           ValidatePaymasterQuoteForClient(
                               response.quote, m_artifacts.intent,
                               m_artifacts.offer,
                               XOnlyPubKey{
                                   m_artifacts.provider_identity_key.GetPubKey()},
                               DDCents{0}, NOW, error);
        if (mutate) assert(!valid);
        if (valid) {
            assert(CanTransition(AttemptState::CANDIDATE,
                                 AttemptState::QUOTED));
            assert(!CanTransition(AttemptState::CANDIDATE,
                                  AttemptState::USER_SIGNED));
        }
        return valid && Finish(
                            Phase::INTENT, Phase::QUOTE, QuoteKey(),
                            CanonicalHash(response));
    }

    bool ClientAcceptance(bool mutate, uint8_t variant)
    {
        if (!CanApply(Phase::QUOTE, Phase::CLIENT_ACCEPTED)) return false;
        ClientAuthorizationManifest manifest{m_artifacts.client_manifest};
        if (mutate) {
            switch (variant % 16) {
            case 0: manifest.manifest_id.SetNull(); break;
            case 1: manifest.provider_id = uint256S("301"); break;
            case 2: manifest.offer_id = uint256S("302"); break;
            case 3: manifest.capacity_snapshot_id = uint256S("303"); break;
            case 4: manifest.recipient_amount.value++; break;
            case 5: manifest.maximum_service_fee.value++; break;
            case 6: manifest.template_commitment = uint256S("304"); break;
            case 7: manifest.capacity_proof_hash = uint256S("305"); break;
            case 8: manifest.canonical_request_hash = uint256S("306"); break;
            case 9: manifest.requested_fee_mode = FeeMode::AUTO; break;
            case 10: manifest.privacy_profile = PrivacyProfile::HIGH; break;
            case 11:
                manifest.selection_mode = SelectionMode::PRIVACY_WEIGHTED;
                break;
            case 12: manifest.user_dd_inputs.front().n++; break;
            case 13:
                manifest.user_dd_change_script.push_back(OP_TRUE);
                break;
            case 14: manifest.service_fee.value++; break;
            case 15: manifest.funding_model = FundingModel::USER_PAID; break;
            }
            if (variant % 16 != 0) {
                // Exercise a self-consistent malicious commitment as well as
                // the trivial stale-id case. Local request/quote/capacity
                // authority must still reject it.
                manifest.manifest_id =
                    GetClientAuthorizationManifestId(manifest);
            }
        }
        std::string error;
        const bool valid = ValidateClientAuthorizationManifest(
                               manifest, m_artifacts.intent,
                               m_artifacts.quote,
                               m_artifacts.capacity.snapshot,
                               m_artifacts.trusted_template, error) &&
                           manifest.manifest_id ==
                               m_artifacts.client_manifest.manifest_id &&
                           !manifest.manifest_id.IsNull();
        if (mutate) assert(!valid);
        if (valid) {
            const uint256 accepted_manifest_id{manifest.manifest_id};
            const int64_t accepted_at{NOW};
            assert(accepted_manifest_id ==
                   m_artifacts.client_manifest.manifest_id);
            assert(accepted_at > 0 &&
                   accepted_at < m_artifacts.client_manifest.expires_at);
            assert(CanTransition(AttemptState::QUOTED,
                                 AttemptState::USER_SIGNED));
            assert(!CanTransition(AttemptState::CANDIDATE,
                                  AttemptState::USER_SIGNED));
        }
        return valid && Finish(
                            Phase::QUOTE, Phase::CLIENT_ACCEPTED,
                            ClientAcceptanceKey(), CanonicalHash(manifest));
    }

    bool Submit(bool mutate, uint8_t variant)
    {
        if (!CanApply(Phase::CLIENT_ACCEPTED, Phase::SUBMIT)) return false;
        PaymasterSubmit submit{m_artifacts.submit};
        if (mutate) {
            switch (variant % 4) {
            case 0: submit.user_psbt.clear(); break;
            case 1: submit.commit_key = uint256{}; break;
            case 2: submit.template_commitment = uint256S("205"); break;
            case 3: submit.user_psbt.push_back(0); break;
            }
        }
        std::string error;
        const PartiallySignedTransaction canonical_user_psbt{
            m_artifacts.UserSignedProviderPSBT()};
        const bool exact_binding =
            submit.provider_id == m_artifacts.intent.provider_id &&
            submit.request_id == m_artifacts.intent.request_id &&
            submit.session_id == m_artifacts.intent.session_id &&
            submit.quote_id == m_artifacts.quote.quote_id &&
            submit.commit_key == m_artifacts.submit.commit_key &&
            submit.template_commitment ==
                m_artifacts.quote.template_commitment &&
            submit.user_psbt == SerializeExact(canonical_user_psbt);
        const bool valid = ValidateSubmitEnvelope(
                               submit, m_artifacts.intent.genesis_hash,
                               error) &&
                           exact_binding &&
                           ValidateClientAuthorizationManifest(
                               m_artifacts.client_manifest,
                               m_artifacts.intent, m_artifacts.quote,
                               m_artifacts.capacity.snapshot,
                               m_artifacts.trusted_template, error) &&
                           ValidateProviderAuthorizationManifest(
                               m_artifacts.provider_manifest,
                               m_artifacts.intent, m_artifacts.quote,
                               m_artifacts.trusted_template, error) &&
                           ValidateCollaborativePSBT(
                               canonical_user_psbt,
                               m_artifacts.trusted_template,
                               CollaborativeSignatureStage::USER_SIGNED,
                               error);
        if (mutate) assert(!valid);
        if (valid) {
            assert(ClientOutflowsMatchManifest(m_artifacts));
            assert(ProviderOutflowsMatchManifest(m_artifacts));
            assert(CanTransition(AttemptState::QUOTED,
                                 AttemptState::USER_SIGNED));
            assert(CanTransition(AttemptState::USER_SIGNED,
                                 AttemptState::USER_PSBT_ACCEPTED));
            assert(!CanTransition(AttemptState::QUOTED,
                                  AttemptState::PROVIDER_SIGNED));
        }
        return valid && Finish(
                            Phase::CLIENT_ACCEPTED, Phase::SUBMIT, SubmitKey(),
                            CanonicalHash(submit));
    }

    bool Result(bool mutate, uint8_t variant)
    {
        if (m_final_committed ||
            !CanApply(Phase::SUBMIT, Phase::RESULT)) {
            return false;
        }
        PaymasterResultMessage message{m_artifacts.result_message};
        if (mutate) {
            switch (variant % 4) {
            case 0: message.result.identity_signature.front() ^= 1; break;
            case 1: message.result.result_sequence = 0; break;
            case 2:
                message.result.status =
                    static_cast<PaymasterResultStatus>(0xff);
                break;
            case 3: message.result.commit_key = uint256S("206"); break;
            }
        }
        std::string error;
        const bool valid = ValidateResultMessageEnvelope(
                               message, m_artifacts.intent.genesis_hash, NOW,
                               error) &&
                           ValidatePaymasterResult(
                               message.result,
                               m_artifacts.intent.genesis_hash,
                               m_artifacts.intent.provider_id,
                               m_artifacts.submit.commit_key,
                               XOnlyPubKey{
                                   m_artifacts.provider_identity_key.GetPubKey()},
                               1, error);
        if (mutate) assert(!valid);
        return valid && Finish(
                            Phase::SUBMIT, Phase::RESULT, ResultKey(),
                            CanonicalHash(message));
    }

    bool FinalResult(bool mutate, uint8_t variant)
    {
        const bool exact_replay{
            m_phase == Phase::RESULT && m_final_committed};
        if (m_phase != Phase::SUBMIT && !exact_replay) return false;

        PaymasterResultMessage message{m_artifacts.FinalResultMessage()};
        ClientAuthorizationManifest client_manifest{
            m_artifacts.client_manifest};
        ProviderAuthorizationManifest provider_manifest{
            m_artifacts.provider_manifest};
        if (mutate) {
            switch (variant % 10) {
            case 0:
                message.result.identity_signature.front() ^= 1;
                break;
            case 1:
                message.result.txid = uint256S("701");
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            case 2:
                message.result.raw_transaction_hash = uint256S("702");
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            case 3: {
                CMutableTransaction& final{
                    *message.result.final_transaction};
                assert(!final.vin.front().scriptWitness.stack.empty());
                final.vin.front().scriptWitness.stack.front().front() ^= 1;
                message.result.raw_transaction_hash =
                    CTransaction{final}.GetWitnessHash();
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            }
            case 4: {
                CMutableTransaction& final{
                    *message.result.final_transaction};
                assert(!final.vout.empty());
                ++final.vout.front().nValue;
                const CTransaction transaction{final};
                message.result.txid = transaction.GetHash();
                message.result.raw_transaction_hash =
                    transaction.GetWitnessHash();
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            }
            case 5:
                ++client_manifest.recipient_amount.value;
                client_manifest.manifest_id =
                    GetClientAuthorizationManifestId(client_manifest);
                break;
            case 6:
                ++provider_manifest.network_fee.value;
                provider_manifest.manifest_id =
                    GetProviderAuthorizationManifestId(provider_manifest);
                break;
            case 7:
                message.result.final_transaction.reset();
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            case 8:
                message.result.status =
                    PaymasterResultStatus::BROADCAST_ATTEMPTED;
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            case 9:
                message.result.result_sequence = 2;
                ++message.result.updated_at;
                SignIdentity(
                    m_artifacts.provider_identity_key,
                    GetPaymasterResultSignatureHash(message.result),
                    message.result.identity_signature);
                break;
            }
        }

        std::string error;
        const PartiallySignedTransaction user_psbt{
            m_artifacts.UserSignedProviderPSBT()};
        const bool exact_result =
            message.result.status == PaymasterResultStatus::FINAL_COMMITTED &&
            message.result.result_sequence == 1 &&
            message.result.txid && message.result.raw_transaction_hash &&
            message.result.final_transaction;
        const bool valid =
            exact_result &&
            ValidateResultMessageEnvelope(
                message, m_artifacts.intent.genesis_hash, NOW, error) &&
            ValidatePaymasterResult(
                message.result, m_artifacts.intent.genesis_hash,
                m_artifacts.intent.provider_id,
                m_artifacts.submit.commit_key,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                1, error) &&
            ValidateClientAuthorizationManifest(
                client_manifest, m_artifacts.intent, m_artifacts.quote,
                m_artifacts.capacity.snapshot,
                m_artifacts.trusted_template, error) &&
            client_manifest.manifest_id ==
                m_artifacts.client_manifest.manifest_id &&
            ValidateProviderAuthorizationManifest(
                provider_manifest, m_artifacts.intent, m_artifacts.quote,
                m_artifacts.trusted_template, error) &&
            provider_manifest.manifest_id ==
                m_artifacts.provider_manifest.manifest_id &&
            ValidateCollaborativePSBT(
                user_psbt, m_artifacts.trusted_template,
                CollaborativeSignatureStage::USER_SIGNED, error) &&
            ValidateFinalCollaborativeTransaction(
                *message.result.final_transaction,
                *message.result.txid,
                *message.result.raw_transaction_hash,
                m_artifacts.trusted_template, error);
        if (mutate) assert(!valid);
        if (!valid) return false;

        const ReplayDisposition replay{m_replays.Observe(
            FinalResultKey(message.result), CanonicalHash(message))};
        if (replay == ReplayDisposition::CONFLICT) return false;
        if (m_phase == Phase::SUBMIT) {
            m_phase = Phase::RESULT;
            m_final_committed = true;
        }
        assert(ClientOutflowsMatchManifest(m_artifacts));
        assert(ProviderOutflowsMatchManifest(m_artifacts));
        assert(CanTransition(AttemptState::USER_PSBT_ACCEPTED,
                             AttemptState::PROVIDER_SIGNED));
        assert(CanTransition(AttemptState::PROVIDER_SIGNED,
                             AttemptState::FINAL_COMMITTED));
        assert(!CanTransition(AttemptState::FINAL_COMMITTED,
                              AttemptState::REJECTED));
        return true;
    }

    bool Recovery(bool mutate, uint8_t variant)
    {
        if (m_final_committed ||
            !CanApply(Phase::RESULT, Phase::RECOVERY)) {
            return false;
        }
        AlternativeRecoveryParameters parameters{
            m_artifacts.RecoveryParameters()};
        if (mutate && variant % 14 == 0) {
            parameters.original_provider_id =
                parameters.recovery_provider_id;
        }
        AlternativeRecoveryTemplate recovery;
        std::string error;
        const CapacityChainstateCallbacks callbacks{
            m_artifacts.CapacityCallbacks(
                m_artifacts.verified_recovery_dgb,
                m_artifacts.recovery_dgb_key)};
        if (!BuildAlternativeRecoveryTemplate(
                parameters, Params(), m_artifacts.coins, callbacks, NOW,
                recovery, error)) {
            assert(mutate);
            return false;
        }

        AlternativeRecoveryRequest request{
            m_artifacts.RecoveryRequest(parameters)};
        AlternativeRecoveryResponse response{
            m_artifacts.RecoveryResponse(parameters, request, recovery)};
        RecoveryAuthorizationManifest authorization;
        assert(BuildRecoveryAuthorizationManifest(
            request, response, authorization, error));
        AlternativeRecoverySubmit submit{
            m_artifacts.RecoverySubmit(response, recovery)};
        AlternativeRecoveryResultMessage result{
            m_artifacts.RecoveryResult(response, recovery)};

        if (mutate) {
            switch (variant % 14) {
            case 0: break; // Rejected by the template builder above.
            case 1:
                request.user_input_proofs.front().signature.front() ^= 1;
                break;
            case 2: response.identity_signature.front() ^= 1; break;
            case 3: authorization.authorization_commitment = uint256S("401"); break;
            case 4: submit.recovery_commit_key = uint256S("402"); break;
            case 5: result.result.result_sequence = 0; break;
            case 6:
                response.unsigned_psbt.push_back(0);
                response.recovery_commit_key =
                    GetAlternativeRecoveryCommitKey(response);
                SignIdentity(
                    m_artifacts.recovery_identity_key,
                    GetAlternativeRecoveryResponseSignatureHash(response),
                    response.identity_signature);
                break;
            case 7:
                request.privacy_profile = PrivacyProfile::HIGH;
                SignBIP86(
                    m_artifacts.user_key,
                    GetAlternativeRecoveryInputControlHash(
                        request, request.user_dd_inputs.front()),
                    request.user_input_proofs.front().signature);
                break;
            case 8:
                result.result.commit_key = uint256S("403");
                SignIdentity(
                    m_artifacts.recovery_identity_key,
                    GetPaymasterResultSignatureHash(result.result),
                    result.result.identity_signature);
                break;
            case 9:
                authorization.service_fee.value++;
                authorization.authorization_commitment =
                    GetRecoveryAuthorizationCommitment(authorization);
                break;
            case 10: {
                CMutableTransaction& final{
                    *result.result.final_transaction};
                assert(!final.vin.front().scriptWitness.stack.empty());
                final.vin.front().scriptWitness.stack.front().front() ^= 1;
                result.result.raw_transaction_hash =
                    CTransaction{final}.GetWitnessHash();
                SignIdentity(
                    m_artifacts.recovery_identity_key,
                    GetPaymasterResultSignatureHash(result.result),
                    result.result.identity_signature);
                break;
            }
            case 11: {
                CMutableTransaction& final{
                    *result.result.final_transaction};
                assert(!final.vout.empty());
                ++final.vout.front().nValue;
                const CTransaction transaction{final};
                result.result.txid = transaction.GetHash();
                result.result.raw_transaction_hash =
                    transaction.GetWitnessHash();
                SignIdentity(
                    m_artifacts.recovery_identity_key,
                    GetPaymasterResultSignatureHash(result.result),
                    result.result.identity_signature);
                break;
            }
            case 12:
                result.result.raw_transaction_hash = uint256S("404");
                SignIdentity(
                    m_artifacts.recovery_identity_key,
                    GetPaymasterResultSignatureHash(result.result),
                    result.result.identity_signature);
                break;
            case 13:
                result.result.final_transaction.reset();
                SignIdentity(
                    m_artifacts.recovery_identity_key,
                    GetPaymasterResultSignatureHash(result.result),
                    result.result.identity_signature);
                break;
            }
        }

        AlternativeRecoveryTemplate validated_recovery;
        PartiallySignedTransaction unsigned_psbt;
        PartiallySignedTransaction user_psbt;
        const bool valid =
            ValidateAlternativeRecoveryRequestEnvelope(
                request, parameters.genesis_hash, NOW, error) &&
            ValidateAlternativeRecoveryRequestInputs(
                request, {BIP86OutputKey(m_artifacts.user_key)}, error) &&
            ValidateAlternativeRecoveryResponseEnvelope(
                response, parameters.genesis_hash, NOW, error) &&
            ValidateAlternativeRecoveryResponseTemplate(
                response, request,
                XOnlyPubKey{m_artifacts.recovery_identity_key.GetPubKey()},
                parameters, Params(), m_artifacts.coins, callbacks, NOW,
                validated_recovery, unsigned_psbt, error) &&
            ValidateRecoveryAuthorizationManifest(
                authorization, request, response, NOW, error) &&
            authorization.authorization_commitment ==
                GetRecoveryAuthorizationCommitment(authorization) &&
            ValidateAlternativeRecoverySubmit(
                submit, response, recovery, parameters, Params(),
                m_artifacts.coins, callbacks, NOW, user_psbt, error) &&
            ValidateAlternativeRecoveryResult(
                result, response,
                XOnlyPubKey{m_artifacts.recovery_identity_key.GetPubKey()},
                parameters.genesis_hash, 1, NOW, error) &&
            result.result.status ==
                PaymasterResultStatus::FINAL_COMMITTED &&
            result.result.txid && result.result.raw_transaction_hash &&
            result.result.final_transaction &&
            ValidateFinalAlternativeRecoveryTransaction(
                *result.result.final_transaction,
                *result.result.raw_transaction_hash, recovery, parameters,
                Params(), m_artifacts.coins, callbacks, NOW,
                /*exact_final_already_known=*/false, error);
        if (mutate) assert(!valid);
        if (valid) {
            assert(RecoveryOutflowsMatchManifest(
                m_artifacts, parameters, recovery));
            assert(SerializeExact(validated_recovery.manifest) ==
                   SerializeExact(recovery.manifest));
            assert(SerializeExact(unsigned_psbt) ==
                   response.unsigned_psbt);
            assert(authorization.privacy_profile ==
                   parameters.privacy_profile);
            assert(CanTransition(SessionState::AUTHORIZED,
                                 SessionState::PENDING_PROVIDER));
            assert(CanTransition(SessionState::PENDING_PROVIDER,
                                 SessionState::CANCELED_SAFE));
            assert(!CanTransition(SessionState::AUTHORIZED,
                                  SessionState::CANCELED_SAFE));
        }
        return valid && Finish(
                            Phase::RESULT, Phase::RECOVERY,
                            RecoveryKey(recovery.manifest),
                            CanonicalHash(
                                request, response, authorization, submit,
                                result));
    }

    void CheckProtocolTimeBoundaries(uint8_t variant)
    {
        std::string error;
        switch (variant % 6) {
        case 0:
            assert(ValidateCapacityProof(
                m_artifacts.capacity.proof, m_artifacts.capacity.request,
                m_artifacts.intent.genesis_hash,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_provider_dgb,
                    m_artifacts.provider_dgb_key),
                m_artifacts.capacity.proof.expires_at - 1, error));
            assert(!ValidateCapacityProof(
                m_artifacts.capacity.proof, m_artifacts.capacity.request,
                m_artifacts.intent.genesis_hash,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_provider_dgb,
                    m_artifacts.provider_dgb_key),
                m_artifacts.capacity.proof.expires_at, error));
            break;
        case 1:
            assert(ValidatePaymentIntent(
                m_artifacts.intent, m_artifacts.intent.genesis_hash,
                m_artifacts.intent.expires_at - 1,
                {BIP86OutputKey(m_artifacts.user_key)}, error));
            assert(!ValidatePaymentIntent(
                m_artifacts.intent, m_artifacts.intent.genesis_hash,
                m_artifacts.intent.expires_at,
                {BIP86OutputKey(m_artifacts.user_key)}, error));
            break;
        case 2:
            assert(ValidatePaymasterQuoteForClient(
                m_artifacts.quote, m_artifacts.intent, m_artifacts.offer,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                DDCents{0}, m_artifacts.quote.expires_at - 1, error));
            assert(!ValidatePaymasterQuoteForClient(
                m_artifacts.quote, m_artifacts.intent, m_artifacts.offer,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                DDCents{0}, m_artifacts.quote.expires_at, error));
            break;
        case 3: {
            const AlternativeRecoveryParameters parameters{
                m_artifacts.RecoveryParameters()};
            AlternativeRecoveryTemplate recovery;
            const CapacityChainstateCallbacks callbacks{
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_recovery_dgb,
                    m_artifacts.recovery_dgb_key)};
            assert(BuildAlternativeRecoveryTemplate(
                parameters, Params(), m_artifacts.coins, callbacks, NOW,
                recovery, error));
            const AlternativeRecoveryRequest request{
                m_artifacts.RecoveryRequest(parameters)};
            const AlternativeRecoveryResponse response{
                m_artifacts.RecoveryResponse(parameters, request, recovery)};
            assert(ValidateAlternativeRecoveryRequestEnvelope(
                request, parameters.genesis_hash, request.expires_at - 1,
                error));
            assert(!ValidateAlternativeRecoveryRequestEnvelope(
                request, parameters.genesis_hash, request.expires_at,
                error));
            assert(ValidateAlternativeRecoveryResponseEnvelope(
                response, parameters.genesis_hash,
                response.expires_at - 1, error));
            assert(!ValidateAlternativeRecoveryResponseEnvelope(
                response, parameters.genesis_hash, response.expires_at,
                error));
            break;
        }
        case 4: {
            const AlternativeRecoveryParameters parameters{
                m_artifacts.RecoveryParameters()};
            AlternativeRecoveryTemplate recovery;
            const CapacityChainstateCallbacks callbacks{
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_recovery_dgb,
                    m_artifacts.recovery_dgb_key)};
            assert(BuildAlternativeRecoveryTemplate(
                parameters, Params(), m_artifacts.coins, callbacks, NOW,
                recovery, error));
            const AlternativeRecoveryRequest request{
                m_artifacts.RecoveryRequest(parameters)};
            const AlternativeRecoveryResponse response{
                m_artifacts.RecoveryResponse(parameters, request, recovery)};
            const AlternativeRecoveryResultMessage result{
                m_artifacts.RecoveryResult(response, recovery)};
            assert(result.result.final_transaction &&
                   result.result.raw_transaction_hash);
            assert(ValidateFinalAlternativeRecoveryTransaction(
                *result.result.final_transaction,
                *result.result.raw_transaction_hash, recovery, parameters,
                Params(), m_artifacts.coins, callbacks,
                parameters.expires_at - 1,
                /*exact_final_already_known=*/false, error));
            assert(!ValidateFinalAlternativeRecoveryTransaction(
                *result.result.final_transaction,
                *result.result.raw_transaction_hash, recovery, parameters,
                Params(), m_artifacts.coins, callbacks,
                parameters.expires_at,
                /*exact_final_already_known=*/false, error));
            assert(ValidateFinalAlternativeRecoveryTransaction(
                *result.result.final_transaction,
                *result.result.raw_transaction_hash, recovery, parameters,
                Params(), m_artifacts.coins, callbacks,
                parameters.expires_at,
                /*exact_final_already_known=*/true, error));
            break;
        }
        case 5: {
            const int64_t earliest_allowed{
                m_artifacts.result.updated_at - 60};
            assert(ValidateResultMessageEnvelope(
                m_artifacts.result_message,
                m_artifacts.intent.genesis_hash, earliest_allowed, error));
            assert(!ValidateResultMessageEnvelope(
                m_artifacts.result_message,
                m_artifacts.intent.genesis_hash, earliest_allowed - 1,
                error));
            break;
        }
        }
    }

    void CheckEquivocation(uint8_t variant)
    {
        std::string error;
        EquivocationKind kind;
        uint256 semantic_key;
        std::vector<unsigned char> first_artifact;
        std::vector<unsigned char> second_artifact;

        if (variant % 2 == 0) {
            const CapacityArtifacts second_capacity{
                m_artifacts.MakeCapacity(
                    m_artifacts.provider_identity_key,
                    m_artifacts.provider_dgb_key,
                    m_artifacts.verified_provider_dgb,
                    m_artifacts.intent.provider_id, uint256S("601"),
                    uint256S("602"), FundingModel::SPONSORED, false)};
            assert(ValidateCapacityProof(
                second_capacity.proof, second_capacity.request,
                m_artifacts.intent.genesis_hash,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_provider_dgb,
                    m_artifacts.provider_dgb_key),
                NOW, error));
            assert(GetCapacityResourceCommitment(
                       m_artifacts.capacity.proof) ==
                   GetCapacityResourceCommitment(second_capacity.proof));
            assert(m_artifacts.capacity.proof.session_id !=
                   second_capacity.proof.session_id);

            HashWriter hasher = TaggedHash(
                "DigiByte Paymaster Capacity Resource v1");
            hasher << m_artifacts.intent.provider_id
                   << m_artifacts.verified_provider_dgb.outpoint;
            semantic_key = hasher.GetSHA256();
            kind = EquivocationKind::CAPACITY;
            first_artifact = SerializeExact(m_artifacts.capacity.proof);
            second_artifact = SerializeExact(second_capacity.proof);
        } else {
            PaymasterQuoteResponse second_quote{
                m_artifacts.quote_response};
            second_quote.quote.created_at++;
            SignIdentity(
                m_artifacts.provider_identity_key,
                GetPaymasterQuoteSignatureHash(second_quote.quote),
                second_quote.quote.identity_signature);
            assert(ValidateQuoteResponseEnvelope(
                second_quote, m_artifacts.intent.genesis_hash, NOW, error));
            assert(ValidatePaymasterQuoteForClient(
                second_quote.quote, m_artifacts.intent, m_artifacts.offer,
                XOnlyPubKey{
                    m_artifacts.provider_identity_key.GetPubKey()},
                DDCents{0}, NOW, error));

            HashWriter hasher = TaggedHash(
                "DigiByte Paymaster Quote Semantic Binding v1");
            hasher << m_artifacts.intent.provider_id
                   << m_artifacts.intent.session_id
                   << m_artifacts.intent.client_nonce
                   << m_artifacts.quote.intent_hash;
            semantic_key = hasher.GetSHA256();
            kind = EquivocationKind::QUOTE;
            first_artifact = SerializeExact(m_artifacts.quote_response);
            second_artifact = SerializeExact(second_quote);
        }

        assert(first_artifact != second_artifact);
        PaymasterEquivocationEvidence evidence;
        evidence.kind = kind;
        evidence.provider_id = m_artifacts.intent.provider_id;
        evidence.semantic_key = semantic_key;
        evidence.first_artifact_hash = Hash(first_artifact);
        evidence.second_artifact_hash = Hash(second_artifact);
        evidence.first_artifact = first_artifact;
        evidence.second_artifact = second_artifact;
        evidence.observed_at = NOW;
        evidence.evidence_id = GetEquivocationEvidenceId(
            evidence.kind, evidence.provider_id, evidence.semantic_key,
            evidence.first_artifact_hash,
            evidence.second_artifact_hash);
        assert(ValidateEquivocationEvidence(evidence));
        assert(evidence.evidence_id == GetEquivocationEvidenceId(
                                           evidence.kind,
                                           evidence.provider_id,
                                           evidence.semantic_key,
                                           evidence.second_artifact_hash,
                                           evidence.first_artifact_hash));

        PaymasterProviderBlock block;
        block.provider_id = evidence.provider_id;
        block.evidence_id = evidence.evidence_id;
        block.kind = evidence.kind;
        block.blocked_at = NOW;
        assert(ValidateProviderBlock(block));
        assert(block.provider_id == evidence.provider_id &&
               block.evidence_id == evidence.evidence_id &&
               block.kind == evidence.kind);

        PaymasterEquivocationEvidence invalid_evidence{evidence};
        switch ((variant / 2) % 6) {
        case 0:
            invalid_evidence.first_artifact.push_back(0);
            break;
        case 1:
            invalid_evidence.second_artifact_hash =
                invalid_evidence.first_artifact_hash;
            invalid_evidence.evidence_id = GetEquivocationEvidenceId(
                invalid_evidence.kind, invalid_evidence.provider_id,
                invalid_evidence.semantic_key,
                invalid_evidence.first_artifact_hash,
                invalid_evidence.second_artifact_hash);
            break;
        case 2:
            invalid_evidence.semantic_key.SetNull();
            invalid_evidence.evidence_id = GetEquivocationEvidenceId(
                invalid_evidence.kind, invalid_evidence.provider_id,
                invalid_evidence.semantic_key,
                invalid_evidence.first_artifact_hash,
                invalid_evidence.second_artifact_hash);
            break;
        case 3:
            invalid_evidence.provider_id.SetNull();
            invalid_evidence.evidence_id = GetEquivocationEvidenceId(
                invalid_evidence.kind, invalid_evidence.provider_id,
                invalid_evidence.semantic_key,
                invalid_evidence.first_artifact_hash,
                invalid_evidence.second_artifact_hash);
            break;
        case 4:
            invalid_evidence.kind = static_cast<EquivocationKind>(0xff);
            invalid_evidence.evidence_id = GetEquivocationEvidenceId(
                invalid_evidence.kind, invalid_evidence.provider_id,
                invalid_evidence.semantic_key,
                invalid_evidence.first_artifact_hash,
                invalid_evidence.second_artifact_hash);
            break;
        case 5:
            invalid_evidence.evidence_id.SetNull();
            break;
        }
        assert(!ValidateEquivocationEvidence(invalid_evidence));

        PaymasterProviderBlock invalid_block{block};
        switch ((variant / 12) % 4) {
        case 0: invalid_block.provider_id.SetNull(); break;
        case 1: invalid_block.evidence_id.SetNull(); break;
        case 2:
            invalid_block.kind = static_cast<EquivocationKind>(0xff);
            break;
        case 3: invalid_block.blocked_at = 0; break;
        }
        assert(!ValidateProviderBlock(invalid_block));
    }

    void CheckFirewall(uint8_t variant)
    {
        std::string error;
        PartiallySignedTransaction candidate{
            m_artifacts.trusted_template.psbt};
        CollaborativePSBTTemplate trusted{m_artifacts.trusted_template};
        bool rejected{false};
        switch (variant % 16) {
        case 0:
            candidate.unknown[std::vector<unsigned char>{0xfc}] = {0x01};
            rejected = !ValidateCollaborativePSBT(
                candidate, trusted, CollaborativeSignatureStage::UNSIGNED,
                error);
            break;
        case 1:
            candidate.inputs.front().unknown[std::vector<unsigned char>{0xfc}] = {0x01};
            rejected = !ValidateCollaborativePSBT(
                candidate, trusted, CollaborativeSignatureStage::UNSIGNED,
                error);
            break;
        case 2:
            candidate.outputs.front().unknown[std::vector<unsigned char>{0xfc}] = {0x01};
            rejected = !ValidateCollaborativePSBT(
                candidate, trusted, CollaborativeSignatureStage::UNSIGNED,
                error);
            break;
        case 3:
            candidate.inputs.front().sighash_type = SIGHASH_ALL;
            rejected = !ValidateCollaborativePSBT(
                candidate, trusted, CollaborativeSignatureStage::UNSIGNED,
                error);
            break;
        case 4:
            trusted.input_roles.front() = static_cast<InputRole>(0xff);
            rejected = !ValidateCollaborativePSBT(
                candidate, trusted, CollaborativeSignatureStage::UNSIGNED,
                error);
            break;
        case 5: {
            ClientAuthorizationManifest manifest{m_artifacts.client_manifest};
            manifest.recipient_amount.value++;
            rejected = !ValidateClientAuthorizationManifest(
                manifest, m_artifacts.intent, m_artifacts.quote,
                m_artifacts.capacity.snapshot, m_artifacts.trusted_template,
                error);
            break;
        }
        case 6:
        case 7: {
            CollaborativeTransferParams transfer;
            transfer.inputs = {
                {m_artifacts.intent.user_dd_inputs.front(),
                 m_artifacts.user_dd,
                 variant % 16 == 6 ? static_cast<InputRole>(0xff) : InputRole::USER_DD},
                {m_artifacts.verified_provider_dgb.outpoint,
                 MakeTransactionRef(m_artifacts.provider_dgb),
                 InputRole::PROVIDER_DGB}};
            transfer.dd_outputs = {
                {m_artifacts.intent.recipient_script, 900,
                 variant % 16 == 7 ? static_cast<DDOutputRole>(0xff) : DDOutputRole::RECIPIENT},
                {m_artifacts.intent.user_dd_change_script, 100,
                 DDOutputRole::USER_CHANGE}};
            transfer.dgb_outputs.emplace_back(
                PROVIDER_DGB_VALUE - NETWORK_FEE,
                BIP86Script(m_artifacts.provider_change_key));
            transfer.fee_rate = 1;
            const CollaborativeTransferResult built =
                BuildUnsignedCollaborativeTransfer(
                    transfer, Params(), m_artifacts.coins);
            rejected = !built.success;
            break;
        }
        case 8: {
            PaymasterQuote quote{m_artifacts.quote};
            quote.reserved_dgb_inputs.front().outpoint =
                m_artifacts.verified_recovery_dgb.outpoint;
            rejected = !ValidateQuoteAgainstCapacitySnapshot(
                quote, m_artifacts.capacity.snapshot, error);
            break;
        }
        case 9: {
            ProviderAuthorizationManifest manifest{
                m_artifacts.provider_manifest};
            if ((variant & 0x10) != 0) {
                manifest.maximum_network_fee.value++;
            } else {
                manifest.budget_reservation_id = uint256S("451");
            }
            manifest.manifest_id =
                GetProviderAuthorizationManifestId(manifest);
            rejected =
                !ValidateProviderAuthorizationManifest(
                    manifest, m_artifacts.intent, m_artifacts.quote,
                    m_artifacts.trusted_template, error) ||
                manifest.manifest_id !=
                    m_artifacts.provider_manifest.manifest_id;
            break;
        }
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15: {
            ProviderAuthorizationManifest manifest{
                m_artifacts.provider_manifest};
            switch (variant % 16) {
            case 10:
                manifest.safety_policy_hash = uint256S("452");
                break;
            case 11:
                manifest.provider_dgb_inputs.front().n++;
                break;
            case 12:
                manifest.dgb_change_scripts.front().push_back(OP_TRUE);
                break;
            case 13:
                manifest.service_fee.value++;
                break;
            case 14:
                manifest.funding_model = FundingModel::USER_PAID;
                break;
            case 15:
                manifest.sponsorship_scope = SponsorshipScope::RESTRICTED;
                break;
            default: assert(false);
            }
            manifest.manifest_id =
                GetProviderAuthorizationManifestId(manifest);
            rejected =
                !ValidateProviderAuthorizationManifest(
                    manifest, m_artifacts.intent, m_artifacts.quote,
                    m_artifacts.trusted_template, error) ||
                manifest.manifest_id !=
                    m_artifacts.provider_manifest.manifest_id;
            break;
        }
        }
        assert(rejected);
    }

    void CheckReplay(uint8_t kind, bool conflict)
    {
        std::string key;
        uint256 canonical;
        uint256 candidate;
        switch (kind % 11) {
        case 0: {
            key = CapacityKey();
            canonical = CanonicalHash(
                m_artifacts.capacity.request, m_artifacts.capacity.proof);
            PaymasterCapacityProof changed{m_artifacts.capacity.proof};
            changed.created_at++;
            candidate = conflict ? CanonicalHash(
                                       m_artifacts.capacity.request, changed) :
                                   canonical;
            break;
        }
        case 1: {
            key = IntentKey();
            canonical = CanonicalHash(m_artifacts.quote_request);
            PaymasterQuoteRequest changed{m_artifacts.quote_request};
            changed.intent.recipient_amount.value++;
            candidate = conflict ? CanonicalHash(changed) : canonical;
            break;
        }
        case 2: {
            key = QuoteKey();
            canonical = CanonicalHash(m_artifacts.quote_response);
            PaymasterQuoteResponse changed{m_artifacts.quote_response};
            changed.quote.network_fee.value++;
            candidate = conflict ? CanonicalHash(changed) : canonical;
            break;
        }
        case 3: {
            key = SubmitKey();
            canonical = CanonicalHash(m_artifacts.submit);
            PaymasterSubmit changed{m_artifacts.submit};
            changed.user_psbt.push_back(0);
            candidate = conflict ? CanonicalHash(changed) : canonical;
            break;
        }
        case 4: {
            key = ResultKey();
            canonical = CanonicalHash(m_artifacts.result_message);
            PaymasterResultMessage changed{m_artifacts.result_message};
            changed.result.updated_at++;
            candidate = conflict ? CanonicalHash(changed) : canonical;
            break;
        }
        case 5: {
            AlternativeRecoveryParameters parameters{
                m_artifacts.RecoveryParameters()};
            AlternativeRecoveryTemplate recovery;
            std::string error;
            const auto callbacks = m_artifacts.CapacityCallbacks(
                m_artifacts.verified_recovery_dgb,
                m_artifacts.recovery_dgb_key);
            assert(BuildAlternativeRecoveryTemplate(
                parameters, Params(), m_artifacts.coins, callbacks, NOW,
                recovery, error));
            key = RecoveryKey(recovery.manifest);
            canonical = CanonicalHash(
                recovery.manifest, recovery.trusted_template.psbt);
            AlternativeRecoveryManifest changed{recovery.manifest};
            changed.service_fee.value++;
            candidate = conflict ? CanonicalHash(
                                       changed, recovery.trusted_template.psbt) :
                                   canonical;
            break;
        }
        case 6: {
            key = ClientAcceptanceKey();
            canonical = CanonicalHash(m_artifacts.client_manifest);
            ClientAuthorizationManifest changed{
                m_artifacts.client_manifest};
            changed.maximum_service_fee.value++;
            changed.manifest_id =
                GetClientAuthorizationManifestId(changed);
            candidate = conflict ? CanonicalHash(changed) : canonical;
            break;
        }
        case 7:
        case 8:
        case 9:
        case 10: {
            AlternativeRecoveryParameters parameters{
                m_artifacts.RecoveryParameters()};
            AlternativeRecoveryTemplate recovery;
            std::string error;
            const CapacityChainstateCallbacks callbacks{
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_recovery_dgb,
                    m_artifacts.recovery_dgb_key)};
            assert(BuildAlternativeRecoveryTemplate(
                parameters, Params(), m_artifacts.coins, callbacks, NOW,
                recovery, error));
            AlternativeRecoveryRequest request{
                m_artifacts.RecoveryRequest(parameters)};
            AlternativeRecoveryResponse response{
                m_artifacts.RecoveryResponse(parameters, request, recovery)};
            AlternativeRecoverySubmit submit{
                m_artifacts.RecoverySubmit(response, recovery)};
            AlternativeRecoveryResultMessage result{
                m_artifacts.RecoveryResult(response, recovery)};
            switch (kind % 11) {
            case 7: {
                key = "recovery-request:" +
                      request.original_commit_key.GetHex() + ":" +
                      request.recovery_provider_id.GetHex();
                canonical = CanonicalHash(request);
                AlternativeRecoveryRequest changed{request};
                changed.created_at++;
                candidate = conflict ? CanonicalHash(changed) : canonical;
                break;
            }
            case 8: {
                key = "recovery-response:" +
                      response.recovery_id.GetHex();
                canonical = CanonicalHash(response);
                AlternativeRecoveryResponse changed{response};
                changed.expires_at++;
                candidate = conflict ? CanonicalHash(changed) : canonical;
                break;
            }
            case 9: {
                key = "recovery-submit:" +
                      submit.recovery_commit_key.GetHex();
                canonical = CanonicalHash(submit);
                AlternativeRecoverySubmit changed{submit};
                changed.user_psbt.push_back(0);
                candidate = conflict ? CanonicalHash(changed) : canonical;
                break;
            }
            case 10: {
                key = "recovery-result:" +
                      result.result.commit_key.GetHex() + ":" +
                      std::to_string(result.result.result_sequence);
                canonical = CanonicalHash(result);
                AlternativeRecoveryResultMessage changed{result};
                changed.result.updated_at++;
                candidate = conflict ? CanonicalHash(changed) : canonical;
                break;
            }
            default: assert(false);
            }
            break;
        }
        }
    // Keep synthetic replay probes independent from lifecycle bindings.
    // A lifecycle artifact and a synthetic probe can use the same semantic
    // key for mutually conflicting result contents.
        const ReplayDisposition first =
            m_replay_probes.Observe(key, canonical);
        assert(first == ReplayDisposition::NEW ||
               first == ReplayDisposition::EXACT);
        assert(m_replay_probes.Observe(key, candidate) ==
               (conflict ? ReplayDisposition::CONFLICT : ReplayDisposition::EXACT));
    }

    void CheckBudgets(uint8_t variant)
    {
        FundingSafetyLimits limits;
        limits.maximum_network_fee_per_transaction =
            DGBSatoshis{NETWORK_FEE};
        limits.maximum_reserved_network_fee =
            DGBSatoshis{2 * NETWORK_FEE};
        limits.maximum_network_fee_per_hour =
            DGBSatoshis{2 * NETWORK_FEE};
        limits.maximum_network_fee_per_day =
            DGBSatoshis{3 * NETWORK_FEE};
        limits.maximum_completed_per_hour = 2;
        limits.maximum_completed_per_day = 3;

        ProviderSafetyPolicy safety;
        safety.public_sponsored = limits;
        safety.maximum_active_quotes_total = 2;
        safety.maximum_active_quotes_per_netgroup = 1;
        safety.maximum_active_quotes_per_recipient = 1;
        safety.maximum_quote_requests_per_netgroup_per_minute = 2;
        safety.updated_at = NOW;
        std::string error;
        assert(ValidateProviderSafetyPolicy(
            safety, m_artifacts.policy, error));

        ProviderBudgetLedger provider_ledger;
        provider_ledger.recipient_bucket_secret = uint256S("501");
        provider_ledger.accounting_time_high_water = NOW;
        const uint256 recipient_bucket = GetRecipientBudgetBucket(
            provider_ledger, m_artifacts.intent.recipient_script);
        const uint256 netgroup_bucket = GetNetgroupBudgetBucket(
            provider_ledger,
            std::vector<unsigned char>{0xfc, 0x00, 0x01, 0x02, 0x03});
        assert(!recipient_bucket.IsNull());
        assert(!netgroup_bucket.IsNull());
        assert(ReserveProviderBudget(
            provider_ledger, safety, FundingModel::SPONSORED,
            SponsorshipScope::PUBLIC, m_artifacts.submit.commit_key,
            DGBSatoshis{NETWORK_FEE}, recipient_bucket, NOW, error,
            netgroup_bucket));
        assert(provider_ledger.reservations.size() == 1);

        ProviderAuthorizationManifest bound_manifest;
        assert(BuildProviderAuthorizationManifest(
            m_artifacts.intent, m_artifacts.quote,
            m_artifacts.trusted_template,
            GetProviderSafetyPolicyHash(safety), bound_manifest, error));
        ProviderAttempt bound_attempt;
        bound_attempt.provider_id = m_artifacts.intent.provider_id;
        bound_attempt.client_nonce = m_artifacts.intent.client_nonce;
        bound_attempt.intent_hash = m_artifacts.quote.intent_hash;
        bound_attempt.quote_id = m_artifacts.quote.quote_id;
        bound_attempt.template_commitment =
            m_artifacts.quote.template_commitment;
        bound_attempt.commit_key = m_artifacts.submit.commit_key;
        bound_attempt.provider_manifest = bound_manifest;
        bound_attempt.provider_netgroup_bucket = netgroup_bucket;
        assert(ValidateProviderBudgetReservationBinding(
            bound_manifest, bound_attempt,
            provider_ledger.reservations.front(), safety,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false,
            /*allow_legacy_durable_commit=*/false, error));

        ProviderAuthorizationManifest invalid_manifest{bound_manifest};
        ProviderAttempt invalid_attempt{bound_attempt};
        ProviderBudgetReservation invalid_reservation{
            provider_ledger.reservations.front()};
        switch ((variant / 8) % 8) {
        case 0:
            invalid_manifest.safety_policy_hash = uint256S("531");
            break;
        case 1:
            invalid_manifest.budget_reservation_id = uint256S("532");
            break;
        case 2:
            invalid_manifest.maximum_network_fee.value++;
            break;
        case 3:
            invalid_attempt.commit_key = uint256S("533");
            break;
        case 4:
            invalid_attempt.client_nonce = uint256S("534");
            break;
        case 5:
            invalid_reservation.netgroup_bucket = uint256S("535");
            break;
        case 6:
            invalid_reservation.network_fee.value--;
            break;
        case 7:
            invalid_reservation.state = BudgetReservationState::SPENT;
            break;
        }
        invalid_manifest.manifest_id =
            GetProviderAuthorizationManifestId(invalid_manifest);
        invalid_attempt.provider_manifest = invalid_manifest;
        assert(!ValidateProviderBudgetReservationBinding(
            invalid_manifest, invalid_attempt, invalid_reservation, safety,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false,
            /*allow_legacy_durable_commit=*/false, error));

        ProviderBudgetReservation spent_reservation{
            provider_ledger.reservations.front()};
        spent_reservation.state = BudgetReservationState::SPENT;
        assert(ValidateProviderBudgetReservationBinding(
            bound_manifest, bound_attempt, spent_reservation, safety,
            BudgetReservationState::SPENT,
            /*allow_historical_policy=*/false,
            /*allow_legacy_durable_commit=*/false, error));
        assert(!ValidateProviderBudgetReservationBinding(
            bound_manifest, bound_attempt, spent_reservation, safety,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false,
            /*allow_legacy_durable_commit=*/false, error));

        const uint256 provider_before{CanonicalHash(provider_ledger)};
        bool provider_result{false};
        switch (variant % 8) {
        case 0:
            provider_result = ReserveProviderBudget(
                provider_ledger, safety, FundingModel::SPONSORED,
                SponsorshipScope::PUBLIC, m_artifacts.submit.commit_key,
                DGBSatoshis{NETWORK_FEE}, recipient_bucket, NOW, error,
                netgroup_bucket);
            assert(provider_result &&
                   provider_ledger.reservations.size() == 1);
            assert(SpendProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW, error));
            assert(SpendProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW, error));
            assert(!ReleaseProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW, error));
            break;
        case 1:
            assert(ReleaseProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW, error));
            assert(ReleaseProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW, error));
            assert(!SpendProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW, error));
            break;
        case 2:
            provider_result = ReserveProviderBudget(
                provider_ledger, safety, FundingModel::SPONSORED,
                SponsorshipScope::PUBLIC, m_artifacts.submit.commit_key,
                DGBSatoshis{NETWORK_FEE - 1}, recipient_bucket, NOW, error,
                netgroup_bucket);
            assert(!provider_result &&
                   CanonicalHash(provider_ledger) == provider_before);
            break;
        case 3:
            provider_result = ReserveProviderBudget(
                provider_ledger, safety, FundingModel::SPONSORED,
                SponsorshipScope::PUBLIC, uint256S("502"),
                DGBSatoshis{NETWORK_FEE + 1}, recipient_bucket, NOW, error,
                netgroup_bucket);
            assert(!provider_result &&
                   CanonicalHash(provider_ledger) == provider_before);
            break;
        case 4:
            provider_result = ReserveProviderBudget(
                provider_ledger, safety, FundingModel::SPONSORED,
                SponsorshipScope::PUBLIC, uint256S("503"),
                DGBSatoshis{NETWORK_FEE}, recipient_bucket, NOW, error,
                netgroup_bucket);
            assert(!provider_result &&
                   CanonicalHash(provider_ledger) == provider_before);
            break;
        case 5:
            assert(SpendProviderBudget(
                provider_ledger, m_artifacts.submit.commit_key, NOW - 1,
                error));
            assert(provider_ledger.accounting_time_high_water == NOW);
            break;
        case 6: {
            ProviderSafetyPolicy disabled{safety};
            disabled.public_sponsored = {};
            provider_result = ReserveProviderBudget(
                provider_ledger, disabled, FundingModel::SPONSORED,
                SponsorshipScope::PUBLIC, uint256S("504"),
                DGBSatoshis{NETWORK_FEE}, uint256S("505"), NOW, error,
                uint256S("506"));
            assert(!provider_result &&
                   CanonicalHash(provider_ledger) == provider_before);
            break;
        }
        case 7:
            assert(!ReleaseProviderBudget(
                provider_ledger, uint256S("507"), NOW, error));
            assert(CanonicalHash(provider_ledger) == provider_before);
            break;
        }

        ClientSafetyPolicy client_policy;
        client_policy.maximum_service_fee_per_transaction = DDCents{100};
        client_policy.maximum_service_fee_per_day = DDCents{150};
        client_policy.updated_at = NOW;

        ClientFeeLedger manifest_client_ledger;
        manifest_client_ledger.accounting_time_high_water = NOW;
        assert(ReserveClientFee(
            manifest_client_ledger, client_policy,
            m_artifacts.submit.commit_key,
            m_artifacts.client_manifest.service_fee, NOW, error));
        assert(manifest_client_ledger.reservations.size() == 1);
        const ClientFeeReservation& manifest_reservation{
            manifest_client_ledger.reservations.front()};
        assert(manifest_reservation.commit_key ==
                   m_artifacts.submit.commit_key &&
               manifest_reservation.service_fee ==
                   m_artifacts.client_manifest.service_fee &&
               manifest_reservation.state ==
                   BudgetReservationState::RESERVED);
        const uint256 manifest_client_before{
            CanonicalHash(manifest_client_ledger)};
        assert(!ReserveClientFee(
            manifest_client_ledger, client_policy,
            m_artifacts.submit.commit_key,
            DDCents{m_artifacts.client_manifest.service_fee.value + 1}, NOW,
            error));
        assert(CanonicalHash(manifest_client_ledger) ==
               manifest_client_before);

        ClientFeeLedger client_ledger;
        client_ledger.accounting_time_high_water = NOW;
        assert(ReserveClientFee(
            client_ledger, client_policy, uint256S("511"), DDCents{50}, NOW,
            error));
        assert(client_ledger.reservations.size() == 1);
        const uint256 client_before{CanonicalHash(client_ledger)};
        switch (variant % 6) {
        case 0:
            assert(ReserveClientFee(
                client_ledger, client_policy, uint256S("511"), DDCents{50},
                NOW, error));
            assert(client_ledger.reservations.size() == 1);
            assert(SpendClientFee(
                client_ledger, uint256S("511"), NOW, error));
            assert(!ReleaseClientFee(
                client_ledger, uint256S("511"), NOW, error));
            break;
        case 1:
            assert(ReleaseClientFee(
                client_ledger, uint256S("511"), NOW, error));
            assert(!SpendClientFee(
                client_ledger, uint256S("511"), NOW, error));
            break;
        case 2:
            assert(!ReserveClientFee(
                client_ledger, client_policy, uint256S("511"), DDCents{51},
                NOW, error));
            assert(CanonicalHash(client_ledger) == client_before);
            break;
        case 3:
            assert(!ReserveClientFee(
                client_ledger, client_policy, uint256S("512"),
                DDCents{101}, NOW, error));
            assert(CanonicalHash(client_ledger) == client_before);
            break;
        case 4:
            assert(ReserveClientFee(
                client_ledger, client_policy, uint256S("512"),
                DDCents{100}, NOW, error));
            assert(!ReserveClientFee(
                client_ledger, client_policy, uint256S("513"), DDCents{1},
                NOW, error));
            break;
        case 5:
            assert(SpendClientFee(
                client_ledger, uint256S("511"), NOW - 1, error));
            assert(client_ledger.accounting_time_high_water == NOW);
            break;
        }
    }

    void CheckResultProgression(bool conflict)
    {
        std::string error;
        PaymasterResult first{m_artifacts.result};
        assert(ValidatePaymasterResult(
            first, m_artifacts.intent.genesis_hash,
            m_artifacts.intent.provider_id, m_artifacts.submit.commit_key,
            XOnlyPubKey{m_artifacts.provider_identity_key.GetPubKey()}, 1,
            error));

        PaymasterResult candidate{first};
        if (conflict) {
            candidate.status = PaymasterResultStatus::USER_PSBT_ACCEPTED;
        } else {
            candidate.result_sequence = 2;
            candidate.updated_at = NOW + 1;
        }
        SignIdentity(
            m_artifacts.provider_identity_key,
            GetPaymasterResultSignatureHash(candidate),
            candidate.identity_signature);
        assert(ValidatePaymasterResult(
            candidate, m_artifacts.intent.genesis_hash,
            m_artifacts.intent.provider_id, m_artifacts.submit.commit_key,
            XOnlyPubKey{m_artifacts.provider_identity_key.GetPubKey()}, 1,
            error));

        SemanticReplayCache replay;
        const std::string first_key =
            "result:" + m_artifacts.submit.commit_key.GetHex() + ":1";
        assert(replay.Observe(first_key, CanonicalHash(first)) ==
               ReplayDisposition::NEW);
        if (conflict) {
            assert(replay.Observe(first_key, CanonicalHash(candidate)) ==
                   ReplayDisposition::CONFLICT);
        } else {
            const std::string second_key =
                "result:" + m_artifacts.submit.commit_key.GetHex() + ":2";
            assert(replay.Observe(second_key, CanonicalHash(candidate)) ==
                   ReplayDisposition::NEW);
            assert(ValidatePaymasterResult(
                candidate, m_artifacts.intent.genesis_hash,
                m_artifacts.intent.provider_id,
                m_artifacts.submit.commit_key,
                XOnlyPubKey{m_artifacts.provider_identity_key.GetPubKey()},
                2, error));
            assert(!ValidatePaymasterResult(
                first, m_artifacts.intent.genesis_hash,
                m_artifacts.intent.provider_id,
                m_artifacts.submit.commit_key,
                XOnlyPubKey{m_artifacts.provider_identity_key.GetPubKey()},
                2, error));
        }
        assert(!CanTransition(AttemptState::FINAL_COMMITTED,
                              AttemptState::REJECTED));
        assert(!CanTransition(AttemptState::MEMPOOL,
                              AttemptState::AMBIGUOUS));
    }

    void CheckPrivacy(uint8_t variant)
    {
        ProviderBudgetLedger ledger;
        ledger.recipient_bucket_secret = uint256S("521");
        ledger.accounting_time_high_water = NOW;
        const std::vector<unsigned char> canonical_netgroup{
            0xfa, 0xfb, 0xfc, 0xfd, 0x01, 0x02, 0x03, 0x04,
            0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
        const uint256 recipient_bucket = GetRecipientBudgetBucket(
            ledger, m_artifacts.intent.recipient_script);
        const uint256 netgroup_bucket =
            GetNetgroupBudgetBucket(ledger, canonical_netgroup);
        assert(!recipient_bucket.IsNull() && !netgroup_bucket.IsNull());
        assert(recipient_bucket == GetRecipientBudgetBucket(
                                       ledger,
                                       m_artifacts.intent.recipient_script));
        assert(netgroup_bucket ==
               GetNetgroupBudgetBucket(ledger, canonical_netgroup));

        ProviderBudgetLedger other{ledger};
        other.recipient_bucket_secret = uint256S("522");
        assert(recipient_bucket != GetRecipientBudgetBucket(
                                       other,
                                       m_artifacts.intent.recipient_script));
        assert(netgroup_bucket !=
               GetNetgroupBudgetBucket(other, canonical_netgroup));

        const std::vector<unsigned char> capacity_bytes = SerializeExact(
            m_artifacts.capacity.request, m_artifacts.capacity.proof);
        assert(!ContainsSubsequence(
            capacity_bytes,
            SerializeExact(m_artifacts.intent.recipient_script)));
        assert(!ContainsSubsequence(
            capacity_bytes,
            SerializeExact(m_artifacts.intent.user_dd_inputs.front())));
        SponsorshipCapability restricted_capability;
        restricted_capability.genesis_hash =
            m_artifacts.intent.genesis_hash;
        restricted_capability.provider_id =
            m_artifacts.intent.provider_id;
        restricted_capability.offer_id = m_artifacts.intent.offer_id;
        restricted_capability.policy_hash =
            m_artifacts.intent.policy_hash;
        restricted_capability.recipient_script =
            m_artifacts.intent.recipient_script;
        restricted_capability.amount =
            m_artifacts.intent.recipient_amount;
        restricted_capability.payment_request_nonce = uint256S("524");
        restricted_capability.expires_at = NOW + 30;
        restricted_capability.sponsor_signature.assign(64, 0xa5);
        assert(!ContainsSubsequence(
            capacity_bytes, SerializeExact(restricted_capability)));

        ProviderBudgetReservation persisted;
        persisted.commit_key = uint256S("523");
        persisted.funding_model = FundingModel::SPONSORED;
        persisted.sponsorship_scope = SponsorshipScope::PUBLIC;
        persisted.network_fee = DGBSatoshis{NETWORK_FEE};
        persisted.recipient_bucket = recipient_bucket;
        persisted.netgroup_bucket = netgroup_bucket;
        persisted.state = BudgetReservationState::RESERVED;
        persisted.reserved_at = NOW;
        persisted.updated_at = NOW;
        ledger.reservations.push_back(persisted);
        const std::vector<unsigned char> persisted_bytes =
            SerializeExact(ledger);
        assert(!ContainsSubsequence(
            persisted_bytes,
            SerializeExact(m_artifacts.intent.recipient_script)));
        assert(!ContainsSubsequence(persisted_bytes, canonical_netgroup));

        if (variant % 2 != 0) {
            AlternativeRecoveryParameters parameters{
                m_artifacts.RecoveryParameters()};
            parameters.privacy_profile = PrivacyProfile::HIGH;
            AlternativeRecoveryTemplate recovery;
            std::string error;
            assert(BuildAlternativeRecoveryTemplate(
                parameters, Params(), m_artifacts.coins,
                m_artifacts.CapacityCallbacks(
                    m_artifacts.verified_recovery_dgb,
                    m_artifacts.recovery_dgb_key),
                NOW, recovery, error));
            const AlternativeRecoveryRequest request{
                m_artifacts.RecoveryRequest(parameters)};
            assert(request.privacy_profile == PrivacyProfile::HIGH);
            assert(recovery.manifest.privacy_profile ==
                   PrivacyProfile::HIGH);
        }
    }
};

void CheckSaturatingTime(FuzzedDataProvider& provider)
{
    const int64_t timestamp = provider.ConsumeIntegral<int64_t>();
    const int64_t seconds = provider.ConsumeIntegral<int64_t>();
    const int64_t result = SaturatingAddSeconds(timestamp, seconds);
    constexpr int64_t MIN{std::numeric_limits<int64_t>::min()};
    constexpr int64_t MAX{std::numeric_limits<int64_t>::max()};
    if (seconds > 0) {
        assert(result >= timestamp);
        if (timestamp > MAX - seconds) {
            assert(result == MAX);
        } else {
            assert(result == timestamp + seconds);
        }
    } else if (seconds < 0) {
        assert(result <= timestamp);
        if (timestamp < MIN - seconds) {
            assert(result == MIN);
        } else {
            assert(result == timestamp + seconds);
        }
    } else {
        assert(result == timestamp);
    }
    assert(SaturatingAddSeconds(MAX, 1) == MAX);
    assert(SaturatingAddSeconds(MIN, -1) == MIN);

    const int64_t later = provider.ConsumeIntegral<int64_t>();
    const int64_t earlier = provider.ConsumeIntegral<int64_t>();
    const int64_t maximum_delta = provider.ConsumeIntegral<int64_t>();
    const bool exceeded = TimeDeltaExceeds(later, earlier, maximum_delta);
    if (later <= earlier) {
        assert(!exceeded);
    } else if (maximum_delta < 0) {
        assert(exceeded);
    } else {
        const uint64_t delta = static_cast<uint64_t>(later) -
                               static_cast<uint64_t>(earlier);
        assert(exceeded ==
               (delta > static_cast<uint64_t>(maximum_delta)));
    }
}

void CheckAmountArithmetic(FuzzedDataProvider& provider)
{
    const int64_t payment_value = provider.ConsumeIntegral<int64_t>();
    const uint32_t fee_rate_bps = provider.ConsumeIntegral<uint32_t>();
    const std::optional<DDCents> fee =
        ComputePaymasterFee(DDCents{payment_value}, fee_rate_bps);
    const bool valid_inputs =
        payment_value > 0 && payment_value <= MAX_DD_OUTPUT_CENTS &&
        fee_rate_bps <= MAX_RATE_BPS && fee_rate_bps % 10 == 0;
    if (!valid_inputs) {
        assert(!fee.has_value());
    } else {
        const int64_t quotient = payment_value / 10000;
        const int64_t remainder = payment_value % 10000;
        const int64_t expected =
            quotient * fee_rate_bps +
            (remainder * fee_rate_bps + 9999) / 10000;
        if (expected > MAX_DD_OUTPUT_CENTS - payment_value) {
            assert(!fee.has_value());
        } else {
            assert(fee.has_value());
            assert(fee->value == expected);
            assert(fee->value >= 0);
            assert(fee->value <=
                   MAX_DD_OUTPUT_CENTS - payment_value);
        }
    }

    const int64_t carrier_value = provider.ConsumeIntegral<int64_t>();
    const int64_t service_fee_value = provider.ConsumeIntegral<int64_t>();
    std::string error;
    const std::optional<DDCents> successor = ComputeCarrierSuccessor(
        DDCents{carrier_value}, DDCents{service_fee_value}, error);
    const bool carrier_inputs_valid =
        carrier_value >= 100 && carrier_value <= MAX_DD_OUTPUT_CENTS &&
        service_fee_value > 0 && service_fee_value < 100 &&
        carrier_value <= MAX_DD_OUTPUT_CENTS - service_fee_value;
    assert(successor.has_value() == carrier_inputs_valid);
    if (successor) {
        assert(successor->value == carrier_value + service_fee_value);
        assert(successor->value <= MAX_DD_OUTPUT_CENTS);
    } else {
        assert(!error.empty());
    }
}

void ParseRawEnvelope(FuzzedDataProvider& provider)
{
    const uint8_t kind = provider.ConsumeIntegralInRange<uint8_t>(0, 6);
    const size_t size = provider.ConsumeIntegralInRange<size_t>(
        0, std::min<size_t>(provider.remaining_bytes(), 256));
    const std::vector<uint8_t> raw = provider.ConsumeBytes<uint8_t>(size);
    CDataStream stream{raw, SER_NETWORK, ::PROTOCOL_VERSION};
    std::string error;
    try {
        switch (kind) {
        case 0: {
            PaymasterCapacityRequest message;
            stream >> message;
            (void)ValidateCapacityRequestEnvelope(
                message, message.genesis_hash, NOW, error);
            break;
        }
        case 1: {
            PaymasterCapacityProof message;
            stream >> message;
            (void)ValidateCapacityProofEnvelope(
                message, message.genesis_hash, NOW, error);
            break;
        }
        case 2: {
            PaymasterQuoteRequest message;
            stream >> message;
            (void)ValidateQuoteRequestEnvelope(
                message, message.intent.genesis_hash, NOW, error);
            break;
        }
        case 3: {
            PaymasterQuoteResponse message;
            stream >> message;
            (void)ValidateQuoteResponseEnvelope(
                message, message.quote.genesis_hash, NOW, error);
            break;
        }
        case 4: {
            PaymasterSubmit message;
            stream >> message;
            (void)ValidateSubmitEnvelope(
                message, message.genesis_hash, error);
            break;
        }
        case 5: {
            PaymasterResultMessage message;
            stream >> message;
            (void)ValidateResultMessageEnvelope(
                message, message.result.genesis_hash, NOW, error);
            break;
        }
        case 6: {
            AlternativeRecoveryManifest manifest;
            stream >> manifest;
            (void)GetAlternativeRecoveryManifestId(manifest);
            break;
        }
        }
    } catch (const std::exception&) {
        // Truncation, invalid compact sizes and unknown enum values are
        // expected input and must remain fail-closed.
    }

    // Reuse the exact legacy payload and selector bytes for v3/v4 recovery
    // envelopes. This adds coverage without remapping existing corpus inputs.
    try {
        CDataStream recovery_stream{raw, SER_NETWORK, ::PROTOCOL_VERSION};
        switch (kind % 4) {
        case 0: {
            AlternativeRecoveryRequest message;
            recovery_stream >> message;
            (void)ValidateAlternativeRecoveryRequestEnvelope(
                message, message.genesis_hash, NOW, error);
            (void)GetAlternativeRecoveryRequestHash(message);
            break;
        }
        case 1: {
            AlternativeRecoveryResponse message;
            recovery_stream >> message;
            (void)ValidateAlternativeRecoveryResponseEnvelope(
                message, message.genesis_hash, NOW, error);
            (void)GetAlternativeRecoveryCommitKey(message);
            break;
        }
        case 2: {
            AlternativeRecoverySubmit message;
            recovery_stream >> message;
            (void)ValidateAlternativeRecoverySubmitEnvelope(
                message, message.genesis_hash, error);
            break;
        }
        case 3: {
            AlternativeRecoveryResultMessage message;
            recovery_stream >> message;
            (void)ValidateAlternativeRecoveryResultEnvelope(
                message, message.result.genesis_hash, NOW, error);
            break;
        }
        }
    } catch (const std::exception&) {
        // Recovery envelope truncation and invalid enums are expected too.
    }
}

} // namespace

/**
 * Stateful acceptance target for:
 *
 *   Capacity -> Intent -> Quote -> client acceptance -> Submit -> Result
 *            -> Alternative Recovery
 *
 * Each action is one byte: low nibble selects the action, high bit requests a
 * binding mutation. Mutation variants consume one additional byte. Useful
 * seed hex cases for the external qa-assets corpus are:
 *
 *   0001020a030405      complete canonical recovery lifecycle
 *   0001020a030f        complete signed FINAL_COMMITTED lifecycle
 *   000102030405        legacy seed; Submit performs exact acceptance first
 *   030002010405        attempted phase skips followed by canonical flow
 *   0607080b0c0d0e      firewall, time boundaries, replay/equivocation,
 *                     budget/manifest binding, amounts, sequence, privacy
 *
 * The corpus itself intentionally remains in qa-assets, as required by
 * doc/fuzzing.md; these comments make the deterministic seeds reproducible.
 */
FUZZ_TARGET(paymaster_stateful_security,
            .init = initialize_paymaster_stateful)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    StatefulArtifacts artifacts;
    StatefulMachine machine{artifacts};

    LIMITED_WHILE(provider.remaining_bytes() > 0, 128)
    {
        const uint8_t action_byte = provider.ConsumeIntegral<uint8_t>();
        const uint8_t action = action_byte & 0x0f;
        const bool mutate = (action_byte & 0x80) != 0;
        const uint8_t variant = mutate && provider.remaining_bytes() > 0 ? provider.ConsumeIntegral<uint8_t>() : 0;
        const Phase before = machine.CurrentPhase();
        switch (action) {
        case 0: (void)machine.Capacity(mutate, variant); break;
        case 1: (void)machine.Intent(mutate, variant); break;
        case 2: (void)machine.Quote(mutate, variant); break;
        case 3:
            // Preserve old corpus seeds while keeping acceptance an explicit
            // state: the legacy Submit opcode first accepts only the exact
            // locally rebuilt client manifest.
            if (machine.CurrentPhase() == Phase::QUOTE) {
                assert(machine.ClientAcceptance(false, 0));
            }
            (void)machine.Submit(mutate, variant);
            break;
        case 4: (void)machine.Result(mutate, variant); break;
        case 5: (void)machine.Recovery(mutate, variant); break;
        case 6: machine.CheckFirewall(variant); break;
        case 7:
            machine.CheckProtocolTimeBoundaries(variant);
            CheckSaturatingTime(provider);
            break;
        case 8:
            machine.CheckReplay(
                variant, mutate || provider.ConsumeBool());
            machine.CheckEquivocation(variant);
            break;
        case 9: ParseRawEnvelope(provider); break;
        case 10: (void)machine.ClientAcceptance(mutate, variant); break;
        case 11: machine.CheckBudgets(variant); break;
        case 12: CheckAmountArithmetic(provider); break;
        case 13:
            machine.CheckResultProgression(
                mutate || provider.ConsumeBool());
            break;
        case 14: machine.CheckPrivacy(variant); break;
        case 15: (void)machine.FinalResult(mutate, variant); break;
        }
        const Phase after = machine.CurrentPhase();
        assert(static_cast<uint8_t>(after) >=
               static_cast<uint8_t>(before));
        const uint8_t maximum_advance =
            action == 3 && before == Phase::QUOTE ? 2 : 1;
        assert(static_cast<uint8_t>(after) <=
               static_cast<uint8_t>(before) + maximum_advance);
    }
}
