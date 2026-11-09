// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERSENDCONTROLLER_H
#define DIGIBYTE_QT_PAYMASTERSENDCONTROLLER_H
#include <qt/paymasterasync.h>
#include <qt/paymasterconfirmation.h>
#include <consensus/amount.h>
#include <QStringList>
#include <functional>

class PaymasterWidgetTests;
namespace PaymasterQt {
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
    UniValue resume_params;
    QString transaction_id;
    QString recovery_transaction_id;
    QString result_status;
    qint64 result_sequence{-1};
    qint64 recovery_expires_at{-1};
    bool recovery_signed{false};
    qint64 created_at{0};
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

bool ReadInt64(const UniValue& object, const char* key, qint64& value);
bool IsKnownSessionState(const QString& state);
bool IsTerminalPaymasterState(const QString& state);

/** Wallet-scoped send workflow. Typed events select effects; the widget only
 * executes effects and presents dialogs. Core remains the authorization authority. */
class SendController {
    friend class ::PaymasterWidgetTests;
public:
    using Clock = std::function<qint64()>;
    class Elapsed {
    public:
        explicit Elapsed(const Clock& clock) : m_clock(clock) {}
        Elapsed(const Elapsed&) = default;
        Elapsed& operator=(const Elapsed& other) { m_started = other.m_started; return *this; }
        void start() { m_started = m_clock(); }
        void invalidate() { m_started = -1; }
        bool isValid() const { return m_started >= 0; }
        qint64 elapsed() const { return isValid() ? m_clock() - m_started : 0; }
        bool hasExpired(qint64 timeout) const { return isValid() && elapsed() > timeout; }
    private:
        const Clock& m_clock;
        qint64 m_started{-1};
    };

    explicit SendController(Clock clock = {});
    SendController(const SendController&) = delete;
    SendController& operator=(const SendController&) = delete;

    enum class Effect { NONE, OBSERVE, RETRY, RECOVER, CONTINUE, WAIT_HISTORY, CLOSE_UNSIGNED, TIMED_OUT };
    enum class PreparedEffect { MALFORMED_COMMITMENT, CHANGED_COMMITMENT, INVALID_AUTHORIZATION_FLAG, MISSING_COMMITMENT, REVIEW, UNLOCK, WAIT, RECONCILE_ARTIFACT, POLL };
    struct PreparedDecision { PreparedEffect effect; QString commitment; };
    PreparedDecision preparedReply(const UniValue& result);
    enum class RecoveryEffect { MISSING_MANIFEST, CHANGED_COMMITMENT, INVALID_EXPIRY, EXPIRED, INVALID_ACCEPTANCE, REVIEW, REJECTED, UNKNOWN_PHASE, INVALID_BROADCAST, POLL };
    struct RecoveryDecision { RecoveryEffect effect; QString phase; QString broadcast_error; bool broadcast{false}; };
    RecoveryDecision recoveryReply(const UniValue& result, const PaymasterRecoveryConfirmationSelection& selection, qint64 now);
    enum class CompletionKind { NONE, ATTENTION, UNSIGNED_CLOSED, ALREADY_SHOWN, PAYMENT_UNVERIFIED, PAYMENT, RECOVERY_UNVERIFIED, RECOVERY_MISMATCH, RECOVERY };
    struct Completion { CompletionKind kind; qint64 recipient_cents{0}; };
    Completion completion(const UniValue& result);
    CompletionKind directCompletion(const UniValue& result);
    struct PollEvent { bool wallet_available; bool history_refreshing; };
    struct ReplyToken { uint64_t wallet; uint64_t request; QString request_id; };
    struct PollOperation { Effect effect; UniValue params; ReplyToken token; };
    PollOperation beginPoll(const PollEvent& event);
    enum class ClosureEffect { CLOSED, COMPLETE, ABANDON, OBSERVE };
    struct ClosureOperation { ClosureEffect effect; UniValue params; };
    ClosureOperation unsignedClosure(bool allow_cancel, bool privacy);
    struct Continuation { UniValue params; Elapsed started; ReplyToken token; };
    Continuation savedContinuation() const;
    void restoreContinuation(const Continuation& continuation);
    Effect advance(const PollEvent& event) const;
    ReplyToken replyToken() const;
    bool accepts(const ReplyToken& token) const;
    ReplyToken startPreparation(UniValue params);
    bool receivePreparation(const ReplyToken& token, bool succeeded);
    bool applySnapshot(const PaymasterSessionSnapshot& snapshot, QString* error = nullptr);
    /** Revoke capabilities and live approvals after an unreadable observation,
     * retaining the durable request identity and known financial artifacts. */
    void observationFailed();
    bool canContinue(bool wallet_available) const;
    bool activeSendExpired() const;
    void historyWaiting(bool waiting);
    void stop();
    void walletChanged();
    int nextPoll(bool state_changed);
    bool allows(const QString& action) const;
    bool canCancelPreparation() const;
    bool requestCancellation();
    bool observeAbsence(const RpcError& error);
    bool acceptReview(const QString& commitment, uint64_t wallet_generation);
    void setBusy(bool busy) { m_state.busy = busy; }
    void clearReview();
    void clearRecoveryReview();
    bool clearAfterClosure();
    void fallbackCompleted();
    bool restoreRequest(const QString& request_id);
    bool beginTransfer(const QString& request_id, const QString& privacy);
    void bindInput(const QString& address, CAmount amount);
    bool beginRetry();
    void stopRetry();
    bool beginRecovery(qint64 maximum_fee);
    void stopRecovery() { m_state.recoveryActive = false; }
    void startRecoveryCall();
    void observeOnly();
    void finishCancellation() { m_state.preparationCancelRequested = false; }
    void markNoticeShown() { m_state.terminalNoticeShown = true; }
    void clearContinuations();
    void stopSendContinuation();
    ReplyToken beginContinuation();
    bool receiveContinuation(const ReplyToken& token);
    bool confirmSelection(const PaymasterConfirmationSelection& selection, uint64_t wallet_generation, qint64 now);
    bool confirmRecovery(const PaymasterRecoveryConfirmationSelection& selection, uint64_t wallet_generation, qint64 now);
    void resetPollDelay() { m_state.pollIntervalMs = 1500; }
    void setSnapshotForTesting(const PaymasterSessionSnapshot& snapshot, bool persisted);

    struct View {
        explicit View(const Clock& clock)
            : activeRecoveryStarted(clock), activeSendStarted(clock), activeRetryStarted(clock) {}
        UniValue activeRecoveryParams;
        Elapsed activeRecoveryStarted;
        UniValue sendTemplate;
        UniValue restoredSendParams;
        UniValue restoredOptions{UniValue::VOBJ};
        QString requestId;
        QString sessionId;
        QString sessionState;
        QString attemptState;
        QString artifact;
        QString pendingPhase;
        QString broadcastState;
        QString confirmationState;
        QString transactionId;
        QString recoveryTransactionId;
        QString resultStatus;
        qint64 resultSequence{-1};
        qint64 recoveryExpiresAt{-1};
        bool recoverySigned{false};
        qint64 sessionCreatedAt{0};
        QStringList allowedActions;
        QString address;
        QString authorizationCommitment;
        QString sessionPrivacy;
        QString recoveryAuthorizationCommitment;
        double amount{0.0};
        CAmount amountCents{0};
        uint64_t walletGeneration{0};
        uint64_t sendGeneration{0};
        qint64 recoveryMaximumServiceFeeCents{0};
        bool busy{false};
        bool preparationCancelRequested{false};
        bool sessionPersisted{false};
        bool unsignedClosed{false};
        bool offerReadyForReview{false};
        bool recoveryActive{false};
        int pollIntervalMs{1500};
        bool allowedActionsKnown{false};
        bool observationAvailable{false};
        bool terminalNoticeShown{false};
        PaymasterConfirmationGuard confirmationGuard;
        PaymasterRecoveryConfirmationGuard recoveryConfirmationGuard;
        UniValue activeSendParams;
        Elapsed activeSendStarted;
        qint64 historyPausedAt{-1};
        qint64 historyPausedMs{0};
        QString activeRetryRequest;
        Elapsed activeRetryStarted;
    };
    const View& view() const { return m_state; }

    struct FailureDecision {
        bool invalidate_review{false};
        bool discard_uncreated_request{false};
        bool allow_unsigned_cancel{false};
    };
    static bool decodeSnapshot(const UniValue& result, PaymasterSessionSnapshot& snapshot, QString& error);
    static FailureDecision failure(const RpcError& error, bool persisted, bool known_unsigned);
    FailureDecision failedReply(const RpcError& error);

private:
    void resetSession();
    void invalidateClosure() { m_completion_verified = false; m_absence_verified = false; }
    bool m_completion_verified{false};
    bool m_absence_verified{false};
    Clock m_clock;
    bool m_poll_scheduled{false};
    static constexpr int MAX_PAYMASTER_ACTIVE_SEND_MS{120000};
    View m_state{m_clock};
};
} // namespace PaymasterQt
#endif // DIGIBYTE_QT_PAYMASTERSENDCONTROLLER_H
