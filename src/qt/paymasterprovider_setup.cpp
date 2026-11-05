// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

void ProviderSetup::initializeStatusTimer()
{
    m_setup_status_timer = new QTimer(this);
    m_setup_status_timer->setObjectName("paymasterSetupStatusTimer");
    m_setup_status_timer->setInterval(30000);
    connect(m_setup_status_timer, &QTimer::timeout, this, [this] {
        if (widget()->isVisible() && m_panel.hasRpcTransport() && !m_panel.busy() && !wizardActive() &&
            !m_panel.configuration().nodeDialogActive() && !m_panel.privacyEnabled())
            m_panel.overview().refreshOperatorStatus(/*background=*/true);
    });
    m_setup_status_timer->start();
}

void ProviderSetup::configureStatusTimer(bool pending, bool running, bool foreground)
{
    if (!m_setup_status_timer) return;
    m_setup_status_timer->setInterval(pending || running ? 2000 : 30000);
    if (m_panel.hasRpcTransport() && !wizardActive() && !m_panel.privacyEnabled() && foreground)
        m_setup_status_timer->start();
    else m_setup_status_timer->stop();
}

void ProviderSetup::observeProviderStatus(bool has_existing_configuration, bool ready)
{
    if (m_setup_mode == PaymasterSetupMode::UNDECIDED && has_existing_configuration) setSetupMode(PaymasterSetupMode::EXISTING);
    if (m_setup_waiting_for_confirmations && ready) {
        m_setup_waiting_for_confirmations = false;
        if (m_setup_status_timer) m_setup_status_timer->stop();
    }
    m_reopen_wizard->setText(has_existing_configuration ? tr("Review setup with assistant…") : tr("Continue guided setup…"));
}

void ProviderSetup::attachReviewAction(QVBoxLayout* layout)
{
    m_reopen_wizard = new QPushButton(tr("Review setup with assistant…"), layout->parentWidget());
    m_reopen_wizard->setObjectName("paymasterSetupWizard");
    m_reopen_wizard->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    layout->addWidget(m_reopen_wizard, 0, Qt::AlignLeft);
}

void ProviderSetup::reviewSetup() { m_reopen_wizard->click(); }

void ProviderSetup::setBusy(bool busy)
{
    for (auto* button : {m_guided_setup, m_expert_setup, m_reopen_wizard}) if (button) button->setEnabled(!busy);
}


void ProviderSetup::closeWalletDialogs()
{
    m_setup_wizard_active = false;
    if (m_setup_status_timer) m_setup_status_timer->stop();
    if (m_setup_wizard) {
        // Approval pages and unlock leases belong to the wallet which opened them.
        m_setup_wizard->setCloseBlocked(false);
        m_setup_wizard->reject();
        m_setup_wizard.clear();
    }
}

void ProviderSetup::resetWalletPresentation()
{
    m_setup_mode = loadSetupMode();
    m_setup_waiting_for_confirmations = false;
    if (m_setup_status_timer) m_setup_status_timer->stop();
}

void ProviderSetup::setPrivacy(bool privacy)
{
    if (privacy) closeWalletDialogs();
}

bool ProviderSetup::isProviderSetupReadinessError(const QString& error)
{
    return error == QLatin1String("PAYMASTER_PROVIDER_NOT_ENABLED") ||
           error == QLatin1String("PAYMASTER_IDENTITY_NOT_FOUND") ||
           error == QLatin1String("PAYMASTER_POLICY_NOT_FOUND") ||
           error == QLatin1String("PAYMASTER_SAFETY_POLICY_NOT_FOUND") ||
           error == QLatin1String("PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND") ||
           error == QLatin1String("PAYMASTER_POLICY_BINDING_MISMATCH") ||
           error == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE") ||
           error == QLatin1String("PAYMASTER_LIQUIDITY_POLICY_NOT_FOUND") ||
           error == QLatin1String("PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED") ||
           error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED") ||
           error == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE");
}

PaymasterSetupMode ProviderSetup::loadSetupMode() const
{
    const QString key = PaymasterSetupModeSettingsKey(m_panel.walletModel());
    if (key.isEmpty()) return PaymasterSetupMode::UNDECIDED;
    QSettings settings;
    const QString value = settings.value(key).toString();
    if (value == QLatin1String("guided")) return PaymasterSetupMode::GUIDED;
    if (value == QLatin1String("expert")) return PaymasterSetupMode::EXPERT;
    if (value == QLatin1String("existing")) return PaymasterSetupMode::EXISTING;
    return PaymasterSetupMode::UNDECIDED;
}

void ProviderSetup::setSetupMode(PaymasterSetupMode mode)
{
    m_setup_mode = mode;
    const QString key = PaymasterSetupModeSettingsKey(m_panel.walletModel());
    if (!key.isEmpty()) {
        QString value;
        if (mode == PaymasterSetupMode::GUIDED) value = QStringLiteral("guided");
        if (mode == PaymasterSetupMode::EXPERT) value = QStringLiteral("expert");
        if (mode == PaymasterSetupMode::EXISTING) value = QStringLiteral("existing");
        QSettings settings;
        if (value.isEmpty()) {
            settings.remove(key);
        } else {
            settings.setValue(key, value);
        }
    }
    updateSetupAccess();
    m_panel.liquidity().collapseAdvancedSettings();
    m_panel.configuration().hideRuntimeSettings();
}

void ProviderSetup::updateSetupAccess()
{
    const bool unlocked = m_setup_mode != PaymasterSetupMode::UNDECIDED;
    m_panel.overview().showOperatorCard(unlocked && !m_panel.privacyEnabled());
    if (m_setup_choice) m_setup_choice->setVisible(!unlocked);
    m_panel.overview().showConfiguredContent(unlocked);
    if (!m_panel.mainPages()) return;
    for (QWidget* page : {static_cast<QWidget*>(m_panel.settingsPages()), m_panel.page(PaymasterWidgetImpl::Page::FINANCE)}) {
        m_panel.setPageAvailable(page, unlocked && !m_panel.privacyEnabled(), m_panel.privacyEnabled()
            ? tr("This provider page is hidden while privacy mode is active.")
            : unlocked ? QString{} : tr("Complete guided setup or choose manual expert setup to unlock this page."));
    }
    // Recovery remains accessible even before local onboarding is complete.
    m_panel.setPageAvailable(m_panel.page(PaymasterWidgetImpl::Page::ACTIVITY), !m_panel.privacyEnabled(), m_panel.privacyEnabled()
        ? tr("Provider activity details are hidden while privacy mode is active.")
        : !unlocked ? tr("Recovery remains available so existing reservations can always be inspected.") : QString{});
    if (!unlocked || m_panel.privacyEnabled()) m_panel.mainPages()->setCurrentIndex(0);
}

// These are non-owning handles to wizard-owned widgets. Value-captured
// handles and the existing shared flags survive helper return; the
// modal wizard itself stays on showSetupWizard's stack.
struct ProviderSetup::SetupWizardContext {
    QPointer<WalletModel> setup_model{};
    QString setup_wallet_name{};
    QString setup_wallet_id{};
    QCheckBox* wallet_confirmation{nullptr};
    std::shared_ptr<bool> setup_start_requested{};
    std::shared_ptr<UniValue> reviewed_pool{};
};

struct ProviderSetup::SetupOfferControls {
    QWizardPage* identity_page{nullptr};
    QLineEdit* display{nullptr};
    QWizardPage* model_page{nullptr};
    QCheckBox* wizard_user_paid{nullptr};
    QCheckBox* wizard_sponsored{nullptr};
    NoWheelComboBox* wizard_scope{nullptr};
    QPushButton* restore_model_defaults{nullptr};
    QWizardPage* policy_page{nullptr};
    NoWheelDoubleSpinBox* fee{nullptr};
    QSpinBox* maximum_user_paid_service_fee{nullptr};
    QSpinBox* minimum{nullptr};
    QSpinBox* maximum{nullptr};
    QSpinBox* lifetime{nullptr};
    QSpinBox* network_fee{nullptr};
    std::function<qint64()> selected_network_fee{};
    std::shared_ptr<bool> replace_unrepresentable_policy{};
};

struct ProviderSetup::SetupSafetyControls {
    NoWheelComboBox* safety_profile{nullptr};
    DgbAmountLineEdit* custom_per_transaction{nullptr};
    DgbAmountLineEdit* custom_reserved{nullptr};
    DgbAmountLineEdit* custom_per_hour{nullptr};
    DgbAmountLineEdit* custom_per_day{nullptr};
    QSpinBox* custom_completed_per_hour{nullptr};
    QSpinBox* custom_completed_per_day{nullptr};
    QSpinBox* custom_max_active_quotes_total{nullptr};
    QSpinBox* custom_max_active_quotes_per_netgroup{nullptr};
    QSpinBox* custom_max_active_quotes_per_recipient{nullptr};
    QSpinBox* custom_max_quote_requests_per_netgroup{nullptr};
    LiquidityPolicyValues existing_liquidity{};
    bool have_existing_liquidity{false};
    DgbAmountLineEdit* custom_maintenance_per_transaction{nullptr};
    DgbAmountLineEdit* custom_maintenance_per_hour{nullptr};
    DgbAmountLineEdit* custom_maintenance_per_day{nullptr};
    std::shared_ptr<bool> safety_profile_edited{};
    std::function<GuidedPaymasterSafetyLimits()> current_safety_limits{};
};

struct ProviderSetup::SetupPoolControls {
    QCheckBox* automatic_replenishment{nullptr};
    QCheckBox* paid_maintenance_approved{nullptr};
    NoWheelComboBox* operation_mode{nullptr};
    QCheckBox* autostart{nullptr};
    bool have_existing_provider_configuration{false};
    QCheckBox* provider_enabled{nullptr};
    QSpinBox* admission_dgb{nullptr};
    QSpinBox* operational_dgb{nullptr};
    QSpinBox* admission_carriers{nullptr};
    QSpinBox* operational_carriers{nullptr};
};

struct ProviderSetup::SetupReviewControls {
    ValidatedWizardPage* review_page{nullptr};
    QVBoxLayout* review_layout{nullptr};
    QLabel* review{nullptr};
    QLabel* preview_status{nullptr};
    QLabel* preview_details{nullptr};
    QPushButton* preview_retry{nullptr};
    QCheckBox* maintenance_confirmation{nullptr};
    int review_id{-1};
    QCheckBox* start_after_setup{nullptr};
};

struct ProviderSetup::SetupProgressControls {
    CompletableWizardPage* progress_page{nullptr};
    QStringList progress_names{};
    QList<QLabel*> progress_steps{};
    QLabel* progress_result{nullptr};
    QPushButton* setup_retry{nullptr};
    QGroupBox* setup_backup{nullptr};
    QLabel* setup_backup_id{nullptr};
    QLabel* setup_funding{nullptr};
    QPushButton* setup_refresh{nullptr};
    int progress_id{-1};
};

bool ProviderSetup::setupWizardAvailable()
{
    const auto report_unavailable = [this](const QString& title,
                                            const QString& message) {
        // Native modal message boxes are unreliable on the minimal and
        // offscreen Qt platforms used by widget tests. Production keeps
        // the explicit dialog; tests can assert the same message through
        // the persistent provider status label.
        if (m_panel.hasSynchronousTestExecutor()) {
            m_panel.showStatus(message);
            return;
        }
        QMessageBox::information(widget(), title, message);
    };
    if (!m_panel.requirePrivacyOffForSensitiveAction(
            tr("Guided Paymaster setup"))) {
        return false;
    }
    if (m_panel.busy()) {
        report_unavailable(
            tr("Paymaster setup"),
            tr("Provider settings are still being loaded or another Paymaster operation is in progress. Wait for it to finish, then reopen the assistant so it can use the authoritative saved values."));
        return false;
    }
    UniValue recovery_provider{UniValue::VOBJ}, recovery_errors{UniValue::VARR};
    for (const auto& error : m_panel.overview().readinessErrors()) recovery_errors.push_back(error.toStdString());
    recovery_provider.pushKV("readiness_errors", recovery_errors);
    recovery_provider.pushKV("last_service_error", m_panel.overview().lastServiceError().toStdString());
    const auto recovery = DigiDollar::Paymaster::SetupRecoveryIssue(recovery_provider);
    if (!recovery.empty()) {
        m_panel.recovery().showRestoreIssue(QString::fromStdString(recovery));
        return false;
    }
    if (m_panel.walletModel() && (!m_panel.overview().providerInfoAvailable() ||
                    !m_panel.configuration().providerSafetyAvailable() ||
                    !m_panel.liquidity().snapshotAvailable())) {
        report_unavailable(
            tr("Paymaster setup data incomplete"),
            tr("The assistant needs complete, exactly representable safety and liquidity snapshots before it can preserve saved values safely. Refresh the Paymaster status and reopen the assistant after those checks succeed."));
        return false;
    }
    if (m_panel.configuration().policyDirty() || m_panel.configuration().providerSafetyDirty() ||
        m_panel.liquidity().policyDirty() || m_panel.configuration().runtimeDirty() || m_panel.configuration().autostartDirty()) {
        report_unavailable(
            tr("Unsaved Paymaster changes"),
            tr("Save or discard the unsaved Expert-mode changes before opening guided setup. The assistant only imports authoritative values already persisted in the wallet."));
        return false;
    }
    return true;
}

QCheckBox* ProviderSetup::buildSetupWalletPages(PaymasterSetupWizard& wizard, const SetupWizardContext& context)
{
    const auto setup_model = context.setup_model;
    const auto setup_wallet_name = context.setup_wallet_name;
    auto* intro = new CompletableWizardPage(&wizard);
    intro->setObjectName("paymasterSetupRequirementsPage");
    intro->setTitle(tr("Confirm the wallet"));
    intro->setSubTitle(tr("Confirm which wallet will own the provider identity, liquidity and safety settings."));
    auto* intro_page_layout = new QVBoxLayout(intro);
    auto* intro_scroll = new QScrollArea(intro);
    intro_scroll->setObjectName("paymasterSetupRequirementsScroll");
    intro_scroll->setWidgetResizable(true);
    intro_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* intro_content = new QWidget(intro_scroll);
    intro_content->setObjectName("paymasterSetupRequirementsContent");
    auto* intro_layout = new QVBoxLayout(intro_content);
    // Word-wrapped labels otherwise report an overly small vertical size
    // hint while the hidden wizard page is being laid out. Preserve the
    // complete content height and let the page scroll on compact displays.
    intro_layout->setSizeConstraint(QLayout::SetMinimumSize);
    intro_scroll->setWidget(intro_content);
    // setWidget() enables the content widget's native background fill.
    // Disable it so Windows cannot paint a light system surface through
    // the wizard's dark theme.
    ConfigureScrollArea(intro_scroll, intro_content);
    intro_page_layout->addWidget(intro_scroll);
    auto* intro_text = new QLabel(tr(
                                      "A Paymaster supplies DGB network fees for DigiDollar transfers. This assistant "
                                      "prepares a complete, conservative starting configuration and explains every step. "
                                      "Before anything is saved, the assistant shows the complete plan and a current liquidity estimate. "
                                      "After your confirmation it saves the identity, policy and safety limits and creates only missing pool outputs. "
                                      "The selected runtime and enabled settings are shown in the final review. The saved autostart choice is retained; an optional one-time start can be included in the final review."),
                                  intro);
    intro_text->setObjectName("paymasterSetupIntroduction");
    intro_text->setWordWrap(true);
    intro_layout->addWidget(intro_text);

    auto* wallet_frame = new QFrame(intro);
    wallet_frame->setObjectName("paymasterSetupWalletCard");
    wallet_frame->setFrameShape(QFrame::StyledPanel);
    auto* wallet_layout = new QVBoxLayout(wallet_frame);
    auto* wallet_name = new QLabel(
        tr("Provider wallet: %1").arg(setup_wallet_name), wallet_frame);
    wallet_name->setObjectName("paymasterSetupWalletName");
    wallet_name->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                         Qt::TextSelectableByMouse);
    wallet_name->setAccessibleName(tr("Selected Paymaster provider wallet"));
    wallet_name->setAccessibleDescription(tr(
        "The wallet whose identity, prepared liquidity, budgets and provider history will be used by this setup."));
    QFont wallet_name_font = wallet_name->font();
    wallet_name_font.setBold(true);
    wallet_name->setFont(wallet_name_font);
    wallet_layout->addWidget(wallet_name);

    auto* wallet_status = new QLabel(wallet_frame);
    wallet_status->setObjectName("paymasterSetupWalletStatus");
    wallet_status->setWordWrap(true);
    if (!setup_model) {
        wallet_status->setText(tr(
            "No wallet is selected. Close this assistant, create or open a suitable wallet and select it before continuing."));
    } else if (m_panel.overview().hasIdentity()) {
        wallet_status->setText(tr(
            "This wallet already has a Paymaster provider identity. The assistant will retain it unchanged and update only this wallet's configuration."));
    } else if (m_panel.overview().eligible()) {
        wallet_status->setText(tr(
            "Core currently reports this wallet as eligible. Eligibility is checked again immediately before any setting is written."));
    } else {
        wallet_status->setText(tr(
            "Eligibility has not yet been confirmed. Before changing anything, Core will require a descriptor wallet with local private keys and no external signer."));
    }
    wallet_layout->addWidget(wallet_status);

    auto* wallet_recommendation = new QLabel(tr(
        "Recommended: use a dedicated provider wallet. This keeps provider identity, prepared DGB/DD outputs, safety budgets and transaction history separate from everyday personal funds. A dedicated wallet is recommended for operations and accounting, but is not a protocol requirement."),
        wallet_frame);
    wallet_recommendation->setObjectName("paymasterSetupWalletRecommendation");
    wallet_recommendation->setWordWrap(true);
    wallet_layout->addWidget(wallet_recommendation);

    auto* wallet_consequence = new QLabel(tr(
        "Continuing does not create another wallet. The assistant will create or reuse the provider identity and prepare the selected liquidity inside the wallet named above. Paymaster carrier DD remains wallet-owned but is shown as reserved until it is safely retired."),
        wallet_frame);
    wallet_consequence->setObjectName("paymasterSetupWalletConsequence");
    wallet_consequence->setWordWrap(true);
    wallet_layout->addWidget(wallet_consequence);

    auto* wallet_confirmation = new QCheckBox(
        tr("Use \"%1\" as the provider wallet").arg(setup_wallet_name),
        wallet_frame);
    wallet_confirmation->setObjectName("paymasterSetupWalletConfirmation");
    wallet_confirmation->setAccessibleName(tr("Confirm Paymaster provider wallet"));
    wallet_confirmation->setAccessibleDescription(tr(
        "Required confirmation that the assistant may configure only the named wallet."));
    wallet_confirmation->setEnabled(setup_model != nullptr);
    wallet_layout->addWidget(wallet_confirmation);

    auto* choose_another_wallet = new QPushButton(
        tr("Close and choose or create another wallet…"), wallet_frame);
    choose_another_wallet->setObjectName("paymasterSetupChooseAnotherWallet");
    choose_another_wallet->setToolTip(tr(
        "Use DigiByte Core's normal wallet menu and wallet selector, then reopen Paymaster Network in that wallet."));
    wallet_layout->addWidget(choose_another_wallet, 0, Qt::AlignLeft);
    intro_layout->addWidget(wallet_frame);

    auto* wallet_scope_note = new QLabel(tr(
        "This assistant cannot switch wallets and will not create a wallet or move balances. To use another wallet, close the assistant, use the normal wallet menu to create or open it, select that wallet, and reopen Paymaster Network."), intro);
    wallet_scope_note->setObjectName("paymasterSetupWalletScopeNote");
    wallet_scope_note->setWordWrap(true);
    intro_layout->addWidget(wallet_scope_note);

    connect(wallet_confirmation, &QCheckBox::toggled, intro,
            [intro, setup_model](bool checked) {
                intro->setComplete(checked && setup_model);
            });
    connect(choose_another_wallet, &QPushButton::clicked, &wizard,
            [&wizard] { wizard.reject(); });
    intro->setComplete(false);

    auto* current_status = new QLabel(
        m_panel.overview().readinessText().isEmpty()
            ? tr("Current check: refresh the provider status after opening the detailed pages.")
            : tr("Current check:\n%1").arg(m_panel.overview().readinessText()), intro);
    current_status->setObjectName("paymasterSetupCurrentReadiness");
    current_status->setWordWrap(true);
    current_status->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                            Qt::TextSelectableByMouse);
    intro_layout->addWidget(current_status);
    auto* requirements = new QLabel(tr(
                                        "Required before the provider can run:\n\n"
                                        "• a descriptor wallet with local private keys\n"
                                        "• DigiDollar active, a synchronized node, txindex=1 and pruning disabled\n"
                                        "• BIP324 version 2 transport enabled\n"
                                        "• a separate -paymasterbind Direct listener and a reachable -paymasterendpoint\n"
                                        "• sufficient connection capacity (at least 45 with default provider limits)\n"
                                        "• network message capture disabled for mainnet operation\n"
                                        "• confirmed DGB liquidity and, for user-paid service, confirmed DD carriers\n"
                                        "• a saved finite provider safety policy"),
                                    intro);
    requirements->setObjectName("paymasterSetupRequirements");
    requirements->setWordWrap(true);
    intro_layout->addWidget(requirements);
    intro_layout->addStretch();
    wizard.addPage(intro);

    auto* connection_page = new CompletableWizardPage(&wizard);
    connection_page->setTitle(tr("Check the connection"));
    connection_page->setSubTitle(tr("The Direct listener and advertised route are node settings; wallet setup cannot replace them."));
    auto* connection_layout = new QVBoxLayout(connection_page);
    auto* connection_help = new QLabel(tr("Use a separate Direct port and sufficient effective capacity (45 connections with standard provider limits). Tor requires an operator-managed port forwarding rule. A ready local listener does not establish external reachability. After changing node settings, restart explicitly and reopen this assistant."), connection_page);
    connection_help->setWordWrap(true);
    connection_layout->addWidget(connection_help);
    auto* connection_configure = new QPushButton(tr("Close setup and prepare node configuration…"), connection_page);
    connection_layout->addWidget(connection_configure);
    connect(connection_configure, &QPushButton::clicked, &wizard, [this, &wizard] {
        wizard.reject();
        QTimer::singleShot(0, widget(), [this] { m_panel.configuration().configureOperatorNode(); });
    });
    const bool node_configuration_missing = std::any_of(m_panel.overview().readinessErrors().begin(), m_panel.overview().readinessErrors().end(), [](const QString& code) {
        return code == QLatin1String("PAYMASTER_DIRECT_LISTENER_NOT_READY") || code == QLatin1String("PAYMASTER_NO_LOCAL_INBOUND_CAPACITY") || code == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_NOT_CONFIGURED") || code == QLatin1String("PAYMASTER_PROVIDER_ENDPOINT_UNROUTABLE") || code == QLatin1String("PAYMASTER_REQUIRES_V2_TRANSPORT") || code == QLatin1String("PAYMASTER_REQUIRES_PRUNE_0") || code == QLatin1String("PAYMASTER_REQUIRES_TXINDEX");
    });
    connection_page->setComplete(!node_configuration_missing);
    wizard.addPage(connection_page);


    return wallet_confirmation;
}

void ProviderSetup::buildSetupIdentityPage(PaymasterSetupWizard& wizard, SetupOfferControls& offer)
{
    auto& identity_page = offer.identity_page;
    auto& display = offer.display;
    identity_page = new QWizardPage(&wizard);
    identity_page->setObjectName("paymasterSetupIdentityPage");
    identity_page->setTitle(tr("Identity"));
    identity_page->setSubTitle(tr("Choose the public name clients will see for this provider."));
    auto* identity_layout = new QFormLayout(identity_page);
    display = new QLineEdit(m_panel.configuration().offerDraft().display_name, identity_page);
    display->setObjectName("paymasterSetupDisplayName");
    display->setMaxLength(32);
    display->setValidator(new PaymasterDisplayNameValidator(display));
    display->setPlaceholderText(tr("Optional provider name"));
    display->setAccessibleName(tr("Public provider display name"));
    display->setAccessibleDescription(tr(
        "An optional informational name shown to clients. The signed provider identity remains authoritative."));
    display->setEnabled(!m_panel.overview().hasIdentity());
    identity_layout->addRow(tr("Public display name:"), display);
    auto* identity_note = new QLabel(tr(
        "Clients see this optional name. The provider identity is created only after your final approval. "
        "An existing identity and its name are retained."),
        identity_page);
    identity_note->setObjectName("paymasterSetupIdentityExplanation");
    identity_note->setWordWrap(true);
    identity_layout->addRow(identity_note);
    auto* restore_identity_defaults = new QPushButton(tr("Restore defaults"), identity_page);
    restore_identity_defaults->setObjectName("paymasterSetupRestoreIdentityDefaults");
    restore_identity_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    restore_identity_defaults->setEnabled(!m_panel.overview().hasIdentity());
    restore_identity_defaults->setToolTip(m_panel.overview().hasIdentity()
        ? tr("The existing provider identity and its saved display name are retained unchanged.")
        : tr("Clear the optional display name."));
    identity_layout->addRow(restore_identity_defaults);
    connect(restore_identity_defaults, &QPushButton::clicked, identity_page,
            [display] { display->clear(); });
    // The optional name is part of the combined offer step.

}

void ProviderSetup::buildSetupFundingPage(PaymasterSetupWizard& wizard, SetupOfferControls& offer)
{
    auto& model_page = offer.model_page;
    auto& wizard_user_paid = offer.wizard_user_paid;
    auto& wizard_sponsored = offer.wizard_sponsored;
    auto& wizard_scope = offer.wizard_scope;
    auto& restore_model_defaults = offer.restore_model_defaults;
    model_page = new QWizardPage(&wizard);
    model_page->setObjectName("paymasterSetupFundingPage");
    model_page->setTitle(tr("Choose the service model"));
    model_page->setSubTitle(tr("Decide who pays for the service. Both public models may be offered together."));
    auto* model_layout = new QVBoxLayout(model_page);
    wizard_user_paid = new QCheckBox(tr("User paid — recommended for general service"), model_page);
    wizard_user_paid->setObjectName("paymasterSetupUserPaid");
    wizard_user_paid->setChecked(m_panel.configuration().offerDraft().user_paid);
    auto* user_paid_note = new QLabel(tr(
        "The client pays your configured service fee in DigiDollar. Your wallet still supplies the DGB network fee."), model_page);
    user_paid_note->setWordWrap(true);
    wizard_sponsored = new QCheckBox(tr("Sponsored — no DigiDollar service fee"), model_page);
    wizard_sponsored->setObjectName("paymasterSetupSponsored");
    wizard_sponsored->setChecked(m_panel.configuration().offerDraft().sponsored);
    auto* sponsored_note = new QLabel(tr(
        "Your wallet pays the DGB fee without compensation. Public sponsorship can be consumed by unknown clients, so finite budgets are essential."), model_page);
    sponsored_note->setWordWrap(true);
    wizard_scope = new NoWheelComboBox(model_page);
    wizard_scope->setObjectName("paymasterSetupSponsorshipScope");
    wizard_scope->addItem(tr("Public — discovered clients may request sponsorship"), QStringLiteral("public"));
    if (m_panel.configuration().offerDraft().scope == QLatin1String("restricted")) {
        wizard_scope->addItem(tr("Unsupported saved sponsorship setting"), QStringLiteral("restricted"));
        static_cast<QStandardItemModel*>(wizard_scope->model())->item(1)->setEnabled(false);
    }
    const int selected_scope = wizard_scope->findData(m_panel.configuration().offerDraft().scope);
    if (selected_scope >= 0) wizard_scope->setCurrentIndex(selected_scope);
    wizard_scope->setEnabled(wizard_sponsored->isChecked());
    auto* scope_note = new QLabel(tr(
        "Both payment models may be combined. Public sponsorship is available to discovered clients."), model_page);
    scope_note->setWordWrap(true);
    model_layout->addWidget(wizard_user_paid);
    model_layout->addWidget(user_paid_note);
    model_layout->addSpacing(8);
    model_layout->addWidget(wizard_sponsored);
    model_layout->addWidget(sponsored_note);
    model_layout->addWidget(wizard_scope);
    model_layout->addWidget(scope_note);
    restore_model_defaults = new QPushButton(tr("Restore defaults"), model_page);
    restore_model_defaults->setObjectName("paymasterSetupRestoreFundingDefaults");
    restore_model_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    model_layout->addWidget(restore_model_defaults, 0, Qt::AlignLeft);
    model_layout->addStretch();
    // Funding models and offer limits are reviewed together.

    connect(wizard_sponsored, &QCheckBox::toggled, wizard_scope, &QWidget::setEnabled);
    connect(wizard_scope, qOverload<int>(&QComboBox::currentIndexChanged), model_page,
            [wizard_scope, wizard_user_paid, wizard_sponsored] {
                if (wizard_scope->currentData().toString() == QLatin1String("restricted")) {
                    wizard_sponsored->setChecked(true);
                    wizard_user_paid->setChecked(false);
                }
            });
    connect(wizard_user_paid, &QCheckBox::toggled, model_page,
            [wizard_scope](bool paid) {
                if (paid) wizard_scope->setCurrentIndex(wizard_scope->findData(QStringLiteral("public")));
            });
    connect(restore_model_defaults, &QPushButton::clicked, model_page,
            [wizard_user_paid, wizard_sponsored, wizard_scope] {
                wizard_scope->setCurrentIndex(
                    wizard_scope->findData(QStringLiteral("public")));
                wizard_sponsored->setChecked(false);
                wizard_user_paid->setChecked(true);
            });

}

void ProviderSetup::buildSetupPolicyPage(PaymasterSetupWizard& wizard, SetupOfferControls& offer)
{
    auto& wizard_user_paid = offer.wizard_user_paid;
    auto& wizard_sponsored = offer.wizard_sponsored;
    auto& wizard_scope = offer.wizard_scope;
    auto& restore_model_defaults = offer.restore_model_defaults;
    auto& policy_page = offer.policy_page;
    auto& fee = offer.fee;
    auto& maximum_user_paid_service_fee = offer.maximum_user_paid_service_fee;
    auto& minimum = offer.minimum;
    auto& maximum = offer.maximum;
    auto& lifetime = offer.lifetime;
    auto& network_fee = offer.network_fee;
    auto& selected_network_fee = offer.selected_network_fee;
    auto& replace_unrepresentable_policy = offer.replace_unrepresentable_policy;
    policy_page = new QWizardPage(&wizard);
    policy_page->setObjectName("paymasterSetupPolicyPage");
    policy_page->setTitle(tr("Offer limits"));
    policy_page->setSubTitle(tr("Set the payment range, quote lifetime and absolute fee ceiling."));
    auto* policy_layout = new QFormLayout(policy_page);
    fee = new NoWheelDoubleSpinBox(policy_page);
    fee->setObjectName("paymasterSetupServiceFeePercent");
    fee->setRange(0.0, 100.0);
    fee->setDecimals(2);
    fee->setSingleStep(0.10);
    fee->setValue(m_panel.configuration().offerDraft().fee_bps / 100.0);
    fee->setSuffix(tr(" %"));
    fee->setAccessibleName(tr("User-paid service fee in percent"));
    fee->setToolTip(tr(
        "Percentage charged on a user-paid DigiDollar transfer. Changes in 0.10% steps; Core stores the exact equivalent in basis points."));
    const auto update_wizard_fee_model =
        [fee](bool user_paid) {
            fee->setEnabled(user_paid);
        };
    connect(wizard_user_paid, &QCheckBox::toggled, policy_page,
            update_wizard_fee_model);
    update_wizard_fee_model(wizard_user_paid->isChecked());
    maximum_user_paid_service_fee = PaymasterWidgetImpl::scaledSpin(
        policy_page, 0, 10000000,
        m_panel.configuration().offerDraft().maximum_service_fee, 100, 2);
    maximum_user_paid_service_fee->setObjectName(
        "paymasterSetupMaximumUserPaidServiceFee");
    maximum_user_paid_service_fee->setSuffix(tr(" DD"));
    maximum_user_paid_service_fee->setAccessibleName(
        tr("Maximum user-paid service fee"));
    maximum_user_paid_service_fee->setToolTip(
        tr("Maximum DD service fee for one user-paid transfer. Enter 0 for no cap."));
    const auto update_wizard_service_fee_cap_model =
        [maximum_user_paid_service_fee](bool user_paid) {
            maximum_user_paid_service_fee->setEnabled(user_paid);
        };
    connect(wizard_user_paid, &QCheckBox::toggled, policy_page,
            update_wizard_service_fee_cap_model);
    update_wizard_service_fee_cap_model(wizard_user_paid->isChecked());
    minimum = new PaymasterAmountSpinBox(policy_page);
    minimum->setRange(100, 10000000);
    minimum->setValue(std::max(100, m_panel.configuration().offerDraft().minimum));
    minimum->setObjectName("paymasterSetupMinimumPayment");

    minimum->setAccessibleName(tr("Smallest supported payment"));
    maximum = new PaymasterAmountSpinBox(policy_page);
    maximum->setRange(100, 10000000);
    maximum->setValue(std::max(100, m_panel.configuration().offerDraft().maximum));
    maximum->setObjectName("paymasterSetupMaximumPayment");

    maximum->setAccessibleName(tr("Largest supported payment"));
    lifetime = PaymasterWidgetImpl::spin(policy_page, 1, 60, m_panel.configuration().offerDraft().quote_ttl);
    lifetime->setObjectName("paymasterSetupQuoteLifetime");
    lifetime->setSuffix(tr(" seconds"));
    lifetime->setAccessibleName(tr("Quote validity in seconds"));
    network_fee = PaymasterWidgetImpl::scaledSpin(
        policy_page, 1, 2000000000, m_panel.configuration().offerDraft().network_fee, 100000000, 8);
    network_fee->setObjectName("paymasterSetupNetworkFee");
    network_fee->setSuffix(tr(" DGB"));
    network_fee->setAccessibleName(tr("Maximum DGB network fee per transfer"));
    qint64 retained_network_fee = m_panel.configuration().offerDraft().network_fee;
    if (!m_panel.configuration().policyRepresentable()) {
        GetInt64Field(m_panel.configuration().preservedPolicy(),
                      "maximum_network_fee_dgb_satoshis",
                      retained_network_fee);
    }
    selected_network_fee =
        [network_fee, retained_network_fee,
         replace_unrepresentable_policy] {
            return *replace_unrepresentable_policy
                ? static_cast<qint64>(network_fee->value())
                : retained_network_fee;
        };
    policy_layout->addRow(tr("User-paid service fee:"), fee);
    policy_layout->addRow(tr("Maximum user-paid service fee:"),
                          maximum_user_paid_service_fee);
    auto* fee_example = new PaymasterFeeExample(policy_page);
    fee_example->addToForm(policy_layout);
    const auto update_fee_example = [fee_example, fee,
                                     maximum_user_paid_service_fee,
                                     wizard_user_paid] {
        fee_example->setFeeRate(
            qRound(fee->value() * 100.0),
            wizard_user_paid->isChecked(),
            maximum_user_paid_service_fee->value());
    };
    connect(fee, qOverload<double>(&QDoubleSpinBox::valueChanged), fee_example, update_fee_example);
    connect(maximum_user_paid_service_fee,
            qOverload<int>(&QSpinBox::valueChanged), fee_example,
            update_fee_example);
    connect(wizard_user_paid, &QCheckBox::toggled, fee_example, update_fee_example);
    update_fee_example();
    policy_layout->addRow(tr("Smallest payment:"), minimum);
    policy_layout->addRow(tr("Largest payment:"), maximum);
    policy_layout->addRow(tr("Quote validity:"), lifetime);
    policy_layout->addRow(tr("Maximum DGB network fee per transfer:"), network_fee);
    auto* imported_policy = new QLineEdit(policy_page);
    imported_policy->setObjectName("paymasterSetupImportedRawPolicy");
    imported_policy->setReadOnly(true);
    imported_policy->setVisible(!m_panel.configuration().policyRepresentable());
    imported_policy->setText(!m_panel.configuration().policyRepresentable()
        ? QString::fromStdString(
              m_panel.configuration().preservedPolicy().write())
        : QString{});
    imported_policy->setAccessibleName(
        tr("Exact persisted provider policy retained unchanged"));
    if (!m_panel.configuration().policyRepresentable()) {
        policy_layout->addRow(
            tr("Persisted policy (read-only):"), imported_policy);
    }
    auto* restore_policy_defaults = new QPushButton(tr("Restore defaults"), policy_page);
    restore_policy_defaults->setObjectName("paymasterSetupRestorePolicyDefaults");
    restore_policy_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    policy_layout->addRow(restore_policy_defaults);
    auto* field_help = new QFrame(policy_page);
    field_help->setObjectName("paymasterSetupFieldHelp");
    field_help->setFrameShape(QFrame::StyledPanel);
    auto* field_help_layout = new QVBoxLayout(field_help);
    field_help_layout->setContentsMargins(12, 9, 12, 9);
    field_help_layout->setSpacing(3);
    auto* field_help_title = new QLabel(field_help);
    field_help_title->setObjectName("paymasterSetupFieldHelpTitle");
    auto* field_help_text = new QLabel(field_help);
    field_help_text->setObjectName("paymasterSetupFieldHelpText");
    field_help_text->setWordWrap(true);
    field_help_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    field_help_layout->addWidget(field_help_title);
    field_help_layout->addWidget(field_help_text);
    policy_layout->addRow(field_help);

    const auto update_policy_help =
        [this, fee, maximum_user_paid_service_fee, minimum, maximum,
         lifetime, network_fee, field_help_title,
         field_help_text](QWidget* field) {
            if (field == fee) {
                field_help_title->setText(tr("User-paid service fee"));
                field_help_text->setText(tr(
                    "For user-paid offers, the provider charges %1% of the transferred $DD amount. "
                    "Core stores this exactly as %2 basis points. Sponsored transfers always charge 0.00% service fee.")
                    .arg(QString::number(fee->value(), 'f', 2))
                    .arg(qRound(fee->value() * 100.0)));
            } else if (field == maximum_user_paid_service_fee) {
                field_help_title->setText(
                    tr("Maximum user-paid service fee"));
                field_help_text->setText(
                    maximum_user_paid_service_fee->value() == 0
                        ? tr("The percentage fee has no DD ceiling.")
                        : tr("The service fee never exceeds %1 DD, even when the percentage would be higher.")
                              .arg(QString::number(
                                  maximum_user_paid_service_fee->value() /
                                      100.0,
                                  'f', 2)));
            } else if (field == minimum) {
                field_help_title->setText(tr("Smallest supported payment"));
                field_help_text->setText(tr(
                    "Your provider will not offer service for payments below %1 $DD. "
                    "A sensible minimum avoids spending provider resources on very small requests.")
                    .arg(PaymasterFormatDD(minimum->value())));
            } else if (field == maximum) {
                field_help_title->setText(tr("Largest supported payment"));
                field_help_text->setText(tr(
                    "Your provider will reject payments above %1 $DD. Keep this limit aligned with the amount of liquidity and financial exposure you intend to provide.")
                    .arg(PaymasterFormatDD(maximum->value())));
            } else if (field == lifetime) {
                field_help_title->setText(tr("Quote validity"));
                field_help_text->setText(tr(
                    "A client has %1 seconds to accept this provider quote. Shorter validity releases unused reservations sooner; longer validity gives slower clients more time.")
                    .arg(lifetime->value()));
            } else if (field == network_fee) {
                field_help_title->setText(tr("Maximum DGB network fee per transfer"));
                field_help_text->setText(tr(
                    "The provider will never fund more than %1 DGB of network fee for one transfer. "
                    "The wallet safety profile does not lower this individual-transfer ceiling; it limits repeated requests through finite hourly, daily and completed-transfer budgets.")
                    .arg(PaymasterFormatDGB(network_fee->value())));
            }
        };
    connect(qApp, &QApplication::focusChanged, policy_page,
            [policy_page, fee, maximum_user_paid_service_fee, minimum,
             maximum, lifetime, network_fee,
             update_policy_help](QWidget*, QWidget* focused) {
                QWidget* candidate = focused;
                while (candidate && candidate != policy_page) {
                    if (candidate == fee ||
                        candidate == maximum_user_paid_service_fee ||
                        candidate == minimum || candidate == maximum ||
                        candidate == lifetime || candidate == network_fee) {
                        update_policy_help(candidate);
                        return;
                    }
                    candidate = candidate->parentWidget();
                }
            });
    connect(fee, qOverload<double>(&QDoubleSpinBox::valueChanged), policy_page,
            [fee, update_policy_help] { update_policy_help(fee); });
    connect(maximum_user_paid_service_fee,
            qOverload<int>(&QSpinBox::valueChanged), policy_page,
            [maximum_user_paid_service_fee, update_policy_help] {
                update_policy_help(maximum_user_paid_service_fee);
            });
    connect(restore_policy_defaults, &QPushButton::clicked, policy_page,
            [fee, maximum_user_paid_service_fee, minimum, maximum,
             lifetime, network_fee,
             wizard_user_paid, wizard_sponsored, wizard_scope,
             restore_model_defaults, imported_policy,
             replace_unrepresentable_policy] {
                *replace_unrepresentable_policy = true;
                imported_policy->hide();
                wizard_user_paid->setEnabled(true);
                wizard_sponsored->setEnabled(true);
                wizard_scope->setEnabled(
                    wizard_sponsored->isChecked());
                restore_model_defaults->setEnabled(true);
                fee->setEnabled(wizard_user_paid->isChecked());
                maximum_user_paid_service_fee->setEnabled(
                    wizard_user_paid->isChecked());
                minimum->setEnabled(true);
                maximum->setEnabled(true);
                lifetime->setEnabled(true);
                network_fee->setEnabled(true);
                fee->setValue(wizard_user_paid->isChecked() ? 0.50 : 0.0);
                maximum_user_paid_service_fee->setValue(0);
                minimum->setValue(100);
                maximum->setValue(100000);
                lifetime->setValue(60);
                network_fee->setValue(20000000);
            });
    if (!m_panel.configuration().policyRepresentable()) {
        wizard_user_paid->setEnabled(false);
        wizard_sponsored->setEnabled(false);
        wizard_scope->setEnabled(false);
        restore_model_defaults->setEnabled(false);
        fee->setEnabled(false);
        maximum_user_paid_service_fee->setEnabled(false);
        minimum->setEnabled(false);
        maximum->setEnabled(false);
        lifetime->setEnabled(false);
        network_fee->setEnabled(false);
        field_help_text->setText(tr(
            "The exact persisted Core policy is retained read-only because at least one value is outside this interface's range. You may edit other setup pages without replacing it. Choose Restore defaults explicitly only if you intend the assistant to replace the complete operating policy."));
    }
    for (QSpinBox* control : {maximum_user_paid_service_fee, minimum,
                  maximum, lifetime, network_fee}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged), policy_page,
                [control, update_policy_help] { update_policy_help(control); });
    }
    update_policy_help(fee);
}

ProviderSetup::SetupOfferControls ProviderSetup::buildSetupOfferPage(PaymasterSetupWizard& wizard, const std::shared_ptr<bool>& replace_unrepresentable_policy)
{
    SetupOfferControls offer;
    offer.replace_unrepresentable_policy = replace_unrepresentable_policy;
    buildSetupIdentityPage(wizard, offer);
    buildSetupFundingPage(wizard, offer);
    buildSetupPolicyPage(wizard, offer);
    const auto identity_page = offer.identity_page;
    const auto model_page = offer.model_page;
    const auto policy_page = offer.policy_page;
    auto* offer_page = new QWizardPage(&wizard);
    offer_page->setObjectName("paymasterSetupOfferPage");
    offer_page->setTitle(tr("Set your offer"));
    offer_page->setSubTitle(tr("Choose who pays for your service and review the price clients will see."));
    auto* offer_page_layout = new QVBoxLayout(offer_page);
    auto* offer_scroll = new QScrollArea(offer_page);
    offer_scroll->setObjectName("paymasterSetupOfferScroll");
    auto* offer_contents = new QWidget(offer_scroll);
    offer_contents->setObjectName("paymasterSetupOfferContent");
    auto* offer_layout = new QVBoxLayout(offer_contents);
    offer_layout->addWidget(identity_page);
    offer_layout->addWidget(model_page);
    offer_layout->addWidget(policy_page);
    offer_scroll->setWidgetResizable(true);
    offer_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    offer_scroll->setWidget(offer_contents);
    ConfigureScrollArea(offer_scroll, offer_contents);
    offer_page_layout->addWidget(offer_scroll);
    wizard.addPage(offer_page);

    return offer;
}

ProviderSetup::SetupSafetyControls ProviderSetup::buildSetupSafetyPage(PaymasterSetupWizard& wizard, const SetupOfferControls& offer)
{
    SetupSafetyControls safety;
    auto& safety_profile = safety.safety_profile;
    auto& custom_per_transaction = safety.custom_per_transaction;
    auto& custom_reserved = safety.custom_reserved;
    auto& custom_per_hour = safety.custom_per_hour;
    auto& custom_per_day = safety.custom_per_day;
    auto& custom_completed_per_hour = safety.custom_completed_per_hour;
    auto& custom_completed_per_day = safety.custom_completed_per_day;
    auto& custom_max_active_quotes_total = safety.custom_max_active_quotes_total;
    auto& custom_max_active_quotes_per_netgroup = safety.custom_max_active_quotes_per_netgroup;
    auto& custom_max_active_quotes_per_recipient = safety.custom_max_active_quotes_per_recipient;
    auto& custom_max_quote_requests_per_netgroup = safety.custom_max_quote_requests_per_netgroup;
    auto& existing_liquidity = safety.existing_liquidity;
    auto& have_existing_liquidity = safety.have_existing_liquidity;
    auto& custom_maintenance_per_transaction = safety.custom_maintenance_per_transaction;
    auto& custom_maintenance_per_hour = safety.custom_maintenance_per_hour;
    auto& custom_maintenance_per_day = safety.custom_maintenance_per_day;
    auto& safety_profile_edited = safety.safety_profile_edited;
    auto& current_safety_limits = safety.current_safety_limits;
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto network_fee = offer.network_fee;
    const auto selected_network_fee = offer.selected_network_fee;
    auto* safety_page = new QWizardPage(&wizard);
    safety_page->setObjectName("paymasterSetupSafetyPage");
    safety_page->setTitle(tr("Limit spending"));
    safety_page->setSubTitle(tr("Choose finite wallet-local spending and request limits."));
    auto* safety_page_layout = new QVBoxLayout(safety_page);
    auto* safety_scroll = new QScrollArea(safety_page);
    safety_scroll->setObjectName("paymasterSetupSafetyScroll");
    safety_scroll->setWidgetResizable(true);
    safety_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* safety_content = new QWidget(safety_scroll);
    safety_content->setObjectName("paymasterSetupSafetyContent");
    auto* safety_layout = new QVBoxLayout(safety_content);
    safety_layout->setSizeConstraint(QLayout::SetMinimumSize);
    safety_scroll->setWidget(safety_content);
    ConfigureScrollArea(safety_scroll, safety_content);
    safety_page_layout->addWidget(safety_scroll);
    auto* safety_text = new QLabel(tr(
        "Safety limits are the final brake against repeated or expensive remote requests. "
        "They are never advertised and never mean unlimited. Selected service models receive "
        "finite limits; models not selected by the operating policy remain unavailable even "
        "when their previously saved limits are retained. When you "
        "explicitly choose or edit a profile, its transfer and aggregate limits are shared by all "
        "selected funding models; otherwise distinct saved model limits are retained."), safety_page);
    safety_text->setObjectName("paymasterSetupSafetyExplanation");
    safety_text->setWordWrap(true);
    safety_profile = new NoWheelComboBox(safety_page);
    safety_profile->setObjectName("paymasterSetupSafetyProfile");
    safety_profile->addItem(tr("Conservative — lower initial hourly and daily exposure"), QStringLiteral("conservative"));
    safety_profile->addItem(tr("Recommended — standard finite starting limits"), QStringLiteral("recommended"));
    safety_profile->addItem(tr("Custom — configure every aggregate and request limit"), QStringLiteral("custom"));
    const auto retained_safety = m_panel.configuration().selectedFundingSafety();
    const FundingSafetyValues existing_safety = retained_safety.value_or(FundingSafetyValues{});
    const bool have_existing_safety = m_panel.configuration().providerSafetyConfigured() &&
        retained_safety && !existing_safety.allZero();
    const GuidedPaymasterSafetyLimits conservative_safety = GuidedSafetyLimits(
        /*conservative=*/true, selected_network_fee());
    auto* custom_safety = new QWidget(safety_page);
    custom_safety->setObjectName("paymasterSetupCustomSafety");
    auto* custom_safety_form = new QFormLayout(custom_safety);
    const GuidedPaymasterSafetyLimits recommended_safety = GuidedSafetyLimits(
        /*conservative=*/false, selected_network_fee());
    const GuidedPaymasterSafetyLimits initial_custom_safety = have_existing_safety
        ? GuidedPaymasterSafetyLimits{
              existing_safety.per_transaction, existing_safety.reserved,
              existing_safety.per_hour, existing_safety.per_day,
              existing_safety.completed_per_hour,
              existing_safety.completed_per_day}
        : recommended_safety;
    custom_per_transaction = new DgbAmountLineEdit(
        initial_custom_safety.per_transaction, custom_safety);
    custom_per_transaction->setObjectName(
        "paymasterSetupCustomPerTransaction");
    custom_reserved = new DgbAmountLineEdit(initial_custom_safety.reserved, custom_safety);
    custom_reserved->setObjectName("paymasterSetupCustomReserved");
    custom_per_hour = new DgbAmountLineEdit(initial_custom_safety.per_hour, custom_safety);
    custom_per_hour->setObjectName("paymasterSetupCustomPerHour");
    custom_per_day = new DgbAmountLineEdit(initial_custom_safety.per_day, custom_safety);
    custom_per_day->setObjectName("paymasterSetupCustomPerDay");
    custom_completed_per_hour = PaymasterWidgetImpl::spin(custom_safety, 1, 1000000,
                                           initial_custom_safety.completed_per_hour);
    custom_completed_per_hour->setObjectName("paymasterSetupCustomCompletedPerHour");
    custom_completed_per_day = PaymasterWidgetImpl::spin(custom_safety, 1, 1000000,
                                          initial_custom_safety.completed_per_day);
    custom_completed_per_day->setObjectName("paymasterSetupCustomCompletedPerDay");
    const int initial_max_active_quotes_total = have_existing_safety
        ? m_panel.configuration().quoteLimits().total : 16;
    const int initial_max_active_quotes_per_netgroup = have_existing_safety
        ? m_panel.configuration().quoteLimits().netgroup : 4;
    const int initial_max_active_quotes_per_recipient = have_existing_safety
        ? m_panel.configuration().quoteLimits().recipient : 2;
    const int initial_max_quote_requests_per_netgroup = have_existing_safety
        ? m_panel.configuration().quoteLimits().requests : 10;
    custom_max_active_quotes_total = PaymasterWidgetImpl::spin(
        custom_safety, 1, 8192, initial_max_active_quotes_total);
    custom_max_active_quotes_total->setObjectName(
        "paymasterSetupCustomMaxActiveQuotesTotal");
    custom_max_active_quotes_per_netgroup = PaymasterWidgetImpl::spin(
        custom_safety, 1, 8192, initial_max_active_quotes_per_netgroup);
    custom_max_active_quotes_per_netgroup->setObjectName(
        "paymasterSetupCustomMaxActiveQuotesPerNetgroup");
    custom_max_active_quotes_per_recipient = PaymasterWidgetImpl::spin(
        custom_safety, 1, 8192, initial_max_active_quotes_per_recipient);
    custom_max_active_quotes_per_recipient->setObjectName(
        "paymasterSetupCustomMaxActiveQuotesPerRecipient");
    custom_max_quote_requests_per_netgroup = PaymasterWidgetImpl::spin(
        custom_safety, 1, 8192, initial_max_quote_requests_per_netgroup);
    custom_max_quote_requests_per_netgroup->setObjectName(
        "paymasterSetupCustomMaxQuoteRequestsPerNetgroupMinute");
    have_existing_liquidity =
        m_panel.liquidity().policyConfigured() &&
        m_panel.liquidity().readLiquidityPolicy(existing_liquidity);
    const auto refill_defaults = DigiDollar::Paymaster::SetupDefaultLiquidity(wizard_user_paid->isChecked());
    custom_maintenance_per_transaction = new DgbAmountLineEdit(
        have_existing_liquidity ? existing_liquidity.fee_per_transaction : refill_defaults.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>(),
        custom_safety);
    custom_maintenance_per_transaction->setObjectName("paymasterSetupCustomMaintenancePerTransaction");
    custom_maintenance_per_hour = new DgbAmountLineEdit(
        have_existing_liquidity ? existing_liquidity.fee_per_hour : refill_defaults.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>(),
        custom_safety);
    custom_maintenance_per_hour->setObjectName("paymasterSetupCustomMaintenancePerHour");
    custom_maintenance_per_day = new DgbAmountLineEdit(
        have_existing_liquidity ? existing_liquidity.fee_per_day : refill_defaults.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>(),
        custom_safety);
    custom_maintenance_per_day->setObjectName("paymasterSetupCustomMaintenancePerDay");
    custom_safety_form->addRow(tr("Network-fee budget per transfer (DGB):"), custom_per_transaction);
    custom_safety_form->addRow(tr("Reserved network-fee budget (DGB):"), custom_reserved);
    custom_safety_form->addRow(tr("Network-fee budget per rolling hour (DGB):"), custom_per_hour);
    custom_safety_form->addRow(tr("Network-fee budget per rolling day (DGB):"), custom_per_day);
    custom_safety_form->addRow(tr("Completed transfers per hour:"), custom_completed_per_hour);
    custom_safety_form->addRow(tr("Completed transfers per day:"), custom_completed_per_day);
    custom_safety_form->addRow(tr("Active quotes across all peers:"), custom_max_active_quotes_total);
    custom_safety_form->addRow(tr("Active quotes per netgroup:"), custom_max_active_quotes_per_netgroup);
    custom_safety_form->addRow(tr("Active quotes per recipient bucket:"), custom_max_active_quotes_per_recipient);
    custom_safety_form->addRow(tr("Quote requests per netgroup/minute:"), custom_max_quote_requests_per_netgroup);
    const auto safety_matches = [](const FundingSafetyValues& values,
                                   const GuidedPaymasterSafetyLimits& limits) {
        return values.per_transaction == limits.per_transaction &&
            values.reserved == limits.reserved &&
            values.per_hour == limits.per_hour &&
            values.per_day == limits.per_day &&
            values.completed_per_hour == limits.completed_per_hour &&
            values.completed_per_day == limits.completed_per_day;
    };
    const auto selected_models_match_safety =
        [this, safety_matches](const GuidedPaymasterSafetyLimits& limits) {
            using namespace DigiDollar::Paymaster;
            const auto offer = m_panel.configuration().offerDraft();
            const auto paid = m_panel.configuration().fundingSafety(FundingModel::USER_PAID);
            if (offer.user_paid && (!paid || !safety_matches(*paid, limits))) return false;
            const auto sponsored = m_panel.configuration().fundingSafety(FundingModel::SPONSORED,
                offer.scope == QLatin1String("restricted") ? SponsorshipScope::RESTRICTED : SponsorshipScope::PUBLIC);
            if (offer.sponsored && (!sponsored || !safety_matches(*sponsored, limits))) return false;
            return true;
        };
    QString initial_safety_profile{QStringLiteral("recommended")};
    const bool default_quote_limits =
        initial_max_active_quotes_total == 16 &&
        initial_max_active_quotes_per_netgroup == 4 &&
        initial_max_active_quotes_per_recipient == 2 &&
        initial_max_quote_requests_per_netgroup == 10;
    if (have_existing_safety) {
        initial_safety_profile =
            (!have_existing_safety ||
             selected_models_match_safety(conservative_safety)) &&
                default_quote_limits
            ? QStringLiteral("conservative")
            : (!have_existing_safety ||
               selected_models_match_safety(recommended_safety)) &&
                  default_quote_limits
                ? QStringLiteral("recommended")
                : QStringLiteral("custom");
    }
    safety_profile->setCurrentIndex(
        safety_profile->findData(initial_safety_profile));
    safety_profile_edited = std::make_shared<bool>(false);
    custom_safety->setVisible(false);
    auto* safety_summary = new QLabel(safety_page);
    safety_summary->setObjectName("paymasterSetupSafetySummary");
    safety_summary->setWordWrap(true);
    current_safety_limits = [safety_profile,
                                        selected_network_fee,
                                        custom_per_transaction, custom_reserved,
                                        custom_per_hour, custom_per_day,
                                        custom_completed_per_hour,
                                        custom_completed_per_day] {
        const bool conservative = safety_profile->currentData().toString() ==
            QLatin1String("conservative");
        GuidedPaymasterSafetyLimits limits = GuidedSafetyLimits(
            conservative, selected_network_fee());
        if (safety_profile->currentData().toString() == QLatin1String("custom")) {
            if (!custom_per_transaction->satoshis(limits.per_transaction) ||
                !custom_reserved->satoshis(limits.reserved) ||
                !custom_per_hour->satoshis(limits.per_hour) ||
                !custom_per_day->satoshis(limits.per_day)) {
                limits.per_transaction = limits.reserved =
                    limits.per_hour = limits.per_day = -1;
            }
            limits.completed_per_hour = custom_completed_per_hour->value();
            limits.completed_per_day = custom_completed_per_day->value();
        }
        return limits;
    };
    const auto update_safety_summary = [this, safety_profile, safety_summary,
                                        custom_safety, current_safety_limits,
                                        custom_max_active_quotes_total,
                                        custom_max_active_quotes_per_netgroup,
                                        custom_max_active_quotes_per_recipient,
                                        custom_max_quote_requests_per_netgroup] {
        custom_safety->setVisible(safety_profile->currentData().toString() ==
                                  QLatin1String("custom"));
        const GuidedPaymasterSafetyLimits limits = current_safety_limits();
        if (limits.reserved < 0) {
            safety_summary->setText(tr("Enter valid DGB amounts for all custom safety limits."));
            return;
        }
        safety_summary->setText(tr(
            "The selected wallet limit allows %1 DGB per transfer. Spam protection allows "
            "%2 DGB reserved, %3 DGB per rolling hour and %4 DGB per rolling day; "
            "%5 completed transfers per hour and %6 per day. Request admission permits at most "
            "%7 active quotes total, %8 per netgroup, %9 per recipient bucket and %10 quote "
            "requests per netgroup/minute.")
            .arg(PaymasterFormatDGB(limits.per_transaction),
                 PaymasterFormatDGB(limits.reserved),
                 PaymasterFormatDGB(limits.per_hour),
                 PaymasterFormatDGB(limits.per_day))
            .arg(limits.completed_per_hour)
            .arg(limits.completed_per_day)
            .arg(custom_max_active_quotes_total->value())
            .arg(custom_max_active_quotes_per_netgroup->value())
            .arg(custom_max_active_quotes_per_recipient->value())
            .arg(custom_max_quote_requests_per_netgroup->value()));
    };
    connect(safety_profile, qOverload<int>(&QComboBox::currentIndexChanged),
            safety_page,
            [update_safety_summary, safety_profile_edited, safety_profile,
             custom_max_active_quotes_total,
             custom_max_active_quotes_per_netgroup,
             custom_max_active_quotes_per_recipient,
             custom_max_quote_requests_per_netgroup] {
                *safety_profile_edited = true;
                if (safety_profile->currentData().toString() !=
                    QLatin1String("custom")) {
                    custom_max_active_quotes_total->setValue(16);
                    custom_max_active_quotes_per_netgroup->setValue(4);
                    custom_max_active_quotes_per_recipient->setValue(2);
                    custom_max_quote_requests_per_netgroup->setValue(10);
                }
                update_safety_summary();
            });
    connect(network_fee, qOverload<int>(&QSpinBox::valueChanged), safety_page,
            [update_safety_summary](int) { update_safety_summary(); });
    for (QLineEdit* control : {custom_per_transaction, custom_reserved, custom_per_hour,
                               custom_per_day}) {
        connect(control, &QLineEdit::textChanged, safety_page,
                [update_safety_summary, safety_profile_edited] {
                    *safety_profile_edited = true;
                    update_safety_summary();
                });
    }
    for (QSpinBox* control : {custom_completed_per_hour, custom_completed_per_day}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged), safety_page,
                [update_safety_summary, safety_profile_edited](int) {
                    *safety_profile_edited = true;
                    update_safety_summary();
                });
    }
    for (QSpinBox* control : {custom_max_active_quotes_total,
                              custom_max_active_quotes_per_netgroup,
                              custom_max_active_quotes_per_recipient,
                              custom_max_quote_requests_per_netgroup}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged), safety_page,
                [update_safety_summary](int) { update_safety_summary(); });
    }
    auto* restore_safety_defaults = new QPushButton(tr("Restore defaults"), safety_page);
    restore_safety_defaults->setObjectName("paymasterSetupRestoreSafetyDefaults");
    restore_safety_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    connect(restore_safety_defaults, &QPushButton::clicked, safety_page,
            [safety_profile, network_fee, custom_per_transaction,
             custom_reserved, custom_per_hour,
             custom_per_day, custom_completed_per_hour,
             custom_completed_per_day,
             custom_max_active_quotes_total,
             custom_max_active_quotes_per_netgroup,
             custom_max_active_quotes_per_recipient,
             custom_max_quote_requests_per_netgroup,
             safety_profile_edited] {
                *safety_profile_edited = true;
                const GuidedPaymasterSafetyLimits defaults = GuidedSafetyLimits(
                    /*conservative=*/false, network_fee->value());
                custom_per_transaction->setSatoshis(
                    defaults.per_transaction);
                custom_reserved->setSatoshis(defaults.reserved);
                custom_per_hour->setSatoshis(defaults.per_hour);
                custom_per_day->setSatoshis(defaults.per_day);
                custom_completed_per_hour->setValue(defaults.completed_per_hour);
                custom_completed_per_day->setValue(defaults.completed_per_day);
                custom_max_active_quotes_total->setValue(16);
                custom_max_active_quotes_per_netgroup->setValue(4);
                custom_max_active_quotes_per_recipient->setValue(2);
                custom_max_quote_requests_per_netgroup->setValue(10);
                safety_profile->setCurrentIndex(
                    safety_profile->findData(QStringLiteral("recommended")));
            });
    update_safety_summary();
    safety_layout->addWidget(safety_text);
    safety_layout->addWidget(safety_profile);
    safety_layout->addWidget(custom_safety);
    safety_layout->addWidget(safety_summary);
    safety_layout->addWidget(restore_safety_defaults, 0, Qt::AlignLeft);
    safety_layout->addStretch();
    wizard.addPage(safety_page);

    return safety;
}

ProviderSetup::SetupPoolControls ProviderSetup::buildSetupLiquidityPage(PaymasterSetupWizard& wizard, const SetupOfferControls& offer, const SetupSafetyControls& safety)
{
    SetupPoolControls pool;
    auto& automatic_replenishment = pool.automatic_replenishment;
    auto& paid_maintenance_approved = pool.paid_maintenance_approved;
    auto& operation_mode = pool.operation_mode;
    auto& autostart = pool.autostart;
    auto& have_existing_provider_configuration = pool.have_existing_provider_configuration;
    auto& provider_enabled = pool.provider_enabled;
    auto& admission_dgb = pool.admission_dgb;
    auto& operational_dgb = pool.operational_dgb;
    auto& admission_carriers = pool.admission_carriers;
    auto& operational_carriers = pool.operational_carriers;
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto network_fee = offer.network_fee;
    const auto selected_network_fee = offer.selected_network_fee;
    const auto existing_liquidity = safety.existing_liquidity;
    const auto have_existing_liquidity = safety.have_existing_liquidity;
    const auto custom_maintenance_per_transaction = safety.custom_maintenance_per_transaction;
    const auto custom_maintenance_per_hour = safety.custom_maintenance_per_hour;
    const auto custom_maintenance_per_day = safety.custom_maintenance_per_day;
    auto* pool_page = new QWizardPage(&wizard);
    pool_page->setObjectName("paymasterSetupLiquidityPage");
    pool_page->setTitle(tr("Reserves and operation"));
    pool_page->setSubTitle(tr("Choose payment capacity and review its capital and refill limits."));
    auto* pool_page_layout = new QVBoxLayout(pool_page);
    auto* pool_scroll = new QScrollArea(pool_page);
    pool_scroll->setObjectName("paymasterSetupLiquidityScroll");
    pool_scroll->setWidgetResizable(true);
    pool_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* pool_content = new QWidget(pool_scroll);
    pool_content->setObjectName("paymasterSetupLiquidityContent");
    auto* pool_layout = new QFormLayout(pool_content);
    pool_layout->setSizeConstraint(QLayout::SetMinimumSize);
    pool_scroll->setWidget(pool_content);
    ConfigureScrollArea(pool_scroll, pool_content);
    pool_page_layout->addWidget(pool_scroll);
    automatic_replenishment = new QCheckBox(
        tr("Automatically refill missing reserves"), pool_page);
    automatic_replenishment->setObjectName("paymasterSetupAutomaticReplenishment");
    automatic_replenishment->setChecked(
        have_existing_liquidity ? existing_liquidity.automatic_replenishment : true);
    paid_maintenance_approved = new QCheckBox(
        tr("Allow refill transactions within these fee limits"),
        pool_page);
    paid_maintenance_approved->setObjectName("paymasterSetupPaidMaintenanceApproved");
    paid_maintenance_approved->setChecked(
        have_existing_liquidity ? existing_liquidity.paid_maintenance_approved : true);
    operation_mode = new NoWheelComboBox(pool_page);
    operation_mode->setObjectName("paymasterSetupOperationMode");
    operation_mode->addItem(tr("Automatic — recommended"), QStringLiteral("automatic"));
    operation_mode->addItem(tr("Manual — expert mode"), QStringLiteral("manual"));
    const int existing_operation_mode = operation_mode->findData(m_panel.overview().operationMode());
    operation_mode->setCurrentIndex(existing_operation_mode >= 0
        ? existing_operation_mode
        : operation_mode->findData(QStringLiteral("automatic")));
    operation_mode->setEnabled(!m_panel.overview().running());
    operation_mode->setToolTip(m_panel.overview().running()
        ? tr("Pause the provider before changing its processing mode.")
        : tr("Automatic is recommended; manual mode is intended for expert diagnosis."));
    autostart = new QCheckBox(
        tr("Start this provider automatically after loading its wallet"), pool_page);
    autostart->setObjectName("paymasterSetupAutostart");
    autostart->setToolTip(tr("Start when this wallet is ready, including after restarts. An encrypted wallet still needs manual unlock after restart. This choice is saved only after the final review; it is separate from a one-time start."));
    // An identity can be created before any ProviderSettings record exists.
    // Treat that identity-only state as a fresh setup so the usable default
    // remains enabled; preserve an explicit disabled state only for an
    // existing policy/configuration.
    have_existing_provider_configuration =
        m_panel.overview().settingsPresent() || m_panel.configuration().policyLoaded() ||
        m_panel.configuration().providerSafetyConfigured() ||
        m_panel.liquidity().policyConfigured();
    autostart->setChecked(have_existing_provider_configuration ? m_panel.configuration().autostartEnabled() : true);
    provider_enabled = new QCheckBox(
        tr("Enable this wallet's provider configuration"), pool_page);
    provider_enabled->setObjectName("paymasterSetupProviderEnabled");
    provider_enabled->setChecked(
        have_existing_provider_configuration ? m_panel.overview().enabled() : true);
    admission_dgb = PaymasterWidgetImpl::spin(
        pool_page, 3, 16,
        have_existing_liquidity ? existing_liquidity.admission_dgb : 3);
    admission_dgb->setObjectName("paymasterSetupAdmissionDgbSlots");
    admission_dgb->setAccessibleName(tr("Reserve DGB slots for network admission"));
    operational_dgb = PaymasterWidgetImpl::spin(
        pool_page, 1, 16,
        have_existing_liquidity ? existing_liquidity.operational_dgb : 1);
    operational_dgb->setObjectName("paymasterSetupOperationalDgbSlots");
    operational_dgb->setAccessibleName(tr("Immediately usable DGB slots"));
    admission_carriers = PaymasterWidgetImpl::spin(
        pool_page, 0, 16,
        have_existing_liquidity
            ? existing_liquidity.admission_carriers
            : wizard_user_paid->isChecked() ? 3 : 0);
    admission_carriers->setObjectName("paymasterSetupAdmissionCarrierSlots");
    admission_carriers->setAccessibleName(tr("Reserve DigiDollar carrier slots"));
    operational_carriers = PaymasterWidgetImpl::spin(
        pool_page, 0, 16,
        have_existing_liquidity
            ? existing_liquidity.operational_carriers
            : wizard_user_paid->isChecked() ? 1 : 0);
    operational_carriers->setObjectName("paymasterSetupOperationalCarrierSlots");
    operational_carriers->setAccessibleName(tr("Immediately usable DigiDollar carrier slots"));
    const auto update_carrier_targets = [admission_carriers, operational_carriers](bool paid) {
        admission_carriers->setEnabled(paid);
        operational_carriers->setEnabled(paid);
        admission_carriers->setValue(paid ? std::max(3, admission_carriers->value()) : 0);
        operational_carriers->setValue(paid ? std::max(1, operational_carriers->value()) : 0);
    };
    connect(wizard_user_paid, &QCheckBox::toggled, pool_page, update_carrier_targets);
    update_carrier_targets(wizard_user_paid->isChecked());
    auto* reserve_editor = new PaymasterReserveEditor(pool_content, QStringLiteral("paymasterSetup"),
        {admission_dgb, operational_dgb, admission_carriers, operational_carriers},
        {custom_maintenance_per_transaction, custom_maintenance_per_hour, custom_maintenance_per_day},
        [wizard_user_paid] { return wizard_user_paid->isChecked(); }, selected_network_fee);
    pool_layout->addRow(reserve_editor);
    connect(wizard_user_paid, &QCheckBox::toggled, reserve_editor, [reserve_editor] { reserve_editor->synchronize(true); });
    connect(network_fee, qOverload<int>(&QSpinBox::valueChanged), reserve_editor, [reserve_editor] { reserve_editor->synchronize(); });
    pool_layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    pool_layout->addRow(automatic_replenishment);
    pool_layout->addRow(paid_maintenance_approved);
    auto* refill_limits = new QWidget(pool_content);
    auto* refill_form = new QFormLayout(refill_limits);
    refill_form->setContentsMargins(0, 0, 0, 0);
    refill_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    refill_form->addRow(tr("Maximum refill fee per transaction (DGB):"), custom_maintenance_per_transaction);
    refill_form->addRow(tr("Maximum refill fees per rolling hour (DGB):"), custom_maintenance_per_hour);
    refill_form->addRow(tr("Maximum refill fees per rolling day (DGB):"), custom_maintenance_per_day);
    pool_layout->addRow(refill_limits);
    pool_layout->addRow(tr("Provider operation:"), operation_mode);
    pool_layout->addRow(autostart);
    pool_layout->addRow(provider_enabled);
    auto* restore_liquidity_defaults = new QPushButton(tr("Restore defaults"), pool_page);
    restore_liquidity_defaults->setObjectName("paymasterSetupRestoreLiquidityDefaults");
    restore_liquidity_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    pool_layout->addRow(restore_liquidity_defaults);
    connect(restore_liquidity_defaults, &QPushButton::clicked, pool_page,
            [reserve_editor,
             automatic_replenishment, paid_maintenance_approved,
             operation_mode, autostart, provider_enabled, have_existing_provider_configuration, this] {
                reserve_editor->applyPreset(1);
                automatic_replenishment->setChecked(true);
                paid_maintenance_approved->setChecked(true);
                if (!m_panel.overview().running()) {
                    operation_mode->setCurrentIndex(
                        operation_mode->findData(QStringLiteral("automatic")));
                }
                autostart->setChecked(have_existing_provider_configuration ? m_panel.configuration().autostartEnabled() : true);
                provider_enabled->setChecked(true);
            });
    auto* liquidity_help = new QFrame(pool_page);
    liquidity_help->setObjectName("paymasterSetupLiquidityFieldHelp");
    liquidity_help->setFrameShape(QFrame::StyledPanel);
    auto* liquidity_help_layout = new QVBoxLayout(liquidity_help);
    liquidity_help_layout->setContentsMargins(12, 9, 12, 9);
    liquidity_help_layout->setSpacing(3);
    auto* liquidity_help_title = new QLabel(liquidity_help);
    liquidity_help_title->setObjectName("paymasterSetupLiquidityFieldHelpTitle");
    auto* liquidity_help_text = new QLabel(liquidity_help);
    liquidity_help_text->setObjectName("paymasterSetupLiquidityFieldHelpText");
    liquidity_help_text->setWordWrap(true);
    liquidity_help_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    liquidity_help_layout->addWidget(liquidity_help_title);
    liquidity_help_layout->addWidget(liquidity_help_text);
    reserve_editor->addManualHelp(liquidity_help);

    const auto update_liquidity_help = [this, wizard_user_paid, admission_dgb,
                                        operational_dgb, admission_carriers,
                                        operational_carriers, liquidity_help_title,
                                        liquidity_help_text](QWidget* field) {
        if (field == admission_dgb) {
            liquidity_help_title->setText(tr("Reserve DGB slots for network admission"));
            liquidity_help_text->setText(tr(
                "%1 separate confirmed DGB outputs will be targeted as the provider's admission reserve. "
                "They prove that the provider has minimum capacity before a client reveals payment details. "
                "At least three are required, and they are separate from immediately spendable payment slots.")
                .arg(admission_dgb->value()));
        } else if (field == operational_dgb) {
            liquidity_help_title->setText(tr("Immediately usable DGB slots"));
            liquidity_help_text->setText(tr(
                "%1 DGB slot(s) will be targeted for active payments. Each slot can be reserved for one transfer at a time and supplies its DigiByte network fee. "
                "A higher value permits more parallel work but keeps more wallet funds prepared for Paymaster use.")
                .arg(operational_dgb->value()));
        } else if (field == admission_carriers) {
            liquidity_help_title->setText(tr("Reserve DigiDollar carrier slots"));
            liquidity_help_text->setText(wizard_user_paid->isChecked()
                ? tr("%1 confirmed DigiDollar carrier output(s) will be targeted for user-paid admission. At least three are required when the provider accepts service fees in $DD.")
                      .arg(admission_carriers->value())
                : admission_carriers->value() == 0
                    ? tr("Carrier outputs are not needed for sponsored-only service, so a fresh setup leaves this target at zero.")
                    : tr("Sponsored-only pool preparation requires zero DD targets. Existing DD outputs are retained; this setup does not withdraw them."));
        } else if (field == operational_carriers) {
            liquidity_help_title->setText(tr("Immediately usable DigiDollar carrier slots"));
            liquidity_help_text->setText(wizard_user_paid->isChecked()
                ? tr("%1 carrier slot(s) will be targeted for active user-paid transfers. A complete user-paid operational slot needs both one DGB slot and one carrier slot; the lower count therefore limits parallel user-paid transfers.")
                      .arg(operational_carriers->value())
                : operational_carriers->value() == 0
                    ? tr("Sponsored transfers charge no $DD service fee and therefore need no operational carrier slots.")
                    : tr("Sponsored-only pool preparation requires zero DD targets. Existing DD outputs remain wallet-owned."));
        }
    };
    connect(qApp, &QApplication::focusChanged, pool_page,
            [pool_page, admission_dgb, operational_dgb, admission_carriers,
             operational_carriers, update_liquidity_help](QWidget*, QWidget* focused) {
                QWidget* candidate = focused;
                while (candidate && candidate != pool_page) {
                    if (candidate == admission_dgb || candidate == operational_dgb ||
                        candidate == admission_carriers || candidate == operational_carriers) {
                        update_liquidity_help(candidate);
                        return;
                    }
                    candidate = candidate->parentWidget();
                }
            });
    for (QSpinBox* control : {admission_dgb, operational_dgb,
                              admission_carriers, operational_carriers}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged), pool_page,
                [control, update_liquidity_help] {
                    if (control->hasFocus()) update_liquidity_help(control);
                });
    }
    update_liquidity_help(admission_dgb);
    wizard.addPage(pool_page);

    return pool;
}

ProviderSetup::SetupReviewControls ProviderSetup::buildSetupReviewPage(PaymasterSetupWizard& wizard, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool)
{
    SetupReviewControls review_controls;
    auto& review_page = review_controls.review_page;
    auto& review_layout = review_controls.review_layout;
    auto& review = review_controls.review;
    auto& preview_status = review_controls.preview_status;
    auto& preview_details = review_controls.preview_details;
    auto& preview_retry = review_controls.preview_retry;
    auto& maintenance_confirmation = review_controls.maintenance_confirmation;
    auto& review_id = review_controls.review_id;
    const auto display = offer.display;
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto wizard_sponsored = offer.wizard_sponsored;
    const auto wizard_scope = offer.wizard_scope;
    const auto fee = offer.fee;
    const auto minimum = offer.minimum;
    const auto maximum = offer.maximum;
    const auto lifetime = offer.lifetime;
    const auto network_fee = offer.network_fee;
    const auto safety_profile = safety.safety_profile;
    const auto custom_per_transaction = safety.custom_per_transaction;
    const auto custom_reserved = safety.custom_reserved;
    const auto custom_per_hour = safety.custom_per_hour;
    const auto custom_per_day = safety.custom_per_day;
    const auto custom_completed_per_hour = safety.custom_completed_per_hour;
    const auto custom_completed_per_day = safety.custom_completed_per_day;
    const auto custom_max_active_quotes_total = safety.custom_max_active_quotes_total;
    const auto custom_max_active_quotes_per_netgroup = safety.custom_max_active_quotes_per_netgroup;
    const auto custom_max_active_quotes_per_recipient = safety.custom_max_active_quotes_per_recipient;
    const auto custom_max_quote_requests_per_netgroup = safety.custom_max_quote_requests_per_netgroup;
    const auto custom_maintenance_per_transaction = safety.custom_maintenance_per_transaction;
    const auto custom_maintenance_per_hour = safety.custom_maintenance_per_hour;
    const auto custom_maintenance_per_day = safety.custom_maintenance_per_day;
    const auto automatic_replenishment = pool.automatic_replenishment;
    const auto paid_maintenance_approved = pool.paid_maintenance_approved;
    const auto operation_mode = pool.operation_mode;
    const auto autostart = pool.autostart;
    const auto provider_enabled = pool.provider_enabled;
    const auto admission_dgb = pool.admission_dgb;
    const auto operational_dgb = pool.operational_dgb;
    const auto admission_carriers = pool.admission_carriers;
    const auto operational_carriers = pool.operational_carriers;
    review_page = new ValidatedWizardPage(&wizard);
    review_page->setObjectName("paymasterSetupReviewPage");
    review_page->setTitle(tr("Review and approve"));
    review_page->setSubTitle(tr("Review the complete configuration and current liquidity estimate before applying it."));
    auto* review_page_layout = new QVBoxLayout(review_page);
    auto* review_scroll = new QScrollArea(review_page);
    review_scroll->setObjectName("paymasterSetupReviewScroll");
    review_scroll->setWidgetResizable(true);
    review_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* review_content = new QWidget(review_scroll);
    review_content->setObjectName("paymasterSetupReviewContent");
    review_layout = new QVBoxLayout(review_content);
    review_layout->setSizeConstraint(QLayout::SetMinimumSize);
    review_scroll->setWidget(review_content);
    ConfigureScrollArea(review_scroll, review_content);
    review_page_layout->addWidget(review_scroll);
    review = new QLabel(review_page);
    review->setObjectName("paymasterSetupReview");
    review->setWordWrap(true);
    review->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                    Qt::TextSelectableByMouse);
    auto* next_actions = new QLabel(tr(
                                        "When you apply this plan, the assistant first places an existing enabled provider safely offline, then creates or reuses the identity, saves the selected operating, safety and automatic-liquidity policies, and asks Core to recheck and create only missing pool outputs. Processing mode, the selected autostart choice and the requested enabled state are saved last. You can also include a one-time start in this review."),
                                    review_page);
    next_actions->setObjectName("paymasterSetupNextActions");
    next_actions->setWordWrap(true);
    preview_status = new QLabel(tr("Liquidity requirement calculated from the current wallet state."), review_page);
    preview_status->setObjectName("paymasterSetupLiquidityPreviewStatus");
    preview_status->setWordWrap(true);
    preview_details = new QLabel(review_page);
    preview_details->setObjectName("paymasterSetupLiquidityPreviewDetails");
    preview_details->setWordWrap(true);
    preview_details->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                             Qt::TextSelectableByMouse);
    preview_retry = new QPushButton(tr("Recalculate liquidity requirement"), review_page);
    preview_retry->setObjectName("paymasterSetupLiquidityPreviewRetry");
    preview_retry->setVisible(false);
    maintenance_confirmation = new QCheckBox(tr(
        "I approve automatic paid liquidity maintenance only within the finite limits shown in this review."),
        review_page);
    maintenance_confirmation->setObjectName(
        "paymasterSetupMaintenanceApproval");
    const auto invalidate_maintenance_confirmation =
        [maintenance_confirmation] {
            maintenance_confirmation->setChecked(false);
        };
    for (QSpinBox* control : {minimum, maximum, lifetime, network_fee,
                              custom_completed_per_hour,
                              custom_completed_per_day,
                              custom_max_active_quotes_total,
                              custom_max_active_quotes_per_netgroup,
                              custom_max_active_quotes_per_recipient,
                              custom_max_quote_requests_per_netgroup,
                              admission_dgb, operational_dgb,
                              admission_carriers, operational_carriers}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged),
                review_page,
                [invalidate_maintenance_confirmation](int) {
                    invalidate_maintenance_confirmation();
                });
    }
    connect(fee, qOverload<double>(&QDoubleSpinBox::valueChanged),
            review_page,
            [invalidate_maintenance_confirmation](double) {
                invalidate_maintenance_confirmation();
            });
    const QList<QLineEdit*> review_line_edits{
        display, custom_per_transaction, custom_reserved, custom_per_hour,
        custom_per_day,
        custom_maintenance_per_transaction, custom_maintenance_per_hour,
        custom_maintenance_per_day};
    for (QLineEdit* control : review_line_edits) {
        connect(control, &QLineEdit::textChanged, review_page,
                [invalidate_maintenance_confirmation] {
                    invalidate_maintenance_confirmation();
                });
    }
    for (QCheckBox* control : {wizard_user_paid, wizard_sponsored,
                               automatic_replenishment,
                               paid_maintenance_approved, autostart,
                               provider_enabled}) {
        connect(control, &QCheckBox::toggled, review_page,
                [invalidate_maintenance_confirmation](bool) {
                    invalidate_maintenance_confirmation();
                });
    }
    for (QComboBox* control : {wizard_scope, safety_profile,
                               operation_mode}) {
        connect(control, qOverload<int>(&QComboBox::currentIndexChanged),
                review_page,
                [invalidate_maintenance_confirmation](int) {
                    invalidate_maintenance_confirmation();
                });
    }
    review_layout->addWidget(review);
    review_layout->addWidget(next_actions);
    review_layout->addWidget(preview_status);
    review_layout->addWidget(preview_details);
    review_layout->addWidget(maintenance_confirmation);
    review_layout->addWidget(preview_retry, 0, Qt::AlignLeft);
    review_layout->addStretch();
    review_id = wizard.addPage(review_page);

    return review_controls;
}

QString ProviderSetup::setupStepLabel(const std::string& method)
{
    if (method == "stoppaymaster") return tr("Pause existing provider");
    if (method == "createpaymasteridentity") return tr("Create provider identity");
    if (method == "setpaymasterpolicy") return tr("Save offer policy");
    if (method == "setpaymastersafetypolicy") return tr("Save finite safety limits");
    if (method == "setpaymasterliquiditypolicy") return tr("Save liquidity targets and refill approval");
    if (method == "preparepaymasterpool") return tr("Review and approve exact pool funding");
    if (method == "setpaymasterruntimesettings") return tr("Save processing mode and selected autostart choice");
    if (method == "setpaymasterenabled") return tr("Save provider enablement");
    return tr("Unknown setup step");
}

ProviderSetup::SetupProgressControls ProviderSetup::buildSetupProgressPage(PaymasterSetupWizard& wizard)
{
    SetupProgressControls progress;
    auto& progress_page = progress.progress_page;
    auto& progress_names = progress.progress_names;
    auto& progress_steps = progress.progress_steps;
    auto& progress_result = progress.progress_result;
    auto& setup_retry = progress.setup_retry;
    auto& setup_backup = progress.setup_backup;
    auto& setup_backup_id = progress.setup_backup_id;
    auto& setup_funding = progress.setup_funding;
    auto& setup_refresh = progress.setup_refresh;
    auto& progress_id = progress.progress_id;
    progress_page = new CompletableWizardPage(&wizard);
    progress_page->setObjectName("paymasterSetupProgressPage");
    progress_page->setTitle(tr("Setup and result"));
    progress_page->setSubTitle(tr("The assistant saves each confirmed setting in order. Provider start and autostart remain separate decisions."));
    auto* progress_page_layout = new QVBoxLayout(progress_page);
    auto* progress_scroll = new QScrollArea(progress_page);
    progress_scroll->setObjectName("paymasterSetupProgressScroll");
    progress_scroll->setWidgetResizable(true);
    progress_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* progress_content = new QWidget(progress_scroll);
    progress_content->setObjectName("paymasterSetupProgressContent");
    auto* progress_layout = new QVBoxLayout(progress_content);
    progress_layout->setSizeConstraint(QLayout::SetMinimumSize);
    progress_scroll->setWidget(progress_content);
    ConfigureScrollArea(progress_scroll, progress_content);
    progress_page_layout->addWidget(progress_scroll);
    auto* progress_intro = new QLabel(tr(
        "Do not close DigiByte Core while a step is running. Completed steps remain safely persisted if a later step needs to be retried."), progress_page);
    progress_intro->setWordWrap(true);
    progress_layout->addWidget(progress_intro);
    progress_names = QStringList{
        tr("Verify selected provider wallet"),
        tr("Place an existing enabled provider safely offline"),
        tr("Unlock wallet when required"),
        tr("Create or reuse provider identity"),
        tr("Prepare a compatible safety transition when required"),
        tr("Save or retain operating policy"),
        tr("Save provider safety policy"),
        tr("Save automatic liquidity policy"),
        tr("Recheck and create missing pool liquidity"),
        tr("Save provider runtime settings"),
        tr("Save provider enabled state"),
    };
    for (int index = 0; index < progress_names.size(); ++index) {
        const QString& name = progress_names.at(index);
        auto* label = new QLabel(tr("○ Pending — %1").arg(name), progress_page);
        label->setObjectName(QStringLiteral("paymasterSetupProgressStep%1").arg(index));
        label->setWordWrap(true);
        progress_layout->addWidget(label);
        progress_steps.push_back(label);
    }
    progress_result = new QLabel(
        tr("The setup has not started yet."), progress_page);
    progress_result->setObjectName("paymasterSetupProgressResult");
    progress_result->setWordWrap(true);
    progress_result->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                             Qt::TextSelectableByMouse);
    setup_retry = new QPushButton(tr("Retry failed step"), progress_page);
    setup_retry->setObjectName("paymasterSetupRetry");
    setup_retry->setVisible(false);
    setup_backup = new QGroupBox(
        tr("Protect the new provider identity"), progress_page);
    setup_backup->setObjectName("paymasterSetupBackupReminder");
    setup_backup->setProperty("paymasterRole", QStringLiteral("notice"));
    setup_backup->setProperty("statusKind", QStringLiteral("waiting"));
    auto* setup_backup_layout = new QVBoxLayout(setup_backup);
    auto* setup_backup_text = new QLabel(tr(
        "The provider identity key, Paymaster settings, pool state and finance ledger are stored in this wallet. "
        "Create a complete wallet backup and keep it offline and confidential. A seed or descriptor export alone is not a complete Paymaster backup. "
        "The reminder is important but does not block provider start.") + QLatin1Char('\n') + tr(
        "Back up again after reserve preparation has completed. Payment and reserve changes can make an earlier backup too old for continued provider operation.") + QLatin1Char('\n') + tr(
        "For continued operation after restoration, Core also requires independently retained current state from the node's paymaster-checkpoints folder. Never restore an old wallet and its old checkpoint together to resume service. Use this provider identity on only one host. Before migration, pause the provider, resolve unfinished work, then create a current backup."), setup_backup);
    setup_backup_text->setObjectName("paymasterSetupBackupHelp");
    setup_backup_text->setWordWrap(true);
    setup_backup_id = new QLabel(setup_backup);
    setup_backup_id->setObjectName("paymasterSetupBackupProviderId");
    setup_backup_id->setWordWrap(true);
    setup_backup_id->setTextInteractionFlags(
        Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    auto* setup_backup_actions = new QHBoxLayout();
    auto* setup_backup_now = new QPushButton(
        tr("Back up provider wallet now…"), setup_backup);
    setup_backup_now->setObjectName("paymasterSetupBackupNow");
    setup_backup_now->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    auto* setup_backup_external = new QPushButton(
        tr("I use another full-wallet backup method…"), setup_backup);
    setup_backup_external->setObjectName("paymasterSetupExternalBackup");
    setup_backup_actions->addWidget(setup_backup_now);
    setup_backup_actions->addWidget(setup_backup_external);
    setup_backup_actions->addStretch();
    setup_backup_layout->addWidget(setup_backup_text);
    setup_backup_layout->addWidget(setup_backup_id);
    setup_backup_layout->addLayout(setup_backup_actions);
    setup_backup->hide();
    connect(setup_backup_now, &QPushButton::clicked, progress_page,
            [this] { m_panel.recovery().requestProviderBackup(); });
    connect(setup_backup_external, &QPushButton::clicked, progress_page,
            [this] { m_panel.recovery().acknowledgeExternalBackup(); });
    progress_layout->addSpacing(10);
    progress_layout->addWidget(progress_result);
    setup_funding = new QLabel(progress_page);
    setup_funding->setObjectName("paymasterSetupFundingStatus");
    setup_funding->setWordWrap(true);
    setup_funding->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    setup_refresh = new QPushButton(tr("Check funding and readiness"), progress_page);
    setup_refresh->setObjectName("paymasterSetupRefreshStatus");
    setup_refresh->hide();
    progress_layout->addWidget(setup_funding);
    progress_layout->addWidget(setup_refresh, 0, Qt::AlignLeft);
    progress_layout->addWidget(setup_backup);
    progress_layout->addWidget(setup_retry, 0, Qt::AlignLeft);
    progress_layout->addStretch();
    progress_id = wizard.addPage(progress_page);

    return progress;
}

void ProviderSetup::addSetupStartChoice(SetupReviewControls& review_controls, const SetupPoolControls& pool)
{
    auto& review_page = review_controls.review_page;
    auto& review_layout = review_controls.review_layout;
    auto& start_after_setup = review_controls.start_after_setup;
    const auto have_existing_provider_configuration = pool.have_existing_provider_configuration;
    const auto provider_enabled = pool.provider_enabled;
    start_after_setup = new QCheckBox(tr("Start provider when this setup is ready"), review_page);
    start_after_setup->setObjectName("paymasterSetupStartWhenReady");
    start_after_setup->setChecked(!have_existing_provider_configuration || m_panel.overview().running());
    start_after_setup->setEnabled(provider_enabled->isChecked());
    if (!provider_enabled->isChecked()) start_after_setup->setChecked(false);
    connect(provider_enabled, &QCheckBox::toggled, start_after_setup, [start_after_setup](bool enabled) {
        start_after_setup->setEnabled(enabled);
        if (!enabled) start_after_setup->setChecked(false);
    });
    review_layout->addWidget(start_after_setup);
}

UniValue ProviderSetup::readSetupPolicy(const SetupOfferControls& offer)
{
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto wizard_sponsored = offer.wizard_sponsored;
    const auto wizard_scope = offer.wizard_scope;
    const auto fee = offer.fee;
    const auto maximum_user_paid_service_fee = offer.maximum_user_paid_service_fee;
    const auto minimum = offer.minimum;
    const auto maximum = offer.maximum;
    const auto lifetime = offer.lifetime;
    const auto network_fee = offer.network_fee;
    const auto replace_unrepresentable_policy = offer.replace_unrepresentable_policy;
    if (!*replace_unrepresentable_policy) {
        UniValue retained = DigiDollar::Paymaster::SetupDefaultPolicy();
        for (const auto& key : retained.getKeys()) retained.pushKV(key, m_panel.configuration().preservedPolicy().find_value(key));
        return retained;
    }
    UniValue funding{UniValue::VARR}, policy{UniValue::VOBJ};
    if (wizard_sponsored->isChecked()) funding.push_back("sponsored");
    if (wizard_user_paid->isChecked()) funding.push_back("user_paid");
    policy.pushKV("funding_models", funding);
    policy.pushKV("sponsorship_scope", wizard_scope->currentData().toString().toStdString());
    policy.pushKV("fee_rate_bps", wizard_user_paid->isChecked() ? qRound(fee->value() * 100.0) : 0);
    policy.pushKV("maximum_user_paid_service_fee_cents",
                  wizard_user_paid->isChecked()
                      ? maximum_user_paid_service_fee->value()
                      : 0);
    policy.pushKV("min_amount_cents", minimum->value());
    policy.pushKV("max_amount_cents", maximum->value());
    policy.pushKV("quote_ttl", lifetime->value());
    policy.pushKV("maximum_network_fee_dgb_satoshis", network_fee->value());
    return policy;
}

UniValue ProviderSetup::readSetupPoolOptions(const SetupPoolControls& pool, bool execute)
{
    const auto admission_dgb = pool.admission_dgb;
    const auto operational_dgb = pool.operational_dgb;
    const auto admission_carriers = pool.admission_carriers;
    const auto operational_carriers = pool.operational_carriers;
    UniValue options{UniValue::VOBJ};
    options.pushKV("admission_dgb_slots", admission_dgb->value());
    options.pushKV("operational_dgb_slots", operational_dgb->value());
    options.pushKV("admission_carrier_slots", admission_carriers->value());
    options.pushKV("operational_carrier_slots", operational_carriers->value());
    options.pushKV("execute", execute);
    qint64 fee{0};
    if (!m_panel.liquidity().preparationFee(fee) || fee <= 0 || fee > MAX_MONEY / 2) throw std::runtime_error("PAYMASTER_POOL_INVALID_FEE_LIMIT");
    options.pushKV("maximum_fee_satoshis", fee);
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    return params;
}

DigiDollar::Paymaster::SetupChoices ProviderSetup::readSetupChoices(const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool)
{
    const auto display = offer.display;
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto wizard_sponsored = offer.wizard_sponsored;
    const auto wizard_scope = offer.wizard_scope;
    const auto custom_max_active_quotes_total = safety.custom_max_active_quotes_total;
    const auto custom_max_active_quotes_per_netgroup = safety.custom_max_active_quotes_per_netgroup;
    const auto custom_max_active_quotes_per_recipient = safety.custom_max_active_quotes_per_recipient;
    const auto custom_max_quote_requests_per_netgroup = safety.custom_max_quote_requests_per_netgroup;
    const auto custom_maintenance_per_transaction = safety.custom_maintenance_per_transaction;
    const auto custom_maintenance_per_hour = safety.custom_maintenance_per_hour;
    const auto custom_maintenance_per_day = safety.custom_maintenance_per_day;
    const auto safety_profile_edited = safety.safety_profile_edited;
    const auto current_safety_limits = safety.current_safety_limits;
    const auto automatic_replenishment = pool.automatic_replenishment;
    const auto paid_maintenance_approved = pool.paid_maintenance_approved;
    const auto operation_mode = pool.operation_mode;
    const auto autostart = pool.autostart;
    const auto provider_enabled = pool.provider_enabled;
    const auto admission_dgb = pool.admission_dgb;
    const auto operational_dgb = pool.operational_dgb;
    const auto admission_carriers = pool.admission_carriers;
    const auto operational_carriers = pool.operational_carriers;
    using namespace DigiDollar::Paymaster;
    SetupChoices choices;
    choices.display_name = display->text().trimmed().toStdString();
    choices.policy = readSetupPolicy(offer);
    choices.operation_mode = operation_mode->currentData().toString().toStdString();
    choices.enabled = provider_enabled->isChecked();
    choices.autostart = autostart->isChecked();
    choices.pool = readSetupPoolOptions(pool, false)[0];
    const auto guided = current_safety_limits();
    const auto cap = choices.policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>();
    const bool paid = wizard_user_paid->isChecked();
    const bool sponsored = wizard_sponsored->isChecked();
    const bool restricted = wizard_scope->currentData().toString() == QLatin1String("restricted");
    const auto add_limits = [&](const char* name, const std::optional<FundingSafetyValues>& saved, bool selected) {
        if (!saved) throw std::runtime_error("PAYMASTER_SETUP_SAFETY_UNAVAILABLE");
        FundingSafetyValues values = *saved;
        if (selected && (!m_panel.configuration().providerSafetyConfigured() || *safety_profile_edited || values.allZero())) {
            values.per_transaction = guided.per_transaction;
            values.reserved = guided.reserved;
            values.per_hour = guided.per_hour;
            values.per_day = guided.per_day;
            values.completed_per_hour = guided.completed_per_hour;
            values.completed_per_day = guided.completed_per_day;
        }
        // Core validates inactive classes too. Lower only a stale
        // per-transfer ceiling; expose the resulting values in review.
        if (!selected) values.per_transaction = std::min<qint64>(values.per_transaction, cap);
        choices.safety.pushKV(name, ProviderConfiguration::fundingSafetyToJSON(values));
    };
    add_limits("user_paid", m_panel.configuration().fundingSafety(DigiDollar::Paymaster::FundingModel::USER_PAID), paid);
    add_limits("public_sponsored", m_panel.configuration().fundingSafety(DigiDollar::Paymaster::FundingModel::SPONSORED), sponsored && !restricted);
    add_limits("restricted_sponsored", m_panel.configuration().fundingSafety(DigiDollar::Paymaster::FundingModel::SPONSORED, DigiDollar::Paymaster::SponsorshipScope::RESTRICTED), sponsored && restricted);
    choices.safety.pushKV("maximum_active_quotes_total", custom_max_active_quotes_total->value());
    choices.safety.pushKV("maximum_active_quotes_per_netgroup", custom_max_active_quotes_per_netgroup->value());
    choices.safety.pushKV("maximum_active_quotes_per_recipient", custom_max_active_quotes_per_recipient->value());
    choices.safety.pushKV("maximum_quote_requests_per_netgroup_per_minute", custom_max_quote_requests_per_netgroup->value());
    LiquidityPolicyValues liquidity;
    liquidity.automatic_replenishment = automatic_replenishment->isChecked();
    liquidity.paid_maintenance_approved = paid_maintenance_approved->isChecked();
    liquidity.admission_dgb = admission_dgb->value();
    liquidity.operational_dgb = operational_dgb->value();
    liquidity.admission_carriers = admission_carriers->value();
    liquidity.operational_carriers = operational_carriers->value();
    if (!custom_maintenance_per_transaction->satoshis(liquidity.fee_per_transaction) ||
         !custom_maintenance_per_hour->satoshis(liquidity.fee_per_hour) ||
         !custom_maintenance_per_day->satoshis(liquidity.fee_per_day))
        throw std::runtime_error("PAYMASTER_INVALID_LIQUIDITY_POLICY");
    choices.liquidity = ProviderLiquidity::liquidityPolicyToJSON(liquidity);
    CheckSetupChoices(choices);
    return choices;
}

QString ProviderSetup::formatSetupPoolPreview(const UniValue& result)
{
    QString text = m_panel.liquidity().formatPoolResult("preparepaymasterpool", result);
    text.replace(
        tr("Review these values. If they are acceptable, use the matching Execute reviewed action without changing the targets."),
        tr("These targets form the current estimate. Core will recheck the exact missing outputs before any funding transaction."));
    return text;
}

void ProviderSetup::renderSetupReview(const SetupWizardContext& context, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool, const SetupReviewControls& review_controls)
{
    const auto setup_wallet_name = context.setup_wallet_name;
    const auto display = offer.display;
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto wizard_sponsored = offer.wizard_sponsored;
    const auto wizard_scope = offer.wizard_scope;
    const auto fee = offer.fee;
    const auto minimum = offer.minimum;
    const auto maximum = offer.maximum;
    const auto lifetime = offer.lifetime;
    const auto selected_network_fee = offer.selected_network_fee;
    const auto replace_unrepresentable_policy = offer.replace_unrepresentable_policy;
    const auto safety_profile = safety.safety_profile;
    const auto custom_max_active_quotes_total = safety.custom_max_active_quotes_total;
    const auto custom_max_active_quotes_per_netgroup = safety.custom_max_active_quotes_per_netgroup;
    const auto custom_max_active_quotes_per_recipient = safety.custom_max_active_quotes_per_recipient;
    const auto custom_max_quote_requests_per_netgroup = safety.custom_max_quote_requests_per_netgroup;
    const auto custom_maintenance_per_transaction = safety.custom_maintenance_per_transaction;
    const auto custom_maintenance_per_hour = safety.custom_maintenance_per_hour;
    const auto custom_maintenance_per_day = safety.custom_maintenance_per_day;
    const auto automatic_replenishment = pool.automatic_replenishment;
    const auto paid_maintenance_approved = pool.paid_maintenance_approved;
    const auto operation_mode = pool.operation_mode;
    const auto autostart = pool.autostart;
    const auto provider_enabled = pool.provider_enabled;
    const auto admission_dgb = pool.admission_dgb;
    const auto operational_dgb = pool.operational_dgb;
    const auto admission_carriers = pool.admission_carriers;
    const auto operational_carriers = pool.operational_carriers;
    const auto review = review_controls.review;
    const auto maintenance_confirmation = review_controls.maintenance_confirmation;
    QString model;
    if (wizard_user_paid->isChecked() && wizard_sponsored->isChecked()) {
        model = tr("User paid and public sponsored");
    } else if (wizard_user_paid->isChecked()) {
        model = tr("User paid");
    } else if (wizard_scope->currentData().toString() == QLatin1String("restricted")) {
        model = tr("Restricted sponsored");
    } else {
        model = tr("Public sponsored");
    }
    QString review_text = tr(
        "Provider wallet: %1\n"
        "Provider name: %2\n"
        "Service model: %3\n"
        "Payment range: %4 to %5 DD\n"
        "User-paid service fee: %6%\n"
        "Quote validity: %7 seconds\n"
        "Network-fee ceiling: %8 DGB per transfer\n"
        "Safety profile: %9\n"
        "Provider operation: %10; autostart %11; configuration %12")
        .arg(setup_wallet_name)
        .arg(display->text().trimmed().isEmpty() ? tr("No public name") : display->text().trimmed())
        .arg(model)
        .arg(PaymasterFormatDD(minimum->value()))
        .arg(PaymasterFormatDD(maximum->value()))
        .arg(QString::number(wizard_user_paid->isChecked() ? fee->value() : 0.0, 'f', 2))
        .arg(lifetime->value())
        .arg(PaymasterFormatDGB(selected_network_fee()))
        .arg(safety_profile->currentText())
        .arg(operation_mode->currentText(),
             autostart->isChecked() ? tr("on") : tr("off"),
             provider_enabled->isChecked() ? tr("enabled")
                                           : tr("disabled"));
    if (!*replace_unrepresentable_policy) {
        review_text += tr(
            "\nOperating policy: the exact persisted Core value remains unchanged because it is outside this interface's numeric range. Exact retained record: %1")
            .arg(QString::fromStdString(
                m_panel.configuration().preservedPolicy().write()));
    }
    review_text += tr(
        "\nLiquidity targets: admission DGB %1, operational DGB %2, admission carriers %3, operational carriers %4")
        .arg(admission_dgb->value())
        .arg(operational_dgb->value())
        .arg(admission_carriers->value())
        .arg(operational_carriers->value());
    review_text += tr(
        "\nRequest limits: %1 active quotes total, %2 per netgroup, %3 per recipient bucket and %4 quote requests per netgroup/minute")
        .arg(custom_max_active_quotes_total->value())
        .arg(custom_max_active_quotes_per_netgroup->value())
        .arg(custom_max_active_quotes_per_recipient->value())
        .arg(custom_max_quote_requests_per_netgroup->value());
    qint64 maintenance_per_transaction{0};
    qint64 maintenance_per_hour{0};
    qint64 maintenance_per_day{0};
    custom_maintenance_per_transaction->satoshis(maintenance_per_transaction);
    custom_maintenance_per_hour->satoshis(maintenance_per_hour);
    custom_maintenance_per_day->satoshis(maintenance_per_day);
    review_text += tr(
        "\nAutomatic replenishment: %1; paid maintenance: %2, limited to %3 DGB per maintenance transaction, %4 DGB per rolling hour and %5 DGB per rolling day")
        .arg(automatic_replenishment->isChecked() ? tr("enabled") : tr("disabled"),
             paid_maintenance_approved->isChecked() ? tr("approved") : tr("disabled"),
             PaymasterWidgetImpl::dgbAmount(maintenance_per_transaction),
             PaymasterWidgetImpl::dgbAmount(maintenance_per_hour),
             PaymasterWidgetImpl::dgbAmount(maintenance_per_day));
    maintenance_confirmation->setText(
        paid_maintenance_approved->isChecked() ? tr("I approve automatic paid liquidity maintenance for wallet \"%1\" up to %2 DGB per transaction, %3 DGB per rolling hour and %4 DGB per rolling day. This approval alone does not start the provider. Autostart is a separate choice shown in this review.")
                                                     .arg(setup_wallet_name,
                                                          PaymasterWidgetImpl::dgbAmount(maintenance_per_transaction),
                                                          PaymasterWidgetImpl::dgbAmount(maintenance_per_hour),
                                                          PaymasterWidgetImpl::dgbAmount(maintenance_per_day)) :
                                                 tr("I understand that paid automatic liquidity maintenance remains disabled for wallet \"%1\". The finite limits are retained but cannot authorize a paid refill until I explicitly enable and save that approval.")
                                                     .arg(setup_wallet_name));
    if (m_panel.overview().hasIdentity()) {
        review_text += tr(
            "\nExisting provider identity: retained unchanged; its saved display name remains authoritative.");
    } else {
        review_text += tr("\nProvider identity: a new persistent BIP86 identity will be created.");
    }
    try {
        const auto choices = readSetupChoices(offer, safety, pool);
        const auto conflicts = m_panel.configuration().configurationConflictText(choices.policy, choices.safety);
        if (!conflicts.isEmpty()) review_text.prepend(tr("Configuration conflict:\n%1\n\n").arg(conflicts));
        review_text += tr("\nSaved inactive budgets remain disabled; their per-transfer ceiling is reduced if needed to fit the reviewed offer.");
        for (const auto& item : {std::pair{"user_paid", tr("User paid")}, {"public_sponsored", tr("Public sponsored")}, {"restricted_sponsored", tr("Inactive legacy sponsorship")}}) {
            const auto& limits = choices.safety.find_value(item.first);
            if (std::string{item.first} == "restricted_sponsored" &&
                std::all_of(limits.getValues().begin(), limits.getValues().end(), [](const UniValue& value) { return value.isNum() && value.getInt<int64_t>() == 0; })) continue;
            review_text += tr("\n%1 limits: %2 DGB per transfer; %3 DGB reserved; %4 DGB/hour; %5 DGB/day; %6 transfers/hour and %7/day")
                .arg(item.second,
                     PaymasterWidgetImpl::dgbAmount(limits.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>()),
                     PaymasterWidgetImpl::dgbAmount(limits.find_value("maximum_reserved_network_fee_satoshis").getInt<int64_t>()),
                     PaymasterWidgetImpl::dgbAmount(limits.find_value("maximum_network_fee_per_hour_satoshis").getInt<int64_t>()),
                     PaymasterWidgetImpl::dgbAmount(limits.find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>()))
                .arg(limits.find_value("maximum_completed_per_hour").getInt<int64_t>())
                .arg(limits.find_value("maximum_completed_per_day").getInt<int64_t>());
        }
    } catch (const std::exception& error) {
        review_text += tr("\nCorrect these settings before continuing: %1").arg(QString::fromUtf8(error.what()));
    }
    review->setText(review_text);
}

QString ProviderSetup::setupReviewError(const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupReviewControls& review_controls)
{
    const auto display = offer.display;
    const auto wizard_user_paid = offer.wizard_user_paid;
    const auto wizard_sponsored = offer.wizard_sponsored;
    const auto replace_unrepresentable_policy = offer.replace_unrepresentable_policy;
    const auto safety_profile = safety.safety_profile;
    const auto custom_maintenance_per_transaction = safety.custom_maintenance_per_transaction;
    const auto custom_maintenance_per_hour = safety.custom_maintenance_per_hour;
    const auto custom_maintenance_per_day = safety.custom_maintenance_per_day;
    const auto current_safety_limits = safety.current_safety_limits;
    const auto maintenance_confirmation = review_controls.maintenance_confirmation;
    QString validation_error;
    const bool custom_profile = safety_profile->currentData().toString() ==
        QLatin1String("custom");
    const GuidedPaymasterSafetyLimits selected_safety = current_safety_limits();
    qint64 custom_maintenance_transaction{0};
    qint64 custom_maintenance_hour{0};
    qint64 custom_maintenance_day{0};
    const bool custom_maintenance_valid =
        custom_maintenance_per_transaction->satoshis(custom_maintenance_transaction) &&
         custom_maintenance_per_hour->satoshis(custom_maintenance_hour) &&
         custom_maintenance_per_day->satoshis(custom_maintenance_day);
    if (!m_panel.overview().hasIdentity() &&
        !DigiDollar::Paymaster::IsValidPaymasterDisplayName(
            display->text().toStdString())) {
        validation_error = tr(
            "The provider display name must contain at most 32 printable ASCII characters and cannot contain '/' or '@'.");
    } else if (*replace_unrepresentable_policy &&
               !wizard_user_paid->isChecked() &&
               !wizard_sponsored->isChecked()) {
        validation_error = tr("Select at least one service model.");
    } else if (custom_profile && selected_safety.per_transaction < 0) {
        validation_error = tr(
            "One or more custom provider-safety amounts are outside the supported whole-satoshi range.");
    } else if (!custom_maintenance_valid) {
        validation_error = tr(
            "One or more custom maintenance-fee amounts are outside the supported whole-satoshi range.");
    } else if (!maintenance_confirmation->isChecked()) {
        validation_error = tr(
            "Review and explicitly approve the finite automatic liquidity-maintenance limits before applying setup.");
    }
    return validation_error;
}

void ProviderSetup::connectSetupReview(PaymasterSetupWizard& wizard, const SetupWizardContext& context, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool, const SetupReviewControls& review_controls)
{
    const auto setup_wallet_name = context.setup_wallet_name;
    const auto setup_start_requested = context.setup_start_requested;
    const auto reviewed_pool = context.reviewed_pool;
    const auto autostart = pool.autostart;
    const auto review_page = review_controls.review_page;
    const auto preview_status = review_controls.preview_status;
    const auto preview_details = review_controls.preview_details;
    const auto preview_retry = review_controls.preview_retry;
    const auto maintenance_confirmation = review_controls.maintenance_confirmation;
    const auto review_id = review_controls.review_id;
    const auto start_after_setup = review_controls.start_after_setup;
    const auto wizard_choices = [this, offer, safety, pool] {
        return readSetupChoices(offer, safety, pool);
    };
    const auto wizard_pool_preview_text = [this](const UniValue& result) {
        return formatSetupPoolPreview(result);
    };
    auto refresh_preview = std::make_shared<std::function<void()>>();
    *refresh_preview = [this, review_page, preview_status, preview_details, preview_retry,
                        reviewed_pool, maintenance_confirmation, wizard_choices, wizard_pool_preview_text] {
        *reviewed_pool = UniValue{};
        review_page->setComplete(false);
        preview_retry->setVisible(false);
        preview_status->setText(tr("Checking the proposed configuration and current reserves…"));
        UniValue params;
        try {
            const auto choices = wizard_choices();
            auto options = choices.pool;
            options.pushKV("preview_policy", choices.policy);
            params = UniValue{UniValue::VARR};
            params.push_back(options);
        } catch (const std::exception& error) { preview_status->setText(QString::fromUtf8(error.what())); preview_retry->show(); return; }
        m_panel.call("preparepaymasterpool", params, false, nullptr,
            [guard = QPointer<QWidget>(review_page), review_page, preview_status, preview_details, preview_retry, reviewed_pool, maintenance_confirmation, wizard_pool_preview_text](const UniValue& result) {
                if (!guard) return;
                if (!IsCompletePoolPreparationResult(result) || !result.find_value("preview_only").isTrue() || result.find_value("accepted").isTrue()) {
                    preview_status->setText(QObject::tr("A complete read-only funding review is unavailable. Retry before applying setup."));
                    preview_retry->show(); return;
                }
                *reviewed_pool = result;
                preview_status->setText(QObject::tr("Review ready. No settings or funds have changed."));
                preview_details->setText(wizard_pool_preview_text(result));
                review_page->setComplete(maintenance_confirmation->isChecked());
            }, false, [guard = QPointer<QWidget>(review_page), preview_status, preview_retry](const QString& error) {
                if (!guard) return;
                preview_status->setText(error); preview_retry->show();
            });
    };
    connect(preview_retry, &QPushButton::clicked, review_page,
            [refresh_preview] { (*refresh_preview)(); });
    connect(maintenance_confirmation, &QCheckBox::toggled, review_page,
            [review_page, reviewed_pool](bool checked) {
                review_page->setComplete(checked && !reviewed_pool->isNull());
            });

    const auto setup_pages = wizard.pageIds();
    for (int index = 0; index < setup_pages.size(); ++index) {
        auto* page = wizard.page(setup_pages[index]);
        page->setTitle(tr("Step %1 of %2 — %3").arg(index + 1).arg(setup_pages.size()).arg(page->title()));
    }
    connect(&wizard, &QWizard::currentIdChanged, &wizard,
            [this, &wizard, review_id, context, offer, safety, pool, review_controls, refresh_preview](int current_id) {
                wizard.setButtonText(QWizard::NextButton,
                    current_id == review_id ? tr("Apply setup…") : tr("Next >"));
                if (current_id != review_id) return;
                renderSetupReview(context, offer, safety, pool, review_controls);
                (*refresh_preview)();
            });

    review_page->setValidator([this, &wizard, offer, safety, review_controls,
                               reviewed_pool, setup_start_requested, start_after_setup,
                               setup_wallet_name, autostart, wizard_pool_preview_text] {
        const QString validation_error = setupReviewError(offer, safety, review_controls);
        if (!validation_error.isEmpty()) {
            QMessageBox::warning(&wizard, tr("Review Paymaster setup"),
                                 validation_error);
            return false;
        }
        if (reviewed_pool->isNull()) return false;
        QString confirmation = tr(
            "Apply the reviewed setup for wallet \"%1\"?\n\n%2\n\n"
            "This approves only the displayed capital and maximum setup fees, together with the recurring limits selected above. "
            "Core will continue automatically. Any larger funding requirement needs a new review.")
            .arg(setup_wallet_name, wizard_pool_preview_text(*reviewed_pool));
        confirmation += start_after_setup->isChecked()
            ? tr("\n\nStart the provider when ready.")
            : tr("\n\nNo one-time start is requested.");
        confirmation += tr("\nSave autostart: %1. When enabled, it can start this provider independently of the one-time start choice, once all prerequisites are met.")
                            .arg(autostart->isChecked() ? tr("on") : tr("off"));
        // The injected RPC transport exists only in widget tests, where
        // native modal message boxes are not reliable on headless Qt
        // platforms. Production always requires the explicit approval.
        if (!m_panel.hasSynchronousTestExecutor() &&
            PaymasterWidgetImpl::askPlainTextQuestion(
                &wizard, tr("Confirm complete Paymaster setup"),
                confirmation) != QMessageBox::Yes) {
            return false;
        }
        *setup_start_requested = start_after_setup->isChecked();
        setSetupMode(PaymasterSetupMode::GUIDED);
        return true;
    });

}

void ProviderSetup::renderSetupStatus(const QPointer<QLabel>& funding_label, const UniValue& expected, const UniValue& current)
{
    if (!funding_label || m_panel.privacyEnabled()) return;
    DigiDollar::Paymaster::CheckSetupContext(expected, current);
    const auto progress = DigiDollar::Paymaster::InspectSetupProgress(current, false);
    const auto& provider = current.find_value("provider");
    QString text;
    if (!provider.find_value("enabled").isTrue()) {
        text = tr("Provider configuration is disabled. Approved preparation is paused until you explicitly enable it.");
    } else if (progress.needs_unlock) {
        text = tr("Waiting for wallet unlock. Close this assistant and unlock the provider wallet on Overview; the approved task remains saved.");
    } else if (progress.complete) {
        if (provider.find_value("running").isTrue()) text = tr("Provider is running and locally ready.");
        else if (provider.find_value("start_requested").isTrue() || provider.find_value("autostart").isTrue())
            text = tr("Reserves are ready. Waiting for Core to confirm the requested or automatic start.");
        else text = tr("Reserves are ready. The provider is stopped; use Start / resume on Overview when you want to operate it.");
    } else {
        text = tr("Configuration is saved; preparation or another prerequisite is still pending.");
    }
    const auto& preparation = provider.find_value("preparation");
    text += QStringLiteral("\n\n") + (preparation.isArray()
        ? m_panel.liquidity().formatPoolPreparation(preparation, PreparationDetail::Operator)
        : tr("Current reserve-funding amounts are unavailable. Check again; missing information does not mean no funding is needed."));
    const auto& errors = provider.find_value("readiness_errors");
    if (errors.isArray()) {
        for (const auto& error : errors.getValues()) {
            if (error.isStr()) text += QStringLiteral("\n") + m_panel.overview().readinessExplanation(QString::fromStdString(error.get_str()));
        }
    }
    text += tr("\n\nStatus is checked every 5 seconds while this assistant is open. Closing it does not cancel approved preparation or a requested start. To change the target or cancel uncreated work, use Settings → Reserves & refill.");
    funding_label->setText(text.trimmed());
}

std::shared_ptr<bool> ProviderSetup::connectSetupExecution(PaymasterSetupWizard& wizard, const SetupWizardContext& context, const SetupOfferControls& offer, const SetupSafetyControls& safety, const SetupPoolControls& pool, const SetupProgressControls& progress)
{
    const auto setup_open = std::make_shared<bool>(true);
    connect(&wizard, &QDialog::finished, &wizard, [setup_open] { *setup_open = false; });
    const auto setup_current = [guard = QPointer<PaymasterSetupWizard>{&wizard}, setup_open] {
        return guard && *setup_open;
    };
    // Closing the wizard invalidates UI continuations immediately, even before
    // its modal stack unwinds. An already dispatched Core operation still
    // finishes, but its response cannot access controls or start the next step.
    const auto setup_model = context.setup_model;
    const auto setup_wallet_id = context.setup_wallet_id;
    const auto wallet_confirmation = context.wallet_confirmation;
    const auto setup_start_requested = context.setup_start_requested;
    const auto reviewed_pool = context.reviewed_pool;
    const auto display = offer.display;
    const auto replace_unrepresentable_policy = offer.replace_unrepresentable_policy;
    const auto operation_mode = pool.operation_mode;
    const auto autostart = pool.autostart;
    const auto provider_enabled = pool.provider_enabled;
    const auto progress_page = progress.progress_page;
    const auto progress_names = progress.progress_names;
    const auto progress_steps = progress.progress_steps;
    const auto progress_result = progress.progress_result;
    const auto setup_retry = progress.setup_retry;
    const auto setup_backup = progress.setup_backup;
    const auto setup_backup_id = progress.setup_backup_id;
    const auto setup_funding = progress.setup_funding;
    const auto setup_refresh = progress.setup_refresh;
    const auto progress_id = progress.progress_id;
    const auto wizard_choices = [this, offer, safety, pool] {
        return readSetupChoices(offer, safety, pool);
    };
    const auto setup_step_label = [](const std::string& method) {
        return setupStepLabel(method);
    };
    auto setup_step = std::make_shared<int>(0);
    auto setup_running = std::make_shared<bool>(false);
    auto setup_started = std::make_shared<bool>(false);
    auto setup_completed = std::make_shared<bool>(false);
    auto setup_needs_quiesce = std::make_shared<bool>(false);
    auto setup_needs_identity = std::make_shared<bool>(false);
    auto setup_needs_liquidity = std::make_shared<bool>(false);
    auto setup_provider_id = std::make_shared<QString>();
    auto policy_params = std::make_shared<UniValue>();
    auto safety_params = std::make_shared<UniValue>();
    auto liquidity_policy_params = std::make_shared<UniValue>();
    auto liquidity_preview_params = std::make_shared<UniValue>();

    const auto set_progress = [setup_current, progress_steps, progress_names](int step,
                                                               const QString& marker,
                                                               const QString& state) {
        if (!setup_current()) return;
        progress_steps.at(step)->setText(
            QStringLiteral("%1 %2 — %3").arg(marker, state, progress_steps.at(step)->property("setupMethod").isValid() ? progress_steps.at(step)->property("setupMethod").toString() : progress_names.at(step)));
    };
    const auto set_wizard_running = [&wizard, setup_current](bool running) {
        if (!setup_current()) return;
        wizard.setCloseBlocked(running);
        if (wizard.button(QWizard::BackButton)) {
            wizard.button(QWizard::BackButton)->setEnabled(false);
        }
        if (wizard.button(QWizard::CancelButton)) {
            wizard.button(QWizard::CancelButton)->setEnabled(!running);
        }
    };
    const auto fail_setup = [setup_current, setup_running, setup_retry, progress_result,
                             setup_step, setup_needs_quiesce, set_progress,
                             set_wizard_running](
                                    const QString& error) {
        if (!setup_current()) return;
        *setup_running = false;
        set_progress(*setup_step, QStringLiteral("✕"), tr("Failed"));
        progress_result->setText(tr(
            "Setup stopped at this step: %1\n\nCompleted earlier steps remain saved. Correct the problem and retry; no completed step needs to be repeated manually.%2")
            .arg(error,
                 *setup_needs_quiesce && *setup_step > 1
                     ? tr(" The existing provider was placed safely offline before configuration changed and remains disabled until the final enabled-state step succeeds.")
                     : QString{}));
        setup_retry->setEnabled(true);
        setup_retry->setVisible(true);
        set_wizard_running(false);
    };

    auto shared_steps = std::make_shared<std::vector<DigiDollar::Paymaster::SetupStep>>();
    auto shared_context = std::make_shared<UniValue>();
    const QPointer<QLabel> funding_label{setup_funding};
    const QPointer<QPushButton> funding_refresh{setup_refresh};
    const auto render_setup_status = [this, setup_current, funding_label, shared_context](const UniValue& current) {
        if (!setup_current()) return;
        renderSetupStatus(funding_label, *shared_context, current);
    };
    const auto refresh_setup_status = [this, setup_current, funding_label, funding_refresh, render_setup_status,
                                       setup_completed, setup_model, setup_wallet_id] {
        if (!setup_current() || !funding_label || !funding_refresh || !*setup_completed || m_panel.privacyEnabled() || m_panel.busy() || m_panel.rpcActive() ||
            !setup_model || m_panel.walletModel() != setup_model || m_panel.walletModel()->getWalletName() != setup_wallet_id) return;
        funding_refresh->setEnabled(false);
        const auto failed = [this, setup_current, funding_label, funding_refresh](const QString&) {
            if (!setup_current() || !funding_label || !funding_refresh || m_panel.privacyEnabled()) return;
            funding_label->setText(tr("Current funding and readiness could not be refreshed. Check again; the previously displayed status is no longer current. Approved settings remain saved."));
            funding_refresh->setEnabled(true);
        };
        m_panel.call("getpaymasteroperatorinfo", {}, false, nullptr,
            [setup_current, funding_label, funding_refresh, render_setup_status, failed](const UniValue& current) {
                if (!setup_current() || !funding_label || !funding_refresh) return;
                try { render_setup_status(current); }
                catch (const std::exception&) { failed({}); return; }
                funding_refresh->setEnabled(true);
            }, false, failed, true);
    };
    auto* setup_status_timer = new QTimer(&wizard);
    setup_status_timer->setObjectName("paymasterSetupStatusTimer");
    setup_status_timer->setInterval(5000);
    connect(setup_status_timer, &QTimer::timeout, progress_page, refresh_setup_status);
    connect(setup_refresh, &QPushButton::clicked, progress_page, refresh_setup_status);
    auto advance_setup = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_advance_setup{advance_setup};
    *advance_setup = [this, &wizard, setup_current, progress_page, progress_result, setup_retry,
                      setup_backup, setup_backup_id, setup_step, setup_running,
                      setup_completed, setup_needs_quiesce, setup_provider_id,
                      progress_steps, set_wizard_running, fail_setup, setup_step_label,
                      weak_advance_setup, shared_steps, shared_context,
                      setup_model, setup_wallet_id, display, policy_params,
                      safety_params, liquidity_policy_params, liquidity_preview_params,
                      operation_mode, provider_enabled, autostart, reviewed_pool, setup_start_requested,
                      render_setup_status, setup_status_timer, setup_refresh] {
        using namespace DigiDollar::Paymaster;
        if (!setup_current()) return;
        if (!setup_model || m_panel.walletModel() != setup_model || m_panel.walletModel()->getWalletName() != setup_wallet_id) {
            fail_setup(tr("The selected wallet changed or was unloaded. Reopen setup in the intended wallet."));
            return;
        }
        *setup_running = true;
        setup_retry->setVisible(false);
        set_wizard_running(true);
        m_panel.call("getpaymasteroperatorinfo", {}, false, nullptr, [this, &wizard, setup_current, progress_page, progress_result, setup_retry, setup_backup, setup_backup_id, setup_step, setup_running, setup_completed, setup_needs_quiesce, setup_provider_id, progress_steps, set_wizard_running, fail_setup, setup_step_label, weak_advance_setup, shared_steps, shared_context, display, policy_params, safety_params, liquidity_policy_params, liquidity_preview_params, operation_mode, provider_enabled, autostart, reviewed_pool, setup_start_requested, render_setup_status, setup_status_timer, setup_refresh](const UniValue& current) {
                if (!setup_current()) return;
                try {
                    if (shared_context->isNull()) {
                        SetupChoices choices;
                        choices.display_name = display->text().trimmed().toStdString();
                        choices.policy = (*policy_params)[0];
                        choices.safety = (*safety_params)[0];
                        choices.liquidity = (*liquidity_policy_params)[0];
                        choices.pool = (*liquidity_preview_params)[0];
                        choices.operation_mode = operation_mode->currentData().toString().toStdString();
                        choices.enabled = provider_enabled->isChecked();
                        choices.autostart = autostart->isChecked();
                        *shared_steps = BuildSetupPlan(current, choices);
                        *setup_needs_quiesce = current.find_value("provider").find_value("enabled").isTrue();
                    } else {
                        CheckSetupContext(*shared_context, current);
                        if (*setup_step > 0) CheckSetupSaved((*shared_steps)[*setup_step - 1], current);
                    }
                    *shared_context = current;
                } catch (const std::exception& error) { fail_setup(QString::fromUtf8(error.what())); return; }
                for (int index = 0; index < progress_steps.size(); ++index) {
                    progress_steps[index]->setVisible(index < int(shared_steps->size()));
                    if (index < int(shared_steps->size())) progress_steps[index]->setProperty("setupMethod", setup_step_label((*shared_steps)[index].method));
                    if (index < int(shared_steps->size())) progress_steps[index]->setText(
                        QStringLiteral("%1 %2").arg(index < *setup_step ? QStringLiteral("✓") : QStringLiteral("○"),
                                                   setup_step_label((*shared_steps)[index].method)));
                }
                if (*setup_step >= int(shared_steps->size())) {
                    *setup_running = false;
                    *setup_completed = true;
                    m_panel.overview().applySetupServiceState(provider_enabled->isChecked(), current.find_value("provider").find_value("running").isTrue());
                    m_panel.configuration().observeRuntime(current.find_value("provider"));
                    m_setup_waiting_for_confirmations = !current.find_value("provider").find_value("pool_ready").isTrue();
                    const auto& id = current.find_value("provider").find_value("provider_id");
                    if (id.isStr()) { *setup_provider_id = QString::fromStdString(id.get_str()); setup_backup_id->setText(*setup_provider_id); }
                    setup_backup->setVisible(true);
                    QString completion_text = tr("Configuration saved. Funding and readiness are shown below. No completed setup steps need to be repeated.");
                    completion_text += tr("\nThe saved autostart choice is %1.").arg(m_panel.configuration().autostartEnabled() ? tr("on") : tr("off"));
                    m_panel.liquidity().acceptSetupOperation();
                    m_panel.overview().renderCurrentTask();
                    if (*setup_start_requested) {
                        if (current.find_value("encrypted").isTrue()) m_panel.overview().unlockOperatorWallet([this, setup_current] { if (setup_current()) m_panel.overview().requestReviewedStart(); });
                        else m_panel.overview().requestReviewedStart();
                    }
                    if (!setup_current()) return;
                    try { render_setup_status(current); }
                    catch (const std::exception&) { completion_text += tr("\nCurrent funding and readiness are unavailable. Check again before acting."); }
                    setup_refresh->show();
                    setup_status_timer->start();
                    progress_result->setText(completion_text);
                    setup_retry->setVisible(false);
                    progress_page->setComplete(true);
                    wizard.setButtonText(QWizard::FinishButton, tr("Finish"));
                    set_wizard_running(false);
                    return;
                }
                const auto step = (*shared_steps)[*setup_step];
                const auto succeed = [setup_current, setup_step, weak_advance_setup] {
                    if (!setup_current()) return;
                    ++*setup_step;
                    if (const auto advance = weak_advance_setup.lock()) (*advance)();
                };
                if (step.method == "preparepaymasterpool") {
                    m_panel.call(step.method, step.params, false, nullptr,
                        [this, &wizard, setup_current, step, succeed, fail_setup, reviewed_pool](const UniValue& preview) {
                            if (!setup_current()) return;
                            if (!IsCompletePoolPreparationResult(preview)) { fail_setup(tr("Core returned an incomplete funding preview.")); return; }
                            try { if (!DigiDollar::Paymaster::SetupPoolNeeded(preview)) { succeed(); return; } }
                            catch (const std::exception& error) { fail_setup(QString::fromUtf8(error.what())); return; }
                            if (!PaymasterOperationController::fundingWithinApproval(*reviewed_pool, preview)) {
                                if (PaymasterWidgetImpl::askPlainTextQuestion(&wizard, tr("Funding requirement changed"),
                                    tr("Review the updated capital and fee limits before continuing.\n\n%1")
                                        .arg(m_panel.liquidity().formatPoolResult("preparepaymasterpool", preview))) != QMessageBox::Yes) {
                                    fail_setup(tr("The changed funding requirement was not approved. No additional funding was authorized.")); return;
                                }
                                if (!setup_current()) return;
                                *reviewed_pool = preview;
                            }
                            auto options = step.params[0]; options.pushKV("execute", true); options.pushKV("plan_id", preview.find_value("plan_id"));
                            UniValue params{UniValue::VARR}; params.push_back(options);
                            m_panel.call("preparepaymasterpool", params, true, nullptr,
                                [setup_current, preview, succeed, fail_setup](const UniValue& result) {
                                    if (!setup_current()) return;
                                    if (!result.find_value("accepted").isTrue() || !SetupMatches(preview.find_value("plan_id"), result.find_value("plan_id"))) { fail_setup(QObject::tr("Core did not confirm the exact funding approval.")); return; }
                                    succeed();
                                }, false, fail_setup);
                        }, false, fail_setup);
                } else {
                    m_panel.call(step.method, step.params, step.unlock, nullptr,
                        [setup_current, step, shared_context, succeed, fail_setup](const UniValue& result) {
                            if (!setup_current()) return;
                            try {
                                CheckSetupReply(step, result);
                                if (step.method == "createpaymasteridentity") {
                                    auto provider = shared_context->find_value("provider"); provider.pushKV("provider_id", result.find_value("provider_id")); shared_context->pushKV("provider", provider);
                                }
                                succeed();
                            }
                            catch (const std::exception& error) { fail_setup(QString::fromUtf8(error.what())); }
                        }, false, fail_setup);
                } }, false, fail_setup);
    };

    const auto prepare_setup_execution = [this, setup_current, wizard_choices, fail_setup, setup_retry, setup_step, setup_started,
                                          setup_needs_identity,
                                           setup_needs_liquidity,
                                           policy_params,
                                           safety_params,
                                          liquidity_policy_params,
                                          liquidity_preview_params,
                                          reviewed_pool, advance_setup,
                                          setup_model, setup_wallet_id,
                                          wallet_confirmation,
                                          replace_unrepresentable_policy] {
        if (!setup_current()) return;
        if (*setup_started) return;

        if (!wallet_confirmation->isChecked() || !setup_model ||
            m_panel.walletModel() != setup_model ||
            m_panel.walletModel()->getWalletName() != setup_wallet_id) {
            *setup_started = true;
            fail_setup(tr(
                "The confirmed provider wallet is no longer selected. No setup setting was written. Close the assistant and reopen it in the intended wallet."));
            setup_retry->setVisible(false);
            return;
        }
        *setup_started = true;

        try {
            const auto choices = wizard_choices();
            *policy_params = UniValue{UniValue::VARR};
            policy_params->push_back(choices.policy);
            *safety_params = UniValue{UniValue::VARR};
            safety_params->push_back(choices.safety);
            *liquidity_policy_params = UniValue{UniValue::VARR};
            liquidity_policy_params->push_back(choices.liquidity);
            *liquidity_preview_params = UniValue{UniValue::VARR};
            liquidity_preview_params->push_back(choices.pool);
        } catch (const std::exception& error) {
            fail_setup(QString::fromUtf8(error.what()));
            return;
        }

        const qint64 missing_outputs =
            ProviderLiquidity::poolNumber(*reviewed_pool, "missing_admission_dgb_slots") +
            ProviderLiquidity::poolNumber(*reviewed_pool, "missing_operational_dgb_slots") +
            ProviderLiquidity::poolNumber(*reviewed_pool, "missing_admission_carrier_slots") +
            ProviderLiquidity::poolNumber(*reviewed_pool, "missing_operational_carrier_slots");
        *setup_needs_identity = !m_panel.overview().hasIdentity();
        *setup_needs_liquidity = missing_outputs > 0;
        *setup_step = 0;
        (*advance_setup)();
    };

    connect(setup_retry, &QPushButton::clicked, progress_page,
            [this, setup_current, progress_page, progress_result, setup_retry,
             setup_running, advance_setup] {
                if (!setup_current() || *setup_running) return;
                setup_retry->setEnabled(false);
                progress_result->setText(tr("Retrying the failed setup step…"));
                // Leave the RPC error callback before starting the next
                // attempt. This also gives Qt a chance to paint immediate
                // retry feedback before an unusually fast failure.
                QTimer::singleShot(0, progress_page,
                    [setup_current, setup_retry, setup_running, advance_setup] {
                        if (!setup_current() || *setup_running) return;
                        setup_retry->setEnabled(true);
                        (*advance_setup)();
                    });
            });
    connect(&wizard, &QWizard::currentIdChanged, progress_page,
            [&wizard, progress_id, prepare_setup_execution](int current_id) {
                if (current_id != progress_id) return;
                wizard.setButtonText(QWizard::FinishButton, tr("Applying setup…"));
                // Applying is intentionally incremental and durable. Once
                // this page is entered, closing cannot roll back completed
                // RPC steps, so do not continue to present it as Cancel.
                wizard.setButtonText(QWizard::CancelButton, tr("Close"));
                if (wizard.button(QWizard::FinishButton)) {
                    wizard.button(QWizard::FinishButton)->setEnabled(false);
                }
                prepare_setup_execution();
            });

    return setup_completed;
}

bool ProviderSetup::showSetupWizard()
{
    if (!setupWizardAvailable()) return false;
    const QPointer<WalletModel> setup_model{m_panel.walletModel()};
    const QString setup_wallet_name = setup_model
        ? setup_model->getDisplayName()
        : tr("No wallet selected");
    const QString setup_wallet_id = setup_model
        ? setup_model->getWalletName()
        : QString{};

    SetupWizardContext context{setup_model, setup_wallet_name, setup_wallet_id};

    PaymasterSetupWizard wizard(widget());
    m_setup_wizard = &wizard;
    auto replace_unrepresentable_policy = std::make_shared<bool>(
        m_panel.configuration().policyRepresentable());
    wizard.setObjectName("PaymasterSetupWizard");
    wizard.setWindowTitle(tr("Guided Paymaster provider setup"));
    wizard.setWizardStyle(QWizard::ClassicStyle);
    wizard.setOption(QWizard::NoBackButtonOnStartPage);
    wizard.setOption(QWizard::HaveHelpButton, false);
    wizard.setSizeGripEnabled(true);
    wizard.setMinimumSize(560, 440);
    wizard.resize(760, 600);
    wizard.setStyleSheet(PaymasterWizardStyleSheet(
        UseDarkPaymasterWizardTheme(m_panel.walletModel(), widget())));

    context.wallet_confirmation = buildSetupWalletPages(wizard, context);
    const auto offer = buildSetupOfferPage(wizard, replace_unrepresentable_policy);
    const auto safety = buildSetupSafetyPage(wizard, offer);
    const auto pool = buildSetupLiquidityPage(wizard, offer, safety);
    auto review = buildSetupReviewPage(wizard, offer, safety, pool);
    const auto progress = buildSetupProgressPage(wizard);
    context.setup_start_requested = std::make_shared<bool>(false);
    addSetupStartChoice(review, pool);
    context.reviewed_pool = std::make_shared<UniValue>();
    connectSetupReview(wizard, context, offer, safety, pool, review);
    const auto setup_completed = connectSetupExecution(wizard, context, offer, safety, pool, progress);

    // The wizard owns a stable wallet snapshot. Pause background provider
    // and finance refreshes for the whole modal session so they cannot
    // overwrite reviewed values or contend with an applying RPC step.
    for (QLabel* label : wizard.findChildren<QLabel*>()) {
        label->setTextFormat(Qt::PlainText);
    }
    m_setup_wizard_active = true;
    m_panel.overview().updateAutomaticRefreshTimer();
    wizard.exec();
    m_setup_wizard_active = false;
    m_setup_wizard.clear();
    m_panel.overview().updateAutomaticRefreshTimer();
    const bool completed = *setup_completed;
    if (!completed && m_panel.walletModel() == setup_model && !m_panel.busy()) {
        // A failed or deliberately closed durable phase can leave a
        // bridge policy persisted. Reload every authoritative component
        // instead of carrying an intermediate value forward as an
        // unsaved Expert-mode edit on the next wizard run.
        m_panel.configuration().discardProviderEdits();
        m_panel.liquidity().discardPolicyEdits();
        m_panel.refreshStatus();
    }
    return completed;
}

void ProviderSetup::buildChoice(PageBindings& pages)
{
    m_setup_choice = new QGroupBox(tr("Set up this Paymaster wallet"), pages.overview);
    m_setup_choice->setObjectName("paymasterSetupChoice");
    auto* setup_choice_layout = new QVBoxLayout(m_setup_choice);
    auto* setup_choice_intro = new QLabel(tr(
        "This is the first time Paymaster operator controls have been opened for this wallet. "
        "Choose how you want to configure the provider. No provider setting is changed until "
        "you explicitly save it, and no funds move without a separate transaction preview and confirmation."),
        m_setup_choice);
    setup_choice_intro->setObjectName("paymasterSetupChoiceIntroduction");
    setup_choice_intro->setWordWrap(true);
    setup_choice_layout->addWidget(setup_choice_intro);

    auto* guided_group = new QGroupBox(tr("Guided setup — recommended"), m_setup_choice);
    auto* guided_layout = new QVBoxLayout(guided_group);
    auto* guided_help = new QLabel(tr(
        "Explains each decision, checks the current provider prerequisites and prepares "
        "recommended policy, safety and liquidity values for your review."), guided_group);
    guided_help->setWordWrap(true);
    m_guided_setup = new QPushButton(tr("Start guided setup…"), guided_group);
    m_guided_setup->setObjectName("paymasterChooseGuidedSetup");
    m_guided_setup->setAccessibleDescription(tr(
        "Open the recommended step-by-step Paymaster provider setup assistant."));
    guided_layout->addWidget(guided_help);
    guided_layout->addWidget(m_guided_setup, 0, Qt::AlignLeft);
    setup_choice_layout->addWidget(guided_group);

    auto* expert_group = new QGroupBox(tr("Manual setup — experts"), m_setup_choice);
    auto* expert_layout = new QVBoxLayout(expert_group);
    auto* expert_help = new QLabel(tr(
        "Unlock all configuration pages immediately. Choose this only if you already understand "
        "provider identities, funding models, finite safety budgets and pool preparation."), expert_group);
    expert_help->setWordWrap(true);
    m_expert_setup = new QPushButton(tr("Use manual expert setup"), expert_group);
    m_expert_setup->setObjectName("paymasterChooseExpertSetup");
    m_expert_setup->setAccessibleDescription(tr(
        "Unlock the advanced Paymaster configuration pages without running the assistant."));
    expert_layout->addWidget(expert_help);
    expert_layout->addWidget(m_expert_setup, 0, Qt::AlignLeft);
    setup_choice_layout->addWidget(expert_group);
    setup_choice_layout->addStretch();
    pages.overview_layout->addWidget(m_setup_choice);

}

void ProviderSetup::connectSetup(PageBindings& pages)
{
    connect(m_guided_setup, &QPushButton::clicked, widget(), [this] {
        if (showSetupWizard()) {
            setSetupMode(PaymasterSetupMode::GUIDED);
            m_panel.mainPages()->setCurrentIndex(0);
            m_panel.refreshStatus();
        }
    });
    connect(m_reopen_wizard, &QPushButton::clicked, widget(), [this] {
        if (showSetupWizard()) {
            m_panel.mainPages()->setCurrentIndex(0);
            m_panel.refreshStatus();
        }
    });
    connect(m_expert_setup, &QPushButton::clicked, widget(), [this] {
        if (QMessageBox::warning(
                widget(), tr("Manual Paymaster setup"),
                tr("Manual setup exposes every provider control immediately. Incomplete settings keep the provider offline, while overly broad sponsored budgets can spend more DGB than intended.\n\nContinue with expert setup?"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) == QMessageBox::Yes) {
            setSetupMode(PaymasterSetupMode::EXPERT);
            m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::CONFIGURATION));
        }
    });
}
