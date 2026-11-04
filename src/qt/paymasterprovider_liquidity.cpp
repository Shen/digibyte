// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

bool ProviderLiquidity::carrierReleaseNeedsReview() const
{
    return m_operation.error_code == QLatin1String("PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE") ||
        m_operation.error_code == QLatin1String("PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED") ||
        m_operation.error_code == QLatin1String("PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO");
}

bool ProviderLiquidity::canReviewReserveReduction() const
{
    return m_failed_guided_task == QLatin1String("reduce_reserves") &&
        (m_operation.error_code == QLatin1String("PAYMASTER_RETIREMENT_FEE_LIMIT") ||
         m_operation.error_code == QLatin1String("PAYMASTER_PROVIDER_BUSY") ||
         m_operation.error_code == QLatin1String("PAYMASTER_POOL_PLAN_CHANGED") ||
         m_operation.error_code == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_CHANGED"));
}

bool ProviderLiquidity::waitForReserveReduction(const QString& error, std::function<void()> retry)
{
    // Only this exact Core rejection proves no transaction was started.
    // Never repeat an execution after a missing or ambiguous response.
    if (m_guided_task != QLatin1String("reduce_reserves") ||
        PaymasterQt::StableCode(m_panel.rpcFailure()) != QLatin1String("PAYMASTER_PROVIDER_BUSY") ||
        PaymasterQt::Disposition(m_panel.rpcFailure()) != QLatin1String("retry_before_execution") || m_guided_busy_retries >= 3) return false;
    ++m_guided_busy_retries;
    m_guided_waiting_for_provider = true;
    m_panel.overview().renderCurrentTask();
    const auto generation = m_panel.walletGeneration();
    const auto task_generation = m_guided_generation;
    const auto phase = m_operation.phase;
    QTimer::singleShot(750, widget(), [this, generation, task_generation, phase, retry] {
        if (generation != m_panel.walletGeneration() || task_generation != m_guided_generation || m_panel.privacyEnabled() ||
            m_guided_task != QLatin1String("reduce_reserves") || m_operation.phase != phase) return;
        m_guided_waiting_for_provider = false;
        retry();
    });
    return true;
}

void ProviderLiquidity::observeOperation(const UniValue& status)
{
    m_operation.observe(status, m_panel.walletGeneration(), QDateTime::currentSecsSinceEpoch());
}

void ProviderLiquidity::resetOperation()
{
    m_operation.reset(m_panel.walletGeneration());
    if (m_cancel_preparation) m_cancel_preparation->hide();
}

void ProviderLiquidity::acceptSetupOperation()
{
    m_operation.title = tr("Set up provider");
    m_operation.generation = m_panel.walletGeneration();
    m_operation.accepted();
}

void ProviderLiquidity::closeWalletDialogs()
{
    if (m_refill_approval) m_refill_approval->reject();
}

void ProviderLiquidity::resetWalletPresentation()
{
    resetOperation();
    m_guided_task.clear();
    m_failed_guided_task.clear();
    restoreLiquidityDefaults(/*announce=*/false);
    m_saved_liquidity_policy = UniValue{};
    m_refill_save_pending = false;
    m_refill_message.clear();
    m_liquidity_snapshot_available = false;
    m_liquidity_output->clear();
    m_pool_preparation = UniValue{};
    m_pool_preparation_status->clear();
    m_pool_preparation_status->hide();
    m_preparation_fee->setSatoshis(DEFAULT_PAYMASTER_PREPARATION_FEE);
    m_overview_liquidity_snapshot = UniValue{};
    m_pool_admission_dgb = 0;
    m_pool_admission_carriers = 0;
    m_pool_operational_dgb = 0;
    m_pool_operational_carriers = 0;
    m_pool_status_loaded = false;
    m_liquidity_policy_configured = false;
    m_liquidity_snapshot_representable = true;
    m_unrepresentable_liquidity_snapshot = UniValue{UniValue::VOBJ};
    setLiquidityPolicyMutationEnabled(true);
    m_liquidity_targets_satisfy_provider_policy = false;
    m_liquidity_policy_dirty = false;
    m_loading_liquidity_policy = false;
    m_maintenance_state = QStringLiteral("unavailable");
    m_pending_successor_carrier_cents = 0;
    m_pending_successor_dgb_satoshis = 0;
    m_liquidity_current_status->setText(tr("Current confirmed pool: status not loaded yet."));
    m_liquidity_policy_status->setText(tr("Liquidity policy has not been loaded yet."));
    m_liquidity_slot_status->setText(tr("Ready, pending and missing target counts have not been loaded yet."));
    m_liquidity_recycling_status->setText(tr("No recyclable successor information is available yet."));
    m_liquidity_budget_status->setText(tr("Maintenance budget use has not been loaded yet."));
    m_liquidity_maintenance_state->setText(tr("Automatic liquidity status is not available yet."));
    m_liquidity_maintenance_next_step->setText(tr("Refresh after selecting a provider wallet."));
    m_liquidity_maintenance_cost->clear();
    m_approve_liquidity_maintenance->setVisible(false);
}

void ProviderLiquidity::setPrivacy(bool privacy)
{
    if (privacy) {
        closeWalletDialogs();
        if (m_cancel_preparation) m_cancel_preparation->hide();
        m_operation.reset(m_panel.walletGeneration());
        m_guided_task.clear();
        m_failed_guided_task.clear();
        m_pool_preparation = UniValue{};
        m_pool_preparation_status->clear();
        m_pool_preparation_status->hide();
        for (QLabel* label : {m_liquidity_maintenance_state, m_liquidity_maintenance_next_step,
                             m_liquidity_maintenance_cost}) {
            if (label) label->setText(m_panel.maskNumericText(label->text()));
        }
    }
    m_liquidity_output->setVisible(!privacy);
}

void PaymasterWidgetImpl::setLiquidityStatusForTesting(const UniValue& status)
{
    m_liquidity.applyLiquidityStatus(status);
}

void PaymasterWidgetImpl::setLiquidityPoolForTesting(const UniValue& pool_info)
{
    m_liquidity.applyLiquidityPoolEntries(pool_info, /*refresh_safety=*/false);
}

bool ProviderLiquidity::isLiquidityReadinessError(const QString& error)
{
    // These errors describe the live inventory, not unsaved provider
    // configuration. Keeping the distinction explicit prevents a spent
    // or not-yet-confirmed slot from sending the operator back through
    // Offer or setup even though the saved targets are already correct.
    return error == QLatin1String("PAYMASTER_POOLS_NOT_PREPARED") ||
           error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
           error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
           error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING") ||
           error == QLatin1String("PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING") ||
           error == QLatin1String("PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED") ||
           error == QLatin1String("PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED");
}

bool ProviderLiquidity::shouldStartAndRestoreLiquidity() const
{
    return m_panel.overview().canStartProvider() &&
           !m_panel.overview().hasPassiveExternalWait() &&
           m_liquidity_targets_satisfy_provider_policy &&
           m_maintenance_state ==
               QLatin1String("replenishing_liquidity");
}

bool ProviderLiquidity::maintenanceFeeLimitExceeded() const
{
    return m_panel.overview().lastServiceError() ==
               QLatin1String("PAYMASTER_MAINTENANCE_FEE_EXCEEDED") ||
           m_panel.overview().lastServiceError() ==
               QLatin1String("PAYMASTER_MAINTENANCE_FEE_CHANGED");
}

bool ProviderLiquidity::maintenanceWaitingForFunds() const
{
    return (m_panel.overview().serviceState() == QLatin1String("replenishing_liquidity") ||
            m_panel.overview().serviceState() == QLatin1String("active")) &&
           (m_panel.overview().lastServiceError() == QLatin1String("PAYMASTER_POOL_WAITING_DGB") ||
            m_panel.overview().lastServiceError() == QLatin1String("PAYMASTER_POOL_WAITING_FUNDS"));
}

void ProviderLiquidity::renderOverviewPool()
{
    const auto& liquidity = m_overview_liquidity_snapshot;
    const std::array<QString, 4> captions{{tr("Admission DGB"), tr("Operational DGB"),
                                           tr("Admission DD carriers"), tr("Operational DD carriers")}};
    int index{0};
    for (const auto* name : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
        const auto& slot = liquidity.find_value(name);
        for (const auto* field : {"ready", "reserved", "pending", "target"}) {
            qint64 count{0};
            m_panel.overview().setFundsValue(QString::fromLatin1(name) + QLatin1Char('_') + QLatin1String(field),
                GetInt64Field(slot, field, count) && count >= 0 ? QString::number(count) : QStringLiteral("—"));
        }
        qint64 ready{0}, target{0}, pending{0}, reserved{0};
        const bool valid = GetInt64Field(slot, "ready", ready) && ready >= 0 &&
            GetInt64Field(slot, "target", target) && target >= 0 &&
            GetInt64Field(slot, "pending", pending) && pending >= 0 &&
            GetInt64Field(slot, "reserved", reserved) && reserved >= 0;
        const QString caption = captions.at(index++);
        if (!valid) {
            m_panel.overview().setCapitalMetric(name, tr("%1: unavailable").arg(caption), "neutral");
            continue;
        }
        const QString kind = target == 0 ? QStringLiteral("neutral") : ready >= target ? QStringLiteral("ready") :
            pending > 0 || reserved > 0 ? QStringLiteral("waiting") : QStringLiteral("error");
        QString text = tr("%1: %2 / %3 ready · %4 reserved · %5 pending")
            .arg(caption).arg(ready).arg(target).arg(reserved).arg(pending);
        if (target == 0 && liquidity.find_value("targets_satisfy_provider_policy").isTrue())
            text += tr(" · not required");
        m_panel.overview().setCapitalMetric(name, text, kind);
    }
    const auto& pool = m_panel.overview().operatorSnapshot().find_value("provider").find_value("pool");
    qint64 complete_slots{0};
    m_panel.overview().setFundsValue("capacity", GetInt64Field(pool, "complete_operational_slots", complete_slots) && complete_slots >= 0
        ? QString::number(complete_slots) : QStringLiteral("—"));
    if (GetInt64Field(pool, "complete_operational_slots", complete_slots) && complete_slots >= 0)
        m_panel.overview().setCapitalMetric("complete_slots", tr("Available payment reserves: %1").arg(complete_slots), complete_slots > 0 ? "ready" : "neutral");
    else
        m_panel.overview().setCapitalMetric("complete_slots", tr("Payment capacity unavailable"), "neutral");
    const auto target = savedRefillCapacity();
    m_panel.overview().setFundsValue("target", target ? tr("Saved target: %1 payment(s)").arg(*target) : tr("No saved target"));
    qint64 earnings{0};
    m_panel.overview().setFundsValue("earnings", GetInt64Field(liquidity, "carrier_withdrawable_excess_cents", earnings) && earnings >= 0
        ? tr("%1 DD").arg(PaymasterWidgetImpl::ddAmount(earnings)) : QStringLiteral("—"));
    m_panel.overview().setCapitalMetric("refill_target", target ? tr("Saved refill target: %1 payment(s)").arg(*target) : tr("Refill target not configured"), "neutral");
    const bool surplus = hasExcessSavedReserves();
    m_panel.overview().setCapitalMetric("surplus", surplus ? tr("Extra reserves remain. Review their release in Settings → Reserves & refill.") : QString{}, "neutral");
    widget()->findChild<QLabel*>("paymasterCapital_surplus")->setVisible(surplus && !m_panel.privacyEnabled());
    updateSavedReserveState();
}

std::optional<qint64> ProviderLiquidity::savedRefillCapacity() const
{
    if (!m_overview_liquidity_snapshot.find_value("policy_configured").isTrue()) return std::nullopt;
    const auto& policy = m_overview_liquidity_snapshot.find_value("policy");
    qint64 dgb{0}, dd{0};
    if (!GetInt64Field(policy, "target_operational_dgb", dgb) || dgb < 1 ||
        (m_panel.overview().confirmedUserPaid() && (!GetInt64Field(policy, "target_operational_carriers", dd) || dd < 0))) return std::nullopt;
    return m_panel.overview().confirmedUserPaid() ? std::min(dgb, dd) : dgb;
}

bool ProviderLiquidity::hasExcessSavedReserves() const
{
    if (!m_overview_liquidity_snapshot.find_value("policy_configured").isTrue()) return false;
    for (const auto* key : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
        qint64 ready{0}, target{0};
        const auto& slot = m_overview_liquidity_snapshot.find_value(key);
        if (GetInt64Field(slot, "ready", ready) && GetInt64Field(slot, "target", target) && target >= 0 && ready > target) return true;
    }
    return false;
}

bool ProviderLiquidity::savedPoolTargetsReady() const
{
    // Unsaved form edits and a surplus in one pool cannot satisfy another
    // pool's persisted target. Pending/reserved entries are not ready.
    if (!m_overview_liquidity_snapshot.find_value("policy_configured").isTrue() ||
        !m_overview_liquidity_snapshot.find_value("targets_satisfy_provider_policy").isTrue()) return false;
    for (const auto* name : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
        const auto& slot = m_overview_liquidity_snapshot.find_value(name);
        qint64 ready{0}, target{0};
        if (!GetInt64Field(slot, "ready", ready) || !GetInt64Field(slot, "target", target) ||
            target < 0 || ready < target) return false;
    }
    return true;
}

void ProviderLiquidity::reviewMaintenanceFeeLimit()
{
    m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::LIQUIDITY));
    if (m_liquidity_advanced_toggle) {
        m_liquidity_advanced_toggle->setChecked(true);
    }
    if (m_maintenance_limits_toggle) {
        m_maintenance_limits_toggle->setChecked(true);
    }
    if (m_maintenance_fee_per_transaction) {
        m_maintenance_fee_per_transaction->setFocus(
            Qt::OtherFocusReason);
    }
    m_liquidity_policy_status->setText(tr(
        "Automatic refill is paused. Its estimated network fee is above the saved per-transaction maintenance limit, so no transaction has been created. Review all three finite limits and increase them only if the cost is acceptable; the new limits do not become active until you confirm and save them."));
}

bool ProviderLiquidity::readLiquidityPolicy(LiquidityPolicyValues& values) const
{
    bool ok_transaction{false};
    bool ok_hour{false};
    bool ok_day{false};
    values.automatic_replenishment =
        m_automatic_replenishment->isChecked();
    values.paid_maintenance_approved =
        m_paid_maintenance_approved->isChecked();
    values.admission_dgb = m_admission_dgb->value();
    values.operational_dgb = m_operational_dgb->value();
    values.admission_carriers = m_admission_carriers->value();
    values.operational_carriers = m_operational_carriers->value();
    ok_transaction = m_maintenance_fee_per_transaction->satoshis(values.fee_per_transaction);
    ok_hour = m_maintenance_fee_per_hour->satoshis(values.fee_per_hour);
    ok_day = m_maintenance_fee_per_day->satoshis(values.fee_per_day);
    return ok_transaction && ok_hour && ok_day;
}

UniValue ProviderLiquidity::liquidityPolicyToJSON(
    const LiquidityPolicyValues& values)
{
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("automatic_replenishment",
                  values.automatic_replenishment);
    policy.pushKV("paid_maintenance_approved",
                  values.paid_maintenance_approved);
    policy.pushKV("target_admission_dgb", values.admission_dgb);
    policy.pushKV("target_operational_dgb", values.operational_dgb);
    policy.pushKV("target_admission_carriers",
                  values.admission_carriers);
    policy.pushKV("target_operational_carriers",
                  values.operational_carriers);
    policy.pushKV(
        "maximum_maintenance_fee_per_transaction_satoshis",
        values.fee_per_transaction);
    policy.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                  values.fee_per_hour);
    policy.pushKV("maximum_maintenance_fee_per_day_satoshis",
                  values.fee_per_day);
    return policy;
}

bool ProviderLiquidity::liquidityPolicyCanRepresent(const UniValue& policy) const
{
    qint64 admission_dgb{0};
    qint64 operational_dgb{0};
    qint64 admission_carriers{0};
    qint64 operational_carriers{0};
    qint64 fee_per_transaction{0};
    qint64 fee_per_hour{0};
    qint64 fee_per_day{0};
    return IsCompleteLiquidityPolicy(policy) &&
        GetInt64Field(policy, "target_admission_dgb",
                      admission_dgb) &&
        GetInt64Field(policy, "target_operational_dgb",
                      operational_dgb) &&
        GetInt64Field(policy, "target_admission_carriers",
                      admission_carriers) &&
        GetInt64Field(policy, "target_operational_carriers",
                      operational_carriers) &&
        GetInt64Field(
            policy,
            "maximum_maintenance_fee_per_transaction_satoshis",
            fee_per_transaction) &&
        GetInt64Field(
            policy,
            "maximum_maintenance_fee_per_hour_satoshis",
            fee_per_hour) &&
        GetInt64Field(
            policy,
            "maximum_maintenance_fee_per_day_satoshis",
            fee_per_day) &&
        PaymasterWidgetImpl::spinCanRepresent(m_admission_dgb, admission_dgb) &&
        PaymasterWidgetImpl::spinCanRepresent(m_operational_dgb, operational_dgb) &&
        PaymasterWidgetImpl::spinCanRepresent(m_admission_carriers,
                         admission_carriers) &&
        PaymasterWidgetImpl::spinCanRepresent(m_operational_carriers,
                         operational_carriers) &&
        fee_per_transaction >= 0 && fee_per_hour >= 0 &&
        fee_per_day >= 0;
}

void ProviderLiquidity::setLiquidityPolicyMutationEnabled(bool enabled)
{
    if (m_reserve_editor) m_reserve_editor->setEnabled(enabled && !m_panel.busy());
    const bool controls_enabled = enabled && !m_panel.busy();
    m_automatic_replenishment->setEnabled(controls_enabled);
    m_paid_maintenance_approved->setEnabled(controls_enabled);
    for (QSpinBox* control : {
             m_admission_dgb, m_operational_dgb,
             m_admission_carriers, m_operational_carriers}) {
        control->setEnabled(controls_enabled);
    }
    for (QLineEdit* control : {
             m_maintenance_fee_per_transaction,
             m_maintenance_fee_per_hour,
             m_maintenance_fee_per_day}) {
        control->setReadOnly(!controls_enabled);
    }
    if (m_restore_liquidity_defaults) {
        m_restore_liquidity_defaults->setEnabled(controls_enabled);
    }
    if (m_save_liquidity_policy) {
        m_save_liquidity_policy->setEnabled(enabled && !m_panel.busy());
    }
    if (m_save_liquidity_policy_primary) {
        m_save_liquidity_policy_primary->setEnabled(
            enabled && !m_panel.busy());
    }
}

void ProviderLiquidity::loadLiquidityPolicy(const UniValue& policy, bool configured)
{
    if (!liquidityPolicyCanRepresent(policy)) return;
    m_loading_liquidity_policy = true;
    const UniValue& automatic =
        policy.find_value("automatic_replenishment");
    const UniValue& approved =
        policy.find_value("paid_maintenance_approved");
    if (automatic.isBool()) {
        m_automatic_replenishment->setChecked(automatic.get_bool());
    }
    if (approved.isBool()) {
        m_paid_maintenance_approved->setChecked(approved.get_bool());
    }
    const auto load_spin = [&policy](QSpinBox* control,
                                     const char* name) {
        const UniValue& value = policy.find_value(name);
        if (value.isNum()) control->setValue(value.getInt<int>());
    };
    load_spin(m_admission_dgb, "target_admission_dgb");
    load_spin(m_operational_dgb, "target_operational_dgb");
    load_spin(m_admission_carriers, "target_admission_carriers");
    load_spin(m_operational_carriers, "target_operational_carriers");
    const auto load_amount = [&policy](DgbAmountLineEdit* control, const char* name) {
        const UniValue& value = policy.find_value(name);
        if (value.isNum()) control->setSatoshis(value.getInt<qint64>());
    };
    load_amount(m_maintenance_fee_per_transaction,
                "maximum_maintenance_fee_per_transaction_satoshis");
    load_amount(m_maintenance_fee_per_hour,
                "maximum_maintenance_fee_per_hour_satoshis");
    load_amount(m_maintenance_fee_per_day,
                "maximum_maintenance_fee_per_day_satoshis");
    m_reserve_editor->synchronize(true);
    m_loading_liquidity_policy = false;
    m_liquidity_policy_configured = configured;
    m_liquidity_policy_dirty = false;
    updateLiquidityDisplay();
    updateQuickAutomation();
    QString status;
    if (!configured) {
        status = tr("Suggested migration values are shown. Review them and explicitly approve finite paid maintenance before Core can create a replenishment transaction.");
    } else if (!automatic.isTrue()) {
        status = approved.isTrue()
            ? tr("Saved: automatic refill is off. Its fee approval is retained; use the automatic-refill switch above to enable it.")
            : tr("Saved: automatic refill is off and paid refill is not approved. Review targets and fees below. The switch above can enable refill after a separate fee-approval review.");
    } else if (!approved.isTrue()) {
        status = tr("Saved: automatic refill is selected, but paid refill is not approved. Select 'Approve refill fees within the limits below', review the costs and save to allow new reserves to be created.");
    } else if (poolNumber(policy, "maximum_maintenance_fee_per_transaction_satoshis") == 0 ||
               poolNumber(policy, "maximum_maintenance_fee_per_hour_satoshis") == 0 ||
               poolNumber(policy, "maximum_maintenance_fee_per_day_satoshis") == 0) {
        status = tr("Saved: automatic refill is selected, but a zero fee limit blocks paid refill. Review the fee limits and save; zero never means unlimited.");
    } else {
        status = tr("Saved: automatic refill is on and paid refill is approved within the displayed DGB limits.");
    }
    m_liquidity_policy_status->setText(status);
}

void ProviderLiquidity::markLiquidityPolicyDirty()
{
    if (m_loading_liquidity_policy) return;
    if (!m_liquidity_policy_dirty) m_liquidity_edit_revision = m_saved_liquidity_policy.find_value("updated_at");
    m_liquidity_policy_dirty = true;
    invalidatePoolPreviews();
    m_panel.recovery().invalidateCarrierWithdrawalPreviews();
    if (m_liquidity_policy_status) {
        QString status = tr("Unsaved changes. Save to apply the displayed reserve settings and limits.");
        if (m_automatic_replenishment->isChecked() && !m_paid_maintenance_approved->isChecked())
            status += tr(" Automatic refill also needs permission for refill transactions and approved fee limits to create new reserves.");
        m_liquidity_policy_status->setText(status);
    }
    updateLiquidityDisplay();
    m_panel.overview().updateProviderButtons();
}

QString ProviderLiquidity::liquidityApprovalText(
    const LiquidityPolicyValues& values) const
{
    const qint64 admission_value =
        DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS;
    const qint64 operational_value = std::max<qint64>(
        admission_value, m_panel.overview().confirmedPaymentFee());
    const qint64 admission_capital = admission_value * values.admission_dgb;
    const QString bound_dgb = operational_value <= (MAX_MONEY - admission_capital) / values.operational_dgb
        ? PaymasterWidgetImpl::dgbAmount(admission_capital + operational_value * values.operational_dgb)
        : tr("exceeds the supported amount range");
    const qint64 bound_carriers =
        100LL * (values.admission_carriers +
                 values.operational_carriers);
    return tr("Automatic refill: %1.\n\n").arg(values.automatic_replenishment ? tr("on") : tr("off")) + tr(
        "Save these reserve settings and approve the listed paid-refill limits?\n\n"
        "Targets: %1 admission DGB, %2 operational DGB, %3 admission carriers and %4 operational carriers.\n"
        "Approximate target capital at the current provider ceiling: %5 DGB and %6 DD carrier base.\n\n"
        "Maintenance network-fee limits:\n"
        "• %7 DGB per transaction\n"
        "• %8 DGB per rolling hour\n"
        "• %9 DGB per rolling day\n\n"
        "Core can spend maintenance fees only while automatic provider operation or autostart is active. Zero never means unlimited.")
        .arg(values.admission_dgb)
        .arg(values.operational_dgb)
        .arg(values.admission_carriers)
        .arg(values.operational_carriers)
        .arg(bound_dgb)
        .arg(PaymasterFormatDD(bound_carriers))
        .arg(PaymasterWidgetImpl::dgbAmount(values.fee_per_transaction))
        .arg(PaymasterWidgetImpl::dgbAmount(values.fee_per_hour))
        .arg(PaymasterWidgetImpl::dgbAmount(values.fee_per_day));
}

void ProviderLiquidity::saveLiquidityPolicy()
{
    if (!m_liquidity_snapshot_representable) {
        m_liquidity_policy_status->setText(tr(
            "Liquidity settings were not changed: the persisted Core policy cannot be represented exactly by this interface."));
        return;
    }
    if (!m_panel.hasRpcTransport() || m_panel.busy()) {
        m_liquidity_policy_status->setText(!m_panel.hasRpcTransport()
            ? tr("Liquidity settings were not saved because no wallet RPC transport is available.")
            : tr("Liquidity settings were not saved because another wallet operation is still running."));
        return;
    }
    LiquidityPolicyValues values;
    if (!readLiquidityPolicy(values)) {
        PaymasterWidgetImpl::showPlainTextWarning(
            widget(), tr("Automatic liquidity policy"),
            tr("One or more maintenance-fee amounts are outside the supported whole-satoshi range."));
        return;
    }
    if (values.paid_maintenance_approved &&
        !m_panel.requirePrivacyOffForSensitiveAction(
            tr("Approving paid liquidity maintenance"))) {
        return;
    }
    const auto generation = m_panel.walletGeneration();
    if (values.paid_maintenance_approved) {
        // Keep the reviewed edit and wallet stable across the dialog's
        // nested event loop. Background reads must not block the save.
        m_panel.setRpcBusyState(true);
        const auto answer = PaymasterWidgetImpl::askPlainTextQuestion(
            widget(), tr("Approve finite paid maintenance"), liquidityApprovalText(values));
        if (generation != m_panel.walletGeneration()) return;
        m_panel.setRpcBusyState(false);
        if (answer != QMessageBox::Yes || m_panel.privacyEnabled()) return;
    }
    const UniValue requested_policy = liquidityPolicyToJSON(values);
    bool reduced_targets{false};
    if (m_liquidity_policy_configured) {
        for (const auto* key : {"target_admission_dgb", "target_operational_dgb", "target_admission_carriers", "target_operational_carriers"})
            reduced_targets |= poolNumber(requested_policy, key) < poolNumber(m_saved_liquidity_policy, key);
    }
    UniValue params{UniValue::VARR};
    params.push_back(requested_policy);
    const auto revision = m_liquidity_policy_dirty ? m_liquidity_edit_revision : m_saved_liquidity_policy.find_value("updated_at");
    if (revision.isNum()) params.push_back(revision);
    m_panel.call("setpaymasterliquiditypolicy", std::move(params), false,
         nullptr, [this, requested_policy, reduced_targets](const UniValue& result) {
             if (!IsExactLiquidityPolicyAcknowledgement(requested_policy, result)) {
                 reconcileLiquidityPolicySave(requested_policy,
                     tr("Core did not confirm the exact settings in its save response."), reduced_targets);
                 return;
             }
             liquidityPolicySaved(result, reduced_targets);
         }, false, [this, requested_policy, reduced_targets](const QString& error) {
             // A lost reply is not proof that the write failed. Read the
             // saved policy before asking for another spending approval.
             reconcileLiquidityPolicySave(requested_policy, error, reduced_targets);
         });
}

void ProviderLiquidity::liquidityPolicySaved(const UniValue& policy, bool reduced_targets)
{
    m_saved_liquidity_policy = policy;
    loadLiquidityPolicy(policy, /*configured=*/true);
    m_liquidity_policy_status->setText(tr("Liquidity settings saved successfully. ") + m_liquidity_policy_status->text());
    m_panel.recovery().invalidateCarrierWithdrawalPreviews();
    if (!reduced_targets) { m_panel.refreshStatus(); return; }
    // Saving the smaller refill goal does not authorize a transfer. Read
    // its confirmed revision before opening the separate release preview.
    m_panel.call("getpaymasterliquiditystatus", {}, false, nullptr,
        [this, policy](const UniValue& status) {
            if (!IsCompleteLiquidityStatus(status) || !status.find_value("policy_configured").isTrue() ||
                !IsExactLiquidityPolicyAcknowledgement(policy, status.find_value("policy")) ||
                policy.find_value("updated_at").write() != status.find_value("policy").find_value("updated_at").write()) {
                m_liquidity_policy_status->setText(tr("The smaller target was saved, but its current revision could not be verified. Refresh before reviewing excess reserves."));
                return;
            }
            applyLiquidityStatus(status);
            beginGuidedTask("reduce_reserves");
        }, false, [this](const QString& error) {
            m_liquidity_policy_status->setText(tr("The smaller target was saved. Excess reserves were not released; refresh to review them.\n%1").arg(error));
        });
}

void ProviderLiquidity::reconcileLiquidityPolicySave(const UniValue& requested, const QString& reason, bool reduced_targets)
{
    const auto unconfirmed = [this, reason](const QString& read_error = {}) {
        m_liquidity_policy_dirty = true;
        QString explanation = tr("The displayed settings could not be confirmed as saved. Your edits have been kept. Check the current status before retrying.");
        explanation += tr("\nReason: %1").arg(reason);
        if (!read_error.isEmpty()) explanation += tr("\nStatus check: %1").arg(read_error);
        m_liquidity_policy_status->setText(explanation);
        updateLiquidityDisplay();
    };
    m_panel.call("getpaymasterliquiditystatus", {}, false, nullptr,
        [this, requested, unconfirmed, reduced_targets](const UniValue& status) {
            const auto& policy = status.find_value("policy");
            if (IsCompleteLiquidityPolicy(policy)) m_saved_liquidity_policy = policy;
            if (status.find_value("policy_configured").isTrue() &&
                IsExactLiquidityPolicyAcknowledgement(requested, policy)) {
                liquidityPolicySaved(policy, reduced_targets);
            } else {
                unconfirmed();
            }
        }, false, [unconfirmed](const QString& error) { unconfirmed(error); });
}

void ProviderLiquidity::approveSuggestedLiquidityMaintenance()
{
    if (!m_panel.hasRpcTransport() || m_panel.busy()) {
        m_liquidity_policy_status->setText(!m_panel.hasRpcTransport()
            ? tr("Liquidity maintenance review was not started because no wallet RPC transport is available.")
            : tr("Liquidity maintenance review was not started because another wallet operation is still running."));
        return;
    }
    m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::LIQUIDITY));
    if (!m_liquidity_targets_satisfy_provider_policy) {
        // Restore only the visible minimums here. The operator must still
        // review and save the policy explicitly; this button never creates
        // a pool transaction or silently reverses an intentional slot
        // release.
        if (m_liquidity_advanced_toggle) {
            m_liquidity_advanced_toggle->setChecked(true);
        }
        m_admission_carriers->setValue(
            std::max(3, m_admission_carriers->value()));
        m_operational_carriers->setValue(
            std::max(1, m_operational_carriers->value()));
        // The focused automation page shows the proposed reserve counts
        // and finite limits before the explicit combined save and review.
        m_paid_maintenance_approved->setChecked(true);
        if (m_maintenance_limits_toggle) {
            m_maintenance_limits_toggle->setChecked(true);
        }
        m_liquidity_policy_status->setText(tr(
            "Proposed reserve targets (capacity checks / payments): DGB %1 / %2; DigiDollar %3 / %4. Review the finite refill limits below, then choose Save targets and approve refill. Change individual targets under Settings → Reserves & refill if needed. No funds have moved.")
                .arg(m_admission_dgb->value()).arg(m_operational_dgb->value())
                .arg(m_admission_carriers->value()).arg(m_operational_carriers->value()));
        return;
    }
    m_paid_maintenance_approved->setChecked(true);
    saveLiquidityPolicy();
}

void ProviderLiquidity::applyLiquidityStatus(const UniValue& result)
{
    m_overview_liquidity_snapshot = result;
    m_saved_liquidity_policy = result.find_value("policy");
    renderOverviewPool();
    const UniValue& policy = result.find_value("policy");
    const bool configured =
        result.find_value("policy_configured").isBool() &&
        result.find_value("policy_configured").get_bool();
    if (!configured && m_saved_liquidity_policy.isObject()) {
        // Suggested values are a draft, not persisted intent or a revision.
        m_saved_liquidity_policy.pushKV("updated_at", 0);
        m_saved_liquidity_policy.pushKV("automatic_replenishment", false);
    }
    const bool representable = liquidityPolicyCanRepresent(policy);
    m_liquidity_snapshot_representable = representable;
    m_liquidity_snapshot_available = representable;
    if (!representable) {
        // Keep the complete Core object separate from the controls. A
        // QSpinBox would clamp future or otherwise out-of-range target
        // values and could make an innocent Save overwrite wallet state.
        m_unrepresentable_liquidity_snapshot = policy;
        m_liquidity_policy_configured = false;
        m_liquidity_policy_dirty = false;
        setLiquidityPolicyMutationEnabled(false);
        m_liquidity_policy_status->setText(tr(
            "Core returned a complete liquidity policy containing a value that this interface cannot represent exactly. The persisted policy was retained unchanged and all liquidity-policy mutation is disabled."));
    } else if (policy.isObject() && !m_liquidity_policy_dirty) {
        m_unrepresentable_liquidity_snapshot =
            UniValue{UniValue::VOBJ};
        setLiquidityPolicyMutationEnabled(true);
        loadLiquidityPolicy(m_saved_liquidity_policy, configured);
    } else {
        m_liquidity_policy_configured = configured;
    }
    const UniValue& state = result.find_value("maintenance_state");
    m_maintenance_state = state.isStr()
        ? QString::fromStdString(state.get_str())
        : QStringLiteral("unavailable");
    const UniValue& targets_satisfy =
        result.find_value("targets_satisfy_provider_policy");
    // Treat the field as true when talking to an older test fixture, but
    // use the explicit Core result whenever it is present. Core owns this
    // invariant because a locally valid zero carrier target can still be
    // insufficient for an active USER_PAID offer.
    m_liquidity_targets_satisfy_provider_policy =
        !targets_satisfy.isBool() || targets_satisfy.get_bool();

    const QStringList slot_lines{
        ProviderOverview::slotStatusText(tr("Admission DGB"),
                       result.find_value("admission_dgb")),
        ProviderOverview::slotStatusText(tr("Operational DGB"),
                       result.find_value("operational_dgb")),
        ProviderOverview::slotStatusText(tr("Admission carriers"),
                       result.find_value("admission_carriers")),
        ProviderOverview::slotStatusText(tr("Operational carriers"),
                       result.find_value("operational_carriers")),
    };
    m_liquidity_slot_status->setText(slot_lines.join('\n'));
    // The liquidity RPC is the authoritative source for the current
    // ready/pending/missing breakdown. Keep the Operation counters in the
    // same snapshot so an out-of-order getpaymasterinfo reply cannot make
    // one card report a missing slot while another treats the target as
    // satisfied. Pending outputs intentionally do not count as ready.
    const UniValue& admission_dgb = result.find_value("admission_dgb");
    const UniValue& operational_dgb = result.find_value("operational_dgb");
    const UniValue& admission_carriers =
        result.find_value("admission_carriers");
    const UniValue& operational_carriers =
        result.find_value("operational_carriers");
    if (admission_dgb.isObject() && operational_dgb.isObject() &&
        admission_carriers.isObject() && operational_carriers.isObject()) {
        m_pool_status_loaded = true;
        m_pool_admission_dgb = poolNumber(admission_dgb, "ready");
        m_pool_operational_dgb = poolNumber(operational_dgb, "ready");
        m_pool_admission_carriers =
            poolNumber(admission_carriers, "ready");
        m_pool_operational_carriers =
            poolNumber(operational_carriers, "ready");
    }
    const auto total_field = [&result](const char* object_name,
                                       const char* field_name) {
        return poolNumber(result.find_value(object_name), field_name);
    };
    const qint64 missing =
        total_field("admission_dgb", "missing") +
        total_field("operational_dgb", "missing") +
        total_field("admission_carriers", "missing") +
        total_field("operational_carriers", "missing");
    const qint64 pending =
        total_field("admission_dgb", "pending") +
        total_field("operational_dgb", "pending") +
        total_field("admission_carriers", "pending") +
        total_field("operational_carriers", "pending");
    const bool approved = policy.isObject() &&
        policy.find_value("paid_maintenance_approved").isBool() &&
        policy.find_value("paid_maintenance_approved").get_bool();
    // The wallet backend distinguishes an unconfirmed maintenance-fee
    // exposure from fees that are already confirmed and charged to the
    // rolling budgets. Keep both visible in the ordinary operator view;
    // otherwise a self-transfer that rebuilds a 1 DD carrier misleadingly
    // looks free because its two DD transaction rows net to zero.
    const qint64 reserved = poolNumber(
        result, "maintenance_fee_reserved_satoshis");
    const qint64 spent_hour = poolNumber(
        result, "maintenance_fee_spent_last_hour_satoshis");
    const qint64 spent_day = poolNumber(
        result, "maintenance_fee_spent_last_day_satoshis");

    QString state_text;
    QString next_step;
    QString status_kind;
    if (m_maintenance_state ==
        QLatin1String("waiting_for_target_configuration")) {
        status_kind = QStringLiteral("action");
        state_text = tr("Saved liquidity targets cannot make this offer ready");
        next_step = tr(
            "User-paid service needs at least three admission DigiDollar carriers and one operational DigiDollar carrier. Review the displayed targets, save them, and then prepare only the missing output.");
    } else if (m_maintenance_state ==
        QLatin1String("waiting_for_maintenance_approval")) {
        status_kind = QStringLiteral("action");
        state_text = tr("Liquidity targets are saved — refill spending is not yet approved");
        next_step = tr(
            "No target update is needed. To create the missing pool output, review the finite transaction, hourly and daily maintenance limits and explicitly approve that bounded refill cost.");
    } else if (m_maintenance_state ==
               QLatin1String("replenishing_liquidity")) {
        if (maintenanceFeeLimitExceeded()) {
            status_kind = QStringLiteral("action");
            state_text = tr(
                "Automatic refill paused — approved fee limit is too low");
            const qint64 fee_limit = policy.isObject()
                ? poolNumber(policy,
                             "maximum_maintenance_fee_per_transaction_satoshis")
                : 0;
            next_step = tr(
                "No transaction was created. The estimated network fee is above the saved per-transaction maintenance limit of %1 DGB. Review the limit and increase it only if that bounded cost is acceptable.")
                            .arg(PaymasterWidgetImpl::dgbAmount(fee_limit));
        } else if (maintenanceWaitingForFunds()) {
            status_kind = QStringLiteral("action");
            state_text = tr("Automatic refill paused — waiting for wallet funds");
            next_step = m_panel.overview().readinessExplanation(m_panel.overview().lastServiceError());
        } else if (pending > 0) {
            status_kind = QStringLiteral("waiting");
            state_text = tr("Replacement liquidity is waiting for confirmation");
            next_step = tr(
                "%1 pending slot(s) already count toward the target. Mine or wait for a confirming block; Core will not create a duplicate refill.")
                .arg(pending);
        } else if (m_panel.overview().readinessErrors().contains(
                       QStringLiteral("PAYMASTER_NODE_NOT_READY"))) {
            status_kind = QStringLiteral("waiting");
            state_text = Params().GetChainType() == ChainType::REGTEST
                ? tr("Refill approved — waiting for a fresh Regtest block")
                : tr("Refill approved — waiting for node readiness");
            next_step = Params().GetChainType() == ChainType::REGTEST
                ? tr("No refill transaction has been created yet. Mine a fresh Regtest block so Core can leave initial synchronization and safely create the missing pool output.")
                : tr("No refill transaction has been created yet. Core will create it automatically after synchronization and transaction-broadcast readiness are restored.");
        } else if (m_panel.overview().readinessErrors().contains(
                       QStringLiteral("PAYMASTER_WALLET_LOCKED"))) {
            status_kind = QStringLiteral("waiting");
            state_text = tr("Refill approved — waiting for wallet unlock");
            next_step = tr(
                "No refill transaction has been created yet. Unlock the provider wallet; automatic maintenance will then resume without another approval.");
        } else if (!m_panel.overview().running()) {
            status_kind = QStringLiteral("waiting");
            state_text = tr("Refill approved — waiting for provider start");
            next_step = tr(
                "No refill transaction has been created yet. Start the provider with “Start and restore liquidity”; Core will then create the bounded refill automatically.");
        } else {
            status_kind = QStringLiteral("waiting");
            state_text = tr("Refill approved — preparing missing liquidity");
            next_step = tr(
                "Core may now create at most one bounded maintenance transaction. Keep the provider service running and the wallet unlocked; the display changes to pending confirmation as soon as that transaction exists.");
        }
    } else if (m_maintenance_state ==
               QLatin1String("waiting_for_liquidity_confirmation")) {
        status_kind = QStringLiteral("waiting");
        state_text = tr("Replacement liquidity is waiting for confirmation");
        next_step = tr(
            "%1 pending slot(s) count toward the target, so Core will not create duplicates. Provider readiness resumes after confirmation.")
            .arg(pending);
    } else if (m_maintenance_state == QLatin1String("ready")) {
        status_kind = QStringLiteral("ready");
        state_text = tr("Liquidity targets are satisfied");
        next_step = tr(
            "No maintenance transaction is required. Confirmed successor outputs are reused automatically.");
    } else {
        status_kind = QStringLiteral("error");
        state_text = tr("Liquidity maintenance status unavailable");
        next_step = tr("Refresh the provider status before changing liquidity.");
    }
    if (result.find_value("automation_status").isObject() ||
        policy.find_value("automatic_replenishment").isFalse()) {
        state_text = automationSummary();
        next_step = tr("Review reserve targets and refill costs below. Changing the saved switch does not undo signed transactions.");
        const auto& observed = result.find_value("automation_status").find_value("state");
        status_kind = observed.isStr() && observed.get_str() == "blocked" ? QStringLiteral("action") : QStringLiteral("waiting");
        if (observed.isStr() && observed.get_str() == "ready") status_kind = QStringLiteral("ready");
    }
    updateQuickAutomation();
    m_panel.setStatusLabel(m_liquidity_maintenance_state, state_text,
                   status_kind);
    // Re-polishing an unchanged card on every poll caused the complete
    // Operation layout to flash even though no user-visible state changed.
    m_panel.setStatusKind(m_liquidity_maintenance_card, status_kind);
    m_liquidity_maintenance_next_step->setText(next_step);
    QString maintenance_cost;
    if (!m_liquidity_targets_satisfy_provider_policy) {
        maintenance_cost = tr(
            "Automatic restoration is paused because the saved operational carrier target is zero. No maintenance transaction has been created.");
    } else {
        maintenance_cost = tr(
            "%1 missing and %2 pending slot(s). Refill spending authorization: %3.")
                .arg(missing)
                .arg(pending)
                .arg(approved ? tr("approved within finite limits")
                              : tr("not approved"));
        if (reserved > 0) {
            maintenance_cost += tr(
                " Reserved fee budget: %1 DGB. Fees of unconfirmed transactions: %2 DGB.")
                                    .arg(PaymasterWidgetImpl::dgbAmount(poolNumber(result, "maintenance_fee_planned_satoshis")),
                                         PaymasterWidgetImpl::dgbAmount(poolNumber(result, "maintenance_fee_broadcast_satoshis")));
        }
        if (spent_hour > 0 || spent_day > 0) {
            maintenance_cost += tr(
                " Confirmed maintenance cost: %1 DGB in the rolling hour and %2 DGB in the rolling day.")
                                    .arg(PaymasterWidgetImpl::dgbAmount(spent_hour),
                                         PaymasterWidgetImpl::dgbAmount(spent_day));
        }
    }
    m_liquidity_maintenance_cost->setText(maintenance_cost);
    m_approve_liquidity_maintenance->setText(
        maintenanceFeeLimitExceeded()
            ? tr("Review refill cost limit…")
        : m_liquidity_targets_satisfy_provider_policy
            ? tr("Approve bounded refill costs…")
            : tr("Review required liquidity targets…"));
    m_approve_liquidity_maintenance->setVisible(
        !m_liquidity_targets_satisfy_provider_policy || !configured ||
        (missing > 0 && !approved) || maintenanceFeeLimitExceeded());
    // Keep polling for either liquidity confirmation or an external node
    // gate. A successful liquidity refresh must not accidentally stop the
    // synchronization poll used by the Operation page.
    m_panel.overview().updateAutomaticRefreshTimer();

    m_liquidity_budget_status->setText(tr(
        "Maintenance fees — reserved: %1 DGB; spent in the rolling hour: %2 DGB; spent in the rolling day: %3 DGB.")
        .arg(PaymasterWidgetImpl::dgbAmount(reserved), PaymasterWidgetImpl::dgbAmount(spent_hour),
             PaymasterWidgetImpl::dgbAmount(spent_day)));
    m_panel.recovery().applyCarrierBalances(poolNumber(result, "carrier_base_cents"),
        poolNumber(result, "carrier_withdrawable_excess_cents"), configured);
    m_panel.overview().updateProviderButtons();
}

void ProviderLiquidity::refreshLiquidityStatus()
{
    m_panel.call("getpaymasterliquiditystatus", {}, false, nullptr,
         [this](const UniValue& result) {
             if (!IsCompleteLiquidityStatus(result)) {
                 m_liquidity_snapshot_available = false;
                 m_liquidity_snapshot_representable = false;
                 setLiquidityPolicyMutationEnabled(false);
                 m_liquidity_policy_status->setText(tr(
                     "Core returned an incomplete liquidity status. Guided setup remains unavailable until a complete refresh succeeds."));
                 m_panel.configuration().refreshProviderSafetyStatus();
                 return;
             }
             applyLiquidityStatus(result);
             refreshLiquidityPoolEntries();
         }, false,
         [this](const QString& error) {
             m_liquidity_snapshot_available = false;
             m_liquidity_snapshot_representable = false;
             setLiquidityPolicyMutationEnabled(false);
             m_maintenance_state = QStringLiteral("unavailable");
             m_liquidity_maintenance_state->setText(tr(
                 "Liquidity maintenance status unavailable"));
             m_liquidity_maintenance_next_step->setText(error);
             m_liquidity_policy_status->setText(tr(
                 "Core did not return a liquidity policy. No paid maintenance action is available."));
             m_panel.configuration().refreshProviderSafetyStatus();
         });
}

void ProviderLiquidity::applyLiquidityPoolEntries(const UniValue& result,
                               bool refresh_safety)
{
    m_pool_preparation = IsCompleteProviderPoolInfo(result) ? result.find_value("preparation") : UniValue{};
    updatePoolPreparationStatus();
    std::vector<ProviderRecovery::CarrierChoice> carriers;
    m_pending_successor_carrier_cents = 0;
    m_pending_successor_dgb_satoshis = 0;
    if (!IsCompleteProviderPoolInfo(result)) {
        m_panel.recovery().clearCarrierChoices();
        m_liquidity_recycling_status->setText(tr(
            "Core returned incomplete Paymaster pool entries. Carrier release remains unavailable until a complete refresh succeeds."));
        if (refresh_safety) m_panel.configuration().refreshProviderSafetyStatus();
        return;
    }
    const UniValue& pool = result.find_value("pool");
    for (const UniValue& entry : pool.getValues()) {
        const QString state = ProviderOverview::activityText(entry, "state");
        const QString purpose = ProviderOverview::activityText(entry, "purpose");
        const QString asset = ProviderOverview::activityText(entry, "asset");
        if (state == QLatin1String("pending_successor")) {
            if (asset == QLatin1String("dd_carrier")) {
                m_pending_successor_carrier_cents +=
                    poolNumber(entry, "dd_cents");
            } else if (asset == QLatin1String("dgb")) {
                m_pending_successor_dgb_satoshis +=
                    poolNumber(entry, "dgb_satoshis");
            }
        }
        if (state != QLatin1String("available") ||
            purpose != QLatin1String("operational") ||
            asset != QLatin1String("dd_carrier") ||
            poolNumber(entry, "confirmation_height") <= 0) {
            continue;
        }
        const QString txid = ProviderOverview::activityText(entry, "txid");
        const qint64 vout = poolNumber(entry, "vout");
        const qint64 cents = poolNumber(entry, "dd_cents");
        const QString key = QStringLiteral("%1:%2").arg(txid).arg(vout);
        carriers.emplace_back(tr("%1 DD — %2…:%3").arg(PaymasterFormatDD(cents), txid.left(12)).arg(vout), key);
    }
    QStringList recycling;
    if (m_pending_successor_carrier_cents > 0) {
        recycling.push_back(tr(
            "Carrier is being reused: %1 DD, waiting for confirmation.")
            .arg(PaymasterFormatDD(m_pending_successor_carrier_cents)));
    }
    if (m_pending_successor_dgb_satoshis > 0) {
        recycling.push_back(tr(
            "DGB successor slot is being reused: %1 DGB, waiting for confirmation.")
            .arg(PaymasterWidgetImpl::dgbAmount(m_pending_successor_dgb_satoshis)));
    }
    if (recycling.isEmpty() &&
        m_maintenance_state == QLatin1String("replenishing_liquidity")) {
        recycling.push_back(tr(
            "A missing DGB slot will be replenished within the approved limits."));
    }
    m_liquidity_recycling_status->setText(
        recycling.isEmpty()
            ? tr("No replacement output is currently waiting for confirmation.")
            : recycling.join('\n'));
    const bool release_available = m_panel.hasCompleteMutationSnapshots() &&
        m_liquidity_policy_configured && !m_panel.overview().running();
    m_panel.recovery().applyCarrierChoices(carriers, release_available);
    if (refresh_safety) m_panel.configuration().refreshProviderSafetyStatus();
}

void ProviderLiquidity::refreshLiquidityPoolEntries()
{
    m_panel.call("getpaymasterpoolinfo", {}, false, nullptr,
         [this](const UniValue& result) {
             applyLiquidityPoolEntries(result, /*refresh_safety=*/true);
         }, false,
         [this](const QString& error) {
             m_pool_preparation = UniValue{};
             updatePoolPreparationStatus();
             m_pool_preparation_status->setText(tr("Pool preparation status unavailable: %1").arg(error));
             m_pool_preparation_status->setVisible(!m_panel.privacyEnabled());
             m_panel.recovery().clearCarrierChoices();
             m_liquidity_recycling_status->setText(tr(
                 "Pool entry details are unavailable: %1").arg(error));
             m_panel.configuration().refreshProviderSafetyStatus();
         });
}

QString ProviderLiquidity::refillFundingExplanation(const QString& error) const
{
    const auto& funding = m_overview_liquidity_snapshot.find_value("refill_funding");
    const bool dgb = error == QLatin1String("PAYMASTER_POOL_WAITING_DGB");
    qint64 required{0}, available{0}, shortfall{0};
    if (GetInt64Field(funding, dgb ? "required_dgb_satoshis" : "required_dd_cents", required) &&
        GetInt64Field(funding, dgb ? "available_dgb_satoshis" : "available_dd_cents", available) &&
        GetInt64Field(funding, dgb ? "shortfall_dgb_satoshis" : "shortfall_dd_cents", shortfall) &&
        required >= 0 && available >= 0 && shortfall == std::max<qint64>(0, required - available)) {
        const QString amounts = dgb
            ? tr("DGB capital to reach the saved reserve target: %1 DGB. Confirmed funds available outside the protected reserves: %2 DGB. Missing capital: %3 DGB. Network fees are additional.")
                  .arg(PaymasterWidgetImpl::dgbAmount(required)).arg(PaymasterWidgetImpl::dgbAmount(available)).arg(PaymasterWidgetImpl::dgbAmount(shortfall))
            : tr("DD capital to reach the saved reserve target: %1 DD. Funds available outside the protected reserves: %2 DD. Missing capital: %3 DD. DGB is also required for network fees.")
                  .arg(PaymasterFormatDD(required)).arg(PaymasterFormatDD(available)).arg(PaymasterFormatDD(shortfall));
        return amounts + QStringLiteral("\n") + tr("Automatic refill creates fewer complete reserve slots when funds cannot cover all missing slots and their fees. It waits for confirmation before continuing toward the unchanged target. Individual slots are never funded with partial amounts. The saved fee limits still apply. No transaction has been created for the remaining refill.");
    }
    return dgb
        ? tr("Not enough confirmed, spendable DGB outside the protected reserves to refill them. Add DGB for the missing reserve capital and its network fee. Smaller refills are attempted within the saved limits; no refill transaction has been created.")
        : tr("Not enough spendable DD outside the protected reserves to refill them. Add DD for the missing reserves and DGB for the network fee. Smaller refills are attempted within the saved limits; no refill transaction has been created.");
}

void ProviderLiquidity::addQuickRefill(QVBoxLayout* layout, QWidget* parent, bool overview)
{
    auto* refill = new QCheckBox(tr("Automatically refill missing reserves"), parent);
    refill->setObjectName(overview ? "paymasterOverviewRefill" : "paymasterSettingsRefill");
    refill->setToolTip(tr("Uses the saved reserve targets and approved fee limits. A running provider may refill immediately. Turning this off does not undo signed transactions."));
    m_refill_switches.append(refill);
    layout->addWidget(refill);
    auto* status = new QLabel(parent);
    status->setObjectName(overview ? "paymasterOverviewRefillStatus" : "paymasterSettingsRefillStatus");
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    m_refill_switch_status.append(status);
    layout->addWidget(status);
    connect(refill, &QCheckBox::toggled, widget(), [this](bool enabled) { saveQuickRefill(enabled); });
}

void ProviderLiquidity::saveQuickRefill(bool enabled)
{
    if (m_panel.busy() || m_panel.privacyEnabled() || m_refill_save_pending || !m_panel.hasRpcTransport() ||
        !IsCompleteLiquidityPolicy(m_saved_liquidity_policy) ||
        !m_saved_liquidity_policy.find_value("updated_at").isNum()) { updateQuickAutomation(); return; }
    const auto generation = m_panel.walletGeneration();
    const auto saved = m_saved_liquidity_policy;
    UniValue requested{UniValue::VOBJ};
    for (const auto& key : saved.getKeys()) if (key != "updated_at") requested.pushKV(key, saved.find_value(key));
    requested.pushKV("automatic_replenishment", enabled);
    m_refill_message.clear();
    if (enabled && (!saved.find_value("paid_maintenance_approved").isTrue() ||
        poolNumber(saved, "maximum_maintenance_fee_per_transaction_satoshis") <= 0 ||
        poolNumber(saved, "maximum_maintenance_fee_per_hour_satoshis") <= 0 ||
        poolNumber(saved, "maximum_maintenance_fee_per_day_satoshis") <= 0)) {
        QDialog review(widget());
        review.setObjectName("paymasterRefillApproval");
        m_refill_approval = &review;
        review.setWindowTitle(tr("Approve automatic refill costs"));
        auto* form = new QFormLayout(&review);
        auto* explanation = new QLabel(tr("Use these reserve targets: %1 DGB capacity checks, %2 DGB payment reserves, %3 DD capacity checks and %4 DD payment reserves. A running provider may immediately bind the missing wallet funds and pay fees within these limits. No other unsaved settings are included.")
            .arg(poolNumber(saved, "target_admission_dgb")).arg(poolNumber(saved, "target_operational_dgb"))
            .arg(poolNumber(saved, "target_admission_carriers")).arg(poolNumber(saved, "target_operational_carriers")), &review);
        const auto& offer = m_panel.overview().operatorSnapshot().find_value("provider").find_value("policy");
        const qint64 slot_value = std::max<qint64>(DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS,
                                                   poolNumber(offer, "maximum_network_fee_dgb_satoshis"));
        const qint64 target_dgb = DigiDollar::Paymaster::MIN_ADMISSION_DGB_SATOSHIS * poolNumber(saved, "target_admission_dgb") +
                                  slot_value * poolNumber(saved, "target_operational_dgb");
        const qint64 target_dd = 100 * (poolNumber(saved, "target_admission_carriers") + poolNumber(saved, "target_operational_carriers"));
        explanation->setText(explanation->text() + tr("\n\nTarget capital: at least %1 DGB and %2 DD, before network fees. Existing usable reserves count toward these targets; only missing capacity is refilled.")
            .arg(PaymasterWidgetImpl::dgbAmount(target_dgb), PaymasterFormatDD(target_dd)));
        explanation->setWordWrap(true);
        explanation->setMaximumWidth(620);
        form->addRow(explanation);
        const auto suggested = DigiDollar::Paymaster::SetupLiquidityPreset(
            poolNumber(saved, "target_admission_carriers") > 0,
            std::max({1, static_cast<int>(poolNumber(saved, "target_operational_dgb")),
                      static_cast<int>(poolNumber(saved, "target_operational_carriers")),
                      static_cast<int>(poolNumber(saved, "target_admission_dgb")) - 2,
                      static_cast<int>(poolNumber(saved, "target_admission_carriers")) - 2}));
        const auto review_limit = [&](const char* key) {
            const qint64 value = poolNumber(saved, key);
            return value > 0 ? value : suggested.find_value(key).getInt<qint64>();
        };
        auto* transaction = new DgbAmountLineEdit(review_limit("maximum_maintenance_fee_per_transaction_satoshis"), &review);
        auto* hour = new DgbAmountLineEdit(review_limit("maximum_maintenance_fee_per_hour_satoshis"), &review);
        auto* day = new DgbAmountLineEdit(review_limit("maximum_maintenance_fee_per_day_satoshis"), &review);
        transaction->setObjectName("paymasterQuickRefillFeeTransaction");
        hour->setObjectName("paymasterQuickRefillFeeHour");
        day->setObjectName("paymasterQuickRefillFeeDay");
        form->addRow(tr("Maximum per transaction (DGB):"), transaction);
        form->addRow(tr("Maximum per rolling hour (DGB):"), hour);
        form->addRow(tr("Maximum per rolling day (DGB):"), day);
        auto* validation = new QLabel(&review);
        validation->setWordWrap(true);
        form->addRow(validation);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &review);
        auto* approve = buttons->addButton(tr("Approve limits and enable refill"), QDialogButtonBox::AcceptRole);
        const auto validate = [=] {
            qint64 t{0}, h{0}, d{0};
            const bool valid = transaction->satoshis(t) && hour->satoshis(h) && day->satoshis(d) && t > 0 && h >= t && d >= h;
            approve->setEnabled(valid);
            validation->setText(valid ? tr("These finite costs apply only to reserve refill.") : tr("Enter positive limits, with the hourly limit at least the transaction limit and the daily limit at least the hourly limit."));
        };
        for (auto* edit : {transaction, hour, day}) connect(edit, &QLineEdit::textChanged, &review, validate);
        connect(buttons, &QDialogButtonBox::accepted, &review, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &review, &QDialog::reject);
        form->addRow(buttons);
        validate();
        m_panel.setRpcBusyState(true);
        const auto answer = review.exec();
        m_refill_approval.clear();
        if (generation != m_panel.walletGeneration()) return;
        m_panel.setRpcBusyState(false);
        if (answer != QDialog::Accepted || m_panel.privacyEnabled()) { updateQuickAutomation(); return; }
        qint64 t{0}, h{0}, d{0};
        if (!transaction->satoshis(t) || !hour->satoshis(h) || !day->satoshis(d) || t <= 0 || h < t || d < h) return;
        requested.pushKV("paid_maintenance_approved", true);
        requested.pushKV("maximum_maintenance_fee_per_transaction_satoshis", t);
        requested.pushKV("maximum_maintenance_fee_per_hour_satoshis", h);
        requested.pushKV("maximum_maintenance_fee_per_day_satoshis", d);
    }
    m_refill_save_pending = true;
    updateQuickAutomation();
    UniValue params{UniValue::VARR};
    params.push_back(requested);
    params.push_back(saved.find_value("updated_at"));
    const auto finish = [this, requested, saved](const UniValue& result) {
        m_refill_save_pending = false;
        const bool confirmed = IsExactLiquidityPolicyAcknowledgement(requested, result);
        if (confirmed) {
            m_saved_liquidity_policy = result;
            if (!m_liquidity_policy_dirty) loadLiquidityPolicy(result, true);
            else {
                // Rebase only our acknowledged, exact change. External edits
                // still require an explicit reload before a stale form can save.
                if (m_liquidity_edit_revision.write() == saved.find_value("updated_at").write())
                    m_liquidity_edit_revision = result.find_value("updated_at");
                const QSignalBlocker automatic_blocker(m_automatic_replenishment);
                m_automatic_replenishment->setChecked(result.find_value("automatic_replenishment").isTrue());
                if (m_paid_maintenance_approved->isChecked() == saved.find_value("paid_maintenance_approved").isTrue()) {
                    const QSignalBlocker approval_blocker(m_paid_maintenance_approved);
                    m_paid_maintenance_approved->setChecked(result.find_value("paid_maintenance_approved").isTrue());
                }
                const auto merge_fee = [&](DgbAmountLineEdit* edit, const char* key) {
                    qint64 value{0};
                    if (edit->satoshis(value) && value == poolNumber(saved, key)) {
                        const QSignalBlocker blocker(edit);
                        edit->setSatoshis(poolNumber(result, key));
                    }
                };
                merge_fee(m_maintenance_fee_per_transaction, "maximum_maintenance_fee_per_transaction_satoshis");
                merge_fee(m_maintenance_fee_per_hour, "maximum_maintenance_fee_per_hour_satoshis");
                merge_fee(m_maintenance_fee_per_day, "maximum_maintenance_fee_per_day_satoshis");
            }
        }
        m_refill_message = confirmed
            ? tr("Reserve automation saved.")
            : tr("The requested change was not confirmed. The checkbox shows the verified wallet setting; review it before trying again.");
        updateQuickAutomation();
    };
    const auto verify = [this, requested, finish](const QString&) {
        m_panel.call("getpaymasterliquiditystatus", {}, false, nullptr, [this, finish](const UniValue& status) {
            applyLiquidityStatus(status);
            finish(status.find_value("policy"));
        }, false, [this](const QString&) {
            m_refill_save_pending = false;
            m_saved_liquidity_policy = UniValue{};
            m_refill_message = tr("The save result is unknown. Refresh the status before trying again.");
            updateQuickAutomation();
        });
    };
    m_panel.call("setpaymasterliquiditypolicy", params, false, nullptr,
        [this, requested, finish, verify](const UniValue& result) {
            if (!IsExactLiquidityPolicyAcknowledgement(requested, result)) { verify({}); return; }
            finish(result);
            m_panel.refreshStatus();
        }, false, verify);
}

bool ProviderLiquidity::repairableLiquidityGap() const
{
    // Starting into maintenance is safe only when the saved policy can
    // actually restore every slot required by the active offer. In
    // particular, an intentionally released zero carrier target must not
    // be presented as a repairable shortfall.
    const bool automatic_liquidity_can_restore =
        m_liquidity_policy_configured &&
        m_liquidity_targets_satisfy_provider_policy &&
        m_automatic_replenishment->isChecked() &&
        m_paid_maintenance_approved->isChecked();
    return !m_panel.overview().readinessErrors().isEmpty() && m_panel.overview().operationMode() != QLatin1String("manual") &&
           automatic_liquidity_can_restore &&
           std::all_of(m_panel.overview().readinessErrors().cbegin(), m_panel.overview().readinessErrors().cend(),
                       [](const QString& error) {
                           return error == QLatin1String("PAYMASTER_ADMISSION_DGB_MISSING") ||
                                  error == QLatin1String("PAYMASTER_ADMISSION_CARRIERS_MISSING") ||
                                  error == QLatin1String("PAYMASTER_OPERATIONAL_SLOT_MISSING");
                       });
}

void ProviderLiquidity::failGuidedTask(const QString& error)
{
    m_operation.fail(error, PaymasterQt::StableCode(m_panel.rpcFailure()));
    m_panel.overview().closeTaskErrorDetails();
    m_failed_guided_task = m_guided_task;
    m_guided_task.clear();
    m_guided_waiting_for_provider = false;
    m_panel.overview().renderCurrentTask();
    m_panel.overview().refreshOperatorStatus();
}

void ProviderLiquidity::beginGuidedTask(const QString& task, bool start_after)
{
    if (start_after && (m_panel.configuration().autostartPending())) {
        m_panel.mainPages()->setCurrentIndex(0);
        m_panel.configuration().updateAutostartControls();
        return;
    }
    // A confirmed cancellation may enqueue its replacement preview in the
    // same RPC chain. Do not drop it in a timer between background refreshes.
    if (m_panel.privacyEnabled() || (m_panel.busy() && m_panel.rpcHandlerDepth() == 0) || !m_panel.hasRpcTransport()) return;
    const QString title = task == QLatin1String("preparepaymasterpool")
        ? (start_after ? tr("Restore reserves and start provider") : tr("Restore reserves"))
        : task == QLatin1String("reduce_reserves") ? tr("Release excess reserves")
        : task == QLatin1String("all_excess") ? tr("Withdraw earnings") : task == QLatin1String("rebalancepaymasterpool") ? tr("Release excess DGB") : tr("Release one DD reserve");
    if (!m_operation.begin(title, m_panel.walletGeneration())) return;
    const auto generation = m_panel.walletGeneration();
    m_guided_task = task;
    ++m_guided_generation;
    m_failed_guided_task.clear();
    m_guided_busy_retries = 0;
    m_guided_waiting_for_provider = false;
    m_guided_start_after = start_after;
    m_panel.overview().renderCurrentTask();
    if (!m_panel.hasCompleteMutationSnapshots()) {
        failGuidedTask(tr("Current wallet information is incomplete. Refresh the status before retrying."));
        return;
    }
    if (task == QLatin1String("release_slot")) {
        // A cached selector may contain a spent, reserved or released output.
        // Always read the current pool before offering a reserve for review.
        m_panel.call("getpaymasterpoolinfo", {}, false, nullptr,
            [this, task, generation](const UniValue& pool) {
                if (generation != m_panel.walletGeneration() || m_panel.privacyEnabled()) return;
                applyLiquidityPoolEntries(pool, false);
                if (!IsCompleteProviderPoolInfo(pool)) {
                    failGuidedTask(tr("The current reserves could not be verified. Refresh the status before choosing a reserve. No release was approved."));
                    return;
                }
                if (!m_panel.recovery().hasReleasableCarrier()) {
                    failGuidedTask(QStringLiteral("PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE"));
                    return;
                }
                previewGuidedTask(task);
            }, false, [this](const QString& error) { failGuidedTask(error); });
    } else {
        previewGuidedTask(task);
    }
}

void ProviderLiquidity::previewGuidedTask(const QString& task)
{
    const auto generation = m_panel.walletGeneration();
    QString selected_reserve;
    if (task == QLatin1String("release_slot")) {
        QStringList choices, sources;
        const auto carriers = m_panel.recovery().carrierChoices();
        for (size_t i = 0; i < carriers.size(); ++i) {
            choices << tr("Reserve %1 · %2").arg(i + 1).arg(carriers[i].first);
            sources << carriers[i].second;
        }
        bool chosen{false};
        const QString selected = QInputDialog::getItem(widget(), m_operation.title, tr("Choose the available reserve to return to this wallet. The provider will be paused and its saved capacity reduced."), choices, 0, false, &chosen);
        if (generation != m_panel.walletGeneration() || m_panel.privacyEnabled()) return;
        if (!chosen) {
            m_operation.reset(m_panel.walletGeneration()); m_guided_task.clear(); m_panel.overview().renderCurrentTask(); return;
        }
        const int selected_index = choices.indexOf(selected);
        if (selected_index < 0) {
            m_operation.reset(m_panel.walletGeneration()); m_guided_task.clear(); m_panel.overview().renderCurrentTask(); return;
        }
        // Bind the review to the choice the user actually saw, even if a
        // nested event loop refreshed/reordered the live combo-box model.
        selected_reserve = sources[selected_index];
        m_panel.recovery().selectCarrier(selected_reserve);
    }
    const bool preparation = task == QLatin1String("preparepaymasterpool");
    const bool reduction = task == QLatin1String("reduce_reserves");
    const bool rebalance = task == QLatin1String("rebalancepaymasterpool") || reduction;
    const UniValue context = m_panel.overview().operatorSnapshot();
    UniValue params, proposed_policy;
    bool repair_targets{false};
    try {
        params = preparation ? poolOptions(false, {}, true) : rebalance ? poolOptions(false) : m_panel.recovery().carrierWithdrawalOptions(task, false);
        if (rebalance) {
            const auto& saved = reduction ? m_saved_liquidity_policy : context.find_value("provider").find_value("liquidity").find_value("policy");
            qint64 revision{0};
            if (!GetInt64Field(saved, "updated_at", revision) || revision <= 0)
                throw std::runtime_error("PAYMASTER_LIQUIDITY_POLICY_CHANGED");
            UniValue options{UniValue::VOBJ};
            options.pushKV("admission_dgb_slots", saved.find_value("target_admission_dgb"));
            options.pushKV("operational_dgb_slots", saved.find_value("target_operational_dgb"));
            options.pushKV("admission_carrier_slots", saved.find_value("target_admission_carriers"));
            options.pushKV("operational_carrier_slots", saved.find_value("target_operational_carriers"));
            qint64 fee{0};
            if (!m_preparation_fee->satoshis(fee) || fee <= 0) throw std::runtime_error("PAYMASTER_RETIREMENT_FEE_LIMIT");
            options.pushKV("maximum_fee_satoshis", fee);
            options.pushKV("dgb_only", !reduction);
            // Bind both release actions to saved targets. Empty or
            // underfilled categories are retained, never refilled here.
            options.pushKV("expected_liquidity_updated_at", saved.find_value("updated_at"));
            if (reduction) {
                options.pushKV("recommend_fee", true);
            }
            options.pushKV("execute", false);
            params.clear(); params.setArray(); params.push_back(options);
        }
        if (task == QLatin1String("release_slot")) {
            const int separator = selected_reserve.lastIndexOf(':');
            UniValue options{UniValue::VOBJ};
            options.pushKV("mode", "release_slot");
            options.pushKV("execute", false);
            options.pushKV("txid", selected_reserve.left(separator).toStdString());
            options.pushKV("vout", selected_reserve.mid(separator + 1).toULongLong());
            params.clear(); params.setArray(); params.push_back(options);
        }
        if (preparation) {
            proposed_policy = PaymasterOperationController::restorationPolicy(context);
            if (!IsCompleteLiquidityPolicy(proposed_policy)) throw std::runtime_error("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND");
            repair_targets = !DigiDollar::Paymaster::SetupMatches(proposed_policy,
                context.find_value("provider").find_value("liquidity").find_value("policy"));
            auto options = params[0];
            options.pushKV("admission_dgb_slots", proposed_policy.find_value("target_admission_dgb"));
            options.pushKV("operational_dgb_slots", proposed_policy.find_value("target_operational_dgb"));
            options.pushKV("admission_carrier_slots", proposed_policy.find_value("target_admission_carriers"));
            options.pushKV("operational_carrier_slots", proposed_policy.find_value("target_operational_carriers"));
            params.clear(); params.setArray(); params.push_back(options);
        }
    } catch (const std::exception& e) { failGuidedTask(QString::fromUtf8(e.what())); return; }
    const std::string command = preparation ? "preparepaymasterpool" : rebalance ? "rebalancepaymasterpool" : "withdrawpaymastercarrier";
    m_panel.call(command, params, false, nullptr, [this, generation, task, params, command, preparation, rebalance, reduction, context, proposed_policy, repair_targets](const UniValue& preview) mutable {
        if (generation != m_panel.walletGeneration() || m_panel.privacyEnabled() || m_guided_task != task) return;
        if (reduction) {
            if (!IsReserveReductionFeeProposal(preview)) {
                failGuidedTask(tr("Core did not return a complete, bounded release fee proposal. No release was approved.")); return;
            }
            auto options = params[0];
            options.pushKV("maximum_fee_satoshis", preview.find_value("maximum_network_fee_satoshis"));
            options.pushKV("recommend_fee", false);
            params.clear(); params.setArray(); params.push_back(options);
        }
        const bool valid = preparation ? IsCompletePoolPreparationResult(preview) : rebalance ?
            IsBoundedReserveReductionResult(preview, params[0]) && (reduction || poolNumber(preview, "retired_carrier_cents") == 0) :
            IsCompleteCarrierWithdrawalResult(preview, task, false, {});
        if (!valid || preview.find_value("executed").isTrue() || preview.find_value("accepted").isTrue()) {
            failGuidedTask(tr("Core did not return a complete current preview. No task was approved.")); return;
        }
        if (rebalance && poolNumber(preview, "retired_dgb_satoshis") == 0 && (!reduction || poolNumber(preview, "retired_carrier_cents") == 0)) {
            m_operation.phase = PaymasterOperationController::Phase::Complete;
            m_operation.title = reduction ? tr("No excess reserves are available above the saved targets") : tr("No excess DGB is available above the saved targets");
            m_guided_task.clear(); m_panel.overview().renderCurrentTask(); return;
        }
        m_operation.review(preview);
        m_guided_review_text = preparation
            ? tr("Create only the missing reserves.\n\nAdditional capital: %1 DGB and %2 DD.\nMaximum setup fees: %3 DGB total; at most %4 DGB per transaction.\n\nExisting and pending reserves are already included. Core will create the approved reserves and wait for confirmations automatically.")
                .arg(PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "total_output_satoshis")),
                     PaymasterFormatDD(poolNumber(preview, "total_carrier_cents")),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "maximum_total_fee_satoshis")),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "maximum_fee_satoshis")))
            : reduction ? tr("The smaller refill target is saved. Release the following excess reserves?\n\nReturn %1 DGB and %2 DD to ordinary wallet funds, before network fees.\nEstimated fees: %5 DGB for the DGB release and %6 DGB for the DD release.\nProposed maximum network fee: %3 DGB per transaction, %4 DGB total. These are one-time release fees; refill approval is unchanged. Only actual transaction fees are spent.\n\nOnly excess available, confirmed reserves are selected. Reserves at or below their saved target are retained; missing reserves are not created by this release. Cancel keeps the smaller target and the existing reserves.")
                .arg(PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "retired_dgb_satoshis")), PaymasterFormatDD(poolNumber(preview, "retired_carrier_cents")),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "maximum_network_fee_satoshis")), PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "maximum_total_fee_satoshis")),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "estimated_dgb_fee_satoshis")), PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "estimated_dd_fee_satoshis")))
            : rebalance ? tr("Return %1 DGB to ordinary wallet funds, less a network fee of at most %2 DGB. Saved targets, DD reserves and refill approval remain unchanged.")
                .arg(PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "retired_dgb_satoshis")), PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "maximum_network_fee_satoshis")))
            : task == QLatin1String("all_excess")
                ? tr("Withdraw %1 DD earnings. Keep %2 DD operating capital. Estimated network fee: %3 DGB.")
                    .arg(PaymasterFormatDD(poolNumber(preview, "withdrawable_excess_cents")), PaymasterFormatDD(poolNumber(preview, "retained_carrier_cents")), PaymasterWidgetImpl::dgbAmount(poolNumber(preview, "estimated_network_fee_satoshis")))
                : tr("Pause the provider, return the selected reserve to ordinary wallet balance and reduce its saved capacity to %1. Released capital will not be replenished automatically.")
                    .arg(poolNumber(preview, "operational_carrier_target"));
        if (repair_targets) {
            const auto& saved = context.find_value("provider").find_value("liquidity").find_value("policy");
            m_guided_review_text.prepend(tr("Restore the DD capacity needed to receive service fees.\nCapacity-check reserves: %1 → %2.\nPayment reserves: %3 → %4.\nPreviously released capital will be recreated only as shown below.\n\n")
                .arg(poolNumber(saved, "target_admission_carriers")).arg(poolNumber(proposed_policy, "target_admission_carriers"))
                .arg(poolNumber(saved, "target_operational_carriers")).arg(poolNumber(proposed_policy, "target_operational_carriers")));
            m_guided_review_text += tr("\n\nAutomatic replenishment: %1. Paid maintenance: %2.\nUnchanged maintenance limits: %3 DGB per transaction, %4 DGB per rolling hour, %5 DGB per rolling day. Customer-payment limits are unchanged.")
                .arg(proposed_policy.find_value("automatic_replenishment").isTrue() ? tr("on") : tr("off"),
                     proposed_policy.find_value("paid_maintenance_approved").isTrue() ? tr("approved") : tr("not approved"),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(proposed_policy, "maximum_maintenance_fee_per_transaction_satoshis")),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(proposed_policy, "maximum_maintenance_fee_per_hour_satoshis")),
                     PaymasterWidgetImpl::dgbAmount(poolNumber(proposed_policy, "maximum_maintenance_fee_per_day_satoshis")));
        }
        if (m_guided_start_after) m_guided_review_text += tr("\nStart the provider automatically when these reserves are ready, within the saved spending limits. Autostart is unchanged.");
        else if (preparation && !m_panel.overview().enabled()) m_guided_review_text += tr("\nEnable the saved provider configuration so Core can finish this task. The saved autostart choice remains in force.");
        m_panel.overview().renderCurrentTask();
        m_panel.deferOperatorReview([this, generation, params, command, task, context, proposed_policy, repair_targets] {
            if (generation != m_panel.walletGeneration() || m_panel.privacyEnabled() || m_guided_task != task || m_panel.busy()) return;
            m_panel.setRpcBusyState(true);
            const auto answer = PaymasterWidgetImpl::askPlainTextQuestion(widget(), m_operation.title, m_guided_review_text);
            if (generation != m_panel.walletGeneration()) return;
            m_panel.setRpcBusyState(false);
            if (answer != QMessageBox::Yes || m_panel.privacyEnabled()) {
                m_operation.reset(m_panel.walletGeneration()); m_guided_task.clear(); m_panel.overview().renderCurrentTask();
                if (task == QLatin1String("reduce_reserves") && !m_panel.privacyEnabled()) m_panel.refreshStatus();
                return;
            }
            UniValue execution{UniValue::VARR};
            UniValue options = params[0];
            options.pushKV("execute", true);
            options.pushKV("plan_id", m_operation.reviewed.find_value("plan_id"));
            execution.push_back(options);
            m_operation.execute();
            m_panel.overview().renderCurrentTask();
            if (repair_targets) {
                applyReviewedReserveRepair(context, proposed_policy, params);
            } else if (task == QLatin1String("release_slot")) {
                UniValue stop{UniValue::VOBJ}, stop_params{UniValue::VARR};
                stop.pushKV("persistent", true); stop.pushKV("pause_setup", true); stop_params.push_back(stop);
                m_panel.call("stoppaymaster", stop_params, false, nullptr, [this, command, execution](const UniValue& result) {
                    if (!result.find_value("running").isFalse()) { failGuidedTask(tr("Core did not confirm the pause. No capital was released.")); return; }
                    executeGuidedTask(command, execution);
                }, false, [this](const QString& error) { failGuidedTask(error); });
            } else {
                executeGuidedTask(command, execution);
            }
        });
        m_panel.showPendingOperatorReview();
    }, false, [this, task](const QString& error) {
        if (!m_panel.liquidity().waitForReserveReduction(error, [this, task] { previewGuidedTask(task); }))
            failGuidedTask(error);
    });
}

void ProviderLiquidity::applyReviewedReserveRepair(const UniValue& context, const UniValue& proposed, const UniValue& preview_params)
{
    using namespace DigiDollar::Paymaster;
    // Preserve all saved approvals. Verify both before writing and after
    // reading the write back, including when its response was lost.
    const auto check_context = [context](const UniValue& current) {
        CheckSetupContext(context, current);
        const auto& before = context.find_value("provider");
        const auto& after = current.find_value("provider");
        for (const char* key : {"policy", "enabled", "autostart", "operation_mode"}) {
            if (!SetupMatches(before.find_value(key), after.find_value(key)))
                throw std::runtime_error("PAYMASTER_SETUP_STATE_CHANGED");
        }
        if (!SetupMatches(context.find_value("safety").find_value("policy"), current.find_value("safety").find_value("policy")))
            throw std::runtime_error("PAYMASTER_SETUP_STATE_CHANGED");
    };
    const auto continue_repair = [this, proposed, preview_params, check_context](const QString& write_error = {}) {
        m_panel.call("getpaymasteroperatorinfo", {}, false, nullptr,
            [this, proposed, preview_params, check_context, write_error](const UniValue& current) {
                try {
                    check_context(current);
                    if (!SetupMatches(proposed, current.find_value("provider").find_value("liquidity").find_value("policy")))
                        throw std::runtime_error("PAYMASTER_SETUP_STATE_CHANGED");
                } catch (const std::exception&) {
                    QString explanation = tr("The saved settings do not match this approval. Check the current status and review a new restoration. No additional preparation was requested.");
                    if (!write_error.isEmpty()) explanation += tr("\nReason: %1").arg(write_error);
                    failGuidedTask(explanation);
                    return;
                }
                m_panel.call("preparepaymasterpool", preview_params, false, nullptr,
                    [this, preview_params](const UniValue& preview) {
                        if (!IsCompletePoolPreparationResult(preview) || preview.find_value("accepted").isTrue() ||
                            preview.find_value("executed").isTrue() ||
                            !PaymasterOperationController::fundingWithinApproval(m_operation.reviewed, preview)) {
                            failGuidedTask(tr("The funding requirement changed. Review a new restoration before approving further preparation. The saved spending limits are unchanged."));
                            return;
                        }
                        // Saving the capacity changes the Core plan identity.
                        // Only a fresh plan within the original bounds may run.
                        m_operation.reviewed = preview;
                        auto options = preview_params[0];
                        options.pushKV("execute", true);
                        options.pushKV("plan_id", preview.find_value("plan_id"));
                        UniValue execution{UniValue::VARR}; execution.push_back(options);
                        executeGuidedTask("preparepaymasterpool", execution);
                    }, false, [this](const QString& error) { failGuidedTask(error); });
            }, false, [this](const QString& error) { failGuidedTask(error); });
    };
    m_panel.call("getpaymasteroperatorinfo", {}, false, nullptr,
        [this, context, proposed, check_context, continue_repair](const UniValue& current) {
            try {
                check_context(current);
                const auto& saved = current.find_value("provider").find_value("liquidity").find_value("policy");
                if (SetupMatches(proposed, saved)) { continue_repair(); return; }
                if (!SetupMatches(context.find_value("provider").find_value("liquidity").find_value("policy"), saved))
                    throw std::runtime_error("PAYMASTER_SETUP_STATE_CHANGED");
            } catch (const std::exception&) {
                failGuidedTask(tr("The provider settings changed during review. Start a new restoration to review the current requirements. No reserve targets were changed."));
                return;
            }
            UniValue params{UniValue::VARR}; params.push_back(proposed);
            m_panel.call("setpaymasterliquiditypolicy", params, false, nullptr,
                [continue_repair](const UniValue&) { continue_repair(); }, false,
                [continue_repair](const QString& error) { continue_repair(error); });
        }, false, [this](const QString& error) { failGuidedTask(error); });
}

void ProviderLiquidity::executeGuidedTask(const std::string& command, const UniValue& params)
{
    const bool preparation = command == "preparepaymasterpool";
    const QString task = m_guided_task;
    const UniValue reviewed = m_operation.reviewed;
    m_panel.call(command, params, preparation || task == QLatin1String("all_excess") || task == QLatin1String("rebalancepaymasterpool") || task == QLatin1String("reduce_reserves"), nullptr,
        [this, preparation, task, reviewed, params](const UniValue& result) {
            const bool release = task == QLatin1String("reduce_reserves") || task == QLatin1String("rebalancepaymasterpool");
            const bool valid = preparation ? IsCompletePoolPreparationResult(result) : release ?
                IsBoundedReserveReductionResult(result, params[0], reviewed) &&
                    (task != QLatin1String("rebalancepaymasterpool") || poolNumber(result, "retired_carrier_cents") == 0) :
                IsCompleteCarrierWithdrawalResult(result, task, true, QString::fromStdString(reviewed.find_value("plan_id").get_str()));
            if (!valid || result.find_value("plan_id").write() != reviewed.find_value("plan_id").write() ||
                !(preparation ? result.find_value("accepted").isTrue() : result.find_value("executed").isTrue())) {
                failGuidedTask(tr("The response did not confirm this exact task. Current saved work will be checked before another attempt.")); return;
            }
            m_operation.accepted();
            if (task == QLatin1String("release_slot")) m_operation.phase = PaymasterOperationController::Phase::Complete;
            m_panel.overview().recordAcceptedTask(m_operation.title);
            m_guided_task.clear();
            m_panel.overview().renderCurrentTask();
            if (preparation && !m_panel.overview().enabled()) {
                UniValue enable{UniValue::VARR}; enable.push_back(true);
                m_panel.call("setpaymasterenabled", enable, false, nullptr, [this](const UniValue& enabled) {
                    if (!enabled.find_value("enabled").isTrue()) { failGuidedTask(tr("The task is saved but the provider configuration is still disabled.")); return; }
                    if (m_guided_start_after) m_panel.overview().requestReviewedStart(); else m_panel.refreshStatus();
                }, false, [this](const QString& error) { failGuidedTask(error); });
            } else if (preparation && m_guided_start_after) m_panel.overview().requestReviewedStart();
            else m_panel.refreshStatus();
        }, false, [this, task, command, params](const QString& error) {
            if (m_panel.liquidity().waitForReserveReduction(error, [this, command, params] { executeGuidedTask(command, params); })) return;
            const bool rejected_before_release = PaymasterQt::StableCode(m_panel.rpcFailure()) == QLatin1String("PAYMASTER_PROVIDER_BUSY") ||
                PaymasterQt::StableCode(m_panel.rpcFailure()) == QLatin1String("PAYMASTER_RETIREMENT_FEE_LIMIT") || PaymasterQt::StableCode(m_panel.rpcFailure()) == QLatin1String("PAYMASTER_RETIREMENT_WALLET_FEE_LIMIT") ||
                PaymasterQt::StableCode(m_panel.rpcFailure()) == QLatin1String("PAYMASTER_POOL_PLAN_CHANGED") || PaymasterQt::StableCode(m_panel.rpcFailure()) == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_CHANGED");
            failGuidedTask(task == QLatin1String("reduce_reserves") && !rejected_before_release
                ? tr("The release was not fully confirmed. Some funds may already have been returned. Check wallet activity before reviewing a fresh plan; do not repeat the old execution.\n%1").arg(error)
                : error);
        });
}

QString ProviderLiquidity::liquidityTargetKey() const
{
    return QStringLiteral("%1:%2:%3:%4")
        .arg(m_admission_dgb->value())
        .arg(m_operational_dgb->value())
        .arg(m_admission_carriers->value())
        .arg(m_operational_carriers->value());
}

QString ProviderLiquidity::preparationTargetKey() const
{
    return liquidityTargetKey() + QLatin1Char(':') + m_preparation_fee->text();
}

void ProviderLiquidity::invalidatePoolPreviews()
{
    m_prepare_preview_target.clear();
    m_rebalance_preview_target.clear();
    m_prepare_plan_id.clear();
    m_rebalance_plan_id.clear();
}

void ProviderLiquidity::restoreLiquidityDefaults(bool announce)
{
    if (announce && !m_liquidity_policy_dirty) m_liquidity_edit_revision = m_saved_liquidity_policy.find_value("updated_at");
    m_loading_liquidity_policy = true;
    m_automatic_replenishment->setChecked(IsCompleteLiquidityPolicy(m_saved_liquidity_policy)
        ? m_saved_liquidity_policy.find_value("automatic_replenishment").isTrue() : true);
    m_paid_maintenance_approved->setChecked(false);
    m_reserve_editor->applyPreset(1);
    m_loading_liquidity_policy = false;
    m_liquidity_policy_dirty = announce;
    invalidatePoolPreviews();
    m_panel.recovery().invalidateCarrierWithdrawalPreviews();
    updateLiquidityDisplay();
    if (announce) {
        m_liquidity_policy_status->setText(tr(
            "Small preset proposed in the form. Paid refill is disabled until you review, approve and save these values."));
        m_liquidity_output->setPlainText(tr(
            "Recommended target fields restored. No wallet output was created or retired. Save the automatic liquidity policy, then preview pool preparation only if you are using manual expert operation."));
    } else {
        m_liquidity_output->clear();
    }
    m_panel.overview().updateProviderButtons();
}

void ProviderLiquidity::updateSavedReserveState()
{
    if (m_reserve_state) {
        const auto target = savedRefillCapacity();
        qint64 available{0};
        const bool known = GetInt64Field(m_panel.overview().operatorSnapshot().find_value("provider").find_value("pool"), "complete_operational_slots", available) && available >= 0;
        QString state = target ? tr("Saved refill target: %1 payment(s).").arg(*target) : tr("Refill target not configured.");
        state += known ? tr(" Available payment reserves: %1.").arg(available) : tr(" Current payment reserves are unavailable.");
        if (hasExcessSavedReserves()) state += tr("\nExtra reserves remain above the saved target. Release requires a separate preview and confirmation.");
        m_reserve_state->setText(m_panel.maskNumericText(state));
        m_release_excess_reserves->setVisible(hasExcessSavedReserves() && !m_panel.privacyEnabled());
        m_release_excess_reserves->setEnabled(!m_panel.busy() && !m_liquidity_policy_dirty && m_panel.hasCompleteMutationSnapshots());
    }
}

void ProviderLiquidity::updateLiquidityDisplay()
{
    if (!m_liquidity_summary || !m_liquidity_preview_status) return;
    m_reserve_editor->synchronize();
    updateSavedReserveState();

    if (m_liquidity_target_save_status) {
        if (m_liquidity_policy_dirty) {
            m_panel.setStatusLabel(
                m_liquidity_target_save_status,
                tr("Not saved — Core is still using the previous wallet targets. Save these values before attempting to refill or start the provider."),
                QStringLiteral("action"));
        } else if (m_liquidity_policy_configured) {
            m_panel.setStatusLabel(
                m_liquidity_target_save_status,
                tr("Saved in this provider wallet."),
                QStringLiteral("ready"));
        } else {
            m_panel.setStatusLabel(
                m_liquidity_target_save_status,
                tr("Not saved yet — review these suggested targets before continuing."),
                QStringLiteral("action"));
        }
    }
    if (m_save_liquidity_policy && m_save_liquidity_policy_primary) {
        const bool needs_approval =
            m_paid_maintenance_approved->isChecked();
        const bool can_save = m_panel.hasRpcTransport() && !m_panel.busy() &&
            m_liquidity_snapshot_representable &&
            (m_liquidity_policy_dirty ||
             !m_liquidity_policy_configured);
        const QString primary_text = needs_approval
            ? tr("Save settings and approve bounded refill…")
            : tr("Save liquidity settings");
        const QString target_text = needs_approval
            ? tr("Save targets and approve refill…")
            : tr("Save liquidity targets");
        m_save_liquidity_policy_primary->setText(primary_text);
        m_save_liquidity_policy_primary->setEnabled(can_save);
        m_save_liquidity_policy->setText(target_text);
        m_save_liquidity_policy->setEnabled(can_save);
    }

    const QString target = liquidityTargetKey();
    const bool preparation_current =
        m_prepare_preview_target == preparationTargetKey() && !m_prepare_plan_id.isEmpty();
    const bool retirement_current =
        m_rebalance_preview_target == target && !m_rebalance_plan_id.isEmpty();
    const bool complete_snapshots = m_panel.hasCompleteMutationSnapshots();
    qint64 preparation_fee{0};
    const bool valid_preparation_fee = m_preparation_fee && m_preparation_fee->satoshis(preparation_fee) && preparation_fee > 0 && preparation_fee <= MAX_MONEY / 2;
    if (m_prepare_preview) {
        m_prepare_preview->setEnabled(
            complete_snapshots && m_liquidity_snapshot_representable &&
            valid_preparation_fee && !m_panel.busy());
    }
    if (m_rebalance_preview) {
        m_rebalance_preview->setEnabled(
            complete_snapshots && m_liquidity_snapshot_representable &&
            !m_panel.busy());
    }
    if (m_prepare_execute) {
        m_prepare_execute->setEnabled(
            complete_snapshots && m_liquidity_snapshot_representable &&
            preparation_current && valid_preparation_fee && !m_panel.busy());
    }
    if (m_rebalance_execute) {
        m_rebalance_execute->setEnabled(
            complete_snapshots && m_liquidity_snapshot_representable &&
            retirement_current && !m_panel.busy());
    }
    if (preparation_current || retirement_current) {
        m_liquidity_preview_status->setText(preparation_current
            ? tr("A preparation preview exists for the current targets.")
            : tr("A retirement preview exists for the current targets."));
    } else {
        m_liquidity_preview_status->setText(tr(
            "No reviewed preview matches the current targets. Preview before executing."));
    }
}

QString ProviderLiquidity::preparationFundingExplanation(const UniValue& step, bool summary) const
{
    const bool dd = ProviderOverview::activityText(step, "asset") == QLatin1String("dd_carrier");
    qint64 required{0}, available{0}, missing{0};
    if (!GetInt64Field(step, dd ? "required_output_cents" : "required_output_satoshis", required) ||
        !GetInt64Field(step, dd ? "confirmed_available_cents" : "confirmed_available_satoshis", available) ||
        !GetInt64Field(step, dd ? "capital_shortfall_cents" : "capital_shortfall_satoshis", missing) ||
        required < 0 || available < 0 || missing != std::max<qint64>(0, required - available) ||
        (!dd && (!MoneyRange(required) || !MoneyRange(available)))) {
        return dd
            ? tr("The last preparation attempt lacked DD reserve capital or DGB for its network fee. Current funding amounts are unavailable. Check this wallet's available DD and DGB balances and the technical details; no reserve transaction has been created.")
            : tr("The last preparation attempt lacked DGB for reserve capital or its network fee. Current funding amounts are unavailable. Check this wallet's available DGB balance and the technical details; no reserve transaction has been created.");
    }
    QString text = dd
        ? tr("Required capital: %1 DD; confirmed available: %2 DD; missing capital: %3 DD.")
              .arg(PaymasterFormatDD(required), PaymasterFormatDD(available), PaymasterFormatDD(missing))
        : tr("Required capital: %1 DGB; confirmed available: %2 DGB; missing capital: %3 DGB.")
              .arg(PaymasterWidgetImpl::dgbAmount(required), PaymasterWidgetImpl::dgbAmount(available), PaymasterWidgetImpl::dgbAmount(missing));
    qint64 pending{0};
    if (dd) {
        if (!GetInt64Field(step, "pending_cents", pending) || pending < 0) pending = 0;
    } else {
        qint64 total{0};
        if (GetInt64Field(step, "available_including_unconfirmed_satoshis", total) &&
            MoneyRange(total) && total >= available) pending = total - available;
    }
    if (summary) {
        if (pending > 0)
            text += tr("Awaiting confirmation: %1 %2.").prepend(QLatin1Char(' ')).arg(dd ? PaymasterFormatDD(pending) : PaymasterWidgetImpl::dgbAmount(pending), dd ? QStringLiteral("DD") : QStringLiteral("DGB"));
        return text;
    }
    if (missing > 0) {
        if (pending >= missing) {
            text += tr(" Incoming funds may cover the missing capital after confirmation. Wait for confirmations; Core rechecks input eligibility before continuing.");
        } else {
            text += dd ? tr(" Receive the missing DD in this wallet, or cancel the uncreated preparation and review a smaller reserve target.")
                       : tr(" Receive the missing DGB plus network fees in this wallet, or cancel the uncreated preparation and review a smaller reserve target.");
        }
    } else {
        text += tr(" Reserve capital is covered. Core still checks network-fee funding and usable inputs on its next automatic attempt.");
    }
    if (pending > 0) {
        text += dd ? tr(" Wallet DD awaiting confirmation: %1 DD.").arg(PaymasterFormatDD(pending))
                   : tr(" Eligible DGB awaiting confirmation: %1 DGB.").arg(PaymasterWidgetImpl::dgbAmount(pending));
        if (m_panel.overview().operatorSnapshot().find_value("network").isStr() &&
            m_panel.overview().operatorSnapshot().find_value("network").get_str() == "regtest")
            text += tr(" On Regtest, mine a block to confirm incoming transactions.");
    }
    if (dd) {
        qint64 dgb{0};
        if (GetInt64Field(step, "confirmed_available_satoshis", dgb) && MoneyRange(dgb))
            text += tr(" Confirmed DGB available for network fees: %1 DGB.").arg(PaymasterWidgetImpl::dgbAmount(dgb));
        text += tr(" This DD preparation also needs a DGB network fee.");
    }
    return text;
}

QString ProviderLiquidity::formatPoolPreparation(const UniValue& records, PreparationDetail detail) const
{
    if (m_panel.privacyEnabled() || records.isNull()) return {};
    if (!records.isArray()) return tr("Pool preparation status is unavailable. Refresh before acting.");
    QStringList lines;
    bool waiting_funding{false};
    for (const auto& step : records.getValues()) {
        const QString state = ProviderOverview::activityText(step, "state");
        if (state == QLatin1String("complete") || state == QLatin1String("cancelled")) continue;
        const QString error = ProviderOverview::activityText(step, "error");
        const QString code = error.section(QLatin1Char(':'), 0, 0);
        const QString asset = ProviderOverview::activityText(step, "asset");
        const QString name = asset == QLatin1String("dd_carrier") ? tr("DigiDollar reserve") : asset == QLatin1String("dgb") ? tr("DGB reserve") :
                                                                                                                               tr("Pool preparation");
        QString explanation;
        if (state == QLatin1String("conflict"))
            explanation = tr("The saved transaction has a conflict. Inspect it before making another plan; automatic replacement is blocked.");
        else if (code == QLatin1String("PAYMASTER_POOL_FEE_LIMIT"))
            explanation = tr("Paused at the fee check. The transaction fee must be valid and within the approved setup limit. Changing recurring refill limits does not change this approval. To review a different limit, cancel the uncreated step, then preview a new plan.");
        else if (code == QLatin1String("PAYMASTER_POOL_FEE_INVALID") || code == QLatin1String("PAYMASTER_POOL_FEE_ESTIMATE_EXCEEDED"))
            explanation = tr("The fee calculation needs review. Raising the approved setup limit does not resolve this check; no replacement is authorized automatically.");
        else if (code == QLatin1String("PAYMASTER_POOL_WAITING_FUNDS") || code == QLatin1String("PAYMASTER_POOL_WAITING_DGB")) {
            explanation = preparationFundingExplanation(step, detail == PreparationDetail::Summary);
            waiting_funding = true;
        } else if (code == QLatin1String("PAYMASTER_POOL_POLICY_CHANGED"))
            explanation = tr("The provider policy changed after approval. Cancel uncreated steps and review a new plan for the current policy.");
        else if (code == QLatin1String("PAYMASTER_WALLET_LOCKED"))
            explanation = tr("Waiting for wallet unlock before creating the approved transaction.");
        else if (code == QLatin1String("PAYMASTER_PROVIDER_DISABLED"))
            explanation = tr("The provider configuration is disabled. Approved preparation can continue when it is enabled; autostart is not required.");
        else if (code == QLatin1String("PAYMASTER_POOL_WAITING_CONFIRMATION") || (error.isEmpty() && state == QLatin1String("pending_confirmation")))
            explanation = state == QLatin1String("pending_creation") ? tr("Waiting for funding inputs to confirm before creating the transaction. On Regtest, mine a block to confirm incoming transactions.") : tr("The saved transaction is waiting for confirmation. In Regtest, a new block must be mined. No new approval is needed.");
        else if (error.isEmpty() && state == QLatin1String("pending_creation"))
            explanation = tr("Approved and waiting for the next automatic check. Core checks approximately every 30 seconds while the provider configuration is enabled and the wallet and index are synchronized.");
        else
            explanation = tr("Preparation needs diagnostic review before continuing.");
        lines << tr("%1: %2").arg(name, explanation);
        if (detail == PreparationDetail::Summary) continue;
        if (state == QLatin1String("pending_creation")) lines << tr("No transaction has been saved for this step.");
        qint64 fee{0};
        if (GetInt64Field(step, "maximum_fee_satoshis", fee) && MoneyRange(fee))
            lines << tr("Approved setup fee limit: %1 DGB. This is a ceiling, not an estimate of the required network fee.").arg(PaymasterWidgetImpl::dgbAmount(fee));
        if (detail == PreparationDetail::Technical && !error.isEmpty()) lines << tr("Last attempt diagnostic: %1").arg(error);
        const QString fee_diagnostic = ProviderOverview::activityText(step, "diagnostic");
        if (detail == PreparationDetail::Technical && !fee_diagnostic.isEmpty()) lines << fee_diagnostic;
        if (detail == PreparationDetail::Technical && IsHex256Field(step, "txid")) lines << tr("Transaction: %1").arg(ProviderOverview::activityText(step, "txid"));
    }
    if (waiting_funding && detail == PreparationDetail::Summary) {
        lines << tr("Network fees are additional. Review funding below to receive missing funds, wait for incoming confirmations, or cancel uncreated work and choose smaller targets.");
    } else if (waiting_funding) {
        lines << tr("Available funds exclude protected reserves and are shared by the pending steps.");
        lines << tr("Preparation continues automatically within the saved approval when the provider configuration is enabled and the wallet is unlocked and synchronized. Automatic refill does not need to be enabled.");
    }
    return lines.join(QLatin1Char('\n'));
}

UniValue ProviderLiquidity::cancellableMaintenance() const
{
    UniValue ids{UniValue::VARR};
    if (m_operation.busy()) return ids;
    const auto& work = m_panel.overview().operatorSnapshot().find_value("provider").find_value("active_operations");
    if (!work.isArray()) return ids;
    for (const auto& item : work.getValues()) {
        const QString kind = ProviderOverview::activityText(item, "kind");
        if ((kind == QLatin1String("maintenance") || kind == QLatin1String("preparation")) &&
            ProviderOverview::activityText(item, "state") == QLatin1String("pending_creation") &&
            item.find_value("txid").isNull() && IsHex256Field(item, "operation_id"))
            ids.push_back(item.find_value("operation_id"));
    }
    return ids;
}

void ProviderLiquidity::cancelMaintenance()
{
    if (m_panel.privacyEnabled() || m_panel.busy() || !m_panel.hasCompleteMutationSnapshots() || !m_panel.hasRpcTransport()) return;
    const UniValue ids = cancellableMaintenance();
    if (ids.empty()) return;
    const auto generation = m_panel.walletGeneration();
    UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
    options.pushKV("operation_ids", ids);
    params.push_back(options);
    m_panel.call("cancelpaymastermaintenance", params, false, nullptr,
        [this, generation, ids, options](const UniValue& review) mutable {
            qint64 budget{0};
            if (!review.find_value("executed").isFalse() || !IsHex256Field(review, "plan_id") ||
                review.find_value("operation_ids").write() != ids.write() ||
                !GetInt64Field(review, "released_fee_budget_satoshis", budget) || !MoneyRange(budget)) {
                failGuidedTask(tr("The cancellation preview could not be verified. No cancellation was approved."));
                return;
            }
            m_panel.setRpcBusyState(true);
            const auto answer = PaymasterWidgetImpl::askPlainTextQuestion(widget(), tr("Cancel uncreated reserve work"),
                tr("Cancel %n uncreated reserve-funding operation(s)?", nullptr, ids.size()) +
                tr("\nAutomatic refill will be turned off so these operations are not recreated. Provider enablement and autostart stay unchanged. Existing reserves, payments and signed transactions are retained.\n\nReleased fee allowance: %1 DGB. This is unused permission, not a payment or refund.").arg(PaymasterWidgetImpl::dgbAmount(budget)));
            if (generation != m_panel.walletGeneration()) return;
            m_panel.setRpcBusyState(false);
            if (answer != QMessageBox::Yes || m_panel.privacyEnabled() || ids.write() != cancellableMaintenance().write()) return;
            const auto plan = review.find_value("plan_id");
            options.pushKV("execute", true);
            options.pushKV("plan_id", plan);
            UniValue execute{UniValue::VARR}; execute.push_back(options);
            m_panel.call("cancelpaymastermaintenance", execute, false, nullptr,
                [this, ids, plan, budget](const UniValue& result) {
                    qint64 released{0};
                    if (!result.find_value("executed").isTrue() || !result.find_value("automatic_refill_disabled").isTrue() ||
                        result.find_value("plan_id").write() != plan.write() || result.find_value("operation_ids").write() != ids.write() ||
                        !GetInt64Field(result, "released_fee_budget_satoshis", released) || released != budget) {
                        failGuidedTask(tr("Cancellation was not confirmed. Refresh the current status before another action."));
                        return;
                    }
                    m_operation.reset(m_panel.walletGeneration());
                    m_operation.title = tr("Uncreated reserve work cancelled; automatic refill is off");
                    m_operation.phase = PaymasterOperationController::Phase::Complete;
                    m_guided_task.clear();
                    m_failed_guided_task.clear();
                    ++m_guided_generation;
                    // Wait for a fresh snapshot before offering the same IDs again.
                    m_panel.overview().invalidateOperatorSnapshot();
                    invalidatePoolPreviews();
                    m_panel.overview().renderCurrentTask();
                    m_panel.refreshStatus();
                }, false, [this](const QString& error) { failGuidedTask(error); });
        }, false, [this](const QString& error) { failGuidedTask(error); });
}

QString ProviderLiquidity::cancellablePreparationPlan() const
{
    if (!m_pool_preparation.isArray()) return {};
    QString plan;
    for (const auto& step : m_pool_preparation.getValues()) {
        const QString state = ProviderOverview::activityText(step, "state");
        if (state == QLatin1String("complete") || state == QLatin1String("cancelled")) continue;
        // Only offer cancellation for one wholly uncreated plan. Core
        // reconciles again before cancelling; a concurrent saved signature
        // can never be discarded by this presentation-layer check.
        if (state != QLatin1String("pending_creation") || !step.find_value("txid").isNull() ||
            !IsHex256Field(step, "plan_id")) return {};
        const QString current = ProviderOverview::activityText(step, "plan_id");
        if (!plan.isEmpty() && plan != current) return {};
        plan = current;
    }
    return plan;
}

void ProviderLiquidity::updatePoolPreparationStatus()
{
    if (!m_pool_preparation_status) return;
    const QString details = formatPoolPreparation(m_pool_preparation);
    m_pool_preparation_status->setText(details);
    m_pool_preparation_status->setVisible(!m_panel.privacyEnabled() && !details.isEmpty());
    if (!details.isEmpty()) {
        m_panel.overview().showPendingPreparation();
        m_liquidity_maintenance_state->setText(tr("Finite pool preparation takes priority over recurring refill"));
        m_liquidity_maintenance_next_step->setText(details);
    }
    const bool cancellable = !m_panel.privacyEnabled() && !cancellablePreparationPlan().isEmpty();
    m_cancel_preparation->setVisible(cancellable);
    m_cancel_preparation->setEnabled(cancellable && m_panel.hasCompleteMutationSnapshots() && m_panel.hasRpcTransport() && !m_panel.busy());
}

bool ProviderLiquidity::canReviewPreparationFee() const
{
    if (cancellablePreparationPlan().isEmpty()) return false;
    for (const auto& step : m_pool_preparation.getValues()) {
        if (ProviderOverview::activityText(step, "state") == QLatin1String("pending_creation") &&
            ProviderOverview::activityText(step, "error").section(QLatin1Char(':'), 0, 0) == QLatin1String("PAYMASTER_POOL_FEE_LIMIT")) return true;
    }
    return false;
}

void ProviderLiquidity::cancelPoolPreparation(bool review_replacement)
{
    if (m_panel.privacyEnabled() || m_panel.busy() || !m_panel.hasCompleteMutationSnapshots() || !m_panel.hasRpcTransport()) return;
    const QString plan = cancellablePreparationPlan();
    if (plan.isEmpty()) return;
    const auto generation = m_panel.walletGeneration();
    qint64 replacement_fee{0};
    bool start_after = m_guided_start_after || m_panel.overview().operatorSnapshot().find_value("provider").find_value("start_requested").isTrue();
    if (review_replacement) {
        if (!canReviewPreparationFee()) return;
        for (const auto& step : m_pool_preparation.getValues()) {
            qint64 fee{0};
            if (ProviderOverview::activityText(step, "state") == QLatin1String("pending_creation") &&
                GetInt64Field(step, "maximum_fee_satoshis", fee) && MoneyRange(fee)) replacement_fee = std::max(replacement_fee, fee);
        }
        QDialog dialog(widget());
        dialog.setObjectName("paymasterReviewPreparationFee");
        dialog.setWindowTitle(tr("Review setup fee and continue"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* explanation = new QLabel(tr("The reserve could not be created within the approved fee limit of %1 DGB per transaction. No transaction was created for this step.\n\nChoose a new limit below. Continue closes only the uncreated plan and shows the replacement for your approval. Recurring maintenance limits stay unchanged.").arg(PaymasterWidgetImpl::dgbAmount(replacement_fee)), &dialog);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        QStringList fee_details;
        for (const auto& step : m_pool_preparation.getValues()) {
            const QString detail = ProviderOverview::activityText(step, "diagnostic");
            if (!detail.isEmpty() && !fee_details.contains(detail)) fee_details << detail;
        }
        if (!fee_details.isEmpty()) {
            auto* estimate = new QLabel(fee_details.join(QLatin1Char('\n')), &dialog);
            estimate->setWordWrap(true);
            estimate->setTextFormat(Qt::PlainText);
            layout->addWidget(estimate);
        }
        auto* details = new QPushButton(tr("Technical details"), &dialog);
        details->setCheckable(true);
        layout->addWidget(details, 0, Qt::AlignLeft);
        auto* diagnostic = new QLabel(formatPoolPreparation(m_pool_preparation), &dialog);
        diagnostic->setWordWrap(true);
        diagnostic->setTextFormat(Qt::PlainText);
        layout->addWidget(diagnostic);
        diagnostic->hide();
        connect(details, &QPushButton::toggled, diagnostic, &QWidget::setVisible);
        layout->addWidget(new QLabel(tr("Maximum setup fee per transaction (DGB)"), &dialog));
        auto* fee = new DgbAmountLineEdit(replacement_fee, &dialog);
        fee->setObjectName("paymasterReplacementSetupFee");
        layout->addWidget(fee);
        auto* start = new QCheckBox(tr("Start provider when the new reserves are ready (autostart stays unchanged)"), &dialog);
        start->setChecked(start_after);
        layout->addWidget(start);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto* proceed = buttons->addButton(tr("Continue to review"), QDialogButtonBox::AcceptRole);
        connect(fee, &QLineEdit::textChanged, &dialog, [fee, proceed] {
            qint64 amount{0}; proceed->setEnabled(fee->satoshis(amount) && MoneyRange(amount) && amount > 0);
        });
        proceed->setEnabled(replacement_fee > 0);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(620, dialog.sizeHint().height());
        m_panel.setRpcBusyState(true);
        const int decision = dialog.exec();
        if (generation != m_panel.walletGeneration()) return;
        m_panel.setRpcBusyState(false);
        if (decision != QDialog::Accepted || m_panel.privacyEnabled() || !fee->satoshis(replacement_fee) || !MoneyRange(replacement_fee) || replacement_fee <= 0) return;
        start_after = start->isChecked();
    } else if (PaymasterWidgetImpl::askPlainTextQuestion(widget(), tr("Cancel pool preparation"),
                             tr("Revoke the uncreated pool preparation below? Core rechecks for saved transactions before cancelling. Existing pool outputs and any saved transaction remain unchanged. A new fee limit requires a fresh preview and approval.\n\n%1")
                                 .arg(formatPoolPreparation(m_pool_preparation))) != QMessageBox::Yes) return;
    if (m_panel.privacyEnabled() || m_panel.busy() || generation != m_panel.walletGeneration() || plan != cancellablePreparationPlan()) return;
    UniValue options{UniValue::VOBJ};
    // The RPC validates target options even for cancellation. Use valid
    // counts for the displayed funding model; these do not authorize any
    // creation when cancel=true. Core still binds cancellation to plan_id.
    options.pushKV("admission_dgb_slots", std::max(3, m_admission_dgb->value()));
    options.pushKV("operational_dgb_slots", std::max(1, m_operational_dgb->value()));
    options.pushKV("admission_carrier_slots", m_panel.configuration().userPaidSelected() ? std::max(3, m_admission_carriers->value()) : 0);
    options.pushKV("operational_carrier_slots", m_panel.configuration().userPaidSelected() ? std::max(1, m_operational_carriers->value()) : 0);
    options.pushKV("execute", true);
    options.pushKV("cancel", true);
    options.pushKV("plan_id", plan.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    m_panel.call("preparepaymasterpool", std::move(params), false, nullptr,
         [this, plan, generation, review_replacement, replacement_fee, start_after](const UniValue& result) {
             const bool valid = ProviderOverview::activityText(result, "plan_id") == plan &&
                                result.find_value("cancelled").isBool() && result.find_value("preparation").isArray();
             m_liquidity_output->setPlainText(valid && result.find_value("cancelled").isTrue() ? tr("Uncreated preparation cancelled. Review the targets and setup fee limit, then preview a new plan. No new preparation has been authorized.") : tr("Cancellation was not confirmed. Refresh the preparation status before another action; a saved transaction may still be pending."));
             invalidatePoolPreviews();
             m_pool_preparation = valid ? result.find_value("preparation") : UniValue{};
             updatePoolPreparationStatus();
             if (review_replacement && valid && result.find_value("cancelled").isTrue() &&
                 std::all_of(result.find_value("preparation").getValues().begin(), result.find_value("preparation").getValues().end(), [](const UniValue& step) {
                     const auto& state = step.find_value("state");
                     return state.isStr() && (state.get_str() == "cancelled" || state.get_str() == "complete");
                 })) {
                 m_preparation_fee->setSatoshis(replacement_fee);
                 m_operation.reset(m_panel.walletGeneration());
                 m_guided_task.clear();
                 beginGuidedTask("preparepaymasterpool", start_after);
             } else {
                 if (review_replacement) m_operation.fail(tr("The old plan could not be safely closed. Refresh its status before retrying; no replacement was authorized."));
                 m_panel.refreshStatus();
             }
         });
}

qint64 ProviderLiquidity::poolNumber(const UniValue& result, const char* name)
{
    qint64 value{0};
    return GetInt64Field(result, name, value) ? value : 0;
}

QString ProviderLiquidity::formatPoolResult(const char* command, const UniValue& result) const
{
    const bool executed = result.find_value("executed").isBool() &&
                          result.find_value("executed").get_bool();
    const bool accepted = result.find_value("accepted").isTrue();
    QStringList lines;
    if (std::string{command} == "preparepaymasterpool") {
        lines.push_back(accepted
            ? tr("Pool preparation accepted. Remaining steps continue automatically when the provider configuration is enabled, funding is confirmed and the wallet is unlocked. This does not start the provider.")
            : executed
                ? tr("Preparation executed: the required pool transaction(s) were created. Wait for confirmation before expecting provider readiness.")
                : tr("Preparation preview only: no wallet funds were moved."));
        const QString progress = formatPoolPreparation(result.find_value("preparation"));
        if (!progress.isEmpty()) lines << progress;
        if (result.find_value("maximum_total_fee_satoshis").isNum()) {
            lines.push_back(tr("Maximum approved setup fees: %1 DGB. Execution authorizes later automatic completion within this limit.")
                .arg(PaymasterFormatDGB(poolNumber(result, "maximum_total_fee_satoshis"))));
        }
        lines.push_back(tr(
            "Missing outputs to create — admission DGB: %1; operational DGB: %2; admission carriers: %3; operational carriers: %4.")
            .arg(poolNumber(result, "missing_admission_dgb_slots"))
            .arg(poolNumber(result, "missing_operational_dgb_slots"))
            .arg(poolNumber(result, "missing_admission_carrier_slots"))
            .arg(poolNumber(result, "missing_operational_carrier_slots")));
        lines.push_back(tr(
            "Value per output — admission DGB: %1 DGB; operational DGB: %2 DGB; DD carrier: %3 DD.")
            .arg(PaymasterFormatDGB(poolNumber(result, "admission_dgb_satoshis_each")),
                 PaymasterFormatDGB(poolNumber(result, "operational_dgb_satoshis_each")),
                 PaymasterFormatDD(poolNumber(result, "carrier_cents_each"))));
        lines.push_back(tr("New pool total — %1 DGB and %2 DD.")
            .arg(PaymasterFormatDGB(poolNumber(result, "total_output_satoshis")),
                 PaymasterFormatDD(poolNumber(result, "total_carrier_cents"))));
    } else {
        lines.push_back(executed
            ? tr("Retirement executed: selected excess available liquidity is being returned to the wallet.")
            : tr("Retirement preview only: no wallet funds were moved."));
        lines.push_back(tr(
            "Outputs selected for retirement — admission DGB: %1; operational DGB: %2; admission carriers: %3; operational carriers: %4.")
            .arg(poolNumber(result, "retired_admission_dgb_slots"))
            .arg(poolNumber(result, "retired_operational_dgb_slots"))
            .arg(poolNumber(result, "retired_admission_carrier_slots"))
            .arg(poolNumber(result, "retired_operational_carrier_slots")));
        lines.push_back(tr("Returned before network fee — %1 DGB and %2 DD.")
            .arg(PaymasterFormatDGB(poolNumber(result, "retired_dgb_satoshis")),
                 PaymasterFormatDD(poolNumber(result, "retired_carrier_cents"))));
    }
    if (result.find_value("network_fee_satoshis").isNum()) {
        lines.push_back(tr("DGB network fee paid: %1 DGB.")
            .arg(PaymasterFormatDGB(poolNumber(result, "network_fee_satoshis"))));
    }
    const UniValue& dgb_txid = result.find_value("dgb_txid");
    const UniValue& dd_txid = result.find_value("dd_txid");
    if (dgb_txid.isStr()) lines.push_back(tr("DGB transaction: %1").arg(QString::fromStdString(dgb_txid.get_str())));
    if (dd_txid.isStr()) lines.push_back(tr("DD transaction: %1").arg(QString::fromStdString(dd_txid.get_str())));
    if (!executed && !accepted) {
        lines.push_back(tr(
            "Review these values. If they are acceptable, use the matching Execute reviewed action without changing the targets."));
    }
    return lines.join('\n');
}

UniValue ProviderLiquidity::poolOptions(bool execute, const QString& plan_id, bool preparation) const
{
    UniValue options{UniValue::VOBJ};
    options.pushKV("admission_dgb_slots", m_admission_dgb->value());
    options.pushKV("operational_dgb_slots", m_operational_dgb->value());
    options.pushKV("admission_carrier_slots", m_admission_carriers->value());
    options.pushKV("operational_carrier_slots", m_operational_carriers->value());
    if (preparation) {
        qint64 fee{0};
        if (!m_preparation_fee->satoshis(fee) || fee <= 0 || fee > MAX_MONEY / 2) throw std::runtime_error("PAYMASTER_POOL_VALUE_OUT_OF_RANGE");
        options.pushKV("maximum_fee_satoshis", fee);
    }
    options.pushKV("execute", execute);
    if (execute && !plan_id.isEmpty()) {
        options.pushKV("plan_id", plan_id.toStdString());
    }
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    return params;
}

void ProviderLiquidity::poolAction(const char* command, bool)
{
    beginGuidedTask(QString::fromLatin1(command));
}

void ProviderLiquidity::buildLiquidity(PageBindings& pages)
{
    pages.liquidity_scroll = new QScrollArea(m_panel.mainPages());
    pages.liquidity_scroll->setObjectName("paymasterLiquidityPage");
    pages.liquidity_scroll->setWidgetResizable(true);
    auto* liquidity = new QWidget(pages.liquidity_scroll);
    liquidity->setObjectName("paymasterLiquidityContents");

    pages.liquidity_column = CreatePaymasterPageColumn(liquidity, pages.liquidity_layout);
    AddPaymasterPageHeading(
        pages.liquidity_layout, pages.liquidity_column, tr("Reserves & refill"),
        tr("Keep payment capacity ready. DGB and DD reserves remain your wallet's capital; creating or refilling them can cost a separate DGB network fee. Customer-payment fees use their own budgets."),
        QStringLiteral("paymasterLiquidity"));


    pages.funds_metrics = new PaymasterResponsiveCards(pages.liquidity_column, 3);
    pages.funds_metrics->setObjectName("paymasterFundsMetrics");
    const QStringList funds_titles{tr("Available payment reserves"), tr("Available DGB reserves"), tr("DD reserve capital")};
    const QStringList funds_keys{QStringLiteral("capacity"), QStringLiteral("dgb_available"), QStringLiteral("carrier_base")};
    const QStringList funds_help{tr("Prepared capacity; accepting a payment also requires a running provider and sufficient budgets."),
        tr("Wallet-owned DGB prepared for network fees. This is capital, not spending."),
        tr("Reusable base capital for receiving service fees. Earnings are shown separately below.")};
    for (int i = 0; i < funds_keys.size(); ++i) {
        auto* card = new QFrame(pages.funds_metrics);
        card->setProperty("paymasterRole", QStringLiteral("statusCard"));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->addWidget(ProviderFinance::financeLabel(funds_titles.at(i), card, "financeRowTitle"));
        layout->addWidget(m_panel.finance().fundsValue(funds_keys.at(i), card, funds_titles.at(i), "financeAmount"));
        if (i == 0) layout->addWidget(m_panel.finance().fundsValue("target", card, tr("Saved refill target"), "mutedText"));
        if (i == 1) {
            layout->addWidget(m_panel.finance().fundsValue("dgb_reserved", card, tr("DGB reserves in use"), "mutedText"));
            layout->addWidget(m_panel.finance().fundsValue("dgb_pending", card, tr("DGB reserves awaiting confirmation"), "mutedText"));
        }
        layout->addWidget(ProviderFinance::financeLabel(funds_help.at(i), card, "mutedText"));
        layout->addStretch();
        pages.funds_metrics->addCard(card);
    }
    pages.liquidity_layout->addWidget(pages.funds_metrics);

    m_pool_preparation_status = new QLabel(pages.liquidity_column);
    m_pool_preparation_status->setObjectName("paymasterPoolPreparationStatus");
    m_pool_preparation_status->setTextFormat(Qt::PlainText);
    m_pool_preparation_status->setWordWrap(true);
    m_pool_preparation_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pool_preparation_status->hide();
    pages.liquidity_layout->addWidget(m_pool_preparation_status);
    m_cancel_preparation = new QPushButton(tr("Cancel uncreated pool preparation…"), pages.liquidity_column);
    m_cancel_preparation->setObjectName("paymasterCancelPoolPreparation");
    m_cancel_preparation->hide();
    pages.task_layout->addWidget(m_cancel_preparation);
    connect(m_cancel_preparation, &QPushButton::clicked, widget(), [this] { cancelPoolPreparation(); });

    auto* capacity_details = new QWidget(pages.liquidity_column);
    auto* capacity_details_layout = new QVBoxLayout(capacity_details);
    capacity_details_layout->setContentsMargins(0, 0, 0, 0);
    // Current counts and saved targets never follow unsaved form edits.
    auto* counts = new QGroupBox(tr("Reserves by purpose"), capacity_details);
    counts->setObjectName("paymasterFundsReserveCounts");
    counts->setProperty("paymasterRole", QStringLiteral("card"));
    auto* counts_grid = new QGridLayout(counts);
    counts_grid->setHorizontalSpacing(16);
    counts_grid->setVerticalSpacing(8);
    const QStringList count_headers{tr("Reserve"), tr("Ready"), tr("In use"), tr("Pending"), tr("Target")};
    const QStringList count_rows{tr("DGB · capacity checks"), tr("DGB · payments"), tr("DD · capacity checks"), tr("DD · payments")};
    const QStringList count_keys{QStringLiteral("admission_dgb"), QStringLiteral("operational_dgb"), QStringLiteral("admission_carriers"), QStringLiteral("operational_carriers")};
    const QStringList count_fields{QStringLiteral("ready"), QStringLiteral("reserved"), QStringLiteral("pending"), QStringLiteral("target")};
    for (int column = 0; column < count_headers.size(); ++column) {
        auto* heading = ProviderFinance::financeLabel(count_headers.at(column), counts, "financeColumnHeading");
        heading->setAlignment((column ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        counts_grid->addWidget(heading, 0, column);
        counts_grid->setColumnStretch(column, column ? 1 : 3);
    }
    for (int row = 0; row < count_rows.size(); ++row) {
        counts_grid->addWidget(ProviderFinance::financeLabel(count_rows.at(row), counts, "financeRowTitle"), row + 1, 0);
        for (int column = 0; column < count_fields.size(); ++column) {
            auto* value = m_panel.finance().fundsValue(count_keys.at(row) + QLatin1Char('_') + count_fields.at(column), counts,
                count_rows.at(row) + QStringLiteral(" · ") + count_headers.at(column + 1));
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            counts_grid->addWidget(value, row + 1, column + 1);
        }
    }
    capacity_details_layout->addWidget(counts);

    auto* capacity_explanation = new QLabel(tr(
        "Why two counts? Before making an offer, Core proves to a client that this provider has dedicated funds. Separate payment capacity is then kept ready for accepted transfers. “Ready” is available now; “target” is the amount Core maintains. These funds always remain under this wallet's control."),
        pages.liquidity_column);
    capacity_explanation->setObjectName("paymasterLiquidityCapacityExplanation");
    capacity_explanation->setProperty("paymasterRole", QStringLiteral("mutedText"));
    capacity_explanation->setWordWrap(true);
    capacity_details_layout->addWidget(capacity_explanation);
    AddPaymasterDisclosure(pages.liquidity_layout, pages.liquidity_column, capacity_details,
                           tr("View reserve counts and targets"), tr("Hide reserve counts and targets"), QStringLiteral("paymasterCapacityDetailsToggle"));
    auto* liquidity_steps = new QLabel(tr(
        "Choose an action above. You will review the exact amounts and fees before anything changes. Confirmed reserves become available automatically. Individual reserve types and manual controls are under Advanced."), liquidity);
    liquidity_steps->setObjectName("paymasterLiquiditySetupSteps");
    liquidity_steps->setWordWrap(true);
    pages.liquidity_layout->addWidget(liquidity_steps);

    pages.automatic_maintenance = new QGroupBox(
        tr("Reserve target and refill costs"), pages.liquidity_column);
    pages.automatic_maintenance->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    pages.automatic_maintenance->setProperty("paymasterRole", QStringLiteral("card"));
    pages.automatic_maintenance->setObjectName(
        "paymasterAutomaticLiquidityPolicy");
    auto* automatic_maintenance_layout = new QFormLayout(
        pages.automatic_maintenance);
    auto* automatic_maintenance_help = new QLabel(tr(
        "Choose how many complete payment reserves to keep ready, then review the required capital and separate refill-fee limits. Editing this form does not change saved settings until you save."),
        pages.automatic_maintenance);
    automatic_maintenance_help->setWordWrap(true);
    automatic_maintenance_layout->addRow(automatic_maintenance_help);
    m_automatic_replenishment = new QCheckBox(
        tr("Automatically refill missing reserves while this provider is running"),
        pages.automatic_maintenance);
    m_automatic_replenishment->setObjectName(
        "paymasterAutomaticReplenishment");
    m_automatic_replenishment->setChecked(true);
    m_paid_maintenance_approved = new QCheckBox(tr(
        "Approve refill fees within the limits below"),
        pages.automatic_maintenance);
    m_paid_maintenance_approved->setObjectName(
        "paymasterPaidMaintenanceApproved");
    m_paid_maintenance_approved->setToolTip(tr("Only the actual network fees of reserve refill are paid. These limits are additional to customer-payment budgets. Fragmented funds can require a higher transaction limit; Core never raises it automatically."));
    const auto refill_defaults = DigiDollar::Paymaster::SetupDefaultLiquidity(true);
    m_maintenance_fee_per_transaction = new DgbAmountLineEdit(0, pages.automatic_maintenance);
    m_maintenance_fee_per_transaction->setSatoshis(refill_defaults.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>());
    m_maintenance_fee_per_transaction->setObjectName(
        "paymasterMaintenanceFeePerTransaction");
    m_maintenance_fee_per_hour = new DgbAmountLineEdit(0, pages.automatic_maintenance);
    m_maintenance_fee_per_hour->setSatoshis(refill_defaults.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>());
    m_maintenance_fee_per_hour->setObjectName(
        "paymasterMaintenanceFeePerHour");
    m_maintenance_fee_per_day = new DgbAmountLineEdit(0, pages.automatic_maintenance);
    m_maintenance_fee_per_day->setSatoshis(refill_defaults.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>());
    m_maintenance_fee_per_day->setObjectName(
        "paymasterMaintenanceFeePerDay");
    m_liquidity_policy_status = new QLabel(tr(
        "Liquidity policy has not been loaded yet."), pages.automatic_maintenance);
    m_liquidity_policy_status->setObjectName(
        "paymasterLiquidityPolicyStatus");
    m_liquidity_policy_status->setWordWrap(true);
    m_liquidity_policy_status->setTextFormat(Qt::PlainText);
    automatic_maintenance_layout->addRow(m_automatic_replenishment);
    automatic_maintenance_layout->addRow(m_liquidity_policy_status);
    m_maintenance_limits_toggle = new QPushButton(
        tr("Set optional paid-refill limits"), pages.automatic_maintenance);
    m_maintenance_limits_toggle->setObjectName(
        "paymasterMaintenanceLimitsToggle");
    m_maintenance_limits_toggle->setProperty(
        "paymasterRole", QStringLiteral("disclosure"));
    m_maintenance_limits_toggle->setCheckable(true);
    auto* maintenance_limits_panel = new QWidget(pages.automatic_maintenance);
    maintenance_limits_panel->setObjectName("paymasterMaintenanceLimitsPanel");
    auto* maintenance_limits_form = new QFormLayout(maintenance_limits_panel);
    maintenance_limits_form->setContentsMargins(0, 6, 0, 0);
    maintenance_limits_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    maintenance_limits_form->addRow(m_paid_maintenance_approved);
    auto* refill_help = new QLabel(tr("Core reuses confirmed reserves first. Refill fees are separate from reserve capital and customer-payment fees. Unused allowances are not spent."), maintenance_limits_panel);
    refill_help->setWordWrap(true);
    maintenance_limits_form->addRow(refill_help);
    maintenance_limits_form->addRow(
        tr("Maximum fee per transaction (DGB):"),
        m_maintenance_fee_per_transaction);
    maintenance_limits_form->addRow(
        tr("Maximum fees per rolling hour (DGB):"),
        m_maintenance_fee_per_hour);
    maintenance_limits_form->addRow(
        tr("Maximum fees per rolling day (DGB):"),
        m_maintenance_fee_per_day);
    maintenance_limits_panel->setVisible(true);
    m_maintenance_limits_toggle->hide();
    automatic_maintenance_layout->addRow(maintenance_limits_panel);
    m_save_liquidity_policy_primary = new QPushButton(
        tr("Save liquidity settings"), pages.automatic_maintenance);
    m_save_liquidity_policy_primary->setObjectName(
        "paymasterSaveLiquidityPolicyPrimary");
    m_save_liquidity_policy_primary->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    m_save_liquidity_policy_primary->setToolTip(tr(
        "Save the automatic-refill setting, finite maintenance limits and displayed pool targets. A running provider may immediately refill within the approved limits; saving does not start a stopped provider."));
    automatic_maintenance_layout->addRow(m_save_liquidity_policy_primary);
    auto* discard_liquidity = new QPushButton(tr("Discard reserve changes"), pages.automatic_maintenance);
    discard_liquidity->setObjectName("paymasterDiscardLiquidity");
    automatic_maintenance_layout->addRow(discard_liquidity);
    connect(discard_liquidity, &QPushButton::clicked, widget(), [this] {
        if (!m_panel.busy() && IsCompleteLiquidityPolicy(m_saved_liquidity_policy))
            loadLiquidityPolicy(m_saved_liquidity_policy, true);
    });
    connect(m_maintenance_limits_toggle, &QPushButton::toggled,
            maintenance_limits_panel,
            [this, maintenance_limits_panel](bool visible) {
                maintenance_limits_panel->setVisible(visible);
                m_maintenance_limits_toggle->setText(
                    visible ? tr("Hide paid-refill limits")
                            : tr("Set optional paid-refill limits"));
            });
    pages.liquidity_layout->addWidget(pages.automatic_maintenance);

    auto* targets = new QGroupBox(tr("Reserve target"), liquidity);
    auto* targets_layout = new QVBoxLayout(targets);
    m_liquidity_current_status = new QLabel(
        tr("Current confirmed pool: status not loaded yet."), targets);
    m_liquidity_current_status->setObjectName("paymasterLiquidityCurrentStatus");
    m_liquidity_current_status->setWordWrap(true);
    m_admission_dgb = PaymasterWidgetImpl::spin(liquidity, 3, 16, 3);
    m_admission_dgb->setObjectName("paymasterAdmissionDgbSlots");
    m_operational_dgb = PaymasterWidgetImpl::spin(liquidity, 1, 16, 1);
    m_operational_dgb->setObjectName("paymasterOperationalDgbSlots");
    m_admission_carriers = PaymasterWidgetImpl::spin(liquidity, 0, 16, 3);
    m_admission_carriers->setObjectName("paymasterAdmissionCarrierSlots");
    m_operational_carriers = PaymasterWidgetImpl::spin(liquidity, 0, 16, 1);
    m_operational_carriers->setObjectName("paymasterOperationalCarrierSlots");
    m_admission_dgb->setToolTip(tr("At least three confirmed admission DGB slots are required."));
    m_operational_dgb->setToolTip(tr("Each operational DGB slot can support one concurrent transfer."));
    m_admission_carriers->setToolTip(tr("User-paid providers require at least three admission DD carriers; sponsored-only providers need none."));
    m_operational_carriers->setToolTip(tr("User-paid transfers require an operational DD carrier paired with operational DGB liquidity."));
    m_reserve_editor = new PaymasterReserveEditor(targets, QStringLiteral("paymaster"),
        {m_admission_dgb, m_operational_dgb, m_admission_carriers, m_operational_carriers},
        {m_maintenance_fee_per_transaction, m_maintenance_fee_per_hour, m_maintenance_fee_per_day},
        [this] { return m_panel.overview().confirmedUserPaid(); }, [this] { return m_panel.overview().confirmedPaymentFee(); });
    targets_layout->addWidget(m_reserve_editor);
    m_reserve_editor->addManualHelp(m_liquidity_current_status);
    m_liquidity_summary = m_reserve_editor->summaryLabel();
    m_liquidity_summary->setObjectName("paymasterLiquidityTargetSummary");
    m_reserve_state = new QLabel(targets);
    m_reserve_state->setObjectName("paymasterReserveState");
    m_reserve_state->setWordWrap(true);
    targets_layout->addWidget(m_reserve_state);
    m_release_excess_reserves = new QPushButton(tr("Review excess reserves…"), targets);
    m_release_excess_reserves->setObjectName("paymasterReviewExcessReserves");
    targets_layout->addWidget(m_release_excess_reserves, 0, Qt::AlignLeft);
    connect(m_release_excess_reserves, &QPushButton::clicked, widget(), [this] { beginGuidedTask("reduce_reserves"); });
    m_liquidity_target_save_status = new QLabel(targets);
    m_liquidity_target_save_status->setObjectName(
        "paymasterLiquidityTargetSaveStatus");
    m_liquidity_target_save_status->setProperty(
        "paymasterRole", QStringLiteral("statusText"));
    m_liquidity_target_save_status->setWordWrap(true);
    // The combined policy status below covers the whole form. Retain the
    // target-specific text for diagnostics without a duplicate banner.
    m_liquidity_target_save_status->hide();
    m_save_liquidity_policy = new QPushButton(
        tr("Save liquidity targets and refill policy"), targets);
    m_save_liquidity_policy->setObjectName(
        "paymasterSaveLiquidityPolicy");
    m_save_liquidity_policy->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    m_save_liquidity_policy->setToolTip(tr(
        "Save the displayed slot targets, automatic-refill setting and finite maintenance limits. A running provider may immediately refill within the approved limits; saving does not start a stopped provider."));
    m_restore_liquidity_defaults = new QPushButton(
        tr("Reset to Small and clear fee approval"), targets);
    m_restore_liquidity_defaults->setObjectName("paymasterRestoreLiquidityDefaults");
    m_restore_liquidity_defaults->setToolTip(tr(
        "Reset the displayed targets and finite maintenance limits, and disable paid maintenance approval. This does not save the policy or create, spend or retire any wallet output."));
    auto* liquidity_target_actions = new QHBoxLayout();
    liquidity_target_actions->addWidget(m_restore_liquidity_defaults);
    liquidity_target_actions->addStretch();
    liquidity_target_actions->addWidget(m_save_liquidity_policy);
    targets_layout->addLayout(liquidity_target_actions);
    pages.liquidity_layout->addWidget(targets);

    auto* maintenance_status_group = new QGroupBox(
        tr("Current replenishment status"), liquidity);
    maintenance_status_group->setObjectName(
        "paymasterLiquidityMaintenanceStatusGroup");
    auto* maintenance_status_layout = new QVBoxLayout(
        maintenance_status_group);
    m_liquidity_slot_status = new QLabel(
        tr("Ready, pending and missing target counts have not been loaded yet."),
        maintenance_status_group);
    m_liquidity_slot_status->setObjectName("paymasterLiquiditySlotStatus");
    m_liquidity_slot_status->setWordWrap(true);
    m_liquidity_recycling_status = new QLabel(
        tr("No recyclable successor information is available yet."),
        maintenance_status_group);
    m_liquidity_recycling_status->setObjectName(
        "paymasterLiquidityRecyclingStatus");
    m_liquidity_recycling_status->setWordWrap(true);
    m_liquidity_budget_status = new QLabel(
        tr("Maintenance budget use has not been loaded yet."),
        maintenance_status_group);
    m_liquidity_budget_status->setObjectName(
        "paymasterLiquidityBudgetStatus");
    m_liquidity_budget_status->setWordWrap(true);
    maintenance_status_layout->addWidget(m_liquidity_slot_status);
    maintenance_status_layout->addWidget(m_liquidity_recycling_status);
    maintenance_status_layout->addWidget(m_liquidity_budget_status);

    m_panel.recovery().buildCarrierControls(pages, liquidity);
    auto* preparation = new QGroupBox(tr("Create missing pool liquidity"), liquidity);
    auto* preparation_layout = new QVBoxLayout(preparation);
    auto* preparation_help = new QLabel(tr(
        "Preparation is additive: existing live pool entries count toward the targets, so only missing outputs are proposed. Always preview the current targets before executing."), preparation);
    preparation_help->setWordWrap(true);
    preparation_layout->addWidget(preparation_help);
    auto* preparation_fee_form = new QFormLayout();
    m_preparation_fee = new DgbAmountLineEdit(DEFAULT_PAYMASTER_PREPARATION_FEE, preparation);
    m_preparation_fee->setObjectName("paymasterPoolPreparationFee");
    preparation_fee_form->addRow(tr("Maximum fee per setup transaction (DGB):"), m_preparation_fee);
    preparation_layout->addLayout(preparation_fee_form);
    connect(m_preparation_fee, &QLineEdit::textChanged, widget(), [this] {
        m_prepare_preview_target.clear();
        m_prepare_plan_id.clear();
        updateLiquidityDisplay();
    });
    m_prepare_preview = new QPushButton(tr("Preview pool preparation"), liquidity);
    auto* prepare_preview = m_prepare_preview;
    m_prepare_execute = new QPushButton(tr("Execute reviewed preparation…"), liquidity);
    auto* prepare_execute = m_prepare_execute;
    prepare_preview->setObjectName("paymasterPreviewPoolPreparation");
    prepare_execute->setObjectName("paymasterExecutePoolPreparation");
    auto* preparation_buttons = new QHBoxLayout();
    preparation_buttons->addWidget(prepare_preview);
    preparation_buttons->addWidget(prepare_execute);
    preparation_layout->addLayout(preparation_buttons);
    pages.liquidity_layout->addWidget(preparation);

    auto* retirement = new QGroupBox(tr("Return excess available liquidity"), liquidity);
    auto* retirement_layout = new QVBoxLayout(retirement);
    auto* retirement_help = new QLabel(tr(
        "Retirement is for deliberately lowering an already prepared pool. It returns only available excess outputs to ordinary wallet liquidity; active reservations are never touched. New operators normally do not need this step."), retirement);
    retirement_help->setObjectName("paymasterLiquidityRetirementExplanation");
    retirement_help->setWordWrap(true);
    retirement_layout->addWidget(retirement_help);
    m_rebalance_preview = new QPushButton(tr("Preview excess retirement"), liquidity);
    auto* rebalance_preview = m_rebalance_preview;
    m_rebalance_execute = new QPushButton(tr("Execute reviewed retirement…"), liquidity);
    auto* rebalance_execute = m_rebalance_execute;
    rebalance_preview->setObjectName("paymasterPreviewPoolRetirement");
    rebalance_execute->setObjectName("paymasterExecutePoolRetirement");
    auto* retirement_buttons = new QHBoxLayout();
    retirement_buttons->addWidget(rebalance_preview);
    retirement_buttons->addWidget(rebalance_execute);
    retirement_layout->addLayout(retirement_buttons);
    pages.liquidity_layout->addWidget(retirement);

    auto* result_group = new QGroupBox(tr("Latest preview or execution result"), liquidity);
    auto* result_layout = new QVBoxLayout(result_group);
    auto* result_help = new QLabel(tr(
        "A preview is side-effect free. Execution buttons remain guarded by confirmation and use the target values currently shown above."), result_group);
    result_help->setWordWrap(true);
    result_layout->addWidget(result_help);
    m_liquidity_preview_status = new QLabel(result_group);
    m_liquidity_preview_status->setObjectName("paymasterLiquidityPreviewStatus");
    m_liquidity_preview_status->setWordWrap(true);
    result_layout->addWidget(m_liquidity_preview_status);
    m_liquidity_output = new QPlainTextEdit(result_group);
    m_liquidity_output->setObjectName("paymasterLiquidityResult");
    m_liquidity_output->setReadOnly(true);
    m_liquidity_output->setMaximumHeight(260);
    m_liquidity_output->setPlaceholderText(tr("Choose Preview pool preparation to calculate the first setup transaction."));
    m_liquidity_output->viewport()->setBackgroundRole(QPalette::Window);
    m_liquidity_output->viewport()->setAutoFillBackground(true);
    result_layout->addWidget(m_liquidity_output);
    pages.liquidity_layout->addWidget(result_group);

    pages.advanced_liquidity = new QWidget(pages.liquidity_column);
    pages.advanced_liquidity->setObjectName("paymasterAdvancedLiquidityPanel");
    pages.advanced_liquidity_layout = new QVBoxLayout(pages.advanced_liquidity);
    pages.advanced_liquidity_layout->setContentsMargins(0, 0, 0, 0);
    pages.advanced_liquidity_layout->setSpacing(12);
    pages.advanced_liquidity_layout->addWidget(maintenance_status_group);
    pages.advanced_liquidity_layout->addWidget(liquidity_steps);
    automatic_maintenance_layout->insertRow(1, targets);
    m_save_liquidity_policy->hide();
    m_automatic_replenishment->hide();
    preparation->hide();
    auto* manual_costs = new QFormLayout;
    manual_costs->addRow(tr("Maximum fee for a manual reserve transaction (DGB):"), m_preparation_fee);
    pages.advanced_liquidity_layout->addLayout(manual_costs);
    retirement->hide();
    pages.advanced_liquidity_layout->addWidget(result_group);
    m_liquidity_advanced_toggle = AddPaymasterDisclosure(
        pages.liquidity_layout, pages.liquidity_column, pages.advanced_liquidity,
        tr("Advanced reserve management"),
        tr("Hide advanced reserve management"),
        QStringLiteral("paymasterAdvancedLiquidityToggle"));
    pages.liquidity_layout->addStretch();
    pages.liquidity_scroll->setWidget(liquidity);
    ConfigureScrollArea(pages.liquidity_scroll, liquidity);
    m_panel.bindPage(PaymasterWidgetImpl::Page::LIQUIDITY, pages.liquidity_scroll);
    pages.liquidity_layout->insertWidget(pages.liquidity_layout->indexOf(pages.automatic_maintenance), m_liquidity_maintenance_card);
    // Reserve configuration joins Settings once the other pages exist.

}

void ProviderLiquidity::connectLiquidity(PageBindings& pages)
{
    connect(m_prepare_preview, &QPushButton::clicked, widget(), [this] { beginGuidedTask("preparepaymasterpool"); });
    connect(m_prepare_execute, &QPushButton::clicked, widget(), [this] { poolAction("preparepaymasterpool", true); });
    connect(m_rebalance_preview, &QPushButton::clicked, widget(), [this] { beginGuidedTask("rebalancepaymasterpool"); });
    connect(m_rebalance_execute, &QPushButton::clicked, widget(), [this] { poolAction("rebalancepaymasterpool", true); });
    connect(m_approve_liquidity_maintenance, &QPushButton::clicked,
            widget(), [this] {
                if (maintenanceFeeLimitExceeded()) {
                    reviewMaintenanceFeeLimit();
                } else {
                    approveSuggestedLiquidityMaintenance();
                }
            });
    connect(m_restore_liquidity_defaults, &QPushButton::clicked,
            widget(), [this] { restoreLiquidityDefaults(); });
    connect(m_save_liquidity_policy, &QPushButton::clicked,
            widget(), [this] { saveLiquidityPolicy(); });
    connect(m_save_liquidity_policy_primary, &QPushButton::clicked,
            widget(), [this] { saveLiquidityPolicy(); });
    connect(m_automatic_replenishment, &QCheckBox::toggled,
            widget(), [this](bool enabled) {
                markLiquidityPolicyDirty();
                if (!m_loading_liquidity_policy && enabled && !m_paid_maintenance_approved->isChecked())
                    m_maintenance_limits_toggle->setChecked(true);
            });
    connect(m_paid_maintenance_approved, &QCheckBox::toggled,
            widget(), [this] { markLiquidityPolicyDirty(); });
    for (QLineEdit* control : {m_maintenance_fee_per_transaction,
                               m_maintenance_fee_per_hour,
                               m_maintenance_fee_per_day}) {
        connect(control, &QLineEdit::textChanged, widget(),
                [this] { markLiquidityPolicyDirty(); });
    }
    for (QSpinBox* control : {m_admission_dgb, m_operational_dgb,
                              m_admission_carriers, m_operational_carriers}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged),
                widget(), [this] {
                    markLiquidityPolicyDirty();
                    updateLiquidityDisplay();
                });
    }
}

void ProviderLiquidity::attachCapitalReleaseControl(QVBoxLayout* layout)
{
    layout->addWidget(m_release_excess_reserves, 0, Qt::AlignLeft);
}

QString ProviderLiquidity::automationSummary() const
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
            m_panel.overview().readinessExplanation(code));
    }
    if (policy.find_value("paid_maintenance_approved").isTrue())
        text += tr(" Approved: up to %1 DGB per refill, %2 per hour and %3 per day.")
            .arg(PaymasterWidgetImpl::dgbAmount(ProviderLiquidity::poolNumber(policy, "maximum_maintenance_fee_per_transaction_satoshis")),
                 PaymasterWidgetImpl::dgbAmount(ProviderLiquidity::poolNumber(policy, "maximum_maintenance_fee_per_hour_satoshis")),
                 PaymasterWidgetImpl::dgbAmount(ProviderLiquidity::poolNumber(policy, "maximum_maintenance_fee_per_day_satoshis")));
    if (observation.find_value("pending_transaction").isTrue() && !policy.find_value("automatic_replenishment").isTrue())
        text += tr(" A previously signed refill can still confirm.");
    return text;
}

void ProviderLiquidity::updateQuickAutomation()
{
    const bool known = IsCompleteLiquidityPolicy(m_saved_liquidity_policy) &&
        m_saved_liquidity_policy.find_value("updated_at").isNum();
    for (auto* control : m_refill_switches) {
        const QSignalBlocker blocker(control);
        control->setChecked(known && m_saved_liquidity_policy.find_value("automatic_replenishment").isTrue());
        control->setEnabled(known && !m_panel.busy() && !m_panel.privacyEnabled() && !m_refill_save_pending && m_panel.hasRpcTransport());
    }
    const QString text = m_panel.privacyEnabled() ? tr("Hidden by privacy mode") : m_refill_save_pending
        ? tr("Saving reserve automation; checking the wallet setting…")
        : automationSummary() + (m_refill_message.isEmpty() ? QString{} : QLatin1Char('\n') + m_refill_message);
    for (auto* label : m_refill_switch_status) label->setText(text);
}


void ProviderLiquidity::buildMaintenanceCard(QWidget* overview_column)
{
    m_liquidity_maintenance_card = new QGroupBox(
        tr("Automatic liquidity maintenance"), overview_column);
    m_liquidity_maintenance_card->setObjectName(
        "paymasterLiquidityMaintenanceCard");
    m_liquidity_maintenance_card->setProperty(
        "paymasterRole", QStringLiteral("maintenanceCard"));
    auto* maintenance_layout = new QVBoxLayout(
        m_liquidity_maintenance_card);
    m_liquidity_maintenance_state = new QLabel(
        tr("Liquidity maintenance status has not been loaded yet."),
        m_liquidity_maintenance_card);
    m_liquidity_maintenance_state->setObjectName(
        "paymasterLiquidityMaintenanceState");
    m_liquidity_maintenance_state->setProperty(
        "paymasterRole", QStringLiteral("statusText"));
    m_liquidity_maintenance_state->setWordWrap(true);
    QFont maintenance_state_font = m_liquidity_maintenance_state->font();
    maintenance_state_font.setBold(true);
    m_liquidity_maintenance_state->setFont(maintenance_state_font);
    m_liquidity_maintenance_next_step = new QLabel(
        m_liquidity_maintenance_card);
    m_liquidity_maintenance_next_step->setObjectName(
        "paymasterLiquidityMaintenanceNextStep");
    m_liquidity_maintenance_next_step->setProperty(
        "paymasterRole", QStringLiteral("mutedText"));
    m_liquidity_maintenance_next_step->setWordWrap(true);
    m_liquidity_maintenance_cost = new QLabel(
        m_liquidity_maintenance_card);
    m_liquidity_maintenance_cost->setObjectName(
        "paymasterLiquidityMaintenanceCost");
    m_liquidity_maintenance_cost->setProperty(
        "paymasterRole", QStringLiteral("mutedText"));
    m_liquidity_maintenance_cost->setWordWrap(true);
    m_approve_liquidity_maintenance = new QPushButton(
        tr("Review and approve maintenance limits…"),
        m_liquidity_maintenance_card);
    m_approve_liquidity_maintenance->setObjectName(
        "paymasterApproveLiquidityMaintenance");
    m_approve_liquidity_maintenance->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    maintenance_layout->addWidget(m_liquidity_maintenance_state);
    maintenance_layout->addWidget(m_liquidity_maintenance_next_step);
    maintenance_layout->addWidget(m_liquidity_maintenance_cost);
    maintenance_layout->addWidget(m_approve_liquidity_maintenance,
                                  0, Qt::AlignLeft);
    // Detailed maintenance guidance appears with operating capital below.

}

void ProviderLiquidity::observePoolCounts(const UniValue& pool)
{
    m_pool_status_loaded = true;
    m_pool_admission_dgb = pool.find_value("admission_dgb").getInt<int>();
    m_pool_admission_carriers = pool.find_value("admission_carriers").getInt<int>();
    m_pool_operational_dgb = pool.find_value("operational_dgb").getInt<int>();
    m_pool_operational_carriers = pool.find_value("operational_carriers").getInt<int>();
    m_liquidity_current_status->setText(tr(
        "Current confirmed and available pool — admission DGB: %1; admission carriers: %2; operational DGB: %3; operational carriers: %4; usable complete operational slots: %5; currently reserved: %6.")
        .arg(pool.find_value("admission_dgb").getInt<int>())
        .arg(pool.find_value("admission_carriers").getInt<int>())
        .arg(pool.find_value("operational_dgb").getInt<int>())
        .arg(pool.find_value("operational_carriers").getInt<int>())
        .arg(pool.find_value("complete_operational_slots").getInt<int>())
        .arg(pool.find_value("reserved").getInt<int>()));
}

void ProviderLiquidity::seedPoolCountsForTesting()
{
    if (!m_pool_status_loaded) {
        m_pool_status_loaded = true;
        m_pool_admission_dgb = m_admission_dgb->value();
        m_pool_operational_dgb = m_operational_dgb->value();
        m_pool_admission_carriers = m_admission_carriers->value();
        m_pool_operational_carriers = m_operational_carriers->value();
    }
}

void ProviderLiquidity::setMutationSnapshotForTesting(bool available)
{
    m_liquidity_snapshot_available = available;
    m_liquidity_snapshot_representable = available;
    setLiquidityPolicyMutationEnabled(available);
}

void ProviderLiquidity::disableMutationControls()
{
    for (QPushButton* action : {
         m_prepare_preview, m_prepare_execute,
         m_rebalance_preview, m_rebalance_execute}) {
        if (action) action->setEnabled(false);
    }
}

void ProviderLiquidity::redactPrivacyText()
{
    for (QLabel* label : {m_liquidity_maintenance_state, m_liquidity_maintenance_next_step, m_liquidity_maintenance_cost}) {
        if (label) label->setText(m_panel.maskNumericText(label->text()));
    }
}

void ProviderLiquidity::showAdvancedSettings()
{
    if (m_liquidity_advanced_toggle) m_liquidity_advanced_toggle->setChecked(true);
}

void ProviderLiquidity::focusPreparationFee()
{
    showAdvancedSettings();
    m_preparation_fee->setFocus();
}
