// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Cryptographic bindings for payment intents, quotes, commits, and results.
 * Validation here is deliberately independent of peer identity: every object
 * must authenticate its complete economic meaning and exact transaction
 * template before wallet code may persist or sign it.
 */

#include <paymaster/protocol.h>

#include <consensus/digidollar.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <kernel/chainparams.h>
#include <paymaster/directory.h>
#include <paymaster/psbt.h>
#include <paymaster/sponsorship.h>
#include <paymaster/txbuilder.h>
#include <paymaster/wire.h>

#include <limits>
#include <set>

namespace DigiDollar::Paymaster {
namespace {

bool IsP2TR(const CScript& script)
{
    int version{-1};
    std::vector<unsigned char> program;
    return script.IsWitnessProgram(version, program) && version == 1 && program.size() == 32 &&
           XOnlyPubKey{program}.IsFullyValid();
}

bool ValidDGBInput(const VerifiedDGBInput& input)
{
    const CTransaction tx{input.creating_tx};
    return !input.outpoint.IsNull() && tx.GetHash() == input.outpoint.hash &&
           input.outpoint.n < tx.vout.size() && input.value.value > 0 &&
           tx.vout[input.outpoint.n].nValue == input.value.value &&
           IsP2TR(tx.vout[input.outpoint.n].scriptPubKey);
}

bool ValidCarrier(const VerifiedDDCarrier& carrier)
{
    const CTransaction tx{carrier.creating_tx};
    CAmount amount{0};
    return !carrier.outpoint.IsNull() && tx.GetHash() == carrier.outpoint.hash &&
           carrier.outpoint.n < tx.vout.size() && carrier.value.value >= 100 &&
           carrier.value.value <= MAX_DD_OUTPUT_CENTS &&
           ExtractDDAmountFromTransaction(tx, carrier.outpoint, amount) &&
           amount == carrier.value.value && IsP2TR(tx.vout[carrier.outpoint.n].scriptPubKey);
}

} // namespace

uint256 GetPaymentIntentCoreHash(const PaymentIntent& intent)
{
    HashWriter hasher = TaggedHash(intent.version >= 2 ? "DigiByte Paymaster Payment Intent Core v2" : "DigiByte Paymaster Payment Intent Core v1");
    hasher << intent.version << intent.genesis_hash << intent.provider_id << intent.request_id
           << intent.session_id << intent.client_nonce << intent.user_dd_inputs
           << intent.recipient_script << intent.recipient_amount << intent.user_dd_change_script
           << intent.offer_id << static_cast<uint8_t>(intent.funding_model)
           << static_cast<uint8_t>(intent.sponsorship_scope)
           << intent.policy_hash << intent.sponsorship_authorization_hash << intent.expires_at;
    if (intent.version >= 2) {
        hasher << intent.canonical_request_hash
               << static_cast<uint8_t>(intent.requested_fee_mode)
               << static_cast<uint8_t>(intent.privacy_profile)
               << static_cast<uint8_t>(intent.selection_mode);
    }
    return hasher.GetSHA256();
}

uint256 GetPaymentIntentHash(const PaymentIntent& intent)
{
    HashWriter hasher = TaggedHash(intent.version >= 2 ? "DigiByte Paymaster Payment Intent v2" : "DigiByte Paymaster Payment Intent v1");
    hasher << GetPaymentIntentCoreHash(intent) << intent.user_input_proofs;
    return hasher.GetSHA256();
}

uint256 GetUserInputControlHash(const PaymentIntent& intent, const COutPoint& outpoint)
{
    HashWriter hasher = TaggedHash(intent.version >= 2 ? "DigiByte Paymaster User Input v2" : "DigiByte Paymaster User Input v1");
    hasher << intent.genesis_hash << intent.provider_id << intent.client_nonce
           << GetPaymentIntentCoreHash(intent) << outpoint << intent.expires_at;
    return hasher.GetSHA256();
}

uint256 GetPaymasterQuoteSignatureHash(const PaymasterQuote& quote)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Quote v1");
    hasher << quote.version << quote.genesis_hash << quote.provider_id << quote.quote_id
           << quote.intent_hash << quote.offer_id << quote.policy_hash
           << static_cast<uint8_t>(quote.funding_model)
           << static_cast<uint8_t>(quote.sponsorship_scope) << quote.sponsorship_authorization_hash
           << quote.fee_rate_bps << quote.service_fee;
    hasher << static_cast<uint8_t>(quote.reserved_carrier.has_value() ? 1U : 0U);
    if (quote.reserved_carrier) {
        hasher << quote.reserved_carrier->outpoint
               << quote.reserved_carrier->creating_tx.GetHash()
               << quote.reserved_carrier->value;
    }
    hasher << static_cast<uint64_t>(quote.reserved_dgb_inputs.size());
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        hasher << input.outpoint << input.creating_tx.GetHash() << input.value;
    }
    hasher
        << Using<OptionalFieldFormatter<CScript>>(quote.carrier_return_script)
        << Using<OptionalFieldFormatter<CScript>>(quote.provider_fee_script)
        << Using<OptionalFieldFormatter<CScript>>(quote.dgb_change_script) << quote.network_fee
        << quote.created_at << quote.expires_at << quote.retry_until
        << quote.unsigned_transaction.GetHash() << quote.unsigned_txid
        << quote.template_commitment;
    return hasher.GetSHA256();
}

uint256 GetPaymasterResultSignatureHash(const PaymasterResult& result)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Result v1");
    hasher << result.version << result.genesis_hash << result.provider_id << result.commit_key
           << result.result_sequence << static_cast<uint8_t>(result.status)
           << Using<OptionalFieldFormatter<uint256>>(result.txid)
           << Using<OptionalFieldFormatter<uint256>>(result.raw_transaction_hash);
    hasher << static_cast<uint8_t>(result.final_transaction.has_value() ? 1U : 0U);
    if (result.final_transaction) {
        const CTransaction transaction{*result.final_transaction};
        hasher << transaction.GetHash() << transaction.GetWitnessHash();
    }
    hasher << result.updated_at;
    return hasher.GetSHA256();
}

uint256 GetPaymasterCommitKey(const PaymasterId& provider_id,
                              const uint256& client_nonce,
                              const uint256& intent_hash,
                              const uint256& quote_id,
                              const uint256& template_commitment)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Commit Key v1");
    hasher << provider_id << client_nonce << intent_hash << quote_id << template_commitment;
    return hasher.GetSHA256();
}

bool ValidatePaymentIntent(const PaymentIntent& intent,
                           const uint256& expected_genesis,
                           int64_t now,
                           const std::vector<XOnlyPubKey>& user_output_keys,
                           std::string& error)
{
    if (intent.version != PaymentIntent::CURRENT_VERSION || intent.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (intent.provider_id.IsNull() || !IsCanonicalRequestId(intent.request_id) ||
        intent.session_id.IsNull() || intent.client_nonce.IsNull() || intent.offer_id.IsNull() ||
        intent.policy_hash.IsNull() || intent.canonical_request_hash.IsNull()) {
        error = "PAYMASTER_INVALID_INTENT_ID";
        return false;
    }
    if ((intent.funding_model != FundingModel::USER_PAID &&
         intent.funding_model != FundingModel::SPONSORED) ||
        (intent.sponsorship_scope != SponsorshipScope::PUBLIC &&
         intent.sponsorship_scope != SponsorshipScope::RESTRICTED) ||
        (intent.requested_fee_mode != FeeMode::PAYMASTER &&
         intent.requested_fee_mode != FeeMode::AUTO) ||
        (intent.privacy_profile != PrivacyProfile::STANDARD &&
         intent.privacy_profile != PrivacyProfile::HIGH) ||
        (intent.selection_mode != SelectionMode::LOWEST_TOTAL_COST &&
         intent.selection_mode != SelectionMode::PRIVACY_WEIGHTED)) {
        error = "PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE";
        return false;
    }
    if (intent.user_dd_inputs.empty() || intent.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        intent.user_input_proofs.size() != intent.user_dd_inputs.size() ||
        user_output_keys.size() != intent.user_dd_inputs.size()) {
        error = "PAYMASTER_INVALID_INTENT_INPUT_COUNT";
        return false;
    }
    if (!IsP2TR(intent.recipient_script) ||
        (!intent.user_dd_change_script.empty() && !IsP2TR(intent.user_dd_change_script)) ||
        intent.recipient_amount.value < 100 || intent.recipient_amount.value > MAX_DD_OUTPUT_CENTS ||
        intent.expires_at <= now || TimeDeltaExceeds(intent.expires_at, now, MAX_QUOTE_TTL_SECONDS)) {
        error = "PAYMASTER_INVALID_INTENT_PAYMENT";
        return false;
    }
    if (!ValidateSponsorshipBinding(intent.funding_model, intent.sponsorship_scope,
                                    0, DDCents{0}, intent.sponsorship_authorization_hash, error)) {
        return false;
    }
    std::set<COutPoint> unique_inputs;
    for (size_t i = 0; i < intent.user_dd_inputs.size(); ++i) {
        if (intent.user_dd_inputs[i].IsNull() ||
            !unique_inputs.insert(intent.user_dd_inputs[i]).second ||
            intent.user_input_proofs[i].outpoint != intent.user_dd_inputs[i] ||
            intent.user_input_proofs[i].signature.size() != 64 ||
            !user_output_keys[i].IsFullyValid()) {
            error = "PAYMASTER_INVALID_USER_INPUT_PROOF";
            return false;
        }
    }
    for (size_t i = 0; i < intent.user_dd_inputs.size(); ++i) {
        if (!user_output_keys[i].VerifySchnorr(GetUserInputControlHash(intent, intent.user_dd_inputs[i]),
                                               intent.user_input_proofs[i].signature)) {
            error = "PAYMASTER_INVALID_USER_INPUT_SIGNATURE";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ValidatePaymasterQuote(const PaymasterQuote& quote,
                            const PaymentIntent& intent,
                            const ProviderPolicy& policy,
                            const XOnlyPubKey& provider_identity_key,
                            DDCents maximum_service_fee,
                            int64_t now,
                            std::string& error)
{
    if (quote.version != PaymasterQuote::CURRENT_VERSION || quote.genesis_hash != intent.genesis_hash ||
        quote.provider_id != intent.provider_id || quote.provider_id != GetPaymasterId(provider_identity_key)) {
        error = "PAYMASTER_QUOTE_CHAIN_OR_PROVIDER_MISMATCH";
        return false;
    }
    if (quote.quote_id.IsNull() || quote.intent_hash != GetPaymentIntentHash(intent) ||
        quote.offer_id != intent.offer_id || quote.policy_hash != intent.policy_hash ||
        quote.policy_hash != GetProviderPolicyHash(policy) || quote.funding_model != intent.funding_model ||
        quote.sponsorship_scope != intent.sponsorship_scope ||
        quote.sponsorship_authorization_hash != intent.sponsorship_authorization_hash) {
        error = "PAYMASTER_QUOTE_BINDING_MISMATCH";
        return false;
    }
    if ((quote.funding_model != FundingModel::USER_PAID &&
         quote.funding_model != FundingModel::SPONSORED) ||
        (quote.sponsorship_scope != SponsorshipScope::PUBLIC &&
         quote.sponsorship_scope != SponsorshipScope::RESTRICTED)) {
        error = "PAYMASTER_INVALID_FUNDING_MODEL_OR_SCOPE";
        return false;
    }
    if (TimeDeltaExceeds(quote.created_at, now, 60) || quote.expires_at <= now ||
        quote.expires_at <= quote.created_at ||
        TimeDeltaExceeds(quote.expires_at, quote.created_at, policy.quote_ttl) ||
        quote.retry_until < quote.expires_at ||
        TimeDeltaExceeds(quote.retry_until, quote.created_at, DEFAULT_RETRY_SECONDS) ||
        quote.network_fee.value <= 0 || quote.network_fee.value > policy.maximum_network_fee.value ||
        quote.reserved_dgb_inputs.empty() || quote.reserved_dgb_inputs.size() > MAX_PAYMENT_INTENT_INPUTS) {
        error = "PAYMASTER_INVALID_QUOTE_LIMITS";
        return false;
    }
    std::set<COutPoint> provider_inputs;
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        if (!ValidDGBInput(input) || !provider_inputs.insert(input.outpoint).second) {
            error = "PAYMASTER_INVALID_QUOTE_DGB_INPUT";
            return false;
        }
    }
    if (quote.reserved_carrier && (!ValidCarrier(*quote.reserved_carrier) ||
                                   !provider_inputs.insert(quote.reserved_carrier->outpoint).second)) {
        error = "PAYMASTER_INVALID_QUOTE_CARRIER";
        return false;
    }
    std::string fee_error;
    const auto expected_fee = EvaluateServiceFee(policy, quote.funding_model,
                                                 intent.recipient_amount, fee_error);
    if (!expected_fee || quote.fee_rate_bps != (quote.funding_model == FundingModel::USER_PAID ? policy.fee_rate_bps : 0) ||
        !(quote.service_fee == expected_fee->service_fee) || quote.service_fee.value < 0 ||
        maximum_service_fee.value < 0 || quote.service_fee.value > maximum_service_fee.value) {
        error = expected_fee ? "PAYMASTER_QUOTE_FEE_MISMATCH" : fee_error;
        return false;
    }
    const bool carrier_fee = quote.service_fee.value > 0 && quote.service_fee.value < 100;
    const bool normal_fee = quote.service_fee.value >= 100;
    if (quote.reserved_carrier.has_value() != carrier_fee ||
        quote.carrier_return_script.has_value() != carrier_fee ||
        quote.provider_fee_script.has_value() != normal_fee ||
        (quote.carrier_return_script && !IsP2TR(*quote.carrier_return_script)) ||
        (quote.provider_fee_script && !IsP2TR(*quote.provider_fee_script)) ||
        (quote.dgb_change_script && !IsP2TR(*quote.dgb_change_script))) {
        error = "PAYMASTER_INVALID_QUOTE_FEE_OUTPUT";
        return false;
    }
    if (quote.unsigned_txid.IsNull() || quote.template_commitment.IsNull() ||
        CTransaction{quote.unsigned_transaction}.GetHash() != quote.unsigned_txid) {
        error = "PAYMASTER_INVALID_QUOTE_TRANSACTION";
        return false;
    }
    if (quote.identity_signature.size() != 64 ||
        !provider_identity_key.VerifySchnorr(GetPaymasterQuoteSignatureHash(quote),
                                             quote.identity_signature)) {
        error = "PAYMASTER_INVALID_QUOTE_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

bool ValidatePaymasterQuoteForClient(const PaymasterQuote& quote,
                                     const PaymentIntent& intent,
                                     const OfferTerms& offer,
                                     const XOnlyPubKey& provider_identity_key,
                                     DDCents maximum_service_fee,
                                     int64_t now,
                                     std::string& error)
{
    if (quote.version != PaymasterQuote::CURRENT_VERSION ||
        quote.genesis_hash != intent.genesis_hash || quote.provider_id != intent.provider_id ||
        quote.provider_id != GetPaymasterId(provider_identity_key)) {
        error = "PAYMASTER_QUOTE_CHAIN_OR_PROVIDER_MISMATCH";
        return false;
    }
    if (quote.quote_id.IsNull() || quote.intent_hash != GetPaymentIntentHash(intent) ||
        quote.offer_id != intent.offer_id || quote.offer_id != offer.offer_id ||
        quote.policy_hash != intent.policy_hash || quote.policy_hash != offer.policy_hash ||
        quote.funding_model != intent.funding_model || quote.funding_model != offer.funding_model ||
        quote.sponsorship_scope != intent.sponsorship_scope ||
        quote.sponsorship_scope != offer.scope ||
        quote.sponsorship_authorization_hash != intent.sponsorship_authorization_hash) {
        error = "PAYMASTER_QUOTE_BINDING_MISMATCH";
        return false;
    }
    if (intent.recipient_amount.value < offer.min_payment.value ||
        intent.recipient_amount.value > offer.max_payment.value ||
        TimeDeltaExceeds(quote.created_at, now, 60) || quote.expires_at <= now ||
        quote.expires_at <= quote.created_at ||
        TimeDeltaExceeds(quote.expires_at, quote.created_at, MAX_QUOTE_TTL_SECONDS) ||
        quote.retry_until < quote.expires_at ||
        TimeDeltaExceeds(quote.retry_until, quote.created_at, DEFAULT_RETRY_SECONDS) ||
        quote.network_fee.value <= 0 || quote.reserved_dgb_inputs.empty() ||
        quote.reserved_dgb_inputs.size() > MAX_PAYMENT_INTENT_INPUTS) {
        error = "PAYMASTER_INVALID_QUOTE_LIMITS";
        return false;
    }
    std::set<COutPoint> provider_inputs;
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        if (!ValidDGBInput(input) || !provider_inputs.insert(input.outpoint).second) {
            error = "PAYMASTER_INVALID_QUOTE_DGB_INPUT";
            return false;
        }
    }
    if (quote.reserved_carrier && (!ValidCarrier(*quote.reserved_carrier) ||
                                   !provider_inputs.insert(quote.reserved_carrier->outpoint).second)) {
        error = "PAYMASTER_INVALID_QUOTE_CARRIER";
        return false;
    }
    const uint32_t expected_rate = offer.funding_model == FundingModel::USER_PAID ? offer.fee_rate_bps : 0;
    const auto computed_fee = ComputePaymasterFee(intent.recipient_amount, expected_rate);
    const DDCents expected_fee = offer.funding_model == FundingModel::SPONSORED ? DDCents{0} : computed_fee.value_or(DDCents{-1});
    std::string binding_error;
    if (!computed_fee || quote.fee_rate_bps != expected_rate ||
        !(quote.service_fee == expected_fee) || maximum_service_fee.value < 0 ||
        quote.service_fee.value > maximum_service_fee.value ||
        !ValidateSponsorshipBinding(quote.funding_model, quote.sponsorship_scope,
                                    quote.fee_rate_bps, quote.service_fee,
                                    quote.sponsorship_authorization_hash, binding_error)) {
        error = binding_error.empty() ? "PAYMASTER_QUOTE_FEE_MISMATCH" : binding_error;
        return false;
    }
    const bool carrier_fee = quote.service_fee.value > 0 && quote.service_fee.value < 100;
    const bool normal_fee = quote.service_fee.value >= 100;
    if (quote.reserved_carrier.has_value() != carrier_fee ||
        quote.carrier_return_script.has_value() != carrier_fee ||
        quote.provider_fee_script.has_value() != normal_fee ||
        (quote.carrier_return_script && !IsP2TR(*quote.carrier_return_script)) ||
        (quote.provider_fee_script && !IsP2TR(*quote.provider_fee_script)) ||
        (quote.dgb_change_script && !IsP2TR(*quote.dgb_change_script))) {
        error = "PAYMASTER_INVALID_QUOTE_FEE_OUTPUT";
        return false;
    }
    if (quote.unsigned_txid.IsNull() || quote.template_commitment.IsNull() ||
        CTransaction{quote.unsigned_transaction}.GetHash() != quote.unsigned_txid) {
        error = "PAYMASTER_INVALID_QUOTE_TRANSACTION";
        return false;
    }
    if (quote.identity_signature.size() != 64 ||
        !provider_identity_key.VerifySchnorr(GetPaymasterQuoteSignatureHash(quote),
                                             quote.identity_signature)) {
        error = "PAYMASTER_INVALID_QUOTE_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

bool PreflightProviderQuoteRequest(
    const PaymasterQuoteRequest& request,
    const ProviderPolicy& policy,
    const std::vector<CollaborativeInput>& user_inputs,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    int64_t now,
    ProviderQuotePreflightResult& result,
    std::string& error)
{
    result = {};
    const PaymentIntent& intent = request.intent;
    if (!ValidateProviderPolicy(policy, error)) return false;
    if (now <= 0 || intent.policy_hash != GetProviderPolicyHash(policy) ||
        !PolicyAllowsFundingModel(policy, intent.funding_model) ||
        policy.sponsorship_scope != intent.sponsorship_scope ||
        user_inputs.size() != intent.user_dd_inputs.size()) {
        error = "PAYMASTER_INVALID_QUOTE_BUILD_PARAMETERS";
        return false;
    }
    for (size_t index = 0; index < user_inputs.size(); ++index) {
        if (user_inputs[index].outpoint != intent.user_dd_inputs[index]) {
            error = "PAYMASTER_INVALID_USER_QUOTE_INPUT";
            return false;
        }
    }
    const uint256& expected_genesis = chain_params.GenesisBlock().GetHash();
    if (!ValidateQuoteRequestEnvelope(request, expected_genesis, now, error)) {
        return false;
    }

    ProviderQuotePreflightResult candidate;
    if (!ValidateCollaborativeUserDDInputs(user_inputs, coins,
                                           candidate.user_inputs, error) ||
        !ValidatePaymentIntent(intent, expected_genesis, now,
                               candidate.user_inputs.output_keys, error)) {
        return false;
    }

    const auto fee_plan = EvaluateServiceFee(policy, intent.funding_model,
                                             intent.recipient_amount, error);
    if (!fee_plan) return false;
    if (fee_plan->total_user_charge.value >
        candidate.user_inputs.total_dd_amount) {
        error = "PAYMASTER_INSUFFICIENT_USER_DD";
        return false;
    }
    candidate.user_change = candidate.user_inputs.total_dd_amount -
                            fee_plan->total_user_charge.value;
    const CAmount minimum_dd =
        GetMinimumDDOutput(chain_params.GetDigiDollarParams());
    if ((candidate.user_change == 0) != intent.user_dd_change_script.empty() ||
        (candidate.user_change > 0 &&
         (candidate.user_change < minimum_dd ||
          candidate.user_change > MAX_DD_OUTPUT_CENTS))) {
        error = "PAYMASTER_USER_CHANGE_MISMATCH";
        return false;
    }

    candidate.fee_plan = *fee_plan;
    result = std::move(candidate);
    error.clear();
    return true;
}

bool BuildUnsignedProviderQuote(
    const PaymasterQuoteRequest& request,
    const ProviderPolicy& policy,
    const ProviderQuoteBuildParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    ProviderQuoteBuildResult& result,
    std::string& error)
{
    PaymasterQuote empty_quote;
    CollaborativePSBTTemplate empty_template;
    result.quote = std::move(empty_quote);
    result.trusted_template = std::move(empty_template);
    const PaymentIntent& intent = request.intent;
    if (parameters.quote_id.IsNull() ||
        parameters.created_at <= 0 || parameters.expires_at <= parameters.created_at ||
        parameters.retry_until < parameters.expires_at ||
        TimeDeltaExceeds(parameters.retry_until, parameters.created_at, DEFAULT_RETRY_SECONDS)) {
        error = "PAYMASTER_INVALID_QUOTE_BUILD_PARAMETERS";
        return false;
    }
    ProviderQuotePreflightResult preflight;
    if (!PreflightProviderQuoteRequest(
            request, policy, parameters.user_inputs, chain_params, coins,
            parameters.created_at, preflight, error)) {
        return false;
    }
    if (TimeDeltaExceeds(parameters.expires_at, parameters.created_at,
                         policy.quote_ttl)) {
        error = "PAYMASTER_INVALID_QUOTE_BUILD_PARAMETERS";
        return false;
    }

    const ServiceFeePlan& fee_plan = preflight.fee_plan;
    const bool carrier_fee = fee_plan.output_kind == FeeOutputKind::CARRIER_SUCCESSOR;
    const bool provider_fee = fee_plan.output_kind == FeeOutputKind::PROVIDER_OUTPUT;
    if (parameters.reserved_carrier.has_value() != carrier_fee ||
        parameters.carrier_return_script.has_value() != carrier_fee ||
        parameters.provider_fee_script.has_value() != provider_fee ||
        (parameters.reserved_carrier && !ValidCarrier(*parameters.reserved_carrier)) ||
        parameters.reserved_dgb_inputs.empty()) {
        error = "PAYMASTER_INVALID_QUOTE_FEE_SLOT";
        return false;
    }

    CAmount dgb_total{0};
    std::set<COutPoint> provider_outpoints;
    for (const VerifiedDGBInput& input : parameters.reserved_dgb_inputs) {
        if (!ValidDGBInput(input) || !provider_outpoints.insert(input.outpoint).second ||
            input.value.value > std::numeric_limits<CAmount>::max() - dgb_total) {
            error = "PAYMASTER_INVALID_QUOTE_DGB_INPUT";
            return false;
        }
        dgb_total += input.value.value;
    }
    if (parameters.reserved_carrier &&
        !provider_outpoints.insert(parameters.reserved_carrier->outpoint).second) {
        error = "PAYMASTER_INVALID_QUOTE_CARRIER";
        return false;
    }
    if (parameters.network_fee.value <= 0 ||
        parameters.network_fee.value > policy.maximum_network_fee.value ||
        parameters.network_fee.value > dgb_total) {
        error = "PAYMASTER_INVALID_QUOTE_NETWORK_FEE";
        return false;
    }

    CollaborativeTransferParams transfer;
    transfer.inputs = parameters.user_inputs;
    if (parameters.reserved_carrier) {
        transfer.inputs.push_back({parameters.reserved_carrier->outpoint,
                                   MakeTransactionRef(parameters.reserved_carrier->creating_tx),
                                   InputRole::PROVIDER_CARRIER});
    }
    for (const VerifiedDGBInput& input : parameters.reserved_dgb_inputs) {
        transfer.inputs.push_back({input.outpoint, MakeTransactionRef(input.creating_tx),
                                   InputRole::PROVIDER_DGB});
    }
    transfer.dd_outputs.push_back(
        {intent.recipient_script, intent.recipient_amount.value, DDOutputRole::RECIPIENT});
    const CAmount user_change = preflight.user_change;
    if (user_change > 0) {
        if (intent.user_dd_change_script.empty()) {
            error = "PAYMASTER_USER_CHANGE_MISMATCH";
            return false;
        }
        transfer.dd_outputs.push_back(
            {intent.user_dd_change_script, user_change, DDOutputRole::USER_CHANGE});
    } else if (!intent.user_dd_change_script.empty()) {
        error = "PAYMASTER_USER_CHANGE_MISMATCH";
        return false;
    }
    if (carrier_fee) {
        const auto successor = ComputeCarrierSuccessor(parameters.reserved_carrier->value,
                                                       fee_plan.service_fee, error);
        if (!successor) return false;
        transfer.dd_outputs.push_back(
            {*parameters.carrier_return_script, successor->value,
             DDOutputRole::CARRIER_SUCCESSOR});
    } else if (provider_fee) {
        transfer.dd_outputs.push_back(
            {*parameters.provider_fee_script, fee_plan.service_fee.value,
             DDOutputRole::PROVIDER_FEE});
    }
    const CAmount dgb_change = dgb_total - parameters.network_fee.value;
    if (dgb_change > 0) {
        if (!parameters.dgb_change_script) {
            error = "PAYMASTER_DGB_CHANGE_MISMATCH";
            return false;
        }
        transfer.dgb_outputs.emplace_back(dgb_change, *parameters.dgb_change_script);
    } else if (parameters.dgb_change_script) {
        error = "PAYMASTER_DGB_CHANGE_MISMATCH";
        return false;
    }
    transfer.fee_rate = 1;
    const auto built = BuildUnsignedCollaborativeTransfer(transfer, chain_params, coins);
    if (!built.success) {
        error = built.error;
        return false;
    }

    PaymasterQuote quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = parameters.quote_id;
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.funding_model = intent.funding_model;
    quote.sponsorship_scope = intent.sponsorship_scope;
    quote.sponsorship_authorization_hash = intent.sponsorship_authorization_hash;
    quote.fee_rate_bps = intent.funding_model == FundingModel::USER_PAID ? policy.fee_rate_bps : 0;
    quote.service_fee = fee_plan.service_fee;
    quote.reserved_carrier = parameters.reserved_carrier;
    quote.reserved_dgb_inputs = parameters.reserved_dgb_inputs;
    quote.carrier_return_script = parameters.carrier_return_script;
    quote.provider_fee_script = parameters.provider_fee_script;
    quote.dgb_change_script = parameters.dgb_change_script;
    quote.network_fee = parameters.network_fee;
    quote.created_at = parameters.created_at;
    quote.expires_at = parameters.expires_at;
    quote.retry_until = parameters.retry_until;
    quote.unsigned_transaction = built.tx;
    quote.unsigned_txid = CTransaction{built.tx}.GetHash();

    CollaborativePSBTTemplate trusted;
    if (!CreateCollaborativePSBTTemplate(built.tx, transfer.inputs, trusted, error)) return false;
    quote.template_commitment = GetCollaborativeTemplateCommitment(intent, quote, trusted);
    result.quote = std::move(quote);
    result.trusted_template = std::move(trusted);
    error.clear();
    return true;
}

bool ValidatePaymasterResult(const PaymasterResult& result,
                             const uint256& expected_genesis,
                             const PaymasterId& expected_provider,
                             const uint256& expected_commit_key,
                             const XOnlyPubKey& provider_identity_key,
                             uint64_t minimum_sequence,
                             std::string& error)
{
    if (result.version != PaymasterResult::CURRENT_VERSION || result.genesis_hash != expected_genesis ||
        result.provider_id != expected_provider || result.provider_id != GetPaymasterId(provider_identity_key) ||
        result.commit_key != expected_commit_key) {
        error = "PAYMASTER_RESULT_BINDING_MISMATCH";
        return false;
    }
    if (!ValidatePaymasterResultShape(result, error)) return false;
    if (result.result_sequence < minimum_sequence) {
        error = "PAYMASTER_INVALID_RESULT_SEQUENCE";
        return false;
    }
    if (!provider_identity_key.VerifySchnorr(GetPaymasterResultSignatureHash(result),
                                             result.identity_signature)) {
        error = "PAYMASTER_INVALID_RESULT_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

bool ValidatePaymasterResultShape(const PaymasterResult& result, std::string& error)
{
    if (result.version != PaymasterResult::CURRENT_VERSION || result.genesis_hash.IsNull() ||
        result.provider_id.IsNull() || result.commit_key.IsNull() || result.result_sequence == 0 ||
        result.updated_at <= 0) {
        error = "PAYMASTER_INVALID_RESULT_SEQUENCE";
        return false;
    }
    if (result.status != PaymasterResultStatus::NO_FINAL_COMMIT &&
        result.status != PaymasterResultStatus::USER_PSBT_ACCEPTED &&
        result.status != PaymasterResultStatus::FINAL_COMMITTED &&
        result.status != PaymasterResultStatus::BROADCAST_ATTEMPTED &&
        result.status != PaymasterResultStatus::SLOT_UNAVAILABLE &&
        result.status != PaymasterResultStatus::REJECTED) {
        error = "PAYMASTER_INVALID_RESULT_STATUS";
        return false;
    }
    const bool has_all_final = result.txid && result.raw_transaction_hash && result.final_transaction;
    const bool has_any_final = result.txid || result.raw_transaction_hash || result.final_transaction;
    const bool requires_final = result.status == PaymasterResultStatus::FINAL_COMMITTED ||
                                result.status == PaymasterResultStatus::BROADCAST_ATTEMPTED;
    if ((has_any_final && !has_all_final) || requires_final != has_all_final) {
        error = "PAYMASTER_INVALID_RESULT_ARTIFACTS";
        return false;
    }
    if (has_all_final) {
        const CTransaction transaction{*result.final_transaction};
        if (transaction.GetHash() != *result.txid || transaction.GetWitnessHash() != *result.raw_transaction_hash) {
            error = "PAYMASTER_INVALID_RESULT_TRANSACTION";
            return false;
        }
    }
    if (result.identity_signature.size() != 64) {
        error = "PAYMASTER_INVALID_RESULT_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

} // namespace DigiDollar::Paymaster
