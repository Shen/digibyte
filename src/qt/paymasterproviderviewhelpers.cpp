// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterproviderviewhelpers.h>

namespace PaymasterQt::ProviderViews {
QString PaymasterSetupModeSettingsKey(const WalletModel* model)
{
    if (!model) return {};
    const QByteArray wallet_identity =
        QByteArray::number(static_cast<int>(Params().GetChainType())) + '\0' +
        model->getWalletName().toUtf8();
    const QByteArray wallet_id = QCryptographicHash::hash(
        wallet_identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("Paymaster/Wallet/%1/SetupMode")
        .arg(QString::fromLatin1(wallet_id));
}

void AlignPaymasterFormLabels(const QList<QFormLayout*>& forms)
{
    QList<QWidget*> labels;
    int width{0};
    for (QFormLayout* form : forms) {
        for (int row = 0; row < form->rowCount(); ++row) {
            auto* item = form->itemAt(row, QFormLayout::LabelRole);
            if (!item || !item->widget()) continue;
            auto* label = item->widget();
            labels.push_back(label);
            width = std::max(width, label->sizeHint().width());
        }
    }
    for (QWidget* label : labels) label->setMinimumWidth(width);
}

void ConfigureScrollArea(QScrollArea* scroll, QWidget* contents)
{
    // Call this after QScrollArea::setWidget(). Qt deliberately enables
    // autoFillBackground on an inserted widget, which would otherwise restore
    // the native Windows window colour on top of the active QSS theme.
    scroll->setFrameShape(QFrame::NoFrame);
    // Do not fill the viewport from the native Windows palette. The Paymaster
    // pages are themed through QSS; forcing QPalette::Window here otherwise
    // leaves a bright surface behind dark-theme controls.
    scroll->setAttribute(Qt::WA_StyledBackground, true);
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    contents->setAttribute(Qt::WA_StyledBackground, true);
    scroll->viewport()->setAutoFillBackground(false);
    contents->setAutoFillBackground(false);
}

QWidget* CreatePaymasterPageColumn(QWidget* page, QVBoxLayout*& layout)
{
    auto* shell = new QHBoxLayout(page);
    shell->setContentsMargins(18, 14, 18, 18);
    shell->setSpacing(0);
    auto* column = new QWidget(page);
    column->setObjectName(QStringLiteral("paymasterPageColumn"));
    column->setMaximumWidth(1100);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    shell->addStretch(0);
    shell->addWidget(column, 1);
    shell->addStretch(0);
    return column;
}

void AddPaymasterPageHeading(QVBoxLayout* layout, QWidget* parent,
                             const QString& title, const QString& description,
                             const QString& object_prefix)
{
    auto* heading = new QLabel(title, parent);
    heading->setObjectName(object_prefix + QStringLiteral("Heading"));
    heading->setProperty("paymasterRole", QStringLiteral("pageHeading"));
    auto* help = new QLabel(description, parent);
    help->setObjectName(object_prefix + QStringLiteral("Introduction"));
    help->setProperty("paymasterRole", QStringLiteral("pageIntroduction"));
    help->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(help);
}

QPushButton* AddPaymasterDisclosure(QVBoxLayout* layout, QWidget* parent,
                                    QWidget* panel, const QString& show_text,
                                    const QString& hide_text,
                                    const QString& object_name,
                                    bool initially_open)
{
    auto* button = new QPushButton(initially_open ? hide_text : show_text, parent);
    button->setObjectName(object_name);
    button->setProperty("paymasterRole", QStringLiteral("disclosure"));
    button->setCheckable(true);
    button->setChecked(initially_open);
    button->setAccessibleName(show_text);
    panel->setVisible(initially_open);
    layout->addWidget(button, 0, Qt::AlignLeft);
    layout->addWidget(panel);
    QObject::connect(button, &QPushButton::toggled, panel,
                     [button, panel, show_text, hide_text](bool visible) {
                         panel->setVisible(visible);
                         button->setText(visible ? hide_text : show_text);
                     });
    return button;
}

QFrame* CreatePaymasterStatusCard(QWidget* parent, const QString& title,
                                  QLabel*& status, QPushButton*& action)
{
    auto* card = new QFrame(parent);
    card->setProperty("paymasterRole", QStringLiteral("statusCard"));
    card->setMinimumHeight(142);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(16, 14, 16, 14);
    card_layout->setSpacing(8);
    auto* heading = new QLabel(title, card);
    heading->setProperty("paymasterRole", QStringLiteral("cardHeading"));
    status = new QLabel(QObject::tr("Checking…"), card);
    status->setProperty("paymasterRole", QStringLiteral("statusText"));
    status->setWordWrap(true);
    action = new QPushButton(QObject::tr("Review"), card);
    action->setProperty("paymasterRole", QStringLiteral("secondaryAction"));
    action->setCursor(Qt::PointingHandCursor);
    card_layout->addWidget(heading);
    card_layout->addWidget(status);
    card_layout->addStretch();
    card_layout->addWidget(action, 0, Qt::AlignLeft);
    return card;
}

bool UseDarkPaymasterWizardTheme(const WalletModel* model, const QWidget* widget)
{
    if (model && model->getOptionsModel()) {
        const QString theme = model->getOptionsModel()->data(
            model->getOptionsModel()->index(OptionsModel::Theme), Qt::EditRole).toString();
        return theme.isEmpty() || theme == QLatin1String("dark");
    }
    const QPalette palette = widget ? widget->palette() : QPalette{};
    return palette.color(QPalette::Window).lightness() < 128;
}

GuidedPaymasterSafetyLimits GuidedSafetyLimits(bool conservative, qint64 advertised_network_fee)
{
    return DigiDollar::Paymaster::SetupSafetyProfile(conservative, advertised_network_fee);
}

bool GetInt64Field(const UniValue& object, const char* name, qint64& value)
{
    const UniValue& field = object.find_value(name);
    if (!field.isNum()) return false;
    try {
        value = field.getInt<qint64>();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool HasInt64Fields(const UniValue& object,
                    std::initializer_list<const char*> names)
{
    if (!object.isObject()) return false;
    return std::all_of(names.begin(), names.end(), [&object](const char* name) {
        qint64 value{0};
        return GetInt64Field(object, name, value);
    });
}

bool GetIntField(const UniValue& object, const char* name, int& value)
{
    qint64 parsed{0};
    if (!GetInt64Field(object, name, parsed) ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool HasIntFields(const UniValue& object,
                  std::initializer_list<const char*> names)
{
    if (!object.isObject()) return false;
    return std::all_of(names.begin(), names.end(), [&object](const char* name) {
        int value{0};
        return GetIntField(object, name, value);
    });
}

bool IsStringArray(const UniValue& value)
{
    if (!value.isArray() ||
        value.size() > MAX_PAYMASTER_RPC_ARRAY_ENTRIES) return false;
    for (const UniValue& entry : value.getValues()) {
        if (!entry.isStr() || entry.get_str().empty() ||
            entry.get_str().size() > MAX_PAYMASTER_RPC_TEXT_BYTES) {
            return false;
        }
    }
    return true;
}

bool IsEnumString(const UniValue& value,
                  std::initializer_list<const char*> allowed)
{
    if (!value.isStr() || value.get_str().empty() ||
        value.get_str().size() > MAX_PAYMASTER_RPC_TEXT_BYTES) {
        return false;
    }
    return std::any_of(allowed.begin(), allowed.end(),
                       [&value](const char* candidate) {
                           return value.get_str() == candidate;
                       });
}

bool IsBoundedOptionalString(const UniValue& value, size_t maximum,
                             bool allow_empty)
{
    return value.isNull() ||
        (value.isStr() && value.get_str().size() <= maximum &&
         (allow_empty || !value.get_str().empty()));
}

bool IsCompleteProviderPolicy(const UniValue& policy)
{
    const UniValue& models = policy.find_value("funding_models");
    const UniValue& maximum_service_fee =
        policy.find_value("maximum_user_paid_service_fee_cents");
    if (!policy.isObject() || !IsStringArray(models) || models.empty() ||
        models.size() > 2 ||
        !IsEnumString(policy.find_value("sponsorship_scope"),
                      {"public", "restricted"})) {
        return false;
    }
    std::set<std::string> unique_models;
    for (const UniValue& model : models.getValues()) {
        if (!IsEnumString(model, {"sponsored", "user_paid"}) ||
            !unique_models.insert(model.get_str()).second) {
            return false;
        }
    }
    return (maximum_service_fee.isNull() || maximum_service_fee.isNum()) &&
        HasIntFields(policy,
                     {"fee_rate_bps", "min_amount_cents",
                      "max_amount_cents", "quote_ttl"}) &&
        HasInt64Fields(policy,
                       {"maximum_network_fee_dgb_satoshis"});
}

bool IsCompleteFundingSafety(const UniValue& limits)
{
    return HasInt64Fields(
               limits,
               {"maximum_network_fee_per_transaction_satoshis",
                "maximum_reserved_network_fee_satoshis",
                "maximum_network_fee_per_hour_satoshis",
                "maximum_network_fee_per_day_satoshis"}) &&
        HasIntFields(limits,
                     {"maximum_completed_per_hour",
                      "maximum_completed_per_day"});
}

bool IsCompleteProviderSafetyPolicy(const UniValue& policy)
{
    return policy.isObject() &&
        IsCompleteFundingSafety(policy.find_value("user_paid")) &&
        IsCompleteFundingSafety(policy.find_value("public_sponsored")) &&
        IsCompleteFundingSafety(policy.find_value("restricted_sponsored")) &&
        HasIntFields(
            policy,
            {"maximum_active_quotes_total",
             "maximum_active_quotes_per_netgroup",
             "maximum_active_quotes_per_recipient",
             "maximum_quote_requests_per_netgroup_per_minute"});
}

bool IsCompleteLiquidityPolicy(const UniValue& policy)
{
    return policy.isObject() &&
        policy.find_value("automatic_replenishment").isBool() &&
        policy.find_value("paid_maintenance_approved").isBool() &&
        HasIntFields(
            policy,
            {"target_admission_dgb", "target_operational_dgb",
             "target_admission_carriers", "target_operational_carriers"}) &&
        HasInt64Fields(
            policy,
            {"maximum_maintenance_fee_per_transaction_satoshis",
             "maximum_maintenance_fee_per_hour_satoshis",
             "maximum_maintenance_fee_per_day_satoshis"});
}

bool IsCompleteLiquiditySlotStatus(const UniValue& status)
{
    return HasInt64Fields(
        status,
        {"target", "ready", "pending", "counted_toward_target",
         "missing"});
}

bool IsCompleteLiquidityStatus(const UniValue& result)
{
    return result.isObject() &&
        result.find_value("policy_configured").isBool() &&
        result.find_value("targets_satisfy_provider_policy").isBool() &&
        result.find_value("maintenance_state").isStr() &&
        !result.find_value("maintenance_state").get_str().empty() &&
        IsCompleteLiquidityPolicy(result.find_value("policy")) &&
        IsCompleteLiquiditySlotStatus(result.find_value("admission_dgb")) &&
        IsCompleteLiquiditySlotStatus(
            result.find_value("operational_dgb")) &&
        IsCompleteLiquiditySlotStatus(
            result.find_value("admission_carriers")) &&
        IsCompleteLiquiditySlotStatus(
            result.find_value("operational_carriers")) &&
        HasInt64Fields(
            result,
            {"maintenance_fee_reserved_satoshis",
             "maintenance_fee_spent_last_hour_satoshis",
             "maintenance_fee_spent_last_day_satoshis",
             "carrier_base_cents",
             "carrier_withdrawable_excess_cents"}) &&
        IsStringArray(result.find_value("readiness_errors"));
}

bool IsCompleteProviderPoolInfo(const UniValue& result)
{
    const UniValue& pool = result.find_value("pool");
    if (!result.isObject() || !pool.isArray() ||
        pool.size() > MAX_PAYMASTER_POOL_ENTRIES) return false;
    const std::set<std::string> purposes{"admission", "operational"};
    const std::set<std::string> assets{"dgb", "dd_carrier"};
    const std::set<std::string> states{
        "available", "reserved", "pending_successor", "spent",
        "committed", "released", "invalidated"};
    for (const UniValue& entry : pool.getValues()) {
        const UniValue& purpose = entry.find_value("purpose");
        const UniValue& asset = entry.find_value("asset");
        const UniValue& state = entry.find_value("state");
        const UniValue& reservation = entry.find_value("reservation_id");
        const UniValue& origin = entry.find_value("origin_commit_key");
        qint64 vout{0};
        qint64 dgb{0};
        qint64 dd{0};
        qint64 height{0};
        qint64 updated_at{0};
        if (!entry.isObject() || !IsHex256Field(entry, "txid") ||
            !purpose.isStr() || purposes.count(purpose.get_str()) == 0 ||
            !asset.isStr() || assets.count(asset.get_str()) == 0 ||
            !state.isStr() || states.count(state.get_str()) == 0 ||
            !GetInt64Field(entry, "vout", vout) || vout < 0 ||
            vout > std::numeric_limits<uint32_t>::max() ||
            !GetInt64Field(entry, "dgb_satoshis", dgb) || dgb < 0 ||
            !MoneyRange(dgb) || !GetInt64Field(entry, "dd_cents", dd) ||
            dd < 0 || dd > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS ||
            !GetInt64Field(entry, "confirmation_height", height) ||
            height < 0 || !GetInt64Field(entry, "updated_at", updated_at) ||
            updated_at < 0 ||
            (!reservation.isNull() &&
             !IsHex256Field(entry, "reservation_id")) ||
            (!origin.isNull() && !IsHex256Field(entry, "origin_commit_key"))) {
            return false;
        }
        if ((asset.get_str() == "dgb" && (dgb <= 0 || dd != 0)) ||
            (asset.get_str() == "dd_carrier" && (dd <= 0 || dgb != 0))) {
            return false;
        }
    }
    return true;
}

bool SameNumericField(const UniValue& expected, const UniValue& actual,
                      const char* name)
{
    const UniValue& expected_value = expected.find_value(name);
    const UniValue& actual_value = actual.find_value(name);
    if (!expected_value.isNum() || !actual_value.isNum()) return false;
    try {
        return expected_value.getInt<int64_t>() ==
               actual_value.getInt<int64_t>();
    } catch (const std::exception&) {
        return false;
    }
}

bool SameNumericFields(const UniValue& expected, const UniValue& actual,
                       std::initializer_list<const char*> names)
{
    return std::all_of(names.begin(), names.end(),
                       [&expected, &actual](const char* name) {
                           return SameNumericField(expected, actual, name);
                       });
}

bool SameBooleanField(const UniValue& expected, const UniValue& actual,
                      const char* name)
{
    const UniValue& expected_value = expected.find_value(name);
    const UniValue& actual_value = actual.find_value(name);
    return expected_value.isBool() && actual_value.isBool() &&
           expected_value.get_bool() == actual_value.get_bool();
}

bool SameStringField(const UniValue& expected, const UniValue& actual,
                     const char* name)
{
    const UniValue& expected_value = expected.find_value(name);
    const UniValue& actual_value = actual.find_value(name);
    return expected_value.isStr() && actual_value.isStr() &&
           expected_value.get_str() == actual_value.get_str();
}

bool SameStringSet(const UniValue& expected, const UniValue& actual,
                   const char* name)
{
    const UniValue& expected_values = expected.find_value(name);
    const UniValue& actual_values = actual.find_value(name);
    if (!IsStringArray(expected_values) || !IsStringArray(actual_values) ||
        expected_values.size() != actual_values.size()) {
        return false;
    }
    std::set<std::string> expected_set;
    std::set<std::string> actual_set;
    for (const UniValue& value : expected_values.getValues()) {
        expected_set.insert(value.get_str());
    }
    for (const UniValue& value : actual_values.getValues()) {
        actual_set.insert(value.get_str());
    }
    return expected_set.size() == expected_values.size() &&
           actual_set.size() == actual_values.size() &&
           expected_set == actual_set;
}

bool IsNonNullHex256(const UniValue& value)
{
    if (!value.isStr() || value.get_str().size() != 64) return false;
    static const QRegularExpression hex256{
        QStringLiteral("^[0-9a-fA-F]{64}$")};
    return value.get_str().find_first_not_of('0') != std::string::npos &&
        hex256.match(QString::fromStdString(value.get_str())).hasMatch();
}

bool IsHex256Field(const UniValue& object, const char* name)
{
    return IsNonNullHex256(object.find_value(name));
}

bool IsExplicitlyStoppedProvider(const UniValue& provider)
{
    const auto& state = provider.find_value("service_state");
    return IsHex256Field(provider, "provider_id") && provider.find_value("enabled").isFalse() &&
        provider.find_value("running").isFalse() && provider.find_value("start_requested").isFalse() &&
        state.isStr() && state.get_str() == "stopped";
}

bool IsExactProviderPolicyAcknowledgement(const UniValue& requested,
                                          const UniValue& persisted)
{
    return IsCompleteProviderPolicy(requested) &&
        IsCompleteProviderPolicy(persisted) &&
        IsHex256Field(persisted, "policy_hash") &&
        SameStringSet(requested, persisted, "funding_models") &&
        SameStringField(requested, persisted, "sponsorship_scope") &&
        SameNumericFields(
            requested, persisted,
            {"fee_rate_bps", "maximum_user_paid_service_fee_cents",
             "min_amount_cents", "max_amount_cents",
             "quote_ttl", "maximum_network_fee_dgb_satoshis"});
}

bool IsExactFundingSafetyAcknowledgement(const UniValue& requested,
                                         const UniValue& persisted)
{
    return IsCompleteFundingSafety(requested) &&
        IsCompleteFundingSafety(persisted) &&
        SameNumericFields(
            requested, persisted,
            {"maximum_network_fee_per_transaction_satoshis",
             "maximum_reserved_network_fee_satoshis",
             "maximum_network_fee_per_hour_satoshis",
             "maximum_network_fee_per_day_satoshis",
             "maximum_completed_per_hour", "maximum_completed_per_day"});
}

bool IsExactProviderSafetyAcknowledgement(const UniValue& requested,
                                          const UniValue& persisted)
{
    return IsCompleteProviderSafetyPolicy(requested) &&
        IsCompleteProviderSafetyPolicy(persisted) &&
        HasInt64Fields(persisted, {"updated_at"}) &&
        IsExactFundingSafetyAcknowledgement(
            requested.find_value("user_paid"),
            persisted.find_value("user_paid")) &&
        IsExactFundingSafetyAcknowledgement(
            requested.find_value("public_sponsored"),
            persisted.find_value("public_sponsored")) &&
        IsExactFundingSafetyAcknowledgement(
            requested.find_value("restricted_sponsored"),
            persisted.find_value("restricted_sponsored")) &&
        SameNumericFields(
            requested, persisted,
            {"maximum_active_quotes_total",
             "maximum_active_quotes_per_netgroup",
             "maximum_active_quotes_per_recipient",
             "maximum_quote_requests_per_netgroup_per_minute"});
}

bool IsExactLiquidityPolicyAcknowledgement(const UniValue& requested,
                                           const UniValue& persisted)
{
    return IsCompleteLiquidityPolicy(requested) &&
        IsCompleteLiquidityPolicy(persisted) &&
        HasInt64Fields(persisted, {"updated_at"}) &&
        SameBooleanField(requested, persisted, "automatic_replenishment") &&
        SameBooleanField(requested, persisted, "paid_maintenance_approved") &&
        SameNumericFields(
            requested, persisted,
            {"target_admission_dgb", "target_operational_dgb",
             "target_admission_carriers", "target_operational_carriers",
             "maximum_maintenance_fee_per_transaction_satoshis",
             "maximum_maintenance_fee_per_hour_satoshis",
             "maximum_maintenance_fee_per_day_satoshis"});
}

bool IsCompletePoolPreparationResult(const UniValue& result)
{
    return result.isObject() && result.find_value("executed").isBool() &&
        result.find_value("accepted").isBool() &&
        result.find_value("cancelled").isBool() &&
        result.find_value("preparation").isArray() &&
        IsHex256Field(result, "plan_id") &&
        HasInt64Fields(
            result,
            {"maximum_fee_satoshis", "maximum_total_fee_satoshis",
             "admission_dgb_slots", "operational_dgb_slots",
             "admission_carrier_slots", "operational_carrier_slots",
             "missing_admission_dgb_slots",
             "missing_operational_dgb_slots",
             "missing_admission_carrier_slots",
             "missing_operational_carrier_slots",
             "admission_dgb_satoshis_each",
             "operational_dgb_satoshis_each", "carrier_cents_each",
             "total_output_satoshis", "total_carrier_cents"});
}

bool IsCompletePoolRebalanceResult(const UniValue& result)
{
    return result.isObject() && result.find_value("executed").isBool() &&
        IsHex256Field(result, "plan_id") &&
        HasInt64Fields(
            result,
            {"retired_admission_dgb_slots",
             "retired_operational_dgb_slots",
             "retired_admission_carrier_slots",
             "retired_operational_carrier_slots",
             "retired_dgb_satoshis", "retired_carrier_cents"});
}

bool IsBoundedReserveReductionResult(const UniValue& result, const UniValue& options, const UniValue& reviewed)
{
    if (!IsCompletePoolRebalanceResult(result)) return false;
    for (const auto* key : {"retired_admission_dgb_slots", "retired_operational_dgb_slots", "retired_admission_carrier_slots", "retired_operational_carrier_slots", "retired_dgb_satoshis", "retired_carrier_cents"}) {
        const auto value = result.find_value(key).getInt<qint64>();
        if (value < 0 || value > MAX_MONEY || (!reviewed.isNull() && result.find_value(key).write() != reviewed.find_value(key).write())) return false;
    }
    qint64 fee{0}, per_transaction{0}, total{0};
    if (!GetInt64Field(options, "maximum_fee_satoshis", fee) || fee <= 0 || fee > MAX_MONEY / 2 ||
        !GetInt64Field(result, "maximum_network_fee_satoshis", per_transaction) || per_transaction != fee ||
        !GetInt64Field(result, "maximum_total_fee_satoshis", total)) return false;
    const bool dgb = result.find_value("retired_dgb_satoshis").getInt<qint64>() > 0;
    const bool dd = result.find_value("retired_carrier_cents").getInt<qint64>() > 0;
    if (options.find_value("dgb_only").isTrue() && (dd ||
        result.find_value("retired_admission_carrier_slots").getInt<qint64>() != 0 ||
        result.find_value("retired_operational_carrier_slots").getInt<qint64>() != 0)) return false;
    if (total != fee * (int(dgb) + int(dd))) return false;
    if (result.find_value("executed").isTrue()) {
        qint64 actual{0};
        if (dgb && (!IsHex256Field(result, "dgb_txid") || !GetInt64Field(result, "network_fee_satoshis", actual) || actual <= 0 || actual > fee)) return false;
        if (dd && (!IsHex256Field(result, "dd_txid") || !GetInt64Field(result, "dd_network_fee_satoshis", actual) || actual <= 0 || actual > fee)) return false;
    }
    return true;
}

bool IsReserveReductionFeeProposal(const UniValue& result)
{
    qint64 fee{0}, wallet_limit{0}, dgb{0}, dd{0}, dgb_value{0}, dd_value{0};
    if (!GetInt64Field(result, "maximum_network_fee_satoshis", fee) || fee <= 0 || fee > MAX_MONEY / 2 ||
        !GetInt64Field(result, "wallet_maximum_fee_satoshis", wallet_limit) || wallet_limit < fee || wallet_limit > MAX_MONEY ||
        !GetInt64Field(result, "estimated_dgb_fee_satoshis", dgb) || dgb < 0 || dgb > fee ||
        !GetInt64Field(result, "estimated_dd_fee_satoshis", dd) || dd < 0 || dd > fee ||
        !GetInt64Field(result, "retired_dgb_satoshis", dgb_value) ||
        !GetInt64Field(result, "retired_carrier_cents", dd_value)) return false;
    return (dgb_value > 0 ? dgb > 0 : dgb == 0) && (dd_value > 0 ? dd > 0 : dd == 0);
}

bool IsCompleteProviderPool(const UniValue& pool)
{
    return HasIntFields(
        pool,
        {"entries", "reserved", "admission_dgb", "admission_carriers",
         "operational_dgb", "operational_carriers",
         "complete_operational_slots"});
}

bool IsCompleteProviderRuntimeStatus(const UniValue& result)
{
    const UniValue& operation_mode = result.find_value("operation_mode");
    const UniValue& service_state = result.find_value("service_state");
    const UniValue& last_service_error =
        result.find_value("last_service_error");
    const UniValue& service_queue = result.find_value("service_queue");
    qint64 waiting_requests{0};
    qint64 waiting_submits{0};
    if (!IsEnumString(operation_mode, {"automatic", "manual"}) ||
        !result.find_value("autostart").isBool() ||
        !IsEnumString(
            service_state,
            {"stopped", "waiting_for_unlock", "waiting_for_readiness",
             "waiting_for_maintenance_approval",
             "replenishing_liquidity",
             "waiting_for_liquidity_confirmation", "active", "manual",
             "drain_only", "error"}) ||
        !IsBoundedOptionalString(last_service_error,
                                 MAX_PAYMASTER_RPC_TEXT_BYTES,
                                 /*allow_empty=*/true) ||
        !GetInt64Field(service_queue, "waiting_requests",
                       waiting_requests) ||
        !GetInt64Field(service_queue, "waiting_submits",
                       waiting_submits) ||
        waiting_requests < 0 || waiting_submits < 0) {
        return false;
    }
    return true;
}

bool IsCompleteProviderInfoSnapshot(const UniValue& result)
{
    const auto required_bool = [&result](const char* name) {
        return result.find_value(name).isBool();
    };
    const UniValue& provider_id = result.find_value("provider_id");
    const UniValue& identity_key = result.find_value("identity_key");
    const UniValue& endpoint = result.find_value("endpoint");
    const UniValue& display_name = result.find_value("display_name");
    const UniValue& policy = result.find_value("policy");
    const UniValue& pool = result.find_value("pool");
    const UniValue& readiness_errors = result.find_value("readiness_errors");
    const UniValue& backup = result.find_value("backup_status");
    const UniValue& finance = result.find_value("finance_summary");

    if (!result.isObject() || !required_bool("settings_present") ||
        !required_bool("wallet_eligible") || !required_bool("enabled") ||
        !required_bool("running") || !required_bool("ready") ||
        !required_bool("wallet_locked") || !required_bool("pool_ready") ||
        !IsCompleteProviderRuntimeStatus(result) ||
        !IsStringArray(readiness_errors) ||
        (!provider_id.isNull() && !IsHex256Field(result, "provider_id")) ||
        (!identity_key.isNull() && !IsHex256Field(result, "identity_key")) ||
        !IsBoundedOptionalString(endpoint,
                                 MAX_PAYMASTER_RPC_TEXT_BYTES) ||
        !IsBoundedOptionalString(display_name, 32,
                                 /*allow_empty=*/true) ||
        (!policy.isNull() && !IsCompleteProviderPolicy(policy)) ||
        !IsCompleteProviderPool(pool)) {
        return false;
    }
    if (!backup.isNull() &&
        (!backup.isObject() ||
         !backup.find_value("required").isBool() ||
         !HasInt64Fields(
             backup,
             {"reminder_updated_at", "last_successful_backup_at",
              "external_backup_acknowledged_at"}))) {
        return false;
    }
    if (!finance.isNull() &&
        (!finance.isObject() ||
         !finance.find_value("history_partially_reconstructable").isBool() ||
         !HasInt64Fields(
             finance,
             {"service_fee_income_cents", "dgb_operating_cost_satoshis",
              "successful_transfers"}))) {
        return false;
    }
    return !result.find_value("running").get_bool() ||
        (result.find_value("enabled").get_bool() && provider_id.isStr());
}

bool IsCompleteBackupAcknowledgement(const UniValue& result)
{
    qint64 acknowledged_at{0};
    return result.isObject() &&
        result.find_value("acknowledged").isBool() &&
        result.find_value("acknowledged").get_bool() &&
        result.find_value("backup_required").isBool() &&
        GetInt64Field(result, "acknowledged_at", acknowledged_at) &&
        acknowledged_at >= 0;
}

bool IsCompleteFinanceSummary(const UniValue& summary)
{
    qint64 income{0};
    qint64 cost{0};
    qint64 transfers{0};
    return GetInt64Field(summary, "service_fee_income_cents", income) &&
        GetInt64Field(summary, "dgb_operating_cost_satoshis", cost) &&
        GetInt64Field(summary, "successful_transfers", transfers) &&
        income >= 0 && cost >= 0 && transfers >= 0;
}

bool IsCompleteFinanceStatus(const UniValue& result,
                             const QString& expected_period)
{
    const UniValue& period = result.find_value("period");
    const UniValue& model_breakdown = result.find_value("model_breakdown");
    const UniValue& period_summaries = result.find_value("period_summaries");
    const UniValue& pool = result.find_value("pool_capital");
    const UniValue& daily_totals = result.find_value("daily_totals");
    const UniValue& events = result.find_value("events");
    if (!result.isObject() || !IsHex256Field(result, "provider_id") ||
        !period.isStr() ||
        QString::fromStdString(period.get_str()) != expected_period ||
        !HasInt64Fields(
            result,
            {"service_fee_income_cents", "dgb_operating_cost_satoshis",
             "successful_transfers", "average_service_fee_cents",
             "user_paid_transfers", "public_sponsored_transfers",
             "restricted_sponsored_transfers", "history_complete_from", "revision",
             "last_successful_backup_at",
             "external_backup_acknowledged_at"}) ||
        !result.find_value("history_partially_reconstructable").isBool() ||
        !result.find_value("backup_required").isBool() ||
        !model_breakdown.isObject() || !period_summaries.isObject() ||
        !HasInt64Fields(
            pool,
            {"dgb_available_satoshis", "dgb_reserved_satoshis",
             "dgb_pending_satoshis", "carrier_base_cents",
             "carrier_earned_cents", "carrier_withdrawable_cents",
             "pending_maintenance_transactions"}) ||
        !daily_totals.isArray() ||
        daily_totals.size() > MAX_PAYMASTER_FINANCE_DAYS ||
        !events.isArray() ||
        events.size() > MAX_PAYMASTER_FINANCE_PAGE_EVENTS) {
        return false;
    }
    for (const char* key : {"user_paid", "public_sponsored",
                            "restricted_sponsored"}) {
        if (!IsCompleteFinanceSummary(model_breakdown.find_value(key))) {
            return false;
        }
    }
    for (const char* key : {"today", "7d", "30d", "all"}) {
        if (!IsCompleteFinanceSummary(period_summaries.find_value(key))) {
            return false;
        }
    }
    for (const UniValue& total : daily_totals.getValues()) {
        qint64 day_start{0};
        qint64 income{0};
        qint64 cost{0};
        qint64 transfers{0};
        qint64 maintenance{0};
        if (!GetInt64Field(total, "day_start", day_start) ||
            !GetInt64Field(total, "service_fee_income_cents", income) ||
            !GetInt64Field(total, "dgb_operating_cost_satoshis", cost) ||
            !GetInt64Field(total, "successful_transfers", transfers) ||
            !GetInt64Field(total, "maintenance_transactions", maintenance) ||
            day_start < 0 || income < 0 || cost < 0 || transfers < 0 ||
            maintenance < 0 || !HasInt64Fields(
                total,
                {"day_start", "service_fee_income_cents",
                 "dgb_operating_cost_satoshis", "successful_transfers",
                 "maintenance_transactions"})) {
            return false;
        }
    }
    for (const UniValue& event : events.getValues()) {
        const UniValue& kind = event.find_value("kind");
        const UniValue& state = event.find_value("state");
        const UniValue& funding_model = event.find_value("funding_model");
        const UniValue& sponsorship_scope =
            event.find_value("sponsorship_scope");
        const UniValue& confirmed_at = event.find_value("confirmed_at");
        qint64 dd_income{0};
        qint64 dgb_cost{0};
        qint64 created_at{0};
        qint64 confirmed_time{0};
        if (!event.isObject() || !IsHex256Field(event, "event_id") ||
            !IsHex256Field(event, "transaction_id") ||
            !IsEnumString(kind, {"transfer", "setup", "replenishment",
                                 "retirement", "withdrawal", "recovery"}) ||
            !IsEnumString(state,
                          {"pending", "confirmed", "invalidated"}) ||
            (!funding_model.isNull() &&
             !IsEnumString(funding_model,
                           {"user_paid", "sponsored"})) ||
            (!sponsorship_scope.isNull() &&
             !IsEnumString(sponsorship_scope,
                           {"public", "restricted"})) ||
            !GetInt64Field(event, "dd_income_cents", dd_income) ||
            !GetInt64Field(event, "dgb_cost_satoshis", dgb_cost) ||
            !GetInt64Field(event, "created_at", created_at) ||
            dd_income < 0 || dgb_cost < 0 || created_at <= 0 ||
            (!confirmed_at.isNull() &&
             (!GetInt64Field(event, "confirmed_at", confirmed_time) ||
              confirmed_time <= 0))) {
            return false;
        }
        const bool transfer = kind.get_str() == "transfer";
        const bool sponsored = transfer && funding_model.isStr() &&
            funding_model.get_str() == "sponsored";
        if ((transfer && !funding_model.isStr()) ||
            (!transfer && (!funding_model.isNull() ||
                           !sponsorship_scope.isNull())) ||
            (sponsored && !sponsorship_scope.isStr()) ||
            (transfer && !sponsored && !sponsorship_scope.isNull()) ||
            ((state.get_str() == "confirmed") !=
             confirmed_at.isNum())) {
            return false;
        }
    }

    const UniValue& oracle = result.find_value("oracle_price_micro_usd");
    const UniValue& valuation_time = result.find_value("valuation_time");
    const UniValue& estimate = result.find_value("estimated_result_usd");
    const bool any_valuation =
        !oracle.isNull() || !valuation_time.isNull() || !estimate.isNull();
    if (any_valuation) {
        qint64 oracle_value{0};
        qint64 valuation_time_value{0};
        if (!GetInt64Field(result, "oracle_price_micro_usd", oracle_value) ||
            !GetInt64Field(result, "valuation_time", valuation_time_value) ||
            !estimate.isNum()) {
            return false;
        }
    }
    const UniValue& next_cursor = result.find_value("next_cursor");
    return next_cursor.isNull() || (next_cursor.isStr() &&
        next_cursor.get_str().size() <= 512 && IsHex(next_cursor.get_str()));
}

bool IsCompleteOverviewFinance(const UniValue& result)
{
    if (!IsHex256Field(result, "provider_id") ||
        !IsCompleteFinanceSummary(result.find_value("period_summaries").find_value("all")) ||
        !result.find_value("history_partially_reconstructable").isBool()) return false;
    const auto& capital = result.find_value("pool_capital");
    for (const auto* key : {"dgb_available_satoshis", "dgb_reserved_satoshis",
                            "dgb_pending_satoshis", "carrier_base_cents",
                            "carrier_earned_cents", "carrier_withdrawable_cents",
                            "pending_maintenance_transactions"}) {
        qint64 value{0};
        if (!GetInt64Field(capital, key, value) || value < 0) return false;
    }
    return true;
}

bool IsCompleteCarrierWithdrawalResult(const UniValue& result,
                                       const QString& expected_mode,
                                       bool expected_execution,
                                       const QString& expected_plan)
{
    const UniValue& executed = result.find_value("executed");
    const UniValue& mode = result.find_value("mode");
    if (!result.isObject() || !executed.isBool() ||
        executed.get_bool() != expected_execution || !mode.isStr() ||
        QString::fromStdString(mode.get_str()) != expected_mode ||
        !IsHex256Field(result, "plan_id")) {
        return false;
    }
    const QString plan = QString::fromStdString(
        result.find_value("plan_id").get_str());
    if (expected_execution && plan != expected_plan) return false;

    qint64 value{0};
    if (!expected_execution) {
        if (!GetInt64Field(result, "source_carriers", value) || value <= 0 ||
            !GetInt64Field(result, "expires_at", value) || value <= 0) {
            return false;
        }
    }
    if (expected_mode == QLatin1String("all_excess")) {
        qint64 withdrawable{0};
        qint64 fee{0};
        if (!GetInt64Field(result, "withdrawable_excess_cents",
                           withdrawable) ||
            !GetInt64Field(result, "estimated_network_fee_satoshis", fee) ||
            withdrawable < Params().GetDigiDollarParams().minOutputAmount || fee <= 0 ||
            (expected_execution && !IsHex256Field(result, "txid"))) {
            return false;
        }
        const UniValue& retained = result.find_value("retained_carrier_cents");
        if (!retained.isNull() &&
            (!GetInt64Field(result, "retained_carrier_cents", value) ||
             value < 0)) {
            return false;
        }
        return true;
    }
    return expected_mode == QLatin1String("release_slot") &&
        GetInt64Field(result, "operational_carrier_target", value) &&
        value >= 0;
}

bool IsCompleteProviderStartResult(const UniValue& result)
{
    const UniValue& mode = result.find_value("operation_mode");
    const UniValue& state = result.find_value("service_state");
    return result.isObject() && result.find_value("running").isBool() &&
        result.find_value("ready").isBool() && mode.isStr() &&
        (mode.get_str() == "automatic" || mode.get_str() == "manual") &&
        IsEnumString(
            state,
            {"stopped", "waiting_for_unlock", "waiting_for_readiness",
             "waiting_for_maintenance_approval",
             "replenishing_liquidity",
             "waiting_for_liquidity_confirmation", "active", "manual",
             "drain_only", "error"});
}

bool IsCompleteClientSafetyPolicy(const UniValue& policy)
{
    return HasInt64Fields(
        policy,
        {"maximum_service_fee_per_transaction_cents",
         "maximum_service_fee_per_day_cents"});
}

bool IsCompleteClientSafetyStatus(const UniValue& result)
{
    const UniValue& configured = result.find_value("configured");
    const UniValue& policy = result.find_value("policy");
    if (!result.isObject() || !configured.isBool() ||
        (!policy.isNull() && !IsCompleteClientSafetyPolicy(policy))) {
        return false;
    }
    return !configured.get_bool() ||
        (IsCompleteClientSafetyPolicy(policy) &&
         HasInt64Fields(
             result,
             {"active_reservations", "reserved_service_fee_cents",
              "spent_service_fee_last_day_cents",
              "available_service_fee_today_cents"}));
}

QString PaymasterWizardStyleSheet(bool dark_theme)
{
    if (dark_theme) {
        return QStringLiteral(
            "QWizard#PaymasterSetupWizard,"
            "QWizard#PaymasterSetupWizard QWizardPage {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "}"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupRequirementsScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupRequirementsContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupOfferScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupOfferContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupSafetyScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupSafetyContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupLiquidityScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupLiquidityContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupReviewScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupReviewContent,"
            "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupProgressScroll,"
            "QWizard#PaymasterSetupWizard QWidget#paymasterSetupProgressContent {"
            "  background-color: #0b2419;"
            "  color: #ffffff;"
            "  border: none;"
            "}"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupWalletCard {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-radius: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel {"
            "  background-color: transparent;"
            "  color: #ffffff;"
            "}"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupFieldHelp,"
            "QWizard#PaymasterSetupWizard QFrame#paymasterSetupLiquidityFieldHelp {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-left: 5px solid #42d884;"
            "  border-radius: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpTitle,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpTitle {"
            "  color: #ffffff;"
            "  font-size: 11pt;"
            "  font-weight: bold;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpText,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpText {"
            "  color: #c6ddcf;"
            "  font-size: 10pt;"
            "  font-weight: normal;"
            "}"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityPreviewDetails,"
            "QWizard#PaymasterSetupWizard QLabel#paymasterSetupProgressResult {"
            "  background-color: #123a29;"
            "  border: 1px solid #42d884;"
            "  border-radius: 5px;"
            "  color: #ffffff;"
            "  padding: 10px;"
            "}"
            "QWizard#PaymasterSetupWizard QLineEdit,"
            "QWizard#PaymasterSetupWizard QSpinBox,"
            "QWizard#PaymasterSetupWizard QDoubleSpinBox,"
            "QWizard#PaymasterSetupWizard QComboBox {"
            "  background-color: #113a29;"
            "  color: #ffffff;"
            "  border: 2px solid #42d884;"
            "  border-radius: 4px;"
            "  padding: 5px;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator {"
            "  width: 16px;"
            "  height: 16px;"
            "  background-color: #09261b;"
            "  border: 2px solid #42d884;"
            "  border-radius: 3px;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator:checked {"
            "  background-color: #1f9d57;"
            "  border-color: #74e5a7;"
            "}"
            "QWizard#PaymasterSetupWizard QCheckBox::indicator:unchecked:hover {"
            "  background-color: #164934;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-button {"
            "  subcontrol-origin: border;"
            "  subcontrol-position: top right;"
            "  width: 24px;"
            "  background-color: #dff5e7;"
            "  border-left: 1px solid #42d884;"
            "  border-bottom: 1px solid #8bc9a6;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-button {"
            "  subcontrol-origin: border;"
            "  subcontrol-position: bottom right;"
            "  width: 24px;"
            "  background-color: #dff5e7;"
            "  border-left: 1px solid #42d884;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-arrow {"
            "  image: url(:/icons/spin_up);"
            "  width: 9px;"
            "  height: 6px;"
            "}"
            "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-arrow {"
            "  image: url(:/icons/spin_down);"
            "  width: 9px;"
            "  height: 6px;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton {"
            "  background-color: #16804f;"
            "  color: #ffffff;"
            "  border: 2px solid #16804f;"
            "  border-radius: 4px;"
            "  padding: 7px 14px;"
            "  font-weight: bold;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:hover {"
            "  background-color: #11683e;"
            "  border-color: #42d884;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:focus,"
            "QWizard#PaymasterSetupWizard QLineEdit:focus,"
            "QWizard#PaymasterSetupWizard QSpinBox:focus,"
            "QWizard#PaymasterSetupWizard QDoubleSpinBox:focus,"
            "QWizard#PaymasterSetupWizard QComboBox:focus {"
            "  border-color: #f4d35e;"
            "}"
            "QWizard#PaymasterSetupWizard QPushButton:disabled {"
            "  background-color: #294b3b;"
            "  border-color: #294b3b;"
            "  color: #9ab5a7;"
            "}");
    }
    return QStringLiteral(
        "QWizard#PaymasterSetupWizard,"
        "QWizard#PaymasterSetupWizard QWizardPage {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "}"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupRequirementsScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupRequirementsContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupOfferScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupOfferContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupSafetyScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupSafetyContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupLiquidityScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupLiquidityContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupReviewScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupReviewContent,"
        "QWizard#PaymasterSetupWizard QScrollArea#paymasterSetupProgressScroll,"
        "QWizard#PaymasterSetupWizard QWidget#paymasterSetupProgressContent {"
        "  background-color: #eef9f2;"
        "  color: #123f2b;"
        "  border: none;"
        "}"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupWalletCard {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-radius: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel {"
        "  background-color: transparent;"
        "  color: #123f2b;"
        "}"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupFieldHelp,"
        "QWizard#PaymasterSetupWizard QFrame#paymasterSetupLiquidityFieldHelp {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-left: 5px solid #1f9d57;"
        "  border-radius: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpTitle,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpTitle {"
        "  color: #123f2b;"
        "  font-size: 11pt;"
        "  font-weight: bold;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupFieldHelpText,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityFieldHelpText {"
        "  color: #416553;"
        "  font-size: 10pt;"
        "  font-weight: normal;"
        "}"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupLiquidityPreviewDetails,"
        "QWizard#PaymasterSetupWizard QLabel#paymasterSetupProgressResult {"
        "  background-color: #ffffff;"
        "  border: 1px solid #9bcbb0;"
        "  border-radius: 5px;"
        "  color: #123f2b;"
        "  padding: 10px;"
        "}"
        "QWizard#PaymasterSetupWizard QLineEdit,"
        "QWizard#PaymasterSetupWizard QSpinBox,"
        "QWizard#PaymasterSetupWizard QDoubleSpinBox,"
        "QWizard#PaymasterSetupWizard QComboBox {"
        "  background-color: #ffffff;"
        "  color: #123f2b;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 5px;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator {"
        "  width: 16px;"
        "  height: 16px;"
        "  background-color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 3px;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator:checked {"
        "  background-color: #1f9d57;"
        "  border-color: #146c3a;"
        "}"
        "QWizard#PaymasterSetupWizard QCheckBox::indicator:unchecked:hover {"
        "  background-color: #e8f7ed;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-button {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: top right;"
        "  width: 24px;"
        "  background-color: #e8f7ed;"
        "  border-left: 1px solid #1f9d57;"
        "  border-bottom: 1px solid #9bcbb0;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-button {"
        "  subcontrol-origin: border;"
        "  subcontrol-position: bottom right;"
        "  width: 24px;"
        "  background-color: #e8f7ed;"
        "  border-left: 1px solid #1f9d57;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::up-arrow {"
        "  image: url(:/icons/spin_up);"
        "  width: 9px;"
        "  height: 6px;"
        "}"
        "QWizard#PaymasterSetupWizard QAbstractSpinBox::down-arrow {"
        "  image: url(:/icons/spin_down);"
        "  width: 9px;"
        "  height: 6px;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton {"
        "  background-color: #146c3a;"
        "  color: #ffffff;"
        "  border: 2px solid #1f9d57;"
        "  border-radius: 4px;"
        "  padding: 7px 14px;"
        "  font-weight: bold;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:hover {"
        "  background-color: #0f5a30;"
        "  border-color: #0f5a30;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:focus,"
        "QWizard#PaymasterSetupWizard QLineEdit:focus,"
        "QWizard#PaymasterSetupWizard QSpinBox:focus,"
        "QWizard#PaymasterSetupWizard QDoubleSpinBox:focus,"
        "QWizard#PaymasterSetupWizard QComboBox:focus {"
        "  border-color: #8a5a00;"
        "}"
        "QWizard#PaymasterSetupWizard QPushButton:disabled {"
        "  background-color: #b9d5c3;"
        "  border-color: #b9d5c3;"
        "  color: #60776a;"
        "}");
}
} // namespace PaymasterQt::ProviderViews
