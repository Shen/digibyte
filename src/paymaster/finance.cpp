// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <paymaster/finance.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
namespace DigiDollar::Paymaster {
namespace {
bool AdjustFinanceTotals(ProviderFinanceTotals& totals, const ProviderFinanceTotals& delta,
                         int sign, std::string& error)
{
    auto next = totals;
    const auto adjust = [&](auto& value, const auto amount) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_signed_v<T>) {
            if (value < 0 || amount < 0) return false;
        }
        if (sign == 1) {
            if (amount > std::numeric_limits<T>::max() - value) return false;
            value += amount;
        } else {
            if (value < amount) return false;
            value -= amount;
        }
        return true;
    };
    if ((sign != 1 && sign != -1) ||
        !adjust(next.total.dd_income.value, delta.total.dd_income.value) ||
        !adjust(next.total.dgb_cost.value, delta.total.dgb_cost.value) ||
        !adjust(next.total.successful_transfers, delta.total.successful_transfers) ||
        !adjust(next.total.user_paid_transfers, delta.total.user_paid_transfers) ||
        !adjust(next.total.public_sponsored_transfers, delta.total.public_sponsored_transfers) ||
        !adjust(next.total.restricted_sponsored_transfers, delta.total.restricted_sponsored_transfers) ||
        !adjust(next.total.maintenance_transactions, delta.total.maintenance_transactions)) {
        error = "PAYMASTER_FINANCE_TOTAL_OVERFLOW";
        return false;
    }
    for (size_t i = 0; i < next.income.size(); ++i) {
        if (!adjust(next.income[i], delta.income[i]) || !adjust(next.cost[i], delta.cost[i])) {
            error = "PAYMASTER_FINANCE_TOTAL_OVERFLOW";
            return false;
        }
    }
    totals = next;
    return true;
}
} // namespace

bool ApplyProviderFinanceContribution(ProviderFinanceTotals& totals,
                                      const ProviderFinanceEvent& event, int sign,
                                      std::string& error)
{
    if (sign != 1 && sign != -1) { error = "PAYMASTER_FINANCE_INVALID_SIGN"; return false; }
    if (!ValidateProviderFinanceEvent(event, error)) return false;
    if (event.state != ProviderFinanceEventState::CONFIRMED) return true;
    ProviderFinanceTotals delta;
    delta.total.dd_income = event.dd_income;
    delta.total.dgb_cost = event.dgb_cost;
    if (event.kind == ProviderFinanceEventKind::TRANSFER) {
        delta.total.successful_transfers = 1;
        const size_t model = event.funding_model == FundingModel::USER_PAID ? 0 :
            event.sponsorship_scope == SponsorshipScope::PUBLIC ? 1 : 2;
        if (model == 0) delta.total.user_paid_transfers = 1;
        if (model == 1) delta.total.public_sponsored_transfers = 1;
        if (model == 2) delta.total.restricted_sponsored_transfers = 1;
        delta.income[model] = event.dd_income.value;
        delta.cost[model] = event.dgb_cost.value;
    } else if (event.kind != ProviderFinanceEventKind::RECOVERY) {
        delta.total.maintenance_transactions = 1;
    }
    return AdjustFinanceTotals(totals, delta, sign, error);
}

bool AddProviderFinanceTotals(ProviderFinanceTotals& totals,
                              const ProviderFinanceTotals& additional, std::string& error)
{
    return AdjustFinanceTotals(totals, additional, 1, error);
}


bool ValidateProviderFinanceEvent(const ProviderFinanceEvent& event,
                                  std::string& error)
{
    error.clear();
    const bool transfer = event.kind == ProviderFinanceEventKind::TRANSFER;
    const bool recovery = event.kind == ProviderFinanceEventKind::RECOVERY;
    if (event.version != ProviderFinanceEvent::CURRENT_VERSION ||
        event.event_id.IsNull() || event.genesis_hash.IsNull() ||
        event.provider_id.IsNull() || event.transaction_id.IsNull() ||
        event.dd_income.value < 0 || event.dgb_cost.value < 0 ||
        event.dd_income.value > MAX_DD_OUTPUT_CENTS ||
        !MoneyRange(event.dgb_cost.value) || event.created_at <= 0 ||
        event.updated_at < event.created_at ||
        (event.state == ProviderFinanceEventState::CONFIRMED &&
         (event.confirmed_at < event.created_at ||
          event.confirmed_at > event.updated_at)) ||
        (event.state != ProviderFinanceEventState::CONFIRMED &&
         event.confirmed_at != 0) ||
        (!transfer && !recovery && event.dd_income.value != 0) ||
        (recovery && (event.funding_model != FundingModel::USER_PAID ||
                      event.sponsorship_scope != SponsorshipScope::PUBLIC)) ||
        (transfer && event.funding_model == FundingModel::SPONSORED &&
         event.dd_income.value != 0)) {
        error = "PAYMASTER_INVALID_FINANCE_EVENT";
        return false;
    }
    return true;
}

bool RebuildProviderFinanceDailyTotals(ProviderFinanceLedger& ledger,
                                       std::string& error)
{
    error.clear();
    std::map<int64_t, ProviderFinanceTotals> by_day;
    std::set<uint256> event_ids;
    for (const ProviderFinanceEvent& event : ledger.events) {
        if (!ValidateProviderFinanceEvent(event, error) ||
            event.genesis_hash != ledger.genesis_hash ||
            event.provider_id != ledger.provider_id) {
            if (error.empty()) error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
            return false;
        }
        if (!event_ids.insert(event.event_id).second) {
            error = "PAYMASTER_DUPLICATE_FINANCE_EVENT";
            return false;
        }
        if (event.state != ProviderFinanceEventState::CONFIRMED) continue;
        const int64_t day_start = (event.confirmed_at / PROVIDER_BUDGET_WINDOW_SECONDS) *
                                  PROVIDER_BUDGET_WINDOW_SECONDS;
        auto& totals = by_day[day_start];
        totals.total.day_start = day_start;
        if (!ApplyProviderFinanceContribution(totals, event, 1, error)) return false;
    }
    ledger.daily_totals.clear();
    ledger.daily_totals.reserve(by_day.size());
    for (auto& [day, totals] : by_day) {
        (void)day;
        ledger.daily_totals.push_back(std::move(totals.total));
    }
    return true;
}

bool ValidateProviderFinanceLedger(const ProviderFinanceLedger& ledger,
                                   std::string& error)
{
    error.clear();
    if (ledger.version != ProviderFinanceLedger::CURRENT_VERSION ||
        ledger.genesis_hash.IsNull() || ledger.provider_id.IsNull() ||
        ledger.history_complete_from <= 0 ||
        ledger.updated_at < ledger.history_complete_from ||
        ledger.events.size() > MAX_PROVIDER_FINANCE_EVENTS) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (std::any_of(ledger.events.begin(), ledger.events.end(),
                    [&](const ProviderFinanceEvent& event) {
                        return event.updated_at > ledger.updated_at;
                    })) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER_TIME";
        return false;
    }
    ProviderFinanceLedger rebuilt{ledger};
    if (!RebuildProviderFinanceDailyTotals(rebuilt, error) ||
        rebuilt.daily_totals.size() != ledger.daily_totals.size()) {
        if (error.empty()) error = "PAYMASTER_INVALID_FINANCE_TOTALS";
        return false;
    }
    for (size_t index = 0; index < ledger.daily_totals.size(); ++index) {
        const ProviderFinanceDailyTotals& stored = ledger.daily_totals[index];
        const ProviderFinanceDailyTotals& expected = rebuilt.daily_totals[index];
        if (!(stored == expected)) {
            error = "PAYMASTER_INVALID_FINANCE_TOTALS";
            return false;
        }
    }
    return true;
}

bool CheckProviderFinanceEventBinding(const ProviderFinanceEvent& existing,
                                     const ProviderFinanceEvent& incoming, std::string& error)
{
    if (existing.event_id != incoming.event_id || existing.genesis_hash != incoming.genesis_hash ||
        existing.provider_id != incoming.provider_id || existing.kind != incoming.kind ||
        existing.funding_model != incoming.funding_model || existing.sponsorship_scope != incoming.sponsorship_scope ||
        existing.transaction_id != incoming.transaction_id || !(existing.dd_income == incoming.dd_income) ||
        !(existing.dgb_cost == incoming.dgb_cost) || existing.created_at != incoming.created_at) {
        error = "PAYMASTER_FINANCE_EVENT_CONFLICT";
        return false;
    }
    return true;
}

bool UpsertProviderFinanceEvent(ProviderFinanceLedger& ledger,
                                const ProviderFinanceEvent& event,
                                std::string& error)
{
    if (!ValidateProviderFinanceEvent(event, error) ||
        event.genesis_hash != ledger.genesis_hash ||
        event.provider_id != ledger.provider_id) {
        if (error.empty()) error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }
    auto existing = std::find_if(
        ledger.events.begin(), ledger.events.end(),
        [&](const ProviderFinanceEvent& candidate) {
            return candidate.event_id == event.event_id;
        });
    if (existing == ledger.events.end()) {
        if (ledger.events.size() >= MAX_PROVIDER_FINANCE_EVENTS) {
            error = "PAYMASTER_FINANCE_LEDGER_FULL";
            return false;
        }
        ledger.events.push_back(event);
    } else {
        if (!CheckProviderFinanceEventBinding(*existing, event, error)) return false;
        // A delayed replay can legitimately carry an older pending or
        // confirmed snapshot. Treat it as an idempotent no-op instead of
        // letting it roll back the more recent chain-derived state.
        if (event.updated_at < existing->updated_at) return true;
        if (event.updated_at == existing->updated_at &&
            (event.state != existing->state ||
             event.confirmed_at != existing->confirmed_at)) {
            error = "PAYMASTER_FINANCE_EVENT_STATE_CONFLICT";
            return false;
        }
        *existing = event;
    }
    ledger.updated_at = std::max(ledger.updated_at, event.updated_at);
    return RebuildProviderFinanceDailyTotals(ledger, error);
}

} // namespace DigiDollar::Paymaster
