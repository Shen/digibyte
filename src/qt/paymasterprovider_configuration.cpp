// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

bool PaymasterWidgetImpl::providerConfigurationComplete() const
{
    return std::none_of(m_readiness_errors.cbegin(),
                        m_readiness_errors.cend(),
                        isProviderSetupReadinessError);
}

void PaymasterWidgetImpl::restorePolicyDefaults()
{
    applyRecommendedPolicyDefaults(/*mark_dirty=*/true);
}

void PaymasterWidgetImpl::applyRecommendedPolicyDefaults(bool mark_dirty)
{
    if (m_review_policy_limits) showPolicySaveStatus({});
    if (m_policy_limits_review) m_policy_limits_review->hide();
    m_loading_policy = true;
    m_scope->setCurrentIndex(m_scope->findData(QStringLiteral("public")));
    m_user_paid->setChecked(true);
    m_sponsored->setChecked(false);
    m_fee_bps->setValue(50);
    m_maximum_user_paid_service_fee->setValue(0);
    m_min_amount->setValue(100);
    m_max_amount->setValue(100000);
    m_quote_ttl->setValue(60);
    m_network_fee->setValue(20000000);
    m_loading_policy = false;
    m_policy_loaded = false;
    m_policy_dirty = mark_dirty;
    updatePolicyDisplay();
    updateAutomaticRefreshTimer();
}

void PaymasterWidgetImpl::setPolicyMutationEnabled(bool enabled)
{
    const bool controls_enabled = enabled && !m_busy;
    m_sponsored->setEnabled(controls_enabled);
    m_user_paid->setEnabled(controls_enabled);
    m_scope->setEnabled(controls_enabled && m_sponsored->isChecked());
    for (QSpinBox* control : {
             m_fee_bps, m_maximum_user_paid_service_fee,
             m_min_amount, m_max_amount,
             m_quote_ttl, m_network_fee}) {
        control->setReadOnly(!controls_enabled);
        control->setProperty("paymasterInvalidPersistedValue",
                             !enabled);
    }
    m_fee_bps->setEnabled(controls_enabled && m_user_paid->isChecked());
    m_maximum_user_paid_service_fee->setEnabled(
        controls_enabled && m_user_paid->isChecked());
    if (m_restore_policy_defaults) {
        m_restore_policy_defaults->setEnabled(controls_enabled);
    }
    if (m_save_policy) {
        m_save_policy->setEnabled(enabled && !m_busy);
    }
    if (m_review_policy_limits) m_review_policy_limits->setEnabled(controls_enabled);
}

void PaymasterWidgetImpl::loadPolicy(const UniValue& policy)
{
    if (!IsCompleteProviderPolicy(policy)) return;
    showPolicySaveStatus({});
    m_policy_limits_review->hide();
    int fee_rate_bps{0};
    qint64 maximum_user_paid_service_fee{0};
    int min_amount_cents{0};
    int max_amount_cents{0};
    int quote_ttl{0};
    qint64 maximum_network_fee{0};
    const bool representable =
        GetIntField(policy, "fee_rate_bps", fee_rate_bps) &&
        (policy.find_value(
             "maximum_user_paid_service_fee_cents").isNull() ||
         GetInt64Field(policy,
                       "maximum_user_paid_service_fee_cents",
                       maximum_user_paid_service_fee)) &&
        GetIntField(policy, "min_amount_cents", min_amount_cents) &&
        GetIntField(policy, "max_amount_cents", max_amount_cents) &&
        GetIntField(policy, "quote_ttl", quote_ttl) &&
        GetInt64Field(policy, "maximum_network_fee_dgb_satoshis",
                      maximum_network_fee) &&
        spinCanRepresent(m_fee_bps, fee_rate_bps) &&
        spinCanRepresent(m_maximum_user_paid_service_fee,
                         maximum_user_paid_service_fee) &&
        spinCanRepresent(m_min_amount, min_amount_cents) &&
        spinCanRepresent(m_max_amount, max_amount_cents) &&
        spinCanRepresent(m_quote_ttl, quote_ttl) &&
        spinCanRepresent(m_network_fee, maximum_network_fee);
    if (!representable) {
        // Preserve the complete authoritative object instead of feeding an
        // out-of-range qint64 through QSpinBox::setValue(), which would
        // silently clamp it and make a later Save overwrite Core state.
        m_unrepresentable_policy_snapshot = policy;
        m_policy_snapshot_representable = false;
        m_policy_loaded = false;
        m_policy_dirty = false;
        // Populate only fields that round-trip exactly. The complete raw
        // object remains authoritative and no setter is enabled, while
        // the wizard can still describe the unaffected imported choices.
        m_loading_policy = true;
        bool raw_sponsored{false};
        bool raw_user_paid{false};
        for (const UniValue& model :
             policy.find_value("funding_models").getValues()) {
            raw_sponsored |= model.get_str() == "sponsored";
            raw_user_paid |= model.get_str() == "user_paid";
        }
        m_sponsored->setChecked(raw_sponsored);
        m_user_paid->setChecked(raw_user_paid);
        const int scope_index = m_scope->findData(
            QString::fromStdString(
                policy.find_value("sponsorship_scope").get_str()));
        if (scope_index >= 0) m_scope->setCurrentIndex(scope_index);
        if (spinCanRepresent(m_fee_bps, fee_rate_bps)) {
            m_fee_bps->setValue(fee_rate_bps);
        }
        if (spinCanRepresent(m_maximum_user_paid_service_fee,
                             maximum_user_paid_service_fee)) {
            m_maximum_user_paid_service_fee->setValue(
                static_cast<int>(maximum_user_paid_service_fee));
        }
        if (spinCanRepresent(m_min_amount, min_amount_cents)) {
            m_min_amount->setValue(min_amount_cents);
        }
        if (spinCanRepresent(m_max_amount, max_amount_cents)) {
            m_max_amount->setValue(max_amount_cents);
        }
        if (spinCanRepresent(m_quote_ttl, quote_ttl)) {
            m_quote_ttl->setValue(quote_ttl);
        }
        if (spinCanRepresent(m_network_fee, maximum_network_fee)) {
            m_network_fee->setValue(
                static_cast<int>(maximum_network_fee));
        }
        m_loading_policy = false;
        setPolicyMutationEnabled(false);
        QString invalid_field;
        QString raw_value;
        if (!spinCanRepresent(m_fee_bps, fee_rate_bps)) {
            invalid_field = tr("service fee");
            raw_value = QString::number(fee_rate_bps);
        } else if (!spinCanRepresent(m_maximum_user_paid_service_fee,
                                     maximum_user_paid_service_fee)) {
            invalid_field = tr("maximum user-paid service fee");
            raw_value = QString::number(maximum_user_paid_service_fee);
        } else if (!spinCanRepresent(m_min_amount,
                                     min_amount_cents)) {
            invalid_field = tr("minimum payment");
            raw_value = QString::number(min_amount_cents);
        } else if (!spinCanRepresent(m_max_amount,
                                     max_amount_cents)) {
            invalid_field = tr("maximum payment");
            raw_value = QString::number(max_amount_cents);
        } else if (!spinCanRepresent(m_quote_ttl, quote_ttl)) {
            invalid_field = tr("quote lifetime");
            raw_value = QString::number(quote_ttl);
        } else {
            invalid_field = tr("maximum network fee");
            raw_value = QString::number(maximum_network_fee);
        }
        m_funding_model_status->setText(tr(
            "The persisted provider policy contains a %1 value (%2) that this interface cannot represent exactly. The original Core policy is retained and all policy mutation is disabled.")
            .arg(invalid_field, raw_value));
        updateProviderButtons();
        updateAutomaticRefreshTimer();
        return;
    }
    const UniValue& funding_models = policy.find_value("funding_models");

    bool sponsored{false};
    bool user_paid{false};
    for (const UniValue& model : funding_models.getValues()) {
        if (!model.isStr()) continue;
        sponsored |= model.get_str() == "sponsored";
        user_paid |= model.get_str() == "user_paid";
    }

    m_loading_policy = true;
    m_policy_snapshot_representable = true;
    m_unrepresentable_policy_snapshot = UniValue{UniValue::VOBJ};
    setPolicyMutationEnabled(true);
    m_sponsored->setChecked(sponsored);
    m_user_paid->setChecked(user_paid);
    const UniValue& scope = policy.find_value("sponsorship_scope");
    if (scope.isStr()) {
        // The parked protocol value is not an advertised option. Preserve
        // an actual legacy policy until the operator explicitly changes it.
        const int legacy_index = m_scope->findData(QStringLiteral("restricted"));
        if (legacy_index >= 0) m_scope->removeItem(legacy_index);
        if (scope.get_str() == "restricted") {
            m_scope->addItem(tr("Unsupported saved sponsorship setting"), QStringLiteral("restricted"));
            static_cast<QStandardItemModel*>(m_scope->model())->item(m_scope->count() - 1)->setEnabled(false);
        }
        const int scope_index = m_scope->findData(QString::fromStdString(scope.get_str()));
        if (scope_index >= 0) m_scope->setCurrentIndex(scope_index);
    }
    m_fee_bps->setValue(fee_rate_bps);
    m_maximum_user_paid_service_fee->setValue(
        static_cast<int>(maximum_user_paid_service_fee));
    m_min_amount->setValue(min_amount_cents);
    m_max_amount->setValue(max_amount_cents);
    m_quote_ttl->setValue(quote_ttl);
    m_network_fee->setValue(static_cast<int>(maximum_network_fee));
    m_loading_policy = false;
    m_policy_loaded = true;
    m_policy_dirty = false;
    updatePolicyDisplay();
    updateAutomaticRefreshTimer();
}

QWidget* PaymasterWidgetImpl::configurationWarningPanel(QLabel*& label, const char* name, const QString& button_text, std::function<void()> navigate)
{
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    label = new QLabel(panel);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setProperty("paymasterRole", QStringLiteral("statusText"));
    layout->addWidget(label);
    auto* link = new QPushButton(button_text, panel);
    link->setObjectName(QString::fromLatin1(name) + QStringLiteral("Link"));
    layout->addWidget(link, 0, Qt::AlignLeft);
    connect(link, &QPushButton::clicked, this, [this, navigate] { if (!m_busy && !m_privacy) navigate(); });
    panel->hide();
    return panel;
}

QString PaymasterWidgetImpl::configurationConflictText(const UniValue& offer, const UniValue& safety) const
{
    QStringList lines;
    const auto conflicts = DigiDollar::Paymaster::OperatorFeeLimitConflicts(offer, safety);
    for (const auto& conflict : conflicts.getValues()) {
        const auto model = conflict.find_value("model").get_str();
        const QString name = model == "user_paid" ? tr("User paid") : model == "public_sponsored" ? tr("Public sponsored") : tr("Restricted sponsored");
        const auto ceiling = conflict.find_value("offer_fee_ceiling_satoshis").getInt<int64_t>();
        const auto limit = conflict.find_value("payment_fee_limit_satoshis").getInt<int64_t>();
        lines << tr("%1: offer network-fee ceiling %2 DGB; per-payment safety limit %3 DGB.")
            .arg(name, PaymasterFormatDGB(ceiling), PaymasterFormatDGB(limit));
        lines << (limit < ceiling
            ? tr("This payment model cannot serve with this offer ceiling. Review the offer ceiling and spending limits; waiting for the rolling budget does not resolve this configuration conflict.")
            : tr("The per-payment limit exceeds the offer ceiling. Core requires it to be at most the offer ceiling, including limits saved for inactive models. Review both settings."));
    }
    return lines.join(QLatin1Char('\n'));
}

void PaymasterWidgetImpl::updateConfigurationWarnings()
{
    if (!m_operator_config_warning || !m_policy_config_warning || !m_safety_config_warning ||
        !m_restricted_sponsored_safety.completed_per_day) return;
    const auto display = [this](QLabel* label, const QString& heading, const QString& detail) {
        label->parentWidget()->setVisible(!m_privacy && !detail.isEmpty());
        setStatusLabel(label, m_privacy || detail.isEmpty() ? QString{} : heading + QLatin1Char('\n') + detail, QStringLiteral("action"));
    };
    const auto& saved_offer = m_operator_snapshot.find_value("provider").find_value("policy");
    const auto& saved_safety = m_operator_snapshot.find_value("safety").find_value("policy");
    display(m_operator_config_warning, tr("Saved configuration conflict"), configurationConflictText(saved_offer, saved_safety));
    UniValue offer{UniValue::VOBJ}, models{UniValue::VARR};
    if (m_user_paid->isChecked()) models.push_back("user_paid");
    if (m_sponsored->isChecked()) models.push_back("sponsored");
    offer.pushKV("funding_models", models);
    offer.pushKV("sponsorship_scope", m_scope->currentData().toString().toStdString());
    offer.pushKV("maximum_network_fee_dgb_satoshis", m_network_fee->value());
    display(m_policy_config_warning, tr("Offer form compared with saved spending limits"),
        configurationConflictText(m_policy_snapshot_representable ? offer : saved_offer, saved_safety));
    FundingSafetyValues paid, sponsored, restricted;
    UniValue safety = saved_safety;
    if (m_provider_safety_snapshot_representable && readFundingSafety(m_user_paid_safety, paid) &&
        readFundingSafety(m_public_sponsored_safety, sponsored) && readFundingSafety(m_restricted_sponsored_safety, restricted)) {
        safety = UniValue{UniValue::VOBJ};
        safety.pushKV("user_paid", fundingSafetyToJSON(paid));
        safety.pushKV("public_sponsored", fundingSafetyToJSON(sponsored));
        safety.pushKV("restricted_sponsored", fundingSafetyToJSON(restricted));
    }
    const QString input_error = providerSafetyInputError();
    display(m_safety_config_warning, tr("Spending-limit form compared with saved offer"),
        input_error.isEmpty() ? configurationConflictText(saved_offer, safety) : input_error);
}

void PaymasterWidgetImpl::updatePolicyDisplay()
{
    updateConfigurationWarnings();
    const bool user_paid = m_user_paid->isChecked();
    m_user_paid_pricing->setVisible(user_paid);
    m_save_policy->setText(m_core_running ? tr("Save and restart provider…") : tr("Save policy"));
    if (m_fee_example) {
        m_fee_example->setFeeRate(
            m_fee_bps->value(), user_paid,
            m_maximum_user_paid_service_fee->value());
    }
    const bool sponsored = m_sponsored->isChecked();
    const bool restricted = m_scope->currentData().toString() == QLatin1String("restricted");
    m_scope->setVisible(sponsored || restricted);
    m_sponsorship_label->setVisible(sponsored || restricted);
    // A complete but future/out-of-range Core policy is deliberately
    // retained outside the QSpinBox controls. Never let a later display
    // refresh re-enable individual editors from that read-only snapshot.
    m_scope->setEnabled(!m_busy && m_policy_snapshot_representable &&
                        (sponsored || restricted));
    m_fee_bps->setEnabled(!m_busy && m_policy_snapshot_representable &&
                          user_paid);
    m_maximum_user_paid_service_fee->setEnabled(
        !m_busy && m_policy_snapshot_representable && user_paid);

    QString model_text;
    if (restricted) {
        const QString message = tr("The saved sponsorship setting is unsupported. Choose Public or a user-paid offer and review its limits before saving. Saved settings have not been converted automatically.");
        m_funding_model_status->setText(message);
        m_policy_summary->setText(message);
        return;
    }
    if (user_paid && sponsored) {
        model_text = tr("User paid and public sponsored");
        m_funding_model_status->setText(tr(
            "Both models may be active together. User-paid transfers charge the configured DigiDollar service fee; public sponsored transfers charge no DigiDollar service fee. Each model has separate safety budgets."));
    } else if (user_paid) {
        model_text = tr("User paid only");
        m_funding_model_status->setText(tr(
            "Clients pay the configured DigiDollar service fee while this provider supplies the DGB network fee."));
    } else if (sponsored) {
        model_text = restricted ? tr("Restricted sponsored only")
                                : tr("Public sponsored only");
        m_funding_model_status->setText(restricted
            ? tr("Only clients with a valid invitation may use this sponsored service. No DigiDollar service fee is charged.")
            : tr("Discovered clients may use this sponsored service. No DigiDollar service fee is charged; finite safety budgets are essential."));
    } else {
        m_funding_model_status->setText(tr("Select at least one payment model."));
        m_policy_summary->setText(tr(
            "No payment model selected. Select at least one model before saving."));
        return;
    }
    if (user_paid && m_fee_bps->value() == 0) {
        m_funding_model_status->setText(m_funding_model_status->text() + QLatin1Char('\n') + tr(
            "User paid is set to 0%: clients pay no DD service fee. Choose a positive rate to charge a fee; switching payment models does not restore a previous rate."));
    }

    const QString minimum = PaymasterFormatDD(m_min_amount->value());
    const QString maximum = PaymasterFormatDD(m_max_amount->value());
    const QString service_fee = PaymasterFormatDD(m_fee_bps->value());
    const QString maximum_service_fee =
        m_maximum_user_paid_service_fee->value() == 0
            ? tr("no cap")
            : PaymasterFormatDD(m_maximum_user_paid_service_fee->value()) + tr(" DD");
    const QString network_fee = PaymasterFormatDGB(m_network_fee->value());
    const QString persistence = m_policy_dirty
        ? tr("Unsaved changes — save the policy to activate them.")
        : m_policy_readback_pending
            ? tr("Save acknowledged — waiting for the updated policy status.")
        : m_policy_loaded
            ? tr("These are the currently saved settings.")
            : tr("Recommended starting values — save the policy to activate them.");
    const QString pricing = user_paid
        ? tr("User-paid service fee %1% (maximum %2).").arg(service_fee, maximum_service_fee)
        : tr("No DD service fee is charged.");
    m_policy_summary->setText(
        tr("%1; payments from %2 to %3 DD. %4 Quotes valid for %5 seconds; DGB network-fee ceiling %6 DGB per transfer. %7")
            .arg(model_text, minimum, maximum, pricing).arg(m_quote_ttl->value()).arg(network_fee, persistence));
}

PaymasterWidgetImpl::FundingSafetyControls PaymasterWidgetImpl::createFundingSafetyControls(
    QTabWidget* tabs, const QString& title, const QString& object_prefix,
    bool recommended_defaults)
{
    auto* page = new QWidget(tabs);
    page->setObjectName(object_prefix + QStringLiteral("Page"));
    auto* form = new QFormLayout(page);
    form->setContentsMargins(14, 14, 14, 14);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(9);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QString explanation;
    if (object_prefix == QLatin1String("paymasterSafetyUserPaid")) {
        explanation = tr(
            "Use these limits when clients compensate this provider with a DigiDollar service fee. The provider still spends DGB for network fees, so finite DGB budgets remain necessary.");
    } else if (object_prefix == QLatin1String("paymasterSafetyPublicSponsored")) {
        explanation = tr(
            "Public sponsorship is available to discovered clients and therefore has the greatest budget-exhaustion risk. Its safe default is disabled; enable it only with finite values you can afford to lose.");
    } else {
        explanation = tr(
            "Restricted sponsorship requires a provider-issued invitation but still needs finite limits. Its safe default is disabled until you deliberately configure a budget.");
    }
    auto* help = new QLabel(explanation, page);
    help->setObjectName(object_prefix + QStringLiteral("Explanation"));
    help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    help->setWordWrap(true);
    form->addRow(help);
    FundingSafetyControls controls;
    controls.per_transaction = dgbAmountField(
        page, recommended_defaults ? m_network_fee->value() : 0);
    controls.reserved = dgbAmountField(page, recommended_defaults ? 100000000 : 0);
    controls.per_hour = dgbAmountField(page, recommended_defaults ? 200000000 : 0);
    controls.per_day = dgbAmountField(page, recommended_defaults ? 1000000000 : 0);
    controls.completed_per_hour = spin(page, 0, 1000000, recommended_defaults ? 10 : 0);
    controls.completed_per_day = spin(page, 0, 1000000, recommended_defaults ? 100 : 0);
    controls.per_transaction->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerTransaction"));
    controls.reserved->setObjectName(object_prefix + QStringLiteral("MaxReservedNetworkFee"));
    controls.per_hour->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerHour"));
    controls.per_day->setObjectName(object_prefix + QStringLiteral("MaxNetworkFeePerDay"));
    controls.completed_per_hour->setObjectName(object_prefix + QStringLiteral("MaxCompletedPerHour"));
    controls.completed_per_day->setObjectName(object_prefix + QStringLiteral("MaxCompletedPerDay"));
    controls.per_transaction->setToolTip(tr("Maximum DGB network fee committed by one transfer."));
    controls.reserved->setToolTip(tr("Maximum DGB network fees reserved across simultaneous open quotes."));
    controls.per_hour->setToolTip(tr("Maximum cumulative DGB network fees in any rolling hour."));
    controls.per_day->setToolTip(tr("Maximum cumulative DGB network fees in any rolling 24 hours."));
    controls.completed_per_hour->setToolTip(tr("Maximum completed transfers in any rolling hour."));
    controls.completed_per_day->setToolTip(tr("Maximum completed transfers in any rolling 24 hours."));
    form->addRow(tr("Maximum per transfer (DGB):"), controls.per_transaction);

    form->addRow(tr("Maximum per rolling hour (DGB):"), controls.per_hour);
    form->addRow(tr("Maximum per rolling day (DGB):"), controls.per_day);
    auto* advanced_container = new QWidget(page);
    auto* advanced_layout = new QVBoxLayout(advanced_container);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    auto* advanced = new QWidget(advanced_container);
    auto* advanced_form = new QFormLayout(advanced);
    advanced_form->addRow(tr("Maximum reserved at once (DGB):"), controls.reserved);
    advanced_form->addRow(tr("Completed transfers per rolling hour:"), controls.completed_per_hour);
    advanced_form->addRow(tr("Completed transfers per rolling day:"), controls.completed_per_day);
    AddPaymasterDisclosure(advanced_layout, advanced_container, advanced,
                           tr("Reservation and transaction-count limits"), tr("Hide additional limits"), object_prefix + QStringLiteral("AdvancedToggle"));
    form->addRow(advanced_container);
    controls.mode = new QLabel(page);
    controls.mode->setObjectName(object_prefix + QStringLiteral("Mode"));
    controls.mode->setWordWrap(true);
    form->addRow(controls.mode);
    auto* restore_defaults = new QPushButton(
        recommended_defaults ? tr("Restore recommended limits")
                             : tr("Restore safe disabled defaults"), page);
    restore_defaults->setObjectName(object_prefix + QStringLiteral("RestoreDefaults"));
    restore_defaults->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    restore_defaults->setToolTip(recommended_defaults
        ? tr("Restore the recommended finite starting limits for this model. Save the provider safety policy to activate them.")
        : tr("Set all six limits to zero so this unused model is safely disabled. Save the provider safety policy to activate the change."));
    form->addRow(restore_defaults);
    connect(restore_defaults, &QPushButton::clicked, this,
            [this, controls, recommended_defaults] {
                restoreFundingSafetyDefaults(controls, recommended_defaults);
            });
    tabs->addTab(page, title);
    return controls;
}

void PaymasterWidgetImpl::restoreFundingSafetyDefaults(const FundingSafetyControls& controls,
                                  bool recommended_defaults,
                                  bool mark_dirty)
{
    m_loading_provider_safety = true;
    static_cast<DgbAmountLineEdit*>(controls.per_transaction)->setSatoshis(
        recommended_defaults ? m_network_fee->value() : 0);
    static_cast<DgbAmountLineEdit*>(controls.reserved)->setSatoshis(
        recommended_defaults ? 100000000 : 0);
    static_cast<DgbAmountLineEdit*>(controls.per_hour)->setSatoshis(
        recommended_defaults ? 200000000 : 0);
    static_cast<DgbAmountLineEdit*>(controls.per_day)->setSatoshis(
        recommended_defaults ? 1000000000 : 0);
    controls.completed_per_hour->setValue(recommended_defaults ? 10 : 0);
    controls.completed_per_day->setValue(recommended_defaults ? 100 : 0);
    m_loading_provider_safety = false;
    if (mark_dirty) {
        markProviderSafetyDirty();
    } else {
        m_provider_safety_dirty = false;
    }
    updateFundingSafetyDisplay(controls);
}

void PaymasterWidgetImpl::restoreQuoteSafetyDefaults(bool mark_dirty)
{
    m_loading_provider_safety = true;
    m_max_active_quotes_total->setValue(16);
    m_max_active_quotes_per_netgroup->setValue(4);
    m_max_active_quotes_per_recipient->setValue(2);
    m_max_quote_requests_per_netgroup->setValue(10);
    m_loading_provider_safety = false;
    if (mark_dirty) {
        markProviderSafetyDirty();
    } else {
        m_provider_safety_dirty = false;
    }
}

void PaymasterWidgetImpl::restoreClientSafetyDefaults(bool mark_dirty)
{
    m_loading_client_safety = true;
    m_client_fee_per_transaction->setValue(100);
    m_client_fee_per_day->setValue(1000);
    m_loading_client_safety = false;
    if (mark_dirty) {
        markClientSafetyDirty();
    } else {
        m_client_safety_dirty = false;
    }
    updateClientSafetyDisplay();
}

void PaymasterWidgetImpl::markProviderSafetyDirty()
{
    m_provider_safety_dirty = true;
    if (!m_policy_limits_review->isHidden()) {
        setStatusLabel(m_policy_limits_hint, tr("Spending limits have unsaved changes. Review and save them, then return to save the offer."), QStringLiteral("action"));
    }
    m_provider_safety_status->setText(tr(
        "Unsaved provider safety changes — the previously saved limits remain active until these values are saved."));
    updateProviderButtons();
}

void PaymasterWidgetImpl::markClientSafetyDirty()
{
    m_client_safety_dirty = true;
    m_client_safety_status->setText(tr(
        "Unsaved client safety changes — the previously saved limits remain active until these values are saved."));
}

void PaymasterWidgetImpl::updateFundingSafetyDisplay(const FundingSafetyControls& controls,
                                bool persisted)
{
    if (!controls.mode) return;
    FundingSafetyValues values;
    if (!readFundingSafety(controls, values)) {
        controls.mode->setText(tr(
            "Not saved — one or more amounts are outside the supported whole-satoshi range."));
    } else if (values.allZero()) {
        controls.mode->setText(tr(
            "Disabled — all limits are zero; zero never means unlimited."));
    } else if (values.hasZero()) {
        controls.mode->setText(tr(
            "Incomplete — an enabled model requires all six limits to be greater than zero. Use all zero values only to disable the model."));
    } else {
        const QString per_transaction = PaymasterFormatDGB(values.per_transaction);
        const QString per_hour = PaymasterFormatDGB(values.per_hour);
        const QString per_day = PaymasterFormatDGB(values.per_day);
        controls.mode->setText(persisted
            ? tr("Active finite limits — at most %1 DGB per transfer, %2 DGB per rolling hour and %3 DGB per rolling day.")
                  .arg(per_transaction, per_hour, per_day)
            : tr("Unsaved limits — at most %1 DGB per transfer, %2 DGB per rolling hour and %3 DGB per rolling day. Save the provider safety policy to activate them.")
                  .arg(per_transaction, per_hour, per_day));
    }
}

void PaymasterWidgetImpl::updateClientSafetyDisplay(bool persisted)
{
    if (!m_client_safety_mode) return;
    if (m_client_fee_per_transaction->value() == 0 &&
        m_client_fee_per_day->value() == 0) {
        m_client_safety_mode->setText(tr(
            "Zero service-fee budget — only zero-fee authorizations fit; zero never means unlimited."));
    } else if (persisted) {
        m_client_safety_mode->setText(tr(
            "Active limits — at most %1 DD per transfer and %2 DD in a rolling day.")
            .arg(PaymasterFormatDD(m_client_fee_per_transaction->value()),
                 PaymasterFormatDD(m_client_fee_per_day->value())));
    } else {
        m_client_safety_mode->setText(tr(
            "Unsaved limits — at most %1 DD per transfer and %2 DD in a rolling day. Save the client safety policy to activate them.")
            .arg(PaymasterFormatDD(m_client_fee_per_transaction->value()),
                 PaymasterFormatDD(m_client_fee_per_day->value())));
    }
}

void PaymasterWidgetImpl::watchFundingSafetyControls(const FundingSafetyControls& controls)
{
    for (QLineEdit* control : {controls.per_transaction, controls.reserved,
                               controls.per_hour, controls.per_day}) {
        connect(control, &QLineEdit::textChanged, this, [this, controls] {
            if (!m_loading_provider_safety) markProviderSafetyDirty();
            updateFundingSafetyDisplay(controls);
            updateProviderButtons();
        });
    }
    for (QSpinBox* control : {controls.completed_per_hour,
                              controls.completed_per_day}) {
        connect(control, qOverload<int>(&QSpinBox::valueChanged), this, [this, controls] {
            if (!m_loading_provider_safety) markProviderSafetyDirty();
            updateFundingSafetyDisplay(controls);
            updateProviderButtons();
        });
    }
}

bool PaymasterWidgetImpl::readFundingSafety(const FundingSafetyControls& controls,
                              FundingSafetyValues& values)
{
    bool ok_per_transaction{false};
    bool ok_reserved{false};
    bool ok_per_hour{false};
    bool ok_per_day{false};
    ok_per_transaction = static_cast<DgbAmountLineEdit*>(
        controls.per_transaction)->satoshis(values.per_transaction);
    ok_reserved = static_cast<DgbAmountLineEdit*>(
        controls.reserved)->satoshis(values.reserved);
    ok_per_hour = static_cast<DgbAmountLineEdit*>(
        controls.per_hour)->satoshis(values.per_hour);
    ok_per_day = static_cast<DgbAmountLineEdit*>(
        controls.per_day)->satoshis(values.per_day);
    values.completed_per_hour = controls.completed_per_hour->value();
    values.completed_per_day = controls.completed_per_day->value();
    return ok_per_transaction && ok_reserved && ok_per_hour && ok_per_day;
}

UniValue PaymasterWidgetImpl::fundingSafetyToJSON(const FundingSafetyValues& values)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_network_fee_per_transaction_satoshis", values.per_transaction);
    result.pushKV("maximum_reserved_network_fee_satoshis", values.reserved);
    result.pushKV("maximum_network_fee_per_hour_satoshis", values.per_hour);
    result.pushKV("maximum_network_fee_per_day_satoshis", values.per_day);
    result.pushKV("maximum_completed_per_hour", values.completed_per_hour);
    result.pushKV("maximum_completed_per_day", values.completed_per_day);
    return result;
}

bool PaymasterWidgetImpl::fundingSafetyCanRepresent(
    const UniValue& value, const FundingSafetyControls& controls)
{
    if (!IsCompleteFundingSafety(value)) return false;
    qint64 per_transaction{0};
    qint64 reserved{0};
    qint64 per_hour{0};
    qint64 per_day{0};
    int completed_per_hour{0};
    int completed_per_day{0};
    return GetInt64Field(
               value,
               "maximum_network_fee_per_transaction_satoshis",
               per_transaction) &&
        GetInt64Field(value,
                      "maximum_reserved_network_fee_satoshis",
                      reserved) &&
        GetInt64Field(value,
                      "maximum_network_fee_per_hour_satoshis",
                      per_hour) &&
        GetInt64Field(value,
                      "maximum_network_fee_per_day_satoshis",
                      per_day) &&
        GetIntField(value, "maximum_completed_per_hour",
                    completed_per_hour) &&
        GetIntField(value, "maximum_completed_per_day",
                    completed_per_day) &&
        dgbAmountCanRepresent(per_transaction) &&
        dgbAmountCanRepresent(reserved) &&
        dgbAmountCanRepresent(per_hour) &&
        dgbAmountCanRepresent(per_day) &&
        spinCanRepresent(controls.completed_per_hour,
                         completed_per_hour) &&
        spinCanRepresent(controls.completed_per_day,
                         completed_per_day);
}

bool PaymasterWidgetImpl::providerSafetyCanRepresent(const UniValue& policy) const
{
    int maximum_active_quotes_total{0};
    int maximum_active_quotes_per_netgroup{0};
    int maximum_active_quotes_per_recipient{0};
    int maximum_quote_requests_per_netgroup{0};
    return IsCompleteProviderSafetyPolicy(policy) &&
        fundingSafetyCanRepresent(policy.find_value("user_paid"),
                                  m_user_paid_safety) &&
        fundingSafetyCanRepresent(
            policy.find_value("public_sponsored"),
            m_public_sponsored_safety) &&
        fundingSafetyCanRepresent(
            policy.find_value("restricted_sponsored"),
            m_restricted_sponsored_safety) &&
        GetIntField(policy, "maximum_active_quotes_total",
                    maximum_active_quotes_total) &&
        GetIntField(policy, "maximum_active_quotes_per_netgroup",
                    maximum_active_quotes_per_netgroup) &&
        GetIntField(policy, "maximum_active_quotes_per_recipient",
                    maximum_active_quotes_per_recipient) &&
        GetIntField(
            policy,
            "maximum_quote_requests_per_netgroup_per_minute",
            maximum_quote_requests_per_netgroup) &&
        spinCanRepresent(m_max_active_quotes_total,
                         maximum_active_quotes_total) &&
        spinCanRepresent(m_max_active_quotes_per_netgroup,
                         maximum_active_quotes_per_netgroup) &&
        spinCanRepresent(m_max_active_quotes_per_recipient,
                         maximum_active_quotes_per_recipient) &&
        spinCanRepresent(m_max_quote_requests_per_netgroup,
                         maximum_quote_requests_per_netgroup);
}

void PaymasterWidgetImpl::setProviderSafetyMutationEnabled(bool enabled)
{
    const bool controls_enabled = enabled && !m_busy;
    if (m_provider_safety_group) {
        m_provider_safety_group->setEnabled(controls_enabled);
    }
    for (const FundingSafetyControls* controls : {
             &m_user_paid_safety, &m_public_sponsored_safety,
             &m_restricted_sponsored_safety}) {
        for (QLineEdit* amount : {
                 controls->per_transaction, controls->reserved,
                 controls->per_hour, controls->per_day}) {
            amount->setReadOnly(!controls_enabled);
        }
        controls->completed_per_hour->setEnabled(controls_enabled);
        controls->completed_per_day->setEnabled(controls_enabled);
    }
    for (QSpinBox* control : {
             m_max_active_quotes_total,
             m_max_active_quotes_per_netgroup,
             m_max_active_quotes_per_recipient,
             m_max_quote_requests_per_netgroup}) {
        control->setEnabled(controls_enabled);
    }
    if (m_save_provider_safety) {
        m_save_provider_safety->setEnabled(enabled && !m_busy && providerSafetyInputError().isEmpty());
    }
}

QString PaymasterWidgetImpl::providerSafetyInputError() const
{
    if (!m_save_provider_safety || !m_restricted_sponsored_safety.completed_per_day) return {};
    const auto& offer = m_operator_snapshot.find_value("provider").find_value("policy");
    const auto& models = offer.find_value("funding_models");
    const auto offers = [&models](const char* name) {
        return models.isArray() && std::any_of(models.getValues().begin(), models.getValues().end(),
            [name](const UniValue& model) { return model.isStr() && model.get_str() == name; });
    };
    qint64 ceiling{0};
    const bool has_ceiling = GetInt64Field(offer, "maximum_network_fee_dgb_satoshis", ceiling) && ceiling > 0;
    const bool restricted = offer.find_value("sponsorship_scope").isStr() && offer.find_value("sponsorship_scope").get_str() == "restricted";
    const std::array<QString, 3> names{tr("User paid"), tr("Public sponsored"), tr("Restricted sponsored")};
    const std::array<bool, 3> required{offers("user_paid"), offers("sponsored") && !restricted, offers("sponsored") && restricted};
    const std::array<const FundingSafetyControls*, 3> controls{&m_user_paid_safety, &m_public_sponsored_safety, &m_restricted_sponsored_safety};
    // Form feedback only. The RPC revalidates these relationships against
    // the current saved offer and remains authoritative for every write.
    for (size_t i = 0; i < controls.size(); ++i) {
        FundingSafetyValues values;
        QString reason;
        if (!readFundingSafety(*controls[i], values))
            reason = tr("Enter valid DGB amounts with at most eight decimal places.");
        else if (values.allZero() && !required[i]) continue;
        else if (values.hasZero())
            reason = tr("An offered model needs six positive limits. Use all zero values only for a model that is not offered.");
        else if (has_ceiling && values.per_transaction > ceiling)
            reason = tr("Maximum per transfer (%1 DGB) must not exceed the saved offer ceiling (%2 DGB). Lower this limit or save a higher offer ceiling first.")
                .arg(PaymasterFormatDGB(values.per_transaction), PaymasterFormatDGB(ceiling));
        else if (values.reserved < values.per_transaction)
            reason = tr("Maximum reserved at once (%1 DGB) must cover at least one transfer (%2 DGB).")
                .arg(PaymasterFormatDGB(values.reserved), PaymasterFormatDGB(values.per_transaction));
        else if (values.per_hour < values.per_transaction)
            reason = tr("The hourly limit (%1 DGB) must cover at least one transfer (%2 DGB).")
                .arg(PaymasterFormatDGB(values.per_hour), PaymasterFormatDGB(values.per_transaction));
        else if (values.per_day < values.per_hour)
            reason = tr("The daily limit (%1 DGB) must be at least the hourly limit (%2 DGB).")
                .arg(PaymasterFormatDGB(values.per_day), PaymasterFormatDGB(values.per_hour));
        else if (values.completed_per_day < values.completed_per_hour)
            reason = tr("The daily transfer count must be at least the hourly transfer count.");
        if (!reason.isEmpty()) return tr("Cannot save %1 limits: %2").arg(names[i], reason);
    }
    if (m_max_active_quotes_per_netgroup->value() > m_max_active_quotes_total->value() ||
        m_max_active_quotes_per_recipient->value() > m_max_active_quotes_total->value())
        return tr("Cannot save: per-network-group and per-recipient offer limits must not exceed the total active-offer limit.");
    return {};
}

void PaymasterWidgetImpl::loadFundingSafety(const UniValue& value,
                              const FundingSafetyControls& controls)
{
    if (!IsCompleteFundingSafety(value)) return;
    static_cast<DgbAmountLineEdit*>(controls.per_transaction)->setSatoshis(
        value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<qint64>());
    static_cast<DgbAmountLineEdit*>(controls.reserved)->setSatoshis(
        value.find_value("maximum_reserved_network_fee_satoshis").getInt<qint64>());
    static_cast<DgbAmountLineEdit*>(controls.per_hour)->setSatoshis(
        value.find_value("maximum_network_fee_per_hour_satoshis").getInt<qint64>());
    static_cast<DgbAmountLineEdit*>(controls.per_day)->setSatoshis(
        value.find_value("maximum_network_fee_per_day_satoshis").getInt<qint64>());
    controls.completed_per_hour->setValue(
        value.find_value("maximum_completed_per_hour").getInt<int>());
    controls.completed_per_day->setValue(
        value.find_value("maximum_completed_per_day").getInt<int>());
}

bool PaymasterWidgetImpl::providerSafetyAllowsSelectedModels() const
{
    // Core is authoritative for funding-model applicability and all limit
    // relationships. Qt only prevents activation with missing or unsaved
    // wallet-local policy data.
    return m_provider_safety_configured && !m_provider_safety_dirty;
}

void PaymasterWidgetImpl::discardAutostart()
{
    m_autostart_dirty = false;
    m_autostart_message.clear();
    const QSignalBlocker blocker(m_autostart);
    m_autostart->setChecked(m_autostart_enabled);
    updateProviderButtons();
}

void PaymasterWidgetImpl::reconcileAutostart()
{
    if (m_autostart_pending_read) {
        m_autostart_pending_read = false;
        const bool mismatch = m_autostart->isChecked() != m_autostart_enabled;
        m_autostart_dirty = false;
        m_autostart_message = mismatch
            ? tr("The change was not saved. The checkbox shows the current wallet setting.")
            : tr("Autostart verified in the wallet.");
    }
    if (m_autostart_dirty && m_autostart->isChecked() == m_autostart_enabled)
        m_autostart_dirty = false;
    if (!m_autostart_dirty) {
        const QSignalBlocker blocker(m_autostart);
        m_autostart->setChecked(m_autostart_enabled);
    }
}

void PaymasterWidgetImpl::updateAutostartControls()
{
    const bool known = m_provider_info_snapshot_available && m_provider_settings_present;
    const bool available = known && hasRpcTransport() && !m_busy && !m_privacy && !m_autostart_pending_read;
    m_autostart->setEnabled(available);
    m_save_autostart->hide();
    m_discard_autostart->hide();
    m_save_autostart->setEnabled(available);
    m_discard_autostart->setEnabled(available);
    QString status = !known ? tr("Autostart is available after the saved provider settings have been read.")
        : m_autostart_enabled ? tr("Saved: autostart on.") : tr("Saved: autostart off.");
    if (m_autostart_pending_read) status = tr("Autostart could not be confirmed. Checking the saved wallet setting; do not repeat the change yet.");
    else if (m_autostart_dirty) status = tr("Saving autostart…");
    if (!m_autostart_message.isEmpty()) status += QStringLiteral("\n") + m_autostart_message;
    m_autostart_status->setText(m_privacy ? tr("Hidden by privacy mode") : status);
    if (m_settings_autostart_status) m_settings_autostart_status->setText(m_autostart_status->text());
    for (auto* control : m_autostart_copies) {
        const QSignalBlocker blocker(control);
        control->setChecked(m_autostart->isChecked());
        control->setEnabled(available);
    }
    updateQuickAutomation();
    if (m_autostart_dirty || m_autostart_pending_read) {
        m_operator_start->setEnabled(false);
        m_operator_start->setToolTip(tr("Save or discard the autostart change before starting."));
        if (m_operator_next_action == QLatin1String("start") ||
            m_operator_next_action == QLatin1String("enable") ||
            (m_operator_next_action == QLatin1String("review_liquidity") && !m_core_running))
            m_operator_action->setEnabled(false);
    }
    updateLifecycleControls();
}

void PaymasterWidgetImpl::saveAutostart()
{
    if (!hasRpcTransport() || m_busy || m_privacy || !m_autostart_dirty ||
        m_autostart_pending_read || !m_provider_info_snapshot_available || !m_provider_settings_present) return;
    const bool requested = m_autostart->isChecked();
    UniValue settings{UniValue::VOBJ}, params{UniValue::VARR};
    settings.pushKV("autostart", requested);
    params.push_back(settings);
    m_autostart_message = tr("Saving autostart…");
    const auto verify = [this] {
        m_autostart_pending_read = true;
        m_autostart_message.clear();
        updateAutostartControls();
        refreshStatus();
    };
    call("setpaymasterruntimesettings", params, false, nullptr,
        [this, requested, verify](const UniValue& result) {
            const auto& value = result.find_value("autostart");
            if (!value.isBool() || value.get_bool() != requested ||
                !result.find_value("operation_mode").isStr() || !result.find_value("running").isBool()) {
                verify();
                return;
            }
            m_autostart_enabled = requested;
            m_autostart_dirty = false;
            m_autostart_message = tr("Autostart saved.");
            updateAutostartControls();
            refreshStatus();
        }, false, [verify](const QString&) { verify(); });
}

void PaymasterWidgetImpl::saveRuntimeSettings()
{
    if (!hasRpcTransport() || m_busy || !m_runtime_settings_dirty) {
        m_runtime_settings_result->setText(!hasRpcTransport()
            ? tr("Runtime settings were not saved because no wallet RPC transport is available.")
            : m_busy
                ? tr("Runtime settings were not saved because another wallet operation is still running.")
                : tr("Runtime settings were not saved because there are no unsaved changes."));
        return;
    }
    const QString selected_mode =
        m_operation_mode_select->currentData().toString();
    UniValue settings{UniValue::VOBJ};
    settings.pushKV("operation_mode", selected_mode.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(settings));
    const ErrorHandler failed = [this](const QString& error) {
        m_runtime_settings_result->setText(error);
        m_status->setText(error);
        updateProviderButtons();
    };
    const auto generation = m_settings_restart_generation;
    applyWithProviderRestart([this, params, selected_mode, failed, generation](bool restart) {
        call("setpaymasterruntimesettings", params, false, nullptr,
             [this, selected_mode, restart, failed, generation](const UniValue& result) {
                 if (m_privacy || generation != m_settings_restart_generation) return;
                 const UniValue& operation_mode =
                     result.find_value("operation_mode");
                 const UniValue& autostart = result.find_value("autostart");
                 if (!result.isObject() || !operation_mode.isStr() ||
                     !autostart.isBool() ||
                     QString::fromStdString(operation_mode.get_str()) !=
                         selected_mode) {
                     m_runtime_settings_result->setText(tr(
                         "Runtime settings were not confirmed by Core. The displayed edit remains unsaved; refresh the provider status before retrying."));
                     updateProviderButtons();
                     return;
                 }
                 m_operation_mode = selected_mode;
                 m_autostart_enabled = autostart.get_bool();
                 reconcileAutostart();
                 m_provider_settings_present = true;
                 m_runtime_settings_dirty = false;
                 m_runtime_settings_result->setText(
                     m_operation_mode == QLatin1String("manual")
                         ? tr("Saved: manual expert processing.")
                         : m_autostart_enabled
                               ? tr("Saved: automatic processing with provider autostart. No passphrase is stored; a locked wallet waits for unlock.")
                               : tr("Saved: automatic processing after an explicit provider start. Provider autostart remains off."));
                 if (restart) {
                     finishProviderSettingsRestart([selected_mode](const UniValue& provider) {
                         return provider.find_value("operation_mode").isStr() &&
                             provider.find_value("operation_mode").get_str() == selected_mode.toStdString();
                     }, failed);
                 } else refreshStatus();
             }, /*show_error=*/false,
             [this](const QString& error) {
                 // Preserve the unsaved edit after a backend rejection and
                 // report the failure beside the affected controls. This
                 // prevents a failed RPC from looking like a successful save.
                 m_runtime_settings_result->setText(
                     tr("Runtime settings were not saved: %1").arg(error));
                 updateProviderButtons();
             });
    }, failed);
}

void PaymasterWidgetImpl::configureOperatorNode()
{
    if (m_privacy || m_busy || !hasRpcTransport() || m_node_connection_dialog) return;
    const auto generation = m_wallet_generation;
    call("getpaymasternodeconfig", {}, false, nullptr, [this, generation](const UniValue& current) {
        QTimer::singleShot(0, this, [this, generation, current] {
            if (generation == m_wallet_generation && !m_privacy && !m_node_connection_dialog)
                configureOperatorNodeFrom(current);
        });
    }, false, [this](const QString& error) {
        if (m_inline_node_editor) delete m_inline_node_editor.data();
        auto* message = new QLabel(tr("Connection settings could not be checked. Refresh the connection settings before editing.\n%1").arg(error), m_node_editor_host);
        message->setWordWrap(true);
        message->setTextFormat(Qt::PlainText);
        m_inline_node_editor = message;
        m_node_editor_layout->addWidget(message);
    });
}

void PaymasterWidgetImpl::configureOperatorNodeFrom(const UniValue& current)
{
    if (m_setup_wizard_active) {
        QDialog dialog(this);
        dialog.setObjectName("paymasterNodeConnectionDialog");
        dialog.setWindowTitle(tr("Node connection — all wallets"));
        dialog.resize(720, 650);
        m_node_connection_dialog = &dialog;
        auto* layout = new QVBoxLayout(&dialog);
        auto* scroll = new QScrollArea(&dialog);
        scroll->setWidgetResizable(true);
        auto* editor = createNodeConfigEditor(current, scroll);
        scroll->setWidget(editor);
        ConfigureScrollArea(scroll, editor);
        layout->addWidget(scroll);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
        m_node_connection_dialog.clear();
        return;
    }
    if (m_inline_node_editor) delete m_inline_node_editor.data();
    m_inline_node_editor = createNodeConfigEditor(current, m_node_editor_host);
    m_node_editor_layout->addWidget(m_inline_node_editor);
    showOperatorPage(m_connection_page);
}

QWidget* PaymasterWidgetImpl::createNodeConfigEditor(const UniValue& current, QWidget* parent)
{
    const auto generation = m_wallet_generation;
    auto* panel = new QWidget(parent);
    panel->setObjectName("paymasterNodeConfigEditor");
    panel->setAttribute(Qt::WA_StyledBackground, true);
    const QPointer<QWidget> guard{panel};
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);
    auto* help = new QLabel(tr("Review all current connection settings here. Only changed values are previewed. Saving requires a reviewed plan and an explicit node restart; this window does not configure your firewall or Tor service."), panel);
    help->setProperty("paymasterRole", QStringLiteral("summaryText"));
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* contents = new QWidget(panel);
    auto* form = new QVBoxLayout(contents);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(16);
    layout->addWidget(contents);
    const auto section = [contents, form](const QString& title, const char* name) {
        auto* group = new QGroupBox(title, contents);
        group->setObjectName(QString::fromLatin1(name));
        group->setProperty("paymasterRole", QStringLiteral("card"));
        auto* rows = new QFormLayout(group);
        rows->setContentsMargins(18, 22, 18, 16);
        rows->setHorizontalSpacing(28);
        rows->setVerticalSpacing(16);
        rows->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        rows->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->addWidget(group);
        return rows;
    };
    auto* addresses = section(tr("Addresses"), "paymasterNodeAddresses");
    auto* features = section(tr("Required node features"), "paymasterNodeFeatures");
    auto* capacity = section(tr("Connection capacity"), "paymasterNodeCapacity");
    const auto display_value = [this](const std::string& key, const UniValue& value) {
        if (key == "paymasterbind" && value.isArray()) {
            QStringList values;
            for (const auto& bind : value.getValues()) {
                if (!bind.isStr()) return tr("Unavailable");
                values << QString::fromStdString(bind.get_str());
            }
            return values.empty() ? tr("Not configured") : values.join('\n');
        }
        if (key == "paymasterendpoint" && value.isStr())
            return value.get_str().empty() ? tr("Not configured") : QString::fromStdString(value.get_str());
        if (value.isNum()) {
            if (key == "maxconnections" || key == "paymastermaxoutbound" || key == "paymastermaxinbound")
                return QString::fromStdString(value.write());
            return (key == "prune" ? value.getInt<int64_t>() == 0 : value.getInt<int64_t>() == 1) ? tr("Yes") : tr("No");
        }
        return tr("Unavailable");
    };
    struct Field { std::string key; QWidget* input; UniValue original; QString caption; };
    std::vector<Field> fields;
    const auto& metadata = current.find_value("fields");
    bool complete = metadata.isObject() && current.find_value("network").isStr() &&
        current.find_value("network").get_str() == ChainTypeToString(Params().GetChainType());
    bool locked{false};
    const std::vector<std::pair<std::string, QString>> names{
        {"paymasterbind", tr("Local Paymaster listener")}, {"paymasterendpoint", tr("Announced address")},
        {"digidollar", tr("DigiDollar enabled")}, {"paymaster", tr("Paymaster enabled")},
        {"txindex", tr("Transaction index")}, {"v2transport", tr("Encrypted transport (BIP324)")},
        {"prune", tr("Keep the full blockchain (no pruning)")},
        {"maxconnections", tr("Total connection limit")}, {"paymastermaxoutbound", tr("Outgoing Paymaster connections (0–4)")},
        {"paymastermaxinbound", tr("Incoming Paymaster connections (0–16)")}};
    for (const auto& [key, caption] : names) {
        const auto& info = metadata.find_value(key);
        const bool editable = info.find_value("editable").isTrue();
        auto value = editable ? info.find_value("configured_value") : info.find_value("value");
        complete &= info.find_value("editable").isBool() && info.find_value("source").isStr() &&
            info.find_value("reason").isStr() && info.find_value("restart_required").isBool();
        QWidget* input{nullptr};
        if (key == "paymasterbind") {
            auto* edit = new QPlainTextEdit(contents);
            edit->setMaximumHeight(80);
            QStringList lines;
            if (value.isArray()) for (const auto& bind : value.getValues()) {
                if (bind.isStr()) lines << QString::fromStdString(bind.get_str()); else complete = false;
            }
            else complete = false;
            edit->setPlainText(lines.join('\n'));
            edit->setToolTip(tr("One numeric IP:port per line, separate from P2P and RPC. For Tor forwarding append =onion, for example 127.0.0.1:18450=onion. Keep existing listeners unless you intend to replace them."));
            input = edit;
        } else if (key == "paymasterendpoint") {
            auto* edit = new QLineEdit(contents);
            if (value.isStr()) edit->setText(QString::fromStdString(value.get_str())); else complete = false;
            edit->setToolTip(tr("The address clients connect to: public IP:port or onion:port. Loopback addresses are for Regtest only. Forward this port to your local listener using your firewall/NAT or Tor service."));
            input = edit;
        } else if (key == "maxconnections" || key == "paymastermaxoutbound" || key == "paymastermaxinbound") {
            auto* spin = new QSpinBox(contents);
            spin->setRange(0, key == "maxconnections" ? 1000000 : key == "paymastermaxoutbound" ? 4 : 16);
            if (value.isNum() && value.getInt<int64_t>() >= 0 && value.getInt<int64_t>() <= spin->maximum()) spin->setValue(value.getInt<int>());
            else complete = false;
            spin->setToolTip(tr("Typical dedicated provider: 125 total connections, 1 outgoing and 16 incoming Paymaster connections. Preserve higher saved capacity when needed."));
            input = spin;
        } else {
            auto* check = new QCheckBox(contents);
            if (value.isNum()) check->setChecked(key == "prune" ? value.getInt<int64_t>() == 0 : value.getInt<int64_t>() == 1);
            else complete = false;
            check->setToolTip(key == "prune" ? tr("Required for a provider. Turning off pruning can require downloading historical blocks; no reindex or download is started here.") : tr("Required for Paymaster operation. Changes take effect only after restarting the node."));
            input = check;
        }
        input->setObjectName(QStringLiteral("paymasterNode_") + QString::fromStdString(key));
        const auto& source = info.find_value("source");
        const auto& reason = info.find_value("reason");
        const QString source_name = !source.isStr() ? tr("Unknown source") :
            source.get_str() == "command_line" ? tr("Start command / BAT") :
            source.get_str() == "settings_json" ? tr("settings.json") :
            source.get_str() == "forced" ? tr("Runtime override") :
            source.get_str() == "loaded_configuration" ? tr("Configuration file") :
            source.get_str() == "default" ? tr("Node default") : tr("Unknown source");
        if (!editable) {
            locked = true;
            const QString tooltip = input->toolTip();
            delete input;
            auto* fixed = new QLabel(display_value(key, value), contents);
            fixed->setProperty("paymasterRole", QStringLiteral("nodeSettingValue"));
            fixed->setTextFormat(Qt::PlainText);
            fixed->setWordWrap(true);
            fixed->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            fixed->setToolTip(tooltip);
            input = fixed;
            input->setObjectName(QStringLiteral("paymasterNode_") + QString::fromStdString(key));
        }
        auto* description = new QWidget(contents);
        description->setMinimumWidth(245);
        description->setMaximumWidth(285);
        auto* description_layout = new QVBoxLayout(description);
        description_layout->setContentsMargins(0, 0, 0, 0);
        description_layout->setSpacing(4);
        auto* name = new QLabel(caption, description);
        name->setProperty("paymasterRole", QStringLiteral("nodeSettingName"));
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        name->setBuddy(input);
        description_layout->addWidget(name);
        auto* option = new QLabel(QStringLiteral("-") + QString::fromStdString(key), description);
        option->setProperty("paymasterRole", QStringLiteral("mutedText"));
        option->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        description_layout->addWidget(option);
        auto* value_column = new QWidget(contents);
        value_column->setMinimumWidth(220);
        auto* value_layout = new QVBoxLayout(value_column);
        value_layout->setContentsMargins(0, 0, 0, 0);
        value_layout->setSpacing(5);
        input->setAccessibleName(caption);
        value_layout->addWidget(input);
        auto* provenance = new QLabel(editable ? tr("Source: %1").arg(source_name) : tr("%1 · Read-only").arg(source_name), value_column);
        provenance->setObjectName(QStringLiteral("paymasterNodeSource_") + QString::fromStdString(key));
        provenance->setProperty("paymasterRole", QStringLiteral("nodeSettingSource"));
        provenance->setTextFormat(Qt::PlainText);
        provenance->setWordWrap(true);
        provenance->setMaximumWidth(300);
        value_layout->addWidget(provenance);
        if (!editable && reason.isStr() && !reason.get_str().empty() &&
            (!source.isStr() || reason.get_str() != source.get_str())) {
            auto* conflict = new QLabel(tr("Review configuration source: %1").arg(QString::fromStdString(reason.get_str())), value_column);
            conflict->setProperty("paymasterRole", QStringLiteral("mutedText"));
            conflict->setWordWrap(true);
            conflict->setTextFormat(Qt::PlainText);
            value_layout->addWidget(conflict);
        }
        auto* rows = key == "paymasterbind" || key == "paymasterendpoint" ? addresses :
            key == "maxconnections" || key == "paymastermaxoutbound" || key == "paymastermaxinbound" ? capacity : features;
        rows->addRow(description, value_column);
        if (editable) {
            fields.push_back({key, input, value, caption});
            if (info.find_value("restart_required").isTrue()) {
                auto* pending = new QLabel(tr("Running now: %1\nSaved change — restart required.").arg(display_value(key, info.find_value("value"))), value_column);
                pending->setProperty("paymasterRole", QStringLiteral("summaryText"));
                pending->setTextFormat(Qt::PlainText);
                pending->setWordWrap(true);
                pending->setObjectName(QStringLiteral("paymasterNodePending_") + QString::fromStdString(key));
                value_layout->addWidget(pending);
            }
        }
    }
    auto* source_help = new QLabel(locked
        ? tr("Read-only values must be changed at their source, for example in the BAT/shortcut or settings.json, followed by a node restart. To manage them here, remove the overriding setting first.")
        : tr("Start arguments and included configuration files can override this file. Core checks conflicts before saving. Tor requires a separately configured service; public reachability must be checked from another node."), panel);
    source_help->setWordWrap(true);
    source_help->setProperty("paymasterRole", QStringLiteral("mutedText"));
    source_help->setObjectName("paymasterNodeSourceHelp");
    layout->addWidget(source_help);
    auto* status = new QLabel(panel);
    status->setObjectName("paymasterNodeConfigStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setText(complete ? tr("No changes have been saved.") : tr("Current startup settings are incomplete. Close this window and refresh the status before editing."));
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(panel);
    auto* preview_button = buttons->addButton(tr("Preview changes"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(tr("Save reviewed changes"), QDialogButtonBox::ActionRole);
    auto* discard = buttons->addButton(tr("Discard changes"), QDialogButtonBox::ResetRole);
    discard->setObjectName("paymasterNodeDiscard");
    preview_button->setObjectName("paymasterNodePreview");
    save->setObjectName("paymasterNodeSave");
    const bool editable = !fields.empty();
    preview_button->setVisible(editable);
    save->setVisible(editable);
    discard->setVisible(editable);
    preview_button->setEnabled(complete && editable);
    if (!editable) {
        help->setText(tr("These settings are managed outside this window. Current values are shown below; the source badge identifies where to change each setting."));
        if (complete) status->hide();
    }
    save->setEnabled(false);
    layout->addWidget(buttons);
    const auto plan = std::make_shared<UniValue>();
    const auto invalidate = [plan, save, status] {
        *plan = UniValue{}; save->setEnabled(false);
        status->setText(QObject::tr("Settings changed. Preview the changes before saving."));
    };
    for (const auto& field : fields) {
        if (auto* edit = qobject_cast<QLineEdit*>(field.input)) connect(edit, &QLineEdit::textChanged, panel, invalidate);
        if (auto* edit = qobject_cast<QPlainTextEdit*>(field.input)) connect(edit, &QPlainTextEdit::textChanged, panel, invalidate);
        if (auto* spin = qobject_cast<QSpinBox*>(field.input)) connect(spin, qOverload<int>(&QSpinBox::valueChanged), panel, invalidate);
        if (auto* check = qobject_cast<QCheckBox*>(field.input)) connect(check, &QCheckBox::toggled, panel, invalidate);
    }
    const auto error_handler = [this, guard, generation, status, preview_button, save, contents](const QString& error) {
        if (!guard || generation != m_wallet_generation || m_privacy) return;
        contents->setEnabled(true); preview_button->setEnabled(true); save->setEnabled(false);
        status->setText(error.startsWith("PAYMASTER_CONFIG_OVERRIDE")
            ? tr("These settings are controlled by a start argument, settings.json or an included file. Change that source and restart the node, or remove its override before saving here. No configuration was saved.\nDetails: %1").arg(error)
            : tr("Configuration could not be completed. Your entries remain available.\n%1").arg(error));
    };
    connect(discard, &QPushButton::clicked, panel, [this, generation, fields, plan, save, status] {
        if (m_busy || m_privacy || generation != m_wallet_generation) return;
        for (const auto& field : fields) {
            const QSignalBlocker blocker(field.input);
            if (auto* edit = qobject_cast<QLineEdit*>(field.input)) edit->setText(QString::fromStdString(field.original.get_str()));
            else if (auto* edit = qobject_cast<QPlainTextEdit*>(field.input)) {
                QStringList values;
                for (const auto& value : field.original.getValues()) values << QString::fromStdString(value.get_str());
                edit->setPlainText(values.join('\n'));
            } else if (auto* spin = qobject_cast<QSpinBox*>(field.input)) spin->setValue(field.original.getInt<int>());
            else if (auto* check = qobject_cast<QCheckBox*>(field.input)) check->setChecked(field.key == "prune" ? field.original.getInt<int64_t>() == 0 : field.original.getInt<int64_t>() != 0);
        }
        *plan = UniValue{};
        save->setEnabled(false);
        status->setText(tr("Edits discarded. The last loaded configuration is shown; refresh to check for file changes."));
    });
    connect(preview_button, &QPushButton::clicked, panel, [this, guard, generation, fields, plan, status, preview_button, save, contents, error_handler] {
        if (m_busy || m_privacy || generation != m_wallet_generation) return;
        UniValue changes{UniValue::VOBJ};
        QStringList summary;
        for (const auto& field : fields) {
            if (!field.input->isEnabled()) continue;
            UniValue value;
            if (auto* edit = qobject_cast<QLineEdit*>(field.input)) value = UniValue{edit->text().trimmed().toStdString()};
            else if (auto* edit = qobject_cast<QPlainTextEdit*>(field.input)) {
                value.setArray();
                for (const auto& line : edit->toPlainText().split('\n')) if (!line.trimmed().isEmpty()) value.push_back(line.trimmed().toStdString());
            } else if (auto* spin = qobject_cast<QSpinBox*>(field.input)) value = UniValue{spin->value()};
            else if (auto* check = qobject_cast<QCheckBox*>(field.input)) {
                if (field.key == "prune" && !check->isChecked()) {
                    if (field.original.isNum() && field.original.getInt<int64_t>() != 0) continue;
                    status->setText(tr("Paymaster requires the full blockchain. Keep no-pruning enabled.")); return;
                }
                value = UniValue{field.key == "prune" ? 0 : int(check->isChecked())};
            }
            if (DigiDollar::Paymaster::SetupMatches(value, field.original)) continue;
            changes.pushKV(field.key, value);
            summary << tr("%1: %2 → %3").arg(field.caption, QString::fromStdString(field.original.write()), QString::fromStdString(value.write()));
        }
        *plan = UniValue{}; save->setEnabled(false);
        if (changes.empty()) { status->setText(tr("No settings changed. The displayed startup values remain in effect.")); return; }
        UniValue params{UniValue::VARR}; params.push_back(changes);
        status->setText(tr("Checking these changes with Core…"));
        contents->setEnabled(false); preview_button->setEnabled(false);
        call("preparepaymasternodeconfig", params, false, nullptr,
            [this, guard, generation, plan, changes, summary, status, preview_button, save, contents, error_handler](const UniValue& result) {
                if (!guard || generation != m_wallet_generation || m_privacy) return;
                contents->setEnabled(true); preview_button->setEnabled(true);
                if (!IsHex256Field(result, "plan_id") || !DigiDollar::Paymaster::SetupMatches(changes, result.find_value("settings")) ||
                    !DigiDollar::Paymaster::SetupMatches(result.find_value("settings"), changes)) { error_handler(tr("Core returned an incomplete or different preview.")); return; }
                *plan = result;
                status->setText(tr("Review before saving:\n%1\n\nSaving backs up the existing configuration. An explicit restart is required. No restart, firewall change or reindex is performed here.").arg(summary.join('\n')));
                save->setEnabled(true);
            }, false, error_handler);
    });
    connect(save, &QPushButton::clicked, panel, [this, guard, generation, plan, status, preview_button, save, contents, error_handler] {
        if (m_busy || m_privacy || generation != m_wallet_generation || !plan->isObject()) return;
        UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
        options.pushKV("plan_id", plan->find_value("plan_id")); options.pushKV("settings", plan->find_value("settings")); params.push_back(options);
        *plan = UniValue{}; save->setEnabled(false); preview_button->setEnabled(false); contents->setEnabled(false);
        status->setText(tr("Saving the reviewed configuration…"));
        call("applypaymasternodeconfig", params, false, nullptr,
            [this, guard, generation, status, error_handler](const UniValue& result) {
                if (!guard || generation != m_wallet_generation || m_privacy) return;
                if (!result.find_value("applied").isTrue() || !result.find_value("backup").isStr()) { error_handler(tr("Core did not confirm the configuration write. Check the file before trying again.")); return; }
                m_operator_restart_required = true;
                status->setText(tr("Configuration saved. Restart the node to use it. Current running values are unchanged.\nBackup: %1").arg(QString::fromStdString(result.find_value("backup").get_str())));
            }, false, error_handler);
    });
    return panel;
}

void PaymasterWidgetImpl::saveProviderSafetyPolicy()
{
    if (m_busy || m_privacy) return;
    const QString input_error = providerSafetyInputError();
    if (!input_error.isEmpty()) {
        updateProviderButtons();
        return;
    }
    if (!m_provider_safety_snapshot_representable) {
        m_provider_safety_status->setText(tr(
            "Provider safety settings were not changed: the persisted Core policy cannot be represented exactly by this interface."));
        return;
    }
    FundingSafetyValues user_paid;
    FundingSafetyValues public_sponsored;
    FundingSafetyValues restricted_sponsored;
    if (!readFundingSafety(m_user_paid_safety, user_paid) ||
        !readFundingSafety(m_public_sponsored_safety, public_sponsored) ||
        !readFundingSafety(m_restricted_sponsored_safety,
                           restricted_sponsored)) {
        QMessageBox::warning(
            this, tr("Paymaster provider safety policy"),
            tr("One or more amounts are outside the supported whole-satoshi range."));
        return;
    }
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("user_paid", fundingSafetyToJSON(user_paid));
    policy.pushKV("public_sponsored", fundingSafetyToJSON(public_sponsored));
    policy.pushKV("restricted_sponsored", fundingSafetyToJSON(restricted_sponsored));
    policy.pushKV("maximum_active_quotes_total", m_max_active_quotes_total->value());
    policy.pushKV("maximum_active_quotes_per_netgroup", m_max_active_quotes_per_netgroup->value());
    policy.pushKV("maximum_active_quotes_per_recipient", m_max_active_quotes_per_recipient->value());
    policy.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                  m_max_quote_requests_per_netgroup->value());
    const UniValue requested_policy = policy;
    UniValue params{UniValue::VARR};
    params.push_back(std::move(policy));
    call("setpaymastersafetypolicy", std::move(params), false, nullptr,
         [this, requested_policy](const UniValue& result) {
             if (!IsExactProviderSafetyAcknowledgement(
                     requested_policy, result)) {
                 m_provider_safety_status->setText(tr(
                     "Core did not confirm the exact provider safety policy. The displayed edit remains unsaved; refresh before retrying."));
                 return;
             }
             m_provider_safety_dirty = false;
             m_provider_safety_status->setText(tr("Spending limits saved."));
             if (!m_policy_limits_review->isHidden()) {
                 showPolicySaveStatus(tr("Spending limits saved. Review your offer edits and save the policy to activate them."));
                 setStatusLabel(m_policy_limits_hint,
                     tr("Spending limits saved. Return to offer settings and save the policy to activate your offer changes."),
                     QStringLiteral("ready"));
             }
             invalidateSavedConfigurationDisplay();
             refreshStatus();
         }, false, [this](const QString& error) {
             const bool rejected = PaymasterQt::StableCode(m_rpc_failure).startsWith(QStringLiteral("PAYMASTER_INVALID_SAFETY")) ||
                 PaymasterQt::StableCode(m_rpc_failure) == QLatin1String("PAYMASTER_SAFETY_LIMITS_REQUIRED");
             m_provider_safety_status->setText(rejected
                 ? tr("Limits were not saved: they do not fit the current offer or the other budgets. Your edits have been kept. Review the current offer and highlighted limits before saving again.")
                 : tr("Saving the limits could not be confirmed. Your edits have been kept. Check the current saved status before trying again."));
             refreshStatus();
         });
}

void PaymasterWidgetImpl::invalidateSavedConfigurationDisplay()
{
    // A successful write makes the previous combined snapshot obsolete.
    // Do not keep displaying its conflict/readiness during readback, and
    // do not synthesize readiness from the write acknowledgement alone.
    m_operator_snapshot = UniValue{};
    m_status_age.invalidate();
    m_provider_info_snapshot_available = false;
    resetOperatorPresentation();
}

void PaymasterWidgetImpl::saveClientSafetyPolicy()
{
    if (!m_client_safety_snapshot_representable) {
        m_client_safety_status->setText(tr(
            "Client safety limits were not changed: the persisted Core policy cannot be represented exactly by this interface."));
        return;
    }
    const int requested_per_transaction =
        m_client_fee_per_transaction->value();
    const int requested_per_day = m_client_fee_per_day->value();
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("maximum_service_fee_per_transaction_cents",
                  requested_per_transaction);
    policy.pushKV("maximum_service_fee_per_day_cents",
                  requested_per_day);
    UniValue params{UniValue::VARR};
    params.push_back(std::move(policy));
    call("setpaymasterclientsafetypolicy", std::move(params), false, nullptr,
         [this, requested_per_transaction,
          requested_per_day](const UniValue& result) {
             if (!IsCompleteClientSafetyPolicy(result)) {
                 m_client_safety_status->setText(tr(
                     "Client safety limits were not confirmed by Core. The displayed edit remains unsaved."));
                 return;
             }
             int persisted_per_transaction{0};
             int persisted_per_day{0};
             try {
                 persisted_per_transaction = result.find_value(
                     "maximum_service_fee_per_transaction_cents")
                                                .getInt<int>();
                 persisted_per_day = result.find_value(
                     "maximum_service_fee_per_day_cents").getInt<int>();
             } catch (const std::exception&) {
                 m_client_safety_status->setText(tr(
                     "Client safety limits were outside the supported range. The displayed edit remains unsaved."));
                 return;
             }
             if (persisted_per_transaction != requested_per_transaction ||
                 persisted_per_day != requested_per_day) {
                 m_client_safety_status->setText(tr(
                     "Core did not confirm the exact client safety limits. The displayed edit remains unsaved."));
                 return;
             }
             m_client_safety_dirty = false;
             refreshClientSafetyStatus();
         });
}

QString PaymasterWidgetImpl::safetyClassStatus(const QString& name, const UniValue& value) const
{
    if (!HasInt64Fields(
            value,
            {"active_quotes", "reserved_network_fee_satoshis",
             "spent_network_fee_last_hour_satoshis",
             "spent_network_fee_last_day_satoshis",
             "completed_last_hour", "completed_last_day"}) ||
        !value.find_value("can_accept_minimum_quote").isBool()) {
        return tr("%1: unavailable").arg(name);
    }
    QString status = tr("%1: %2 active, %3 sat reserved, %4 sat spent/hour, %5 sat spent/day, "
                        "%6 completed/hour, %7 completed/day, accepting: %8")
        .arg(name)
        .arg(value.find_value("active_quotes").getInt<qint64>())
        .arg(value.find_value("reserved_network_fee_satoshis").getInt<qint64>())
        .arg(value.find_value("spent_network_fee_last_hour_satoshis").getInt<qint64>())
        .arg(value.find_value("spent_network_fee_last_day_satoshis").getInt<qint64>())
        .arg(value.find_value("completed_last_hour").getInt<qint64>())
        .arg(value.find_value("completed_last_day").getInt<qint64>())
        .arg(yesNo(value.find_value("can_accept_minimum_quote").get_bool()));
    QStringList errors;
    const UniValue& error_values = value.find_value("errors");
    if (error_values.isArray()) {
        for (const UniValue& error : error_values.getValues()) {
            if (error.isStr()) errors.push_back(QString::fromStdString(error.get_str()));
        }
    }
    if (!errors.isEmpty()) status += tr(" · limits: %1").arg(errors.join(tr(", ")));
    return status;
}

void PaymasterWidgetImpl::applyProviderSafetyStatus(const UniValue& result)
{
             const UniValue& configured = result.find_value("configured");
             const UniValue& policy = result.find_value("policy");
             if (!result.isObject() || !configured.isBool() ||
                 (configured.get_bool() &&
                  !IsCompleteProviderSafetyPolicy(policy))) {
                 m_provider_safety_snapshot_available = false;
                 m_provider_safety_snapshot_representable = false;
                 setProviderSafetyMutationEnabled(false);
                 m_provider_safety_status->setText(tr(
                     "Core returned an incomplete provider safety status. Guided setup remains unavailable until a complete refresh succeeds."));
                 updateProviderButtons();
                 return;
             }
             if (configured.get_bool() &&
                 !providerSafetyCanRepresent(policy)) {
                 m_unrepresentable_provider_safety_snapshot = policy;
                 m_provider_safety_snapshot_representable = false;
                 m_provider_safety_snapshot_available = false;
                 m_provider_safety_configured = false;
                 m_provider_safety_dirty = false;
                 setProviderSafetyMutationEnabled(false);
                 m_provider_safety_status->setText(tr(
                     "Core returned a complete provider safety policy containing a value that this interface cannot represent exactly. The persisted policy was retained unchanged and all provider-safety mutation is disabled."));
                 updateProviderButtons();
                 return;
             }
             m_unrepresentable_provider_safety_snapshot =
                 UniValue{UniValue::VOBJ};
             m_provider_safety_snapshot_representable = true;
             setProviderSafetyMutationEnabled(true);
             m_provider_safety_snapshot_available = true;
             m_provider_safety_configured =
                 result.find_value("configured").isBool() &&
                 result.find_value("configured").get_bool();
             if (policy.isObject() && !m_provider_safety_dirty) {
                 m_loading_provider_safety = true;
                 loadFundingSafety(policy.find_value("user_paid"), m_user_paid_safety);
                 loadFundingSafety(policy.find_value("public_sponsored"), m_public_sponsored_safety);
                 loadFundingSafety(policy.find_value("restricted_sponsored"), m_restricted_sponsored_safety);
                 m_max_active_quotes_total->setValue(
                     policy.find_value("maximum_active_quotes_total").getInt<int>());
                 m_max_active_quotes_per_netgroup->setValue(
                     policy.find_value("maximum_active_quotes_per_netgroup").getInt<int>());
                 m_max_active_quotes_per_recipient->setValue(
                     policy.find_value("maximum_active_quotes_per_recipient").getInt<int>());
                 m_max_quote_requests_per_netgroup->setValue(
                     policy.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<int>());
                 m_loading_provider_safety = false;
                 m_provider_safety_dirty = false;
                 updateFundingSafetyDisplay(m_user_paid_safety,
                                            m_provider_safety_configured);
                 updateFundingSafetyDisplay(m_public_sponsored_safety,
                                            m_provider_safety_configured);
                 updateFundingSafetyDisplay(m_restricted_sponsored_safety,
                                            m_provider_safety_configured);
             }
             if (!m_provider_safety_dirty) {
                 m_provider_safety_status->setText(m_provider_safety_configured
                     ? tr("Provider safety policy: configured and persisted")
                     : tr("Provider safety policy: not configured (provider not ready)"));
             }
             QStringList usage;
             usage.push_back(safetyClassStatus(tr("User paid"), result.find_value("user_paid")));
             usage.push_back(safetyClassStatus(tr("Public sponsored"), result.find_value("public_sponsored")));
             usage.push_back(safetyClassStatus(tr("Restricted sponsored"), result.find_value("restricted_sponsored")));
             m_provider_safety_usage->setText(usage.join('\n'));
             updateProviderButtons();
}

void PaymasterWidgetImpl::refreshProviderSafetyStatus()
{
    if (!m_client_safety_dirty) {
        m_client_safety_status->setText(
            tr("Client safety policy: unavailable (automatic Paymaster transfers unavailable)"));
    }
    updateProviderButtons();
    call("getpaymastersafetystatus", {}, false, nullptr,
         [this](const UniValue& result) {
             applyProviderSafetyStatus(result);
             refreshClientSafetyStatus();
         }, false, [this](const QString& error) {
             m_provider_safety_snapshot_available = false;
             m_provider_safety_snapshot_representable = false;
             setProviderSafetyMutationEnabled(false);
             m_provider_safety_status->setText(tr(
                 "Provider safety status unavailable: %1. Saved values are retained locally but cannot be used by guided setup until a complete refresh succeeds.")
                     .arg(error));
             updateProviderButtons();
             refreshClientSafetyStatus();
         });
}

void PaymasterWidgetImpl::refreshClientSafetyStatus()
{
    if (!m_client_safety_dirty) {
        m_client_safety_status->setText(
            tr("Client safety policy: unavailable (automatic Paymaster transfers unavailable)"));
    }
    call("getpaymasterclientsafetystatus", {}, false, nullptr,
         [this](const UniValue& result) {
             if (!IsCompleteClientSafetyStatus(result)) {
                 m_client_safety_configured = false;
                 m_client_safety_snapshot_representable = false;
                 if (m_client_safety_group) {
                     m_client_safety_group->setEnabled(false);
                 }
                 m_client_safety_status->setText(tr(
                     "Core returned an incomplete client-safety status. Automatic Paymaster transfers remain unavailable until a complete refresh succeeds."));
                 return;
             }
             const bool configured =
                 result.find_value("configured").get_bool();
             m_client_safety_configured = configured;
             const UniValue& policy = result.find_value("policy");
             if (!policy.isObject()) {
                 m_unrepresentable_client_safety_snapshot =
                     UniValue{UniValue::VOBJ};
                 m_client_safety_snapshot_representable = true;
                 if (m_client_safety_group) {
                     m_client_safety_group->setEnabled(!m_busy);
                 }
             }
             if (policy.isObject() && !m_client_safety_dirty) {
                 qint64 per_transaction{0};
                 qint64 per_day{0};
                 try {
                     per_transaction = policy.find_value(
                         "maximum_service_fee_per_transaction_cents")
                                           .getInt<qint64>();
                     per_day = policy.find_value(
                         "maximum_service_fee_per_day_cents")
                                   .getInt<qint64>();
                 } catch (const std::exception&) {
                     m_client_safety_configured = false;
                     m_client_safety_status->setText(tr(
                         "Core returned client-safety values outside the supported range. Automatic Paymaster transfers remain unavailable."));
                     return;
                 }
                 if (!spinCanRepresent(m_client_fee_per_transaction,
                                       per_transaction) ||
                     !spinCanRepresent(m_client_fee_per_day, per_day)) {
                     m_unrepresentable_client_safety_snapshot = policy;
                     m_client_safety_snapshot_representable = false;
                     m_client_safety_configured = false;
                     if (m_client_safety_group) {
                         m_client_safety_group->setEnabled(false);
                     }
                     m_client_safety_status->setText(tr(
                         "Core returned client-safety limits that this interface cannot represent exactly. The persisted values were not changed and automatic Paymaster transfers remain unavailable here."));
                     return;
                 }
                 m_unrepresentable_client_safety_snapshot =
                     UniValue{UniValue::VOBJ};
                 m_client_safety_snapshot_representable = true;
                 if (m_client_safety_group) {
                     m_client_safety_group->setEnabled(!m_busy);
                 }
                 m_loading_client_safety = true;
                 m_client_fee_per_transaction->setValue(
                     static_cast<int>(per_transaction));
                 m_client_fee_per_day->setValue(
                     static_cast<int>(per_day));
                 m_loading_client_safety = false;
                 m_client_safety_dirty = false;
                 updateClientSafetyDisplay(configured);
             }
             if (m_client_safety_dirty) return;
             if (!m_client_safety_configured) {
                 m_client_safety_status->setText(
                     tr("Client safety policy: not configured (automatic Paymaster transfers unavailable)"));
                 return;
             }
             qint64 active_reservations{0};
             qint64 reserved_cents{0};
             qint64 spent_cents{0};
             qint64 available_cents{0};
             try {
                 active_reservations = result.find_value(
                     "active_reservations").getInt<qint64>();
                 reserved_cents = result.find_value(
                     "reserved_service_fee_cents").getInt<qint64>();
                 spent_cents = result.find_value(
                     "spent_service_fee_last_day_cents").getInt<qint64>();
                 available_cents = result.find_value(
                     "available_service_fee_today_cents").getInt<qint64>();
             } catch (const std::exception&) {
                 m_client_safety_configured = false;
                 m_client_safety_status->setText(tr(
                     "Core returned client-safety usage outside the supported range. Automatic Paymaster transfers remain unavailable."));
                 return;
             }
             if (active_reservations < 0 || reserved_cents < 0 ||
                 spent_cents < 0 || available_cents < 0) {
                 m_client_safety_configured = false;
                 m_client_safety_status->setText(tr(
                     "Core returned invalid client-safety usage. Automatic Paymaster transfers remain unavailable."));
                 return;
             }
              m_client_safety_status->setText(
                  tr("Client safety policy: configured · active reservations %1 · "
                     "reserved %2 cents · spent today %3 cents · available today %4 cents")
                      .arg(active_reservations)
                      .arg(reserved_cents)
                      .arg(spent_cents)
                      .arg(available_cents));
         }, false);
}

void PaymasterWidgetImpl::showPolicySaveStatus(const QString& message)
{
    setStatusLabel(m_policy_save_status, message, QStringLiteral("info"));
    m_policy_save_status->setVisible(!message.isEmpty());
    m_review_policy_limits->hide();
}

void PaymasterWidgetImpl::reviewPolicySpendingLimits()
{
    if (m_busy || m_privacy || !m_policy_dirty) return;
    showOperatorPage(m_safety_page);
    m_policy_limits_review->show();
    const bool restricted = m_scope->currentData().toString() == QLatin1String("restricted");
    m_funding_safety_tabs->setCurrentIndex(m_sponsored->isChecked() ? (restricted ? 2 : 1) : 0);
    qint64 saved_fee{0};
    if (!m_policy_snapshot_representable || !m_provider_safety_snapshot_available ||
        !m_provider_safety_snapshot_representable || !m_provider_safety_configured ||
        !m_network_fee->hasAcceptableInput() ||
        !GetInt64Field(m_operator_snapshot.find_value("provider").find_value("policy"),
                       "maximum_network_fee_dgb_satoshis", saved_fee) || saved_fee <= 0) {
        setStatusLabel(m_policy_limits_hint, tr("Refresh the current offer and spending limits before preparing changes."), QStringLiteral("action"));
        return;
    }
    // Never overwrite a manually edited budget, including incomplete text.
    if (m_provider_safety_dirty) {
        setStatusLabel(m_policy_limits_hint,
            tr("Your unsaved spending limits have been kept. Every selected payment model needs six positive limits; each per-transfer limit must fit both the saved and edited offer's network-fee ceiling. Save the limits, then return to save the offer."),
            QStringLiteral("action"));
        return;
    }
    m_network_fee->interpretText();
    const qint64 fee_cap = std::min(saved_fee, qint64{m_network_fee->value()});
    const std::array<FundingSafetyControls, 3> controls{
        m_user_paid_safety, m_public_sponsored_safety, m_restricted_sponsored_safety};
    const std::array<bool, 3> required{
        m_user_paid->isChecked(), m_sponsored->isChecked() && !restricted, m_sponsored->isChecked() && restricted};
    bool changed{false};
    for (size_t i = 0; i < controls.size(); ++i) {
        FundingSafetyValues values;
        if (!readFundingSafety(controls[i], values)) return;
        if (required[i] && values.allZero()) {
            restoreFundingSafetyDefaults(controls[i], true);
            // A suggestion must be valid under the still-active offer,
            // even when its replacement raises the advertised ceiling.
            static_cast<DgbAmountLineEdit*>(controls[i].per_transaction)->setSatoshis(
                std::min(fee_cap, qint64{20000000}));
            changed = true;
        } else if (values.per_transaction > fee_cap) {
            static_cast<DgbAmountLineEdit*>(controls[i].per_transaction)->setSatoshis(fee_cap);
            changed = true;
        }
        updateFundingSafetyDisplay(controls[i]);
    }
    if (changed) markProviderSafetyDirty();
    setStatusLabel(m_policy_limits_hint,
        tr("Review these limits before saving: finite starting limits were filled only for selected models with zero limits, and per-transfer caps were lowered where needed to fit both offers. Other limits were kept. Sponsored payments spend your DGB without a DD service fee. Nothing is saved yet. Save the limits below, then return to save the offer."),
        QStringLiteral("action"));
}

void PaymasterWidgetImpl::savePolicy()
{
    if (m_scope->currentData().toString() == QLatin1String("restricted")) {
        const QString message = readinessExplanation(QStringLiteral("PAYMASTER_RESTRICTED_SPONSORSHIP_DISABLED"));
        m_status->setText(message);
        showPolicySaveStatus(message);
        setStatusLabel(m_policy_save_status, message, QStringLiteral("action"));
        return; // Reject before pausing a provider or dispatching any RPC.
    }
    for (QSpinBox* control : {m_fee_bps, m_maximum_user_paid_service_fee,
                              m_min_amount, m_max_amount, m_quote_ttl, m_network_fee}) {
        if (!m_user_paid->isChecked() && (control == m_fee_bps || control == m_maximum_user_paid_service_fee)) continue;
        if (!control->hasAcceptableInput()) {
            const QString message = tr("Policy was not saved. Correct the highlighted value; your edits have been kept.");
            m_status->setText(message);
            showPolicySaveStatus(message);
            control->setFocus();
            return;
        }
    }
    m_fee_bps->interpretText();
    m_maximum_user_paid_service_fee->interpretText();
    m_min_amount->interpretText();
    m_max_amount->interpretText();
    m_quote_ttl->interpretText();
    m_network_fee->interpretText();
    if (!m_policy_snapshot_representable) {
        m_status->setText(tr(
            "Provider policy was not changed: the persisted Core value cannot be represented exactly by this interface."));
        return;
    }
    UniValue funding{UniValue::VARR};
    if (m_sponsored->isChecked()) funding.push_back("sponsored");
    if (m_user_paid->isChecked()) funding.push_back("user_paid");
    if (funding.empty()) {
        QMessageBox::warning(this, tr("Paymaster policy"), tr("Select at least one funding model."));
        return;
    }
    UniValue policy{UniValue::VOBJ};
    policy.pushKV("funding_models", std::move(funding));
    policy.pushKV("sponsorship_scope", m_scope->currentData().toString().toStdString());
    policy.pushKV("fee_rate_bps",
                  m_user_paid->isChecked() ? m_fee_bps->value() : 0);
    policy.pushKV("maximum_user_paid_service_fee_cents",
                  m_user_paid->isChecked()
                      ? m_maximum_user_paid_service_fee->value()
                      : 0);
    policy.pushKV("min_amount_cents", m_min_amount->value());
    policy.pushKV("max_amount_cents", m_max_amount->value());
    policy.pushKV("quote_ttl", m_quote_ttl->value());
    policy.pushKV("maximum_network_fee_dgb_satoshis", m_network_fee->value());
    const UniValue requested_policy = policy;
    UniValue params{UniValue::VARR};
    params.push_back(std::move(policy));
    const ErrorHandler failed = [this](const QString& error) {
        m_status->setText(error);
        showPolicySaveStatus(error);
        setStatusLabel(m_policy_save_status, error, QStringLiteral("action"));
    };
    const auto generation = m_settings_restart_generation;
    applyWithProviderRestart([this, params, requested_policy, failed, generation](bool restart) {
        m_policy_dirty = true;
        showPolicySaveStatus(tr("Saving policy…"));
        call("setpaymasterpolicy", params, false, nullptr,
             [this, requested_policy, restart, failed, generation](const UniValue& result) {
                 if (m_privacy || generation != m_settings_restart_generation) return;
                 if (!IsExactProviderPolicyAcknowledgement(
                         requested_policy, result)) {
                     const QString message = tr(
                         "Core did not confirm the exact provider operating policy. The displayed edit remains unsaved; refresh before retrying. No automatic restart was attempted.");
                     m_status->setText(message);
                     showPolicySaveStatus(message);
                     return;
                 }
                 m_policy_loaded = true;
                 m_provider_settings_present = true;
                 m_pending_policy_hash = QString::fromStdString(
                     result.find_value("policy_hash").get_str());
                 m_policy_readback_pending = true;
                 m_policy_dirty = false;
                 showPolicySaveStatus(tr("Policy saved."));
                 invalidateSavedConfigurationDisplay();
                 updatePolicyDisplay();
                 updateAutomaticRefreshTimer();
                 if (restart) {
                     const std::string hash = result.find_value("policy_hash").get_str();
                     finishProviderSettingsRestart([requested_policy, hash](const UniValue& provider) {
                         const auto& saved = provider.find_value("policy");
                         return IsExactProviderPolicyAcknowledgement(requested_policy, saved) &&
                             saved.find_value("policy_hash").get_str() == hash;
                     }, failed);
                 } else refreshStatus();
             }, false, [this, restart](const QString& error) {
                 const bool safety_conflict = PaymasterQt::StableCode(m_rpc_failure) == QLatin1String("PAYMASTER_PROVIDER_SAFETY_POLICY_CONFLICT");
                 QString message = safety_conflict
                     ? tr("Offer not saved: the selected payment models or network-fee ceiling do not match the saved spending limits. Your saved offer is unchanged. Review and save compatible limits first, then save this offer.")
                     : tr("The policy save could not be confirmed. Your edits have been kept. Check the current status before retrying.");
                 if (restart) message += tr(" The provider was paused and was not automatically restarted.");
                 m_status->setText(message);
                 showPolicySaveStatus(message);
                 if (safety_conflict) {
                     setStatusLabel(m_policy_save_status, message, QStringLiteral("action"));
                     m_review_policy_limits->show();
                 }
             });
    }, failed);
}
