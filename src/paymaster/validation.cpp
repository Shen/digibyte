// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Chainstate-backed validation for capacity and recovery artifacts.
 * Wire signatures prove who made a claim; these routines additionally prove
 * that each claimed outpoint exists, is unspent, and matches the committed
 * value and Taproot key at the selected reference block.
 */

#include <paymaster/validation.h>

#include <coins.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <sync.h>
#include <txmempool.h>
#include <validation.h>

#include <algorithm>

namespace DigiDollar::Paymaster {
namespace {

constexpr int MAX_REFERENCE_BLOCK_AGE{24};

bool GetP2TROutputKey(const CScript& script, XOnlyPubKey& key)
{
    int version{-1};
    std::vector<unsigned char> program;
    if (!script.IsWitnessProgram(version, program) || version != 1 || program.size() != 32) return false;
    key = XOnlyPubKey{program};
    return key.IsFullyValid();
}

template <typename Callback>
bool WithCapacityChainstate(const ChainstateManager& chainman,
                            Callback&& callback,
                            std::string& error,
                            const CTransaction* exact_known_spender = nullptr,
                            bool exact_spender_is_known = false,
                            bool allow_historical_active_reference = false)
{
    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    const int tip_height = active_chain.Height();
    const CCoinsViewCache& coins = chainman.ActiveChainstate().CoinsTip();
    CTxMemPool* const mempool = chainman.ActiveChainstate().GetMempool();

    const auto exact_spender_spends = [&](const COutPoint& outpoint) {
        return exact_known_spender &&
               std::any_of(exact_known_spender->vin.begin(),
                           exact_known_spender->vin.end(),
                           [&](const CTxIn& input) {
                               return input.prevout == outpoint;
                           });
    };
    const auto foreign_mempool_conflict = [&](const COutPoint& outpoint) {
        if (mempool == nullptr) return false;
        LOCK(mempool->cs);
        const CTransaction* conflict = mempool->GetConflictTx(outpoint);
        return conflict &&
               (!exact_spender_is_known || !exact_known_spender ||
                conflict->GetWitnessHash() !=
                    exact_known_spender->GetWitnessHash());
    };
    CapacityChainstateCallbacks callbacks;
    callbacks.validate_reference_block = [&](const uint256& block_hash,
                                             std::string& callback_error) {
        const CBlockIndex* reference = chainman.m_blockman.LookupBlockIndex(block_hash);
        if (!reference || !active_chain.Contains(reference) ||
            (!allow_historical_active_reference &&
             tip_height - reference->nHeight > MAX_REFERENCE_BLOCK_AGE)) {
            callback_error = "PAYMASTER_STALE_CAPACITY_REFERENCE_BLOCK";
            return false;
        }
        return true;
    };
    callbacks.validate_dgb_input = [&](const VerifiedDGBInput& input,
                                       XOnlyPubKey& output_key,
                                       std::string& callback_error) {
        const CTransaction creating_tx{input.creating_tx};
        const Coin& coin = coins.AccessCoin(input.outpoint);
        if (creating_tx.GetHash() != input.outpoint.hash ||
            input.outpoint.n >= creating_tx.vout.size()) {
            callback_error = "PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE";
            return false;
        }
        const bool known_spend = exact_spender_is_known &&
                                 exact_spender_spends(input.outpoint);
        const CTxOut& actual_output = coin.IsSpent()
                                          ? creating_tx.vout.at(input.outpoint.n)
                                          : coin.out;
        if ((coin.IsSpent() && !known_spend) ||
            (!coin.IsSpent() && coin.IsCoinBase()) ||
            foreign_mempool_conflict(input.outpoint) ||
            actual_output != creating_tx.vout[input.outpoint.n] ||
            actual_output.nValue != input.value.value || input.value.value <= 0 ||
            (!known_spend &&
             (coin.nHeight == MEMPOOL_HEIGHT ||
              tip_height - static_cast<int>(coin.nHeight) + 1 < 1)) ||
            !GetP2TROutputKey(actual_output.scriptPubKey, output_key)) {
            callback_error = "PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE";
            return false;
        }
        return true;
    };
    callbacks.validate_dd_carrier = [&](const VerifiedDDCarrier& carrier,
                                        XOnlyPubKey& output_key,
                                        std::string& callback_error) {
        const CTransaction creating_tx{carrier.creating_tx};
        const Coin& coin = coins.AccessCoin(carrier.outpoint);
        if (creating_tx.GetHash() != carrier.outpoint.hash ||
            carrier.outpoint.n >= creating_tx.vout.size()) {
            callback_error = "PAYMASTER_INVALID_CAPACITY_CARRIER_CHAINSTATE";
            return false;
        }
        const bool known_spend = exact_spender_is_known &&
                                 exact_spender_spends(carrier.outpoint);
        const CTxOut& actual_output = coin.IsSpent()
                                          ? creating_tx.vout.at(carrier.outpoint.n)
                                          : coin.out;
        CAmount amount{0};
        if ((coin.IsSpent() && !known_spend) ||
            (!coin.IsSpent() && coin.IsCoinBase()) ||
            foreign_mempool_conflict(carrier.outpoint) ||
            actual_output != creating_tx.vout[carrier.outpoint.n] ||
            !ExtractDDAmountFromTransaction(creating_tx, carrier.outpoint, amount) ||
            amount != carrier.value.value || amount < 100 ||
            (!known_spend &&
             (coin.nHeight == MEMPOOL_HEIGHT ||
              tip_height - static_cast<int>(coin.nHeight) + 1 < 1)) ||
            !GetP2TROutputKey(actual_output.scriptPubKey, output_key)) {
            callback_error = "PAYMASTER_INVALID_CAPACITY_CARRIER_CHAINSTATE";
            return false;
        }
        return true;
    };
    return callback(coins, callbacks, error);
}

bool ValidateAuthorizedRetryContext(
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    const CTransaction* exact_final_transaction,
    std::string& error)
{
    if (authorization_time <= 0 || observation_time < authorization_time) {
        error = "PAYMASTER_INVALID_AUTHORIZED_CAPACITY_TIME";
        return false;
    }
    const bool exact_final_known{
        resource_mode ==
        AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN};
    if (exact_final_known != (exact_final_transaction != nullptr)) {
        error = "PAYMASTER_INVALID_AUTHORIZED_CAPACITY_RESOURCE_MODE";
        return false;
    }
    return true;
}

} // namespace

uint256 GetAdmissionControlHash(const PaymasterId& provider_id,
                                uint64_t sequence,
                                const uint256& reference_block,
                                const COutPoint& outpoint,
                                int64_t expires_at,
                                bool carrier)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Admission v1");
    hasher << provider_id << sequence << reference_block << outpoint << expires_at
           << static_cast<uint8_t>(carrier ? 1 : 0);
    return hasher.GetSHA256();
}

bool ValidateAdmissionProofs(const Announcement& announcement,
                             const ChainstateManager& chainman,
                             std::string& error)
{
    const PaymasterId provider_id = GetPaymasterId(announcement.identity_key);
    const bool needs_carriers = std::any_of(announcement.offers.begin(), announcement.offers.end(),
        [](const OfferTerms& offer) { return offer.funding_model == FundingModel::USER_PAID; });

    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    const int tip_height = active_chain.Height();
    const CCoinsViewCache& coins = chainman.ActiveChainstate().CoinsTip();
    CTxMemPool* const mempool = chainman.ActiveChainstate().GetMempool();
    const auto has_mempool_conflict = [&](const COutPoint& outpoint) {
        if (mempool == nullptr) return false;
        LOCK(mempool->cs);
        return mempool->GetConflictTx(outpoint) != nullptr;
    };

    for (const AdmissionSlotProof& slot : announcement.admission_slots) {
        const CBlockIndex* reference = chainman.m_blockman.LookupBlockIndex(slot.reference_block);
        if (!reference || !active_chain.Contains(reference) || tip_height - reference->nHeight > MAX_REFERENCE_BLOCK_AGE) {
            error = "PAYMASTER_STALE_REFERENCE_BLOCK";
            return false;
        }

        const CTransaction dgb_tx{slot.dgb_creating_tx};
        const Coin& dgb_coin = coins.AccessCoin(slot.dgb_outpoint);
        if (dgb_coin.IsSpent() || dgb_coin.IsCoinBase() || dgb_tx.GetHash() != slot.dgb_outpoint.hash ||
            slot.dgb_outpoint.n >= dgb_tx.vout.size() || dgb_coin.out != dgb_tx.vout[slot.dgb_outpoint.n] ||
            dgb_coin.out.nValue != slot.dgb_value.value || slot.dgb_value.value < MIN_ADMISSION_DGB_SATOSHIS ||
            has_mempool_conflict(slot.dgb_outpoint) ||
            reference->nHeight < static_cast<int>(dgb_coin.nHeight) ||
            tip_height - static_cast<int>(dgb_coin.nHeight) + 1 < announcement.min_confirmations) {
            error = "PAYMASTER_INVALID_DGB_ADMISSION_PROOF";
            return false;
        }
        XOnlyPubKey dgb_key;
        if (!GetP2TROutputKey(dgb_coin.out.scriptPubKey, dgb_key) ||
            !dgb_key.VerifySchnorr(GetAdmissionControlHash(provider_id, announcement.sequence,
                                  slot.reference_block, slot.dgb_outpoint, slot.expires_at, false),
                                  slot.dgb_control_signature)) {
            error = "PAYMASTER_INVALID_DGB_CONTROL_SIGNATURE";
            return false;
        }

        if (!needs_carriers) continue;
        const CTransaction carrier_tx{slot.carrier_creating_tx};
        const Coin& carrier_coin = coins.AccessCoin(slot.carrier_outpoint);
        CAmount carrier_amount{0};
        if (carrier_coin.IsSpent() || carrier_coin.IsCoinBase() || carrier_tx.GetHash() != slot.carrier_outpoint.hash ||
            slot.carrier_outpoint.n >= carrier_tx.vout.size() ||
            carrier_coin.out != carrier_tx.vout[slot.carrier_outpoint.n] ||
            !ExtractDDAmountFromTransaction(carrier_tx, slot.carrier_outpoint, carrier_amount) ||
            carrier_amount != slot.carrier_value.value || carrier_amount < 100 ||
            has_mempool_conflict(slot.carrier_outpoint) ||
            reference->nHeight < static_cast<int>(carrier_coin.nHeight) ||
            tip_height - static_cast<int>(carrier_coin.nHeight) + 1 < announcement.min_confirmations) {
            error = "PAYMASTER_INVALID_CARRIER_ADMISSION_PROOF";
            return false;
        }
        XOnlyPubKey carrier_key;
        if (!GetP2TROutputKey(carrier_coin.out.scriptPubKey, carrier_key) ||
            !carrier_key.VerifySchnorr(GetAdmissionControlHash(provider_id, announcement.sequence,
                                      slot.reference_block, slot.carrier_outpoint, slot.expires_at, true),
                                      slot.carrier_control_signature)) {
            error = "PAYMASTER_INVALID_CARRIER_CONTROL_SIGNATURE";
            return false;
        }
    }

    error.clear();
    return true;
}

bool ValidateRestrictedAdmissionProofs(const RestrictedServiceDescriptor& descriptor,
                                       const XOnlyPubKey& provider_identity_key,
                                       const ChainstateManager& chainman,
                                       std::string& error)
{
    if (!provider_identity_key.IsFullyValid() ||
        descriptor.provider_id != GetPaymasterId(provider_identity_key) ||
        descriptor.admission_sequence == 0 || descriptor.min_confirmations == 0 ||
        descriptor.minimum_liquidity_proof.size() != REQUIRED_ADMISSION_SLOTS) {
        error = "PAYMASTER_INVALID_RESTRICTED_ADMISSION_PROOF";
        return false;
    }
    Announcement proof;
    proof.identity_key = provider_identity_key;
    proof.sequence = descriptor.admission_sequence;
    proof.min_confirmations = descriptor.min_confirmations;
    proof.offers.push_back(OfferTerms{descriptor.offer_id, descriptor.policy_hash,
                                     FundingModel::SPONSORED, SponsorshipScope::RESTRICTED,
                                     0, DDCents{100}, DDCents{MAX_DD_OUTPUT_CENTS}});
    proof.admission_slots = descriptor.minimum_liquidity_proof;
    return ValidateAdmissionProofs(proof, chainman, error);
}

bool ValidateCapacityProofAgainstChainstate(
    const PaymasterCapacityProof& proof,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const ChainstateManager& chainman,
    int64_t now,
    std::string& error,
    const CTransaction* exact_known_spender,
    bool exact_spender_is_known)
{
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache&, const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateCapacityProof(proof, request, request.genesis_hash,
                                         provider_identity_key, callbacks, now,
                                         callback_error);
        },
        error, exact_known_spender, exact_spender_is_known);
}

bool ValidateAuthorizedCapacityRetryAgainstChainstate(
    const PaymasterCapacityProof& proof,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    std::string& error,
    const CTransaction* exact_final_transaction)
{
    if (!ValidateAuthorizedRetryContext(
            authorization_time, observation_time, resource_mode,
            exact_final_transaction, error)) {
        return false;
    }
    const bool exact_final_known{
        resource_mode ==
        AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN};
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache&,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateCapacityProof(
                proof, request, request.genesis_hash,
                provider_identity_key, callbacks, authorization_time,
                callback_error);
        },
        error, exact_final_transaction, exact_final_known,
        /*allow_historical_active_reference=*/true);
}

bool BuildAlternativeRecoveryTemplateAgainstChainstate(
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    AlternativeRecoveryTemplate& result,
    std::string& error)
{
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return BuildAlternativeRecoveryTemplate(parameters, chain_params, coins,
                                                    callbacks, now, result,
                                                    callback_error);
        },
        error);
}

bool ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    AlternativeRecoveryTemplate& trusted,
    PartiallySignedTransaction& unsigned_psbt,
    std::string& error,
    const CTransaction* exact_known_spender,
    bool exact_spender_is_known)
{
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateAlternativeRecoveryResponseTemplate(
                response, request, provider_identity_key, parameters,
                chain_params, coins, callbacks, now, trusted, unsigned_psbt,
                callback_error);
        },
        error, exact_known_spender, exact_spender_is_known);
}

bool ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    AlternativeRecoveryTemplate& trusted,
    PartiallySignedTransaction& unsigned_psbt,
    std::string& error,
    const CTransaction* exact_final_transaction)
{
    if (!ValidateAuthorizedRetryContext(
            authorization_time, observation_time, resource_mode,
            exact_final_transaction, error)) {
        return false;
    }
    const bool exact_final_known{
        resource_mode ==
        AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN};
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateAlternativeRecoveryResponseTemplate(
                response, request, provider_identity_key, parameters,
                chain_params, coins, callbacks, authorization_time, trusted,
                unsigned_psbt, callback_error);
        },
        error, exact_final_transaction, exact_final_known,
        /*allow_historical_active_reference=*/true);
}

bool ValidateAlternativeRecoveryPSBTAgainstChainstate(
    const PartiallySignedTransaction& candidate,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    CollaborativeSignatureStage expected_stage,
    std::string& error)
{
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateAlternativeRecoveryPSBT(
                candidate, trusted, parameters, chain_params, coins, callbacks,
                now, expected_stage, callback_error);
        },
        error);
}

bool ValidateAlternativeRecoverySubmitAgainstChainstate(
    const AlternativeRecoverySubmit& submit,
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    PartiallySignedTransaction& user_psbt,
    std::string& error,
    const CTransaction* exact_known_spender,
    bool exact_spender_is_known)
{
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateAlternativeRecoverySubmit(
                submit, response, trusted, parameters, chain_params, coins,
                callbacks, now, user_psbt, callback_error);
        },
        error, exact_known_spender, exact_spender_is_known);
}

bool ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
    const AlternativeRecoverySubmit& submit,
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    PartiallySignedTransaction& user_psbt,
    std::string& error,
    const CTransaction* exact_final_transaction)
{
    if (!ValidateAuthorizedRetryContext(
            authorization_time, observation_time, resource_mode,
            exact_final_transaction, error)) {
        return false;
    }
    const bool exact_final_known{
        resource_mode ==
        AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN};
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateAlternativeRecoverySubmit(
                submit, response, trusted, parameters, chain_params, coins,
                callbacks, authorization_time, user_psbt, callback_error);
        },
        error, exact_final_transaction, exact_final_known,
        /*allow_historical_active_reference=*/true);
}

bool ValidateFinalAlternativeRecoveryAgainstChainstate(
    const CMutableTransaction& final_transaction,
    const uint256& expected_wtxid,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    bool exact_final_already_known,
    std::string& error)
{
    const CTransaction exact_final{final_transaction};
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateFinalAlternativeRecoveryTransaction(
                final_transaction, expected_wtxid, trusted, parameters,
                chain_params, coins, callbacks, now,
                exact_final_already_known, callback_error);
        },
        error, &exact_final, exact_final_already_known);
}

bool ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
    const CMutableTransaction& final_transaction,
    const uint256& expected_wtxid,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    std::string& error)
{
    const CTransaction exact_final{final_transaction};
    const bool exact_final_known{
        resource_mode ==
        AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN};
    const CTransaction* const known_final{
        exact_final_known ? &exact_final : nullptr};
    if (!ValidateAuthorizedRetryContext(
            authorization_time, observation_time, resource_mode,
            known_final, error)) {
        return false;
    }
    return WithCapacityChainstate(
        chainman,
        [&](const CCoinsViewCache& coins,
            const CapacityChainstateCallbacks& callbacks,
            std::string& callback_error) {
            return ValidateFinalAlternativeRecoveryTransaction(
                final_transaction, expected_wtxid, trusted, parameters,
                chain_params, coins, callbacks, authorization_time,
                exact_final_known, callback_error);
        },
        error, known_final, exact_final_known,
        /*allow_historical_active_reference=*/true);
}

bool ValidateFinalAlternativeRecoveryResultAgainstChainstate(
    const AlternativeRecoveryResultMessage& message,
    const AlternativeRecoveryResponse& response,
    const XOnlyPubKey& provider_identity_key,
    uint64_t minimum_sequence,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    bool exact_final_already_known,
    std::string& error)
{
    if (!ValidateAlternativeRecoveryResult(
            message, response, provider_identity_key,
            parameters.genesis_hash, minimum_sequence, now, error)) {
        return false;
    }
    const PaymasterResult& result = message.result;
    if ((result.status != PaymasterResultStatus::FINAL_COMMITTED &&
         result.status != PaymasterResultStatus::BROADCAST_ATTEMPTED) ||
        !result.final_transaction || !result.raw_transaction_hash) {
        error = "PAYMASTER_RECOVERY_FINAL_RESULT_REQUIRED";
        return false;
    }
    return ValidateFinalAlternativeRecoveryAgainstChainstate(
        *result.final_transaction, *result.raw_transaction_hash, trusted,
        parameters, chain_params, chainman, now, exact_final_already_known,
        error);
}

bool ValidateDurableAlternativeRecoveryResultAgainstChainstate(
    const AlternativeRecoveryResultMessage& message,
    const AlternativeRecoveryResponse& response,
    const XOnlyPubKey& provider_identity_key,
    uint64_t minimum_sequence,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    bool exact_final_already_known,
    std::string& error)
{
    if (authorization_time <= 0 || observation_time < authorization_time ||
        !ValidateAlternativeRecoveryResult(
            message, response, provider_identity_key,
            parameters.genesis_hash, minimum_sequence, observation_time,
            error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_DURABLE_RECOVERY_TIME";
        return false;
    }
    const PaymasterResult& result = message.result;
    if ((result.status != PaymasterResultStatus::FINAL_COMMITTED &&
         result.status != PaymasterResultStatus::BROADCAST_ATTEMPTED) ||
        !result.final_transaction || !result.raw_transaction_hash) {
        error = "PAYMASTER_RECOVERY_FINAL_RESULT_REQUIRED";
        return false;
    }
    const AuthorizedCapacityResourceMode resource_mode{
        exact_final_already_known
            ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN
            : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT};
    return ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
        *result.final_transaction, *result.raw_transaction_hash, trusted,
        parameters, chain_params, chainman, authorization_time,
        observation_time, resource_mode, error);
}

} // namespace DigiDollar::Paymaster
