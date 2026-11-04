// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymastersendcontroller.h>
#include <qt/paymasterconfirmation.h>
#include <paymaster/types.h>
#include <QDateTime>
#include <exception>
#include <limits>
namespace PaymasterQt {
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
} // namespace PaymasterQt
