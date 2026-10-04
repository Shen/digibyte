// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/paymastersendwidget.h>
#include <qt/paymasteramount.h>
#include <qt/digidollarsendwidget.h>
#include <qt/digidollarstatus.h>

#include <qt/walletmodel.h>
#include <qt/guiutil.h>
#include <qt/guiconstants.h>
#include <consensus/amount.h>
#include <logging.h>
#include <pubkey.h>
#include <uint256.h>
#include <paymaster/types.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>

#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPushButton>
#include <QProgressBar>
#include <QPainter>
#include <QPixmap>
#include <QShowEvent>
#include <QHideEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QFont>
#include <QMessageBox>
#include <QSizePolicy>
#include <QPointer>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QUuid>
#include <QWheelEvent>

using namespace std::chrono_literals;

namespace {
class NoWheelSpinBox final : public QSpinBox
{
public:
    explicit NoWheelSpinBox(QWidget* parent) : QSpinBox(parent) { setLocale(QLocale::c()); }

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelComboBox final : public QComboBox
{
public:
    explicit NoWheelComboBox(QWidget* parent) : QComboBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class FeeFundingCard final : public QFrame
{
public:
    explicit FeeFundingCard(QWidget* parent) : QFrame(parent) {}

    void setChoiceButton(QRadioButton* button) { m_button = button; }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_button && isEnabled()) {
            m_button->setChecked(true);
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    QRadioButton* m_button{nullptr};
};

bool DecodeClientSafetyPolicy(const UniValue& policy,
                              qint64& maximum_per_transaction,
                              qint64& maximum_per_day)
{
    const UniValue& per_transaction =
        policy.find_value("maximum_service_fee_per_transaction_cents");
    const UniValue& per_day =
        policy.find_value("maximum_service_fee_per_day_cents");
    if (!policy.isObject() || !per_transaction.isNum() || !per_day.isNum()) {
        return false;
    }
    try {
        maximum_per_transaction = per_transaction.getInt<qint64>();
        maximum_per_day = per_day.getInt<qint64>();
    } catch (const std::exception&) {
        return false;
    }
    // These are representation checks only. Relationships between the two
    // limits are Core policy and must not be reimplemented in Qt.
    return maximum_per_transaction >= 0 &&
        maximum_per_transaction <=
            DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS &&
        maximum_per_day >= 0 &&
        maximum_per_day <= DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS;
}

bool DecodeClientSafetyStatus(const UniValue& result, bool& configured,
                              qint64& maximum_per_transaction,
                              qint64& maximum_per_day,
                              qint64& active_reservations,
                              qint64& reserved_cents,
                              qint64& spent_cents,
                              qint64& available_cents)
{
    const UniValue& configured_value = result.find_value("configured");
    if (!result.isObject() || !configured_value.isBool()) return false;
    configured = configured_value.get_bool();
    if (!configured) return true;

    const UniValue& active = result.find_value("active_reservations");
    const UniValue& reserved =
        result.find_value("reserved_service_fee_cents");
    const UniValue& spent =
        result.find_value("spent_service_fee_last_day_cents");
    const UniValue& available =
        result.find_value("available_service_fee_today_cents");
    if (!DecodeClientSafetyPolicy(
            result.find_value("policy"), maximum_per_transaction,
            maximum_per_day) ||
        !active.isNum() || !reserved.isNum() || !spent.isNum() ||
        !available.isNum()) {
        return false;
    }
    try {
        active_reservations = active.getInt<qint64>();
        reserved_cents = reserved.getInt<qint64>();
        spent_cents = spent.getInt<qint64>();
        available_cents = available.getInt<qint64>();
    } catch (const std::exception&) {
        return false;
    }
    return active_reservations >= 0 && reserved_cents >= 0 &&
        spent_cents >= 0 && available_cents >= 0;
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
        state == QStringLiteral("CANCELED_SAFE") ||
        state == QStringLiteral("FAILED") ||
        state == QStringLiteral("CONFLICTED");
}

struct PaymasterSessionSnapshot {
    bool authoritative{false};
    bool reported_final{false};
    bool unsigned_closed{false};
    bool offer_ready_for_review{false};
    QString request_id;
    QString session_id;
    QString state;
    QString pending_phase;
    QString broadcast_state;
    QString confirmation_state;
    QString provider_id;
    QString policy_hash;
    QString artifact;
    QString attempt_state;
    QString recipient;
    QString privacy_profile;
    UniValue restored_options{UniValue::VOBJ};
    QString transaction_id;
    QString recovery_transaction_id;
    QString result_status;
    qint64 result_sequence{-1};
    qint64 recovery_expires_at{-1};
    QStringList allowed_actions;
    qint64 payment_cents{-1};
    qint64 service_fee_cents{-1};
    qint64 total_cents{-1};
    qint64 expires_at{-1};
    qint64 requested_amount_cents{-1};
    bool has_costs{false};
    bool has_expiry{false};
    bool allowed_actions_known{false};
};

bool DecodePaymasterSessionSnapshot(const UniValue& result,
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
} // namespace


PaymasterSendWidget::PaymasterSendWidget(DigiDollarSendWidget& form)
    : QWidget(&form), m_form(form), m_paymasterPollTimer(new QTimer(this))
{
    setObjectName("paymasterSendWidget");
    setupFeeSection();
    connectSignals();
}

void PaymasterSendWidget::amountChanged(bool setting_sweep)
{
    if (!setting_sweep) m_sendAllSpendableDD = false;
    invalidatePaymasterOfferPreview();
}

void PaymasterSendWidget::beginSweep()
{
    if (paymasterModeSelected()) m_subtractPaymasterFeeCheck->setChecked(true);
    m_sendAllSpendableDD = paymasterModeSelected();
}

void PaymasterSendWidget::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updateFeeDisplay();
    if (m_privacy) {
        m_paymasterActiveRetryRequest.clear();
        m_paymasterActiveRetryStarted.invalidate();
        m_feeValue->setText(m_form.maskValue(m_feeValue->text()));
        m_totalValue->setText(m_form.maskValue(m_form.formatDDAmount(0)));
        m_feeSummary->setText(m_form.maskValue(m_feeSummary->text()));
    }
    applyPaymasterPrivacy();
}

void PaymasterSendWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateFeeChoiceLayout();
}

void PaymasterSendWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateOfferCheckIcon();
}

void PaymasterSendWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (m_offerIconTimer) m_offerIconTimer->stop();
}

void PaymasterSendWidget::connectSignals()
{
    connect(m_dgbFeeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked) return;
        m_feeModeCombo->setCurrentIndex(m_feeModeCombo->findData(QStringLiteral("dgb")));
    });
    connect(m_autoFeeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked) return;
        m_feeModeCombo->setCurrentIndex(m_feeModeCombo->findData(QStringLiteral("auto")));
    });
    connect(m_paymasterFeeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked) return;
        m_feeModeCombo->setCurrentIndex(m_feeModeCombo->findData(QStringLiteral("paymaster")));
    });
    connect(m_feeModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
                invalidatePaymasterOfferPreview();
                onFeeModeChanged();
            });
    connect(m_subtractPaymasterFeeCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) m_sendAllSpendableDD = false;
        invalidatePaymasterOfferPreview();
        updateFeeDisplay();
    });
    connect(m_paymasterExplanationButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::showPaymasterExplanation);
    connect(m_advancedPaymasterButton, &QPushButton::toggled, this, [this](bool checked) {
        m_advancedPaymasterButton->setText(
            checked ? DigiDollarSendWidget::tr("Hide advanced Paymaster settings")
                    : DigiDollarSendWidget::tr("Advanced Paymaster settings"));
        onFeeModeChanged();
    });
    connect(m_configureClientSafetyButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::configureClientSafetyPolicy);
    connect(m_feeCapSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        invalidatePaymasterOfferPreview();
        updateFeeDisplay();
    });
    connect(m_privacyCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool high = m_privacyCombo->currentData().toString() == QStringLiteral("high");
        if (high) m_maxAttemptsSpin->setValue(1);
        m_maxAttemptsSpin->setEnabled(!high);
        invalidatePaymasterOfferPreview();
    });
    connect(m_selectionCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { invalidatePaymasterOfferPreview(); });
    connect(m_maxAttemptsSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this] { invalidatePaymasterOfferPreview(); });
    connect(m_refreshOffersButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::refreshPaymasterOffers);
    connect(m_loadPersistedPaymasterSessionButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::loadSelectedPersistedPaymasterSession);
    connect(m_retrySessionButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::retryPaymasterSession);
    connect(m_fallbackSessionButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::fallbackPaymasterSession);
    connect(m_recoverSessionButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::recoverPaymasterSessionToSelf);
    connect(m_abandonSessionButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::abandonUnsignedPaymasterSession);
    connect(m_cancelQuoteButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::cancelPaymasterQuote);
    connect(m_paymasterPrimaryButton, &QPushButton::clicked,
            this, &PaymasterSendWidget::onPaymasterPrimaryAction);
    connect(m_paymasterMoreButton, &QPushButton::toggled,
            m_paymasterSecondaryActions, &QFrame::setVisible);
    connect(m_paymasterTechnicalButton, &QPushButton::toggled,
            m_paymasterTechnicalDetails, &QFrame::setVisible);
    connect(m_paymasterPollTimer, &QTimer::timeout,
            this, &PaymasterSendWidget::pollPaymasterSession);
    m_paymasterPollTimer->setObjectName("paymasterSessionPollTimer");
    m_paymasterPollTimer->setInterval(1500);
    m_paymasterPollTimer->setSingleShot(true);
}

void PaymasterSendWidget::updateFeeChoiceLayout()
{
    if (!m_feeChoicesLayout || !m_dgbFeeCard || !m_autoFeeCard || !m_paymasterFeeCard) return;

    const bool stack = width() < 980;
    const std::array<QFrame*, 3> cards{m_dgbFeeCard, m_autoFeeCard, m_paymasterFeeCard};
    for (QFrame* card : cards) m_feeChoicesLayout->removeWidget(card);
    for (int column = 0; column < 3; ++column) m_feeChoicesLayout->setColumnStretch(column, 0);
    for (int index = 0; index < static_cast<int>(cards.size()); ++index) {
        m_feeChoicesLayout->addWidget(cards[index], stack ? index : 0, stack ? 0 : index);
        if (!stack) m_feeChoicesLayout->setColumnStretch(index, 1);
    }
}

void PaymasterSendWidget::setupFeeSection()
{
    // This is intentionally a user decision about who funds the network fee,
    // not a raw protocol-mode selector. Own DGB remains the safe default;
    // Paymaster-specific limits and offer details stay hidden until relevant.
    m_feeFrame = new QFrame(this);
    m_feeFrame->setFrameStyle(QFrame::StyledPanel);
    m_feeFrame->setFrameShadow(QFrame::Sunken);
    m_feeFrame->setObjectName("feeFrame");

    m_feeLayout = new QGridLayout(m_feeFrame);
    m_feeLayout->setSpacing(8);
    m_feeLayout->setContentsMargins(10, 10, 10, 10);
    m_feeLayout->setHorizontalSpacing(12);
    m_feeLayout->setVerticalSpacing(8);

    m_feeHeading = new QLabel(DigiDollarSendWidget::tr("How should the network fee be paid?"), m_feeFrame);
    m_feeHeading->setObjectName("feeFundingHeading");
    QFont heading_font = m_feeHeading->font();
    heading_font.setBold(true);
    heading_font.setPointSize(heading_font.pointSize() + 1);
    m_feeHeading->setFont(heading_font);
    m_feeLayout->addWidget(m_feeHeading, 0, 0, 1, 2);

    m_feeIntroduction = new QLabel(DigiDollarSendWidget::tr(
        "Every $DD transfer needs a DigiByte network fee. This fee is paid "
        "separately and is never deducted from the amount received."), m_feeFrame);
    m_feeIntroduction->setObjectName("feeFundingIntroduction");
    m_feeIntroduction->setWordWrap(true);
    m_feeLayout->addWidget(m_feeIntroduction, 1, 0, 1, 2);

    m_feeChoicesFrame = new QFrame(m_feeFrame);
    m_feeChoicesFrame->setObjectName("feeFundingChoices");
    m_feeChoicesLayout = new QGridLayout(m_feeChoicesFrame);
    m_feeChoicesLayout->setContentsMargins(0, 0, 0, 0);
    m_feeChoicesLayout->setHorizontalSpacing(10);
    m_feeChoicesLayout->setVerticalSpacing(8);
    m_feeLayout->addWidget(m_feeChoicesFrame, 2, 0, 1, 2);

    auto* choices = new QButtonGroup(this);
    auto add_choice = [this, choices](QFrame*& card_member, QRadioButton*& radio,
                                      const QString& object_name,
                                      const QString& title, const QString& description,
                                      bool include_paymaster_help = false) {
        auto* card = new FeeFundingCard(m_feeFrame);
        card_member = card;
        card->setObjectName(object_name + QStringLiteral("Card"));
        card->setProperty("feeChoice", true);
        card->setProperty("feeSelected", false);
        card->setFrameShape(QFrame::StyledPanel);
        card->setCursor(Qt::PointingHandCursor);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        card->setMinimumHeight(112);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(14, 10, 14, 10);
        layout->setSpacing(5);
        auto* title_row = new QHBoxLayout();
        radio = new QRadioButton(title, card);
        radio->setObjectName(object_name);
        radio->setAccessibleName(title);
        radio->setAccessibleDescription(description);
        QFont title_font = radio->font();
        title_font.setBold(true);
        radio->setFont(title_font);
        auto* selected_badge = new QLabel(DigiDollarSendWidget::tr("Selected"), card);
        selected_badge->setProperty("feeChoiceBadge", true);
        selected_badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        selected_badge->hide();
        title_row->addWidget(radio, 1);
        title_row->addWidget(selected_badge, 0, Qt::AlignRight | Qt::AlignVCenter);
        auto* help = new QLabel(description, card);
        help->setObjectName(object_name + QStringLiteral("Description"));
        help->setProperty("feeChoiceDescription", true);
        help->setWordWrap(true);
        help->setTextInteractionFlags(Qt::TextSelectableByMouse);
        help->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addLayout(title_row);
        layout->addWidget(help);
        if (include_paymaster_help) {
            m_paymasterExplanationButton = new QPushButton(DigiDollarSendWidget::tr("Learn how Paymasters work"), card);
            m_paymasterExplanationButton->setObjectName("paymasterExplanationButton");
            m_paymasterExplanationButton->setAccessibleDescription(
                DigiDollarSendWidget::tr("Explain Paymaster fees, privacy and transaction checks"));
            layout->addWidget(m_paymasterExplanationButton, 0, Qt::AlignLeft);
        }
        choices->addButton(radio);
        card->setChoiceButton(radio);
        connect(radio, &QRadioButton::toggled, card, [card, selected_badge](bool checked) {
            card->setProperty("feeSelected", checked);
            selected_badge->setVisible(checked);
            card->style()->unpolish(card);
            card->style()->polish(card);
            card->update();
        });
    };
    add_choice(m_dgbFeeCard, m_dgbFeeRadio, QStringLiteral("feeFundingDgb"),
               DigiDollarSendWidget::tr("Own DGB"),
               DigiDollarSendWidget::tr("Recommended · estimated ~0.1 DGB · no additional $DD fee."));
    add_choice(m_autoFeeCard, m_autoFeeRadio, QStringLiteral("feeFundingAuto"),
               DigiDollarSendWidget::tr("Automatic"),
               DigiDollarSendWidget::tr("Use own DGB first; find a Paymaster only when suitable DGB is insufficient."));
    add_choice(m_paymasterFeeCard, m_paymasterFeeRadio, QStringLiteral("feeFundingPaymaster"),
               DigiDollarSendWidget::tr("Paymaster"),
               DigiDollarSendWidget::tr("A provider supplies DGB; the additional $DD service fee may be zero."), true);
    updateFeeChoiceLayout();
    m_dgbFeeRadio->setChecked(true);

    // Keep the canonical RPC values in one internal control. It is deliberately
    // hidden; the explanatory radio choices above are the user-facing surface.
    m_feeModeCombo = new NoWheelComboBox(this);
    m_feeModeCombo->setObjectName("paymasterFeeMode");
    m_feeModeCombo->addItem(DigiDollarSendWidget::tr("Own DGB"), QStringLiteral("dgb"));
    m_feeModeCombo->addItem(DigiDollarSendWidget::tr("Automatic (DGB first, Paymaster if needed)"), QStringLiteral("auto"));
    m_feeModeCombo->addItem(DigiDollarSendWidget::tr("Paymaster Network"), QStringLiteral("paymaster"));
    m_feeModeCombo->hide();

    m_feeModeExplanation = new QLabel(m_feeFrame);
    m_feeModeExplanation->setObjectName("feeFundingModeExplanation");
    m_feeModeExplanation->setWordWrap(true);
    m_feeModeExplanation->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Retained as a hidden compatibility/accessibility source for existing
    // translations and tests. The visible summary below now carries the one
    // contextual explanation, avoiding duplicate paragraphs.
    m_feeModeExplanation->hide();

    m_feeSummary = new QLabel(m_feeFrame);
    m_feeSummary->setObjectName("feeFundingSummary");
    m_feeSummary->setWordWrap(true);
    m_feeSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_feeSummary->setFrameShape(QFrame::StyledPanel);
    m_feeSummary->setMargin(8);
    m_feeLayout->addWidget(m_feeSummary, 3, 0, 1, 2);

    m_subtractPaymasterFeeCheck = new QCheckBox(
        DigiDollarSendWidget::tr("Deduct the Paymaster fee from the entered amount — the recipient receives less."),
        m_feeFrame);
    m_subtractPaymasterFeeCheck->setObjectName("subtractPaymasterFeeFromAmount");
    m_subtractPaymasterFeeCheck->setToolTip(DigiDollarSendWidget::tr(
        "The amount entered above becomes the exact maximum $DD outflow. Core accepts only "
        "an offer whose rounded service fee and recipient amount add up exactly to it."));
    m_subtractPaymasterFeeCheck->setAccessibleDescription(DigiDollarSendWidget::tr(
        "Use the entered amount as an exact total and subtract the authenticated "
        "Paymaster service fee before paying the recipient"));
    m_feeLayout->addWidget(m_subtractPaymasterFeeCheck, 4, 0);

    m_advancedPaymasterButton = new QPushButton(DigiDollarSendWidget::tr("Advanced Paymaster settings"), m_feeFrame);
    m_advancedPaymasterButton->setObjectName("advancedPaymasterSettingsToggle");
    m_advancedPaymasterButton->setCheckable(true);
    m_advancedPaymasterButton->setAccessibleDescription(
        DigiDollarSendWidget::tr("Show or hide optional provider, privacy and offer controls"));
    m_feeLayout->addWidget(m_advancedPaymasterButton, 4, 1, Qt::AlignRight);

    m_clientSafetyFrame = new QFrame(m_feeFrame);
    m_clientSafetyFrame->setObjectName("paymasterClientSafetyFrame");
    m_clientSafetyFrame->setFrameShape(QFrame::StyledPanel);
    DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::WAITING);
    auto* safety_layout = new QHBoxLayout(m_clientSafetyFrame);
    m_clientSafetyStatus = new QLabel(
        DigiDollarSendWidget::tr("Checking this wallet's Paymaster service-fee limits…"), m_clientSafetyFrame);
    m_clientSafetyStatus->setObjectName("sendPaymasterClientSafetyStatus");
    m_clientSafetyStatus->setWordWrap(true);
    m_configureClientSafetyButton = new QPushButton(
        DigiDollarSendWidget::tr("Set service-fee limits…"), m_clientSafetyFrame);
    m_configureClientSafetyButton->setObjectName("configurePaymasterClientSafety");
    m_configureClientSafetyButton->setAccessibleDescription(
        DigiDollarSendWidget::tr("Configure wallet-local maximum Paymaster service fees"));
    safety_layout->addWidget(m_clientSafetyStatus, 1);
    safety_layout->addWidget(m_configureClientSafetyButton);
    m_feeLayout->addWidget(m_clientSafetyFrame, 6, 0, 1, 2);

    m_advancedPaymasterFrame = new QFrame(m_feeFrame);
    m_advancedPaymasterFrame->setObjectName("advancedPaymasterSettings");
    m_advancedPaymasterFrame->setFrameShape(QFrame::StyledPanel);
    auto* advanced_layout = new QGridLayout(m_advancedPaymasterFrame);

    auto* advanced_help = new QLabel(DigiDollarSendWidget::tr(
        "These controls are optional. Core always enforces the lower of this transfer's "
        "limit, the wallet-local safety limit and the provider's signed offer."),
        m_advancedPaymasterFrame);
    advanced_help->setObjectName("advancedPaymasterSettingsExplanation");
    advanced_help->setWordWrap(true);
    advanced_layout->addWidget(advanced_help, 0, 0, 1, 2);

    m_clientSafetyDetails = new QLabel(
        DigiDollarSendWidget::tr("Wallet protection details are being loaded…"), m_advancedPaymasterFrame);
    m_clientSafetyDetails->setObjectName("sendPaymasterClientSafetyDetails");
    m_clientSafetyDetails->setWordWrap(true);
    advanced_layout->addWidget(m_clientSafetyDetails, 1, 0, 1, 2);

    m_feeCapSpin = new PaymasterAmountSpinBox(m_advancedPaymasterFrame);
    m_feeCapSpin->setObjectName("paymasterFeeCap");
    m_feeCapSpin->setRange(0, 10000000);
    m_feeCapSpin->setValue(100);
    m_feeCapSpin->setToolTip(DigiDollarSendWidget::tr("Hard maximum Paymaster service fee; it can never be exceeded"));
    m_feeCapSpin->setAccessibleName(DigiDollarSendWidget::tr("Maximum additional Paymaster service fee"));
    advanced_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Maximum additional Paymaster fee:"), m_advancedPaymasterFrame), 2, 0);
    auto* fee_cap_layout = new QVBoxLayout();
    fee_cap_layout->setContentsMargins(0, 0, 0, 0);
    fee_cap_layout->addWidget(m_feeCapSpin);
    m_feeCapPercent = new QLabel(m_advancedPaymasterFrame);
    m_feeCapPercent->setObjectName("paymasterFeeCapPercent");
    m_feeCapPercent->setWordWrap(true);
    m_feeCapPercent->setToolTip(DigiDollarSendWidget::tr("Comparison only: changing the payment amount never increases your DD fee limit. The provider's actual fee is shown separately."));
    fee_cap_layout->addWidget(m_feeCapPercent);
    advanced_layout->addLayout(fee_cap_layout, 2, 1);

    m_maxAttemptsSpin = new NoWheelSpinBox(m_advancedPaymasterFrame);
    m_maxAttemptsSpin->setObjectName("paymasterMaximumAttempts");
    m_maxAttemptsSpin->setRange(1, 16);
    m_maxAttemptsSpin->setValue(3);
    m_maxAttemptsSpin->setToolTip(DigiDollarSendWidget::tr("Maximum number of strictly sequential provider attempts"));
    m_maxAttemptsSpin->setAccessibleName(DigiDollarSendWidget::tr("Maximum Paymaster provider attempts"));
    advanced_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Maximum provider attempts:"), m_advancedPaymasterFrame), 3, 0);
    advanced_layout->addWidget(m_maxAttemptsSpin, 3, 1);

    m_privacyCombo = new NoWheelComboBox(m_advancedPaymasterFrame);
    m_privacyCombo->setObjectName("paymasterPrivacy");
    m_privacyCombo->addItem(DigiDollarSendWidget::tr("Standard privacy (BIP324)"), QStringLiteral("standard"));
    m_privacyCombo->addItem(DigiDollarSendWidget::tr("High privacy (Tor only)"), QStringLiteral("high"));
    m_privacyCombo->setToolTip(DigiDollarSendWidget::tr(
        "Paymaster privacy improves pseudonymity but does not guarantee anonymity. "
        "The selected provider necessarily receives the payment details needed to sign."));
    m_privacyCombo->setAccessibleName(DigiDollarSendWidget::tr("Paymaster privacy profile"));
    m_selectionCombo = new NoWheelComboBox(m_advancedPaymasterFrame);
    m_selectionCombo->setObjectName("paymasterSelection");
    m_selectionCombo->addItem(DigiDollarSendWidget::tr("Lowest total cost"), QStringLiteral("lowest_total_cost"));
    m_selectionCombo->addItem(DigiDollarSendWidget::tr("Privacy weighted"), QStringLiteral("privacy_weighted"));
    m_selectionCombo->setAccessibleName(DigiDollarSendWidget::tr("Paymaster provider selection policy"));
    advanced_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Privacy:"), m_advancedPaymasterFrame), 4, 0);
    advanced_layout->addWidget(m_privacyCombo, 4, 1);
    advanced_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Provider selection:"), m_advancedPaymasterFrame), 5, 0);
    advanced_layout->addWidget(m_selectionCombo, 5, 1);

    m_offerCheckFrame = new QFrame(m_feeFrame);
    m_offerCheckFrame->setObjectName("paymasterOfferCheckFrame");
    auto* offer_check_layout = new QGridLayout(m_offerCheckFrame);
    DigiDollarStatus::SetBanner(m_offerCheckFrame, DigiDollarStatus::Kind::INFO);
    offer_check_layout->setContentsMargins(16, 16, 16, 16);
    offer_check_layout->setHorizontalSpacing(12);
    offer_check_layout->setVerticalSpacing(6);
    offer_check_layout->setColumnStretch(1, 1);
    m_offerStateIcon = new QLabel(m_offerCheckFrame);
    m_offerStateIcon->setObjectName("paymasterOfferStateIcon");
    m_offerStateIcon->setTextFormat(Qt::PlainText);
    m_offerStateIcon->setAlignment(Qt::AlignCenter);
    m_offerStateIcon->setFixedSize(44, 44);
    offer_check_layout->addWidget(m_offerStateIcon, 0, 0, 2, 1, Qt::AlignTop);
    m_offerIconTimer = new QTimer(this);
    m_offerIconTimer->setObjectName("paymasterOfferIconTimer");
    m_offerIconTimer->setInterval(80);
    connect(m_offerIconTimer, &QTimer::timeout, this, &PaymasterSendWidget::renderOfferSpinner);
    m_offersStatus = new QLabel(
        DigiDollarSendWidget::tr("Public offers have not been checked yet."),
        m_offerCheckFrame);
    m_offersStatus->setObjectName("paymasterOffersStatus");
    m_offersStatus->setTextFormat(Qt::PlainText);
    m_offersStatus->setWordWrap(true);
    m_offersStatus->setAccessibleDescription(m_offersStatus->text());
    DigiDollarStatus::SetText(m_offersStatus, DigiDollarStatus::Kind::INFO);
    offer_check_layout->addWidget(m_offersStatus, 0, 1);
    m_refreshOffersButton = new QPushButton(DigiDollarSendWidget::tr("Check offers"), m_offerCheckFrame);
    m_refreshOffersButton->setObjectName("refreshPaymasterOffers");
    m_refreshOffersButton->setAutoDefault(false);
    m_refreshOffersButton->setDefault(false);
    m_refreshOffersButton->setToolTip(DigiDollarSendWidget::tr(
        "Read the latest public offers already known to this node. This does not contact a provider, reserve funds or authorize a payment."));
    m_refreshOffersButton->setAccessibleDescription(m_refreshOffersButton->toolTip());
    offer_check_layout->addWidget(m_refreshOffersButton, 0, 2, Qt::AlignRight | Qt::AlignTop);
    m_offersUpdated = new QLabel(m_offerCheckFrame);
    m_offersUpdated->setObjectName("paymasterOffersUpdated");
    m_offersUpdated->setTextFormat(Qt::PlainText);
    m_offersUpdated->setWordWrap(true);
    m_offersUpdated->setText(DigiDollarSendWidget::tr("Automatic check every 10 s while this form is open."));
    offer_check_layout->addWidget(m_offersUpdated, 2, 1, 1, 2);
    m_offerCheckHelp = new QLabel(DigiDollarSendWidget::tr(
                                            "Prepare payment verifies the provider and fee and may temporarily reserve $DD. You confirm before signing."),
                                        m_offerCheckFrame);
    m_offerCheckHelp->setObjectName("paymasterOfferCheckHelp");
    m_offerCheckHelp->setTextFormat(Qt::PlainText);
    m_offerCheckHelp->setWordWrap(true);
    m_offerCheckHelp->setToolTip(DigiDollarSendWidget::tr(
        "The preview reads public announcements already known to this node. It does not verify your fee limit, privacy choice or the provider connection. "
        "Preparation uses the latest offers and checks these requirements; it can still fail if no suitable provider is available. "
        "Neither checking offers nor preparing a payment authorizes a signature."));
    offer_check_layout->addWidget(m_offerCheckHelp, 1, 1, 1, 2);
    m_feeLayout->addWidget(m_offerCheckFrame, 5, 0, 1, 2);
    m_offerExpiryTimer = new QTimer(this);
    m_offerExpiryTimer->setObjectName("paymasterOfferExpiryTimer");
    m_offerExpiryTimer->setSingleShot(true);
    connect(m_offerExpiryTimer, &QTimer::timeout, this, &PaymasterSendWidget::expirePaymasterOfferPreview);
    auto* offer_refresh_timer = new QTimer(this);
    offer_refresh_timer->setObjectName("paymasterOfferRefreshTimer");
    offer_refresh_timer->setInterval(10000);
    connect(offer_refresh_timer, &QTimer::timeout, this, &PaymasterSendWidget::pollPaymasterOffers);
    offer_refresh_timer->start();
    m_offersTable = new QTableWidget(0, 6, m_advancedPaymasterFrame);
    m_offersTable->setObjectName("paymasterOffers");
    QHeaderView* offers_header = m_offersTable->horizontalHeader();
    offers_header->setObjectName("paymasterOffersHeader");
    // QTableWidget constructs and may polish its internal header before the
    // caller can assign an object name. Re-polish it so the scoped header rule
    // wins immediately even when the application stylesheet was loaded first.
    offers_header->style()->unpolish(offers_header);
    offers_header->style()->polish(offers_header);
    // Row numbers do not add information to this read-only preview and would
    // otherwise expose a separately styled header strip beside the DD table.
    m_offersTable->verticalHeader()->hide();
    m_offersTable->setAccessibleName(DigiDollarSendWidget::tr("Eligible Paymaster offer preview"));
    m_offersTable->setAccessibleDescription(DigiDollarSendWidget::tr(
        "Read-only preview. Core authenticates and confirms the exact selected offer before signing."));
    m_offersTable->setHorizontalHeaderLabels({DigiDollarSendWidget::tr("Provider"), DigiDollarSendWidget::tr("Payment model"), DigiDollarSendWidget::tr("Service fee"),
                                               DigiDollarSendWidget::tr("Total $DD"), DigiDollarSendWidget::tr("Reliability"), DigiDollarSendWidget::tr("Announcement expires")});
    offers_header->setSectionResizeMode(QHeaderView::Stretch);
    m_offersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_offersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_offersTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_offersTable->setFocusPolicy(Qt::StrongFocus);
    m_offersTable->setAlternatingRowColors(true);
    m_offersTable->setToolTip(DigiDollarSendWidget::tr(
        "Offer preview only. Core authenticates and selects the exact offer when the transfer is prepared."));
    m_offersTable->setMinimumHeight(130);
    advanced_layout->addWidget(m_offersTable, 8, 0, 1, 2);
    m_feeLayout->addWidget(m_advancedPaymasterFrame, 7, 0, 1, 2);

    m_persistedPaymasterSessionsFrame = new QFrame(m_feeFrame);
    m_persistedPaymasterSessionsFrame->setObjectName(
        "persistedPaymasterSessionsFrame");
    auto* persisted_layout = new QGridLayout(
        m_persistedPaymasterSessionsFrame);
    m_persistedSessionsStatus = new QLabel(
        DigiDollarSendWidget::tr("Interrupted Paymaster transfers:"),
        m_persistedPaymasterSessionsFrame);
    m_persistedSessionsStatus->setWordWrap(true);
    m_persistedSessionsStatus->setTextFormat(Qt::PlainText);
    m_sessionDiscoveryError = new QLabel(m_persistedPaymasterSessionsFrame);
    m_sessionDiscoveryError->setObjectName("paymasterSessionDiscoveryError");
    m_sessionDiscoveryError->setTextFormat(Qt::PlainText);
    m_sessionDiscoveryError->setWordWrap(true);
    m_sessionDiscoveryError->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sessionDiscoveryError->hide();
    m_sessionDiscoveryProgress = new QProgressBar(m_persistedPaymasterSessionsFrame);
    m_sessionDiscoveryProgress->setObjectName("paymasterSessionDiscoveryProgress");
    m_sessionDiscoveryProgress->setRange(0, 0);
    m_sessionDiscoveryProgress->setTextVisible(false);
    m_sessionDiscoveryProgress->setAccessibleName(DigiDollarSendWidget::tr("Loading saved Paymaster transfers"));
    m_sessionDiscoveryProgress->hide();
    m_persistedSessionsStatus->setObjectName("persistedPaymasterSessionsStatus");
    m_persistedPaymasterSessions = new QComboBox(
        m_persistedPaymasterSessionsFrame);
    m_persistedPaymasterSessions->setObjectName(
        "persistedPaymasterSessions");
    m_persistedPaymasterSessions->setAccessibleDescription(DigiDollarSendWidget::tr(
        "Persisted wallet sessions found by Core. Selecting one never signs, broadcasts or recovers it."));
    m_loadPersistedPaymasterSessionButton = new QPushButton(
        DigiDollarSendWidget::tr("Review selected transfer"), m_persistedPaymasterSessionsFrame);
    m_loadPersistedPaymasterSessionButton->setObjectName(
        "loadPersistedPaymasterSession");
    persisted_layout->addWidget(m_persistedSessionsStatus, 0, 0, 1, 2);
    persisted_layout->addWidget(m_sessionDiscoveryProgress, 1, 0, 1, 2);
    persisted_layout->addWidget(m_sessionDiscoveryError, 2, 0, 1, 2);
    persisted_layout->addWidget(m_persistedPaymasterSessions, 3, 0);
    persisted_layout->addWidget(m_loadPersistedPaymasterSessionButton, 3, 1);
    m_feeLayout->addWidget(m_persistedPaymasterSessionsFrame, 9, 0, 1, 2);
    m_persistedPaymasterSessionsFrame->hide();

    auto* totals = new QFrame(m_feeFrame);
    totals->setObjectName("feeTotalsFrame");
    auto* totals_layout = new QGridLayout(totals);
    m_feeLabel = new QLabel(DigiDollarSendWidget::tr("Transaction fee:"), totals);
    m_feeLabel->setObjectName("feeLabel");
    m_feeLabel->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_feeLabel->setToolTip(DigiDollarSendWidget::tr("Network fee paid in DGB (not deducted from $DD amount)"));
    m_feeValue = new QLabel("~0.1 DGB", totals);
    m_feeValue->setObjectName("feeValue");
    QFont monospaceFont = GUIUtil::fixedPitchFont();
    m_feeValue->setFont(monospaceFont);
    m_feeValue->setToolTip(DigiDollarSendWidget::tr("Estimated network fee paid in DGB from your DGB balance"));
    totals_layout->addWidget(m_feeLabel, 0, 0);
    totals_layout->addWidget(m_feeValue, 0, 1);

    m_totalLabel = new QLabel(DigiDollarSendWidget::tr("Recipient receives:"), totals);
    m_totalLabel->setObjectName("totalLabel");
    m_totalLabel->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_totalLabel->setToolTip(DigiDollarSendWidget::tr("Exact DigiDollar amount delivered to the recipient"));
    QFont boldFont = m_totalLabel->font();
    boldFont.setBold(true);
    m_totalLabel->setFont(boldFont);
    m_totalValue = new QLabel("0.00 $DD", totals);
    m_totalValue->setObjectName("totalValue");
    m_totalValue->setFont(monospaceFont);
    m_totalValue->setToolTip(DigiDollarSendWidget::tr("The network or service fee is shown separately"));
    totals_layout->addWidget(m_totalLabel, 1, 0);
    totals_layout->addWidget(m_totalValue, 1, 1);
    totals_layout->setColumnStretch(1, 1);
    m_feeLayout->addWidget(totals, 8, 0, 1, 2);
    // The concise summary above already shows these values. Keep the legacy
    // labels available to existing update paths and accessibility tests without
    // presenting the same totals twice in the normal send flow.
    totals->hide();

    m_paymasterSessionFrame = new QFrame(m_feeFrame);
    m_paymasterSessionFrame->setObjectName("paymasterSessionFrame");
    m_paymasterSessionFrame->setFrameShape(QFrame::StyledPanel);
    auto* session_layout = new QGridLayout(m_paymasterSessionFrame);
    auto* session_heading = new QLabel(DigiDollarSendWidget::tr("Current Paymaster transfer"), m_paymasterSessionFrame);
    session_heading->setObjectName("paymasterSessionHeading");
    QFont session_heading_font = session_heading->font();
    session_heading_font.setBold(true);
    session_heading_font.setPointSize(session_heading_font.pointSize() + 1);
    session_heading->setFont(session_heading_font);
    session_layout->addWidget(session_heading, 0, 0, 1, 2);

    m_paymasterNextStepValue = new QLabel(DigiDollarSendWidget::tr("Checking the protected transfer…"), m_paymasterSessionFrame);
    m_paymasterNextStepValue->setObjectName("paymasterSessionNextStep");
    m_paymasterNextStepValue->setWordWrap(true);
    m_paymasterNextStepValue->setTextFormat(Qt::PlainText);
    m_paymasterNextStepValue->setAccessibleName(DigiDollarSendWidget::tr("Paymaster transfer status and next step"));
    session_layout->addWidget(m_paymasterNextStepValue, 1, 0, 1, 2);

    m_paymasterTransferValue = new QLabel(DigiDollarSendWidget::tr("—"), m_paymasterSessionFrame);
    m_paymasterTransferValue->setObjectName("paymasterSessionTransfer");
    m_paymasterTransferValue->setWordWrap(true);
    m_paymasterTransferValue->setTextFormat(Qt::PlainText);
    m_paymasterTransferValue->setAccessibleName(DigiDollarSendWidget::tr("Locked Paymaster transfer recipient and amount"));
    session_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Transfer:"), m_paymasterSessionFrame), 2, 0);
    session_layout->addWidget(m_paymasterTransferValue, 2, 1);

    m_paymasterIdentityValue = new QLabel(DigiDollarSendWidget::tr("Provider not selected yet"), m_paymasterSessionFrame);
    m_paymasterIdentityValue->setObjectName("paymasterSessionProvider");
    m_paymasterIdentityValue->setTextFormat(Qt::PlainText);
    m_paymasterIdentityValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    session_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Provider:"), m_paymasterSessionFrame), 3, 0);
    session_layout->addWidget(m_paymasterIdentityValue, 3, 1);
    m_paymasterCostValue = new QLabel(DigiDollarSendWidget::tr("No service fee authorized yet"), m_paymasterSessionFrame);
    m_paymasterCostValue->setObjectName("paymasterSessionCost");
    m_paymasterCostValue->setWordWrap(true);
    m_paymasterCostValue->setTextFormat(Qt::PlainText);
    session_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Cost:"), m_paymasterSessionFrame), 4, 0);
    session_layout->addWidget(m_paymasterCostValue, 4, 1);

    m_paymasterPrimaryButton = new QPushButton(DigiDollarSendWidget::tr("Check status"), m_paymasterSessionFrame);
    m_paymasterPrimaryButton->setObjectName("paymasterSessionPrimaryAction");
    m_paymasterPrimaryButton->setAccessibleDescription(
        DigiDollarSendWidget::tr("Perform the one recommended safe action for the protected Paymaster transfer"));
    session_layout->addWidget(m_paymasterPrimaryButton, 5, 0, 1, 2);

    auto* disclosure_row = new QHBoxLayout();
    m_paymasterMoreButton = new QPushButton(DigiDollarSendWidget::tr("More options"), m_paymasterSessionFrame);
    m_paymasterMoreButton->setObjectName("paymasterSessionMoreOptions");
    m_paymasterMoreButton->setCheckable(true);
    m_paymasterMoreButton->setAccessibleDescription(
        DigiDollarSendWidget::tr("Show additional actions that Core allows for this exact session"));
    m_paymasterTechnicalButton = new QPushButton(DigiDollarSendWidget::tr("Technical details"), m_paymasterSessionFrame);
    m_paymasterTechnicalButton->setObjectName("paymasterSessionTechnicalToggle");
    m_paymasterTechnicalButton->setCheckable(true);
    m_paymasterTechnicalButton->setAccessibleDescription(
        DigiDollarSendWidget::tr("Show raw wallet state and expiry information for troubleshooting"));
    disclosure_row->addWidget(m_paymasterMoreButton);
    disclosure_row->addWidget(m_paymasterTechnicalButton);
    disclosure_row->addStretch();
    session_layout->addLayout(disclosure_row, 6, 0, 1, 2);

    m_paymasterSecondaryActions = new QFrame(m_paymasterSessionFrame);
    m_paymasterSecondaryActions->setObjectName("paymasterSessionSecondaryActions");
    auto* secondary_layout = new QGridLayout(m_paymasterSecondaryActions);
    secondary_layout->setContentsMargins(0, 4, 0, 0);
    m_retrySessionButton = new QPushButton(DigiDollarSendWidget::tr("Retry the exact provider step"), m_paymasterSecondaryActions);
    m_retrySessionButton->setObjectName("retryPaymasterSession");
    m_retrySessionButton->setToolTip(DigiDollarSendWidget::tr(
        "Idempotently retry only the already persisted provider step; this never selects a different transaction"));
    m_fallbackSessionButton = new QPushButton(DigiDollarSendWidget::tr("Try another Paymaster"), m_paymasterSecondaryActions);
    m_fallbackSessionButton->setObjectName("fallbackPaymasterSession");
    m_fallbackSessionButton->setToolTip(
        DigiDollarSendWidget::tr("Available only before a user payment signature exists; keeps the exact reserved inputs"));
    m_recoverSessionButton = new QPushButton(DigiDollarSendWidget::tr("Recover safely to this wallet"), m_paymasterSecondaryActions);
    m_recoverSessionButton->setObjectName("recoverPaymasterSession");
    m_recoverSessionButton->setToolTip(
        DigiDollarSendWidget::tr("Creates one durable same-input conflict transaction; the original payment may still confirm first"));
    m_abandonSessionButton = new QPushButton(
        DigiDollarSendWidget::tr("Cancel unsigned transfer and release $DD"), m_paymasterSecondaryActions);
    m_abandonSessionButton->setObjectName("abandonUnsignedPaymasterSession");
    m_abandonSessionButton->setToolTip(DigiDollarSendWidget::tr(
        "Available only while Core can prove that no transaction signature or final transaction exists"));
    m_cancelQuoteButton = new QPushButton(DigiDollarSendWidget::tr("Stop automatic checks"), m_paymasterSecondaryActions);
    m_cancelQuoteButton->setObjectName("stopPaymasterAutomaticChecks");
    m_cancelQuoteButton->setToolTip(DigiDollarSendWidget::tr("Stop polling without releasing any durable reservation"));
    secondary_layout->addWidget(m_retrySessionButton, 0, 0);
    secondary_layout->addWidget(m_fallbackSessionButton, 0, 1);
    secondary_layout->addWidget(m_recoverSessionButton, 1, 0, 1, 2);
    secondary_layout->addWidget(m_abandonSessionButton, 2, 0, 1, 2);
    secondary_layout->addWidget(m_cancelQuoteButton, 3, 0, 1, 2);
    session_layout->addWidget(m_paymasterSecondaryActions, 7, 0, 1, 2);

    m_paymasterTechnicalDetails = new QFrame(m_paymasterSessionFrame);
    m_paymasterTechnicalDetails->setObjectName("paymasterSessionTechnicalDetails");
    auto* technical_layout = new QGridLayout(m_paymasterTechnicalDetails);
    technical_layout->setContentsMargins(0, 4, 0, 0);
    m_paymasterStateValue = new QLabel(DigiDollarSendWidget::tr("No active session"), m_paymasterTechnicalDetails);
    m_paymasterStateValue->setObjectName("paymasterSessionState");
    m_paymasterStateValue->setTextFormat(Qt::PlainText);
    m_paymasterStateValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_paymasterExpiryValue = new QLabel(DigiDollarSendWidget::tr("—"), m_paymasterTechnicalDetails);
    m_paymasterExpiryValue->setTextFormat(Qt::PlainText);
    technical_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Core state:"), m_paymasterTechnicalDetails), 0, 0);
    technical_layout->addWidget(m_paymasterStateValue, 0, 1);
    technical_layout->addWidget(new QLabel(DigiDollarSendWidget::tr("Valid until:"), m_paymasterTechnicalDetails), 1, 0);
    technical_layout->addWidget(m_paymasterExpiryValue, 1, 1);
    session_layout->addWidget(m_paymasterTechnicalDetails, 8, 0, 1, 2);

    m_feeLayout->addWidget(m_paymasterSessionFrame, 10, 0, 1, 2);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_paymasterNotice = new QLabel(this);
    m_paymasterNotice->setObjectName("paymasterTransferNotice");
    m_paymasterNotice->setTextFormat(Qt::PlainText);
    m_paymasterNotice->setWordWrap(true);
    DigiDollarStatus::SetBanner(m_paymasterNotice, DigiDollarStatus::Kind::INFO);
    m_paymasterNotice->hide();
    layout->addWidget(m_paymasterNotice);
    layout->addWidget(m_feeFrame);

    m_advancedPaymasterFrame->hide();
    m_paymasterSessionFrame->hide();
    m_paymasterSecondaryActions->hide();
    m_paymasterTechnicalDetails->hide();
    onFeeModeChanged();
}

bool PaymasterSendWidget::paymasterOnlySelected() const
{
    return m_feeModeCombo->currentData().toString() == QStringLiteral("paymaster");
}

bool PaymasterSendWidget::paymasterModeSelected() const
{
    return feeMode() != QStringLiteral("dgb");
}

QString PaymasterSendWidget::feeMode() const
{
    return m_feeModeCombo ? m_feeModeCombo->currentData().toString() : QStringLiteral("dgb");
}

void PaymasterSendWidget::onFeeModeChanged()
{
    const QString mode = feeMode();
    if (m_dgbFeeRadio && mode == QStringLiteral("dgb")) m_dgbFeeRadio->setChecked(true);
    if (m_autoFeeRadio && mode == QStringLiteral("auto")) m_autoFeeRadio->setChecked(true);
    if (m_paymasterFeeRadio && mode == QStringLiteral("paymaster")) m_paymasterFeeRadio->setChecked(true);
    const bool paymaster_enabled = paymasterModeSelected();
    if (!paymaster_enabled && m_subtractPaymasterFeeCheck) {
        m_subtractPaymasterFeeCheck->setChecked(false);
        m_sendAllSpendableDD = false;
    }
    if (m_subtractPaymasterFeeCheck) {
        m_subtractPaymasterFeeCheck->setVisible(paymaster_enabled);
    }
    m_form.updatePaymasterFormMode(paymaster_enabled);
    if (m_advancedPaymasterButton) m_advancedPaymasterButton->setVisible(paymaster_enabled);
    if (m_clientSafetyFrame) m_clientSafetyFrame->setVisible(paymaster_enabled);
    if (m_advancedPaymasterFrame) {
        m_advancedPaymasterFrame->setVisible(
            paymaster_enabled && m_advancedPaymasterButton->isChecked());
    }
    updateClientSafetyDisplay();
    updateFeeDisplay();
    updatePaymasterFocusMode();
    m_form.updateSendButton();
}

QString PaymasterSendWidget::friendlyPaymasterSessionStatus() const
{
    if (m_paymasterBusy) {
        return DigiDollarSendWidget::tr("Core is securely checking the current Paymaster transfer. Please wait.");
    }
    if (m_paymasterConfirmationState == QStringLiteral("payment_confirmed")) {
        return DigiDollarSendWidget::tr("Transfer confirmed. The recipient has received the DigiDollar payment.");
    }
    if (m_paymasterConfirmationState == QStringLiteral("recovery_confirmed")) {
        return DigiDollarSendWidget::tr("Recovery confirmed. The original recipient payment was canceled.");
    }
    if (m_paymasterSessionState == QStringLiteral("CONFIRMED") ||
        m_paymasterSessionState == QStringLiteral("CANCELED_SAFE")) {
        return DigiDollarSendWidget::tr("This session is closed. A current payment or recovery confirmation is unavailable.");
    }
    if (m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE")) {
        if (m_paymasterOfferReadyForReview) {
            return DigiDollarSendWidget::tr("The exact offer is ready. Review the provider, recipient amount and fee before authorizing payment.");
        }
        if (canContinueActivePaymasterSend()) {
            return DigiDollarSendWidget::tr("Waiting for the provider's exact offer. Preparation continues automatically; no refresh or payment approval is needed yet.");
        }
        return DigiDollarSendWidget::tr("Preparation is paused. Continue preparation to load and verify the offer. No payment has been authorized.");
    }
    if (m_paymasterRecoveryActive && m_paymasterActiveRecoveryParams.isArray()) {
        return m_paymasterRecoveryAuthorizationCommitment.isEmpty()
            ? DigiDollarSendWidget::tr("Preparing alternative recovery automatically. You will review the exact wallet returns and fees before a recovery transaction is signed.")
            : DigiDollarSendWidget::tr("Completing the exact recovery you authorized. No additional authorization or offer search is needed.");
    }
    if (m_paymasterSessionState == QStringLiteral("PENDING_PROVIDER")) {
        return m_paymasterArtifact == QStringLiteral("user_psbt")
            ? DigiDollarSendWidget::tr("Your authorized transaction is waiting for the provider. Keep this session protected until it completes or is safely recovered.")
            : DigiDollarSendWidget::tr("Waiting for the selected provider. No additional action is normally required.");
    }
    if (m_paymasterSessionState == QStringLiteral("MEMPOOL") ||
        m_paymasterSessionState == QStringLiteral("STEMPOOL")) {
        return DigiDollarSendWidget::tr("The transaction was submitted and is waiting for blockchain confirmation.");
    }
    if (m_paymasterUnsignedClosed) {
        return DigiDollarSendWidget::tr("This unsigned request is closed. No payment was sent and no funds remain reserved for it. You can start a new transfer.");
    }
    if (m_paymasterSessionState == QStringLiteral("FAILED")) {
        if (m_paymasterArtifact == QStringLiteral("none")) {
            return DigiDollarSendWidget::tr("This attempt failed before a transaction was signed. Check the protected wallet state; Core will offer only cancellation or provider fallback actions that are still provably safe.");
        }
        if (!m_paymasterArtifact.isEmpty()) {
            return DigiDollarSendWidget::tr("The transfer needs recovery attention. A transaction authorization may already exist, so the reserved DigiDollar remains protected.");
        }
        return DigiDollarSendWidget::tr("The transfer needs attention. Check the exact session before choosing a recovery action.");
    }
    if (m_paymasterSessionState == QStringLiteral("CONFLICTED")) {
        return DigiDollarSendWidget::tr("A transaction conflict was detected. Check the protected session before taking further action.");
    }
    if (m_paymasterSessionState == QStringLiteral("CREATED") &&
        m_paymasterSessionPersisted && m_paymasterAddress.isEmpty()) {
        return DigiDollarSendWidget::tr("Preparation stopped before payment details were saved. Cancel this unsigned request under More options, then prepare the payment again.");
    }
    if (m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK")) {
        return DigiDollarSendWidget::tr("Wallet unlock is required to continue. Use Unlock and continue; a new offer search is not needed.");
    }
    if (m_paymasterSessionState == QStringLiteral("CREATED") ||
        m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED")) {
        if (!canContinueActivePaymasterSend()) {
            return DigiDollarSendWidget::tr("Preparation is paused. Check the current status to see which continuation is safe; refreshing the offer list will not advance this request.");
        }
        if (m_paymasterTransportState == QStringLiteral("waiting_capacity")) {
            return DigiDollarSendWidget::tr("Waiting for a free payment connection. Preparation continues automatically; no new offer search is needed.");
        }
        if (m_paymasterTransportState == QStringLiteral("connecting") ||
            m_paymasterTransportState == QStringLiteral("handshaking")) {
            return DigiDollarSendWidget::tr("Provider found. Establishing the secure connection… This continues automatically; no refresh is needed.");
        }
        return DigiDollarSendWidget::tr("Requesting and verifying the exact provider offer… This continues automatically; no refresh is needed. No service fee has been authorized.");
    }
    return DigiDollarSendWidget::tr("The Paymaster transfer is protected. Check its current wallet state before continuing.");
}

void PaymasterSendWidget::updatePaymasterFocusMode()
{
    // A durable session owns the recipient, amount, inputs, and authorization
    // choices shown here. Hide the editable compose controls until Core reports
    // a terminal/cancelled state so the UI cannot suggest that editing fields
    // mutates an already persisted authorization.
    if (!m_feeChoicesFrame || !m_paymasterSessionFrame) return;
    const bool focus = !m_paymasterRequestId.isEmpty();
    // A closed session permits composing another transfer, independently of
    // whether retained artifacts can still prove its outcome.
    const bool completed = m_paymasterUnsignedClosed ||
        m_paymasterSessionState == QStringLiteral("CONFIRMED") ||
        m_paymasterSessionState == QStringLiteral("CANCELED_SAFE") ||
        m_paymasterConfirmationState == QStringLiteral("payment_confirmed") ||
        m_paymasterConfirmationState == QStringLiteral("recovery_confirmed");
    const auto core_allows = [this](const QString& action) {
        return m_paymasterAllowedActionsKnown &&
            m_paymasterAllowedActions.contains(action);
    };

    m_feeHeading->setVisible(!focus);
    m_feeIntroduction->setVisible(!focus);
    m_feeChoicesFrame->setVisible(!focus);
    m_feeSummary->setVisible(!focus);
    m_subtractPaymasterFeeCheck->setVisible(!focus && paymasterModeSelected());
    m_advancedPaymasterButton->setVisible(!focus && paymasterModeSelected());
    m_clientSafetyFrame->setVisible(!focus && paymasterModeSelected());
    m_advancedPaymasterFrame->setVisible(
        !focus && paymasterModeSelected() && m_advancedPaymasterButton->isChecked());
    m_paymasterSessionFrame->setVisible(focus);

    m_form.setPaymasterFormFocus(focus);

    updateOfferCheckControls();
    if (!focus) return;

    m_paymasterTransferValue->setText(
        m_paymasterAddress.isEmpty()
            ? DigiDollarSendWidget::tr("Recipient and amount are not yet available; Core only persisted the initial request")
            : DigiDollarSendWidget::tr("%1 to %2").arg(m_form.formatDDAmount(m_paymasterAmount),
                                  m_paymasterAddress));
    m_paymasterNextStepValue->setText(friendlyPaymasterSessionStatus());

    const bool artifact_none = m_paymasterArtifact == QStringLiteral("none");
    const bool signed_artifact = m_paymasterArtifact == QStringLiteral("user_psbt") ||
                                 m_paymasterArtifact == QStringLiteral("final_transaction") ||
                                 m_paymasterArtifact == QStringLiteral("alternative_recovery");
    const bool exact_offer_reviewable =
        m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") &&
        m_paymasterOfferReadyForReview && artifact_none && core_allows(QStringLiteral("resume"));
    const bool unlock_and_resume =
        m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK") &&
        artifact_none && core_allows(QStringLiteral("resume"));
    const bool preparation_resumable =
        (m_paymasterSessionState == QStringLiteral("CREATED") ||
         m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
         (m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") && !m_paymasterOfferReadyForReview)) &&
        artifact_none && core_allows(QStringLiteral("resume"));
    const bool fallback_state =
        m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") ||
        m_paymasterSessionState == QStringLiteral("AUTHORIZED");
    const bool fallback_attempt =
        m_paymasterAttemptState == QStringLiteral("CANDIDATE") ||
        m_paymasterAttemptState == QStringLiteral("QUOTED") ||
        m_paymasterAttemptState == QStringLiteral("QUOTE_EXPIRED") ||
        (m_paymasterAttemptState == QStringLiteral("REJECTED") &&
         m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED"));
    const bool safe_fallback = m_paymasterSessionPersisted && artifact_none &&
                               fallback_state && fallback_attempt &&
                               core_allows(QStringLiteral("fallback"));
    const bool signed_state_consistent =
        m_paymasterSessionState == QStringLiteral("AUTHORIZED") ||
        m_paymasterSessionState == QStringLiteral("PENDING_PROVIDER") ||
        m_paymasterSessionState == QStringLiteral("STEMPOOL") ||
        m_paymasterSessionState == QStringLiteral("MEMPOOL") ||
        m_paymasterSessionState == QStringLiteral("FAILED") ||
        m_paymasterSessionState == QStringLiteral("CONFLICTED");
    const bool recovery_not_expired =
        m_paymasterRecoveryExpiresAt <= 0 ||
        m_paymasterRecoveryExpiresAt > QDateTime::currentSecsSinceEpoch();
    const bool safe_recovery = m_paymasterSessionPersisted && signed_artifact &&
                               signed_state_consistent && !completed &&
                               recovery_not_expired &&
                               (core_allows(QStringLiteral("cancel_to_self")) ||
                                core_allows(QStringLiteral("recover")));
    if (completed) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::NEW_TRANSFER;
        m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Start a new transfer"));
    } else if (!m_paymasterClosureReason.isEmpty()) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::REFRESH;
        m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Check current status"));
    } else if (exact_offer_reviewable) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::REVIEW_OFFER;
        m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Review exact offer"));
    } else if (unlock_and_resume || preparation_resumable) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::RESUME;
        m_paymasterPrimaryButton->setText(
            unlock_and_resume ? DigiDollarSendWidget::tr("Unlock and continue")
                              : DigiDollarSendWidget::tr("Resume preparation"));
    } else if (safe_fallback) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::FALLBACK;
        m_paymasterPrimaryButton->setText(
            m_paymasterAttemptState == QStringLiteral("REJECTED")
                ? DigiDollarSendWidget::tr("Check another offer")
                : DigiDollarSendWidget::tr("Try another Paymaster"));
    } else if ((m_paymasterSessionState == QStringLiteral("FAILED") ||
                m_paymasterSessionState == QStringLiteral("CONFLICTED")) && safe_recovery) {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::RECOVER;
        m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Start safe recovery"));
    } else {
        m_paymasterPrimaryAction = PaymasterPrimaryAction::REFRESH;
        m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Check current status"));
    }

    const bool have_persisted = m_paymasterSessionPersisted;
    const bool abandon_state =
        m_paymasterSessionState == QStringLiteral("CREATED") ||
        m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK") ||
        m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE") ||
        m_paymasterSessionState == QStringLiteral("FAILED");
    const bool unsigned_state_consistent = abandon_state || fallback_state;
    const bool consistent_artifact_state =
        (artifact_none && unsigned_state_consistent) ||
        (signed_artifact && signed_state_consistent);
    const bool can_retry = have_persisted && signed_artifact &&
        signed_state_consistent && !completed &&
        core_allows(QStringLiteral("retry_same"));
    const bool no_pending_provider_action =
        m_paymasterPendingPhase.isEmpty() || m_paymasterPendingPhase == QStringLiteral("NONE");
    const bool can_fallback = safe_fallback;
    const bool can_abandon = have_persisted && artifact_none && abandon_state &&
                             no_pending_provider_action && !completed &&
                             core_allows(QStringLiteral("abandon_unsigned"));
    const bool can_recover = safe_recovery;
    const bool can_stop = m_paymasterPollTimer->isActive() &&
                          consistent_artifact_state && !completed;
    m_retrySessionButton->setVisible(can_retry);
    m_fallbackSessionButton->setVisible(
        can_fallback && m_paymasterPrimaryAction != PaymasterPrimaryAction::FALLBACK);
    m_abandonSessionButton->setVisible(can_abandon);
    m_recoverSessionButton->setVisible(
        can_recover && m_paymasterPrimaryAction != PaymasterPrimaryAction::RECOVER);
    m_cancelQuoteButton->setVisible(can_stop);
    const bool have_more = can_retry || can_fallback || can_abandon || can_recover || can_stop;
    m_paymasterMoreButton->setVisible(have_more);
    if (!have_more) {
        m_paymasterMoreButton->setChecked(false);
        m_paymasterSecondaryActions->hide();
    }
    bool primary_allowed{false};
    switch (m_paymasterPrimaryAction) {
    case PaymasterPrimaryAction::REFRESH:
        primary_allowed = !m_paymasterAllowedActionsKnown ||
            core_allows(QStringLiteral("refresh"));
        break;
    case PaymasterPrimaryAction::RESUME:
    case PaymasterPrimaryAction::REVIEW_OFFER:
        primary_allowed = core_allows(QStringLiteral("resume"));
        break;
    case PaymasterPrimaryAction::FALLBACK:
        primary_allowed = can_fallback;
        break;
    case PaymasterPrimaryAction::RECOVER:
        primary_allowed = can_recover;
        break;
    case PaymasterPrimaryAction::NEW_TRANSFER:
        primary_allowed = completed;
        break;
    }
    const bool action_needs_wallet =
        m_paymasterPrimaryAction != PaymasterPrimaryAction::NEW_TRANSFER;
    const bool live_recovery = m_paymasterRecoveryActive && m_paymasterActiveRecoveryParams.isArray();
    const bool live_preparation = canContinueActivePaymasterSend() && m_paymasterAuthorizationCommitment.isEmpty();
    if (live_preparation) {
        m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Preparing payment…"));
    }
    if (live_recovery) m_paymasterPrimaryButton->setText(DigiDollarSendWidget::tr("Preparing recovery…"));
    m_paymasterPrimaryButton->setEnabled(
        !m_paymasterBusy && !live_preparation && !live_recovery && primary_allowed &&
        (!action_needs_wallet || m_walletModel));
    applyPaymasterPrivacy();
}

void PaymasterSendWidget::onPaymasterPrimaryAction()
{
    switch (m_paymasterPrimaryAction) {
    case PaymasterPrimaryAction::REVIEW_OFFER:
    case PaymasterPrimaryAction::RESUME:
        refreshPaymasterSessionForAction(
            QStringLiteral("resume"),
            [guard = QPointer<PaymasterSendWidget>(this)] {
                if (!guard) return;
                guard->executePaymasterTransfer(
                    guard->m_paymasterAddress,
                    guard->m_paymasterAmountCents,
                    guard->m_paymasterSessionState ==
                        QStringLiteral("AWAITING_WALLET_UNLOCK"), /*retry_transport=*/true);
            });
        break;
    case PaymasterPrimaryAction::FALLBACK:
        fallbackPaymasterSession();
        break;
    case PaymasterPrimaryAction::RECOVER:
        recoverPaymasterSessionToSelf();
        break;
    case PaymasterPrimaryAction::NEW_TRANSFER:
        stopPaymasterPolling();
        m_paymasterRequestId.clear();
        m_paymasterSessionId.clear();
        m_paymasterSessionState.clear();
        m_paymasterUnsignedClosed = false;
        m_paymasterOfferReadyForReview = false;
        m_paymasterAttemptState.clear();
        m_paymasterArtifact.clear();
        m_paymasterPendingPhase.clear();
        m_paymasterBroadcastState.clear();
        m_paymasterConfirmationState.clear();
        m_paymasterTransactionId.clear();
        m_paymasterRecoveryTransactionId.clear();
        m_paymasterResultStatus.clear();
        m_paymasterResultSequence = -1;
        m_paymasterRecoveryExpiresAt = -1;
        m_paymasterTerminalNoticeShown = false;
        m_paymasterSessionPersisted = false;
        m_paymasterAddress.clear();
        m_paymasterAmount = 0.0;
        m_paymasterAmountCents = 0;
        m_paymasterInitialAvailableBalance = 0.0;
        m_sendAllSpendableDD = false;
        if (m_subtractPaymasterFeeCheck) m_subtractPaymasterFeeCheck->setChecked(false);
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterSessionPrivacy.clear();
        m_paymasterRecoveryAuthorizationCommitment.clear();
        m_paymasterRecoveryMaximumServiceFeeCents = 0;
        m_paymasterRecoveryActive = false;
        m_paymasterConfirmationGuard.Reset();
        m_paymasterRecoveryConfirmationGuard.Reset();
        m_paymasterMoreButton->setChecked(false);
        m_paymasterTechnicalButton->setChecked(false);
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr("No active session"));
        m_paymasterIdentityValue->setText(DigiDollarSendWidget::tr("Provider not selected yet"));
        m_paymasterCostValue->setText(DigiDollarSendWidget::tr("No service fee authorized yet"));
        m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
        m_form.clearInputFields();
        updatePaymasterFocusMode();
        m_form.updateSendButton();
        break;
    case PaymasterPrimaryAction::REFRESH:
        refreshPaymasterSessionState();
        break;
    }
}

void PaymasterSendWidget::showPaymasterExplanation()
{
    QMessageBox::information(
        this, DigiDollarSendWidget::tr("What is a Paymaster?"),
        DigiDollarSendWidget::tr("A Paymaster is an independent provider that supplies the DGB needed for "
           "the DigiByte network fee.\n\n"
           "• User-paid offer: you pay an additional service fee in $DD.\n"
           "• Sponsored offer: the provider pays the network fee and charges no $DD service fee.\n\n"
           "The provider never receives your wallet keys. Your wallet reconstructs and checks "
           "the complete transaction locally, and you see the exact provider and service fee "
           "again before your wallet signs.\n\n"
           "A provider necessarily receives the payment details needed to participate. "
           "Paymaster transport improves pseudonymity but does not provide complete anonymity."));
}

void PaymasterSendWidget::configureClientSafetyPolicy()
{
    if (!m_walletModel) {
        m_form.showWarning(DigiDollarSendWidget::tr("Paymaster service-fee limits"),
                    DigiDollarSendWidget::tr("Select an available wallet before configuring its limits."));
        return;
    }

    if (m_paymasterBusy || m_privacy) return;
    const uint64_t wallet_generation = m_paymasterWalletGeneration;
    QDialog dialog(this);
    // This is a DigiDollar-owned dialog, so give the shared theme a stable
    // selector instead of inheriting the application's blue default dialog
    // palette. The selector also scopes visible spin-box arrows to this modal.
    dialog.setObjectName(QStringLiteral("digiDollarClientSafetyDialog"));
    dialog.setWindowTitle(DigiDollarSendWidget::tr("Set Paymaster service-fee limits"));
    dialog.setMinimumWidth(520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(DigiDollarSendWidget::tr(
        "These wallet-local limits protect your $DD. A Paymaster offer can never charge more "
        "than the lower of these limits and the limit selected for the transfer. Limits are "
        "not shared with providers. Both values must be positive."), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* form = new QFormLayout();
    auto* per_transfer = new NoWheelSpinBox(&dialog);
    per_transfer->setObjectName("clientSafetyPerTransferDialog");
    per_transfer->setRange(1, 10000000);
    per_transfer->setValue(static_cast<int>(m_clientSafetyMaximumPerTransaction));
    per_transfer->setSuffix(DigiDollarSendWidget::tr(" cents"));
    auto* per_day = new NoWheelSpinBox(&dialog);
    per_day->setObjectName("clientSafetyPerDayDialog");
    per_day->setRange(1, 10000000);
    per_day->setValue(static_cast<int>(m_clientSafetyMaximumPerDay));
    per_day->setSuffix(DigiDollarSendWidget::tr(" cents"));
    form->addRow(DigiDollarSendWidget::tr("Maximum per transfer:"), per_transfer);
    form->addRow(DigiDollarSendWidget::tr("Maximum in a rolling day:"), per_day);
    layout->addLayout(form);

    auto* recommendation = new QLabel(
        DigiDollarSendWidget::tr("Recommended starting values: 1.00 $DD (100 cents) per transfer and "
           "10.00 $DD (1,000 cents) per rolling day."), &dialog);
    recommendation->setWordWrap(true);
    layout->addWidget(recommendation);
    auto* restore = new QPushButton(DigiDollarSendWidget::tr("Restore recommended limits"), &dialog);
    restore->setObjectName(QStringLiteral("clientSafetyRestoreDefaultsButton"));
    connect(restore, &QPushButton::clicked, &dialog, [per_transfer, per_day] {
        per_transfer->setValue(100);
        per_day->setValue(1000);
    });
    layout->addWidget(restore);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("clientSafetyDialogButtons"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    setPaymasterBusy(true);
    const auto answer = dialog.exec();
    if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel) return;
    setPaymasterBusy(false);
    if (answer != QDialog::Accepted || m_privacy) return;
    if (per_day->value() < per_transfer->value()) {
        m_form.showWarning(DigiDollarSendWidget::tr("Invalid Paymaster service-fee limits"),
                    DigiDollarSendWidget::tr("The rolling-day limit must be at least as large as the per-transfer limit."));
        return;
    }

    // Invalidate a status read that may still contain the policy from before
    // this edit. The exact write acknowledgement below remains authoritative
    // until its own follow-up refresh completes.
    ++m_clientSafetyRefreshGeneration;
    UniValue policy{UniValue::VOBJ};
    const qint64 requested_per_transaction = per_transfer->value();
    const qint64 requested_per_day = per_day->value();
    policy.pushKV("maximum_service_fee_per_transaction_cents",
                  requested_per_transaction);
    policy.pushKV("maximum_service_fee_per_day_cents", requested_per_day);
    UniValue params{UniValue::VARR};
    params.push_back(std::move(policy));
    m_configureClientSafetyButton->setEnabled(false);
    m_clientSafetyStatus->setText(DigiDollarSendWidget::tr("Saving wallet-local Paymaster service-fee limits…"));
    QPointer<PaymasterSendWidget> guard{this};
    WalletModel* requested_model = m_walletModel;
    executePaymasterRpcAsync("setpaymasterclientsafetypolicy", std::move(params),
        [guard, requested_model, requested_per_transaction,
         requested_per_day](UniValue result, QString error) {
            if (!guard || guard->m_walletModel != requested_model) return;
            guard->m_configureClientSafetyButton->setEnabled(true);
            if (!error.isEmpty()) {
                guard->m_clientSafetyStatusKnown = true;
                guard->m_clientSafetyConfigured = false;
                guard->m_clientSafetyError = error;
                guard->updateClientSafetyDisplay();
                guard->m_form.showWarning(DigiDollarSendWidget::tr("Paymaster service-fee limits"), error);
                return;
            }
            qint64 persisted_per_transaction{0};
            qint64 persisted_per_day{0};
            if (!DecodeClientSafetyPolicy(
                    result, persisted_per_transaction, persisted_per_day) ||
                persisted_per_transaction != requested_per_transaction ||
                persisted_per_day != requested_per_day) {
                const QString malformed = DigiDollarSendWidget::tr(
                    "Core did not confirm the exact wallet-local Paymaster service-fee limits. The controls remain fail-closed until status is refreshed.");
                guard->m_clientSafetyStatusKnown = true;
                guard->m_clientSafetyConfigured = false;
                guard->m_clientSafetyError = malformed;
                guard->updateClientSafetyDisplay();
                guard->m_form.showWarning(
                    DigiDollarSendWidget::tr("Paymaster service-fee limits"), malformed);
                return;
            }
            guard->m_clientSafetyMaximumPerTransaction =
                persisted_per_transaction;
            guard->m_clientSafetyMaximumPerDay = persisted_per_day;
            guard->m_clientSafetyStatusKnown = true;
            guard->m_clientSafetyConfigured = true;
            guard->m_clientSafetyError.clear();
            if (guard->m_feeCapSpin->value() > guard->m_clientSafetyMaximumPerTransaction) {
                guard->m_feeCapSpin->setValue(
                    static_cast<int>(guard->m_clientSafetyMaximumPerTransaction));
            }
            guard->refreshClientSafetyStatus();
        });
}

void PaymasterSendWidget::refreshClientSafetyStatus()
{
    if (!m_walletModel) {
        m_clientSafetyStatusKnown = false;
        m_clientSafetyConfigured = false;
        updateClientSafetyDisplay();
        return;
    }
    m_clientSafetyStatusKnown = false;
    m_clientSafetyError.clear();
    updateClientSafetyDisplay();
    QPointer<PaymasterSendWidget> guard{this};
    WalletModel* requested_model = m_walletModel;
    const uint64_t refresh_generation = ++m_clientSafetyRefreshGeneration;
    executePaymasterRpcAsync("getpaymasterclientsafetystatus", UniValue{UniValue::VARR},
        [guard, requested_model, refresh_generation](UniValue result,
                                                      QString error) {
            if (!guard || guard->m_walletModel != requested_model ||
                guard->m_clientSafetyRefreshGeneration !=
                    refresh_generation) {
                return;
            }
            bool configured{false};
            qint64 maximum_per_transaction{0};
            qint64 maximum_per_day{0};
            qint64 active_reservations{0};
            qint64 reserved_cents{0};
            qint64 spent_cents{0};
            qint64 available_cents{0};
            if (error.isEmpty() &&
                !DecodeClientSafetyStatus(
                    result, configured, maximum_per_transaction,
                    maximum_per_day, active_reservations, reserved_cents,
                    spent_cents, available_cents)) {
                error = DigiDollarSendWidget::tr(
                    "Core returned an incomplete Paymaster client-safety status. No provider fee is authorized until a complete refresh succeeds.");
            }
            guard->m_clientSafetyStatusKnown = true;
            guard->m_clientSafetyConfigured = error.isEmpty() && configured;
            guard->m_clientSafetyError = error;
            if (guard->m_clientSafetyConfigured) {
                guard->m_clientSafetyMaximumPerTransaction =
                    maximum_per_transaction;
                guard->m_clientSafetyMaximumPerDay = maximum_per_day;
                guard->m_clientSafetyActiveReservations = active_reservations;
                guard->m_clientSafetyReservedCents = reserved_cents;
                guard->m_clientSafetySpentTodayCents = spent_cents;
                guard->m_clientSafetyAvailableTodayCents = available_cents;
                if (guard->m_feeCapSpin->value() > guard->m_clientSafetyMaximumPerTransaction) {
                    guard->m_feeCapSpin->setValue(
                        static_cast<int>(guard->m_clientSafetyMaximumPerTransaction));
                }
            }
            guard->updateClientSafetyDisplay();
            guard->updateFeeDisplay();
            guard->m_form.updateSendButton();
        });
}

void PaymasterSendWidget::updateClientSafetyDisplay()
{
    if (!m_clientSafetyStatus || !m_configureClientSafetyButton) return;
    m_configureClientSafetyButton->setEnabled(m_walletModel && !m_paymasterBusy);
    m_clientSafetyStatus->setToolTip(QString());
    if (!m_walletModel) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::INFO);
        m_clientSafetyStatus->setText(
            DigiDollarSendWidget::tr("ℹ Select a wallet to check its Paymaster service-fee limits."));
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(DigiDollarSendWidget::tr("No wallet selected."));
    } else if (!m_clientSafetyStatusKnown) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::WAITING);
        m_clientSafetyStatus->setText(
            DigiDollarSendWidget::tr("… Checking this wallet's Paymaster service-fee limits…"));
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(DigiDollarSendWidget::tr("Loading wallet protection details…"));
    } else if (!m_clientSafetyError.isEmpty()) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::ERR);
        m_clientSafetyStatus->setText(DigiDollarSendWidget::tr(
            "✕ Paymaster unavailable · wallet protection could not be read. "
            "Open Advanced Paymaster settings for technical details."));
        m_clientSafetyStatus->setToolTip(m_clientSafetyError);
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(m_clientSafetyError);
    } else if (!m_clientSafetyConfigured) {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::ACTION);
        m_clientSafetyStatus->setText(DigiDollarSendWidget::tr(
            "! Action required · Set wallet-local Paymaster service-fee limits before sending. "
            "They cap how much $DD a provider may charge."));
        if (m_clientSafetyDetails) m_clientSafetyDetails->setText(DigiDollarSendWidget::tr("No positive wallet-local Paymaster limits are saved."));
    } else {
        DigiDollarStatus::SetBanner(m_clientSafetyFrame, DigiDollarStatus::Kind::SUCCESS);
        m_clientSafetyStatus->setText(DigiDollarSendWidget::tr(
            "✓ Service-fee limits: %1 per payment · %2 per rolling 24 h")
            .arg(formatCents(m_clientSafetyMaximumPerTransaction),
                 formatCents(m_clientSafetyMaximumPerDay)));
        m_clientSafetyStatus->setToolTip(DigiDollarSendWidget::tr(
            "Available today: %1\nReserved: %2 in %3 session(s)\nSpent today: %4\n"
            "The transfer-specific limit can only reduce these wallet limits.")
            .arg(formatCents(m_clientSafetyAvailableTodayCents),
                 formatCents(m_clientSafetyReservedCents))
            .arg(m_clientSafetyActiveReservations)
            .arg(formatCents(m_clientSafetySpentTodayCents)));
        if (m_clientSafetyDetails) {
            m_clientSafetyDetails->setText(DigiDollarSendWidget::tr(
                "Available today: %1 · reserved: %2 in %3 session(s) · spent today: %4. "
                "The transfer-specific limit below can only reduce these wallet limits.")
                .arg(formatCents(m_clientSafetyAvailableTodayCents),
                     formatCents(m_clientSafetyReservedCents))
                .arg(m_clientSafetyActiveReservations)
                .arg(formatCents(m_clientSafetySpentTodayCents)));
        }
    }
    m_clientSafetyFrame->setAccessibleDescription(m_clientSafetyStatus->text());
}

void PaymasterSendWidget::setPaymasterBusy(bool busy)
{
    if (busy && m_paymasterPollTimer) {
        // A mutation or user-confirmation sequence owns the session while it
        // is busy. Pause the single-shot poll without resetting its backoff;
        // the result handler will explicitly reschedule when appropriate.
        m_paymasterPollTimer->stop();
    }
    m_paymasterBusy = busy;
    m_form.updateSendButton();
    const bool idle_with_wallet = !busy && m_walletModel;
    updateOfferCheckControls();
    updateClientSafetyDisplay();
    const bool have_persisted_session =
        !m_paymasterRequestId.isEmpty() && m_paymasterSessionPersisted;
    m_retrySessionButton->setEnabled(idle_with_wallet &&
                                     have_persisted_session);
    m_fallbackSessionButton->setEnabled(idle_with_wallet &&
                                        have_persisted_session);
    m_recoverSessionButton->setEnabled(idle_with_wallet &&
                                       have_persisted_session);
    m_abandonSessionButton->setEnabled(idle_with_wallet &&
                                       have_persisted_session);
    m_cancelQuoteButton->setEnabled(idle_with_wallet &&
                                    have_persisted_session);
    m_paymasterPrimaryButton->setEnabled(idle_with_wallet);
    updatePaymasterFocusMode();
}

void PaymasterSendWidget::reportPaymasterOperationNotStarted(
    const QString& reason)
{
    if (m_paymasterStateValue) {
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Paymaster operation not started: %1").arg(reason));
    }
    applyPaymasterPrivacy();
}

void PaymasterSendWidget::discoverPersistedPaymasterSessions(bool user_requested)
{
    if (!m_walletModel || !m_paymasterRequestId.isEmpty() || m_sessionDiscoveryPending) return;
    m_sessionDiscoveryReady = false;
    m_sessionDiscoveryPending = true;
    m_persistedPaymasterSessions->clear();
    m_persistedPaymasterSessions->hide();
    m_persistedPaymasterSessionsFrame->show();
    m_persistedSessionsStatus->setText(DigiDollarSendWidget::tr("Checking saved Paymaster transfers…"));
    m_sessionDiscoveryError->clear();
    m_sessionDiscoveryError->hide();
    m_sessionDiscoveryProgress->show();
    DigiDollarStatus::SetBanner(m_persistedPaymasterSessionsFrame, DigiDollarStatus::Kind::WAITING);
    m_loadPersistedPaymasterSessionButton->setText(DigiDollarSendWidget::tr("Loading transfers…"));
    m_loadPersistedPaymasterSessionButton->setEnabled(false);
    m_form.updateSendButton();

    UniValue options{UniValue::VOBJ};
    options.pushKV("active_only", true);
    // The RPC caps one page at 100. Refuse to display a partial active-session
    // set: a truncated chooser could make an older transfer look absent.
    options.pushKV("limit", 100);
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    executePaymasterRpcAsync(
        "listdigidollarsendsessions", std::move(params),
        [guard = QPointer<PaymasterSendWidget>(this), user_requested](
            UniValue result, QString error) {
            if (!guard) return;
            guard->m_sessionDiscoveryPending = false;
            guard->m_sessionDiscoveryProgress->hide();
            const auto failed = [&](const QString& detail, bool reservation_conflict = false) {
                guard->m_loadPersistedPaymasterSessionButton->setText(DigiDollarSendWidget::tr("Retry loading transfers"));
                guard->m_persistedSessionsStatus->setText(DigiDollarSendWidget::tr("Saved transfers could not be read completely. New Paymaster payments remain paused."));
                const QString guidance = reservation_conflict
                    ? DigiDollarSendWidget::tr("Core found a conflict between a saved transfer and an input reservation. Retrying the same data will not fix it. Keep the saved transfers for diagnosis; do not manually release their inputs.")
                    : DigiDollarSendWidget::tr("Loading failed. Retry once the cause below is resolved; no payment was started by this check.");
                const QString message = guidance + QStringLiteral("\n\n") + DigiDollarSendWidget::tr("Technical details: %1").arg(detail.left(2048));
                guard->m_sessionDiscoveryError->setText(message);
                guard->m_sessionDiscoveryError->show();
                DigiDollarStatus::SetBanner(guard->m_persistedPaymasterSessionsFrame, DigiDollarStatus::Kind::ERR);
                guard->applyPaymasterPrivacy();
                guard->m_form.updateSendButton();
                // Automatic startup checks stay inline. An explicit retry must
                // report a repeated failure even when the RPC finishes instantly.
                if (user_requested && !guard->m_privacy) {
                    guard->m_form.showWarning(DigiDollarSendWidget::tr("Saved transfers unavailable"), message);
                }
            };
            if (!error.isEmpty()) {
                failed(error, error.contains(QStringLiteral("PAYMASTER_RESERVATION_SESSION_CONFLICT")));
                return;
            }
            if (!result.isObject() || !result.find_value("active_only").isBool() ||
                !result.find_value("active_only").get_bool() || !result.find_value("sessions").isArray()) {
                failed(QStringLiteral("PAYMASTER_SESSION_LIST_INVALID_RESPONSE"));
                return;
            }
            const UniValue& sessions = result.find_value("sessions");
            const UniValue& next_cursor = result.find_value("next_cursor");
            qint64 count{0};
            if (!ReadInt64(result, "count", count) || count < 0 ||
                count != static_cast<qint64>(sessions.size()) ||
                sessions.size() > 100 || (!next_cursor.isNull() && !next_cursor.isStr())) {
                failed(QStringLiteral("PAYMASTER_SESSION_LIST_INVALID_COUNT_OR_CURSOR"));
                return;
            }

            if (next_cursor.isStr() && !next_cursor.get_str().empty()) {
                failed(DigiDollarSendWidget::tr("More saved transfers exist than this view can load. Inspect the remaining pages with listdigidollarsendsessions in the RPC console."));
                return;
            }

            struct ListedSession {
                QString request_id;
                QString state;
                qint64 amount_cents{-1};
            };
            std::vector<ListedSession> decoded;
            decoded.reserve(sessions.size());
            QStringList request_ids;
            for (const UniValue& session : sessions.getValues()) {
                if (!session.isObject()) {
                    failed(QStringLiteral("PAYMASTER_SESSION_LIST_INVALID_ENTRY"));
                    return;
                }
                const UniValue& request = session.find_value("request_id");
                const UniValue& session_id = session.find_value("session_id");
                const UniValue& state = session.find_value("session_state");
                if (!request.isStr() || !session_id.isStr() ||
                    !state.isStr()) {
                    failed(QStringLiteral("PAYMASTER_SESSION_LIST_INVALID_ENTRY"));
                    return;
                }
                ListedSession entry;
                entry.request_id = QString::fromStdString(request.get_str());
                entry.state = QString::fromStdString(state.get_str());
                const QString session_hash =
                    QString::fromStdString(session_id.get_str());
                if (!DigiDollar::Paymaster::IsCanonicalRequestId(
                        entry.request_id.toStdString()) ||
                    !IsCanonicalNonNullPaymasterHash(session_hash) ||
                    !IsKnownSessionState(entry.state) ||
                    request_ids.contains(entry.request_id)) {
                    failed(QStringLiteral("PAYMASTER_SESSION_LIST_INVALID_ENTRY"));
                    return;
                }
                request_ids.push_back(entry.request_id);
                const UniValue& amount =
                    session.find_value("requested_amount_cents");
                if (!amount.isNull() &&
                    (!ReadInt64(session, "requested_amount_cents",
                                entry.amount_cents) ||
                     entry.amount_cents <= 0 ||
                     entry.amount_cents >
                         DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS)) {
                    failed(QStringLiteral("PAYMASTER_SESSION_LIST_INVALID_ENTRY"));
                    return;
                }
                decoded.push_back(std::move(entry));
            }

            guard->m_sessionDiscoveryReady = true;
            guard->m_sessionDiscoveryError->clear();
            guard->m_sessionDiscoveryError->hide();
            DigiDollarStatus::SetBanner(guard->m_persistedPaymasterSessionsFrame, DigiDollarStatus::Kind::INFO);
            guard->m_persistedPaymasterSessionsFrame->hide();
            guard->m_form.updateSendButton();
            if (decoded.empty()) return;
            if (decoded.size() == 1) {
                guard->activatePersistedPaymasterSession(
                    decoded.front().request_id);
                return;
            }

            guard->m_persistedSessionsStatus->setText(DigiDollarSendWidget::tr("Interrupted Paymaster transfers:"));
            guard->m_loadPersistedPaymasterSessionButton->setText(DigiDollarSendWidget::tr("Review selected transfer"));
            guard->m_persistedPaymasterSessions->show();
            guard->m_persistedPaymasterSessions->clear();
            for (const ListedSession& entry : decoded) {
                const QString amount = entry.amount_cents > 0 ? guard->formatCents(entry.amount_cents) : DigiDollarSendWidget::tr("amount unavailable");
                guard->m_persistedPaymasterSessions->addItem(
                    DigiDollarSendWidget::tr("%1 · %2 · request %3…")
                        .arg(entry.state, amount,
                             entry.request_id.left(12)),
                    entry.request_id);
            }
            guard->m_persistedPaymasterSessionsFrame->show();
            guard->applyPaymasterPrivacy();
        });
}

void PaymasterSendWidget::loadSelectedPersistedPaymasterSession()
{
    if (!m_sessionDiscoveryReady) {
        discoverPersistedPaymasterSessions(/*user_requested=*/true);
        return;
    }
    if (!m_walletModel || m_paymasterBusy ||
        m_persistedPaymasterSessions->currentIndex() < 0) {
        reportPaymasterOperationNotStarted(
            !m_walletModel ? DigiDollarSendWidget::tr("wallet is no longer available") :
            m_paymasterBusy ? DigiDollarSendWidget::tr("another Paymaster operation is still active") :
                              DigiDollarSendWidget::tr("no persisted transfer is selected"));
        return;
    }
    const QString request_id =
        m_persistedPaymasterSessions->currentData().toString();
    if (!DigiDollar::Paymaster::IsCanonicalRequestId(
            request_id.toStdString())) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("the selected persisted request identifier is invalid"));
        return;
    }
    activatePersistedPaymasterSession(request_id);
}

void PaymasterSendWidget::activatePersistedPaymasterSession(
    const QString& request_id)
{
    if (!m_walletModel || !m_paymasterRequestId.isEmpty() ||
        !DigiDollar::Paymaster::IsCanonicalRequestId(
            request_id.toStdString())) {
        return;
    }
    m_paymasterRequestId = request_id;
    m_paymasterUnsignedClosed = false;
    m_paymasterOfferReadyForReview = false;
    m_paymasterSessionPersisted = true;
    m_paymasterAllowedActions.clear();
    m_paymasterAllowedActionsKnown = false;
    m_paymasterStateValue->setText(
        DigiDollarSendWidget::tr("Loading the authoritative persisted Paymaster transfer…"));
    m_persistedPaymasterSessionsFrame->hide();
    m_paymasterSessionFrame->show();
    updatePaymasterFocusMode();
    applyPaymasterPrivacy();
    // This is deliberately read-only. The user must explicitly choose every
    // subsequent resume, retry or recovery action returned by Core.
    refreshPaymasterSessionState();
}

bool PaymasterSendWidget::hasCurrentPaymasterOffer() const
{
    // Check the timer as well as the state: a queued expiry callback must not
    // leave an expired preview usable for a new request in this event turn.
    return m_offerCheckState == OfferCheckState::FOUND && m_offerExpiryTimer &&
           m_offerExpiryTimer->isActive() && m_offerExpiryTimer->remainingTime() > 0;
}

bool PaymasterSendWidget::hasOwnDgbForFees() const
{
    // This cheap compose check uses the wallet's cached spendable balance.
    // Core still decides whether suitable inputs cover the actual network fee.
    return m_walletModel && m_walletModel->getAvailableDGBBalance() > 0;
}

bool PaymasterSendWidget::preparesPaymasterPayment() const
{
    return paymasterModeSelected() && (paymasterOnlySelected() || !hasOwnDgbForFees());
}

bool PaymasterSendWidget::hasFeeFundingCandidate() const
{
    return m_walletModel && ((!paymasterOnlySelected() && hasOwnDgbForFees()) ||
                            (paymasterModeSelected() && hasCurrentPaymasterOffer()));
}

void PaymasterSendWidget::updateOfferCheckControls()
{
    if (!m_offerCheckFrame) return;
    m_offerCheckFrame->setVisible(paymasterModeSelected() && m_paymasterRequestId.isEmpty());
    const bool checking = m_offerCheckState == OfferCheckState::CHECKING;
    m_refreshOffersButton->setText(checking                                                                                     ? DigiDollarSendWidget::tr("Checking…") :
                                   m_offerCheckState == OfferCheckState::EMPTY                                                  ? DigiDollarSendWidget::tr("Check again") :
                                   m_offerCheckState == OfferCheckState::FAILED                                                 ? DigiDollarSendWidget::tr("Try again") :
                                   (m_offerCheckState == OfferCheckState::FOUND || m_offerCheckState == OfferCheckState::STALE) ? DigiDollarSendWidget::tr("Refresh offers") :
                                                                                                                                  DigiDollarSendWidget::tr("Check offers"));
    m_refreshOffersButton->setEnabled(m_walletModel && !m_paymasterBusy &&
                                      !checking && m_paymasterRequestId.isEmpty());
    if (m_offerCheckHelp) {
        if (!paymasterOnlySelected() && hasOwnDgbForFees()) {
            m_offerCheckHelp->setText(DigiDollarSendWidget::tr("Automatic uses your DGB first. Core checks whether it covers the network fee; any Paymaster fallback requires approval of the exact fee."));
        } else if (!paymasterOnlySelected() && !hasCurrentPaymasterOffer()) {
            m_offerCheckHelp->setText(DigiDollarSendWidget::tr("No spendable DGB and no current Paymaster offer. Add DGB or wait for an offer; the button updates automatically."));
        } else {
            m_offerCheckHelp->setText(m_offerCheckState == OfferCheckState::FOUND ? DigiDollarSendWidget::tr("Connection not checked yet. Preparing may temporarily reserve $DD; you approve the exact provider and fee before signing.") :
                                     m_offerCheckState == OfferCheckState::FAILED ? DigiDollarSendWidget::tr("Try the offer check again before preparing a payment.") :
                                     m_offerCheckState == OfferCheckState::STALE ? DigiDollarSendWidget::tr("The offer must be checked again before preparing a payment.") :
                                     DigiDollarSendWidget::tr("Preparation becomes available automatically when an offer is found."));
        }
    }
    updateOfferCheckIcon();
}

void PaymasterSendWidget::renderOfferSpinner()
{
    // Presentation only: reuse the status-bar animation without any RPC work.
    const qreal ratio = devicePixelRatioF();
    QPixmap pixmap(QString(":/animation/spinner-%1").arg(m_offerIconFrame, 3, 10, QLatin1Char('0')));
    pixmap = pixmap.scaled(QSize(qRound(36 * ratio), qRound(36 * ratio)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(ratio);
    {
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), m_offerStateIcon->palette().color(QPalette::WindowText));
    }
    m_offerStateIcon->setPixmap(pixmap);
    m_offerIconFrame = (m_offerIconFrame + 1) % SPINNER_FRAMES;
}

void PaymasterSendWidget::updateOfferCheckIcon()
{
    if (!m_offerStateIcon || !m_offerIconTimer) return;
    m_offerIconTimer->stop();
    const auto kind = m_privacy                                          ? DigiDollarStatus::Kind::INFO :
                      m_offerCheckState == OfferCheckState::FOUND        ? DigiDollarStatus::Kind::SUCCESS :
                      m_offerCheckState == OfferCheckState::CHECKING     ? DigiDollarStatus::Kind::WAITING :
                      m_offerCheckState == OfferCheckState::FAILED       ? DigiDollarStatus::Kind::ERR :
                      m_offerCheckState == OfferCheckState::NEEDS_AMOUNT ? DigiDollarStatus::Kind::ACTION :
                                                                           DigiDollarStatus::Kind::INFO;
    DigiDollarStatus::SetBanner(m_offerCheckFrame, kind);
    DigiDollarStatus::SetText(m_offersStatus, kind);
    DigiDollarStatus::SetText(m_offerStateIcon, kind);
    m_offerStateIcon->setVisible(!m_privacy && m_walletModel);
    if (m_privacy || !m_walletModel) {
        m_offerStateIcon->clear();
        m_offerStateIcon->setAccessibleName(QString{});
        return;
    }
    m_offerStateIcon->setAccessibleName(m_offersStatus->text());
    if (m_offerCheckState == OfferCheckState::CHECKING) {
        renderOfferSpinner();
        if (m_offerStateIcon->isVisible()) m_offerIconTimer->start();
    } else {
        const QString symbol = m_offerCheckState == OfferCheckState::FOUND                                                        ? QStringLiteral("✓") :
                               m_offerCheckState == OfferCheckState::FAILED || m_offerCheckState == OfferCheckState::NEEDS_AMOUNT ? QStringLiteral("!") :
                               m_offerCheckState == OfferCheckState::STALE                                                        ? QStringLiteral("↻") :
                               m_offerCheckState == OfferCheckState::EMPTY                                                        ? QStringLiteral("—") :
                                                                                                                                    QStringLiteral("?");
        m_offerStateIcon->setText(symbol);
    }
}

void PaymasterSendWidget::setOfferCheckStatus(OfferCheckState state, const QString& text)
{
    m_offerCheckState = state;
    m_offersStatus->setText(text);
    m_offersStatus->setAccessibleDescription(text);
    if (state == OfferCheckState::FOUND || state == OfferCheckState::EMPTY) {
        const QDateTime checked_at = QDateTime::currentDateTime();
        m_offersUpdated->setText(DigiDollarSendWidget::tr("List updated: %1 · Automatic check every 10 s")
                                     .arg(QLocale().toString(checked_at.time(), QLocale::LongFormat)));
        m_offersUpdated->setToolTip(DigiDollarSendWidget::tr(
            "This is the local list update time, not a connection test. A stopped provider can remain listed until its last signed announcement expires (up to 10 minutes).") +
            QStringLiteral("\n") + QLocale().toString(checked_at, QLocale::LongFormat));
    } else {
        m_offersUpdated->setText(DigiDollarSendWidget::tr("Automatic check every 10 s while this form is open."));
        m_offersUpdated->setToolTip(QString{});
    }
    updateOfferCheckControls();
    applyPaymasterPrivacy();
    m_form.updateSendButton();
}

void PaymasterSendWidget::expirePaymasterOfferPreview()
{
    if (!m_paymasterRequestId.isEmpty()) return;
    invalidatePaymasterOfferPreview();
    setOfferCheckStatus(OfferCheckState::STALE, DigiDollarSendWidget::tr(
        "The public-offer preview has expired."));
    updateFeeDisplay();
}

void PaymasterSendWidget::invalidatePaymasterOfferPreview()
{
    if (!m_paymasterRequestId.isEmpty()) return;
    ++m_paymasterOfferPreviewGeneration;
    if (m_offerExpiryTimer) m_offerExpiryTimer->stop();
    const bool had_preview =
        m_offerCheckState != OfferCheckState::NOT_CHECKED ||
        m_paymasterPreviewRecipientCents >= 0 ||
        m_paymasterPreviewServiceFeeCents >= 0 ||
        m_paymasterPreviewTotalCents >= 0 ||
        (m_offersTable && m_offersTable->rowCount() > 0);
    m_paymasterPreviewRecipientCents = -1;
    m_paymasterPreviewServiceFeeCents = -1;
    m_paymasterPreviewTotalCents = -1;
    if (m_offersTable) m_offersTable->setRowCount(0);
    if (had_preview && m_offersStatus) {
        setOfferCheckStatus(OfferCheckState::STALE, DigiDollarSendWidget::tr(
            "Offer inputs changed; the preview needs updating."));
    }
}

void PaymasterSendWidget::pollPaymasterOffers()
{
    // This only reads the local directory. It never requests a quote, opens a
    // Direct connection or reserves funds. Invisible/active forms stay idle.
    if (!isVisible() || m_privacy || !paymasterModeSelected() || !m_walletModel ||
        m_paymasterBusy || !m_paymasterRequestId.isEmpty() ||
        !m_sessionDiscoveryReady || !m_form.paymentInput().amount_valid) return;
    requestPaymasterOffers(/*background=*/true);
}

void PaymasterSendWidget::refreshPaymasterOffers()
{
    requestPaymasterOffers(/*background=*/false);
}

void PaymasterSendWidget::requestPaymasterOffers(bool background)
{
    const auto input = m_form.paymentInput();
    if (!m_walletModel || m_paymasterBusy || !m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            !m_walletModel ? DigiDollarSendWidget::tr("wallet is no longer available") :
                             DigiDollarSendWidget::tr("another Paymaster operation is still active"));
        return;
    }
    invalidatePaymasterOfferPreview();
    const CAmount amount_cents = input.amount_cents;
    if (!input.amount_valid) {
        setOfferCheckStatus(OfferCheckState::NEEDS_AMOUNT, DigiDollarSendWidget::tr("Enter a valid $DD amount, then check offers."));
        m_form.showWarning(DigiDollarSendWidget::tr("Paymaster offers"), DigiDollarSendWidget::tr("Enter an amount before refreshing offers."));
        return;
    }
    setOfferCheckStatus(OfferCheckState::CHECKING, DigiDollarSendWidget::tr("Checking Paymaster offers…"));
    setPaymasterBusy(true);
    UniValue params{UniValue::VARR};
    params.push_back(amount_cents);
    UniValue preview_options{UniValue::VOBJ};
    preview_options.pushKV(
        "subtract_paymaster_fee_from_amount",
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked());
    preview_options.pushKV("maximum_paymaster_fee_cents", m_feeCapSpin->value());
    preview_options.pushKV("privacy", m_privacyCombo->currentData().toString().toStdString());
    const bool subtract_from_amount =
        m_subtractPaymasterFeeCheck &&
        m_subtractPaymasterFeeCheck->isChecked();
    const uint64_t preview_generation = m_paymasterOfferPreviewGeneration;
    params.push_back(std::move(preview_options));
    QPointer<PaymasterSendWidget> guard{this};
    executePaymasterRpcAsync("getpaymasteroffers", std::move(params),
        [guard, background, preview_generation, amount_cents,
         subtract_from_amount](UniValue result, QString error) {
            if (!guard) return;
            if (guard->m_paymasterOfferPreviewGeneration !=
                preview_generation) {
                // Inputs changed while this asynchronous preview was in
                // flight. Keep the invalidated state instead of presenting a
                // response bound to the old amount or limits.
                guard->setPaymasterBusy(false);
                return;
            }
            guard->m_offersTable->setRowCount(0);
            guard->m_paymasterPreviewRecipientCents = -1;
            guard->m_paymasterPreviewServiceFeeCents = -1;
            guard->m_paymasterPreviewTotalCents = -1;
            if (!error.isEmpty()) {
                guard->setOfferCheckStatus(OfferCheckState::FAILED, DigiDollarSendWidget::tr(
                    "Public offers could not be checked. Try again."));
                if (!background) guard->m_form.showWarning(DigiDollarSendWidget::tr("Paymaster offers unavailable"), error);
                guard->setPaymasterBusy(false);
                return;
            }
            const auto fail_malformed = [guard, background] {
                guard->m_offersTable->setRowCount(0);
                guard->m_paymasterPreviewRecipientCents = -1;
                guard->m_paymasterPreviewServiceFeeCents = -1;
                guard->m_paymasterPreviewTotalCents = -1;
                guard->setOfferCheckStatus(OfferCheckState::FAILED, DigiDollarSendWidget::tr(
                    "Offer check failed: the response was malformed. Check node and wallet status before trying again. No fee was authorized."));
                if (!background) guard->m_form.showWarning(
                    DigiDollarSendWidget::tr("Paymaster offers unavailable"),
                    DigiDollarSendWidget::tr("Core returned an invalid Paymaster offer preview. Refresh node and wallet status before trying again."));
                guard->setPaymasterBusy(false);
            };
            constexpr size_t MAX_VISIBLE_PAYMASTER_OFFERS{100};
            if (!result.isArray() ||
                result.size() > MAX_VISIBLE_PAYMASTER_OFFERS) {
                fail_malformed();
                return;
            }
            try {
                for (const UniValue& offer : result.getValues()) {
                    const UniValue& display_name = offer.find_value("display_name");
                    const UniValue& funding_model = offer.find_value("funding_model");
                    const UniValue& service_fee = offer.find_value("service_fee_cents");
                    const UniValue& payment = offer.find_value("payment_cents");
                    const UniValue& total = offer.find_value("user_total_cents");
                    const UniValue& fee_rate = offer.find_value("fee_rate_bps");
                    const UniValue& maximum_service_fee =
                        offer.find_value(
                            "maximum_user_paid_service_fee_cents");
                    const UniValue& enough = offer.find_value("reputation_sufficient_data");
                    const UniValue& deprioritized = offer.find_value("recommendation_deprioritized");
                    const UniValue& success_rate = offer.find_value("success_rate_basis_points");
                    const UniValue& expiry = offer.find_value("expires_at");
                    const UniValue& provider_id = offer.find_value("provider_id");
                    const UniValue& offer_id = offer.find_value("offer_id");
                    const UniValue& policy_hash = offer.find_value("policy_hash");
                    const UniValue& subtract =
                        offer.find_value("subtract_paymaster_fee_from_amount");
                    if (!offer.isObject() || !display_name.isStr() ||
                        !funding_model.isStr() || !service_fee.isNum() ||
                        !payment.isNum() || !total.isNum() ||
                        !fee_rate.isNum() || !maximum_service_fee.isNum() ||
                        !enough.isBool() ||
                        (!deprioritized.isNull() && !deprioritized.isBool()) ||
                        !expiry.isNum() || !provider_id.isStr() ||
                        !offer_id.isStr() || !policy_hash.isStr() ||
                        !subtract.isBool() ||
                        (enough.get_bool() && !success_rate.isNum())) {
                        fail_malformed();
                        return;
                    }
                    const std::string model = funding_model.get_str();
                    const std::string provider = provider_id.get_str();
                    const std::string bound_offer = offer_id.get_str();
                    const std::string bound_policy = policy_hash.get_str();
                    const qint64 fee_cents = service_fee.getInt<qint64>();
                    const qint64 payment_cents = payment.getInt<qint64>();
                    const qint64 total_cents = total.getInt<qint64>();
                    const int fee_rate_bps = fee_rate.getInt<int>();
                    const qint64 maximum_service_fee_cents =
                        maximum_service_fee.getInt<qint64>();
                    const qint64 expires_at = expiry.getInt<qint64>();
                    const int success_bps = enough.get_bool()
                        ? success_rate.getInt<int>() : 0;
                    const QString display =
                        QString::fromStdString(display_name.get_str());
                    const QDateTime expiry_time =
                        QDateTime::fromSecsSinceEpoch(expires_at);
                    if ((model != "user_paid" && model != "sponsored") ||
                        display.size() > 128 ||
                        display_name.get_str().size() > 512 ||
                        fee_cents < 0 || payment_cents <= 0 ||
                        fee_cents > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
                        fee_rate_bps < 0 || fee_rate_bps > 10000 ||
                        fee_rate_bps % 10 != 0 ||
                        maximum_service_fee_cents < 0 ||
                        maximum_service_fee_cents >
                            DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
                        payment_cents >
                            DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
                        payment_cents >
                            std::numeric_limits<qint64>::max() - fee_cents ||
                        total_cents != payment_cents + fee_cents ||
                        (subtract_from_amount
                             ? total_cents != amount_cents
                             : payment_cents != amount_cents) ||
                                                (model == "sponsored" &&
                                                 (fee_cents != 0 || fee_rate_bps != 0 ||
                                                    maximum_service_fee_cents != 0)) ||
                        !IsCanonicalNonNullPaymasterHash(
                            QString::fromStdString(provider)) ||
                        !IsCanonicalNonNullPaymasterHash(
                            QString::fromStdString(bound_offer)) ||
                        !IsCanonicalNonNullPaymasterHash(
                            QString::fromStdString(bound_policy)) ||
                        !expiry_time.isValid() ||
                        expires_at <= QDateTime::currentSecsSinceEpoch() ||
                        subtract.get_bool() != subtract_from_amount ||
                        success_bps < 0 ||
                        success_bps > 10000) {
                        fail_malformed();
                        return;
                    }
                }
            } catch (const std::exception&) {
                fail_malformed();
                return;
            }
            int row{0};
            for (const UniValue& offer : result.getValues()) {
                guard->m_offersTable->insertRow(row);
                const QString provider = QString::fromStdString(offer.find_value("display_name").get_str());
                const QString model = QString::fromStdString(offer.find_value("funding_model").get_str());
                const qint64 fee = offer.find_value("service_fee_cents").getInt<qint64>();
                const qint64 payment = offer.find_value("payment_cents").getInt<qint64>();
                const qint64 total = offer.find_value("user_total_cents").getInt<qint64>();
                const int fee_rate_bps =
                    offer.find_value("fee_rate_bps").getInt<int>();
                const qint64 maximum_service_fee_cents =
                    offer.find_value(
                        "maximum_user_paid_service_fee_cents")
                        .getInt<qint64>();
                if (row == 0) {
                    guard->m_paymasterPreviewRecipientCents = payment;
                    guard->m_paymasterPreviewServiceFeeCents = fee;
                    guard->m_paymasterPreviewTotalCents = total;
                }
                const bool enough = offer.find_value("reputation_sufficient_data").get_bool();
                QString reputation = enough
                    ? DigiDollarSendWidget::tr("%1%").arg(offer.find_value("success_rate_basis_points").getInt<int>() / 100.0, 0, 'f', 2)
                    : DigiDollarSendWidget::tr("New provider — not enough history yet");
                const qint64 expiry = offer.find_value("expires_at").getInt<qint64>();
                QString fee_display =
                    DigiDollarSendWidget::tr("%1 (≈ %2%)")
                        .arg(guard->formatCents(fee),
                             PaymasterEffectivePercent(
                                 fee, offer.find_value("payment_cents")
                                          .getInt<qint64>()));
                if (maximum_service_fee_cents > 0) {
                    fee_display += DigiDollarSendWidget::tr(" · max %1")
                                       .arg(guard->formatCents(
                                           maximum_service_fee_cents));
                }
                const QStringList cells{provider, guard->friendlyFundingModel(model),
                                        fee_display, guard->formatCents(total), reputation,
                                        QDateTime::fromSecsSinceEpoch(expiry).toLocalTime().toString(Qt::ISODate)};
                for (int column = 0; column < cells.size(); ++column) {
                    auto* item = new QTableWidgetItem(cells[column]);
                    item->setToolTip(QString::fromStdString(offer.find_value("provider_id").get_str()) +
                                     QStringLiteral("\n") +
                                     QString::fromStdString(offer.find_value("policy_hash").get_str()) +
                                     DigiDollarSendWidget::tr("\nRate: %1%; maximum service fee: %2")
                                         .arg(QString::number(fee_rate_bps / 100.0,
                                                              'f', 2),
                                              maximum_service_fee_cents == 0
                                                  ? DigiDollarSendWidget::tr("no cap")
                                                  : guard->formatCents(
                                                        maximum_service_fee_cents)));
                    guard->m_offersTable->setItem(row, column, item);
                }
                ++row;
            }
            guard->setOfferCheckStatus(row == 0 ? OfferCheckState::EMPTY : OfferCheckState::FOUND,
                                       row == 0 ? DigiDollarSendWidget::tr("No public offer for this amount yet.") : row == 1 ? DigiDollarSendWidget::tr("Public Paymaster offer found") :
                                                                                                                                DigiDollarSendWidget::tr("Public Paymaster offers found: %1").arg(row));
            if (row > 0) {
                qint64 earliest_expiry = std::numeric_limits<qint64>::max();
                for (const auto& offer : result.getValues()) {
                    earliest_expiry = std::min(earliest_expiry, offer.find_value("expires_at").getInt<qint64>());
                }
                const qint64 remaining_seconds = earliest_expiry - QDateTime::currentSecsSinceEpoch();
                if (remaining_seconds <= 0) guard->expirePaymasterOfferPreview();
                else guard->m_offerExpiryTimer->start(static_cast<int>(std::min<qint64>(remaining_seconds, std::numeric_limits<int>::max() / 1000) * 1000));
            }
            guard->setPaymasterBusy(false);
            guard->updateFeeDisplay();
            guard->applyPaymasterPrivacy();
        });
}

UniValue PaymasterSendWidget::buildPaymasterSendParams(const QString& address, CAmount amount_cents) const
{
    // Keep the exact original order through review, unlock, retries and
    // provider fallback. Re-reading editable controls here can change the
    // canonical request after Core has already reserved its inputs.
    if (m_paymasterSendTemplate.isArray() && m_paymasterSendTemplate.size() == 7 &&
        m_paymasterSendTemplate[6].find_value("request_id").isStr() &&
        m_paymasterSendTemplate[6].find_value("request_id").get_str() == m_paymasterRequestId.toStdString()) {
        UniValue params{UniValue::VARR};
        for (size_t i = 0; i < 6; ++i)
            params.push_back(m_paymasterSendTemplate[i]);
        UniValue options{UniValue::VOBJ};
        for (const auto& key : m_paymasterSendTemplate[6].getKeys()) {
            if (key != "prepare_only" && key != "authorization_commitment" && key != "retry_transport") {
                options.pushKV(key, m_paymasterSendTemplate[6].find_value(key));
            }
        }
        if (m_paymasterAuthorizationCommitment.isEmpty())
            options.pushKV("prepare_only", true);
        else
            options.pushKV("authorization_commitment", m_paymasterAuthorizationCommitment.toStdString());
        params.push_back(std::move(options));
        return params;
    }
    const auto input = m_form.paymentInput();
    UniValue params{UniValue::VARR};
    params.push_back(address.toStdString());
    params.push_back(amount_cents);
    params.push_back(input.comment.toStdString());
    params.push_back(0);
    if (!input.selected_inputs.empty()) {
        UniValue selected{UniValue::VARR};
        for (const COutPoint& outpoint : input.selected_inputs) {
            UniValue input{UniValue::VOBJ};
            input.pushKV("txid", outpoint.hash.GetHex());
            input.pushKV("vout", outpoint.n);
            selected.push_back(std::move(input));
        }
        params.push_back(std::move(selected));
    } else {
        params.push_back(UniValue{});
    }
    params.push_back("cents"); // Upstream amount_unit precedes Paymaster options.
    UniValue options{UniValue::VOBJ};
    // A Prepare payment click must never become direct-spend authorization.
    // Freeze the effective mode in the existing immutable request template.
    options.pushKV("fee_mode", preparesPaymasterPayment() ? "paymaster" : feeMode().toStdString());
    options.pushKV("request_id", m_paymasterRequestId.toStdString());
    options.pushKV("maximum_paymaster_fee_cents", m_feeCapSpin->value());
    options.pushKV("maximum_provider_attempts", m_maxAttemptsSpin->value());
    options.pushKV("privacy", m_privacyCombo->currentData().toString().toStdString());
    options.pushKV("selection", m_selectionCombo->currentData().toString().toStdString());
    options.pushKV(
        "subtract_paymaster_fee_from_amount",
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked());
    options.pushKV("send_all_spendable_dd", m_sendAllSpendableDD);
    if (m_paymasterSessionPersisted) {
        // A restored Paymaster session must never inherit Own DGB from the
        // freshly opened compose form. Core still validates the exact order.
        options.pushKV("fee_mode", "paymaster");
        for (const auto& key : m_paymasterRestoredOptions.getKeys()) {
            options.pushKV(key, m_paymasterRestoredOptions.find_value(key));
        }
        if (!m_paymasterSessionPrivacy.isEmpty()) {
            options.pushKV("privacy", m_paymasterSessionPrivacy.toStdString());
            if (m_paymasterSessionPrivacy == QLatin1String("high")) options.pushKV("maximum_provider_attempts", 1);
        }
    }
    if (m_paymasterAuthorizationCommitment.isEmpty()) {
        options.pushKV("prepare_only", true);
    } else {
        options.pushKV("authorization_commitment",
                       m_paymasterAuthorizationCommitment.toStdString());
    }
    params.push_back(std::move(options));
    return params;
}

void PaymasterSendWidget::executePaymasterRpcAsync(
    std::string command, UniValue params, WalletModel::RpcCallback callback)
{
    const uint64_t wallet_generation = m_paymasterWalletGeneration;
    QPointer<PaymasterSendWidget> guard{this};
    WalletModel::RpcCallback wallet_bound_callback =
        [guard, wallet_generation, callback = std::move(callback)](
            UniValue result, QString error) mutable {
            // A reply from a previously selected wallet must never populate
            // the current wallet's offer, session, recovery or safety state.
            if (!guard || guard->m_paymasterWalletGeneration !=
                              wallet_generation) {
                return;
            }
            callback(std::move(result), std::move(error));
        };
    if (m_paymasterAsyncRpcExecutorForTesting) {
        try {
            m_paymasterAsyncRpcExecutorForTesting(
                command, params, wallet_bound_callback);
        } catch (const std::exception& exception) {
            wallet_bound_callback(
                UniValue{}, QString::fromUtf8(exception.what()));
        } catch (...) {
            wallet_bound_callback(UniValue{}, DigiDollarSendWidget::tr("Unknown RPC error"));
        }
        return;
    }
    if (m_paymasterRpcExecutorForTesting) {
        UniValue result;
        QString error;
        try {
            result = m_paymasterRpcExecutorForTesting(command, params);
        } catch (const UniValue& rpc_error) {
            const UniValue& message = rpc_error.find_value("message");
            error = message.isStr()
                ? QString::fromStdString(message.get_str())
                : QString::fromStdString(rpc_error.write());
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
        } catch (...) {
            error = DigiDollarSendWidget::tr("Unknown RPC error");
        }
        wallet_bound_callback(std::move(result), std::move(error));
        return;
    }

    if (!m_walletModel) {
        wallet_bound_callback(UniValue{}, DigiDollarSendWidget::tr("Wallet is not available"));
        return;
    }
    m_walletModel->executeRpcAsync(
        std::move(command), std::move(params),
        std::move(wallet_bound_callback));
}

void PaymasterSendWidget::executePaymasterTransfer(
    const QString& address, CAmount amount_cents, bool allow_unlock, bool retry_transport)
{
    // The first call prepares and authenticates an offer only. Signing is never
    // inferred from clicking Send: handlePaymasterResult requires the exact
    // authorization commitment and presents its approval separately.
    if (!m_walletModel) {
        reportPaymasterOperationNotStarted(DigiDollarSendWidget::tr("wallet is no longer available"));
        return;
    }
    if (m_paymasterBusy) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("another Paymaster operation is still active"));
        return;
    }
    if (m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("no durable request is selected"));
        return;
    }
    const uint64_t wallet_generation = m_paymasterWalletGeneration;
    // Reserve the operation before a wallet-unlock dialog can re-enter the
    // event loop. No Paymaster button or poll may start a second operation.
    setPaymasterBusy(true);
    std::shared_ptr<WalletModel::UnlockContext> unlock;
    if (allow_unlock) {
        unlock = m_walletModel->requestUnlockForAsync();
        if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel) return;
        if (!unlock->isValid()) {
            m_paymasterStateValue->setText(DigiDollarSendWidget::tr("AWAITING_WALLET_UNLOCK"));
            setPaymasterBusy(false);
            return;
        }
    }
    m_paymasterSessionFrame->show();
    m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Contacting Paymaster Network…"));
    QPointer<PaymasterSendWidget> guard{this};
    m_paymasterActiveSendParams = buildPaymasterSendParams(address, amount_cents);
    m_paymasterSendTemplate = m_paymasterActiveSendParams;
    m_paymasterActiveSendStarted.start();
    UniValue initial_params{UniValue::VARR};
    for (size_t i = 0; i < 6; ++i)
        initial_params.push_back(m_paymasterActiveSendParams[i]);
    UniValue initial_options = m_paymasterActiveSendParams[6];
    if (retry_transport) initial_options.pushKV("retry_transport", true);
    initial_params.push_back(std::move(initial_options));
    executePaymasterRpcAsync("senddigidollar", std::move(initial_params),
                             [guard, address, amount_cents, unlock = std::move(unlock)](UniValue result, QString error) mutable {
                                 // Relock before result handling: that path may open confirmation or
                                 // error dialogs and must never extend the signing unlock across a
                                 // user-controlled modal wait.
                                 unlock.reset();
                                 if (guard) {
                                     guard->handlePaymasterResult(result, error, address,
                                                                  amount_cents);
                                 }
                             });
}

void PaymasterSendWidget::handlePaymasterResult(const UniValue& result, const QString& error,
                                                 const QString& address, CAmount amount_cents)
{
    if (!error.isEmpty()) {
        if (error.contains(QStringLiteral("PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED")) ||
            error.contains(QStringLiteral("PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH")) ||
            error.contains(QStringLiteral("PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED")) ||
            error.contains(QStringLiteral("Paymaster quote expired"))) {
            m_paymasterAuthorizationCommitment.clear();
            m_paymasterConfirmationGuard.Reset();
        }
        if ((error.contains(QStringLiteral("PAYMASTER_NO_ELIGIBLE_OFFER")) ||
             error.contains(QStringLiteral("PAYMASTER_NO_EXACT_GROSS_OFFER")) ||
             error.contains(QStringLiteral("PAYMASTER_SWEEP_BALANCE_CHANGED"))) &&
            !m_paymasterSessionPersisted) {
            // Offer selection failed before Core returned a durable session.
            // Do not present retry/recovery actions for a local request id that
            // has no authoritative wallet state behind it. A later Send click
            // must receive a fresh request id so newly announced offers can be
            // considered immediately.
            m_paymasterRequestId.clear();
            m_paymasterSessionId.clear();
            m_paymasterSessionState.clear();
            m_paymasterUnsignedClosed = false;
            m_paymasterOfferReadyForReview = false;
            m_paymasterAttemptState.clear();
            m_paymasterArtifact.clear();
            m_paymasterPendingPhase.clear();
            m_paymasterBroadcastState.clear();
            m_paymasterConfirmationState.clear();
            m_paymasterTransactionId.clear();
            m_paymasterRecoveryTransactionId.clear();
            m_paymasterResultStatus.clear();
            m_paymasterResultSequence = -1;
            m_paymasterRecoveryExpiresAt = -1;
            m_paymasterTerminalNoticeShown = false;
            m_paymasterSessionPersisted = false;
            m_paymasterAuthorizationCommitment.clear();
            m_paymasterConfirmationGuard.Reset();
            m_paymasterStateValue->setText(DigiDollarSendWidget::tr(
                "No Paymaster transfer was created. Any unsigned input reservation was released. You can prepare the payment again."));
            m_paymasterIdentityValue->setText(DigiDollarSendWidget::tr("No provider selected"));
            m_paymasterCostValue->setText(DigiDollarSendWidget::tr("No service fee reserved or authorized"));
            m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
        } else {
            m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Paymaster error: %1").arg(error));
        }
        stopPaymasterPolling();
        updatePaymasterFocusMode();
        if (!m_paymasterRequestId.isEmpty()) {
            m_paymasterStateValue->setText(error); // Technical details only.
            setPaymasterBusy(false);
            const bool expired = error.contains(QStringLiteral("EXPIRED")) ||
                                 error.contains(QStringLiteral("quote expired"), Qt::CaseInsensitive);
            const bool database_error = error.contains(QStringLiteral("PAYMASTER_SESSION_DATABASE_READ")) ||
                                        error.contains(QStringLiteral("PAYMASTER_SESSION_READ_FAILED")) ||
                                        error.contains(QStringLiteral("PAYMASTER_UNSUPPORTED_PERSISTED_VERSION"));
            QString reason = DigiDollarSendWidget::tr("The payment attempt could not be completed.");
            if (database_error)
                reason = DigiDollarSendWidget::tr("Wallet data could not be read reliably. Existing reservations and authorizations remain protected. Preserve the wallet backup and debug log; check the current status before continuing.");
            else if (error.startsWith(QStringLiteral("PAYMASTER_DD_INPUT_SELECTION_FAILED: ")))
                reason = DigiDollarSendWidget::tr("The payment and Paymaster fee cannot be funded with the current $DD inputs. %1")
                             .arg(error.mid(QStringLiteral("PAYMASTER_DD_INPUT_SELECTION_FAILED: ").size()));
            else if (expired) reason = DigiDollarSendWidget::tr("The provider offer expired before the payment could continue.");
            else if (error.contains(QStringLiteral("PAYMASTER_NO_ELIGIBLE_OFFER")))
                reason = DigiDollarSendWidget::tr("No usable provider offer remains for this attempt.");
            else if (error.contains(QStringLiteral("PAYMASTER_PROXY_OR_ENDPOINT_UNREACHABLE")) ||
                     error.contains(QStringLiteral("PAYMASTER_DIRECT_CONNECTION_FAILED")))
                reason = DigiDollarSendWidget::tr("The provider could not be reached. Its public announcement may still be listed after it goes offline.");
            else if (error.contains(QStringLiteral("PAYMASTER_INPUT_ALREADY_RESERVED")))
                reason = DigiDollarSendWidget::tr("The required funds are locked or reserved by another request. Check existing transfers before retrying.");
            // A database-read failure is not evidence of an unusable offer.
            // Reconcile read-only and retain the specific diagnosis inline.
            closeUnusablePaymasterOffer(reason, /*allow_cancel=*/!database_error);
            return;
        }
        if (m_paymasterSessionPersisted && error.contains(QStringLiteral("PAYMASTER_NO_ELIGIBLE_OFFER"))) {
            m_form.showWarning(DigiDollarSendWidget::tr("Offer request ended"),
                DigiDollarSendWidget::tr("This request has no eligible remaining provider. An expired or rejected attempt cannot be reused. The saved request will now be checked; if it closed without payment authorization, you can start a new transfer.\n\nTechnical details: %1").arg(error));
        } else {
            m_form.showBackendError(static_cast<int>(WalletModel::TransactionCreationFailed), error);
        }
        setPaymasterBusy(false);
        if (m_walletModel && !m_paymasterRequestId.isEmpty()) {
            // A failed/lost first reply may follow a successful durable write.
            // Resolve this UUID before presenting a new transfer or cancellation.
            refreshPaymasterSessionState();
        }
        return;
    }
    const UniValue& txid = result.find_value("txid");
    const UniValue& direct_status = result.find_value("status");
    const QString direct_status_text = direct_status.isStr() ? QString::fromStdString(direct_status.get_str()) : QString{};
    const bool direct_status_present = !direct_status.isNull() &&
                                       result.find_value("session_state").isNull() &&
                                       result.find_value("session").isNull();
    const bool direct_success = direct_status_present &&
                                direct_status.isStr() &&
                                result.find_value("session_state").isNull() &&
                                result.find_value("session").isNull() &&
                                IsValidatedPaymasterCompletion(
                                    txid.isStr() ? QString::fromStdString(txid.get_str()) : QString{},
                                    QString{}, direct_status_text, QString{});
    if (direct_status_present && !direct_success) {
        stopPaymasterPolling();
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Transfer status blocked: malformed direct completion"));
        m_form.showWarning(
            DigiDollarSendWidget::tr("DigiDollar transfer status unavailable"),
            DigiDollarSendWidget::tr("Core returned a direct-transfer status without the exact successful status and a valid nonzero transaction id. The form was not cleared and no Paymaster continuation was started."));
        setPaymasterBusy(false);
        return;
    }
    if (!direct_success) {
        QString decode_error;
        if (!updatePaymasterSessionView(result, &decode_error)) {
            stopPaymasterPolling();
            m_paymasterStateValue->setText(
                DigiDollarSendWidget::tr("Paymaster status blocked: malformed Core response"));
            m_form.showWarning(
                DigiDollarSendWidget::tr("Paymaster status unavailable"),
                DigiDollarSendWidget::tr("Core returned an incomplete or inconsistent Paymaster session (%1). No automatic signing, retry or recovery will continue.")
                    .arg(decode_error));
            setPaymasterBusy(false);
            return;
        }
        if (handleAuthoritativePaymasterCompletion(result)) {
            setPaymasterBusy(false);
            if (m_paymasterSessionState == QStringLiteral("FAILED")) {
                closeUnusablePaymasterOffer(DigiDollarSendWidget::tr("The provider offer could not be completed."));
            }
            return;
        }
        if (m_paymasterSessionState == QStringLiteral("MEMPOOL")) {
            // Submission is complete, but payment confirmation is still pending.
            // Continue read-only polling without requesting another signature.
            m_paymasterActiveSendParams = UniValue{};
            m_paymasterActiveSendStarted.invalidate();
            setPaymasterBusy(false);
            schedulePaymasterPoll(/*state_changed=*/true);
            return;
        }
    }
    // updatePaymasterSessionView() may have learned that Core persisted the
    // session. Re-evaluate the recovery controls with that authoritative fact.
    const QString txid_text = txid.isStr()
        ? QString::fromStdString(txid.get_str()) : QString{};
    if (direct_success && IsValidatedPaymasterCompletion(
            txid_text, QString{}, direct_status_text, QString{})) {
        // A FINAL_COMMITTED result reaches Qt only after Core has reloaded the
        // durable client manifest, validated every final input/output and
        // witness, and accepted or observed the exact transaction. The
        // response echo is presentation metadata at this point; a missing echo
        // must not turn a completed payment into a false authorization error.
        stopPaymasterPolling();
        qint64 payment_cents{0};
        const double recipient_amount =
            ReadInt64(result, "payment_cents", payment_cents) &&
                payment_cents > 0 &&
                payment_cents <= DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS
            ? payment_cents / 100.0
            : amount_cents / 100.0;
        m_form.showSuccess(txid_text, recipient_amount);
        m_form.onClearClicked();
        m_form.updateBalance();
        setPaymasterBusy(false);
        return;
    }
    const UniValue& returned_commitment_value = result.find_value("authorization_commitment");
    const QString returned_commitment = returned_commitment_value.isStr()
        ? QString::fromStdString(returned_commitment_value.get_str())
        : QString{};
    if (!returned_commitment.isEmpty() &&
        !IsCanonicalNonNullPaymasterHash(returned_commitment)) {
        stopPaymasterPolling();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterConfirmationGuard.Reset();
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster authorization blocked"),
            DigiDollarSendWidget::tr("Core returned a malformed Paymaster authorization commitment. No signature or automatic retry will continue."));
        setPaymasterBusy(false);
        return;
    }
    if (!m_paymasterAuthorizationCommitment.isEmpty() &&
        (returned_commitment.isEmpty() ||
         returned_commitment != m_paymasterAuthorizationCommitment)) {
        stopPaymasterPolling();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterConfirmationGuard.Reset();
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Paymaster authorization blocked: commitment changed or missing"));
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster authorization blocked"),
            DigiDollarSendWidget::tr("The wallet did not return the exact authorization commitment that was "
               "approved. No signature or automatic retry will continue. Review the "
               "current provider offer again."));
        setPaymasterBusy(false);
        return;
    }
    const UniValue& authorization_required_value =
        result.find_value("authorization_required");
    if (!authorization_required_value.isNull() &&
        !authorization_required_value.isBool()) {
        stopPaymasterPolling();
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster authorization blocked"),
            DigiDollarSendWidget::tr("Core returned an invalid authorization-required flag. No signature or automatic retry will continue."));
        setPaymasterBusy(false);
        return;
    }
    const bool authorization_required = authorization_required_value.isBool() &&
        authorization_required_value.get_bool();
    if (authorization_required) {
        stopPaymasterPolling();
        if (returned_commitment.isEmpty()) {
            m_paymasterStateValue->setText(
                DigiDollarSendWidget::tr("Paymaster authorization blocked: missing exact commitment"));
            m_form.showWarning(
                DigiDollarSendWidget::tr("Paymaster authorization blocked"),
                DigiDollarSendWidget::tr("The prepared Paymaster transaction did not include its exact client "
                   "authorization commitment. No Qt authorization will continue."));
            setPaymasterBusy(false);
            return;
        }
        const uint64_t wallet_generation = m_paymasterWalletGeneration;
        const OfferReviewResult review = confirmPaymasterSelectionBeforeSigning(result, address);
        if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel) return;
        if (review != OfferReviewResult::ACCEPTED || m_privacy) {
            setPaymasterBusy(false);
            if (review == OfferReviewResult::EXPIRED) {
                closeUnusablePaymasterOffer(DigiDollarSendWidget::tr("The provider offer expired before payment approval."));
            } else if (review == OfferReviewResult::CANCELED && !m_privacy) {
                // Cancel is an explicit decision to leave this unsigned order.
                // Core must still prove cancellation safe before editing resumes.
                cancelUnsignedPaymasterSession(/*confirm=*/false);
            }
            return;
        }
        m_paymasterAuthorizationCommitment = returned_commitment;
        setPaymasterBusy(false);
        executePaymasterTransfer(address, amount_cents, /*allow_unlock=*/true);
        return;
    }
    if (m_paymasterSessionState == QStringLiteral("AWAITING_WALLET_UNLOCK")) {
        // Before a quote exists, Core may need the unlocked wallet only for
        // the input-control proof. prepare_only remains set, so this call
        // cannot produce the collaborative transaction signature. If the
        // exact authorization was already approved, the same unlock also
        // permits the separately committed authorize stage.
        stopPaymasterPolling();
        // The first response may predate the authoritative action list. An
        // explicit Core `resume` capability is required once a refresh has
        // supplied one.
        if (!m_paymasterAllowedActionsKnown ||
            m_paymasterAllowedActions.contains(QStringLiteral("resume"))) {
            setPaymasterBusy(false);
            executePaymasterTransfer(address, amount_cents,
                                     /*allow_unlock=*/true);
        } else {
            setPaymasterBusy(false);
        }
        return;
    }
    if (m_paymasterActiveSendParams.isArray() &&
        !m_paymasterAuthorizationCommitment.isEmpty() && m_paymasterArtifact.isEmpty() &&
        (m_paymasterSessionState == QStringLiteral("AUTHORIZED") ||
         m_paymasterSessionState == QStringLiteral("PENDING_PROVIDER"))) {
        // The send RPC may omit artifact metadata. Confirm the durable signed
        // artifact through the read-only envelope before continuing this live
        // request; a state label alone must not authorize another send.
        const UniValue params = m_paymasterActiveSendParams;
        const QElapsedTimer started = m_paymasterActiveSendStarted;
        setPaymasterBusy(false);
        refreshPaymasterSessionForAction(QStringLiteral("refresh"),
            [guard = QPointer<PaymasterSendWidget>(this), params, started] {
                if (!guard) return;
                guard->m_paymasterActiveSendParams = params;
                guard->m_paymasterActiveSendStarted = started;
                if (!guard->canContinueActivePaymasterSend()) {
                    guard->m_paymasterActiveSendParams = UniValue{};
                    guard->m_paymasterActiveSendStarted.invalidate();
                }
                guard->schedulePaymasterPoll(/*state_changed=*/false);
            });
        return;
    }
    if (!canContinueActivePaymasterSend()) {
        m_paymasterActiveSendParams = UniValue{};
        m_paymasterActiveSendStarted.invalidate();
    }
    setPaymasterBusy(false);
    schedulePaymasterPoll(/*state_changed=*/false);
    updatePaymasterFocusMode();
}

bool PaymasterSendWidget::updatePaymasterSessionView(
    const UniValue& result, QString* decode_error)
{
    PaymasterSessionSnapshot snapshot;
    QString error;
    if (!DecodePaymasterSessionSnapshot(result, snapshot, error)) {
        if (decode_error) *decode_error = error;
        return false;
    }
    if (!m_paymasterRequestId.isEmpty() && !snapshot.request_id.isEmpty() &&
        snapshot.request_id != m_paymasterRequestId) {
        if (decode_error) {
            *decode_error = DigiDollarSendWidget::tr("response belongs to a different request");
        }
        return false;
    }
    if (!m_paymasterSessionId.isEmpty() && !snapshot.session_id.isEmpty() &&
        snapshot.session_id != m_paymasterSessionId) {
        if (decode_error) {
            *decode_error = DigiDollarSendWidget::tr("response belongs to a different session");
        }
        return false;
    }
    if (!m_paymasterAddress.isEmpty() && !snapshot.recipient.isEmpty() &&
        snapshot.recipient != m_paymasterAddress) {
        if (decode_error) {
            *decode_error = DigiDollarSendWidget::tr("response recipient does not match the active transfer");
        }
        return false;
    }

    const QString previous_state = m_paymasterSessionState;
    m_paymasterSessionFrame->show();
    if (!snapshot.request_id.isEmpty()) m_paymasterRequestId = snapshot.request_id;
    if (!snapshot.session_id.isEmpty()) {
        m_paymasterSessionId = snapshot.session_id;
        m_paymasterSessionPersisted = true;
    }
    if (!snapshot.restored_options.empty()) m_paymasterRestoredOptions = snapshot.restored_options;
    m_paymasterSessionState = snapshot.state;
    m_paymasterUnsignedClosed = snapshot.unsigned_closed;
    m_paymasterOfferReadyForReview = snapshot.offer_ready_for_review;
    m_paymasterPendingPhase = snapshot.pending_phase;
    m_paymasterBroadcastState = snapshot.broadcast_state;
    m_paymasterConfirmationState = snapshot.confirmation_state;
    m_paymasterTransactionId = snapshot.transaction_id;
    m_paymasterRecoveryTransactionId = snapshot.recovery_transaction_id;
    m_paymasterResultStatus = snapshot.result_status;
    m_paymasterResultSequence = snapshot.result_sequence;
    m_paymasterRecoveryExpiresAt = snapshot.recovery_expires_at;
    // Replace optional state atomically. Missing fields explicitly clear stale
    // values; a new session can never inherit an old attempt or artifact.
    m_paymasterArtifact = snapshot.artifact;
    m_paymasterAttemptState = snapshot.attempt_state;
    m_paymasterAllowedActions = snapshot.allowed_actions;
    m_paymasterAllowedActionsKnown = snapshot.allowed_actions_known;
    if (snapshot.state != QStringLiteral("MEMPOOL") &&
        snapshot.state != QStringLiteral("CONFIRMED") &&
        snapshot.state != QStringLiteral("CANCELED_SAFE") &&
        snapshot.state != QStringLiteral("FAILED") &&
        snapshot.state != QStringLiteral("CONFLICTED")) {
        m_paymasterTerminalNoticeShown = false;
    }
    if (snapshot.authoritative || !snapshot.recipient.isEmpty()) {
        m_paymasterAddress = snapshot.recipient;
    }
    if (snapshot.authoritative || !snapshot.privacy_profile.isEmpty()) {
        m_paymasterSessionPrivacy = snapshot.privacy_profile;
    }
    if (snapshot.requested_amount_cents > 0) {
        m_paymasterAmountCents = snapshot.requested_amount_cents;
        m_paymasterAmount = snapshot.requested_amount_cents / 100.0;
    } else if (snapshot.has_costs) {
        m_paymasterAmountCents = snapshot.payment_cents;
        m_paymasterAmount = snapshot.payment_cents / 100.0;
    } else if (snapshot.authoritative) {
        // Do not let a sparse restart snapshot inherit an amount from a
        // previously displayed session.
        m_paymasterAmountCents = 0;
        m_paymasterAmount = 0.0;
    }

    QString status = snapshot.pending_phase.isEmpty()
        ? snapshot.state
        : DigiDollarSendWidget::tr("%1 — %2").arg(snapshot.state, snapshot.pending_phase);
    if (!snapshot.broadcast_state.isEmpty() ||
        !snapshot.confirmation_state.isEmpty()) {
        status += DigiDollarSendWidget::tr(" · broadcast: %1 · confirmation: %2")
            .arg(snapshot.broadcast_state.isEmpty()
                     ? DigiDollarSendWidget::tr("unknown") : snapshot.broadcast_state,
                 snapshot.confirmation_state.isEmpty()
                     ? DigiDollarSendWidget::tr("unknown") : snapshot.confirmation_state);
    }
    const UniValue& transport_state = result.find_value("transport").find_value("state");
    m_paymasterTransportState = transport_state.isStr() ? QString::fromStdString(transport_state.get_str()) : QString{};
    if (transport_state.isStr() && !IsTerminalPaymasterState(snapshot.state)) {
        const std::string& state = transport_state.get_str();
        if (state == "waiting_capacity") {
            status = DigiDollarSendWidget::tr("Waiting for a free payment connection");
        } else if (state == "connecting" || state == "handshaking") {
            status = DigiDollarSendWidget::tr("Connecting to the selected provider");
        }
    }
    m_paymasterStateValue->setText(status);
    m_paymasterIdentityValue->setText(snapshot.provider_id.isEmpty()
        ? DigiDollarSendWidget::tr("Provider not selected yet")
        : DigiDollarSendWidget::tr("Selected provider · %1…").arg(snapshot.provider_id.left(12)));
    m_paymasterIdentityValue->setToolTip(
        snapshot.provider_id.isEmpty() ? QString{} :
        DigiDollarSendWidget::tr("Provider: %1\nPolicy: %2")
            .arg(snapshot.provider_id, snapshot.policy_hash));
    if (snapshot.has_costs) {
        m_paymasterCostValue->setText(DigiDollarSendWidget::tr("Recipient %1 + service fee %2 = %3 maximum wallet outflow (limit %4)")
            .arg(formatCents(snapshot.payment_cents),
                 DigiDollarSendWidget::tr("%1 (≈ %2%)").arg(formatCents(snapshot.service_fee_cents), PaymasterEffectivePercent(snapshot.service_fee_cents, snapshot.payment_cents)),
                 formatCents(snapshot.total_cents),
                 formatCents(m_feeCapSpin->value())));
    } else {
        m_paymasterCostValue->setText(DigiDollarSendWidget::tr("Exact service fee not available in this status snapshot"));
    }
    if (snapshot.has_expiry) {
        m_paymasterExpiryValue->setText(
            QDateTime::fromSecsSinceEpoch(snapshot.expires_at)
                .toLocalTime().toString(Qt::ISODate));
    } else {
        m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
    }
    if (IsTerminalPaymasterState(m_paymasterSessionState)) {
        stopPaymasterPolling();
    } else if (previous_state != m_paymasterSessionState) {
        m_paymasterPollIntervalMs = 1500;
    }
    updatePaymasterFocusMode();
    applyPaymasterPrivacy();
    return true;
}

bool PaymasterSendWidget::handleAuthoritativePaymasterCompletion(
    const UniValue& result)
{
    const UniValue& embedded_session = result.find_value("session");
    const UniValue& completion_session = embedded_session.isObject()
        ? embedded_session : result;
    const UniValue& reported_final_value =
        completion_session.find_value("final");
    const bool reported_final = reported_final_value.isBool() &&
        reported_final_value.get_bool();
    const UniValue& status = completion_session.find_value("status");
    const bool payment_success = IsValidatedPaymasterCompletion(
        m_paymasterTransactionId, m_paymasterSessionState,
        status.isStr() ? QString::fromStdString(status.get_str()) : QString{},
        m_paymasterResultStatus, reported_final,
        completion_session.find_value("payment_confirmed").isTrue());
    const bool payment_state = payment_success ||
        m_paymasterSessionState == QStringLiteral("CONFIRMED");
    const bool recovery_state =
        m_paymasterSessionState == QStringLiteral("CANCELED_SAFE");
    const bool attention_state =
        m_paymasterSessionState == QStringLiteral("FAILED") ||
        m_paymasterSessionState == QStringLiteral("CONFLICTED");
    if (!payment_state && !recovery_state && !attention_state) return false;

    stopPaymasterPolling();
    m_paymasterRecoveryActive = false;
    if (attention_state) {
        if (m_paymasterUnsignedClosed) {
            m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Unsigned request closed; no payment was sent"));
            m_paymasterTerminalNoticeShown = true;
            applyPaymasterPrivacy();
            return true;
        }
        if (!m_paymasterTerminalNoticeShown) {
            m_paymasterStateValue->setText(
                DigiDollarSendWidget::tr("Paymaster transfer requires attention; no successful transfer was reported"));
            applyPaymasterPrivacy();
            m_paymasterTerminalNoticeShown = true;
        }
        return true;
    }
    if (m_paymasterTerminalNoticeShown) return true;

    if (payment_state) {
        int64_t recipient_cents{0};
        if (!payment_success ||
            !ReadInt64(completion_session, "payment_cents", recipient_cents) ||
            recipient_cents <= 0 ||
            recipient_cents > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
            (m_paymasterSessionState == QStringLiteral("CONFIRMED") &&
             m_paymasterConfirmationState !=
                 QStringLiteral("payment_confirmed"))) {
            m_paymasterStateValue->setText(
                DigiDollarSendWidget::tr("Recipient payment confirmation is unavailable"));
            m_form.showWarning(
                DigiDollarSendWidget::tr("Paymaster payment status unavailable"),
                DigiDollarSendWidget::tr("The stored session state alone does not prove payment. A current validated recipient amount, transaction and local confirmation are required; retained details may be unavailable."));
            m_paymasterTerminalNoticeShown = true;
            return true;
        }
        m_form.showSuccess(m_paymasterTransactionId, recipient_cents / 100.0);
        m_form.onClearClicked();
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Original payment confirmed"));
        m_form.updateBalance();
        m_paymasterTerminalNoticeShown = true;
        return true;
    }

    const UniValue& recovery = completion_session.find_value("payment_view").find_value("recovery");
    int64_t recovery_depth{0};
    if (!recovery.isObject() || !recovery.find_value("details_available").isTrue() ||
        !recovery.find_value("observation_available").isTrue() ||
        !ReadInt64(recovery, "confirmations", recovery_depth) || recovery_depth <= 0) {
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Recovery confirmation is unavailable"));
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster recovery status unavailable"),
            DigiDollarSendWidget::tr("This session is closed, but its recovery transaction is not currently confirmed by the local wallet observation."));
        m_paymasterTerminalNoticeShown = true;
        return true;
    }
    const UniValue& recovery_txid_value = recovery.find_value("txid");
    const QString recovery_txid = recovery_txid_value.isStr()
        ? QString::fromStdString(recovery_txid_value.get_str()) : QString{};
    if (m_paymasterConfirmationState !=
            QStringLiteral("recovery_confirmed") ||
        !IsCanonicalNonNullPaymasterHash(
            m_paymasterRecoveryTransactionId) ||
        !IsCanonicalNonNullPaymasterHash(recovery_txid) ||
        recovery_txid != m_paymasterRecoveryTransactionId) {
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Recovery status blocked: inconsistent confirmation"));
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster recovery status unavailable"),
            DigiDollarSendWidget::tr("Core reported a safely canceled session without one matching confirmed recovery transaction id."));
        m_paymasterTerminalNoticeShown = true;
        return true;
    }

    m_paymasterStateValue->setText(
        DigiDollarSendWidget::tr("Recovery confirmed; reserved inputs are safely resolved"));
    m_form.updateBalance();
    applyPaymasterPrivacy();
    m_form.showDialog(
        QMessageBox::Information,
        DigiDollarSendWidget::tr("Paymaster recovery confirmed"),
        m_privacy
            ? DigiDollarSendWidget::tr("The Paymaster recovery is confirmed. Details are hidden while privacy mode is enabled.")
            : DigiDollarSendWidget::tr("The Paymaster recovery transaction is confirmed.\n\nRecovery transaction ID: %1")
                  .arg(m_paymasterRecoveryTransactionId));
    m_paymasterTerminalNoticeShown = true;
    return true;
}

PaymasterConfirmationSelection PaymasterSendWidget::paymasterConfirmationSelection(
    const UniValue& result, const QString& address) const
{
    Q_UNUSED(address)
    const auto string_value = [&result](const char* key) {
        const UniValue& value = result.find_value(key);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    };
    PaymasterConfirmationSelection selection;
    selection.provider_id = string_value("provider_id");
    selection.offer_id = string_value("offer_id");
    selection.policy_hash = string_value("policy_hash");
    selection.funding_model = string_value("funding_model");
    selection.recipient = string_value("to_address");
    selection.authorization_commitment = string_value("authorization_commitment");
    ReadInt64(result, "payment_cents", selection.payment_cents);
    ReadInt64(result, "service_fee_cents", selection.service_fee_cents);
    ReadInt64(result, "user_total_cents", selection.user_total_cents);
    ReadInt64(result, "expires_at", selection.expires_at);
    return selection;
}

PaymasterSendWidget::OfferReviewResult PaymasterSendWidget::confirmPaymasterSelectionBeforeSigning(
    const UniValue& result, const QString& address)
{
    if (m_privacy) {
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster authorization paused"),
            DigiDollarSendWidget::tr("Disable privacy mode to review the exact recipient, provider, amount and authorization commitment before signing."));
        return OfferReviewResult::BLOCKED;
    }
    // Display values are taken from Core's bound selection, not recomputed from
    // the currently visible widgets. Any provider, model, recipient, amount, or
    // fee change therefore produces a new commitment and another confirmation.
    const PaymasterConfirmationSelection selection =
        paymasterConfirmationSelection(result, address);
    if (!selection.IsComplete() || selection.recipient != address) {
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Paymaster authorization blocked: incomplete exact offer details"));
        m_form.showWarning(
            DigiDollarSendWidget::tr("Paymaster authorization blocked"),
            DigiDollarSendWidget::tr("The exact provider, offer, policy, funding model, recipient, amount, "
                                     "service fee, total outflow, unexpired validity or validated authorization commitment was "
                                     "not returned consistently by the wallet. "
                                     "No Qt authorization will continue until those details are available."));
        return OfferReviewResult::BLOCKED;
    }
    if (selection.expires_at <= QDateTime::currentSecsSinceEpoch()) return OfferReviewResult::EXPIRED;
    if (!m_paymasterConfirmationGuard.RequiresConfirmation(selection)) return OfferReviewResult::ACCEPTED;

    QStringList changed_fields;
    for (const QString& field : m_paymasterConfirmationGuard.ChangedFields(selection)) {
        if (field == QStringLiteral("provider"))
            changed_fields.push_back(DigiDollarSendWidget::tr("provider"));
        else if (field == QStringLiteral("offer"))
            changed_fields.push_back(DigiDollarSendWidget::tr("offer"));
        else if (field == QStringLiteral("policy"))
            changed_fields.push_back(DigiDollarSendWidget::tr("provider policy"));
        else if (field == QStringLiteral("funding_model"))
            changed_fields.push_back(DigiDollarSendWidget::tr("funding model"));
        else if (field == QStringLiteral("recipient"))
            changed_fields.push_back(DigiDollarSendWidget::tr("recipient"));
        else if (field == QStringLiteral("amount"))
            changed_fields.push_back(DigiDollarSendWidget::tr("amount"));
        else if (field == QStringLiteral("service_fee"))
            changed_fields.push_back(DigiDollarSendWidget::tr("service fee"));
        else if (field == QStringLiteral("total"))
            changed_fields.push_back(DigiDollarSendWidget::tr("total wallet outflow"));
        else if (field == QStringLiteral("expiry"))
            changed_fields.push_back(DigiDollarSendWidget::tr("offer expiry"));
        else if (field == QStringLiteral("authorization_commitment")) {
            changed_fields.push_back(DigiDollarSendWidget::tr("transaction or capacity commitment"));
        }
    }
    const QString prompt = DigiDollarSendWidget::tr(
                               "Review the exact Paymaster offer before your wallet signs:\n\n"
                               "Provider: %1\nPayment model: %2\nRecipient: %3\n\n"
                               "Total $DD outflow: %6\nRecipient receives: %4\nProvider receives: %5\n"
                               "Spendable $DD remaining after this transfer: %7\n\n"
                               "Valid until: %8\n\nContinue to wallet unlock and signature?\n"
                               "Cancel closes this unsigned request and returns to payment settings after Core confirms that cancellation is safe.")
                               .arg(selection.provider_id, friendlyFundingModel(selection.funding_model), selection.recipient)
                               .arg(formatCents(selection.payment_cents))
                               .arg(DigiDollarSendWidget::tr("%1 (≈ %2%)").arg(formatCents(selection.service_fee_cents), PaymasterEffectivePercent(selection.service_fee_cents, selection.payment_cents)))
                               .arg(formatCents(selection.user_total_cents))
                               .arg(m_form.formatDDAmount(std::max(
                                   0.0, m_paymasterInitialAvailableBalance -
                                            selection.user_total_cents / 100.0)))
                               .arg(QDateTime::fromSecsSinceEpoch(selection.expires_at)
                                        .toLocalTime()
                                        .toString(Qt::ISODate));
    const QString details = DigiDollarSendWidget::tr("Technical authorization commitment: %1\nNew or changed fields: %2")
        .arg(selection.authorization_commitment, changed_fields.join(DigiDollarSendWidget::tr(", ")));
    const uint64_t wallet_generation = m_paymasterWalletGeneration;
    QElapsedTimer elapsed;
    elapsed.start();
    const qint64 initial_seconds = selection.expires_at - QDateTime::currentSecsSinceEpoch();
    const auto remaining = [&] {
        // A wall-clock rollback must not extend this reviewed offer.
        return std::max<qint64>(0, std::min(selection.expires_at - QDateTime::currentSecsSinceEpoch(),
                                          initial_seconds - elapsed.elapsed() / 1000));
    };
    const auto countdown = [&] {
        return DigiDollarSendWidget::tr("Time until offer expires: %1 seconds").arg(remaining());
    };
    QMessageBox::StandardButton answer{QMessageBox::Cancel};
    if (m_form.m_dialogHandlerForTesting) {
        answer = m_form.showDialog(QMessageBox::Question,
            DigiDollarSendWidget::tr("Confirm exact Paymaster authorization"), prompt + QStringLiteral("\n\n") + countdown() + QStringLiteral("\n\n") + details,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    } else {
        QMessageBox dialog(QMessageBox::Question,
            DigiDollarSendWidget::tr("Confirm exact Paymaster authorization"), prompt,
            QMessageBox::Yes | QMessageBox::Cancel, &m_form);
        dialog.setObjectName("paymasterOfferReviewDialog");
        dialog.setTextFormat(Qt::PlainText);
        dialog.setDefaultButton(QMessageBox::Cancel);
        dialog.setDetailedText(details);
        dialog.button(QMessageBox::Yes)->setText(DigiDollarSendWidget::tr("Approve payment"));
        dialog.setInformativeText(countdown());
        QTimer timer(&dialog);
        timer.setInterval(200);
        connect(&timer, &QTimer::timeout, &dialog, [&] {
            if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel || m_privacy) {
                dialog.reject();
                return;
            }
            dialog.setInformativeText(countdown());
            if (remaining() == 0) {
                dialog.button(QMessageBox::Yes)->setEnabled(false);
                dialog.reject();
            }
        });
        timer.start();
        answer = static_cast<QMessageBox::StandardButton>(dialog.exec());
    }
    if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel || m_privacy) return OfferReviewResult::BLOCKED;
    if (remaining() == 0) return OfferReviewResult::EXPIRED;
    if (answer != QMessageBox::Yes) {
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Canceling the unsigned request before returning to payment settings…"));
        return OfferReviewResult::CANCELED;
    }
    return m_paymasterConfirmationGuard.Accept(selection) ? OfferReviewResult::ACCEPTED : OfferReviewResult::BLOCKED;
}

bool PaymasterSendWidget::canContinueActivePaymasterSend() const
{
    if (!m_walletModel || !m_paymasterActiveSendParams.isArray() ||
        m_paymasterActiveSendParams.size() != 7 || !m_paymasterSessionPersisted ||
        !m_paymasterActiveSendStarted.isValid() || m_paymasterRecoveryActive) return false;
    const UniValue& options = m_paymasterActiveSendParams[6];
    const UniValue& request_id = options.find_value("request_id");
    if (!request_id.isStr() || request_id.get_str() != m_paymasterRequestId.toStdString()) return false;
    const UniValue& commitment = options.find_value("authorization_commitment");
    if (commitment.isNull() && options.find_value("prepare_only").isTrue()) {
        return m_paymasterAuthorizationCommitment.isEmpty() &&
               (m_paymasterArtifact.isEmpty() || m_paymasterArtifact == QStringLiteral("none")) &&
               (m_paymasterSessionState == QStringLiteral("CREATED") ||
                m_paymasterSessionState == QStringLiteral("INPUTS_RESERVED") ||
                m_paymasterSessionState == QStringLiteral("AWAITING_USER_SIGNATURE"));
    }
    // After explicit exact-offer approval, only finish the already signed
    // artifact. Never infer permission to sign from a restored status snapshot.
    return commitment.isStr() && !m_paymasterAuthorizationCommitment.isEmpty() &&
           commitment.get_str() == m_paymasterAuthorizationCommitment.toStdString() &&
           m_paymasterArtifact == QStringLiteral("user_psbt") &&
           (m_paymasterSessionState == QStringLiteral("AUTHORIZED") ||
            m_paymasterSessionState == QStringLiteral("PENDING_PROVIDER"));
}

void PaymasterSendWidget::pollPaymasterSession()
{
    if (m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    if (m_paymasterActiveRetryRequest == m_paymasterRequestId) {
        continuePaymasterRetry();
        return;
    }
    if (m_paymasterRecoveryActive && m_paymasterActiveRecoveryParams.isArray()) {
        if (!m_paymasterActiveRecoveryStarted.isValid() ||
            m_paymasterActiveRecoveryStarted.hasExpired(MAX_PAYMASTER_ACTIVE_SEND_MS)) {
            stopPaymasterPolling();
            m_paymasterRecoveryActive = false;
            updatePaymasterFocusMode();
            m_form.showWarning(DigiDollarSendWidget::tr("Recovery processing paused"),
                DigiDollarSendWidget::tr("Recovery did not finish within two minutes. Its authorization and reservations remain protected. Check the current status before explicitly continuing."));
            return;
        }
        setPaymasterBusy(true);
        executePaymasterRpcAsync("resolvepaymastersession", m_paymasterActiveRecoveryParams,
            [guard = QPointer<PaymasterSendWidget>(this)](UniValue result, QString error) {
                if (guard) guard->handleAlternativePaymasterRecoveryResult(result, error);
            });
        return;
    }
    if (canContinueActivePaymasterSend()) {
        if (m_paymasterActiveSendStarted.hasExpired(MAX_PAYMASTER_ACTIVE_SEND_MS)) {
            stopPaymasterPolling();
            m_form.showWarning(DigiDollarSendWidget::tr("Paymaster processing paused"),
                               DigiDollarSendWidget::tr("This request did not finish within two minutes. Its saved state and any existing authorization remain protected. Check the current status before choosing a resume or recovery action."));
            updatePaymasterFocusMode();
            return;
        }
        // Freeze all original fields, including the UUID, inputs, fee cap and
        // privacy and selection policy. No retry_transport or renewed
        // authorization is added. Core advances the same idempotent request.
        const UniValue params = m_paymasterActiveSendParams;
        const QString address = QString::fromStdString(params[0].get_str());
        const CAmount amount_cents = params[1].getInt<int64_t>();
        setPaymasterBusy(true);
        executePaymasterRpcAsync("senddigidollar", params,
                                 [guard = QPointer<PaymasterSendWidget>(this), address, amount_cents](UniValue result, QString error) {
                                     if (guard) guard->handlePaymasterResult(result, error, address, amount_cents);
                                 });
        return;
    }
    // Outside the explicitly initiated live request, polling only observes.
    // A reopened wallet must never sign, rebroadcast or resume recovery merely
    // because a timer fired.
    refreshPaymasterSessionForAction(
        QStringLiteral("refresh"),
        [guard = QPointer<PaymasterSendWidget>(this)] {
            if (guard) guard->schedulePaymasterPoll(/*state_changed=*/false);
        });
}

void PaymasterSendWidget::refreshPaymasterSessionState()
{
    if (!m_paymasterClosureReason.isEmpty()) {
        closeUnusablePaymasterOffer(m_paymasterClosureReason, /*allow_cancel=*/false);
        return;
    }
    // Polling observes durable state only. It does not authorize retries,
    // provider changes, cancellation, or recovery; those actions remain gated
    // by the artifact/state fields returned by resolvepaymastersession.
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            !m_walletModel ? DigiDollarSendWidget::tr("wallet is no longer available") :
            m_paymasterBusy ? DigiDollarSendWidget::tr("another Paymaster operation is still active") :
                              DigiDollarSendWidget::tr("no durable session is selected"));
        return;
    }
    if (canContinueActivePaymasterSend() || m_paymasterActiveRetryRequest == m_paymasterRequestId) {
        pollPaymasterSession();
        return;
    }
    refreshPaymasterSessionForAction(
        QStringLiteral("refresh"),
        [guard = QPointer<PaymasterSendWidget>(this)] {
            if (guard) guard->schedulePaymasterPoll(/*state_changed=*/true);
        });
}

void PaymasterSendWidget::stopPaymasterPolling()
{
    m_paymasterActiveRetryRequest.clear();
    m_paymasterActiveRetryStarted.invalidate();
    m_paymasterActiveRecoveryParams = UniValue{};
    m_paymasterActiveRecoveryStarted.invalidate();
    m_paymasterActiveSendParams = UniValue{};
    m_paymasterActiveSendStarted.invalidate();
    m_paymasterPollTimer->stop();
    m_paymasterPollIntervalMs = 1500;
    m_paymasterPollTimer->setProperty("paymasterPollScheduled", false);
}

void PaymasterSendWidget::schedulePaymasterPoll(bool state_changed)
{
    if (m_paymasterRequestId.isEmpty() ||
        IsTerminalPaymasterState(m_paymasterSessionState)) {
        stopPaymasterPolling();
        return;
    }
    if (state_changed) {
        m_paymasterPollIntervalMs = 1500;
        m_paymasterPollTimer->setProperty("paymasterPollScheduled", false);
    } else if (m_paymasterPollTimer->property(
                   "paymasterPollScheduled").toBool()) {
        m_paymasterPollIntervalMs = std::min(
            MAX_PAYMASTER_POLL_INTERVAL_MS,
            m_paymasterPollIntervalMs * 2);
    }
    m_paymasterPollTimer->setProperty("paymasterPollScheduled", true);
    m_paymasterPollTimer->start(m_paymasterPollIntervalMs);
    updatePaymasterFocusMode();
}

void PaymasterSendWidget::refreshPaymasterSessionForAction(
    const QString& required_action, std::function<void()> continuation)
{
    if (!m_walletModel) {
        reportPaymasterOperationNotStarted(DigiDollarSendWidget::tr("wallet is no longer available"));
        return;
    }
    if (m_paymasterBusy) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("another Paymaster operation is still active"));
        return;
    }
    if (m_paymasterRequestId.isEmpty() ||
        (!m_paymasterSessionPersisted && required_action != QStringLiteral("refresh"))) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("no durable session is selected"));
        return;
    }
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("refresh");
    // Explicit session actions supersede the in-memory live continuation.
    m_paymasterActiveRetryRequest.clear();
    m_paymasterActiveRetryStarted.invalidate();
    m_paymasterActiveRecoveryParams = UniValue{};
    m_paymasterActiveRecoveryStarted.invalidate();
    m_paymasterActiveSendParams = UniValue{};
    m_paymasterActiveSendStarted.invalidate();
    setPaymasterBusy(true);
    QPointer<PaymasterSendWidget> guard{this};
    executePaymasterRpcAsync("resolvepaymastersession", std::move(params),
        [guard, required_action,
         continuation = std::move(continuation)](
            UniValue result, QString error) mutable {
            if (!guard) return;
            if (!error.isEmpty()) {
                guard->m_form.showWarning(DigiDollarSendWidget::tr("Paymaster status unavailable"), error);
                guard->setPaymasterBusy(false);
                return;
            }
            QString decode_error;
            if (!guard->updatePaymasterSessionView(result, &decode_error)) {
                guard->stopPaymasterPolling();
                guard->m_form.showWarning(
                    DigiDollarSendWidget::tr("Paymaster status unavailable"),
                    DigiDollarSendWidget::tr("Core returned an incomplete or inconsistent Paymaster session (%1). No action was performed.")
                        .arg(decode_error));
                guard->setPaymasterBusy(false);
                return;
            }
            const bool terminal_handled =
                guard->handleAuthoritativePaymasterCompletion(result);
            const bool attention_state =
                guard->m_paymasterSessionState == QStringLiteral("FAILED") ||
                guard->m_paymasterSessionState == QStringLiteral("CONFLICTED");
            if (terminal_handled && (!attention_state || guard->m_paymasterUnsignedClosed)) {
                guard->setPaymasterBusy(false);
                if (guard->m_paymasterUnsignedClosed && required_action == QStringLiteral("abandon_unsigned")) {
                    guard->clearUnsignedPaymasterSession();
                }
                return;
            }
            if (!guard->m_paymasterAllowedActionsKnown ||
                !guard->m_paymasterAllowedActions.contains(required_action)) {
                guard->m_form.showWarning(
                    DigiDollarSendWidget::tr("Paymaster action no longer available"),
                    DigiDollarSendWidget::tr("Core no longer allows this action for the refreshed session. No wallet mutation was performed."));
                guard->setPaymasterBusy(false);
                return;
            }
            guard->setPaymasterBusy(false);
            if (continuation) continuation();
        });
}

void PaymasterSendWidget::retryPaymasterSession()
{
    if (!m_walletModel) {
        reportPaymasterOperationNotStarted(DigiDollarSendWidget::tr("wallet is no longer available"));
        return;
    }
    if (m_paymasterBusy) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("another Paymaster operation is still active"));
        return;
    }
    if (m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            DigiDollarSendWidget::tr("no durable request is selected"));
        return;
    }
    refreshPaymasterSessionForAction(
        QStringLiteral("retry_same"),
        [guard = QPointer<PaymasterSendWidget>(this)] {
            if (!guard) return;
            if (guard->m_paymasterArtifact != QStringLiteral("user_psbt") &&
                guard->m_paymasterArtifact !=
                    QStringLiteral("final_transaction")) {
                guard->reportPaymasterOperationNotStarted(
                    DigiDollarSendWidget::tr("the refreshed session has no retryable signed artifact"));
                return;
            }
            guard->m_paymasterActiveRetryRequest = guard->m_paymasterRequestId;
            guard->m_paymasterActiveRetryStarted.start();
            guard->continuePaymasterRetry();
        });
}

void PaymasterSendWidget::continuePaymasterRetry()
{
    if (!m_walletModel || m_paymasterBusy || m_privacy ||
        m_paymasterActiveRetryRequest.isEmpty() ||
        m_paymasterActiveRetryRequest != m_paymasterRequestId) return;
    if (!m_paymasterActiveRetryStarted.isValid() ||
        m_paymasterActiveRetryStarted.hasExpired(MAX_PAYMASTER_ACTIVE_SEND_MS)) {
        stopPaymasterPolling();
        m_form.showWarning(DigiDollarSendWidget::tr("Exact retry paused"),
            DigiDollarSendWidget::tr("The provider result has not arrived within two minutes. The existing transfer remains protected. Check its status before choosing the next step."));
        return;
    }
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("retry_same");
    setPaymasterBusy(true);
    executePaymasterRpcAsync("resolvepaymastersession", std::move(params),
        [guard = QPointer<PaymasterSendWidget>(this)](UniValue result, QString error) {
            if (!guard) return;
            if (!error.isEmpty()) {
                guard->stopPaymasterPolling();
                guard->m_form.showWarning(DigiDollarSendWidget::tr("Exact retry paused"),
                    DigiDollarSendWidget::tr("The existing transfer could not be reconciled yet. It may already have completed; this message does not mean the payment failed. Check its status before creating another payment.\n\nTechnical details: %1").arg(error));
                guard->setPaymasterBusy(false);
                return;
            }
            QString decode_error;
            if (!guard->updatePaymasterSessionView(result, &decode_error)) {
                guard->stopPaymasterPolling();
                guard->m_form.showWarning(DigiDollarSendWidget::tr("Paymaster status unavailable"),
                    DigiDollarSendWidget::tr("Core returned an invalid retry snapshot (%1).").arg(decode_error));
                guard->setPaymasterBusy(false);
                return;
            }
            const bool completed = guard->handleAuthoritativePaymasterCompletion(result);
            if (guard->m_paymasterArtifact != QStringLiteral("user_psbt")) {
                guard->m_paymasterActiveRetryRequest.clear();
                guard->m_paymasterActiveRetryStarted.invalidate();
            }
            guard->setPaymasterBusy(false);
            if (!completed) guard->schedulePaymasterPoll(/*state_changed=*/false);
        });
}

void PaymasterSendWidget::fallbackPaymasterSession()
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            !m_walletModel ? DigiDollarSendWidget::tr("wallet is no longer available") :
            m_paymasterBusy ? DigiDollarSendWidget::tr("another Paymaster operation is still active") :
                              DigiDollarSendWidget::tr("no durable session is selected"));
        return;
    }
    refreshPaymasterSessionForAction(
        QStringLiteral("fallback"),
        [guard = QPointer<PaymasterSendWidget>(this)] {
            if (!guard) return;
            if (guard->m_paymasterArtifact != QStringLiteral("none")) {
                guard->reportPaymasterOperationNotStarted(
                    DigiDollarSendWidget::tr("the refreshed session is no longer unsigned"));
                return;
            }
            UniValue lookup{UniValue::VOBJ};
            lookup.pushKV("request_id",
                          guard->m_paymasterRequestId.toStdString());
            UniValue params{UniValue::VARR};
            params.push_back(std::move(lookup));
            params.push_back("fallback");
            params.push_back(UniValue{UniValue::VOBJ});
            guard->setPaymasterBusy(true);
            guard->executePaymasterRpcAsync(
                "resolvepaymastersession", std::move(params),
                [guard](UniValue result, QString error) {
                    if (!guard) return;
                    if (!error.isEmpty()) {
                        guard->m_paymasterStateValue->setText(
                            DigiDollarSendWidget::tr("Paymaster fallback unavailable: %1")
                                .arg(error));
                        guard->applyPaymasterPrivacy();
                        guard->setPaymasterBusy(false);
                        return;
                    }
                    guard->m_paymasterAuthorizationCommitment.clear();
                    guard->m_paymasterConfirmationGuard.Reset();
                    guard->m_paymasterRecoveryAuthorizationCommitment.clear();
                    guard->m_paymasterRecoveryMaximumServiceFeeCents = 0;
                    guard->m_paymasterRecoveryActive = false;
                    guard->m_paymasterRecoveryConfirmationGuard.Reset();
                    QString decode_error;
                    if (!guard->updatePaymasterSessionView(
                            result, &decode_error)) {
                        guard->stopPaymasterPolling();
                        guard->m_form.showWarning(
                            DigiDollarSendWidget::tr("Paymaster status unavailable"),
                            DigiDollarSendWidget::tr("Core returned an invalid fallback snapshot (%1).")
                                .arg(decode_error));
                        guard->setPaymasterBusy(false);
                        return;
                    }
                    guard->m_paymasterStateValue->setText(DigiDollarSendWidget::tr(
                        "Previous unsigned attempt closed. Core determines which actions remain available."));
                    guard->setPaymasterBusy(false);
                    if (guard->m_paymasterArtifact == QStringLiteral("none") &&
                        guard->m_paymasterAllowedActions.contains(QStringLiteral("resume"))) {
                        guard->executePaymasterTransfer(guard->m_paymasterAddress,
                                                       guard->m_paymasterAmountCents, false);
                    }
                });
        });
}

void PaymasterSendWidget::recoverPaymasterSessionToSelf()
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            !m_walletModel ? DigiDollarSendWidget::tr("wallet is no longer available") :
            m_paymasterBusy ? DigiDollarSendWidget::tr("another Paymaster operation is still active") :
                              DigiDollarSendWidget::tr("no durable session is selected"));
        return;
    }
    refreshPaymasterSessionForAction(
        QStringLiteral("cancel_to_self"),
        [guard = QPointer<PaymasterSendWidget>(this)] {
            if (!guard) return;
            // The refresh operation has completed, but this confirmation is
            // still part of the same user operation. Keep controls and the
            // poll timer paused across the modal dialog.
            guard->setPaymasterBusy(true);
            if (guard->m_paymasterSessionPrivacy.isEmpty()) {
                guard->m_form.showWarning(
                    DigiDollarSendWidget::tr("Paymaster recovery unavailable"),
                    DigiDollarSendWidget::tr("The authoritative original-session privacy profile is unavailable. Recovery will not continue because Qt must never guess or downgrade it."));
                guard->setPaymasterBusy(false);
                return;
            }
            const uint64_t wallet_generation = guard->m_paymasterWalletGeneration;
            const auto answer = guard->m_form.showDialog(
                QMessageBox::Warning,
                DigiDollarSendWidget::tr("Prepare alternative-provider self-recovery"),
                DigiDollarSendWidget::tr("The wallet will first select and authenticate a different recovery provider, then "
                          "show the exact wallet returns, provider, fees, privacy profile and Core authorization "
                          "commitment before any recovery transaction signature. The original authorized "
                          "payment may still confirm first; recovery is not final until confirmed.\n\n"
                          "Wallet unlock at this stage may create input-control proofs only. Core's prepare-only "
                          "gate cannot create the recovery transaction signature."),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (!guard || wallet_generation != guard->m_paymasterWalletGeneration || !guard->m_walletModel) return;
            if (answer != QMessageBox::Yes || guard->m_privacy) {
                guard->setPaymasterBusy(false);
                return;
            }

            guard->stopPaymasterPolling();
            guard->m_paymasterRecoveryAuthorizationCommitment.clear();
            guard->m_paymasterRecoveryConfirmationGuard.Reset();
            guard->m_paymasterRecoveryMaximumServiceFeeCents =
                guard->m_feeCapSpin->value();
            guard->m_paymasterRecoveryActive = true;
            guard->setPaymasterBusy(false);
            guard->executeAlternativePaymasterRecovery(
                /*allow_unlock=*/true);
        });
}

void PaymasterSendWidget::abandonUnsignedPaymasterSession()
{
    cancelUnsignedPaymasterSession(/*confirm=*/true);
}

void PaymasterSendWidget::cancelUnsignedPaymasterSession(bool confirm)
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty() ||
        !m_paymasterSessionPersisted) {
        reportPaymasterOperationNotStarted(
            !m_walletModel  ? DigiDollarSendWidget::tr("wallet is no longer available") :
            m_paymasterBusy ? DigiDollarSendWidget::tr("another Paymaster operation is still active") :
                              DigiDollarSendWidget::tr("no durable session is selected"));
        return;
    }
    refreshPaymasterSessionForAction(
        QStringLiteral("abandon_unsigned"),
        [guard = QPointer<PaymasterSendWidget>(this), confirm] {
            if (!guard) return;
            if (guard->m_paymasterArtifact != QStringLiteral("none")) {
                guard->reportPaymasterOperationNotStarted(
                    DigiDollarSendWidget::tr("the refreshed session is no longer unsigned"));
                return;
            }
            guard->setPaymasterBusy(true);
            const uint64_t wallet_generation = guard->m_paymasterWalletGeneration;
            const auto answer = confirm ? guard->m_form.showDialog(
                                              QMessageBox::Question,
                                              DigiDollarSendWidget::tr("Cancel unsigned Paymaster transfer"),
                                              DigiDollarSendWidget::tr("Core will release the reserved $DD only if it can prove that no "
                                                                       "transaction signature or final transaction exists. If any spending "
                                                                       "authorization may exist, cancellation is refused and the protected "
                                                                       "recovery choices remain available.\n\nContinue?"),
                                              QMessageBox::Yes | QMessageBox::Cancel,
                                              QMessageBox::Cancel) :
                                          QMessageBox::Yes;
            if (!guard || wallet_generation != guard->m_paymasterWalletGeneration || !guard->m_walletModel) return;
            if (answer != QMessageBox::Yes || guard->m_privacy) {
                guard->setPaymasterBusy(false);
                return;
            }

            guard->stopPaymasterPolling();
            UniValue lookup{UniValue::VOBJ};
            lookup.pushKV("request_id",
                          guard->m_paymasterRequestId.toStdString());
            UniValue params{UniValue::VARR};
            params.push_back(std::move(lookup));
            params.push_back("abandon_unsigned");
            params.push_back(UniValue{UniValue::VOBJ});
            guard->setPaymasterBusy(true);
            guard->m_paymasterStateValue->setText(DigiDollarSendWidget::tr(
                "Verifying that the unsigned transfer can be canceled safely…"));
            guard->executePaymasterRpcAsync(
                "resolvepaymastersession", std::move(params),
                [guard](UniValue result, QString error) {
                    if (!guard) return;
                    if (!error.isEmpty()) {
                        guard->m_paymasterStateValue->setText(
                            DigiDollarSendWidget::tr("Cancellation outcome unavailable. Check current status before starting another transfer."));
                        guard->m_form.showWarning(
                            DigiDollarSendWidget::tr("Paymaster cancellation status unavailable"), error);
                        guard->setPaymasterBusy(false);
                        return;
                    }
                    QString decode_error;
                    if (!guard->updatePaymasterSessionView(result, &decode_error) ||
                        !guard->m_paymasterUnsignedClosed) {
                        guard->m_paymasterStateValue->setText(
                            DigiDollarSendWidget::tr("Cancellation returned an unexpected wallet state"));
                        guard->m_form.showWarning(
                            DigiDollarSendWidget::tr("Paymaster transfer was not cleared"),
                            DigiDollarSendWidget::tr("Core did not confirm a terminal unsigned FAILED state (%1).")
                                .arg(decode_error));
                        guard->setPaymasterBusy(false);
                        return;
                    }
                    guard->clearUnsignedPaymasterSession();
                });
        });
}

void PaymasterSendWidget::setPaymasterNotice(const QString& text)
{
    m_paymasterNotice->setText(text);
    m_paymasterNotice->setAccessibleDescription(text);
    m_paymasterNotice->setVisible(!m_privacy && !text.isEmpty());
}

void PaymasterSendWidget::closeUnusablePaymasterOffer(const QString& reason, bool allow_cancel)
{
    if (!m_walletModel || m_paymasterBusy || m_paymasterRequestId.isEmpty()) return;
    // This is a continuation of the user's live attempt, never a new payment.
    // A fresh Core capability is required even when the local countdown expired.
    m_paymasterClosureReason = reason;
    stopPaymasterPolling();
    m_paymasterAuthorizationCommitment.clear();
    m_paymasterConfirmationGuard.Reset();
    setPaymasterNotice(DigiDollarSendWidget::tr("%1 Checking whether the unsigned request can be closed safely…").arg(reason));
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(lookup);
    params.push_back("refresh");
    setPaymasterBusy(true);
    executePaymasterRpcAsync("resolvepaymastersession", std::move(params),
        [guard = QPointer<PaymasterSendWidget>(this), reason, allow_cancel, lookup](UniValue result, QString error) {
            if (!guard || guard->m_paymasterRequestId.toStdString() != lookup.find_value("request_id").get_str()) return;
            // The first prepare-only call may fail before creating a session.
            // Only Core's status-aware absence result can release this local
            // compose state. A known session disappearing remains an error.
            if (error == QStringLiteral("PAYMASTER_SESSION_NOT_FOUND: Paymaster session not found") &&
                !guard->m_paymasterSessionPersisted && guard->m_paymasterSessionId.isEmpty() &&
                (guard->m_paymasterArtifact.isEmpty() || guard->m_paymasterArtifact == QStringLiteral("none")) &&
                guard->m_paymasterTransactionId.isEmpty() && guard->m_paymasterRecoveryTransactionId.isEmpty()) {
                guard->clearClosedPaymasterSession();
                guard->m_paymasterStateValue->setText(DigiDollarSendWidget::tr("No Paymaster transfer was created"));
                guard->setPaymasterNotice(DigiDollarSendWidget::tr(
                    "%1 No Paymaster transfer was created. Recipient and amount were kept; review the payment or choose another funding method.").arg(reason));
                return;
            }
            QString decode_error;
            if (!error.isEmpty() || !guard->updatePaymasterSessionView(result, &decode_error)) {
                guard->setPaymasterNotice(DigiDollarSendWidget::tr(
                    "%1 The saved status could not be verified. Use Check current status before starting another payment.").arg(reason));
                guard->setPaymasterBusy(false);
                return;
            }
            if (guard->m_paymasterUnsignedClosed) {
                guard->clearUnsignedPaymasterSession();
                guard->m_paymasterClosureReason.clear();
                guard->setPaymasterNotice(DigiDollarSendWidget::tr(
                    "%1 The unsigned request is closed. No payment was sent for this request and it no longer reserves funds. Recipient and amount were kept; prepare a new offer when ready.").arg(reason));
                return;
            }
            if (guard->m_paymasterSessionState == QStringLiteral("CONFIRMED") ||
                guard->m_paymasterSessionState == QStringLiteral("CANCELED_SAFE")) {
                guard->m_paymasterClosureReason.clear();
                guard->setPaymasterNotice(QString{});
                guard->handleAuthoritativePaymasterCompletion(result);
                guard->setPaymasterBusy(false);
                return;
            }
            if (allow_cancel && !guard->m_privacy && guard->m_paymasterArtifact == QStringLiteral("none") &&
                guard->m_paymasterAllowedActionsKnown && guard->m_paymasterAllowedActions.contains(QStringLiteral("abandon_unsigned"))) {
                UniValue cancel{UniValue::VARR};
                cancel.push_back(lookup);
                cancel.push_back("abandon_unsigned");
                cancel.push_back(UniValue{UniValue::VOBJ});
                guard->executePaymasterRpcAsync("resolvepaymastersession", std::move(cancel),
                    [guard, reason](UniValue, QString) {
                        if (!guard) return;
                        guard->setPaymasterBusy(false);
                        // Also handles a lost/malformed cancellation reply. Read
                        // back the durable outcome; never repeat the mutation.
                        guard->closeUnusablePaymasterOffer(reason, /*allow_cancel=*/false);
                    });
                return;
            }
            guard->handleAuthoritativePaymasterCompletion(result);
            guard->setPaymasterNotice(DigiDollarSendWidget::tr(
                "%1 Core has not confirmed safe unsigned closure. An authorization or transaction may already exist. Keep this transfer and check its status; use only the recovery or cancellation actions shown below.").arg(reason));
            guard->setPaymasterBusy(false);
        });
}

void PaymasterSendWidget::clearUnsignedPaymasterSession()
{
    if (!m_paymasterUnsignedClosed) return;
    clearClosedPaymasterSession();
    setPaymasterNotice(DigiDollarSendWidget::tr(
        "The unsigned request is closed. No payment was sent for this request and it no longer reserves funds. Recipient and amount were kept."));
    m_paymasterStateValue->setText(
        DigiDollarSendWidget::tr("Unsigned transfer canceled; reserved $DD is available again"));
    m_paymasterCostValue->setText(DigiDollarSendWidget::tr("No service fee was authorized"));
}

void PaymasterSendWidget::clearClosedPaymasterSession()
{
    m_paymasterClosureReason.clear();
    stopPaymasterPolling();
    m_paymasterSendTemplate = UniValue{};
    m_paymasterRequestId.clear();
    m_paymasterSessionId.clear();
    m_paymasterSessionState.clear();
    m_paymasterUnsignedClosed = false;
    m_paymasterOfferReadyForReview = false;
    m_paymasterAttemptState.clear();
    m_paymasterArtifact.clear();
    m_paymasterPendingPhase.clear();
    m_paymasterBroadcastState.clear();
    m_paymasterConfirmationState.clear();
    m_paymasterTransactionId.clear();
    m_paymasterRecoveryTransactionId.clear();
    m_paymasterResultStatus.clear();
    m_paymasterResultSequence = -1;
    m_paymasterRecoveryExpiresAt = -1;
    m_paymasterTerminalNoticeShown = false;
    m_paymasterAllowedActions.clear();
    m_paymasterAllowedActionsKnown = false;
    m_paymasterSessionPersisted = false;
    m_paymasterAddress.clear();
    m_paymasterAuthorizationCommitment.clear();
    m_paymasterSessionPrivacy.clear();
    m_paymasterRecoveryAuthorizationCommitment.clear();
    m_paymasterAmount = 0.0;
    m_paymasterAmountCents = 0;
    m_paymasterRecoveryMaximumServiceFeeCents = 0;
    m_paymasterRecoveryActive = false;
    m_paymasterConfirmationGuard.Reset();
    m_paymasterRecoveryConfirmationGuard.Reset();
    m_paymasterStateValue->setText(DigiDollarSendWidget::tr("No active session"));
    m_paymasterIdentityValue->setText(DigiDollarSendWidget::tr("—"));
    m_paymasterCostValue->setText(
        DigiDollarSendWidget::tr("No service fee was authorized"));
    m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
    m_form.updateBalance();
    refreshClientSafetyStatus();
    updatePaymasterFocusMode();
    m_form.updateSendButton();
    applyPaymasterPrivacy();
    setPaymasterBusy(false);
}

UniValue PaymasterSendWidget::buildAlternativePaymasterRecoveryParams() const
{
    UniValue lookup{UniValue::VOBJ};
    lookup.pushKV("request_id", m_paymasterRequestId.toStdString());
    UniValue options{UniValue::VOBJ};
    options.pushKV("maximum_recovery_service_fee_cents",
                   m_paymasterRecoveryMaximumServiceFeeCents);
    if (m_paymasterRecoveryAuthorizationCommitment.isEmpty()) {
        options.pushKV("prepare_only", true);
    } else {
        options.pushKV("recovery_authorization_commitment",
                       m_paymasterRecoveryAuthorizationCommitment.toStdString());
    }
    UniValue params{UniValue::VARR};
    params.push_back(std::move(lookup));
    params.push_back("cancel_to_self");
    params.push_back(std::move(options));
    return params;
}

void PaymasterSendWidget::executeAlternativePaymasterRecovery(bool allow_unlock)
{
    if (!m_walletModel || m_paymasterBusy || !m_paymasterRecoveryActive ||
        m_paymasterRequestId.isEmpty()) {
        reportPaymasterOperationNotStarted(
            !m_walletModel ? DigiDollarSendWidget::tr("wallet is no longer available") :
            m_paymasterBusy ? DigiDollarSendWidget::tr("another Paymaster operation is still active") :
            !m_paymasterRecoveryActive ? DigiDollarSendWidget::tr("no recovery operation was approved") :
                                         DigiDollarSendWidget::tr("no durable session is selected"));
        return;
    }
    const uint64_t wallet_generation = m_paymasterWalletGeneration;
    setPaymasterBusy(true);
    std::shared_ptr<WalletModel::UnlockContext> unlock;
    if (allow_unlock) {
        unlock = m_walletModel->requestUnlockForAsync();
        if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel) return;
        if (!unlock->isValid()) {
            m_paymasterStateValue->setText(DigiDollarSendWidget::tr("RECOVERY_AWAITING_WALLET_UNLOCK"));
            m_paymasterRecoveryActive = false;
            setPaymasterBusy(false);
            return;
        }
    }
    m_paymasterStateValue->setText(
        m_paymasterRecoveryAuthorizationCommitment.isEmpty()
            ? DigiDollarSendWidget::tr("Preparing authenticated alternative recovery…")
            : DigiDollarSendWidget::tr("Submitting the exact confirmed recovery authorization…"));
    m_paymasterActiveRecoveryParams = buildAlternativePaymasterRecoveryParams();
    if (!m_paymasterActiveRecoveryStarted.isValid()) m_paymasterActiveRecoveryStarted.start();
    executePaymasterRpcAsync(
        "resolvepaymastersession", m_paymasterActiveRecoveryParams,
        [guard = QPointer<PaymasterSendWidget>(this), unlock = std::move(unlock)](
            UniValue result, QString error) mutable {
            unlock.reset();
            if (!guard) return;
            guard->handleAlternativePaymasterRecoveryResult(result, error);
        });
}

PaymasterRecoveryConfirmationSelection
PaymasterSendWidget::paymasterRecoveryConfirmationSelection(
    const UniValue& recovery) const
{
    const auto string_value = [&recovery](const char* key) {
        const UniValue& value = recovery.find_value(key);
        return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
    };
    const auto number_value = [&recovery](const char* key) {
        qint64 value{-1};
        return ReadInt64(recovery, key, value) ? value : qint64{-1};
    };

    PaymasterRecoveryConfirmationSelection selection;
    selection.recovery_provider_id = string_value("recovery_provider_id");
    selection.privacy_profile = string_value("privacy_profile");
    selection.offer_id = string_value("offer_id");
    selection.policy_hash = string_value("policy_hash");
    selection.original_commit_key = string_value("original_commit_key");
    selection.original_template_commitment =
        string_value("original_template_commitment");
    selection.authorization_commitment = string_value("authorization_commitment");
    selection.maximum_service_fee_cents =
        number_value("maximum_service_fee_cents");
    selection.service_fee_cents = number_value("service_fee_cents");
    selection.network_fee_satoshis = number_value("network_fee_satoshis");
    selection.expires_at = number_value("expires_at");

    const UniValue& wallet_returns = recovery.find_value("wallet_returns");
    if (wallet_returns.isArray() && wallet_returns.size() <= 64) {
        int output_index{0};
        qint64 total_return_cents{0};
        for (const UniValue& wallet_return : wallet_returns.getValues()) {
            if (!wallet_return.isObject()) {
                selection.wallet_returns.push_back(QString{});
                continue;
            }
            const UniValue& script = wallet_return.find_value("script_pub_key");
            const UniValue& address = wallet_return.find_value("address");
            qint64 amount_cents{-1};
            const std::string script_hex =
                script.isStr() ? script.get_str() : std::string{};
            const QString return_address = address.isStr()
                ? QString::fromStdString(address.get_str()) : QString{};
            if (script_hex.empty() || script_hex.size() > 20000 ||
                script_hex.size() % 2 != 0 || !IsHex(script_hex) ||
                return_address.size() > 160 ||
                !ReadInt64(wallet_return, "amount_cents", amount_cents) ||
                amount_cents <= 0 ||
                amount_cents > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
                total_return_cents >
                    std::numeric_limits<qint64>::max() - amount_cents) {
                selection.wallet_returns.push_back(QString{});
                continue;
            }
            total_return_cents += amount_cents;
            selection.wallet_returns.push_back(
                DigiDollarSendWidget::tr("Output %1: %2 cents to %3 (script %4)")
                    .arg(++output_index)
                    .arg(amount_cents)
                    .arg(!return_address.isEmpty()
                             ? return_address
                             : DigiDollarSendWidget::tr("fresh wallet-owned script"))
                    .arg(QString::fromStdString(script_hex)));
        }
    } else if (!wallet_returns.isNull()) {
        selection.wallet_returns.push_back(QString{});
    }
    return selection;
}

bool PaymasterSendWidget::confirmPaymasterRecoveryBeforeSigning(
    const PaymasterRecoveryConfirmationSelection& selection)
{
    if (m_privacy) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization paused by privacy mode"),
            DigiDollarSendWidget::tr("Disable privacy mode to review the exact recovery provider, wallet returns, fees and authorization commitment before signing."));
        return false;
    }
    if (!selection.IsComplete()) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization blocked: incomplete exact manifest"),
            DigiDollarSendWidget::tr("Core did not return every exact RecoveryAuthorization field required for "
               "review. No recovery transaction signature or automatic retry will continue."));
        return false;
    }
    if (selection.privacy_profile != m_paymasterSessionPrivacy) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization blocked: privacy changed"),
            DigiDollarSendWidget::tr("The recovery privacy profile (%1) does not exactly match the original "
               "session profile (%2). Qt never permits a recovery privacy downgrade or override.")
                .arg(selection.privacy_profile, m_paymasterSessionPrivacy));
        return false;
    }
    if (!m_paymasterRecoveryConfirmationGuard.RequiresConfirmation(selection)) {
        return true;
    }

    QStringList changed_fields;
    for (const QString& field :
         m_paymasterRecoveryConfirmationGuard.ChangedFields(selection)) {
        if (field == QStringLiteral("recovery_provider")) changed_fields.push_back(DigiDollarSendWidget::tr("recovery provider"));
        else if (field == QStringLiteral("privacy")) changed_fields.push_back(DigiDollarSendWidget::tr("privacy"));
        else if (field == QStringLiteral("offer")) changed_fields.push_back(DigiDollarSendWidget::tr("offer"));
        else if (field == QStringLiteral("policy")) changed_fields.push_back(DigiDollarSendWidget::tr("policy"));
        else if (field == QStringLiteral("original_commit")) changed_fields.push_back(DigiDollarSendWidget::tr("original commit"));
        else if (field == QStringLiteral("original_template_commitment")) changed_fields.push_back(DigiDollarSendWidget::tr("original template"));
        else if (field == QStringLiteral("wallet_returns")) changed_fields.push_back(DigiDollarSendWidget::tr("wallet returns"));
        else if (field == QStringLiteral("maximum_service_fee")) changed_fields.push_back(DigiDollarSendWidget::tr("maximum service fee"));
        else if (field == QStringLiteral("service_fee")) changed_fields.push_back(DigiDollarSendWidget::tr("service fee"));
        else if (field == QStringLiteral("network_fee")) changed_fields.push_back(DigiDollarSendWidget::tr("network fee"));
        else if (field == QStringLiteral("expiry")) changed_fields.push_back(DigiDollarSendWidget::tr("expiry"));
        else if (field == QStringLiteral("authorization_commitment")) changed_fields.push_back(DigiDollarSendWidget::tr("authorization commitment"));
    }

    const QString prompt = DigiDollarSendWidget::tr(
        "Review the exact alternative-provider cancel-to-self authorization before any recovery transaction signature:\n\n"
        "Recovery provider: %1\nPrivacy: %2\nOffer: %3\nPolicy: %4\n"
        "Original commit: %5\nOriginal template: %6\n\nWallet-owned returns:\n%7\n\n"
        "Maximum service fee: %8 cents\nService fee: %9 cents\nNetwork fee: %10 satoshis\n"
        "Expires: %11\nAuthorization commitment: %12\n\nChanged fields: %13\n\n"
        "Continue to wallet unlock and the exact recovery signature?")
        .arg(selection.recovery_provider_id, selection.privacy_profile,
             selection.offer_id, selection.policy_hash,
             selection.original_commit_key,
             selection.original_template_commitment,
             selection.wallet_returns.join(QLatin1Char('\n')))
        .arg(selection.maximum_service_fee_cents)
        .arg(selection.service_fee_cents)
        .arg(selection.network_fee_satoshis)
        .arg(QDateTime::fromSecsSinceEpoch(selection.expires_at)
                 .toLocalTime().toString(Qt::ISODate))
        .arg(selection.authorization_commitment)
        .arg(changed_fields.join(DigiDollarSendWidget::tr(", ")));
    if (m_form.showDialog(
            QMessageBox::Question,
            DigiDollarSendWidget::tr("Confirm exact Paymaster recovery authorization"), prompt,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        m_paymasterRecoveryActive = false;
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Recovery authorization paused by user"));
        return false;
    }
    return m_paymasterRecoveryConfirmationGuard.Accept(selection);
}

void PaymasterSendWidget::blockAlternativePaymasterRecovery(
    const QString& reason, const QString& detail)
{
    stopPaymasterPolling();
    m_paymasterRecoveryActive = false;
    m_paymasterStateValue->setText(reason);
    m_form.showWarning(DigiDollarSendWidget::tr("Paymaster recovery blocked"), detail);
    setPaymasterBusy(false);
}

void PaymasterSendWidget::handleAlternativePaymasterRecoveryResult(
    const UniValue& result, const QString& error)
{
    if (!error.isEmpty()) {
        m_paymasterRecoveryActive = false;
        stopPaymasterPolling();
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Paymaster recovery failed: %1").arg(error));
        // Keep the backend's stable error text unchanged so RPC and Qt expose
        // the same recovery/security failure.
        m_form.showWarning(DigiDollarSendWidget::tr("Paymaster recovery"), error);
        setPaymasterBusy(false);
        return;
    }

    QString decode_error;
    if (!updatePaymasterSessionView(result, &decode_error)) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery status blocked: malformed Core response"),
            DigiDollarSendWidget::tr("Core returned an incomplete or inconsistent authoritative recovery snapshot (%1). No signature or automatic retry will continue.")
                .arg(decode_error));
        return;
    }
    if (handleAuthoritativePaymasterCompletion(result)) {
        setPaymasterBusy(false);
        return;
    }

    const UniValue& recovery = result.find_value("recovery");
    if (!recovery.isObject()) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization blocked: missing recovery manifest"),
            DigiDollarSendWidget::tr("Core did not return the validated alternative recovery object. No "
               "signature or automatic retry will continue."));
        return;
    }

    const PaymasterRecoveryConfirmationSelection selection =
        paymasterRecoveryConfirmationSelection(recovery);
    const QString returned_commitment = selection.authorization_commitment;
    if (!m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        (returned_commitment.isEmpty() ||
         returned_commitment != m_paymasterRecoveryAuthorizationCommitment)) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization blocked: commitment changed or missing"),
            DigiDollarSendWidget::tr("Core did not return the exact RecoveryAuthorization commitment that was "
               "confirmed. No signature or automatic retry will continue."));
        return;
    }
    const UniValue& expired = recovery.find_value("expired");
    const bool expiry_elapsed = selection.expires_at > 0 &&
        selection.expires_at <= QDateTime::currentSecsSinceEpoch();
    if (!expired.isBool() || selection.expires_at <= 0 ||
        expired.get_bool() != expiry_elapsed) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization blocked: inconsistent expiry"),
            DigiDollarSendWidget::tr("Core returned inconsistent recovery expiry fields. Qt will not guess which authorization is current."));
        return;
    }
    if (expired.get_bool()) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization expired"),
            DigiDollarSendWidget::tr("The exact alternative recovery authorization expired. Start a fresh "
               "two-stage review; Qt will not reuse the old acceptance."));
        return;
    }

    const UniValue& accepted_value = recovery.find_value("authorization_accepted");
    if (!accepted_value.isNull() && !accepted_value.isBool()) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization blocked: invalid acceptance state"),
            DigiDollarSendWidget::tr("Core returned a malformed recovery authorization acceptance flag."));
        return;
    }
    const bool authorization_accepted = accepted_value.isBool() &&
                                        accepted_value.get_bool();
    if (m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        !returned_commitment.isEmpty()) {
        const uint64_t wallet_generation = m_paymasterWalletGeneration;
        const bool accepted = confirmPaymasterRecoveryBeforeSigning(selection);
        if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel) return;
        if (!accepted || m_privacy) {
            stopPaymasterPolling();
            setPaymasterBusy(false);
            return;
        }
        m_paymasterRecoveryAuthorizationCommitment = returned_commitment;
        setPaymasterBusy(false);
        executeAlternativePaymasterRecovery(/*allow_unlock=*/true);
        return;
    }
    if (!m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        !authorization_accepted) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery authorization was not accepted"),
            DigiDollarSendWidget::tr("The wallet did not atomically accept the exact confirmed recovery "
               "commitment. Qt will not automatically repeat a signing attempt."));
        return;
    }

    const UniValue& phase_value = recovery.find_value("phase");
    const QString phase = phase_value.isStr()
        ? QString::fromStdString(phase_value.get_str())
        : QString{};
    if (phase != QStringLiteral("capacity_pending") &&
        phase != QStringLiteral("request_ready") &&
        phase != QStringLiteral("response_validated") &&
        phase != QStringLiteral("user_signed") &&
        phase != QStringLiteral("final_committed")) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery status blocked: unknown phase"),
            DigiDollarSendWidget::tr("Core returned an unknown alternative-recovery phase."));
        return;
    }

    const UniValue& broadcast = result.find_value("broadcast");
    const UniValue& broadcast_error = result.find_value("broadcast_error");
    if ((!broadcast.isNull() && !broadcast.isBool()) ||
        (!broadcast_error.isNull() &&
         (!broadcast_error.isStr() ||
          broadcast_error.get_str().size() > 1024))) {
        blockAlternativePaymasterRecovery(
            DigiDollarSendWidget::tr("Recovery status blocked: malformed broadcast state"),
            DigiDollarSendWidget::tr("Core returned invalid recovery broadcast metadata."));
        return;
    }
    if (broadcast_error.isStr()) {
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Recovery %1; broadcast pending: %2")
                .arg(phase, QString::fromStdString(broadcast_error.get_str())));
    } else if (broadcast.isBool() && broadcast.get_bool()) {
        m_paymasterStateValue->setText(
            DigiDollarSendWidget::tr("Recovery broadcast; confirmation pending"));
    } else {
        m_paymasterStateValue->setText(
            phase.isEmpty()
                ? DigiDollarSendWidget::tr("Alternative recovery preparation pending")
                : DigiDollarSendWidget::tr("Alternative recovery: %1").arg(phase));
    }
    // Only the current explicit recovery can advance network work. A restored
    // session has no in-memory parameters and remains observation-only.
    const bool preparing = m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        (phase == QStringLiteral("capacity_pending") || phase == QStringLiteral("request_ready"));
    const bool finishing_signed = !m_paymasterRecoveryAuthorizationCommitment.isEmpty() &&
        authorization_accepted && phase == QStringLiteral("user_signed");
    if (!m_paymasterRecoveryActive || !m_paymasterAllowedActions.contains(QStringLiteral("cancel_to_self")) ||
        (!preparing && !finishing_signed)) {
        m_paymasterActiveRecoveryParams = UniValue{};
        m_paymasterActiveRecoveryStarted.invalidate();
        m_paymasterRecoveryActive = false;
    }
    setPaymasterBusy(false);
    applyPaymasterPrivacy();
    schedulePaymasterPoll(/*state_changed=*/false);
}

void PaymasterSendWidget::cancelPaymasterQuote()
{
    stopPaymasterPolling();
    m_paymasterRecoveryActive = false;
    m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Automatic checks stopped; durable reservations remain protected"));
    updatePaymasterFocusMode();
}

void PaymasterSendWidget::updateFeeDisplay()
{
    const auto input = m_form.paymentInput();
    const QString mode = feeMode();
    const double amount = input.amount_text.toDouble();
    const qint64 amount_cents = static_cast<qint64>(std::llround(amount * 100));
    const bool subtract_fee = paymasterModeSelected() &&
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked();
    if (m_feeCapPercent) {
        if (m_privacy) {
            m_feeCapPercent->setText(DigiDollarSendWidget::tr("Hidden by privacy mode"));
        } else if (!input.amount_valid) {
            m_feeCapPercent->setText(DigiDollarSendWidget::tr("Enter a recipient amount to see the equivalent percentage."));
        } else if (subtract_fee) {
            m_feeCapPercent->setText(DigiDollarSendWidget::tr("With fee deduction, the effective percentage is shown with the exact recipient amount in the offer."));
        } else {
            m_feeCapPercent->setText(DigiDollarSendWidget::tr("Fee limit ≈ %1% of the recipient amount. This is your ceiling, not the provider's price.")
                .arg(PaymasterEffectivePercent(m_feeCapSpin->value(), input.amount_cents)));
        }
    }
    const bool exact_preview = subtract_fee &&
        m_paymasterPreviewRecipientCents >= 0 &&
        m_paymasterPreviewServiceFeeCents >= 0 &&
        m_paymasterPreviewTotalCents == amount_cents;
    const QString recipient_amount = m_form.formatDDAmount(amount);
    const QString maximum_fee = formatCents(m_feeCapSpin->value());
    const QString maximum_outflow = subtract_fee
        ? m_form.formatDDAmount(amount)
        : m_form.formatDDAmount(amount + m_feeCapSpin->value() / 100.0);
    const QString remaining_balance = m_form.formatDDAmount(std::max(0.0, input.available_balance - amount));
    const QString fallback_recipient = exact_preview
        ? formatCents(m_paymasterPreviewRecipientCents)
        : DigiDollarSendWidget::tr("Calculated from the exact offer before signing");
    const QString fallback_fee = exact_preview
        ? formatCents(m_paymasterPreviewServiceFeeCents)
        : DigiDollarSendWidget::tr("Exact offer required; never more than %1").arg(maximum_fee);
    QString dgb_status;
    if (m_walletModel) {
        const CAmount dgb_balance = m_walletModel->getAvailableDGBBalance();
        const QString readable_balance = PaymasterFormatDGB(dgb_balance);
        dgb_status = DigiDollarSendWidget::tr("Available DGB: %1 DGB").arg(readable_balance);
        if (dgb_balance < COIN / 10) {
            dgb_status += DigiDollarSendWidget::tr(" — currently below the estimated network fee");
        }
    }

    if (mode == QStringLiteral("dgb")) {
        m_feeValue->setText(QStringLiteral("~0.1 DGB"));
        m_feeLabel->setText(DigiDollarSendWidget::tr("Transaction fee:"));
        m_feeModeExplanation->setText(DigiDollarSendWidget::tr(
            "Selected: Own DGB. No Paymaster and no additional $DD service fee will be used."));
        m_feeSummary->setText(DigiDollarSendWidget::tr(
            "<table cellspacing=\"3\">"
            "<tr><td><b>Recipient receives</b></td><td>%1</td></tr>"
            "<tr><td><b>Network fee</b></td><td>Own DGB (estimated ~0.1 DGB)</td></tr>"
            "<tr><td><b>Additional $DD fee</b></td><td>None</td></tr>"
            "<tr><td><b>Maximum wallet outflow</b></td><td>%1</td></tr>"
            "</table>").arg(recipient_amount));
        m_feeSummary->setToolTip(dgb_status);
    } else if (mode == QStringLiteral("auto")) {
        m_feeValue->setText(DigiDollarSendWidget::tr("~0.1 DGB, or at most %1").arg(maximum_fee));
        m_feeLabel->setText(DigiDollarSendWidget::tr("Fee funding:"));
        m_feeModeExplanation->setText(DigiDollarSendWidget::tr(
            "Selected: Automatic. Core first tries to pay approximately 0.1 DGB from this wallet. It looks for "
            "a Paymaster only if suitable DGB fee inputs are insufficient; wallet-lock and "
            "other errors never cause an automatic fallback."));
        if (subtract_fee) {
            m_feeSummary->setText(DigiDollarSendWidget::tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Exact total $DD outflow</b></td><td>%1</td></tr>"
                "<tr><td><b>With own DGB</b></td><td>Recipient receives %1; no $DD fee</td></tr>"
                "<tr><td><b>With Paymaster fallback</b></td><td>Recipient receives %2</td></tr>"
                "<tr><td><b>Paymaster service fee</b></td><td>%3</td></tr>"
                "<tr><td><b>Spendable $DD remaining</b></td><td>%4</td></tr>"
                "</table>%5")
                .arg(recipient_amount, fallback_recipient, fallback_fee,
                     remaining_balance,
                     m_sendAllSpendableDD
                         ? DigiDollarSendWidget::tr("<br><b>Wallet emptying:</b> every confirmed, ordinary spendable $DD input will be bound to this exact request.")
                         : QString{}));
        } else {
            m_feeSummary->setText(DigiDollarSendWidget::tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Recipient receives</b></td><td>%1</td></tr>"
                "<tr><td><b>Network fee</b></td><td>Own DGB first; Paymaster only if needed</td></tr>"
                "<tr><td><b>Additional $DD fee</b></td><td>None with own DGB; otherwise up to %2</td></tr>"
                "<tr><td><b>Maximum wallet outflow</b></td><td>%3</td></tr>"
                "</table>").arg(recipient_amount, maximum_fee, maximum_outflow));
        }
        m_feeSummary->setToolTip(dgb_status);
    } else {
        m_feeValue->setText(DigiDollarSendWidget::tr("Exact provider quote; at most %1").arg(maximum_fee));
        m_feeLabel->setText(DigiDollarSendWidget::tr("Service fee:"));
        m_feeModeExplanation->setText(DigiDollarSendWidget::tr(
            "Selected: Paymaster. A provider must supply the DGB network fee. A sponsored offer costs no "
            "$DD service fee; a user-paid offer may charge up to the limit shown below. "
            "The exact provider and fee are shown again before signing."));
        if (subtract_fee) {
            m_feeSummary->setText(DigiDollarSendWidget::tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Total $DD outflow</b></td><td>%1</td></tr>"
                "<tr><td><b>Recipient receives</b></td><td>%2</td></tr>"
                "<tr><td><b>Provider receives</b></td><td>%3</td></tr>"
                "<tr><td><b>Network fee</b></td><td>Paid in DGB by the selected provider</td></tr>"
                "<tr><td><b>Spendable $DD remaining</b></td><td>%4</td></tr>"
                "</table>%5")
                .arg(recipient_amount, fallback_recipient, fallback_fee,
                     remaining_balance,
                     m_sendAllSpendableDD
                         ? DigiDollarSendWidget::tr("<br><b>Wallet emptying:</b> every confirmed, ordinary spendable $DD input will be bound to this exact request.")
                         : QString{}));
        } else {
            m_feeSummary->setText(DigiDollarSendWidget::tr(
                "<table cellspacing=\"3\">"
                "<tr><td><b>Recipient receives</b></td><td>%1</td></tr>"
                "<tr><td><b>Network fee</b></td><td>Paid in DGB by the selected provider</td></tr>"
                "<tr><td><b>Additional $DD fee</b></td><td>0.00 $DD to %2</td></tr>"
                "<tr><td><b>Maximum wallet outflow</b></td><td>%3</td></tr>"
                "</table>").arg(recipient_amount, maximum_fee, maximum_outflow));
        }
        m_feeSummary->setToolTip(QString());
    }

    updateOfferCheckControls();
    QString amountText = input.amount_text;
    if (!amountText.isEmpty()) {
        double amount = amountText.toDouble();
        m_totalValue->setText(exact_preview
            ? formatCents(m_paymasterPreviewRecipientCents)
            : m_form.formatDDAmount(amount));
    } else {
        m_totalValue->setText(m_form.formatDDAmount(0));
    }

    if (m_privacy) {
        m_feeValue->setText(m_form.maskValue(m_feeValue->text()));
        m_totalValue->setText(m_form.maskValue(m_totalValue->text()));
        m_feeSummary->setText(m_form.maskValue(m_feeSummary->text()));
    }

}

QString PaymasterSendWidget::formatCents(qint64 cents) const
{
    // Keep the two decimal places exact without converting integer cents to a
    // floating-point amount. The UI uses one unit; RPC values remain cents.
    // Match the fixed decimal notation used by the recipient/outflow labels.
    return DigiDollarSendWidget::tr("%1 $DD").arg(PaymasterFormatDD(cents));
}

QString PaymasterSendWidget::friendlyFundingModel(const QString& model) const
{
    if (model == QStringLiteral("user_paid")) return DigiDollarSendWidget::tr("Service fee in $DD");
    if (model == QStringLiteral("sponsored")) return DigiDollarSendWidget::tr("Sponsored by provider");
    return model.isEmpty() ? DigiDollarSendWidget::tr("Not specified") : model;
}

bool PaymasterSendWidget::showConfirmationDialog(const QString& address, double amount)
{
    const auto input = m_form.paymentInput();
    if (m_privacy) {
        m_form.showWarning(
            DigiDollarSendWidget::tr("Transfer review paused"),
            DigiDollarSendWidget::tr("Disable privacy mode to review the exact recipient and amount before preparing or signing this transfer."));
        return false;
    }
    const QString mode = feeMode();
    const bool paymaster_only = mode == QStringLiteral("paymaster");
    const bool automatic = mode == QStringLiteral("auto");
    const bool subtract_fee = paymasterModeSelected() &&
        m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked();
    const QString title = paymaster_only
        ? DigiDollarSendWidget::tr("Request a Paymaster offer")
        : automatic ? DigiDollarSendWidget::tr("Confirm automatic fee funding")
                    : DigiDollarSendWidget::tr("Confirm send DigiDollar");
    QString question_string;

    question_string.append(paymaster_only
        ? DigiDollarSendWidget::tr("Do you want Core to prepare this Paymaster transfer?")
        : DigiDollarSendWidget::tr("Do you want to send this DigiDollar transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    question_string.append(paymaster_only
        ? DigiDollarSendWidget::tr("This step requests and verifies an offer. It does not sign or broadcast a transaction.")
        : DigiDollarSendWidget::tr("Please review the transfer before continuing."));
    question_string.append("</span>");

    question_string.append("<hr /><b>");
    question_string.append(paymaster_only ? DigiDollarSendWidget::tr("Planned transfer") : DigiDollarSendWidget::tr("Transfer"));
    question_string.append("</b><br />");
    if (subtract_fee) {
        question_string.append(DigiDollarSendWidget::tr("Exact maximum $DD outflow: %1").arg(m_form.formatDDAmount(amount)));
        question_string.append("<br />");
        if (automatic) {
            question_string.append(DigiDollarSendWidget::tr(
                "Recipient receives the full %1 when this wallet can pay in DGB. "
                "Only a Paymaster fallback deducts its exact service fee.")
                .arg(m_form.formatDDAmount(amount)));
        } else {
            question_string.append(DigiDollarSendWidget::tr(
                "The exact recipient amount is calculated from the authenticated offer. "
                "Recipient amount plus provider fee must equal %1 exactly.")
                .arg(m_form.formatDDAmount(amount)));
        }
        if (m_sendAllSpendableDD) {
            question_string.append("<br /><b>");
            question_string.append(DigiDollarSendWidget::tr(
                "Wallet emptying is enabled: every confirmed, ordinary spendable $DD input "
                "must still equal this total when Core reserves it."));
            question_string.append("</b>");
        }
    } else {
        question_string.append(DigiDollarSendWidget::tr("Recipient receives: %1").arg(m_form.formatDDAmount(amount)));
    }
    question_string.append("<br /><span style='font-family:monospace;'>");
    question_string.append(address.toHtmlEscaped());
    question_string.append("</span>");

    if (!input.selected_inputs.empty()) {
        const int selected_count = static_cast<int>(input.selected_inputs.size());
        const CAmount selected_amount = m_form.selectedDigiDollarAmount();
        question_string.append("<hr /><b>");
        question_string.append(DigiDollarSendWidget::tr("Selected $DD inputs"));
        question_string.append("</b>: ");
        question_string.append(DigiDollarSendWidget::tr("%1 input(s), %2 selected")
            .arg(selected_count)
            .arg(m_form.formatDDAmount(selected_amount / 100.0)));
    }

    question_string.append("<hr /><b>");
    question_string.append(DigiDollarSendWidget::tr("Selected fee method"));
    question_string.append("</b><br />");
    if (automatic) {
        qint64 effective_fee_cap = m_feeCapSpin->value();
        if (m_clientSafetyStatusKnown && m_clientSafetyConfigured) {
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyMaximumPerTransaction);
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyAvailableTodayCents);
        }
        question_string.append(DigiDollarSendWidget::tr("Automatic: use this wallet's DGB first."));
        question_string.append("<br />");
        question_string.append(DigiDollarSendWidget::tr(
            "Only if suitable DGB fee inputs are insufficient will Core request a Paymaster offer."));
        question_string.append("<br />");
        question_string.append(DigiDollarSendWidget::tr("Paymaster fee ceiling: %1").arg(formatCents(effective_fee_cap)));
        if (subtract_fee) {
            question_string.append("<br />");
            question_string.append(DigiDollarSendWidget::tr(
                "On Paymaster fallback, that exact fee is deducted from the total above; "
                "it is never added beyond the authorized outflow."));
        }
        question_string.append("<br />");
        question_string.append(DigiDollarSendWidget::tr(
            "This is a safety ceiling, not an expected fee. If Paymaster fallback is needed, "
            "a separate confirmation shows the exact provider, payment model, service fee and total before signing."));
    } else if (paymaster_only) {
        qint64 effective_fee_cap = m_feeCapSpin->value();
        if (m_clientSafetyStatusKnown && m_clientSafetyConfigured) {
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyMaximumPerTransaction);
            effective_fee_cap = std::min(effective_fee_cap,
                                         m_clientSafetyAvailableTodayCents);
        }
        question_string.append(DigiDollarSendWidget::tr("Paymaster required: a provider must supply the DGB network fee."));
        question_string.append("<br />");
        question_string.append(DigiDollarSendWidget::tr("Maximum permitted service fee for this request: %1")
                                   .arg(formatCents(effective_fee_cap)));
        if (subtract_fee) {
            question_string.append("<br />");
            question_string.append(DigiDollarSendWidget::tr(
                "The exact fee will be deducted from the entered total. Offers that cannot "
                "produce an exact cent-level split are rejected before inputs are reserved."));
        }
        question_string.append("<br /><span style='color:#aa0000; font-weight:bold;'>");
        question_string.append(DigiDollarSendWidget::tr("No service fee is authorized by this step."));
        question_string.append("</span><br />");
        question_string.append(DigiDollarSendWidget::tr(
            "Core now authenticates and selects an eligible offer within that ceiling. "
            "Before any payment signature, a separate confirmation shows the exact provider, "
            "payment model, service fee and maximum wallet outflow."));
    } else {
        question_string.append(DigiDollarSendWidget::tr("Own DGB: this wallet pays the estimated ~0.1 DGB network fee."));
        question_string.append("<br />");
        question_string.append(DigiDollarSendWidget::tr("No Paymaster and no additional $DD service fee will be used."));
    }

    question_string.append("<hr />");
    question_string.append(paymaster_only
        ? DigiDollarSendWidget::tr("Next action: request and verify an exact Paymaster offer.")
        : automatic ? DigiDollarSendWidget::tr("Next action: use DGB if possible, otherwise prepare an exact Paymaster offer.")
                    : DigiDollarSendWidget::tr("Next action: unlock the wallet if necessary, then sign and broadcast."));

    const QString confirm_button_text = paymaster_only
        ? DigiDollarSendWidget::tr("Find offer")
        : automatic ? DigiDollarSendWidget::tr("Continue") : DigiDollarSendWidget::tr("Send");
    auto confirmationDialog = new DDSendConfirmationDialog(
        title,
        question_string,
        QString{},
        DD_SEND_CONFIRM_DELAY,
        confirm_button_text,
        this
    );
    confirmationDialog->setObjectName(paymaster_only
        ? QStringLiteral("paymasterOfferRequestConfirmation")
        : QStringLiteral("digiDollarSendConfirmation"));
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);

    int result = confirmationDialog->exec();

    if (result == QMessageBox::Yes) {
        if (paymaster_only || automatic) {
            // Automatic mode may fall back to a Paymaster. Do not put a
            // Paymaster-capable recipient or amount into the standard log.
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: User approved Paymaster-capable transfer preparation after the required review\n");
        } else {
            LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: User confirmed transfer of %f DD to %s after 3-second review\n",
                      amount, address.toStdString());
        }
        return true;
    } else {
        LogPrint(BCLog::DIGIDOLLAR, "DigiDollar: User cancelled the send preparation\n");
        return false;
    }
}

void PaymasterSendWidget::setPaymasterSessionForTesting(
    const QString& state, const QString& artifact, bool persisted,
    const QString& address, double amount, const QString& attempt_state,
    const QString& pending_phase, const QStringList& allowed_actions,
    bool allowed_actions_known)
{
    m_paymasterRequestId = QStringLiteral("00000000-0000-4000-8000-000000000001");
    m_paymasterSessionId = QString(64, QLatin1Char('1'));
    m_paymasterSessionState = state;
    m_paymasterUnsignedClosed = false;
    m_paymasterOfferReadyForReview = state == QStringLiteral("AWAITING_USER_SIGNATURE") && attempt_state == QStringLiteral("QUOTED");
    m_paymasterArtifact = artifact;
    m_paymasterAttemptState = attempt_state;
    m_paymasterPendingPhase = pending_phase;
    m_paymasterAllowedActions = allowed_actions;
    m_paymasterAllowedActionsKnown = allowed_actions_known;
    m_paymasterRecoveryExpiresAt = -1;
    m_paymasterSessionPersisted = persisted;
    m_paymasterAddress = address;
    m_paymasterAmount = amount;
    m_paymasterAmountCents = static_cast<CAmount>(std::llround(amount * 100));
    updatePaymasterFocusMode();
}

void PaymasterSendWidget::setPaymasterRpcExecutorForTesting(
    PaymasterRpcExecutorForTesting executor)
{
    m_paymasterRpcExecutorForTesting = std::move(executor);
}

void PaymasterSendWidget::setPaymasterAsyncRpcExecutorForTesting(
    PaymasterAsyncRpcExecutorForTesting executor)
{
    m_paymasterAsyncRpcExecutorForTesting = std::move(executor);
}

void PaymasterSendWidget::applyPaymasterPrivacy()
{
    if (!m_paymasterSessionFrame || !m_offersTable) return;
    m_paymasterNotice->setVisible(!m_privacy && !m_paymasterNotice->text().isEmpty());

    const QString hidden = DigiDollarSendWidget::tr("Hidden while privacy mode is enabled");
    const auto mask_label = [this, &hidden](QLabel* label) {
        if (!label) return;
        constexpr auto MASKED = "paymasterPrivacyMasked";
        constexpr auto RAW_TEXT = "paymasterPrivacyRawText";
        constexpr auto RAW_TOOLTIP = "paymasterPrivacyRawTooltip";
        constexpr auto RAW_ACCESSIBLE = "paymasterPrivacyRawAccessible";
        constexpr auto RAW_INTERACTION = "paymasterPrivacyRawInteraction";
        const bool masked = label->property(MASKED).toBool();
        if (m_privacy) {
            // A status callback may update a label while it is already masked.
            // Preserve that new value without exposing it for one event loop.
            if (!masked || label->text() != hidden) {
                label->setProperty(RAW_TEXT, label->text());
                label->setProperty(RAW_TOOLTIP, label->toolTip());
                label->setProperty(RAW_ACCESSIBLE,
                                   label->accessibleDescription());
                label->setProperty(
                    RAW_INTERACTION,
                    static_cast<int>(label->textInteractionFlags()));
            }
            label->setText(hidden);
            label->setToolTip(QString{});
            label->setAccessibleDescription(QString{});
            label->setTextInteractionFlags(Qt::NoTextInteraction);
            label->setProperty(MASKED, true);
        } else if (masked) {
            label->setText(label->property(RAW_TEXT).toString());
            label->setToolTip(label->property(RAW_TOOLTIP).toString());
            label->setAccessibleDescription(
                label->property(RAW_ACCESSIBLE).toString());
            label->setTextInteractionFlags(Qt::TextInteractionFlags(
                label->property(RAW_INTERACTION).toInt()));
            label->setProperty(MASKED, false);
        }
    };

    for (QLabel* label : {m_paymasterTransferValue,
                          m_paymasterIdentityValue,
                          m_paymasterCostValue,
                          m_paymasterExpiryValue,
                          m_paymasterStateValue,
                          m_paymasterNextStepValue,
                          m_offersStatus,
                          m_offersUpdated,
                          m_clientSafetyStatus,
                          m_clientSafetyDetails,
                          m_sessionDiscoveryError}) {
        mask_label(label);
    }

    updateOfferCheckIcon();
    m_form.setPaymasterFormPrivacy(m_privacy);

    constexpr int RAW_TEXT_ROLE = Qt::UserRole + 70;
    constexpr int RAW_TOOLTIP_ROLE = Qt::UserRole + 71;
    constexpr int RAW_ACCESSIBLE_TEXT_ROLE = Qt::UserRole + 72;
    constexpr int RAW_ACCESSIBLE_DESCRIPTION_ROLE = Qt::UserRole + 73;
    constexpr int MASKED_ROLE = Qt::UserRole + 74;
    for (int row = 0; row < m_offersTable->rowCount(); ++row) {
        for (int column = 0; column < m_offersTable->columnCount(); ++column) {
            QTableWidgetItem* item = m_offersTable->item(row, column);
            if (!item) continue;
            const bool masked = item->data(MASKED_ROLE).toBool();
            if (m_privacy) {
                if (!masked || item->text() != hidden) {
                    item->setData(RAW_TEXT_ROLE, item->text());
                    item->setData(RAW_TOOLTIP_ROLE, item->toolTip());
                    item->setData(RAW_ACCESSIBLE_TEXT_ROLE,
                                  item->data(Qt::AccessibleTextRole));
                    item->setData(RAW_ACCESSIBLE_DESCRIPTION_ROLE,
                                  item->data(Qt::AccessibleDescriptionRole));
                }
                item->setText(hidden);
                item->setToolTip(QString{});
                item->setData(Qt::AccessibleTextRole, hidden);
                item->setData(Qt::AccessibleDescriptionRole, QString{});
                item->setData(MASKED_ROLE, true);
            } else if (masked) {
                item->setText(item->data(RAW_TEXT_ROLE).toString());
                item->setToolTip(item->data(RAW_TOOLTIP_ROLE).toString());
                item->setData(Qt::AccessibleTextRole,
                              item->data(RAW_ACCESSIBLE_TEXT_ROLE));
                item->setData(Qt::AccessibleDescriptionRole,
                              item->data(RAW_ACCESSIBLE_DESCRIPTION_ROLE));
                item->setData(MASKED_ROLE, false);
            }
        }
    }
    m_offersTable->setEnabled(!m_privacy);

    constexpr int RAW_COMBO_TEXT_ROLE = Qt::UserRole + 70;
    constexpr int COMBO_MASKED_ROLE = Qt::UserRole + 71;
    for (int index = 0; index < m_persistedPaymasterSessions->count(); ++index) {
        const bool masked = m_persistedPaymasterSessions
            ->itemData(index, COMBO_MASKED_ROLE).toBool();
        if (m_privacy) {
            const QString numbered_hidden =
                DigiDollarSendWidget::tr("Hidden persisted transfer %1").arg(index + 1);
            if (!masked || m_persistedPaymasterSessions->itemText(index) !=
                               numbered_hidden) {
                m_persistedPaymasterSessions->setItemData(
                    index, m_persistedPaymasterSessions->itemText(index),
                    RAW_COMBO_TEXT_ROLE);
            }
            m_persistedPaymasterSessions->setItemText(index,
                                                       numbered_hidden);
            m_persistedPaymasterSessions->setItemData(index, true,
                                                       COMBO_MASKED_ROLE);
        } else if (masked) {
            m_persistedPaymasterSessions->setItemText(
                index, m_persistedPaymasterSessions
                           ->itemData(index, RAW_COMBO_TEXT_ROLE).toString());
            m_persistedPaymasterSessions->setItemData(index, false,
                                                       COMBO_MASKED_ROLE);
        }
    }
    m_persistedPaymasterSessions->setEnabled(!m_privacy);
    m_loadPersistedPaymasterSessionButton->setEnabled(
        !m_privacy && !m_paymasterBusy && !m_sessionDiscoveryPending && m_walletModel);

    if (m_privacy) {
        m_paymasterTechnicalButton->setChecked(false);
        m_paymasterTechnicalDetails->hide();
    }
    m_paymasterTechnicalButton->setEnabled(!m_privacy && !m_paymasterBusy);
}

void PaymasterSendWidget::setWalletModel(WalletModel* model)
{
    const bool wallet_changed = m_walletModel != model;
    if (wallet_changed) {
        // Invalidate every wallet-bound callback before touching visible
        // state. This is the Paymaster-local operation token across close and
        // wallet switch boundaries.
        ++m_paymasterWalletGeneration;
        m_paymasterClosureReason.clear();
        setPaymasterNotice(QString{});
        m_sessionDiscoveryReady = false;
        m_sessionDiscoveryPending = false;
        m_sessionDiscoveryProgress->hide();
        m_sessionDiscoveryError->clear();
        m_sessionDiscoveryError->hide();
        m_paymasterSendTemplate = UniValue{};
        m_paymasterRestoredOptions = UniValue{UniValue::VOBJ};
        m_paymasterTransportState.clear();
        m_offerCheckState = OfferCheckState::NOT_CHECKED;
        m_offerExpiryTimer->stop();
        m_offersTable->setRowCount(0);
        m_offersStatus->setText(DigiDollarSendWidget::tr("Offers not checked for this wallet. Check offers or wait for the automatic check."));
        m_offersStatus->setAccessibleDescription(m_offersStatus->text());
        DigiDollarStatus::SetText(m_offersStatus, DigiDollarStatus::Kind::INFO);
        stopPaymasterPolling();
        m_paymasterRequestId.clear();
        m_paymasterSessionId.clear();
        m_paymasterSessionState.clear();
        m_paymasterUnsignedClosed = false;
        m_paymasterOfferReadyForReview = false;
        m_paymasterAttemptState.clear();
        m_paymasterArtifact.clear();
        m_paymasterPendingPhase.clear();
        m_paymasterBroadcastState.clear();
        m_paymasterConfirmationState.clear();
        m_paymasterTransactionId.clear();
        m_paymasterRecoveryTransactionId.clear();
        m_paymasterResultStatus.clear();
        m_paymasterResultSequence = -1;
        m_paymasterRecoveryExpiresAt = -1;
        m_paymasterTerminalNoticeShown = false;
        m_paymasterAllowedActions.clear();
        m_paymasterAllowedActionsKnown = false;
        m_paymasterSessionPersisted = false;
        m_paymasterAddress.clear();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterSessionPrivacy.clear();
        m_paymasterRecoveryAuthorizationCommitment.clear();
        m_paymasterAmount = 0.0;
        m_paymasterAmountCents = 0;
        m_paymasterInitialAvailableBalance = 0.0;
        m_paymasterPreviewRecipientCents = -1;
        m_paymasterPreviewServiceFeeCents = -1;
        m_paymasterPreviewTotalCents = -1;
        m_sendAllSpendableDD = false;
        if (m_subtractPaymasterFeeCheck) m_subtractPaymasterFeeCheck->setChecked(false);
        m_paymasterRecoveryMaximumServiceFeeCents = 0;
        m_paymasterRecoveryActive = false;
        m_paymasterConfirmationGuard.Reset();
        m_paymasterRecoveryConfirmationGuard.Reset();
        m_clientSafetyStatusKnown = false;
        m_clientSafetyConfigured = false;
        m_clientSafetyActiveReservations = 0;
        m_clientSafetyReservedCents = 0;
        m_clientSafetySpentTodayCents = 0;
        m_clientSafetyAvailableTodayCents = 0;
        m_clientSafetyError.clear();
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr("No active session"));
        m_paymasterIdentityValue->setText(DigiDollarSendWidget::tr("—"));
        m_paymasterCostValue->setText(DigiDollarSendWidget::tr("—"));
        m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
        m_paymasterSessionFrame->hide();
        m_persistedPaymasterSessions->clear();
        m_persistedPaymasterSessionsFrame->hide();
        invalidatePaymasterOfferPreview();
    }
    m_walletModel = model;

    if (m_walletModel) {
        // Connect wallet model signals
        m_form.updateBalance();
        refreshClientSafetyStatus();
        if (wallet_changed) discoverPersistedPaymasterSessions();
        // REMOVED: applyTheme() - Let CSS handle all theming
    } else {
        updateClientSafetyDisplay();
    }
    // Reset controls through the same path used by completed asynchronous
    // work. Assigning m_paymasterBusy directly left buttons disabled after a
    // wallet switch, while enabling them without a wallet was equally wrong.
    // Keep controls and polling paused while response parsing or a modal
    // authorization/error dialog is active. The unlock lease was already
    // released by executePaymasterTransfer()'s callback.
    if (wallet_changed) {
        setPaymasterBusy(false);
    } else {
        updatePaymasterFocusMode();
    }
    applyPaymasterPrivacy();
}

void PaymasterSendWidget::send(const QString& address, CAmount amount_cents)
{
    if (!m_walletModel || m_paymasterBusy) return;
    if (!m_sessionDiscoveryReady) {
        if (!m_sessionDiscoveryPending) discoverPersistedPaymasterSessions();
        return;
    }
    const double amount = amount_cents / 100.0;
    if (!m_clientSafetyStatusKnown || !m_clientSafetyConfigured) {
        m_form.showWarning(DigiDollarSendWidget::tr("Paymaster service-fee limits required"),
                    DigiDollarSendWidget::tr("Configure positive wallet-local Paymaster service-fee limits before "
                       "using Automatic or Paymaster fee funding."));
        return;
    }

    {
        const CAmount active_amount_cents = m_paymasterAmountCents;
        const bool new_request = m_paymasterRequestId.isEmpty();
        if (new_request) {
            m_paymasterClosureReason.clear();
            setPaymasterNotice(QString{});
        }

        // This is a compose-view guard, not an authorization decision. Existing
        // sessions keep their Core-provided continuation/recovery actions, and
        // Automatic with no own DGB needs the same current offer as Paymaster.
        if (new_request && (!hasFeeFundingCandidate() ||
                            (preparesPaymasterPayment() && m_form.paymentInput().amount_cents != amount_cents))) {
            m_form.updateSendButton();
            return;
        }

        if (!new_request &&
            (address != m_paymasterAddress || amount_cents != active_amount_cents)) {
            m_form.showWarning(
                DigiDollarSendWidget::tr("Paymaster transfer already in progress"),
                DigiDollarSendWidget::tr("The active Paymaster request is bound to %1 for %2.\n\n"
                   "Changing its recipient or amount cannot reuse the same authorization. "
                   "Finish or safely cancel the active request before starting a different transfer.")
                    .arg(m_paymasterAddress, m_form.formatDDAmount(m_paymasterAmount)));
            return;
        }

        // In explicit Paymaster mode, the clearly labelled Prepare payment
        // button authorizes preparation only. The exact provider/fee approval
        // remains mandatory. Automatic with own DGB can spend directly and
        // retains its ordinary transaction confirmation before the first RPC.
        // Privacy mode still requires exposing the payment fields for review.
        const uint64_t wallet_generation = m_paymasterWalletGeneration;
        if (new_request && (!preparesPaymasterPayment() || m_privacy) && !showConfirmationDialog(address, amount)) {
            return;
        }

        if (wallet_generation != m_paymasterWalletGeneration || !m_walletModel) return;
        if (new_request && !hasFeeFundingCandidate()) {
            m_form.updateSendButton();
            return;
        }
        if (new_request) {
            m_paymasterSendTemplate = UniValue{};
            m_paymasterRestoredOptions = UniValue{UniValue::VOBJ};
            m_paymasterRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            m_paymasterSessionPersisted = false;
            m_paymasterSessionId.clear();
            m_paymasterSessionState.clear();
            m_paymasterUnsignedClosed = false;
            m_paymasterOfferReadyForReview = false;
            m_paymasterAttemptState.clear();
            m_paymasterArtifact.clear();
            m_paymasterPendingPhase.clear();
            m_paymasterBroadcastState.clear();
            m_paymasterConfirmationState.clear();
            m_paymasterTransactionId.clear();
            m_paymasterRecoveryTransactionId.clear();
            m_paymasterResultStatus.clear();
            m_paymasterResultSequence = -1;
            m_paymasterRecoveryExpiresAt = -1;
            m_paymasterTerminalNoticeShown = false;
            m_paymasterAuthorizationCommitment.clear();
            m_paymasterSessionPrivacy = m_privacyCombo->currentData().toString();
            m_paymasterRecoveryAuthorizationCommitment.clear();
            m_paymasterRecoveryMaximumServiceFeeCents = 0;
            m_paymasterRecoveryActive = false;
            m_paymasterInitialAvailableBalance = m_form.paymentInput().available_balance;
            m_paymasterConfirmationGuard.Reset();
            m_paymasterRecoveryConfirmationGuard.Reset();
            m_paymasterStateValue->setText(DigiDollarSendWidget::tr("Preparing a new Paymaster request…"));
            m_paymasterIdentityValue->setText(DigiDollarSendWidget::tr("No provider selected yet"));
            m_paymasterCostValue->setText(DigiDollarSendWidget::tr("No service fee authorized yet"));
            m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
        }
        m_paymasterAddress = address;
        m_paymasterAmountCents = amount_cents;
        m_paymasterAmount = amount_cents / 100.0;
        executePaymasterTransfer(address, amount_cents,
                                 /*allow_unlock=*/false);
    }
}

void PaymasterSendWidget::resetEntry()
{
    m_paymasterClosureReason.clear();
    setPaymasterNotice(QString{});
    m_sendAllSpendableDD = false;
    m_paymasterInitialAvailableBalance = 0.0;
    m_paymasterPreviewRecipientCents = -1;
    m_paymasterPreviewServiceFeeCents = -1;
    m_paymasterPreviewTotalCents = -1;
    if (m_subtractPaymasterFeeCheck) m_subtractPaymasterFeeCheck->setChecked(false);
    const bool preserve_durable_session =
        m_paymasterSessionPersisted && !m_paymasterRequestId.isEmpty();
    if (!m_paymasterPollTimer->isActive() && !preserve_durable_session) {
        m_paymasterRequestId.clear();
        m_paymasterSessionId.clear();
        m_paymasterSessionState.clear();
        m_paymasterUnsignedClosed = false;
        m_paymasterOfferReadyForReview = false;
        m_paymasterAttemptState.clear();
        m_paymasterArtifact.clear();
        m_paymasterPendingPhase.clear();
        m_paymasterBroadcastState.clear();
        m_paymasterConfirmationState.clear();
        m_paymasterTransactionId.clear();
        m_paymasterRecoveryTransactionId.clear();
        m_paymasterResultStatus.clear();
        m_paymasterResultSequence = -1;
        m_paymasterRecoveryExpiresAt = -1;
        m_paymasterTerminalNoticeShown = false;
        m_paymasterSessionPersisted = false;
        m_paymasterAddress.clear();
        m_paymasterAuthorizationCommitment.clear();
        m_paymasterSessionPrivacy.clear();
        m_paymasterRecoveryAuthorizationCommitment.clear();
        m_paymasterAmount = 0.0;
        m_paymasterAmountCents = 0;
        m_paymasterRecoveryMaximumServiceFeeCents = 0;
        m_paymasterRecoveryActive = false;
        m_paymasterConfirmationGuard.Reset();
        m_paymasterRecoveryConfirmationGuard.Reset();
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr("No active session"));
        m_paymasterIdentityValue->setText(DigiDollarSendWidget::tr("—"));
        m_paymasterCostValue->setText(DigiDollarSendWidget::tr("—"));
        m_paymasterExpiryValue->setText(DigiDollarSendWidget::tr("—"));
        m_paymasterSessionFrame->hide();
    } else if (preserve_durable_session) {
        m_paymasterStateValue->setText(DigiDollarSendWidget::tr(
            "Entry fields cleared. The durable Paymaster transfer remains protected. "
            "Use ‘Cancel unsigned transfer and release $DD’ if no signature exists, "
            "or use the recovery actions shown here."));
    }
    updatePaymasterFocusMode();
}

bool PaymasterSendWidget::subtractFee() const
{
    return m_subtractPaymasterFeeCheck && m_subtractPaymasterFeeCheck->isChecked();
}

bool PaymasterSendWidget::DescribeBackendError(
    const QString& reasonFailed, QString& errorTitle, QString& errorMessage)
{
    if (reasonFailed.contains(QStringLiteral("PAYMASTER_CLIENT_SAFETY"))) {
        errorTitle = DigiDollarSendWidget::tr("Paymaster service-fee limits required");
        errorMessage = DigiDollarSendWidget::tr(
            "This wallet does not yet have valid Paymaster service-fee limits.\n\n"
            "Return to Network fee and select Set service-fee limits. No provider "
            "can be used until positive wallet-local limits are saved.\n\n"
            "Technical details: %1").arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_NO_EXACT_GROSS_OFFER"))) {
        errorTitle = DigiDollarSendWidget::tr("No exact Paymaster split available");
        errorMessage = DigiDollarSendWidget::tr(
            "No current offer can split this total exactly into a recipient amount and "
            "the provider's rounded service fee. No $DD was reserved or signed.\n\n"
            "Try a sponsored offer, another provider, or change the total by one cent.\n\n"
            "Technical details: %1").arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_SWEEP_BALANCE_CHANGED"))) {
        errorTitle = DigiDollarSendWidget::tr("Spendable $DD balance changed");
        errorMessage = DigiDollarSendWidget::tr(
            "The confirmed, ordinary spendable $DD inputs changed after Wallet emptying "
            "was previewed. Core stopped before authorizing a different total.\n\n"
            "Refresh the balance and start Wallet emptying again. Reserved, pending and "
            "Paymaster-pool $DD remain untouched.\n\nTechnical details: %1")
            .arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_NO_ELIGIBLE_OFFER"))) {
        errorTitle = DigiDollarSendWidget::tr("No suitable Paymaster available");
        errorMessage = DigiDollarSendWidget::tr(
            "No currently advertised provider matches this amount, fee limit and "
            "privacy selection.\n\nTry again later, raise only the limit you are "
            "comfortable paying, or select Own DGB.\n\nTechnical details: %1")
            .arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_PROXY_OR_ENDPOINT_UNREACHABLE"))) {
        errorTitle = DigiDollarSendWidget::tr("Paymaster provider unreachable");
        errorMessage = DigiDollarSendWidget::tr(
            "The provider endpoint or your configured proxy could not be reached. "
            "The provider may have stopped; its announcement can remain listed until it expires.\n\n"
            "If you use Tor, check your local Tor connection. Otherwise try again later "
            "or use another provider. Check the current transfer's status for safe retry "
            "or cancellation options.\n\nTechnical details: %1").arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_DIRECT_CONNECTION_FAILED"))) {
        errorTitle = DigiDollarSendWidget::tr("Paymaster connection failed");
        errorMessage = DigiDollarSendWidget::tr(
            "The secure connection to the provider could not be completed. "
            "A listed announcement does not confirm that the provider is currently reachable.\n\n"
            "Try again later or use another provider. Check the current transfer's status "
            "for safe retry or cancellation options.\n\nTechnical details: %1").arg(reasonFailed);
    } else if (reasonFailed.startsWith(QStringLiteral("PAYMASTER_DD_INPUT_SELECTION_FAILED: "))) {
        errorTitle = DigiDollarSendWidget::tr("Cannot fund the Paymaster payment");
        errorMessage = DigiDollarSendWidget::tr(
            "The payment and Paymaster fee cannot be funded with the current $DD inputs.\n\n%1")
            .arg(reasonFailed.mid(QStringLiteral("PAYMASTER_DD_INPUT_SELECTION_FAILED: ").size()));
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_INSUFFICIENT_USER_DD"))) {
        errorTitle = DigiDollarSendWidget::tr("Not enough $DD for the selected offer");
        errorMessage = DigiDollarSendWidget::tr(
            "The recipient amount is available, but this wallet cannot also cover the "
            "exact user-paid Paymaster service fee. No transaction was signed.\n\n"
            "Choose a lower-fee or sponsored offer, send a smaller amount, or select "
            "Own DGB.\n\nTechnical details: %1").arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_SAFETY_LIMIT_EXHAUSTED"))) {
        errorTitle = DigiDollarSendWidget::tr("Paymaster daily limit reached");
        errorMessage = DigiDollarSendWidget::tr(
            "This wallet's service-fee budget is already reserved or spent for the "
            "current rolling day. Existing sessions remain protected.\n\n"
            "Technical details: %1").arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_NODE_NOT_READY")) ||
               reasonFailed.contains(QStringLiteral("PAYMASTER_TXINDEX_NOT_READY"))) {
        errorTitle = DigiDollarSendWidget::tr("Paymaster is not ready");
        errorMessage = DigiDollarSendWidget::tr(
            "The node is not currently ready for Paymaster transfers. Check node "
            "synchronization, txindex and Paymaster prerequisites, or select Own DGB.\n\n"
            "Technical details: %1").arg(reasonFailed);
    } else if (reasonFailed.contains(QStringLiteral("PAYMASTER_SESSION_DATABASE_READ"))) {
        errorTitle = DigiDollarSendWidget::tr("Paymaster wallet data unavailable");
        errorMessage = DigiDollarSendWidget::tr(
            "Core could not safely read this wallet's persisted Paymaster session. "
            "Core stopped rather than continue without authoritative persisted session "
            "state. It did not create a replacement authorization; existing reservations "
            "or prior authorizations may remain protected.\n\n"
            "Do not delete the wallet or repeatedly start a new transfer. Preserve the "
            "wallet backup and debug log, restart Core once, and check the protected "
            "session again. If the error remains, use a verified wallet backup or seek "
            "technical support before changing the wallet files.\n\n"
            "Technical details: %1").arg(reasonFailed);
    } else {
        return false;
    }
    return true;
}
