// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERPROVIDER_INTERNAL_H
#define DIGIBYTE_QT_PAYMASTERPROVIDER_INTERNAL_H
#include <qt/paymasterproviderviewhelpers.h>
#include <qt/paymasterasync.h>

using namespace PaymasterQt::ProviderViews;

class PaymasterWidgetImpl final : public DigiDollarPaymasterWidget
{
    struct FundingSafetyControls {
        QLineEdit* per_transaction{nullptr};
        QLineEdit* reserved{nullptr};
        QLineEdit* per_hour{nullptr};
        QLineEdit* per_day{nullptr};
        QSpinBox* completed_per_hour{nullptr};
        QSpinBox* completed_per_day{nullptr};
        QLabel* mode{nullptr};
    };

    struct FundingSafetyValues {
        qint64 per_transaction{0};
        qint64 reserved{0};
        qint64 per_hour{0};
        qint64 per_day{0};
        int completed_per_hour{0};
        int completed_per_day{0};

        bool allZero() const
        {
            return per_transaction == 0 && reserved == 0 && per_hour == 0 &&
                   per_day == 0 && completed_per_hour == 0 &&
                   completed_per_day == 0;
        }

        bool hasZero() const
        {
            return per_transaction == 0 || reserved == 0 || per_hour == 0 ||
                   per_day == 0 || completed_per_hour == 0 ||
                   completed_per_day == 0;
        }

    };

    struct LiquidityPolicyValues {
        bool automatic_replenishment{true};
        bool paid_maintenance_approved{false};
        int admission_dgb{3};
        int operational_dgb{1};
        int admission_carriers{0};
        int operational_carriers{0};
        qint64 fee_per_transaction{0};
        qint64 fee_per_hour{0};
        qint64 fee_per_day{0};
    };

public:
    explicit PaymasterWidgetImpl(QWidget* parent = nullptr);

    ~PaymasterWidgetImpl() override;

    /**
     * Install the application-level full-wallet backup action.
     *
     * The provider console deliberately reuses WalletView's established file
     * picker and backup reporting instead of duplicating backup mechanics in
     * this presentation-only widget.
     */
    void setProviderBackupRequestHandler(std::function<void()> handler) override;

    void setWalletModel(WalletModel* model) override;

    void setPrivacy(bool privacy) override;

    void showEvent(QShowEvent* event) override;

    void refreshStatus();

    void applyProviderInfo(const UniValue& result);


    void setOperatorStatusForTesting(const UniValue& status) override;

    void setReadinessStatusForTesting(const UniValue& status) override;

    void setStartResultForTesting(const UniValue& result) override;

    void setLiquidityStatusForTesting(const UniValue& status) override;

    void setLiquidityPoolForTesting(const UniValue& pool_info) override;

    void expireOperatorStatusForTesting() override;

    void setMutationSnapshotsAvailableForTesting(
        bool provider_info, bool provider_safety, bool liquidity) override;

    void setRpcExecutorForTesting(
        RpcExecutor executor) override;

    void setAsyncRpcExecutorForTesting(AsyncRpcExecutor executor) override;

    void setFinanceExportFilenameForTesting(QString filename) override;

private:
    static constexpr int FINANCE_PAGE_SIZE{250};

    struct FinancePage {
        UniValue result{UniValue::VOBJ};
        std::vector<UniValue> events;
        QString request_cursor;
        QString next_cursor;
    };

    struct FinanceExportState {
        QString filename;
        QString period;
        QString provider_id;
        UniValue summary{UniValue::VOBJ};
        std::vector<UniValue> events;
        std::set<QString> requested_cursors;
        std::set<QString> event_ids;
    };
    enum class FinanceWritePhase { WRITING, CANCELLED, COMMITTING };

    enum class OracleState {
        UNKNOWN,
        CHECKING,
        AVAILABLE,
        UNAVAILABLE,
    };

    QString maskNumericText(QString value) const;

    static bool spinCanRepresent(const QSpinBox* control, qint64 value);

    static bool dgbAmountCanRepresent(qint64 value);

    static void showPlainTextWarning(QWidget* parent, const QString& title,
                                     const QString& message);

    static void showPlainTextInformation(QWidget* parent,
                                         const QString& title,
                                         const QString& message);

    static QMessageBox::StandardButton askPlainTextQuestion(
        QWidget* parent, const QString& title, const QString& message,
        QMessageBox::StandardButtons buttons =
            QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::StandardButton default_button = QMessageBox::Cancel);

    bool requirePrivacyOffForSensitiveAction(const QString& action);

    QString privacySafeBackendError(const QString& error) const;

    void setSensitivePagePrivacy(bool privacy);

    void redactVisiblePrivacyText();

    QString visibleProviderId(const QString& provider_id) const;

    void updateIdentityLabels();

    void presentProviderStartResult(const UniValue& result);

    bool hasRpcTransport() const;

    bool hasCompleteMutationSnapshots() const;

    static bool isPassiveExternalReadinessError(const QString& error);

    static bool isProviderSetupReadinessError(const QString& error);

    static bool isLiquidityReadinessError(const QString& error);

    bool providerConfigurationComplete() const;

    bool hasPassiveExternalWait() const;

    bool shouldStartAndRestoreLiquidity() const;

    bool maintenanceFeeLimitExceeded() const;

    bool maintenanceWaitingForFunds() const;

    void updateAutomaticRefreshTimer();

    void updateProviderHeaderStatus();

    void updateExternalPrerequisites();

    void refreshOracleStatus();

    static QString dgbAmount(qint64 satoshis);

    static QString compactDgbAmount(qint64 satoshis);

    static QString ddAmount(qint64 cents);

    static qint64 financeNumber(const UniValue& object, const char* name);

    static QString financeEventKind(const QString& kind);

    static QString financeEventState(const QString& state);

    static QString financeFundingModel(const UniValue& event);

    void updateBackupReminder(bool required, const QString& provider_id);

    void reviewBackupAdoption(bool stopped = false);

    void requestProviderBackup();

    void acknowledgeExternalBackup();

    void refreshFinanceStatus();

    void clearOverviewCapital();

    void setCapitalMetric(const char* name, const QString& text, const QString& kind);

    void setFundsValue(const QString& key, const QString& text);

    void renderOverviewPool();

    std::optional<qint64> savedRefillCapacity() const;

    bool hasExcessSavedReserves() const;

    bool savedPoolTargetsReady() const;

    void overviewFinanceUnavailable();

    void applyOverviewFinance(const UniValue& result);

    void refreshOverviewFinance(bool background = false);

    void clearFinanceValues();

    void requestSelectedFinanceStatus();

    void requestFinancePage(const QString& requested_period,
                            const QString& cursor, bool replace_pages);

    void showPreviousFinancePage();

    void showNextFinancePage();

    void setFinanceLoading(bool loading, bool exporting);

    void updateFinanceControls();

    void finishFinancePageFailure(const QString& message);

    void renderFinancePage(int index);

    void applyFinanceStatus(const UniValue& result,
                            const std::vector<UniValue>& events);

    static QString csvCell(QString value);

    void exportFinanceCsv();

    void requestFinanceExportPage(
        const std::shared_ptr<FinanceExportState>& state,
        const QString& cursor);

    void finishFinanceExportFailure(const QString& message);

    void cancelFinanceWrite();

    void writeFinanceCsv(const std::shared_ptr<FinanceExportState>& state);

    static QString writeFinanceFile(const FinanceExportState& data, std::atomic<FinanceWritePhase>& phase);

    void openLiquidityFinanceControl(QWidget* control);

    void reviewMaintenanceFeeLimit();

    bool readLiquidityPolicy(LiquidityPolicyValues& values) const;

    static UniValue liquidityPolicyToJSON(
        const LiquidityPolicyValues& values);

    bool liquidityPolicyCanRepresent(const UniValue& policy) const;

    void setLiquidityPolicyMutationEnabled(bool enabled);

    void loadLiquidityPolicy(const UniValue& policy, bool configured);

    void markLiquidityPolicyDirty();

    bool confirmedUserPaid() const;

    qint64 confirmedPaymentFee() const;

    QString liquidityApprovalText(
        const LiquidityPolicyValues& values) const;

    void saveLiquidityPolicy();

    void liquidityPolicySaved(const UniValue& policy, bool reduced_targets = false);

    void reconcileLiquidityPolicySave(const UniValue& requested, const QString& reason, bool reduced_targets = false);

    void approveSuggestedLiquidityMaintenance();

    static QString slotStatusText(const QString& name,
                                  const UniValue& status);

    void applyLiquidityStatus(const UniValue& result);

    void refreshLiquidityStatus();

    void applyLiquidityPoolEntries(const UniValue& result,
                                   bool refresh_safety);

    void refreshLiquidityPoolEntries();

    void invalidateCarrierWithdrawalPreviews();

    bool carrierPlanCurrent(const QString& plan_id, qint64 expires_at) const;

    void updateCarrierWithdrawalButtons();

    UniValue carrierWithdrawalOptions(const QString& mode, bool execute,
                                      const QString& plan_id = {}) const;

    void carrierWithdrawalAction(const QString& mode, bool);

    PaymasterSetupMode loadSetupMode() const;

    void setSetupMode(PaymasterSetupMode mode);

    void updateSetupAccess();

    void restorePolicyDefaults();

    void applyRecommendedPolicyDefaults(bool mark_dirty);

    void setPolicyMutationEnabled(bool enabled);

    void loadPolicy(const UniValue& policy);

    QWidget* configurationWarningPanel(QLabel*& label, const char* name, const QString& button_text, std::function<void()> navigate);

    QString configurationConflictText(const UniValue& offer, const UniValue& safety) const;

    void updateConfigurationWarnings();

    void updatePolicyDisplay();

    QString refillFundingExplanation(const QString& error) const;

    QString readinessExplanation(const QString& error) const;

    void updateReadinessSummary(const QStringList& errors, bool running, bool ready);

    static QLineEdit* integerField(QWidget* parent, qint64 value);

    FundingSafetyControls createFundingSafetyControls(
        QTabWidget* tabs, const QString& title, const QString& object_prefix,
        bool recommended_defaults);

    void restoreFundingSafetyDefaults(const FundingSafetyControls& controls,
                                      bool recommended_defaults,
                                      bool mark_dirty = true);

    void restoreQuoteSafetyDefaults(bool mark_dirty = true);

    void restoreClientSafetyDefaults(bool mark_dirty = true);

    void markProviderSafetyDirty();

    void markClientSafetyDirty();

    void updateFundingSafetyDisplay(const FundingSafetyControls& controls,
                                    bool persisted = false);

    void updateClientSafetyDisplay(bool persisted = false);

    void watchFundingSafetyControls(const FundingSafetyControls& controls);

    static bool readFundingSafety(const FundingSafetyControls& controls,
                                  FundingSafetyValues& values);

    static UniValue fundingSafetyToJSON(const FundingSafetyValues& values);

    static bool fundingSafetyCanRepresent(
        const UniValue& value, const FundingSafetyControls& controls);

    bool providerSafetyCanRepresent(const UniValue& policy) const;

    void setProviderSafetyMutationEnabled(bool enabled);

    QString providerSafetyInputError() const;

    static void loadFundingSafety(const UniValue& value,
                                  const FundingSafetyControls& controls);

    bool providerSafetyAllowsSelectedModels() const;

    QString providerStartConfirmationText() const;

    QString providerServiceStatusText() const;

    void addQuickRefill(QVBoxLayout* layout, QWidget* parent, bool overview);

    QString automationSummary() const;

    void updateQuickAutomation();

    void saveQuickRefill(bool enabled);

    void discardAutostart();

    void reconcileAutostart();

    void updateAutostartControls();

    void saveAutostart();

    // Use the existing atomic disable-and-stop RPC so autostart cannot race a
    // setting change. Unlike a persistent operator pause, this retains the
    // saved autostart preference. All callbacks use the wallet-bound RPC chain.
    void applyWithProviderRestart(std::function<void(bool)> apply, std::function<void(const QString&)> failed);

    void finishProviderSettingsRestart(std::function<bool(const UniValue&)> verified, std::function<void(const QString&)> failed);

    void saveRuntimeSettings();

    bool repairableLiquidityGap() const;

    bool canStartProvider() const;

    void updateProviderButtons();

    void setStatusLabel(QLabel* label, const QString& text,
                        const QString& kind) const;

    void setStatusKind(QWidget* widget, const QString& kind) const;

    void updateOperatorDashboard();

    bool isOperatorPage(QWidget* page) const;

    void showOperatorPage(QWidget* page);

    void updateNavigationLayout();

    void resizeEvent(QResizeEvent* event) override;

    void setPageAvailable(QWidget* page, bool enabled, const QString& explanation);

    bool isCurrentOperatorRefresh() const;

    void updateOperatorProgress();

    void updateLifecycleControls();

    void showOperatorBusyState();

    void renderEndpointDiagnostic(const UniValue& snapshot);

    void resetOperatorPresentation();

    void renderOperatorPresentation(const UniValue& snapshot);

    void renderLiveOverview(const UniValue& snapshot, bool locked);

    void performOperatorNextAction();

    void markProviderStatusCurrent();

    void refreshOperatorStatus(bool background = false);

    void configureOperatorNode();

    void configureOperatorNodeFrom(const UniValue& current);

    QWidget* createNodeConfigEditor(const UniValue& current, QWidget* parent);

    void unlockOperatorWallet(std::function<void()> on_unlocked = {});

    void stopOperator(bool retire = false);

    void reviewFullCapitalRelease(bool retire = false, bool recovery = false);

    void reviewRetirementBalances(bool recovery = false);

    void pauseOperator();

    void resumeOperator();

    // Operating-capital actions retain the guarded start-and-refill path.
    // Overview and lifecycle settings share the general start/pause controls.
    void updateEarningsWithdrawalHint();

    bool carrierReleaseNeedsReview() const;

    void renderCurrentTask();

    void failGuidedTask(const QString& error);

    bool canReviewReserveReduction() const;

    bool waitForReserveReduction(const QString& error, std::function<void()> retry);

    void requestReviewedStart();

    void beginGuidedTask(const QString& task, bool start_after = false);

    void previewGuidedTask(const QString& task);

    void applyReviewedReserveRepair(const UniValue& context, const UniValue& proposed, const UniValue& preview_params);

    void executeGuidedTask(const std::string& command, const UniValue& params);

    void startProvider();

    void saveProviderSafetyPolicy();

    void invalidateSavedConfigurationDisplay();

    void saveClientSafetyPolicy();

    QString safetyClassStatus(const QString& name, const UniValue& value) const;

    void applyProviderSafetyStatus(const UniValue& result);

    void refreshProviderSafetyStatus();

    void refreshClientSafetyStatus();

    static QSpinBox* spin(QWidget* parent, int minimum, int maximum, int value);

    static QLineEdit* dgbAmountField(QWidget* parent, qint64 satoshis);

    static QSpinBox* scaledSpin(QWidget* parent, int minimum, int maximum,
                                int value, int scale, int decimals);

    QString yesNo(bool value) const;

    QString liquidityTargetKey() const;

    QString preparationTargetKey() const;

    void invalidatePoolPreviews();

    void restoreLiquidityDefaults(bool announce = true);

    void updateSavedReserveState();

    void updateLiquidityDisplay();

    enum class PreparationDetail { Technical, Operator, Summary };

    QString preparationFundingExplanation(const UniValue& step, bool summary) const;

    QString formatPoolPreparation(const UniValue& records, PreparationDetail detail = PreparationDetail::Technical) const;

    UniValue cancellableMaintenance() const;

    void cancelMaintenance();

    QString cancellablePreparationPlan() const;

    void updatePoolPreparationStatus();

    bool canReviewPreparationFee() const;

    void cancelPoolPreparation(bool review_replacement = false);

    static qint64 poolNumber(const UniValue& result, const char* name);

    QString formatPoolResult(const char* command, const UniValue& result) const;

    UniValue poolOptions(bool execute, const QString& plan_id = {}, bool preparation = false) const;

    void poolAction(const char* command, bool);

    static QString activityText(const UniValue& object, const char* name);

    QString reservationPurpose(const UniValue& entry) const;

    QString reservationAsset(const UniValue& entry) const;

    QString reservationState(const UniValue& entry) const;

    void showReservations(const UniValue& result);

    QString formatActivityResult(const UniValue& result, bool submitted_payment) const;

    void refreshActivity();

    void processActivityRequest();

    void processActivitySubmit();

    void showPolicySaveStatus(const QString& message);

    void reviewPolicySpendingLimits();

    void savePolicy();

    bool showSetupWizard();

    using ResultHandler = std::function<void(const UniValue&)>;
    using ErrorHandler = std::function<void(const QString&)>;

    struct PendingRpcCall {
        std::string command;
        UniValue params;
        bool needs_unlock{false};
        QPointer<QPlainTextEdit> output;
        ResultHandler handler;
        bool show_error{true};
        ErrorHandler error_handler;
        bool background{false};
    };

    void setRpcBusyState(bool busy);

    void showPendingOperatorReview();

    void finishRpcCall(uint64_t wallet_generation,
                       QPointer<QPlainTextEdit> output,
                       ResultHandler handler, bool show_error,
                       ErrorHandler error_handler, UniValue result,
                       QString error);

    void finishRpcCall(uint64_t wallet_generation, QPointer<QPlainTextEdit> output,
                       ResultHandler handler, bool show_error, ErrorHandler error_handler,
                       UniValue result, PaymasterQt::RpcError error);

    void dispatchRpcCall(PendingRpcCall request);

    void call(std::string command, UniValue params, bool needs_unlock,
              QPlainTextEdit* output = nullptr, ResultHandler handler = {},
              bool show_error = true, ErrorHandler error_handler = {},
              bool background = false);

    // Structured failure for the active callback, restored across nested replies.
    PaymasterQt::RpcError m_rpc_failure;
    WalletModel* m_model{nullptr};
    QMetaObject::Connection m_history_wait_connection;
    bool m_history_deferred_status{false};
    uint64_t m_wallet_generation{0};
    uint64_t m_settings_restart_generation{0};
    uint64_t m_oracle_request_generation{0};
    RpcExecutor m_rpc_executor_for_testing;
    AsyncRpcExecutor m_async_rpc_executor_for_testing;
    std::string m_active_rpc_command;
    bool m_busy{false};
    bool m_rpc_active{false};
    bool m_rpc_chain_foreground{false};
    bool m_active_rpc_background{false};
    int m_rpc_handler_depth{0};
    std::function<void()> m_pending_operator_review;
    uint64_t m_active_rpc_handler_token{0};
    uint64_t m_next_rpc_handler_token{0};
    // Follow-up status calls requested by the currently executing handler are
    // serialized in-order. The bounded queue supports compound authoritative
    // refreshes without reopening the UI to external re-entrant mutations.
    std::deque<PendingRpcCall> m_pending_handler_calls;
    PaymasterSetupMode m_setup_mode{PaymasterSetupMode::UNDECIDED};
    QStackedWidget* m_tabs{nullptr};
    QWidget* m_navigation_shell{nullptr};
    QListWidget* m_navigation{nullptr};
    QComboBox* m_navigation_select{nullptr};
    QWidget* m_automation_page{nullptr};
    QWidget* m_lifecycle_page{nullptr};
    QGroupBox* m_setup_choice{nullptr};
    QWidget* m_setup_content{nullptr};
    QWidget* m_configuration_page{nullptr};
    QWidget* m_user_paid_pricing{nullptr};
    QMap<QString, QLabel*> m_live_status;
    QMap<QString, QLabel*> m_live_details;
    QList<QFormLayout*> m_offer_forms;
    QWidget* m_safety_page{nullptr};
    QWidget* m_liquidity_page{nullptr};
    QWidget* m_finance_page{nullptr};
    QLabel* m_finance_selected_summary{nullptr};
    QWidget* m_activity_page{nullptr};
    QPushButton* m_guided_setup{nullptr};
    QPushButton* m_expert_setup{nullptr};
    QPushButton* m_reopen_wizard{nullptr};
    QLabel* m_overview_offer_status{nullptr};
    QLabel* m_overview_safety_status{nullptr};
    QWidget* m_overview_budget_details{nullptr};
    QLabel* m_overview_liquidity_status{nullptr};
    QLabel* m_overview_operation_status{nullptr};
    QLabel* m_overview_finance_status{nullptr};
    QWidget* m_overview_finance_details{nullptr};
    QWidget* m_overview_capital_details{nullptr};
    UniValue m_overview_liquidity_snapshot;
    QPushButton* m_overview_offer_action{nullptr};
    QPushButton* m_overview_safety_action{nullptr};
    QPushButton* m_overview_liquidity_action{nullptr};
    QPushButton* m_overview_operation_action{nullptr};
    QPushButton* m_overview_finance_action{nullptr};
    QGroupBox* m_overview_backup_notice{nullptr};
    QLabel* m_overview_backup_provider_id{nullptr};
    QPushButton* m_overview_backup_now{nullptr};
    QPushButton* m_overview_backup_external{nullptr};
    QLabel* m_wallet;
    QLabel* m_last_checked{nullptr};
    QStackedWidget* m_settings_tabs{nullptr};
    QElapsedTimer m_status_age;
    QElapsedTimer m_overview_finance_age;
    bool m_overview_finance_observed{false};
    PaymasterOperationController m_operation;
    QGroupBox* m_task_card{nullptr};
    QLabel* m_task_error_details{nullptr};
    QPushButton* m_task_error_toggle{nullptr};
    QLabel* m_task_status{nullptr};
    QProgressBar* m_task_progress{nullptr};
    QPushButton* m_restore_task{nullptr};
    QPushButton* m_withdraw_task{nullptr};
    QLabel* m_withdrawal_hint{nullptr};
    QPushButton* m_release_task{nullptr};
    QPushButton* m_task_continue{nullptr};
    QPushButton* m_cancel_maintenance{nullptr};
    QString m_guided_task;
    QString m_failed_guided_task;
    uint64_t m_guided_generation{0};
    int m_guided_busy_retries{0};
    bool m_guided_waiting_for_provider{false};
    QString m_guided_review_text;
    bool m_guided_start_after{false};
    QWidget* m_connection_page{nullptr};
    QWidget* m_diagnostics_page{nullptr};
    QLabel* m_diagnostic_endpoint{nullptr};
    QLabel* m_diagnostic_hint{nullptr};
    QPlainTextEdit* m_diagnostic_rpc{nullptr};
    QPlainTextEdit* m_diagnostic_cli{nullptr};
    QPushButton* m_diagnostic_rpc_copy{nullptr};
    QPushButton* m_diagnostic_cli_copy{nullptr};
    QWidget* m_management_page{nullptr};
    QWidget* m_node_editor_host{nullptr};
    QVBoxLayout* m_node_editor_layout{nullptr};
    QPointer<QWidget> m_inline_node_editor;
    QLabel* m_backup_summary{nullptr};
    QPushButton* m_backup_adopt{nullptr};
    QLabel* m_backup_adopt_status{nullptr};
    QPushButton* m_backup_notice{nullptr};
    QList<QCheckBox*> m_refill_switches;
    QList<QCheckBox*> m_autostart_copies;
    QList<QLabel*> m_refill_switch_status;
    UniValue m_saved_liquidity_policy;
    UniValue m_liquidity_edit_revision;
    bool m_refill_save_pending{false};
    QString m_refill_message;
    QGroupBox* m_operator_card{nullptr};
    QLabel* m_operator_headline{nullptr};
    QLabel* m_operator_hint{nullptr};
    QWidget* m_operator_loading{nullptr};
    QProgressBar* m_operator_progress{nullptr};
    QLabel* m_operator_loading_step{nullptr};
    QTimer* m_operator_progress_timer{nullptr};
    QElapsedTimer m_operator_read_elapsed;
    QLabel* m_operator_connection{nullptr};
    QLabel* m_operator_wallet{nullptr};
    QPushButton* m_operator_action{nullptr};
    QPushButton* m_operator_start{nullptr};
    QPushButton* m_lifecycle_start{nullptr};
    QPushButton* m_lifecycle_pause{nullptr};
    QLabel* m_lifecycle_status{nullptr};
    bool m_operator_start_allowed{false};
    QPushButton* m_operator_pause{nullptr};
    QPushButton* m_operator_stop{nullptr};
    QPushButton* m_operator_retire{nullptr};
    QLabel* m_operator_stop_status{nullptr};
    QGroupBox* m_retirement_summary{nullptr};
    QLabel* m_retirement_balances{nullptr};
    QPushButton* m_retirement_backup{nullptr};
    QLabel* m_retirement_backup_status{nullptr};
    QString m_operator_next_action{QStringLiteral("refresh")};
    QString m_operator_next_area;
    QLabel* m_operator_summary{nullptr};
    QPointer<QDialog> m_node_connection_dialog;
    QPointer<QDialog> m_refill_approval;
    UniValue m_operator_snapshot;
    bool m_operator_restart_required{false};
    QLabel* m_status;
    QLabel* m_identity;
    QLabel* m_pool;
    QLabel* m_next_step;
    QLabel* m_readiness_summary;
    QGroupBox* m_external_prerequisites_card{nullptr};
    QLabel* m_external_blockchain_status{nullptr};
    QLabel* m_external_txindex_status{nullptr};
    QLabel* m_external_broadcast_status{nullptr};
    QLabel* m_external_activation_status{nullptr};
    QLabel* m_external_oracle_status{nullptr};
    QLabel* m_external_oracle_note{nullptr};
    QLabel* m_external_other_heading{nullptr};
    QLabel* m_external_other_status{nullptr};
    QPlainTextEdit* m_readiness;
    QGroupBox* m_liquidity_maintenance_card{nullptr};
    QLabel* m_pool_preparation_status{nullptr};
    QPushButton* m_cancel_preparation{nullptr};
    DgbAmountLineEdit* m_preparation_fee{nullptr};
    UniValue m_pool_preparation;
    QLabel* m_liquidity_maintenance_state{nullptr};
    QLabel* m_liquidity_maintenance_next_step{nullptr};
    QLabel* m_liquidity_maintenance_cost{nullptr};
    QPushButton* m_approve_liquidity_maintenance{nullptr};
    QLabel* m_enable_status;
    QPushButton* m_create_identity{nullptr};
    QLabel* m_offer_identity_status{nullptr};
    QLabel* m_offer_identity_id{nullptr};
    QLineEdit* m_display_name;
    QCheckBox* m_sponsored;
    QCheckBox* m_user_paid;
    QComboBox* m_scope;
    QLabel* m_sponsorship_label{nullptr};
    QSpinBox* m_fee_bps;
    QSpinBox* m_maximum_user_paid_service_fee;
    PaymasterFeeExample* m_fee_example{nullptr};
    QSpinBox* m_min_amount;
    QSpinBox* m_max_amount;
    QSpinBox* m_quote_ttl;
    QSpinBox* m_network_fee;
    QPushButton* m_save_policy{nullptr};
    QPushButton* m_restore_policy_defaults{nullptr};
    QGroupBox* m_provider_safety_group{nullptr};
    QPushButton* m_save_provider_safety{nullptr};
    QLabel* m_funding_model_status;
    QLabel* m_policy_summary;
    QLabel* m_policy_save_status{nullptr};
    QPushButton* m_review_policy_limits{nullptr};
    QWidget* m_policy_limits_review{nullptr};
    QLabel* m_policy_limits_hint{nullptr};
    QTabWidget* m_funding_safety_tabs{nullptr};
    PaymasterReserveEditor* m_reserve_editor{nullptr};
    QLabel* m_reserve_state{nullptr};
    QPushButton* m_release_excess_reserves{nullptr};
    QSpinBox* m_admission_dgb;
    QSpinBox* m_operational_dgb;
    QSpinBox* m_admission_carriers;
    QSpinBox* m_operational_carriers;
    QLabel* m_liquidity_summary;
    QLabel* m_liquidity_current_status;
    QLabel* m_liquidity_preview_status;
    QPushButton* m_liquidity_advanced_toggle{nullptr};
    QCheckBox* m_automatic_replenishment{nullptr};
    QCheckBox* m_paid_maintenance_approved{nullptr};
    QPushButton* m_maintenance_limits_toggle{nullptr};
    DgbAmountLineEdit* m_maintenance_fee_per_transaction{nullptr};
    DgbAmountLineEdit* m_maintenance_fee_per_hour{nullptr};
    DgbAmountLineEdit* m_maintenance_fee_per_day{nullptr};
    QPushButton* m_save_liquidity_policy_primary{nullptr};
    QPushButton* m_save_liquidity_policy{nullptr};
    QPushButton* m_restore_liquidity_defaults{nullptr};
    QLabel* m_liquidity_policy_status{nullptr};
    QLabel* m_liquidity_target_save_status{nullptr};
    QLabel* m_liquidity_slot_status{nullptr};
    QLabel* m_liquidity_recycling_status{nullptr};
    QLabel* m_liquidity_budget_status{nullptr};
    QLabel* m_carrier_value_status{nullptr};
    QLabel* m_carrier_withdrawal_status{nullptr};
    QPushButton* m_preview_carrier_excess{nullptr};
    QPushButton* m_execute_carrier_excess{nullptr};
    QComboBox* m_release_carrier_select{nullptr};
    QPushButton* m_preview_carrier_release{nullptr};
    QPushButton* m_execute_carrier_release{nullptr};
    QPushButton* m_prepare_execute;
    QPushButton* m_rebalance_execute;
    QPushButton* m_prepare_preview{nullptr};
    QPushButton* m_rebalance_preview{nullptr};
    QPlainTextEdit* m_liquidity_output;
    QString m_prepare_preview_target;
    QString m_rebalance_preview_target;
    QString m_prepare_plan_id;
    QString m_rebalance_plan_id;
    QComboBox* m_finance_period_select{nullptr};
    QPushButton* m_finance_refresh{nullptr};
    QPushButton* m_finance_export{nullptr};
    std::array<QLabel*, 4> m_finance_period_income{};
    std::array<QLabel*, 4> m_finance_period_cost{};
    std::array<QLabel*, 4> m_finance_period_transfers{};
    QLabel* m_finance_result_estimate{nullptr};
    QMap<QString, QLabel*> m_finance_values;
    QMap<QString, QLabel*> m_funds_values;
    QLabel* m_finance_pending_maintenance{nullptr};
    QPushButton* m_finance_withdraw_fees{nullptr};
    QPushButton* m_finance_release_carrier{nullptr};
    QPushButton* m_finance_review_dgb{nullptr};
    QPushButton* m_finance_add_liquidity{nullptr};
    QPushButton* m_finance_details_toggle{nullptr};
    QPushButton* m_finance_previous_page{nullptr};
    QPushButton* m_finance_next_page{nullptr};
    QLabel* m_finance_page_status{nullptr};
    QGroupBox* m_finance_history_notice{nullptr};
    QLabel* m_finance_history_notice_text{nullptr};
    QLabel* m_finance_history_status{nullptr};
    QTableWidget* m_finance_daily_totals{nullptr};
    QTableWidget* m_finance_events{nullptr};
    UniValue m_finance_last_result{UniValue::VOBJ};
    std::vector<FinancePage> m_finance_pages;
    int m_finance_page_index{-1};
    bool m_finance_loaded{false};
    bool m_finance_loading{false};
    bool m_finance_export_loading{false};
    std::shared_ptr<std::atomic<FinanceWritePhase>> m_finance_write_phase;
    QString m_finance_export_filename_for_testing;
    QString m_provider_id;
    QString m_provider_endpoint;
    bool m_backup_required{false};
    std::function<void()> m_provider_backup_request_handler;
    QLabel* m_activity_runtime_status;
    QLabel* m_activity_action_result;
    QLabel* m_activity_summary;
    QTableWidget* m_activity_reservations{nullptr};
    QLabel* m_activity_tasks{nullptr};
    QPushButton* m_operation_primary{nullptr};
    QGroupBox* m_reservations_group{nullptr};
    QGroupBox* m_recovery_group{nullptr};
    QPushButton* m_runtime_settings_toggle{nullptr};
    QGroupBox* m_runtime_settings_panel{nullptr};
    QComboBox* m_operation_mode_select{nullptr};
    QCheckBox* m_autostart{nullptr};
    QPushButton* m_save_autostart{nullptr};
    QPushButton* m_discard_autostart{nullptr};
    QLabel* m_autostart_status{nullptr};
    QLabel* m_settings_autostart_status{nullptr};
    bool m_autostart_dirty{false};
    bool m_autostart_pending_read{false};
    QString m_autostart_message;
    QPushButton* m_save_runtime_settings{nullptr};
    QLabel* m_runtime_settings_result{nullptr};
    QGroupBox* m_request_processing_group{nullptr};
    QGroupBox* m_submit_processing_group{nullptr};
    QGroupBox* m_activity_result_group{nullptr};
    QPushButton* m_process_requests;
    QPushButton* m_process_submits;
    QPlainTextEdit* m_activity_output;
    FundingSafetyControls m_user_paid_safety;
    FundingSafetyControls m_public_sponsored_safety;
    FundingSafetyControls m_restricted_sponsored_safety;
    QSpinBox* m_max_active_quotes_total;
    QSpinBox* m_max_active_quotes_per_netgroup;
    QSpinBox* m_max_active_quotes_per_recipient;
    QSpinBox* m_max_quote_requests_per_netgroup;
    QLabel* m_provider_safety_status;
    QLabel* m_operator_config_warning{nullptr};
    QLabel* m_policy_config_warning{nullptr};
    QLabel* m_safety_config_warning{nullptr};
    QLabel* m_provider_safety_usage;
    QSpinBox* m_client_fee_per_transaction;
    QSpinBox* m_client_fee_per_day;
    QLabel* m_client_safety_status;
    QLabel* m_client_safety_mode;
    QGroupBox* m_client_safety_group{nullptr};
    QPushButton* m_save_client_safety{nullptr};
    bool m_loading_policy{false};
    bool m_policy_loaded{false};
    bool m_policy_dirty{false};
    bool m_policy_readback_pending{false};
    QString m_pending_policy_hash;
    bool m_policy_snapshot_representable{true};
    UniValue m_unrepresentable_policy_snapshot{UniValue::VOBJ};
    bool m_provider_safety_configured{false};
    bool m_provider_safety_snapshot_representable{true};
    UniValue m_unrepresentable_provider_safety_snapshot{UniValue::VOBJ};
    bool m_provider_safety_dirty{false};
    bool m_loading_provider_safety{false};
    bool m_loading_liquidity_policy{false};
    bool m_liquidity_policy_configured{false};
    bool m_liquidity_snapshot_representable{true};
    UniValue m_unrepresentable_liquidity_snapshot{UniValue::VOBJ};
    bool m_liquidity_targets_satisfy_provider_policy{false};
    bool m_liquidity_policy_dirty{false};
    bool m_client_safety_configured{false};
    bool m_client_safety_snapshot_representable{true};
    UniValue m_unrepresentable_client_safety_snapshot{UniValue::VOBJ};
    bool m_client_safety_dirty{false};
    bool m_loading_client_safety{false};
    bool m_core_eligible{false};
    bool m_core_enabled{false};
    bool m_core_running{false};
    bool m_core_ready{false};
    bool m_core_locked{true};
    bool m_core_has_identity{false};
    bool m_provider_info_snapshot_available{false};
    bool m_provider_safety_snapshot_available{false};
    bool m_liquidity_snapshot_available{false};
    bool m_provider_settings_present{false};
    OracleState m_oracle_state{OracleState::UNKNOWN};
    qint64 m_oracle_price_micro_usd{0};
    bool m_loading_runtime_settings{false};
    bool m_runtime_settings_dirty{false};
    bool m_autostart_enabled{false};
    QString m_operation_mode{QStringLiteral("automatic")};
    QString m_service_state{QStringLiteral("stopped")};
    QString m_last_service_error;
    QString m_maintenance_state{QStringLiteral("unavailable")};
    QStringList m_readiness_errors;
    qint64 m_carrier_base_cents{0};
    qint64 m_carrier_excess_cents{0};
    qint64 m_pending_successor_carrier_cents{0};
    qint64 m_pending_successor_dgb_satoshis{0};
    QString m_excess_plan_id;
    QString m_release_plan_id;
    qint64 m_excess_plan_expires_at{0};
    qint64 m_release_plan_expires_at{0};
    qint64 m_excess_preview_cents{0};
    qint64 m_excess_preview_fee_satoshis{0};
    qint64 m_waiting_provider_requests{0};
    qint64 m_waiting_provider_submits{0};
    int m_pool_admission_dgb{0};
    int m_pool_admission_carriers{0};
    int m_pool_operational_dgb{0};
    int m_pool_operational_carriers{0};
    bool m_pool_status_loaded{false};
    bool m_setup_waiting_for_confirmations{false};
    bool m_privacy{false};
    bool m_setup_wizard_active{false};
    QTimer* m_setup_status_timer{nullptr};
    QPointer<PaymasterSetupWizard> m_setup_wizard;
};

#endif
