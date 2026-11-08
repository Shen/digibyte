// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Wallet ownership checks and role-limited signing for collaborative PSBTs.
 * This is the final wallet boundary: only locally owned inputs assigned to the
 * requested party are signed, after the shared authorization manifest passes.
 */

#include <wallet/paymasterpsbt.h>

#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <key.h>
#include <paymaster/txbuilder.h>
#include <paymaster/types.h>
#include <paymaster/wire.h>
#include <random.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <util/time.h>
#include <version.h>
#include <wallet/paymasteridentity.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <limits>

namespace wallet {
namespace {

template <typename T>
std::vector<unsigned char> SerializeExact(const T& value)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool ToInputRole(DigiDollar::Paymaster::ReservationRole role,
                 DigiDollar::Paymaster::InputRole& result)
{
    using namespace DigiDollar::Paymaster;
    switch (role) {
    case ReservationRole::USER_DD:
        result = InputRole::USER_DD;
        return true;
    case ReservationRole::USER_DGB:
        result = InputRole::USER_DGB;
        return true;
    case ReservationRole::PROVIDER_CARRIER:
        result = InputRole::PROVIDER_CARRIER;
        return true;
    case ReservationRole::PROVIDER_DGB:
        result = InputRole::PROVIDER_DGB;
        return true;
    }
    return false;
}

void CopySignatures(const PSBTInput& source, PSBTInput& destination)
{
    destination.partial_sigs = source.partial_sigs;
    destination.final_script_sig = source.final_script_sig;
    destination.final_script_witness = source.final_script_witness;
    destination.m_tap_key_sig = source.m_tap_key_sig;
    destination.m_tap_script_sigs = source.m_tap_script_sigs;
}

} // namespace

bool CheckPaymasterNewSignatureInputs(
    const CWallet& wallet, const std::vector<COutPoint>& inputs, std::string& error)
{
    LOCK(wallet.cs_wallet);
    for (const auto& input : inputs) {
        if (wallet.IsOrdinaryLockedCoin(input)) {
            error = "PAYMASTER_PROVIDER_INPUT_LOCKED";
            return false;
        }
    }
    error.clear();
    return true;
}

Bip86ControlSigningError SignPaymasterBip86Control(
    CWallet& wallet, const COutPoint& input, const CScript& expected_script,
    const uint256& digest, Bip86ControlSignature& result)
{
    result = {};
    LOCK(wallet.cs_wallet);
    if (wallet.IsLocked()) return Bip86ControlSigningError::WALLET_LOCKED;
    const auto* transaction = wallet.GetWalletTx(input.hash);
    if (!transaction || input.n >= transaction->tx->vout.size() ||
        transaction->tx->vout[input.n].scriptPubKey != expected_script) {
        return Bip86ControlSigningError::PREVOUT_NOT_FOUND;
    }
    if (!(wallet.IsMine(expected_script) & ISMINE_SPENDABLE)) {
        return Bip86ControlSigningError::NOT_SPENDABLE;
    }
    CTxDestination destination;
    if (!ExtractDestination(expected_script, destination)) return Bip86ControlSigningError::NOT_BIP86;
    const auto* taproot = std::get_if<WitnessV1Taproot>(&destination);
    if (!taproot) return Bip86ControlSigningError::NOT_BIP86;
    const XOnlyPubKey output_key{*taproot};
    for (ScriptPubKeyMan* manager : wallet.GetScriptPubKeyMans(expected_script)) {
        std::unique_ptr<SigningProvider> provider;
        if (auto* descriptor = dynamic_cast<DescriptorScriptPubKeyMan*>(manager)) {
            provider = descriptor->GetSigningProviderWithKeys(expected_script);
        } else {
            provider = manager->GetSolvingProvider(expected_script);
        }
        TaprootSpendData spend_data;
        CKey key;
        if (!provider || !provider->GetTaprootSpendData(output_key, spend_data) ||
            !spend_data.internal_key.IsFullyValid() || !spend_data.merkle_root.IsNull() ||
            !spend_data.scripts.empty() || !provider->GetKeyByXOnly(spend_data.internal_key, key) || !key.IsValid()) continue;
        const auto tweaked = spend_data.internal_key.CreateTapTweak(nullptr);
        if (!tweaked || tweaked->first != output_key) continue;
        std::vector<unsigned char> signature(64);
        const uint256 empty_merkle_root;
        if (!key.SignSchnorr(digest, signature, &empty_merkle_root, GetRandHash())) {
            return Bip86ControlSigningError::SIGNING_FAILED;
        }
        result.output_key = output_key;
        result.signature = std::move(signature);
        return Bip86ControlSigningError::NONE;
    }
    return Bip86ControlSigningError::KEY_UNAVAILABLE;
}

namespace {
bool SetInputControlSigningError(Bip86ControlSigningError result, const char* signing_error, std::string& error)
{
    switch (result) {
    case Bip86ControlSigningError::NONE: return true;
    case Bip86ControlSigningError::WALLET_LOCKED: error = "PAYMASTER_WALLET_LOCKED"; break;
    case Bip86ControlSigningError::PREVOUT_NOT_FOUND: error = "PAYMASTER_USER_PREVOUT_NOT_FOUND"; break;
    case Bip86ControlSigningError::NOT_SPENDABLE: error = "PAYMASTER_USER_PREVOUT_NOT_SPENDABLE"; break;
    case Bip86ControlSigningError::NOT_BIP86: error = "PAYMASTER_USER_PREVOUT_NOT_BIP86"; break;
    case Bip86ControlSigningError::KEY_UNAVAILABLE: error = "PAYMASTER_USER_PREVOUT_KEY_UNAVAILABLE"; break;
    case Bip86ControlSigningError::SIGNING_FAILED: error = signing_error; break;
    }
    return false;
}
} // namespace

bool SignCollaborativePSBTForParty(
    CWallet& wallet,
    PartiallySignedTransaction& psbt,
    const DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted_template,
    DigiDollar::Paymaster::SigningParty party,
    std::string& error)
{
    return SignCollaborativePSBTForParty(wallet, psbt, trusted_template, party,
                                       std::numeric_limits<int64_t>::max(), error);
}

bool SignCollaborativePSBTForParty(
    CWallet& wallet,
    PartiallySignedTransaction& psbt,
    const DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted_template,
    DigiDollar::Paymaster::SigningParty party,
    int64_t valid_until,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    LOCK(wallet.cs_wallet);
    if (party == SigningParty::PROVIDER &&
        !CheckPaymasterProviderRestoreGuard(wallet, error)) return false;
    const CollaborativeSignatureStage before = party == SigningParty::USER ? CollaborativeSignatureStage::UNSIGNED : CollaborativeSignatureStage::USER_SIGNED;
    const CollaborativeSignatureStage after = party == SigningParty::USER ? CollaborativeSignatureStage::USER_SIGNED : CollaborativeSignatureStage::FULLY_SIGNED;
    if (!ValidateCollaborativePSBT(psbt, trusted_template, before, error)) return false;
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }

    // This entry point creates a new signature. Exact durable provider replay
    // uses its saved bytes instead; our own reservation reason is not a veto.
    if (party == SigningParty::PROVIDER) {
        std::vector<COutPoint> provider_inputs;
        for (size_t index = 0; index < psbt.inputs.size(); ++index) {
            if (IsInputOwnedBy(trusted_template.input_roles[index], party)) provider_inputs.push_back(psbt.tx->vin[index].prevout);
        }
        if (!CheckPaymasterNewSignatureInputs(wallet, provider_inputs, error)) return false;
    }

    PartiallySignedTransaction working{psbt};
    for (size_t index = 0; index < working.inputs.size(); ++index) {
        if (!IsInputOwnedBy(trusted_template.input_roles[index], party) &&
            !PSBTInputSigned(working.inputs[index])) {
            // FillPSBT skips finalized inputs. This temporary marker never
            // leaves this function and is discarded before validation.
            working.inputs[index].final_script_sig = CScript{} << OP_0;
        }
    }

    bool complete{false};
    size_t signed_inputs{0};
    const int64_t signing_time{GetTime()};
    if (signing_time <= 0 || signing_time > valid_until) {
        error = "PAYMASTER_SIGNING_AUTHORIZATION_EXPIRED";
        return false;
    }
    const TransactionError signing_result = wallet.FillPSBT(
        working, complete, SIGHASH_DEFAULT, /*sign=*/true,
        /*bip32derivs=*/false, &signed_inputs, /*finalize=*/true);
    if (signing_result != TransactionError::OK) {
        error = "PAYMASTER_PSBT_SIGNING_FAILED";
        return false;
    }

    PartiallySignedTransaction signed_result{psbt};
    for (size_t index = 0; index < working.inputs.size(); ++index) {
        if (IsInputOwnedBy(trusted_template.input_roles[index], party)) {
            CopySignatures(working.inputs[index], signed_result.inputs[index]);
        }
    }
    if (!ValidateCollaborativePSBT(signed_result, trusted_template, after, error)) return false;
    psbt = std::move(signed_result);
    return true;
}

bool SignPaymentIntentInputs(
    CWallet& wallet,
    DigiDollar::Paymaster::PaymentIntent& intent,
    int64_t now,
    std::vector<XOnlyPubKey>& output_keys,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    output_keys.clear();
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }
    if (intent.user_dd_inputs.empty() || intent.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        !intent.user_input_proofs.empty()) {
        error = "PAYMASTER_INVALID_INTENT_INPUTS";
        return false;
    }

    std::vector<UserInputControlProof> proofs;
    std::vector<XOnlyPubKey> resolved_output_keys;
    proofs.reserve(intent.user_dd_inputs.size());
    resolved_output_keys.reserve(intent.user_dd_inputs.size());
    LOCK(wallet.cs_wallet);
    for (const COutPoint& outpoint : intent.user_dd_inputs) {
        const CWalletTx* wallet_tx = wallet.GetWalletTx(outpoint.hash);
        if (!wallet_tx || outpoint.n >= wallet_tx->tx->vout.size()) {
            error = "PAYMASTER_USER_PREVOUT_NOT_FOUND";
            return false;
        }
        Bip86ControlSignature signed_control;
        if (!SetInputControlSigningError(SignPaymasterBip86Control(
                wallet, outpoint, wallet_tx->tx->vout[outpoint.n].scriptPubKey,
                GetUserInputControlHash(intent, outpoint), signed_control),
                "PAYMASTER_USER_INPUT_SIGNING_FAILED", error)) return false;
        UserInputControlProof proof;
        proof.outpoint = outpoint;
        proof.signature = std::move(signed_control.signature);
        proofs.push_back(std::move(proof));
        resolved_output_keys.push_back(signed_control.output_key);
    }
    PaymentIntent signed_intent{intent};
    signed_intent.user_input_proofs = proofs;
    if (!ValidatePaymentIntent(signed_intent, signed_intent.genesis_hash, now,
                               resolved_output_keys, error)) {
        return false;
    }
    intent.user_input_proofs = std::move(proofs);
    output_keys = std::move(resolved_output_keys);
    return true;
}

bool ValidateClientAuthorizationOwnership(
    CWallet& wallet,
    const DigiDollar::Paymaster::ClientAuthorizationManifest& manifest,
    std::string& error)
{
    error.clear();
    if (manifest.version !=
            DigiDollar::Paymaster::ClientAuthorizationManifest::CURRENT_VERSION ||
        manifest.manifest_id.IsNull() ||
        manifest.manifest_id !=
            DigiDollar::Paymaster::GetClientAuthorizationManifestId(manifest) ||
        manifest.user_dd_inputs.empty()) {
        error = "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID";
        return false;
    }
    LOCK(wallet.cs_wallet);
    for (const COutPoint& outpoint : manifest.user_dd_inputs) {
        const CWalletTx* wallet_tx = wallet.GetWalletTx(outpoint.hash);
        if (!wallet_tx || outpoint.n >= wallet_tx->tx->vout.size() ||
            !(wallet.IsMine(wallet_tx->tx->vout[outpoint.n].scriptPubKey) &
              ISMINE_SPENDABLE)) {
            error = "PAYMASTER_CLIENT_INPUT_NOT_WALLET_OWNED";
            return false;
        }
    }
    if (!manifest.user_dd_change_script.empty() &&
        !(wallet.IsMine(manifest.user_dd_change_script) & ISMINE_SPENDABLE)) {
        error = "PAYMASTER_CLIENT_CHANGE_NOT_WALLET_OWNED";
        return false;
    }
    return true;
}

bool ValidateProviderAuthorizationOwnership(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderAuthorizationManifest& manifest,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (manifest.version != ProviderAuthorizationManifest::CURRENT_VERSION ||
        manifest.manifest_id.IsNull() ||
        manifest.manifest_id !=
            GetProviderAuthorizationManifestId(manifest) ||
        (manifest.provider_dgb_inputs.empty() &&
         manifest.provider_carrier_inputs.empty())) {
        error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID";
        return false;
    }

    LOCK(wallet.cs_wallet);
    const auto inputs_owned = [&](const std::vector<COutPoint>& outpoints) {
        return std::all_of(
            outpoints.begin(), outpoints.end(), [&](const COutPoint& outpoint) {
                const CWalletTx* wallet_tx = wallet.GetWalletTx(outpoint.hash);
                return wallet_tx && outpoint.n < wallet_tx->tx->vout.size() &&
                       bool(wallet.IsMine(
                                wallet_tx->tx->vout[outpoint.n].scriptPubKey) &
                            ISMINE_SPENDABLE);
            });
    };
    if (!inputs_owned(manifest.provider_dgb_inputs) ||
        !inputs_owned(manifest.provider_carrier_inputs)) {
        error = "PAYMASTER_PROVIDER_INPUT_NOT_WALLET_OWNED";
        return false;
    }
    const auto scripts_owned = [&](const std::vector<CScript>& scripts) {
        return std::all_of(scripts.begin(), scripts.end(),
                           [&](const CScript& script) {
                               return bool(wallet.IsMine(script) &
                                           ISMINE_SPENDABLE);
                           });
    };
    if (!scripts_owned(manifest.carrier_return_scripts) ||
        !scripts_owned(manifest.provider_fee_scripts) ||
        !scripts_owned(manifest.dgb_change_scripts)) {
        error = "PAYMASTER_PROVIDER_CHANGE_NOT_WALLET_OWNED";
        return false;
    }
    return true;
}

bool SignAlternativeRecoveryRequestInputs(
    CWallet& wallet,
    DigiDollar::Paymaster::AlternativeRecoveryRequest& request,
    int64_t now,
    std::vector<XOnlyPubKey>& output_keys,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    output_keys.clear();
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }
    if (request.user_dd_inputs.empty() ||
        request.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        !request.user_input_proofs.empty()) {
        error = "PAYMASTER_INVALID_RECOVERY_REQUEST_SHAPE";
        return false;
    }

    AlternativeRecoveryRequest signed_request{request};
    std::vector<XOnlyPubKey> resolved_keys;
    signed_request.user_input_proofs.reserve(request.user_dd_inputs.size());
    resolved_keys.reserve(request.user_dd_inputs.size());
    LOCK(wallet.cs_wallet);
    for (const COutPoint& outpoint : request.user_dd_inputs) {
        const CWalletTx* wallet_tx = wallet.GetWalletTx(outpoint.hash);
        if (!wallet_tx || outpoint.n >= wallet_tx->tx->vout.size()) {
            error = "PAYMASTER_USER_PREVOUT_NOT_FOUND";
            return false;
        }
        Bip86ControlSignature signed_control;
        if (!SetInputControlSigningError(SignPaymasterBip86Control(
                wallet, outpoint, wallet_tx->tx->vout[outpoint.n].scriptPubKey,
                GetAlternativeRecoveryInputControlHash(signed_request, outpoint), signed_control),
                "PAYMASTER_RECOVERY_INPUT_SIGNING_FAILED", error)) return false;
        AlternativeRecoveryInputProof proof;
        proof.outpoint = outpoint;
        proof.signature = std::move(signed_control.signature);
        signed_request.user_input_proofs.push_back(std::move(proof));
        resolved_keys.push_back(signed_control.output_key);
    }
    if (!ValidateAlternativeRecoveryRequestEnvelope(
            signed_request, signed_request.genesis_hash, now, error) ||
        !ValidateAlternativeRecoveryRequestInputs(signed_request, resolved_keys,
                                                  error)) {
        return false;
    }
    request = std::move(signed_request);
    output_keys = std::move(resolved_keys);
    return true;
}

bool BuildTrustedQuoteTemplate(
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted_template,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    trusted_template = {};
    std::vector<CollaborativeInput> user_inputs;
    CAmount user_dd_total{0};
    {
        LOCK(wallet.cs_wallet);
        for (const COutPoint& outpoint : intent.user_dd_inputs) {
            const CWalletTx* wallet_tx = wallet.GetWalletTx(outpoint.hash);
            CAmount amount{0};
            if (!wallet_tx || outpoint.n >= wallet_tx->tx->vout.size() ||
                !(wallet.IsMine(wallet_tx->tx->vout[outpoint.n].scriptPubKey) & ISMINE_SPENDABLE) ||
                !DigiDollar::ExtractDDAmountFromTransaction(*wallet_tx->tx, outpoint, amount) ||
                amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - user_dd_total) {
                error = "PAYMASTER_USER_PREVOUT_INVALID";
                return false;
            }
            user_dd_total += amount;
            user_inputs.push_back({outpoint, wallet_tx->tx, InputRole::USER_DD});
        }
    }

    CAmount dgb_total{0};
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        if (input.value.value <= 0 || input.value.value > std::numeric_limits<CAmount>::max() - dgb_total) {
            error = "PAYMASTER_DGB_AMOUNT_OVERFLOW";
            return false;
        }
        dgb_total += input.value.value;
    }

    CollaborativeTransferParams params;
    if (!BuildQuoteTransferParameters(
            intent, quote, user_inputs, user_dd_total, dgb_total, params, error)) {
        if (error == "PAYMASTER_INVALID_CARRIER_SUCCESSOR") {
            error = "PAYMASTER_CARRIER_OUTPUT_MISMATCH";
        }
        return false;
    }

    const auto built = BuildUnsignedCollaborativeTransfer(params, chain_params, chainman);
    if (!built.success) {
        error = built.error;
        return false;
    }
    if (built.miner_fee != quote.network_fee.value ||
        CTransaction{built.tx} != CTransaction{quote.unsigned_transaction}) {
        error = "PAYMASTER_QUOTE_TRANSACTION_MISMATCH";
        return false;
    }
    if (!CreateCollaborativePSBTTemplate(built.tx, params.inputs, trusted_template, error)) {
        return false;
    }
    if (GetCollaborativeTemplateCommitment(intent, quote, trusted_template) !=
        quote.template_commitment) {
        trusted_template = {};
        error = "PAYMASTER_TEMPLATE_COMMITMENT_MISMATCH";
        return false;
    }
    error.clear();
    return true;
}

bool BuildAlternativeRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    parameters = {};
    if (recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        recovery.phase < AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
        recovery.recovery_response.manifest.recovery_provider_dd_scripts.size() > 1 ||
        recovery.recovery_response.manifest.recovery_provider_dgb_change_scripts.size() > 1) {
        error = "PAYMASTER_RECOVERY_RESPONSE_MISSING";
        return false;
    }

    parameters.genesis_hash = recovery.capacity_request.genesis_hash;
    parameters.request_id = recovery.request_id;
    parameters.session_id = recovery.session_id;
    parameters.original_provider_id = recovery.original_provider_id;
    parameters.recovery_provider_id = recovery.recovery_provider_id;
    parameters.privacy_profile = recovery.privacy_profile;
    parameters.offer_id = recovery.offer_id;
    parameters.policy_hash = recovery.policy_hash;
    parameters.original_commit_key = recovery.original_commit_key;
    parameters.original_template_commitment =
        recovery.original_template_commitment;
    parameters.capacity_request = recovery.capacity_request;
    parameters.capacity_snapshot = recovery.capacity_snapshot;
    parameters.recovery_provider_identity_key =
        recovery.recovery_provider_identity_key;
    parameters.persisted_user_dd_inputs =
        recovery.recovery_request.user_dd_inputs;
    parameters.wallet_returns = recovery.recovery_request.wallet_returns;

    for (const AlternativeRecoveryReturn& output : parameters.wallet_returns) {
        if (!recovery.provider_side &&
            !(WITH_LOCK(wallet.cs_wallet,
                        return bool(wallet.IsMine(output.script_pub_key) &
                                    ISMINE_SPENDABLE)))) {
            error = "PAYMASTER_RECOVERY_DESTINATION_NOT_OWNED";
            return false;
        }
        parameters.wallet_verified_fresh_return_scripts.push_back(
            output.script_pub_key);
    }

    for (const COutPoint& outpoint : parameters.persisted_user_dd_inputs) {
        CTransactionRef creating_tx;
        {
            LOCK(wallet.cs_wallet);
            const CWalletTx* wallet_tx = wallet.GetWalletTx(outpoint.hash);
            if (wallet_tx) creating_tx = wallet_tx->tx;
        }
        if (!creating_tx && recovery.provider_side) {
            uint256 block_hash;
            if (!g_txindex ||
                !g_txindex->FindTx(outpoint.hash, block_hash, creating_tx)) {
                creating_tx.reset();
            }
        }
        if (!creating_tx || outpoint.n >= creating_tx->vout.size()) {
            error = "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_FOUND";
            return false;
        }
        if (!recovery.provider_side &&
            !(WITH_LOCK(wallet.cs_wallet,
                        return bool(wallet.IsMine(
                                        creating_tx->vout[outpoint.n].scriptPubKey) &
                                    ISMINE_SPENDABLE)))) {
            error = "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_OWNED";
            return false;
        }
        parameters.user_dd_inputs.push_back(
            {outpoint, creating_tx, InputRole::USER_DD});
    }

    const AlternativeRecoveryManifest& manifest =
        recovery.recovery_response.manifest;
    if (!manifest.recovery_provider_dd_scripts.empty()) {
        parameters.recovery_provider_dd_script =
            manifest.recovery_provider_dd_scripts.front();
    }
    if (!manifest.recovery_provider_dgb_change_scripts.empty()) {
        parameters.recovery_provider_dgb_change_script =
            manifest.recovery_provider_dgb_change_scripts.front();
    }
    if (recovery.provider_side) {
        const auto inputs_owned = [&](const std::vector<COutPoint>& outpoints) {
            return std::all_of(
                outpoints.begin(), outpoints.end(),
                [&](const COutPoint& outpoint) {
                    return WITH_LOCK(
                        wallet.cs_wallet,
                        const CWalletTx* wallet_tx =
                            wallet.GetWalletTx(outpoint.hash);
                        return wallet_tx &&
                               outpoint.n < wallet_tx->tx->vout.size() &&
                               bool(wallet.IsMine(
                                        wallet_tx->tx->vout[outpoint.n]
                                            .scriptPubKey) &
                                    ISMINE_SPENDABLE));
                });
        };
        if (!inputs_owned(manifest.recovery_provider_carrier_inputs) ||
            !inputs_owned(manifest.recovery_provider_dgb_inputs)) {
            error = "PAYMASTER_RECOVERY_PROVIDER_INPUT_NOT_OWNED";
            parameters = {};
            return false;
        }
        const auto is_wallet_owned = [&](const std::optional<CScript>& script) {
            return !script ||
                   WITH_LOCK(wallet.cs_wallet,
                             return bool(wallet.IsMine(*script) &
                                         ISMINE_SPENDABLE));
        };
        if (!is_wallet_owned(parameters.recovery_provider_dd_script) ||
            !is_wallet_owned(parameters.recovery_provider_dgb_change_script)) {
            error = "PAYMASTER_RECOVERY_PROVIDER_OUTPUT_NOT_OWNED";
            parameters = {};
            return false;
        }
    }
    parameters.maximum_service_fee = manifest.maximum_service_fee;
    parameters.service_fee = manifest.service_fee;
    parameters.network_fee = manifest.network_fee;
    // The exact fixed fee is rebuilt and compared by the core validator. One
    // sat/vB remains only its lower-bound policy input.
    parameters.fee_rate = 1;
    parameters.expires_at = recovery.recovery_response.expires_at;
    error.clear();
    return true;
}

} // namespace wallet
