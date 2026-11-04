// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

PaymasterWidgetImpl::PaymasterWidgetImpl(QWidget* parent) : DigiDollarPaymasterWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(12);

    auto* operator_header = new QFrame(this);
    operator_header->setObjectName("paymasterOperatorHeader");
    operator_header->setProperty("paymasterRole", QStringLiteral("operatorHeader"));
    auto* operator_header_layout = new QVBoxLayout(operator_header);
    operator_header_layout->setContentsMargins(18, 14, 18, 14);
    operator_header_layout->setSpacing(4);
    auto* heading = new QLabel(tr("Paymaster provider"), operator_header);
    heading->setObjectName("paymasterOperatorHeading");
    heading->setProperty("paymasterRole", QStringLiteral("operatorHeading"));
    QFont heading_font = heading->font();
    heading_font.setPointSize(18);
    heading_font.setBold(true);
    heading->setFont(heading_font);
    operator_header_layout->addWidget(heading);

    m_wallet = new QLabel(tr("No wallet selected"), operator_header);
    m_wallet->setObjectName("paymasterSelectedWallet");
    m_wallet->setProperty("paymasterRole", QStringLiteral("walletHeading"));
    m_status = new QLabel(tr("Loading provider status…"), operator_header);
    m_status->setObjectName("paymasterProviderStatus");
    m_status->setProperty("paymasterRole", QStringLiteral("globalStatus"));
    m_status->setWordWrap(true);
    operator_header_layout->addWidget(m_wallet);
    operator_header_layout->addWidget(m_status);
    m_last_checked = new QLabel(tr("Status has not been checked yet."), operator_header);
    m_last_checked->setObjectName("paymasterLastChecked");
    m_last_checked->setProperty("paymasterRole", QStringLiteral("mutedText"));
    m_last_checked->setWordWrap(true);
    operator_header_layout->addWidget(m_last_checked);
    outer->addWidget(operator_header);

    m_navigation_shell = new QWidget(this);
    auto* navigation_layout = new QVBoxLayout(m_navigation_shell);
    navigation_layout->setContentsMargins(0, 0, 0, 0);
    m_navigation_select = new NoWheelComboBox(m_navigation_shell);
    m_navigation_select->setObjectName("paymasterNavigationSelect");
    m_navigation_select->setAccessibleName(tr("Paymaster area"));
    navigation_layout->addWidget(m_navigation_select);
    auto* navigation_row = new QHBoxLayout();
    m_navigation = new QListWidget(m_navigation_shell);
    m_navigation->setObjectName("paymasterNavigation");
    m_navigation->setAccessibleName(tr("Paymaster areas"));
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    navigation_row->addWidget(m_navigation);
    m_tabs = new QStackedWidget(m_navigation_shell);
    m_tabs->setObjectName("paymasterOperatorPages");
    navigation_row->addWidget(m_tabs, 1);
    navigation_layout->addLayout(navigation_row, 1);
    outer->addWidget(m_navigation_shell, 1);

    auto* overview_scroll = new QScrollArea(m_tabs);
    overview_scroll->setObjectName("paymasterOverviewPage");
    overview_scroll->setWidgetResizable(true);
    auto* overview = new QWidget(overview_scroll);
    overview->setObjectName("paymasterOverviewContents");
    QVBoxLayout* overview_layout{nullptr};
    auto* overview_column = CreatePaymasterPageColumn(overview, overview_layout);

    m_operator_card = new QGroupBox(tr("Your Paymaster"), overview);
    m_operator_card->setObjectName("paymasterOperatorCard");
    m_operator_card->setProperty("paymasterRole", QStringLiteral("heroCard"));
    auto* operator_layout = new QVBoxLayout(m_operator_card);
    m_operator_headline = new QLabel(tr("Reading current status…"), m_operator_card);
    m_operator_headline->setObjectName("paymasterOperatorHeadline");
    m_operator_headline->setProperty("paymasterRole", QStringLiteral("cardHeading"));
    m_operator_hint = new QLabel(tr("Wait for the selected wallet's current status before starting operation."), m_operator_card);
    m_operator_hint->setObjectName("paymasterOperatorHint");
    m_operator_connection = new QLabel(m_operator_card);
    m_operator_connection->setObjectName("paymasterOperatorConnection");
    m_operator_wallet = new QLabel(m_operator_card);
    m_operator_wallet->setObjectName("paymasterOperatorWallet");
    for (auto* label : {m_operator_headline, m_operator_hint, m_operator_connection, m_operator_wallet}) {
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        operator_layout->addWidget(label);
    }
    operator_layout->addWidget(configurationWarningPanel(m_operator_config_warning, "paymasterOperatorConfigWarning",
        tr("Review spending limits"), [this] { showOperatorPage(m_safety_page); }));
    m_operator_loading = new QWidget(m_operator_card);
    m_operator_loading->setObjectName("paymasterOperatorLoading");
    auto* loading_layout = new QVBoxLayout(m_operator_loading);
    loading_layout->setContentsMargins(0, 0, 0, 0);
    loading_layout->setSpacing(6);
    m_operator_progress = new QProgressBar(m_operator_loading);
    m_operator_progress->setObjectName("paymasterOperatorProgress");
    m_operator_progress->setRange(0, 0); // Busy indicator; RPCs have no time estimate.
    m_operator_progress->setTextVisible(false);
    m_operator_progress->setFixedHeight(10);
    m_operator_progress->setAccessibleName(tr("Loading provider status"));
    m_operator_progress->setToolTip(tr("The current check is still pending. Remaining time is not known."));
    m_operator_loading_step = new QLabel(m_operator_loading);
    m_operator_loading_step->setObjectName("paymasterOperatorLoadingStep");
    m_operator_loading_step->setTextFormat(Qt::PlainText);
    m_operator_loading_step->setWordWrap(true);
    m_operator_loading_step->setProperty("paymasterRole", QStringLiteral("mutedText"));
    loading_layout->addWidget(m_operator_progress);
    loading_layout->addWidget(m_operator_loading_step);
    operator_layout->insertWidget(2, m_operator_loading);
    m_operator_loading->hide();
    m_operator_progress_timer = new QTimer(this);
    m_operator_progress_timer->setObjectName("paymasterOperatorProgressTimer");
    m_operator_progress_timer->setInterval(1000);
    connect(m_operator_progress_timer, &QTimer::timeout, this, [this] { showOperatorBusyState(); });
    auto* operator_actions = new QHBoxLayout;
    m_operator_action = new QPushButton(tr("Refresh status"), m_operator_card);
    m_operator_action->setObjectName("paymasterOperatorNextAction");
    m_operator_action->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    m_operator_start = new QPushButton(tr("Start Paymaster…"), m_operator_card);
    m_operator_start->setObjectName("paymasterOperatorStart");
    m_operator_start->setEnabled(false);
    connect(m_operator_start, &QPushButton::clicked, this, [this] {
        if (m_operator_start_allowed && !m_privacy && !m_busy) resumeOperator();
    });
    m_operator_pause = new QPushButton(tr("Pause provider…"), m_operator_card);
    m_operator_pause->setObjectName("paymasterOperatorPause");
    m_operator_pause->setToolTip(tr("Pause persistently, including autostart and new pool signatures. Already signed transactions can still confirm."));
    m_operator_pause->hide();
    operator_actions->addWidget(m_operator_start);
    operator_actions->addWidget(m_operator_action);
    operator_actions->addWidget(m_operator_pause);
    operator_actions->addStretch();
    operator_layout->addLayout(operator_actions);
    auto* operation_help = new QLabel(tr("Pause keeps prepared reserves and turns autostart off. Start again when you are ready; autostart stays off until you enable it."), m_operator_card);
    operation_help->setWordWrap(true);
    operation_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    operator_layout->addWidget(operation_help);
    overview_layout->addWidget(m_operator_card);
    m_task_card = new QGroupBox(tr("Provider management task"), overview_column);
    m_task_card->setObjectName("paymasterCurrentTask");
    m_task_card->setProperty("paymasterRole", QStringLiteral("card"));
    auto* task_layout = new QVBoxLayout(m_task_card);
    m_task_status = new QLabel(m_task_card);
    m_task_status->setObjectName("paymasterTaskStatus");
    m_task_status->setWordWrap(true);
    m_task_status->setTextFormat(Qt::PlainText);
    m_task_progress = new QProgressBar(m_task_card);
    m_task_progress->setObjectName("paymasterTaskProgress");
    m_task_progress->setRange(0, 0);
    m_task_progress->setTextVisible(false);
    task_layout->addWidget(m_task_status);
    auto* task_scope = new QLabel(tr("This progress belongs to provider management and reserve work. Customer offers and payments are tracked separately in Activity."), m_task_card);
    task_scope->setWordWrap(true);
    task_scope->setProperty("paymasterRole", QStringLiteral("mutedText"));
    task_layout->addWidget(task_scope);
    task_layout->addWidget(m_task_progress);
    m_task_continue = new QPushButton(tr("Continue task…"), m_task_card);
    m_task_continue->setObjectName("paymasterContinueTask");
    task_layout->addWidget(m_task_continue, 0, Qt::AlignLeft);
    m_cancel_maintenance = new QPushButton(tr("Cancel uncreated reserve work…"), m_task_card);
    m_cancel_maintenance->setObjectName("paymasterCancelMaintenance");
    task_layout->addWidget(m_cancel_maintenance, 0, Qt::AlignLeft);
    connect(m_cancel_maintenance, &QPushButton::clicked, this, [this] { cancelMaintenance(); });
    m_task_error_details = new QLabel(m_task_card);
    m_task_error_details->setObjectName("paymasterTaskErrorDetails");
    m_task_error_details->setTextFormat(Qt::PlainText);
    m_task_error_details->setWordWrap(true);
    m_task_error_details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_task_error_toggle = AddPaymasterDisclosure(task_layout, m_task_card, m_task_error_details,
        tr("Show technical details"), tr("Hide technical details"), QStringLiteral("paymasterTaskErrorToggle"));
    m_task_error_toggle->hide();
    connect(m_task_continue, &QPushButton::clicked, this, [this] {
        if (m_operation.error_code.contains(QLatin1String("WAITING_FUNDS")) ||
            m_operation.error_code.contains(QLatin1String("WAITING_DGB"))) {
            showOperatorPage(m_liquidity_page);
            refreshStatus();
        } else if (canReviewReserveReduction()) {
            beginGuidedTask("reduce_reserves");
        } else if (m_operation.error_code == QLatin1String("PAYMASTER_POOL_PREPARATION_REQUIRED") ||
            m_operation.error_code == QLatin1String("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL") ||
            m_operation.error_code == QLatin1String("PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE")) {
            beginGuidedTask("preparepaymasterpool", m_guided_start_after ||
                m_operator_snapshot.find_value("provider").find_value("start_requested").isTrue());
        } else if (carrierReleaseNeedsReview()) {
            showOperatorPage(m_liquidity_page);
            call("getpaymasterpoolinfo", {}, false, nullptr, [this](const UniValue& pool) {
                applyLiquidityPoolEntries(pool, false);
                if (!IsCompleteProviderPoolInfo(pool)) {
                    failGuidedTask(tr("The current reserves could not be verified. Refresh the status before choosing a reserve. No release was approved."));
                    return;
                }
                if (m_release_carrier_select->count() > 0) m_operation.reset(m_wallet_generation);
                renderCurrentTask();
                refreshOperatorStatus();
            }, false, [this](const QString& error) { failGuidedTask(error); });
        } else if (m_operation.error_code.contains(QLatin1String("WALLET_LOCKED"))) unlockOperatorWallet();
        else if (m_operation.error_code == QLatin1String("PAYMASTER_MAINTENANCE_FEE_EXCEEDED")) reviewMaintenanceFeeLimit();
        else if (m_operation.error_code == QLatin1String("PAYMASTER_RETIREMENT_WALLET_FEE_LIMIT")) refreshStatus();
        else if (canReviewPreparationFee()) cancelPoolPreparation(true);
        else if (m_operation.error_code.contains(QLatin1String("FEE_LIMIT"))) {
            showOperatorPage(m_liquidity_page);
            if (m_liquidity_advanced_toggle) m_liquidity_advanced_toggle->setChecked(true);
            m_preparation_fee->setFocus();
        } else if (m_operation.error_code == QLatin1String("PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED")) {
            showOperatorPage(m_liquidity_page);
        } else if (m_operation.error_code.contains(QLatin1String("PROVIDER_DISABLED"))) {
            const auto generation = m_wallet_generation;
            setRpcBusyState(true);
            const auto answer = askPlainTextQuestion(this, tr("Resume approved task"),
                tr("Enable the saved provider configuration so Core can continue this approved task within its existing limits? The saved autostart choice remains unchanged."));
            if (generation != m_wallet_generation) return;
            setRpcBusyState(false);
            if (answer != QMessageBox::Yes || m_privacy) return;
            UniValue params{UniValue::VARR}; params.push_back(true);
            call("setpaymasterenabled", params, false, nullptr, [this](const UniValue&) { refreshStatus(); },
                false, [this](const QString& error) { failGuidedTask(error); });
        } else refreshStatus();
    });
    overview_layout->addWidget(m_task_card);
    m_task_card->hide();
    auto* task_actions = new QHBoxLayout();
    m_restore_task = new QPushButton(tr("Restore reserves…"), overview_column);
    m_restore_task->setObjectName("paymasterRestoreTask");
    m_withdraw_task = new QPushButton(tr("Withdraw earnings…"), overview_column);
    m_withdraw_task->setObjectName("paymasterWithdrawTask");
    m_release_task = new QPushButton(tr("Release one DD reserve…"), overview_column);
    m_release_task->setObjectName("paymasterReleaseTask");
    for (auto* button : {m_restore_task, m_withdraw_task, m_release_task}) {
        button->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        task_actions->addWidget(button);
    }
    // Attached to Settings > Reserves & refill after all pages exist.
    m_withdrawal_hint = new QLabel(overview_column);
    m_withdrawal_hint->setObjectName("paymasterEarningsWithdrawalHint");
    m_withdrawal_hint->setWordWrap(true);
    m_withdrawal_hint->setTextFormat(Qt::PlainText);
    m_withdrawal_hint->hide();

    connect(m_restore_task, &QPushButton::clicked, this, [this] { beginGuidedTask("preparepaymasterpool"); });
    connect(m_withdraw_task, &QPushButton::clicked, this, [this] { beginGuidedTask("all_excess"); });
    connect(m_release_task, &QPushButton::clicked, this, [this] { beginGuidedTask("release_slot"); });

    connect(m_operator_action, &QPushButton::clicked, this, [this] { performOperatorNextAction(); });
    connect(m_operator_pause, &QPushButton::clicked, this, [this] { if (!m_privacy) pauseOperator(); });

    // Node and wallet controls belong together, away from the daily overview.
    auto* connection_scroll = new QScrollArea;
    connection_scroll->setObjectName("paymasterConnectionPage");
    connection_scroll->setWidgetResizable(true);
    auto* connection = new QWidget(connection_scroll);
    connection->setObjectName("paymasterConnectionContents");
    QVBoxLayout* connection_layout{nullptr};
    auto* connection_column = CreatePaymasterPageColumn(connection, connection_layout);
    AddPaymasterPageHeading(connection_layout, connection_column, tr("Node connection"),
                            tr("Connection settings apply to every wallet on this node. Review reachability and changes that need a restart."), QStringLiteral("paymasterConnection"));
    auto* network_group = new QGroupBox(tr("Connection checks"), connection_column);
    network_group->setObjectName("paymasterConnectionNetwork");
    auto* network_layout = new QVBoxLayout(network_group);
    auto* connection_help = new QLabel(tr("Local readiness means this node can accept connections. To check reachability, run checkpaymasterendpoint with the announced IP or Tor address from another node. The RPC result describes that connection attempt; it does not verify provider identity, available reserves or payments."), network_group);
    connection_help->setObjectName("paymasterConnectionHelp");
    connection_help->setWordWrap(true);
    network_layout->addWidget(connection_help);
    auto* configure_node = new QPushButton(tr("Refresh connection settings"), network_group);
    configure_node->setObjectName("paymasterReviewConnection");
    network_layout->addWidget(configure_node, 0, Qt::AlignLeft);
    connection_layout->addWidget(network_group);
    auto* access_group = new QGroupBox(tr("Wallet access"), connection_column);
    access_group->setObjectName("paymasterConnectionWalletAccess");
    auto* access_layout = new QVBoxLayout(access_group);
    auto* access_help = new QLabel(tr("The provider needs an unlocked wallet to sign. Unlocking applies to the whole wallet, not just Paymaster. Choose a time limit or continuous operation until manual locking, wallet closure or node restart. No passphrase is saved; unlocking alone does not start the provider."), access_group);
    access_help->setWordWrap(true);
    access_layout->addWidget(access_help);
    auto* unlock_operation = new QPushButton(tr("Unlock wallet for operation…"), access_group);
    unlock_operation->setObjectName("paymasterUnlockOperation");
    configure_node->setToolTip(tr("Review listener, endpoint and configuration changes before saving. Saved node changes require an explicit restart."));
    unlock_operation->setToolTip(tr("Choose continuous operation until manual lock or restart, or a timed wallet-wide unlock. No passphrase is saved."));
    access_layout->addWidget(unlock_operation, 0, Qt::AlignLeft);
    connection_layout->addWidget(access_group);
    connect(configure_node, &QPushButton::clicked, this, [this] { configureOperatorNode(); });
    connect(unlock_operation, &QPushButton::clicked, this, [this] { unlockOperatorWallet(); });
    m_connection_page = connection_scroll;

    auto* management_scroll = new QScrollArea;
    m_management_page = management_scroll;
    management_scroll->setObjectName("paymasterManagementPage");
    management_scroll->setWidgetResizable(true);
    auto* management = new QWidget(management_scroll);
    management->setObjectName("paymasterManagementContents");
    QVBoxLayout* management_layout{nullptr};
    auto* management_column = CreatePaymasterPageColumn(management, management_layout);
    AddPaymasterPageHeading(management_layout, management_column, tr("Wallet & backup"),
                            tr("Manage operating access and protect this provider identity, configuration and financial records."), QStringLiteral("paymasterManagement"));
    auto* lifecycle_scroll = new QScrollArea;
    m_lifecycle_page = lifecycle_scroll;
    lifecycle_scroll->setObjectName("paymasterLifecyclePage");
    lifecycle_scroll->setWidgetResizable(true);
    auto* lifecycle_contents = new QWidget(lifecycle_scroll);
    lifecycle_contents->setObjectName("paymasterLifecycleContents");
    QVBoxLayout* lifecycle_page_layout{nullptr};
    auto* lifecycle_column = CreatePaymasterPageColumn(lifecycle_contents, lifecycle_page_layout);
    AddPaymasterPageHeading(lifecycle_page_layout, lifecycle_column, tr("Start, pause & retire"),
        tr("Choose how to operate this provider. The checklists describe each action after confirmation; they are not completion indicators."),
        QStringLiteral("paymasterLifecycle"));
    m_lifecycle_status = new QLabel(lifecycle_column);
    m_lifecycle_status->setObjectName("paymasterLifecycleStatus");
    m_lifecycle_status->setTextFormat(Qt::PlainText);
    m_lifecycle_status->setWordWrap(true);
    lifecycle_page_layout->addWidget(m_lifecycle_status);
    auto* status_link = new QPushButton(tr("View status and prerequisites on Overview"), lifecycle_column);
    status_link->setObjectName("paymasterLifecycleOverviewLink");
    connect(status_link, &QPushButton::clicked, this, [this] { m_tabs->setCurrentIndex(0); });
    lifecycle_page_layout->addWidget(status_link, 0, Qt::AlignLeft);
    const auto action_card = [lifecycle_page_layout, lifecycle_column](const QString& title, const QStringList& effects, const char* name) {
        auto* card = new QGroupBox(title, lifecycle_column);
        card->setObjectName(name);
        auto* layout = new QVBoxLayout(card);
        for (const auto& effect : effects) {
            auto* label = new QLabel(QStringLiteral("✓ ") + effect, card);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            layout->addWidget(label);
        }
        lifecycle_page_layout->addWidget(card);
        return layout;
    };
    auto* start_layout = action_card(tr("Start / resume"), {
        tr("Request operation with saved settings, subject to readiness and wallet checks."),
        tr("Keep budgets and automation preferences; autostart stays unchanged.")}, "paymasterLifecycleStartCard");
    m_lifecycle_start = new QPushButton(tr("Start Paymaster…"), lifecycle_column);
    m_lifecycle_start->setObjectName("paymasterLifecycleStart");
    start_layout->addWidget(m_lifecycle_start, 0, Qt::AlignLeft);
    connect(m_lifecycle_start, &QPushButton::clicked, m_operator_start, &QPushButton::click);
    auto* pause_layout = action_card(tr("Pause — keep reserves"), {
        tr("Stop service, disable autostart and pause new reserve-setup signatures."),
        tr("Keep reserves and unfinished work protected."),
        tr("Keep refill settings; refill execution waits for an explicit restart.")}, "paymasterLifecyclePauseCard");
    m_lifecycle_pause = new QPushButton(tr("Pause provider…"), lifecycle_column);
    m_lifecycle_pause->setObjectName("paymasterLifecyclePause");
    pause_layout->addWidget(m_lifecycle_pause, 0, Qt::AlignLeft);
    connect(m_lifecycle_pause, &QPushButton::clicked, m_operator_pause, &QPushButton::click);
    auto* stop_layout = action_card(tr("Stop — optional reserve release"), {
        tr("Apply the same persistent pause as Pause; there is no separate Stop state."),
        tr("Optionally review full reserve release in a second confirmation."),
        tr("After approved release, turn off automatic refill and its fee approval.")}, "paymasterLifecycleStopCard");
    m_operator_stop = new QPushButton(tr("Stop Paymaster…"), lifecycle_column);
    m_operator_stop->setObjectName("paymasterOperatorStop");
    stop_layout->addWidget(m_operator_stop, 0, Qt::AlignLeft);
    auto* retire_layout = action_card(tr("Retire — release reserves and review backup"), {
        tr("Stop persistently and check unfinished work."),
        tr("Separately review exact reserve release and cancellation of funding tasks with no transaction."),
        tr("After approved release, turn off automatic refill and its fee approval."),
        tr("Show remaining wallet funds and offer a full-wallet backup before archiving.")}, "paymasterLifecycleRetireCard");
    m_operator_retire = new QPushButton(tr("Retire Paymaster…"), lifecycle_column);
    m_operator_retire->setObjectName("paymasterOperatorRetire");
    retire_layout->addWidget(m_operator_retire, 0, Qt::AlignLeft);
    auto* lifecycle = new QGroupBox(tr("What remains protected"), lifecycle_column);
    lifecycle->setObjectName("paymasterLifecycleSettings");
    auto* lifecycle_layout = new QVBoxLayout(lifecycle);
    auto* stop_help = new QLabel(tr("The node and other wallets keep running. Already signed transactions can still confirm. Release makes reserves available in this wallet; it does not send funds, close the wallet or delete identity and recovery records. Declining release keeps the reserves protected. After a confirmed stop, an incomplete retirement leaves the provider stopped."), lifecycle);
    stop_help->setWordWrap(true);
    lifecycle_layout->addWidget(stop_help);
    m_lifecycle_start->setEnabled(false);
    m_lifecycle_pause->setEnabled(false);
    m_operator_stop->setEnabled(false);
    m_operator_retire->setEnabled(false);
    connect(m_operator_stop, &QPushButton::clicked, this, [this] { stopOperator(); });
    connect(m_operator_retire, &QPushButton::clicked, this, [this] { stopOperator(true); });
    m_operator_stop_status = new QLabel(lifecycle);
    m_operator_stop_status->setObjectName("paymasterStopStatus");
    m_operator_stop_status->setProperty("paymasterRole", QStringLiteral("statusText"));
    m_operator_stop_status->setWordWrap(true);
    m_operator_stop_status->setTextFormat(Qt::PlainText);
    m_operator_stop_status->hide();
    lifecycle_layout->addWidget(m_operator_stop_status);
    lifecycle_page_layout->addWidget(lifecycle);

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
    connect(m_retirement_backup, &QPushButton::clicked, this, [this] { requestProviderBackup(); });
    m_retirement_backup_status = new QLabel(m_retirement_summary);
    m_retirement_backup_status->setObjectName("paymasterRetirementBackupStatus");
    m_retirement_backup_status->setWordWrap(true);
    retirement_summary_layout->addWidget(m_retirement_backup_status);
    auto* close_help = new QLabel(tr("After reviewing funds and securing a complete backup, you may use File → Close Wallet if this wallet is only used for this provider. Closing unloads the wallet; it does not delete it. Do not close a shared wallet that is still needed for other activity. Never run a restored copy of the same provider identity alongside the original."), m_retirement_summary);
    close_help->setObjectName("paymasterRetirementCloseHelp");
    close_help->setWordWrap(true);
    retirement_summary_layout->addWidget(close_help);
    lifecycle_page_layout->addWidget(m_retirement_summary);
    m_retirement_summary->hide();
    lifecycle_page_layout->addStretch();
    lifecycle_scroll->setWidget(lifecycle_contents);
    ConfigureScrollArea(lifecycle_scroll, lifecycle_contents);

    m_operator_summary = new QLabel(tr("Reading current provider status…"), overview);
    m_operator_summary->setObjectName("paymasterOperatorSummary");
    m_operator_summary->setWordWrap(true);
    m_operator_summary->setTextFormat(Qt::PlainText);
    m_operator_summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);


    m_setup_choice = new QGroupBox(tr("Set up this Paymaster wallet"), overview);
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
    overview_layout->addWidget(m_setup_choice);

    m_setup_content = new QWidget(overview);
    m_setup_content->setObjectName("paymasterConfiguredOverview");
    auto* configured_layout = new QVBoxLayout(m_setup_content);
    configured_layout->setContentsMargins(0, 0, 0, 0);

    auto* introduction = new QGroupBox(tr("Review the saved setup"), management_column);
    introduction->setObjectName("paymasterOverviewIntroduction");
    introduction->setProperty("paymasterRole", QStringLiteral("heroCard"));
    auto* introduction_layout = new QVBoxLayout(introduction);
    auto* introduction_text = new QLabel(tr(
        "Help other wallets send DigiDollar when they do not hold DGB. "
        "This wallet supplies the network fee within the limits shown below."), introduction);
    introduction_text->setObjectName("paymasterIntroduction");
    introduction_text->setWordWrap(true);
    introduction_layout->addWidget(introduction_text);
    auto* responsibility = new QLabel(tr(
        "Core validates every payment locally. Your configured spending limits remain the final financial safeguard."), introduction);
    responsibility->setObjectName("paymasterResponsibilityNotice");
    responsibility->setProperty("paymasterRole", QStringLiteral("mutedText"));
    responsibility->setWordWrap(true);
    introduction_layout->addWidget(responsibility);
    m_reopen_wizard = new QPushButton(tr("Review setup with assistant…"), introduction);
    m_reopen_wizard->setObjectName("paymasterSetupWizard");
    m_reopen_wizard->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    introduction_layout->addWidget(m_reopen_wizard, 0, Qt::AlignLeft);
    AddPaymasterDisclosure(management_layout, management_column, introduction,
                           tr("Review setup with assistant"), tr("Hide setup review"), QStringLiteral("paymasterSetupReviewToggle"));

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
    management_layout->addWidget(access_group);
    management_layout->addWidget(m_overview_backup_notice);
    m_backup_summary = new QLabel(management_column);
    m_backup_summary->setObjectName("paymasterBackupSummary");
    m_backup_summary->setWordWrap(true);
    management_layout->addWidget(m_backup_summary);
    m_backup_adopt = new QPushButton(tr("Review restored provider wallet…"), management_column);
    m_backup_adopt->setObjectName("paymasterAdoptBackup");
    m_backup_adopt->hide();
    management_layout->addWidget(m_backup_adopt, 0, Qt::AlignLeft);
    m_backup_adopt_status = new QLabel(management_column);
    m_backup_adopt_status->setObjectName("paymasterAdoptBackupStatus");
    m_backup_adopt_status->setWordWrap(true);
    m_backup_adopt_status->setTextFormat(Qt::PlainText);
    m_backup_adopt_status->hide();
    management_layout->addWidget(m_backup_adopt_status);
    connect(m_backup_adopt, &QPushButton::clicked, this, [this] { reviewBackupAdoption(); });
    m_backup_notice = new QPushButton(tr("A full-wallet backup is recommended. Review backup…"), overview_column);
    m_backup_notice->setObjectName("paymasterBackupReminder");
    m_backup_notice->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    m_backup_notice->hide();
    overview_layout->addWidget(m_backup_notice);
    connect(m_backup_notice, &QPushButton::clicked, this, [this] { showOperatorPage(m_management_page); });
    m_node_editor_host = new QWidget(connection_column);
    m_node_editor_host->setObjectName("paymasterNodeEditorHost");
    m_node_editor_layout = new QVBoxLayout(m_node_editor_host);
    m_node_editor_layout->setContentsMargins(0, 0, 0, 0);
    connection_layout->addWidget(m_node_editor_host);

    m_external_prerequisites_card = new QGroupBox(
        tr("Local prerequisites"), overview_column);
    m_external_prerequisites_card->setObjectName("paymasterOverviewNextStep");
    m_external_prerequisites_card->setProperty(
        "paymasterRole", QStringLiteral("nextStepCard"));
    m_external_prerequisites_card->setProperty(
        "statusKind", QStringLiteral("waiting"));
    auto* readiness_layout = new QVBoxLayout(m_external_prerequisites_card);
    m_next_step = new QLabel(tr("Checking what is required next…"),
                             m_external_prerequisites_card);
    m_next_step->setObjectName("paymasterNextStep");
    m_next_step->setProperty("paymasterRole", QStringLiteral("statusText"));
    m_next_step->setWordWrap(true);
    QFont next_step_font = m_next_step->font();
    next_step_font.setBold(true);
    m_next_step->setFont(next_step_font);
    m_readiness_summary = new QLabel(m_external_prerequisites_card);
    m_readiness_summary->setObjectName("paymasterReadinessSummary");
    m_readiness_summary->setWordWrap(true);
    readiness_layout->addWidget(m_next_step);
    readiness_layout->addWidget(m_readiness_summary);
    auto* prerequisite_grid = new QGridLayout();
    prerequisite_grid->setColumnStretch(1, 1);
    const auto add_prerequisite = [this, prerequisite_grid](
                                      int row, const QString& title,
                                      QLabel*& value,
                                      const QString& object_name) {
        auto* heading = new QLabel(title, m_external_prerequisites_card);
        heading->setProperty("paymasterRole", QStringLiteral("mutedText"));
        value = new QLabel(m_external_prerequisites_card);
        value->setObjectName(object_name);
        value->setWordWrap(true);
        prerequisite_grid->addWidget(heading, row, 0, Qt::AlignTop);
        prerequisite_grid->addWidget(value, row, 1);
    };
    add_prerequisite(0, tr("Blockchain synchronization"),
                     m_external_blockchain_status,
                     QStringLiteral("paymasterExternalBlockchainStatus"));
    add_prerequisite(1, tr("Transaction index"),
                     m_external_txindex_status,
                     QStringLiteral("paymasterExternalTxIndexStatus"));
    add_prerequisite(2, tr("Transactions ready"),
                     m_external_broadcast_status,
                     QStringLiteral("paymasterExternalBroadcastStatus"));
    add_prerequisite(3, tr("DigiDollar activation"),
                     m_external_activation_status,
                     QStringLiteral("paymasterExternalActivationStatus"));
    add_prerequisite(4, tr("Oracle price"), m_external_oracle_status,
                     QStringLiteral("paymasterExternalOracleStatus"));
    m_external_other_heading = new QLabel(
        tr("Additional provider requirement"),
        m_external_prerequisites_card);
    m_external_other_heading->setProperty(
        "paymasterRole", QStringLiteral("mutedText"));
    m_external_other_status = new QLabel(m_external_prerequisites_card);
    m_external_other_status->setObjectName(
        "paymasterExternalOtherStatus");
    m_external_other_status->setWordWrap(true);
    prerequisite_grid->addWidget(m_external_other_heading, 5, 0,
                                 Qt::AlignTop);
    prerequisite_grid->addWidget(m_external_other_status, 5, 1);
    m_external_other_heading->hide();
    m_external_other_status->hide();
    readiness_layout->addLayout(prerequisite_grid);
    m_external_oracle_note = new QLabel(tr(
        "Informational: Paymaster transfers do not require an Oracle price. "
        "Minting, redemption and DigiDollar system displays remain limited until one is available."),
        m_external_prerequisites_card);
    m_external_oracle_note->setObjectName("paymasterExternalOracleNote");
    m_external_oracle_note->setProperty("paymasterRole", QStringLiteral("mutedText"));
    m_external_oracle_note->setWordWrap(true);
    readiness_layout->addWidget(m_external_oracle_note);
    AddPaymasterDisclosure(network_layout, network_group, m_external_prerequisites_card,
                           tr("Show local prerequisites"), tr("Hide local prerequisites"), QStringLiteral("paymasterPrerequisitesToggle"));

    auto* dashboard = new PaymasterResponsiveCards(overview_column, 3);
    dashboard->setObjectName("paymasterOverviewDashboard");
    auto* offer_card = CreatePaymasterStatusCard(
        dashboard, tr("Offer"), m_overview_offer_status,
        m_overview_offer_action);
    offer_card->setObjectName("paymasterOverviewOfferCard");
    auto* safety_card = CreatePaymasterStatusCard(
        dashboard, tr("Spending limits"), m_overview_safety_status,
        m_overview_safety_action);
    safety_card->setObjectName("paymasterOverviewSafetyCard");
    m_overview_budget_details = new QWidget(safety_card);
    m_overview_budget_details->setObjectName("paymasterOverviewBudgetDetails");
    auto* budget_layout = new QVBoxLayout(m_overview_budget_details);
    budget_layout->setContentsMargins(0, 0, 0, 0);
    budget_layout->setSpacing(14);
    auto* period = new QLabel(tr("Rolling 24 hours · DGB"), m_overview_budget_details);
    period->setWordWrap(true);
    period->setProperty("paymasterRole", QStringLiteral("mutedText"));
    budget_layout->addWidget(period);
    for (const auto& name : {"user_paid", "public_sponsored", "restricted_sponsored", "refill"}) {
        auto* section = new QWidget(m_overview_budget_details);
        section->setObjectName(QStringLiteral("paymasterBudget_") + name);
        auto* rows = new QGridLayout(section);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setHorizontalSpacing(12);
        rows->setVerticalSpacing(4);
        rows->setColumnStretch(0, 1);
        const QString title = std::string(name) == "user_paid" ? tr("Customer payments") :
            std::string(name) == "public_sponsored" ? tr("Public sponsorship") :
            std::string(name) == "restricted_sponsored" ? tr("Restricted sponsorship") : tr("Reserve maintenance");
        auto* heading = new QLabel(title, section);
        heading->setWordWrap(true);
        QFont heading_font = heading->font();
        heading_font.setBold(true);
        heading->setFont(heading_font);
        rows->addWidget(heading, 0, 0, 1, 2);
        int row{1};
        for (const auto& field : {"day_spent", "day_limit", "reserved"}) {
            auto* caption = new QLabel(std::string(field) == "day_spent" ? tr("Spent") :
                std::string(field) == "day_limit" ? tr("Limit") : tr("Reserved budget"), section);
            caption->setWordWrap(true);
            caption->setProperty("paymasterRole", QStringLiteral("mutedText"));
            auto* amount = new QLabel(section);
            amount->setObjectName(QStringLiteral("paymasterBudget_") + name + QLatin1Char('_') + field);
            amount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rows->addWidget(caption, row, 0);
            rows->addWidget(amount, row++, 1);
        }
        budget_layout->addWidget(section);
        section->hide();
    }
    qobject_cast<QVBoxLayout*>(safety_card->layout())->insertWidget(2, m_overview_budget_details);
    m_overview_budget_details->hide();
    auto* liquidity_card = CreatePaymasterStatusCard(
        dashboard, tr("Operating capital"), m_overview_liquidity_status,
        m_overview_liquidity_action);
    liquidity_card->setObjectName("paymasterOverviewLiquidityCard");
    m_overview_capital_details = new QWidget(liquidity_card);
    m_overview_capital_details->setObjectName("paymasterOverviewCapitalDetails");
    auto* capital_layout = new QVBoxLayout(m_overview_capital_details);
    capital_layout->setContentsMargins(0, 0, 0, 0);
    const auto add_metric = [](QVBoxLayout* layout, QWidget* parent, const char* name) {
        auto* value = new QLabel(QObject::tr("Not loaded"), parent);
        value->setObjectName(QStringLiteral("paymasterCapital_") + name);
        value->setWordWrap(true);
        value->setProperty("paymasterRole", QStringLiteral("capitalMetric"));
        layout->addWidget(value);
    };
    const auto add_heading = [](QVBoxLayout* layout, QWidget* parent, const QString& text) {
        auto* heading = new QLabel(text, parent);
        heading->setWordWrap(true);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        layout->addWidget(heading);
    };
    add_heading(capital_layout, m_overview_capital_details, tr("Payment capacity"));
    add_metric(capital_layout, m_overview_capital_details, "complete_slots");
    add_metric(capital_layout, m_overview_capital_details, "refill_target");
    add_metric(capital_layout, m_overview_capital_details, "surplus");
    add_metric(capital_layout, m_overview_capital_details, "maintenance_pending");
    capital_layout->addSpacing(10);
    add_heading(capital_layout, m_overview_capital_details, tr("Network-fee reserve"));
    for (const auto* name : {"dgb_available", "dgb_reserved", "dgb_pending"})
        add_metric(capital_layout, m_overview_capital_details, name);
    capital_layout->addSpacing(10);
    add_heading(capital_layout, m_overview_capital_details, tr("DigiDollar operating reserve"));
    add_metric(capital_layout, m_overview_capital_details, "carrier_base");
    auto* reserve_details = new QWidget(m_overview_capital_details);
    auto* reserve_layout = new QVBoxLayout(reserve_details);
    reserve_layout->setContentsMargins(0, 0, 0, 0);
    for (const auto* name : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"})
        add_metric(reserve_layout, reserve_details, name);
    AddPaymasterDisclosure(capital_layout, m_overview_capital_details, reserve_details,
        tr("Show reserve counts"), tr("Hide reserve counts"), QStringLiteral("paymasterOverviewReserveCountsToggle"));
    m_overview_capital_details->setToolTip(tr("Current pool capital, not the total wallet balance. Ready / saved target is shown separately for each pool. Reserved and pending funds are not immediately available. DD earnings retained in carriers are part of the reserves, not additional capital."));
    qobject_cast<QVBoxLayout*>(liquidity_card->layout())->insertWidget(2, m_overview_capital_details);
    auto* operation_card = CreatePaymasterStatusCard(
        dashboard, tr("Provider operation"), m_overview_operation_status,
        m_overview_operation_action);
    operation_card->setObjectName("paymasterOverviewOperationCard");
    auto* finance_card = CreatePaymasterStatusCard(
        dashboard, tr("Income & costs"), m_overview_finance_status,
        m_overview_finance_action);
    finance_card->setObjectName("paymasterOverviewFinanceCard");
    m_overview_finance_details = new QWidget(finance_card);
    m_overview_finance_details->setObjectName("paymasterOverviewFinanceDetails");
    auto* income_layout = new QVBoxLayout(m_overview_finance_details);
    income_layout->setContentsMargins(0, 0, 0, 0);
    add_heading(income_layout, m_overview_finance_details, tr("Confirmed results · all time"));
    for (const auto* name : {"income", "costs", "transfers"})
        add_metric(income_layout, m_overview_finance_details, name);
    income_layout->addSpacing(10);
    add_heading(income_layout, m_overview_finance_details, tr("Current earnings in reserves"));
    for (const auto* name : {"carrier_earned", "carrier_withdrawable"})
        add_metric(income_layout, m_overview_finance_details, name);
    income_layout->addWidget(m_withdrawal_hint);
    qobject_cast<QVBoxLayout*>(finance_card->layout())->insertWidget(2, m_overview_finance_details);
    m_overview_finance_status->setObjectName("paymasterOverviewFinanceStatus");
    m_overview_liquidity_status->setObjectName(
        "paymasterOverviewLiquidityStatus");
    m_overview_operation_status->setObjectName(
        "paymasterOverviewOperationStatus");
    m_overview_liquidity_action->setObjectName(
        "paymasterOverviewLiquidityAction");
    m_overview_operation_action->setObjectName(
        "paymasterOverviewOperationAction");
    // Settings have one home. Operation presents the next task, not a
    // second set of links into the configuration pages.
    m_overview_liquidity_action->hide();
    m_overview_safety_action->hide();
    offer_card->hide();
    dashboard->addCard(liquidity_card);
    dashboard->addCard(safety_card);
    dashboard->addCard(finance_card);
    operation_card->hide();
    liquidity_card->setToolTip(tr("Prepared funds remain in your wallet. Preparing outputs can incur a network fee; that fee is shown separately in the approval."));
    safety_card->setToolTip(tr("Limits constrain DGB costs. Each payment model and paid refill have separate budgets; reserved amounts are already committed."));
    finance_card->setToolTip(tr("Recorded DD income and DGB costs are different currencies. Operating capital is not an expense."));
    auto* live_cards = new PaymasterResponsiveCards(m_setup_content, 3);
    live_cards->setObjectName("paymasterLiveOverview");
    for (const auto& key : {QStringLiteral("user_paid"), QStringLiteral("sponsored"), QStringLiteral("activity")}) {
        auto* card = new QGroupBox(key == QLatin1String("user_paid") ? tr("User paid") :
            key == QLatin1String("sponsored") ? tr("Sponsored") : tr("Customer activity"), live_cards);
        card->setProperty("paymasterRole", QStringLiteral("card"));
        auto* layout = new QVBoxLayout(card);
        auto* status = new QLabel(tr("Status unavailable"), card);
        status->setObjectName(QStringLiteral("paymasterLive_") + key);
        status->setProperty("paymasterRole", QStringLiteral("statusText"));
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        auto* detail = new QLabel(card);
        detail->setObjectName(QStringLiteral("paymasterLive_") + key + QStringLiteral("_detail"));
        detail->setWordWrap(true);
        detail->setTextFormat(Qt::PlainText);
        m_live_status.insert(key, status);
        m_live_details.insert(key, detail);
        layout->addWidget(status);
        layout->addWidget(detail);
        layout->addStretch();
        auto* review = new QPushButton(key == QLatin1String("activity") ? tr("View individual payments") : tr("Review offer and limits"), card);
        layout->addWidget(review, 0, Qt::AlignLeft);
        connect(review, &QPushButton::clicked, this, [this, key] {
            showOperatorPage(key == QLatin1String("activity") ? m_activity_page : m_configuration_page);
        });
        live_cards->addCard(card);
    }
    configured_layout->addWidget(live_cards);
    configured_layout->addWidget(dashboard);

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

    auto* details_toggle = new QPushButton(tr("Show technical details"), overview);
    details_toggle->setObjectName("paymasterTechnicalDetailsToggle");
    details_toggle->setCheckable(true);
    details_toggle->setFlat(true);
    configured_layout->addWidget(details_toggle, 0, Qt::AlignLeft);

    auto* details = new QWidget(overview);
    details->setObjectName("paymasterTechnicalDetails");
    auto* details_layout = new QVBoxLayout(details);
    details_layout->setContentsMargins(0, 0, 0, 0);
    m_identity = new QLabel(tr("Identity: not configured"), overview);
    m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pool = new QLabel(tr("Liquidity pool: not configured"), overview);
    m_readiness = new QPlainTextEdit(overview);
    m_readiness->setObjectName("paymasterReadinessCodes");
    m_readiness->setReadOnly(true);
    m_readiness->setMaximumHeight(130);
    m_readiness->setPlaceholderText(tr("No technical readiness information available"));
    details_layout->addWidget(m_operator_summary);
    details_layout->addWidget(offer_card);
    details_layout->addWidget(operation_card);
    offer_card->show();
    operation_card->show();
    details_layout->addWidget(m_identity);
    details_layout->addWidget(m_pool);
    details_layout->addWidget(m_readiness);
    details->setVisible(false);
    configured_layout->addWidget(details);

    auto* refresh = new QPushButton(tr("Refresh diagnostics"), details);
    refresh->setObjectName("paymasterOverviewRefresh");
    details_layout->addWidget(refresh, 0, Qt::AlignLeft);
    configured_layout->addStretch();
    overview_layout->addWidget(m_setup_content);
    overview_scroll->setWidget(overview);
    ConfigureScrollArea(overview_scroll, overview);
    m_tabs->addWidget(overview_scroll);

    auto* configuration_scroll = new QScrollArea(m_tabs);
    configuration_scroll->setObjectName("paymasterConfigurationPage");
    configuration_scroll->setWidgetResizable(true);
    auto* configuration = new QWidget(configuration_scroll);
    configuration->setObjectName("paymasterConfigurationContents");
    QVBoxLayout* configuration_layout{nullptr};
    auto* configuration_column = CreatePaymasterPageColumn(
        configuration, configuration_layout);
    AddPaymasterPageHeading(
        configuration_layout, configuration_column, tr("Offer & pricing"),
        tr("Choose who can use your service and the DD fee customers pay you. The DGB network-fee ceiling must fit your customer-payment budgets. Reserve capital and refill costs are configured separately."),
        QStringLiteral("paymasterConfiguration"));

    auto* identity_group = new QGroupBox(tr("Provider identity"), configuration_column);
    identity_group->setProperty("paymasterRole", QStringLiteral("card"));
    auto* identity_form = new QFormLayout(identity_group);
    identity_form->setHorizontalSpacing(18);
    identity_form->setVerticalSpacing(10);
    identity_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* identity_help = new QLabel(tr(
        "The identity lets clients verify that offers come from the same provider. It is stored in this wallet and should normally be created only once."), identity_group);
    identity_help->setWordWrap(true);
    identity_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    m_display_name = new QLineEdit(identity_group);
    m_display_name->setObjectName("paymasterDisplayName");
    m_display_name->setMaxLength(32);
    m_display_name->setValidator(
        new PaymasterDisplayNameValidator(m_display_name));
    m_display_name->setPlaceholderText(tr("Name shown to clients"));
    m_create_identity = new QPushButton(tr("Create provider identity"), identity_group);
    auto* create_identity = m_create_identity;
    create_identity->setObjectName("paymasterCreateIdentity");
    create_identity->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    m_offer_identity_status = new QLabel(
        tr("Action required · no provider identity has been created yet."),
        identity_group);
    m_offer_identity_status->setObjectName("paymasterOfferIdentityStatus");
    m_offer_identity_status->setProperty("paymasterRole", QStringLiteral("statusText"));
    m_offer_identity_status->setWordWrap(true);
    m_offer_identity_id = new QLabel(tr("Not created yet"), identity_group);
    m_offer_identity_id->setObjectName("paymasterOfferIdentityId");
    m_offer_identity_id->setProperty("paymasterRole", QStringLiteral("mutedText"));
    m_offer_identity_id->setWordWrap(true);
    m_offer_identity_id->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_offer_identity_id->setAccessibleName(tr("Full provider identity"));
    identity_form->addRow(identity_help);
    identity_form->addRow(tr("Status:"), m_offer_identity_status);
    auto* identity_details = new QWidget(identity_group);
    auto* identity_details_layout = new QVBoxLayout(identity_details);
    identity_details_layout->setContentsMargins(0, 0, 0, 0);
    AddPaymasterDisclosure(identity_details_layout, identity_details, m_offer_identity_id,
                           tr("Show provider ID"), tr("Hide provider ID"), QStringLiteral("paymasterIdentityDetailsToggle"));
    identity_form->addRow(identity_details);
    identity_form->addRow(tr("Display name:"), m_display_name);
    identity_form->addRow(create_identity);
    configuration_layout->addWidget(identity_group);

    auto* policy_group = new QGroupBox(tr("Services offered"), configuration_column);
    policy_group->setProperty("paymasterRole", QStringLiteral("card"));
    auto* policy_form = new QFormLayout(policy_group);
    policy_form->setHorizontalSpacing(18);
    policy_form->setVerticalSpacing(10);
    policy_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* policy_help = new QLabel(tr(
        "The policy defines which transfers you are willing to support. Clients still verify every quote and transaction locally."), policy_group);
    policy_help->setWordWrap(true);
    policy_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    policy_form->addRow(policy_help);
    m_sponsored = new QCheckBox(tr("Sponsored · provider pays the DGB fee"), policy_group);
    m_sponsored->setObjectName("paymasterPolicySponsored");
    m_sponsored->setProperty("paymasterRole", QStringLiteral("choiceCard"));
    m_user_paid = new QCheckBox(tr("User paid · client pays a DD service fee"), policy_group);
    m_user_paid->setObjectName("paymasterPolicyUserPaid");
    m_user_paid->setProperty("paymasterRole", QStringLiteral("choiceCard"));
    m_user_paid->setChecked(true);
    auto* user_paid_help = new QLabel(tr(
        "The client pays a service fee in DigiDollar; your provider wallet supplies the DGB network fee."), policy_group);
    user_paid_help->setWordWrap(true);
    user_paid_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    auto* sponsored_help = new QLabel(tr(
        "Your provider pays the DGB network fee without charging a DigiDollar service fee. Use strict safety budgets to cap the cost."), policy_group);
    sponsored_help->setWordWrap(true);
    sponsored_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    auto* models = new PaymasterResponsiveCards(policy_group);
    models->setObjectName("paymasterOfferModelCards");
    auto* user_paid_card = new QFrame(models);
    user_paid_card->setObjectName("paymasterUserPaidOfferCard");
    user_paid_card->setProperty("paymasterRole", QStringLiteral("choicePanel"));
    auto* user_paid_layout = new QVBoxLayout(user_paid_card);
    user_paid_layout->setContentsMargins(14, 12, 14, 12);
    user_paid_layout->addWidget(m_user_paid);
    user_paid_layout->addWidget(user_paid_help);
    user_paid_layout->addStretch();
    auto* sponsored_card = new QFrame(models);
    sponsored_card->setObjectName("paymasterSponsoredOfferCard");
    sponsored_card->setProperty("paymasterRole", QStringLiteral("choicePanel"));
    auto* sponsored_layout = new QVBoxLayout(sponsored_card);
    sponsored_layout->setContentsMargins(14, 12, 14, 12);
    sponsored_layout->addWidget(m_sponsored);
    sponsored_layout->addWidget(sponsored_help);
    sponsored_layout->addStretch();
    models->addCard(user_paid_card);
    models->addCard(sponsored_card);
    const auto update_model_card = [](QFrame* card, bool selected) {
        card->setProperty("selected", selected);
        if (card->style()) {
            card->style()->unpolish(card);
            card->style()->polish(card);
        }
    };
    connect(m_user_paid, &QCheckBox::toggled, user_paid_card,
            [user_paid_card, update_model_card](bool checked) {
                update_model_card(user_paid_card, checked);
            });
    connect(m_sponsored, &QCheckBox::toggled, sponsored_card,
            [sponsored_card, update_model_card](bool checked) {
                update_model_card(sponsored_card, checked);
            });
    update_model_card(user_paid_card, m_user_paid->isChecked());
    update_model_card(sponsored_card, m_sponsored->isChecked());
    m_scope = new NoWheelComboBox(policy_group);
    m_scope->setObjectName("paymasterPolicySponsorshipScope");
    m_scope->addItem(tr("Public — available to discovered clients"), QStringLiteral("public"));
    m_scope->setToolTip(tr(
        "Public sponsorship can be offered together with user-paid service."));
    m_fee_bps = scaledSpin(policy_group, 0, 10000, 50, 100, 2);
    m_fee_bps->setObjectName("paymasterPolicyFeeBps");
    m_fee_bps->setSingleStep(10);
    m_fee_bps->setSuffix(tr(" %"));
    m_fee_bps->setToolTip(tr(
        "Service fee for user-paid transfers. 100 basis points equal 1%. Sponsored transfers always charge zero DigiDollar service fee."));
    m_maximum_user_paid_service_fee = scaledSpin(
        policy_group, 0, 10000000, 0, 100, 2);
    m_maximum_user_paid_service_fee->setObjectName(
        "paymasterPolicyMaximumUserPaidServiceFeeCents");
    m_maximum_user_paid_service_fee->setSuffix(tr(" DD"));
    m_maximum_user_paid_service_fee->setToolTip(tr(
        "Maximum DD service fee on one user-paid transfer. Enter 0 for no cap."));
    m_min_amount = scaledSpin(policy_group, 100, 10000000, 100, 100, 2);
    m_min_amount->setObjectName("paymasterPolicyMinimumCents");
    m_min_amount->setSuffix(tr(" DD"));
    m_min_amount->setToolTip(tr("Smallest DigiDollar payment this provider will accept."));
    m_max_amount = scaledSpin(policy_group, 100, 10000000, 100000, 100, 2);
    m_max_amount->setObjectName("paymasterPolicyMaximumCents");
    m_max_amount->setSuffix(tr(" DD"));
    m_max_amount->setToolTip(tr("Largest DigiDollar payment this provider will accept."));
    m_quote_ttl = spin(policy_group, 1, 60, 60);
    m_quote_ttl->setObjectName("paymasterPolicyQuoteLifetime");
    m_quote_ttl->setSuffix(tr(" seconds"));
    m_quote_ttl->setToolTip(tr("How long a client may accept a quote before it expires."));
    m_network_fee = scaledSpin(policy_group, 1, 2000000000, 20000000,
                               100000000, 8);
    m_network_fee->setObjectName("paymasterPolicyMaximumNetworkFee");
    m_network_fee->setSuffix(tr(" DGB"));
    m_network_fee->setToolTip(tr(
        "Absolute DGB network-fee ceiling for one Paymaster transaction. Guided setup keeps the active per-transaction safety value aligned with this ceiling and uses the other safety fields for aggregate spam protection."));
    policy_form->addRow(tr("Payment models:"), models);
    m_funding_model_status = new QLabel(policy_group);
    m_funding_model_status->setObjectName("paymasterFundingModelExplanation");
    m_funding_model_status->setWordWrap(true);
    policy_form->addRow(QString(), m_funding_model_status);
    m_sponsorship_label = new QLabel(tr("Who may use sponsorship:"), policy_group);
    m_sponsorship_label->setBuddy(m_scope);
    policy_form->addRow(m_sponsorship_label, m_scope);
    m_user_paid_pricing = new QWidget(policy_group);
    m_user_paid_pricing->setObjectName("paymasterUserPaidPricing");
    auto* pricing_form = new QFormLayout(m_user_paid_pricing);
    pricing_form->setContentsMargins(0, 0, 0, 0);
    pricing_form->addRow(tr("User-paid service fee:"), m_fee_bps);
    pricing_form->addRow(tr("Maximum user-paid service fee:"), m_maximum_user_paid_service_fee);
    m_fee_example = new PaymasterFeeExample(m_user_paid_pricing);
    m_fee_example->addToForm(pricing_form);
    policy_form->addRow(m_user_paid_pricing);
    policy_form->addRow(tr("Smallest payment:"), m_min_amount);
    policy_form->addRow(tr("Largest payment:"), m_max_amount);
    auto* offer_details_container = new QWidget(policy_group);
    auto* offer_details_layout = new QVBoxLayout(offer_details_container);
    offer_details_layout->setContentsMargins(0, 0, 0, 0);
    auto* offer_details = new QWidget(offer_details_container);
    auto* offer_details_form = new QFormLayout(offer_details);
    offer_details_form->addRow(tr("Quote validity:"), m_quote_ttl);
    offer_details_form->addRow(tr("Network-fee ceiling per transfer:"), m_network_fee);
    auto* limits_help = new QLabel(tr(
        "The payment range filters requests before a quote is created. Quote validity limits how long resources remain offered to one client. The network-fee ceiling is the absolute per-transfer guard; guided safety profiles keep their per-transfer value aligned with it and limit repeated requests through aggregate budgets and counters."), policy_group);
    limits_help->setObjectName("paymasterPolicyLimitsExplanation");
    limits_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    limits_help->setWordWrap(true);
    offer_details_form->addRow(limits_help);
    AddPaymasterDisclosure(offer_details_layout, offer_details_container, offer_details,
                           tr("Quote duration and network-fee ceiling"), tr("Hide technical offer limits"), QStringLiteral("paymasterOfferDetailsToggle"));
    policy_form->addRow(offer_details_container);
    m_offer_forms = {identity_form, policy_form, pricing_form, offer_details_form};
    for (QFormLayout* form : m_offer_forms) {
        form->setHorizontalSpacing(18);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    }
    AlignPaymasterFormLabels(m_offer_forms);
    m_policy_summary = new QLabel(policy_group);
    m_policy_summary->setObjectName("paymasterPolicySummary");
    m_policy_summary->setProperty("paymasterRole", QStringLiteral("summaryText"));
    m_policy_summary->setWordWrap(true);
    policy_form->addRow(tr("Policy summary:"), m_policy_summary);
    policy_form->addRow(configurationWarningPanel(m_policy_config_warning, "paymasterPolicyConfigWarning",
        tr("Open spending limits"), [this] { showOperatorPage(m_safety_page); }));
    m_policy_save_status = new QLabel(policy_group);
    m_policy_save_status->setObjectName("paymasterPolicySaveStatus");
    m_policy_save_status->setProperty("paymasterRole", QStringLiteral("statusText"));
    m_policy_save_status->setWordWrap(true);
    m_policy_save_status->hide();
    policy_form->addRow(QString{}, m_policy_save_status);
    m_review_policy_limits = new QPushButton(tr("Prepare and review spending limits…"), policy_group);
    m_review_policy_limits->setObjectName("paymasterReviewPolicyLimits");
    m_review_policy_limits->hide();
    policy_form->addRow(QString{}, m_review_policy_limits);
    connect(m_review_policy_limits, &QPushButton::clicked, this, [this] { reviewPolicySpendingLimits(); });
    m_save_policy = new QPushButton(tr("Save policy"), policy_group);
    m_save_policy->setObjectName("savePaymasterPolicy");
    m_save_policy->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    m_restore_policy_defaults = new QPushButton(
        tr("Restore recommended defaults"), policy_group);
    m_restore_policy_defaults->setObjectName("paymasterRestorePolicyDefaults");
    m_restore_policy_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    m_restore_policy_defaults->setToolTip(tr(
        "Reset only the unsaved policy form. This does not change the provider identity, safety limits or liquidity."));
    auto* policy_buttons = new QHBoxLayout();
    policy_buttons->addWidget(m_restore_policy_defaults);
    policy_buttons->addStretch();
    auto* discard_offer = new QPushButton(tr("Discard offer changes"), policy_group);
    discard_offer->setObjectName("paymasterDiscardOffer");
    policy_buttons->addWidget(discard_offer);
    connect(discard_offer, &QPushButton::clicked, this, [this] {
        if (!m_busy && m_operator_snapshot.find_value("provider").find_value("policy").isObject())
            loadPolicy(m_operator_snapshot.find_value("provider").find_value("policy"));
    });
    policy_buttons->addWidget(m_save_policy);
    policy_form->addRow(policy_buttons);
    m_enable_status = new QLabel(
        tr("Provider configuration is currently disabled for this wallet."),
        policy_group);
    m_enable_status->setObjectName("paymasterEnableStatus");
    m_enable_status->setWordWrap(true);
    details_layout->addWidget(m_enable_status);
    configuration_layout->addWidget(policy_group);
    configuration_layout->addStretch();
    configuration_scroll->setWidget(configuration);
    ConfigureScrollArea(configuration_scroll, configuration);
    m_configuration_page = configuration_scroll;
    // Offer is grouped with the other settings after all pages exist.

    auto* safety_scroll = new QScrollArea(m_tabs);
    safety_scroll->setObjectName("paymasterSafetyPolicyPage");
    safety_scroll->setWidgetResizable(true);
    auto* safety = new QWidget(safety_scroll);
    safety->setObjectName("paymasterSafetyPolicyContents");
    QVBoxLayout* safety_layout{nullptr};
    auto* safety_column = CreatePaymasterPageColumn(safety, safety_layout);
    AddPaymasterPageHeading(
        safety_layout, safety_column, tr("Customer-payment budgets"),
        tr("Limit the DGB network fees paid for customers, separately for each payment model. User-paid customers compensate you in DD; your wallet still pays the network fee in DGB."),
        QStringLiteral("paymasterSafetyPolicy"));
    auto* safety_warning = new QLabel(tr(
        "These budgets cover customer payments only. Reserve capital and refill fees are separate. Limits are ceilings, not spending targets; each payment model and refill has its own budget, not one shared wallet limit."), safety_column);
    safety_warning->setObjectName("paymasterSafetyPolicyWarning");
    safety_warning->setProperty("paymasterRole", QStringLiteral("notice"));
    safety_warning->setWordWrap(true);
    safety_layout->addWidget(safety_warning);
    auto* safety_instructions = new QLabel(tr(
        "Configure the models you actually offer, then save the provider safety policy. All six limits for an active model must be finite and greater than zero. Setting all six values to zero safely disables an unused model; zero never means unlimited. Start with the recommended values and lower them if the potential DGB cost is too high."), safety);
    safety_instructions->setObjectName("paymasterSafetyPolicyInstructions");
    safety_instructions->setWordWrap(true);
    safety_layout->addWidget(safety_instructions);
    safety_layout->addWidget(configurationWarningPanel(m_safety_config_warning, "paymasterSafetyConfigWarning",
        tr("Open offer settings"), [this] { showOperatorPage(m_configuration_page); }));

    m_policy_limits_review = new QWidget(safety_column);
    auto* limits_review_layout = new QVBoxLayout(m_policy_limits_review);
    limits_review_layout->setContentsMargins(0, 0, 0, 0);
    m_policy_limits_hint = new QLabel(m_policy_limits_review);
    m_policy_limits_hint->setObjectName("paymasterPolicyLimitsHint");
    m_policy_limits_hint->setProperty("paymasterRole", QStringLiteral("statusText"));
    m_policy_limits_hint->setWordWrap(true);
    limits_review_layout->addWidget(m_policy_limits_hint);
    auto* return_to_offer = new QPushButton(tr("Return to offer settings"), m_policy_limits_review);
    return_to_offer->setObjectName("paymasterReturnToOffer");
    limits_review_layout->addWidget(return_to_offer, 0, Qt::AlignLeft);
    connect(return_to_offer, &QPushButton::clicked, this, [this] { showOperatorPage(m_configuration_page); });
    m_policy_limits_review->hide();
    safety_layout->addWidget(m_policy_limits_review);

    auto* provider_safety = new QGroupBox(tr("Budgets by payment model"), safety_column);
    provider_safety->setProperty("paymasterRole", QStringLiteral("card"));
    auto* provider_safety_layout = new QVBoxLayout(provider_safety);
    m_provider_safety_status = new QLabel(
        tr("Provider safety policy: not configured (provider not ready)"),
        provider_safety);
    m_provider_safety_status->setObjectName("paymasterProviderSafetyStatus");
    m_provider_safety_status->setWordWrap(true);
    provider_safety_layout->addWidget(m_provider_safety_status);
    auto* provider_limits_help = new QLabel(tr(
        "Each tab controls one payment model. For an active model, match the per-payment limit to the offer's network-fee ceiling. Hourly and daily limits cap cumulative costs; additional limits protect simultaneous quotes and transaction counts."), provider_safety);
    provider_limits_help->setObjectName("paymasterProviderSafetyExplanation");
    provider_limits_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    provider_limits_help->setWordWrap(true);
    provider_safety_layout->addWidget(provider_limits_help);
    auto* funding_tabs = m_funding_safety_tabs = new QTabWidget(provider_safety);
    funding_tabs->setObjectName("paymasterFundingSafetyModels");
    m_user_paid_safety = createFundingSafetyControls(
        funding_tabs, tr("User paid"), QStringLiteral("paymasterSafetyUserPaid"),
        /*recommended_defaults=*/true);
    m_public_sponsored_safety = createFundingSafetyControls(
        funding_tabs, tr("Public sponsored"), QStringLiteral("paymasterSafetyPublicSponsored"),
        /*recommended_defaults=*/false);
    m_restricted_sponsored_safety = createFundingSafetyControls(
        funding_tabs, tr("Restricted sponsored"), QStringLiteral("paymasterSafetyRestrictedSponsored"),
        /*recommended_defaults=*/false);
    // Preserve stored limits in readback/save payloads, but do not offer
    // configuration for the parked sponsorship mode.
    funding_tabs->setTabVisible(2, false);
    provider_safety_layout->addWidget(funding_tabs);

    auto* quote_limits = new QGroupBox(tr("Quote and request limits"), provider_safety);
    auto* quote_form = new QFormLayout(quote_limits);
    auto* quote_help = new QLabel(tr(
        "These limits reduce resource exhaustion. Total limits protect the wallet, netgroup limits restrict groups of related network addresses, and recipient buckets stop repeated quotes aimed at the same destination. Reaching a limit rejects new work but does not invalidate an already authorized transfer."), quote_limits);
    quote_help->setObjectName("paymasterQuoteLimitsExplanation");
    quote_help->setWordWrap(true);
    quote_form->addRow(quote_help);
    m_max_active_quotes_total = spin(quote_limits, 1, 8192, 16);
    m_max_active_quotes_total->setObjectName("paymasterSafetyMaxActiveQuotesTotal");
    m_max_active_quotes_per_netgroup = spin(quote_limits, 1, 8192, 4);
    m_max_active_quotes_per_netgroup->setObjectName("paymasterSafetyMaxActiveQuotesPerNetgroup");
    m_max_active_quotes_per_recipient = spin(quote_limits, 1, 8192, 2);
    m_max_active_quotes_per_recipient->setObjectName("paymasterSafetyMaxActiveQuotesPerRecipient");
    m_max_quote_requests_per_netgroup = spin(quote_limits, 1, 8192, 10);
    m_max_quote_requests_per_netgroup->setObjectName("paymasterSafetyMaxQuoteRequestsPerNetgroupMinute");
    quote_form->addRow(tr("Active quotes (total):"), m_max_active_quotes_total);
    quote_form->addRow(tr("Active quotes per netgroup:"), m_max_active_quotes_per_netgroup);
    quote_form->addRow(tr("Active quotes per recipient bucket:"), m_max_active_quotes_per_recipient);
    quote_form->addRow(tr("Quote requests per netgroup/minute:"), m_max_quote_requests_per_netgroup);
    auto* restore_quote_defaults = new QPushButton(
        tr("Restore recommended quote limits"), quote_limits);
    restore_quote_defaults->setObjectName("paymasterRestoreQuoteSafetyDefaults");
    restore_quote_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    quote_form->addRow(restore_quote_defaults);
    // Technical anti-abuse limits are available through the advanced
    // disclosure below; the normal view stays focused on financial risk.

    m_provider_safety_group = provider_safety;
    m_save_provider_safety = new QPushButton(tr("Save customer-payment budgets"), provider_safety);
    m_save_provider_safety->setObjectName("savePaymasterProviderSafetyPolicy");
    m_save_provider_safety->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    provider_safety_layout->addWidget(m_save_provider_safety);
    auto* usage_group = new QGroupBox(tr("Current budget usage"), provider_safety);
    usage_group->setProperty("paymasterRole", QStringLiteral("technicalCard"));
    auto* usage_layout = new QVBoxLayout(usage_group);
    auto* usage_help = new QLabel(tr(
        "This read-only status shows what is currently reserved or spent. Saving new limits does not reset existing accounting."), usage_group);
    usage_help->setWordWrap(true);
    usage_layout->addWidget(usage_help);
    m_provider_safety_usage = new QLabel(usage_group);
    m_provider_safety_usage->setObjectName("paymasterProviderSafetyUsage");
    m_provider_safety_usage->setWordWrap(true);
    m_provider_safety_usage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_provider_safety_usage->setText(tr("Budget usage is not available yet."));
    usage_layout->addWidget(m_provider_safety_usage);
    safety_layout->addWidget(provider_safety);

    auto* advanced_safety = new QWidget(safety_column);
    advanced_safety->setObjectName("paymasterAdvancedSafetyPanel");
    auto* advanced_safety_layout = new QVBoxLayout(advanced_safety);
    advanced_safety_layout->setContentsMargins(0, 0, 0, 0);
    advanced_safety_layout->addWidget(safety_instructions);
    advanced_safety_layout->addWidget(usage_group);
    advanced_safety_layout->addWidget(quote_limits);
    AddPaymasterDisclosure(
        safety_layout, safety_column, advanced_safety,
        tr("Show advanced protection limits"),
        tr("Hide advanced protection limits"),
        QStringLiteral("paymasterAdvancedSafetyToggle"));

    auto* client_safety = new QGroupBox(tr("Client service-fee limits"), safety);
    m_client_safety_group = client_safety;
    client_safety->setObjectName("paymasterClientSafetyGroup");
    auto* client_safety_form = new QFormLayout(client_safety);
    auto* client_safety_help = new QLabel(tr(
        "These limits protect this wallet when it uses another Paymaster. They cap the DigiDollar service fee for one transfer and across a rolling day. The effective per-transfer limit is always the lowest applicable wallet, request and signed-offer limit."), client_safety);
    client_safety_help->setObjectName("paymasterClientSafetyExplanation");
    client_safety_help->setWordWrap(true);
    client_safety_form->addRow(client_safety_help);
    m_client_fee_per_transaction = spin(client_safety, 0, 10000000, 100);
    m_client_fee_per_transaction->setObjectName("paymasterClientSafetyMaxFeePerTransaction");
    m_client_fee_per_day = spin(client_safety, 0, 10000000, 1000);
    m_client_fee_per_day->setObjectName("paymasterClientSafetyMaxFeePerDay");
    m_client_safety_status = new QLabel(
        tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"),
        client_safety);
    m_client_safety_status->setObjectName("paymasterClientSafetyStatus");
    m_client_safety_status->setWordWrap(true);
    m_client_safety_mode = new QLabel(client_safety);
    m_client_safety_mode->setObjectName("paymasterClientSafetyMode");
    m_client_safety_mode->setWordWrap(true);
    m_save_client_safety = new QPushButton(tr("Save client safety policy"), client_safety);
    m_save_client_safety->setObjectName("savePaymasterClientSafetyPolicy");
    auto* restore_client_defaults = new QPushButton(
        tr("Restore recommended client limits"), client_safety);
    restore_client_defaults->setObjectName("paymasterRestoreClientSafetyDefaults");
    client_safety_form->addRow(tr("Maximum service fee per transfer (cents):"), m_client_fee_per_transaction);
    client_safety_form->addRow(tr("Maximum service fees per rolling day (cents):"), m_client_fee_per_day);
    client_safety_form->addRow(m_client_safety_mode);
    client_safety_form->addRow(m_client_safety_status);
    auto* client_buttons = new QHBoxLayout();
    client_buttons->addWidget(restore_client_defaults);
    client_buttons->addStretch();
    client_buttons->addWidget(m_save_client_safety);
    client_safety_form->addRow(client_buttons);
    // Client-side DD fee protection belongs to Send $DD. Keep these
    // widgets alive for compatibility with the existing wallet status
    // refresh path, but never expose them in the provider console.
    client_safety->setVisible(false);
    auto* discard_safety = new QPushButton(tr("Discard limit changes"), safety_column);
    discard_safety->setObjectName("paymasterDiscardSafety");
    safety_layout->addWidget(discard_safety, 0, Qt::AlignLeft);
    connect(discard_safety, &QPushButton::clicked, this, [this] {
        if (!m_busy && m_operator_snapshot.find_value("safety").isObject()) {
            m_provider_safety_dirty = false;
            applyProviderSafetyStatus(m_operator_snapshot.find_value("safety"));
            if (!m_policy_limits_review->isHidden()) {
                setStatusLabel(m_policy_limits_hint, tr("Limit edits discarded. The saved limits remain active; your offer edits are still unsaved."), QStringLiteral("action"));
            }
        }
    });
    safety_layout->addStretch();
    safety_scroll->setWidget(safety);
    ConfigureScrollArea(safety_scroll, safety);
    m_safety_page = safety_scroll;
    // Spending limits are a settings subpage.

    // Shared typography for the operator's funds and finance pages.
    const auto finance_label = [](const QString& text, QWidget* parent, const char* role) {
        auto* label = new QLabel(text, parent);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setProperty("paymasterRole", QString::fromLatin1(role));
        return label;
    };
    auto* liquidity_scroll = new QScrollArea(m_tabs);
    liquidity_scroll->setObjectName("paymasterLiquidityPage");
    liquidity_scroll->setWidgetResizable(true);
    auto* liquidity = new QWidget(liquidity_scroll);
    liquidity->setObjectName("paymasterLiquidityContents");
    QVBoxLayout* liquidity_layout{nullptr};
    auto* liquidity_column = CreatePaymasterPageColumn(liquidity, liquidity_layout);
    AddPaymasterPageHeading(
        liquidity_layout, liquidity_column, tr("Reserves & refill"),
        tr("Keep payment capacity ready. DGB and DD reserves remain your wallet's capital; creating or refilling them can cost a separate DGB network fee. Customer-payment fees use their own budgets."),
        QStringLiteral("paymasterLiquidity"));

    const auto funds_value = [&](const QString& key, QWidget* parent, const QString& caption, const char* role = "financeValue") {
        auto* value = finance_label(QStringLiteral("—"), parent, role);
        value->setObjectName(QStringLiteral("paymasterFunds_") + key);
        value->setAccessibleName(caption);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_funds_values.insert(key, value);
        return value;
    };
    auto* funds_metrics = new PaymasterResponsiveCards(liquidity_column, 3);
    funds_metrics->setObjectName("paymasterFundsMetrics");
    const QStringList funds_titles{tr("Available payment reserves"), tr("Available DGB reserves"), tr("DD reserve capital")};
    const QStringList funds_keys{QStringLiteral("capacity"), QStringLiteral("dgb_available"), QStringLiteral("carrier_base")};
    const QStringList funds_help{tr("Prepared capacity; accepting a payment also requires a running provider and sufficient budgets."),
        tr("Wallet-owned DGB prepared for network fees. This is capital, not spending."),
        tr("Reusable base capital for receiving service fees. Earnings are shown separately below.")};
    for (int i = 0; i < funds_keys.size(); ++i) {
        auto* card = new QFrame(funds_metrics);
        card->setProperty("paymasterRole", QStringLiteral("statusCard"));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->addWidget(finance_label(funds_titles.at(i), card, "financeRowTitle"));
        layout->addWidget(funds_value(funds_keys.at(i), card, funds_titles.at(i), "financeAmount"));
        if (i == 0) layout->addWidget(funds_value("target", card, tr("Saved refill target"), "mutedText"));
        if (i == 1) {
            layout->addWidget(funds_value("dgb_reserved", card, tr("DGB reserves in use"), "mutedText"));
            layout->addWidget(funds_value("dgb_pending", card, tr("DGB reserves awaiting confirmation"), "mutedText"));
        }
        layout->addWidget(finance_label(funds_help.at(i), card, "mutedText"));
        layout->addStretch();
        funds_metrics->addCard(card);
    }
    liquidity_layout->addWidget(funds_metrics);

    m_pool_preparation_status = new QLabel(liquidity_column);
    m_pool_preparation_status->setObjectName("paymasterPoolPreparationStatus");
    m_pool_preparation_status->setTextFormat(Qt::PlainText);
    m_pool_preparation_status->setWordWrap(true);
    m_pool_preparation_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pool_preparation_status->hide();
    liquidity_layout->addWidget(m_pool_preparation_status);
    m_cancel_preparation = new QPushButton(tr("Cancel uncreated pool preparation…"), liquidity_column);
    m_cancel_preparation->setObjectName("paymasterCancelPoolPreparation");
    m_cancel_preparation->hide();
    task_layout->addWidget(m_cancel_preparation);
    connect(m_cancel_preparation, &QPushButton::clicked, this, [this] { cancelPoolPreparation(); });

    auto* capacity_details = new QWidget(liquidity_column);
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
        auto* heading = finance_label(count_headers.at(column), counts, "financeColumnHeading");
        heading->setAlignment((column ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        counts_grid->addWidget(heading, 0, column);
        counts_grid->setColumnStretch(column, column ? 1 : 3);
    }
    for (int row = 0; row < count_rows.size(); ++row) {
        counts_grid->addWidget(finance_label(count_rows.at(row), counts, "financeRowTitle"), row + 1, 0);
        for (int column = 0; column < count_fields.size(); ++column) {
            auto* value = funds_value(count_keys.at(row) + QLatin1Char('_') + count_fields.at(column), counts,
                count_rows.at(row) + QStringLiteral(" · ") + count_headers.at(column + 1));
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            counts_grid->addWidget(value, row + 1, column + 1);
        }
    }
    capacity_details_layout->addWidget(counts);

    auto* capacity_explanation = new QLabel(tr(
        "Why two counts? Before making an offer, Core proves to a client that this provider has dedicated funds. Separate payment capacity is then kept ready for accepted transfers. “Ready” is available now; “target” is the amount Core maintains. These funds always remain under this wallet's control."),
        liquidity_column);
    capacity_explanation->setObjectName("paymasterLiquidityCapacityExplanation");
    capacity_explanation->setProperty("paymasterRole", QStringLiteral("mutedText"));
    capacity_explanation->setWordWrap(true);
    capacity_details_layout->addWidget(capacity_explanation);
    AddPaymasterDisclosure(liquidity_layout, liquidity_column, capacity_details,
                           tr("View reserve counts and targets"), tr("Hide reserve counts and targets"), QStringLiteral("paymasterCapacityDetailsToggle"));
    auto* liquidity_steps = new QLabel(tr(
        "Choose an action above. You will review the exact amounts and fees before anything changes. Confirmed reserves become available automatically. Individual reserve types and manual controls are under Advanced."), liquidity);
    liquidity_steps->setObjectName("paymasterLiquiditySetupSteps");
    liquidity_steps->setWordWrap(true);
    liquidity_layout->addWidget(liquidity_steps);

    auto* automatic_maintenance = new QGroupBox(
        tr("Reserve target and refill costs"), liquidity_column);
    automatic_maintenance->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    automatic_maintenance->setProperty("paymasterRole", QStringLiteral("card"));
    automatic_maintenance->setObjectName(
        "paymasterAutomaticLiquidityPolicy");
    auto* automatic_maintenance_layout = new QFormLayout(
        automatic_maintenance);
    auto* automatic_maintenance_help = new QLabel(tr(
        "Choose how many complete payment reserves to keep ready, then review the required capital and separate refill-fee limits. Editing this form does not change saved settings until you save."),
        automatic_maintenance);
    automatic_maintenance_help->setWordWrap(true);
    automatic_maintenance_layout->addRow(automatic_maintenance_help);
    m_automatic_replenishment = new QCheckBox(
        tr("Automatically refill missing reserves while this provider is running"),
        automatic_maintenance);
    m_automatic_replenishment->setObjectName(
        "paymasterAutomaticReplenishment");
    m_automatic_replenishment->setChecked(true);
    m_paid_maintenance_approved = new QCheckBox(tr(
        "Approve refill fees within the limits below"),
        automatic_maintenance);
    m_paid_maintenance_approved->setObjectName(
        "paymasterPaidMaintenanceApproved");
    m_paid_maintenance_approved->setToolTip(tr("Only the actual network fees of reserve refill are paid. These limits are additional to customer-payment budgets. Fragmented funds can require a higher transaction limit; Core never raises it automatically."));
    const auto refill_defaults = DigiDollar::Paymaster::SetupDefaultLiquidity(true);
    m_maintenance_fee_per_transaction = new DgbAmountLineEdit(0, automatic_maintenance);
    m_maintenance_fee_per_transaction->setSatoshis(refill_defaults.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>());
    m_maintenance_fee_per_transaction->setObjectName(
        "paymasterMaintenanceFeePerTransaction");
    m_maintenance_fee_per_hour = new DgbAmountLineEdit(0, automatic_maintenance);
    m_maintenance_fee_per_hour->setSatoshis(refill_defaults.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>());
    m_maintenance_fee_per_hour->setObjectName(
        "paymasterMaintenanceFeePerHour");
    m_maintenance_fee_per_day = new DgbAmountLineEdit(0, automatic_maintenance);
    m_maintenance_fee_per_day->setSatoshis(refill_defaults.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>());
    m_maintenance_fee_per_day->setObjectName(
        "paymasterMaintenanceFeePerDay");
    m_liquidity_policy_status = new QLabel(tr(
        "Liquidity policy has not been loaded yet."), automatic_maintenance);
    m_liquidity_policy_status->setObjectName(
        "paymasterLiquidityPolicyStatus");
    m_liquidity_policy_status->setWordWrap(true);
    m_liquidity_policy_status->setTextFormat(Qt::PlainText);
    automatic_maintenance_layout->addRow(m_automatic_replenishment);
    automatic_maintenance_layout->addRow(m_liquidity_policy_status);
    m_maintenance_limits_toggle = new QPushButton(
        tr("Set optional paid-refill limits"), automatic_maintenance);
    m_maintenance_limits_toggle->setObjectName(
        "paymasterMaintenanceLimitsToggle");
    m_maintenance_limits_toggle->setProperty(
        "paymasterRole", QStringLiteral("disclosure"));
    m_maintenance_limits_toggle->setCheckable(true);
    auto* maintenance_limits_panel = new QWidget(automatic_maintenance);
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
        tr("Save liquidity settings"), automatic_maintenance);
    m_save_liquidity_policy_primary->setObjectName(
        "paymasterSaveLiquidityPolicyPrimary");
    m_save_liquidity_policy_primary->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    m_save_liquidity_policy_primary->setToolTip(tr(
        "Save the automatic-refill setting, finite maintenance limits and displayed pool targets. A running provider may immediately refill within the approved limits; saving does not start a stopped provider."));
    automatic_maintenance_layout->addRow(m_save_liquidity_policy_primary);
    auto* discard_liquidity = new QPushButton(tr("Discard reserve changes"), automatic_maintenance);
    discard_liquidity->setObjectName("paymasterDiscardLiquidity");
    automatic_maintenance_layout->addRow(discard_liquidity);
    connect(discard_liquidity, &QPushButton::clicked, this, [this] {
        if (!m_busy && IsCompleteLiquidityPolicy(m_saved_liquidity_policy))
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
    liquidity_layout->addWidget(automatic_maintenance);

    auto* targets = new QGroupBox(tr("Reserve target"), liquidity);
    auto* targets_layout = new QVBoxLayout(targets);
    m_liquidity_current_status = new QLabel(
        tr("Current confirmed pool: status not loaded yet."), targets);
    m_liquidity_current_status->setObjectName("paymasterLiquidityCurrentStatus");
    m_liquidity_current_status->setWordWrap(true);
    m_admission_dgb = spin(liquidity, 3, 16, 3);
    m_admission_dgb->setObjectName("paymasterAdmissionDgbSlots");
    m_operational_dgb = spin(liquidity, 1, 16, 1);
    m_operational_dgb->setObjectName("paymasterOperationalDgbSlots");
    m_admission_carriers = spin(liquidity, 0, 16, 3);
    m_admission_carriers->setObjectName("paymasterAdmissionCarrierSlots");
    m_operational_carriers = spin(liquidity, 0, 16, 1);
    m_operational_carriers->setObjectName("paymasterOperationalCarrierSlots");
    m_admission_dgb->setToolTip(tr("At least three confirmed admission DGB slots are required."));
    m_operational_dgb->setToolTip(tr("Each operational DGB slot can support one concurrent transfer."));
    m_admission_carriers->setToolTip(tr("User-paid providers require at least three admission DD carriers; sponsored-only providers need none."));
    m_operational_carriers->setToolTip(tr("User-paid transfers require an operational DD carrier paired with operational DGB liquidity."));
    m_reserve_editor = new PaymasterReserveEditor(targets, QStringLiteral("paymaster"),
        {m_admission_dgb, m_operational_dgb, m_admission_carriers, m_operational_carriers},
        {m_maintenance_fee_per_transaction, m_maintenance_fee_per_hour, m_maintenance_fee_per_day},
        [this] { return confirmedUserPaid(); }, [this] { return confirmedPaymentFee(); });
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
    connect(m_release_excess_reserves, &QPushButton::clicked, this, [this] { beginGuidedTask("reduce_reserves"); });
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
    liquidity_layout->addWidget(targets);

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
    liquidity_layout->addWidget(carrier_management);

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
    connect(m_preparation_fee, &QLineEdit::textChanged, this, [this] {
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
    liquidity_layout->addWidget(preparation);

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
    liquidity_layout->addWidget(retirement);

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
    liquidity_layout->addWidget(result_group);

    auto* advanced_liquidity = new QWidget(liquidity_column);
    advanced_liquidity->setObjectName("paymasterAdvancedLiquidityPanel");
    auto* advanced_liquidity_layout = new QVBoxLayout(advanced_liquidity);
    advanced_liquidity_layout->setContentsMargins(0, 0, 0, 0);
    advanced_liquidity_layout->setSpacing(12);
    advanced_liquidity_layout->addWidget(maintenance_status_group);
    advanced_liquidity_layout->addWidget(liquidity_steps);
    automatic_maintenance_layout->insertRow(1, targets);
    m_save_liquidity_policy->hide();
    m_automatic_replenishment->hide();
    carrier_management->hide();
    preparation->hide();
    auto* manual_costs = new QFormLayout;
    manual_costs->addRow(tr("Maximum fee for a manual reserve transaction (DGB):"), m_preparation_fee);
    advanced_liquidity_layout->addLayout(manual_costs);
    retirement->hide();
    advanced_liquidity_layout->addWidget(result_group);
    m_liquidity_advanced_toggle = AddPaymasterDisclosure(
        liquidity_layout, liquidity_column, advanced_liquidity,
        tr("Advanced reserve management"),
        tr("Hide advanced reserve management"),
        QStringLiteral("paymasterAdvancedLiquidityToggle"));
    liquidity_layout->addStretch();
    liquidity_scroll->setWidget(liquidity);
    ConfigureScrollArea(liquidity_scroll, liquidity);
    m_liquidity_page = liquidity_scroll;
    liquidity_layout->insertWidget(liquidity_layout->indexOf(automatic_maintenance), m_liquidity_maintenance_card);
    // Reserve configuration joins Settings once the other pages exist.

    auto* finance_scroll = new QScrollArea(m_tabs);
    finance_scroll->setObjectName("paymasterFinancesPage");
    finance_scroll->setWidgetResizable(true);
    auto* finance = new QWidget(finance_scroll);
    finance->setObjectName("paymasterFinancesContents");
    QVBoxLayout* finance_layout{nullptr};
    auto* finance_column = CreatePaymasterPageColumn(finance, finance_layout);
    AddPaymasterPageHeading(
        finance_layout, finance_column, tr("Income & costs"),
        tr("See what your service earned, what it spent and which funds are available now."),
        QStringLiteral("paymasterFinances"));

    auto* finance_toolbar = new QGroupBox(
        tr("Reporting period"), finance_column);
    finance_toolbar->setProperty(
        "paymasterRole", QStringLiteral("card"));
    auto* finance_toolbar_layout = new QGridLayout(finance_toolbar);
    auto* finance_period_help = new QLabel(tr(
                                               "Income, costs and CSV export use this period. Today starts at midnight UTC; 7 and 30 days are rolling periods."),
                                           finance_toolbar);
    finance_period_help->setWordWrap(true);
    finance_toolbar_layout->addWidget(finance_period_help, 1, 0, 1, 3);
    m_finance_period_select = new NoWheelComboBox(finance_toolbar);
    m_finance_period_select->setObjectName("paymasterFinancePeriod");
    m_finance_period_select->addItem(tr("Today (UTC)"), QStringLiteral("today"));
    m_finance_period_select->addItem(tr("Last 7 days"), QStringLiteral("7d"));
    m_finance_period_select->addItem(tr("Last 30 days"), QStringLiteral("30d"));
    m_finance_period_select->addItem(tr("All recorded history"), QStringLiteral("all"));
    m_finance_period_select->setCurrentIndex(2);
    m_finance_refresh = new QPushButton(tr("Refresh"), finance_toolbar);
    m_finance_refresh->setObjectName("paymasterFinanceRefresh");
    m_finance_export = new QPushButton(tr("Export CSV…"), finance_toolbar);
    m_finance_export->setObjectName("paymasterFinanceExport");
    m_finance_export->setEnabled(false);
    finance_toolbar_layout->addWidget(m_finance_period_select, 0, 0);
    finance_toolbar_layout->addWidget(m_finance_refresh, 0, 1);
    finance_toolbar_layout->addWidget(m_finance_export, 0, 2);
    finance_toolbar_layout->setColumnStretch(0, 1);
    finance_layout->addWidget(finance_toolbar);

    // Legacy provider commits may predate the manifest-bound finance
    // ledger. Keep that coverage boundary next to the headline totals,
    // rather than hiding it in booking details where a zero income value
    // could otherwise be mistaken for a complete historical statement.
    m_finance_history_notice = new QGroupBox(
        tr("Recorded-history coverage"), finance_column);
    m_finance_history_notice->setObjectName(
        "paymasterFinanceHistoryNotice");
    m_finance_history_notice->setProperty(
        "paymasterRole", QStringLiteral("notice"));
    m_finance_history_notice->setProperty(
        "statusKind", QStringLiteral("waiting"));
    auto* finance_history_notice_layout =
        new QVBoxLayout(m_finance_history_notice);
    m_finance_history_notice_text = new QLabel(
        m_finance_history_notice);
    m_finance_history_notice_text->setObjectName(
        "paymasterFinanceHistoryNoticeText");
    m_finance_history_notice_text->setWordWrap(true);
    finance_history_notice_layout->addWidget(
        m_finance_history_notice_text);
    m_finance_history_notice->hide();
    finance_layout->addWidget(m_finance_history_notice);

    // Keep each metric in one presentation surface. These grids only
    // consume the existing finance RPC snapshot; they never query Core.
    const auto finance_value = [&](const QString& key, QWidget* parent, const QString& accessible_name, const char* role = "financeValue") {
        auto* value = finance_label(QStringLiteral("—"), parent, role);
        value->setObjectName(QStringLiteral("paymasterFinance_") + key);
        value->setAccessibleName(accessible_name);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_finance_values.insert(key, value);
        return value;
    };
    const auto finance_group = [&](const QString& title) {
        auto* group = new QGroupBox(title, finance_column);
        group->setProperty("paymasterRole", QStringLiteral("card"));
        return group;
    };
    const auto grid_headers = [&](QGridLayout* grid, const QStringList& headers) {
        grid->setHorizontalSpacing(20);
        grid->setVerticalSpacing(4);
        for (int column = 0; column < headers.size(); ++column) {
            auto* label = finance_label(headers.at(column), grid->parentWidget(), "financeColumnHeading");
            label->setAlignment((column ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
            grid->addWidget(label, 0, column);
            grid->setColumnStretch(column, column ? 2 : 3);
        }
    };

    m_finance_selected_summary = finance_label(tr("Choose a period to load recorded income and costs."), finance_column, "cardHeading");
    m_finance_selected_summary->setObjectName("paymasterFinanceSelectedSummary");
    finance_layout->addWidget(m_finance_selected_summary);
    auto* finance_metrics = new PaymasterResponsiveCards(finance_column, 3);
    finance_metrics->setObjectName("paymasterFinanceMetrics");
    const std::array<QString, 3> metric_titles{tr("Service-fee income"), tr("Operating costs"), tr("Successful payments")};
    const std::array<QString, 3> metric_keys{QStringLiteral("income"), QStringLiteral("costs"), QStringLiteral("transfers")};
    const std::array<QString, 3> metric_help{tr("DD earned in this period"), tr("DGB network fees, including reserves"), tr("Confirmed customer transfers")};
    for (size_t index = 0; index < metric_keys.size(); ++index) {
        auto* card = new QFrame(finance_metrics);
        card->setProperty("paymasterRole", QStringLiteral("statusCard"));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->addWidget(finance_label(metric_titles.at(index), card, "financeRowTitle"));
        layout->addWidget(finance_value(metric_keys.at(index), card, metric_titles.at(index), "financeAmount"));
        layout->addWidget(finance_label(metric_help.at(index), card, "mutedText"));
        if (index == 2) layout->addWidget(finance_value(QStringLiteral("average"), card, tr("Average service fee per successful payment"), "mutedText"));
        layout->addStretch();
        finance_metrics->addCard(card);
    }
    finance_layout->addWidget(finance_metrics);

    auto* breakdown = finance_group(tr("Income and costs by source"));
    breakdown->setObjectName("paymasterFinanceCostBreakdown");
    auto* breakdown_layout = new QVBoxLayout(breakdown);
    auto* breakdown_grid = new QGridLayout;
    breakdown_layout->addLayout(breakdown_grid);
    grid_headers(breakdown_grid, {tr("Source"), tr("Payments"), tr("Income (DD)"), tr("Costs (DGB)")});
    const std::array<QString, 5> source_titles{tr("Customer-paid payments"), tr("Public sponsorship"), tr("Restricted sponsorship"), tr("Reserve maintenance"), tr("Total")};
    const std::array<QString, 5> source_keys{QStringLiteral("user_paid"), QStringLiteral("public_sponsored"), QStringLiteral("restricted_sponsored"), QStringLiteral("maintenance"), QStringLiteral("total")};
    const std::array<QString, 3> source_columns{QStringLiteral("transfers"), QStringLiteral("income"), QStringLiteral("costs")};
    const std::array<QString, 3> source_column_titles{tr("Payments"), tr("Income (DD)"), tr("Costs (DGB)")};
    for (int row = 0; row < 5; ++row) {
        breakdown_grid->addWidget(finance_label(source_titles.at(row), breakdown, row == 4 ? "financeTotal" : "financeRowTitle"), row + 1, 0);
        for (int column = 0; column < 3; ++column) {
            auto* value = finance_value(source_keys.at(row) + QLatin1Char('_') + source_columns.at(column), breakdown,
                source_titles.at(row) + QStringLiteral(" · ") + source_column_titles.at(column), row == 4 ? "financeTotal" : "financeValue");
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            breakdown_grid->addWidget(value, row + 1, column + 1);
        }
    }
    breakdown_layout->addWidget(finance_label(tr("Reserve maintenance includes setup, refill, withdrawal and release fees. Capital held in reserves is not an expense."), breakdown, "mutedText"));
    finance_layout->addWidget(breakdown);

    auto* comparison = finance_group(tr("Reporting periods compared"));
    comparison->setObjectName("paymasterFinancePeriodCards");
    auto* comparison_layout = new QVBoxLayout(comparison);
    auto* comparison_grid = new QGridLayout;
    comparison_layout->addLayout(comparison_grid);
    grid_headers(comparison_grid, {tr("Period"), tr("Income (DD)"), tr("Costs (DGB)"), tr("Payments")});
    const std::array<QString, 4> period_titles{tr("Today (UTC)"), tr("Last 7 days"), tr("Last 30 days"), tr("All recorded history")};
    for (size_t index = 0; index < period_titles.size(); ++index) {
        comparison_grid->addWidget(finance_label(period_titles.at(index), comparison, "financeRowTitle"), index + 1, 0);
        const std::array<std::pair<QLabel**, QString>, 3> cells{{
            {&m_finance_period_income.at(index), QStringLiteral("paymasterFinancePeriodIncome%1").arg(index)},
            {&m_finance_period_cost.at(index), QStringLiteral("paymasterFinancePeriodCost%1").arg(index)},
            {&m_finance_period_transfers.at(index), QStringLiteral("paymasterFinancePeriodTransfers%1").arg(index)}}};
        for (int column = 0; column < 3; ++column) {
            auto* value = finance_label(QStringLiteral("—"), comparison, "financeValue");
            value->setObjectName(cells.at(column).second);
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            *cells.at(column).first = value;
            comparison_grid->addWidget(value, index + 1, column + 1);
        }
    }
    comparison_layout->addWidget(finance_label(tr("Periods overlap; do not add these rows together. Only confirmed bookings are included."), comparison, "mutedText"));
    AddPaymasterDisclosure(finance_layout, finance_column, comparison,
                           tr("Compare reporting periods"), tr("Hide period comparison"), QStringLiteral("paymasterFinanceCompareToggle"));

    auto* valuation = finance_group(tr("Estimate at the current DGB price"));
    auto* valuation_layout = new QVBoxLayout(valuation);
    valuation_layout->addWidget(finance_label(tr("Estimated income minus costs"), valuation, "financeRowTitle"));
    m_finance_result_estimate = finance_label(tr("No estimate loaded"), valuation, "financeAmount");
    m_finance_result_estimate->setObjectName("paymasterFinanceResultEstimate");
    valuation_layout->addWidget(m_finance_result_estimate);
    valuation_layout->addWidget(finance_value(QStringLiteral("valuation_price"), valuation, tr("DGB valuation price")));
    valuation_layout->addWidget(finance_value(QStringLiteral("valuation_time"), valuation, tr("Valuation timestamp"), "mutedText"));
    valuation_layout->addWidget(finance_label(tr("DD income is valued at its USD denomination; DGB costs use the current oracle price. This is an estimate, not historical profit or loss."), valuation, "mutedText"));
    AddPaymasterDisclosure(finance_layout, finance_column, valuation,
                           tr("Estimate result in USD"), tr("Hide USD estimate"), QStringLiteral("paymasterFinanceValuationToggle"));

    auto* finance_pool_group = finance_group(tr("Currently held in reserves"));
    finance_pool_group->setObjectName("paymasterFinanceCurrentCapital");
    auto* finance_pool_layout = new QVBoxLayout(finance_pool_group);
    finance_pool_layout->addWidget(finance_label(tr("Current wallet balances, independent of the reporting period. Reserve capital remains yours; accumulated fees are part of the income already recorded."), finance_pool_group, "mutedText"));
    auto* capital_columns = new PaymasterResponsiveCards(finance_pool_group);
    finance_pool_layout->addWidget(capital_columns);
    for (int asset = 0; asset < 2; ++asset) {
        auto* column = new QWidget(capital_columns);
        auto* layout = new QGridLayout(column);
        layout->setContentsMargins(0, 8, 0, 8);
        layout->setHorizontalSpacing(20);
        layout->setColumnStretch(0, 3);
        layout->setColumnStretch(1, 2);
        layout->addWidget(finance_label(asset ? tr("DigiDollar reserves") : tr("DGB network-fee reserves"), column, "financeColumnHeading"), 0, 0, 1, 2);
        const QStringList titles = asset ? QStringList{tr("Base capital"), tr("Accumulated service fees"), tr("Fees available to withdraw")} : QStringList{tr("Available reserve funds"), tr("Reserved or committed"), tr("Awaiting confirmation")};
        const QStringList keys = asset ? QStringList{QStringLiteral("carrier_base"), QStringLiteral("carrier_earned"), QStringLiteral("carrier_withdrawable")} : QStringList{QStringLiteral("dgb_available"), QStringLiteral("dgb_reserved"), QStringLiteral("dgb_pending")};
        for (int row = 0; row < 3; ++row) {
            layout->addWidget(finance_label(titles.at(row), column, "financeRowTitle"), row + 1, 0);
            auto* value = finance_value(keys.at(row), column, titles.at(row));
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(value, row + 1, 1);
        }
        capital_columns->addCard(column);
    }
    m_finance_pending_maintenance = finance_label({}, finance_pool_group, "mutedText");
    m_finance_pending_maintenance->setObjectName("paymasterFinancePendingMaintenance");
    finance_pool_layout->addWidget(m_finance_pending_maintenance);
    finance_layout->addWidget(finance_pool_group);

    auto* finance_actions_group = new QGroupBox(tr("Manage your reserves"), liquidity_column);
    finance_actions_group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    finance_actions_group->setObjectName("paymasterCapitalActions");
    finance_actions_group->setProperty("paymasterRole", QStringLiteral("card"));
    auto* capital_actions_layout = new QVBoxLayout(finance_actions_group);
    auto* action_cards = new PaymasterResponsiveCards(finance_actions_group, 3);
    capital_actions_layout->addWidget(action_cards);
    const auto add_capital_action = [&](QPushButton* action, const QString& title, const QString& explanation) {
        auto* card = new QWidget(action_cards);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->addWidget(finance_label(title, card, "cardHeading"));
        layout->addWidget(finance_label(explanation, card, "mutedText"));
        layout->addStretch();
        layout->addWidget(action, 0, Qt::AlignLeft);
        action_cards->addCard(card);
        return layout;
    };
    m_finance_add_liquidity = m_restore_task;
    m_finance_withdraw_fees = m_withdraw_task;
    m_finance_release_carrier = m_release_task;
    m_finance_review_dgb = new QPushButton(tr("Release excess DGB…"), finance_actions_group);
    m_finance_review_dgb->setObjectName("paymasterFinanceReviewDgb");
    add_capital_action(m_restore_task, tr("Keep capacity ready"), tr("Fill gaps in the saved reserve target. Existing reserves count toward it; review missing capital and network fees before approving."));
    auto* earnings_action = add_capital_action(m_withdraw_task, tr("Collect service fees"), tr("Make accumulated earnings spendable in this wallet. The DD base stays in reserves; a network fee may apply."));
    earnings_action->insertWidget(1, funds_value("earnings", earnings_action->parentWidget(), tr("Accumulated service fees"), "financeAmount"));
    earnings_action->insertWidget(3, m_withdrawal_hint);
    auto* change_capacity = new QPushButton(tr("Change target capacity…"), finance_actions_group);
    change_capacity->setObjectName("paymasterChangeCapacity");
    connect(change_capacity, &QPushButton::clicked, this, [liquidity_scroll, automatic_maintenance] {
        liquidity_scroll->ensureWidgetVisible(automatic_maintenance);
        if (auto* choice = automatic_maintenance->findChild<QComboBox*>()) choice->setFocus();
    });
    auto* reduction_action = add_capital_action(change_capacity, tr("Reduce reserved capital"), tr("Choose a smaller target below and save it. Then review the excess DGB and DD together before releasing them. Active payments remain protected."));
    reduction_action->addWidget(m_release_excess_reserves, 0, Qt::AlignLeft);
    liquidity_layout->insertWidget(liquidity_layout->indexOf(funds_metrics) + 1, finance_actions_group);

    auto* individual = new QGroupBox(tr("Individual reserve adjustments"), advanced_liquidity);
    individual->setObjectName("paymasterIndividualReserves");
    individual->setProperty("paymasterRole", QStringLiteral("card"));
    auto* individual_layout = new QVBoxLayout(individual);
    individual_layout->addWidget(finance_label(tr("For special cases. To reduce overall capacity, use a smaller target and review all excess reserves together."), individual, "mutedText"));
    individual_layout->addWidget(m_release_task, 0, Qt::AlignLeft);
    individual_layout->addWidget(finance_label(tr("Returns one complete DD payment reserve, including its earnings, to spendable wallet funds. No transaction or network fee is needed. This pauses the provider, turns off autostart and lowers only the DD payment-reserve target; other targets stay unchanged."), individual, "mutedText"));
    individual_layout->addWidget(m_finance_review_dgb, 0, Qt::AlignLeft);
    individual_layout->addWidget(finance_label(tr("Release only DGB above the saved targets. DD reserves and the target capacity stay unchanged. Review any network fees before approving."), individual, "mutedText"));
    advanced_liquidity_layout->insertWidget(0, individual);
    auto* funds_link = new QPushButton(tr("Manage funds and reserves…"), finance_column);
    funds_link->setObjectName("paymasterFinanceFundsLink");
    finance_pool_layout->addWidget(funds_link, 0, Qt::AlignLeft);
    connect(funds_link, &QPushButton::clicked, this, [this] { showOperatorPage(m_liquidity_page); });

    auto* finance_details = new QWidget(finance_column);
    finance_details->setObjectName("paymasterFinanceDetailsPanel");
    auto* finance_details_layout = new QVBoxLayout(finance_details);
    finance_details_layout->setContentsMargins(0, 0, 0, 0);
    m_finance_history_status = new QLabel(finance_details);
    m_finance_history_status->setObjectName(
        "paymasterFinanceHistoryStatus");
    m_finance_history_status->setWordWrap(true);
    auto* finance_daily_title = new QLabel(
        tr("Daily progression (UTC)"), finance_details);
    finance_daily_title->setProperty(
        "paymasterRole", QStringLiteral("cardHeading"));
    m_finance_daily_totals = new QTableWidget(0, 5, finance_details);
    m_finance_daily_totals->setObjectName(
        "paymasterFinanceDailyTotals");
    m_finance_daily_totals->setHorizontalHeaderLabels({
        tr("Day"), tr("DD income"), tr("DGB cost"),
        tr("Transfers"), tr("Maintenance")});
    m_finance_daily_totals->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    m_finance_daily_totals->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_finance_daily_totals->setAlternatingRowColors(true);
    m_finance_daily_totals->verticalHeader()->hide();
    m_finance_daily_totals->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_finance_daily_totals->horizontalHeader()->setStretchLastSection(true);
    m_finance_daily_totals->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_finance_daily_totals->setMinimumHeight(180);
    auto* finance_bookings_title = new QLabel(
        tr("Bookings"), finance_details);
    finance_bookings_title->setProperty(
        "paymasterRole", QStringLiteral("cardHeading"));
    m_finance_events = new QTableWidget(0, 6, finance_details);
    m_finance_events->setObjectName("paymasterFinanceEvents");
    m_finance_events->setHorizontalHeaderLabels({
        tr("Date (UTC)"), tr("Booking"), tr("Status"),
        tr("DD income"), tr("DGB cost"), tr("Payment model")});
    m_finance_events->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_finance_events->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_finance_events->setAlternatingRowColors(true);
    m_finance_events->verticalHeader()->hide();
    m_finance_events->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_finance_events->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    m_finance_events->horizontalHeader()->setStretchLastSection(true);
    m_finance_events->setMinimumHeight(260);
    m_finance_events->setAccessibleName(tr("Paymaster finance bookings"));
    m_finance_previous_page = new QPushButton(
        tr("← Previous 250"), finance_details);
    m_finance_previous_page->setObjectName(
        "paymasterFinancePreviousPage");
    m_finance_page_status = new QLabel(
        tr("No booking page loaded"), finance_details);
    m_finance_page_status->setObjectName("paymasterFinancePageStatus");
    m_finance_page_status->setAlignment(Qt::AlignCenter);
    m_finance_page_status->setAccessibleName(
        tr("Paymaster finance booking page"));
    m_finance_next_page = new QPushButton(
        tr("Next 250 →"), finance_details);
    m_finance_next_page->setObjectName("paymasterFinanceNextPage");
    auto* finance_page_navigation = new QHBoxLayout();
    finance_page_navigation->addWidget(m_finance_previous_page);
    finance_page_navigation->addStretch();
    finance_page_navigation->addWidget(m_finance_page_status);
    finance_page_navigation->addStretch();
    finance_page_navigation->addWidget(m_finance_next_page);
    finance_details_layout->addWidget(m_finance_history_status);
    finance_details_layout->addWidget(finance_daily_title);
    finance_details_layout->addWidget(m_finance_daily_totals);
    finance_details_layout->addWidget(finance_bookings_title);
    finance_details_layout->addWidget(m_finance_events);
    finance_details_layout->addLayout(finance_page_navigation);
    m_finance_details_toggle = AddPaymasterDisclosure(
        finance_layout, finance_column, finance_details,
        tr("Show booking details"), tr("Hide booking details"),
        QStringLiteral("paymasterFinanceDetailsToggle"));
    finance_layout->addStretch();
    finance_scroll->setWidget(finance);
    ConfigureScrollArea(finance_scroll, finance);
    m_finance_page = finance_scroll;
    m_tabs->addWidget(finance_scroll);

    auto* activity_scroll = new QScrollArea(m_tabs);
    activity_scroll->setObjectName("paymasterActivityPage");
    activity_scroll->setWidgetResizable(true);
    auto* activity = new QWidget(activity_scroll);
    activity->setObjectName("paymasterActivityContents");
    QVBoxLayout* activity_layout{nullptr};
    auto* activity_column = CreatePaymasterPageColumn(activity, activity_layout);
    AddPaymasterPageHeading(
        activity_layout, activity_column, tr("Activity and recovery"),
        tr("Monitor the automatic provider service, queued work and any durable reservations that require attention."),
        QStringLiteral("paymasterActivity"));

    auto* activity_intro_group = new QGroupBox(
        tr("Current service status"), activity_column);
    activity_intro_group->setProperty("paymasterRole", QStringLiteral("card"));
    auto* activity_intro_layout = new QVBoxLayout(activity_intro_group);
    auto* activity_intro = new QLabel(tr(
        "This page is used after setup. In the recommended automatic mode, Core answers "
        "capacity and quote requests and completes validated client submissions while the "
        "provider is running. The client must still confirm its exact provider and fee before signing."), activity_intro_group);
    activity_intro->setObjectName("paymasterActivityAutomaticExplanation");
    activity_intro->setWordWrap(true);
    activity_intro_layout->addWidget(activity_intro);
    auto* activity_responsibility = new QLabel(tr(
        "Keep the provider running and the wallet unlocked while serving clients. If the wallet "
        "is locked, automatic processing pauses without consuming queued messages and resumes "
        "after unlock. Wallet-local safety limits and full transaction validation remain enforced."), activity_intro_group);
    activity_responsibility->setObjectName("paymasterActivityResponsibility");
    activity_responsibility->setWordWrap(true);
    activity_intro_layout->addWidget(activity_responsibility);
    m_activity_runtime_status = new QLabel(
        tr("Provider status has not been loaded yet."), activity_intro_group);
    m_activity_runtime_status->setObjectName("paymasterActivityRuntimeStatus");
    m_activity_runtime_status->setWordWrap(true);
    QFont activity_status_font = m_activity_runtime_status->font();
    activity_status_font.setBold(true);
    m_activity_runtime_status->setFont(activity_status_font);
    activity_intro_layout->addWidget(m_activity_runtime_status);
    m_operation_primary = new QPushButton(
        tr("Open operation overview"), activity_intro_group);
    m_operation_primary->setObjectName("paymasterOperationPrimaryAction");
    m_operation_primary->setProperty(
        "paymasterRole", QStringLiteral("secondaryAction"));
    activity_intro_layout->addWidget(m_operation_primary, 0, Qt::AlignLeft);
    m_runtime_settings_toggle = new QPushButton(
        tr("Change request processing"), management_column);
    m_runtime_settings_toggle->setObjectName("paymasterRuntimeSettingsToggle");
    m_runtime_settings_toggle->setCheckable(true);
    m_runtime_settings_toggle->setFlat(true);
    management_layout->insertWidget(2, m_runtime_settings_toggle, 0, Qt::AlignLeft);
    activity_layout->addWidget(activity_intro_group);

    m_runtime_settings_panel = new QGroupBox(
        tr("Request processing"), management_column);
    m_runtime_settings_panel->setObjectName("paymasterRuntimeSettingsPanel");
    auto* runtime_settings_form = new QFormLayout(m_runtime_settings_panel);
    auto* runtime_settings_help = new QLabel(tr(
        "Automatic is recommended and processes bounded queue work after you consciously start "
        "the provider. Manual mode is intended only for diagnosis or expert-controlled operation. "
        "Saving a mode change offers to restart the Paymaster service. Automatic processing and autostart are different: processing handles requests after Start; autostart additionally requests a start when this wallet loads. Neither bypasses wallet locking, readiness checks or spending limits."), m_runtime_settings_panel);
    runtime_settings_help->setObjectName("paymasterRuntimeSettingsExplanation");
    runtime_settings_help->setWordWrap(true);
    runtime_settings_form->addRow(runtime_settings_help);
    m_operation_mode_select = new NoWheelComboBox(m_runtime_settings_panel);
    m_operation_mode_select->setObjectName("paymasterOperationMode");
    m_operation_mode_select->addItem(
        tr("Automatic — recommended"), QStringLiteral("automatic"));
    m_operation_mode_select->addItem(
        tr("Manual — expert mode"), QStringLiteral("manual"));
    runtime_settings_form->addRow(tr("Processing mode:"), m_operation_mode_select);
    auto* autostart_panel = new QWidget(m_operator_card);
    autostart_panel->setObjectName("paymasterAutostartPanel");
    auto* autostart_layout = new QVBoxLayout(autostart_panel);
    autostart_layout->setContentsMargins(0, 8, 0, 0);
    m_autostart = new QCheckBox(
        tr("Automatically start Paymaster when this wallet is ready"), autostart_panel);
    m_autostart->setObjectName("paymasterProviderAutostart");
    m_autostart->setToolTip(tr(
        "Autostart never stores a passphrase. An encrypted wallet waits for you to unlock it."));
    autostart_layout->addWidget(m_autostart);
    auto* autostart_help = new QLabel(tr(
        "Applies now and after this wallet is loaded again, while the provider configuration is enabled. "
        "An encrypted wallet still needs manual unlock after restart. Turning autostart off does not pause an already running provider."),
        autostart_panel);
    autostart_help->setWordWrap(true);
    autostart_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    autostart_layout->addWidget(autostart_help);
    m_autostart_status = new QLabel(autostart_panel);
    m_autostart_status->setObjectName("paymasterAutostartStatus");
    m_autostart_status->setWordWrap(true);
    m_autostart_status->setTextFormat(Qt::PlainText);
    autostart_layout->addWidget(m_autostart_status);
    addQuickRefill(autostart_layout, autostart_panel, true);
    auto* autostart_actions = new QHBoxLayout();
    m_save_autostart = new QPushButton(tr("Save autostart"), autostart_panel);
    m_save_autostart->setObjectName("paymasterSaveAutostart");
    m_discard_autostart = new QPushButton(tr("Discard change"), autostart_panel);
    m_discard_autostart->setObjectName("paymasterDiscardAutostart");
    autostart_actions->addWidget(m_save_autostart);
    autostart_actions->addWidget(m_discard_autostart);
    autostart_actions->addStretch();
    autostart_layout->addLayout(autostart_actions);
    operator_layout->insertWidget(operator_layout->count() - 1, autostart_panel);
    connect(m_save_autostart, &QPushButton::clicked, this, [this] { saveAutostart(); });
    connect(m_discard_autostart, &QPushButton::clicked, this, [this] { discardAutostart(); });
    m_save_runtime_settings = new QPushButton(
        tr("Save processing mode"), m_runtime_settings_panel);
    m_save_runtime_settings->setObjectName("savePaymasterRuntimeSettings");
    m_save_runtime_settings->setProperty(
        "paymasterRole", QStringLiteral("primaryAction"));
    runtime_settings_form->addRow(m_save_runtime_settings);
    auto* discard_mode = new QPushButton(tr("Discard processing change"), m_runtime_settings_panel);
    runtime_settings_form->addRow(discard_mode);
    connect(discard_mode, &QPushButton::clicked, this, [this] {
        if (m_busy) return;
        const QSignalBlocker blocker(m_operation_mode_select);
        m_operation_mode_select->setCurrentIndex(m_operation_mode_select->findData(m_operation_mode));
        m_runtime_settings_dirty = false;
        updateProviderButtons();
    });
    m_runtime_settings_result = new QLabel(
        tr("Automatic processing is the recommended default. Provider autostart is off by default."),
        m_runtime_settings_panel);
    m_runtime_settings_result->setObjectName("paymasterRuntimeSettingsResult");
    m_runtime_settings_result->setWordWrap(true);
    runtime_settings_form->addRow(m_runtime_settings_result);
    management_layout->insertWidget(3, m_runtime_settings_panel);
    // The settings page itself is still hidden while it is built. Force
    // an explicit show/hide transition so Qt remembers that this expert
    // panel must remain collapsed when the page is shown later.
    m_runtime_settings_panel->show();
    m_runtime_settings_panel->hide();

    m_request_processing_group = new QGroupBox(
        tr("1. Answer one waiting request"), activity);
    m_request_processing_group->setObjectName("paymasterManualRequestGroup");
    auto* request_layout = new QVBoxLayout(m_request_processing_group);
    auto* request_explanation = new QLabel(tr(
        "Handles one capacity check, quote request, or recovery-provider request. "
        "Core verifies the message and may sign a response and reserve a pool slot and "
        "part of the safety budget. It does not yet authorize the client's final payment."),
        m_request_processing_group);
    request_explanation->setObjectName("paymasterRequestProcessingExplanation");
    request_explanation->setWordWrap(true);
    request_layout->addWidget(request_explanation);
    m_process_requests = new QPushButton(
        tr("Process one waiting request"), m_request_processing_group);
    m_process_requests->setObjectName("paymasterProcessOneRequest");
    request_layout->addWidget(m_process_requests, 0, Qt::AlignLeft);
    activity_layout->addWidget(m_request_processing_group);

    m_submit_processing_group = new QGroupBox(
        tr("2. Complete one submitted payment"), activity);
    m_submit_processing_group->setObjectName("paymasterManualSubmitGroup");
    auto* submit_layout = new QVBoxLayout(m_submit_processing_group);
    auto* submit_explanation = new QLabel(tr(
        "Handles one client-signed payment or recovery submission. Core revalidates the "
        "exact authorized transaction before signing provider inputs. A successful action "
        "can spend reserved provider DGB, commit the transaction durably, and broadcast it."),
        m_submit_processing_group);
    submit_explanation->setObjectName("paymasterSubmitProcessingExplanation");
    submit_explanation->setWordWrap(true);
    submit_layout->addWidget(submit_explanation);
    m_process_submits = new QPushButton(
        tr("Review and process one submitted payment…"), m_submit_processing_group);
    m_process_submits->setObjectName("paymasterProcessOneSubmit");
    submit_layout->addWidget(m_process_submits, 0, Qt::AlignLeft);
    activity_layout->addWidget(m_submit_processing_group);

    m_activity_result_group = new QGroupBox(
        tr("Last processing result"), activity);
    m_activity_result_group->setObjectName("paymasterManualResultGroup");
    auto* activity_result_layout = new QVBoxLayout(m_activity_result_group);
    m_activity_action_result = new QLabel(
        tr("No request or submitted payment has been processed from this screen yet."),
        m_activity_result_group);
    m_activity_action_result->setObjectName("paymasterActivityActionResult");
    m_activity_action_result->setWordWrap(true);
    m_activity_action_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    activity_result_layout->addWidget(m_activity_action_result);
    activity_layout->addWidget(m_activity_result_group);

    m_reservations_group = new QGroupBox(
        tr("Reserved and committed pool outputs"), activity);
    auto* reservations_group = m_reservations_group;
    reservations_group->setProperty("paymasterRole", QStringLiteral("card"));
    auto* reservations_layout = new QVBoxLayout(reservations_group);
    auto* reservations_explanation = new QLabel(tr(
        "A reservation is a wallet pool output temporarily assigned to an in-progress "
        "quote, payment, or recovery. It is not an additional charge. This list also "
        "shows committed, spent, or replacement-pending outputs while Core preserves "
        "their durable state. They cannot be offered to another client."),
        reservations_group);
    reservations_explanation->setObjectName("paymasterReservationsExplanation");
    reservations_explanation->setWordWrap(true);
    reservations_layout->addWidget(reservations_explanation);
    auto* refresh_activity = new QPushButton(
        tr("Refresh provider activity"), reservations_group);
    refresh_activity->setObjectName("paymasterRefreshReservations");
    reservations_layout->addWidget(refresh_activity, 0, Qt::AlignLeft);
    m_activity_summary = new QLabel(
        tr("Reservations have not been checked yet."), reservations_group);
    m_activity_summary->setObjectName("paymasterActivitySummary");
    m_activity_summary->setWordWrap(true);
    m_activity_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    reservations_layout->addWidget(m_activity_summary);
    m_activity_reservations = new QTableWidget(0, 4, reservations_group);
    m_activity_reservations->setObjectName("paymasterActivityReservations");
    m_activity_reservations->setHorizontalHeaderLabels({tr("Purpose"), tr("Reserve"), tr("State"), tr("Next step")});
    m_activity_reservations->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_activity_reservations->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_activity_reservations->setWordWrap(true);
    m_activity_reservations->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_activity_reservations->verticalHeader()->hide();
    m_activity_reservations->setMinimumHeight(180);
    reservations_layout->addWidget(m_activity_reservations);
    m_activity_tasks = new QLabel(activity_column);
    m_activity_tasks->setObjectName("paymasterActivityTasks");
    m_activity_tasks->setWordWrap(true);
    activity_layout->addWidget(m_activity_tasks);
    activity_layout->addWidget(reservations_group);

    m_recovery_group = new QGroupBox(
        tr("What recovery means here"), activity);
    auto* recovery_group = m_recovery_group;
    recovery_group->setProperty("paymasterRole", QStringLiteral("notice"));
    auto* recovery_layout = new QVBoxLayout(recovery_group);
    auto* recovery_explanation = new QLabel(tr(
        "Recovery is not a manual way to rewrite or release a payment. Core keeps signed "
        "and committed work durable across restarts. Recovery requests use the first queue; "
        "recovery submissions use the second. Exact committed transactions are also "
        "reconciled when the provider starts. If an item remains reserved, inspect the "
        "technical result before attempting any cancellation through the RPC console."),
        recovery_group);
    recovery_explanation->setObjectName("paymasterRecoveryExplanation");
    recovery_explanation->setWordWrap(true);
    recovery_layout->addWidget(recovery_explanation);
    activity_layout->addWidget(recovery_group);

    auto* activity_details_toggle = new QPushButton(
        tr("Show last technical result"), activity);
    activity_details_toggle->setObjectName("paymasterActivityDetailsToggle");
    activity_details_toggle->setCheckable(true);
    activity_details_toggle->setFlat(true);
    activity_layout->addWidget(activity_details_toggle, 0, Qt::AlignLeft);
    m_activity_output = new QPlainTextEdit(activity);
    m_activity_output->setObjectName("paymasterActivityTechnicalResult");
    m_activity_output->setReadOnly(true);
    m_activity_output->setMaximumHeight(240);
    m_activity_output->setPlaceholderText(tr("No technical result available"));
    m_activity_output->viewport()->setBackgroundRole(QPalette::Window);
    m_activity_output->viewport()->setAutoFillBackground(true);
    m_activity_output->setVisible(false);
    activity_layout->addWidget(m_activity_output);
    activity_layout->addStretch();
    activity_scroll->setWidget(activity);
    ConfigureScrollArea(activity_scroll, activity);
    m_activity_page = activity_scroll;
    m_tabs->insertWidget(1, activity_scroll);
    m_settings_tabs = new QStackedWidget(m_tabs);
    m_settings_tabs->setObjectName("paymasterSettingsPages");
    auto* settings_home = new QScrollArea(m_settings_tabs);
    settings_home->setObjectName("paymasterSettingsHome");
    auto* settings_contents = new QWidget(settings_home);
    settings_contents->setObjectName("paymasterSettingsContents");
    QVBoxLayout* settings_layout{nullptr};
    auto* settings_column = CreatePaymasterPageColumn(settings_contents, settings_layout);
    AddPaymasterPageHeading(settings_layout, settings_column, tr("Settings"),
        tr("Configure your service, its costs and the reserves it needs. Form edits take effect when saved; operation and automation switches have their own explicit actions."),
        QStringLiteral("paymasterSettings"));
    settings_home->setWidgetResizable(true);
    settings_home->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settings_home->setWidget(settings_contents);
    ConfigureScrollArea(settings_home, settings_contents);
    m_settings_tabs->addWidget(settings_home);

    auto* automation_scroll = new QScrollArea(m_settings_tabs);
    automation_scroll->setObjectName("paymasterAutomationPage");
    auto* automation_contents = new QWidget(automation_scroll);
    automation_contents->setObjectName("paymasterAutomationContents");
    QVBoxLayout* automation_layout{nullptr};
    auto* automation_column = CreatePaymasterPageColumn(automation_contents, automation_layout);
    AddPaymasterPageHeading(automation_layout, automation_column, tr("Startup & processing"),
        tr("Autostart requests operation when this wallet is ready. Automatic processing handles customer requests after startup. Both respect wallet locking, readiness and saved budgets."),
        QStringLiteral("paymasterAutomation"));
    auto* start = new QCheckBox(tr("Automatically start Paymaster when this wallet is ready"), automation_column);
    start->setObjectName("paymasterSettingsAutostart");
    m_autostart_copies.append(start);
    automation_layout->addWidget(start);
    connect(start, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!m_busy && !m_privacy) m_autostart->setChecked(enabled);
    });
    auto* startup_help = new QLabel(tr("This switch saves immediately. An encrypted wallet still needs manual unlock after restart. Turning autostart off does not pause a running provider. The saved result is shown below."), automation_column);
    startup_help->setWordWrap(true);
    automation_layout->addWidget(startup_help);
    m_settings_autostart_status = new QLabel(automation_column);
    m_settings_autostart_status->setObjectName("paymasterSettingsAutostartStatus");
    m_settings_autostart_status->setWordWrap(true);
    m_settings_autostart_status->setTextFormat(Qt::PlainText);
    automation_layout->addWidget(m_settings_autostart_status);
    automation_layout->addWidget(m_runtime_settings_toggle);
    automation_layout->addWidget(m_runtime_settings_panel);
    auto* startup_link = new QPushButton(tr("View startup status on Overview"), automation_column);
    startup_link->setObjectName("paymasterAutostartOverviewLink");
    connect(startup_link, &QPushButton::clicked, this, [this] {
        m_tabs->setCurrentIndex(0);
        m_autostart->setFocus(Qt::OtherFocusReason);
    });
    automation_layout->addWidget(startup_link, 0, Qt::AlignLeft);
    automation_layout->addStretch();
    automation_scroll->setWidgetResizable(true);
    automation_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    automation_scroll->setWidget(automation_contents);
    ConfigureScrollArea(automation_scroll, automation_contents);
    m_automation_page = automation_scroll;

    auto* diagnostics_scroll = new QScrollArea(m_settings_tabs);
    diagnostics_scroll->setObjectName("paymasterDiagnosticsPage");
    auto* diagnostics_contents = new QWidget(diagnostics_scroll);
    diagnostics_contents->setObjectName("paymasterDiagnosticsContents");
    QVBoxLayout* diagnostics_layout{nullptr};
    auto* diagnostics_column = CreatePaymasterPageColumn(diagnostics_contents, diagnostics_layout);
    AddPaymasterPageHeading(diagnostics_layout, diagnostics_column, tr("Connection test"),
        tr("Copy a connection-test command and run it on a second node on the same network, with Paymaster and V2 transport enabled and outgoing Paymaster capacity. Opening this page does not start a connection test."),
        QStringLiteral("paymasterDiagnostics"));
    auto* endpoint_group = new QGroupBox(tr("Connection test from another node"), diagnostics_column);
    auto* endpoint_layout = new QVBoxLayout(endpoint_group);
    auto* endpoint_help = new QLabel(tr("The command uses this node's current announced Paymaster address (-paymasterendpoint), including its port. It is not a measurement of reachability. A public address behind NAT or a Tor service address cannot be inferred from the local listener. Saved connection changes require a node restart."), endpoint_group);
    endpoint_help->setWordWrap(true);
    endpoint_layout->addWidget(endpoint_help);
    m_diagnostic_endpoint = new QLabel(endpoint_group);
    m_diagnostic_endpoint->setObjectName("paymasterDiagnosticEndpoint");
    m_diagnostic_endpoint->setWordWrap(true);
    m_diagnostic_endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    endpoint_layout->addWidget(m_diagnostic_endpoint);
    m_diagnostic_hint = new QLabel(endpoint_group);
    m_diagnostic_hint->setObjectName("paymasterDiagnosticHint");
    m_diagnostic_hint->setWordWrap(true);
    endpoint_layout->addWidget(m_diagnostic_hint);
    const auto command_field = [this, endpoint_group, endpoint_layout](const QString& title, const char* name,
        QPlainTextEdit*& command, QPushButton*& copy) {
        auto* label = new QLabel(title, endpoint_group);
        endpoint_layout->addWidget(label);
        command = new QPlainTextEdit(endpoint_group);
        command->setObjectName(QString::fromLatin1(name));
        command->setReadOnly(true);
        command->setAccessibleName(title);
        command->setWordWrapMode(QTextOption::WrapAnywhere);
        command->setFixedHeight(command->fontMetrics().height() * 3 + 20);
        endpoint_layout->addWidget(command);
        copy = new QPushButton(tr("Copy command"), endpoint_group);
        copy->setObjectName(QString::fromLatin1(name) + QStringLiteral("Copy"));
        copy->setAccessibleName(tr("Copy command: %1").arg(title));
        endpoint_layout->addWidget(copy, 0, Qt::AlignLeft);
        connect(copy, &QPushButton::clicked, this, [this, command] {
            if (!m_privacy && !command->toPlainText().isEmpty()) GUIUtil::setClipboard(command->toPlainText());
        });
    };
    command_field(tr("RPC console on the second node"), "paymasterDiagnosticRpc", m_diagnostic_rpc, m_diagnostic_rpc_copy);
    command_field(tr("Command line on the second node"), "paymasterDiagnosticCli", m_diagnostic_cli, m_diagnostic_cli_copy);
    auto* cli_help = new QLabel(tr("The command-line example selects the network. If the second node uses a separate data directory or RPC port, add that node's -datadir or -rpcport option before the command. Run digibyte-cli from its installation directory or use its full path; in PowerShell, use .\\digibyte-cli.exe for an executable in the current directory."), endpoint_group);
    cli_help->setWordWrap(true);
    endpoint_layout->addWidget(cli_help);
    auto* diagnostic_result_help = new QLabel(tr("Read the result on the second node: transport_ready=true means the encrypted connection and Paymaster negotiation succeeded. It does not verify provider identity, available reserves or payments. The check may take up to 30 seconds; only one check may start per 30 seconds. There is no result import."), endpoint_group);
    diagnostic_result_help->setWordWrap(true);
    endpoint_layout->addWidget(diagnostic_result_help);
    auto* diagnostic_refresh = new QPushButton(tr("Refresh announced address"), endpoint_group);
    diagnostic_refresh->setObjectName("paymasterDiagnosticRefresh");
    endpoint_layout->addWidget(diagnostic_refresh, 0, Qt::AlignLeft);
    connect(diagnostic_refresh, &QPushButton::clicked, this, [this] { refreshOperatorStatus(); });
    auto* diagnostic_connection = new QPushButton(tr("Open node connection settings"), endpoint_group);
    diagnostic_connection->setObjectName("paymasterDiagnosticConnectionLink");
    endpoint_layout->addWidget(diagnostic_connection, 0, Qt::AlignLeft);
    connect(diagnostic_connection, &QPushButton::clicked, this, [this] { showOperatorPage(m_connection_page); });
    diagnostics_layout->addWidget(endpoint_group);
    diagnostics_layout->addStretch();
    diagnostics_scroll->setWidgetResizable(true);
    diagnostics_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    diagnostics_scroll->setWidget(diagnostics_contents);
    ConfigureScrollArea(diagnostics_scroll, diagnostics_contents);
    m_diagnostics_page = diagnostics_scroll;
    renderEndpointDiagnostic(UniValue{});

    const auto settings_group = [settings_layout, settings_column](const QString& title) {
        auto* group = new QGroupBox(QString(title).replace(QLatin1Char('&'), QStringLiteral("&&")), settings_column);
        group->setProperty("paymasterRole", QStringLiteral("card"));
        settings_layout->addWidget(group);
        return new QVBoxLayout(group);
    };
    const auto add_settings_page = [this](QVBoxLayout* menu_layout,
        QWidget* page, QVBoxLayout* page_layout, const QString& title, const QString& help) {
        auto* open = new QPushButton(QString(title).replace(QLatin1Char('&'), QStringLiteral("&&")));
        open->setAccessibleName(title);
        open->setObjectName(page->objectName() + QStringLiteral("Link"));
        open->setProperty("paymasterRole", QStringLiteral("settingsAction"));
        open->setAccessibleDescription(help);
        menu_layout->addWidget(open);
        auto* description = new QLabel(help);
        description->setWordWrap(true);
        description->setProperty("paymasterRole", QStringLiteral("mutedText"));
        menu_layout->addWidget(description);
        connect(open, &QPushButton::clicked, this, [this, page] { showOperatorPage(page); });
        auto* back = new QPushButton(tr("Back to settings"), page);
        back->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        page_layout->insertWidget(0, back, 0, Qt::AlignLeft);
        connect(back, &QPushButton::clicked, this, [this] { m_settings_tabs->setCurrentIndex(0); });
        m_settings_tabs->addWidget(page);
    };
    connect(m_settings_tabs, &QStackedWidget::currentChanged, this, [this](int) {
        updateAutomaticRefreshTimer();
        if (m_settings_tabs->currentWidget() == m_connection_page && !m_inline_node_editor && !m_busy)
            configureOperatorNode();
        if (m_settings_tabs->currentWidget() == m_diagnostics_page && !m_privacy && !m_busy)
            refreshOperatorStatus();
    });
    auto* operation_settings = settings_group(tr("Operation"));
    add_settings_page(operation_settings, lifecycle_scroll, lifecycle_page_layout, tr("Start, pause & retire"),
        tr("Compare operation controls, keep or release reserves, and review retirement."));
    add_settings_page(operation_settings, automation_scroll, automation_layout, tr("Startup & processing"),
        tr("Choose autostart and automatic or manual request processing. Configure reserve refill separately below."));
    auto* service_settings = settings_group(tr("Service, reserves & costs"));
    add_settings_page(service_settings, configuration_scroll, configuration_layout, tr("1. Offer & pricing"),
        tr("What customers receive: payment models, amount range and DD service fee. The offer's DGB network-fee ceiling must fit the customer-payment budgets."));
    add_settings_page(service_settings, safety_scroll, safety_layout, tr("2. Customer-payment budgets"),
        tr("What customer payments may cost you: DGB network fees per payment, rolling hour and day, separately for each payment model."));
    add_settings_page(service_settings, liquidity_scroll, liquidity_layout, tr("3. Reserves & refill"),
        tr("What capital to keep ready: complete DGB/DD reserves. Automatic refill has separate DGB fee limits; reserve capital itself is not a fee."));
    auto* budget_scope = new QLabel(tr("Payment-model budgets and refill budgets are independent and can both be used. They are not a shared daily wallet cap. Unused allowances are not spent."));
    budget_scope->setObjectName("paymasterSettingsBudgetScope");
    budget_scope->setWordWrap(true);
    service_settings->addWidget(budget_scope);
    auto* technical_settings = settings_group(tr("Node & wallet"));
    add_settings_page(technical_settings, connection_scroll, connection_layout, tr("Node connection"),
        tr("Edit node settings and review their sources and reachability."));
    add_settings_page(technical_settings, management_scroll, management_layout, tr("Wallet & backup"),
        tr("Manage wallet access and protect the provider with a full-wallet backup."));
    // This detail view belongs to Node connection, not the Settings menu.
    auto* diagnostics_back = new QPushButton(tr("Back to node connection"), diagnostics_column);
    diagnostics_back->setObjectName("paymasterDiagnosticsBack");
    diagnostics_layout->insertWidget(0, diagnostics_back, 0, Qt::AlignLeft);
    connect(diagnostics_back, &QPushButton::clicked, this, [this] { showOperatorPage(m_connection_page); });
    m_settings_tabs->addWidget(diagnostics_scroll);
    auto* connection_test = new QPushButton(tr("Test connection from another node…"), network_group);
    connection_test->setObjectName("paymasterConnectionDiagnosticsLink");
    connection_test->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    network_layout->insertWidget(1, connection_test, 0, Qt::AlignLeft);
    connect(connection_test, &QPushButton::clicked, this, [this] { showOperatorPage(m_diagnostics_page); });

    const auto related_settings = [this](QVBoxLayout* layout, const QString& title,
                                         const char* name, QWidget* destination) {
        auto* link = new QPushButton(title);
        link->setObjectName(QString::fromLatin1(name));
        layout->insertWidget(3, link, 0, Qt::AlignLeft);
        connect(link, &QPushButton::clicked, this, [this, destination] { showOperatorPage(destination); });
    };
    related_settings(configuration_layout, tr("Review customer-payment budgets"), "paymasterOfferBudgetsLink", m_safety_page);
    related_settings(safety_layout, tr("Configure reserve capital and refill costs"), "paymasterBudgetsReservesLink", m_liquidity_page);
    related_settings(liquidity_layout, tr("Review customer-payment budgets"), "paymasterReservesBudgetsLink", m_safety_page);
    related_settings(automation_layout, tr("Configure automatic reserve refill"), "paymasterStartupReservesLink", m_liquidity_page);

    auto* refill_controls = new QGroupBox(tr("Automatic refill — saved settings"), liquidity_column);
    refill_controls->setObjectName("paymasterSavedRefillControls");
    refill_controls->setProperty("paymasterRole", QStringLiteral("card"));
    auto* refill_layout = new QVBoxLayout(refill_controls);
    auto* saved_refill_help = new QLabel(tr("This switch applies to the saved reserve targets and fee approval immediately. Edit and save targets or fees in the form below. A stopped provider must be started separately."), refill_controls);
    saved_refill_help->setWordWrap(true);
    refill_layout->addWidget(saved_refill_help);
    addQuickRefill(refill_layout, refill_controls, false);
    liquidity_layout->insertWidget(liquidity_layout->indexOf(automatic_maintenance), refill_controls);
    auto* lifecycle_link = new QPushButton(tr("Start, pause, stop or retire Paymaster — open Settings"), liquidity_column);
    lifecycle_link->setObjectName("paymasterLiquidityLifecycleLink");
    liquidity_layout->addWidget(lifecycle_link, 0, Qt::AlignLeft);
    connect(lifecycle_link, &QPushButton::clicked, this, [this] { showOperatorPage(m_lifecycle_page); });
    settings_layout->addStretch();
    management_layout->addStretch();
    management_scroll->setWidget(management);
    ConfigureScrollArea(management_scroll, management);
    connection_layout->addStretch();
    connection_scroll->setWidget(connection);
    ConfigureScrollArea(connection_scroll, connection);
    m_tabs->addWidget(m_settings_tabs);

    const QStringList destinations{tr("Overview"), tr("Activity"),
        tr("Income & costs"), tr("Settings")};
    for (int index = 0; index < destinations.size(); ++index) {
        m_tabs->widget(index)->setProperty("paymasterPageTitle", destinations[index]);
        m_navigation->addItem(destinations[index]);
        m_navigation_select->addItem(destinations[index]);
    }
    connect(m_navigation, &QListWidget::currentRowChanged, m_tabs, &QStackedWidget::setCurrentIndex);
    connect(m_navigation_select, qOverload<int>(&QComboBox::currentIndexChanged),
        m_tabs, &QStackedWidget::setCurrentIndex);
    connect(m_tabs, &QStackedWidget::currentChanged, this, [this](int index) {
        const QSignalBlocker list_blocker(m_navigation);
        const QSignalBlocker select_blocker(m_navigation_select);
        m_navigation->setCurrentRow(index);
        m_navigation_select->setCurrentIndex(index);
        updateAutomaticRefreshTimer();
    });
    m_navigation->setCurrentRow(0);
    updateNavigationLayout();
    // Keep the same guarded task visible when the user reviews another area.
    outer->insertWidget(1, m_task_card);
    delete task_actions;

    connect(refresh, &QPushButton::clicked, this, [this] { refreshStatus(); });
    connect(m_guided_setup, &QPushButton::clicked, this, [this] {
        if (showSetupWizard()) {
            setSetupMode(PaymasterSetupMode::GUIDED);
            m_tabs->setCurrentIndex(0);
            refreshStatus();
        }
    });
    connect(m_reopen_wizard, &QPushButton::clicked, this, [this] {
        if (showSetupWizard()) {
            m_tabs->setCurrentIndex(0);
            refreshStatus();
        }
    });
    connect(m_expert_setup, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(
                this, tr("Manual Paymaster setup"),
                tr("Manual setup exposes every provider control immediately. Incomplete settings keep the provider offline, while overly broad sponsored budgets can spend more DGB than intended.\n\nContinue with expert setup?"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) == QMessageBox::Yes) {
            setSetupMode(PaymasterSetupMode::EXPERT);
            showOperatorPage(m_configuration_page);
        }
    });
    connect(details_toggle, &QPushButton::toggled, this,
            [this, details_toggle, details](bool visible) {
                details->setVisible(visible);
                details_toggle->setText(visible ? tr("Hide technical details")
                                                : tr("Show technical details"));
            });
    connect(activity_details_toggle, &QPushButton::toggled, this,
            [this, activity_details_toggle](bool visible) {
                m_activity_output->setVisible(visible);
                activity_details_toggle->setText(
                    visible ? tr("Hide last technical result")
                             : tr("Show last technical result"));
            });
    connect(m_runtime_settings_toggle, &QPushButton::toggled, this,
            [this](bool visible) {
                m_runtime_settings_panel->setVisible(visible);
                m_runtime_settings_toggle->setText(
                    visible ? tr("Hide request processing")
                            : tr("Change request processing"));
            });
    connect(m_operation_mode_select,
            qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
                if (!m_loading_runtime_settings) {
                    m_runtime_settings_dirty = true;
                    updateProviderButtons();
                }
            });
    connect(m_autostart, &QCheckBox::toggled, this, [this] {
        if (!m_loading_runtime_settings) {
            m_autostart_dirty = m_autostart->isChecked() != m_autostart_enabled;
            m_autostart_message.clear();
            saveAutostart();
            updateProviderButtons();
        }
    });
    connect(m_save_runtime_settings, &QPushButton::clicked, this,
            [this] { saveRuntimeSettings(); });
    connect(m_operation_primary, &QPushButton::clicked, this, [this] {
        m_tabs->setCurrentIndex(0);
    });
    connect(m_overview_offer_action, &QPushButton::clicked, this,
            [this] { showOperatorPage(m_configuration_page); });
    connect(m_overview_safety_action, &QPushButton::clicked, this,
            [this] { showOperatorPage(m_safety_page); });
    connect(m_overview_liquidity_action, &QPushButton::clicked, this,
            [this] {
                if (m_operator_next_action == QLatin1String("wait") &&
                    (m_operator_next_area == QLatin1String("payments") || m_operator_next_area == QLatin1String("liquidity"))) {
                    performOperatorNextAction();
                    return;
                }
                if (maintenanceFeeLimitExceeded()) {
                    reviewMaintenanceFeeLimit();
                    return;
                }
                if (m_maintenance_state ==
                    QLatin1String("waiting_for_maintenance_approval")) {
                    approveSuggestedLiquidityMaintenance();
                    return;
                }
                // This label is an action, not a navigation shortcut. If
                // the bounded refill is already approved and the provider
                // is stopped, start the provider through the same guarded
                // confirmation path as the primary Operation button.
                if (shouldStartAndRestoreLiquidity()) {
                    startProvider();
                    return;
                }
                showOperatorPage(m_liquidity_page);
                if (!m_liquidity_targets_satisfy_provider_policy &&
                    m_liquidity_advanced_toggle) {
                    m_liquidity_advanced_toggle->setChecked(true);
                }
            });
    connect(m_overview_operation_action, &QPushButton::clicked, this,
            [this] {
                if (maintenanceWaitingForFunds()) {
                    showOperatorPage(m_liquidity_page);
                } else if (maintenanceFeeLimitExceeded()) {
                    reviewMaintenanceFeeLimit();
                } else if (!m_liquidity_targets_satisfy_provider_policy) {
                    showOperatorPage(m_liquidity_page);
                    if (m_liquidity_advanced_toggle) {
                        m_liquidity_advanced_toggle->setChecked(true);
                    }
                } else if (!m_core_running && providerConfigurationComplete() &&
                    hasPassiveExternalWait()) {
                    refreshStatus();
                } else if (m_maintenance_state ==
                           QLatin1String("waiting_for_maintenance_approval")) {
                    approveSuggestedLiquidityMaintenance();
                } else if (shouldStartAndRestoreLiquidity()) {
                    showOperatorPage(m_liquidity_page);
                } else {
                    showOperatorPage(m_activity_page);
                }
            });
    connect(m_overview_finance_action, &QPushButton::clicked, this,
            [this] {
                showOperatorPage(m_finance_page);
                if (!m_busy) refreshFinanceStatus();
            });
    connect(m_overview_backup_now, &QPushButton::clicked, this,
            [this] { requestProviderBackup(); });
    connect(m_overview_backup_external, &QPushButton::clicked, this,
            [this] { acknowledgeExternalBackup(); });
    connect(m_finance_refresh, &QPushButton::clicked, this,
            [this] { refreshFinanceStatus(); });
    connect(m_finance_period_select,
            qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
                if (isOperatorPage(m_finance_page) && !m_busy) {
                    refreshFinanceStatus();
                }
            });
    connect(m_finance_export, &QPushButton::clicked, this,
            [this] { exportFinanceCsv(); });
    connect(m_finance_previous_page, &QPushButton::clicked, this,
            [this] { showPreviousFinancePage(); });
    connect(m_finance_next_page, &QPushButton::clicked, this,
            [this] { showNextFinancePage(); });
    connect(m_finance_review_dgb, &QPushButton::clicked, this,
            [this] { beginGuidedTask("rebalancepaymasterpool"); });
    connect(m_tabs, &QStackedWidget::currentChanged, this, [this](int) {
        if (isOperatorPage(m_finance_page) &&
            hasRpcTransport() && !m_busy) {
            refreshFinanceStatus();
        }
    });
    connect(create_identity, &QPushButton::clicked, this, [this] {
        if (!m_display_name->hasAcceptableInput()) {
            QMessageBox::warning(
                this, tr("Paymaster provider identity"),
                tr("The display name must contain at most 32 printable ASCII characters and cannot contain '/' or '@'."));
            return;
        }
        UniValue params{UniValue::VARR};
        params.push_back(m_display_name->text().trimmed().toStdString());
        call("createpaymasteridentity", std::move(params), true);
    });
    connect(m_display_name, &QLineEdit::textChanged, this,
            [create_identity, this] {
                create_identity->setEnabled(
                    !m_core_has_identity &&
                    m_display_name->hasAcceptableInput() && !m_busy);
            });
    connect(m_save_policy, &QPushButton::clicked, this,
            [this] { savePolicy(); });
    connect(m_save_provider_safety, &QPushButton::clicked, this, [this] { saveProviderSafetyPolicy(); });
    connect(m_save_client_safety, &QPushButton::clicked, this, [this] { saveClientSafetyPolicy(); });
    connect(restore_quote_defaults, &QPushButton::clicked,
            this, [this] { restoreQuoteSafetyDefaults(); });
    connect(restore_client_defaults, &QPushButton::clicked,
            this, [this] { restoreClientSafetyDefaults(); });
    connect(m_approve_liquidity_maintenance, &QPushButton::clicked,
            this, [this] {
                if (maintenanceFeeLimitExceeded()) {
                    reviewMaintenanceFeeLimit();
                } else {
                    approveSuggestedLiquidityMaintenance();
                }
            });
    connect(m_restore_policy_defaults, &QPushButton::clicked,
            this, [this] { restorePolicyDefaults(); });
    connect(m_restore_liquidity_defaults, &QPushButton::clicked,
            this, [this] { restoreLiquidityDefaults(); });
    connect(m_save_liquidity_policy, &QPushButton::clicked,
            this, [this] { saveLiquidityPolicy(); });
    connect(m_save_liquidity_policy_primary, &QPushButton::clicked,
            this, [this] { saveLiquidityPolicy(); });
    connect(m_automatic_replenishment, &QCheckBox::toggled,
            this, [this](bool enabled) {
                markLiquidityPolicyDirty();
                if (!m_loading_liquidity_policy && enabled && !m_paid_maintenance_approved->isChecked())
                    m_maintenance_limits_toggle->setChecked(true);
            });
    connect(m_paid_maintenance_approved, &QCheckBox::toggled,
            this, [this] { markLiquidityPolicyDirty(); });
    for (QLineEdit* control : {m_maintenance_fee_per_transaction,
                               m_maintenance_fee_per_hour,
                               m_maintenance_fee_per_day}) {
        connect(control, &QLineEdit::textChanged, this,
                [this] { markLiquidityPolicyDirty(); });
    }
    connect(m_preview_carrier_excess, &QPushButton::clicked,
            this, [this] { carrierWithdrawalAction("all_excess", false); });
    connect(m_execute_carrier_excess, &QPushButton::clicked,
            this, [this] { carrierWithdrawalAction("all_excess", true); });
    connect(m_preview_carrier_release, &QPushButton::clicked,
            this, [this] { carrierWithdrawalAction("release_slot", false); });
    connect(m_execute_carrier_release, &QPushButton::clicked,
            this, [this] { carrierWithdrawalAction("release_slot", true); });
    connect(m_release_carrier_select,
            qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { invalidateCarrierWithdrawalPreviews(); });
    connect(prepare_preview, &QPushButton::clicked, this, [this] { beginGuidedTask("preparepaymasterpool"); });
    connect(prepare_execute, &QPushButton::clicked, this, [this] { poolAction("preparepaymasterpool", true); });
    connect(rebalance_preview, &QPushButton::clicked, this, [this] { beginGuidedTask("rebalancepaymasterpool"); });
    connect(rebalance_execute, &QPushButton::clicked, this, [this] { poolAction("rebalancepaymasterpool", true); });
    connect(refresh_activity, &QPushButton::clicked, this, [this] {
        refreshActivity();
    });
    connect(m_process_requests, &QPushButton::clicked, this, [this] {
        processActivityRequest();
    });
    connect(m_process_submits, &QPushButton::clicked, this, [this] {
        processActivitySubmit();
    });
    watchFundingSafetyControls(m_user_paid_safety);
    watchFundingSafetyControls(m_public_sponsored_safety);
    watchFundingSafetyControls(m_restricted_sponsored_safety);
    for (QSpinBox* control : {m_max_active_quotes_total,
                              m_max_active_quotes_per_netgroup,
                              m_max_active_quotes_per_recipient,
                              m_max_quote_requests_per_netgroup}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            if (!m_loading_provider_safety) markProviderSafetyDirty();
            updateProviderButtons();
        });
    }
    connect(m_sponsored, &QCheckBox::toggled, this, [this] {
        if (!m_loading_policy) {
            m_policy_dirty = true;
            invalidatePoolPreviews();
        }
        updatePolicyDisplay();
        updateLiquidityDisplay();
        updateProviderButtons();
        updateAutomaticRefreshTimer();
    });
    connect(m_user_paid, &QCheckBox::toggled, this, [this] {
        if (!m_loading_policy) {
            m_policy_dirty = true;
            invalidatePoolPreviews();
        }
        updatePolicyDisplay();
        updateLiquidityDisplay();
        updateProviderButtons();
        updateAutomaticRefreshTimer();
    });
    connect(m_scope, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_loading_policy) {
            m_policy_dirty = true;
            invalidatePoolPreviews();
        }
        updatePolicyDisplay();
        updateProviderButtons();
        updateAutomaticRefreshTimer();
    });
    for (QSpinBox* control : {m_fee_bps,
                              m_maximum_user_paid_service_fee,
                              m_min_amount, m_max_amount,
                  m_quote_ttl, m_network_fee}) {
        const auto edited = [this] {
            if (!m_loading_policy) {
                m_policy_dirty = true;
                invalidatePoolPreviews();
            }
            updatePolicyDisplay();
            updateAutomaticRefreshTimer();
        };
        connect(control, qOverload<int>(&QSpinBox::valueChanged), this, edited);
        // Empty/partial text can leave the numeric value unchanged. It is
        // still a draft and must survive status refresh/navigation.
        connect(control->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, edited);
    }
    for (QSpinBox* control : {m_admission_dgb, m_operational_dgb,
                              m_admission_carriers, m_operational_carriers}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged),
                this, [this] {
                    markLiquidityPolicyDirty();
                    updateLiquidityDisplay();
                });
    }
    connect(m_client_fee_per_transaction, qOverload<int>(&QSpinBox::valueChanged),
            this, [this] {
                if (!m_loading_client_safety) markClientSafetyDirty();
                updateClientSafetyDisplay();
            });
    connect(m_client_fee_per_day, qOverload<int>(&QSpinBox::valueChanged),
            this, [this] {
                if (!m_loading_client_safety) markClientSafetyDirty();
                updateClientSafetyDisplay();
            });
    // Classify every retained operator control so the generic bright-blue
    // wallet button style cannot leak into these pages. Explicit primary,
    // danger and disclosure roles assigned above remain untouched.
    for (QGroupBox* group : findChildren<QGroupBox*>()) {
        if (!group->property("paymasterRole").isValid()) {
            group->setProperty("paymasterRole", QStringLiteral("card"));
        }
    }
    for (QPushButton* button : findChildren<QPushButton*>()) {
        if (!button->property("paymasterRole").isValid()) {
            button->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
        }
        button->setCursor(Qt::PointingHandCursor);
    }
    // QLabel defaults to AutoText. Provider endpoints and backend error
    // strings can contain angle brackets, so treating every operator label
    // as plain text prevents those values from being interpreted as rich
    // text while preserving selectable status output.
    for (QLabel* label : findChildren<QLabel*>()) {
        label->setTextFormat(Qt::PlainText);
    }
    updateFundingSafetyDisplay(m_user_paid_safety);
    updateFundingSafetyDisplay(m_public_sponsored_safety);
    updateFundingSafetyDisplay(m_restricted_sponsored_safety);
    updateClientSafetyDisplay();
    updatePolicyDisplay();
    updateLiquidityDisplay();
    updateFinanceControls();
    updateSetupAccess();
    updateProviderButtons();

    m_setup_status_timer = new QTimer(this);
    m_setup_status_timer->setObjectName("paymasterSetupStatusTimer");
    m_setup_status_timer->setInterval(30000);
    connect(m_setup_status_timer, &QTimer::timeout, this, [this] {
        if (isVisible() && hasRpcTransport() && !m_busy && !m_setup_wizard_active && !m_node_connection_dialog && !m_privacy)
            refreshOperatorStatus(/*background=*/true);
    });
    m_setup_status_timer->start();

}

DigiDollarPaymasterWidget* CreatePaymasterWidget(QWidget* parent)
{
return new PaymasterWidgetImpl(parent);
}

void ConfigurePaymasterScrollArea(QScrollArea* scroll, QWidget* contents)
{
ConfigureScrollArea(scroll, contents);
}
