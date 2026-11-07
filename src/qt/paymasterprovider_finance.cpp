// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/paymasterprovider_internal.h>

void PaymasterWidgetImpl::setFinanceExportFilenameForTesting(QString filename)
{
    m_finance.setExportFilenameForTesting(std::move(filename));
}

void ProviderFinance::setExportFilenameForTesting(QString filename)
{
    m_finance_export_filename_for_testing = std::move(filename);
}

void ProviderFinance::resetWalletPresentation()
{
    cancelFinanceWrite();
    for (auto& serial : m_read_serials) ++serial;
    m_finance_last_result = UniValue{UniValue::VOBJ};
    m_finance_selected_summary->setText(tr("Load the selected reporting period to view income and costs."));
    m_finance_pages.clear();
    m_finance_event_ids.clear();
    m_finance_page_index = -1;
    m_finance_loaded = false;
    m_finance_loading = false;
    m_finance_export_loading = false;
    m_finance_refresh->setText(tr("Refresh"));
    m_finance_export->setText(tr("Export CSV…"));
    m_finance_daily_totals->setRowCount(0);
    m_finance_events->setRowCount(0);
    setHistoryMessage(tr("Finance history has not been loaded yet."));
    clearFinanceValues();
    updateFinanceControls();
}

void ProviderFinance::setHistoryMessage(const QString& message)
{
    m_finance_history_status->setText(message);
}

void ProviderFinance::clearFundsValues()
{
    for (auto* value : m_funds_values) value->setText(QStringLiteral("—"));
}

void ProviderFinance::setFundsValue(const QString& key, const QString& text)
{
    if (auto* value = m_funds_values.value(key)) value->setText(m_panel.maskNumericText(text));
}

void ProviderFinance::setPrivacy(bool privacy)
{
    if (privacy) {
        for (auto& serial : m_read_serials) ++serial;
        cancelFinanceWrite();
        m_finance_loading = m_finance_export_loading = false;
        clearFundsValues();
        m_finance_selected_summary->setText(tr("Financial summary hidden by privacy mode"));
    }
    m_finance_daily_totals->setVisible(!privacy);
    m_finance_events->setVisible(!privacy);
    if (m_finance_page_index >= 0 && m_finance_page_index < static_cast<int>(m_finance_pages.size())) {
        renderFinancePage(m_finance_page_index);
    }
    updateFinanceControls();
}

qint64 ProviderFinance::financeNumber(const UniValue& object, const char* name)
{
    qint64 value{0};
    return GetInt64Field(object, name, value) ? value : 0;
}

QString ProviderFinance::financeEventKind(const QString& kind)
{
    if (kind == QLatin1String("transfer")) return tr("Paymaster transfer");
    if (kind == QLatin1String("setup")) return tr("Pool setup");
    if (kind == QLatin1String("replenishment")) return tr("Liquidity replenishment");
    if (kind == QLatin1String("retirement")) return tr("Liquidity retirement");
    if (kind == QLatin1String("withdrawal")) return tr("Carrier withdrawal");
    if (kind == QLatin1String("recovery")) return tr("Payment recovery");
    return tr("Unknown booking");
}

QString ProviderFinance::financeEventState(const QString& state)
{
    if (state == QLatin1String("confirmed")) return tr("Confirmed");
    if (state == QLatin1String("pending")) return tr("Pending");
    if (state == QLatin1String("invalidated")) return tr("Reversed");
    return tr("Unknown");
}

QString ProviderFinance::financeFundingModel(const UniValue& event)
{
    if (event.find_value("kind").isStr() && event.find_value("kind").get_str() == "recovery") return tr("Recovery");
    const UniValue& model = event.find_value("funding_model");
    if (!model.isStr()) return tr("Maintenance");
    const QString funding = QString::fromStdString(model.get_str());
    if (funding == QLatin1String("user_paid")) return tr("User paid");
    const UniValue& scope = event.find_value("sponsorship_scope");
    return scope.isStr() && scope.get_str() == "restricted"
        ? tr("Restricted sponsored") : tr("Public sponsored");
}

void ProviderFinance::refreshFinanceStatus()
{
    if (!m_panel.hasRpcTransport() || m_panel.busy() || !m_finance_period_select) {
        if (m_finance_history_status) {
            m_finance_history_status->setText(!m_panel.hasRpcTransport()
                ? tr("Finance refresh was not started because no wallet RPC transport is available.")
                : m_panel.busy()
                    ? tr("Finance refresh was not started because another wallet operation is still running.")
                    : tr("Finance refresh was not started because the reporting-period control is unavailable."));
        }
        return;
    }
    requestSelectedFinanceStatus();
}

bool ProviderFinance::retryFinanceReconciliation(FinanceRead read, uint64_t serial, std::function<void()> retry)
{
    if (PaymasterQt::StableCode(m_panel.rpcFailure()) != QLatin1String("PAYMASTER_FINANCE_RECONCILING")) return false;
    const auto generation = m_panel.walletGeneration();
    if (read == FinanceRead::OVERVIEW) {
        m_panel.overview().showFinanceReconciliation();
    } else {
        m_finance_history_status->setText(tr("Reconciling financial history… The report will refresh automatically."));
    }
    QTimer::singleShot(250, this, [this, read, serial, generation, retry = std::move(retry)] {
        if (m_panel.walletGeneration() != generation ||
            m_read_serials[static_cast<size_t>(read)] != serial) return;
        if (m_panel.privacyEnabled()) {
            if (read != FinanceRead::OVERVIEW) setFinanceLoading(false, read == FinanceRead::EXPORT);
            return;
        }
        retry(); // Only the same read is retried; no payment or funding action.
    });
    return true;
}





void ProviderFinance::refreshOverviewFinance(bool background)
{
    if (!m_panel.overview().hasIdentity() || m_panel.privacyEnabled()) return;
    const auto serial = ++m_read_serials[static_cast<size_t>(FinanceRead::OVERVIEW)];
    UniValue options{UniValue::VOBJ}, params{UniValue::VARR};
    options.pushKV("period", "all");
    options.pushKV("include_events", false);
    params.push_back(options);
    // Serialized continuation of the operator read, with the same wallet
    // generation guard as all other calls. Never signs or sends a payment.
    m_panel.call("getpaymasterfinancestatus", params, false, nullptr,
         [this](const UniValue& result) { if (!m_panel.privacyEnabled()) { m_panel.overview().applyOverviewFinance(result); m_panel.overview().noteFinanceObservation(); } },
         false, [this, background, serial](const QString&) {
             if (retryFinanceReconciliation(FinanceRead::OVERVIEW, serial, [this, background] { refreshOverviewFinance(background); })) return;
             if (!m_panel.privacyEnabled()) { m_panel.overview().overviewFinanceUnavailable(); m_panel.overview().noteFinanceObservation(); }
         }, background);
}

void ProviderFinance::clearFinanceValues()
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

void ProviderFinance::requestSelectedFinanceStatus()
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
            m_finance_event_ids.clear();
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

void ProviderFinance::requestFinancePage(const QString& requested_period,
                        const QString& cursor, bool replace_pages)
{
    const auto serial = ++m_read_serials[static_cast<size_t>(FinanceRead::PAGE)];
    UniValue options{UniValue::VOBJ};
    options.pushKV("period", requested_period.toStdString());
    options.pushKV("include_events", true);
    options.pushKV("limit", FINANCE_PAGE_SIZE);
    if (!cursor.isEmpty()) options.pushKV("cursor", cursor.toStdString());
    UniValue params{UniValue::VARR};
    params.push_back(std::move(options));
    setFinanceLoading(true, /*exporting=*/false);
    m_panel.call("getpaymasterfinancestatus", std::move(params), false, nullptr,
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
                 if (!replace_pages && m_finance_event_ids.count(event_id) != 0) {
                     finishFinancePageFailure(tr(
                         "Finance data could not be loaded: Core repeated an event across cursor pages."));
                     return;
                 }
             }
             const UniValue& next_cursor =
                 result.find_value("next_cursor");
             page.next_cursor = next_cursor.isStr()
                 ? QString::fromStdString(next_cursor.get_str())
                 : QString{};
             if (!page.next_cursor.isEmpty() &&
                 (page.result.find_value("events").empty() ||
                  page.next_cursor == cursor)) {
                 finishFinancePageFailure(tr(
                     "Finance data could not be loaded: Core returned an invalid next-page cursor."));
                 return;
             }

             if (replace_pages) {
                 m_finance_pages.clear();
                 m_finance_pages.push_back(std::move(page));
                 m_finance_event_ids = std::move(page_event_ids);
                 m_finance_page_index = 0;
                 m_finance_loaded = true;
             } else {
                 m_finance_pages.push_back(std::move(page));
                 m_finance_event_ids.merge(page_event_ids);
                 m_finance_page_index =
                     static_cast<int>(m_finance_pages.size()) - 1;
             }
             setFinanceLoading(false, /*exporting=*/false);
             renderFinancePage(m_finance_page_index);
         }, true, [this, requested_period, cursor, replace_pages, serial](const QString& error) {
             if (m_finance_period_select->currentData().toString() !=
                 requested_period) {
                 setFinanceLoading(false, /*exporting=*/false);
                 requestSelectedFinanceStatus();
                 return;
             }
             if (retryFinanceReconciliation(FinanceRead::PAGE, serial,
                     [this, requested_period, cursor, replace_pages] {
                         if (m_finance_period_select->currentData().toString() == requested_period)
                             requestFinancePage(requested_period, cursor, replace_pages);
                     })) return;
             finishFinancePageFailure(
                 tr("Finance data could not be loaded: %1").arg(error));
         });
}

void ProviderFinance::showPreviousFinancePage()
{
    if (m_finance_loading || m_panel.privacyEnabled() || m_finance_page_index <= 0) return;
    renderFinancePage(--m_finance_page_index);
}

void ProviderFinance::showNextFinancePage()
{
    if (m_finance_loading || m_panel.privacyEnabled() || m_finance_page_index < 0 ||
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

void ProviderFinance::setFinanceLoading(bool loading, bool exporting)
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

void ProviderFinance::updateFinanceControls()
{
    const bool has_page = m_finance_page_index >= 0 &&
        m_finance_page_index < static_cast<int>(m_finance_pages.size());
    m_finance_period_select->setEnabled(!m_finance_loading && !m_panel.busy());
    m_finance_refresh->setEnabled(!m_finance_loading && !m_panel.busy());
    m_finance_export->setEnabled(
        m_finance_loaded && !m_finance_loading && !m_panel.privacyEnabled() &&
        !m_panel.busy());
    m_finance_previous_page->setEnabled(
        has_page && !m_finance_loading && !m_panel.privacyEnabled() && !m_panel.busy() &&
        m_finance_page_index > 0);
    const bool cached_next = has_page && m_finance_page_index + 1 <
        static_cast<int>(m_finance_pages.size());
    const bool remote_next = has_page &&
        !m_finance_pages.at(m_finance_page_index).next_cursor.isEmpty();
    m_finance_next_page->setEnabled(
        !m_finance_loading && !m_panel.privacyEnabled() && !m_panel.busy() &&
        (cached_next || remote_next));

    if (m_finance_export_loading) {
        m_finance_page_status->setText(
            tr("Loading all pages for export…"));
    } else if (m_panel.privacyEnabled()) {
        m_finance_page_status->setText(
            tr("Booking details hidden by privacy mode"));
    } else if (!has_page) {
        m_finance_page_status->setText(tr("No booking page loaded"));
    } else {
        const int rows = static_cast<int>(
            m_finance_pages.at(m_finance_page_index).result.find_value("events").size());
        m_finance_page_status->setText(
            tr("Page %1 · %2 booking(s)")
                .arg(m_finance_page_index + 1)
                .arg(rows));
    }
}

void ProviderFinance::finishFinancePageFailure(const QString& message)
{
    setFinanceLoading(false, /*exporting=*/false);
    m_finance_history_status->setText(message);
    m_finance_selected_summary->setText(message);
}

void ProviderFinance::renderFinancePage(int index)
{
    if (index < 0 || index >= static_cast<int>(m_finance_pages.size())) {
        return;
    }
    m_finance_page_index = index;
    const FinancePage& page = m_finance_pages.at(index);
    applyFinanceStatus(page.result, page.result.find_value("events").getValues());
    updateFinanceControls();
}

void ProviderFinance::applyFinanceStatus(const UniValue& result,
                        const std::vector<UniValue>& events)
{
    if (!result.isObject()) return;
    m_finance_last_result = result;
    m_panel.overview().applyOverviewFinance(result);
    const auto set_value = [this](const QString& key, const QString& text) {
        m_finance_values.value(key)->setText(m_panel.maskNumericText(text));
    };
    const UniValue& summaries = result.find_value("period_summaries");
    const std::array<const char*, 4> keys{{"today", "7d", "30d", "all"}};
    for (size_t index = 0; index < keys.size(); ++index) {
        const UniValue& summary = summaries.find_value(keys.at(index));
        m_finance_period_income.at(index)->setText(m_panel.maskNumericText(PaymasterWidgetImpl::ddAmount(financeNumber(summary, "service_fee_income_cents"))));
        m_finance_period_cost.at(index)->setText(m_panel.maskNumericText(PaymasterWidgetImpl::compactDgbAmount(financeNumber(summary, "dgb_operating_cost_satoshis"))));
        m_finance_period_transfers.at(index)->setText(m_panel.maskNumericText(QString::number(financeNumber(summary, "successful_transfers"))));
    }

    const qint64 income = financeNumber(result, "service_fee_income_cents");
    const qint64 cost = financeNumber(result, "dgb_operating_cost_satoshis");
    const qint64 transfers = financeNumber(result, "successful_transfers");
    m_finance_selected_summary->setText(m_panel.privacyEnabled() ? tr("Financial summary hidden by privacy mode") :
        tr("%1 · confirmed bookings").arg(m_finance_period_select->currentText()));
    set_value(QStringLiteral("income"), tr("%1 DD").arg(PaymasterWidgetImpl::ddAmount(income)));
    set_value(QStringLiteral("costs"), tr("%1 DGB").arg(PaymasterWidgetImpl::compactDgbAmount(cost)));
    set_value(QStringLiteral("transfers"), QString::number(transfers));
    set_value(QStringLiteral("average"), transfers > 0 ? tr("Average fee: %1 DD / payment").arg(PaymasterWidgetImpl::ddAmount(financeNumber(result, "average_service_fee_cents"))) : tr("No confirmed payments in this period"));

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
        set_value(prefix + QStringLiteral("income"), PaymasterWidgetImpl::ddAmount(financeNumber(model, "service_fee_income_cents")));
        set_value(prefix + QStringLiteral("costs"), PaymasterWidgetImpl::compactDgbAmount(model_cost));
    }
    set_value(QStringLiteral("maintenance_transfers"), QStringLiteral("—"));
    set_value(QStringLiteral("maintenance_income"), QStringLiteral("—"));
    set_value(QStringLiteral("maintenance_costs"), breakdown_known ? PaymasterWidgetImpl::compactDgbAmount(maintenance_cost) : tr("Unavailable"));
    set_value(QStringLiteral("total_transfers"), QString::number(transfers));
    set_value(QStringLiteral("total_income"), PaymasterWidgetImpl::ddAmount(income));
    set_value(QStringLiteral("total_costs"), PaymasterWidgetImpl::compactDgbAmount(cost));

    const UniValue& estimate = result.find_value("estimated_result_usd");
    const UniValue& oracle = result.find_value("oracle_price_micro_usd");
    const UniValue& valuation_time = result.find_value("valuation_time");
    if (estimate.isNum() && oracle.isNum() && oracle.getInt<qint64>() > 0) {
        m_finance_result_estimate->setText(m_panel.maskNumericText(tr("≈ %1 USD").arg(estimate.get_real(), 0, 'f', 2)));
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
        set_value(QString::fromLatin1(key), tr("%1 DGB").arg(PaymasterWidgetImpl::compactDgbAmount(financeNumber(pool, field.c_str()))));
    }
    for (const auto* key : {"carrier_base", "carrier_earned", "carrier_withdrawable"}) {
        const std::string field = std::string(key) + "_cents";
        set_value(QString::fromLatin1(key), tr("%1 DD").arg(PaymasterWidgetImpl::ddAmount(financeNumber(pool, field.c_str()))));
    }
    const qint64 pending = financeNumber(pool, "pending_maintenance_transactions");
    m_finance_pending_maintenance->setText(m_panel.maskNumericText(pending == 0
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
        m_finance_history_notice_text->setText(m_panel.maskNumericText(tr(
            "The totals below are complete only from %1 UTC. Earlier Paymaster transfers may be absent because their exact provider fee and transaction role cannot be proven from the remaining wallet records. New transfers are recorded automatically; no earlier income is estimated.")
                                                   .arg(complete_date)));
    }
    m_finance_history_status->setText(m_panel.privacyEnabled()
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
                PaymasterWidgetImpl::ddAmount(financeNumber(
                    total, "service_fee_income_cents")),
                PaymasterWidgetImpl::dgbAmount(financeNumber(
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
                PaymasterWidgetImpl::ddAmount(financeNumber(event, "dd_income_cents")),
                PaymasterWidgetImpl::dgbAmount(financeNumber(event, "dgb_cost_satoshis")),
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
    m_finance_daily_totals->setVisible(!m_panel.privacyEnabled());
    m_finance_events->setVisible(!m_panel.privacyEnabled());
    const UniValue& provider = result.find_value("provider_id");
    const QString provider_id = provider.isStr()
        ? QString::fromStdString(provider.get_str()) : m_panel.overview().providerId();
    const bool backup_required = result.find_value("backup_required").isBool() &&
                                 result.find_value("backup_required").get_bool();
    m_panel.recovery().updateBackupReminder(backup_required, provider_id);
}

QString ProviderFinance::csvCell(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

void ProviderFinance::exportFinanceCsv()
{
    if (m_panel.privacyEnabled()) {
        QMessageBox::information(
            widget(), tr("Export provider accounting"),
            tr("Disable privacy mode before exporting provider accounting."));
        return;
    }
    if (!m_finance_loaded || m_finance_pages.empty()) {
        QMessageBox::information(
            widget(), tr("Export provider accounting"),
            tr("Refresh the selected reporting period before exporting."));
        return;
    }
    QString filename = m_finance_export_filename_for_testing;
    if (filename.isEmpty()) {
        if (QMessageBox::information(
                widget(), tr("Export provider accounting"),
                tr("This CSV contains accounting figures for the selected period. "
                   "It is not a wallet backup and cannot restore the provider identity, "
                   "keys, pool state or ledger."),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok) != QMessageBox::Ok) {
            return;
        }
        filename = GUIUtil::getSaveFileName(
            widget(), tr("Export Paymaster accounting"), QString(),
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

void ProviderFinance::requestFinanceExportPage(
    const std::shared_ptr<FinanceExportState>& state,
    const QString& cursor)
{
    const auto serial = ++m_read_serials[static_cast<size_t>(FinanceRead::EXPORT)];
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
    m_panel.call("getpaymasterfinancestatus", std::move(params), false, nullptr,
         [this, state, cursor](const UniValue& result) {
             if (m_panel.privacyEnabled()) {
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
                     m_panel.walletGeneration();
                 // Yield between pages even with the synchronous widget-
                 // test executor. Large exports therefore stay responsive
                 // and never build a recursive handler stack.
                 QTimer::singleShot(
                     0, widget(),
                     [this, state, next_cursor, wallet_generation] {
                         if (m_panel.walletGeneration() != wallet_generation) {
                             return;
                         }
                         if (m_panel.privacyEnabled()) {
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
         [this, state, cursor, serial](const QString& error) {
             if (retryFinanceReconciliation(FinanceRead::EXPORT, serial,
                     [this, state, cursor] { requestFinanceExportPage(state, cursor); })) {
                 // The failed read consumed no page; permit that cursor once
                 // more without relaxing repeated-successful-page detection.
                 state->requested_cursors.erase(cursor);
                 return;
             }
             finishFinanceExportFailure(tr(
                 "Export stopped while loading finance history: %1. No file was written.")
                 .arg(error));
         });
}

void ProviderFinance::finishFinanceExportFailure(const QString& message)
{
    setFinanceLoading(false, /*exporting=*/true);
    m_finance_history_status->setText(message);
}

void ProviderFinance::cancelFinanceWrite()
{
    if (!m_finance_write_phase) return;
    auto expected = FinanceWritePhase::WRITING;
    m_finance_write_phase->compare_exchange_strong(expected, FinanceWritePhase::CANCELLED);
    m_finance_write_phase.reset();
}

void ProviderFinance::writeFinanceCsv(const std::shared_ptr<FinanceExportState>& state)
{
    // The paged RPC reads already yield. Serialization, file writes and
    // atomic replacement must not put the final unbounded step back on Qt.
    auto phase = std::make_shared<std::atomic<FinanceWritePhase>>(FinanceWritePhase::WRITING);
    m_finance_write_phase = phase;
    m_finance_history_status->setText(tr("Writing complete export…"));
    const auto generation = m_panel.walletGeneration();
    QPointer<ProviderFinance> guard{this};
    QThread* writer = QThread::create([guard, state, phase, generation] {
        QString failure;
        try {
            failure = writeFinanceFile(*state, *phase);
        } catch (const std::exception& error) {
            failure = tr("Export failed: %1").arg(QString::fromUtf8(error.what()));
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, state, phase, generation, failure] {
            if (!guard || guard->m_panel.walletGeneration() != generation || guard->m_finance_write_phase != phase) return;
            guard->m_finance_write_phase.reset();
            guard->setFinanceLoading(false, /*exporting=*/true);
            if (!failure.isEmpty()) {
                guard->finishFinanceExportFailure(failure);
                PaymasterWidgetImpl::showPlainTextWarning(guard->widget(), tr("Export failed"), failure);
                return;
            }
            guard->m_finance_history_status->setText(tr("Complete export written: %1 booking(s).")
                .arg(static_cast<qulonglong>(state->events.size())));
            if (guard->m_finance_export_filename_for_testing.isEmpty())
                QMessageBox::information(guard->widget(), tr("Export complete"), tr("The selected accounting period was exported successfully."));
        }, Qt::QueuedConnection);
    });
    connect(writer, &QThread::finished, writer, &QObject::deleteLater);
    writer->start();
}

QString ProviderFinance::writeFinanceFile(const FinanceExportState& data, std::atomic<FinanceWritePhase>& phase)
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
        // CSV amount cells are machine-readable decimals. Grouping belongs
        // only in the on-screen monetary display, never in exported numbers.
        stream << csvCell(QDateTime::fromSecsSinceEpoch(
                              created_at, Qt::UTC).toString(Qt::ISODate))
               << ',' << csvCell(financeEventKind(kind))
               << ',' << csvCell(financeEventState(state))
               << ',' << DigiByteUnits::format(DigiByteUnits::Unit::uDGB, dd_income,
                                              /*plussign=*/false, DigiByteUnits::SeparatorStyle::NEVER)
               << ',' << DigiByteUnits::format(DigiByteUnits::Unit::DGB, dgb_cost,
                                              /*plussign=*/false, DigiByteUnits::SeparatorStyle::NEVER)
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

QLabel* ProviderFinance::financeLabel(const QString& text, QWidget* parent, const char* role)
{
        auto* label = new QLabel(text, parent);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setProperty("paymasterRole", QString::fromLatin1(role));
        return label;
}

QLabel* ProviderFinance::fundsValue(const QString& key, QWidget* parent, const QString& caption, const char* role)
{
        auto* value = financeLabel(QStringLiteral("—"), parent, role);
        value->setObjectName(QStringLiteral("paymasterFunds_") + key);
        value->setAccessibleName(caption);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_funds_values.insert(key, value);
        return value;
}

void ProviderFinance::buildFinance(PageBindings& pages)
{
    auto* finance_scroll = new QScrollArea(m_panel.mainPages());
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
        auto* value = financeLabel(QStringLiteral("—"), parent, role);
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
            auto* label = financeLabel(headers.at(column), grid->parentWidget(), "financeColumnHeading");
            label->setAlignment((column ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
            grid->addWidget(label, 0, column);
            grid->setColumnStretch(column, column ? 2 : 3);
        }
    };

    m_finance_selected_summary = financeLabel(tr("Choose a period to load recorded income and costs."), finance_column, "cardHeading");
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
        layout->addWidget(financeLabel(metric_titles.at(index), card, "financeRowTitle"));
        layout->addWidget(finance_value(metric_keys.at(index), card, metric_titles.at(index), "financeAmount"));
        layout->addWidget(financeLabel(metric_help.at(index), card, "mutedText"));
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
    const std::array<QString, 5> source_titles{tr("Customer-paid payments"), tr("Public sponsorship"), tr("Restricted sponsorship"), tr("Recovery and reserve maintenance"), tr("Total")};
    const std::array<QString, 5> source_keys{QStringLiteral("user_paid"), QStringLiteral("public_sponsored"), QStringLiteral("restricted_sponsored"), QStringLiteral("maintenance"), QStringLiteral("total")};
    const std::array<QString, 3> source_columns{QStringLiteral("transfers"), QStringLiteral("income"), QStringLiteral("costs")};
    const std::array<QString, 3> source_column_titles{tr("Payments"), tr("Income (DD)"), tr("Costs (DGB)")};
    for (int row = 0; row < 5; ++row) {
        breakdown_grid->addWidget(financeLabel(source_titles.at(row), breakdown, row == 4 ? "financeTotal" : "financeRowTitle"), row + 1, 0);
        for (int column = 0; column < 3; ++column) {
            auto* value = finance_value(source_keys.at(row) + QLatin1Char('_') + source_columns.at(column), breakdown,
                source_titles.at(row) + QStringLiteral(" · ") + source_column_titles.at(column), row == 4 ? "financeTotal" : "financeValue");
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            breakdown_grid->addWidget(value, row + 1, column + 1);
        }
    }
    breakdown_layout->addWidget(financeLabel(tr("Recovery and reserve maintenance costs include returns to the sending wallet, setup, refill, withdrawal and release fees. Capital held in reserves is not an expense."), breakdown, "mutedText"));
    finance_layout->addWidget(breakdown);

    auto* comparison = finance_group(tr("Reporting periods compared"));
    comparison->setObjectName("paymasterFinancePeriodCards");
    auto* comparison_layout = new QVBoxLayout(comparison);
    auto* comparison_grid = new QGridLayout;
    comparison_layout->addLayout(comparison_grid);
    grid_headers(comparison_grid, {tr("Period"), tr("Income (DD)"), tr("Costs (DGB)"), tr("Payments")});
    const std::array<QString, 4> period_titles{tr("Today (UTC)"), tr("Last 7 days"), tr("Last 30 days"), tr("All recorded history")};
    for (size_t index = 0; index < period_titles.size(); ++index) {
        comparison_grid->addWidget(financeLabel(period_titles.at(index), comparison, "financeRowTitle"), index + 1, 0);
        const std::array<std::pair<QLabel**, QString>, 3> cells{{
            {&m_finance_period_income.at(index), QStringLiteral("paymasterFinancePeriodIncome%1").arg(index)},
            {&m_finance_period_cost.at(index), QStringLiteral("paymasterFinancePeriodCost%1").arg(index)},
            {&m_finance_period_transfers.at(index), QStringLiteral("paymasterFinancePeriodTransfers%1").arg(index)}}};
        for (int column = 0; column < 3; ++column) {
            auto* value = financeLabel(QStringLiteral("—"), comparison, "financeValue");
            value->setObjectName(cells.at(column).second);
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            *cells.at(column).first = value;
            comparison_grid->addWidget(value, index + 1, column + 1);
        }
    }
    comparison_layout->addWidget(financeLabel(tr("Periods overlap; do not add these rows together. Only confirmed bookings are included."), comparison, "mutedText"));
    AddPaymasterDisclosure(finance_layout, finance_column, comparison,
                           tr("Compare reporting periods"), tr("Hide period comparison"), QStringLiteral("paymasterFinanceCompareToggle"));

    auto* valuation = finance_group(tr("Estimate at the current DGB price"));
    auto* valuation_layout = new QVBoxLayout(valuation);
    valuation_layout->addWidget(financeLabel(tr("Estimated income minus costs"), valuation, "financeRowTitle"));
    m_finance_result_estimate = financeLabel(tr("No estimate loaded"), valuation, "financeAmount");
    m_finance_result_estimate->setObjectName("paymasterFinanceResultEstimate");
    valuation_layout->addWidget(m_finance_result_estimate);
    valuation_layout->addWidget(finance_value(QStringLiteral("valuation_price"), valuation, tr("DGB valuation price")));
    valuation_layout->addWidget(finance_value(QStringLiteral("valuation_time"), valuation, tr("Valuation timestamp"), "mutedText"));
    valuation_layout->addWidget(financeLabel(tr("DD income is valued at its USD denomination; DGB costs use the current oracle price. This is an estimate, not historical profit or loss."), valuation, "mutedText"));
    AddPaymasterDisclosure(finance_layout, finance_column, valuation,
                           tr("Estimate result in USD"), tr("Hide USD estimate"), QStringLiteral("paymasterFinanceValuationToggle"));

    auto* finance_pool_group = finance_group(tr("Currently held in reserves"));
    finance_pool_group->setObjectName("paymasterFinanceCurrentCapital");
    auto* finance_pool_layout = new QVBoxLayout(finance_pool_group);
    finance_pool_layout->addWidget(financeLabel(tr("Current wallet balances, independent of the reporting period. Reserve capital remains yours; accumulated fees are part of the income already recorded."), finance_pool_group, "mutedText"));
    auto* capital_columns = new PaymasterResponsiveCards(finance_pool_group);
    finance_pool_layout->addWidget(capital_columns);
    for (int asset = 0; asset < 2; ++asset) {
        auto* column = new QWidget(capital_columns);
        auto* layout = new QGridLayout(column);
        layout->setContentsMargins(0, 8, 0, 8);
        layout->setHorizontalSpacing(20);
        layout->setColumnStretch(0, 3);
        layout->setColumnStretch(1, 2);
        layout->addWidget(financeLabel(asset ? tr("DigiDollar reserves") : tr("DGB network-fee reserves"), column, "financeColumnHeading"), 0, 0, 1, 2);
        const QStringList titles = asset ? QStringList{tr("Base capital"), tr("Accumulated service fees"), tr("Fees available to withdraw")} : QStringList{tr("Available reserve funds"), tr("Reserved or committed"), tr("Awaiting confirmation")};
        const QStringList keys = asset ? QStringList{QStringLiteral("carrier_base"), QStringLiteral("carrier_earned"), QStringLiteral("carrier_withdrawable")} : QStringList{QStringLiteral("dgb_available"), QStringLiteral("dgb_reserved"), QStringLiteral("dgb_pending")};
        for (int row = 0; row < 3; ++row) {
            layout->addWidget(financeLabel(titles.at(row), column, "financeRowTitle"), row + 1, 0);
            auto* value = finance_value(keys.at(row), column, titles.at(row));
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(value, row + 1, 1);
        }
        capital_columns->addCard(column);
    }
    m_finance_pending_maintenance = financeLabel({}, finance_pool_group, "mutedText");
    m_finance_pending_maintenance->setObjectName("paymasterFinancePendingMaintenance");
    finance_pool_layout->addWidget(m_finance_pending_maintenance);
    finance_layout->addWidget(finance_pool_group);

    m_panel.overview().buildCapitalActions(pages);
    auto* funds_link = new QPushButton(tr("Manage funds and reserves…"), finance_column);
    funds_link->setObjectName("paymasterFinanceFundsLink");
    finance_pool_layout->addWidget(funds_link, 0, Qt::AlignLeft);
    connect(funds_link, &QPushButton::clicked, widget(), [this] { m_panel.showOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::LIQUIDITY)); });

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
    m_panel.bindPage(PaymasterWidgetImpl::Page::FINANCE, finance_scroll);
    m_panel.mainPages()->addWidget(finance_scroll);

}

void ProviderFinance::connectFinance(PageBindings& pages)
{
    m_panel.overview().connectFinanceAction();
    connect(m_finance_refresh, &QPushButton::clicked, widget(),
            [this] { refreshFinanceStatus(); });
    connect(m_finance_period_select,
            qOverload<int>(&QComboBox::currentIndexChanged), widget(),
            [this] {
                if (m_panel.isOperatorPage(m_panel.page(PaymasterWidgetImpl::Page::FINANCE)) && !m_panel.busy()) {
                    refreshFinanceStatus();
                }
            });
    connect(m_finance_export, &QPushButton::clicked, widget(),
            [this] { exportFinanceCsv(); });
    connect(m_finance_previous_page, &QPushButton::clicked, widget(),
            [this] { showPreviousFinancePage(); });
    connect(m_finance_next_page, &QPushButton::clicked, widget(),
            [this] { showNextFinancePage(); });
}
