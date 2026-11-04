// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

void PaymasterWidgetImpl::setFinanceExportFilenameForTesting(QString filename)
{
    m_finance_export_filename_for_testing = std::move(filename);
}

qint64 PaymasterWidgetImpl::financeNumber(const UniValue& object, const char* name)
{
    qint64 value{0};
    return GetInt64Field(object, name, value) ? value : 0;
}

QString PaymasterWidgetImpl::financeEventKind(const QString& kind)
{
    if (kind == QLatin1String("transfer")) return tr("Paymaster transfer");
    if (kind == QLatin1String("setup")) return tr("Pool setup");
    if (kind == QLatin1String("replenishment")) return tr("Liquidity replenishment");
    if (kind == QLatin1String("retirement")) return tr("Liquidity retirement");
    if (kind == QLatin1String("withdrawal")) return tr("Carrier withdrawal");
    return tr("Unknown booking");
}

QString PaymasterWidgetImpl::financeEventState(const QString& state)
{
    if (state == QLatin1String("confirmed")) return tr("Confirmed");
    if (state == QLatin1String("pending")) return tr("Pending");
    if (state == QLatin1String("invalidated")) return tr("Reversed");
    return tr("Unknown");
}

QString PaymasterWidgetImpl::financeFundingModel(const UniValue& event)
{
    const UniValue& model = event.find_value("funding_model");
    if (!model.isStr()) return tr("Maintenance");
    const QString funding = QString::fromStdString(model.get_str());
    if (funding == QLatin1String("user_paid")) return tr("User paid");
    const UniValue& scope = event.find_value("sponsorship_scope");
    return scope.isStr() && scope.get_str() == "restricted"
        ? tr("Restricted sponsored") : tr("Public sponsored");
}

void PaymasterWidgetImpl::refreshFinanceStatus()
{
    if (!hasRpcTransport() || m_busy || !m_finance_period_select) {
        if (m_finance_history_status) {
            m_finance_history_status->setText(!hasRpcTransport()
                ? tr("Finance refresh was not started because no wallet RPC transport is available.")
                : m_busy
                    ? tr("Finance refresh was not started because another wallet operation is still running.")
                    : tr("Finance refresh was not started because the reporting-period control is unavailable."));
        }
        return;
    }
    requestSelectedFinanceStatus();
}

void PaymasterWidgetImpl::overviewFinanceUnavailable()
{
    for (const auto* key : {"dgb_available", "dgb_reserved", "dgb_pending", "carrier_base"}) setFundsValue(key, QStringLiteral("—"));
    m_overview_finance_observed = true;
    for (const auto* name : {"dgb_available", "dgb_reserved", "dgb_pending", "carrier_base",
                             "carrier_earned", "carrier_withdrawable", "maintenance_pending", "income", "costs", "transfers"})
        setCapitalMetric(name, tr("Capital value unavailable — refresh to retry"), "neutral");
    setStatusLabel(m_overview_finance_status, tr("Finance data unavailable — refresh to retry"), "action");
}

void PaymasterWidgetImpl::applyOverviewFinance(const UniValue& result)
{
    if (!IsCompleteOverviewFinance(result) || (!m_provider_id.isEmpty() &&
        QString::fromStdString(result.find_value("provider_id").get_str()) != m_provider_id)) {
        overviewFinanceUnavailable();
        return;
    }
    const auto& summary = result.find_value("period_summaries").find_value("all");
    m_overview_finance_observed = true;
    QString text = tr("All time · confirmed");
    setCapitalMetric("income", tr("Service-fee income: %1 DD").arg(ddAmount(financeNumber(summary, "service_fee_income_cents"))), "neutral");
    setCapitalMetric("costs", tr("Operating costs: %1 DGB").arg(compactDgbAmount(financeNumber(summary, "dgb_operating_cost_satoshis"))), "neutral");
    setCapitalMetric("transfers", tr("Successful payments: %1").arg(financeNumber(summary, "successful_transfers")), "neutral");
    const bool partial = result.find_value("history_partially_reconstructable").isTrue();
    if (partial) text += tr("\nHistory is only partially reconstructable; open finances for details.");
    setStatusLabel(m_overview_finance_status, maskNumericText(text), partial ? "action" : "neutral");
    const auto& capital = result.find_value("pool_capital");
    const auto amount = [&](const char* key) { return financeNumber(capital, key); };
    setFundsValue("dgb_available", tr("%1 DGB").arg(compactDgbAmount(amount("dgb_available_satoshis"))));
    setFundsValue("dgb_reserved", tr("In use: %1 DGB").arg(compactDgbAmount(amount("dgb_reserved_satoshis"))));
    setFundsValue("dgb_pending", tr("Awaiting confirmation: %1 DGB").arg(compactDgbAmount(amount("dgb_pending_satoshis"))));
    setFundsValue("carrier_base", tr("%1 DD").arg(ddAmount(amount("carrier_base_cents"))));
    setCapitalMetric("dgb_available", tr("Available: %1 DGB").arg(compactDgbAmount(amount("dgb_available_satoshis"))), "neutral");
    setCapitalMetric("dgb_reserved", tr("In use: %1 DGB").arg(compactDgbAmount(amount("dgb_reserved_satoshis"))), "neutral");
    setCapitalMetric("dgb_pending", tr("Awaiting confirmation: %1 DGB").arg(compactDgbAmount(amount("dgb_pending_satoshis"))), amount("dgb_pending_satoshis") > 0 ? "waiting" : "neutral");
    setCapitalMetric("carrier_base", tr("Base capital: %1 DD").arg(ddAmount(amount("carrier_base_cents"))), "neutral");
    setCapitalMetric("carrier_earned", tr("Accumulated: %1 DD").arg(ddAmount(amount("carrier_earned_cents"))), "neutral");
    setCapitalMetric("carrier_withdrawable", tr("Withdrawable: %1 DD").arg(ddAmount(amount("carrier_withdrawable_cents"))), "neutral");
    setCapitalMetric("maintenance_pending", tr("Open maintenance tasks: %1").arg(amount("pending_maintenance_transactions")), amount("pending_maintenance_transactions") > 0 ? "waiting" : "neutral");
}

void PaymasterWidgetImpl::refreshOverviewFinance(bool background)
{
    if (!m_core_has_identity || m_privacy) return;
    UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
    options.pushKV("period", "all");
    options.pushKV("include_events", false);
    params.push_back(options);
    // Serialized continuation of the operator read, with the same wallet
    // generation guard as all other calls. Never signs or sends a payment.
    call("getpaymasterfinancestatus", params, false, nullptr,
         [this](const UniValue& result) { if (!m_privacy) { applyOverviewFinance(result); m_overview_finance_age.start(); } },
         false, [this](const QString&) { if (!m_privacy) { overviewFinanceUnavailable(); m_overview_finance_age.start(); } }, background);
}

void PaymasterWidgetImpl::clearFinanceValues()
{
    for (auto* value : m_finance_values) value->setText(QStringLiteral("—"));
    for (size_t index = 0; index < m_finance_period_income.size(); ++index) {
        m_finance_period_income.at(index)->setText(QStringLiteral("—"));
        m_finance_period_cost.at(index)->setText(QStringLiteral("—"));
        m_finance_period_transfers.at(index)->setText(QStringLiteral("—"));
    }
    m_finance_result_estimate->setText(tr("No estimate loaded"));
    m_finance_pending_maintenance->clear();
    m_finance_history_notice->hide();
    m_finance_history_notice_text->clear();
}

void PaymasterWidgetImpl::requestSelectedFinanceStatus()
{
    m_finance_selected_summary->setText(tr("Loading the selected reporting period…"));
    const QString requested_period =
        m_finance_period_select->currentData().toString();
    if (!m_finance_pages.empty()) {
        const UniValue& displayed_period =
            m_finance_pages.front().result.find_value("period");
        if (!displayed_period.isStr() ||
            QString::fromStdString(displayed_period.get_str()) !=
                requested_period) {
            // Never leave a successfully loaded page visible under a
            // different period selector while its replacement is pending
            // or after that replacement fails validation.
            m_finance_pages.clear();
            m_finance_page_index = -1;
            m_finance_loaded = false;
            m_finance_last_result = UniValue{UniValue::VOBJ};
            clearFinanceValues();
            m_finance_daily_totals->setRowCount(0);
            m_finance_events->setRowCount(0);
        }
    }
    requestFinancePage(requested_period, {}, /*replace_pages=*/true);
}

void PaymasterWidgetImpl::requestFinancePage(const QString& requested_period,
                        const QString& cursor, bool replace_pages)
{
    UniValue options{UniValue::VOBJ};
    options.pushKV("period", requested_period.toStdString());
    options.pushKV("include_events", true);
    options.pushKV("limit", FINANCE_PAGE_SIZE);
    if (!cursor.isEmpty()) options.pushKV("cursor", cursor.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    setFinanceLoading(true, /*exporting=*/false);
    call("getpaymasterfinancestatus", std::move(params), false, nullptr,
         [this, requested_period, cursor,
          replace_pages](const UniValue& result) {
             if (m_finance_period_select->currentData().toString() !=
                 requested_period) {
                // Never label an old asynchronous response as the newly
                // selected period. Queue the authoritative replacement as
                // this handler's single serialized continuation.
                setFinanceLoading(false, /*exporting=*/false);
                requestSelectedFinanceStatus();
                return;
             }
             if (!IsCompleteFinanceStatus(result, requested_period)) {
                 finishFinancePageFailure(tr(
                     "Finance data could not be loaded: Core returned an incomplete or mismatched provider-finance record."));
                 return;
             }
             const QString provider_id = QString::fromStdString(
                 result.find_value("provider_id").get_str());
             if (!replace_pages &&
                 (m_finance_pages.empty() ||
                  QString::fromStdString(
                      m_finance_pages.front().result
                          .find_value("provider_id").get_str()) !=
                      provider_id ||
                  m_finance_pages.back().next_cursor != cursor ||
                  !SameNumericField(m_finance_pages.front().result, result, "revision"))) {
                 finishFinancePageFailure(tr(
                     "Finance data could not be loaded: The provider, cursor or finance revision changed. Refresh the report to restart paging."));
                 return;
             }

             FinancePage page;
             page.result = result;
             page.request_cursor = cursor;
             std::set<QString> page_event_ids;
             for (const UniValue& event :
                  result.find_value("events").getValues()) {
                 const QString event_id = QString::fromStdString(
                     event.find_value("event_id").get_str());
                 if (!page_event_ids.insert(event_id).second) {
                     finishFinancePageFailure(tr(
                         "Finance data could not be loaded: Core returned a duplicate event in one page."));
                     return;
                 }
                 if (!replace_pages) {
                     for (const FinancePage& existing_page :
                          m_finance_pages) {
                         const bool duplicate = std::any_of(
                             existing_page.events.cbegin(),
                             existing_page.events.cend(),
                             [&event_id](const UniValue& existing) {
                                 return QString::fromStdString(
                                     existing.find_value("event_id")
                                         .get_str()) == event_id;
                             });
                         if (duplicate) {
                             finishFinancePageFailure(tr(
                                 "Finance data could not be loaded: Core repeated an event across cursor pages."));
                             return;
                         }
                     }
                 }
                 page.events.push_back(event);
             }
             const UniValue& next_cursor =
                 result.find_value("next_cursor");
             page.next_cursor = next_cursor.isStr()
                 ? QString::fromStdString(next_cursor.get_str())
                 : QString{};
             if (!page.next_cursor.isEmpty() &&
                 (page.events.empty() ||
                  page.next_cursor == cursor)) {
                 finishFinancePageFailure(tr(
                     "Finance data could not be loaded: Core returned an invalid next-page cursor."));
                 return;
             }

             if (replace_pages) {
                 m_finance_pages.clear();
                 m_finance_pages.push_back(std::move(page));
                 m_finance_page_index = 0;
                 m_finance_loaded = true;
             } else {
                 m_finance_pages.push_back(std::move(page));
                 m_finance_page_index =
                     static_cast<int>(m_finance_pages.size()) - 1;
             }
             setFinanceLoading(false, /*exporting=*/false);
             renderFinancePage(m_finance_page_index);
         }, true, [this, requested_period](const QString& error) {
             if (m_finance_period_select->currentData().toString() !=
                 requested_period) {
                 setFinanceLoading(false, /*exporting=*/false);
                 requestSelectedFinanceStatus();
                 return;
             }
             finishFinancePageFailure(
                 tr("Finance data could not be loaded: %1").arg(error));
         });
}

void PaymasterWidgetImpl::showPreviousFinancePage()
{
    if (m_finance_loading || m_privacy || m_finance_page_index <= 0) return;
    renderFinancePage(--m_finance_page_index);
}

void PaymasterWidgetImpl::showNextFinancePage()
{
    if (m_finance_loading || m_privacy || m_finance_page_index < 0 ||
        m_finance_page_index >= static_cast<int>(m_finance_pages.size())) {
        return;
    }
    if (m_finance_page_index + 1 <
        static_cast<int>(m_finance_pages.size())) {
        renderFinancePage(++m_finance_page_index);
        return;
    }
    const QString cursor =
        m_finance_pages.at(m_finance_page_index).next_cursor;
    if (!cursor.isEmpty()) {
        requestFinancePage(
            m_finance_period_select->currentData().toString(), cursor,
            /*replace_pages=*/false);
    }
}

void PaymasterWidgetImpl::setFinanceLoading(bool loading, bool exporting)
{
    m_finance_loading = loading;
    m_finance_export_loading = loading && exporting;
    m_finance_refresh->setText(
        loading && !exporting ? tr("Loading…") : tr("Refresh"));
    m_finance_export->setText(
        loading && exporting ? tr("Preparing export…")
                             : tr("Export CSV…"));
    updateFinanceControls();
}

void PaymasterWidgetImpl::updateFinanceControls()
{
    const bool has_page = m_finance_page_index >= 0 &&
        m_finance_page_index < static_cast<int>(m_finance_pages.size());
    m_finance_period_select->setEnabled(!m_finance_loading && !m_busy);
    m_finance_refresh->setEnabled(!m_finance_loading && !m_busy);
    m_finance_export->setEnabled(
        m_finance_loaded && !m_finance_loading && !m_privacy &&
        !m_busy);
    m_finance_previous_page->setEnabled(
        has_page && !m_finance_loading && !m_privacy && !m_busy &&
        m_finance_page_index > 0);
    const bool cached_next = has_page && m_finance_page_index + 1 <
        static_cast<int>(m_finance_pages.size());
    const bool remote_next = has_page &&
        !m_finance_pages.at(m_finance_page_index).next_cursor.isEmpty();
    m_finance_next_page->setEnabled(
        !m_finance_loading && !m_privacy && !m_busy &&
        (cached_next || remote_next));

    if (m_finance_export_loading) {
        m_finance_page_status->setText(
            tr("Loading all pages for export…"));
    } else if (m_privacy) {
        m_finance_page_status->setText(
            tr("Booking details hidden by privacy mode"));
    } else if (!has_page) {
        m_finance_page_status->setText(tr("No booking page loaded"));
    } else {
        const int rows = static_cast<int>(
            m_finance_pages.at(m_finance_page_index).events.size());
        m_finance_page_status->setText(
            tr("Page %1 · %2 booking(s)")
                .arg(m_finance_page_index + 1)
                .arg(rows));
    }
}

void PaymasterWidgetImpl::finishFinancePageFailure(const QString& message)
{
    setFinanceLoading(false, /*exporting=*/false);
    m_finance_history_status->setText(message);
    m_finance_selected_summary->setText(message);
}

void PaymasterWidgetImpl::renderFinancePage(int index)
{
    if (index < 0 || index >= static_cast<int>(m_finance_pages.size())) {
        return;
    }
    m_finance_page_index = index;
    const FinancePage& page = m_finance_pages.at(index);
    applyFinanceStatus(page.result, page.events);
    updateFinanceControls();
}

void PaymasterWidgetImpl::applyFinanceStatus(const UniValue& result,
                        const std::vector<UniValue>& events)
{
    if (!result.isObject()) return;
    m_finance_last_result = result;
    applyOverviewFinance(result);
    const auto set_value = [this](const QString& key, const QString& text) {
        m_finance_values.value(key)->setText(maskNumericText(text));
    };
    const UniValue& summaries = result.find_value("period_summaries");
    const std::array<const char*, 4> keys{{"today", "7d", "30d", "all"}};
    for (size_t index = 0; index < keys.size(); ++index) {
        const UniValue& summary = summaries.find_value(keys.at(index));
        m_finance_period_income.at(index)->setText(maskNumericText(ddAmount(financeNumber(summary, "service_fee_income_cents"))));
        m_finance_period_cost.at(index)->setText(maskNumericText(compactDgbAmount(financeNumber(summary, "dgb_operating_cost_satoshis"))));
        m_finance_period_transfers.at(index)->setText(maskNumericText(QString::number(financeNumber(summary, "successful_transfers"))));
    }

    const qint64 income = financeNumber(result, "service_fee_income_cents");
    const qint64 cost = financeNumber(result, "dgb_operating_cost_satoshis");
    const qint64 transfers = financeNumber(result, "successful_transfers");
    m_finance_selected_summary->setText(m_privacy ? tr("Financial summary hidden by privacy mode") :
        tr("%1 · confirmed bookings").arg(m_finance_period_select->currentText()));
    set_value(QStringLiteral("income"), tr("%1 DD").arg(ddAmount(income)));
    set_value(QStringLiteral("costs"), tr("%1 DGB").arg(compactDgbAmount(cost)));
    set_value(QStringLiteral("transfers"), QString::number(transfers));
    set_value(QStringLiteral("average"), transfers > 0 ? tr("Average fee: %1 DD / payment").arg(ddAmount(financeNumber(result, "average_service_fee_cents"))) : tr("No confirmed payments in this period"));

    // Model totals contain only transfer fees; the remainder of the same
    // selected-period total is reserve maintenance, not locked capital.
    // Subtract with bounds checks so incomplete/inconsistent replies cannot
    // produce a plausible negative expense or overflow a display total.
    const UniValue& models = result.find_value("model_breakdown");
    qint64 maintenance_cost = cost;
    bool breakdown_known = cost >= 0;
    for (const auto* key : {"user_paid", "public_sponsored", "restricted_sponsored"}) {
        const UniValue& model = models.find_value(key);
        qint64 model_cost{0};
        if (!GetInt64Field(model, "dgb_operating_cost_satoshis", model_cost) || model_cost < 0 || model_cost > maintenance_cost) {
            breakdown_known = false;
        } else {
            maintenance_cost -= model_cost;
        }
        const QString prefix = QString::fromLatin1(key) + QLatin1Char('_');
        set_value(prefix + QStringLiteral("transfers"), QString::number(financeNumber(model, "successful_transfers")));
        set_value(prefix + QStringLiteral("income"), ddAmount(financeNumber(model, "service_fee_income_cents")));
        set_value(prefix + QStringLiteral("costs"), compactDgbAmount(model_cost));
    }
    set_value(QStringLiteral("maintenance_transfers"), QStringLiteral("—"));
    set_value(QStringLiteral("maintenance_income"), QStringLiteral("—"));
    set_value(QStringLiteral("maintenance_costs"), breakdown_known ? compactDgbAmount(maintenance_cost) : tr("Unavailable"));
    set_value(QStringLiteral("total_transfers"), QString::number(transfers));
    set_value(QStringLiteral("total_income"), ddAmount(income));
    set_value(QStringLiteral("total_costs"), compactDgbAmount(cost));

    const UniValue& estimate = result.find_value("estimated_result_usd");
    const UniValue& oracle = result.find_value("oracle_price_micro_usd");
    const UniValue& valuation_time = result.find_value("valuation_time");
    if (estimate.isNum() && oracle.isNum() && oracle.getInt<qint64>() > 0) {
        m_finance_result_estimate->setText(maskNumericText(tr("≈ %1 USD").arg(estimate.get_real(), 0, 'f', 2)));
        set_value(QStringLiteral("valuation_price"), tr("1 DGB = %1 USD").arg(oracle.getInt<qint64>() / 1000000.0, 0, 'f', 6));
        set_value(QStringLiteral("valuation_time"), valuation_time.isNum()
            ? tr("Valued at: %1 UTC").arg(GUIUtil::dateTimeStr(QDateTime::fromSecsSinceEpoch(valuation_time.getInt<qint64>(), Qt::UTC)))
            : tr("Valuation timestamp unavailable"));
    } else {
        m_finance_result_estimate->setText(tr("Estimate unavailable"));
        set_value(QStringLiteral("valuation_price"), tr("No current Oracle price is available."));
        set_value(QStringLiteral("valuation_time"), QString{});
    }

    const UniValue& pool = result.find_value("pool_capital");
    for (const auto* key : {"dgb_available", "dgb_reserved", "dgb_pending"}) {
        const std::string field = std::string(key) + "_satoshis";
        set_value(QString::fromLatin1(key), tr("%1 DGB").arg(compactDgbAmount(financeNumber(pool, field.c_str()))));
    }
    for (const auto* key : {"carrier_base", "carrier_earned", "carrier_withdrawable"}) {
        const std::string field = std::string(key) + "_cents";
        set_value(QString::fromLatin1(key), tr("%1 DD").arg(ddAmount(financeNumber(pool, field.c_str()))));
    }
    const qint64 pending = financeNumber(pool, "pending_maintenance_transactions");
    m_finance_pending_maintenance->setText(maskNumericText(pending == 0
        ? tr("No reserve transaction is awaiting completion.")
        : tr("Reserve transactions awaiting completion: %1").arg(pending)));

    const bool partial = result.find_value(
        "history_partially_reconstructable").isBool() &&
        result.find_value("history_partially_reconstructable").get_bool();
    const qint64 complete_from = financeNumber(
        result, "history_complete_from");
    const QString complete_date = complete_from > 0
        ? QDateTime::fromSecsSinceEpoch(complete_from, Qt::UTC)
              .toString(Qt::ISODate)
        : tr("unknown");
    m_finance_history_notice->setVisible(partial);
    if (partial) {
        m_finance_history_notice_text->setText(maskNumericText(tr(
            "The totals below are complete only from %1 UTC. Earlier Paymaster transfers may be absent because their exact provider fee and transaction role cannot be proven from the remaining wallet records. New transfers are recorded automatically; no earlier income is estimated.")
                                                   .arg(complete_date)));
    }
    m_finance_history_status->setText(m_privacy
        ? tr("Finance totals and booking details are hidden by privacy mode.")
        : partial
              ? tr("History is complete since %1 UTC. Earlier unprovable transfers are excluded from every total and export.")
                    .arg(complete_date)
              : tr("History is complete since %1 UTC.").arg(complete_date));

    const UniValue& daily_totals = result.find_value("daily_totals");
    m_finance_daily_totals->setRowCount(
        daily_totals.isArray()
            ? static_cast<int>(daily_totals.size()) : 0);
    if (daily_totals.isArray()) {
        int row{0};
        for (const UniValue& total : daily_totals.getValues()) {
            const qint64 day_start = financeNumber(total, "day_start");
            const std::array<QString, 5> cells{{
                QDateTime::fromSecsSinceEpoch(day_start, Qt::UTC)
                    .date().toString(Qt::ISODate),
                ddAmount(financeNumber(
                    total, "service_fee_income_cents")),
                dgbAmount(financeNumber(
                    total, "dgb_operating_cost_satoshis")),
                QString::number(financeNumber(
                    total, "successful_transfers")),
                QString::number(financeNumber(
                    total, "maintenance_transactions")),
            }};
            for (int column = 0;
                 column < static_cast<int>(cells.size()); ++column) {
                auto* item = new QTableWidgetItem(cells.at(column));
                item->setTextAlignment((column == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
                m_finance_daily_totals->setItem(row, column, item);
            }
            ++row;
        }
    }

    m_finance_events->setRowCount(static_cast<int>(events.size()));
    {
        int row{0};
        for (const UniValue& event : events) {
            const qint64 created_at = financeNumber(event, "created_at");
            const QString kind = event.find_value("kind").isStr()
                ? QString::fromStdString(event.find_value("kind").get_str())
                : QString{};
            const QString state = event.find_value("state").isStr()
                ? QString::fromStdString(event.find_value("state").get_str())
                : QString{};
            const std::array<QString, 6> cells{{
                QDateTime::fromSecsSinceEpoch(created_at, Qt::UTC)
                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                financeEventKind(kind), financeEventState(state),
                ddAmount(financeNumber(event, "dd_income_cents")),
                dgbAmount(financeNumber(event, "dgb_cost_satoshis")),
                financeFundingModel(event),
            }};
            for (int column = 0; column < static_cast<int>(cells.size()); ++column) {
                auto* item = new QTableWidgetItem(cells.at(column));
                item->setTextAlignment((column == 3 || column == 4 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
                item->setToolTip(cells.at(column));
                m_finance_events->setItem(row, column, item);
            }
            ++row;
        }
    }
    m_finance_daily_totals->setVisible(!m_privacy);
    m_finance_events->setVisible(!m_privacy);
    const UniValue& provider = result.find_value("provider_id");
    const QString provider_id = provider.isStr()
        ? QString::fromStdString(provider.get_str()) : m_provider_id;
    const bool backup_required = result.find_value("backup_required").isBool() &&
                                 result.find_value("backup_required").get_bool();
    updateBackupReminder(backup_required, provider_id);
}

QString PaymasterWidgetImpl::csvCell(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

void PaymasterWidgetImpl::exportFinanceCsv()
{
    if (m_privacy) {
        QMessageBox::information(
            this, tr("Export provider accounting"),
            tr("Disable privacy mode before exporting provider accounting."));
        return;
    }
    if (!m_finance_loaded || m_finance_pages.empty()) {
        QMessageBox::information(
            this, tr("Export provider accounting"),
            tr("Refresh the selected reporting period before exporting."));
        return;
    }
    QString filename = m_finance_export_filename_for_testing;
    if (filename.isEmpty()) {
        if (QMessageBox::information(
                this, tr("Export provider accounting"),
                tr("This CSV contains accounting figures for the selected period. "
                   "It is not a wallet backup and cannot restore the provider identity, "
                   "keys, pool state or ledger."),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok) != QMessageBox::Ok) {
            return;
        }
        filename = GUIUtil::getSaveFileName(
            this, tr("Export Paymaster accounting"), QString(),
            tr("Comma-separated values") + QLatin1String(" (*.csv)"), nullptr);
    }
    if (filename.isEmpty()) return;

    auto state = std::make_shared<FinanceExportState>();
    state->filename = filename;
    state->period = m_finance_period_select->currentData().toString();
    setFinanceLoading(true, /*exporting=*/true);
    m_finance_history_status->setText(tr(
        "Loading every booking page for a complete export…"));
    requestFinanceExportPage(state, {});
}

void PaymasterWidgetImpl::requestFinanceExportPage(
    const std::shared_ptr<FinanceExportState>& state,
    const QString& cursor)
{
    if (!cursor.isEmpty() &&
        !state->requested_cursors.insert(cursor).second) {
        finishFinanceExportFailure(tr(
            "Export stopped: Core repeated a finance-page cursor. No file was written."));
        return;
    }

    UniValue options{UniValue::VOBJ};
    options.pushKV("period", state->period.toStdString());
    options.pushKV("include_events", true);
    options.pushKV("limit", FINANCE_PAGE_SIZE);
    if (!cursor.isEmpty()) options.pushKV("cursor", cursor.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    call("getpaymasterfinancestatus", std::move(params), false, nullptr,
         [this, state, cursor](const UniValue& result) {
             if (m_privacy) {
                 finishFinanceExportFailure(tr(
                     "Export stopped because privacy mode was enabled. No file was written."));
                 return;
             }
             if (!IsCompleteFinanceStatus(result, state->period)) {
                 finishFinanceExportFailure(tr(
                     "Export stopped: Core returned an incomplete or mismatched provider-finance page. No file was written."));
                 return;
             }
             const QString provider_id = QString::fromStdString(
                 result.find_value("provider_id").get_str());
             if (state->provider_id.isEmpty()) {
                 state->provider_id = provider_id;
                 state->summary = result;
             } else if (state->provider_id != provider_id ||
                        !SameNumericField(state->summary, result, "revision")) {
                 finishFinanceExportFailure(tr(
                     "Export stopped: the provider identity or finance revision changed between pages. No file was written."));
                 return;
             }

             const UniValue& page_events = result.find_value("events");
             for (const UniValue& event : page_events.getValues()) {
                 const QString event_id = QString::fromStdString(
                     event.find_value("event_id").get_str());
                 if (!state->event_ids.insert(event_id).second) {
                     finishFinanceExportFailure(tr(
                         "Export stopped: Core repeated an event across finance pages. No file was written."));
                     return;
                 }
                 state->events.push_back(event);
             }

             const UniValue& next_value =
                 result.find_value("next_cursor");
             const QString next_cursor = next_value.isStr()
                 ? QString::fromStdString(next_value.get_str())
                 : QString{};
             if (!next_cursor.isEmpty() &&
                 (page_events.empty() ||
                  next_cursor == cursor ||
                  state->requested_cursors.count(next_cursor) != 0)) {
                 finishFinanceExportFailure(tr(
                     "Export stopped: Core returned an invalid next-page cursor. No file was written."));
                 return;
             }

             m_finance_history_status->setText(tr(
                 "Preparing complete export… %1 booking(s) loaded")
                 .arg(static_cast<qulonglong>(state->events.size())));
             if (!next_cursor.isEmpty()) {
                 const uint64_t wallet_generation =
                     m_wallet_generation;
                 // Yield between pages even with the synchronous widget-
                 // test executor. Large exports therefore stay responsive
                 // and never build a recursive handler stack.
                 QTimer::singleShot(
                     0, this,
                     [this, state, next_cursor, wallet_generation] {
                         if (m_wallet_generation != wallet_generation) {
                             return;
                         }
                         if (m_privacy) {
                             finishFinanceExportFailure(tr(
                                 "Export stopped because privacy mode was enabled. No file was written."));
                             return;
                         }
                         requestFinanceExportPage(state, next_cursor);
                     });
                 return;
             }

             writeFinanceCsv(state);
         }, /*show_error=*/false,
         [this](const QString& error) {
             finishFinanceExportFailure(tr(
                 "Export stopped while loading finance history: %1. No file was written.")
                 .arg(error));
         });
}

void PaymasterWidgetImpl::finishFinanceExportFailure(const QString& message)
{
    setFinanceLoading(false, /*exporting=*/true);
    m_finance_history_status->setText(message);
}

void PaymasterWidgetImpl::cancelFinanceWrite()
{
    if (!m_finance_write_phase) return;
    auto expected = FinanceWritePhase::WRITING;
    m_finance_write_phase->compare_exchange_strong(expected, FinanceWritePhase::CANCELLED);
    m_finance_write_phase.reset();
}

void PaymasterWidgetImpl::writeFinanceCsv(const std::shared_ptr<FinanceExportState>& state)
{
    // The paged RPC reads already yield. Serialization, file writes and
    // atomic replacement must not put the final unbounded step back on Qt.
    auto phase = std::make_shared<std::atomic<FinanceWritePhase>>(FinanceWritePhase::WRITING);
    m_finance_write_phase = phase;
    m_finance_history_status->setText(tr("Writing complete export…"));
    const auto generation = m_wallet_generation;
    QPointer<PaymasterWidgetImpl> guard{this};
    QThread* writer = QThread::create([guard, state, phase, generation] {
        QString failure;
        try {
            failure = writeFinanceFile(*state, *phase);
        } catch (const std::exception& error) {
            failure = tr("Export failed: %1").arg(QString::fromUtf8(error.what()));
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, state, phase, generation, failure] {
            if (!guard || guard->m_wallet_generation != generation || guard->m_finance_write_phase != phase) return;
            guard->m_finance_write_phase.reset();
            guard->setFinanceLoading(false, /*exporting=*/true);
            if (!failure.isEmpty()) {
                guard->finishFinanceExportFailure(failure);
                showPlainTextWarning(guard, tr("Export failed"), failure);
                return;
            }
            guard->m_finance_history_status->setText(tr("Complete export written: %1 booking(s).")
                .arg(static_cast<qulonglong>(state->events.size())));
            if (guard->m_finance_export_filename_for_testing.isEmpty())
                QMessageBox::information(guard, tr("Export complete"), tr("The selected accounting period was exported successfully."));
        }, Qt::QueuedConnection);
    });
    connect(writer, &QThread::finished, writer, &QObject::deleteLater);
    writer->start();
}

QString PaymasterWidgetImpl::writeFinanceFile(const FinanceExportState& data, std::atomic<FinanceWritePhase>& phase)
{
    const auto cancelled = [] { return tr("Export stopped before file replacement. No destination file was replaced."); };
    if (phase.load() == FinanceWritePhase::CANCELLED) return cancelled();
    QSaveFile file(data.filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return tr("Could not open %1 for writing. No destination file was replaced.").arg(data.filename);
    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    const UniValue& oracle = data.summary.find_value("oracle_price_micro_usd");
    const bool include_valuation = oracle.isNum();
    stream << "utc_time,booking,status,dd_income,dgb_cost,payment_model";
    if (include_valuation) stream << ",current_value_usd";
    stream << "\n";
    for (const UniValue& event : data.events) {
        if (phase.load() == FinanceWritePhase::CANCELLED) return cancelled();
        const qint64 created_at = financeNumber(event, "created_at");
        const qint64 dd_income = financeNumber(event, "dd_income_cents");
        const qint64 dgb_cost = financeNumber(event, "dgb_cost_satoshis");
        const QString kind = event.find_value("kind").isStr()
            ? QString::fromStdString(event.find_value("kind").get_str())
            : QString{};
        const QString state = event.find_value("state").isStr()
            ? QString::fromStdString(event.find_value("state").get_str())
            : QString{};
        stream << csvCell(QDateTime::fromSecsSinceEpoch(
                              created_at, Qt::UTC).toString(Qt::ISODate))
               << ',' << csvCell(financeEventKind(kind))
               << ',' << csvCell(financeEventState(state))
               << ',' << ddAmount(dd_income)
               << ',' << dgbAmount(dgb_cost)
               << ',' << csvCell(financeFundingModel(event));
        if (include_valuation) {
            const long double current_value =
                static_cast<long double>(dd_income) / 100.0L -
                static_cast<long double>(dgb_cost) *
                    oracle.getInt<qint64>() / 100000000.0L / 1000000.0L;
            stream << ',' << QString::number(
                static_cast<double>(current_value), 'f', 8);
        }
        stream << '\n';
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        return tr("Export could not be written completely. No destination file was replaced.");
    // Cancellation wins until atomic replacement starts. Once committing,
    // the already authorized export finishes without blocking navigation.
    auto expected = FinanceWritePhase::WRITING;
    if (!phase.compare_exchange_strong(expected, FinanceWritePhase::COMMITTING)) return cancelled();
    if (!file.commit()) return tr("Could not finish writing %1. No partial destination file was accepted.").arg(data.filename);
    return {};
}

void PaymasterWidgetImpl::openLiquidityFinanceControl(QWidget* control)
{
    const bool automation = control && m_automation_page->isAncestorOf(control);
    auto* page = automation ? m_automation_page : m_liquidity_page;
    showOperatorPage(page);
    auto* disclosure = automation ? m_maintenance_limits_toggle : m_liquidity_advanced_toggle;
    if (disclosure) disclosure->setChecked(true);
    if (control) {
        control->setFocus(Qt::OtherFocusReason);
        if (auto* scroll = qobject_cast<QScrollArea*>(page)) {
            scroll->ensureWidgetVisible(control, 24, 24);
        }
    }
}
