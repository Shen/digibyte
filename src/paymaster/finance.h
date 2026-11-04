// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_PAYMASTER_FINANCE_H
#define DIGIBYTE_PAYMASTER_FINANCE_H

#include <paymaster/provider.h>

#include <array>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {


/** Derived accounting only, never payment or spending authority. Slots are
 * user-paid, public-sponsored and restricted-sponsored transfers. */
struct ProviderFinanceTotals {
    ProviderFinanceDailyTotals total;
    std::array<int64_t, 3> income{};
    std::array<int64_t, 3> cost{};
    bool operator==(const ProviderFinanceTotals& other) const
    {
        return total == other.total && income == other.income && cost == other.cost;
    }
    SERIALIZE_METHODS(ProviderFinanceTotals, obj)
    {
        READWRITE(obj.total);
        for (auto& amount : obj.income) READWRITE(amount);
        for (auto& amount : obj.cost) READWRITE(amount);
    }
};

/** Version two replaces the serialized event vector with individual records.
 * The old pmfinance key holds this header so old readers reject the version. */
struct ProviderFinanceMetadata {
    static constexpr uint16_t CURRENT_VERSION{2};
    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    int64_t history_complete_from{0};
    bool earlier_history_partial{false};
    int64_t updated_at{0};
    uint64_t revision{0};
    uint64_t event_count{0};
    ProviderFinanceTotals totals;
    SERIALIZE_METHODS(ProviderFinanceMetadata, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id,
                  obj.history_complete_from, obj.earlier_history_partial,
                  obj.updated_at, obj.revision, obj.event_count, obj.totals);
    }
};

enum class ProviderFinancePeriod : uint8_t { TODAY, DAYS7, DAYS30, ALL };

struct ProviderFinanceQuery {
    ProviderFinancePeriod period{ProviderFinancePeriod::DAYS30};
    int64_t now{0};
    bool include_events{false};
    size_t limit{100};
    std::string cursor;
};

struct ProviderFinanceReport {
    ProviderFinanceMetadata metadata;
    std::array<ProviderFinanceTotals, 4> periods;
    std::vector<ProviderFinanceTotals> days;
    std::vector<ProviderFinanceEvent> events;
    std::string next_cursor;
};

/** Checked contribution to a day/all-time aggregate; sign is exactly +1 or -1. */
bool ApplyProviderFinanceContribution(ProviderFinanceTotals& totals,
                                      const ProviderFinanceEvent& event, int sign,
                                      std::string& error);
bool AddProviderFinanceTotals(ProviderFinanceTotals& totals,
                              const ProviderFinanceTotals& additional, std::string& error);
} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_FINANCE_H
