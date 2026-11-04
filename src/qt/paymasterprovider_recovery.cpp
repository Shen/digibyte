// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

void ProviderRecovery::resetWalletPresentation()
{
    m_backup_adopt_status->clear();
    m_backup_adopt_status->hide();
    m_preview_carrier_excess->setToolTip({});
    m_backup_required = false;
    updateBackupReminder(false, {});
    clearRetirementSummary();
    m_retirement_backup_status->clear();
    m_carrier_base_cents = m_carrier_excess_cents = 0;
    invalidateCarrierWithdrawalPreviews();
    m_carrier_value_status->setText(tr("Carrier base and withdrawable excess have not been loaded yet."));
    m_release_carrier_select->clear();
    m_preview_carrier_excess->setEnabled(false);
    m_preview_carrier_release->setEnabled(false);
}

void ProviderRecovery::clearRetirementSummary()
{
    m_retirement_summary->hide();
    m_retirement_balances->clear();
}

void ProviderRecovery::setPrivacy(bool privacy)
{
    if (privacy) {
        m_backup_adopt->hide();
        m_backup_adopt_status->hide();
        m_preview_carrier_excess->setToolTip({});
        clearRetirementSummary();
    }
    updateBackupReminder(m_backup_required, m_panel.overview().providerId());
}

void ProviderRecovery::setBackupRequestHandler(std::function<void()> handler)
{
    m_provider_backup_request_handler = std::move(handler);
}

void ProviderRecovery::focusBackupControls()
{
    m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::CONNECTION));
    m_overview_backup_now->setFocus(Qt::OtherFocusReason);
    if (auto* scroll = qobject_cast<QScrollArea*>(m_panel.page(PaymasterWidgetImpl::Page::CONNECTION)))
        scroll->ensureWidgetVisible(m_overview_backup_notice);
}

void ProviderRecovery::applyCarrierBalances(qint64 base, qint64 excess, bool configured)
{
    m_carrier_base_cents = base;
    m_carrier_excess_cents = excess;
    m_carrier_value_status->setText(tr(
        "Confirmed carrier base: %1 DD · accumulated earnings: %2 DD. The base remains wallet-owned and reserved only while its slot is active.")
        .arg(PaymasterFormatDD(base), PaymasterFormatDD(excess)));
    m_preview_carrier_excess->setEnabled(configured && m_panel.hasCompleteMutationSnapshots() &&
        excess >= Params().GetDigiDollarParams().minOutputAmount && !m_panel.busy());
    updateEarningsWithdrawalHint();
}

void ProviderRecovery::applyCarrierChoices(const std::vector<CarrierChoice>& choices, bool release_enabled)
{
    const auto previous = m_release_carrier_select->currentData().toString();
    const QSignalBlocker blocker(m_release_carrier_select);
    m_release_carrier_select->clear();
    for (const auto& [title, outpoint] : choices) m_release_carrier_select->addItem(title, outpoint);
    const auto previous_index = m_release_carrier_select->findData(previous);
    if (previous_index >= 0) m_release_carrier_select->setCurrentIndex(previous_index);
    else if (!previous.isEmpty()) {
        m_release_plan_id.clear();
        m_release_plan_expires_at = 0;
    }
    m_preview_carrier_release->setEnabled(release_enabled && hasReleasableCarrier());
    updateCarrierWithdrawalButtons();
}

void ProviderRecovery::clearCarrierChoices()
{
    const QSignalBlocker blocker(m_release_carrier_select);
    m_release_carrier_select->clear();
    invalidateCarrierWithdrawalPreviews();
    m_preview_carrier_release->setEnabled(false);
}

std::vector<ProviderRecovery::CarrierChoice> ProviderRecovery::carrierChoices() const
{
    std::vector<CarrierChoice> result;
    for (int i = 0; i < m_release_carrier_select->count(); ++i)
        result.emplace_back(m_release_carrier_select->itemText(i), m_release_carrier_select->itemData(i).toString());
    return result;
}

bool ProviderRecovery::hasReleasableCarrier() const
{
    return m_release_carrier_select->count() != 0;
}

void ProviderRecovery::selectCarrier(const QString& outpoint)
{
    m_release_carrier_select->setCurrentIndex(m_release_carrier_select->findData(outpoint));
}

void ProviderRecovery::updateBackupAdoptionAvailability(const QStringList& errors)
{
    m_backup_adopt->setVisible(!m_panel.privacyEnabled() && errors.contains(QStringLiteral("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED")));
    m_backup_adopt->setEnabled(!m_panel.privacyEnabled() && !m_panel.busy() && m_panel.hasRpcTransport());
}

void ProviderRecovery::disableWithdrawalControls()
{
    for (auto* button : {m_preview_carrier_excess, m_execute_carrier_excess, m_preview_carrier_release, m_execute_carrier_release})
        button->setEnabled(false);
}

void ProviderRecovery::refreshBackupReminder()
{
    updateBackupReminder(m_backup_required, m_panel.overview().providerId());
}

void ProviderRecovery::showBackupAdoptionMessage(const QString& message)
{
    m_backup_adopt_status->setText(message);
    m_backup_adopt_status->setVisible(!m_panel.privacyEnabled());
}

void ProviderRecovery::showRestoreIssue(const QString& code)
{
    m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::MANAGEMENT));
    const auto message = code == QLatin1String("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED")
        ? tr("This wallet was restored. Use Review restored provider wallet to check whether its current state can be adopted. New setup cannot remove the restoration protection.")
        : tr("This provider's checkpoint needs review. New setup cannot repair missing or older financial state. Do not delete or replace checkpoint files; inspect the current provider diagnostics.");
    m_panel.setStatusLabel(m_backup_adopt_status, message, "action");
    m_backup_adopt_status->show();
    m_panel.showStatus(message);
}

void PaymasterWidgetImpl::setProviderBackupRequestHandler(std::function<void()> handler)
{
    m_recovery.setBackupRequestHandler(std::move(handler));
}

void ProviderRecovery::updateBackupReminder(bool required, const QString& provider_id)
{
    m_backup_required = required;
    m_panel.overview().recordIdentity(provider_id);
    if (m_retirement_backup_status) {
        const bool reminder_known = m_panel.overview().operatorSnapshot().find_value("provider").find_value("backup_status").find_value("required").isBool();
        m_retirement_backup_status->setText(required
            ? tr("A current full-wallet backup is still recommended. Cancelling or failing the backup does not complete archiving.")
            : reminder_known
                ? tr("Core reports no pending backup reminder. Keep your complete backup offline and confidential; this status does not verify that the backup file is safe or recoverable.")
                : tr("Backup status is not verified. Make a current complete wallet backup before archiving."));
        m_retirement_backup->setEnabled(!m_panel.privacyEnabled() && !m_panel.busy() && !provider_id.isEmpty());
    }
    const bool visible = required && !provider_id.isEmpty();
    if (m_backup_notice) m_backup_notice->setVisible(visible && !m_panel.privacyEnabled());
    if (m_backup_summary) {
        const auto& backup = m_panel.overview().operatorSnapshot().find_value("provider").find_value("backup_status");
        const auto when = ProviderLiquidity::poolNumber(backup, "last_successful_backup_at");
        const auto external = ProviderLiquidity::poolNumber(backup, "external_backup_acknowledged_at");
        const auto date = [](qint64 value) { return QLocale().toString(QDateTime::fromSecsSinceEpoch(value).toLocalTime(), QLocale::ShortFormat); };
        const QString local = when > 0 ? tr("Last recorded wallet backup: %1.").arg(date(when)) : tr("No completed wallet backup is recorded.");
        const QString remote = external > 0 ? tr("External full-wallet backup acknowledged: %1. This acknowledgement does not verify the backup file.").arg(date(external)) : tr("No external backup has been acknowledged.");
        m_backup_summary->setText(m_panel.privacyEnabled() ? tr("Hidden by privacy mode") : local + QLatin1Char('\n') + remote);
    }
    const QString identity = tr("Provider ID: %1").arg(
        m_panel.visibleProviderId(provider_id));
    if (m_overview_backup_notice) {
        m_overview_backup_notice->setVisible(!provider_id.isEmpty());
        m_overview_backup_provider_id->setText(identity);
        m_overview_backup_provider_id->setTextInteractionFlags(
            m_panel.privacyEnabled() ? Qt::NoTextInteraction
                      : Qt::TextSelectableByKeyboard |
                            Qt::TextSelectableByMouse);
        m_overview_backup_now->setEnabled(!m_panel.privacyEnabled() && !m_panel.busy());
        m_overview_backup_external->setEnabled(!m_panel.privacyEnabled() && !m_panel.busy());
    }
    // The setup wizard is a child of this widget while it is open. Hide
    // its completion reminder as soon as the ordinary WalletView backup
    // workflow has successfully updated the wallet-scoped status.
    if (auto* setup_notice =
            widget()->findChild<QGroupBox*>(
                QStringLiteral("paymasterSetupBackupReminder"))) {
        setup_notice->setVisible(visible);
    }
}

void ProviderRecovery::reviewBackupAdoption(bool stopped)
{
    if ((!stopped && m_panel.busy()) || m_panel.privacyEnabled() || !m_panel.hasRpcTransport()) return;
    const auto generation = m_panel.walletGeneration();
    if (!stopped && (m_panel.overview().enabled() || m_panel.configuration().autostartEnabled())) {
        const auto answer = PaymasterWidgetImpl::askPlainTextQuestion(widget(), tr("Stop and review restored wallet"),
            tr("Stop this provider persistently, including autostart, then check whether this restored wallet can be adopted. Reserves remain prepared. Adoption requires a separate review and confirmation. Cancelling here changes nothing."));
        if (generation != m_panel.walletGeneration() || m_panel.privacyEnabled() || answer != QMessageBox::Yes) return;
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("persistent", true);
        options.pushKV("pause_setup", true);
        params.push_back(options);
        m_panel.call("stoppaymaster", params, false, nullptr, [this](const UniValue& result) {
            if (!result.find_value("running").isFalse()) return;
            m_panel.overview().recordStopped();
            m_panel.configuration().markPersistentlyStopped();
            m_panel.configuration().reconcileAutostart();
            reviewBackupAdoption(true);
        });
        return;
    }
    m_backup_adopt_status->show();
    m_panel.call("adoptpaymasterbackup", {}, false, nullptr, [this](const UniValue& preview) {
        if (m_panel.privacyEnabled()) return;
        const auto generation = m_panel.walletGeneration();
        const auto& plan = preview.find_value("plan_id");
        if (!IsHex256Field(preview, "plan_id") || !IsHex256Field(preview, "provider_id") ||
            !preview.find_value("executed").isFalse() ||
            !preview.find_value("provider_stopped").isTrue() ||
            ProviderFinance::financeNumber(preview, "network_fee_satoshis") != 0) return;
        for (const auto* key : {"pool_entries", "dgb_satoshis", "dd_cents", "network_fee_satoshis"}) {
            qint64 value{0};
            if (!GetInt64Field(preview, key, value) || value < 0) return;
        }
        QDialog dialog(widget());
        dialog.setObjectName("paymasterAdoptBackupDialog");
        dialog.setWindowTitle(tr("Adopt current provider backup"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* summary = new QLabel(tr("The wallet matches this node's saved checkpoint and has no unfinished payments or reserve operations.\n\nKeep %1 DGB and %2 DD in %3 prepared reserves. Adoption preserves the provider identity, settings, spent budgets and history. No transaction or network fee is created. Older copies using this checkpoint store will be blocked. The provider stays stopped; start it separately after review.")
            .arg(PaymasterWidgetImpl::compactDgbAmount(ProviderFinance::financeNumber(preview, "dgb_satoshis")), PaymasterWidgetImpl::ddAmount(ProviderFinance::financeNumber(preview, "dd_cents")))
            .arg(ProviderFinance::financeNumber(preview, "pool_entries")) + tr("\n\nProvider: %1").arg(QString::fromStdString(preview.find_value("provider_id").get_str())), &dialog);
        summary->setWordWrap(true);
        summary->setTextFormat(Qt::PlainText);
        layout->addWidget(summary);
        auto* independent_help = new QLabel(tr("Only confirm if the current checkpoint was kept independently of this wallet. Restoring an old wallet and its old checkpoint together cannot prove freshness. Do not operate this provider identity on another host."), &dialog);
        independent_help->setWordWrap(true);
        independent_help->setTextFormat(Qt::PlainText);
        layout->addWidget(independent_help);
        auto* independent = new QCheckBox(tr("I confirm independently preserved state and sole use of this identity."), &dialog);
        independent->setObjectName("paymasterAdoptIndependentCheckpoint");
        layout->addWidget(independent);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Adopt reviewed wallet"));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        connect(independent, &QCheckBox::toggled, buttons->button(QDialogButtonBox::Ok), &QPushButton::setEnabled);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        m_panel.setRpcBusyState(true);
        const bool accepted = dialog.exec() == QDialog::Accepted && independent->isChecked();
        if (generation != m_panel.walletGeneration()) return;
        m_panel.setRpcBusyState(false);
        if (!accepted || m_panel.privacyEnabled()) return;
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("execute", true);
        options.pushKV("independent_checkpoint", true);
        options.pushKV("plan_id", plan.get_str());
        params.push_back(options);
        m_panel.call("adoptpaymasterbackup", params, false, nullptr, [this, preview](const UniValue& result) {
            const bool confirmed = result.find_value("executed").isTrue() &&
                result.find_value("provider_stopped").isTrue() &&
                IsHex256Field(result, "plan_id") && IsHex256Field(result, "provider_id") &&
                result.find_value("plan_id").get_str() == preview.find_value("plan_id").get_str() &&
                result.find_value("provider_id").get_str() == preview.find_value("provider_id").get_str();
            m_panel.setStatusLabel(m_backup_adopt_status, confirmed
                ? tr("Current backup adopted. Reserves and spent budgets were preserved. The provider remains stopped. Review its settings, create a new full-wallet backup, then start separately.")
                : tr("Adoption result is unclear. Check current provider status before acting; do not repeat automatically."), confirmed ? "ready" : "action");
            m_panel.overview().refreshOperatorStatus();
        }, false, [this](const QString&) {
            m_panel.setStatusLabel(m_backup_adopt_status, tr("Adoption was not confirmed. Check current status before acting; do not retry automatically. The wallet may remain protected after an interrupted commit."), "action");
            m_panel.overview().refreshOperatorStatus();
        });
    }, false, [this](const QString& error) {
        m_panel.setStatusLabel(m_backup_adopt_status, tr("This backup cannot currently be adopted. %1").arg(
            error.contains(QLatin1String("CHECKPOINT"))
                ? tr("A current independently retained checkpoint is required; an older, missing or retired checkpoint cannot authorize continued service.")
                : error.contains(QLatin1String("BACKUP_INCOMPLETE_RECORDS"))
                    ? tr("Required financial records are missing or invalid. Use the complete current provider wallet; spent fee budgets must not be recreated. Existing protection remains active.")
                : tr("Stop the provider persistently and resolve unfinished payments, reserved funds, manual coin locks and unconfirmed reserve transactions. Existing protection remains active.")), "action");
    });
}

void ProviderRecovery::requestProviderBackup()
{
    if (!m_panel.requirePrivacyOffForSensitiveAction(
            tr("Provider-wallet backup"))) {
        return;
    }
    if (!m_provider_backup_request_handler) {
        QMessageBox::warning(
            widget(), tr("Provider-wallet backup"),
            tr("The wallet backup action is not available in this window."));
        return;
    }
    m_provider_backup_request_handler();
    // WalletView records a successful full-wallet backup in the same
    // wallet database. Refresh only after its modal file workflow returns.
    QTimer::singleShot(0, widget(), [this] {
        if (m_panel.hasRpcTransport() && !m_panel.busy()) m_panel.refreshStatus();
    });
}

void ProviderRecovery::acknowledgeExternalBackup()
{
    if (!m_panel.requirePrivacyOffForSensitiveAction(
            tr("Provider-wallet backup acknowledgement"))) {
        return;
    }
    if (!m_panel.hasRpcTransport() || m_panel.busy()) {
        m_panel.finance().setHistoryMessage(!m_panel.hasRpcTransport()
            ? tr("External-backup acknowledgement was not started because no wallet RPC transport is available.")
            : tr("External-backup acknowledgement was not started because another wallet operation is still running."));
        return;
    }
    const QString prompt = tr(
        "Only acknowledge this if another process has created a current, "
        "complete backup of this wallet file. Seed phrases, descriptor exports, "
        "CSV files and copied addresses do not preserve the complete Paymaster "
        "identity and accounting state.\n\nAcknowledge an external full-wallet backup?");
    if (QMessageBox::warning(
            widget(), tr("Acknowledge external wallet backup"), prompt,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    UniValue options{UniValue::VOBJ};
    options.pushKV("external_backup", true);
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    m_panel.call("acknowledgepaymasterproviderbackup", std::move(params), false,
         nullptr, [this](const UniValue& result) {
             if (!IsCompleteBackupAcknowledgement(result)) {
                 const QString message = tr(
                     "Core did not confirm a complete external-backup acknowledgement. The reminder remains visible until authoritative status proves that it was cleared.");
                 m_panel.finance().setHistoryMessage(message);
                 if (!m_panel.hasSynchronousTestExecutor()) {
                     QMessageBox::warning(
                         widget(), tr("External backup not confirmed"),
                         message);
                 }
                 m_panel.refreshStatus();
                 return;
             }
             const bool backup_required =
                 result.find_value("backup_required").get_bool();
             updateBackupReminder(backup_required, m_panel.overview().providerId());
             QMessageBox::information(
                 widget(), tr("External backup acknowledged"),
                 backup_required
                     ? tr("The external backup was acknowledged, but Core reports that a newer provider change still requires another complete wallet backup.")
                     : tr("The backup reminder has been cleared for the current provider configuration."));
             m_panel.refreshStatus();
         });
}

void ProviderRecovery::invalidateCarrierWithdrawalPreviews()
{
    m_excess_plan_id.clear();
    m_release_plan_id.clear();
    m_excess_plan_expires_at = 0;
    m_release_plan_expires_at = 0;
    m_excess_preview_cents = 0;
    m_excess_preview_fee_satoshis = 0;
    if (m_carrier_withdrawal_status) {
        m_carrier_withdrawal_status->setText(tr(
            "No carrier withdrawal has been previewed."));
    }
    updateCarrierWithdrawalButtons();
}

void ProviderRecovery::updateCarrierWithdrawalButtons()
{
    if (!m_execute_carrier_excess || !m_execute_carrier_release) return;
    m_execute_carrier_excess->setEnabled(
        m_panel.hasCompleteMutationSnapshots() &&
        m_panel.overview().carrierPlanCurrent(m_excess_plan_id,
                           m_excess_plan_expires_at));
    m_execute_carrier_release->setEnabled(
        m_panel.hasCompleteMutationSnapshots() && !m_panel.overview().running() &&
        m_panel.overview().carrierPlanCurrent(m_release_plan_id,
                           m_release_plan_expires_at));
}

UniValue ProviderRecovery::carrierWithdrawalOptions(const QString& mode, bool execute,
                                  const QString& plan_id) const
{
    UniValue options{UniValue::VOBJ};
    options.pushKV("mode", mode.toStdString());
    options.pushKV("execute", execute);
    if (execute) {
        options.pushKV("plan_id", plan_id.toStdString());
    } else if (mode == QLatin1String("release_slot")) {
        const QString selected =
            m_release_carrier_select->currentData().toString();
        const int separator = selected.lastIndexOf(':');
        if (separator > 0) {
            options.pushKV("txid",
                           selected.left(separator).toStdString());
            options.pushKV("vout",
                           selected.mid(separator + 1).toInt());
        }
    }
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    return params;
}

void ProviderRecovery::carrierWithdrawalAction(const QString& mode, bool)
{
    m_panel.liquidity().beginGuidedTask(mode);
}

void ProviderRecovery::reviewFullCapitalRelease(bool retire, bool recovery)
{
    if (m_panel.privacyEnabled() || !m_panel.hasRpcTransport()) return;
    UniValue preview_params{UniValue::VARR};
    if (recovery || retire) {
        UniValue options{UniValue::VOBJ};
        if (recovery) options.pushKV("recovery", true);
        else options.pushKV("cancel_uncreated", true);
        preview_params.push_back(options);
    }
    m_panel.call("releasepaymastercapital", preview_params, false, nullptr, [this, retire, recovery](const UniValue& preview) {
        if (m_panel.privacyEnabled()) return;
        const auto valid = [retire, recovery](const UniValue& result) {
            if (!IsHex256Field(result, "plan_id") || !result.find_value("executed").isBool()) return false;
            if (!result.find_value("recovery_release").isNull() && !result.find_value("recovery_release").isBool()) return false;
            qint64 uncreated{0};
            if (retire && !recovery &&
                (!GetInt64Field(result, "uncreated_maintenance", uncreated) ||
                 uncreated < 0 || uncreated > std::numeric_limits<int>::max())) return false;
            for (const auto* key : {"pool_entries", "dgb_satoshis", "dd_cents", "network_fee_satoshis"}) {
                qint64 value{0};
                if (!GetInt64Field(result, key, value) || value < 0) return false;
            }
            return result.find_value("network_fee_satoshis").getInt<int64_t>() == 0;
        };
        if (!valid(preview) || !preview.find_value("executed").isFalse() ||
            preview.find_value("recovery_release").isTrue() != recovery) {
            m_panel.overview().setStopStatus(tr("Provider stopped, but the capital preview is invalid. Nothing was released."), "error");
            return;
        }
        const auto generation = m_panel.walletGeneration();
        QString review = tr("Release %1 DGB and %2 DD from %3 pool outputs to ordinary wallet funds?\n\nNo transaction or network fee is created. Automatic refill and paid-maintenance approval are disabled. Identity, targets, budgets and recovery history remain. The provider is already stopped; cancelling keeps its capital prepared.")
                .arg(PaymasterWidgetImpl::compactDgbAmount(ProviderFinance::financeNumber(preview, "dgb_satoshis")), PaymasterWidgetImpl::ddAmount(ProviderFinance::financeNumber(preview, "dd_cents")))
                .arg(ProviderFinance::financeNumber(preview, "pool_entries"));
        if (recovery) review += tr("\n\nRecovery release permanently retires this provider identity, including other copies using this node's checkpoint. Signing protections remain active. Afterwards, use the normal Send pages to transfer the released funds. Cancelling makes no retirement or release change.");
        if (retire && !recovery && ProviderFinance::financeNumber(preview, "uncreated_maintenance") > 0) {
            review += tr("\n\nAlso cancel %n reserve-funding task(s) for which no transaction has been created. Signed work is not cancelled.", nullptr,
                         static_cast<int>(ProviderFinance::financeNumber(preview, "uncreated_maintenance")));
        }
        const auto answer = PaymasterWidgetImpl::askPlainTextQuestion(widget(),
            recovery ? tr("Recover capital and retire provider identity") : tr("Release all operating capital"), review);
        if (generation != m_panel.walletGeneration() || m_panel.privacyEnabled()) return;
        if (answer != QMessageBox::Yes) {
            m_panel.overview().setStopStatus(tr("Provider stopped. Capital release was not authorized; reserves remain prepared."), "neutral");
            m_panel.overview().refreshOperatorStatus();
            return;
        }
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("execute", true);
        options.pushKV("plan_id", preview.find_value("plan_id"));
        if (recovery) options.pushKV("recovery", true);
        else if (retire) options.pushKV("cancel_uncreated", true);
        params.push_back(options);
        m_panel.call("releasepaymastercapital", params, false, nullptr, [this, preview, valid, retire](const UniValue& result) {
            if (m_panel.privacyEnabled()) return;
            bool matches = valid(result) && result.find_value("executed").isTrue();
            for (const auto* key : {"plan_id", "pool_entries", "dgb_satoshis", "dd_cents", "network_fee_satoshis", "recovery_release", "uncreated_maintenance"})
                matches &= result.find_value(key).write() == preview.find_value(key).write();
            m_panel.overview().setStopStatus(matches
                ? (preview.find_value("recovery_release").isTrue()
                    ? tr("Provider identity permanently retired; reviewed capital is available in this wallet for normal transfers. No funds were sent. Keep the recovery records and create a new full-wallet backup.")
                    : tr("Provider stopped; all reviewed pool capital was released to this wallet. No funds were sent. Create a new full-wallet backup."))
                : tr("Capital release result is unclear. Inspect pool state; do not repeat automatically."), matches ? "ready" : "error");
            if (matches && retire) reviewRetirementBalances(preview.find_value("recovery_release").isTrue());
            else m_panel.overview().refreshOperatorStatus();
        }, false, [this](const QString& error) {
            if (m_panel.privacyEnabled()) return;
            const QString detail = error.contains(QLatin1String("PAYMASTER_CAPITAL_RECOVERY_CHECKPOINT_CHANGED"))
                ? m_panel.overview().readinessExplanation(QStringLiteral("PAYMASTER_CAPITAL_RECOVERY_CHECKPOINT_CHANGED")) : error;
            m_panel.overview().setStopStatus(tr("Capital release was not confirmed. No automatic retry. Inspect pool and recovery state: %1").arg(detail), "action");
            m_panel.overview().refreshOperatorStatus();
        });
    }, false, [this, retire, recovery](const QString& error) {
        if (m_panel.privacyEnabled()) return;
        if (!recovery && error.contains(QLatin1String("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"))) {
            reviewFullCapitalRelease(retire, true);
            return;
        }
        QString detail = error;
        for (const auto* code : {"PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION", "PAYMASTER_CAPITAL_RECOVERY_REQUIRES_RESTORED_WALLET"}) {
            if (error.contains(QLatin1String(code))) { detail = m_panel.overview().readinessExplanation(QLatin1String(code)); break; }
        }
        m_panel.overview().setStopStatus(tr("Provider stopped; capital was not released. Check synchronization, confirmations, reservations, manual coin locks and unfinished setup in Activity before requesting another review. Never force-unlock signed work. %1").arg(detail), "action");
        m_panel.overview().refreshOperatorStatus();
    });
}

void ProviderRecovery::reviewRetirementBalances(bool recovery)
{
    if (m_panel.privacyEnabled() || !m_panel.hasRpcTransport()) return;
    m_retirement_summary->show();
    m_retirement_balances->setText(tr("Pool capital was released. Reading remaining wallet balances…"));
    const auto unavailable = [this](const QString&) {
        if (m_panel.privacyEnabled()) return;
        m_retirement_balances->setText(tr("Remaining balances could not be verified. Do not assume the wallet is empty. Check the normal wallet and DigiDollar balances before archiving; no transfer or wallet closure was attempted."));
        m_panel.overview().setStopStatus(tr("Stopped and capital released; retirement review is incomplete."), "action");
        m_panel.overview().refreshOperatorStatus();
    };
    m_panel.call("getbalances", {}, false, nullptr, [this, unavailable, recovery](const UniValue& balances) {
        if (m_panel.privacyEnabled()) return;
        const auto& mine = balances.find_value("mine");
        QStringList dgb_lines;
        for (const auto& field : std::array<std::pair<const char*, QString>, 4>{{
                 {"trusted", tr("DGB trusted (may include own unconfirmed change)")},
                 {"untrusted_pending", tr("DGB untrusted pending")},
                 {"immature", tr("DGB immature")},
                 {"used", tr("DGB excluded by address-reuse avoidance")}}}) {
            const auto& value = mine.find_value(field.first);
            if (std::string(field.first) == "used" && value.isNull()) continue;
            int64_t satoshis{0};
            if ((!value.isNum() && !value.isStr()) ||
                !ParseFixedPoint(value.getValStr(), 8, &satoshis) || !MoneyRange(satoshis)) {
                unavailable({});
                return;
            }
            dgb_lines << tr("%1: %2 DGB").arg(field.second, PaymasterWidgetImpl::compactDgbAmount(satoshis));
        }
        // minconf=0 includes pending DD; the default would omit it.
        UniValue params{UniValue::VARR}; params.push_back(""); params.push_back(0);
        m_panel.call("getdigidollarbalance", params, false, nullptr, [this, dgb_lines, unavailable, recovery](const UniValue& dd) {
            if (m_panel.privacyEnabled()) return;
            qint64 confirmed{0}, pending{0}, total{0};
            if (!GetInt64Field(dd, "confirmed", confirmed) || !GetInt64Field(dd, "unconfirmed", pending) ||
                !GetInt64Field(dd, "total", total) || confirmed < 0 || pending < 0 || total < confirmed || total - confirmed != pending) {
                unavailable({});
                return;
            }
            m_retirement_balances->setText(dgb_lines.join(QLatin1Char('\n')) +
                tr("\nDD confirmed: %1 DD\nDD pending: %2 DD\nBalance observations: %3 (UTC). Not a live or atomic balance guarantee.")
                    .arg(PaymasterWidgetImpl::ddAmount(confirmed), PaymasterWidgetImpl::ddAmount(pending), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)));
            m_panel.overview().setStopStatus(recovery
                ? tr("Provider identity permanently retired and pool capital released. Review the remaining funds for normal transfers and complete the full-wallet backup before archiving.")
                : tr("Provider stopped and pool capital released. Review the remaining funds and complete the full-wallet backup before archiving."), "neutral");
            m_panel.overview().refreshOperatorStatus();
        }, false, unavailable);
    }, false, unavailable);
}

void ProviderRecovery::updateEarningsWithdrawalHint()
{
    const QString withdrawal_hint = m_panel.privacyEnabled() || !m_panel.hasCompleteMutationSnapshots() ? QString{} : tr(
        "Minimum payout: %2 DD. Currently accumulated: %1 DD. Earnings remain in the reserve until the minimum is reached; base capital is retained.")
        .arg(PaymasterFormatDD(m_carrier_excess_cents),
             PaymasterFormatDD(Params().GetDigiDollarParams().minOutputAmount));
    m_panel.overview().updateWithdrawalHint(withdrawal_hint, !m_panel.privacyEnabled() && m_panel.hasCompleteMutationSnapshots() &&
        m_carrier_excess_cents < Params().GetDigiDollarParams().minOutputAmount &&
        !m_panel.liquidity().operationView().busy() && m_panel.liquidity().operationView().phase != PaymasterOperationController::Phase::Waiting);
    m_preview_carrier_excess->setToolTip(withdrawal_hint);
}







void ProviderRecovery::buildRecoveryPages(PageBindings& pages)
{
    pages.management_scroll = new QScrollArea;
    m_panel.bindPage(PaymasterWidgetImpl::Page::MANAGEMENT, pages.management_scroll);
    pages.management_scroll->setObjectName("paymasterManagementPage");
    pages.management_scroll->setWidgetResizable(true);
    pages.management = new QWidget(pages.management_scroll);
    pages.management->setObjectName("paymasterManagementContents");

    pages.management_column = CreatePaymasterPageColumn(pages.management, pages.management_layout);
    AddPaymasterPageHeading(pages.management_layout, pages.management_column, tr("Wallet & backup"),
                            tr("Manage operating access and protect this provider identity, configuration and financial records."), QStringLiteral("paymasterManagement"));
    pages.lifecycle_scroll = new QScrollArea;
    m_panel.bindPage(PaymasterWidgetImpl::Page::LIFECYCLE, pages.lifecycle_scroll);
    pages.lifecycle_scroll->setObjectName("paymasterLifecyclePage");
    pages.lifecycle_scroll->setWidgetResizable(true);
    auto* lifecycle_contents = new QWidget(pages.lifecycle_scroll);
    lifecycle_contents->setObjectName("paymasterLifecycleContents");

    auto* lifecycle_column = CreatePaymasterPageColumn(lifecycle_contents, pages.lifecycle_page_layout);
    AddPaymasterPageHeading(pages.lifecycle_page_layout, lifecycle_column, tr("Start, pause & retire"),
        tr("Choose how to operate this provider. The checklists describe each action after confirmation; they are not completion indicators."),
        QStringLiteral("paymasterLifecycle"));
    m_panel.overview().buildLifecycleControls(pages, lifecycle_column);

    m_retirement_summary = new QGroupBox(tr("Finish retirement — funds and backup"), lifecycle_column);
    m_retirement_summary->setObjectName("paymasterRetirementSummary");
    auto* retirement_summary_layout = new QVBoxLayout(m_retirement_summary);
    m_retirement_balances = new QLabel(m_retirement_summary);
    m_retirement_balances->setObjectName("paymasterRetirementBalances");
    m_retirement_balances->setTextFormat(Qt::PlainText);
    m_retirement_balances->setWordWrap(true);
    retirement_summary_layout->addWidget(m_retirement_balances);
    auto* retirement_summary_help = new QLabel(tr("These are wallet balance snapshots, not a guarantee that every output is spendable. Funds, including future incoming payments, still belong to this wallet. If you want to move them, use the normal Send pages and wait for confirmation, then make a new full-wallet backup. Nothing here transfers funds or deletes keys."), m_retirement_summary);
    retirement_summary_help->setWordWrap(true);
    retirement_summary_layout->addWidget(retirement_summary_help);
    m_retirement_backup = new QPushButton(tr("Back up this wallet for archiving…"), m_retirement_summary);
    m_retirement_backup->setObjectName("paymasterRetirementBackup");
    retirement_summary_layout->addWidget(m_retirement_backup, 0, Qt::AlignLeft);
    connect(m_retirement_backup, &QPushButton::clicked, widget(), [this] { requestProviderBackup(); });
    m_retirement_backup_status = new QLabel(m_retirement_summary);
    m_retirement_backup_status->setObjectName("paymasterRetirementBackupStatus");
    m_retirement_backup_status->setWordWrap(true);
    retirement_summary_layout->addWidget(m_retirement_backup_status);
    auto* close_help = new QLabel(tr("After reviewing funds and securing a complete backup, you may use File → Close Wallet if this wallet is only used for this provider. Closing unloads the wallet; it does not delete it. Do not close a shared wallet that is still needed for other activity. Never run a restored copy of the same provider identity alongside the original."), m_retirement_summary);
    close_help->setObjectName("paymasterRetirementCloseHelp");
    close_help->setWordWrap(true);
    retirement_summary_layout->addWidget(close_help);
    pages.lifecycle_page_layout->addWidget(m_retirement_summary);
    m_retirement_summary->hide();
    pages.lifecycle_page_layout->addStretch();
    pages.lifecycle_scroll->setWidget(lifecycle_contents);
    ConfigureScrollArea(pages.lifecycle_scroll, lifecycle_contents);

}

void ProviderRecovery::buildBackupControls(PageBindings& pages, QWidget* connection_column, QWidget* overview_column)
{
    m_overview_backup_notice = new QGroupBox(
        tr("Identity and full-wallet backup"), connection_column);
    m_overview_backup_notice->setObjectName(
        "paymasterOverviewBackupNotice");
    m_overview_backup_notice->setProperty(
        "paymasterRole", QStringLiteral("notice"));
    m_overview_backup_notice->setProperty(
        "statusKind", QStringLiteral("waiting"));
    auto* overview_backup_layout = new QVBoxLayout(
        m_overview_backup_notice);
    auto* overview_backup_text = new QLabel(tr(
        "This provider identity, its private key, configuration, pool state and finance history are stored in this wallet. "
        "Create a new full-wallet backup and keep it offline and confidential. A seed or descriptor export alone is not a complete Paymaster backup.") + QLatin1Char('\n') + tr(
        "For continued operation after restoration, Core also requires independently retained current state from the node's paymaster-checkpoints folder. Never restore an old wallet and its old checkpoint together to resume service. Use this provider identity on only one host. Before migration, pause the provider, resolve unfinished work, then create a current backup."),
        m_overview_backup_notice);
    overview_backup_text->setWordWrap(true);
    overview_backup_layout->addWidget(overview_backup_text);
    m_overview_backup_provider_id = new QLabel(
        m_overview_backup_notice);
    m_overview_backup_provider_id->setObjectName(
        "paymasterOverviewBackupProviderId");
    m_overview_backup_provider_id->setWordWrap(true);
    m_overview_backup_provider_id->setTextInteractionFlags(
        Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    overview_backup_layout->addWidget(m_overview_backup_provider_id);
    auto* overview_backup_actions = new QHBoxLayout();
    m_overview_backup_now = new QPushButton(
        tr("Back up provider wallet now…"), m_overview_backup_notice);
    m_overview_backup_now->setObjectName("paymasterOverviewBackupNow");
    m_overview_backup_now->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    m_overview_backup_external = new QPushButton(
        tr("I use another full-wallet backup method…"),
        m_overview_backup_notice);
    m_overview_backup_external->setObjectName(
        "paymasterOverviewExternalBackup");
    overview_backup_actions->addWidget(m_overview_backup_now);
    overview_backup_actions->addWidget(m_overview_backup_external);
    overview_backup_actions->addStretch();
    overview_backup_layout->addLayout(overview_backup_actions);
    m_overview_backup_notice->hide();
    pages.management_layout->addWidget(m_overview_backup_notice);
    m_backup_summary = new QLabel(pages.management_column);
    m_backup_summary->setObjectName("paymasterBackupSummary");
    m_backup_summary->setWordWrap(true);
    pages.management_layout->addWidget(m_backup_summary);
    m_backup_adopt = new QPushButton(tr("Review restored provider wallet…"), pages.management_column);
    m_backup_adopt->setObjectName("paymasterAdoptBackup");
    m_backup_adopt->hide();
    pages.management_layout->addWidget(m_backup_adopt, 0, Qt::AlignLeft);
    m_backup_adopt_status = new QLabel(pages.management_column);
    m_backup_adopt_status->setObjectName("paymasterAdoptBackupStatus");
    m_backup_adopt_status->setWordWrap(true);
    m_backup_adopt_status->setTextFormat(Qt::PlainText);
    m_backup_adopt_status->hide();
    pages.management_layout->addWidget(m_backup_adopt_status);
    connect(m_backup_adopt, &QPushButton::clicked, widget(), [this] { reviewBackupAdoption(); });
    m_backup_notice = new QPushButton(tr("A full-wallet backup is recommended. Review backup…"), overview_column);
    m_backup_notice->setObjectName("paymasterBackupReminder");
    m_backup_notice->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    m_backup_notice->hide();
    pages.overview_layout->addWidget(m_backup_notice);
    connect(m_backup_notice, &QPushButton::clicked, widget(), [this] { m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::MANAGEMENT)); });
}

void ProviderRecovery::buildCarrierControls(PageBindings& pages, QWidget* liquidity)
{
    auto* carrier_management = new QGroupBox(
        tr("Carrier value and withdrawal"), liquidity);
    carrier_management->setObjectName("paymasterCarrierManagement");
    auto* carrier_management_layout = new QVBoxLayout(carrier_management);
    auto* carrier_management_help = new QLabel(tr(
        "Every active carrier keeps exactly 1.00 DD as its reusable base. User-paid "
        "service fees can accumulate above that base. Previewing excess withdrawal "
        "keeps every base intact. Releasing a whole operational carrier instead "
        "reduces its target and is allowed only while the provider is stopped."),
        carrier_management);
    carrier_management_help->setWordWrap(true);
    m_carrier_value_status = new QLabel(
        tr("Carrier base and withdrawable excess have not been loaded yet."),
        carrier_management);
    m_carrier_value_status->setObjectName("paymasterCarrierValueStatus");
    m_carrier_value_status->setWordWrap(true);
    auto* excess_buttons = new QHBoxLayout();
    m_preview_carrier_excess = new QPushButton(
        tr("Preview bundled excess withdrawal"), carrier_management);
    m_preview_carrier_excess->setObjectName(
        "paymasterPreviewCarrierExcess");
    m_preview_carrier_excess->setEnabled(false);
    m_execute_carrier_excess = new QPushButton(
        tr("Execute reviewed excess withdrawal…"), carrier_management);
    m_execute_carrier_excess->setObjectName(
        "paymasterExecuteCarrierExcess");
    m_execute_carrier_excess->setEnabled(false);
    excess_buttons->addWidget(m_preview_carrier_excess);
    excess_buttons->addWidget(m_execute_carrier_excess);
    auto* release_form = new QFormLayout();
    m_release_carrier_select = new NoWheelComboBox(carrier_management);
    m_release_carrier_select->setObjectName(
        "paymasterReleaseCarrierSelection");
    m_release_carrier_select->setAccessibleName(tr(
        "Confirmed operational carrier slot to release"));
    release_form->addRow(tr("Operational carrier to release:"),
                         m_release_carrier_select);
    auto* release_buttons = new QHBoxLayout();
    m_preview_carrier_release = new QPushButton(
        tr("Preview full slot release"), carrier_management);
    m_preview_carrier_release->setObjectName(
        "paymasterPreviewCarrierRelease");
    m_preview_carrier_release->setEnabled(false);
    m_execute_carrier_release = new QPushButton(
        tr("Execute reviewed slot release…"), carrier_management);
    m_execute_carrier_release->setObjectName(
        "paymasterExecuteCarrierRelease");
    m_execute_carrier_release->setEnabled(false);
    release_buttons->addWidget(m_preview_carrier_release);
    release_buttons->addWidget(m_execute_carrier_release);
    m_carrier_withdrawal_status = new QLabel(
        tr("No carrier withdrawal has been previewed."), carrier_management);
    m_carrier_withdrawal_status->setObjectName(
        "paymasterCarrierWithdrawalStatus");
    m_carrier_withdrawal_status->setWordWrap(true);
    carrier_management_layout->addWidget(carrier_management_help);
    carrier_management_layout->addWidget(m_carrier_value_status);
    carrier_management_layout->addLayout(excess_buttons);
    carrier_management_layout->addLayout(release_form);
    carrier_management_layout->addLayout(release_buttons);
    carrier_management_layout->addWidget(m_carrier_withdrawal_status);
    pages.liquidity_layout->addWidget(carrier_management);
    carrier_management->hide();

}

void ProviderRecovery::connectRecovery(PageBindings& pages)
{
    connect(m_overview_backup_now, &QPushButton::clicked, widget(),
        [this] { requestProviderBackup(); });
    connect(m_overview_backup_external, &QPushButton::clicked, widget(),
            [this] { acknowledgeExternalBackup(); });
    connect(m_preview_carrier_excess, &QPushButton::clicked,
            widget(), [this] { carrierWithdrawalAction("all_excess", false); });
    connect(m_execute_carrier_excess, &QPushButton::clicked,
            widget(), [this] { carrierWithdrawalAction("all_excess", true); });
    connect(m_preview_carrier_release, &QPushButton::clicked,
            widget(), [this] { carrierWithdrawalAction("release_slot", false); });
    connect(m_execute_carrier_release, &QPushButton::clicked,
            widget(), [this] { carrierWithdrawalAction("release_slot", true); });
    connect(m_release_carrier_select,
            qOverload<int>(&QComboBox::currentIndexChanged), widget(),
            [this] { invalidateCarrierWithdrawalPreviews(); });
}
