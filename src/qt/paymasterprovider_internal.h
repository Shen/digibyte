// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERPROVIDER_INTERNAL_H
#define DIGIBYTE_QT_PAYMASTERPROVIDER_INTERNAL_H
#include <qt/paymasterproviderviewhelpers.h>
#include <qt/paymasterasync.h>

using namespace PaymasterQt::ProviderViews;

class PaymasterWidgetImpl;

struct ProviderTypes
{
    struct PageBindings {
        QScrollArea* connection_scroll{nullptr};
        QScrollArea* management_scroll{nullptr};
        QScrollArea* lifecycle_scroll{nullptr};
        QScrollArea* configuration_scroll{nullptr};
        QScrollArea* safety_scroll{nullptr};
        QScrollArea* liquidity_scroll{nullptr};
        QScrollArea* automation_scroll{nullptr};
        QScrollArea* diagnostics_scroll{nullptr};
        QWidget* overview{nullptr};
        QWidget* connection{nullptr};
        QWidget* management{nullptr};
        QWidget* management_column{nullptr};
        QWidget* liquidity_column{nullptr};
        QWidget* diagnostics_column{nullptr};
        QWidget* details{nullptr};
        QWidget* advanced_liquidity{nullptr};
        QVBoxLayout* overview_layout{nullptr};
        QVBoxLayout* connection_layout{nullptr};
        QVBoxLayout* network_layout{nullptr};
        QVBoxLayout* management_layout{nullptr};
        QVBoxLayout* lifecycle_page_layout{nullptr};
        QVBoxLayout* configuration_layout{nullptr};
        QVBoxLayout* safety_layout{nullptr};
        QVBoxLayout* liquidity_layout{nullptr};
        QVBoxLayout* automation_layout{nullptr};
        QVBoxLayout* diagnostics_layout{nullptr};
        QVBoxLayout* advanced_liquidity_layout{nullptr};
        QVBoxLayout* details_layout{nullptr};
        QVBoxLayout* task_layout{nullptr};
        QGroupBox* network_group{nullptr};
        QGroupBox* automatic_maintenance{nullptr};
        PaymasterResponsiveCards* funds_metrics{nullptr};
        QPushButton* details_toggle{nullptr};
        QPushButton* refresh{nullptr};
        QPushButton* create_identity{nullptr};
        QPushButton* restore_quote_defaults{nullptr};
        QPushButton* restore_client_defaults{nullptr};
        QPushButton* refresh_activity{nullptr};
        QPushButton* activity_details_toggle{nullptr};
        QHBoxLayout* task_actions{nullptr};
    };
    struct OfferDraft {
        QString display_name;
        bool user_paid{false};
        bool sponsored{false};
        QString scope;
        int fee_bps{0};
        int maximum_service_fee{0};
        int minimum{0};
        int maximum{0};
        int quote_ttl{0};
        qint64 network_fee{0};
    };
    struct QuoteLimits {
        int total{0};
        int netgroup{0};
        int recipient{0};
        int requests{0};
    };
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

    enum class PreparationDetail { Technical, Operator, Summary };

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
};

/** Shared lifetime/translation context only; workflow state stays in each component. */
class ProviderComponent : public QObject, public ProviderTypes
{
public:
    explicit ProviderComponent(PaymasterWidgetImpl& panel);
    QWidget* widget() const;
    static QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
    {
        return QWidget::tr(text, disambiguation, n);
    }
protected:
    PaymasterWidgetImpl& m_panel;
};

class ProviderOverview final : public ProviderComponent
{
public:
    void connectOverview(PageBindings& pages);
    void resetWalletPresentation();
    void resetTaskPresentation();
    void setPrivacy(bool privacy);
    void alignForms();
    void buildDiagnostics(PageBindings& pages);
    void buildActivity(PageBindings& pages);
    void buildOverview(PageBindings& pages);
    using ProviderComponent::ProviderComponent;
    void overviewFinanceUnavailable();
    void applyOverviewFinance(const UniValue& result);
    void showFinanceReconciliation();
    void noteFinanceObservation();
    void connectFinanceAction();
    void buildCapitalActions(PageBindings& pages);
    void buildLifecycleControls(PageBindings& pages, QWidget* lifecycle_column);
    bool hasIdentity() const { return m_core_has_identity; }
    const QString& providerId() const { return m_provider_id; }
    bool running() const { return m_core_running; }
    bool enabled() const { return m_core_enabled; }
    bool eligible() const { return m_core_eligible; }
    bool providerInfoAvailable() const { return m_provider_info_snapshot_available; }
    bool settingsPresent() const { return m_provider_settings_present; }
    const UniValue& operatorSnapshot() const { return m_operator_snapshot; }
    const QStringList& readinessErrors() const { return m_readiness_errors; }
    const QString& serviceState() const { return m_service_state; }
    const QString& lastServiceError() const { return m_last_service_error; }
    const QString& operationMode() const { return m_operation_mode; }
    void showConfiguredContent(bool visible) { m_setup_content->setVisible(visible); }
    void applySetupServiceState(bool enabled, bool running) { m_core_enabled = enabled; m_core_running = running; }
    void showOperatorCard(bool visible) { m_operator_card->setVisible(visible); }
    void attachTaskCard(QVBoxLayout* layout);
    void applyReadinessFixture(const UniValue& status);
    void observeOperatorSnapshot(const UniValue& status) { m_operator_snapshot = status; }
    void expireSnapshot() { m_status_age.invalidate(); }
    void beginObservation() { m_status_age.restart(); }
    void setMutationSnapshotForTesting(bool available) { m_provider_info_snapshot_available = available; }
    bool snapshotCurrent() const { return m_status_age.isValid() && m_status_age.elapsed() <= 30000; }
    void beginRpcRead() { m_operator_read_elapsed.invalidate(); }
    void applyBusyState(bool busy);
    void redactPrivacyText();
    QString readinessText() const { return m_readiness_summary->text(); }
    void applyRuntimeSettings(const QString& mode) { m_operation_mode = mode; m_provider_settings_present = true; }
    void recordSettingsPresent() { m_provider_settings_present = true; }
    void requireRestart() { m_operator_restart_required = true; }
    void invalidateProviderObservation() { m_operator_snapshot = UniValue{}; m_status_age.invalidate(); m_provider_info_snapshot_available = false; }
    void gateAutostartPending();
    void buildEnableStatus(QVBoxLayout* layout, QWidget* parent);
    void recordStopped() { m_core_enabled = m_core_running = false; }
    void recordIdentity(const QString& provider_id) { m_provider_id = provider_id; }
    void invalidateOperatorSnapshot() { m_operator_snapshot = UniValue{}; }
    void setStopStatus(const QString& message, const QString& kind);
    void updateWithdrawalHint(const QString& message, bool visible);
    void closeTaskErrorDetails() { m_task_error_toggle->setChecked(false); }
    void recordAcceptedTask(const QString& title);
    void showPendingPreparation();
    void applyProviderInfo(const UniValue& result);

    void updateIdentityLabels();

    void presentProviderStartResult(const UniValue& result);

    static bool isPassiveExternalReadinessError(const QString& error);

    bool hasPassiveExternalWait() const;

    void updateAutomaticRefreshTimer();

    void updateProviderHeaderStatus();

    void updateExternalPrerequisites();

    void refreshOracleStatus();

    void clearOverviewCapital();

    void setCapitalMetric(const char* name, const QString& text, const QString& kind);

    void setFundsValue(const QString& key, const QString& text);

    bool confirmedUserPaid() const;

    qint64 confirmedPaymentFee() const;

    static QString slotStatusText(const QString& name,
                                  const UniValue& status);

    bool carrierPlanCurrent(const QString& plan_id, qint64 expires_at) const;

    QString readinessExplanation(const QString& error) const;

    void updateReadinessSummary(const QStringList& errors, bool running, bool ready);

    static QLineEdit* integerField(QWidget* parent, qint64 value);

    QString providerStartConfirmationText() const;

    QString providerServiceStatusText() const;

    // Use the existing atomic disable-and-stop RPC so autostart cannot race a
    // setting change. Unlike a persistent operator pause, this retains the
    // saved autostart preference. All callbacks use the wallet-bound RPC chain.




    bool canStartProvider() const;

    void updateProviderButtons();

    void updateOperatorDashboard();

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

    void unlockOperatorWallet(std::function<void()> on_unlocked = {});

    void stopOperator(bool retire = false);

    void pauseOperator();

    void resumeOperator();

    void renderCurrentTask();

    void requestReviewedStart();

    void startProvider();

    static QString activityText(const UniValue& object, const char* name);

    QString reservationPurpose(const UniValue& entry) const;

    QString reservationAsset(const UniValue& entry) const;

    QString reservationState(const UniValue& entry) const;

    void showReservations(const UniValue& result);

    QString formatActivityResult(const UniValue& result, bool submitted_payment) const;

    void refreshActivity();

    void processActivityRequest();

    void processActivitySubmit();

private:
    uint64_t m_oracle_request_generation{0};
    QMap<QString, QLabel*> m_live_status;
    QMap<QString, QLabel*> m_live_details;
    QLabel* m_overview_finance_status{nullptr};
    QWidget* m_overview_finance_details{nullptr};
    QPushButton* m_overview_finance_action{nullptr};
    QElapsedTimer m_overview_finance_age;
    bool m_overview_finance_observed{false};
    QPushButton* m_review_dgb{nullptr};
    QWidget* m_setup_content{nullptr};
    QLabel* m_overview_offer_status{nullptr};
    QLabel* m_overview_safety_status{nullptr};
    QWidget* m_overview_budget_details{nullptr};
    QLabel* m_overview_liquidity_status{nullptr};
    QLabel* m_overview_operation_status{nullptr};
    QWidget* m_overview_capital_details{nullptr};
    QPushButton* m_overview_offer_action{nullptr};
    QPushButton* m_overview_safety_action{nullptr};
    QPushButton* m_overview_liquidity_action{nullptr};
    QPushButton* m_overview_operation_action{nullptr};
    QElapsedTimer m_status_age;
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
    QLabel* m_diagnostic_endpoint{nullptr};
    QLabel* m_diagnostic_hint{nullptr};
    QPlainTextEdit* m_diagnostic_rpc{nullptr};
    QPlainTextEdit* m_diagnostic_cli{nullptr};
    QPushButton* m_diagnostic_rpc_copy{nullptr};
    QPushButton* m_diagnostic_cli_copy{nullptr};
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
    QString m_operator_next_action{QStringLiteral("refresh")};
    QString m_operator_next_area;
    QLabel* m_operator_summary{nullptr};
    UniValue m_operator_snapshot;
    bool m_operator_restart_required{false};
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
    QLabel* m_enable_status;
    QString m_provider_id;
    QString m_provider_endpoint;
    QLabel* m_activity_runtime_status;
    QLabel* m_activity_action_result;
    QLabel* m_activity_summary;
    QTableWidget* m_activity_reservations{nullptr};
    QLabel* m_activity_tasks{nullptr};
    QPushButton* m_operation_primary{nullptr};
    QGroupBox* m_reservations_group{nullptr};
    QGroupBox* m_recovery_group{nullptr};
    QGroupBox* m_request_processing_group{nullptr};
    QGroupBox* m_submit_processing_group{nullptr};
    QGroupBox* m_activity_result_group{nullptr};
    QPushButton* m_process_requests;
    QPushButton* m_process_submits;
    QPlainTextEdit* m_activity_output;
    bool m_core_eligible{false};
    bool m_core_enabled{false};
    bool m_core_running{false};
    bool m_core_ready{false};
    bool m_core_locked{true};
    bool m_core_has_identity{false};
    bool m_provider_info_snapshot_available{false};
    bool m_provider_settings_present{false};
    OracleState m_oracle_state{OracleState::UNKNOWN};
    qint64 m_oracle_price_micro_usd{0};
    QString m_operation_mode{QStringLiteral("automatic")};
    QString m_service_state{QStringLiteral("stopped")};
    QString m_last_service_error;
    QStringList m_readiness_errors;
    qint64 m_waiting_provider_requests{0};
    qint64 m_waiting_provider_submits{0};
};

class ProviderConfiguration final : public ProviderComponent
{
public:
    void connectConfiguration(PageBindings& pages);
    void alignForms();
    void observePolicy(const UniValue& provider);
    OfferDraft offerDraft() const;
    QuoteLimits quoteLimits() const;
    std::optional<FundingSafetyValues> fundingSafety(DigiDollar::Paymaster::FundingModel model,
        DigiDollar::Paymaster::SponsorshipScope scope = DigiDollar::Paymaster::SponsorshipScope::PUBLIC) const;
    std::optional<FundingSafetyValues> selectedFundingSafety() const;
    void buildRuntimeSettings(PageBindings& pages, QWidget* operator_card);
    void buildNodeEditor(PageBindings& pages, QWidget* connection_column);
    void buildWalletAccess(PageBindings& pages);
    void attachOperatorWarning(QVBoxLayout* layout);
    QString safetyLimitExplanation() const;
    QStringList fundingBudgetSummary() const;
    void updateControls(bool running);
    void setBusy(bool busy);
    void hideRuntimeSettings();
    void refreshFundingSafetyDisplays();
    void clearConfigurationWarnings();
    void observeRuntime(const UniValue& provider);
    void applySetupStatus(const UniValue& provider);
    void markPersistentlyStopped();
    void discardProviderEdits();
    void setSnapshotAvailabilityForTesting(bool policy, bool safety);
    void updateIdentityPresentation(bool exists, const QString& provider_id);
    void updateIdentityText(const QString& text);
    bool autostartEnabled() const { return m_autostart_enabled; }
    bool autostartPending() const { return m_autostart_dirty || m_autostart_pending_read; }
    bool policyDirty() const { return m_policy_dirty; }
    bool policyLoaded() const { return m_policy_loaded; }
    bool policyRepresentable() const { return m_policy_snapshot_representable; }
    bool providerSafetyConfigured() const { return m_provider_safety_configured; }
    bool providerSafetyDirty() const { return m_provider_safety_dirty; }
    bool providerSafetyAvailable() const { return m_provider_safety_snapshot_available; }
    bool runtimeDirty() const { return m_runtime_settings_dirty; }
    bool autostartDirty() const { return m_autostart_dirty; }
    bool nodeDialogActive() const { return !m_node_connection_dialog.isNull(); }
    bool nodeEditorActive() const { return !m_inline_node_editor.isNull(); }
    bool userPaidSelected() const { return m_user_paid->isChecked(); }

    const UniValue& preservedPolicy() const { return m_unrepresentable_policy_snapshot; }
    void applyWithProviderRestart(std::function<void(bool)> apply, std::function<void(const QString&)> failed);
    void finishProviderSettingsRestart(std::function<bool(const UniValue&)> verified, std::function<void(const QString&)> failed);
    void closeWalletDialogs();
    void resetWalletPresentation();
    void setPrivacy(bool privacy);
    void buildAutomation(PageBindings& pages);
    void buildSafety(PageBindings& pages);
    void buildConfiguration(PageBindings& pages);
    void buildConnection(PageBindings& pages);
    using ProviderComponent::ProviderComponent;
    bool providerConfigurationComplete() const;

    void restorePolicyDefaults();

    void applyRecommendedPolicyDefaults(bool mark_dirty);

    void setPolicyMutationEnabled(bool enabled);

    void loadPolicy(const UniValue& policy);

    QWidget* configurationWarningPanel(QLabel*& label, const char* name, const QString& button_text, std::function<void()> navigate);

    QString configurationConflictText(const UniValue& offer, const UniValue& safety) const;

    void updateConfigurationWarnings();

    void updatePolicyDisplay();

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

    void discardAutostart();

    void reconcileAutostart();

    void updateAutostartControls();

    void saveAutostart();

    void saveRuntimeSettings();

    void configureOperatorNode();

    void configureOperatorNodeFrom(const UniValue& current);

    QWidget* createNodeConfigEditor(const UniValue& current, QWidget* parent);

    void saveProviderSafetyPolicy();

    void invalidateSavedConfigurationDisplay();

    void saveClientSafetyPolicy();

    QString safetyClassStatus(const QString& name, const UniValue& value) const;

    void applyProviderSafetyStatus(const UniValue& result);

    void refreshProviderSafetyStatus();

    void refreshClientSafetyStatus();

    void showPolicySaveStatus(const QString& message);

    void reviewPolicySpendingLimits();

    void savePolicy();

private:
    QList<QFormLayout*> m_offer_forms;
    uint64_t m_settings_restart_generation{0};
    QWidget* m_user_paid_pricing{nullptr};
    QWidget* m_node_editor_host{nullptr};
    QVBoxLayout* m_node_editor_layout{nullptr};
    QPointer<QWidget> m_inline_node_editor;
    QList<QCheckBox*> m_autostart_copies;
    QPointer<QDialog> m_node_connection_dialog;
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
    UniValue m_unrepresentable_policy_snapshot{UniValue::VOBJ};
    bool m_policy_snapshot_representable{true};
    bool m_provider_safety_configured{false};
    bool m_provider_safety_snapshot_representable{true};
    UniValue m_unrepresentable_provider_safety_snapshot{UniValue::VOBJ};
    bool m_provider_safety_dirty{false};
    bool m_loading_provider_safety{false};
    bool m_client_safety_configured{false};
    bool m_client_safety_snapshot_representable{true};
    UniValue m_unrepresentable_client_safety_snapshot{UniValue::VOBJ};
    bool m_client_safety_dirty{false};
    bool m_loading_client_safety{false};
    bool m_provider_safety_snapshot_available{false};
    bool m_loading_runtime_settings{false};
    bool m_runtime_settings_dirty{false};
    bool m_autostart_enabled{false};
};

class ProviderLiquidity final : public ProviderComponent
{
public:
    bool snapshotAvailable() const { return m_liquidity_snapshot_available; }
    bool snapshotRepresentable() const { return m_liquidity_snapshot_representable; }
    bool policyConfigured() const { return m_liquidity_policy_configured; }
    bool policyDirty() const { return m_liquidity_policy_dirty; }
    bool targetsSatisfyProviderPolicy() const { return m_liquidity_targets_satisfy_provider_policy; }
    const QString& maintenanceState() const { return m_maintenance_state; }
    bool poolStatusLoaded() const { return m_pool_status_loaded; }
    int operationalCarrierCount() const { return m_pool_operational_carriers; }
    const UniValue& overviewSnapshot() const { return m_overview_liquidity_snapshot; }
    QString automationSummary() const;
    void updateQuickAutomation();
    void observePoolCounts(const UniValue& pool);
    void seedPoolCountsForTesting();
    void setMutationSnapshotForTesting(bool available);
    void invalidateOverviewSnapshot() { m_overview_liquidity_snapshot = UniValue{}; }
    void applyPreparationSnapshot(const UniValue& preparation) { m_pool_preparation = preparation; }
    void disableMutationControls();
    void redactPrivacyText();
    void showAdvancedSettings();
    void collapseAdvancedSettings() { if (m_liquidity_advanced_toggle) m_liquidity_advanced_toggle->setChecked(false); }
    void discardPolicyEdits() { m_liquidity_policy_dirty = false; }
    void focusPreparationFee();
    bool preparationFee(qint64& fee) const { return m_preparation_fee->satoshis(fee); }
    void buildMaintenanceCard(QWidget* parent);
    const PaymasterOperationController& operationView() const { return m_operation; }
    void observeOperation(const UniValue& status);
    void resetOperation();
    void acceptSetupOperation();
    bool guidedWaitingForProvider() const { return m_guided_waiting_for_provider; }
    bool guidedStartAfter() const { return m_guided_start_after; }
    bool carrierReleaseNeedsReview() const;
    bool canReviewReserveReduction() const;
    bool waitForReserveReduction(const QString& error, std::function<void()> retry);
    void attachCapitalReleaseControl(QVBoxLayout* layout);
    void connectLiquidity(PageBindings& pages);
    void closeWalletDialogs();
    void resetWalletPresentation();
    void setPrivacy(bool privacy);
    void buildLiquidity(PageBindings& pages);
    using ProviderComponent::ProviderComponent;
    static bool isLiquidityReadinessError(const QString& error);

    bool shouldStartAndRestoreLiquidity() const;

    bool maintenanceFeeLimitExceeded() const;

    bool maintenanceWaitingForFunds() const;

    void renderOverviewPool();

    std::optional<qint64> savedRefillCapacity() const;

    bool hasExcessSavedReserves() const;

    bool savedPoolTargetsReady() const;

    void reviewMaintenanceFeeLimit();

    bool readLiquidityPolicy(LiquidityPolicyValues& values) const;

    static UniValue liquidityPolicyToJSON(
        const LiquidityPolicyValues& values);

    bool liquidityPolicyCanRepresent(const UniValue& policy) const;

    void setLiquidityPolicyMutationEnabled(bool enabled);

    void loadLiquidityPolicy(const UniValue& policy, bool configured);

    void markLiquidityPolicyDirty();

    QString liquidityApprovalText(
        const LiquidityPolicyValues& values) const;

    void saveLiquidityPolicy();

    void liquidityPolicySaved(const UniValue& policy, bool reduced_targets = false);

    void reconcileLiquidityPolicySave(const UniValue& requested, const QString& reason, bool reduced_targets = false);

    void approveSuggestedLiquidityMaintenance();

    void applyLiquidityStatus(const UniValue& result);

    void refreshLiquidityStatus();

    void applyLiquidityPoolEntries(const UniValue& result,
                                   bool refresh_safety);

    void refreshLiquidityPoolEntries();

    QString refillFundingExplanation(const QString& error) const;

    void addQuickRefill(QVBoxLayout* layout, QWidget* parent, bool overview);

    void saveQuickRefill(bool enabled);

    bool repairableLiquidityGap() const;

    void failGuidedTask(const QString& error);

    void beginGuidedTask(const QString& task, bool start_after = false);

    void previewGuidedTask(const QString& task);

    void applyReviewedReserveRepair(const UniValue& context, const UniValue& proposed, const UniValue& preview_params);

    void executeGuidedTask(const std::string& command, const UniValue& params);

    QString liquidityTargetKey() const;

    QString preparationTargetKey() const;

    void invalidatePoolPreviews();

    void restoreLiquidityDefaults(bool announce = true);

    void updateSavedReserveState();

    void updateLiquidityDisplay();

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

private:
    UniValue m_overview_liquidity_snapshot;
    PaymasterOperationController m_operation;
    QPushButton* m_cancel_preparation{nullptr};
    QString m_guided_task;
    QString m_failed_guided_task;
    uint64_t m_guided_generation{0};
    int m_guided_busy_retries{0};
    bool m_guided_waiting_for_provider{false};
    QString m_guided_review_text;
    bool m_guided_start_after{false};
    QList<QCheckBox*> m_refill_switches;
    QList<QLabel*> m_refill_switch_status;
    UniValue m_saved_liquidity_policy;
    UniValue m_liquidity_edit_revision;
    bool m_refill_save_pending{false};
    QString m_refill_message;
    QPointer<QDialog> m_refill_approval;
    QGroupBox* m_liquidity_maintenance_card{nullptr};
    QLabel* m_pool_preparation_status{nullptr};
    DgbAmountLineEdit* m_preparation_fee{nullptr};
    UniValue m_pool_preparation;
    QLabel* m_liquidity_maintenance_state{nullptr};
    QLabel* m_liquidity_maintenance_next_step{nullptr};
    QLabel* m_liquidity_maintenance_cost{nullptr};
    QPushButton* m_approve_liquidity_maintenance{nullptr};
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
    QPushButton* m_prepare_execute;
    QPushButton* m_rebalance_execute;
    QPushButton* m_prepare_preview{nullptr};
    QPushButton* m_rebalance_preview{nullptr};
    QPlainTextEdit* m_liquidity_output;
    QString m_prepare_preview_target;
    QString m_rebalance_preview_target;
    QString m_prepare_plan_id;
    QString m_rebalance_plan_id;
    bool m_loading_liquidity_policy{false};
    bool m_liquidity_policy_configured{false};
    bool m_liquidity_snapshot_representable{true};
    UniValue m_unrepresentable_liquidity_snapshot{UniValue::VOBJ};
    bool m_liquidity_targets_satisfy_provider_policy{false};
    bool m_liquidity_policy_dirty{false};
    bool m_liquidity_snapshot_available{false};
    QString m_maintenance_state{QStringLiteral("unavailable")};
    qint64 m_pending_successor_carrier_cents{0};
    qint64 m_pending_successor_dgb_satoshis{0};
    int m_pool_admission_dgb{0};
    int m_pool_admission_carriers{0};
    int m_pool_operational_dgb{0};
    int m_pool_operational_carriers{0};
    bool m_pool_status_loaded{false};
};

class ProviderFinance final : public ProviderComponent
{
public:
    void resetWalletPresentation();
    void setPrivacy(bool privacy);
    void setHistoryMessage(const QString& message);
    void setExportFilenameForTesting(QString filename);
    void clearFundsValues();
    void setFundsValue(const QString& key, const QString& text);
    void connectFinance(PageBindings& pages);
    void buildFinance(PageBindings& pages);
    QLabel* fundsValue(const QString& key, QWidget* parent, const QString& caption, const char* role = "financeValue");
    static QLabel* financeLabel(const QString& text, QWidget* parent, const char* role);
    using ProviderComponent::ProviderComponent;
    static qint64 financeNumber(const UniValue& object, const char* name);

    static QString financeEventKind(const QString& kind);

    static QString financeEventState(const QString& state);

    static QString financeFundingModel(const UniValue& event);

    void refreshFinanceStatus();
    enum class FinanceRead { OVERVIEW, PAGE, EXPORT };
    bool retryFinanceReconciliation(FinanceRead read, uint64_t serial, std::function<void()> retry);



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


private:
    static constexpr int FINANCE_PAGE_SIZE{250};
    QLabel* m_finance_selected_summary{nullptr};
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
    std::array<uint64_t, 3> m_read_serials{};
};

class ProviderRecovery final : public ProviderComponent
{
public:
    void resetWalletPresentation();
    void setPrivacy(bool privacy);
    void clearRetirementSummary();
    void setBackupRequestHandler(std::function<void()> handler);
    void buildBackupControls(PageBindings& pages, QWidget* connection_column, QWidget* overview_column);
    void buildCarrierControls(PageBindings& pages, QWidget* liquidity);
    void focusBackupControls();
    using CarrierChoice = std::pair<QString, QString>;
    void applyCarrierBalances(qint64 base, qint64 excess, bool configured);
    void applyCarrierChoices(const std::vector<CarrierChoice>& choices, bool release_enabled);
    void clearCarrierChoices();
    std::vector<CarrierChoice> carrierChoices() const;
    bool hasReleasableCarrier() const;
    void selectCarrier(const QString& outpoint);
    qint64 carrierExcess() const { return m_carrier_excess_cents; }
    void updateBackupAdoptionAvailability(const QStringList& errors);
    void disableWithdrawalControls();
    void refreshBackupReminder();
    void showBackupAdoptionMessage(const QString& message);
    void showRestoreIssue(const QString& code);
    void connectRecovery(PageBindings& pages);
    void buildRecoveryPages(PageBindings& pages);
    using ProviderComponent::ProviderComponent;
    void updateBackupReminder(bool required, const QString& provider_id);

    void reviewBackupAdoption(bool stopped = false);

    void requestProviderBackup();

    void acknowledgeExternalBackup();

    void invalidateCarrierWithdrawalPreviews();

    void updateCarrierWithdrawalButtons();

    UniValue carrierWithdrawalOptions(const QString& mode, bool execute,
                                      const QString& plan_id = {}) const;

    void carrierWithdrawalAction(const QString& mode, bool);

    void reviewFullCapitalRelease(bool retire = false, bool recovery = false);

    void reviewRetirementBalances(bool recovery = false);

    // Operating-capital actions retain the guarded start-and-refill path.
    // Overview and lifecycle settings share the general start/pause controls.
    void updateEarningsWithdrawalHint();

private:
    QGroupBox* m_overview_backup_notice{nullptr};
    QPushButton* m_overview_backup_now{nullptr};
    QLabel* m_overview_backup_provider_id{nullptr};
    QPushButton* m_overview_backup_external{nullptr};
    QLabel* m_backup_summary{nullptr};
    QPushButton* m_backup_adopt{nullptr};
    QLabel* m_backup_adopt_status{nullptr};
    QPushButton* m_backup_notice{nullptr};
    QGroupBox* m_retirement_summary{nullptr};
    QLabel* m_retirement_balances{nullptr};
    QPushButton* m_retirement_backup{nullptr};
    QLabel* m_retirement_backup_status{nullptr};
    QLabel* m_carrier_value_status{nullptr};
    QLabel* m_carrier_withdrawal_status{nullptr};
    QPushButton* m_preview_carrier_excess{nullptr};
    QPushButton* m_execute_carrier_excess{nullptr};
    QComboBox* m_release_carrier_select{nullptr};
    QPushButton* m_preview_carrier_release{nullptr};
    QPushButton* m_execute_carrier_release{nullptr};
    bool m_backup_required{false};
    std::function<void()> m_provider_backup_request_handler;
    qint64 m_carrier_base_cents{0};
    qint64 m_carrier_excess_cents{0};
    QString m_excess_plan_id;
    QString m_release_plan_id;
    qint64 m_excess_plan_expires_at{0};
    qint64 m_release_plan_expires_at{0};
    qint64 m_excess_preview_cents{0};
    qint64 m_excess_preview_fee_satoshis{0};
};

class ProviderSetup final : public ProviderComponent
{
public:
    void connectSetup(PageBindings& pages);
    bool wizardActive() const { return m_setup_wizard_active; }
    bool isModalCompatible(QWidget* modal) const { return !modal || modal == m_setup_wizard.data(); }
    PaymasterSetupMode mode() const { return m_setup_mode; }
    bool waitingForConfirmations() const { return m_setup_waiting_for_confirmations; }
    void initializeStatusTimer();
    void configureStatusTimer(bool pending, bool running, bool foreground);
    void observeProviderStatus(bool has_existing_configuration, bool ready);
    void attachReviewAction(QVBoxLayout* layout);
    void reviewSetup();
    void setBusy(bool busy);

    void closeWalletDialogs();
    void resetWalletPresentation();
    void setPrivacy(bool privacy);
    void buildChoice(PageBindings& pages);
    using ProviderComponent::ProviderComponent;
    static bool isProviderSetupReadinessError(const QString& error);

    PaymasterSetupMode loadSetupMode() const;

    void setSetupMode(PaymasterSetupMode mode);

    void updateSetupAccess();

    bool showSetupWizard();

private:
    struct SetupWizardContext;
    struct SetupOfferControls;
    struct SetupSafetyControls;
    struct SetupPoolControls;
    struct SetupReviewControls;
    struct SetupProgressControls;

    bool setupWizardAvailable();
    QCheckBox* buildSetupWalletPages(PaymasterSetupWizard& wizard, const SetupWizardContext& context);
    void buildSetupIdentityPage(PaymasterSetupWizard& wizard, SetupOfferControls& offer);
    void buildSetupFundingPage(PaymasterSetupWizard& wizard, SetupOfferControls& offer);
    void buildSetupPolicyPage(PaymasterSetupWizard& wizard, SetupOfferControls& offer);
    SetupOfferControls buildSetupOfferPage(PaymasterSetupWizard& wizard, const std::shared_ptr<bool>& replace_unrepresentable_policy);
    SetupSafetyControls buildSetupSafetyPage(PaymasterSetupWizard& wizard, const SetupOfferControls& offer);
    SetupPoolControls buildSetupLiquidityPage(PaymasterSetupWizard& wizard, const SetupOfferControls& offer, const SetupSafetyControls& safety);
    SetupReviewControls buildSetupReviewPage(PaymasterSetupWizard& wizard, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool);
    static QString setupStepLabel(const std::string& method);
    SetupProgressControls buildSetupProgressPage(PaymasterSetupWizard& wizard);
    void addSetupStartChoice(SetupReviewControls& review_controls, const SetupPoolControls& pool);
    UniValue readSetupPolicy(const SetupOfferControls& offer);
    UniValue readSetupPoolOptions(const SetupPoolControls& pool, bool execute);
    DigiDollar::Paymaster::SetupChoices readSetupChoices(const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool);
    QString formatSetupPoolPreview(const UniValue& result);
    void renderSetupReview(const SetupWizardContext& context, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool, const SetupReviewControls& review_controls);
    QString setupReviewError(const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupReviewControls& review_controls);
    void connectSetupReview(PaymasterSetupWizard& wizard, const SetupWizardContext& context, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool, const SetupReviewControls& review_controls);
    void renderSetupStatus(const QPointer<QLabel>& funding_label, const UniValue& expected, const UniValue& current);
    std::shared_ptr<bool> connectSetupExecution(PaymasterSetupWizard& wizard, const SetupWizardContext& context, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool, const SetupProgressControls& progress);

    PaymasterSetupMode m_setup_mode{PaymasterSetupMode::UNDECIDED};
    QGroupBox* m_setup_choice{nullptr};
    QPushButton* m_guided_setup{nullptr};
    QPushButton* m_expert_setup{nullptr};
    QPushButton* m_reopen_wizard{nullptr};
    bool m_setup_waiting_for_confirmations{false};
    bool m_setup_wizard_active{false};
    QTimer* m_setup_status_timer{nullptr};
    QPointer<PaymasterSetupWizard> m_setup_wizard;
};

class PaymasterWidgetImpl final : public DigiDollarPaymasterWidget, public ProviderTypes
{
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

    bool hasRpcTransport() const;

    bool hasCompleteMutationSnapshots() const;

    static QString dgbAmount(qint64 satoshis);

    static QString compactDgbAmount(qint64 satoshis);

    static QString ddAmount(qint64 cents);

    void setStatusLabel(QLabel* label, const QString& text,
                        const QString& kind) const;

    void setStatusKind(QWidget* widget, const QString& kind) const;

    bool isOperatorPage(QWidget* page) const;

    void showOperatorPage(QWidget* page);

    void updateNavigationLayout();

    void resizeEvent(QResizeEvent* event) override;

    void setPageAvailable(QWidget* page, bool enabled, const QString& explanation);

    static QSpinBox* spin(QWidget* parent, int minimum, int maximum, int value);

    static QLineEdit* dgbAmountField(QWidget* parent, qint64 satoshis);

    static QSpinBox* scaledSpin(QWidget* parent, int minimum, int maximum,
                                int value, int scale, int decimals);

    QString yesNo(bool value) const;

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


    // Components observe shared wallet/navigation context; their mutable workflow
    // state stays private to the component that owns it.
    ProviderOverview& overview() { return m_overview; }
    ProviderConfiguration& configuration() { return m_configuration; }
    ProviderLiquidity& liquidity() { return m_liquidity; }
    ProviderFinance& finance() { return m_finance; }
    ProviderRecovery& recovery() { return m_recovery; }
    ProviderSetup& setup() { return m_setup; }
    WalletModel* walletModel() const { return m_model; }
    bool privacyEnabled() const { return m_privacy; }
    bool busy() const { return m_busy; }
    bool rpcActive() const { return m_rpc_active; }
    int rpcHandlerDepth() const { return m_rpc_handler_depth; }
    uint64_t walletGeneration() const { return m_wallet_generation; }
    const PaymasterQt::RpcError& rpcFailure() const { return m_rpc_failure; }
    const std::string& activeRpcCommand() const { return m_active_rpc_command; }
    bool hasSynchronousTestExecutor() const { return bool(m_rpc_executor_for_testing); }
    bool hasPendingOperatorReview() const { return bool(m_pending_operator_review); }
    void deferOperatorReview(std::function<void()> review) { m_pending_operator_review = std::move(review); }
    void deferHistoryStatus() { m_history_deferred_status = true; }
    void showStatus(const QString& text) { m_status->setText(text); }
    void updateLastChecked(const QString& text) { if (m_last_checked->text() != text) m_last_checked->setText(text); }
    QStackedWidget* mainPages() const { return m_tabs; }
    QStackedWidget* settingsPages() const { return m_settings_tabs; }
    enum class Page { AUTOMATION, LIFECYCLE, CONFIGURATION, SAFETY, LIQUIDITY, FINANCE, ACTIVITY, CONNECTION, DIAGNOSTICS, MANAGEMENT };
    QWidget* page(Page page) const
    {
        switch (page) {
        case Page::AUTOMATION: return m_automation_page;
        case Page::LIFECYCLE: return m_lifecycle_page;
        case Page::CONFIGURATION: return m_configuration_page;
        case Page::SAFETY: return m_safety_page;
        case Page::LIQUIDITY: return m_liquidity_page;
        case Page::FINANCE: return m_finance_page;
        case Page::ACTIVITY: return m_activity_page;
        case Page::CONNECTION: return m_connection_page;
        case Page::DIAGNOSTICS: return m_diagnostics_page;
        case Page::MANAGEMENT: return m_management_page;
        }
        return nullptr;
    }
    void bindPage(Page page, QWidget* widget)
    {
        switch (page) {
        case Page::AUTOMATION: m_automation_page = widget; break;
        case Page::LIFECYCLE: m_lifecycle_page = widget; break;
        case Page::CONFIGURATION: m_configuration_page = widget; break;
        case Page::SAFETY: m_safety_page = widget; break;
        case Page::LIQUIDITY: m_liquidity_page = widget; break;
        case Page::FINANCE: m_finance_page = widget; break;
        case Page::ACTIVITY: m_activity_page = widget; break;
        case Page::CONNECTION: m_connection_page = widget; break;
        case Page::DIAGNOSTICS: m_diagnostics_page = widget; break;
        case Page::MANAGEMENT: m_management_page = widget; break;
        }
    }

private:
    ProviderOverview m_overview{*this};
    ProviderConfiguration m_configuration{*this};
    ProviderLiquidity m_liquidity{*this};
    ProviderFinance m_finance{*this};
    ProviderRecovery m_recovery{*this};
    ProviderSetup m_setup{*this};
    // Structured failure for the active callback, restored across nested replies.
    PaymasterQt::RpcError m_rpc_failure;
    WalletModel* m_model{nullptr};
    QMetaObject::Connection m_history_wait_connection;
    bool m_history_deferred_status{false};
    uint64_t m_wallet_generation{0};
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
    QStackedWidget* m_tabs{nullptr};
    QWidget* m_navigation_shell{nullptr};
    QListWidget* m_navigation{nullptr};
    QComboBox* m_navigation_select{nullptr};
    QWidget* m_automation_page{nullptr};
    QWidget* m_lifecycle_page{nullptr};
    QWidget* m_configuration_page{nullptr};
    QWidget* m_safety_page{nullptr};
    QWidget* m_liquidity_page{nullptr};
    QWidget* m_finance_page{nullptr};
    QWidget* m_activity_page{nullptr};
    QLabel* m_wallet;
    QLabel* m_last_checked{nullptr};
    QStackedWidget* m_settings_tabs{nullptr};
    QWidget* m_connection_page{nullptr};
    QWidget* m_diagnostics_page{nullptr};
    QWidget* m_management_page{nullptr};
    QLabel* m_status;
    bool m_privacy{false};
};

inline ProviderComponent::ProviderComponent(PaymasterWidgetImpl& panel) : QObject(&panel), m_panel(panel) {}
inline QWidget* ProviderComponent::widget() const { return &m_panel; }

#endif
