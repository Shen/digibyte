// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

PaymasterWidgetImpl::~PaymasterWidgetImpl()
{ cancelFinanceWrite(); }

void PaymasterWidgetImpl::setWalletModel(WalletModel* model)
{
    disconnect(m_history_wait_connection);
    m_history_deferred_status = false;
    cancelFinanceWrite();
    if (m_node_connection_dialog) m_node_connection_dialog->reject();
    if (m_refill_approval) m_refill_approval->reject();
    m_operator_snapshot = UniValue{};
    m_overview_finance_age.invalidate();
    m_overview_finance_observed = false;
    m_policy_readback_pending = false;
    m_pending_policy_hash.clear();
    m_last_checked->setText(tr("Status has not been checked yet."));
    m_operator_summary->setText(tr("Reading the selected wallet's operator status…"));
    resetOperatorPresentation();
    m_setup_wizard_active = false;
    if (m_setup_status_timer) m_setup_status_timer->stop();
    if (m_setup_wizard) {
        // A modal assistant belongs to exactly one wallet. Closing its
        // nested event loop before changing m_model guarantees that no
        // review, unlock lease or progress page survives a wallet switch.
        m_setup_wizard->setCloseBlocked(false);
        m_setup_wizard->reject();
        m_setup_wizard.clear();
    }
    ++m_wallet_generation;
    m_operation.reset(m_wallet_generation);
    m_guided_task.clear();
    m_failed_guided_task.clear();
    if (m_task_card) m_task_card->hide();
    m_task_error_details->clear();
    m_task_error_toggle->setChecked(false);
    m_withdrawal_hint->clear();
    m_withdrawal_hint->hide();
    m_backup_adopt_status->clear();
    m_backup_adopt_status->hide();
    m_withdraw_task->setToolTip({});
    m_preview_carrier_excess->setToolTip({});
    m_pending_handler_calls.clear();
    m_pending_operator_review = {};
    m_rpc_handler_depth = 0;
    m_rpc_failure = {};
    m_active_rpc_handler_token = 0;
    m_next_rpc_handler_token = 0;
    m_model = model;
    if (model) {
        m_history_wait_connection = connect(model, &WalletModel::digiDollarHistoryRefreshStateChanged, this, [this] {
            const bool waiting = m_model && m_model->digiDollarHistoryRefreshInProgress();
            if (waiting) m_history_deferred_status = true;
            setRpcBusyState(m_busy);
            showOperatorBusyState();
            if (!waiting && m_history_deferred_status && !m_busy && !m_rpc_active) {
                m_history_deferred_status = false;
                refreshStatus();
            }
        });
    }
    m_busy = false;
    m_rpc_active = false;
    m_rpc_chain_foreground = false;
    m_active_rpc_background = false;
    m_tabs->setEnabled(true);
    m_navigation->setEnabled(true);
    m_navigation_select->setEnabled(true);
    m_task_card->setEnabled(true);
    m_active_rpc_command.clear();
    updateOperatorProgress();
    m_setup_mode = loadSetupMode();
    applyRecommendedPolicyDefaults(/*mark_dirty=*/false);
    restoreLiquidityDefaults(/*announce=*/false);
    restoreFundingSafetyDefaults(m_user_paid_safety,
                                 /*recommended_defaults=*/true,
                                 /*mark_dirty=*/false);
    restoreFundingSafetyDefaults(m_public_sponsored_safety,
                                 /*recommended_defaults=*/false,
                                 /*mark_dirty=*/false);
    restoreFundingSafetyDefaults(m_restricted_sponsored_safety,
                                 /*recommended_defaults=*/false,
                                 /*mark_dirty=*/false);
    restoreQuoteSafetyDefaults(/*mark_dirty=*/false);
    restoreClientSafetyDefaults(/*mark_dirty=*/false);
    m_loading_runtime_settings = true;
    m_operation_mode = QStringLiteral("automatic");
    m_autostart_enabled = false;
    m_service_state = QStringLiteral("stopped");
    m_last_service_error.clear();
    m_waiting_provider_requests = 0;
    m_waiting_provider_submits = 0;
    m_runtime_settings_dirty = false;
    m_autostart_dirty = false;
    m_autostart_pending_read = false;
    m_autostart_message.clear();
    m_saved_liquidity_policy = UniValue{};
    m_refill_save_pending = false;
    m_refill_message.clear();
    if (m_inline_node_editor) delete m_inline_node_editor.data();
    m_operation_mode_select->setCurrentIndex(
        m_operation_mode_select->findData(m_operation_mode));
    m_autostart->setChecked(false);
    m_loading_runtime_settings = false;
    m_runtime_settings_result->setText(tr(
        "Automatic processing is the recommended default. Provider autostart is off by default."));
    m_provider_safety_configured = false;
    m_provider_safety_snapshot_representable = true;
    m_unrepresentable_provider_safety_snapshot =
        UniValue{UniValue::VOBJ};
    setProviderSafetyMutationEnabled(true);
    m_policy_snapshot_representable = true;
    m_unrepresentable_policy_snapshot = UniValue{UniValue::VOBJ};
    setPolicyMutationEnabled(true);
    m_provider_info_snapshot_available = false;
    m_provider_safety_snapshot_available = false;
    m_liquidity_snapshot_available = false;
    m_provider_settings_present = false;
    m_client_safety_configured = false;
    m_client_safety_snapshot_representable = true;
    m_unrepresentable_client_safety_snapshot =
        UniValue{UniValue::VOBJ};
    if (m_client_safety_group) {
        m_client_safety_group->setEnabled(!m_busy);
    }
    m_core_eligible = false;
    m_core_enabled = false;
    m_core_running = false;
    m_core_ready = false;
    m_core_locked = true;
    m_core_has_identity = false;
    m_provider_id.clear();
    m_provider_endpoint.clear();
    m_display_name->clear();
    updateIdentityLabels();
    m_backup_required = false;
    m_finance_last_result = UniValue{UniValue::VOBJ};
    m_finance_selected_summary->setText(tr("Load the selected reporting period to view income and costs."));
    m_finance_pages.clear();
    m_finance_page_index = -1;
    m_finance_loaded = false;
    m_finance_loading = false;
    m_finance_export_loading = false;
    m_finance_refresh->setText(tr("Refresh"));
    m_finance_export->setText(tr("Export CSV…"));
    updateBackupReminder(false, {});
    m_finance_daily_totals->setRowCount(0);
    m_finance_events->setRowCount(0);
    updateFinanceControls();
    m_finance_history_status->setText(
        tr("Finance history has not been loaded yet."));
    clearFinanceValues();
    m_activity_output->clear();
    if (m_activity_reservations) m_activity_reservations->setRowCount(0);
    if (m_activity_tasks) m_activity_tasks->clear();
    m_liquidity_output->clear();
    m_pool_preparation = UniValue{};
    m_pool_preparation_status->clear();
    m_pool_preparation_status->hide();
    m_cancel_preparation->hide();
    m_preparation_fee->setSatoshis(DEFAULT_PAYMASTER_PREPARATION_FEE);
    m_activity_action_result->setText(
        tr("No manual operation has been run for this wallet."));
    m_activity_summary->setText(
        tr("Waiting for an authoritative provider status."));
    m_overview_liquidity_snapshot = UniValue{};
    clearOverviewCapital();
    m_operator_stop_status->clear();
    m_operator_stop_status->hide();
    m_retirement_summary->hide();
    m_retirement_balances->clear();
    m_retirement_backup_status->clear();
    setStatusLabel(m_overview_finance_status,
                   tr("Available after the provider identity is created"),
                   QStringLiteral("neutral"));
    m_oracle_state = OracleState::UNKNOWN;
    m_oracle_price_micro_usd = 0;
    m_pool_admission_dgb = 0;
    m_pool_admission_carriers = 0;
    m_pool_operational_dgb = 0;
    m_pool_operational_carriers = 0;
    m_pool_status_loaded = false;
    m_readiness_errors.clear();
    m_liquidity_policy_configured = false;
    m_liquidity_snapshot_representable = true;
    m_unrepresentable_liquidity_snapshot = UniValue{UniValue::VOBJ};
    setLiquidityPolicyMutationEnabled(true);
    m_liquidity_targets_satisfy_provider_policy = false;
    m_liquidity_policy_dirty = false;
    m_loading_liquidity_policy = false;
    m_maintenance_state = QStringLiteral("unavailable");
    m_carrier_base_cents = 0;
    m_carrier_excess_cents = 0;
    m_pending_successor_carrier_cents = 0;
    m_pending_successor_dgb_satoshis = 0;
    invalidateCarrierWithdrawalPreviews();
    m_setup_waiting_for_confirmations = false;
    if (m_setup_status_timer) m_setup_status_timer->stop();
    m_enable_status->setText(
        tr("Provider configuration is currently disabled for this wallet."));
    m_provider_safety_status->setText(
        tr("Provider safety policy: not configured (provider not ready)"));
    m_client_safety_status->setText(
        tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"));
    m_provider_safety_usage->setText(tr("Budget usage is not available yet."));
    m_liquidity_current_status->setText(
        tr("Current confirmed pool: status not loaded yet."));
    m_liquidity_policy_status->setText(
        tr("Liquidity policy has not been loaded yet."));
    m_liquidity_slot_status->setText(
        tr("Ready, pending and missing target counts have not been loaded yet."));
    m_liquidity_recycling_status->setText(
        tr("No recyclable successor information is available yet."));
    m_liquidity_budget_status->setText(
        tr("Maintenance budget use has not been loaded yet."));
    m_carrier_value_status->setText(
        tr("Carrier base and withdrawable excess have not been loaded yet."));
    m_liquidity_maintenance_state->setText(
        tr("Automatic liquidity status is not available yet."));
    m_liquidity_maintenance_next_step->setText(
        tr("Refresh after selecting a provider wallet."));
    m_liquidity_maintenance_cost->clear();
    updateExternalPrerequisites();
    m_approve_liquidity_maintenance->setVisible(false);
    m_release_carrier_select->clear();
    m_preview_carrier_excess->setEnabled(false);
    m_preview_carrier_release->setEnabled(false);
    m_wallet->setText(model ? tr("Selected wallet: %1").arg(model->getDisplayName())
                            : tr("No wallet selected"));
    setEnabled(model != nullptr);
    updateSetupAccess();
    updateProviderButtons();
    if (model) refreshStatus();
}

void PaymasterWidgetImpl::setPrivacy(bool privacy)
{
    if (m_privacy == privacy) return;
    ++m_settings_restart_generation;
    m_privacy = privacy;
    if (privacy) {
        m_backup_adopt->hide();
        m_backup_adopt_status->hide();
    }
    if (privacy && m_finance_write_phase) {
        cancelFinanceWrite();
        setFinanceLoading(false, /*exporting=*/true);
    }
    if (privacy && m_node_connection_dialog) m_node_connection_dialog->reject();
    if (privacy && m_refill_approval) m_refill_approval->reject();
    if (privacy && m_inline_node_editor) {
        m_inline_node_editor->deleteLater();
        m_inline_node_editor.clear();
    }
    m_node_editor_host->setVisible(!privacy);
    m_activity_reservations->setVisible(!privacy);
    m_activity_tasks->setVisible(!privacy);
    if (privacy) {
        m_operation.reset(m_wallet_generation);
        m_guided_task.clear();
        m_failed_guided_task.clear();
        m_pending_operator_review = {};
        if (m_task_card) m_task_card->hide();
        m_task_error_details->clear();
        m_task_error_toggle->setChecked(false);
        m_withdrawal_hint->clear();
        m_withdrawal_hint->hide();
        m_withdraw_task->setToolTip({});
        m_preview_carrier_excess->setToolTip({});
    }
    m_operator_summary->setVisible(!privacy);
    m_operator_card->setVisible(!privacy);
    showOperatorBusyState();
    if (privacy) {
        resetOperatorPresentation();
        for (auto* value : m_funds_values) value->setText(QStringLiteral("—"));
        m_finance_selected_summary->setText(tr("Financial summary hidden by privacy mode"));
    }
    if (privacy) {
        m_operator_snapshot = UniValue{};
    }

    if (privacy) {
        // Privacy can be toggled by an application-level setting while a
        // nested modal loop is active. Close every Paymaster message box
        // before redacting the underlying pages so an exact funding
        // preview or backend detail cannot remain visible above them.
        for (QMessageBox* dialog : findChildren<QMessageBox*>()) {
            dialog->reject();
        }
    }

    if (privacy && m_setup_wizard) {
        // The wizard's review pages intentionally expose exact provider
        // identity, policy and funding values. Closing it is the only
        // fail-closed response when privacy mode changes mid-session.
        m_setup_wizard_active = false;
        m_setup_wizard->setCloseBlocked(false);
        m_setup_wizard->reject();
        m_setup_wizard.clear();
    }

    // Detailed records and raw expert-operation output are more useful
    // hidden than partially redacted: dates, counts and identifiers can be
    // sensitive even when currency digits themselves are masked.
    m_finance_daily_totals->setVisible(!privacy);
    m_finance_events->setVisible(!privacy);
    m_overview_capital_details->setVisible(!privacy);
    m_overview_finance_details->setVisible(!privacy);
    m_operator_stop_status->setVisible(!privacy && !m_operator_stop_status->text().isEmpty());
    if (privacy) {
        m_retirement_summary->hide();
        m_retirement_balances->clear();
    }
    m_activity_output->setVisible(!privacy);
    m_liquidity_output->setVisible(!privacy);
    if (privacy) {
        m_pool_preparation = UniValue{};
        m_pool_preparation_status->clear();
        m_pool_preparation_status->hide();
        m_cancel_preparation->hide();
    }
    m_display_name->setEchoMode(
        privacy ? QLineEdit::Password : QLineEdit::Normal);
    setSensitivePagePrivacy(privacy);

    if (privacy) {
        if (m_core_has_identity) {
            m_identity->setText(tr(
                "Identity: hidden by privacy mode\nEndpoint: hidden by privacy mode"));
            m_offer_identity_id->setText(tr("Hidden by privacy mode"));
        }
        m_overview_finance_status->setText(
            maskNumericText(m_overview_finance_status->text()));
        m_pool->setText(maskNumericText(m_pool->text()));
        m_wallet->setText(tr("Selected wallet hidden by privacy mode"));
        m_activity_summary->setText(tr(
            "Provider activity details are hidden by privacy mode."));
        m_activity_action_result->setText(tr(
            "Operation results are hidden by privacy mode."));
        for (QLabel* label : {
                 m_overview_offer_status, m_overview_safety_status,
                 m_overview_liquidity_status,
                 m_overview_operation_status,
                 m_overview_finance_status,
                 m_liquidity_maintenance_state,
                 m_liquidity_maintenance_next_step,
                 m_liquidity_maintenance_cost}) {
            if (label) label->setText(maskNumericText(label->text()));
        }
    } else {
        updateIdentityLabels();
        m_wallet->setText(m_model
            ? tr("Selected wallet: %1").arg(m_model->getDisplayName())
            : tr("No wallet selected"));
    }
    if (privacy) m_last_checked->setText(tr("Hidden by privacy mode"));
    else m_last_checked->setText(tr("Checking current status…"));
    if (m_activity_result_group) {
        m_activity_result_group->setVisible(
            !privacy && m_operation_mode == QLatin1String("manual"));
    }

    if (m_finance_page_index >= 0 &&
        m_finance_page_index < static_cast<int>(m_finance_pages.size())) {
        renderFinancePage(m_finance_page_index);
    }
    updateBackupReminder(m_backup_required, m_provider_id);
    updateFinanceControls();
    updateSetupAccess();
    updateAutomaticRefreshTimer();
    redactVisiblePrivacyText();
    updateAutostartControls();

    // The overview contains pool counts not retained by the finance-page
    // cache. Re-read them when privacy mode is removed rather than trying
    // to reverse a visual redaction.
    if (!privacy && hasRpcTransport() && !m_busy) refreshStatus();
}

void PaymasterWidgetImpl::showEvent(QShowEvent* event)
{
    DigiDollarPaymasterWidget::showEvent(event);
    AlignPaymasterFormLabels(m_offer_forms);
    if (m_model && !m_busy && !m_setup_wizard_active && !m_privacy)
        QTimer::singleShot(0, this, [this] { if (m_model && !m_busy && !m_privacy) refreshStatus(); });
}

void PaymasterWidgetImpl::refreshStatus()
{
    refreshOperatorStatus();
}

void PaymasterWidgetImpl::applyProviderInfo(const UniValue& result)
{
        const UniValue& policy = result.find_value("policy");
        const UniValue& pool = result.find_value("pool");
        if (!IsCompleteProviderInfoSnapshot(result)) {
            m_provider_info_snapshot_available = false;
            m_status->setText(tr(
                "Provider status unavailable: Core returned an incomplete Paymaster provider record."));
            updateProviderButtons();
            return;
        }
        m_provider_info_snapshot_available = true;
        markProviderStatusCurrent();
        const UniValue& settings_present =
            result.find_value("settings_present");
        m_provider_settings_present = settings_present.isBool() &&
                                      settings_present.get_bool();
        const bool eligible = result.find_value("wallet_eligible").get_bool();
        const bool enabled = result.find_value("enabled").get_bool();
        const bool running = result.find_value("running").get_bool();
        const bool ready = result.find_value("ready").get_bool();
        const bool locked = result.find_value("wallet_locked").get_bool();
        m_core_eligible = eligible;
        m_core_enabled = enabled;
        m_core_running = running;
        m_core_ready = ready;
        m_core_locked = locked;
        const UniValue& operation_mode = result.find_value("operation_mode");
        const UniValue& autostart = result.find_value("autostart");
        const UniValue& service_state = result.find_value("service_state");
        const UniValue& last_service_error = result.find_value("last_service_error");
        const UniValue& service_queue = result.find_value("service_queue");
        m_operation_mode = operation_mode.isStr()
            ? QString::fromStdString(operation_mode.get_str())
            : QStringLiteral("automatic");
        m_autostart_enabled = autostart.isBool() && autostart.get_bool();
        m_service_state = service_state.isStr()
            ? QString::fromStdString(service_state.get_str())
            : QStringLiteral("stopped");
        m_last_service_error = last_service_error.isStr()
            ? QString::fromStdString(last_service_error.get_str())
            : QString{};
        const UniValue& waiting_requests =
            service_queue.find_value("waiting_requests");
        const UniValue& waiting_submits =
            service_queue.find_value("waiting_submits");
        m_waiting_provider_requests = waiting_requests.isNum()
            ? waiting_requests.getInt<qint64>()
            : 0;
        m_waiting_provider_submits = waiting_submits.isNum()
            ? waiting_submits.getInt<qint64>()
            : 0;
        if (!m_runtime_settings_dirty) {
            m_loading_runtime_settings = true;
            const int mode_index = m_operation_mode_select->findData(m_operation_mode);
            if (mode_index >= 0) m_operation_mode_select->setCurrentIndex(mode_index);
            m_loading_runtime_settings = false;
        }
        reconcileAutostart();
        m_enable_status->setText(enabled
            ? m_autostart_enabled
                ? tr("Provider configuration is enabled and saved in this wallet. Autostart may bring it online whenever all readiness requirements pass.")
                : tr("Provider configuration is enabled and saved in this wallet. Autostart is disabled. This does not stop an already started provider.")
            : tr("Provider configuration is currently disabled for this wallet."));
        if (m_service_state == QLatin1String("active")) {
            m_status->setText(tr("Automatic provider operation active — eligible requests and validated payments are processed by Core within the saved limits."));
        } else if (m_service_state == QLatin1String("manual")) {
            m_status->setText(tr("Provider running in manual expert mode — waiting messages require manual processing from Activity and recovery."));
        } else if (m_service_state == QLatin1String("waiting_for_unlock")) {
            m_status->setText(tr("Provider paused — unlock this wallet to resume automatic processing. No queued message is consumed while locked."));
        } else if (m_service_state == QLatin1String("waiting_for_readiness")) {
            m_status->setText(tr("Provider processing is waiting for the remaining readiness requirements."));
        } else if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
            m_status->setText(tr("Provider started safely but is waiting for your one-time approval of finite paid liquidity-maintenance limits."));
        } else if (m_service_state == QLatin1String("replenishing_liquidity")) {
            m_status->setText(tr("Provider is automatically restoring missing liquidity within the approved maintenance limits."));
        } else if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
            m_status->setText(tr("Provider is waiting for replacement liquidity to confirm before accepting new requests."));
        } else if (m_service_state == QLatin1String("drain_only")) {
            m_status->setText(tr("Provider in recovery-only operation — no new quotes are accepted while existing durable work is completed safely."));
        } else if (m_service_state == QLatin1String("error")) {
            m_status->setText(tr("Automatic provider operation requires attention. Open Activity and recovery for the stable error code."));
        } else if (running) {
            m_status->setText(tr("Provider running — this wallet is currently available to eligible DigiDollar clients."));
        } else if (ready) {
            m_status->setText(tr("Setup complete — start from Overview using the saved settings."));
        } else {
            m_status->setText(tr("Setup incomplete — no provider service is currently running."));
        }
        const UniValue& provider = result.find_value("provider_id");
        m_core_has_identity = provider.isStr() && !provider.get_str().empty();
        const QString provider_id = m_core_has_identity
            ? QString::fromStdString(provider.get_str()) : QString{};
        if (m_provider_id != provider_id) {
            m_overview_finance_observed = false;
            m_overview_finance_age.invalidate();
            clearOverviewCapital();
        }
        m_provider_id = provider_id;
        const UniValue& backup_status = result.find_value("backup_status");
        const bool backup_required = backup_status.isObject() &&
            backup_status.find_value("required").isBool() &&
            backup_status.find_value("required").get_bool();
        updateBackupReminder(backup_required, m_provider_id);
        const UniValue& finance_summary = result.find_value("finance_summary");
        m_overview_finance_action->setText(tr("Open finances"));
        if (finance_summary.isObject()) {
            const qint64 income = financeNumber(
                finance_summary, "service_fee_income_cents");
            const qint64 cost = financeNumber(
                finance_summary, "dgb_operating_cost_satoshis");
            const qint64 transfers = financeNumber(
                finance_summary, "successful_transfers");
            setStatusLabel(
                m_overview_finance_status,
                maskNumericText(
                    tr("%1 DD service fees · %2 DGB costs · %3 successful transfer(s)")
                        .arg(ddAmount(income), compactDgbAmount(cost))
                        .arg(transfers)),
                QStringLiteral("ready"));
        } else if (m_core_has_identity && !m_overview_finance_observed) {
            setStatusLabel(
                m_overview_finance_status,
                tr("Loading income and costs…"),
                QStringLiteral("neutral"));
        } else if (!m_core_has_identity) {
            setStatusLabel(
                m_overview_finance_status,
                tr("Available after the provider identity is created"),
                QStringLiteral("neutral"));
        }
        const UniValue& endpoint = result.find_value("endpoint");
        m_provider_endpoint = endpoint.isStr()
            ? QString::fromStdString(endpoint.get_str()) : QString{};
        const UniValue& display_name = result.find_value("display_name");
        if (display_name.isStr() && !m_policy_dirty) {
            m_display_name->setText(QString::fromStdString(display_name.get_str()));
        }
        if (policy.isObject() && !m_policy_dirty) {
            if (m_policy_readback_pending) {
                const UniValue& policy_hash =
                    policy.find_value("policy_hash");
                if (policy_hash.isStr() &&
                    QString::fromStdString(policy_hash.get_str()) ==
                        m_pending_policy_hash) {
                    m_policy_readback_pending = false;
                    m_pending_policy_hash.clear();
                }
            }
            if (!m_policy_readback_pending) loadPolicy(policy);
        }
        updateIdentityLabels();
        bool has_pool_entries{false};
        if (pool.isObject()) {
            m_pool_status_loaded = true;
            m_pool_admission_dgb = pool.find_value("admission_dgb").getInt<int>();
            m_pool_admission_carriers = pool.find_value("admission_carriers").getInt<int>();
            m_pool_operational_dgb = pool.find_value("operational_dgb").getInt<int>();
            m_pool_operational_carriers = pool.find_value("operational_carriers").getInt<int>();
            has_pool_entries =
                m_pool_admission_dgb > 0 ||
                m_pool_admission_carriers > 0 ||
                m_pool_operational_dgb > 0 ||
                m_pool_operational_carriers > 0 ||
                pool.find_value("reserved").getInt<int>() > 0;
            m_pool->setText(maskNumericText(
                tr("Pool: admission DGB %1, carriers %2; operational DGB %3, carriers %4; reserved %5")
                    .arg(m_pool_admission_dgb)
                    .arg(m_pool_admission_carriers)
                    .arg(m_pool_operational_dgb)
                    .arg(m_pool_operational_carriers)
                    .arg(pool.find_value("reserved").getInt<int>())));
            m_liquidity_current_status->setText(tr(
                "Current confirmed and available pool — admission DGB: %1; admission carriers: %2; operational DGB: %3; operational carriers: %4; usable complete operational slots: %5; currently reserved: %6.")
                .arg(pool.find_value("admission_dgb").getInt<int>())
                .arg(pool.find_value("admission_carriers").getInt<int>())
                .arg(pool.find_value("operational_dgb").getInt<int>())
                .arg(pool.find_value("operational_carriers").getInt<int>())
                .arg(pool.find_value("complete_operational_slots").getInt<int>())
                .arg(pool.find_value("reserved").getInt<int>()));
        }
        const bool has_existing_configuration =
            m_provider_settings_present ||
            (provider.isStr() && !provider.get_str().empty()) || policy.isObject() ||
            enabled || running || ready || has_pool_entries;
        if (m_setup_mode == PaymasterSetupMode::UNDECIDED &&
            has_existing_configuration) {
            setSetupMode(PaymasterSetupMode::EXISTING);
        }
        QStringList errors;
        for (const UniValue& error : result.find_value("readiness_errors").getValues()) {
            errors.push_back(QString::fromStdString(error.get_str()));
        }
        m_readiness_errors = errors;
        m_readiness->setPlainText(errors.isEmpty() ? tr("All readiness gates pass.") : errors.join('\n'));
        updateReadinessSummary(errors, running, ready);
        updateProviderHeaderStatus();
        if (m_setup_waiting_for_confirmations) {
            if (ready || running) {
                m_setup_waiting_for_confirmations = false;
                m_setup_status_timer->stop();
            } else if (errors.contains(QStringLiteral("PAYMASTER_POOLS_NOT_PREPARED"))) {
                m_next_step->setText(tr("Waiting for pool funding or confirmations"));
                m_readiness_summary->setText(tr(
                    "All settings have been saved and no further Save buttons are required. "
                    "The approved pool plan must finish and its outputs must confirm before the provider can start. "
                    "This status refreshes automatically."));
            }
        }
        m_reopen_wizard->setText(has_existing_configuration
            ? tr("Review setup with assistant…")
            : tr("Continue guided setup…"));
        if (m_offer_identity_status) {
            setStatusLabel(
                m_offer_identity_status,
                m_core_has_identity
                    ? tr("Ready · persistent identity is stored in this wallet")
                    : tr("Action required · no provider identity has been created yet."),
                m_core_has_identity ? QStringLiteral("ready")
                                    : QStringLiteral("action"));
            m_offer_identity_id->setText(
                m_core_has_identity && provider.isStr()
                    ? visibleProviderId(m_provider_id)
                    : tr("Not created yet"));
            m_create_identity->setVisible(!m_core_has_identity);
            m_create_identity->setEnabled(
                !m_core_has_identity &&
                m_display_name->hasAcceptableInput() && !m_busy);
        }
        updateProviderButtons();
        updateAutomaticRefreshTimer();
}

void PaymasterWidgetImpl::setOperatorStatusForTesting(const UniValue& status)
{
    m_operator_snapshot = status;
    m_operation.observe(status, m_wallet_generation, QDateTime::currentSecsSinceEpoch());
    renderCurrentTask();
    renderOperatorPresentation(status);
}

void PaymasterWidgetImpl::setReadinessStatusForTesting(const UniValue& status)
{
    m_status_age.restart();
    m_provider_info_snapshot_available = true;
    const UniValue& settings_present =
        status.find_value("settings_present");
    m_provider_settings_present = settings_present.isBool()
        ? settings_present.get_bool()
        : status.find_value("has_policy").isBool() &&
            status.find_value("has_policy").get_bool();
    m_core_eligible = status.find_value("wallet_eligible").isBool()
        ? status.find_value("wallet_eligible").get_bool() : true;
    m_core_enabled = status.find_value("enabled").isBool()
        ? status.find_value("enabled").get_bool() : true;
    m_core_running = status.find_value("running").isBool() &&
                     status.find_value("running").get_bool();
    m_core_ready = status.find_value("ready").isBool() &&
                   status.find_value("ready").get_bool();
    m_core_locked = status.find_value("wallet_locked").isBool() &&
                    status.find_value("wallet_locked").get_bool();
    const UniValue& operation_mode = status.find_value("operation_mode");
    const UniValue& autostart = status.find_value("autostart");
    const UniValue& service_state = status.find_value("service_state");
    const UniValue& last_service_error =
        status.find_value("last_service_error");
    const UniValue& service_queue = status.find_value("service_queue");
    m_operation_mode = operation_mode.isStr()
        ? QString::fromStdString(operation_mode.get_str())
        : QStringLiteral("automatic");
    m_autostart_enabled = autostart.isBool() && autostart.get_bool();
    m_service_state = service_state.isStr()
        ? QString::fromStdString(service_state.get_str())
        : QStringLiteral("stopped");
    m_last_service_error = last_service_error.isStr()
        ? QString::fromStdString(last_service_error.get_str())
        : QString{};
    const UniValue& waiting_requests =
        service_queue.find_value("waiting_requests");
    const UniValue& waiting_submits =
        service_queue.find_value("waiting_submits");
    m_waiting_provider_requests = waiting_requests.isNum()
        ? waiting_requests.getInt<qint64>()
        : 0;
    m_waiting_provider_submits = waiting_submits.isNum()
        ? waiting_submits.getInt<qint64>()
        : 0;
    if (!m_runtime_settings_dirty) {
        m_loading_runtime_settings = true;
        const int mode_index =
            m_operation_mode_select->findData(m_operation_mode);
        if (mode_index >= 0) {
            m_operation_mode_select->setCurrentIndex(mode_index);
        }
        m_loading_runtime_settings = false;
    }
    m_core_has_identity = status.find_value("has_identity").isBool()
        ? status.find_value("has_identity").get_bool() : true;
    const UniValue& provider_id = status.find_value("provider_id");
    if (m_core_has_identity && provider_id.isStr()) {
        m_provider_id = QString::fromStdString(provider_id.get_str());
    }
    if (m_offer_identity_id) {
        m_offer_identity_id->setText(
            m_core_has_identity && provider_id.isStr()
                ? visibleProviderId(m_provider_id)
                : tr("Not created yet"));
    }
    m_policy_loaded = status.find_value("has_policy").isBool()
        ? status.find_value("has_policy").get_bool() : true;
    m_provider_safety_configured =
        status.find_value("has_safety_policy").isBool()
            ? status.find_value("has_safety_policy").get_bool() : true;
    // A readiness-only fixture has no pool object. Seed a neutral pool
    // snapshot only when no more specific liquidity fixture was applied
    // before it; otherwise preserve the authoritative missing/pending
    // counts just like the production asynchronous refresh does.
    if (!m_pool_status_loaded) {
        m_pool_status_loaded = true;
        m_pool_admission_dgb = m_admission_dgb->value();
        m_pool_operational_dgb = m_operational_dgb->value();
        m_pool_admission_carriers = m_admission_carriers->value();
        m_pool_operational_carriers = m_operational_carriers->value();
    }
    m_readiness_errors.clear();
    const UniValue& errors = status.find_value("readiness_errors");
    if (errors.isArray()) {
        for (const UniValue& error : errors.getValues()) {
            if (error.isStr()) {
                m_readiness_errors.push_back(
                    QString::fromStdString(error.get_str()));
            }
        }
    }
    qint64 oracle_price{0};
    m_oracle_price_micro_usd =
        GetInt64Field(status, "oracle_price_micro_usd", oracle_price)
        ? oracle_price : 0;
    m_oracle_state = m_oracle_price_micro_usd > 0
        ? OracleState::AVAILABLE : OracleState::UNAVAILABLE;
    updateReadinessSummary(m_readiness_errors, m_core_running, m_core_ready);
    updateProviderHeaderStatus();
    reconcileAutostart();
    updateProviderButtons();
}

void PaymasterWidgetImpl::setStartResultForTesting(const UniValue& result)
{
    presentProviderStartResult(result);
}

void PaymasterWidgetImpl::expireOperatorStatusForTesting()
{ m_status_age.invalidate(); }

void PaymasterWidgetImpl::setMutationSnapshotsAvailableForTesting(
    bool provider_info, bool provider_safety, bool liquidity)
{
    m_status_age.restart();
    // Workflow tests use intentionally narrow fixtures. Keep production's
    // fail-closed decoder path intact while allowing those tests to state
    // explicitly which authoritative snapshots are assumed available.
    m_provider_info_snapshot_available = provider_info;
    m_provider_safety_snapshot_available = provider_safety;
    m_liquidity_snapshot_available = liquidity;
    m_policy_snapshot_representable = provider_info;
    m_provider_safety_snapshot_representable = provider_safety;
    m_liquidity_snapshot_representable = liquidity;
    setPolicyMutationEnabled(provider_info);
    setProviderSafetyMutationEnabled(provider_safety);
    setLiquidityPolicyMutationEnabled(liquidity);
    updateProviderButtons();
    updateLiquidityDisplay();
    updateCarrierWithdrawalButtons();
    updateFinanceControls();
}

void PaymasterWidgetImpl::setRpcExecutorForTesting(
    RpcExecutor executor)
{
    m_rpc_executor_for_testing = std::move(executor);
}

void PaymasterWidgetImpl::setAsyncRpcExecutorForTesting(AsyncRpcExecutor executor)
{
    m_async_rpc_executor_for_testing = std::move(executor);
}

QString PaymasterWidgetImpl::maskNumericText(QString value) const
{
    if (!m_privacy) return value;
    for (int index = 0; index < value.size(); ++index) {
        if (value.at(index).isDigit()) value[index] = QLatin1Char('#');
    }
    return value;
}

bool PaymasterWidgetImpl::spinCanRepresent(const QSpinBox* control, qint64 value)
{
    return control && value >= control->minimum() &&
        value <= control->maximum();
}

bool PaymasterWidgetImpl::dgbAmountCanRepresent(qint64 value)
{
    return value >= 0 && MoneyRange(value);
}

void PaymasterWidgetImpl::showPlainTextWarning(QWidget* parent, const QString& title,
                                 const QString& message)
{
    QMessageBox box(QMessageBox::Warning, title, message,
                    QMessageBox::Ok, parent);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

void PaymasterWidgetImpl::showPlainTextInformation(QWidget* parent,
                                     const QString& title,
                                     const QString& message)
{
    QMessageBox box(QMessageBox::Information, title, message,
                    QMessageBox::Ok, parent);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

QMessageBox::StandardButton PaymasterWidgetImpl::askPlainTextQuestion(
    QWidget* parent, const QString& title, const QString& message,
    QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton default_button)
{
    QMessageBox box(QMessageBox::Question, title, message, buttons,
                    parent);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(default_button);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

bool PaymasterWidgetImpl::requirePrivacyOffForSensitiveAction(const QString& action)
{
    if (!m_privacy) return true;
    const QString message = tr(
        "%1 is unavailable while privacy mode is active because its required review contains provider identifiers, amounts or technical transaction details. Disable privacy mode and review the exact values before continuing.")
                                .arg(action);
    m_status->setText(message);
    if (!m_rpc_executor_for_testing) {
        showPlainTextInformation(this, tr("Privacy mode active"),
                                 message);
    }
    return false;
}

QString PaymasterWidgetImpl::privacySafeBackendError(const QString& error) const
{
    return m_privacy
        ? tr("The Paymaster operation failed. Disable privacy mode to inspect the technical error and retry only after reviewing it.")
        : error;
}

void PaymasterWidgetImpl::setSensitivePagePrivacy(bool privacy)
{
    const QList<QWidget*> pages{
        m_configuration_page, m_safety_page, m_liquidity_page,
        m_finance_page, m_activity_page, m_connection_page, m_management_page, m_automation_page, m_lifecycle_page, m_diagnostics_page};
    for (QWidget* page : pages) {
        if (!page) continue;
        QList<QWidget*> controls{page};
        controls.append(page->findChildren<QWidget*>());
        for (QWidget* control : controls) {
            if (privacy) {
                control->setProperty(
                    "paymasterPrivacySavedToolTip",
                    control->toolTip());
                control->setProperty(
                    "paymasterPrivacySavedAccessibleName",
                    control->accessibleName());
                control->setProperty(
                    "paymasterPrivacySavedAccessibleDescription",
                    control->accessibleDescription());
                control->setToolTip({});
                control->setAccessibleName({});
                control->setAccessibleDescription({});
            } else {
                const QVariant tooltip = control->property(
                    "paymasterPrivacySavedToolTip");
                const QVariant name = control->property(
                    "paymasterPrivacySavedAccessibleName");
                const QVariant description = control->property(
                    "paymasterPrivacySavedAccessibleDescription");
                if (tooltip.isValid()) {
                    control->setToolTip(tooltip.toString());
                    control->setProperty(
                        "paymasterPrivacySavedToolTip", QVariant{});
                }
                if (name.isValid()) {
                    control->setAccessibleName(name.toString());
                    control->setProperty(
                        "paymasterPrivacySavedAccessibleName",
                        QVariant{});
                }
                if (description.isValid()) {
                    control->setAccessibleDescription(
                        description.toString());
                    control->setProperty(
                        "paymasterPrivacySavedAccessibleDescription",
                        QVariant{});
                }
            }
        }
    }
}

void PaymasterWidgetImpl::redactVisiblePrivacyText()
{
    if (!m_privacy) return;
    m_identity->setText(tr(
        "Identity: hidden by privacy mode\nEndpoint: hidden by privacy mode"));
    m_offer_identity_id->setText(tr("Hidden by privacy mode"));
    m_wallet->setText(tr("Selected wallet hidden by privacy mode"));
    m_activity_summary->setText(tr(
        "Provider activity details are hidden by privacy mode."));
    m_activity_action_result->setText(tr(
        "Operation results are hidden by privacy mode."));
    for (QLabel* label : {
             m_pool, m_overview_offer_status,
             m_overview_safety_status, m_overview_liquidity_status,
             m_overview_operation_status, m_overview_finance_status,
             m_liquidity_maintenance_state,
             m_liquidity_maintenance_next_step,
             m_liquidity_maintenance_cost}) {
        if (label) label->setText(maskNumericText(label->text()));
    }
}

QString PaymasterWidgetImpl::visibleProviderId(const QString& provider_id) const
{
    if (provider_id.isEmpty()) return tr("Not created yet");
    return m_privacy ? tr("Hidden by privacy mode") : provider_id;
}

void PaymasterWidgetImpl::updateIdentityLabels()
{
    m_offer_identity_id->setText(
        m_core_has_identity ? visibleProviderId(m_provider_id)
                            : tr("Not created yet"));
    m_identity->setText(tr("Identity: %1\nEndpoint: %2")
        .arg(m_core_has_identity ? visibleProviderId(m_provider_id)
                                 : tr("not configured"),
             m_privacy && !m_provider_endpoint.isEmpty()
                 ? tr("hidden by privacy mode")
                 : !m_provider_endpoint.isEmpty()
                     ? m_provider_endpoint
                     : tr("set -paymasterendpoint=<ip:port> and restart")));
}

void PaymasterWidgetImpl::presentProviderStartResult(const UniValue& result)
{
    if (!IsCompleteProviderStartResult(result)) {
        const QString message = tr(
            "Core did not confirm a complete provider-start result. The runtime state will be refreshed before another action is offered.");
        if (m_rpc_executor_for_testing) {
            m_status->setText(message);
        } else {
            QMessageBox::warning(
                this, tr("Paymaster provider start not confirmed"),
                message);
        }
        return;
    }
    const UniValue& result_mode = result.find_value("operation_mode");
    const QString mode = QString::fromStdString(result_mode.get_str());
    const bool running = result.find_value("running").get_bool();
    const bool ready = result.find_value("ready").get_bool();
    const UniValue& result_service_state =
        result.find_value("service_state");
    const QString service_state =
        QString::fromStdString(result_service_state.get_str());

    if (!running) {
        if (result.find_value("start_requested").isTrue() &&
            (service_state == QLatin1String("waiting_for_readiness") || service_state == QLatin1String("waiting_for_unlock"))) {
            QMessageBox::information(this, tr("Paymaster start requested"),
                tr("Core accepted the start request for this wallet session and is waiting for the prerequisites. Review the missing requirements on Overview. Refill approval, budgets and autostart remain unchanged. Pause or Stop cancels the request."));
            return;
        }
        QMessageBox::warning(
            this, tr("Paymaster provider not started"),
            !m_liquidity_targets_satisfy_provider_policy
                ? tr("The saved liquidity targets cannot satisfy the active offer. A user-paid provider needs at least three admission DigiDollar carriers and one operational DigiDollar carrier. Review and save the liquidity targets before starting the provider.")
                : tr("The provider is not running because one or more readiness requirements are still unmet. Refresh Overview for the next safe action."));
        return;
    }

    if (ready && (service_state == QLatin1String("active") ||
                  service_state == QLatin1String("manual"))) {
        QMessageBox::information(
            this, tr("Paymaster provider started"),
            mode == QLatin1String("manual")
                ? tr("The provider is now online in manual expert mode. Queue items must be processed from Activity.")
                : tr("Automatic provider operation is active. Core now processes eligible requests and validated submissions within the saved safety budgets. No provider-side confirmation is required for each transfer."));
        return;
    }

    QString pending;
    if (service_state ==
        QLatin1String("waiting_for_maintenance_approval")) {
        pending = tr("waiting for approval of the finite refill limits");
    } else if (service_state ==
               QLatin1String("waiting_for_liquidity_confirmation")) {
        pending = tr("replacement liquidity is waiting for confirmation");
    } else if (service_state ==
               QLatin1String("replenishing_liquidity")) {
        pending = tr("restoring the saved liquidity targets");
    } else if (service_state == QLatin1String("drain_only")) {
        pending = tr("finishing previously authorized work in recovery-only mode");
    } else {
        pending = tr("checking the remaining readiness requirement");
    }

    // A pending start is normal background progress, not a result the
    // operator must acknowledge. A modal message box runs a nested event
    // loop, so the periodic refresh can make the Operation ready while the
    // old "startup pending" text remains in front of it. Keep progress in
    // the live status area instead; the caller immediately refreshes the
    // authoritative Core state after this short transitional message.
    m_status->setText(
        tr("Provider start accepted — %1.").arg(pending));
}

bool PaymasterWidgetImpl::hasRpcTransport() const
{
    return m_model || static_cast<bool>(m_rpc_executor_for_testing) || static_cast<bool>(m_async_rpc_executor_for_testing);
}

bool PaymasterWidgetImpl::hasCompleteMutationSnapshots() const
{
    return m_status_age.isValid() && m_status_age.elapsed() <= 30000 &&
        m_provider_info_snapshot_available && m_provider_safety_snapshot_available &&
        m_liquidity_snapshot_available;
}

bool PaymasterWidgetImpl::isPassiveExternalReadinessError(const QString& error)
{
    return error == QLatin1String("PAYMASTER_NODE_NOT_READY") ||
           error == QLatin1String("PAYMASTER_REQUIRES_READY_TXINDEX") ||
           error == QLatin1String("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE");
}

bool PaymasterWidgetImpl::hasPassiveExternalWait() const
{
    return !m_readiness_errors.isEmpty() &&
           std::any_of(m_readiness_errors.cbegin(),
                       m_readiness_errors.cend(),
                       isPassiveExternalReadinessError);
}

void PaymasterWidgetImpl::updateAutomaticRefreshTimer()
{
    if (!m_setup_status_timer) return;
    const bool pending = m_operation.phase == PaymasterOperationController::Phase::Waiting ||
        (m_operation.phase == PaymasterOperationController::Phase::Blocked && m_operation.observed_work) ||
        m_setup_waiting_for_confirmations || hasPassiveExternalWait() ||
        m_maintenance_state == QLatin1String("replenishing_liquidity") ||
        m_maintenance_state == QLatin1String("waiting_for_liquidity_confirmation");
    m_setup_status_timer->setInterval(pending || m_core_running ? 2000 : 30000);
    if (hasRpcTransport() && !m_setup_wizard_active && !m_privacy &&
        (m_tabs->currentWidget() != m_settings_tabs || isOperatorPage(m_liquidity_page)) &&
        !m_operation.busy())
        m_setup_status_timer->start();
    else
        m_setup_status_timer->stop();
}

void PaymasterWidgetImpl::updateProviderHeaderStatus()
{
    const bool liquidity_attention = std::any_of(
        m_readiness_errors.cbegin(), m_readiness_errors.cend(),
        [](const QString& error) { return isLiquidityReadinessError(error); });
    if (m_service_state == QLatin1String("active")) {
        m_status->setText(tr("Automatic provider operation active — eligible requests and validated payments are processed by Core within the saved limits."));
    } else if (m_service_state == QLatin1String("manual")) {
        m_status->setText(tr("Provider running in manual expert mode — waiting messages require manual processing from Activity."));
    } else if (m_service_state == QLatin1String("waiting_for_unlock")) {
        m_status->setText(tr("Provider paused — unlock this wallet to resume automatic processing. No queued message is consumed while locked."));
    } else if (m_readiness_errors.contains(QStringLiteral(
                   "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
               (!m_liquidity_targets_satisfy_provider_policy &&
                (m_last_service_error == QLatin1String(
                     "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
                 m_last_service_error == QLatin1String(
                     "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
        m_status->setText(tr("Provider configuration needs one liquidity change — the active user-paid offer has no saved operational DigiDollar carrier target."));
    } else if (!m_core_ready && providerConfigurationComplete() &&
               hasPassiveExternalWait()) {
        m_status->setText(tr("Provider fully configured — waiting for blockchain and node readiness. You do not need to change provider settings."));
    } else if (maintenanceWaitingForFunds()) {
        m_status->setText(readinessExplanation(m_last_service_error));
    } else if (providerConfigurationComplete() && liquidity_attention) {
        m_status->setText(tr("Provider settings are saved — restore the missing wallet liquidity before the service can accept transfers."));
    } else if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
        m_status->setText(tr("Provider started safely but is waiting for your one-time approval of finite paid liquidity-maintenance limits."));
    } else if (m_service_state == QLatin1String("replenishing_liquidity") &&
               maintenanceFeeLimitExceeded()) {
        m_status->setText(tr(
            "Provider running, but automatic liquidity refill is paused because the approved per-transaction maintenance-fee limit is too low. No refill transaction was created."));
    } else if (m_service_state == QLatin1String("replenishing_liquidity")) {
        m_status->setText(tr("Provider is automatically restoring missing liquidity within the approved maintenance limits."));
    } else if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
        m_status->setText(tr("Provider is waiting for replacement liquidity to confirm before accepting new requests."));
    } else if (m_service_state == QLatin1String("drain_only")) {
        m_status->setText(tr("Provider in recovery-only operation — no new quotes are accepted while existing durable work is completed safely."));
    } else if (m_service_state == QLatin1String("error")) {
        m_status->setText(tr("Automatic provider operation requires attention. Open Operations for details."));
    } else if (m_core_running) {
        m_status->setText(tr("Provider running — this wallet is currently available to eligible DigiDollar clients."));
    } else if (m_core_ready) {
        m_status->setText(tr("Settings saved. Start Paymaster from Overview when you are ready."));
    } else {
        m_status->setText(tr("Setup incomplete — no provider service is currently running."));
    }
}

void PaymasterWidgetImpl::updateExternalPrerequisites()
{
    const bool node_wait = m_readiness_errors.contains(
        QStringLiteral("PAYMASTER_NODE_NOT_READY"));
    const bool txindex_missing = m_readiness_errors.contains(
        QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"));
    const bool txindex_wait = m_readiness_errors.contains(
        QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"));
    const bool activation_wait = m_readiness_errors.contains(
        QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"));

    // Readiness errors not represented by the explicit rows must remain
    // visible. Otherwise every listed prerequisite could appear green
    // while Core is still correctly refusing to start the provider.
    const QStringList represented_errors{
        QStringLiteral("PAYMASTER_NODE_NOT_READY"),
        QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"),
        QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"),
        QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"),
    };
    const bool has_other_requirement = std::any_of(
        m_readiness_errors.cbegin(), m_readiness_errors.cend(),
        [&represented_errors](const QString& error) {
            // Provider setup problems are presented by the task cards and
            // the next-action banner. Do not mislabel them as an external
            // node prerequisite merely because they have no row here.
            return !represented_errors.contains(error) &&
                   !isProviderSetupReadinessError(error) &&
                   !isLiquidityReadinessError(error);
        });
    const bool has_unclassified_failure = std::any_of(
        m_readiness_errors.cbegin(), m_readiness_errors.cend(),
        [&represented_errors](const QString& error) {
            return !represented_errors.contains(error) &&
                   !isProviderSetupReadinessError(error) &&
                   !isLiquidityReadinessError(error);
        });

    setStatusLabel(m_external_blockchain_status,
                   node_wait
                       ? Params().GetChainType() == ChainType::REGTEST
                             ? tr("Waiting — mine a fresh Regtest block so the node leaves initial synchronization")
                             : tr("Waiting — blockchain synchronization is not complete")
                             : tr("Ready"),
                   node_wait ? QStringLiteral("waiting")
                             : QStringLiteral("ready"));
    setStatusLabel(m_external_txindex_status,
                   txindex_missing
                       ? tr("Action required — transaction index is disabled")
                       : txindex_wait
                             ? tr("Waiting — transaction index is synchronizing")
                             : tr("Ready"),
                   txindex_missing ? QStringLiteral("action")
                                   : txindex_wait ? QStringLiteral("waiting")
                                                  : QStringLiteral("ready"));
    setStatusLabel(m_external_broadcast_status,
                   node_wait
                       ? Params().GetChainType() == ChainType::REGTEST
                             ? tr("Waiting — Regtest has no sufficiently recent confirmed tip")
                             : tr("Waiting — node is not ready to broadcast transactions")
                             : tr("Ready"),
                   node_wait ? QStringLiteral("waiting")
                             : QStringLiteral("ready"));
    setStatusLabel(m_external_activation_status,
                   activation_wait ? tr("Waiting — DigiDollar is not active yet")
                                   : tr("Ready"),
                   activation_wait ? QStringLiteral("waiting")
                                   : QStringLiteral("ready"));

    QString oracle_text;
    QString oracle_kind{QStringLiteral("waiting")};
    if (m_oracle_state == OracleState::AVAILABLE) {
        oracle_text = tr("Available — %1 $USD")
                          .arg(QString::number(
                              m_oracle_price_micro_usd / 1000000.0,
                              'f', 6));
        oracle_kind = QStringLiteral("ready");
    } else if (m_oracle_state == OracleState::CHECKING) {
        oracle_text = tr("Checking…");
    } else if (m_oracle_state == OracleState::UNAVAILABLE) {
        oracle_text = tr("Not available yet");
    } else {
        oracle_text = tr("Not checked yet");
    }
    setStatusLabel(m_external_oracle_status, oracle_text, oracle_kind);
    if (m_external_oracle_note) {
        m_external_oracle_note->setVisible(
            m_oracle_state != OracleState::AVAILABLE);
    }

    if (m_external_other_heading && m_external_other_status) {
        m_external_other_heading->setVisible(has_other_requirement);
        m_external_other_status->setVisible(has_other_requirement);
        if (has_other_requirement) {
            setStatusLabel(
                m_external_other_status,
                has_unclassified_failure
                    ? tr("Provider cannot start — review the next safe action and technical details")
                    : tr("Action required — complete the provider setup item shown above"),
                has_unclassified_failure ? QStringLiteral("error")
                                         : QStringLiteral("action"));
        }
    }

    QString card_kind{QStringLiteral("ready")};
    if (has_unclassified_failure) {
        card_kind = QStringLiteral("error");
    } else if (has_other_requirement || txindex_missing) {
        card_kind = QStringLiteral("action");
    } else if (node_wait || txindex_wait || activation_wait) {
        card_kind = QStringLiteral("waiting");
    }
    setStatusKind(m_external_prerequisites_card, card_kind);
}

void PaymasterWidgetImpl::refreshOracleStatus()
{
    if (!m_model) return;
    // Keep an already displayed Oracle result stable while refreshing it.
    // Switching AVAILABLE -> CHECKING -> AVAILABLE every ten seconds made
    // the status card resize and visibly flash even when nothing changed.
    if (m_oracle_state == OracleState::UNKNOWN) {
        m_oracle_state = OracleState::CHECKING;
        updateExternalPrerequisites();
    }
    QPointer<PaymasterWidgetImpl> guard{this};
    WalletModel* request_model = m_model;
    const uint64_t wallet_generation = m_wallet_generation;
    const uint64_t oracle_generation = ++m_oracle_request_generation;
    m_model->executeRpcAsync(
        "getoracleprice", UniValue{UniValue::VARR},
        [guard, request_model, wallet_generation,
         oracle_generation](UniValue result, QString error) {
            if (!guard || guard->m_model != request_model ||
                guard->m_wallet_generation != wallet_generation ||
                guard->m_oracle_request_generation !=
                    oracle_generation) {
                return;
            }
            qint64 price{0};
            guard->m_oracle_price_micro_usd =
                error.isEmpty() &&
                    GetInt64Field(result, "price_micro_usd", price)
                ? price : 0;
            guard->m_oracle_state = guard->m_oracle_price_micro_usd > 0
                ? OracleState::AVAILABLE : OracleState::UNAVAILABLE;
            guard->updateExternalPrerequisites();
        });
}

QString PaymasterWidgetImpl::dgbAmount(qint64 satoshis)
{
    return PaymasterFormatDGB(satoshis);
}

QString PaymasterWidgetImpl::compactDgbAmount(qint64 satoshis)
{
    QString value = dgbAmount(satoshis);
    while (value.endsWith(QLatin1Char('0')))
        value.chop(1);
    if (value.endsWith(QLatin1Char('.'))) value.chop(1);
    return value;
}

QString PaymasterWidgetImpl::ddAmount(qint64 cents)
{
    return PaymasterFormatDD(cents);
}

void PaymasterWidgetImpl::clearOverviewCapital()
{
    for (auto* value : m_funds_values) value->setText(QStringLiteral("—"));
    if (!m_overview_capital_details) return;
    for (auto* label : findChildren<QLabel*>()) {
        if (label->objectName().startsWith(QLatin1String("paymasterCapital_")))
            setStatusLabel(label, tr("Not loaded"), QStringLiteral("neutral"));
    }
}

void PaymasterWidgetImpl::setCapitalMetric(const char* name, const QString& text, const QString& kind)
{
    setStatusLabel(findChild<QLabel*>(
                       QStringLiteral("paymasterCapital_") + name),
                   maskNumericText(text), kind);
}

void PaymasterWidgetImpl::setFundsValue(const QString& key, const QString& text)
{
    if (auto* value = m_funds_values.value(key)) value->setText(maskNumericText(text));
}

bool PaymasterWidgetImpl::confirmedUserPaid() const
{
    const auto& models = m_operator_snapshot.find_value("provider").find_value("policy").find_value("funding_models");
    if (!models.isArray()) return true; // Fresh setup proposal.
    return std::any_of(models.getValues().begin(), models.getValues().end(), [](const UniValue& model) {
        return model.isStr() && model.get_str() == "user_paid";
    });
}

qint64 PaymasterWidgetImpl::confirmedPaymentFee() const
{
    qint64 value{0};
    const auto& policy = m_operator_snapshot.find_value("provider").find_value("policy");
    if (GetInt64Field(policy, "maximum_network_fee_dgb_satoshis", value) && value > 0 && MoneyRange(value)) return value;
    // Before an offer has been saved, show the setup proposal. Never use
    // an unrelated, unsaved offer draft to describe a reserve approval.
    return DigiDollar::Paymaster::SetupDefaultPolicy().find_value("maximum_network_fee_dgb_satoshis").getInt<qint64>();
}

QString PaymasterWidgetImpl::slotStatusText(const QString& name,
                              const UniValue& status)
{
    if (!status.isObject()) return {};
    return QStringLiteral("%1: %2 ready, %3 pending, %4 missing (target %5)")
        .arg(name)
        .arg(poolNumber(status, "ready"))
        .arg(poolNumber(status, "pending"))
        .arg(poolNumber(status, "missing"))
        .arg(poolNumber(status, "target"));
}

bool PaymasterWidgetImpl::carrierPlanCurrent(const QString& plan_id, qint64 expires_at) const
{
    return !plan_id.isEmpty() && expires_at > 0 &&
           QDateTime::currentSecsSinceEpoch() <= expires_at;
}

QString PaymasterWidgetImpl::readinessExplanation(const QString& error) const
{
    if (error == QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_RETIRED"))
        return tr("This provider identity was permanently retired when its recovered capital was released. New payments and reserve spending are blocked. Its released funds remain in this wallet for normal transfers. Keep the recovery records; use a fresh provider identity for any future service.");
    if (error == QLatin1String("PAYMASTER_CAPITAL_RECOVERY_CHECKPOINT_CHANGED"))
        return tr("The independently saved wallet state changed or could not be secured. No capital release was confirmed. Review the checkpoint and current wallet state before requesting another preview.");
    if (error == QLatin1String("PAYMASTER_CAPITAL_RECOVERY_REQUIRES_RESTORED_WALLET"))
        return tr("Recovery release requires an explicitly restored provider wallet. For a current wallet, use the normal stop and capital-release workflow.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED"))
        return tr("This provider wallet does not match its independently saved safety checkpoint. New payments and reserve spending are blocked. Use the complete current provider wallet and checkpoint together. Do not delete the checkpoint or increase fee limits to bypass this protection.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION"))
        return tr("A reserve transaction was interrupted before its accounting was fully saved. New payments and reserve spending are blocked. Review recorded transactions and diagnostics before continuing; the operation must not be repeated blindly.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_WRITE"))
        return tr("The independent provider safety checkpoint could not be saved. New payments and reserve spending are blocked. Check storage availability and diagnostics before continuing.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"))
        return tr("This provider wallet was restored from a backup. New payments and reserve spending are blocked because signed transactions and spent fee budgets may be missing. A rescan or a higher fee limit cannot resolve this. Recorded exact transactions remain recoverable. In Settings → Wallet & backup, review adoption only if this wallet matches the independently retained current checkpoint and has no unfinished work. Adoption keeps reserves and spent budgets, and leaves the provider stopped. Settings → Start, pause & retire also offers separate capital recovery and permanent retirement.");
    if (error == QLatin1String("PAYMASTER_RESTRICTED_SPONSORSHIP_DISABLED"))
        return tr("The saved sponsorship setting is unsupported. Review a supported offer in Settings. Existing payment records remain available in Activity.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_SYNCING"))
        return tr("Wallet and transaction index updates are catching up with the node. Processing resumes automatically when they agree.");
    if (error == QLatin1String("PAYMASTER_DISABLED"))
        return tr("Paymaster support is disabled for this node. Enable it and restart DigiByte Core.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_NOT_ENABLED"))
        return tr("The provider configuration has not been enabled yet.");
    if (error == QLatin1String("PAYMASTER_IDENTITY_NOT_FOUND"))
        return tr("Create the provider identity under Settings → Offer & pricing.");
    if (error == QLatin1String("PAYMASTER_POLICY_NOT_FOUND"))
        return tr("Choose and save an operating policy under Settings → Offer & pricing.");
    if (error == QLatin1String("PAYMASTER_SAFETY_POLICY_NOT_FOUND") ||
        error == QLatin1String("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND"))
        return tr("Review and save finite spending and rate limits under Settings → Customer-payment budgets.");
    if (error == QLatin1String("PAYMASTER_POOL_PREPARATION_PENDING"))
        return tr("An approved pool preparation is still pending. Review its current step and diagnostic under Settings → Reserves & refill.");
    if (error == QLatin1String("PAYMASTER_POOLS_NOT_PREPARED"))
        return tr("Preview and prepare the required wallet liquidity under Settings → Reserves & refill.");
    if (error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
        error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
        error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING"))
        return tr("The saved targets are correct, but the wallet does not yet contain every confirmed pool output. Restore the missing liquidity; no target update is required.");
    if (error == QLatin1String("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED") ||
        error == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND"))
        return tr("Review and explicitly approve finite automatic liquidity-maintenance limits under Settings → Reserves & refill.");
    if (error == QLatin1String("PAYMASTER_PAYMENT_CAPACITY_IN_USE"))
        return tr("Operating capacity is temporarily in use. Automatic refill stays enabled.");
    if (error == QLatin1String("PAYMASTER_POOL_WAITING_DGB") ||
        error == QLatin1String("PAYMASTER_POOL_WAITING_FUNDS"))
        return refillFundingExplanation(error);
    if (error == QLatin1String("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING"))
        return tr("Wait for the automatically prepared replacement liquidity to confirm.");
    if (error == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
        error == QLatin1String("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL"))
        return tr("The saved DD reserve capacity is too low to receive service fees. Restore reserves to review the required capacity, capital and fee limit together. Previously released capital is restored only with your approval.");
    if (error == QLatin1String("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"))
        return tr("The finite automatic-maintenance budget is exhausted. Wait for the rolling limit to recover or review the saved limits under Settings → Reserves & refill.");
    if (error == QLatin1String("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"))
        return tr("Automatic replenishment could not create or resume a safe maintenance transaction. Review the detailed liquidity status before retrying.");
    if (error == QLatin1String("PAYMASTER_WALLET_LOCKED"))
        return tr("Unlock the wallet before starting the provider or signing provider transactions.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_REQUIRES_DESCRIPTOR_WALLET"))
        return tr("Select a descriptor wallet. Legacy wallets cannot hold the Paymaster provider identity.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_REQUIRES_PRIVATE_KEYS"))
        return tr("Select a wallet with local private keys. Watch-only wallets cannot operate a Paymaster provider.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_REJECTS_EXTERNAL_SIGNER"))
        return tr("Select a wallet without an external signer. Provider identity and transaction signing must be available locally.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED"))
        return tr("Configure a reachable provider endpoint and restart DigiByte Core.");
    if (error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE"))
        return tr("The configured provider endpoint is not reachable by clients on this network.");
    if (error == QLatin1String("PAYMASTER_REQUIRES_PRUNE_0"))
        return tr("Provider operation requires an unpruned node.");
    if (error == QLatin1String("PAYMASTER_REQUIRES_TXINDEX"))
        return tr("Enable the transaction index and restart DigiByte Core.");
    if (error == QLatin1String("PAYMASTER_REQUIRES_READY_TXINDEX"))
        return tr("Wait until the transaction index has finished synchronizing.");
    if (error == QLatin1String("PAYMASTER_REQUIRES_V2_TRANSPORT"))
        return tr("Enable BIP324 version 2 transport and restart DigiByte Core.");
    if (error == QLatin1String("PAYMASTER_NODE_NOT_READY"))
        return tr("Wait until the node has synchronized and is ready to broadcast transactions.");
    if (error == QLatin1String("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"))
        return tr("DigiDollar is not active on the current chain yet.");
    if (error == QLatin1String("PAYMASTER_SAFETY_LIMIT_EXHAUSTED")) {
        qint64 applicable_per_transaction{std::numeric_limits<qint64>::max()};
        const auto include_model = [&applicable_per_transaction](
                const FundingSafetyControls& controls, bool selected) {
            if (!selected) return;
            FundingSafetyValues values;
            if (readFundingSafety(controls, values) && values.per_transaction > 0) {
                applicable_per_transaction = std::min(
                    applicable_per_transaction, values.per_transaction);
            }
        };
        include_model(m_user_paid_safety, m_user_paid->isChecked());
        include_model(
            m_public_sponsored_safety,
            m_sponsored->isChecked() &&
                m_scope->currentData().toString() == QLatin1String("public"));
        include_model(
            m_restricted_sponsored_safety,
            m_sponsored->isChecked() &&
                m_scope->currentData().toString() == QLatin1String("restricted"));
        if (applicable_per_transaction != std::numeric_limits<qint64>::max() &&
            m_network_fee->value() > applicable_per_transaction) {
            return tr(
                "The advertised network-fee ceiling (%1 DGB) exceeds the applicable safety limit per transfer (%2 DGB). Lower the ceiling in Configuration or increase the limit in Safety limits, then save the changed setting.")
                .arg(PaymasterFormatDGB(m_network_fee->value()))
                .arg(PaymasterFormatDGB(applicable_per_transaction));
        }
        return tr("The configured provider budget or rate limit has been reached.");
    }
    if (error == QLatin1String("PAYMASTER_MESSAGE_CAPTURE_ENABLED"))
        return tr("Disable network message capture before operating a mainnet provider.");
    if (error.contains(QLatin1String("WALLET")))
        return tr("The selected wallet does not meet the provider requirements.");
    if (error.startsWith(QLatin1String("PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED:")) &&
        error.contains(QLatin1String("Amount below minimum DigiDollar output."))) {
        return tr("The payout must be at least %1 DD. Wait until enough confirmed earnings have accumulated. No withdrawal was submitted; your earnings remain in the wallet.")
            .arg(PaymasterFormatDD(Params().GetDigiDollarParams().minOutputAmount));
    }
    if (error == QLatin1String("PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE"))
        return tr("This DD reserve is no longer available for release. It may be in use, awaiting confirmation, spent or already released. Refresh the reserves, wait for open payments to finish, then review an available reserve. This attempt did not release capital.");
    if (error == QLatin1String("PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED"))
        return tr("The reserves or saved capacity changed after the review. This attempt did not release capital. Refresh the reserves and review a new plan before approving a release.");
    if (error == QLatin1String("PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO"))
        return tr("The saved DD payment capacity is already zero, so another reserve cannot be released through this action. Refresh the reserves. To retire the provider and review all remaining capital, use Settings → Stop or retire.");
    if (error == QLatin1String("PAYMASTER_NO_WITHDRAWABLE_CARRIER_EXCESS"))
        return tr("There are no confirmed earnings available for withdrawal. Reserved operating capital is retained.");
    if (error == QLatin1String("PAYMASTER_MAINTENANCE_FEE_EXCEEDED"))
        return tr("The estimated fee exceeds the saved maintenance limit per transaction. Review Automation & reserves; the limit is not raised automatically.");
    return tr("The operation could not continue. Technical details: %1").arg(error);
}

void PaymasterWidgetImpl::updateReadinessSummary(const QStringList& errors, bool running, bool ready)
{
    m_backup_adopt->setVisible(!m_privacy && errors.contains(
        QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED")));
    m_backup_adopt->setEnabled(!m_privacy && !m_busy && hasRpcTransport());
    updateExternalPrerequisites();
    for (const auto& error : errors) {
        if (error.startsWith(QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_"))) {
            setStatusLabel(m_next_step, tr("Provider wallet safety check needs review"), QStringLiteral("action"));
            m_readiness_summary->setText(readinessExplanation(error));
            return;
        }
    }
    if (errors.contains(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"))) {
        setStatusLabel(m_next_step, tr("Restored provider wallet is protected"), QStringLiteral("action"));
        m_readiness_summary->setText(readinessExplanation(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED")));
        return;
    }
    if (running) {
        setStatusLabel(m_next_step, tr("Provider is running"),
                       QStringLiteral("ready"));
        m_readiness_summary->setText(tr(
            "Keep this node online and monitor reservations, liquidity and safety-budget usage."));
        return;
    }
    if (ready) {
        setStatusLabel(m_next_step,
                       tr("All local prerequisites are ready"),
                       QStringLiteral("ready"));
        m_readiness_summary->setText(tr(
            "All requirements are satisfied and all setup settings are saved. No additional Save action is required. Start the provider only when you are ready for it to accept client requests within the displayed safety limits."));
        return;
    }

    if (providerConfigurationComplete() && hasPassiveExternalWait()) {
        setStatusLabel(m_next_step,
                       tr("Waiting for blockchain synchronization"),
                       QStringLiteral("waiting"));
        m_readiness_summary->setText(tr(
            "Provider configuration is complete. You do not need to change any provider settings. "
            "Core will enable provider start automatically when the node is synchronized, the transaction index is ready and transactions can be broadcast."));
        return;
    }

    const bool liquidity_attention = std::any_of(
        errors.cbegin(), errors.cend(),
        [](const QString& error) { return isLiquidityReadinessError(error); });
    if (providerConfigurationComplete() && liquidity_attention) {
        setStatusLabel(
            m_next_step,
            tr("All local prerequisites are ready"),
            QStringLiteral("ready"));
        m_readiness_summary->setText(tr(
            "Blockchain synchronization, transaction indexing, transaction broadcast and DigiDollar activation are ready. The remaining wallet-liquidity action is shown separately below."));
        return;
    }

    QString next;
    const QStringList priority{
        QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"),
        QStringLiteral("PAYMASTER_DISABLED"),
        QStringLiteral("PAYMASTER_IDENTITY_NOT_FOUND"),
        QStringLiteral("PAYMASTER_POLICY_NOT_FOUND"),
        QStringLiteral("PAYMASTER_SAFETY_POLICY_NOT_FOUND"),
        QStringLiteral("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND"),
        QStringLiteral("PAYMASTER_PROVIDER_NOT_ENABLED"),
        QStringLiteral("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED"),
        QStringLiteral("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND"),
        QStringLiteral("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE"),
        QStringLiteral("PAYMASTER_POOLS_NOT_PREPARED"),
        QStringLiteral("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING"),
        QStringLiteral("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"),
        QStringLiteral("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"),
        QStringLiteral("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED"),
        QStringLiteral("PAYMASTER_REQUIRES_PRUNE_0"),
        QStringLiteral("PAYMASTER_REQUIRES_TXINDEX"),
        QStringLiteral("PAYMASTER_REQUIRES_READY_TXINDEX"),
        QStringLiteral("PAYMASTER_REQUIRES_V2_TRANSPORT"),
        QStringLiteral("PAYMASTER_NODE_NOT_READY"),
        QStringLiteral("PAYMASTER_WALLET_LOCKED"),
    };
    for (const QString& candidate : priority) {
        if (errors.contains(candidate)) {
            next = readinessExplanation(candidate);
            break;
        }
    }
    if (next.isEmpty() && !errors.isEmpty()) next = readinessExplanation(errors.first());
    const bool unknown_error = std::any_of(
        errors.cbegin(), errors.cend(), [](const QString& error) {
            return !isProviderSetupReadinessError(error) &&
                   !isLiquidityReadinessError(error) &&
                   !isPassiveExternalReadinessError(error) &&
                   error != QLatin1String("PAYMASTER_REQUIRES_TXINDEX");
        });
    setStatusLabel(
        m_next_step,
        next.isEmpty() ? tr("Setup status unavailable")
                       : tr("Next step: %1").arg(next),
        unknown_error ? QStringLiteral("error")
                      : QStringLiteral("action"));

    QStringList explanations;
    for (const QString& error : errors) {
        const QString explanation = readinessExplanation(error);
        if (!explanations.contains(explanation)) explanations.push_back(explanation);
    }
    m_readiness_summary->setText(explanations.isEmpty()
        ? tr("Refresh the status to check the provider requirements.")
        : QStringLiteral("• ") + explanations.join(QStringLiteral("\n• ")));
}

QLineEdit* PaymasterWidgetImpl::integerField(QWidget* parent, qint64 value)
{
    auto* result = new QLineEdit(QString::number(value), parent);
    result->setMaxLength(19);
    result->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,19}")), result));
    return result;
}

QString PaymasterWidgetImpl::providerStartConfirmationText() const
{
    QStringList budgets;
    const auto append_budget = [this, &budgets](const QString& name,
                                                 const FundingSafetyControls& controls) {
        FundingSafetyValues values;
        if (!readFundingSafety(controls, values) || values.allZero()) return;
        budgets.push_back(tr(
            "%1: at most %2 DGB per transfer, %3 DGB per rolling hour and %4 DGB per rolling day; %5 transfers/hour and %6 transfers/day.")
            .arg(name)
            .arg(PaymasterFormatDGB(values.per_transaction))
            .arg(PaymasterFormatDGB(values.per_hour))
            .arg(PaymasterFormatDGB(values.per_day))
            .arg(values.completed_per_hour)
            .arg(values.completed_per_day));
    };
    if (m_user_paid->isChecked()) {
        append_budget(tr("User paid"), m_user_paid_safety);
    }
    if (m_sponsored->isChecked() &&
        m_scope->currentData().toString() == QLatin1String("restricted")) {
        append_budget(tr("Restricted sponsored"),
                      m_restricted_sponsored_safety);
    } else if (m_sponsored->isChecked()) {
        append_budget(tr("Public sponsored"), m_public_sponsored_safety);
    }

    const bool automatic = m_operation_mode != QLatin1String("manual");
    QString liquidity_budget;
    LiquidityPolicyValues liquidity_values;
    if (automatic && m_liquidity_policy_configured &&
        !m_liquidity_policy_dirty &&
        readLiquidityPolicy(liquidity_values) &&
        liquidity_values.paid_maintenance_approved) {
        liquidity_budget = tr(
            "\nAutomatic liquidity maintenance: at most %1 DGB per maintenance transaction, %2 DGB per rolling hour and %3 DGB per rolling day.")
            .arg(PaymasterFormatDGB(liquidity_values.fee_per_transaction))
            .arg(PaymasterFormatDGB(liquidity_values.fee_per_hour))
            .arg(PaymasterFormatDGB(liquidity_values.fee_per_day));
    }
    return tr(
        "Start this provider now in %1 mode?\n\n%2%3\n\n"
        "Once online, the provider may accept client requests and spend DGB for network fees only within these persisted wallet-local limits. "
        "%4")
        .arg(automatic ? tr("automatic") : tr("manual expert"),
              budgets.isEmpty() ? tr("No active funding-model budget is available.")
                               : budgets.join('\n'),
              liquidity_budget,
              automatic
                 ? tr("Core will process eligible provider-side work without a separate dialog for every transfer. Clients must still confirm the exact provider and fee before signing.")
                 : tr("You must process waiting requests and submitted payments manually from Activity and recovery."));
}

QString PaymasterWidgetImpl::providerServiceStatusText() const
{
    for (const auto& error : m_readiness_errors) {
        if (error.startsWith(QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_"))) return readinessExplanation(error);
    }
    if (m_last_service_error.startsWith(QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_"))) return readinessExplanation(m_last_service_error);
    if (m_readiness_errors.contains(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED")) ||
        m_last_service_error == QLatin1String("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED")) {
        return readinessExplanation(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"));
    }
    if (m_service_state == QLatin1String("waiting_for_readiness") &&
        (m_last_service_error ==
             QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
         (!m_liquidity_targets_satisfy_provider_policy &&
          m_last_service_error ==
              QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
        return tr("Automatic start is waiting because the saved user-paid liquidity targets do not include an operational DigiDollar carrier. Review Automation & reserves, save a target of at least one payment carrier and prepare the missing output.");
    }
    if (m_service_state == QLatin1String("replenishing_liquidity") &&
        maintenanceFeeLimitExceeded()) {
        return tr(
            "Automatic refill is paused because its estimated network fee exceeds the saved per-transaction maintenance limit. No transaction was created and no DGB was spent. Review the refill cost limit under Automation & reserves before retrying.");
    }
    if (maintenanceWaitingForFunds()) return readinessExplanation(m_last_service_error);
    const QString error_suffix = m_last_service_error.isEmpty()
        ? QString{}
        : tr(" Stable error: %1").arg(m_last_service_error);
    const QString queue_suffix = tr(
        " Queue now: %1 request(s), %2 submitted payment(s).")
        .arg(m_waiting_provider_requests)
        .arg(m_waiting_provider_submits);
    if (!m_model) {
        return tr("No wallet is selected. Select a provider wallet first.");
    }
    if (m_service_state == QLatin1String("active")) {
        return tr("Automatic operation active — Core processes bounded request and submission work in the background. No manual queue action is required.") + queue_suffix;
    }
    if (m_service_state == QLatin1String("manual")) {
        return tr("Manual expert mode active — use the two processing actions below in order as messages arrive.") + queue_suffix;
    }
    if (m_service_state == QLatin1String("waiting_for_unlock")) {
        return tr("Automatic operation paused while the wallet is locked. Queued messages remain untouched and processing resumes after unlock.") + queue_suffix + error_suffix;
    }
    if (m_service_state == QLatin1String("waiting_for_readiness")) {
        return tr("Core is waiting for provider readiness. Review Overview for the next required action.") + error_suffix;
    }
    if (m_service_state == QLatin1String("waiting_for_maintenance_approval")) {
        return tr("Automatic operation is paused until finite paid liquidity-maintenance limits are explicitly approved. No maintenance transaction is created before approval.") + queue_suffix + error_suffix;
    }
    if (m_service_state == QLatin1String("replenishing_liquidity")) {
        return tr("Automatic operation is restoring missing DGB or carrier slots within the saved maintenance budget. New requests wait until confirmed capacity is ready.") + queue_suffix + error_suffix;
    }
    if (m_service_state == QLatin1String("waiting_for_liquidity_confirmation")) {
        return tr("Replacement liquidity has been prepared and is waiting for blockchain confirmation. Existing authorized work remains recoverable.") + queue_suffix + error_suffix;
    }
    if (m_service_state == QLatin1String("drain_only")) {
        return tr("Recovery-only operation active — Core rejects new quotes and automatically completes only previously authorized durable work.") + queue_suffix + error_suffix;
    }
    if (m_service_state == QLatin1String("error")) {
        return tr("Automatic operation stopped on a safe, privacy-neutral error. Review the stable code and provider readiness before retrying.") + error_suffix;
    }
    if (!m_core_running) {
        if (!m_core_enabled) {
            return tr(
                "Provider configuration is disabled. Autostart cannot bring this wallet online until the provider configuration is enabled again.");
        }
        return m_autostart_enabled
            ? tr("Provider stopped. Autostart is enabled and will start it when the wallet and readiness requirements permit.")
            : tr("Provider stopped — start it on Overview after completing setup. Automatic processing begins only after that conscious start.");
    }
    if (m_core_locked) {
        return tr("Provider paused while the wallet is locked. No queued message is consumed.");
    }
    return tr("Provider runtime state is being refreshed. Core exposes no manual action until a known safe state is available.");
}

QString PaymasterWidgetImpl::automationSummary() const
{
    const auto& policy = m_saved_liquidity_policy;
    if (!IsCompleteLiquidityPolicy(policy)) return tr("Reserve settings have not been verified.");
    const auto& observation = m_overview_liquidity_snapshot.find_value("automation_status");
    const QString state = observation.find_value("state").isStr()
        ? QString::fromStdString(observation.find_value("state").get_str()) : QString{};
    const auto& reason = observation.find_value("reason");
    const bool node_wait = reason.isStr() &&
        (reason.get_str() == "PAYMASTER_REQUIRES_READY_TXINDEX" ||
         reason.get_str() == "PAYMASTER_PROVIDER_SYNCING" ||
         reason.get_str() == "PAYMASTER_TXINDEX_NOT_READY");
    QString text;
    if (!policy.find_value("automatic_replenishment").isTrue()) text = tr("Automatic refill is off.");
    else if (!policy.find_value("paid_maintenance_approved").isTrue()) text = tr("Automatic refill is selected; finite refill costs still need approval.");
    else if (state == QLatin1String("working")) text = tr("Automatic refill is on. An approved refill is being prepared.");
    else if (state == QLatin1String("waiting_confirmation")) text = tr("Automatic refill is on. A refill transaction is waiting for confirmation.");
    else if (state == QLatin1String("paused")) text = tr("Automatic refill is on; execution is temporarily paused.");
    else if (state == QLatin1String("blocked") && node_wait) text = tr("Automatic refill is on; waiting for current block data. Core resumes automatically when the node and transaction index are ready.");
    else if (state == QLatin1String("blocked")) text = tr("Automatic refill is on; a prerequisite needs attention.");
    else if (state == QLatin1String("ready")) text = tr("Automatic refill is on and ready within the saved limits.");
    else text = tr("Automatic refill is on. Refresh the status to verify execution.");
    if (reason.isStr() && !reason.get_str().empty() && policy.find_value("automatic_replenishment").isTrue() &&
        !(state == QLatin1String("blocked") && node_wait)) {
        const auto code = QString::fromStdString(reason.get_str());
        text += QLatin1Char(' ') + (code == QLatin1String("PAYMASTER_PROVIDER_STOPPED") ? tr("Start the provider to use it.") :
            code == QLatin1String("PAYMASTER_MANUAL_PROCESSING") ? tr("Automatic request processing is required.") :
            code == QLatin1String("PAYMASTER_WALLET_LOCKED") ? tr("Unlock the wallet to continue.") :
            code.contains(QLatin1String("LIMIT")) || code.contains(QLatin1String("BUDGET")) ? tr("Review the saved refill limits and remaining budget.") :
            code.contains(QLatin1String("APPROVAL")) ? tr("Review and approve finite refill costs.") :
            readinessExplanation(code));
    }
    if (policy.find_value("paid_maintenance_approved").isTrue())
        text += tr(" Approved: up to %1 DGB per refill, %2 per hour and %3 per day.")
            .arg(dgbAmount(poolNumber(policy, "maximum_maintenance_fee_per_transaction_satoshis")),
                 dgbAmount(poolNumber(policy, "maximum_maintenance_fee_per_hour_satoshis")),
                 dgbAmount(poolNumber(policy, "maximum_maintenance_fee_per_day_satoshis")));
    if (observation.find_value("pending_transaction").isTrue() && !policy.find_value("automatic_replenishment").isTrue())
        text += tr(" A previously signed refill can still confirm.");
    return text;
}

void PaymasterWidgetImpl::updateQuickAutomation()
{
    const bool known = IsCompleteLiquidityPolicy(m_saved_liquidity_policy) &&
        m_saved_liquidity_policy.find_value("updated_at").isNum();
    for (auto* control : m_refill_switches) {
        const QSignalBlocker blocker(control);
        control->setChecked(known && m_saved_liquidity_policy.find_value("automatic_replenishment").isTrue());
        control->setEnabled(known && !m_busy && !m_privacy && !m_refill_save_pending && hasRpcTransport());
    }
    const QString text = m_privacy ? tr("Hidden by privacy mode") : m_refill_save_pending
        ? tr("Saving reserve automation; checking the wallet setting…")
        : automationSummary() + (m_refill_message.isEmpty() ? QString{} : QLatin1Char('\n') + m_refill_message);
    for (auto* label : m_refill_switch_status) label->setText(text);
}

void PaymasterWidgetImpl::applyWithProviderRestart(std::function<void(bool)> apply, std::function<void(const QString&)> failed)
{
    if (m_privacy) return;
    if (m_busy || m_pending_operator_review || !hasRpcTransport()) {
        failed(tr("Settings were not saved because another Paymaster operation is still pending or the wallet is unavailable. Your edits have been kept."));
        return;
    }
    const auto generation = m_wallet_generation;
    const auto settings_generation = m_settings_restart_generation;
    if (!hasCompleteMutationSnapshots()) {
        // Offer and runtime settings suspend polling while being edited.
        // Refresh expired observations before deciding whether to restart;
        // never silently drop Save or weaken the 30-second freshness gate.
        const auto context = m_operator_snapshot;
        failed(tr("Checking current provider status before saving. Your edits have been kept; no settings have been changed yet."));
        m_pending_operator_review = [this, generation, settings_generation, context, apply, failed] {
            if (generation != m_wallet_generation || settings_generation != m_settings_restart_generation || m_privacy) return;
            if (!hasCompleteMutationSnapshots()) {
                failed(tr("Settings were not saved: current provider status could not be verified. Your edits have been kept. Retry saving after the status check succeeds."));
                return;
            }
            try {
                if (!context.isNull()) DigiDollar::Paymaster::CheckSetupContext(context, m_operator_snapshot);
            } catch (const std::exception&) {
                failed(tr("The provider wallet or identity changed during the status check. No settings were saved; review the selected wallet before retrying."));
                return;
            }
            applyWithProviderRestart(apply, failed);
        };
        refreshOperatorStatus();
        return;
    }
    if (!m_core_running) { apply(false); return; }
    setRpcBusyState(true);
    const auto answer = askPlainTextQuestion(this, tr("Save settings and restart Paymaster"),
        tr("Temporarily stop this Paymaster service, save the edited settings and start again after checking the saved values?\n\nNew requests pause briefly. Existing offers may become invalid and clients may need to request a new offer. Already signed payments and their reservations remain protected.\n\nThe saved autostart choice, reserves and spending limits are retained. If any step cannot be confirmed, no further step is attempted; review the status before resuming. DigiByte Core itself is not restarted."));
    if (generation != m_wallet_generation) return;
    setRpcBusyState(false);
    if (m_privacy || settings_generation != m_settings_restart_generation || !hasRpcTransport()) return;
    if (answer != QMessageBox::Yes) {
        failed(tr("Saving was cancelled. Your edits have been kept; the provider was not stopped."));
        return;
    }
    UniValue params{UniValue::VARR}; params.push_back(false);
    call("setpaymasterenabled", params, false, nullptr, [this, apply, failed, settings_generation](const UniValue& result) {
        if (m_privacy || settings_generation != m_settings_restart_generation) return;
        if (!result.find_value("enabled").isFalse() || !result.find_value("running").isFalse()) {
            failed(tr("The service pause could not be confirmed. No setting change or restart was attempted. Refresh the provider status."));
            return;
        }
        m_core_enabled = m_core_running = false;
        apply(true);
    }, false, [failed](const QString&) {
        failed(tr("The service could not be paused. It may still be processing a payment. No setting change or restart was attempted. Check current status before trying again."));
    });
}

void PaymasterWidgetImpl::finishProviderSettingsRestart(std::function<bool(const UniValue&)> verified, std::function<void(const QString&)> failed)
{
    const auto generation = m_settings_restart_generation;
    call("getpaymasteroperatorinfo", {}, false, nullptr, [this, verified, failed, generation](const UniValue& snapshot) {
        if (m_privacy || generation != m_settings_restart_generation) return;
        const auto& provider = snapshot.find_value("provider");
        if (!provider.find_value("enabled").isFalse() || !provider.find_value("running").isFalse() || !verified(provider)) {
            failed(tr("The saved settings or paused state could not be verified. Restart was not attempted. Refresh and review the provider before resuming."));
            return;
        }
        UniValue params{UniValue::VARR}; params.push_back(true);
        call("setpaymasterenabled", params, false, nullptr, [this, failed, generation](const UniValue& result) {
            if (m_privacy || generation != m_settings_restart_generation) return;
            if (!result.find_value("enabled").isTrue()) {
                failed(tr("The settings were saved, but provider enablement could not be confirmed. Check current status before resuming."));
                return;
            }
            UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
            options.pushKV("wait_for_readiness", true);
            params.push_back(options);
            call("startpaymaster", params, false, nullptr, [this, failed, generation](const UniValue& result) {
                if (m_privacy || generation != m_settings_restart_generation) return;
                if (!IsCompleteProviderStartResult(result) ||
                    (!result.find_value("running").isTrue() && !result.find_value("start_requested").isTrue())) {
                    failed(tr("The settings were saved, but the provider start was not confirmed. Review the current status in Overview."));
                    return;
                }
                refreshOperatorStatus();
            }, false, [failed](const QString&) {
                failed(tr("The settings were saved and the provider enabled, but start could not be confirmed. Check Overview; saved autostart may still start the service."));
            });
        }, false, [failed](const QString&) {
            failed(tr("The settings were saved, but resuming the provider could not be confirmed. Check Overview before trying again."));
        });
    }, false, [failed](const QString&) {
        failed(tr("The settings could not be read back. Restart was not attempted. Check current status before resuming."));
    });
}

bool PaymasterWidgetImpl::canStartProvider() const
{
    return hasCompleteMutationSnapshots() && !m_busy && m_core_eligible &&
           m_core_enabled && (m_core_ready || repairableLiquidityGap()) &&
           !m_core_running && !m_core_locked && providerSafetyAllowsSelectedModels() &&
           !m_policy_dirty && !m_runtime_settings_dirty && !m_liquidity_policy_dirty && !m_autostart_dirty && !m_autostart_pending_read;
}

void PaymasterWidgetImpl::updateProviderButtons()
{
    updateConfigurationWarnings();
    if (m_save_provider_safety) {
        const QString error = m_privacy ? QString{} : providerSafetyInputError();
        m_save_provider_safety->setEnabled(!m_privacy && !m_busy && m_provider_safety_snapshot_representable && error.isEmpty());
    }
    const bool complete_snapshots = hasCompleteMutationSnapshots();
    updatePoolPreparationStatus();
    const bool manual_mode = m_operation_mode == QLatin1String("manual");
    if (m_request_processing_group) {
        m_request_processing_group->setVisible(manual_mode);
    }
    if (m_submit_processing_group) {
        m_submit_processing_group->setVisible(manual_mode);
    }
    if (m_activity_result_group) {
        m_activity_result_group->setVisible(manual_mode && !m_privacy);
    }
    if (m_process_requests) {
        m_process_requests->setEnabled(
            complete_snapshots && manual_mode && m_core_running &&
            !m_core_locked && !m_busy);
    }
    if (m_process_submits) {
        m_process_submits->setEnabled(
            complete_snapshots && manual_mode && m_core_running &&
            !m_core_locked && !m_busy);
    }
    if (m_operation_mode_select) {
        m_operation_mode_select->setEnabled(!m_busy);
    }
    updateAutostartControls();
    if (m_save_runtime_settings) {
        m_save_runtime_settings->setEnabled(
            hasRpcTransport() && m_runtime_settings_dirty && !m_busy);
        m_save_runtime_settings->setText(m_core_running ? tr("Save and restart provider…") : tr("Save processing mode"));
    }
    if (m_activity_runtime_status) {
        m_activity_runtime_status->setText(providerServiceStatusText());
    }
    if (!complete_snapshots) {
        // A malformed response must revoke every transaction-producing
        // action immediately instead of leaving controls enabled from an
        // older valid snapshot.
        for (QPushButton* action : {
                 m_prepare_preview, m_prepare_execute,
                 m_rebalance_preview, m_rebalance_execute,
                 m_preview_carrier_excess,
                 m_execute_carrier_excess,
                 m_preview_carrier_release,
                 m_execute_carrier_release}) {
            if (action) action->setEnabled(false);
        }
    }
    if (m_operation_primary) {
        m_operation_primary->setText(tr("Open operation overview"));
        m_operation_primary->setEnabled(!m_privacy && !m_busy);
    }
    updateOperatorDashboard();
    updateAutostartControls();
}

void PaymasterWidgetImpl::setStatusLabel(QLabel* label, const QString& text,
                    const QString& kind) const
{
    if (!label) return;
    QString symbol{QStringLiteral("•")};
    if (kind == QLatin1String("ready")) symbol = QStringLiteral("✓");
    if (kind == QLatin1String("waiting")) symbol = QStringLiteral("…");
    if (kind == QLatin1String("action")) symbol = QStringLiteral("!");
    if (kind == QLatin1String("error")) symbol = QStringLiteral("!");
    const QString rendered = QStringLiteral("%1  %2").arg(symbol, text);
    const bool kind_changed =
        label->property("statusKind").toString() != kind;
    if (label->text() != rendered) label->setText(rendered);
    if (kind_changed) {
        label->setProperty("statusKind", kind);
        if (label->style()) {
            label->style()->unpolish(label);
            label->style()->polish(label);
        }
    }
}

void PaymasterWidgetImpl::setStatusKind(QWidget* widget, const QString& kind) const
{
    if (!widget) return;
    if (widget->property("statusKind").toString() == kind) return;
    widget->setProperty("statusKind", kind);
    if (widget->style()) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }
}

void PaymasterWidgetImpl::updateOperatorDashboard()
{
    const bool offer_ready = m_core_has_identity && m_policy_loaded &&
                             !m_policy_dirty && m_core_enabled;
    setStatusLabel(
        m_overview_offer_status,
        offer_ready ? tr("Ready · identity and offer are saved")
                    : m_policy_dirty
                          ? tr("Action required · save the offer changes")
                          : !m_core_has_identity
                                ? tr("Action required · create a provider identity")
                                : tr("Action required · complete and enable the offer"),
        offer_ready ? QStringLiteral("ready") : QStringLiteral("action"));
    if (m_overview_offer_action) {
        m_overview_offer_action->setText(offer_ready ? tr("Review offer")
                                                     : tr("Complete offer"));
    }

    const bool safety_ready = providerSafetyAllowsSelectedModels() &&
                              !m_provider_safety_dirty;
    setStatusLabel(
        m_overview_safety_status,
        safety_ready ? tr("Ready · finite spending limits are active")
                     : m_provider_safety_dirty
                           ? tr("Action required · save the changed limits")
                           : tr("Action required · set finite spending limits"),
        safety_ready ? QStringLiteral("ready") : QStringLiteral("action"));
    if (m_overview_safety_action) {
        m_overview_safety_action->setText(safety_ready ? tr("Review limits")
                                                       : tr("Set limits"));
    }

    const bool liquidity_ready = m_pool_status_loaded &&
                                 m_liquidity_targets_satisfy_provider_policy &&
                                 savedPoolTargetsReady();
    const bool liquidity_waiting = m_maintenance_state == QLatin1String("replenishing") ||
                                   m_maintenance_state == QLatin1String("waiting_for_confirmation") ||
                                   m_service_state == QLatin1String("replenishing_liquidity") ||
                                   m_service_state == QLatin1String("waiting_for_liquidity_confirmation");
    const bool refill_approval_needed =
        m_maintenance_state ==
        QLatin1String("waiting_for_maintenance_approval");
    const bool refill_waiting_for_start =
        shouldStartAndRestoreLiquidity() && !liquidity_ready;
    const bool refill_fee_limit_exceeded =
        maintenanceFeeLimitExceeded() && !liquidity_ready;
    setStatusLabel(
        m_overview_liquidity_status,
        m_overview_liquidity_snapshot.isNull() ? tr("Current capital status unavailable")
                        : liquidity_ready ? tr("Ready · target capacity is available")
                        : !m_liquidity_targets_satisfy_provider_policy
                              ? tr("Action required · restore DD reserve capacity for service fees")
                        : refill_approval_needed
                              ? tr("Action required · targets are saved; approve the bounded refill cost")
                        : refill_fee_limit_exceeded
                              ? tr("Action required · refill cost exceeds the approved limit")
                        : maintenanceWaitingForFunds()
                              ? tr("Action required · refill needs wallet funds")
                        : refill_waiting_for_start
                              ? tr("Action required · refill approved; start the provider to restore liquidity")
                        : liquidity_waiting
                              ? tr("Waiting · replacement liquidity is being prepared")
                              : tr("Action required · liquidity is below its target"),
        m_overview_liquidity_snapshot.isNull() ? QStringLiteral("neutral")
                        : liquidity_ready ? QStringLiteral("ready")
                        : !m_liquidity_targets_satisfy_provider_policy
                              ? QStringLiteral("action")
                              : refill_fee_limit_exceeded || maintenanceWaitingForFunds()
                                    ? QStringLiteral("action")
                              : liquidity_waiting
                                    ? QStringLiteral("waiting")
                                    : QStringLiteral("action"));
    if (m_overview_liquidity_action) {
        m_overview_liquidity_action->setText(
            liquidity_ready
                ? tr("Review liquidity")
                : !m_liquidity_targets_satisfy_provider_policy
                      ? tr("Review liquidity targets")
                : refill_approval_needed
                      ? tr("Approve refill costs")
                : refill_fee_limit_exceeded
                      ? tr("Review refill cost limit")
                : maintenanceWaitingForFunds()
                      ? tr("View funds and reserves")
                : refill_waiting_for_start
                      ? tr("Start and restore liquidity")
                      : tr("Restore liquidity"));
    }

    QString operation_kind{QStringLiteral("action")};
    QString operation_text;
    if (m_service_state == QLatin1String("active")) {
        operation_kind = QStringLiteral("ready");
        operation_text = tr("Ready · automatic provider operation is active");
    } else if (m_readiness_errors.contains(QStringLiteral(
                   "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ||
               (!m_liquidity_targets_satisfy_provider_policy &&
                (m_last_service_error == QLatin1String(
                     "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
                 m_last_service_error == QLatin1String(
                     "PAYMASTER_OPERATIONAL_SLOT_MISSING")))) {
        operation_text = tr(
            "Action required · save a payment-carrier target before starting");
    } else if (refill_fee_limit_exceeded) {
        operation_kind = QStringLiteral("action");
        operation_text = tr(
            "Paused · automatic refill exceeds the approved cost limit");
    } else if (maintenanceWaitingForFunds()) {
        operation_text = tr("Action required · refill needs wallet funds");
    } else if (m_service_state.startsWith(QLatin1String("waiting_")) ||
               m_service_state == QLatin1String("replenishing_liquidity")) {
        operation_kind = QStringLiteral("waiting");
        operation_text = providerServiceStatusText();
    } else if (m_service_state == QLatin1String("error")) {
        operation_kind = QStringLiteral("error");
        operation_text = tr("Error · provider operation requires attention");
    } else if (providerConfigurationComplete() &&
               m_readiness_errors.contains(
                   QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
        operation_kind = QStringLiteral("waiting");
        operation_text = Params().GetChainType() == ChainType::REGTEST
            ? tr("Waiting · mine a fresh Regtest block; provider settings are already saved")
            : tr("Waiting · node is not ready to broadcast transactions");
    } else if (refill_approval_needed) {
        operation_kind = QStringLiteral("action");
        operation_text = tr(
            "Action required · approve bounded refill costs; targets are already saved");
    } else if (refill_waiting_for_start) {
        operation_kind = QStringLiteral("action");
        operation_text = tr(
            "Action required · start the provider to restore missing liquidity");
    } else if (m_core_ready) {
        operation_text = tr("Action required · provider is ready to start");
    } else {
        operation_text = tr("Action required · complete the remaining setup steps");
    }
    setStatusLabel(m_overview_operation_status, operation_text, operation_kind);
    if (m_overview_operation_action) {
        if (!m_liquidity_targets_satisfy_provider_policy) {
            m_overview_operation_action->setText(
                tr("Review liquidity targets"));
        } else if (m_service_state == QLatin1String("active")) {
            m_overview_operation_action->setText(tr("Open operations"));
        } else if (providerConfigurationComplete() &&
                   hasPassiveExternalWait()) {
            m_overview_operation_action->setText(tr("Refresh status"));
        } else if (refill_approval_needed) {
            m_overview_operation_action->setText(
                tr("Approve refill costs"));
        } else if (refill_fee_limit_exceeded) {
            m_overview_operation_action->setText(
                tr("Review refill cost limit"));
        } else if (maintenanceWaitingForFunds()) {
            m_overview_operation_action->setText(tr("View funds and reserves"));
        } else if (refill_waiting_for_start) {
            m_overview_operation_action->setText(
                tr("Review liquidity"));
        } else {
            m_overview_operation_action->setText(tr("Review operation"));
        }
    }

    updatePoolPreparationStatus();
}

bool PaymasterWidgetImpl::isOperatorPage(QWidget* page) const
{
    return m_tabs->currentWidget() == page ||
        (m_tabs->currentWidget() == m_settings_tabs && m_settings_tabs->currentWidget() == page);
}

void PaymasterWidgetImpl::showOperatorPage(QWidget* page)
{
    if (m_settings_tabs->indexOf(page) >= 0) {
        m_tabs->setCurrentWidget(m_settings_tabs);
        m_settings_tabs->setCurrentWidget(page);
    } else {
        m_tabs->setCurrentWidget(page);
    }
}

void PaymasterWidgetImpl::updateNavigationLayout()
{
    const bool compact = width() < 960;
    m_navigation->setVisible(!compact);
    m_navigation_select->setVisible(compact);
    m_navigation->setFixedWidth(std::max(180, m_navigation->sizeHintForColumn(0) + 32));
}

void PaymasterWidgetImpl::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    AlignPaymasterFormLabels(m_offer_forms);
    updateNavigationLayout();
}

void PaymasterWidgetImpl::setPageAvailable(QWidget* page, bool enabled, const QString& explanation)
{
    const int index = m_tabs->indexOf(page);
    if (index < 0) return;
    page->setEnabled(enabled && !m_busy);
    page->setProperty("paymasterPageAvailable", enabled);
    auto* item = m_navigation->item(index);
    item->setFlags(enabled ? item->flags() | Qt::ItemIsEnabled : item->flags() & ~Qt::ItemIsEnabled);
    item->setToolTip(explanation);
    if (auto* model = qobject_cast<QStandardItemModel*>(m_navigation_select->model()))
        model->item(index)->setEnabled(enabled);
    m_navigation_select->setItemData(index, explanation, Qt::ToolTipRole);
}

bool PaymasterWidgetImpl::isCurrentOperatorRefresh() const
{
    return (m_active_rpc_command == "getpaymasteroperatorinfo" ||
            m_active_rpc_command == "getpaymasterfinancestatus") &&
           !m_operator_snapshot.isNull() &&
           !m_operation.stale(QDateTime::currentSecsSinceEpoch());
}

void PaymasterWidgetImpl::updateOperatorProgress()
{
    const bool history_wait = m_model && m_model->digiDollarHistoryRefreshInProgress();
    const bool reading = history_wait || (m_rpc_active && m_active_rpc_command.rfind("get", 0) == 0);
    if (!reading) m_operator_read_elapsed.invalidate();
    if (!reading || m_privacy) {
        m_operator_progress_timer->stop();
        m_operator_loading->hide();
        m_operator_loading_step->clear();
        m_operator_progress->setAccessibleDescription({});
        return;
    }
    if (!m_operator_read_elapsed.isValid()) m_operator_read_elapsed.start();
    if (!m_operator_progress_timer->isActive()) m_operator_progress_timer->start();
    // Fast polls retain the last verified presentation without flashing.
    // A slow poll must still explain why actions remain unavailable.
    const bool quick_history_read = history_wait && !m_operator_snapshot.isNull() &&
        !m_operation.stale(QDateTime::currentSecsSinceEpoch()) && m_operator_read_elapsed.elapsed() < 1000;
    if (quick_history_read || (!history_wait && isCurrentOperatorRefresh() && m_operator_read_elapsed.elapsed() < 2000)) {
        m_operator_loading->hide();
        m_operator_loading_step->clear();
        m_operator_progress->setAccessibleDescription({});
        return;
    }
    QString step = tr("Reading wallet data…");
    if (m_active_rpc_command == "getpaymasterinfo")
        step = tr("Reading provider settings…");
    else if (m_active_rpc_command == "getpaymasterliquiditystatus")
        step = tr("Reading operating capital…");
    else if (m_active_rpc_command == "getpaymasteroperatorinfo")
        step = tr("Reading service and wallet status…");
    else if (m_active_rpc_command == "getpaymasterfinancestatus")
        step = tr("Reading income and costs…");
    else if (m_active_rpc_command == "getpaymasterpoolinfo")
        step = tr("Reading prepared reserves…");
    else if (m_active_rpc_command == "getpaymastersafetystatus")
        step = tr("Reading provider spending limits…");
    else if (m_active_rpc_command == "getpaymasterclientsafetystatus")
        step = tr("Reading client fee limits…");
    if (history_wait) step = tr("Waiting for the full DigiDollar history check on this node…");
    const qint64 seconds = m_operator_read_elapsed.elapsed() / 1000;
    const QString text = tr("%1 (%2 s elapsed)").arg(step).arg(seconds);
    m_operator_loading_step->setText(text);
    m_operator_progress->setAccessibleDescription(text);
    m_operator_loading->show();
    if (!m_operator_progress_timer->isActive()) m_operator_progress_timer->start();
}

void PaymasterWidgetImpl::updateLifecycleControls()
{
    // Settings and Overview share the same reviewed actions and gates.
    m_lifecycle_start->setText(m_operator_start->text());
    m_lifecycle_start->setEnabled(!m_privacy && !m_busy && m_operator_start->isEnabled());
    m_lifecycle_start->setToolTip(m_privacy ? QString{} : m_operator_start->toolTip());
    m_lifecycle_pause->setEnabled(!m_privacy && !m_busy && !m_operator_pause->isHidden() && m_operator_pause->isEnabled());
    m_lifecycle_status->setText(m_privacy ? tr("Hidden by privacy mode") : m_operator_headline->text());
}

void PaymasterWidgetImpl::showOperatorBusyState()
{
    updateOperatorProgress();
    if (m_model && m_model->digiDollarHistoryRefreshInProgress() && !m_privacy) {
        if (!m_operator_snapshot.isNull() && !m_operation.stale(QDateTime::currentSecsSinceEpoch()) &&
            m_operator_read_elapsed.elapsed() < 1000) return;
        m_operator_headline->setText(tr("Waiting for the DigiDollar history check…"));
        m_status->setText(tr("Wallet history is being checked; provider status will refresh automatically."));
        m_operator_hint->setText(tr("The original DigiDollar ownership and key-recovery checks are running on this node. Paymaster controls wait for those checks. This wait does not start or stop the Core provider, approve a payment, or discard protected transfers. The current service readiness has not been rechecked yet."));
        m_operator_action->setEnabled(false);
        m_operator_start->setEnabled(false);
        m_operator_pause->setEnabled(false);
        for (const auto& key : {QStringLiteral("user_paid"), QStringLiteral("sponsored")}) {
            if (auto* status = m_live_status.value(key))
                setStatusLabel(status, tr("Waiting for the history check; readiness not rechecked"), QStringLiteral("waiting"));
        }
        setStatusKind(m_operator_card, QStringLiteral("waiting"));
        updateLifecycleControls();
        return;
    }
    // Polling keeps a recent, validated status in place; initial reads and
    // stale/error recovery still show loading. RPC mutation guards remain active.
    if (!m_busy || m_privacy || isCurrentOperatorRefresh()) return;
    const bool reading = m_active_rpc_command.rfind("get", 0) == 0;
    m_operator_headline->setText(reading ? tr("Reading provider status…") : tr("Wallet operation in progress…"));
    if (reading) m_status->setText(tr("Reading current provider status…"));
    m_operator_hint->setText(reading ? tr("The display is loading saved settings and current wallet status. This read does not start or stop the provider. An autostarted provider may already be serving requests, and earlier public announcements may still be listed by clients. Controls become available automatically when the checks finish; you do not need to repeat setup.") : tr("Complete any open confirmation and wait for the current wallet operation to finish."));
    if (reading) {
        for (const auto& key : {QStringLiteral("user_paid"), QStringLiteral("sponsored")}) {
            auto* status = m_live_status.value(key);
            if (status && status->property("statusKind").toString() == QLatin1String("waiting"))
                setStatusLabel(status, tr("Reading current readiness…"), QStringLiteral("waiting"));
        }
    }
    m_operator_action->setText(tr("Please wait…"));
    m_operator_action->setToolTip(m_operator_hint->text());
    m_operator_action->setEnabled(false);
    m_operator_start->setEnabled(false);
    m_operator_pause->setEnabled(false);
    m_operator_stop->setEnabled(false);
    m_operator_retire->setEnabled(false);
    setStatusKind(m_operator_card, QStringLiteral("waiting"));
    updateLifecycleControls();
}

void PaymasterWidgetImpl::renderEndpointDiagnostic(const UniValue& snapshot)
{
    m_diagnostic_rpc->clear();
    m_diagnostic_cli->clear();
    m_diagnostic_rpc_copy->setEnabled(false);
    m_diagnostic_cli_copy->setEnabled(false);
    m_diagnostic_hint->clear();
    m_diagnostic_endpoint->setText(m_privacy ? tr("Hidden by privacy mode") : tr("Current announced address unavailable. Refresh status to read it."));
    if (m_privacy || snapshot.isNull()) return;
    const auto& network_value = snapshot.find_value("network");
    const auto& endpoint_value = snapshot.find_value("provider").find_value("endpoint");
    if (!network_value.isStr()) return;
    const auto network = QString::fromStdString(network_value.get_str());
    if (network != QLatin1String("main") && network != QLatin1String("test") &&
        network != QLatin1String("regtest") && network != QLatin1String("signet")) return;
    if (!endpoint_value.isStr() || endpoint_value.get_str().empty()) {
        m_diagnostic_endpoint->setText(tr("No announced Paymaster address is configured. Open node connection settings and configure -paymasterendpoint with the address and port reachable from the second node, then restart this node."));
        return;
    }
    // Parse without DNS and only format the normalized IP/Tor endpoint. Never
    // interpolate arbitrary RPC strings into a copyable shell command.
    const auto endpoint = LookupNumeric(endpoint_value.get_str(), 0);
    const bool loopback = network == QLatin1String("regtest") && endpoint.IsLocal();
    if (!endpoint.IsValid() || !endpoint.GetPort() ||
        !(endpoint.IsIPv4() || endpoint.IsIPv6() || endpoint.IsTor()) ||
        !(endpoint.IsRoutable() || loopback)) {
        m_diagnostic_endpoint->setText(tr("The announced address is not supported by checkpaymasterendpoint. Use a public numeric IP or Tor v3 address with a port; loopback is allowed only on regtest. Review node connection settings."));
        return;
    }
    const auto address = QString::fromStdString(endpoint.ToStringAddrPort());
    m_diagnostic_endpoint->setText(tr("Announced endpoint: %1\nNetwork: %2").arg(address, network));
    if (endpoint.IsTor()) {
        m_diagnostic_hint->setText(tr("Tor: the second node needs a working Tor SOCKS5 proxy configured with -onion or -proxy, with -proxyrandomize=1 and -logips=0. The proxy must accept randomized credentials. This test uses Tor without a direct-connection fallback."));
    } else if (loopback) {
        m_diagnostic_hint->setText(tr("Loopback address: run the test on a second node on this same computer. This does not test reachability from the Internet or another computer."));
    } else {
        m_diagnostic_hint->setText(tr("For an external reachability check, run the command on a node outside the provider's network. The announced address and port must route to the Paymaster listener through any firewall or NAT."));
    }
    const auto rpc = QStringLiteral("checkpaymasterendpoint \"%1\"").arg(address);
    m_diagnostic_rpc->setPlainText(rpc);
    m_diagnostic_cli->setPlainText(QStringLiteral("digibyte-cli -chain=%1 %2").arg(network, rpc));
    m_diagnostic_rpc_copy->setEnabled(true);
    m_diagnostic_cli_copy->setEnabled(true);
}

void PaymasterWidgetImpl::resetOperatorPresentation()
{
    renderEndpointDiagnostic(UniValue{});
    for (auto* warning : {m_operator_config_warning, m_policy_config_warning, m_safety_config_warning}) {
        if (!warning) continue;
        warning->clear();
        warning->parentWidget()->hide();
    }
    for (auto* label : m_live_status) setStatusLabel(label, tr("Status unavailable"), QStringLiteral("waiting"));
    for (auto* label : m_live_details) label->clear();
    m_overview_budget_details->hide();
    for (auto* label : m_overview_budget_details->findChildren<QLabel*>()) {
        if (label->objectName().startsWith(QLatin1String("paymasterBudget_"))) label->clear();
    }
    for (auto* section : m_overview_budget_details->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (section->objectName().startsWith(QLatin1String("paymasterBudget_"))) section->hide();
    m_operator_next_action = QStringLiteral("refresh");
    m_operator_start_allowed = false;
    m_operator_start->hide();
    m_operator_start->setEnabled(false);
    m_operator_start->setText(tr("Start Paymaster…"));
    m_operator_action->show();
    m_operator_headline->setText(tr("Current status unavailable"));
    m_operator_hint->setText(tr("Refresh the selected wallet's status before starting operation."));
    m_operator_connection->clear();
    m_operator_wallet->clear();
    m_operator_action->setText(tr("Refresh status"));
    m_operator_action->setToolTip(tr("Read current status from the selected wallet."));
    m_operator_next_area.clear();
    m_operator_headline->setAccessibleDescription(QString());
    m_operator_action->setEnabled(!m_privacy && hasRpcTransport() && !m_busy);
    m_operator_pause->hide();
    m_operator_stop->setEnabled(false);
    m_operator_retire->setEnabled(false);
    setStatusKind(m_operator_card, QStringLiteral("waiting"));
    showOperatorBusyState();
    updateLifecycleControls();
}

void PaymasterWidgetImpl::renderOperatorPresentation(const UniValue& snapshot)
{
    updateConfigurationWarnings();
    if (m_privacy) { resetOperatorPresentation(); return; }
    const auto& provider = snapshot.find_value("provider");
    const auto& diagnostics = snapshot.find_value("diagnostics");
    const auto& service = provider.find_value("service_state");
    if (!snapshot.find_value("schema_version").isNum() || snapshot.find_value("schema_version").getInt<int>() != 1 ||
        !provider.find_value("running").isBool() || !provider.find_value("ready").isBool() ||
        !provider.find_value("wallet_locked").isBool() || !service.isStr() || !diagnostics.isArray() || diagnostics.empty()) { resetOperatorPresentation(); return; }
    const QString state = QString::fromStdString(service.get_str());
    const QStringList states{QStringLiteral("stopped"), QStringLiteral("active"), QStringLiteral("manual"),
                             QStringLiteral("waiting_for_unlock"), QStringLiteral("waiting_for_readiness"),
                             QStringLiteral("waiting_for_maintenance_approval"), QStringLiteral("replenishing_liquidity"),
                             QStringLiteral("waiting_for_liquidity_confirmation"), QStringLiteral("drain_only"), QStringLiteral("error")};
    if (!states.contains(state)) { resetOperatorPresentation(); return; }
    renderEndpointDiagnostic(snapshot);
    const bool running = provider.find_value("running").get_bool();
    const bool ready = provider.find_value("ready").get_bool();
    const bool stopped = IsExplicitlyStoppedProvider(provider);
    const bool start_pending = !running && provider.find_value("enabled").isTrue() &&
        provider.find_value("start_requested").isTrue() &&
        (state == QLatin1String("waiting_for_readiness") || state == QLatin1String("waiting_for_unlock"));
    const auto& unlock_deadline = snapshot.find_value("unlocked_until");
    const bool expired = unlock_deadline.isNum() && unlock_deadline.getInt<int64_t>() > 0 &&
                         unlock_deadline.getInt<int64_t>() <= QDateTime::currentSecsSinceEpoch();
    const bool locked = provider.find_value("wallet_locked").get_bool() || expired;
    if ((state == QLatin1String("stopped") && running) ||
        ((state == QLatin1String("active") || state == QLatin1String("manual")) && !running)) { resetOperatorPresentation(); return; }
    QString action;
    QString area;
    for (const auto& item : diagnostics.getValues()) {
        if (!item.find_value("code").isStr() || !item.find_value("state").isStr() ||
            !item.find_value("action").isStr() || !item.find_value("area").isStr()) { resetOperatorPresentation(); return; }
        const QString code = QString::fromStdString(item.find_value("code").get_str());
        const QString diagnostic_state = QString::fromStdString(item.find_value("state").get_str());
        const QString next = QString::fromStdString(item.find_value("action").get_str());
        const QStringList diagnostic_states{QStringLiteral("ready"), QStringLiteral("waiting"), QStringLiteral("action_required"), QStringLiteral("error"), QStringLiteral("unknown")};
        const QStringList diagnostic_actions{QStringLiteral("none"), QStringLiteral("start"), QStringLiteral("enable"), QStringLiteral("unlock"), QStringLiteral("review_unlock"), QStringLiteral("wait"), QStringLiteral("setup"), QStringLiteral("review_liquidity"), QStringLiteral("review_budget"), QStringLiteral("backup_wallet"), QStringLiteral("configure_node"), QStringLiteral("check_external"), QStringLiteral("inspect_error")};
        if (!diagnostic_states.contains(diagnostic_state) || !diagnostic_actions.contains(next) ||
            ((next == QLatin1String("start") || next == QLatin1String("none")) && code != QLatin1String("PAYMASTER_LOCAL_READY"))) { resetOperatorPresentation(); return; }
        // A new/unknown success or status code must never become a start permission.
        if ((diagnostic_state == QLatin1String("ready") && code != QLatin1String("PAYMASTER_LOCAL_READY")) ||
            (diagnostic_state == QLatin1String("unknown") && code != QLatin1String("PAYMASTER_EXTERNAL_REACHABILITY_UNKNOWN")) ||
            diagnostic_state == QLatin1String("error") || next == QLatin1String("inspect_error")) {
            action = QStringLiteral("inspect_error");
            break;
        }
        if (action.isEmpty() && next != QLatin1String("none") && next != QLatin1String("check_external") && next != QLatin1String("backup_wallet")) {
            action = next;
            area = code.contains(QLatin1String("MAINTENANCE")) ? QStringLiteral("liquidity") : QString::fromStdString(item.find_value("area").get_str());
        }
    }
    if (action.isEmpty() && state == QLatin1String("drain_only")) {
        action = QStringLiteral("wait");
        area = QStringLiteral("service");
    }
    if (action.isEmpty()) action = ready && !running ? QStringLiteral("start") : QStringLiteral("activity");
    if (locked && action != QLatin1String("inspect_error")) action = QStringLiteral("unlock");
    if (state == QLatin1String("error")) action = QStringLiteral("inspect_error");
    if ((action == QLatin1String("start") && (!ready || running)) ||
        (action == QLatin1String("activity") && (!ready || state == QLatin1String("error")))) action = QStringLiteral("inspect_error");
    QString headline;
    QString hint;
    QString button;
    QString kind = QStringLiteral("waiting");
    QString checkpoint_error;
    for (const auto& error : m_readiness_errors) {
        if (error.startsWith(QLatin1String("PAYMASTER_PROVIDER_CHECKPOINT_"))) { checkpoint_error = error; break; }
    }
    if (!checkpoint_error.isEmpty()) {
        headline = tr("Provider wallet safety check needs review");
        hint = readinessExplanation(checkpoint_error);
        button = tr("View diagnostics");
        kind = QStringLiteral("action");
        action = QStringLiteral("inspect_error");
    } else if (m_readiness_errors.contains(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"))) {
        headline = tr("Restored provider wallet is protected");
        hint = readinessExplanation(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"));
        button = tr("View diagnostics");
        kind = QStringLiteral("action");
        action = QStringLiteral("inspect_error");
    } else if (action == QLatin1String("start") || action == QLatin1String("enable")) {
        headline = ready ? tr("Ready to start") : tr("Operation is paused");
        hint = tr("Start using the saved settings. You do not need to repeat setup; existing spending limits remain in force.");
        button = ready ? tr("Start provider…") : tr("Resume provider…");
    } else if (action == QLatin1String("unlock") || action == QLatin1String("review_unlock")) {
        headline = locked ? tr("Waiting for wallet unlock") : tr("Wallet will lock soon");
        hint = tr("Choose continuous operation until manual lock or restart, or a timed unlock. Access applies to the whole wallet; the passphrase is never saved.");
        button = tr("Unlock wallet…");
    } else if (action == QLatin1String("wait") && area == QLatin1String("service") && state == QLatin1String("drain_only")) {
        headline = tr("New payments temporarily paused");
        hint = tr("Core is only completing existing work. It checks readiness again automatically and resumes new requests when the saved limits and prerequisites allow it.");
        button = tr("Check current status");
    } else if (action == QLatin1String("wait") && area == QLatin1String("payments")) {
        headline = tr("Payment reserves in use");
        hint = tr("Open offers and payments reserve capacity. After a client cancels an unsigned request, the provider may keep its reservation until the offer expires. Core checks this automatically; no refill or higher fee limit is needed for reserved capacity.");
        button = tr("View payment activity");
    } else if (action == QLatin1String("wait")) {
        headline = area == QLatin1String("liquidity") ? tr("Waiting for confirmations") : tr("Waiting for the node");
        hint = tr("Synchronization or confirmations are still pending. Review progress; no new approval is needed to wait.");
        const auto& prerequisites = provider.find_value("prerequisites");
        const auto& chain_height = prerequisites.find_value("chain_height");
        const auto& index_height = prerequisites.find_value("txindex_height");
        if (running && area == QLatin1String("node") &&
            prerequisites.find_value("txindex_synced").isTrue() &&
            chain_height.isNum() && index_height.isNum() &&
            chain_height.getInt<int64_t>() >= 0 &&
            index_height.getInt<int64_t>() >= 0 &&
            index_height.getInt<int64_t>() <= chain_height.getInt<int64_t>() &&
            chain_height.getInt<int64_t>() - index_height.getInt<int64_t>() <= 1 &&
            !m_readiness_errors.contains(QStringLiteral("PAYMASTER_NODE_NOT_READY")) &&
            !m_readiness_errors.contains(QStringLiteral("PAYMASTER_DIGIDOLLAR_NOT_ACTIVE"))) {
            // Presentation only. Core still gates each quote/signature on
            // exact wallet, index and chain-tip agreement.
            headline = tr("Provider is running · checking the latest block");
            hint = tr("Wallet and transaction index are catching up with the latest block. New requests and reserve refills wait for current data and resume automatically. Existing reservations remain protected.");
        }
        button = tr("View progress");
    } else if (action == QLatin1String("review_liquidity")) {
        headline = m_liquidity_targets_satisfy_provider_policy ? tr("Reserves need restoring") : tr("A DD reserve is required to receive service fees");
        hint = m_liquidity_targets_satisfy_provider_policy
            ? tr("Check the missing capital and maximum fees, then approve once. Core will create the missing reserves and wait for confirmations.")
            : tr("The saved DD reserve capacity is too low for your offer. The next step proposes the required capacity and shows the capital and fees before you approve. Your spending limits stay unchanged.");
        button = m_core_running ? tr("Restore reserves…") : tr("Restore reserves and start…");
        if (maintenanceWaitingForFunds()) {
            headline = tr("Reserve refill needs wallet funds");
            hint = readinessExplanation(m_last_service_error);
            button = tr("View funds and reserves");
            kind = QStringLiteral("action");
        }
    } else if (action == QLatin1String("review_budget")) {
        headline = tr("Spending limits need attention");
        hint = tr("Compare the offer's network-fee ceiling with the per-payment limits and remaining rolling budgets. Rolling budgets recover over time; limits are never raised automatically.");
        button = tr("Review spending limits");
    } else if (action == QLatin1String("configure_node")) {
        headline = tr("Connection needs attention");
        hint = tr("Review the listener, public endpoint and node prerequisites before starting operation.");
        button = tr("Review connection");
    } else if (action == QLatin1String("setup")) {
        headline = tr("Complete provider setup");
        hint = tr("The assistant reviews your saved choices and shows the remaining steps.");
        button = tr("Continue setup…");
    } else if (action == QLatin1String("activity") && running && ready) {
        headline = state == QLatin1String("manual") ? tr("Running in manual mode") : tr("Provider is running");
        hint = state == QLatin1String("manual") ? tr("Review incoming requests in Activity. This mode requires manual processing.") : tr("Requests are processed within your approved limits. Review payments and pending work in Activity.");
        button = tr("View activity");
        kind = QStringLiteral("ready");
    } else {
        action = QStringLiteral("inspect_error");
        headline = tr("Status needs review");
        hint = tr("A problem or an unknown status prevents a reliable readiness assessment. Open the diagnostic details before acting.");
        button = tr("View diagnostics");
    }
    const QString preparation_details = formatPoolPreparation(provider.find_value("preparation"), PreparationDetail::Operator);
    if (!preparation_details.isEmpty()) {
        if (area == QLatin1String("liquidity") && action == QLatin1String("wait")) {
            headline = tr("Pool preparation is pending");
            hint = preparation_details;
            button = tr("View pool preparation");
        } else if (area == QLatin1String("liquidity") && action == QLatin1String("review_liquidity")) {
            headline = tr("Pool preparation needs attention");
            hint = preparation_details;
            button = tr("View reserve funding and next steps");
            if (canReviewPreparationFee()) {
                headline = tr("Setup fee approval needs updating");
                hint = tr("The missing reserve has not been created. Review the setup fee below; the wallet will safely close the uncreated step and show a new plan for your approval. Recurring maintenance limits are unchanged.");
                button = tr("Review setup fee and continue…");
            }
        }
    }
    // A reserved payment slot is not a failed provider or proof of a
    // broadcast maintenance transaction. Keep all overview text consistent
    // with the same current snapshot, without weakening error precedence.
    const auto& service_error = provider.find_value("last_service_error");
    const bool capacity_wait = action == QLatin1String("wait") && area == QLatin1String("liquidity") &&
        running && state == QLatin1String("waiting_for_liquidity_confirmation") &&
        service_error.isStr() && service_error.get_str() == "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING" &&
        preparation_details.isEmpty();
    const std::string work_phase = DigiDollar::Paymaster::OperatorWorkPhase(provider);
    const bool observed_work = running && state != QLatin1String("drain_only") && work_phase != "idle" && preparation_details.isEmpty() &&
        (action == QLatin1String("activity") || (action == QLatin1String("wait") &&
         (area == QLatin1String("liquidity") || area == QLatin1String("payments"))));
    if (capacity_wait || observed_work) {
        const auto& reserved = provider.find_value("pool").find_value("reserved");
        const bool payment_in_progress = work_phase == "payment" || work_phase == "capacity" ||
            (provider.find_value("pool").find_value("activity").isNull() && reserved.isNum() && reserved.getInt<int64_t>() > 0);
        action = QStringLiteral("wait");
        kind = QStringLiteral("waiting");
        if (payment_in_progress) {
            area = QStringLiteral("payments");
            const bool accepting = ready && !locked && provider.find_value("enabled").isTrue() &&
                (state == QLatin1String("active") || state == QLatin1String("manual"));
            headline = accepting ? (state == QLatin1String("manual") ? tr("Running in manual mode · customer work open") : tr("Provider is running · customer work open"))
                : work_phase == "capacity" ? tr("Payment capacity reserved") : tr("Payment reserves in use");
            hint = accepting
                ? tr("Offers and payments are using part of the reserves. Independent free reserves remain available for further requests within the saved limits. Review individual requests in Activity; an open offer is not a completed payment.")
                : state == QLatin1String("manual")
                ? tr("Operating capital is reserved for a payment. Review the current request in Activity; manual processing remains enabled.")
                : tr("New requests are temporarily paused. Open offers and payments reserve capacity. After a client cancels an unsigned request, the provider may keep its reservation until the offer expires. Core continues automatically within the saved limits. A reservation is not a completed payment or a refill expense.");
            button = tr("View payment activity");
            kind = accepting ? QStringLiteral("ready") : QStringLiteral("waiting");
            if (accepting && state == QLatin1String("manual")) hint += tr(" Manual processing remains enabled.");
            setStatusLabel(m_overview_liquidity_status, accepting ? tr("Ready · free payment reserves are available") : tr("In use · capacity is reserved for a payment"), kind);
        } else {
            area = QStringLiteral("liquidity");
            headline = tr("Waiting for reserve confirmations");
            hint = tr("Reserve outputs are waiting for blockchain confirmation. Core continues automatically when they are confirmed. No new approval is needed.");
            button = tr("View progress");
            setStatusLabel(m_overview_liquidity_status, tr("Waiting · reserves need blockchain confirmation"), QStringLiteral("waiting"));
        }
        m_overview_liquidity_action->setText(tr("View progress"));
    }
    if (running && ready && !locked && provider.find_value("enabled").isTrue() &&
        state == QLatin1String("active") && preparation_details.isEmpty() &&
        area == QLatin1String("liquidity") &&
        (action == QLatin1String("review_liquidity") || action == QLatin1String("review_budget") ||
         action == QLatin1String("wait"))) {
        headline = tr("Provider is running · reserve refill pending");
        hint.prepend(tr("Confirmed free reserves are available for new payments within your saved limits. The refill target has not yet been reached. "));
        button = tr("Review reserve refill");
        action = QStringLiteral("review_liquidity");
        kind = QStringLiteral("ready");
        setStatusLabel(m_overview_liquidity_status,
                       tr("Ready · free payment reserves available; refill pending"), QStringLiteral("ready"));
    }
    if (action == QLatin1String("wait") && area == QLatin1String("payments")) {
        const auto& recipient_limit = snapshot.find_value("safety").find_value("policy").find_value("maximum_active_quotes_per_recipient");
        if (recipient_limit.isNum() && recipient_limit.getInt<int64_t>() > 0) {
            hint += tr(" Open-offer limit per recipient: %1.").arg(recipient_limit.getInt<int64_t>());
        }
    }
    if (stopped && action != QLatin1String("inspect_error")) {
        // Service state and readiness for a future restart are separate.
        // Keep the prerequisite action separate from an explicit one-shot
        // start request. Core still decides when service may actually run.
        const QString prerequisite_hint = hint;
        headline = tr("Paymaster is stopped");
        hint = tr("The provider is disabled and accepts no new requests. Start/resume requests operation with saved settings; Core waits until all prerequisites are met. It does not approve refill costs or change limits. Already signed transactions can still confirm.");
        if (action != QLatin1String("start") && action != QLatin1String("enable"))
            hint += QLatin1Char('\n') + prerequisite_hint;
        kind = QStringLiteral("neutral");
        const auto& operations = provider.find_value("active_operations");
        if (operations.isArray() && operations.empty() &&
            DigiDollar::Paymaster::OperatorWorkPhase(provider) == "idle" &&
            provider.find_value("liquidity").find_value("policy").find_value("automatic_replenishment").isFalse() &&
            !m_overview_liquidity_snapshot.isNull()) {
            setStatusLabel(m_overview_liquidity_status, savedPoolTargetsReady()
                ? tr("Refill off · reserves retained") : tr("Refill off · reserves below saved target"), QStringLiteral("neutral"));
            m_overview_liquidity_action->setText(tr("Review liquidity"));
        }
    } else if (start_pending && action != QLatin1String("inspect_error")) {
        headline = area == QLatin1String("liquidity") && !preparation_details.isEmpty()
            ? tr("Start requested · %1").arg(headline)
            : tr("Start requested · waiting for prerequisites");
        hint.prepend(tr("A start is requested for this wallet session. Core starts only when all checks pass. Pause or Stop cancels the request. "));
    }
    if (m_operator_restart_required) hint.prepend(tr("Node settings were saved. Restart the node explicitly to apply them. "));
    m_operator_next_action = action;
    m_operator_next_area = area;
    m_operator_headline->setText(headline);
    m_status->setText(headline);
    m_operator_hint->setText(hint);
    m_operator_action->setText(button);
    m_operator_action->setToolTip(hint);
    m_operator_action->setEnabled(!m_privacy && hasRpcTransport() && !m_busy);
    const bool stopped_start_request = stopped && provider.find_value("settings_present").isTrue() &&
        provider.find_value("policy").isObject() && !provider.find_value("policy").empty() &&
        (action == QLatin1String("review_budget") || action == QLatin1String("review_liquidity") ||
         action == QLatin1String("wait") || action == QLatin1String("configure_node"));
    m_operator_start_allowed = !running && !locked && !provider.find_value("start_requested").isTrue() && IsHex256Field(provider, "provider_id") &&
        (action == QLatin1String("start") || action == QLatin1String("enable") ||
         (action == QLatin1String("backup_wallet") && ready) || stopped_start_request);
    m_operator_start->setEnabled(m_operator_start_allowed && hasRpcTransport() && !m_busy);
    m_operator_start->setText(stopped || action == QLatin1String("enable") ? tr("Resume Paymaster…") : tr("Start Paymaster…"));
    m_operator_start->setToolTip(m_operator_start_allowed
        ? tr("Request a start with saved settings. Core waits for readiness; refill approval, budgets and autostart remain unchanged.")
        : start_pending ? tr("A start is already requested. Review the missing prerequisites, or cancel the request with Pause or Stop.")
                        : tr("Already running or prerequisites are missing. Review the current status before starting."));
    m_operator_action->setVisible(!m_operator_start_allowed || stopped_start_request);
    const QString action_role = m_operator_start_allowed ? QStringLiteral("secondaryAction") : QStringLiteral("primaryAction");
    if (m_operator_action->property("paymasterRole").toString() != action_role) {
        m_operator_action->setProperty("paymasterRole", action_role);
        m_operator_action->style()->unpolish(m_operator_action);
        m_operator_action->style()->polish(m_operator_action);
    }
    m_operator_start->setVisible(m_operator_start_allowed);
    m_operator_start->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    m_operator_pause->setVisible(running || provider.find_value("enabled").isTrue() || provider.find_value("autostart").isTrue() || provider.find_value("start_requested").isTrue());
    m_operator_pause->setEnabled(hasRpcTransport() && !m_busy);
    const bool can_stop = IsHex256Field(provider, "provider_id") && hasRpcTransport() && !m_busy;
    m_operator_stop->setEnabled(can_stop && (!stopped || provider.find_value("autostart").isTrue()));
    m_operator_stop->setToolTip(stopped && !provider.find_value("autostart").isTrue()
        ? tr("Already stopped. Use Retire Paymaster to review reserve release and backup.") : QString{});
    m_operator_retire->setEnabled(can_stop);
    if (running || provider.find_value("enabled").isTrue() || provider.find_value("autostart").isTrue()) {
        m_retirement_summary->hide();
        m_retirement_balances->clear();
    }
    // A completed stop/retirement message describes the previous run.
    // Clear it only after a current snapshot confirms running again and
    // the RPC chain has finished. Preserve failed/incomplete stop errors.
    const auto stop_result_kind = m_operator_stop_status->property("statusKind").toString();
    if (running && !m_busy && (stop_result_kind == QLatin1String("neutral") || stop_result_kind == QLatin1String("ready"))) {
        m_operator_stop_status->clear();
        m_operator_stop_status->hide();
    }
    setStatusKind(m_operator_card, kind);
    const auto& listener = provider.find_value("transport").find_value("listener_ready");
    const QString local = !listener.isBool() ? tr("Local listener: unknown") : listener.isTrue() ? tr("Local listener: ready") :
                                                                                                   tr("Local listener: unavailable");
    m_operator_connection->setText(local + tr(" · External reachability: no current measurement"));
    QString wallet = locked ? tr("Wallet: locked") : tr("Wallet: unlocked");
    const auto& until = snapshot.find_value("unlocked_until");
    if (!locked && until.isNum() && until.getInt<int64_t>() == -1)
        wallet += tr(" · Continuous operation until manual lock or node restart");
    else if (!locked && until.isNum() && until.getInt<int64_t>() > 0)
        wallet += tr(" until %1").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(until.getInt<int64_t>()), QLocale::ShortFormat));
    m_operator_wallet->setText(wallet);
    renderLiveOverview(snapshot, locked);
    try {
        QStringList budgets;
        const UniValue budget_rows = DigiDollar::Paymaster::OperatorBudgets(snapshot);
        for (const auto& budget : budget_rows.getValues()) {
            if (!budget.find_value("approved").isTrue()) continue;
            const auto name = budget.find_value("name").get_str();
            const auto& policy = provider.find_value("policy");
            const auto& models = policy.find_value("funding_models");
            const auto active_model = [&](const char* model) {
                return models.isArray() && std::any_of(models.getValues().begin(), models.getValues().end(),
                                                       [model](const UniValue& value) { return value.isStr() && value.get_str() == model; });
            };
            if (name == "user_paid" && !active_model("user_paid")) continue;
            if (name == "public_sponsored" && (!active_model("sponsored") || !policy.find_value("sponsorship_scope").isStr() || policy.find_value("sponsorship_scope").get_str() != "public")) continue;
            if (name == "restricted_sponsored" && (!active_model("sponsored") || !policy.find_value("sponsorship_scope").isStr() || policy.find_value("sponsorship_scope").get_str() != "restricted")) continue;
            auto* section = m_overview_budget_details->findChild<QWidget*>(QStringLiteral("paymasterBudget_") + QString::fromStdString(name));
            if (!section) continue;
            for (const auto& field : {"day_spent", "day_limit", "reserved"}) {
                auto* amount = section->findChild<QLabel*>(QStringLiteral("paymasterBudget_") + QString::fromStdString(name) + QLatin1Char('_') + field);
                amount->setText(tr("%1 DGB").arg(compactDgbAmount(budget.find_value(field).getInt<int64_t>())));
            }
            budgets << QString::fromStdString(name);
        }
        for (const char* name : {"user_paid", "public_sponsored", "restricted_sponsored", "refill"}) {
            auto* section = m_overview_budget_details->findChild<QWidget*>(QStringLiteral("paymasterBudget_") + name);
            if (section) section->setVisible(budgets.contains(QString::fromLatin1(name)));
        }
        m_overview_budget_details->setVisible(!budgets.isEmpty());
        setStatusLabel(m_overview_safety_status, budgets.isEmpty() ? tr("No spending authorization available") :
            action == QLatin1String("review_budget") ? (stopped ? tr("Saved limits · review before restart") : tr("Review spending limits")) :
            stopped ? tr("Saved limits") : tr("Limits active"),
            action == QLatin1String("review_budget") || budgets.isEmpty() ? QStringLiteral("action") :
            stopped ? QStringLiteral("neutral") : QStringLiteral("ready"));
    } catch (const std::exception&) {
        m_overview_budget_details->hide();
        setStatusLabel(m_overview_safety_status, tr("Budget status unavailable"), QStringLiteral("waiting"));
    }
    showOperatorBusyState();
    updateAutostartControls();
}

void PaymasterWidgetImpl::renderLiveOverview(const UniValue& snapshot, bool locked)
{
    const auto& provider = snapshot.find_value("provider");
    const auto& policy = provider.find_value("policy");
    const auto& safety = snapshot.find_value("safety");
    const auto& state = provider.find_value("service_state");
    const bool stopped = IsExplicitlyStoppedProvider(provider);
    const bool accepting = provider.find_value("running").isTrue() && provider.find_value("enabled").isTrue() &&
        provider.find_value("ready").isTrue() && !locked && state.isStr() &&
        m_operator_next_action != QLatin1String("inspect_error") &&
        (state.get_str() == "active" || state.get_str() == "manual");
    for (const auto& key : {QStringLiteral("user_paid"), QStringLiteral("sponsored")}) {
        auto* status = m_live_status.value(key);
        auto* detail = m_live_details.value(key);
        if (!IsCompleteProviderPolicy(policy)) {
            setStatusLabel(status, tr("Saved offer unavailable"), QStringLiteral("waiting"));
            detail->clear();
            continue;
        }
        const auto& models = policy.find_value("funding_models").getValues();
        const bool enabled = std::any_of(models.begin(), models.end(), [&key](const UniValue& item) {
            return item.get_str() == key.toStdString();
        });
        if (!enabled) {
            setStatusLabel(status, tr("Off · not offered"), QStringLiteral("neutral"));
            detail->setText(tr("Disabled in the saved offer."));
            continue;
        }
        const bool user_paid = key == QLatin1String("user_paid");
        const bool restricted = policy.find_value("sponsorship_scope").get_str() == "restricted";
        const char* budget = user_paid ? "user_paid" : restricted ? "restricted_sponsored" : "public_sponsored";
        const auto& limits = safety.find_value("policy").find_value(budget);
        const auto& usage = safety.find_value(budget);
        qint64 limit{0};
        bool configured = safety.find_value("configured").isTrue() && IsCompleteFundingSafety(limits) &&
            GetInt64Field(limits, "maximum_network_fee_per_transaction_satoshis", limit) && limit > 0;
        if (configured) {
            for (const char* field : {"maximum_reserved_network_fee_satoshis", "maximum_network_fee_per_hour_satoshis",
                                     "maximum_network_fee_per_day_satoshis", "maximum_completed_per_hour", "maximum_completed_per_day"})
                configured &= limits.find_value(field).getInt<int64_t>() > 0;
        }
        const auto& can_accept = usage.find_value("can_accept_minimum_quote");
        if (stopped) {
            setStatusLabel(status, tr("Configured · provider stopped"), QStringLiteral("neutral"));
        } else if (!configured) {
            setStatusLabel(status, tr("Enabled · spending approval needed"), QStringLiteral("action"));
        } else if (!can_accept.isBool()) {
            setStatusLabel(status, tr("Enabled · budget status unavailable"), QStringLiteral("waiting"));
        } else if (!can_accept.isTrue()) {
            setStatusLabel(status, tr("Enabled · spending or offer limit reached"), QStringLiteral("action"));
        } else if (!accepting) {
            setStatusLabel(status,
                locked ? tr("Enabled · waiting for wallet unlock") :
                m_operator_next_area == QLatin1String("payments") ? tr("Enabled · new requests paused while reserves are in use") :
                m_operator_next_area == QLatin1String("node") ? tr("Enabled · waiting for current block data") :
                tr("Enabled · waiting for provider readiness"), QStringLiteral("waiting"));
        } else {
            setStatusLabel(status, state.get_str() == "manual" ? tr("Locally ready · manual processing") : tr("Locally ready · accepting requests"), QStringLiteral("ready"));
        }
        QString pricing;
        if (user_paid) {
            const auto rate = policy.find_value("fee_rate_bps").getInt<int>();
            pricing = rate == 0 ? tr("Saved fee: 0% · clients pay no DD service fee.")
                : tr("Saved service fee: %1%.").arg(PaymasterFormatDD(rate));
            qint64 cap{0};
            if (GetInt64Field(policy, "maximum_user_paid_service_fee_cents", cap) && cap > 0)
                pricing += tr(" Maximum: %1 DD.").arg(PaymasterFormatDD(cap));
        } else {
            pricing = restricted ? tr("Invitation only · no DD service fee.") : tr("Public sponsorship · no DD service fee.");
        }
        detail->setText(pricing + QLatin1Char('\n') + tr("This wallet pays the DGB network fee. Each request is checked against current reserves and limits."));
    }
    const auto& activity = provider.find_value("pool").find_value("activity");
    const auto& queue = provider.find_value("service_queue");
    qint64 payments{0}, capacity{0}, requests{0}, submits{0};
    auto* status = m_live_status.value(QStringLiteral("activity"));
    auto* detail = m_live_details.value(QStringLiteral("activity"));
    if (!GetInt64Field(activity, "active_payments", payments) || payments < 0 ||
        !GetInt64Field(activity, "capacity_requests", capacity) || capacity < 0 ||
        !GetInt64Field(queue, "waiting_requests", requests) || requests < 0 ||
        !GetInt64Field(queue, "waiting_submits", submits) || submits < 0) {
        setStatusLabel(status, tr("Payment counts unavailable"), QStringLiteral("waiting"));
        detail->setText(tr("Open Activity to inspect individual requests. Reserve progress is shown separately."));
        return;
    }
    setStatusLabel(status, payments || capacity || requests || submits ? tr("Customer work in progress") : tr("No open customer work"),
        payments || capacity || requests || submits ? QStringLiteral("waiting") : QStringLiteral("neutral"));
    detail->setText(maskNumericText(tr("Offers / payments holding a budget: %1\nCapacity reservations: %2\nQueued requests: %3 · signed submissions: %4\nCounts describe different stages and can overlap. An open offer is not a completed payment.")
        .arg(payments).arg(capacity).arg(requests).arg(submits)));
}

void PaymasterWidgetImpl::performOperatorNextAction()
{
    if (m_privacy || m_busy) return;
    const QString action = m_operator_next_action;
    if (action == QLatin1String("refresh"))
        refreshStatus();
    else if (action == QLatin1String("start") || action == QLatin1String("enable"))
        resumeOperator();
    else if (action == QLatin1String("unlock") || action == QLatin1String("review_unlock"))
        unlockOperatorWallet();
    else if (action == QLatin1String("backup_wallet")) {
        showOperatorPage(m_connection_page);
        m_overview_backup_now->setFocus(Qt::OtherFocusReason);
        if (auto* scroll = qobject_cast<QScrollArea*>(m_connection_page))
            scroll->ensureWidgetVisible(m_overview_backup_notice);
    }
    else if (action == QLatin1String("setup"))
        m_reopen_wizard->click();
    else if (action == QLatin1String("review_budget"))
        showOperatorPage(m_operator_next_area == QLatin1String("liquidity") ? m_liquidity_page : m_safety_page);
    else if (action == QLatin1String("review_liquidity")) {
        if (maintenanceWaitingForFunds()) { showOperatorPage(m_liquidity_page); return; }
        if (canReviewPreparationFee()) { cancelPoolPreparation(true); return; }
        if (m_operation.startNeedsReserveApproval() ||
            (!m_operation.observed_work && m_operation.phase != PaymasterOperationController::Phase::Waiting))
            beginGuidedTask("preparepaymasterpool", !m_core_running);
        else if (m_operation.phase == PaymasterOperationController::Phase::Blocked) m_task_continue->click();
        else if (m_operation.phase == PaymasterOperationController::Phase::Waiting) m_task_card->setFocus();
        else beginGuidedTask("preparepaymasterpool", !m_core_running);
    } else if (action == QLatin1String("wait") && m_operator_next_area == QLatin1String("service"))
        refreshStatus();
    else if (action == QLatin1String("wait") && m_operator_next_area == QLatin1String("payments"))
        showOperatorPage(m_activity_page);
    else if (action == QLatin1String("wait") && m_operator_next_area == QLatin1String("liquidity")) {
        if (m_task_card->isHidden()) showOperatorPage(m_liquidity_page);
        else m_task_card->setFocus();
    } else if (action == QLatin1String("configure_node") || action == QLatin1String("wait")) {
        showOperatorPage(m_connection_page);
        if (auto* toggle = findChild<QPushButton*>(QStringLiteral("paymasterPrerequisitesToggle"))) toggle->setChecked(true);
    } else if (action == QLatin1String("activity"))
        showOperatorPage(m_activity_page);
    else if (auto* toggle = findChild<QPushButton*>(QStringLiteral("paymasterTechnicalDetailsToggle")))
        toggle->setChecked(true);
}

void PaymasterWidgetImpl::markProviderStatusCurrent()
{
    m_status_age.restart();
    const QString text = tr("Network: %1 · Last checked: %2")
        .arg(QString::fromStdString(Params().GetChainTypeString()),
             QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat));
    if (m_last_checked->text() != text) m_last_checked->setText(text);
}

void PaymasterWidgetImpl::refreshOperatorStatus(bool background)
{
    if (m_model && m_model->digiDollarHistoryRefreshInProgress()) {
        m_history_deferred_status = true;
        showOperatorBusyState();
        return;
    }
    if (!hasRpcTransport() || ((m_busy || (background && m_rpc_active)) && m_rpc_handler_depth == 0) || m_setup_wizard_active || m_privacy) return;
    const QPointer<WalletModel> selected = m_model;
    call("getpaymasteroperatorinfo", {}, false, nullptr, [this, selected, background](const UniValue& snapshot) {
             if (m_model != selected || m_privacy) return;
             try {
                 const auto& provider = snapshot.find_value("provider");
                 if (!snapshot.find_value("schema_version").isNum() || snapshot.find_value("schema_version").getInt<int>() != 1 || !snapshot.find_value("diagnostics").isArray() || !provider.find_value("service_state").isStr()) throw std::runtime_error("PAYMASTER_STATUS_INCOMPLETE");
                 if (!IsCompleteProviderInfoSnapshot(provider)) throw std::runtime_error("PAYMASTER_PROVIDER_STATUS_INCOMPLETE");
                 // Refresh freshness on every successful observation, but
                 // do not reload forms or rebuild presentation for identical
                 // data. observed_at alone is not a content change.
                 bool unchanged = !m_operator_snapshot.isNull();
                 for (const auto* key : {"provider", "safety", "diagnostics", "wallet_generation", "unlocked_until", "encrypted", "node_settings", "node_setting_overrides"})
                     unchanged &= m_operator_snapshot.find_value(key).write() == snapshot.find_value(key).write();
                 if (background && unchanged) {
                     m_operator_snapshot = snapshot;
                     markProviderStatusCurrent();
                     m_operation.observe(snapshot, m_wallet_generation, QDateTime::currentSecsSinceEpoch());
                     renderCurrentTask();
                     renderOperatorPresentation(snapshot);
                     if (!m_overview_finance_age.isValid() || m_overview_finance_age.elapsed() >= 30000)
                         refreshOverviewFinance(/*background=*/true);
                     return;
                 }
                 const bool capital_changed = m_operator_snapshot.find_value("provider").find_value("pool").write() != provider.find_value("pool").write() ||
                     m_operator_snapshot.find_value("safety").write() != snapshot.find_value("safety").write();
                 m_operator_snapshot = snapshot;
                 applyProviderInfo(provider);
                 applyProviderSafetyStatus(snapshot.find_value("safety"));
                 if (!m_provider_safety_snapshot_available) throw std::runtime_error("PAYMASTER_SAFETY_STATUS_INCOMPLETE");
                 if (!IsCompleteLiquidityStatus(provider.find_value("liquidity")))
                     throw std::runtime_error("PAYMASTER_LIQUIDITY_STATUS_INCOMPLETE");
                 applyLiquidityStatus(provider.find_value("liquidity"));
                 m_pool_preparation = provider.find_value("preparation");
                 updatePoolPreparationStatus();
                 m_operation.observe(snapshot, m_wallet_generation, QDateTime::currentSecsSinceEpoch());
                 renderCurrentTask();

                 QStringList lines;
                 lines << tr("Service: %1 — local readiness: %2").arg(QString::fromStdString(provider.find_value("service_state").get_str()), provider.find_value("ready").isTrue() ? tr("ready") : tr("requirements pending"));
                 const auto& transport = provider.find_value("transport");
                 lines << tr("Connection: listener %1; outgoing %2/%3; incoming %4/%5; waiting %6").arg(transport.find_value("listener_ready").isTrue() ? tr("ready") : tr("unavailable"), QString::fromStdString(transport.find_value("outbound_in_use").write()), QString::fromStdString(transport.find_value("outbound_limit").write()), QString::fromStdString(transport.find_value("inbound_in_use").write()), QString::fromStdString(transport.find_value("inbound_limit").write()), QString::fromStdString(transport.find_value("queued").write()));
                 const auto& liquidity = provider.find_value("liquidity");
                 lines << tr("Liquidity: %1").arg(QString::fromStdString(liquidity.find_value("maintenance_state").get_str()));
                 for (const auto& asset : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
                     const auto& counts = liquidity.find_value(asset);
                     lines << tr("%1: available %2, reserved %3, pending %4, missing %5").arg(QString::fromUtf8(asset), QString::fromStdString(counts.find_value("ready").write()), QString::fromStdString(counts.find_value("reserved").write()), QString::fromStdString(counts.find_value("pending").write()), QString::fromStdString(counts.find_value("missing").write()));
                 }
                 const QString preparation_details = formatPoolPreparation(provider.find_value("preparation"));
                 if (!preparation_details.isEmpty()) lines << preparation_details;
                 const auto budgets = DigiDollar::Paymaster::OperatorBudgets(snapshot);
                 for (const auto& budget : budgets.getValues()) {
                     const auto amount = [&](const char* key) { return QString::fromStdString(DigiDollar::Paymaster::OperatorAmount(budget.find_value(key).getInt<int64_t>())); };
                     lines << tr("Budget %1 (DGB): approved %2; per transaction %3; reserved %4; hour spent/limit/remaining %5/%6/%7; day spent/limit/remaining %8/%9/%10.")
                         .arg(QString::fromStdString(budget.find_value("name").get_str()), budget.find_value("approved").isTrue() ? tr("yes") : tr("no"), amount("transaction_limit"), amount("reserved"), amount("hour_spent"), amount("hour_limit"), amount("hour_remaining"), amount("day_spent"), amount("day_limit")).arg(amount("day_remaining"));
                 }
                 const auto& prerequisites = provider.find_value("prerequisites");
                 if (prerequisites.find_value("txindex_enabled").isTrue() && !prerequisites.find_value("txindex_synced").isTrue()) lines << tr("Waiting for transaction index: indexed height %1, chain height %2.").arg(QString::fromStdString(prerequisites.find_value("txindex_height").write()), QString::fromStdString(prerequisites.find_value("chain_height").write()));
                 const auto& queue = provider.find_value("service_queue");
                 lines << tr("Open work: %1 requests, %2 submissions; durable payments and recovery remain in Activity.").arg(QString::fromStdString(queue.find_value("waiting_requests").write()), QString::fromStdString(queue.find_value("waiting_submits").write()));
                 lines << tr("Wallet: %1. Unlock deadline: %2. Full-wallet backup: %3.").arg(provider.find_value("wallet_locked").isTrue() ? tr("locked") : tr("unlocked"), snapshot.find_value("unlocked_until").getInt<int64_t>() > 0 ? QDateTime::fromSecsSinceEpoch(snapshot.find_value("unlocked_until").getInt<int64_t>(), Qt::UTC).toString(Qt::ISODate) : tr("none"), provider.find_value("backup_status").find_value("required").isTrue() ? tr("recommended") : tr("no pending reminder"));
                 const auto& diagnostics = snapshot.find_value("diagnostics");
                 for (const auto& item : diagnostics.getValues()) {
                     if (item.find_value("state").get_str() == "ready" && item.find_value("action").get_str() == "none") continue;
                     const auto action = item.find_value("action").get_str();
                     const QString wait_instruction = item.find_value("area").get_str() == "payments"
                         ? tr("Wait for the current payment or capacity reservation; review Activity.")
                         : tr("Wait for synchronization or confirmation; configuration changes are not required.");
                     const QString instruction = action == "configure_node" ? tr("Open Node connection to review startup settings and routing.") : action == "unlock" || action == "review_unlock" ? tr("Review the wallet unlock duration.") : action == "wait" ? wait_instruction : action == "review_liquidity" ? tr("Open Overview to inspect the current task and confirmations.") : action == "review_budget" ? tr("Review the finite budgets; wait or explicitly approve a change.") : action == "backup_wallet" ? tr("Create a full-wallet backup.") : (action == "enable" || action == "start") ? tr("Use Start provider or Resume provider with the saved settings; repeating setup is optional.") : action == "check_external" ? tr("Verify the advertised endpoint from your independent node.") : action == "setup" ? tr("Continue guided setup.") : tr("Inspect the diagnostic; unknown errors are not treated as readiness.");
                     const QString diagnostic = tr("%1: %2").arg(QString::fromStdString(item.find_value("code").get_str()), instruction);
                     if (&item == &diagnostics.getValues().front()) lines.prepend(tr("Next action: %1").arg(diagnostic));
                     else lines << diagnostic;
                 }
                 if (m_operator_restart_required) lines.prepend(tr("Node configuration was saved. Restart explicitly before expecting the new settings to become active."));
                 m_operator_summary->setText(lines.join(QLatin1Char('\n')));
                 renderOperatorPresentation(snapshot);
                 if (!background || capital_changed || !m_overview_finance_age.isValid() || m_overview_finance_age.elapsed() >= 30000)
                     refreshOverviewFinance(background);
             } catch (const std::exception&) {
                 m_status->setText(tr("Provider status is unavailable. Refresh before acting."));
                 m_operator_summary->setText(tr("Operator status unavailable. Refresh before acting; previous readiness is not current."));
                 m_operator_snapshot = UniValue{};
                 m_status_age.invalidate();
                 m_provider_info_snapshot_available = false;
                 m_overview_liquidity_snapshot = UniValue{};
                 clearOverviewCapital();
                 overviewFinanceUnavailable();
                 resetOperatorPresentation();
                 renderCurrentTask();
             }
         }, false, [this, selected](const QString& error) {
             if (selected != m_model) return;
             m_operator_snapshot = UniValue{};
             m_status_age.invalidate();
             m_provider_info_snapshot_available = false;
             m_overview_liquidity_snapshot = UniValue{};
             clearOverviewCapital();
             overviewFinanceUnavailable();
             renderCurrentTask();
             m_status->setText(tr("Provider status is unavailable. Refresh before acting."));
             m_operator_summary->setText(tr("Operator status unknown: %1").arg(error));
             resetOperatorPresentation();
         }, background);
}

void PaymasterWidgetImpl::unlockOperatorWallet(std::function<void()> on_unlocked)
{
    // The reviewed setup may request operating access from its final RPC
    // handler. All resulting calls still use the serialized handler queue.
    if (m_privacy || (m_busy && !(on_unlocked && m_rpc_handler_depth > 0)) || !hasRpcTransport()) return;
    const auto generation = m_wallet_generation;
    call("getpaymasteroperatorinfo", {}, false, nullptr, [this, generation, on_unlocked](const UniValue& current) {
        if (!current.find_value("encrypted").isBool()) return;
        if (!current.find_value("encrypted").isTrue()) {
            if (on_unlocked) on_unlocked();
            else refreshOperatorStatus();
            return;
        }
        // Continuing an already approved operating lease needs no second
        // password. The explicit Unlock action can still replace its mode.
        if (on_unlocked && !current.find_value("provider").find_value("wallet_locked").isTrue() &&
            current.find_value("unlocked_until").isNum() && current.find_value("unlocked_until").getInt<int64_t>() == -1) {
            on_unlocked();
            return;
        }
        QDialog review(this);
        review.setObjectName("paymasterOperatingUnlockDialog");
        review.setWindowTitle(tr("Unlock provider wallet"));
        auto* layout = new QFormLayout(&review);
        auto* explanation = new QLabel(tr("This unlocks the entire selected wallet. The passphrase is not saved. After a node restart or wallet reload, enter it again. Paymaster spending limits remain unchanged."), &review);
        explanation->setWordWrap(true);
        layout->addRow(explanation);
        auto* mode = new QComboBox(&review);
        mode->setObjectName("paymasterOperatingUnlockMode");
        mode->addItem(tr("Continuous operation — until manual lock or node restart"));
        mode->addItem(tr("Timed unlock"));
        layout->addRow(tr("Operating access:"), mode);
        auto* seconds = new QSpinBox(&review);
        seconds->setObjectName("paymasterOperatingUnlockSeconds");
        seconds->setRange(60, 86400);
        seconds->setValue(3600);
        seconds->setSuffix(tr(" seconds"));
        seconds->setEnabled(false);
        connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), &review, [seconds](int index) { seconds->setEnabled(index == 1); });
        layout->addRow(tr("Duration:"), seconds);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &review);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Continue to unlock…"));
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &review, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &review, &QDialog::reject);
        if (review.exec() != QDialog::Accepted || generation != m_wallet_generation || m_privacy) return;
        bool ok{false};
        QString pass = QInputDialog::getText(this, tr("Unlock provider wallet"), tr("Wallet passphrase (not stored)"), QLineEdit::Password, {}, &ok);
        if (!ok || generation != m_wallet_generation || m_privacy) {
            pass.fill(QChar{});
            return;
        }
        UniValue params{UniValue::VARR};
        params.push_back(pass.toStdString());
        params.push_back(mode->currentIndex() == 0 ? 0 : seconds->value());
        params.push_back(mode->currentIndex() == 0);
        pass.fill(QChar{});
        call("walletpassphrase", params, false, nullptr, [this, on_unlocked](const UniValue&) {
            if (on_unlocked) on_unlocked();
            else refreshOperatorStatus();
        });
    });
}

void PaymasterWidgetImpl::stopOperator(bool retire)
{
    if (m_busy || m_privacy || !hasRpcTransport()) return;
    const auto generation = m_wallet_generation;
    setRpcBusyState(true);
    QDialog dialog(this);
    dialog.setObjectName("paymasterStopDialog");
    dialog.setWindowTitle(retire ? tr("Retire Paymaster") : tr("Stop Paymaster"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(tr("Stop this wallet's provider persistently, including autostart and new setup signatures. The node and other wallets keep running. Pause keeps your capital prepared for resuming. Stopping can additionally release the entire pool after a separate review. Wallet, identity and recovery records are retained."), &dialog);
    explanation->setWordWrap(true);
    explanation->setTextFormat(Qt::PlainText);
    layout->addWidget(explanation);
    if (retire) {
        auto* steps = new QLabel(tr("Retirement steps: stop → check unfinished work → separately confirm full capital release → review wallet balances → secure a full-wallet backup. After a confirmed stop, a failed check or cancelled release leaves the provider stopped and the remaining steps incomplete. Cancelling this first dialog changes nothing. There is no automatic retry, transfer, wallet closure or deletion."), &dialog);
        steps->setWordWrap(true);
        layout->addWidget(steps);
    }
    QCheckBox* release{nullptr};
    if (retire) {
        auto* release_notice = new QLabel(tr("Retirement includes reviewing all operating capital for release. You approve the exact release separately in the next step. To stop while keeping reserves, cancel here and choose Stop Paymaster instead."), &dialog);
        release_notice->setObjectName("paymasterRetirementReleaseNotice");
        release_notice->setWordWrap(true);
        layout->addWidget(release_notice);
    } else {
        release = new QCheckBox(tr("Also review release of all operating capital"), &dialog);
        release->setObjectName("paymasterStopReleaseAll");
        release->setChecked(false);
        layout->addWidget(release);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(retire ? tr("Stop and review retirement…") : tr("Stop provider"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    const bool release_all = retire || (release && release->isChecked());
    if (generation != m_wallet_generation) return;
    setRpcBusyState(false);
    if (!accepted || m_privacy || !hasRpcTransport()) return;
    m_retirement_summary->hide();
    m_retirement_balances->clear();
    m_operator_stop_status->show();
    setStatusLabel(m_operator_stop_status, tr("Stopping provider…"), "neutral");
    UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
    options.pushKV("persistent", true);
    options.pushKV("pause_setup", true);
    params.push_back(options);
    call("stoppaymaster", params, false, nullptr, [this, release_all, retire](const UniValue& result) {
        if (m_privacy) return;
        if (!result.find_value("running").isFalse()) {
            setStatusLabel(m_operator_stop_status, tr("Stop not confirmed. No capital release was attempted; refresh before acting."), "error");
            return;
        }
        m_core_enabled = m_core_running = m_autostart_enabled = false;
        m_autostart_dirty = m_autostart_pending_read = false;
        reconcileAutostart();
        if (release_all) reviewFullCapitalRelease(retire);
        else {
            setStatusLabel(m_operator_stop_status, tr("Provider stopped. Capital remains in its pools; signed transactions can still confirm."), "neutral");
            refreshOperatorStatus();
        }
    }, false, [this](const QString& error) {
        if (m_privacy) return;
        setStatusLabel(m_operator_stop_status, tr("Stop not confirmed; no capital was released. %1").arg(error), "error");
    });
}

void PaymasterWidgetImpl::pauseOperator()
{
    if (m_busy || !hasRpcTransport()) return;
    const uint64_t wallet_generation = m_wallet_generation;
    setRpcBusyState(true);
    const auto answer = askPlainTextQuestion(this, tr("Pause provider"), tr("Persistently disable autostart and provider operation, including new pool preparation signatures? Reservations and signed transactions remain saved; signed transactions can still confirm."));
    if (m_wallet_generation != wallet_generation) return;
    setRpcBusyState(false);
    if (answer != QMessageBox::Yes || m_privacy || !hasRpcTransport()) return;
    UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
    options.pushKV("persistent", true);
    options.pushKV("pause_setup", true);
    params.push_back(options);
    call("stoppaymaster", params, false, nullptr, [this](const UniValue& result) {
        if (!result.find_value("running").isBool() || result.find_value("running").get_bool()) {
            showPlainTextWarning(this, tr("Pause not confirmed"), tr("Core did not confirm the pause."));
            return;
        }
        m_core_enabled = false;
        m_core_running = false;
        m_autostart_enabled = false;
        m_autostart_dirty = m_autostart_pending_read = false;
        reconcileAutostart();
        refreshOperatorStatus();
    });
}

void PaymasterWidgetImpl::resumeOperator()
{
    if (m_busy || !hasRpcTransport() || m_autostart_dirty || m_autostart_pending_read) return;
    const uint64_t wallet_generation = m_wallet_generation;
    setRpcBusyState(true);
    const auto answer = askPlainTextQuestion(this, IsExplicitlyStoppedProvider(m_operator_snapshot.find_value("provider")) || m_operator_next_action == QLatin1String("enable") ? tr("Resume provider") : tr("Start provider"), tr("Enable the saved provider configuration and request a start within its existing budgets? Unlock and missing readiness prerequisites remain separate; budgets are not reset."));
    if (m_wallet_generation != wallet_generation) return;
    setRpcBusyState(false);
    if (answer != QMessageBox::Yes || m_privacy || !hasRpcTransport()) return;
    UniValue params{UniValue::VARR};
    params.push_back(true);
    call("setpaymasterenabled", params, false, nullptr, [this](const UniValue& enabled) {
        if (!enabled.find_value("enabled").isTrue()) {
            m_operator_snapshot = UniValue{};
            resetOperatorPresentation();
            refreshOperatorStatus();
            return;
        }
        call("startpaymaster", [] { UniValue options{UniValue::VOBJ}, params{UniValue::VARR}; options.pushKV("wait_for_readiness", true); params.push_back(options); return params; }(), false, nullptr, [this](const UniValue& result) {
            presentProviderStartResult(result);
            refreshOperatorStatus();
        });
    });
}

void PaymasterWidgetImpl::renderCurrentTask()
{
    if (m_activity_tasks) {
        QStringList tasks;
        const auto& operations = m_operation.snapshot.find_value("provider").find_value("active_operations");
        if (!m_privacy && operations.isArray()) for (const auto& item : operations.getValues()) {
            const auto& state = item.find_value("state");
            const QString name = state.isStr() ? QString::fromStdString(state.get_str()) : QStringLiteral("unknown");
            QString task = name == QLatin1String("pending_confirmation") ? tr("Reserve transaction: waiting for confirmation.")
                : name == QLatin1String("pending_creation") ? tr("Reserve preparation: approved work is waiting for its prerequisites.")
                : tr("Reserve operation needs attention. Review the current task and its diagnostic details.");
            const auto& id = item.find_value("operation_id");
            if (id.isStr()) task.prepend(tr("Operation %1: ").arg(QString::fromStdString(id.get_str()).left(12)));
            tasks << task;
        }
        m_activity_tasks->setText(m_privacy ? tr("Hidden by privacy mode") : tasks.isEmpty()
            ? tr("No active reserve preparation is reported.") : tasks.join(QLatin1Char('\n')));
    }

    using Phase = PaymasterOperationController::Phase;
    if (!m_task_card) return;
    m_task_card->setTitle(m_operation.phase == Phase::Complete ? tr("Last completed management task") : tr("Provider management task"));
    const bool details_available = !m_privacy && m_operation.phase == Phase::Blocked && !m_operation.error.isEmpty();
    m_task_error_details->setText(details_available ? m_operation.error : QString{});
    m_task_error_toggle->setVisible(details_available);
    m_task_error_details->setVisible(details_available && m_task_error_toggle->isChecked());
    const bool available = !m_privacy && hasRpcTransport() && hasCompleteMutationSnapshots() && !m_busy && !m_operation.busy();
    m_restore_task->setEnabled(available && m_operation.phase != Phase::Waiting);
    m_restore_task->setVisible(m_operator_next_action != QLatin1String("review_liquidity"));
    const bool enough_earnings = m_carrier_excess_cents >= Params().GetDigiDollarParams().minOutputAmount;
    updateEarningsWithdrawalHint();
    m_withdraw_task->setEnabled(available && enough_earnings && m_operation.phase != Phase::Waiting);
    m_release_task->setEnabled(available && m_pool_operational_carriers > 0 && m_operation.phase != Phase::Waiting);
    m_task_card->setVisible(!m_privacy && m_operation.phase != Phase::Idle);
    m_cancel_maintenance->setVisible(!m_privacy && !cancellableMaintenance().empty());
    m_cancel_maintenance->setEnabled(available);
    const bool needs_reserves = m_operation.error_code == QLatin1String("PAYMASTER_POOL_PREPARATION_REQUIRED");
    m_task_continue->setVisible(m_operation.phase == Phase::Blocked && !canReviewPreparationFee() &&
        !(needs_reserves && m_operator_next_action == QLatin1String("review_liquidity")));
    m_task_continue->setEnabled(!m_busy && !m_privacy);
    m_task_continue->setText(m_operation.error_code.contains(QLatin1String("WAITING_FUNDS")) ||
        m_operation.error_code.contains(QLatin1String("WAITING_DGB")) ? tr("View reserve funding and next steps") :
        canReviewReserveReduction() ? tr("Recalculate fees and review release…") :
        (needs_reserves || m_operation.error_code == QLatin1String("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL") ||
        m_operation.error_code == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) ? tr("Restore required reserves…") :
        m_operation.error_code.contains(QLatin1String("WALLET_LOCKED")) ? tr("Unlock wallet…") :
        m_operation.error_code == QLatin1String("PAYMASTER_MAINTENANCE_FEE_EXCEEDED") ? tr("Review refill cost limit") :
        m_operation.error_code == QLatin1String("PAYMASTER_RETIREMENT_WALLET_FEE_LIMIT") ? tr("Check current status") :
        m_operation.error_code.contains(QLatin1String("FEE_LIMIT")) ? tr("Review fee limit…") :
        m_operation.error_code == QLatin1String("PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED") ? tr("Review refill settings…") :
        m_operation.error_code.contains(QLatin1String("PROVIDER_DISABLED")) ? tr("Resume approved task…") :
        carrierReleaseNeedsReview() ? tr("Refresh reserves") : tr("Check current status"));
    QString message;
    switch (m_operation.phase) {
    case Phase::Checking: message = m_guided_waiting_for_provider
        ? tr("Waiting briefly for the current provider task. This release has not started.")
        : tr("1/5 · Checking the current wallet and preparing the review…"); break;
    case Phase::Review: message = tr("2/5 · Review the amounts and limits. Nothing has been authorized yet."); break;
    case Phase::Executing: message = m_guided_waiting_for_provider
        ? tr("Waiting briefly for the current provider task. This release has not started. Core will recheck the same approved plan and fee limit.")
        : tr("3/5 · Applying the approved task. Please wait…"); break;
    case Phase::Waiting:
        message = maintenanceWaitingForFunds() && m_operation.required_confirmations == 0
            ? readinessExplanation(m_last_service_error)
            : m_operation.required_confirmations > 0
            ? tr("4/5 · Waiting for confirmations: %1/%2. Continuation is automatic.")
                .arg(m_operation.completed_confirmations).arg(m_operation.required_confirmations)
            : m_operation.snapshot.find_value("provider").find_value("start_requested").isTrue()
                ? tr("Waiting for provider readiness. The requested start will continue automatically.")
                : tr("A saved task has not produced a transaction yet. No confirmation is pending. Core will check whether the task is still needed.");
        break;
    case Phase::Complete: message = tr("✓ 5/5 · Task complete."); break;
    case Phase::Blocked:
        if (canReviewReserveReduction()) {
            message = m_operation.error_code == QLatin1String("PAYMASTER_RETIREMENT_FEE_LIMIT")
                ? tr("No reserves were released by this attempt. Its one-time fee limit was too low. Recalculate the release fees and approve a fresh preview. Changing automatic refill limits does not change release fees.")
                : tr("No reserves were released by this attempt. Another provider task was busy, or the reviewed reserve plan changed. Review a fresh release plan when the current task has finished.");
            break;
        }
        if (m_operation.error_code == QLatin1String("PAYMASTER_RETIREMENT_WALLET_FEE_LIMIT")) {
            message = tr("No reserves were released by this attempt. The estimated fee exceeds the wallet-wide transaction limit. Review that limit or wait for lower fees; increasing refill limits cannot solve this.");
            break;
        }
        if (needs_reserves) {
            message = tr("The start is waiting for missing reserves. No reserve transaction is pending. Choose 'Restore reserves and start' to review the capital and fees; after approval, preparation and start continue automatically.");
            break;
        }
        message = canReviewPreparationFee()
            ? tr("Next: choose ‘Review setup fee and continue’ above. Review a new fee ceiling and then approve the replacement plan. Nothing will be spent without your approval.")
            : m_operation.error_code.contains(QLatin1String("FEE_LIMIT"))
            ? tr("The approved fee limit is insufficient. Review it; no higher fee is authorized automatically.")
            : m_operation.error_code == QLatin1String("PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED")
                ? tr("Automatic refill is off. Cancel the uncreated reserve work, or review refill settings to resume it. No transaction is pending for the uncreated steps.")
            : m_operation.error_code.contains(QLatin1String("PROVIDER_DISABLED"))
                ? tr("This task is paused. Resume the approved task to allow the remaining steps within the saved limits.")
            : m_operation.error_code.contains(QLatin1String("WAITING_FUNDS")) || m_operation.error_code.contains(QLatin1String("WAITING_DGB"))
                ? tr("Waiting for confirmed wallet funding. Core continues automatically when the required funds are available.")
                : readinessExplanation(m_operation.error);
        if (message.isEmpty() || message == m_operation.error) message = tr("The task needs attention. Check the explanation below before retrying.");
        if (m_operation.error.startsWith(QLatin1String("PAYMASTER_")) && message.contains(m_operation.error))
            message = tr("The task could not continue. Check current status before trying again. Technical details are available below.");
        else if (!m_operation.error.startsWith(QLatin1String("PAYMASTER_")))
            message = m_operation.error;
        break;
    case Phase::Idle: break;
    }
    const auto& work = m_operation.snapshot.find_value("provider").find_value("active_operations");
    if ((m_operation.phase == Phase::Blocked || m_operation.phase == Phase::Waiting) &&
        (m_operation.error_code.contains(QLatin1String("WAITING_FUNDS")) ||
         m_operation.error_code.contains(QLatin1String("WAITING_DGB")))) {
        const QString preparation = formatPoolPreparation(
            m_operation.snapshot.find_value("provider").find_value("preparation"), PreparationDetail::Summary);
        if (!preparation.isEmpty()) message = preparation;
    }
    const bool withdrawing = work.isArray() && !work.empty() && work[0].find_value("kind").isStr() &&
        work[0].find_value("kind").get_str() == "withdrawal";
    QString saved_title = withdrawing ? tr("Withdraw earnings") : needs_reserves ? tr("Start provider") : tr("Maintaining reserves");
    if (work.isArray() && work.size() == 1 && !withdrawing) {
        const QString kind = activityText(work[0], "kind");
        const QString asset = activityText(work[0], "asset");
        if (kind == QLatin1String("preparation"))
            saved_title = asset == QLatin1String("dgb") ? tr("Prepare DGB reserves") :
                asset == QLatin1String("dd_carrier") ? tr("Prepare DD reserves") : tr("Prepare reserves");
        else if (kind == QLatin1String("maintenance"))
            saved_title = asset == QLatin1String("dgb") ? tr("Refill DGB reserves") :
                asset == QLatin1String("dd_carrier") ? tr("Refill DD reserves") : tr("Refill reserves");
    }
    const QString title = !m_operation.busy() && work.isArray() && work.size() > 1 ? tr("Reserve management") :
        m_operation.title.isEmpty() ? saved_title : m_operation.title;
    if (work.isArray() && work.size() > 1) {
        message += tr("\nCombined progress of %1 reserve operations; individual transactions are listed in Activity.").arg(work.size());
    }
    m_task_status->setText(title + QStringLiteral("\n") + message);
    m_task_progress->setVisible(m_operation.phase == Phase::Checking || m_operation.phase == Phase::Executing ||
        (m_operation.phase == Phase::Waiting && (!maintenanceWaitingForFunds() || m_operation.required_confirmations > 0)));
    if (m_operation.phase == Phase::Waiting && m_operation.required_confirmations > 0) {
        m_task_progress->setRange(0, m_operation.required_confirmations);
        m_task_progress->setValue(m_operation.completed_confirmations);
    } else {
        m_task_progress->setRange(0, 0);
    }
    updateAutomaticRefreshTimer();
}

void PaymasterWidgetImpl::requestReviewedStart()
{
    UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
    options.pushKV("wait_for_readiness", true);
    params.push_back(options);
    call("startpaymaster", params, false, nullptr, [this](const UniValue& result) {
        if (!result.find_value("running").isTrue() && !result.find_value("start_requested").isTrue()) {
            failGuidedTask(tr("Core did not accept the provider start. Review the current status."));
            return;
        }
        refreshStatus();
    }, false, [this](const QString& error) { failGuidedTask(error); });
}

void PaymasterWidgetImpl::startProvider()
{
    if (!canStartProvider()) return;
    if (!requirePrivacyOffForSensitiveAction(
            tr("Starting the Paymaster provider"))) {
        return;
    }
    if (!hasCompleteMutationSnapshots()) {
        m_status->setText(tr(
            "Provider start was not attempted because a complete current provider, safety and liquidity snapshot is unavailable."));
        updateProviderButtons();
        return;
    }
    const uint64_t wallet_generation = m_wallet_generation;
    if (askPlainTextQuestion(
            this, tr("Start provider"),
            providerStartConfirmationText()) != QMessageBox::Yes) {
        return;
    }
    if (m_wallet_generation != wallet_generation || m_privacy || !canStartProvider()) return;
    call("startpaymaster", [] { UniValue options{UniValue::VOBJ}, params{UniValue::VARR}; options.pushKV("wait_for_readiness", true); params.push_back(options); return params; }(), false, nullptr,
         [this](const UniValue& result) {
             presentProviderStartResult(result);
             refreshStatus();
         });
}

QSpinBox* PaymasterWidgetImpl::spin(QWidget* parent, int minimum, int maximum, int value)
{
    auto* result = new NoWheelSpinBox(parent);
    result->setRange(minimum, maximum);
    result->setValue(value);
    return result;
}

QLineEdit* PaymasterWidgetImpl::dgbAmountField(QWidget* parent, qint64 satoshis)
{
    return new DgbAmountLineEdit(satoshis, parent);
}

QSpinBox* PaymasterWidgetImpl::scaledSpin(QWidget* parent, int minimum, int maximum,
                            int value, int scale, int decimals)
{
    auto* result = new NoWheelScaledSpinBox(scale, decimals, parent);
    result->setRange(minimum, maximum);
    result->setValue(value);
    return result;
}

QString PaymasterWidgetImpl::yesNo(bool value) const
{ return value ? tr("yes") : tr("no"); }

QString PaymasterWidgetImpl::activityText(const UniValue& object, const char* name)
{
    const UniValue& value = object.find_value(name);
    return value.isStr() ? QString::fromStdString(value.get_str()) : QString{};
}

QString PaymasterWidgetImpl::reservationPurpose(const UniValue& entry) const
{
    const QString purpose = activityText(entry, "purpose");
    if (purpose == QLatin1String("admission")) return tr("admission reserve");
    if (purpose == QLatin1String("operational")) return tr("operational payment slot");
    return purpose.isEmpty() ? tr("unknown purpose") : purpose;
}

QString PaymasterWidgetImpl::reservationAsset(const UniValue& entry) const
{
    const QString asset = activityText(entry, "asset");
    if (asset == QLatin1String("dgb")) {
        return tr("%1 DGB").arg(PaymasterFormatDGB(poolNumber(entry, "dgb_satoshis")));
    }
    if (asset == QLatin1String("dd_carrier")) {
        return tr("%1 DD carrier").arg(PaymasterFormatDD(poolNumber(entry, "dd_cents")));
    }
    return asset.isEmpty() ? tr("unknown asset") : asset;
}

QString PaymasterWidgetImpl::reservationState(const UniValue& entry) const
{
    const QString state = activityText(entry, "state");
    if (state == QLatin1String("reserved")) return tr("reserved for in-progress work");
    if (state == QLatin1String("pending_successor")) return tr("waiting for a replacement output");
    if (state == QLatin1String("committed")) return tr("committed to an exact payment");
    if (state == QLatin1String("spent")) return tr("spent and retained as durable history");
    return state.isEmpty() ? tr("unknown") : state;
}

void PaymasterWidgetImpl::showReservations(const UniValue& result)
{
    m_activity_output->setPlainText(QString::fromStdString(result.write(2)));
    m_activity_reservations->setRowCount(0);
    if (!result.isArray()) {
        if (m_reservations_group) m_reservations_group->setVisible(true);
        if (m_recovery_group) m_recovery_group->setVisible(true);
        m_activity_summary->setText(tr(
            "The wallet returned an unexpected reservation result. See the technical result."));
        return;
    }
    const auto& entries = result.getValues();
    if (entries.empty()) {
        if (m_reservations_group) m_reservations_group->setVisible(false);
        if (m_recovery_group) m_recovery_group->setVisible(false);
        m_activity_summary->setText(tr(
            "Healthy · no reservations or recovery actions require attention."));
        return;
    }
    if (m_reservations_group) m_reservations_group->setVisible(true);
    if (m_recovery_group) m_recovery_group->setVisible(true);
    QStringList lines;
    lines.push_back(tr(
        "%1 unavailable or durable pool record(s). Core controls when each output can safely return to the available pool.")
                        .arg(static_cast<qulonglong>(entries.size())));
    m_activity_reservations->setRowCount(static_cast<int>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const QStringList cells{reservationPurpose(entry), reservationAsset(entry), reservationState(entry),
            tr("Core monitors this output; review the current task if action is required.")};
        for (int column = 0; column < cells.size(); ++column)
            m_activity_reservations->setItem(static_cast<int>(i), column, new QTableWidgetItem(cells[column]));
    }
    m_activity_reservations->resizeRowsToContents();
    m_activity_summary->setText(lines.join('\n'));
}

QString PaymasterWidgetImpl::formatActivityResult(const UniValue& result, bool submitted_payment) const
{
    if (!result.isObject()) {
        return tr("Core returned an unexpected processing result. See the technical result.");
    }
    const UniValue& processed_value = result.find_value("processed");
    const bool processed = processed_value.isBool() && processed_value.get_bool();
    if (!processed) {
        return submitted_payment
            ? tr("No submitted payment is waiting in this provider wallet.")
            : tr("No capacity, quote, or recovery request is waiting in this provider wallet.");
    }

    const QString type = activityText(result, "message_type");
    QString type_description;
    if (type == QLatin1String("capacity")) {
        type_description = tr("capacity check");
    } else if (type == QLatin1String("quote")) {
        type_description = tr("quote request");
    } else if (type == QLatin1String("recovery_request")) {
        type_description = tr("recovery-provider request");
    } else if (type == QLatin1String("recovery_submit")) {
        type_description = tr("recovery submission");
    } else if (type == QLatin1String("submit")) {
        type_description = tr("submitted payment");
    } else {
        type_description = type.isEmpty() ? tr("provider message") : type;
    }

    const UniValue& queued_value = result.find_value("queued");
    const bool queued = queued_value.isBool() && queued_value.get_bool();
    QStringList lines;
    lines.push_back(tr("Processed one %1.").arg(type_description));
    lines.push_back(queued
        ? tr("The signed provider response was queued for delivery to the client.")
        : tr("The provider response was not queued. Check the technical result before retrying."));
    const QString result_status = activityText(result, "result_status");
    const QString attempt_state = activityText(result, "attempt_state");
    const QString rejection_code = activityText(result, "rejection_code");
    const QString txid = activityText(result, "txid");
    if (!result_status.isEmpty()) {
        lines.push_back(tr("Result status: %1").arg(result_status));
    }
    if (!attempt_state.isEmpty()) {
        lines.push_back(tr("Payment state: %1").arg(attempt_state));
    }
    if (!rejection_code.isEmpty()) {
        lines.push_back(tr("Rejected safely: %1").arg(rejection_code));
    }
    if (!txid.isEmpty()) {
        lines.push_back(tr("Transaction: %1").arg(txid));
    }
    if (!submitted_payment) {
        lines.push_back(tr(
            "If more clients are waiting, process another request. A quote may now hold a reservation."));
    }
    return lines.join('\n');
}

void PaymasterWidgetImpl::refreshActivity()
{
    call("listpaymasterreservations", {}, false, m_activity_output,
         [this](const UniValue& result) {
             showReservations(result);
             refreshStatus();
         });
}

void PaymasterWidgetImpl::processActivityRequest()
{
    if (!requirePrivacyOffForSensitiveAction(
            tr("Processing Paymaster requests"))) {
        return;
    }
    if (!hasCompleteMutationSnapshots() || !m_core_running ||
        m_core_locked) {
        m_activity_action_result->setText(tr(
            "No request was processed because the current provider snapshots or unlocked runtime state are incomplete."));
        updateProviderButtons();
        return;
    }
    call("processpaymasterrequests", {}, true, m_activity_output,
         [this](const UniValue& result) {
             m_activity_action_result->setText(
                 formatActivityResult(result, /*submitted_payment=*/false));
             refreshStatus();
         });
}

void PaymasterWidgetImpl::processActivitySubmit()
{
    if (!requirePrivacyOffForSensitiveAction(
            tr("Processing Paymaster submissions"))) {
        return;
    }
    if (m_busy || !hasRpcTransport() || !hasCompleteMutationSnapshots() || !m_core_running ||
        m_core_locked) {
        m_activity_action_result->setText(tr(
            "No submission was processed because the current provider snapshots or unlocked runtime state are incomplete."));
        updateProviderButtons();
        return;
    }
    const auto generation = m_wallet_generation;
    setRpcBusyState(true);
    const auto answer = askPlainTextQuestion(
            this, tr("Process one submitted Paymaster payment?"),
            tr("Core will validate one waiting client-signed submission. If it exactly "
               "matches the durable authorization, this action may sign provider inputs, "
               "spend reserved DGB within your safety policy, and broadcast the transaction.\n\n"
               "Continue?"));
    if (generation != m_wallet_generation) return;
    setRpcBusyState(false);
    if (answer != QMessageBox::Yes || m_privacy || !hasRpcTransport() ||
        !hasCompleteMutationSnapshots() || !m_core_running || m_core_locked) return;
    call("processpaymastersubmits", {}, true, m_activity_output,
         [this](const UniValue& result) {
             m_activity_action_result->setText(
                 formatActivityResult(result, /*submitted_payment=*/true));
             refreshStatus();
         });
}

void PaymasterWidgetImpl::setRpcBusyState(bool busy)
{
    m_busy = busy;
    // Keep pages paintable while asynchronous work is pending. Disabling
    // updates across an RPC loses the visible page after navigation,
    // resizing or uncovering the window. Command gating is independent.
    // An ancestor gate covers every page action, including locally owned
    // buttons without an individual busy predicate. Child refresh handlers
    // cannot accidentally re-enable actions while the RPC chain is active.
    // Review dialogs are separate windows parented to this widget, not tabs.
    m_tabs->setEnabled(true);
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto* page = m_tabs->widget(index);
        page->setEnabled(!busy && (!page->property("paymasterPageAvailable").isValid() ||
                                  page->property("paymasterPageAvailable").toBool()));
    }
    m_navigation->setEnabled(true);
    m_navigation_select->setEnabled(true);
    m_task_card->setEnabled(!busy);
    m_operator_action->setEnabled(!m_privacy && hasRpcTransport() && !busy);
    m_operator_start->setEnabled(!m_privacy && hasRpcTransport() && !busy && m_operator_start_allowed);
    m_operator_pause->setEnabled(!m_privacy && hasRpcTransport() && !busy);
    const bool can_stop = !m_privacy && hasRpcTransport() && !busy &&
        IsHex256Field(m_operator_snapshot.find_value("provider"), "provider_id");
    m_operator_stop->setEnabled(can_stop);
    m_operator_retire->setEnabled(can_stop);

    // Stop automatic refresh before an unlock dialog or RPC can enter a
    // nested event loop. Mutation controls remain disabled until every
    // result/error handler and any modal review it opens have returned.
    updateAutomaticRefreshTimer();
    setPolicyMutationEnabled(m_policy_snapshot_representable);
    setProviderSafetyMutationEnabled(
        m_provider_safety_snapshot_representable);
    setLiquidityPolicyMutationEnabled(
        m_liquidity_snapshot_representable);
    if (m_client_safety_group) {
        m_client_safety_group->setEnabled(
            !m_busy && m_client_safety_snapshot_representable);
    }
    if (m_create_identity) {
        m_create_identity->setEnabled(
            !m_busy && !m_core_has_identity &&
            m_display_name->hasAcceptableInput());
    }
    for (QPushButton* setup_action : {
             m_guided_setup, m_expert_setup, m_reopen_wizard}) {
        if (setup_action) setup_action->setEnabled(!m_busy);
    }
    updateProviderButtons();
    updateLiquidityDisplay();
    updateCarrierWithdrawalButtons();
    updateFinanceControls();
    updateBackupReminder(m_backup_required, m_provider_id);
    if (!m_operator_snapshot.isNull() && !m_privacy) renderOperatorPresentation(m_operator_snapshot);
    else resetOperatorPresentation();
    renderCurrentTask();
    if (m_model && m_model->digiDollarHistoryRefreshInProgress()) {
        for (int index = 0; index < m_tabs->count(); ++index) m_tabs->widget(index)->setEnabled(false);
        showOperatorBusyState();
    }
}

void PaymasterWidgetImpl::showPendingOperatorReview()
{
    if (!m_pending_operator_review) return;
    const auto generation = m_wallet_generation;
    QTimer::singleShot(0, this, [this, generation] {
        // A refresh can start before this event is delivered. Keep the
        // review until that RPC chain finishes instead of losing it.
        if (generation != m_wallet_generation || m_busy || m_rpc_active) return;
        auto review = std::move(m_pending_operator_review);
        m_pending_operator_review = {};
        if (review) review();
    });
}

void PaymasterWidgetImpl::finishRpcCall(uint64_t wallet_generation,
                   QPointer<QPlainTextEdit> output,
                   ResultHandler handler, bool show_error,
                   ErrorHandler error_handler, UniValue result,
                   QString error)
{
    PaymasterQt::RpcError failure;
    failure.failed = !error.isEmpty();
    failure.message = std::move(error);
    finishRpcCall(wallet_generation, output, std::move(handler), show_error,
                  std::move(error_handler), std::move(result), std::move(failure));
}

void PaymasterWidgetImpl::finishRpcCall(uint64_t wallet_generation,
                   QPointer<QPlainTextEdit> output,
                   ResultHandler handler, bool show_error,
                   ErrorHandler error_handler, UniValue result,
                   PaymasterQt::RpcError failure)
{
    if (m_wallet_generation != wallet_generation) return;

    QPointer<PaymasterWidgetImpl> self{this};
    auto previous_failure = std::move(m_rpc_failure);
    m_rpc_failure = std::move(failure);
    const QString error = m_rpc_failure.message;
    uint64_t handler_token = ++m_next_rpc_handler_token;
    if (handler_token == 0) handler_token = ++m_next_rpc_handler_token;
    const uint64_t previous_token = m_active_rpc_handler_token;
    ++m_rpc_handler_depth;
    m_active_rpc_handler_token = handler_token;

    if (m_rpc_failure.failed) {
        const QString visible_error = privacySafeBackendError(error);
        if (output) output->setPlainText(visible_error);
        if (error_handler) {
            error_handler(visible_error);
        } else if (show_error) {
            showPlainTextWarning(
                this, tr("Paymaster operation failed"), visible_error);
        } else {
            m_status->setText(
                tr("Provider status unavailable: %1")
                    .arg(visible_error));
        }
    } else {
        if (output) {
            output->setPlainText(QString::fromStdString(result.write(2)));
        }
        if (handler) {
            handler(result);
        } else {
            refreshStatus();
        }
    }

    // A wallet switch clears the handler and pending-call state. Do not
    // decrement or dispatch against the newly selected wallet.
    if (!self || m_wallet_generation != wallet_generation) return;
    m_rpc_failure = std::move(previous_failure);
    m_active_rpc_handler_token = previous_token;
    --m_rpc_handler_depth;
    redactVisiblePrivacyText();

    if (m_rpc_handler_depth == 0 && !m_pending_handler_calls.empty()) {
        PendingRpcCall next =
            std::move(m_pending_handler_calls.front());
        m_pending_handler_calls.pop_front();
        dispatchRpcCall(std::move(next));
        return;
    }
    if (m_rpc_handler_depth == 0) {
        m_active_rpc_command.clear();
        m_rpc_active = false;
        m_active_rpc_background = false;
        if (m_rpc_chain_foreground) setRpcBusyState(false);
        m_rpc_chain_foreground = false;
        showOperatorBusyState();
        if (m_history_deferred_status && m_model && !m_model->digiDollarHistoryRefreshInProgress()) {
            m_history_deferred_status = false;
            QTimer::singleShot(0, this, [this] { refreshStatus(); });
        }
        showPendingOperatorReview();
    }
}

void PaymasterWidgetImpl::dispatchRpcCall(PendingRpcCall request)
{
    const uint64_t wallet_generation = m_wallet_generation;
    // A scan can begin after a read queued its next action. Recheck at
    // dispatch as well as at the page/command boundary. Do not replay a
    // delayed approval automatically when the history read completes.
    if (m_model && m_model->digiDollarHistoryRefreshInProgress()) {
        m_history_deferred_status = true;
        finishRpcCall(wallet_generation, request.output, std::move(request.handler), request.show_error,
                      std::move(request.error_handler), UniValue{},
                      tr("The full DigiDollar history check is still running. This Paymaster operation was not started; review it again after the check finishes."));
        return;
    }
    // Elapsed time belongs to this read, not to an earlier slow step in
    // the serialized chain. A fresh finance read must not inherit loading.
    m_operator_read_elapsed.invalidate();
    m_active_rpc_command = request.command;
    m_active_rpc_background = request.background;
    if (!request.background && !m_rpc_chain_foreground) {
        m_rpc_chain_foreground = true;
        setRpcBusyState(true);
    }
    showOperatorBusyState();

    if (m_async_rpc_executor_for_testing) {
        QPointer<PaymasterWidgetImpl> guard{this};
        m_async_rpc_executor_for_testing(request.command, request.params,
                                         [guard, wallet_generation, output = request.output,
                                          handler = std::move(request.handler), show_error = request.show_error,
                                          error_handler = std::move(request.error_handler)](UniValue result, QString error) mutable {
                                             if (!guard) return;
                                             guard->finishRpcCall(wallet_generation, output, std::move(handler), show_error,
                                                                  std::move(error_handler), std::move(result), std::move(error));
                                         });
        return;
    }

    // Widget tests use a synchronous executor so each user action and its
    // resulting state can be asserted deterministically. Follow-up calls
    // still pass through the same busy/handler-token lifecycle.
    if (m_rpc_executor_for_testing) {
        UniValue result;
        PaymasterQt::RpcError error;
        try {
            result = m_rpc_executor_for_testing(request.command,
                                                request.params);
        } catch (const UniValue& rpc_error) {
            error = PaymasterQt::RpcError::fromRpc(rpc_error);
        } catch (const std::exception& exception) {
            error.failed = true;
            error.message = QString::fromUtf8(exception.what());
        } catch (...) {
            error.failed = true;
            error.message = tr("Unknown RPC error");
        }
        finishRpcCall(wallet_generation, request.output,
                      std::move(request.handler), request.show_error,
                      std::move(request.error_handler),
                      std::move(result), std::move(error));
        return;
    }

    WalletModel* request_model = m_model;
    if (!request_model) {
        finishRpcCall(
            wallet_generation, request.output,
            std::move(request.handler), request.show_error,
            std::move(request.error_handler), UniValue{},
            tr("No wallet is selected, so the Paymaster RPC was not started."));
        return;
    }

    QPointer<PaymasterWidgetImpl> guard{this};
    WalletModel::DetailedRpcCallback callback =
        [guard, request_model, wallet_generation,
         output = request.output, handler = std::move(request.handler),
         show_error = request.show_error,
         error_handler = std::move(request.error_handler)](
            UniValue result, PaymasterQt::RpcError error) mutable {
            if (!guard || guard->m_model != request_model ||
                guard->m_wallet_generation != wallet_generation) {
                return;
            }
            if (PaymasterQt::StableCode(error) == QLatin1String("PAYMASTER_WALLET_UNLOCK_CANCELLED"))
                error.message = tr("The wallet was not unlocked, so the signing operation was not started.");
            guard->finishRpcCall(
                wallet_generation, output, std::move(handler),
                show_error, std::move(error_handler),
                std::move(result), std::move(error));
        };
    if (request.needs_unlock) {
        PaymasterQt::ExecuteSigningRpcAsyncDetailed(*request_model, std::move(request.command), std::move(request.params),
            [guard, request_model, wallet_generation] {
                return guard && guard->m_model == request_model && guard->m_wallet_generation == wallet_generation;
            }, std::move(callback));
    } else {
        request_model->executeRpcAsyncDetailed(std::move(request.command), std::move(request.params), std::move(callback));
    }
}

void PaymasterWidgetImpl::call(std::string command, UniValue params, bool needs_unlock,
          QPlainTextEdit* output, ResultHandler handler,
          bool show_error, ErrorHandler error_handler,
          bool background)
{
    const auto report_not_started =
        [this, output, show_error,
         &error_handler](const QString& error) {
            if (output) output->setPlainText(error);
            if (error_handler) {
                error_handler(error);
            } else if (show_error) {
                showPlainTextWarning(
                    this, tr("Paymaster operation not started"), error);
            } else {
                m_status->setText(error);
            }
        };
    if (!hasRpcTransport()) {
        report_not_started(tr(
            "No wallet is selected, so the Paymaster RPC was not started."));
        return;
    }
    if (m_model && m_model->digiDollarHistoryRefreshInProgress()) {
        report_not_started(tr("The full DigiDollar history check is still running. No new Paymaster wallet operation was started. Wait for the check to finish."));
        m_history_deferred_status = true;
        showOperatorBusyState();
        return;
    }
    if (params.isNull()) params = UniValue{UniValue::VARR};

    PendingRpcCall request;
    request.command = std::move(command);
    request.params = std::move(params);
    request.needs_unlock = needs_unlock;
    request.output = output;
    request.handler = handler;
    request.show_error = show_error;
    request.error_handler = error_handler;
    // Only these explicitly read-only, wallet-bound observations may run
    // without disabling pages. Their continuations remain serialized.
    request.background = background && !needs_unlock &&
        (request.command == "getpaymasteroperatorinfo" ||
         request.command == "getpaymasterfinancestatus");

    if (m_busy || m_rpc_active) {
        QWidget* const modal = QApplication::activeModalWidget();
        const bool only_setup_wizard_is_modal =
            !modal || modal == m_setup_wizard.data();
        if (m_rpc_handler_depth > 0 &&
            m_active_rpc_handler_token != 0 &&
            only_setup_wizard_is_modal &&
            m_pending_handler_calls.size() < 32) {
            m_pending_handler_calls.push_back(std::move(request));
            return;
        }
        // A deliberate user action can follow a passive observation. Gate
        // further actions now, then dispatch it once the read completes;
        // Core still validates the exact command and any reviewed plan.
        if (m_rpc_active && m_active_rpc_background && !m_busy &&
            !request.background && m_pending_handler_calls.empty()) {
            m_pending_handler_calls.push_back(std::move(request));
            m_rpc_chain_foreground = true;
            setRpcBusyState(true);
            return;
        }
        report_not_started(tr(
            "Another Paymaster wallet operation is still running, so this RPC was not started."));
        return;
    }
    m_active_rpc_command = request.command;
    m_rpc_active = true;
    m_rpc_chain_foreground = !request.background;
    if (m_rpc_chain_foreground) setRpcBusyState(true);
    dispatchRpcCall(std::move(request));
}
