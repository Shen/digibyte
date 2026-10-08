// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_PAYMASTERSENDWIDGET_H
#define DIGIBYTE_QT_PAYMASTERSENDWIDGET_H

#include <qt/digidollarstatus.h>
#include <qt/paymasterconfirmation.h>
#include <qt/walletmodel.h>

#include <univalue.h>

#include <QElapsedTimer>
#include <QWidget>
#include <QStringList>
#include <cstdint>
#include <functional>

class DigiDollarSendWidget;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QButtonGroup;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QHideEvent;
class QShowEvent;
class QRadioButton;
class QProgressBar;
class QResizeEvent;
class QSpinBox;
class QTableWidget;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

/** Owns fee selection and the Paymaster client presentation state. Core remains
 * authoritative for authorization, reservations and durable payment sessions.
 * The parent owns editable payment fields; this component reads snapshots and
 * uses its presentation hooks without retaining a second editable form.
 */
class PaymasterSendWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit PaymasterSendWidget(DigiDollarSendWidget& form);
    void amountChanged(bool setting_sweep);
    void beginSweep();
    void setPrivacy(bool privacy);
    bool isBusy() const { return m_paymasterBusy; }
    bool isReady() const { return m_clientSafetyStatusKnown && m_clientSafetyConfigured && m_sessionDiscoveryReady; }
    bool sessionDiscoveryReady() const { return m_sessionDiscoveryReady; }
    bool safetyStatusKnown() const { return m_clientSafetyStatusKnown; }
    bool hasCurrentPaymasterOffer() const;
    bool hasOwnDgbForFees() const;
    bool preparesPaymasterPayment() const;
    bool hasFeeFundingCandidate() const;
    QString feeFundingProblem() const;
    bool subtractFee() const;
    static bool DescribeBackendError(const QString& reasonFailed, QString& title, QString& message);
    bool paymasterModeSelected() const;
    bool paymasterOnlySelected() const;
    void invalidatePaymasterOfferPreview(bool preserve_choice = false);
    void updateFeeDisplay();
    bool showConfirmationDialog(const QString& address, double amount);
    void setWalletModel(WalletModel* model);
    void send(const QString& address, CAmount amount_cents);
    void resetEntry();
    /** Test hook for exercising the non-mutating Paymaster focus-state presentation. */
    void setPaymasterSessionForTesting(const QString& state, const QString& artifact,
                                       bool persisted, const QString& address, double amount,
                                       const QString& attempt_state = QString{},
                                       const QString& pending_phase = QString{},
                                       const QStringList& allowed_actions = {},
                                       bool allowed_actions_known = false);
    /**
     * Install a deterministic wallet RPC transport for Paymaster widget tests.
     *
     * Production leaves this unset and continues through WalletModel's
     * asynchronous worker. The synchronous test transport makes each client
     * state transition observable without a live Paymaster network.
     */
    using PaymasterRpcExecutorForTesting =
        std::function<UniValue(const std::string&, const UniValue&)>;
    void setPaymasterRpcExecutorForTesting(PaymasterRpcExecutorForTesting executor);
    /** Delayed transport hook for lifecycle and callback-ordering tests. */
    using PaymasterAsyncRpcExecutorForTesting = std::function<void(
        const std::string&, const UniValue&, WalletModel::RpcCallback)>;
    void setPaymasterAsyncRpcExecutorForTesting(
        PaymasterAsyncRpcExecutorForTesting executor);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    bool hasMatchingPaymasterOffer() const;
    qint64 paymasterBalanceShortfall() const;
    void updateFeeChoiceLayout();
    void setupFeeSection();
    Q_SLOT void onFeeModeChanged();
    QString friendlyPaymasterSessionStatus() const;
    void updatePaymasterFocusMode();
    void onPaymasterPrimaryAction();
    Q_SLOT void showPaymasterExplanation();
    Q_SLOT void configureClientSafetyPolicy();
    Q_SLOT void refreshClientSafetyStatus();
    void updateClientSafetyDisplay();
    void reportPaymasterOperationNotStarted(
        const QString& reason);
    void discoverPersistedPaymasterSessions(bool user_requested = false);
    Q_SLOT void loadSelectedPersistedPaymasterSession();
    void activatePersistedPaymasterSession(
        const QString& request_id);
    enum class OfferCheckState { NOT_CHECKED, CHECKING, FOUND, EMPTY, STALE, FAILED, NEEDS_AMOUNT };
    void setOfferCheckStatus(OfferCheckState state, const QString& text);
    void updateOfferCheckControls();
    void clearOfferCards();
    void renderOfferCards();
    void selectOfferCard(int index, bool manual);
    void updateOfferCheckIcon();
    void renderOfferSpinner();
    Q_SLOT void pollPaymasterOffers();
    void requestPaymasterOffers(bool background);
    Q_SLOT void expirePaymasterOfferPreview();
    Q_SLOT void refreshPaymasterOffers();
    UniValue buildPaymasterSendParams(const QString& address, CAmount amount_cents) const;
    void executePaymasterRpcAsync(
        std::string command, UniValue params, WalletModel::RpcCallback callback, bool needs_unlock = false);
    void executePaymasterTransfer(
        const QString& address, CAmount amount_cents, bool allow_unlock, bool retry_transport = false);
    void handlePaymasterResult(const UniValue& result, const QString& error,
                               const QString& address, CAmount amount_cents);
    bool updatePaymasterSessionView(
        const UniValue& result, QString* decode_error = nullptr);
    bool handleAuthoritativePaymasterCompletion(
        const UniValue& result);
    PaymasterConfirmationSelection paymasterConfirmationSelection(
        const UniValue& result, const QString& address) const;
    enum class OfferReviewResult { ACCEPTED, CANCELED, EXPIRED, BLOCKED };
    OfferReviewResult confirmPaymasterSelectionBeforeSigning(
        const UniValue& result, const QString& address);
    bool canContinueActivePaymasterSend() const;
    Q_SLOT void pollPaymasterSession();
    Q_SLOT void refreshPaymasterSessionState();
    void stopPaymasterPolling();
    void schedulePaymasterPoll(bool state_changed);
    void refreshPaymasterSessionForAction(
        const QString& required_action, std::function<void()> continuation);
    Q_SLOT void retryPaymasterSession();
    void continuePaymasterRetry();
    Q_SLOT void fallbackPaymasterSession();
    Q_SLOT void recoverPaymasterSessionToSelf();
    Q_SLOT void abandonUnsignedPaymasterSession();
    void cancelUnsignedPaymasterSession(bool confirm);
    void clearUnsignedPaymasterSession();
    /** Clear presentation only after Core confirms absence or unsigned closure. */
    void clearClosedPaymasterSession();
    void closeUnusablePaymasterOffer(const QString& reason, bool allow_cancel = true);
    void setPaymasterNotice(const QString& text, DigiDollarStatus::Kind kind = DigiDollarStatus::Kind::ACTION);
    UniValue buildAlternativePaymasterRecoveryParams() const;
    void executeAlternativePaymasterRecovery(bool allow_unlock);
    PaymasterRecoveryConfirmationSelection paymasterRecoveryConfirmationSelection(
        const UniValue& recovery) const;
    bool confirmPaymasterRecoveryBeforeSigning(
        const PaymasterRecoveryConfirmationSelection& selection);
    void blockAlternativePaymasterRecovery(
        const QString& reason, const QString& detail);
    void handleAlternativePaymasterRecoveryResult(
        const UniValue& result, const QString& error);
    Q_SLOT void cancelPaymasterQuote();
    QString formatCents(qint64 cents) const;
    QString friendlyFundingModel(const QString& model) const;
    void applyPaymasterPrivacy();

private:
    QString feeMode() const;
    void connectSignals();
    void setPaymasterBusy(bool busy);
    DigiDollarSendWidget& m_form;
    WalletModel* m_walletModel{nullptr};
    bool m_privacy{false};
    // Fee section
    QLabel* m_paymasterNotice{nullptr};
    QString m_paymasterClosureReason;
    QFrame* m_feeFrame{nullptr};
    QGridLayout* m_feeLayout{nullptr};
    QLabel* m_feeHeading{nullptr};
    QFrame* m_feeChoicesFrame{nullptr};
    QGridLayout* m_feeChoicesLayout{nullptr};
    QFrame* m_dgbFeeCard{nullptr};
    QFrame* m_autoFeeCard{nullptr};
    QFrame* m_paymasterFeeCard{nullptr};
    QLabel* m_feeLabel{nullptr};
    QLabel* m_feeValue{nullptr};
    QLabel* m_totalLabel{nullptr};
    QLabel* m_totalValue{nullptr};
    QLabel* m_feeIntroduction{nullptr};
    QRadioButton* m_dgbFeeRadio{nullptr};
    QRadioButton* m_autoFeeRadio{nullptr};
    QRadioButton* m_paymasterFeeRadio{nullptr};
    QLabel* m_feeModeExplanation{nullptr};
    QLabel* m_feeSummary{nullptr};
    QCheckBox* m_subtractPaymasterFeeCheck{nullptr};
    QPushButton* m_paymasterExplanationButton{nullptr};
    QPushButton* m_advancedPaymasterButton{nullptr};
    QComboBox* m_feeModeCombo{nullptr};
    QFrame* m_advancedPaymasterFrame{nullptr};
    QComboBox* m_privacyCombo{nullptr};
    QComboBox* m_selectionCombo{nullptr};
    QSpinBox* m_feeCapSpin{nullptr};
    QLabel* m_feeCapPercent{nullptr};
    QSpinBox* m_maxAttemptsSpin{nullptr};
    QPushButton* m_refreshOffersButton{nullptr};
    QLabel* m_offersStatus{nullptr};
    QLabel* m_offersUpdated{nullptr};
    QLabel* m_offerCheckHelp{nullptr};
    QTableWidget* m_offersTable{nullptr};
    QFrame* m_offerCardsFrame{nullptr};
    QVBoxLayout* m_offerCardsLayout{nullptr};
    QButtonGroup* m_offerSelectionGroup{nullptr};
    UniValue m_offerPreview{UniValue::VARR};
    QString m_selectedOfferProvider;
    QString m_selectedOfferId;
    bool m_offerManuallySelected{false};
    QFrame* m_persistedPaymasterSessionsFrame{nullptr};
    QLabel* m_persistedSessionsStatus{nullptr};
    QLabel* m_sessionDiscoveryError{nullptr};
    QProgressBar* m_sessionDiscoveryProgress{nullptr};
    bool m_sessionDiscoveryReady{false};
    bool m_sessionDiscoveryPending{false};
    QComboBox* m_persistedPaymasterSessions{nullptr};
    QPushButton* m_loadPersistedPaymasterSessionButton{nullptr};
    QFrame* m_clientSafetyFrame{nullptr};
    QLabel* m_clientSafetyStatus{nullptr};
    QLabel* m_clientSafetyDetails{nullptr};
    QPushButton* m_configureClientSafetyButton{nullptr};
    QFrame* m_paymasterSessionFrame{nullptr};
    QLabel* m_paymasterStateValue{nullptr};
    QLabel* m_paymasterTransferValue{nullptr};
    QLabel* m_paymasterIdentityValue{nullptr};
    QLabel* m_paymasterCostValue{nullptr};
    QLabel* m_paymasterExpiryValue{nullptr};
    QPushButton* m_retrySessionButton{nullptr};
    QPushButton* m_fallbackSessionButton{nullptr};
    QPushButton* m_recoverSessionButton{nullptr};
    QPushButton* m_abandonSessionButton{nullptr};
    QPushButton* m_cancelQuoteButton{nullptr};
    QLabel* m_paymasterNextStepValue{nullptr};
    QPushButton* m_paymasterPrimaryButton{nullptr};
    QPushButton* m_paymasterMoreButton{nullptr};
    QPushButton* m_paymasterTechnicalButton{nullptr};
    QFrame* m_paymasterSecondaryActions{nullptr};
    QFrame* m_paymasterTechnicalDetails{nullptr};

    double m_paymasterInitialAvailableBalance{0.0};
    UniValue m_paymasterActiveRecoveryParams;
    QElapsedTimer m_paymasterActiveRecoveryStarted;
    UniValue m_paymasterSendTemplate;
    UniValue m_paymasterRestoredOptions{UniValue::VOBJ};
    QString m_paymasterRequestId;
    QString m_paymasterSessionId;
    QString m_paymasterSessionState;
    QString m_paymasterAttemptState;
    QString m_paymasterArtifact;
    QString m_paymasterPendingPhase;
    QString m_paymasterBroadcastState;
    QString m_paymasterConfirmationState;
    QString m_paymasterTransactionId;
    QString m_paymasterRecoveryTransactionId;
    QString m_paymasterResultStatus;
    qint64 m_paymasterResultSequence{-1};
    qint64 m_paymasterRecoveryExpiresAt{-1};
    QStringList m_paymasterAllowedActions;
    QString m_paymasterAddress;
    QString m_paymasterAuthorizationCommitment;
    QString m_paymasterSessionPrivacy;
    QString m_paymasterRecoveryAuthorizationCommitment;
    double m_paymasterAmount{0.0};
    CAmount m_paymasterAmountCents{0};
    qint64 m_paymasterPreviewRecipientCents{-1};
    qint64 m_paymasterPreviewServiceFeeCents{-1};
    qint64 m_paymasterPreviewTotalCents{-1};
    uint64_t m_paymasterOfferPreviewGeneration{0};
    uint64_t m_paymasterWalletGeneration{0};
    uint64_t m_clientSafetyRefreshGeneration{0};
    bool m_sendAllSpendableDD{false};
    qint64 m_paymasterRecoveryMaximumServiceFeeCents{0};
    bool m_paymasterBusy{false};
    bool m_paymasterSessionPersisted{false};
    bool m_paymasterUnsignedClosed{false};
    bool m_paymasterOfferReadyForReview{false};
    bool m_paymasterRecoveryActive{false};
    enum class PaymasterPrimaryAction {
        REFRESH,
        RESUME,
        REVIEW_OFFER,
        FALLBACK,
        RECOVER,
        NEW_TRANSFER,
    };
    PaymasterPrimaryAction m_paymasterPrimaryAction{PaymasterPrimaryAction::REFRESH};
    bool m_clientSafetyStatusKnown{false};
    bool m_clientSafetyConfigured{false};
    qint64 m_clientSafetyMaximumPerTransaction{100};
    qint64 m_clientSafetyMaximumPerDay{1000};
    qint64 m_clientSafetyActiveReservations{0};
    qint64 m_clientSafetyReservedCents{0};
    qint64 m_clientSafetySpentTodayCents{0};
    qint64 m_clientSafetyAvailableTodayCents{0};
    QString m_clientSafetyError;
    QTimer* m_paymasterPollTimer;
    int m_paymasterPollIntervalMs{1500};
    static constexpr int MAX_PAYMASTER_POLL_INTERVAL_MS{15000};
    bool m_paymasterAllowedActionsKnown{false};
    bool m_paymasterTerminalNoticeShown{false};
    PaymasterRpcExecutorForTesting m_paymasterRpcExecutorForTesting;
    PaymasterAsyncRpcExecutorForTesting m_paymasterAsyncRpcExecutorForTesting;
    PaymasterConfirmationGuard m_paymasterConfirmationGuard;
    PaymasterRecoveryConfirmationGuard m_paymasterRecoveryConfirmationGuard;
    // Only the request explicitly started in this wallet view may advance on
    // the timer. Restored sessions have no continuation or signing authority.
    UniValue m_paymasterActiveSendParams;
    QElapsedTimer m_paymasterActiveSendStarted;
    QString m_paymasterActiveRetryRequest;
    QElapsedTimer m_paymasterActiveRetryStarted;
    static constexpr int MAX_PAYMASTER_ACTIVE_SEND_MS{120000};
    QFrame* m_offerCheckFrame{nullptr};
    QLabel* m_offerStateIcon{nullptr};
    QTimer* m_offerIconTimer{nullptr};
    int m_offerIconFrame{0};
    QTimer* m_offerExpiryTimer{nullptr};
    OfferCheckState m_offerCheckState{OfferCheckState::NOT_CHECKED};
    QString m_paymasterTransportState;

};

#endif // DIGIBYTE_QT_PAYMASTERSENDWIDGET_H
