// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_PAYMASTER_SETUP_H
#define DIGIBYTE_PAYMASTER_SETUP_H

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <univalue.h>
#include <vector>

namespace DigiDollar::Paymaster {
struct SetupSafetyLimits {
    int64_t per_transaction, reserved, per_hour, per_day;
    int completed_per_hour, completed_per_day;
};
SetupSafetyLimits SetupSafetyProfile(bool conservative, int64_t advertised_fee);
UniValue SetupDefaultPolicy();
UniValue SetupDefaultSafety(int64_t advertised_fee, bool user_paid, bool sponsored, bool restricted);
UniValue SetupDefaultLiquidity(bool user_paid);
UniValue SetupNodePrerequisites(const UniValue& snapshot);
int64_t SetupFundingFee(const UniValue& snapshot);
/** All JSON amounts are integer satoshis / DD cents. Core validates policy. */
bool SetupMatches(const UniValue& expected, const UniValue& actual);
UniValue SetupSafetyBridge(const UniValue& previous, const UniValue& target, int64_t previous_fee, int64_t target_fee);
UniValue OperatorDiagnostics(const UniValue& provider, int64_t unlocked_until, int64_t now);
struct SetupProgress {
    bool complete{false};
    bool needs_unlock{false};
    bool blocked{false};
    std::string message;
};
/** Read-only setup completion; balances never substitute for Core readiness. */
SetupProgress InspectSetupProgress(const UniValue& snapshot, bool require_running);
std::string OperatorSummary(const UniValue& snapshot);
std::string OperatorAmount(int64_t satoshis);
int64_t OperatorRemaining(int64_t limit, int64_t spent, int64_t reserved);
UniValue OperatorBudgets(const UniValue& snapshot);

struct SetupChoices {
    std::string display_name;
    UniValue policy{UniValue::VOBJ}, safety{UniValue::VOBJ}, liquidity{UniValue::VOBJ};
    UniValue pool{UniValue::VOBJ};
    std::string operation_mode{"automatic"};
    bool enabled{true};
    // Empty preserves saved autostart (the GUI contract). CLI reviews a choice.
    std::optional<bool> autostart;
};
/** Validate complete proposed settings with the Core policy validators, without writes. */
void CheckSetupChoices(const SetupChoices& choices);
/** Proposed CLI defaults only: callers must obtain explicit approval to apply. */
SetupChoices SetupCliDefaults(const UniValue& snapshot);
struct SetupMenuItem {
    std::string value, title, explanation;
};
struct SetupField {
    std::string key, title, explanation, unit;
    int decimals;
    int64_t minimum, maximum, increment{1};
};
const std::vector<SetupField>& SetupFields();
std::string SetupFormatNumber(int64_t value, int decimals);
std::string SetupPrompt(std::istream& input, std::ostream& output, const std::string& label, const std::string& current = {});
std::string SetupSelect(std::istream& input, std::ostream& output, const std::string& label, const std::vector<SetupMenuItem>& items, const std::string& current);
bool SetupConfirm(std::istream& input, std::ostream& output, const std::string& label);
int64_t SetupReadNumber(std::istream& input, std::ostream& output, const SetupField& field, int64_t current);

struct SetupStep {
    std::string method;
    UniValue params{UniValue::VARR};
    UniValue expected{UniValue::VOBJ};
    std::string result_field;
    bool unlock{false};
};
/** Ordered, restartable plan; funding requires its own fresh Core preview. */
std::vector<SetupStep> BuildSetupPlan(const UniValue& snapshot, const SetupChoices& choices);
void CheckSetupContext(const UniValue& expected, const UniValue& current);
void CheckSetupReply(const SetupStep& step, const UniValue& result);
void CheckSetupSaved(const SetupStep& step, const UniValue& snapshot);
bool SetupPoolNeeded(const UniValue& preview);
} // namespace DigiDollar::Paymaster
#endif
