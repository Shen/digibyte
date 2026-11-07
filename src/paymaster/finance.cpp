// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <paymaster/finance.h>
#include <limits>
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


} // namespace DigiDollar::Paymaster
