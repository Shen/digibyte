// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymastersendcontroller.h>
#include <qt/paymasterconfirmation.h>
#include <paymaster/types.h>
#include <QDateTime>
#include <exception>
#include <chrono>
#include <algorithm>
#include <QCoreApplication>
#include <limits>
namespace PaymasterQt {
SendController::SendController(Clock clock)
    : m_clock(clock ? std::move(clock) : Clock{[] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
      }})
{
}

SendController::ReplyToken SendController::replyToken() const
{
    return {m_state.walletGeneration, m_state.sendGeneration, m_state.requestId};
}

bool SendController::accepts(const ReplyToken& token) const
{
    return token.wallet == m_state.walletGeneration &&
           token.request == m_state.sendGeneration && token.request_id == m_state.requestId;
}

SendController::Continuation SendController::savedContinuation() const
{
    return {m_state.activeSendParams, m_state.activeSendStarted, replyToken()};
}

void SendController::restoreContinuation(const Continuation& continuation)
{
    if (!accepts(continuation.token)) return;
    m_state.activeSendParams = continuation.params;
    m_state.activeSendStarted = continuation.started;
    if (!canContinue(true)) stopSendContinuation();
}

SendController::ReplyToken SendController::startPreparation(UniValue params)
{
    invalidateClosure();
    m_state.activeSendParams = std::move(params);
    m_state.sendTemplate = m_state.activeSendParams;
    m_state.activeSendStarted.invalidate();
    m_state.historyPausedAt = -1;
    m_state.historyPausedMs = 0;
    ++m_state.sendGeneration;
    return replyToken();
}

bool SendController::receivePreparation(const ReplyToken& token, bool succeeded)
{
    if (!accepts(token)) return false;
    ++m_state.sendGeneration;
    if (succeeded) m_state.activeSendStarted.start();
    return true;
}

SendController::Effect SendController::advance(const PollEvent& event) const
{
    if (!event.wallet_available || m_state.busy || m_state.requestId.isEmpty()) return Effect::NONE;
    if (m_state.preparationCancelRequested) return Effect::OBSERVE;
    if (m_state.activeRetryRequest == m_state.requestId) {
        return !m_state.activeRetryStarted.isValid() || m_state.activeRetryStarted.hasExpired(MAX_PAYMASTER_ACTIVE_SEND_MS)
            ? Effect::TIMED_OUT : Effect::RETRY;
    }
    if (m_state.recoveryActive && m_state.activeRecoveryParams.isArray()) {
        return !m_state.activeRecoveryStarted.isValid() || m_state.activeRecoveryStarted.hasExpired(MAX_PAYMASTER_ACTIVE_SEND_MS)
            ? Effect::TIMED_OUT : Effect::RECOVER;
    }
    if (!canContinue(event.wallet_available)) return Effect::OBSERVE;
    const bool unsigned_preparation = m_state.authorizationCommitment.isEmpty() &&
        (m_state.artifact.isEmpty() || m_state.artifact == QLatin1String("none"));
    if (event.history_refreshing && unsigned_preparation) return Effect::WAIT_HISTORY;
    if (activeSendExpired()) return unsigned_preparation ? Effect::CLOSE_UNSIGNED : Effect::TIMED_OUT;
    return Effect::CONTINUE;
}

int SendController::nextPoll(bool state_changed)
{
    if (m_state.requestId.isEmpty() || m_state.unsignedClosed || IsTerminalPaymasterState(m_state.sessionState)) {
        stop();
        return 0;
    }
    if (state_changed) m_state.pollIntervalMs = 1500;
    else if (m_poll_scheduled) m_state.pollIntervalMs = std::min(15000, m_state.pollIntervalMs * 2);
    m_poll_scheduled = true;
    return m_state.pollIntervalMs;
}

bool SendController::allows(const QString& action) const
{
    return m_state.allowedActionsKnown && m_state.allowedActions.contains(action);
}

bool SendController::requestCancellation()
{
    if (!canCancelPreparation()) return false;
    m_state.preparationCancelRequested = true;
    m_state.authorizationCommitment.clear();
    m_state.confirmationGuard.Reset();
    return true;
}

bool SendController::acceptReview(const QString& commitment, uint64_t wallet_generation)
{
    if (wallet_generation != m_state.walletGeneration || m_state.preparationCancelRequested ||
        !IsCanonicalNonNullPaymasterHash(commitment)) return false;
    return m_state.confirmationGuard.HasAcceptedSelection() && commitment == m_state.authorizationCommitment;
}

void SendController::clearReview()
{
    m_state.authorizationCommitment.clear();
    m_state.confirmationGuard.Reset();
}

bool SendController::observeAbsence(const RpcError& error)
{
    invalidateClosure();
    m_absence_verified = error.failed && StableCode(error) == QLatin1String("PAYMASTER_SESSION_NOT_FOUND") &&
        error.data.find_value("session_absent").isTrue() && !m_state.sessionPersisted && m_state.sessionId.isEmpty() &&
        (m_state.artifact.isEmpty() || m_state.artifact == QLatin1String("none")) &&
        m_state.transactionId.isEmpty() && m_state.recoveryTransactionId.isEmpty();
    return m_absence_verified;
}

bool SendController::clearAfterClosure()
{
    if (!m_state.requestId.isEmpty() && !m_state.unsignedClosed && !m_completion_verified && !m_absence_verified) return false;
    resetSession();
    return true;
}

SendController::Completion SendController::completion(const UniValue& result)
{
    invalidateClosure();
    const auto& embedded = result.find_value("session");
    const auto& session = embedded.isObject() ? embedded : result;
    const auto& status = session.find_value("status");
    const bool payment_success = IsValidatedPaymasterCompletion(m_state.transactionId, m_state.sessionState,
        status.isStr() ? QString::fromStdString(status.get_str()) : QString{}, m_state.resultStatus,
        session.find_value("final").isTrue(), session.find_value("payment_confirmed").isTrue());
    const bool payment = payment_success || m_state.sessionState == QLatin1String("CONFIRMED");
    const bool recovery_state = m_state.sessionState == QLatin1String("CANCELED_SAFE");
    const bool attention = m_state.sessionState == QLatin1String("FAILED") || m_state.sessionState == QLatin1String("CONFLICTED");
    if (!payment && !recovery_state && !attention) return {CompletionKind::NONE};
    stop();
    m_state.recoveryActive = false;
    if (attention) return {m_state.unsignedClosed ? CompletionKind::UNSIGNED_CLOSED : CompletionKind::ATTENTION};
    if (payment) {
        qint64 amount{0};
        if (!payment_success || !ReadInt64(session, "payment_cents", amount) || amount <= 0 ||
            amount > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
            (m_state.sessionState == QLatin1String("CONFIRMED") && m_state.confirmationState != QLatin1String("payment_confirmed")))
            return {CompletionKind::PAYMENT_UNVERIFIED};
        m_completion_verified = true;
        if (m_state.terminalNoticeShown) return {CompletionKind::ALREADY_SHOWN};
        return {CompletionKind::PAYMENT, amount};
    }
    const auto& recovery = session.find_value("payment_view").find_value("recovery");
    qint64 depth{0};
    if (!recovery.isObject() || !recovery.find_value("details_available").isTrue() ||
        !recovery.find_value("observation_available").isTrue() || !ReadInt64(recovery, "confirmations", depth) || depth <= 0)
        return {CompletionKind::RECOVERY_UNVERIFIED};
    const auto& encoded = recovery.find_value("txid");
    const QString txid = encoded.isStr() ? QString::fromStdString(encoded.get_str()) : QString{};
    if (m_state.confirmationState != QLatin1String("recovery_confirmed") || !IsCanonicalNonNullPaymasterHash(m_state.recoveryTransactionId) ||
        !IsCanonicalNonNullPaymasterHash(txid) || txid != m_state.recoveryTransactionId) return {CompletionKind::RECOVERY_MISMATCH};
    m_completion_verified = true;
    if (m_state.terminalNoticeShown) return {CompletionKind::ALREADY_SHOWN};
    return {CompletionKind::RECOVERY};
}

SendController::CompletionKind SendController::directCompletion(const UniValue& result)
{
    invalidateClosure();
    const auto& status = result.find_value("status");
    if (status.isNull() || !result.find_value("session_state").isNull() || !result.find_value("session").isNull()) return CompletionKind::NONE;
    const auto& txid = result.find_value("txid");
    if (!status.isStr() || !IsValidatedPaymasterCompletion(
            txid.isStr() ? QString::fromStdString(txid.get_str()) : QString{}, QString{},
            QString::fromStdString(status.get_str()), QString{})) return CompletionKind::PAYMENT_UNVERIFIED;
    m_completion_verified = true;
    return CompletionKind::PAYMENT;
}

void SendController::clearRecoveryReview()
{
    m_state.recoveryAuthorizationCommitment.clear();
    m_state.recoveryConfirmationGuard.Reset();
}

void SendController::fallbackCompleted()
{
    clearReview();
    clearRecoveryReview();
    m_state.recoveryMaximumServiceFeeCents = 0;
    m_state.recoveryActive = false;
}

bool SendController::restoreRequest(const QString& request_id)
{
    if (!m_state.requestId.isEmpty() || !DigiDollar::Paymaster::IsCanonicalRequestId(request_id.toStdString())) return false;
    clearAfterClosure();
    m_state.requestId = request_id;
    m_state.sessionPersisted = true;
    return true;
}

bool SendController::beginTransfer(const QString& request_id, const QString& privacy)
{
    if (!clearAfterClosure()) return false;
    m_state.requestId = request_id;
    m_state.sessionPrivacy = privacy;
    return true;
}

void SendController::bindInput(const QString& address, CAmount amount)
{
    m_state.address = address;
    m_state.amountCents = amount;
    m_state.amount = amount / 100.0;
}

bool SendController::beginRetry()
{
    if (!allows(QStringLiteral("retry_same")) ||
        (m_state.artifact != QLatin1String("user_psbt") && m_state.artifact != QLatin1String("final_transaction"))) return false;
    invalidateClosure();
    m_state.activeRetryRequest = m_state.requestId;
    m_state.activeRetryStarted.start();
    return true;
}

void SendController::stopRetry()
{
    m_state.activeRetryRequest.clear();
    m_state.activeRetryStarted.invalidate();
}

bool SendController::beginRecovery(qint64 maximum_fee)
{
    if (!allows(QStringLiteral("cancel_to_self")) || m_state.sessionPrivacy.isEmpty() || maximum_fee < 0) return false;
    invalidateClosure();
    clearRecoveryReview();
    m_state.recoveryMaximumServiceFeeCents = maximum_fee;
    m_state.recoveryActive = true;
    return true;
}

void SendController::startRecoveryCall(UniValue params)
{
    invalidateClosure();
    m_state.activeRecoveryParams = std::move(params);
    if (!m_state.activeRecoveryStarted.isValid()) m_state.activeRecoveryStarted.start();
}

void SendController::stopSendContinuation()
{
    m_state.activeSendParams = UniValue{};
    m_state.activeSendStarted.invalidate();
}

void SendController::clearContinuations()
{
    stopRetry();
    m_state.activeRecoveryParams = UniValue{};
    m_state.activeRecoveryStarted.invalidate();
    stopSendContinuation();
}

SendController::ReplyToken SendController::beginContinuation()
{
    invalidateClosure();
    ++m_state.sendGeneration;
    return replyToken();
}

bool SendController::receiveContinuation(const ReplyToken& token)
{
    if (!accepts(token)) return false;
    ++m_state.sendGeneration;
    return true;
}

bool SendController::confirmSelection(const PaymasterConfirmationSelection& selection, uint64_t wallet_generation, qint64 now)
{
    if (wallet_generation != m_state.walletGeneration || m_state.preparationCancelRequested ||
        selection.recipient != m_state.address || selection.expires_at <= now || !m_state.confirmationGuard.Accept(selection)) return false;
    m_state.authorizationCommitment = selection.authorization_commitment;
    return true;
}

bool SendController::confirmRecovery(const PaymasterRecoveryConfirmationSelection& selection, uint64_t wallet_generation, qint64 now)
{
    if (wallet_generation != m_state.walletGeneration || selection.expires_at <= now ||
        selection.privacy_profile != m_state.sessionPrivacy || !m_state.recoveryConfirmationGuard.Accept(selection)) return false;
    m_state.recoveryAuthorizationCommitment = selection.authorization_commitment;
    return true;
}

void SendController::setSnapshotForTesting(const PaymasterSessionSnapshot& snapshot, bool persisted)
{
    m_state.requestId = snapshot.request_id;
    m_state.sessionId = snapshot.session_id;
    m_state.sessionState = snapshot.state;
    m_state.unsignedClosed = snapshot.unsigned_closed;
    m_state.offerReadyForReview = snapshot.offer_ready_for_review;
    m_state.artifact = snapshot.artifact;
    m_state.attemptState = snapshot.attempt_state;
    m_state.pendingPhase = snapshot.pending_phase;
    m_state.allowedActions = snapshot.allowed_actions;
    m_state.allowedActionsKnown = snapshot.allowed_actions_known;
    m_state.recoveryExpiresAt = snapshot.recovery_expires_at;
    m_state.sessionCreatedAt = snapshot.created_at;
    m_state.sessionPersisted = persisted;
    bindInput(snapshot.recipient, snapshot.requested_amount_cents);
}

SendController::PreparedDecision SendController::preparedReply(const UniValue& result)
{
    const auto& encoded = result.find_value("authorization_commitment");
    const QString commitment = encoded.isStr() ? QString::fromStdString(encoded.get_str()) : QString{};
    const auto block = [&](PreparedEffect effect, bool invalidate_review) {
        stop();
        if (invalidate_review) {
            m_state.authorizationCommitment.clear();
            m_state.confirmationGuard.Reset();
        }
        return PreparedDecision{effect, commitment};
    };
    if (!commitment.isEmpty() && !IsCanonicalNonNullPaymasterHash(commitment))
        return block(PreparedEffect::MALFORMED_COMMITMENT, true);
    if (!m_state.authorizationCommitment.isEmpty() && commitment != m_state.authorizationCommitment)
        return block(PreparedEffect::CHANGED_COMMITMENT, true);
    const auto& authorization = result.find_value("authorization_required");
    if (!authorization.isNull() && !authorization.isBool())
        return block(PreparedEffect::INVALID_AUTHORIZATION_FLAG, false);
    if (authorization.isTrue()) {
        stop();
        return {commitment.isEmpty() ? PreparedEffect::MISSING_COMMITMENT : PreparedEffect::REVIEW, commitment};
    }
    if (m_state.sessionState == QLatin1String("AWAITING_WALLET_UNLOCK")) {
        stop();
        return {!m_state.allowedActionsKnown || allows(QStringLiteral("resume"))
            ? PreparedEffect::UNLOCK : PreparedEffect::WAIT, commitment};
    }
    if (m_state.activeSendParams.isArray() && !m_state.authorizationCommitment.isEmpty() &&
        m_state.artifact.isEmpty() &&
        (m_state.sessionState == QLatin1String("AUTHORIZED") || m_state.sessionState == QLatin1String("PENDING_PROVIDER")))
        return {PreparedEffect::RECONCILE_ARTIFACT, commitment};
    if (!canContinue(true)) {
        m_state.activeSendParams = UniValue{};
        m_state.activeSendStarted.invalidate();
    }
    return {PreparedEffect::POLL, commitment};
}

SendController::RecoveryDecision SendController::recoveryReply(
    const UniValue& result, const PaymasterRecoveryConfirmationSelection& selection, qint64 now)
{
    const auto& recovery = result.find_value("recovery");
    if (!recovery.isObject()) return {RecoveryEffect::MISSING_MANIFEST};
    if (!m_state.recoveryAuthorizationCommitment.isEmpty() &&
        selection.authorization_commitment != m_state.recoveryAuthorizationCommitment)
        return {RecoveryEffect::CHANGED_COMMITMENT};
    const auto& expired = recovery.find_value("expired");
    if (!expired.isBool() || selection.expires_at <= 0 || expired.get_bool() != (selection.expires_at <= now))
        return {RecoveryEffect::INVALID_EXPIRY};
    if (expired.get_bool()) return {RecoveryEffect::EXPIRED};
    const auto& accepted = recovery.find_value("authorization_accepted");
    if (!accepted.isNull() && !accepted.isBool()) return {RecoveryEffect::INVALID_ACCEPTANCE};
    if (m_state.recoveryAuthorizationCommitment.isEmpty() && !selection.authorization_commitment.isEmpty())
        return {RecoveryEffect::REVIEW};
    if (!m_state.recoveryAuthorizationCommitment.isEmpty() && !accepted.isTrue()) return {RecoveryEffect::REJECTED};
    const auto& phase_value = recovery.find_value("phase");
    const QString phase = phase_value.isStr() ? QString::fromStdString(phase_value.get_str()) : QString{};
    if (phase != QLatin1String("capacity_pending") && phase != QLatin1String("request_ready") &&
        phase != QLatin1String("response_validated") && phase != QLatin1String("user_signed") && phase != QLatin1String("final_committed"))
        return {RecoveryEffect::UNKNOWN_PHASE};
    const auto& broadcast = result.find_value("broadcast");
    const auto& broadcast_error = result.find_value("broadcast_error");
    if ((!broadcast.isNull() && !broadcast.isBool()) ||
        (!broadcast_error.isNull() && (!broadcast_error.isStr() || broadcast_error.get_str().size() > 1024)))
        return {RecoveryEffect::INVALID_BROADCAST};
    const bool preparing = m_state.recoveryAuthorizationCommitment.isEmpty() &&
        (phase == QLatin1String("capacity_pending") || phase == QLatin1String("request_ready"));
    const bool finishing_signed = !m_state.recoveryAuthorizationCommitment.isEmpty() && accepted.isTrue() && phase == QLatin1String("user_signed");
    if (!m_state.recoveryActive || !allows(QStringLiteral("cancel_to_self")) || (!preparing && !finishing_signed)) {
        m_state.activeRecoveryParams = UniValue{};
        m_state.activeRecoveryStarted.invalidate();
        m_state.recoveryActive = false;
    }
    return {RecoveryEffect::POLL, phase, broadcast_error.isStr() ? QString::fromStdString(broadcast_error.get_str()) : QString{}, broadcast.isTrue()};
}

bool ReadInt64(const UniValue& object, const char* key, qint64& value)
{
    const UniValue& encoded = object.find_value(key);
    if (!encoded.isNum()) return false;
    try {
        value = encoded.getInt<qint64>();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}


bool IsKnownSessionState(const QString& state)
{
    static const QStringList STATES{
        QStringLiteral("CREATED"),
        QStringLiteral("INPUTS_RESERVED"),
        QStringLiteral("AWAITING_WALLET_UNLOCK"),
        QStringLiteral("AWAITING_USER_SIGNATURE"),
        QStringLiteral("AUTHORIZED"),
        QStringLiteral("DGB_COMMITTING"),
        QStringLiteral("STEMPOOL"),
        QStringLiteral("MEMPOOL"),
        QStringLiteral("CONFIRMED"),
        QStringLiteral("PENDING_PROVIDER"),
        QStringLiteral("FAILED"),
        QStringLiteral("CANCELED_SAFE"),
        QStringLiteral("CONFLICTED"),
    };
    return STATES.contains(state);
}

bool IsKnownAttemptState(const QString& state)
{
    static const QStringList STATES{
        QStringLiteral("CANDIDATE"),
        QStringLiteral("QUOTED"),
        QStringLiteral("USER_SIGNED"),
        QStringLiteral("USER_PSBT_ACCEPTED"),
        QStringLiteral("PROVIDER_SIGNED"),
        QStringLiteral("FINAL_COMMITTED"),
        QStringLiteral("BROADCAST"),
        QStringLiteral("STEMPOOL"),
        QStringLiteral("MEMPOOL"),
        QStringLiteral("REJECTED"),
        QStringLiteral("QUOTE_EXPIRED"),
        QStringLiteral("AMBIGUOUS"),
        QStringLiteral("CONFLICTED"),
    };
    return STATES.contains(state);
}

bool IsKnownPaymasterArtifact(const QString& artifact)
{
    return artifact == QStringLiteral("none") ||
        artifact == QStringLiteral("user_psbt") ||
        artifact == QStringLiteral("final_transaction") ||
        artifact == QStringLiteral("alternative_recovery");
}

bool IsKnownPaymasterResultStatus(const QString& status)
{
    return status == QStringLiteral("no_final_commit") ||
        status == QStringLiteral("user_psbt_accepted") ||
        status == QStringLiteral("final_committed") ||
        status == QStringLiteral("broadcast_attempted") ||
        status == QStringLiteral("slot_unavailable") ||
        status == QStringLiteral("rejected");
}

bool IsKnownPaymasterAction(const QString& action)
{
    return action == QStringLiteral("refresh") ||
        action == QStringLiteral("resume") ||
        action == QStringLiteral("retry_same") ||
        action == QStringLiteral("fallback") ||
        action == QStringLiteral("abandon_unsigned") ||
        action == QStringLiteral("recover") ||
        action == QStringLiteral("cancel_to_self");
}

bool IsTerminalPaymasterState(const QString& state)
{
    return state == QStringLiteral("CONFIRMED") ||
        state == QStringLiteral("CANCELED_SAFE");
}

bool SendController::decodeSnapshot(const UniValue& result,
                                    PaymasterSessionSnapshot& snapshot,
                                    QString& error)
{
    if (!result.isObject()) {
        error = QStringLiteral("response is not an object");
        return false;
    }
    const UniValue& embedded_session = result.find_value("session");
    const bool wrapped = embedded_session.isObject();
    snapshot.authoritative = wrapped;
    const UniValue& session = wrapped ? embedded_session : result;
    const auto read_string = [](const UniValue& object, const char* key) {
        const UniValue& value = object.find_value(key);
        return value.isStr() ? QString::fromStdString(value.get_str())
                             : QString{};
    };

    snapshot.request_id = read_string(session, "request_id");
    snapshot.session_id = read_string(session, "session_id");
    snapshot.state = read_string(session, "session_state");
    snapshot.pending_phase = read_string(session, "pending_phase");
    snapshot.broadcast_state = read_string(session, "broadcast_state");
    snapshot.confirmation_state = read_string(session, "confirmation_state");
    snapshot.recipient = read_string(session, "to_address");
    if (!wrapped && snapshot.recipient.isEmpty()) {
        snapshot.recipient = read_string(result, "to_address");
    }
    snapshot.privacy_profile = read_string(session, "privacy_profile");
    const UniValue& requested_mode = session.find_value("requested_fee_mode");
    if (!requested_mode.isNull()) {
        if (!requested_mode.isStr() || (requested_mode.get_str() != "paymaster" && requested_mode.get_str() != "auto")) {
            error = QStringLiteral("invalid persisted Paymaster fee mode");
            return false;
        }
        snapshot.restored_options.pushKV("fee_mode", requested_mode);
    }
    for (const char* key : {"subtract_paymaster_fee_from_amount", "send_all_spendable_dd"}) {
        const auto& value = session.find_value(key);
        if (!value.isNull()) {
            if (!value.isBool()) {
                error = QStringLiteral("invalid persisted amount semantics");
                return false;
            }
            snapshot.restored_options.pushKV(key, value);
        }
    }
    if (session.exists("created_at") &&
        (!ReadInt64(session, "created_at", snapshot.created_at) || snapshot.created_at <= 0)) {
        error = QStringLiteral("invalid session creation time");
        return false;
    }
    snapshot.transaction_id = read_string(session, "txid");
    snapshot.recovery_transaction_id = read_string(session, "recovery_txid");
    const UniValue& reported_final = session.find_value("final");
    if ((wrapped && !reported_final.isBool()) ||
        (!reported_final.isNull() && !reported_final.isBool())) {
        error = QStringLiteral("session final marker is not a boolean");
        return false;
    }
    snapshot.reported_final = reported_final.isBool() &&
        reported_final.get_bool();

    if (!IsKnownSessionState(snapshot.state) ||
        (wrapped && (snapshot.request_id.isEmpty() ||
                     snapshot.session_id.isEmpty())) ||
        (!snapshot.session_id.isEmpty() &&
         !IsCanonicalNonNullPaymasterHash(snapshot.session_id)) ||
        (!snapshot.request_id.isEmpty() &&
         !DigiDollar::Paymaster::IsCanonicalRequestId(
             snapshot.request_id.toStdString())) ||
        snapshot.recipient.size() > 160) {
        error = QStringLiteral("invalid session identity, state or recipient");
        return false;
    }
    if ((!snapshot.transaction_id.isEmpty() &&
         !IsCanonicalNonNullPaymasterHash(snapshot.transaction_id)) ||
        (!snapshot.recovery_transaction_id.isEmpty() &&
         !IsCanonicalNonNullPaymasterHash(
             snapshot.recovery_transaction_id))) {
        error = QStringLiteral("invalid payment or recovery transaction id");
        return false;
    }

    if (!snapshot.pending_phase.isEmpty() &&
        snapshot.pending_phase != QStringLiteral("NONE") &&
        snapshot.pending_phase != QStringLiteral("USER_SIGNATURE_SENT") &&
        snapshot.pending_phase != QStringLiteral("PROVIDER_SIGNED_KNOWN") &&
        snapshot.pending_phase != QStringLiteral("PENDING_NETWORK") &&
        snapshot.pending_phase != QStringLiteral("CANCEL_MEMPOOL")) {
        error = QStringLiteral("unknown pending phase");
        return false;
    }
    if (!snapshot.confirmation_state.isEmpty() &&
        snapshot.confirmation_state != QStringLiteral("unconfirmed") &&
        snapshot.confirmation_state != QStringLiteral("payment_confirmed") &&
        snapshot.confirmation_state != QStringLiteral("recovery_confirmed") &&
        snapshot.confirmation_state != QStringLiteral("conflicted")) {
        error = QStringLiteral("unknown confirmation state");
        return false;
    }
    const UniValue& payment_confirmed = session.find_value("payment_confirmed");
    const QString payment_status = read_string(session, "status");
    if ((!payment_confirmed.isNull() && !payment_confirmed.isBool()) ||
        (snapshot.confirmation_state == QStringLiteral("payment_confirmed") &&
         (!payment_confirmed.isTrue() || payment_status != QStringLiteral("success"))) ||
        (payment_confirmed.isTrue() &&
         (snapshot.confirmation_state != QStringLiteral("payment_confirmed") ||
          payment_status != QStringLiteral("success")))) {
        error = QStringLiteral("inconsistent recipient payment confirmation");
        return false;
    }
    if (!snapshot.broadcast_state.isEmpty() &&
        snapshot.broadcast_state != QStringLiteral("not_attempted") &&
        snapshot.broadcast_state != QStringLiteral("unknown") &&
        snapshot.broadcast_state != QStringLiteral("accepted_mempool") &&
        snapshot.broadcast_state != QStringLiteral("accepted_stempool") &&
        snapshot.broadcast_state != QStringLiteral("confirmed")) {
        error = QStringLiteral("unknown broadcast state");
        return false;
    }

    const UniValue& attempt = result.find_value("attempt");
    if ((wrapped && !result.exists("attempt")) ||
        (!attempt.isNull() && !attempt.isObject())) {
        error = QStringLiteral("attempt is not an object");
        return false;
    }
    const UniValue& recovery = result.find_value("recovery");
    if (wrapped && (!result.exists("recovery") ||
                    (!recovery.isNull() && !recovery.isObject()))) {
        error = QStringLiteral("recovery is neither an object nor null");
        return false;
    }
    const UniValue& requires_attention =
        result.find_value("requires_attention");
    if (wrapped && !requires_attention.isBool()) {
        error = QStringLiteral("refresh response omitted requires_attention");
        return false;
    }
    snapshot.attempt_state = attempt.isObject()
        ? read_string(attempt, "attempt_state")
        : read_string(result, "attempt_state");
    if (snapshot.privacy_profile.isEmpty() && attempt.isObject()) {
        snapshot.privacy_profile = read_string(attempt, "privacy_profile");
    }
    if (snapshot.privacy_profile.isEmpty()) {
        snapshot.privacy_profile = read_string(result, "privacy_profile");
    }
    if (!snapshot.privacy_profile.isEmpty() &&
        snapshot.privacy_profile != QStringLiteral("standard") &&
        snapshot.privacy_profile != QStringLiteral("high")) {
        error = QStringLiteral("unknown privacy profile");
        return false;
    }
    if (!snapshot.attempt_state.isEmpty() &&
        !IsKnownAttemptState(snapshot.attempt_state)) {
        error = QStringLiteral("unknown attempt state");
        return false;
    }
    snapshot.provider_id = read_string(session, "provider_id");
    if (!wrapped && snapshot.provider_id.isEmpty()) {
        snapshot.provider_id = read_string(result, "provider_id");
    }
    if (snapshot.provider_id.isEmpty() && attempt.isObject()) {
        snapshot.provider_id = read_string(attempt, "provider_id");
    }
    snapshot.policy_hash = read_string(session, "policy_hash");
    if (!wrapped && snapshot.policy_hash.isEmpty()) {
        snapshot.policy_hash = read_string(result, "policy_hash");
    }
    if ((!snapshot.provider_id.isEmpty() &&
         !IsCanonicalNonNullPaymasterHash(snapshot.provider_id)) ||
        (!snapshot.policy_hash.isEmpty() &&
         !IsCanonicalNonNullPaymasterHash(snapshot.policy_hash))) {
        error = QStringLiteral("invalid provider or policy binding");
        return false;
    }

    const UniValue& artifact = result.find_value("artifact");
    if (artifact.isStr()) {
        snapshot.artifact = QString::fromStdString(artifact.get_str());
        if (!IsKnownPaymasterArtifact(snapshot.artifact)) {
            error = QStringLiteral("unknown persisted artifact");
            return false;
        }
    } else if (wrapped) {
        error = QStringLiteral("refresh response omitted the artifact");
        return false;
    }

    const UniValue& actions = result.find_value("allowed_actions");
    if (actions.isArray()) {
        if (actions.size() > 8) {
            error = QStringLiteral("too many allowed actions");
            return false;
        }
        for (const UniValue& action_value : actions.getValues()) {
            if (!action_value.isStr()) {
                error = QStringLiteral("allowed action is not a string");
                return false;
            }
            const QString action = QString::fromStdString(action_value.get_str());
            if (!IsKnownPaymasterAction(action) ||
                snapshot.allowed_actions.contains(action)) {
                error = QStringLiteral("unknown or duplicate allowed action");
                return false;
            }
            snapshot.allowed_actions.push_back(action);
        }
        snapshot.allowed_actions_known = true;
    } else if (wrapped) {
        error = QStringLiteral("refresh response omitted allowed actions");
        return false;
    }
    if (wrapped) {
        const bool recovery_artifact =
            snapshot.artifact == QStringLiteral("alternative_recovery");
        if (recovery_artifact != recovery.isObject() ||
            ((snapshot.artifact == QStringLiteral("user_psbt") ||
              snapshot.artifact == QStringLiteral("final_transaction")) &&
             !attempt.isObject()) ||
            (snapshot.allowed_actions.contains(
                 QStringLiteral("retry_same")) &&
             snapshot.artifact != QStringLiteral("user_psbt") &&
             snapshot.artifact != QStringLiteral("final_transaction")) ||
            ((snapshot.allowed_actions.contains(QStringLiteral("resume")) ||
              snapshot.allowed_actions.contains(QStringLiteral("fallback")) ||
              snapshot.allowed_actions.contains(
                  QStringLiteral("abandon_unsigned"))) &&
             snapshot.artifact != QStringLiteral("none"))) {
            error = QStringLiteral(
                "artifact, attempt, recovery and allowed actions disagree");
            return false;
        }

        if (recovery.isObject()) {
            const UniValue& expired = recovery.find_value("expired");
            qint64 recovery_expires_at{-1};
            if (!expired.isBool() ||
                !ReadInt64(recovery, "expires_at", recovery_expires_at) ||
                recovery_expires_at <= 0 ||
                !QDateTime::fromSecsSinceEpoch(recovery_expires_at).isValid()) {
                error = QStringLiteral("invalid recovery expiry fields");
                return false;
            }
            const bool elapsed = recovery_expires_at <=
                QDateTime::currentSecsSinceEpoch();
            if (expired.get_bool() != elapsed) {
                error = QStringLiteral("inconsistent recovery expiry fields");
                return false;
            }
            if (elapsed) {
                // Keep an expired recovery snapshot visible, but never expose
                // an action that could advance it to wallet signing.
                snapshot.allowed_actions.removeAll(QStringLiteral("recover"));
                snapshot.allowed_actions.removeAll(
                    QStringLiteral("cancel_to_self"));
            }
            snapshot.recovery_expires_at = recovery_expires_at;
        }

        // Only Core can prove that a failed unsigned session has no remaining
        // authorization or live reservation. FAILED alone is insufficient.
        const bool unsigned_attempt = attempt.isNull() ||
                                      snapshot.attempt_state == QStringLiteral("CANDIDATE") ||
                                      snapshot.attempt_state == QStringLiteral("QUOTED") ||
                                      snapshot.attempt_state == QStringLiteral("QUOTE_EXPIRED") ||
                                      snapshot.attempt_state == QStringLiteral("REJECTED");
        snapshot.unsigned_closed = snapshot.state == QStringLiteral("FAILED") &&
                                   payment_status == QStringLiteral("failed") && unsigned_attempt &&
                                   snapshot.reported_final && snapshot.artifact == QStringLiteral("none") &&
                                   !requires_attention.get_bool() && (payment_confirmed.isBool() && !payment_confirmed.get_bool()) &&
                                   snapshot.allowed_actions.size() == 1 &&
                                   snapshot.allowed_actions.front() == QStringLiteral("refresh") &&
                                   (snapshot.pending_phase.isEmpty() || snapshot.pending_phase == QStringLiteral("NONE")) &&
                                   snapshot.broadcast_state == QStringLiteral("not_attempted") &&
                                   snapshot.confirmation_state == QStringLiteral("unconfirmed") &&
                                   snapshot.transaction_id.isEmpty() && snapshot.recovery_transaction_id.isEmpty() &&
                                   result.find_value("result_status").isNull() &&
                                   result.find_value("result_sequence").isNull();

        // A crash may leave a CREATED record before an attempt and recipient
        // are persisted. Its unsigned cancellation is a valid recipientless
        // FAILED snapshot as well. Neither exception can authorize a payment.
        qint64 provider_attempts{-1};
        const UniValue& inputs = session.find_value("reserved_user_inputs");
        const bool recipientless_closed = snapshot.unsigned_closed &&
                                          attempt.isNull() &&
                                          ReadInt64(session, "provider_attempts", provider_attempts) && provider_attempts == 0 &&
                                          inputs.isArray() && inputs.empty();
        const bool recipientless_created = snapshot.recipient.isEmpty() &&
                                           snapshot.state == QStringLiteral("CREATED") &&
                                           snapshot.artifact == QStringLiteral("none") &&
                                           !attempt.isObject();
        if (snapshot.recipient.isEmpty() && !recipientless_created && !recipientless_closed) {
            error = QStringLiteral(
                "session recipient is unavailable for an actionable artifact");
            return false;
        }
        if (recipientless_created) {
            snapshot.allowed_actions.removeAll(QStringLiteral("resume"));
            snapshot.allowed_actions.removeAll(QStringLiteral("fallback"));
            snapshot.allowed_actions.removeAll(QStringLiteral("retry_same"));
            snapshot.allowed_actions.removeAll(QStringLiteral("recover"));
            snapshot.allowed_actions.removeAll(QStringLiteral("cancel_to_self"));
        }
    }

    const UniValue& result_status = result.find_value("result_status");
    const UniValue& result_sequence = result.find_value("result_sequence");
    if (wrapped && (!result.exists("result_status") ||
                    !result.exists("result_sequence"))) {
        error = QStringLiteral("refresh response omitted provider result state");
        return false;
    }
    if (result_status.isNull() != result_sequence.isNull()) {
        error = QStringLiteral("provider result status and sequence disagree");
        return false;
    }
    if (!result_status.isNull()) {
        if (!result_status.isStr()) {
            error = QStringLiteral("invalid provider result status");
            return false;
        }
        snapshot.result_status =
            QString::fromStdString(result_status.get_str());
        if (!IsKnownPaymasterResultStatus(snapshot.result_status)) {
            error = QStringLiteral("unknown provider result status");
            return false;
        }
    }
    if (!result_sequence.isNull() &&
        (!ReadInt64(result, "result_sequence",
                    snapshot.result_sequence) ||
         snapshot.result_sequence < 0)) {
        error = QStringLiteral("invalid provider result sequence");
        return false;
    }

    const UniValue& presentation = wrapped ? session : result;
    const UniValue& payment = presentation.find_value("payment_cents");
    const UniValue& fee = presentation.find_value("service_fee_cents");
    const UniValue& total = presentation.find_value("user_total_cents");
    const bool any_cost = !payment.isNull() || !fee.isNull() || !total.isNull();
    if (any_cost) {
        if (!ReadInt64(presentation, "payment_cents", snapshot.payment_cents) ||
            !ReadInt64(presentation, "service_fee_cents", snapshot.service_fee_cents) ||
            !ReadInt64(presentation, "user_total_cents", snapshot.total_cents) ||
            snapshot.payment_cents <= 0 || snapshot.service_fee_cents < 0 ||
            snapshot.payment_cents >
                DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
            snapshot.service_fee_cents >
                DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
            snapshot.payment_cents >
                std::numeric_limits<qint64>::max() -
                    snapshot.service_fee_cents ||
            snapshot.total_cents !=
                snapshot.payment_cents + snapshot.service_fee_cents) {
            error = QStringLiteral("invalid payment totals");
            return false;
        }
        snapshot.has_costs = true;
    }

    const UniValue& requested = session.find_value("requested_amount_cents");
    if (!requested.isNull() &&
        (!ReadInt64(session, "requested_amount_cents",
                    snapshot.requested_amount_cents) ||
         snapshot.requested_amount_cents <= 0 ||
         snapshot.requested_amount_cents >
             DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS)) {
        error = QStringLiteral("invalid requested amount");
        return false;
    }

    const UniValue& expiry = presentation.find_value("expires_at");
    if (!expiry.isNull()) {
        if (!ReadInt64(presentation, "expires_at", snapshot.expires_at) ||
            snapshot.expires_at <= 0) {
            error = QStringLiteral("invalid expiry");
            return false;
        }
        snapshot.has_expiry = true;
    }
    // AWAITING_USER_SIGNATURE also covers a queued quote request. Only a
    // quoted attempt or exact commitment with current costs is reviewable.
    snapshot.offer_ready_for_review = snapshot.state == QStringLiteral("AWAITING_USER_SIGNATURE") &&
                                      snapshot.has_costs && snapshot.has_expiry && snapshot.expires_at > QDateTime::currentSecsSinceEpoch() &&
                                      (snapshot.attempt_state == QStringLiteral("QUOTED") ||
                                       (result.find_value("authorization_required").isTrue() &&
                                        IsCanonicalNonNullPaymasterHash(read_string(result, "authorization_commitment"))));
    return true;
}

SendController::FailureDecision SendController::failure(const RpcError& error, bool persisted, bool known_unsigned)
{
    FailureDecision decision;
    if (!error.failed) return decision;
    decision.invalidate_review = true;
    decision.discard_uncreated_request = !persisted && error.data.find_value("session_absent").isTrue() &&
        PaymasterQt::Disposition(error) == QStringLiteral("retry_before_execution");
    decision.allow_unsigned_cancel = known_unsigned && PaymasterQt::Disposition(error) == QStringLiteral("user_decision");
    return decision;
}

bool SendController::activeSendExpired() const
{
    if (!m_state.activeSendStarted.isValid()) return false;
    const qint64 elapsed = m_state.activeSendStarted.elapsed();
    const qint64 current_pause = m_state.historyPausedAt < 0 ? 0 : elapsed - m_state.historyPausedAt;
    return elapsed - m_state.historyPausedMs - current_pause >= MAX_PAYMASTER_ACTIVE_SEND_MS;
}

bool SendController::canContinue(bool wallet_available) const
{
    if (!wallet_available || !m_state.activeSendParams.isArray() ||
        m_state.activeSendParams.size() != 7 || !m_state.sessionPersisted ||
        !m_state.activeSendStarted.isValid() || m_state.recoveryActive) return false;
    const UniValue& options = m_state.activeSendParams[6];
    const UniValue& request_id = options.find_value("request_id");
    if (!request_id.isStr() || request_id.get_str() != m_state.requestId.toStdString()) return false;
    const UniValue& commitment = options.find_value("authorization_commitment");
    if (commitment.isNull() && options.find_value("prepare_only").isTrue()) {
        return m_state.authorizationCommitment.isEmpty() &&
               (m_state.artifact.isEmpty() || m_state.artifact == QStringLiteral("none")) &&
               (m_state.sessionState == QStringLiteral("CREATED") ||
                m_state.sessionState == QStringLiteral("INPUTS_RESERVED") ||
                m_state.sessionState == QStringLiteral("AWAITING_USER_SIGNATURE"));
    }
    // After explicit exact-offer approval, only finish the already signed
    // artifact. Never infer permission to sign from a restored status snapshot.
    return commitment.isStr() && !m_state.authorizationCommitment.isEmpty() &&
           commitment.get_str() == m_state.authorizationCommitment.toStdString() &&
           m_state.artifact == QStringLiteral("user_psbt") &&
           (m_state.sessionState == QStringLiteral("AUTHORIZED") ||
            m_state.sessionState == QStringLiteral("PENDING_PROVIDER"));
}

bool SendController::applySnapshot(const PaymasterSessionSnapshot& snapshot, QString* decode_error)
{
    if (!m_state.requestId.isEmpty() && !snapshot.request_id.isEmpty() &&
        snapshot.request_id != m_state.requestId) {
        if (decode_error) {
            *decode_error = QCoreApplication::translate("DigiDollarSendWidget", "response belongs to a different request");
        }
        return false;
    }
    if (!m_state.sessionId.isEmpty() && !snapshot.session_id.isEmpty() &&
        snapshot.session_id != m_state.sessionId) {
        if (decode_error) {
            *decode_error = QCoreApplication::translate("DigiDollarSendWidget", "response belongs to a different session");
        }
        return false;
    }
    if (!m_state.address.isEmpty() && !snapshot.recipient.isEmpty() &&
        snapshot.recipient != m_state.address) {
        if (decode_error) {
            *decode_error = QCoreApplication::translate("DigiDollarSendWidget", "response recipient does not match the active transfer");
        }
        return false;
    }

    // A new authoritative observation supersedes earlier completion or absence
    // evidence, including a reorg and a late persisted mutation result.
    invalidateClosure();
    const QString previous_state = m_state.sessionState;
    if (previous_state != snapshot.state) m_state.terminalNoticeShown = false;
    if (!snapshot.request_id.isEmpty()) m_state.requestId = snapshot.request_id;
    if (!snapshot.session_id.isEmpty()) {
        m_state.sessionId = snapshot.session_id;
        m_state.sessionPersisted = true;
    }
    if (!snapshot.restored_options.empty()) m_state.restoredOptions = snapshot.restored_options;
    m_state.sessionState = snapshot.state;
    m_state.unsignedClosed = snapshot.unsigned_closed;
    m_state.offerReadyForReview = snapshot.offer_ready_for_review;
    m_state.pendingPhase = snapshot.pending_phase;
    m_state.broadcastState = snapshot.broadcast_state;
    m_state.confirmationState = snapshot.confirmation_state;
    m_state.transactionId = snapshot.transaction_id;
    m_state.recoveryTransactionId = snapshot.recovery_transaction_id;
    m_state.resultStatus = snapshot.result_status;
    m_state.resultSequence = snapshot.result_sequence;
    m_state.recoveryExpiresAt = snapshot.recovery_expires_at;
    m_state.sessionCreatedAt = snapshot.created_at;
    // Replace optional state atomically. Missing fields explicitly clear stale
    // values; a new session can never inherit an old attempt or artifact.
    m_state.artifact = snapshot.artifact;
    m_state.attemptState = snapshot.attempt_state;
    m_state.allowedActions = snapshot.allowed_actions;
    m_state.allowedActionsKnown = snapshot.allowed_actions_known;
    if (snapshot.state != QStringLiteral("MEMPOOL") &&
        snapshot.state != QStringLiteral("CONFIRMED") &&
        snapshot.state != QStringLiteral("CANCELED_SAFE") &&
        snapshot.state != QStringLiteral("FAILED") &&
        snapshot.state != QStringLiteral("CONFLICTED")) {
        m_state.terminalNoticeShown = false;
    }
    if (snapshot.authoritative || !snapshot.recipient.isEmpty()) {
        m_state.address = snapshot.recipient;
    }
    if (snapshot.authoritative || !snapshot.privacy_profile.isEmpty()) {
        m_state.sessionPrivacy = snapshot.privacy_profile;
    }
    if (snapshot.requested_amount_cents > 0) {
        m_state.amountCents = snapshot.requested_amount_cents;
        m_state.amount = snapshot.requested_amount_cents / 100.0;
    } else if (snapshot.has_costs) {
        m_state.amountCents = snapshot.payment_cents;
        m_state.amount = snapshot.payment_cents / 100.0;
    } else if (snapshot.authoritative) {
        // Do not let a sparse restart snapshot inherit an amount from a
        // previously displayed session.
        m_state.amountCents = 0;
        m_state.amount = 0.0;
    }

    return true;
}

void SendController::stop()
{
    clearContinuations();
    m_state.pollIntervalMs = 1500;
    m_poll_scheduled = false;
}

void SendController::historyWaiting(bool waiting)
{
    const bool unsigned_preparation = m_state.authorizationCommitment.isEmpty() &&
        (m_state.artifact.isEmpty() || m_state.artifact == QLatin1String("none"));
    if (waiting && unsigned_preparation && m_state.activeSendStarted.isValid() && m_state.historyPausedAt < 0) {
        m_state.historyPausedAt = m_state.activeSendStarted.elapsed();
    } else if (!waiting && m_state.historyPausedAt >= 0) {
        if (m_state.activeSendStarted.isValid()) {
            m_state.historyPausedMs += m_state.activeSendStarted.elapsed() - m_state.historyPausedAt;
        }
        m_state.historyPausedAt = -1;
    }
}

void SendController::walletChanged()
{
    ++m_state.walletGeneration;
    resetSession();
    m_state.historyPausedAt = -1;
    m_state.historyPausedMs = 0;
}

void SendController::resetSession()
{
    m_completion_verified = false;
    m_absence_verified = false;
    stop();
    ++m_state.sendGeneration;
    m_state.preparationCancelRequested = false;
    m_state.sendTemplate = UniValue{};
    m_state.requestId.clear();
    m_state.sessionId.clear();
    m_state.sessionState.clear();
    m_state.unsignedClosed = false;
    m_state.offerReadyForReview = false;
    m_state.attemptState.clear();
    m_state.artifact.clear();
    m_state.pendingPhase.clear();
    m_state.broadcastState.clear();
    m_state.confirmationState.clear();
    m_state.transactionId.clear();
    m_state.recoveryTransactionId.clear();
    m_state.resultStatus.clear();
    m_state.resultSequence = -1;
    m_state.recoveryExpiresAt = -1;
    m_state.sessionCreatedAt = 0;
    m_state.terminalNoticeShown = false;
    m_state.allowedActions.clear();
    m_state.allowedActionsKnown = false;
    m_state.sessionPersisted = false;
    m_state.address.clear();
    m_state.authorizationCommitment.clear();
    m_state.sessionPrivacy.clear();
    m_state.recoveryAuthorizationCommitment.clear();
    m_state.amount = 0.0;
    m_state.amountCents = 0;
    m_state.recoveryMaximumServiceFeeCents = 0;
    m_state.recoveryActive = false;
    m_state.confirmationGuard.Reset();
    m_state.recoveryConfirmationGuard.Reset();
    m_state.restoredOptions = UniValue{UniValue::VOBJ};
}

SendController::FailureDecision SendController::failedReply(const RpcError& error)
{
    const auto decision = failure(error, m_state.sessionPersisted, m_state.artifact == QLatin1String("none") && allows(QStringLiteral("abandon_unsigned")));
    if (decision.invalidate_review) {
        clearReview();
    }
    if (decision.discard_uncreated_request) {
        m_state.requestId.clear();
        m_state.sessionId.clear();
        m_state.sessionState.clear();
        m_state.unsignedClosed = false;
        m_state.offerReadyForReview = false;
        m_state.attemptState.clear();
        m_state.artifact.clear();
        m_state.pendingPhase.clear();
        m_state.broadcastState.clear();
        m_state.confirmationState.clear();
        m_state.transactionId.clear();
        m_state.recoveryTransactionId.clear();
        m_state.resultStatus.clear();
        m_state.resultSequence = -1;
        m_state.recoveryExpiresAt = -1;
        m_state.sessionCreatedAt = 0;
        m_state.terminalNoticeShown = false;
        m_state.sessionPersisted = false;
        clearReview();
    }
    return decision;
}


bool SendController::canCancelPreparation() const
{
    if (m_state.preparationCancelRequested ||
        m_state.requestId.isEmpty() || m_state.recoveryActive ||
        !m_state.authorizationCommitment.isEmpty() ||
        (!m_state.artifact.isEmpty() && m_state.artifact != QLatin1String("none")) ||
        IsTerminalPaymasterState(m_state.sessionState) ||
        !m_state.activeSendParams.isArray() || m_state.activeSendParams.size() != 7) return false;
    const UniValue& options = m_state.activeSendParams[6];
    return options.find_value("prepare_only").isTrue() &&
        options.find_value("authorization_commitment").isNull() &&
        options.find_value("fee_mode").isStr() && options.find_value("fee_mode").get_str() == "paymaster" &&
        options.find_value("request_id").isStr() && options.find_value("request_id").get_str() == m_state.requestId.toStdString();
}

} // namespace PaymasterQt
