// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

PaymasterWidgetImpl::PaymasterWidgetImpl(QWidget* parent) : DigiDollarPaymasterWidget(parent)
{
    PageBindings pages;
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

    m_overview.buildOverview(pages);

    m_configuration.buildConfiguration(pages);

    m_configuration.buildSafety(pages);

    m_liquidity.buildLiquidity(pages);

    m_finance.buildFinance(pages);

    m_overview.buildActivity(pages);

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

    m_configuration.buildAutomation(pages);

    m_overview.buildDiagnostics(pages);

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
        m_overview.updateAutomaticRefreshTimer();
        if (m_settings_tabs->currentWidget() == m_connection_page && !m_configuration.nodeEditorActive() && !m_busy)
            m_configuration.configureOperatorNode();
        if (m_settings_tabs->currentWidget() == m_diagnostics_page && !m_privacy && !m_busy)
            m_overview.refreshOperatorStatus();
    });
    auto* operation_settings = settings_group(tr("Operation"));
    add_settings_page(operation_settings, pages.lifecycle_scroll, pages.lifecycle_page_layout, tr("Start, pause & retire"),
        tr("Compare operation controls, keep or release reserves, and review retirement."));
    add_settings_page(operation_settings, pages.automation_scroll, pages.automation_layout, tr("Startup & processing"),
        tr("Choose autostart and automatic or manual request processing. Configure reserve refill separately below."));
    auto* service_settings = settings_group(tr("Service, reserves & costs"));
    add_settings_page(service_settings, pages.configuration_scroll, pages.configuration_layout, tr("1. Offer & pricing"),
        tr("What customers receive: payment models, amount range and DD service fee. The offer's DGB network-fee ceiling must fit the customer-payment budgets."));
    add_settings_page(service_settings, pages.safety_scroll, pages.safety_layout, tr("2. Customer-payment budgets"),
        tr("What customer payments may cost you: DGB network fees per payment, rolling hour and day, separately for each payment model."));
    add_settings_page(service_settings, pages.liquidity_scroll, pages.liquidity_layout, tr("3. Reserves & refill"),
        tr("What capital to keep ready: complete DGB/DD reserves. Automatic refill has separate DGB fee limits; reserve capital itself is not a fee."));
    auto* budget_scope = new QLabel(tr("Payment-model budgets and refill budgets are independent and can both be used. They are not a shared daily wallet cap. Unused allowances are not spent."));
    budget_scope->setObjectName("paymasterSettingsBudgetScope");
    budget_scope->setWordWrap(true);
    service_settings->addWidget(budget_scope);
    auto* technical_settings = settings_group(tr("Node & wallet"));
    add_settings_page(technical_settings, pages.connection_scroll, pages.connection_layout, tr("Node connection"),
        tr("Edit node settings and review their sources and reachability."));
    add_settings_page(technical_settings, pages.management_scroll, pages.management_layout, tr("Wallet & backup"),
        tr("Manage wallet access and protect the provider with a full-wallet backup."));
    // This detail view belongs to Node connection, not the Settings menu.
    auto* diagnostics_back = new QPushButton(tr("Back to node connection"), pages.diagnostics_column);
    diagnostics_back->setObjectName("paymasterDiagnosticsBack");
    pages.diagnostics_layout->insertWidget(0, diagnostics_back, 0, Qt::AlignLeft);
    connect(diagnostics_back, &QPushButton::clicked, this, [this] { showOperatorPage(m_connection_page); });
    m_settings_tabs->addWidget(pages.diagnostics_scroll);
    auto* connection_test = new QPushButton(tr("Test connection from another node…"), pages.network_group);
    connection_test->setObjectName("paymasterConnectionDiagnosticsLink");
    connection_test->setProperty("paymasterRole", QStringLiteral("primaryAction"));
    pages.network_layout->insertWidget(1, connection_test, 0, Qt::AlignLeft);
    connect(connection_test, &QPushButton::clicked, this, [this] { showOperatorPage(m_diagnostics_page); });

    const auto related_settings = [this](QVBoxLayout* layout, const QString& title,
                                         const char* name, QWidget* destination) {
        auto* link = new QPushButton(title);
        link->setObjectName(QString::fromLatin1(name));
        layout->insertWidget(3, link, 0, Qt::AlignLeft);
        connect(link, &QPushButton::clicked, this, [this, destination] { showOperatorPage(destination); });
    };
    related_settings(pages.configuration_layout, tr("Review customer-payment budgets"), "paymasterOfferBudgetsLink", m_safety_page);
    related_settings(pages.safety_layout, tr("Configure reserve capital and refill costs"), "paymasterBudgetsReservesLink", m_liquidity_page);
    related_settings(pages.liquidity_layout, tr("Review customer-payment budgets"), "paymasterReservesBudgetsLink", m_safety_page);
    related_settings(pages.automation_layout, tr("Configure automatic reserve refill"), "paymasterStartupReservesLink", m_liquidity_page);

    auto* refill_controls = new QGroupBox(tr("Automatic refill — saved settings"), pages.liquidity_column);
    refill_controls->setObjectName("paymasterSavedRefillControls");
    refill_controls->setProperty("paymasterRole", QStringLiteral("card"));
    auto* refill_layout = new QVBoxLayout(refill_controls);
    auto* saved_refill_help = new QLabel(tr("This switch applies to the saved reserve targets and fee approval immediately. Edit and save targets or fees in the form below. A stopped provider must be started separately."), refill_controls);
    saved_refill_help->setWordWrap(true);
    refill_layout->addWidget(saved_refill_help);
    m_liquidity.addQuickRefill(refill_layout, refill_controls, false);
    pages.liquidity_layout->insertWidget(pages.liquidity_layout->indexOf(pages.automatic_maintenance), refill_controls);
    auto* lifecycle_link = new QPushButton(tr("Start, pause, stop or retire Paymaster — open Settings"), pages.liquidity_column);
    lifecycle_link->setObjectName("paymasterLiquidityLifecycleLink");
    pages.liquidity_layout->addWidget(lifecycle_link, 0, Qt::AlignLeft);
    connect(lifecycle_link, &QPushButton::clicked, this, [this] { showOperatorPage(m_lifecycle_page); });
    settings_layout->addStretch();
    pages.management_layout->addStretch();
    pages.management_scroll->setWidget(pages.management);
    ConfigureScrollArea(pages.management_scroll, pages.management);
    pages.connection_layout->addStretch();
    pages.connection_scroll->setWidget(pages.connection);
    ConfigureScrollArea(pages.connection_scroll, pages.connection);
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
        m_overview.updateAutomaticRefreshTimer();
    });
    m_navigation->setCurrentRow(0);
    updateNavigationLayout();
    // Keep the same guarded task visible when the user reviews another area.
    m_overview.attachTaskCard(outer);
    delete pages.task_actions;

    m_overview.connectOverview(pages);
    m_configuration.connectConfiguration(pages);
    m_liquidity.connectLiquidity(pages);
    m_finance.connectFinance(pages);
    m_recovery.connectRecovery(pages);
    m_setup.connectSetup(pages);
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
    m_configuration.refreshFundingSafetyDisplays();
    m_configuration.updateClientSafetyDisplay();
    m_configuration.updatePolicyDisplay();
    m_liquidity.updateLiquidityDisplay();
    m_finance.updateFinanceControls();
    m_setup.updateSetupAccess();
    m_overview.updateProviderButtons();

    m_setup.initializeStatusTimer();

}

DigiDollarPaymasterWidget* CreatePaymasterWidget(QWidget* parent)
{
return new PaymasterWidgetImpl(parent);
}

void ConfigurePaymasterScrollArea(QScrollArea* scroll, QWidget* contents)
{
ConfigureScrollArea(scroll, contents);
}
