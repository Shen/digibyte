// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <wallet/paymasterservice.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>
#include <wallet/paymasterapplication.h>

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
#include <logging/timer.h>
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
#include <paymaster/setup.h>
#include <paymaster/validation.h>
#include <random.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasterclientview.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/spend.h>

#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <variant>



namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;

namespace {
// These native failures occur before accepting new user/provider authority.
// Network processing may classify them again after a fresh durable reload;
// RPC callers retain the existing ApplicationError category and message.
class ProviderSubmitExpired final : public ApplicationError
{
public:
    explicit ProviderSubmitExpired(const char* message)
        : ApplicationError{ErrorKind::WALLET, message} {}
};

bool LoadProviderSubmitAuthority(const ProviderAttempt& attempt,
    PaymentIntent& intent, PaymasterQuote& quote,
    CollaborativePSBTTemplate& trusted, std::string& error)
{
    return LoadAttemptAuthorizationArtifacts(attempt, intent, quote, trusted, error) &&
           ValidateCollaborativePSBT(trusted.psbt, trusted, CollaborativeSignatureStage::UNSIGNED, error) &&
           ValidateProviderAuthorizationManifest(attempt.provider_manifest, intent, quote, trusted, error);
}

ProviderProcessingResult RejectProviderMessage(
    Manager& manager, const DirectMessage& message,
    const std::string& reason, const std::string& message_type)
{
    std::string error;
    if (!AcknowledgeRejectedDirectMessage(manager, message, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    ProviderProcessingResult result;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.queued = false;
    result.disposition = ProviderProcessingDisposition::REJECTED;
    result.rejection_reason = reason;
    result.message_type = message_type;
    return result;
}

// The caller has authenticated local quote/manifest artifacts and verified the
// incoming USER signatures. Reject only established peer defects: persisted
// signatures, commits and unknown states remain local authority to reconcile.
std::optional<std::string> GetPermanentProviderSubmitRejection(
    CWallet& wallet, const PaymentSession& session, const ProviderAttempt& attempt,
    const PartiallySignedTransaction& submitted, const CollaborativePSBTTemplate& trusted,
    int64_t now)
{
    std::optional<std::string> reason;
    switch (attempt.state) {
    case AttemptState::QUOTED:
        if (now > attempt.retry_until) reason = "PAYMASTER_SUBMIT_RETRY_EXPIRED";
        else if (now > attempt.quote_expires_at) reason = "PAYMASTER_SUBMIT_QUOTE_EXPIRED";
        break;
    case AttemptState::USER_SIGNED:
    case AttemptState::USER_PSBT_ACCEPTED:
        if (now > attempt.retry_until) reason = "PAYMASTER_SUBMIT_RETRY_EXPIRED";
        break;
    case AttemptState::QUOTE_EXPIRED:
        reason = "PAYMASTER_SUBMIT_QUOTE_EXPIRED";
        break;
    case AttemptState::PROVIDER_SIGNED:
    case AttemptState::FINAL_COMMITTED:
    case AttemptState::BROADCAST:
    case AttemptState::STEMPOOL:
    case AttemptState::MEMPOOL:
    case AttemptState::REJECTED:
        break;
    default:
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_AUTHORIZATION_STATE");
    }

    const auto canonical_submitted = SerializePSBT(submitted);
    if (!attempt.user_signed_psbt.empty() && attempt.user_signed_psbt != canonical_submitted) {
        PartiallySignedTransaction persisted;
        std::string error;
        if (!DecodeRawPSBT(persisted, MakeByteSpan(attempt.user_signed_psbt), error) ||
            SerializePSBT(persisted) != attempt.user_signed_psbt ||
            !ValidateCollaborativePSBT(persisted, trusted, CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PERSISTED_USER_PSBT_INVALID");
        }
        reason = "PAYMASTER_USER_PSBT_CONFLICT";
    }
    if (!reason) return std::nullopt;

    const bool user_authority = attempt.state >= AttemptState::USER_SIGNED &&
                                attempt.state <= AttemptState::MEMPOOL;
    const bool provider_authority = attempt.state >= AttemptState::PROVIDER_SIGNED &&
                                    attempt.state <= AttemptState::MEMPOOL;
    const bool unsigned_quote = attempt.state == AttemptState::QUOTED ||
                                attempt.state == AttemptState::QUOTE_EXPIRED;
    if (!session.provider_side ||
        (user_authority && attempt.user_signed_psbt.empty()) ||
        (unsigned_quote && !attempt.user_signed_psbt.empty()) ||
        (!provider_authority && (!attempt.final_txid.IsNull() || !attempt.final_transaction.empty() ||
                                 attempt.provider_signed_at != 0 || !attempt.provider_signed_result.empty()))) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_AUTHORIZATION_STATE");
    }

    ProviderCommitRecord commit;
    UserAuthorizationRecord authorization;
    PaymasterResult result;
    DatabaseReadStatus commit_status;
    DatabaseReadStatus authorization_status;
    DatabaseReadStatus result_status;
    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        commit_status = batch.ReadPaymasterProviderCommitWithStatus(attempt.commit_key, commit);
        authorization_status = batch.ReadPaymasterUserAuthorizationWithStatus(attempt.commit_key, authorization);
        result_status = batch.ReadPaymasterResultWithStatus(attempt.commit_key, result);
    }
    const auto readable = [](DatabaseReadStatus status) {
        return status == DatabaseReadStatus::FOUND || status == DatabaseReadStatus::NOT_FOUND;
    };
    if (!readable(commit_status) || !readable(authorization_status) || !readable(result_status)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_AUTHORIZATION_DATABASE_READ");
    }
    if ((unsigned_quote && (authorization_status != DatabaseReadStatus::NOT_FOUND ||
                            result_status != DatabaseReadStatus::NOT_FOUND)) ||
        (user_authority && attempt.state != AttemptState::USER_SIGNED &&
         authorization_status != DatabaseReadStatus::FOUND) ||
        (authorization_status == DatabaseReadStatus::FOUND &&
         (authorization.attempt_id != attempt.attempt_id ||
          authorization.canonical_psbt_hash != Hash(attempt.user_signed_psbt) ||
          authorization.retry_until != attempt.retry_until))) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_AUTHORIZATION_STATE");
    }
    std::string error;
    if (result_status == DatabaseReadStatus::FOUND &&
        (!ValidatePaymasterResult(result, Params().GenesisBlock().GetHash(), attempt.provider_id,
                                  attempt.commit_key, attempt.provider_identity_key, 1, error) ||
         (result.final_transaction && !provider_authority))) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_PERSISTED_RESULT_INVALID");
    }
    if (commit_status == DatabaseReadStatus::FOUND) {
        CMutableTransaction final;
        if (!ValidateProviderCommitForExecution(attempt, commit,
                std::max(now, commit.committed_at), final, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    } else if (attempt.state == AttemptState::PROVIDER_SIGNED) {
        PaymasterResult signed_result;
        if (!paymaster_store::internal::DecodeProviderSignedResultEnvelope(attempt, signed_result, error) ||
            signed_result.genesis_hash != Params().GenesisBlock().GetHash() ||
            !ValidateFinalCollaborativeTransaction(*signed_result.final_transaction,
                *signed_result.txid, *signed_result.raw_transaction_hash, trusted, error)) {
            throw ApplicationError(ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID" : error);
        }
    } else if (provider_authority) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_PROVIDER_COMMIT_MISSING");
    }
    return reason;
}
} // namespace

PaymentResult SubmitPaymasterTransaction(CWallet& target, PartiallySignedTransaction user_psbt, ExecutionMode mode)
{
    CWallet* const wallet = &target;
    using namespace DigiDollar::Paymaster;
    // processpaymastersubmits invokes this handler on the validation
    // scheduler. A new tip can arrive after its initial readiness
    // check; waiting here would deadlock that same notification queue.
    const bool automatic_service = mode == ExecutionMode::AUTOMATIC;
    const ExactFinalTxIndexMode txindex_mode = automatic_service
        ? ExactFinalTxIndexMode::NONBLOCKING
        : ExactFinalTxIndexMode::WAIT_FOR_SYNC;
    if (automatic_service) {
        if (!AutomaticProviderStateIsSynchronized(*wallet)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_SYNCING");
        }
    } else {
        wallet->BlockUntilSyncedToCurrentChain();
    }

    std::string error;
    if (!user_psbt.tx) {
        throw ApplicationError(ErrorKind::DESERIALIZATION,
                           strprintf("Paymaster PSBT decode failed: %s", error));
    }
    const uint256 unsigned_txid = CTransaction{*user_psbt.tx}.GetHash();
    PaymasterStore store{*wallet};
    DigiDollar::Paymaster::ProviderAttempt attempt;
    if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
        throw ApplicationError(ErrorKind::WALLET, "No persistent Paymaster template matches this PSBT");
    }
    DigiDollar::Paymaster::ProviderIdentityRecord provider_identity;
    if (!CheckPaymasterProviderWallet(*wallet, error) ||
        !GetPaymasterIdentity(*wallet, provider_identity) ||
        provider_identity.provider_id != attempt.provider_id) {
        throw ApplicationError(ErrorKind::WALLET,
                           error.empty() ? "Paymaster provider identity does not match the quote" : error);
    }
    DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
    if (!LoadTrustedTemplate(attempt, trusted, error) ||
        !DigiDollar::Paymaster::ValidateCollaborativePSBT(
            user_psbt, trusted,
            DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }
    const std::vector<unsigned char> canonical_user_psbt = SerializePSBT(user_psbt);
    if (!attempt.user_signed_psbt.empty() && attempt.user_signed_psbt != canonical_user_psbt) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_USER_PSBT_CONFLICT");
    }

    DigiDollar::Paymaster::PaymentSession session;
    if (!store.GetSessionBySessionId(attempt.session_id, session)) {
        throw ApplicationError(ErrorKind::WALLET, "Paymaster session not found");
    }
    const int64_t now = GetTime();
    DigiDollar::Paymaster::ProviderCommitRecord committed;
    const bool already_committed = !attempt.commit_key.IsNull() &&
                                   store.GetProviderCommit(attempt.commit_key, committed);
    const bool exact_persisted_provider_signature =
        attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
        !attempt.final_txid.IsNull() &&
        !attempt.final_transaction.empty() &&
        attempt.provider_signed_at > 0 &&
        attempt.provider_signed_at <= attempt.retry_until &&
        !attempt.provider_signed_result.empty();
    if (!already_committed && !exact_persisted_provider_signature &&
        now > attempt.retry_until) {
        throw ProviderSubmitExpired{"Paymaster retry window expired"};
    }
    if (!already_committed && attempt.state == AttemptState::QUOTE_EXPIRED) {
        // Reconciliation can expire an unsigned quote between the network
        // precheck and this reload. Only proven commit absence is terminal;
        // unreadable or unexpectedly present authority must remain local.
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        ProviderCommitRecord current_commit;
        if (batch.ReadPaymasterProviderCommitWithStatus(attempt.commit_key, current_commit) !=
            DatabaseReadStatus::NOT_FOUND) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_AUTHORIZATION_DATABASE_READ");
        }
        throw ProviderSubmitExpired{"Paymaster attempt is not recoverable by exact provider retry"};
    }
    if (!already_committed && !exact_persisted_provider_signature) {
        PaymentIntent authorized_intent;
        PaymasterQuote authorized_quote;
        CollaborativePSBTTemplate authorized_template;
        if (!LoadAttemptAuthorizationArtifacts(
                attempt, authorized_intent, authorized_quote,
                authorized_template, error) ||
            attempt.provider_manifest.manifest_id.IsNull() ||
            now > attempt.provider_manifest.expires_at ||
            !ValidateProviderAuthorizationManifest(
                attempt.provider_manifest, authorized_intent,
                authorized_quote, authorized_template, error)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_EXPIRED" : error);
        }
        if (!ValidateProviderAuthorizationOwnership(
                *wallet, attempt.provider_manifest, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }
    if (!already_committed &&
        attempt.state != DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
        wallet->IsLocked()) {
        throw ApplicationError(ErrorKind::UNLOCK,
                           "Provider wallet unlock is required for a new Paymaster signature");
    }

    if (!already_committed) {
        bool rejected{false};
        DigiDollar::Paymaster::PaymasterResult rejection;
        if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *user_psbt.tx,
                                             now, rejected, rejection, error)) {
            throw ApplicationError(
                error == "PAYMASTER_WALLET_LOCKED" ? ErrorKind::UNLOCK : ErrorKind::WALLET,
                error);
        }
        if (rejected) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE");
        }
    }

    if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::QUOTED) {
        if (now > attempt.quote_expires_at) {
            throw ProviderSubmitExpired{"Paymaster quote expired"};
        }
        if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
            session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
            session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) {
            if (!store.TransitionSession(session.request_id,
                                         DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                         DigiDollar::Paymaster::PendingPhase::NONE, {}, now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
        } else if (session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED &&
                   session.state != DigiDollar::Paymaster::SessionState::PENDING_PROVIDER) {
            throw ApplicationError(ErrorKind::WALLET,
                               "Paymaster session cannot accept a user authorization");
        }
        attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
        attempt.user_signed_psbt = canonical_user_psbt;
        attempt.updated_at = std::max(attempt.updated_at, now);
        if (!store.UpdateAttempt(session.request_id, attempt, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_SIGNED) {
        if (!store.AcceptUserAuthorization(session.request_id, attempt.attempt_id,
                                           Hash(canonical_user_psbt), now, error) ||
            !store.GetAttempt(attempt.attempt_id, attempt)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    PartiallySignedTransaction full_psbt{user_psbt};
    bool signed_now{false};
    if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_PSBT_ACCEPTED) {
        ProviderAttempt signing_attempt;
        CollaborativePSBTTemplate signing_template;
        if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
            signing_attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
            !ValidateDurableProviderUserAuthorization(
                store, signing_attempt, error) ||
            !ValidateProviderAuthorizationForExecution(
                signing_attempt, now, signing_template, error) ||
            !ValidateCollaborativePSBT(
                full_psbt, signing_template,
                CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_STATE" : error);
        }
        attempt = std::move(signing_attempt);
        node::NodeContext* node_ctx = wallet->chain().context();
        PaymasterCapacityRequest capacity_request;
        PaymasterCapacityProof capacity_proof;
        if (!node_ctx || !node_ctx->chainman) {
            throw ApplicationError(
                ErrorKind::INTERNAL,
                "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
        }
        if (!store.ValidateProviderPreSignatureAuthorization(
                attempt, Params().GenesisBlock().GetHash(), now,
                capacity_request, capacity_proof, error) ||
            !ValidateCapacityProofAgainstChainstate(
                capacity_proof, capacity_request,
                attempt.provider_identity_key, *node_ctx->chainman,
                now, error) ||
            !TemplateInputsAvailable(*wallet, *full_psbt.tx)) {
            throw ApplicationError(
                ErrorKind::REJECTED,
                error.empty() ? "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE" : error);
        }
        if (!ValidateProviderAuthorizationOwnership(
                *wallet, attempt.provider_manifest, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!SignCollaborativePSBTForParty(*wallet, full_psbt,
                                           signing_template,
                                           DigiDollar::Paymaster::SigningParty::PROVIDER, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        CMutableTransaction final_transaction;
        if (!FinalizeAndExtractPSBT(full_psbt, final_transaction)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_FINALIZATION_FAILED");
        }
        const CTransaction signed_transaction{final_transaction};
        if (!ValidateFinalCollaborativeTransaction(
                final_transaction, signed_transaction.GetHash(),
                signed_transaction.GetWitnessHash(), signing_template,
                error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        // The signed result below is private crash-recovery authority.
        // Do not persist even that envelope until the exact transaction
        // has passed the shared script, witness, chain and mempool
        // firewall. The second preflight before the atomic commit stays
        // in place to close the race after this observation.
        FinalTransactionPresence signed_presence{
            FinalTransactionPresence::NONE};
        if (!PreflightFinalPaymasterTransaction(
                *wallet, MakeTransactionRef(final_transaction),
                signed_presence, error, txindex_mode)) {
            const bool transient = ClassifyPaymasterFinalizationFailure(
                error, PaymasterFinalizationStage::BEFORE_PROVIDER_COMMIT) == PaymasterFinalizationFailure::LOCAL_UNAVAILABLE;
            throw ApplicationError(
                transient ? ErrorKind::WALLET : ErrorKind::REJECTED,
                error);
        }
        signed_now = true;
        attempt.state = DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED;
        attempt.final_transaction = SerializeTransaction(final_transaction);
        attempt.final_txid = CTransaction{final_transaction}.GetHash();
        const int64_t provider_signed_at =
            std::max(attempt.updated_at, now);
        DigiDollar::Paymaster::ProviderCommitRecord signed_commit;
        signed_commit.commit_key = attempt.commit_key;
        signed_commit.provider_id = attempt.provider_id;
        signed_commit.quote_id = attempt.quote_id;
        signed_commit.template_commitment =
            attempt.template_commitment;
        signed_commit.final_txid = attempt.final_txid;
        signed_commit.raw_transaction_hash =
            Hash(attempt.final_transaction);
        signed_commit.final_transaction =
            attempt.final_transaction;
        signed_commit.provider_inputs = ProviderInputs(
            *full_psbt.tx, attempt.input_roles);
        signed_commit.committed_at = provider_signed_at;
        signed_commit.retry_until = attempt.retry_until;
        DigiDollar::Paymaster::PaymasterResult signed_result;
        if (!BuildFinalProviderResult(
                *wallet, signed_commit, provider_signed_at,
                signed_result, error)) {
            throw ApplicationError(
                error == "PAYMASTER_WALLET_LOCKED" ? ErrorKind::UNLOCK : ErrorKind::WALLET,
                error);
        }
        attempt.provider_signed_at = provider_signed_at;
        attempt.provider_signed_result =
            SerializeRecoveryMessage(signed_result);
        attempt.updated_at = provider_signed_at;
        if (!store.UpdateAttempt(session.request_id, attempt, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED) {
        DigiDollar::Paymaster::ProviderCommitRecord commit;
        commit.commit_key = attempt.commit_key;
        commit.provider_id = attempt.provider_id;
        commit.quote_id = attempt.quote_id;
        commit.template_commitment = attempt.template_commitment;
        commit.final_txid = attempt.final_txid;
        commit.raw_transaction_hash = Hash(attempt.final_transaction);
        commit.final_transaction = attempt.final_transaction;
        commit.provider_inputs = ProviderInputs(*user_psbt.tx, attempt.input_roles);
        // A crash may leave the complete provider-signed transaction
        // durable without its atomic ProviderCommit. Preserve the
        // signature boundary while recording the actual (monotonic)
        // commit time, even when the retry window has since elapsed.
        const int64_t commit_time =
            std::max(now, attempt.provider_signed_at);
        commit.committed_at = commit_time;
        commit.retry_until = attempt.retry_until;
        const std::optional<uint256> sponsorship_hash =
            attempt.sponsorship_capability_hash.IsNull() ? std::nullopt : std::optional<uint256>{attempt.sponsorship_capability_hash};
        DigiDollar::Paymaster::PaymasterResult final_result;
        if (!DeserializeCanonicalRecoveryMessage(
                attempt.provider_signed_result, final_result,
                error)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID");
        }

        // Reload the durable PROVIDER_SIGNED attempt and re-run both
        // the provider authorization firewall and the full final
        // transaction verifier at the last possible point before the
        // atomic FINAL_COMMITTED transition.
        ProviderAttempt committing_attempt;
        CMutableTransaction validated_final;
        FinalTransactionPresence final_presence{
            FinalTransactionPresence::NONE};
        if (!store.GetAttempt(attempt.attempt_id, committing_attempt) ||
            committing_attempt.state != AttemptState::PROVIDER_SIGNED ||
            !ValidateProviderCommitForExecution(
                committing_attempt, commit, commit_time, validated_final,
                error) ||
            !store.ValidateProviderBudgetAuthorization(
                committing_attempt,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true, error)) {
            throw ApplicationError(
                ErrorKind::REJECTED,
                error.empty() ? "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_INVALID" : error);
        }
        if (!PreflightFinalPaymasterTransaction(
                *wallet, MakeTransactionRef(validated_final),
                final_presence, error, txindex_mode)) {
            const bool transient = ClassifyPaymasterFinalizationFailure(
                error, PaymasterFinalizationStage::BEFORE_PROVIDER_COMMIT) == PaymasterFinalizationFailure::LOCAL_UNAVAILABLE;
            throw ApplicationError(
                transient ? ErrorKind::WALLET : ErrorKind::REJECTED,
                error);
        }
        attempt = std::move(committing_attempt);
        if (!store.CommitProviderFinalTransaction(session.request_id, attempt.attempt_id,
                                                  commit, sponsorship_hash,
                                                  final_result,
                                                  Params().GenesisBlock().GetHash(),
                                                  error) ||
            !store.GetProviderCommit(attempt.commit_key, committed)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }
    if (!already_committed && committed.final_transaction.empty()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "Paymaster attempt is not recoverable by exact provider retry");
    }

    ProviderCommitRecoveryResult recovery =
        RecoverProviderCommit(
            *wallet, store, committed,
            std::max(now, committed.committed_at));
    if (recovery.provider_result.commit_key.IsNull()) {
        const auto code = recovery.error == "PAYMASTER_WALLET_LOCKED" ? ErrorKind::UNLOCK : ErrorKind::WALLET;
        throw ApplicationError(code, recovery.error);
    }

    DigiDollar::Paymaster::PaymentSession authoritative;
    if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
        !store.GetAttempt(attempt.attempt_id, attempt)) {
        throw ApplicationError(ErrorKind::WALLET, "Failed to reload durable provider commit");
    }
    PaymentResult result;
    if (signed_now) result.psbt = SerializePSBT(full_psbt);
    result.final_transaction = committed.final_transaction;
    result.txid = committed.final_txid;
    result.broadcast = recovery.broadcast;
    if (!recovery.error.empty()) result.broadcast_error = recovery.error;
    result.request_id = authoritative.request_id;
    result.session_id = authoritative.session_id;
    result.provider_id = attempt.provider_id;
    result.attempt_id = attempt.attempt_id;
    result.quote_id = attempt.quote_id;
    result.unsigned_txid = attempt.unsigned_txid;
    result.template_commitment = attempt.template_commitment;
    result.session_state = authoritative.state;
    result.attempt_state = attempt.state;
    result.result_status = recovery.provider_result.status;
    result.result_sequence = recovery.provider_result.result_sequence;
    result.expires_at = attempt.retry_until;
    return result;
}

namespace {
struct ProviderSubmitProcessingContext {
    WalletContext& context;
    CWallet* wallet;
    ExecutionMode mode;
    ExactFinalTxIndexMode service_txindex_mode;
    int64_t now;
};

bool QueueProviderRecoveryResult(const ProviderSubmitProcessingContext& processing,
                                 const DirectMessage& direct, const AlternativeRecoveryRecord& record)
{
    WalletContext& context = processing.context;
    CWallet* const wallet = processing.wallet;
    const int64_t now = processing.now;
    AlternativeRecoveryResultMessage message;
    message.request_id = record.request_id;
    message.session_id = record.session_id;
    message.recovery_id = record.recovery_id;
    message.recovery_request_hash =
        record.recovery_request_hash;
    message.result = record.signed_result;
    const std::vector<unsigned char> encoded =
        SerializeRecoveryMessage(message);
    const uint256 message_id{Hash(encoded)};
    const bool queued =
        context.paymaster->HasOutboundDirectMessage(
            direct.peer_id, message_id) ||
        context.paymaster->QueueOutboundDirectMessage(
            direct.peer_id, message_id, encoded.size(),
            DirectPayload{message}, now);
    node::NodeContext* node_ctx = wallet->chain().context();
    if (queued && node_ctx && node_ctx->connman) {
        node_ctx->connman->WakeMessageHandler();
    }
    return queued;
}

struct CommittedProviderRecovery {
    AlternativeRecoveryRecord recovery;
    ProviderCommitRecord commit;
    CMutableTransaction transaction;
};

CommittedProviderRecovery SignAndCommitProviderRecoverySubmit(
    const ProviderSubmitProcessingContext& processing, PaymasterStore& store,
    AlternativeRecoveryRecord recovery, const AlternativeRecoverySubmit& submit,
    node::NodeContext& node)
{
    CWallet* const wallet = processing.wallet;
    const int64_t now = processing.now;
    const int64_t authorization_time{now};
    const ExactFinalTxIndexMode service_txindex_mode = processing.service_txindex_mode;
    node::NodeContext* const node_ctx = &node;
    std::string error;
    // Reload and rebuild every local authority immediately before
    // exposing a provider signature. The peer-supplied PSBT is
    // never itself trusted as the signing policy.
    AlternativeRecoveryParameters parameters;
    AlternativeRecoveryTemplate trusted;
    PartiallySignedTransaction unsigned_psbt;
    PartiallySignedTransaction user_psbt;
    AlternativeRecoverySubmit persisted_submit{submit};
    persisted_submit.user_psbt = recovery.user_signed_psbt;
    if (!store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
            recovery, Params().GenesisBlock().GetHash(), now,
            error) ||
        !BuildRecoveryParametersFromRecord(
            *wallet, recovery, parameters, error) ||
        !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
            recovery.recovery_response,
            recovery.recovery_request,
            recovery.recovery_provider_identity_key, parameters,
            Params(), *node_ctx->chainman, authorization_time,
            trusted,
            unsigned_psbt, error) ||
        !ValidateAlternativeRecoverySubmitAgainstChainstate(
            persisted_submit, recovery.recovery_response, trusted,
            parameters, Params(), *node_ctx->chainman,
            authorization_time,
            user_psbt, error)) {
        throw ApplicationError(
            ErrorKind::REJECTED,
            error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_AUTHORIZATION_INVALID" : error);
    }
    const AlternativeRecoveryManifest& manifest =
        recovery.recovery_response.manifest;
    if (!SignCollaborativePSBTForParty(
            *wallet, user_psbt, trusted.trusted_template,
            SigningParty::PROVIDER, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    CMutableTransaction final_transaction;
    if (!FinalizeAndExtractPSBT(user_psbt, final_transaction)) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_FINALIZATION_FAILED");
    }
    const CTransaction final_tx{final_transaction};
    if (!ValidateFinalAlternativeRecoveryAgainstChainstate(
            final_transaction, final_tx.GetWitnessHash(), trusted,
            parameters, Params(), *node_ctx->chainman,
            authorization_time,
            /*exact_final_already_known=*/false, error)) {
        throw ApplicationError(ErrorKind::REJECTED, error);
    }
    FinalTransactionPresence presence{FinalTransactionPresence::NONE};
    const CTransactionRef final_ref =
        MakeTransactionRef(final_transaction);
    if (!PreflightFinalPaymasterTransaction(
            *wallet, final_ref, presence, error,
            service_txindex_mode)) {
        throw ApplicationError(ErrorKind::REJECTED, error);
    }

    ProviderCommitRecord commit;
    commit.commit_key =
        recovery.recovery_response.recovery_commit_key;
    commit.provider_id = recovery.recovery_provider_id;
    commit.quote_id = recovery.recovery_id;
    commit.template_commitment = manifest.template_commitment;
    commit.final_txid = final_tx.GetHash();
    commit.final_transaction =
        SerializeTransaction(final_transaction);
    commit.raw_transaction_hash =
        Hash(commit.final_transaction);
    commit.provider_inputs =
        manifest.recovery_provider_carrier_inputs;
    commit.provider_inputs.insert(
        commit.provider_inputs.end(),
        manifest.recovery_provider_dgb_inputs.begin(),
        manifest.recovery_provider_dgb_inputs.end());
    commit.committed_at = now;
    commit.retry_until =
        recovery.recovery_response.expires_at;
    PaymasterResult final_result;
    if (!BuildFinalProviderResult(
            *wallet, commit, now, final_result, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
    recovery.final_transaction = commit.final_transaction;
    recovery.expected_wtxid = final_tx.GetWitnessHash();
    recovery.signed_result = final_result;
    recovery.updated_at = std::max(recovery.updated_at, now);
    if (!store.CommitProviderAlternativeRecoveryFinal(
            recovery, commit, final_result,
            Params().GenesisBlock().GetHash(), error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }

    return {std::move(recovery), std::move(commit), std::move(final_transaction)};
}

ProviderProcessingResult BroadcastCommittedProviderRecoverySubmit(
    const ProviderSubmitProcessingContext& processing, const DirectMessage& direct,
    const AlternativeRecoverySubmit& submit, PaymasterStore& store,
    node::NodeContext& node, CommittedProviderRecovery committed)
{
    CWallet* const wallet = processing.wallet;
    const int64_t authorization_time{processing.now};
    const ExactFinalTxIndexMode service_txindex_mode = processing.service_txindex_mode;
    node::NodeContext* const node_ctx = &node;
    AlternativeRecoveryRecord& recovery = committed.recovery;
    ProviderCommitRecord& commit = committed.commit;
    const CMutableTransaction& final_transaction = committed.transaction;
    const CTransaction final_tx{final_transaction};
    const CTransactionRef final_ref = MakeTransactionRef(final_transaction);
    std::string error;
    // The commit above irrevocably consumes the provider budget
    // and pool slots because a valid provider signature now
    // exists. Reload those exact durable artifacts and repeat the
    // complete authority and mempool firewall. A failure leaves
    // FINAL_COMMITTED intact for safe retry; it must never roll
    // the economic reservation back to AVAILABLE.
    ProviderCommitRecord broadcast_commit;
    PaymasterResult broadcast_result;
    AlternativeRecoveryRecord broadcast_recovery;
    if (!store.GetProviderCommit(
            commit.commit_key, broadcast_commit) ||
        !store.GetProviderResult(
            commit.commit_key, broadcast_result) ||
        !store.GetProviderAlternativeRecoveryByCommit(
            broadcast_commit, broadcast_recovery, error) ||
        broadcast_commit.final_txid != commit.final_txid ||
        broadcast_commit.raw_transaction_hash !=
            commit.raw_transaction_hash ||
        broadcast_commit.final_transaction !=
            commit.final_transaction ||
        !broadcast_result.txid ||
        *broadcast_result.txid != commit.final_txid ||
        !broadcast_result.raw_transaction_hash ||
        *broadcast_result.raw_transaction_hash !=
            final_tx.GetWitnessHash() ||
        !broadcast_result.final_transaction ||
        SerializeTransaction(
            *broadcast_result.final_transaction) !=
            commit.final_transaction) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_FINAL_RELOAD_FAILED" : error);
    }
    AlternativeRecoveryParameters broadcast_parameters;
    AlternativeRecoveryTemplate broadcast_trusted;
    PartiallySignedTransaction broadcast_unsigned_psbt;
    PartiallySignedTransaction broadcast_user_psbt;
    AlternativeRecoverySubmit broadcast_submit{submit};
    broadcast_submit.user_psbt =
        broadcast_recovery.user_signed_psbt;
    FinalTransactionPresence broadcast_presence{
        FinalTransactionPresence::NONE};
    if (!BuildRecoveryParametersFromRecord(
            *wallet, broadcast_recovery, broadcast_parameters,
            error) ||
        !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
            broadcast_recovery.recovery_response,
            broadcast_recovery.recovery_request,
            broadcast_recovery.recovery_provider_identity_key,
            broadcast_parameters, Params(), *node_ctx->chainman,
            authorization_time, broadcast_trusted,
            broadcast_unsigned_psbt, error) ||
        !ValidateAlternativeRecoverySubmitAgainstChainstate(
            broadcast_submit,
            broadcast_recovery.recovery_response,
            broadcast_trusted, broadcast_parameters, Params(),
            *node_ctx->chainman, authorization_time,
            broadcast_user_psbt, error)) {
        throw ApplicationError(
            IsTransientPaymasterFinalizationError(error) ? ErrorKind::WALLET : ErrorKind::REJECTED,
            error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_CONFLICT" : error);
    }
    if (!PreflightFinalPaymasterTransaction(
            *wallet, final_ref, broadcast_presence, error,
            service_txindex_mode)) {
        throw ApplicationError(
            IsTransientPaymasterFinalizationError(error) ? ErrorKind::WALLET : ErrorKind::REJECTED,
            error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_PREFLIGHT_FAILED" : error);
    }
    if (!ValidateFinalAlternativeRecoveryAgainstChainstate(
            final_transaction,
            broadcast_recovery.expected_wtxid,
            broadcast_trusted, broadcast_parameters, Params(),
            *node_ctx->chainman, authorization_time,
            broadcast_presence != FinalTransactionPresence::NONE,
            error)) {
        throw ApplicationError(
            IsTransientPaymasterFinalizationError(error) ? ErrorKind::WALLET : ErrorKind::REJECTED,
            error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_POST_COMMIT_CONFLICT" : error);
    }
    recovery = std::move(broadcast_recovery);
    commit = std::move(broadcast_commit);
    bool already_confirmed{false};
    std::string broadcast_error;
    const bool broadcast = InsertAndBroadcastPaymasterTransaction(
        *wallet, final_ref, already_confirmed, broadcast_error);
    if (!broadcast) {
        throw ApplicationError(
            IsTransientPaymasterFinalizationError(broadcast_error) ? ErrorKind::WALLET : ErrorKind::REJECTED,
            broadcast_error.empty() ? "PAYMASTER_PROVIDER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_PROVIDER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
    }
    ProviderProcessingResult result;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.queued = QueueProviderRecoveryResult(processing, direct, recovery);
    result.message_type = "recovery_submit";
    result.request_id = recovery.request_id;
    result.session_id = recovery.session_id;
    result.provider_id = recovery.recovery_provider_id;
    result.commit_key = commit.commit_key;
    result.txid = commit.final_txid;
    result.result_status = PaymasterResultStatus::FINAL_COMMITTED;
    result.broadcast = broadcast;
    if (!broadcast_error.empty()) {
        result.broadcast_error = broadcast_error;
    }
    result.recovery = recovery;
    return result;
}

ProviderProcessingResult ProcessProviderRecoverySubmit(
    const ProviderSubmitProcessingContext& processing, const DirectMessage& direct)
{
    WalletContext& context = processing.context;
    CWallet* const wallet = processing.wallet;
    const int64_t now = processing.now;
    std::string error;
    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                              direct};
    const auto* submit =
        std::get_if<AlternativeRecoverySubmit>(&direct.payload);
    if (!submit || !ValidateAlternativeRecoverySubmitEnvelope(
                       *submit, Params().GenesisBlock().GetHash(),
                       error)) {
        return RejectProviderMessage(
            *context.paymaster, direct,
            error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error,
            "recovery_submit");
    }
    PaymasterStore store{*wallet};
    AlternativeRecoveryRecord recovery;
    const auto recovery_status = store.GetAlternativeRecoveryByIdWithStatus(
        submit->recovery_id, recovery);
    if (recovery_status == DatabaseReadStatus::NOT_FOUND) {
        return RejectProviderMessage(
            *context.paymaster, direct,
            "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH", "recovery_submit");
    }
    if (recovery_status != DatabaseReadStatus::FOUND) {
        throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_RECOVERY_SUBMIT_DATABASE_READ");
    }
    // Only intact local records may classify a peer's payload. Unknown
    // versions, corrupt authority and read failures retain the message.
    if (!paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(
            recovery, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (!recovery.provider_side || recovery.expired ||
        recovery.request_id != submit->request_id ||
        recovery.session_id != submit->session_id ||
        recovery.recovery_provider_id !=
            submit->recovery_provider_id ||
        recovery.recovery_request_hash !=
            submit->recovery_request_hash ||
        recovery.recovery_response.recovery_commit_key !=
            submit->recovery_commit_key ||
        recovery.recovery_response.manifest.template_commitment !=
            submit->template_commitment) {
        return RejectProviderMessage(
            *context.paymaster, direct,
            "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH", "recovery_submit");
    }


    if (recovery.phase ==
        AlternativeRecoveryPhase::FINAL_COMMITTED) {
        if (recovery.signed_result.status !=
                PaymasterResultStatus::FINAL_COMMITTED) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
        }
        if (recovery.user_signed_psbt != submit->user_psbt) {
            return RejectProviderMessage(
                *context.paymaster, direct,
                "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT", "recovery_submit");
        }
        ProviderProcessingResult result;
        result.disposition = ProviderProcessingDisposition::PROCESSED;
        result.queued = QueueProviderRecoveryResult(processing, direct, recovery);
        result.message_type = "recovery_submit";
        result.request_id = recovery.request_id;
        result.session_id = recovery.session_id;
        result.provider_id = recovery.recovery_provider_id;
        result.commit_key = recovery.recovery_response.recovery_commit_key;
        result.txid = *recovery.signed_result.txid;
        result.result_status = PaymasterResultStatus::FINAL_COMMITTED;
        result.recovery = recovery;
        return result;
    }

    if (recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED &&
        recovery.phase != AlternativeRecoveryPhase::USER_SIGNED) {
        throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
    }
    if (recovery.phase == AlternativeRecoveryPhase::USER_SIGNED &&
        recovery.user_signed_psbt != submit->user_psbt) {
        return RejectProviderMessage(
            *context.paymaster, direct,
            "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT", "recovery_submit");
    }
    if (recovery.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED &&
        recovery.recovery_response.expires_at <= now) {
        // Consume the late request without changing reservations or any
        // durable authority. Signed recovery has a separate retry firewall.
        return RejectProviderMessage(
            *context.paymaster, direct,
            "PAYMASTER_RECOVERY_SUBMIT_EXPIRED", "recovery_submit");
    }

    node::NodeContext* node_ctx = wallet->chain().context();
    if (!node_ctx || !node_ctx->chainman) {
        throw ApplicationError(ErrorKind::INTERNAL,
                           "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
    }
    // Receiving an exact USER-signed replay is not authority to
    // create a new provider signature after the response or
    // Capacity window expired. Only an already durable final may
    // use the separately named authorized-retry firewall.
    const int64_t authorization_time{now};
    AlternativeRecoveryParameters parameters;
    AlternativeRecoveryTemplate trusted;
    PartiallySignedTransaction unsigned_psbt;
    PartiallySignedTransaction user_psbt;
    if (!BuildRecoveryParametersFromRecord(
            *wallet, recovery, parameters, error) ||
        !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
            recovery.recovery_response,
            recovery.recovery_request,
            recovery.recovery_provider_identity_key, parameters,
            Params(), *node_ctx->chainman, authorization_time, trusted,
            unsigned_psbt, error)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error);
    }
    if (!ValidateAlternativeRecoverySubmitPayload(
            *submit, recovery.recovery_response, trusted, user_psbt, error)) {
        return RejectProviderMessage(
            *context.paymaster, direct, error, "recovery_submit");
    }
    // The payload check uses only the validated local template. Recheck
    // full authority before persistence; a chain race remains local.
    if (!ValidateAlternativeRecoverySubmitAgainstChainstate(
            *submit, recovery.recovery_response, trusted,
            parameters, Params(), *node_ctx->chainman,
            authorization_time,
            user_psbt, error)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_INVALID_RECOVERY_SUBMIT" : error);
    }

    if (recovery.phase ==
        AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
        recovery.user_signed_psbt = submit->user_psbt;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecoveryById(
                submit->recovery_id, recovery)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }
    if (wallet->IsLocked()) {
        throw ApplicationError(
            ErrorKind::UNLOCK,
            "Provider wallet unlock is required for a recovery signature");
    }

    auto committed = SignAndCommitProviderRecoverySubmit(
        processing, store, std::move(recovery), *submit, *node_ctx);
    return BroadcastCommittedProviderRecoverySubmit(
        processing, direct, *submit, store, *node_ctx, std::move(committed));
}

ProviderProcessingResult ProcessProviderPaymentSubmit(
    const ProviderSubmitProcessingContext& processing, const DirectMessage& direct)
{
    WalletContext& context = processing.context;
    CWallet* const wallet = processing.wallet;
    const ExecutionMode mode = processing.mode;
    std::string error;
    ProviderProcessingResult result;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.queued = false;
    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                              direct};
    const auto* submit = std::get_if<PaymasterSubmit>(&direct.payload);
    if (!submit) {
        if (!AcknowledgeRejectedDirectMessage(
                *context.paymaster, direct, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        throw ApplicationError(ErrorKind::INTERNAL,
                           "PAYMASTER_SUBMIT_QUEUE_CORRUPT");
    }
    if (!ValidateSubmitEnvelope(*submit, Params().GenesisBlock().GetHash(), error)) {
        return RejectProviderMessage(*context.paymaster, direct, error, "submit");
    }
    PaymasterStore store{*wallet};
    ProviderAttempt attempt;
    bool have_attempt{false};
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        uint256 indexed_attempt;
        const auto status = batch.ReadPaymasterTemplateWithStatus(submit->template_commitment, indexed_attempt);
        if (status == DatabaseReadStatus::FOUND) {
            if (batch.ReadPaymasterAttemptWithStatus(indexed_attempt, attempt) != DatabaseReadStatus::FOUND ||
                attempt.template_commitment != submit->template_commitment)
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_ATTEMPT_DATABASE_READ");
            have_attempt = true;
        } else if (status != DatabaseReadStatus::NOT_FOUND) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_TEMPLATE_DATABASE_READ");
        }
    }
    if (!have_attempt ||
        attempt.provider_id != submit->provider_id || attempt.session_id != submit->session_id ||
        attempt.quote_id != submit->quote_id || attempt.commit_key != submit->commit_key) {
        // Stale or foreign submissions are peer-local failures. In
        // particular, a confirmed session may already be a tombstone.
        // Consume only this message so it cannot pin the service queue.
        if (!AcknowledgeRejectedDirectMessage(*context.paymaster, direct, error))
            throw ApplicationError(ErrorKind::WALLET, error);
        result.disposition = ProviderProcessingDisposition::REJECTED;
        result.rejection_reason = "PAYMASTER_SUBMIT_BINDING_MISMATCH";
        return result;
    }
    PaymentSession session;
    if (store.GetSessionBySessionIdWithStatus(attempt.session_id, session) !=
        DatabaseReadStatus::FOUND) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_SESSION_DATABASE_READ");
    }
    PartiallySignedTransaction submitted_psbt;
    PaymentIntent authorized_intent;
    PaymasterQuote authorized_quote;
    CollaborativePSBTTemplate trusted;
    if (!LoadProviderSubmitAuthority(attempt, authorized_intent, authorized_quote, trusted, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (session.request_id != submit->request_id) {
        return RejectProviderMessage(*context.paymaster, direct,
                                     "PAYMASTER_SUBMIT_SESSION_MISMATCH", "submit");
    }
    if (!DecodeRawPSBT(submitted_psbt, MakeByteSpan(submit->user_psbt), error) ||
        !submitted_psbt.tx) {
        return RejectProviderMessage(*context.paymaster, direct,
                                     "PAYMASTER_INVALID_SUBMIT_PSBT", "submit");
    }
    if (!ValidateCollaborativePSBT(submitted_psbt, trusted,
                                   CollaborativeSignatureStage::USER_SIGNED, error)) {
        return RejectProviderMessage(*context.paymaster, direct, error, "submit");
    }
    if (const auto reason = GetPermanentProviderSubmitRejection(
            *wallet, session, attempt, submitted_psbt, trusted, GetTime())) {
        return RejectProviderMessage(*context.paymaster, direct, *reason, "submit");
    }

    PaymasterResult provider_result;
    bool rejected{false};
    if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *submitted_psbt.tx,
                                         GetTime(), rejected, provider_result, error)) {
        throw ApplicationError(
            error == "PAYMASTER_WALLET_LOCKED" ? ErrorKind::UNLOCK : ErrorKind::WALLET,
            error);
    }

    PaymentResult committed;
    if (!rejected) {
        try {
            committed = SubmitPaymasterTransaction(*wallet, std::move(submitted_psbt), mode);
        } catch (const ProviderSubmitExpired&) {
            // The deadline or unsigned cleanup may have advanced after the
            // first observation. Revalidate current local authority and peer
            // bindings; no other application failure is converted to an ACK.
            ProviderAttempt current;
            {
                LOCK(wallet->cs_wallet);
                WalletBatch batch{wallet->GetDatabase()};
                if (batch.ReadPaymasterAttemptWithStatus(attempt.attempt_id, current) != DatabaseReadStatus::FOUND) {
                    throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SUBMIT_ATTEMPT_DATABASE_READ");
                }
            }
            PaymentSession current_session;
            CollaborativePSBTTemplate current_trusted;
            PaymentIntent current_intent;
            PaymasterQuote current_quote;
            PartiallySignedTransaction current_submitted;
            if (store.GetSessionBySessionIdWithStatus(current.session_id, current_session) != DatabaseReadStatus::FOUND ||
                current.provider_id != submit->provider_id || current.session_id != submit->session_id ||
                current.quote_id != submit->quote_id || current.commit_key != submit->commit_key ||
                current.template_commitment != submit->template_commitment || current_session.request_id != submit->request_id ||
                !LoadProviderSubmitAuthority(current, current_intent, current_quote, current_trusted, error) ||
                !DecodeRawPSBT(current_submitted, MakeByteSpan(submit->user_psbt), error) ||
                !ValidateCollaborativePSBT(current_submitted, current_trusted, CollaborativeSignatureStage::USER_SIGNED, error)) {
                throw ApplicationError(ErrorKind::WALLET,
                    error.empty() ? "PAYMASTER_SUBMIT_AUTHORIZATION_STATE" : error);
            }
            if (const auto reason = GetPermanentProviderSubmitRejection(
                    *wallet, current_session, current, current_submitted, current_trusted, GetTime())) {
                return RejectProviderMessage(*context.paymaster, direct, *reason, "submit");
            }
            throw;
        }
        if (!store.GetProviderResult(submit->commit_key, provider_result) ||
            !store.GetAttempt(attempt.attempt_id, attempt)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RESULT_NOT_DURABLE");
        }
    }
    PaymasterResultMessage message;
    message.request_id = submit->request_id;
    message.session_id = submit->session_id;
    message.result = provider_result;
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << message;
    const auto bytes = MakeUCharSpan(stream);
    const std::vector<unsigned char> encoded{bytes.begin(), bytes.end()};
    const bool queued = context.paymaster->QueueOutboundDirectMessage(
        direct.peer_id, Hash(encoded), encoded.size(), DirectPayload{message}, GetTime());
    node::NodeContext* node = wallet->chain().context();
    if (queued && node && node->connman) node->connman->WakeMessageHandler();
    result.queued = queued;
    result.request_id = message.request_id;
    result.session_id = message.session_id;
    result.provider_id = provider_result.provider_id;
    result.commit_key = provider_result.commit_key;
    if (provider_result.txid) result.txid = *provider_result.txid;
    result.result_status = provider_result.status;
    result.attempt_state = attempt.state;
    if (rejected) {
        result.rejection_code = "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE";
    } else {
        result.commit = committed;
    }
    return result;
}
} // namespace

ProviderProcessingResult ProcessPaymasterSubmits(WalletContext& context, CWallet& target, ExecutionMode mode)
{
    CWallet* const wallet = &target;
    using namespace DigiDollar::Paymaster;
    const bool automatic_service = mode == ExecutionMode::AUTOMATIC;
    const ExactFinalTxIndexMode service_txindex_mode =
        automatic_service ? ExactFinalTxIndexMode::NONBLOCKING
                          : ExactFinalTxIndexMode::WAIT_FOR_SYNC;
    if (automatic_service) {
        if (!AutomaticProviderStateIsSynchronized(*wallet)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_SYNCING");
        }
    } else {
        wallet->BlockUntilSyncedToCurrentChain();
    }
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentity(*wallet, identity) || !context.paymaster ||
        !context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_NOT_RUNNING");
    }
    ProviderSettings runtime_settings;
    if (!GetPaymasterProviderSettings(*wallet, runtime_settings)) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND");
    }
    if (runtime_settings.operation_mode ==
            ProviderOperationMode::AUTOMATIC &&
        !automatic_service) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE");
    }
    ProviderWorkGuard work_guard{*context.paymaster,
                                 wallet->GetName(),
                                 identity.provider_id};
    if (!work_guard.Acquired()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_SERVICE_BUSY");
    }
    const int64_t now = GetTime();
    const auto submit_lease = context.paymaster->LeaseProviderSubmit(identity.provider_id);
    const auto& messages = submit_lease.Messages();
    if (messages.empty()) {
        ProviderProcessingResult result;
        result.queued = false;
        return result;
    }
    // Keep the provider work guard and fair inbox lease alive across every
    // validation, signature, commit and acknowledgement in the selected phase.
    const ProviderSubmitProcessingContext processing{
        context, wallet, mode, service_txindex_mode, now};
    if (std::holds_alternative<AlternativeRecoverySubmit>(messages.front().payload)) {
        return ProcessProviderRecoverySubmit(processing, messages.front());
    }
    return ProcessProviderPaymentSubmit(processing, messages.front());
}

namespace {
struct ProviderRequestProcessingContext {
    WalletContext& context;
    CWallet* wallet;
    const ProviderReadiness& readiness;
    node::NodeContext* node;
    bool automatic_service;
    int64_t now;
    std::string& error;
};

ProviderProcessingResult ProcessProviderCapacityRequest(
    const ProviderRequestProcessingContext& request,
    const DirectMessage& direct)
{
    WalletContext& context = request.context;
    CWallet* const wallet = request.wallet;
    const ProviderReadiness& readiness = request.readiness;
    node::NodeContext* const node = request.node;
    const int64_t now = request.now;
    std::string& error = request.error;
    ProviderProcessingResult result;
    result.queued = false;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.message_type = "capacity";
    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                              direct};
    const auto* capacity_request = std::get_if<PaymasterCapacityRequest>(
        &direct.payload);
    if (!capacity_request || !ValidateCapacityRequestEnvelope(
                                 *capacity_request,
                                 Params().GenesisBlock().GetHash(),
                                 now, error)) {
        if (!AcknowledgeRejectedDirectMessage(
                *context.paymaster, direct, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        result.disposition = ProviderProcessingDisposition::REJECTED;
        result.rejection_reason = error.empty() ? "PAYMASTER_INVALID_CAPACITY_REQUEST" : error;
        return result;
    }
    if (wallet->IsLocked()) {
        throw ApplicationError(ErrorKind::UNLOCK,
                           "Provider wallet unlock is required for a capacity proof");
    }
    bool have_stored_response{false};
    if (!readiness.ready && CapacityContinuationMayProceed(readiness)) {
        // An exact retry can arrive after its own proof reserved
        // the last slot. Do not mistake it for fresh admission.
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        std::vector<unsigned char> stored_response;
        have_stored_response = batch.ReadPaymasterCapacityResponse(
            Hash(SerializeCapacityRequest(*capacity_request)), stored_response);
    }
    if (!CapacityRequestMayProceed(readiness, have_stored_response)) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_NOT_RUNNING");
    }
    uint256 reference_block;
    int tip_height{-1};
    {
        LOCK(cs_main);
        const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
        if (tip) {
            reference_block = tip->GetBlockHash();
            tip_height = tip->nHeight;
        }
    }
    if (reference_block.IsNull() || tip_height < 0) {
        throw ApplicationError(ErrorKind::GENERAL,
                           "PAYMASTER_CAPACITY_CHAINSTATE_UNAVAILABLE");
    }
    if (direct.canonical_netgroup.empty()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
    }
    PaymasterCapacityProof proof;
    std::vector<ProviderPoolEntry> reserved;
    if (!ReserveAndBuildPaymasterCapacityProof(
            *wallet, readiness.identity, *capacity_request,
            Params().GenesisBlock().GetHash(), reference_block,
            tip_height, /*minimum_confirmations=*/1,
            direct.canonical_netgroup, now,
            proof, reserved, error)) {
        if (error == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" ||
            error == "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED") {
            message_guard.Defer();
            result.disposition = ProviderProcessingDisposition::DEFERRED;
            result.deferral_reason = error;
        } else {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    } else {
        const std::vector<unsigned char> proof_bytes =
            SerializeCapacityProof(proof);
        const uint256 message_id = Hash(proof_bytes);
        const bool queued = context.paymaster->QueueCapacityProof(
            direct.peer_id, message_id,
            proof_bytes.size(), proof, now);
        if (queued && node->connman) node->connman->WakeMessageHandler();
        result.queued = queued;
        result.provider_id = proof.provider_id;
        result.client_nonce = proof.client_nonce;
        result.capacity_snapshot_id = proof.snapshot_id;
        return result;
    }
    return result;
}

struct ValidatedProviderRecoveryInputs {
    PaymasterCapacityProof capacity_proof;
    std::vector<CollaborativeInput> user_inputs;
};

std::optional<ProviderProcessingResult> ReplayProviderRecoveryRequest(
    const ProviderRequestProcessingContext& request,
    const DirectMessage& direct, const AlternativeRecoveryRequest& recovery_request,
    PaymasterStore& store)
{
    WalletContext& context = request.context;
    node::NodeContext* const node = request.node;
    const int64_t now = request.now;
    const uint256 recovery_id = GetAlternativeRecoveryId(
        recovery_request.request_id,
        recovery_request.session_id,
        recovery_request.recovery_provider_id,
        recovery_request.client_nonce);
    AlternativeRecoveryRecord persisted;
    const DatabaseReadStatus recovery_status = store.GetAlternativeRecoveryByIdWithStatus(recovery_id, persisted);
    if (recovery_status != DatabaseReadStatus::FOUND && recovery_status != DatabaseReadStatus::NOT_FOUND) {
        throw ApplicationError(ErrorKind::WALLET,
            recovery_status == DatabaseReadStatus::UNSUPPORTED_VERSION
                ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_RECOVERY_DATABASE_READ");
    }
    if (recovery_status == DatabaseReadStatus::NOT_FOUND) return std::nullopt;
    ProviderProcessingResult result;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.queued = false;
    result.message_type = "recovery_request";
    if (!persisted.provider_side ||
        persisted.recovery_request_hash != GetAlternativeRecoveryRequestHash(persisted.recovery_request) ||
        persisted.phase < AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
    }
    if (persisted.expired ||
        persisted.recovery_request_hash != GetAlternativeRecoveryRequestHash(recovery_request) ||
        SerializeRecoveryMessage(persisted.recovery_request) != SerializeRecoveryMessage(recovery_request)) {
        return RejectProviderMessage(*context.paymaster, direct,
            "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT", "recovery_request");
    }
    const std::vector<unsigned char> encoded =
        SerializeRecoveryMessage(persisted.recovery_response);
    const bool queued =
        context.paymaster->HasOutboundDirectMessage(
            direct.peer_id, Hash(encoded)) ||
        context.paymaster->QueueOutboundDirectMessage(
            direct.peer_id, Hash(encoded), encoded.size(),
            DirectPayload{persisted.recovery_response}, now);
    if (queued && node->connman) {
        node->connman->WakeMessageHandler();
    }
    result.queued = queued;
    result.request_id = persisted.request_id;
    result.session_id = persisted.session_id;
    result.provider_id = persisted.recovery_provider_id;
    result.recovery_id = persisted.recovery_id;
    result.template_commitment = persisted.recovery_response.manifest
            .template_commitment;
    return result;
}

// A rejection is returned only after the remote defect has been established.
// Unknown local storage/index/chain authority throws and retains the inbox lease.
std::variant<ValidatedProviderRecoveryInputs, ProviderProcessingResult> PreflightProviderRecoveryRequest(
    const ProviderRequestProcessingContext& request,
    const DirectMessage& direct, const AlternativeRecoveryRequest& recovery_request,
    PaymasterStore& store)
{
    WalletContext& context = request.context;
    CWallet* const wallet = request.wallet;
    const ProviderReadiness& readiness = request.readiness;
    node::NodeContext* const node = request.node;
    const bool automatic_service = request.automatic_service;
    const int64_t now = request.now;
    std::string& error = request.error;
    if (wallet->IsLocked()) {
        throw ApplicationError(
            ErrorKind::UNLOCK,
            "Provider wallet unlock is required for a recovery quote");
    }
    HashWriter offer_hasher =
        TaggedHash("DigiByte Paymaster Offer v1");
    offer_hasher << readiness.identity.provider_id
                 << recovery_request.policy_hash
                 << static_cast<uint8_t>(FundingModel::USER_PAID);
    const uint256 expected_offer_id = offer_hasher.GetSHA256();
    if (recovery_request.policy_hash !=
            GetProviderPolicyHash(readiness.policy) ||
        recovery_request.offer_id !=
            expected_offer_id ||
        !PolicyAllowsFundingModel(readiness.policy,
                                  FundingModel::USER_PAID)) {
        return RejectProviderMessage(*context.paymaster, direct,
                                     "PAYMASTER_RECOVERY_POLICY_MISMATCH", "recovery_request");
    }

    PaymasterCapacityProof capacity_proof;
    const auto capacity_status = store.GetProviderCapacityProofWithStatus(
        recovery_request.capacity_request, now, capacity_proof, error);
    if (capacity_status == ProviderCapacityProofStatus::READ_ERROR) {
        throw ApplicationError(ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_CAPACITY_DATABASE_READ" : error);
    }
    if (capacity_status != ProviderCapacityProofStatus::FOUND) {
        return RejectProviderMessage(*context.paymaster, direct, error, "recovery_request");
    }
    if (capacity_proof.snapshot_id !=
            recovery_request.capacity_snapshot_id ||
        GetCapacityResourceCommitment(capacity_proof) !=
            recovery_request.capacity_resource_commitment) {
        return RejectProviderMessage(*context.paymaster, direct,
                                     "PAYMASTER_RECOVERY_CAPACITY_BINDING_MISMATCH", "recovery_request");
    }
    // Immutable peer mistakes above need no new financial authority. Any valid
    // continuation still waits for the unchanged wallet/index readiness gates.
    const bool continuation_ready = CapacityContinuationMayProceed(readiness);
    if (!continuation_ready || !ProviderTxIndexIsReady(*wallet, !automatic_service)) {
        throw ApplicationError(ErrorKind::WALLET,
            !continuation_ready ? "PAYMASTER_PROVIDER_DRAIN_ONLY" : "PAYMASTER_REQUIRES_READY_TXINDEX");
    }
    if (!ValidateCapacityProofAgainstChainstate(
            capacity_proof,
            recovery_request.capacity_request,
            readiness.identity.identity_key, *node->chainman,
            now, error)) {
        // The proof was loaded from local authority. A changed chain or damaged
        // saved proof must be reconciled without acknowledging a peer rejection.
        throw ApplicationError(ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_RECOVERY_CAPACITY_BINDING_MISMATCH" : error);
    }

    std::vector<CollaborativeInput> user_inputs;
    for (const COutPoint& outpoint :
         recovery_request.user_dd_inputs) {
        uint256 block_hash;
        CTransactionRef creating_tx;
        if (!g_txindex->FindTx(outpoint.hash, block_hash,
                               creating_tx) ||
            !creating_tx) {
            bool missing_or_spent;
            {
                LOCK(cs_main);
                missing_or_spent = node->chainman->ActiveChainstate()
                                       .CoinsTip().AccessCoin(outpoint).IsSpent();
            }
            if (missing_or_spent) {
                return RejectProviderMessage(*context.paymaster, direct,
                    "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_FOUND", "recovery_request");
            }
            throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_RECOVERY_USER_PREVOUT_LOOKUP_FAILED");
        }
        user_inputs.push_back(
            {outpoint, creating_tx, InputRole::USER_DD});
    }
    ValidatedUserDDInputs validated_user_inputs;
    bool valid_user_inputs;
    {
        LOCK(cs_main);
        valid_user_inputs = ValidateCollaborativeUserDDInputs(
            user_inputs, node->chainman->ActiveChainstate().CoinsTip(),
            validated_user_inputs, error);
    }
    if (!valid_user_inputs) {
        return RejectProviderMessage(*context.paymaster, direct, error, "recovery_request");
    }
    if (!ValidateAlternativeRecoveryRequestInputs(
            recovery_request, validated_user_inputs.output_keys,
            error)) {
        return RejectProviderMessage(*context.paymaster, direct, error, "recovery_request");
    }
    const CAmount total_dd{
        validated_user_inputs.total_dd_amount};
    CAmount returned_dd{0};
    for (const AlternativeRecoveryReturn& output :
         recovery_request.wallet_returns) {
        if (output.amount.value >
            std::numeric_limits<CAmount>::max() - returned_dd) {
            return RejectProviderMessage(*context.paymaster, direct,
                                         "PAYMASTER_RECOVERY_AMOUNT_OVERFLOW", "recovery_request");
        }
        returned_dd += output.amount.value;
    }
    const auto fee_plan = EvaluateServiceFee(
        readiness.policy, FundingModel::USER_PAID,
        DDCents{total_dd}, error);
    if (!fee_plan ||
        !(fee_plan->service_fee ==
          recovery_request.service_fee) ||
        total_dd - returned_dd !=
            recovery_request.service_fee.value) {
        return RejectProviderMessage(*context.paymaster, direct,
            error.empty() ? "PAYMASTER_RECOVERY_SERVICE_FEE_MISMATCH" : error,
            "recovery_request");
    }
    return ValidatedProviderRecoveryInputs{std::move(capacity_proof), std::move(user_inputs)};
}

ProviderProcessingResult CommitProviderRecoveryQuote(
    const ProviderRequestProcessingContext& request,
    const DirectMessage& direct, const AlternativeRecoveryRequest& recovery_request,
    PaymasterStore& store, ValidatedProviderRecoveryInputs validated)
{
    WalletContext& context = request.context;
    CWallet* const wallet = request.wallet;
    const ProviderReadiness& readiness = request.readiness;
    node::NodeContext* const node = request.node;
    const int64_t now = request.now;
    std::string& error = request.error;
    ProviderProcessingResult result;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.queued = false;
    result.message_type = "recovery_request";
    const uint256 recovery_id = GetAlternativeRecoveryId(recovery_request.request_id,
        recovery_request.session_id, recovery_request.recovery_provider_id, recovery_request.client_nonce);
    uint256 netgroup_bucket;
    const PaymasterCapacityProof& capacity_proof = validated.capacity_proof;
    auto& user_inputs = validated.user_inputs;
    ValidatedCapacitySnapshot snapshot;
    snapshot.snapshot_id = capacity_proof.snapshot_id;
    snapshot.resource_commitment =
        GetCapacityResourceCommitment(capacity_proof);
    snapshot.session_id = recovery_request.session_id;
    snapshot.attempt_id = recovery_id;
    snapshot.provider_id = capacity_proof.provider_id;
    snapshot.client_nonce = capacity_proof.client_nonce;
    snapshot.request_hash = Hash(
        SerializeCapacityRequest(
            recovery_request.capacity_request));
    snapshot.capacity_proof =
        SerializeCapacityProof(capacity_proof);
    snapshot.created_at = capacity_proof.created_at;
    snapshot.expires_at = capacity_proof.expires_at;
    snapshot.validated_at = now;
    snapshot.funding_model = capacity_proof.funding_model;
    snapshot.requires_carrier = capacity_proof.requires_carrier;

    AlternativeRecoveryParameters parameters;
    parameters.genesis_hash = recovery_request.genesis_hash;
    parameters.request_id = recovery_request.request_id;
    parameters.session_id = recovery_request.session_id;
    parameters.original_provider_id =
        recovery_request.original_provider_id;
    parameters.recovery_provider_id =
        recovery_request.recovery_provider_id;
    parameters.privacy_profile =
        recovery_request.privacy_profile;
    parameters.offer_id = recovery_request.offer_id;
    parameters.policy_hash = recovery_request.policy_hash;
    parameters.original_commit_key =
        recovery_request.original_commit_key;
    parameters.original_template_commitment =
        recovery_request.original_template_commitment;
    parameters.capacity_request =
        recovery_request.capacity_request;
    parameters.capacity_snapshot = snapshot;
    parameters.recovery_provider_identity_key =
        readiness.identity.identity_key;
    parameters.persisted_user_dd_inputs =
        recovery_request.user_dd_inputs;
    parameters.user_dd_inputs = std::move(user_inputs);
    parameters.wallet_returns =
        recovery_request.wallet_returns;
    for (const AlternativeRecoveryReturn& output :
         parameters.wallet_returns) {
        parameters.wallet_verified_fresh_return_scripts.push_back(
            output.script_pub_key);
    }
    const PaymasterLiquiditySlot& slot =
        capacity_proof.liquidity_slots.front();
    const CAmount network_fee{COIN / 10};
    CAmount provider_dgb{0};
    for (const CapacityDGBInput& input : slot.dgb_inputs) {
        if (input.input.value.value >
            std::numeric_limits<CAmount>::max() - provider_dgb) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_RECOVERY_AMOUNT_OVERFLOW");
        }
        provider_dgb += input.input.value.value;
    }
    if (network_fee > readiness.policy.maximum_network_fee.value ||
        provider_dgb < network_fee) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
    }
    if (!store.CheckProviderRecoveryAdmission(
            recovery_request,
            GetAlternativeRecoveryRequestHash(recovery_request),
            direct.canonical_netgroup, slot.carrier.has_value(),
            DGBSatoshis{network_fee}, now, netgroup_bucket,
            error)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : error);
    }
    const bool need_dd_script =
        slot.carrier.has_value() ||
        recovery_request.service_fee.value > 0;
    if (need_dd_script) {
        const auto destination = wallet->GetNewDestination(
            OutputType::BECH32M,
            "Paymaster recovery DD return");
        if (!destination) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_RECOVERY_CHANGE_UNAVAILABLE");
        }
        parameters.recovery_provider_dd_script =
            GetScriptForDestination(*destination);
    }
    if (provider_dgb > network_fee) {
        const auto destination = wallet->GetNewDestination(
            OutputType::BECH32M,
            "Paymaster recovery DGB change");
        if (!destination) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_RECOVERY_CHANGE_UNAVAILABLE");
        }
        parameters.recovery_provider_dgb_change_script =
            GetScriptForDestination(*destination);
    }
    parameters.maximum_service_fee =
        recovery_request.maximum_service_fee;
    parameters.service_fee = recovery_request.service_fee;
    parameters.network_fee = DGBSatoshis{network_fee};
    parameters.fee_rate = 1;
    parameters.expires_at = std::min(
        recovery_request.expires_at,
        SaturatingAddSeconds(now, readiness.policy.quote_ttl));

    AlternativeRecoveryTemplate built;
    if (!BuildAlternativeRecoveryTemplateAgainstChainstate(
            parameters, Params(), *node->chainman, now, built,
            error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }
    AlternativeRecoveryResponse response;
    response.genesis_hash = recovery_request.genesis_hash;
    response.request_id = recovery_request.request_id;
    response.session_id = recovery_request.session_id;
    response.recovery_id = recovery_id;
    response.recovery_provider_id =
        recovery_request.recovery_provider_id;
    response.recovery_request_hash =
        GetAlternativeRecoveryRequestHash(recovery_request);
    response.manifest = built.manifest;
    response.unsigned_psbt =
        SerializePSBT(built.trusted_template.psbt);
    response.created_at = now;
    response.expires_at = parameters.expires_at;
    response.recovery_commit_key =
        GetAlternativeRecoveryCommitKey(response);
    CKey identity_key;
    ProviderIdentityRecord identity;
    response.identity_signature.resize(64);
    if (!GetPaymasterIdentityKey(*wallet, identity_key, identity,
                                 error) ||
        identity.provider_id !=
            response.recovery_provider_id ||
        !identity_key.SignSchnorr(
            GetAlternativeRecoveryResponseSignatureHash(response),
            response.identity_signature, nullptr, GetRandHash())) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_RECOVERY_RESPONSE_SIGNING_FAILED" : error);
    }
    if (!ValidateAlternativeRecoveryResponse(
            response, recovery_request, identity.identity_key,
            now, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }

    AlternativeRecoveryRecord recovery;
    recovery.provider_side = true;
    recovery.request_id = recovery_request.request_id;
    recovery.session_id = recovery_request.session_id;
    recovery.recovery_id = recovery_id;
    recovery.recovery_request_hash =
        response.recovery_request_hash;
    recovery.original_provider_id =
        recovery_request.original_provider_id;
    recovery.recovery_provider_id =
        recovery_request.recovery_provider_id;
    recovery.privacy_profile =
        recovery_request.privacy_profile;
    recovery.offer_id = recovery_request.offer_id;
    recovery.policy_hash = recovery_request.policy_hash;
    recovery.recovery_provider_identity_key =
        identity.identity_key;
    recovery.recovery_provider_endpoint =
        readiness.endpoint.ToStringAddrPort();
    recovery.original_commit_key =
        recovery_request.original_commit_key;
    recovery.original_template_commitment =
        recovery_request.original_template_commitment;
    recovery.selected_maximum_service_fee =
        recovery_request.maximum_service_fee;
    recovery.selected_service_fee =
        recovery_request.service_fee;
    recovery.client_nonce = recovery_request.client_nonce;
    recovery.capacity_request =
        recovery_request.capacity_request;
    recovery.capacity_snapshot = snapshot;
    recovery.recovery_request = recovery_request;
    recovery.recovery_response = response;
    recovery.provider_safety_policy_hash =
        GetProviderSafetyPolicyHash(readiness.safety_policy);
    recovery.provider_budget_reservation_id =
        response.recovery_commit_key;
    recovery.provider_netgroup_bucket = netgroup_bucket;
    recovery.provider_maximum_network_fee =
        response.manifest.network_fee;
    if (!BuildRecoveryAuthorizationManifest(
            recovery_request, response,
            recovery.recovery_authorization, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    recovery.phase =
        AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    recovery.created_at = recovery.updated_at = now;
    if (!store.CommitProviderAlternativeRecoveryQuote(
            recovery, netgroup_bucket,
            Params().GenesisBlock().GetHash(), now, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    const std::vector<unsigned char> encoded =
        SerializeRecoveryMessage(response);
    const bool queued =
        context.paymaster->QueueOutboundDirectMessage(
            direct.peer_id, Hash(encoded), encoded.size(),
            DirectPayload{response}, now);
    if (queued && node->connman) {
        node->connman->WakeMessageHandler();
    }
    result.queued = queued;
    result.request_id = response.request_id;
    result.session_id = response.session_id;
    result.provider_id = response.recovery_provider_id;
    result.recovery_id = recovery_id;
    result.template_commitment = response.manifest.template_commitment;
    return result;
}

ProviderProcessingResult ProcessProviderRecoveryRequest(
    const ProviderRequestProcessingContext& request,
    const DirectMessage& direct, ProviderProcessingResult& result)
{
    WalletContext& context = request.context;
    CWallet* const wallet = request.wallet;
    node::NodeContext* const node = request.node;
    const int64_t now = request.now;
    std::string& error = request.error;
    result.disposition = ProviderProcessingDisposition::PROCESSED;
    result.message_type = "recovery_request";
    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                              direct};
    const auto* recovery_request =
        std::get_if<AlternativeRecoveryRequest>(&direct.payload);
    if (!recovery_request ||
        !ValidateAlternativeRecoveryRequestEnvelope(
            *recovery_request, Params().GenesisBlock().GetHash(),
            now, error)) {
        return RejectProviderMessage(*context.paymaster, direct,
            error.empty() ? "PAYMASTER_INVALID_RECOVERY_REQUEST" : error,
            "recovery_request");
    }
    if (direct.canonical_netgroup.empty()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
    }
    if (recovery_request->privacy_profile == PrivacyProfile::HIGH) {
        if (!node->connman) {
            throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_NODE_RUNTIME_UNAVAILABLE");
        }
        bool onion_peer{false};
        node->connman->ForEachNode([&](CNode* peer) {
            if (peer->GetId() == direct.peer_id && peer->addr.IsTor()) {
                onion_peer = true;
            }
        });
        if (!onion_peer) {
            return RejectProviderMessage(*context.paymaster, direct,
                "PAYMASTER_RECOVERY_HIGH_PRIVACY_REQUIRES_ONION", "recovery_request");
        }
    }

    PaymasterStore store{*wallet};
    if (const auto replay = ReplayProviderRecoveryRequest(request, direct, *recovery_request, store)) {
        return *replay;
    }
    auto preflight = PreflightProviderRecoveryRequest(request, direct, *recovery_request, store);
    if (const auto* rejection = std::get_if<ProviderProcessingResult>(&preflight)) return *rejection;
    return CommitProviderRecoveryQuote(request, direct, *recovery_request, store,
        std::get<ValidatedProviderRecoveryInputs>(std::move(preflight)));
}

ProviderProcessingResult ProcessProviderQuoteRequest(
    const ProviderRequestProcessingContext& request,
    const DirectMessage& direct, ProviderProcessingResult& result)
{
    WalletContext& context = request.context;
    CWallet* const wallet = request.wallet;
    const ProviderReadiness& readiness = request.readiness;
    node::NodeContext* const node = request.node;
    const bool automatic_service = request.automatic_service;
    const int64_t now = request.now;
    std::string& error = request.error;
    result.message_type = "quote";
    ProviderInboundMessageGuard message_guard{*context.paymaster,
                                              direct};
    const auto* quote_request =
        std::get_if<PaymasterQuoteRequest>(&direct.payload);
    if (!quote_request) {
        if (!AcknowledgeRejectedDirectMessage(
                *context.paymaster, direct, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        throw ApplicationError(ErrorKind::INTERNAL,
                           "PAYMASTER_REQUEST_QUEUE_CORRUPT");
    }

    if (!CheckNewSponsorshipScope(quote_request->intent.sponsorship_scope, error) ||
        !ValidateQuoteRequestEnvelope(*quote_request, Params().GenesisBlock().GetHash(),
                                      now, error)) {
        if (!AcknowledgeRejectedDirectMessage(
                *context.paymaster, direct, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        result.disposition = ProviderProcessingDisposition::REJECTED;
        result.rejection_reason = error;
        return result;
    }
    std::optional<SponsorshipAuthorizationRecord> sponsorship;
    const bool restricted = quote_request->intent.funding_model == FundingModel::SPONSORED &&
                            quote_request->intent.sponsorship_scope ==
                                SponsorshipScope::RESTRICTED;
    const auto validate_restricted_request = [&] {
        if (!restricted) return;
        const auto& descriptor = *quote_request->restricted_descriptor;
        const auto& capability = *quote_request->restricted_capability;
        if (readiness.policy.sponsorship_scope != SponsorshipScope::RESTRICTED ||
            readiness.policy.funding_models != FUNDING_MODEL_SPONSORED ||
            descriptor.policy_hash != GetProviderPolicyHash(readiness.policy) ||
            !ValidateRestrictedServiceDescriptor(descriptor, readiness.identity.identity_key,
                                                 Params().GenesisBlock().GetHash(), now, error,
                                                 Params().GetChainType() == ChainType::REGTEST) ||
            !ValidateSponsorshipCapability(capability, descriptor,
                                           quote_request->intent.recipient_script,
                                           quote_request->intent.recipient_amount,
                                           quote_request->intent.client_nonce,
                                           Params().GenesisBlock().GetHash(), now, error) ||
            !ValidateRestrictedAdmissionProofs(descriptor, readiness.identity.identity_key,
                                               *node->chainman, error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               error.empty() ? "PAYMASTER_RESTRICTED_POLICY_MISMATCH" : error);
        }
    };

    PaymasterStore store{*wallet};
    const std::vector<unsigned char> redacted_request =
        SerializeRedactedQuoteRequest(*quote_request);
    PaymentSession persisted_session;
    const auto session_status = store.GetSessionByRequestIdWithStatus(
        quote_request->intent.request_id, persisted_session);
    if (session_status != DatabaseReadStatus::FOUND &&
        session_status != DatabaseReadStatus::NOT_FOUND) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DATABASE_READ");
    }
    if (session_status == DatabaseReadStatus::FOUND) {
        // An intact client session or retained tombstone owns this request ID,
        // but does not promise a provider attempt that can answer this peer.
        if (!persisted_session.provider_side) {
            return RejectProviderMessage(*context.paymaster, direct,
                "PAYMASTER_REQUEST_ID_CONFLICT", "quote");
        }
        ProviderAttempt persisted_attempt;
        PaymentIntent authorized_intent;
        PaymasterQuote authorized_quote;
        CollaborativePSBTTemplate trusted;
        // Validate saved authority independently of the incoming request. A
        // conflicting peer must never hide a damaged local attempt as rejection.
        if (persisted_session.attempt_ids.size() != 1 ||
            !store.GetAttempt(persisted_session.attempt_ids.front(), persisted_attempt) ||
            !LoadAttemptAuthorizationArtifacts(persisted_attempt, authorized_intent,
                authorized_quote, trusted, error) ||
            persisted_session.request_id != authorized_intent.request_id ||
            persisted_session.session_id != authorized_intent.session_id ||
            persisted_session.canonical_request_hash != Hash(persisted_attempt.quote_request) ||
            persisted_attempt.session_id != authorized_intent.session_id ||
            persisted_attempt.provider_id != authorized_intent.provider_id ||
            persisted_attempt.client_nonce != authorized_intent.client_nonce ||
            persisted_attempt.intent_hash != GetPaymentIntentHash(authorized_intent) ||
            persisted_attempt.created_at <= 0 ||
            persisted_attempt.quote_expires_at <= persisted_attempt.created_at ||
            persisted_attempt.retry_until < persisted_attempt.quote_expires_at ||
            persisted_attempt.created_at != authorized_quote.created_at ||
            persisted_attempt.quote_expires_at != authorized_quote.expires_at ||
            persisted_attempt.retry_until != authorized_quote.retry_until) {
            throw ApplicationError(ErrorKind::WALLET,
                "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
        }

        PaymasterQuoteRequest persisted_request;
        PaymasterQuoteResponse response;
        try {
            CDataStream request_stream{persisted_attempt.quote_request, SER_NETWORK,
                                       ::PROTOCOL_VERSION};
            request_stream >> persisted_request;
            SpanReader stream{::PROTOCOL_VERSION, persisted_attempt.signed_quote};
            stream >> response;
            if (!request_stream.empty() ||
                SerializeRedactedQuoteRequest(persisted_request) != persisted_attempt.quote_request ||
                !IsSupportedDirectProtocolVersion(persisted_request.version) ||
                !IsSupportedDirectProtocolVersion(response.version) ||
                !stream.empty() || SerializeQuoteResponse(response) !=
                                       persisted_attempt.signed_quote) {
                throw std::ios_base::failure("non-canonical persisted quote");
            }
        } catch (const std::ios_base::failure&) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
        }
        if (response.request_id != persisted_session.request_id ||
            response.session_id != persisted_session.session_id ||
            response.quote.provider_id != persisted_attempt.provider_id ||
            response.quote.quote_id != persisted_attempt.quote_id ||
            response.quote.template_commitment !=
                persisted_attempt.template_commitment ||
            !ValidateQuoteResponseEnvelope(response, Params().GenesisBlock().GetHash(),
                response.quote.created_at, error) ||
            !persisted_attempt.provider_identity_key.IsFullyValid() ||
            GetPaymasterId(persisted_attempt.provider_identity_key) !=
                persisted_attempt.provider_id ||
            !persisted_attempt.provider_identity_key.VerifySchnorr(
                GetPaymasterQuoteSignatureHash(response.quote),
                response.quote.identity_signature) ||
            response.quote.template_commitment !=
                GetCollaborativeTemplateCommitment(authorized_intent, response.quote, trusted)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
        }
        if (persisted_session.session_id != quote_request->intent.session_id ||
            persisted_session.canonical_request_hash != Hash(redacted_request) ||
            persisted_attempt.provider_id != readiness.identity.provider_id ||
            persisted_attempt.client_nonce != quote_request->intent.client_nonce ||
            persisted_attempt.intent_hash != GetPaymentIntentHash(quote_request->intent) ||
            persisted_attempt.quote_request != redacted_request) {
            return RejectProviderMessage(*context.paymaster, direct,
                "PAYMASTER_REQUEST_ID_CONFLICT", "quote");
        }
        if (persisted_attempt.quote_expires_at <= now ||
            persisted_attempt.retry_until < now) {
            return RejectProviderMessage(*context.paymaster, direct,
                "PAYMASTER_PROVIDER_RETRY_EXPIRED", "quote");
        }
        validate_restricted_request();

        const uint256 message_id = Hash(persisted_attempt.signed_quote);
        const bool queued = context.paymaster->QueueOutboundDirectMessage(
            direct.peer_id, message_id,
            persisted_attempt.signed_quote.size(), DirectPayload{response}, now);
        if (queued && node->connman) node->connman->WakeMessageHandler();
        result.queued = queued;
        result.request_id = response.request_id;
        result.session_id = response.session_id;
        result.provider_id = response.quote.provider_id;
        result.attempt_id = persisted_attempt.attempt_id;
        result.quote_id = response.quote.quote_id;
        result.template_commitment = response.quote.template_commitment;
        return result;
    }

    if (direct.canonical_netgroup.empty()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE");
    }
    if (!CapacityContinuationMayProceed(readiness)) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_DRAIN_ONLY");
    }
    uint256 provider_netgroup_bucket;

    if (wallet->IsLocked()) {
        throw ApplicationError(ErrorKind::UNLOCK,
                           "Provider wallet unlock is required for a new quote");
    }
    if (!ProviderTxIndexIsReady(*wallet, !automatic_service)) {
        throw ApplicationError(ErrorKind::GENERAL, "PAYMASTER_REQUIRES_READY_TXINDEX");
    }
    validate_restricted_request();

    // These preflight inputs are local. A malformed policy or clock must not
    // turn a request into a permanent peer rejection.
    if (!ValidateProviderPolicy(readiness.policy, error) || now <= 0) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_INVALID_QUOTE_BUILD_PARAMETERS" : error);
    }
    std::vector<CollaborativeInput> user_inputs;
    for (const COutPoint& outpoint : quote_request->intent.user_dd_inputs) {
        uint256 block_hash;
        CTransactionRef creating_tx;
        if (!g_txindex->FindTx(outpoint.hash, block_hash, creating_tx) || !creating_tx) {
            bool missing_or_spent;
            {
                LOCK(cs_main);
                missing_or_spent = node->chainman->ActiveChainstate()
                                       .CoinsTip().AccessCoin(outpoint).IsSpent();
            }
            if (missing_or_spent) {
                return RejectProviderMessage(
                    *context.paymaster, direct,
                    "PAYMASTER_USER_PREVOUT_NOT_FOUND", "quote");
            }
            // A live UTXO proves that its creating transaction should be
            // resolvable. FindTx(false) alone also covers local index/I/O errors.
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_USER_PREVOUT_LOOKUP_FAILED");
        }
        user_inputs.push_back({outpoint, creating_tx, InputRole::USER_DD});
    }
    ProviderQuotePreflightResult quote_preflight;
    bool valid_preflight;
    {
        LOCK(cs_main);
        valid_preflight = PreflightProviderQuoteRequest(
            *quote_request, readiness.policy, user_inputs, Params(),
            node->chainman->ActiveChainstate().CoinsTip(), now,
            quote_preflight, error);
    }
    if (!valid_preflight) {
        return RejectProviderMessage(*context.paymaster, direct, error, "quote");
    }
    const ServiceFeePlan* const fee_plan{&quote_preflight.fee_plan};
    const bool needs_carrier = fee_plan->output_kind == FeeOutputKind::CARRIER_SUCCESSOR;
    const CAmount network_fee{COIN / 10};
    if (!store.CheckProviderQuoteAdmission(
            *quote_request, Hash(redacted_request),
            direct.canonical_netgroup, needs_carrier,
            DGBSatoshis{network_fee}, now,
            provider_netgroup_bucket, error)) {
        if (IsPermanentCapacityContinuationError(error)) {
            // The exact quote can no longer be authorized because its
            // preceding Capacity reservation is absent, expired, or
            // conflicts with the persisted proof. Consume only this
            // invalid inbox entry and leave the provider's maintenance
            // state intact; a remote peer must not be able to turn a
            // safely rejected continuation into a provider fault.
            const std::string rejection_reason{error};
            if (!AcknowledgeRejectedDirectMessage(
                    *context.paymaster, direct, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            result.disposition = ProviderProcessingDisposition::REJECTED;
            result.rejection_reason = rejection_reason;
            return result;
        }
        if (error == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED") {
            // A recipient/netgroup or proposed-fee limit is a decision
            // about this request, not a provider-wide drain command.
            // Keep the exact continuation in the fair inbox and repeat
            // all admission checks next time, within its original TTL.
            message_guard.Defer();
            result.disposition = ProviderProcessingDisposition::DEFERRED;
            result.deferral_reason = error;
            return result;
        }
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : error);
    }
    std::vector<ProviderPoolEntry> available;
    if (!GetPaymasterProviderPoolEntries(*wallet, available)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOLS_NOT_PREPARED");
    }
    std::sort(available.begin(), available.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.outpoint < rhs.outpoint;
    });
    const auto usable = [&](const ProviderPoolEntry& entry, PoolAsset asset) {
        return entry.purpose == PoolPurpose::OPERATIONAL && entry.asset == asset &&
               entry.state == PoolEntryState::RESERVED &&
               entry.reservation_id == quote_request->intent.client_nonce &&
               entry.confirmation_height > 0;
    };
    const auto dgb_it = std::find_if(available.begin(), available.end(), [&](const auto& entry) {
        return usable(entry, PoolAsset::DGB) &&
               entry.dgb_value.value >= readiness.policy.maximum_network_fee.value;
    });
    const auto carrier_it = std::find_if(available.begin(), available.end(), [&](const auto& entry) {
        return usable(entry, PoolAsset::DD_CARRIER);
    });
    if (dgb_it == available.end() || (needs_carrier && carrier_it == available.end())) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_OPERATIONAL_SLOT_MISSING");
    }
    if (readiness.policy.maximum_network_fee.value < network_fee ||
        dgb_it->dgb_value.value < network_fee) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
    }
    CTransactionRef dgb_creating_tx;
    CTransactionRef carrier_creating_tx;
    {
        LOCK(wallet->cs_wallet);
        const CWalletTx* dgb_wallet_tx = wallet->GetWalletTx(dgb_it->outpoint.hash);
        if (dgb_wallet_tx) dgb_creating_tx = dgb_wallet_tx->tx;
        if (needs_carrier) {
            const CWalletTx* carrier_wallet_tx = wallet->GetWalletTx(carrier_it->outpoint.hash);
            if (carrier_wallet_tx) carrier_creating_tx = carrier_wallet_tx->tx;
        }
    }
    if (!dgb_creating_tx || (needs_carrier && !carrier_creating_tx)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_POOL_PREVOUT_NOT_FOUND");
    }

    ProviderQuoteBuildParameters parameters;
    do {
        parameters.quote_id = GetRandHash();
    } while (parameters.quote_id.IsNull());
    parameters.user_inputs = std::move(user_inputs);
    parameters.reserved_dgb_inputs.push_back(
        {dgb_it->outpoint, CMutableTransaction{*dgb_creating_tx}, dgb_it->dgb_value});
    if (needs_carrier) {
        parameters.reserved_carrier = VerifiedDDCarrier{
            carrier_it->outpoint, CMutableTransaction{*carrier_creating_tx},
            carrier_it->carrier_value};
        const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                           "Paymaster carrier successor");
        if (!destination) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CHANGE_UNAVAILABLE");
        parameters.carrier_return_script = GetScriptForDestination(*destination);
    } else if (fee_plan->output_kind == FeeOutputKind::PROVIDER_OUTPUT) {
        const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                           "Paymaster service fee");
        if (!destination) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CHANGE_UNAVAILABLE");
        parameters.provider_fee_script = GetScriptForDestination(*destination);
    }
    if (dgb_it->dgb_value.value > network_fee) {
        const auto destination = wallet->GetNewDestination(OutputType::BECH32M,
                                                           "Paymaster DGB change");
        if (!destination) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CHANGE_UNAVAILABLE");
        parameters.dgb_change_script = GetScriptForDestination(*destination);
    }
    parameters.network_fee = DGBSatoshis{network_fee};
    parameters.created_at = now;
    parameters.expires_at = SaturatingAddSeconds(now, readiness.policy.quote_ttl);
    parameters.retry_until = SaturatingAddSeconds(now, DEFAULT_RETRY_SECONDS);
    ProviderQuoteBuildResult built;
    {
        LOCK(cs_main);
        if (!BuildUnsignedProviderQuote(*quote_request, readiness.policy, parameters,
                                        Params(), node->chainman->ActiveChainstate().CoinsTip(),
                                        built, error)) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, error);
        }
    }
    ProviderAuthorizationManifest provider_manifest;
    if (!BuildProviderAuthorizationManifest(
            quote_request->intent, built.quote, built.trusted_template,
            GetProviderSafetyPolicyHash(readiness.safety_policy),
            provider_manifest, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (!ValidateProviderAuthorizationOwnership(
            *wallet, provider_manifest, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    CKey identity_key;
    ProviderIdentityRecord identity;
    built.quote.identity_signature.resize(64);
    if (!GetPaymasterIdentityKey(*wallet, identity_key, identity, error) ||
        !identity_key.SignSchnorr(GetPaymasterQuoteSignatureHash(built.quote),
                                  built.quote.identity_signature, nullptr,
                                  GetRandHash())) {
        throw ApplicationError(ErrorKind::WALLET,
                           error.empty() ? "PAYMASTER_QUOTE_SIGNING_FAILED" : error);
    }
    PaymasterQuoteResponse response;
    response.version = quote_request->version;
    response.request_id = quote_request->intent.request_id;
    response.session_id = quote_request->intent.session_id;
    response.quote = built.quote;
    ProviderAttempt attempt;
    do {
        attempt.attempt_id = GetRandHash();
    } while (attempt.attempt_id.IsNull());
    attempt.session_id = response.session_id;
    attempt.provider_id = response.quote.provider_id;
    attempt.provider_identity_key = identity.identity_key;
    attempt.state = AttemptState::QUOTED;
    attempt.client_nonce = quote_request->intent.client_nonce;
    attempt.intent_hash = response.quote.intent_hash;
    attempt.quote_id = response.quote.quote_id;
    attempt.unsigned_txid = response.quote.unsigned_txid;
    attempt.template_commitment = response.quote.template_commitment;
    attempt.commit_key = GetPaymasterCommitKey(attempt.provider_id, attempt.client_nonce,
                                               attempt.intent_hash, attempt.quote_id,
                                               attempt.template_commitment);
    if (restricted) {
        attempt.sponsorship_capability_hash =
            GetSponsorshipCapabilityHash(*quote_request->restricted_capability);
        SponsorshipAuthorizationRecord authorization;
        authorization.capability_hash = attempt.sponsorship_capability_hash;
        authorization.payment_binding_hash = attempt.intent_hash;
        authorization.reservation_id = attempt.commit_key;
        authorization.reserved_at = now;
        authorization.expires_at = quote_request->restricted_capability->expires_at;
        sponsorship = std::move(authorization);
    }
    attempt.quote_request = redacted_request;
    attempt.signed_quote = SerializeQuoteResponse(response);
    attempt.provider_manifest = std::move(provider_manifest);
    attempt.provider_netgroup_bucket = provider_netgroup_bucket;
    attempt.unsigned_transaction = SerializeTransaction(response.quote.unsigned_transaction);
    attempt.unsigned_psbt = SerializePSBT(built.trusted_template.psbt);
    for (const InputRole role : built.trusted_template.input_roles) {
        attempt.input_roles.push_back(static_cast<ReservationRole>(role));
    }
    attempt.quote_expires_at = response.quote.expires_at;
    attempt.retry_until = response.quote.retry_until;
    attempt.created_at = attempt.updated_at = now;
    if (!store.CommitProviderQuote(
            attempt, sponsorship, Params().GenesisBlock().GetHash(),
            now, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    const std::vector<unsigned char> response_bytes = SerializeQuoteResponse(response);
    const uint256 message_id = Hash(response_bytes);
    const bool queued = context.paymaster->QueueOutboundDirectMessage(
        direct.peer_id, message_id, response_bytes.size(), DirectPayload{response}, now);
    if (queued && node->connman) node->connman->WakeMessageHandler();
    result.queued = queued;
    result.request_id = response.request_id;
    result.session_id = response.session_id;
    result.provider_id = response.quote.provider_id;
    result.attempt_id = attempt.attempt_id;
    result.quote_id = response.quote.quote_id;
    result.template_commitment = response.quote.template_commitment;
    return result;
}

} // namespace

ProviderProcessingResult ProcessPaymasterRequests(WalletContext& context, CWallet& target, ExecutionMode mode)
{
    CWallet* const wallet = &target;
    using namespace DigiDollar::Paymaster;
    const bool automatic_service = mode == ExecutionMode::AUTOMATIC;
    if (automatic_service) {
        if (!AutomaticProviderStateIsSynchronized(*wallet)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_SYNCING");
        }
    } else {
        wallet->BlockUntilSyncedToCurrentChain();
    }
    const ProviderReadiness readiness =
        GetProviderReadiness(*wallet, context, !automatic_service);
    if (automatic_service &&
        std::find(readiness.errors.begin(), readiness.errors.end(),
                  "PAYMASTER_REQUIRES_READY_TXINDEX") !=
            readiness.errors.end()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_SYNCING");
    }
    if (!context.paymaster || !readiness.have_identity ||
        !context.paymaster->IsProviderRunning(wallet->GetName(), readiness.identity.provider_id)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_NOT_RUNNING");
    }
    if (readiness.settings.operation_mode ==
            ProviderOperationMode::AUTOMATIC &&
        !automatic_service) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE");
    }
    ProviderWorkGuard work_guard{*context.paymaster,
                                 wallet->GetName(),
                                 readiness.identity.provider_id};
    if (!work_guard.Acquired()) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_SERVICE_BUSY");
    }
    ProviderProcessingResult result;
    result.queued = false;
    const int64_t now = GetTime();
    std::string error;
    node::NodeContext* node = wallet->chain().context();
    if (!node || !node->chainman) {
        throw ApplicationError(ErrorKind::INTERNAL, "Node context unavailable");
    }
    const ProviderRequestProcessingContext request_context{
        context, wallet, readiness, node, automatic_service, now, error};

    // A reserved slot can make fresh capacity ineligible without stranding
    // admitted quote/recovery work. Dispatch only one eligible message per
    // call; a deferred capacity request stays queued, but its lease advances
    // the cursor so the next call gives another eligible type its turn.
    const auto request_lease = context.paymaster->LeaseProviderRequest(
        readiness.identity.provider_id, !automatic_service || readiness.ready);
    const auto& messages = request_lease.Messages();
    result.disposition = messages.empty() ? ProviderProcessingDisposition::NO_MESSAGE : ProviderProcessingDisposition::PROCESSED;
    if (messages.empty()) return result;
    const DirectMessage& direct = messages.front();
    if (std::holds_alternative<PaymasterCapacityRequest>(direct.payload)) {
        return ProcessProviderCapacityRequest(request_context, direct);
    }
    if (std::holds_alternative<AlternativeRecoveryRequest>(direct.payload)) {
        return ProcessProviderRecoveryRequest(
            request_context, direct, result);
    }
    return ProcessProviderQuoteRequest(request_context, direct, result);
}

PaymentResult ProcessPaymasterResult(WalletContext& context, CWallet& target, const std::string& request_id)
{
    CWallet* const wallet = &target;
    if (!context.paymaster || !context.paymaster->Enabled()) {
        throw ApplicationError(ErrorKind::GENERAL, "DigiDollar Paymaster support is disabled");
    }
    if (!IsCanonicalRequestId(request_id)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "request_id must be a canonical lowercase UUID");
    }
    PaymasterStore store{*wallet};
    PaymentSession session;
    if (!FindClientSession(request_id, store, session)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND");
    }
    if (session.provider_side || session.attempt_ids.empty()) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_RESULT_INELIGIBLE");
    }
    ProviderAttempt attempt;
    if (!store.GetAttempt(session.attempt_ids.back(), attempt) ||
        !attempt.provider_identity_key.IsFullyValid()) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_IDENTITY_NOT_PERSISTED");
    }
    const int64_t operation_now{GetTime()};
    std::string error;
    if (!DrainClientAttemptEquivocations(
            *context.paymaster, store, session, attempt, error,
            EquivocationBlockPolicy::OBSERVE_ONLY)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    PaymentIntent authorized_intent;
    PaymasterQuote authorized_quote;
    CollaborativePSBTTemplate authorized_template;
    if (!HasDurableClientAuthorization(attempt, operation_now) ||
        attempt.client_manifest_accepted_at > session.updated_at ||
        !LoadAttemptAuthorizationArtifacts(
            attempt, authorized_intent, authorized_quote,
            authorized_template, error) ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, authorized_intent,
            authorized_quote, attempt.capacity_snapshot,
            authorized_template, error) ||
        !ValidateClientAuthorizationOwnership(
            *wallet, attempt.client_manifest, error)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED" : error);
    }
    const bool observed_payment = ReconcileConfirmedClientPayment(*wallet, session, attempt);
    const auto result_lease = context.paymaster->LeaseResults(
        request_id, session.session_id, attempt.provider_id, observed_payment ? 0 : 1);
    const auto& messages = result_lease.Messages();
    const PaymasterResultMessage* message{nullptr};
    if (!messages.empty()) {
        message = std::get_if<PaymasterResultMessage>(&messages.front().payload);
        if (!message) throw ApplicationError(ErrorKind::INTERNAL, "PAYMASTER_RESULT_QUEUE_CORRUPT");
        // Authenticate against this wallet's own durable attempt before a
        // local predecessor read can fail. Retention grants no authority: the
        // latest sequence and full final transaction are still checked below.
        if (!ValidatePaymasterResult(message->result, Params().GenesisBlock().GetHash(),
                                     attempt.provider_id, attempt.commit_key,
                                     attempt.provider_identity_key, 1, error)) {
            const std::string rejection{error};
            if (!AcknowledgeRejectedDirectMessage(*context.paymaster, messages.front(), error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(ErrorKind::INVALID_INPUT, rejection);
        }
        if (!context.paymaster->RetainAuthenticatedResult(messages.front())) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DIRECT_INBOX_RETAIN_CONFLICT");
        }
    }
    uint64_t minimum_sequence{1};
    if (!observed_payment) {
        PaymasterResult previous;
        const DatabaseReadStatus result_status = store.GetProviderResultWithStatus(attempt.commit_key, previous);
        if (result_status == DatabaseReadStatus::FOUND) {
            // Readable local bytes are not sequence authority until their
            // signature and wallet-local binding have been authenticated.
            if (!ValidatePaymasterResult(previous, Params().GenesisBlock().GetHash(),
                                         attempt.provider_id, attempt.commit_key,
                                         attempt.provider_identity_key, 1, error)) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RESULT_DATABASE_READ");
            }
            minimum_sequence = previous.result_sequence;
        } else if (result_status != DatabaseReadStatus::NOT_FOUND) {
            // Keep the incoming result queued until its durable predecessor can be read.
            throw ApplicationError(ErrorKind::WALLET,
                result_status == DatabaseReadStatus::UNSUPPORTED_VERSION
                    ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_RESULT_DATABASE_READ");
        }
    }
    std::optional<PaymasterResult> accepted_result;
    FinalTransactionPresence final_presence{FinalTransactionPresence::NONE};
    if (message) {
        if (minimum_sequence > 1 && !ValidatePaymasterResult(message->result, Params().GenesisBlock().GetHash(),
                                     attempt.provider_id, attempt.commit_key,
                                     attempt.provider_identity_key, minimum_sequence, error)) {
            const std::string rejection{error};
            if (!AcknowledgeRejectedDirectMessage(*context.paymaster, messages.front(), error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(ErrorKind::INVALID_INPUT, rejection);
        }
        if (message->result.final_transaction) {
            CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            if (!ValidateFinalCollaborativeTransaction(
                    *message->result.final_transaction, *message->result.txid,
                    *message->result.raw_transaction_hash, trusted, error)) {
                const std::string validation_error{error};
                std::string persistence_error;
                if (!MarkFinalValidationFailureRecoverable(
                        store, session, attempt, GetTime(), persistence_error)) {
                    throw ApplicationError(ErrorKind::WALLET, persistence_error);
                }
                if (!AcknowledgeRejectedDirectMessage(*context.paymaster, messages.front(), persistence_error)) {
                    throw ApplicationError(ErrorKind::WALLET, persistence_error);
                }
                throw ApplicationError(ErrorKind::INVALID_INPUT, validation_error);
            }
            if (!PreflightFinalPaymasterTransaction(
                    *wallet, MakeTransactionRef(*message->result.final_transaction),
                    final_presence, error)) {
                const std::string preflight_error{error};
                const bool transient =
                    IsTransientPaymasterFinalizationError(preflight_error);
                if (!transient) {
                    std::string persistence_error;
                    if (!MarkFinalValidationFailureRecoverable(
                            store, session, attempt, GetTime(), persistence_error)) {
                        throw ApplicationError(ErrorKind::WALLET, persistence_error);
                    }
                }
                throw ApplicationError(
                    transient ? ErrorKind::WALLET : ErrorKind::REJECTED,
                    preflight_error);
            }
        }
        if (!DrainClientAttemptEquivocations(
                *context.paymaster, store, session, attempt, error,
                EquivocationBlockPolicy::OBSERVE_ONLY)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!store.StoreClientResult(message->result, Params().GenesisBlock().GetHash(),
                                     attempt.attempt_id, GetTime(), error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!context.paymaster->AcknowledgeDirectMessagesIfPresent({messages.front().message_id})) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DIRECT_INBOX_ACK_CONFLICT");
        }
        accepted_result = message->result;
        const bool final = message->result.final_transaction.has_value();
        if (final) {
            if (const auto* node = wallet->chain().context(); node && node->connman) {
                node->connman->ReleasePaymasterConnection(PaymentChannelKey(*wallet, session, attempt.provider_id));
            }
            const CMutableTransaction& transaction = *message->result.final_transaction;
            const CTransaction exact_final{transaction};
            const auto fail_after_final_store =
                [&](ErrorKind rpc_code, const std::string& failure) -> void {
                std::string persistence_error;
                if (!store.RecordClientFinalConflict(
                        request_id, attempt.attempt_id,
                        exact_final.GetHash(), exact_final.GetWitnessHash(),
                        GetTime(), persistence_error)) {
                    throw ApplicationError(
                        ErrorKind::WALLET,
                        persistence_error.empty() ? "PAYMASTER_FINAL_CONFLICT_NOT_PERSISTED" : persistence_error);
                }
                throw ApplicationError(
                    rpc_code,
                    failure.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : failure);
            };
            // StoreClientResult commits the signed result, final
            // artifacts, user-authorization evidence, attempt and
            // session transition in one wallet transaction.
            if (!store.GetAttempt(attempt.attempt_id, attempt) ||
                !store.GetSessionByRequestId(request_id, session)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
            }

            // Result persistence and state reconciliation can perform
            // database work. Reload and revalidate the exact authority
            // and final bytes at the last possible point before the
            // transaction is handed to the node for broadcast.
            ProviderAttempt broadcast_attempt;
            PaymentIntent broadcast_intent;
            PaymasterQuote broadcast_quote;
            CollaborativePSBTTemplate broadcast_template;
            FinalTransactionPresence broadcast_presence{
                FinalTransactionPresence::NONE};
            if (!store.GetAttempt(attempt.attempt_id, broadcast_attempt)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
            }
            if (!HasDurableClientAuthorization(
                    broadcast_attempt, GetTime()) ||
                broadcast_attempt.client_manifest_accepted_at >
                    session.updated_at ||
                !LoadAttemptAuthorizationArtifacts(
                    broadcast_attempt, broadcast_intent, broadcast_quote,
                    broadcast_template, error) ||
                !ValidateClientAuthorizationManifest(
                    broadcast_attempt.client_manifest, broadcast_intent,
                    broadcast_quote, broadcast_attempt.capacity_snapshot,
                    broadcast_template, error) ||
                !ValidateClientAuthorizationOwnership(
                    *wallet, broadcast_attempt.client_manifest, error) ||
                !ValidateFinalCollaborativeTransaction(
                    transaction, CTransaction{transaction}.GetHash(),
                    CTransaction{transaction}.GetWitnessHash(),
                    broadcast_template, error) ||
                broadcast_attempt.final_txid != CTransaction{transaction}.GetHash() ||
                broadcast_attempt.final_transaction !=
                    SerializeTransaction(transaction)) {
                fail_after_final_store(
                    ErrorKind::REJECTED,
                    error.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            if (!PreflightFinalPaymasterTransaction(
                    *wallet, MakeTransactionRef(transaction),
                    broadcast_presence, error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                fail_after_final_store(ErrorKind::REJECTED, error);
            }
            if (!store.ValidateClientDurableFinalForBroadcast(
                    CTransaction{transaction}, GetTime(),
                    broadcast_presence != FinalTransactionPresence::NONE,
                    error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                fail_after_final_store(
                    ErrorKind::REJECTED,
                    error.empty() ? "PAYMASTER_CLIENT_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session,
                    broadcast_attempt, error,
                    EquivocationBlockPolicy::OBSERVE_ONLY)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            bool already_confirmed{false};
            std::string broadcast_error;
            if (InsertAndBroadcastPaymasterTransaction(
                    *wallet, MakeTransactionRef(transaction), already_confirmed,
                    broadcast_error)) {
                if (!store.RecordClientFinalObservation(
                        request_id, attempt.attempt_id,
                        already_confirmed, GetTime(), error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
            } else {
                if (IsTransientPaymasterFinalizationError(
                        broadcast_error)) {
                    throw ApplicationError(
                        ErrorKind::WALLET,
                        broadcast_error.empty() ? "PAYMASTER_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
                }
                fail_after_final_store(
                    ErrorKind::REJECTED,
                    broadcast_error.empty() ? "PAYMASTER_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
            }
        } else if (message->result.status == PaymasterResultStatus::REJECTED ||
                   message->result.status == PaymasterResultStatus::SLOT_UNAVAILABLE) {
            if (!store.GetAttempt(attempt.attempt_id, attempt) ||
                !store.GetSessionByRequestId(request_id, session)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
        }
    }
    if (!observed_payment) {
        LOCK(wallet->cs_wallet);
        if (!store.GetSessionByRequestId(request_id, session) || session.attempt_ids.empty() ||
            !store.GetAttempt(session.attempt_ids.back(), attempt)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_FINAL_STATE_RELOAD_FAILED");
        }
    }

    // Collaborative Paymaster transactions bypass the ordinary
    // TransferDigiDollarMany() path that records an outgoing DD row.
    // Once exact final bytes are durable, repair that display-only
    // history idempotently. A history-write failure must never turn a
    // valid, possibly already broadcast payment into an RPC failure;
    // every later result poll will retry the same deterministic row.
    if (!attempt.final_txid.IsNull()) {
        if (DigiDollarWallet* dd_wallet = wallet->GetDDWallet()) {
            std::string history_error;
            const CAmount user_total =
                authorized_intent.recipient_amount.value +
                authorized_quote.service_fee.value;
            if (!dd_wallet->RecordPaymasterSendHistory(
                    attempt.final_txid,
                    authorized_intent.recipient_script,
                    user_total,
                    history_error)) {
                LogPrintf(
                    "Paymaster: unable to record client DD history: %s\n",
                    history_error);
            }
        }
    }
    PaymentResult result;
    result.processed = observed_payment || accepted_result.has_value();
    result.request_id = request_id;
    result.session_id = session.session_id;
    result.provider_id = attempt.provider_id;
    // Always echo the exact durable authorization, including on the
    // final result response. Callers must never reconstruct a funding
    // model or fee from a stale directory preview, and Qt needs to
    // distinguish a completed payment from a changed pre-signing
    // authorization without guessing missing fields.
    result.offer_id = authorized_intent.offer_id;
    result.policy_hash = authorized_intent.policy_hash;
    result.funding_model = authorized_intent.funding_model;
    result.payment_cents = authorized_intent.recipient_amount;
    result.service_fee_cents = authorized_quote.service_fee;
    result.user_total_cents = DDCents{authorized_intent.recipient_amount.value +
                      authorized_quote.service_fee.value};
    result.authorization_commitment = attempt.client_manifest.manifest_id;
    result.authorization_accepted = attempt.accepted_client_manifest_id ==
                attempt.client_manifest.manifest_id &&
            attempt.client_manifest_accepted_at > 0;
    result.session_state = session.state;
    result.attempt_state = attempt.state;
    if (accepted_result) {
        result.result_status = accepted_result->status;
        result.result_sequence = accepted_result->result_sequence;
        if (accepted_result->txid) result.txid = *accepted_result->txid;
    }
    if (!session.final_txid.IsNull()) result.txid = session.final_txid;
    return result;
}

PaymentResult CaptureSignedClientPSBT(const PaymentSession& session,
                               const ProviderAttempt& attempt,
                               const PaymasterSubmitQueueState& queue_state)
{
    PaymentResult result;
    result.psbt = attempt.user_signed_psbt;
    result.request_id = session.request_id;
    result.session_id = session.session_id;
    result.provider_id = attempt.provider_id;
    result.attempt_id = attempt.attempt_id;
    result.quote_id = attempt.quote_id;
    result.unsigned_txid = attempt.unsigned_txid;
    result.template_commitment = attempt.template_commitment;
    if (!attempt.client_manifest.manifest_id.IsNull()) {
        result.authorization_commitment = attempt.client_manifest.manifest_id;
    }
    const bool authorization_accepted =
        attempt.accepted_client_manifest_id ==
            attempt.client_manifest.manifest_id &&
        !attempt.accepted_client_manifest_id.IsNull() &&
        attempt.client_manifest_accepted_at > 0;
    result.authorization_accepted = authorization_accepted;
    if (authorization_accepted) {
        result.authorization_accepted_at = attempt.client_manifest_accepted_at;
    }
    result.session_state = session.state;
    result.attempt_state = attempt.state;
    result.expires_at = attempt.retry_until;
    result.queued = queue_state.queued;
    result.connection_pending = queue_state.connection_pending;
    result.route_available = queue_state.route_available;
    if (!session.final_txid.IsNull()) result.txid = session.final_txid;
    return result;
}

PaymentResult AuthorizePaymasterPSBT(WalletContext& context, CWallet& wallet, const std::vector<unsigned char>& encoded_psbt,
                              const std::optional<uint256>& authorization_commitment)
{
    std::string error;
    if (!CheckPaymasterClientReadiness(wallet, context, error)) throw ApplicationError(ErrorKind::GENERAL, error);
    PartiallySignedTransaction candidate;
    if (!DeserializePaymasterBytes(encoded_psbt, "PSBT", candidate, error) || !candidate.tx)
        throw ApplicationError(ErrorKind::DESERIALIZATION, strprintf("Paymaster PSBT decode failed: %s", error));
    return AuthorizePaymasterPSBT(context, wallet, std::move(candidate), authorization_commitment);
}

PaymentResult AuthorizePaymasterPSBT(WalletContext& context, CWallet& target, PartiallySignedTransaction candidate,
                              const std::optional<uint256>& supplied_authorization_commitment)
{
    CWallet* const wallet = &target;
    std::string error;
    if (!CheckPaymasterClientReadiness(*wallet, context, error)) throw ApplicationError(ErrorKind::GENERAL, error);
    if (!candidate.tx) throw ApplicationError(ErrorKind::DESERIALIZATION, "PAYMASTER_INVALID_PSBT");
    const uint256 unsigned_txid = CTransaction{*candidate.tx}.GetHash();
    PaymasterStore store{*wallet};
    DigiDollar::Paymaster::ProviderAttempt attempt;
    if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
        throw ApplicationError(ErrorKind::WALLET, "No persistent Paymaster template matches this PSBT");
    }
    DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
    if (!LoadTrustedTemplate(attempt, trusted, error) ||
        !DigiDollar::Paymaster::ValidateCollaborativePSBT(
            candidate, trusted,
            DigiDollar::Paymaster::CollaborativeSignatureStage::UNSIGNED, error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, error);
    }

    DigiDollar::Paymaster::PaymentSession session;
    if (!store.GetSessionBySessionId(attempt.session_id, session)) {
        throw ApplicationError(ErrorKind::WALLET, "Paymaster session not found");
    }
    if (!DrainClientSessionEquivocations(
            *context.paymaster, store, session, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (supplied_authorization_commitment &&
        *supplied_authorization_commitment != attempt.client_manifest.manifest_id) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
    }
    if (ReconcileConfirmedClientPayment(*wallet, session, attempt) ||
        (session.state == SessionState::CONFIRMED &&
         !session.final_txid.IsNull() && session.final_txid == attempt.final_txid)) {
        return CaptureSignedClientPSBT(session, attempt, {});
    }
    const int64_t now = GetTime();
    PaymentIntent authorized_intent;
    PaymasterQuote authorized_quote;
    CollaborativePSBTTemplate authorized_template;
    if (!LoadAttemptAuthorizationArtifacts(
            attempt, authorized_intent, authorized_quote,
            authorized_template, error) ||
        attempt.client_manifest.manifest_id.IsNull() ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, authorized_intent, authorized_quote,
            attempt.capacity_snapshot, authorized_template, error)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
    }
    if (attempt.state == AttemptState::QUOTED) {
        if (!supplied_authorization_commitment) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED");
        }
        if (*supplied_authorization_commitment !=
            attempt.client_manifest.manifest_id) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
        }
        PaymasterCapacityRequest capacity_request;
        PaymasterCapacityProof capacity_proof;
        try {
            SpanReader request_stream{::PROTOCOL_VERSION,
                                      attempt.capacity_request};
            request_stream >> capacity_request;
            SpanReader proof_stream{
                ::PROTOCOL_VERSION,
                attempt.capacity_snapshot.capacity_proof};
            proof_stream >> capacity_proof;
            if (!request_stream.empty() || !proof_stream.empty()) {
                throw std::ios_base::failure("trailing capacity artifact data");
            }
        } catch (const std::ios_base::failure&) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
        }
        node::NodeContext* node = wallet->chain().context();
        if (!node || !node->chainman ||
            !ValidateCapacityProofAgainstChainstate(
                capacity_proof, capacity_request,
                attempt.provider_identity_key, *node->chainman, now,
                error)) {
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
        }
        if (!store.AcceptClientAuthorization(
                session.request_id, attempt.attempt_id,
                *supplied_authorization_commitment, now, error) ||
            !store.GetAttempt(attempt.attempt_id, attempt)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
    }
    const int64_t operation_now = std::max(
        now, std::max(session.updated_at, attempt.updated_at));
    if (attempt.state != DigiDollar::Paymaster::AttemptState::QUOTED) {
        if (attempt.state >= DigiDollar::Paymaster::AttemptState::USER_SIGNED &&
            attempt.state <= DigiDollar::Paymaster::AttemptState::MEMPOOL &&
            !attempt.user_signed_psbt.empty()) {
            if (operation_now > attempt.retry_until) {
                throw ApplicationError(ErrorKind::WALLET, "Paymaster retry window expired");
            }
            candidate = PartiallySignedTransaction{};
        } else {
            throw ApplicationError(ErrorKind::WALLET, "Paymaster attempt is not awaiting a user signature");
        }
    } else if (operation_now > attempt.quote_expires_at) {
        throw ApplicationError(ErrorKind::WALLET, "Paymaster quote expired");
    }

    if (candidate.tx &&
        session.state != DigiDollar::Paymaster::SessionState::INPUTS_RESERVED &&
        session.state != DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK &&
        session.state != DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE &&
        session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED) {
        throw ApplicationError(ErrorKind::WALLET,
                           "Paymaster session is not awaiting a user authorization");
    }

    if (!candidate.tx) {
        if (!DecodeRawPSBT(candidate, MakeByteSpan(attempt.user_signed_psbt), error) ||
            !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                candidate, trusted,
                DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw ApplicationError(ErrorKind::WALLET, "Persisted Paymaster PSBT is corrupt");
        }
    } else {
        if (wallet->IsLocked()) {
            if ((session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                 session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                 session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) &&
                !store.TransitionSession(session.request_id,
                                         DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK,
                                         DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(ErrorKind::UNLOCK,
                               "Wallet unlock is required to sign Paymaster USER inputs");
        }
        if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
            session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK) {
            if (!store.TransitionSession(session.request_id,
                                         DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE,
                                         DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
        }
        // Re-read the immutable authorization immediately before the
        // only operation that can sign client inputs. Earlier RPC
        // validation is deliberately not treated as signing authority.
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, session, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        ProviderAttempt signing_attempt;
        PaymentIntent signing_intent;
        PaymasterQuote signing_quote;
        CollaborativePSBTTemplate signing_template;
        if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
            signing_attempt.state != AttemptState::QUOTED ||
            !supplied_authorization_commitment ||
            signing_attempt.accepted_client_manifest_id !=
                *supplied_authorization_commitment ||
            signing_attempt.accepted_client_manifest_id !=
                signing_attempt.client_manifest.manifest_id ||
            signing_attempt.client_manifest_accepted_at <= 0 ||
            std::max(operation_now, signing_attempt.updated_at) >
                signing_attempt.client_manifest.expires_at ||
            !LoadAttemptAuthorizationArtifacts(
                signing_attempt, signing_intent, signing_quote,
                signing_template, error) ||
            !ValidateClientAuthorizationManifest(
                signing_attempt.client_manifest, signing_intent,
                signing_quote, signing_attempt.capacity_snapshot,
                signing_template, error) ||
            !ValidateCollaborativePSBT(
                candidate, signing_template,
                CollaborativeSignatureStage::UNSIGNED, error)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
        }
        if (signing_intent.canonical_request_hash !=
                session.canonical_request_hash ||
            signing_intent.requested_fee_mode !=
                session.fee_mode_requested ||
            signing_intent.privacy_profile !=
                signing_attempt.privacy_profile) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH");
        }
        PaymasterCapacityRequest signing_capacity_request;
        PaymasterCapacityProof signing_capacity_proof;
        try {
            SpanReader request_stream{::PROTOCOL_VERSION,
                                      signing_attempt.capacity_request};
            request_stream >> signing_capacity_request;
            SpanReader proof_stream{
                ::PROTOCOL_VERSION,
                signing_attempt.capacity_snapshot.capacity_proof};
            proof_stream >> signing_capacity_proof;
            if (!request_stream.empty() || !proof_stream.empty()) {
                throw std::ios_base::failure(
                    "trailing capacity artifact data");
            }
        } catch (const std::ios_base::failure&) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
        }
        node::NodeContext* signing_node = wallet->chain().context();
        if (!signing_node || !signing_node->chainman ||
            !ValidateCapacityProofAgainstChainstate(
                signing_capacity_proof, signing_capacity_request,
                signing_attempt.provider_identity_key,
                *signing_node->chainman,
                std::max(operation_now, signing_attempt.updated_at), error)) {
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
        }
        if (!ValidateClientAuthorizationOwnership(
                *wallet, signing_attempt.client_manifest, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!DrainClientAttemptEquivocations(
                *context.paymaster, store, session, signing_attempt,
                error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        attempt = std::move(signing_attempt);
        {
            LOCK(wallet->cs_wallet);
            if (!CheckPaymasterClientServiceFee(*wallet, attempt.client_manifest.recipient_amount,
                    attempt.client_manifest.service_fee, error) ||
                !SignCollaborativePSBTForParty(*wallet, candidate, signing_template,
                                               DigiDollar::Paymaster::SigningParty::USER, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
        }
        const std::vector<unsigned char> signed_psbt = SerializePSBT(candidate);
        if (!store.TransitionSession(session.request_id,
                                     DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                     DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
        attempt.user_signed_psbt = signed_psbt;
        attempt.updated_at = std::max(attempt.updated_at, operation_now);
        if (!store.UpdateAttempt(session.request_id, attempt, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    DigiDollar::Paymaster::PaymentSession authoritative;
    if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
        !store.GetAttempt(attempt.attempt_id, attempt)) {
        throw ApplicationError(ErrorKind::WALLET, "Failed to reload durable Paymaster authorization");
    }

    PaymasterSubmitQueueState queue_state;
    if (!QueuePersistedPaymasterSubmit(context, *wallet, authoritative,
                                       attempt, operation_now, queue_state, error)) {
        throw ApplicationError(
            error == "PAYMASTER_DIRECT_QUEUE_FULL" ? ErrorKind::NODE_CAPACITY : ErrorKind::WALLET,
            error);
    }
    return CaptureSignedClientPSBT(authoritative, attempt, queue_state);
}

} // namespace wallet::paymaster_app
