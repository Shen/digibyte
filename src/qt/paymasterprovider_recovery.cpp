// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

void PaymasterWidgetImpl::setProviderBackupRequestHandler(std::function<void()> handler)
{
    m_provider_backup_request_handler = std::move(handler);
}

void PaymasterWidgetImpl::updateBackupReminder(bool required, const QString& provider_id)
{
    m_backup_required = required;
    m_provider_id = provider_id;
    if (m_retirement_backup_status) {
        const bool reminder_known = m_operator_snapshot.find_value("provider").find_value("backup_status").find_value("required").isBool();
        m_retirement_backup_status->setText(required
            ? tr("A current full-wallet backup is still recommended. Cancelling or failing the backup does not complete archiving.")
            : reminder_known
                ? tr("Core reports no pending backup reminder. Keep your complete backup offline and confidential; this status does not verify that the backup file is safe or recoverable.")
                : tr("Backup status is not verified. Make a current complete wallet backup before archiving."));
        m_retirement_backup->setEnabled(!m_privacy && !m_busy && !provider_id.isEmpty());
    }
    const bool visible = required && !provider_id.isEmpty();
    if (m_backup_notice) m_backup_notice->setVisible(visible && !m_privacy);
    if (m_backup_summary) {
        const auto& backup = m_operator_snapshot.find_value("provider").find_value("backup_status");
        const auto when = poolNumber(backup, "last_successful_backup_at");
        const auto external = poolNumber(backup, "external_backup_acknowledged_at");
        const auto date = [](qint64 value) { return QLocale().toString(QDateTime::fromSecsSinceEpoch(value).toLocalTime(), QLocale::ShortFormat); };
        const QString local = when > 0 ? tr("Last recorded wallet backup: %1.").arg(date(when)) : tr("No completed wallet backup is recorded.");
        const QString remote = external > 0 ? tr("External full-wallet backup acknowledged: %1. This acknowledgement does not verify the backup file.").arg(date(external)) : tr("No external backup has been acknowledged.");
        m_backup_summary->setText(m_privacy ? tr("Hidden by privacy mode") : local + QLatin1Char('\n') + remote);
    }
    const QString identity = tr("Provider ID: %1").arg(
        visibleProviderId(provider_id));
    if (m_overview_backup_notice) {
        m_overview_backup_notice->setVisible(!provider_id.isEmpty());
        m_overview_backup_provider_id->setText(identity);
        m_overview_backup_provider_id->setTextInteractionFlags(
            m_privacy ? Qt::NoTextInteraction
                      : Qt::TextSelectableByKeyboard |
                            Qt::TextSelectableByMouse);
        m_overview_backup_now->setEnabled(!m_privacy && !m_busy);
        m_overview_backup_external->setEnabled(!m_privacy && !m_busy);
    }
    // The setup wizard is a child of this widget while it is open. Hide
    // its completion reminder as soon as the ordinary WalletView backup
    // workflow has successfully updated the wallet-scoped status.
    if (auto* setup_notice =
            findChild<QGroupBox*>(
                QStringLiteral("paymasterSetupBackupReminder"))) {
        setup_notice->setVisible(visible);
    }
}

void PaymasterWidgetImpl::reviewBackupAdoption(bool stopped)
{
    if ((!stopped && m_busy) || m_privacy || !hasRpcTransport()) return;
    const auto generation = m_wallet_generation;
    if (!stopped && (m_core_enabled || m_autostart_enabled)) {
        const auto answer = askPlainTextQuestion(this, tr("Stop and review restored wallet"),
            tr("Stop this provider persistently, including autostart, then check whether this restored wallet can be adopted. Reserves remain prepared. Adoption requires a separate review and confirmation. Cancelling here changes nothing."));
        if (generation != m_wallet_generation || m_privacy || answer != QMessageBox::Yes) return;
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("persistent", true);
        options.pushKV("pause_setup", true);
        params.push_back(options);
        call("stoppaymaster", params, false, nullptr, [this](const UniValue& result) {
            if (!result.find_value("running").isFalse()) return;
            m_core_enabled = m_core_running = m_autostart_enabled = false;
            m_autostart_dirty = m_autostart_pending_read = false;
            reconcileAutostart();
            reviewBackupAdoption(true);
        });
        return;
    }
    m_backup_adopt_status->show();
    call("adoptpaymasterbackup", {}, false, nullptr, [this](const UniValue& preview) {
        if (m_privacy) return;
        const auto generation = m_wallet_generation;
        const auto& plan = preview.find_value("plan_id");
        if (!IsHex256Field(preview, "plan_id") || !IsHex256Field(preview, "provider_id") ||
            !preview.find_value("executed").isFalse() ||
            !preview.find_value("provider_stopped").isTrue() ||
            financeNumber(preview, "network_fee_satoshis") != 0) return;
        for (const auto* key : {"pool_entries", "dgb_satoshis", "dd_cents", "network_fee_satoshis"}) {
            qint64 value{0};
            if (!GetInt64Field(preview, key, value) || value < 0) return;
        }
        QDialog dialog(this);
        dialog.setObjectName("paymasterAdoptBackupDialog");
        dialog.setWindowTitle(tr("Adopt current provider backup"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* summary = new QLabel(tr("The wallet matches this node's saved checkpoint and has no unfinished payments or reserve operations.\n\nKeep %1 DGB and %2 DD in %3 prepared reserves. Adoption preserves the provider identity, settings, spent budgets and history. No transaction or network fee is created. Older copies using this checkpoint store will be blocked. The provider stays stopped; start it separately after review.")
            .arg(compactDgbAmount(financeNumber(preview, "dgb_satoshis")), ddAmount(financeNumber(preview, "dd_cents")))
            .arg(financeNumber(preview, "pool_entries")) + tr("\n\nProvider: %1").arg(QString::fromStdString(preview.find_value("provider_id").get_str())), &dialog);
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
        setRpcBusyState(true);
        const bool accepted = dialog.exec() == QDialog::Accepted && independent->isChecked();
        if (generation != m_wallet_generation) return;
        setRpcBusyState(false);
        if (!accepted || m_privacy) return;
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("execute", true);
        options.pushKV("independent_checkpoint", true);
        options.pushKV("plan_id", plan.get_str());
        params.push_back(options);
        call("adoptpaymasterbackup", params, false, nullptr, [this, preview](const UniValue& result) {
            const bool confirmed = result.find_value("executed").isTrue() &&
                result.find_value("provider_stopped").isTrue() &&
                IsHex256Field(result, "plan_id") && IsHex256Field(result, "provider_id") &&
                result.find_value("plan_id").get_str() == preview.find_value("plan_id").get_str() &&
                result.find_value("provider_id").get_str() == preview.find_value("provider_id").get_str();
            setStatusLabel(m_backup_adopt_status, confirmed
                ? tr("Current backup adopted. Reserves and spent budgets were preserved. The provider remains stopped. Review its settings, create a new full-wallet backup, then start separately.")
                : tr("Adoption result is unclear. Check current provider status before acting; do not repeat automatically."), confirmed ? "ready" : "action");
            refreshOperatorStatus();
        }, false, [this](const QString&) {
            setStatusLabel(m_backup_adopt_status, tr("Adoption was not confirmed. Check current status before acting; do not retry automatically. The wallet may remain protected after an interrupted commit."), "action");
            refreshOperatorStatus();
        });
    }, false, [this](const QString& error) {
        setStatusLabel(m_backup_adopt_status, tr("This backup cannot currently be adopted. %1").arg(
            error.contains(QLatin1String("CHECKPOINT"))
                ? tr("A current independently retained checkpoint is required; an older, missing or retired checkpoint cannot authorize continued service.")
                : error.contains(QLatin1String("BACKUP_INCOMPLETE_RECORDS"))
                    ? tr("Required financial records are missing or invalid. Use the complete current provider wallet; spent fee budgets must not be recreated. Existing protection remains active.")
                : tr("Stop the provider persistently and resolve unfinished payments, reserved funds, manual coin locks and unconfirmed reserve transactions. Existing protection remains active.")), "action");
    });
}

void PaymasterWidgetImpl::requestProviderBackup()
{
    if (!requirePrivacyOffForSensitiveAction(
            tr("Provider-wallet backup"))) {
        return;
    }
    if (!m_provider_backup_request_handler) {
        QMessageBox::warning(
            this, tr("Provider-wallet backup"),
            tr("The wallet backup action is not available in this window."));
        return;
    }
    m_provider_backup_request_handler();
    // WalletView records a successful full-wallet backup in the same
    // wallet database. Refresh only after its modal file workflow returns.
    QTimer::singleShot(0, this, [this] {
        if (hasRpcTransport() && !m_busy) refreshStatus();
    });
}

void PaymasterWidgetImpl::acknowledgeExternalBackup()
{
    if (!requirePrivacyOffForSensitiveAction(
            tr("Provider-wallet backup acknowledgement"))) {
        return;
    }
    if (!hasRpcTransport() || m_busy) {
        m_finance_history_status->setText(!hasRpcTransport()
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
            this, tr("Acknowledge external wallet backup"), prompt,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    UniValue options{UniValue::VOBJ};
    options.pushKV("external_backup", true);
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    call("acknowledgepaymasterproviderbackup", std::move(params), false,
         nullptr, [this](const UniValue& result) {
             if (!IsCompleteBackupAcknowledgement(result)) {
                 const QString message = tr(
                     "Core did not confirm a complete external-backup acknowledgement. The reminder remains visible until authoritative status proves that it was cleared.");
                 m_finance_history_status->setText(message);
                 if (!m_rpc_executor_for_testing) {
                     QMessageBox::warning(
                         this, tr("External backup not confirmed"),
                         message);
                 }
                 refreshStatus();
                 return;
             }
             const bool backup_required =
                 result.find_value("backup_required").get_bool();
             updateBackupReminder(backup_required, m_provider_id);
             QMessageBox::information(
                 this, tr("External backup acknowledged"),
                 backup_required
                     ? tr("The external backup was acknowledged, but Core reports that a newer provider change still requires another complete wallet backup.")
                     : tr("The backup reminder has been cleared for the current provider configuration."));
             refreshStatus();
         });
}

void PaymasterWidgetImpl::invalidateCarrierWithdrawalPreviews()
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

void PaymasterWidgetImpl::updateCarrierWithdrawalButtons()
{
    if (!m_execute_carrier_excess || !m_execute_carrier_release) return;
    m_execute_carrier_excess->setEnabled(
        hasCompleteMutationSnapshots() &&
        carrierPlanCurrent(m_excess_plan_id,
                           m_excess_plan_expires_at));
    m_execute_carrier_release->setEnabled(
        hasCompleteMutationSnapshots() && !m_core_running &&
        carrierPlanCurrent(m_release_plan_id,
                           m_release_plan_expires_at));
}

UniValue PaymasterWidgetImpl::carrierWithdrawalOptions(const QString& mode, bool execute,
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

void PaymasterWidgetImpl::carrierWithdrawalAction(const QString& mode, bool)
{
    beginGuidedTask(mode);
}

void PaymasterWidgetImpl::reviewFullCapitalRelease(bool retire, bool recovery)
{
    if (m_privacy || !hasRpcTransport()) return;
    UniValue preview_params{UniValue::VARR};
    if (recovery || retire) {
        UniValue options{UniValue::VOBJ};
        if (recovery) options.pushKV("recovery", true);
        else options.pushKV("cancel_uncreated", true);
        preview_params.push_back(options);
    }
    call("releasepaymastercapital", preview_params, false, nullptr, [this, retire, recovery](const UniValue& preview) {
        if (m_privacy) return;
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
            setStatusLabel(m_operator_stop_status, tr("Provider stopped, but the capital preview is invalid. Nothing was released."), "error");
            return;
        }
        const auto generation = m_wallet_generation;
        QString review = tr("Release %1 DGB and %2 DD from %3 pool outputs to ordinary wallet funds?\n\nNo transaction or network fee is created. Automatic refill and paid-maintenance approval are disabled. Identity, targets, budgets and recovery history remain. The provider is already stopped; cancelling keeps its capital prepared.")
                .arg(compactDgbAmount(financeNumber(preview, "dgb_satoshis")), ddAmount(financeNumber(preview, "dd_cents")))
                .arg(financeNumber(preview, "pool_entries"));
        if (recovery) review += tr("\n\nRecovery release permanently retires this provider identity, including other copies using this node's checkpoint. Signing protections remain active. Afterwards, use the normal Send pages to transfer the released funds. Cancelling makes no retirement or release change.");
        if (retire && !recovery && financeNumber(preview, "uncreated_maintenance") > 0) {
            review += tr("\n\nAlso cancel %n reserve-funding task(s) for which no transaction has been created. Signed work is not cancelled.", nullptr,
                         static_cast<int>(financeNumber(preview, "uncreated_maintenance")));
        }
        const auto answer = askPlainTextQuestion(this,
            recovery ? tr("Recover capital and retire provider identity") : tr("Release all operating capital"), review);
        if (generation != m_wallet_generation || m_privacy) return;
        if (answer != QMessageBox::Yes) {
            setStatusLabel(m_operator_stop_status, tr("Provider stopped. Capital release was not authorized; reserves remain prepared."), "neutral");
            refreshOperatorStatus();
            return;
        }
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("execute", true);
        options.pushKV("plan_id", preview.find_value("plan_id"));
        if (recovery) options.pushKV("recovery", true);
        else if (retire) options.pushKV("cancel_uncreated", true);
        params.push_back(options);
        call("releasepaymastercapital", params, false, nullptr, [this, preview, valid, retire](const UniValue& result) {
            if (m_privacy) return;
            bool matches = valid(result) && result.find_value("executed").isTrue();
            for (const auto* key : {"plan_id", "pool_entries", "dgb_satoshis", "dd_cents", "network_fee_satoshis", "recovery_release", "uncreated_maintenance"})
                matches &= result.find_value(key).write() == preview.find_value(key).write();
            setStatusLabel(m_operator_stop_status, matches
                ? (preview.find_value("recovery_release").isTrue()
                    ? tr("Provider identity permanently retired; reviewed capital is available in this wallet for normal transfers. No funds were sent. Keep the recovery records and create a new full-wallet backup.")
                    : tr("Provider stopped; all reviewed pool capital was released to this wallet. No funds were sent. Create a new full-wallet backup."))
                : tr("Capital release result is unclear. Inspect pool state; do not repeat automatically."), matches ? "ready" : "error");
            if (matches && retire) reviewRetirementBalances(preview.find_value("recovery_release").isTrue());
            else refreshOperatorStatus();
        }, false, [this](const QString& error) {
            if (m_privacy) return;
            const QString detail = error.contains(QLatin1String("PAYMASTER_CAPITAL_RECOVERY_CHECKPOINT_CHANGED"))
                ? readinessExplanation(QStringLiteral("PAYMASTER_CAPITAL_RECOVERY_CHECKPOINT_CHANGED")) : error;
            setStatusLabel(m_operator_stop_status, tr("Capital release was not confirmed. No automatic retry. Inspect pool and recovery state: %1").arg(detail), "action");
            refreshOperatorStatus();
        });
    }, false, [this, retire, recovery](const QString& error) {
        if (m_privacy) return;
        if (!recovery && error.contains(QLatin1String("PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"))) {
            reviewFullCapitalRelease(retire, true);
            return;
        }
        QString detail = error;
        for (const auto* code : {"PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION", "PAYMASTER_CAPITAL_RECOVERY_REQUIRES_RESTORED_WALLET"}) {
            if (error.contains(QLatin1String(code))) { detail = readinessExplanation(QLatin1String(code)); break; }
        }
        setStatusLabel(m_operator_stop_status, tr("Provider stopped; capital was not released. Check synchronization, confirmations, reservations, manual coin locks and unfinished setup in Activity before requesting another review. Never force-unlock signed work. %1").arg(detail), "action");
        refreshOperatorStatus();
    });
}

void PaymasterWidgetImpl::reviewRetirementBalances(bool recovery)
{
    if (m_privacy || !hasRpcTransport()) return;
    m_retirement_summary->show();
    m_retirement_balances->setText(tr("Pool capital was released. Reading remaining wallet balances…"));
    const auto unavailable = [this](const QString&) {
        if (m_privacy) return;
        m_retirement_balances->setText(tr("Remaining balances could not be verified. Do not assume the wallet is empty. Check the normal wallet and DigiDollar balances before archiving; no transfer or wallet closure was attempted."));
        setStatusLabel(m_operator_stop_status, tr("Stopped and capital released; retirement review is incomplete."), "action");
        refreshOperatorStatus();
    };
    call("getbalances", {}, false, nullptr, [this, unavailable, recovery](const UniValue& balances) {
        if (m_privacy) return;
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
            dgb_lines << tr("%1: %2 DGB").arg(field.second, compactDgbAmount(satoshis));
        }
        // minconf=0 includes pending DD; the default would omit it.
        UniValue params{UniValue::VARR}; params.push_back(""); params.push_back(0);
        call("getdigidollarbalance", params, false, nullptr, [this, dgb_lines, unavailable, recovery](const UniValue& dd) {
            if (m_privacy) return;
            qint64 confirmed{0}, pending{0}, total{0};
            if (!GetInt64Field(dd, "confirmed", confirmed) || !GetInt64Field(dd, "unconfirmed", pending) ||
                !GetInt64Field(dd, "total", total) || confirmed < 0 || pending < 0 || total < confirmed || total - confirmed != pending) {
                unavailable({});
                return;
            }
            m_retirement_balances->setText(dgb_lines.join(QLatin1Char('\n')) +
                tr("\nDD confirmed: %1 DD\nDD pending: %2 DD\nBalance observations: %3 (UTC). Not a live or atomic balance guarantee.")
                    .arg(ddAmount(confirmed), ddAmount(pending), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)));
            setStatusLabel(m_operator_stop_status, recovery
                ? tr("Provider identity permanently retired and pool capital released. Review the remaining funds for normal transfers and complete the full-wallet backup before archiving.")
                : tr("Provider stopped and pool capital released. Review the remaining funds and complete the full-wallet backup before archiving."), "neutral");
            refreshOperatorStatus();
        }, false, unavailable);
    }, false, unavailable);
}

void PaymasterWidgetImpl::updateEarningsWithdrawalHint()
{
    const QString withdrawal_hint = m_privacy || !hasCompleteMutationSnapshots() ? QString{} : tr(
        "Minimum payout: %2 DD. Currently accumulated: %1 DD. Earnings remain in the reserve until the minimum is reached; base capital is retained.")
        .arg(PaymasterFormatDD(m_carrier_excess_cents),
             PaymasterFormatDD(Params().GetDigiDollarParams().minOutputAmount));
    m_withdrawal_hint->setText(withdrawal_hint);
    m_withdrawal_hint->setVisible(!m_privacy && hasCompleteMutationSnapshots() &&
        m_carrier_excess_cents < Params().GetDigiDollarParams().minOutputAmount &&
        !m_operation.busy() && m_operation.phase != PaymasterOperationController::Phase::Waiting);
    m_withdraw_task->setToolTip(withdrawal_hint);
    m_preview_carrier_excess->setToolTip(withdrawal_hint);
}

bool PaymasterWidgetImpl::carrierReleaseNeedsReview() const
{
    return m_operation.error_code == QLatin1String("PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE") ||
        m_operation.error_code == QLatin1String("PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED") ||
        m_operation.error_code == QLatin1String("PAYMASTER_OPERATIONAL_CARRIER_TARGET_ALREADY_ZERO");
}

bool PaymasterWidgetImpl::canReviewReserveReduction() const
{
    return m_failed_guided_task == QLatin1String("reduce_reserves") &&
        (m_operation.error_code == QLatin1String("PAYMASTER_RETIREMENT_FEE_LIMIT") ||
         m_operation.error_code == QLatin1String("PAYMASTER_PROVIDER_BUSY") ||
         m_operation.error_code == QLatin1String("PAYMASTER_POOL_PLAN_CHANGED") ||
         m_operation.error_code == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_CHANGED"));
}

bool PaymasterWidgetImpl::waitForReserveReduction(const QString& error, std::function<void()> retry)
{
    // Only this exact Core rejection proves no transaction was started.
    // Never repeat an execution after a missing or ambiguous response.
    if (m_guided_task != QLatin1String("reduce_reserves") ||
        PaymasterQt::StableCode(m_rpc_failure) != QLatin1String("PAYMASTER_PROVIDER_BUSY") ||
        PaymasterQt::Disposition(m_rpc_failure) != QLatin1String("retry_before_execution") || m_guided_busy_retries >= 3) return false;
    ++m_guided_busy_retries;
    m_guided_waiting_for_provider = true;
    renderCurrentTask();
    const auto generation = m_wallet_generation;
    const auto task_generation = m_guided_generation;
    const auto phase = m_operation.phase;
    QTimer::singleShot(750, this, [this, generation, task_generation, phase, retry] {
        if (generation != m_wallet_generation || task_generation != m_guided_generation || m_privacy ||
            m_guided_task != QLatin1String("reduce_reserves") || m_operation.phase != phase) return;
        m_guided_waiting_for_provider = false;
        retry();
    });
    return true;
}
