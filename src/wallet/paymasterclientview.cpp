// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <wallet/paymasterclientview.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>
#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/validation.h>
#include <random.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/spend.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <algorithm>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;
bool FindClientSession(const ClientSessionLookup& lookup, PaymasterStore& store, PaymentSession& session)
{
    DatabaseReadStatus status;
    if (const auto* request_id = std::get_if<std::string>(&lookup)) {
        if (!IsCanonicalRequestId(*request_id))
            throw ApplicationError(ErrorKind::INVALID_INPUT, "request_id must be a canonical lowercase UUID");
        status = store.GetSessionByRequestIdWithStatus(*request_id, session);
    } else {
        status = store.GetSessionBySessionIdWithStatus(std::get<uint256>(lookup), session);
    }
    if (status == DatabaseReadStatus::FOUND) return true;
    if (status == DatabaseReadStatus::NOT_FOUND) return false;
    throw ApplicationError(ErrorKind::WALLET, status == DatabaseReadStatus::UNSUPPORTED_VERSION
        ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_SESSION_READ_FAILED");
}

bool LoadClientIntentDisplayBinding(const PaymentSession& session,
                                    const ProviderAttempt& attempt,
                                    std::string& recipient_address,
                                    std::string& error)
{
    PaymentIntent intent;
    try {
        SpanReader stream{::PROTOCOL_VERSION, attempt.unsigned_intent};
        stream >> intent;
        if (!stream.empty() ||
            SerializePaymentIntent(intent) != attempt.unsigned_intent) {
            throw std::ios_base::failure(
                "non-canonical persisted payment intent");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PERSISTED_INTENT_CORRUPT";
        return false;
    }

    if (intent.version != PaymentIntent::CURRENT_VERSION ||
        intent.genesis_hash != Params().GenesisBlock().GetHash() ||
        attempt.provider_id.IsNull() ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != attempt.provider_id ||
        intent.request_id != session.request_id ||
        intent.session_id != session.session_id ||
        intent.canonical_request_hash != session.canonical_request_hash ||
        intent.requested_fee_mode != session.fee_mode_requested ||
        intent.provider_id != attempt.provider_id ||
        intent.client_nonce != attempt.client_nonce ||
        intent.user_dd_inputs != session.user_inputs ||
        intent.privacy_profile != attempt.privacy_profile ||
        intent.sponsorship_authorization_hash !=
            attempt.sponsorship_capability_hash ||
        (intent.funding_model != FundingModel::USER_PAID &&
         intent.funding_model != FundingModel::SPONSORED) ||
        (intent.sponsorship_scope != SponsorshipScope::PUBLIC &&
         intent.sponsorship_scope != SponsorshipScope::RESTRICTED) ||
        (intent.requested_fee_mode != FeeMode::PAYMASTER &&
         intent.requested_fee_mode != FeeMode::AUTO) ||
        (intent.privacy_profile != PrivacyProfile::STANDARD &&
         intent.privacy_profile != PrivacyProfile::HIGH) ||
        (intent.selection_mode != SelectionMode::LOWEST_TOTAL_COST &&
         intent.selection_mode != SelectionMode::PRIVACY_WEIGHTED) ||
        intent.offer_id.IsNull() || intent.policy_hash.IsNull() ||
        intent.user_dd_inputs.empty() ||
        intent.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        !intent.user_input_proofs.empty() ||
        intent.recipient_amount.value < 100 ||
        intent.recipient_amount.value > MAX_DD_OUTPUT_CENTS ||
        attempt.created_at <= 0 || intent.expires_at <= attempt.created_at ||
        TimeDeltaExceeds(intent.expires_at, attempt.created_at,
                         MAX_DIRECT_MESSAGE_TTL_SECONDS)) {
        error = "PAYMASTER_PERSISTED_INTENT_BINDING_MISMATCH";
        return false;
    }
    std::set<COutPoint> unique_inputs;
    if (std::any_of(intent.user_dd_inputs.begin(), intent.user_dd_inputs.end(),
                    [&](const COutPoint& input) {
                        return input.IsNull() ||
                            !unique_inputs.insert(input).second;
                    })) {
        error = "PAYMASTER_PERSISTED_INTENT_BINDING_MISMATCH";
        return false;
    }
    std::string sponsorship_error;
    if (!ValidateSponsorshipBinding(
            intent.funding_model, intent.sponsorship_scope,
            /*fee_rate_bps=*/0, DDCents{0},
            intent.sponsorship_authorization_hash, sponsorship_error)) {
        error = sponsorship_error.empty()
            ? "PAYMASTER_PERSISTED_INTENT_BINDING_MISMATCH"
            : sponsorship_error;
        return false;
    }

    const ClientAuthorizationManifest& manifest = attempt.client_manifest;
    if (!manifest.manifest_id.IsNull() &&
        (manifest.version != ClientAuthorizationManifest::CURRENT_VERSION ||
         GetClientAuthorizationManifestId(manifest) != manifest.manifest_id ||
         manifest.request_id != intent.request_id ||
         manifest.session_id != intent.session_id ||
         manifest.provider_id != intent.provider_id ||
         manifest.offer_id != intent.offer_id ||
         manifest.policy_hash != intent.policy_hash ||
         manifest.user_dd_inputs != intent.user_dd_inputs ||
         manifest.recipient_script != intent.recipient_script ||
         manifest.recipient_amount != intent.recipient_amount ||
         manifest.canonical_request_hash != intent.canonical_request_hash ||
         manifest.requested_fee_mode != intent.requested_fee_mode ||
         manifest.privacy_profile != intent.privacy_profile)) {
        error = "PAYMASTER_PERSISTED_MANIFEST_BINDING_MISMATCH";
        return false;
    }

    const auto address = DigiDollarAddressForScript(intent.recipient_script);
    if (!address ||
        (!intent.user_dd_change_script.empty() &&
         !DigiDollarAddressForScript(intent.user_dd_change_script))) {
        error = "PAYMASTER_PERSISTED_RECIPIENT_INVALID";
        return false;
    }
    recipient_address = *address;
    return true;
}

bool IsUnsignedAttempt(const ProviderAttempt& attempt)
{
    return (attempt.state == AttemptState::CANDIDATE ||
            attempt.state == AttemptState::QUOTED ||
            attempt.state == AttemptState::REJECTED ||
            attempt.state == AttemptState::QUOTE_EXPIRED) &&
           attempt.user_signed_psbt.empty() &&
           attempt.final_transaction.empty() && attempt.final_txid.IsNull() &&
           attempt.provider_signed_at == 0 &&
           attempt.provider_signed_result.empty();
}

bool LoadClientSessionView(PaymasterStore& store,
                           const PaymentSession& session,
                           int64_t now,
                           ClientSessionView& view,
                           std::string& error)
{
    view = {};
    view.observed_at = now;
    error.clear();
    if (session.provider_side) {
        error = "PAYMASTER_CLIENT_SESSION_REQUIRED";
        return false;
    }

    std::vector<ProviderAttempt> attempts;
    attempts.reserve(session.attempt_ids.size());
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!store.GetAttempt(attempt_id, attempt) ||
            attempt.attempt_id != attempt_id ||
            attempt.session_id != session.session_id) {
            error = "PAYMASTER_SESSION_ATTEMPT_DATABASE_READ";
            return false;
        }
        if (!IsUnsignedAttempt(attempt)) view.authorization_may_exist = true;
        attempts.push_back(std::move(attempt));
    }
    if (!attempts.empty()) view.attempt = attempts.back();

    for (const ProviderAttempt& attempt : attempts) {
        std::string recipient_address;
        if (!LoadClientIntentDisplayBinding(
                session, attempt, recipient_address, error)) {
            return false;
        }
        if (view.recipient_address &&
            *view.recipient_address != recipient_address) {
            error = "PAYMASTER_SESSION_RECIPIENT_CONFLICT";
            return false;
        }
        view.recipient_address = std::move(recipient_address);
    }

    if (view.attempt && !view.attempt->commit_key.IsNull()) {
        PaymasterResult provider_result;
        if (store.GetProviderResult(view.attempt->commit_key,
                                    provider_result)) {
            if (!ValidatePaymasterResult(
                    provider_result, Params().GenesisBlock().GetHash(),
                    view.attempt->provider_id,
                    view.attempt->commit_key,
                    view.attempt->provider_identity_key, 1, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_PERSISTED_RESULT_CORRUPT";
                }
                return false;
            }
            view.provider_result = std::move(provider_result);
        }
    }

    bool has_live_reservations{false};
    if (!store.ClientSessionHasLiveReservations(
            session, has_live_reservations, error)) {
        return false;
    }
    if (has_live_reservations) view.authorization_may_exist = true;

    AlternativeRecoveryRecord recovery;
    if (store.GetAlternativeRecovery(session.request_id, recovery)) {
        if (recovery.session_id != session.session_id) {
            error = "PAYMASTER_RECOVERY_SESSION_CONFLICT";
            return false;
        }
        view.recovery = std::move(recovery);
        view.artifact = "alternative_recovery";
        view.authorization_may_exist = true;
    } else if (view.attempt &&
               (!view.attempt->final_transaction.empty() ||
                !view.attempt->final_txid.IsNull())) {
        // The client-side attempt is the durable authority for a received
        // provider result. ProviderCommitRecord belongs to the provider
        // wallet and is therefore not required for this classification.
        if (view.attempt->final_transaction.empty() ||
            view.attempt->final_txid.IsNull()) {
            error = "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT";
            return false;
        }
        try {
            CMutableTransaction decoded;
            SpanReader stream{::PROTOCOL_VERSION,
                              view.attempt->final_transaction};
            stream >> decoded;
            if (!stream.empty() ||
                SerializeTransaction(decoded) !=
                    view.attempt->final_transaction ||
                CTransaction{decoded}.GetHash() !=
                    view.attempt->final_txid) {
                error = "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT";
                return false;
            }
            const bool final_result_status =
                view.provider_result &&
                (view.provider_result->status ==
                     PaymasterResultStatus::FINAL_COMMITTED ||
                 view.provider_result->status ==
                     PaymasterResultStatus::BROADCAST_ATTEMPTED);
            view.has_retriable_final =
                final_result_status && view.provider_result->txid &&
                *view.provider_result->txid ==
                    view.attempt->final_txid &&
                view.provider_result->raw_transaction_hash &&
                *view.provider_result->raw_transaction_hash ==
                    CTransaction{decoded}.GetWitnessHash() &&
                view.provider_result->final_transaction &&
                SerializeTransaction(
                    *view.provider_result->final_transaction) ==
                    view.attempt->final_transaction;
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_PERSISTED_FINAL_ARTIFACT_CORRUPT";
            return false;
        }
        view.artifact = "final_transaction";
        view.authorization_may_exist = true;
    } else if (view.attempt && !view.attempt->user_signed_psbt.empty()) {
        view.artifact = "user_psbt";
        view.authorization_may_exist = true;
    }

    view.allowed_actions.push_back("refresh");
    if (!IsTerminal(session.state) && !view.recovery) {
        const bool early_session =
            session.state == SessionState::CREATED ||
            session.state == SessionState::INPUTS_RESERVED ||
            session.state == SessionState::AWAITING_WALLET_UNLOCK ||
            session.state == SessionState::AWAITING_USER_SIGNATURE ||
            session.state == SessionState::AUTHORIZED;
        if (view.artifact == "none" && early_session) {
            // Resume is deliberately a capability token for the idempotent
            // send RPC, not a new resolvepaymastersession mutation.
            view.allowed_actions.push_back("resume");
        }
        if (view.attempt && view.artifact == "none" &&
            (view.attempt->state == AttemptState::CANDIDATE ||
             view.attempt->state == AttemptState::QUOTED ||
             view.attempt->state == AttemptState::QUOTE_EXPIRED) &&
            (session.state == SessionState::INPUTS_RESERVED ||
             session.state == SessionState::AWAITING_WALLET_UNLOCK ||
             session.state == SessionState::AWAITING_USER_SIGNATURE ||
             session.state == SessionState::AUTHORIZED)) {
            view.allowed_actions.push_back("fallback");
        }
        if (view.artifact == "none" &&
            (session.state == SessionState::CREATED ||
             session.state == SessionState::INPUTS_RESERVED ||
             session.state == SessionState::AWAITING_WALLET_UNLOCK ||
             session.state == SessionState::AWAITING_USER_SIGNATURE) &&
            std::all_of(attempts.begin(), attempts.end(), IsUnsignedAttempt)) {
            view.allowed_actions.push_back("abandon_unsigned");
        }
        if (view.attempt && view.artifact == "user_psbt" &&
            (view.attempt->retry_until <= 0 || now <= view.attempt->retry_until)) {
            view.allowed_actions.push_back("retry_same");
        } else if (view.has_retriable_final) {
            view.allowed_actions.push_back("retry_same");
        }
    }

    const bool resumable_recovery =
        view.recovery && !view.recovery->expired &&
        !IsTerminal(session.state);
    const bool recoverable_original =
        !view.recovery && view.attempt &&
        session.state == SessionState::PENDING_PROVIDER &&
        !view.attempt->commit_key.IsNull() &&
        !view.attempt->template_commitment.IsNull() &&
        ((view.attempt->state >= AttemptState::USER_SIGNED &&
          view.attempt->state <= AttemptState::MEMPOOL) ||
         view.attempt->state == AttemptState::AMBIGUOUS);
    if (resumable_recovery || recoverable_original) {
        view.allowed_actions.push_back("cancel_to_self");
    }

    view.requires_attention =
        session.state == SessionState::CONFLICTED ||
        (session.state == SessionState::FAILED &&
         view.authorization_may_exist) ||
        (view.artifact == "final_transaction" &&
         !view.has_retriable_final) ||
        (view.recovery && !IsTerminal(session.state));
    return true;
}

UniValue AllowedActionsToJSON(const ClientSessionView& view)
{
    UniValue actions{UniValue::VARR};
    for (const std::string& action : view.allowed_actions) {
        actions.push_back(action);
    }
    return actions;
}

UniValue ClientSessionSnapshotToJSON(PaymasterStore& store,
                                     const PaymentSession& session,
                                     std::string_view action)
{
    ClientSessionView view;
    std::string error;
    if (!LoadClientSessionView(store, session, GetTime(), view, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("action", std::string{action});
    UniValue session_result = SessionToJSON(session, &store);
    if (view.recipient_address && !session_result.exists("to_address")) {
        session_result.pushKV("to_address", *view.recipient_address);
    }
    result.pushKV("session", std::move(session_result));
    if (view.attempt) {
        result.pushKV("attempt", AttemptToJSON(*view.attempt));
    } else {
        result.pushKV("attempt", UniValue{UniValue::VNULL});
    }
    result.pushKV("artifact", view.artifact);
    if (view.recovery) {
        result.pushKV("recovery", AlternativeRecoveryToJSON(*view.recovery));
    } else {
        result.pushKV("recovery", UniValue{UniValue::VNULL});
    }
    if (view.provider_result) {
        result.pushKV("result_status",
                      ResultStatusName(view.provider_result->status));
        result.pushKV("result_sequence",
                      view.provider_result->result_sequence);
    } else {
        result.pushKV("result_status", UniValue{UniValue::VNULL});
        result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    }
    result.pushKV("requires_attention", view.requires_attention);
    result.pushKV("allowed_actions", AllowedActionsToJSON(view));
    result.pushKV("observed_at", view.observed_at);
    return result;
}

UniValue ClientSessionSummaryToJSON(const PaymasterStore& store,
                                    const PaymentSession& session,
                                    const ClientSessionView& view)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", session.request_id);
    result.pushKV("session_id", session.session_id.GetHex());
    if (session.requested_amount.value > 0) {
        result.pushKV("requested_amount_cents", session.requested_amount.value);
    }
    result.pushKV("requested_fee_mode", FeeModeName(session.fee_mode_requested));
    result.pushKV("fee_mode_used", FeeModeName(session.fee_mode_used));
    result.pushKV("session_state", std::string{SessionStateName(session.state)});
    if (session.pending_phase != PendingPhase::NONE) {
        result.pushKV("pending_phase", std::string{PendingPhaseName(session.pending_phase)});
    }
    result.pushKV("created_at", session.created_at);
    result.pushKV("updated_at", session.updated_at);
    result.pushKV("provider_attempts",
                  static_cast<uint64_t>(session.attempt_ids.size()));
    if (!session.final_txid.IsNull()) {
        result.pushKV("txid", session.final_txid.GetHex());
    }
    if (!session.recovery_txid.IsNull()) {
        result.pushKV("recovery_txid", session.recovery_txid.GetHex());
    }
    AddSessionPaymentStatus(result, session, &store);
    result.pushKV("artifact", view.artifact);
    result.pushKV("requires_attention", view.requires_attention);
    result.pushKV("allowed_actions", AllowedActionsToJSON(view));
    result.pushKV("observed_at", view.observed_at);
    return result;
}

bool IsActiveClientSession(const PaymentSession& session,
                           const ClientSessionView& view)
{
    if (session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE) {
        return false;
    }
    if (session.state == SessionState::FAILED &&
        !view.authorization_may_exist) {
        return false;
    }
    return true;
}

} // namespace wallet::paymaster_app
