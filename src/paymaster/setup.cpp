// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <algorithm>
#include <consensus/amount.h>
#include <istream>
#include <limits>
#include <ostream>
#include <paymaster/types.h>
#include <paymaster/provider.h>
#include <util/strencodings.h>
#include <paymaster/setup.h>
#include <set>
#include <stdexcept>

namespace DigiDollar::Paymaster {
SetupSafetyLimits SetupSafetyProfile(bool conservative, int64_t fee)
{
    return {fee, std::max<int64_t>(conservative ? 20000000 : 100000000, fee),
            std::max<int64_t>(conservative ? 50000000 : 200000000, fee),
            std::max<int64_t>(conservative ? 200000000 : 1000000000, fee),
            conservative ? 5 : 10, conservative ? 25 : 100};
}
UniValue SetupDefaultPolicy()
{
    UniValue result{UniValue::VOBJ};
    UniValue models{UniValue::VARR};
    models.push_back("user_paid");
    result.pushKV("funding_models", models);
    result.pushKV("sponsorship_scope", "public");
    result.pushKV("fee_rate_bps", 50);
    result.pushKV("maximum_user_paid_service_fee_cents", 0);
    result.pushKV("min_amount_cents", 100);
    result.pushKV("max_amount_cents", 100000);
    result.pushKV("quote_ttl", 60);
    result.pushKV("maximum_network_fee_dgb_satoshis", 20000000);
    return result;
}
UniValue SetupDefaultSafety(int64_t fee, bool user_paid, bool sponsored, bool restricted)
{
    UniValue result{UniValue::VOBJ};
    const auto limits = SetupSafetyProfile(false, fee);
    for (const auto& mode : {std::string{"user_paid"}, std::string{"public_sponsored"}, std::string{"restricted_sponsored"}}) {
        const bool active = mode == "user_paid" ? user_paid : sponsored && (restricted == (mode == "restricted_sponsored"));
        UniValue values{UniValue::VOBJ};
        values.pushKV("maximum_network_fee_per_transaction_satoshis", active ? limits.per_transaction : 0);
        values.pushKV("maximum_reserved_network_fee_satoshis", active ? limits.reserved : 0);
        values.pushKV("maximum_network_fee_per_hour_satoshis", active ? limits.per_hour : 0);
        values.pushKV("maximum_network_fee_per_day_satoshis", active ? limits.per_day : 0);
        values.pushKV("maximum_completed_per_hour", active ? limits.completed_per_hour : 0);
        values.pushKV("maximum_completed_per_day", active ? limits.completed_per_day : 0);
        result.pushKV(mode, values);
    }
    result.pushKV("maximum_active_quotes_total", 16);
    result.pushKV("maximum_active_quotes_per_netgroup", 4);
    result.pushKV("maximum_active_quotes_per_recipient", 2);
    result.pushKV("maximum_quote_requests_per_netgroup_per_minute", 10);
    return result;
}
UniValue SetupLiquidityPreset(bool user_paid, int capacity)
{
    if (capacity < 1 || capacity > 16) throw std::invalid_argument("Invalid Paymaster capacity");
    // Reserve transactions can have several inputs/outputs and a DD fee floor
    // of 0.10 DGB. Leave room for both DGB and carrier creation; these bounded
    // proposals are not fee estimates. Fragmentation can still require review.
    const int64_t per_transaction = capacity == 1 ? 50000000 :
                                    capacity <= 3 ? 75000000 :
                                    capacity <= 6 ? 100000000 :
                                                    200000000;
    UniValue result{UniValue::VOBJ};
    result.pushKV("automatic_replenishment", false);
    result.pushKV("paid_maintenance_approved", false);
    result.pushKV("target_admission_dgb", 3);
    result.pushKV("target_operational_dgb", capacity);
    result.pushKV("target_admission_carriers", user_paid ? 3 : 0);
    result.pushKV("target_operational_carriers", user_paid ? capacity : 0);
    result.pushKV("maximum_maintenance_fee_per_transaction_satoshis", per_transaction);
    result.pushKV("maximum_maintenance_fee_per_hour_satoshis", 4 * per_transaction);
    result.pushKV("maximum_maintenance_fee_per_day_satoshis", 20 * per_transaction);
    return result;
}
UniValue SetupDefaultLiquidity(bool user_paid)
{
    return SetupLiquidityPreset(user_paid, 1);
}
SetupChoices SetupCliDefaults(const UniValue& snapshot)
{
    CheckSetupContext(snapshot, snapshot);
    const auto& provider = snapshot.find_value("provider");
    const bool existing = provider.find_value("settings_present").isTrue();
    SetupChoices result;
    result.enabled = existing ? provider.find_value("enabled").get_bool() : true;
    result.operation_mode = existing ? provider.find_value("operation_mode").get_str() : "automatic";
    result.autostart = existing ? provider.find_value("autostart").get_bool() : true;
    if (provider.find_value("display_name").isStr()) result.display_name = provider.find_value("display_name").get_str();
    const auto retain = [](UniValue& proposed, const UniValue& saved) {
        if (!saved.isObject()) return;
        for (const auto& key : proposed.getKeys()) {
            if (saved.find_value(key).isNull()) throw std::runtime_error("PAYMASTER_SETUP_STATUS_INCOMPLETE");
            proposed.pushKV(key, saved.find_value(key));
        }
    };
    result.policy = SetupDefaultPolicy();
    retain(result.policy, provider.find_value("policy"));
    bool paid{false}, sponsored{false};
    for (const auto& model : result.policy.find_value("funding_models").getValues()) {
        paid |= model.get_str() == "user_paid";
        sponsored |= model.get_str() == "sponsored";
    }
    result.safety = SetupDefaultSafety(result.policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>(), paid, sponsored, result.policy.find_value("sponsorship_scope").get_str() == "restricted");
    retain(result.safety, snapshot.find_value("safety").find_value("policy"));
    result.liquidity = SetupDefaultLiquidity(paid);
    const auto& liquidity = provider.find_value("liquidity");
    if (liquidity.find_value("policy_configured").isTrue()) {
        retain(result.liquidity, liquidity.find_value("policy"));
    } else if (!existing) {
        // Recommendations are only proposals until the complete review is
        // approved. Do not silently enable refill on an existing provider.
        result.liquidity.pushKV("automatic_replenishment", true);
        result.liquidity.pushKV("paid_maintenance_approved", true);
    }
    const auto& preparation = provider.find_value("preparation");
    result.pool.pushKV("maximum_fee_satoshis", existing || (preparation.isArray() && !preparation.empty()) ? SetupFundingFee(snapshot) : 50000000);
    for (const auto& names : {std::pair{"admission_dgb_slots", "target_admission_dgb"}, {"operational_dgb_slots", "target_operational_dgb"}, {"admission_carrier_slots", "target_admission_carriers"}, {"operational_carrier_slots", "target_operational_carriers"}})
        result.pool.pushKV(names.first, result.liquidity.find_value(names.second));
    return result;
}

const std::vector<SetupField>& SetupFields()
{
    static const std::vector<SetupField> fields{
        {"fee_rate_bps", "Customer service fee", "Your DD income as a share of the payment. 0.50 means 0.50%, rounded up to whole DD cents per payment. Choose in steps of 0.10%. It does not guarantee that DD income covers DGB costs.", "%", 2, 0, MAX_RATE_BPS, 10},
        {"maximum_user_paid_service_fee_cents", "Maximum user-paid service fee", "Maximum DD service fee charged on one user-paid transfer. Enter 0 for no fee cap.", "DD", 2, 0, MAX_DD_OUTPUT_CENTS},
        {"min_amount_cents", "Smallest accepted payment", "Payments below this DD amount are declined. The protocol minimum is 1.00 DD.", "DD", 2, 100, MAX_DD_OUTPUT_CENTS},
        {"max_amount_cents", "Largest accepted payment", "Payments above this DD amount are declined. Must be at least the smallest payment.", "DD", 2, 100, MAX_DD_OUTPUT_CENTS},
        {"quote_ttl", "Offer lifetime", "Time for a customer to review and accept an offer. 60 seconds gives the maximum supported review time.", "seconds", 0, 1, 60},
        {"maximum_network_fee_dgb_satoshis", "Advertised network-fee ceiling", "Upper DGB fee advertised for one customer payment. This is a ceiling, not a fixed charge. Actual spending must also fit the limits below.", "DGB", 8, 1, MAX_MONEY},
        {"maximum_network_fee_per_transaction_satoshis", "Maximum fee per payment", "Cap on DGB spent for one customer payment. Must not exceed the advertised ceiling. Zero disables this payment model.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_reserved_network_fee_satoshis", "Maximum reserved payment budget", "Budget held for accepted offers at the same time; it is not a confirmed expense. Must cover at least one payment fee ceiling.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_network_fee_per_hour_satoshis", "Payment fees per rolling hour", "Total DGB fee allowance in any rolling 60 minutes. At the limit, new payments wait or are declined; the limit never increases itself.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_network_fee_per_day_satoshis", "Payment fees per rolling day", "Total DGB fee allowance in any rolling 24 hours, not a midnight reset. Must be at least the hourly limit.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_completed_per_hour", "Successful payments per rolling hour", "Additional count limit, independent of fee spending. Must be positive for an enabled payment model.", "payments", 0, 0, UINT32_MAX},
        {"maximum_completed_per_day", "Successful payments per rolling day", "Additional rolling 24-hour count limit. Must be at least the hourly count.", "payments", 0, 0, UINT32_MAX},
        {"maximum_active_quotes_total", "Simultaneous customer offers", "Limits accepted offers that can hold payment capacity. Keep the default unless you need more concurrent customers.", "offers", 0, 1, 8192},
        {"maximum_active_quotes_per_netgroup", "Offers per customer network group", "Limits one network group's share of offers. Must not exceed the total offer limit.", "offers", 0, 1, 8192},
        {"maximum_active_quotes_per_recipient", "Offers per recipient", "Limits simultaneous offers paying the same recipient. Must not exceed the total offer limit.", "offers", 0, 1, 8192},
        {"maximum_quote_requests_per_netgroup_per_minute", "Offer requests per network group per minute", "Rate limit against repeated requests. Raising it can increase provider workload.", "requests", 0, 1, 8192},
        {"target_admission_dgb", "DGB capacity-check reserves", "Separate outputs used to prove capacity before accepting payments. Keep 3 for a small provider; these funds remain wallet-owned.", "reserves", 0, 3, 16},
        {"target_operational_dgb", "DGB payment reserves", "Prepared DGB outputs for actual payments. Start with 1; more can support additional concurrent payments but tie up more capital.", "reserves", 0, 1, 16},
        {"target_admission_carriers", "DD capacity-check reserves", "Needed for customer-paid service fees. Use at least 3 with customer-paid offers; sponsored-only providers can use 0.", "reserves", 0, 0, 16},
        {"target_operational_carriers", "DD payment reserves", "DD reserves to receive service fees. With customer-paid offers, match the DGB payment count for complete payment capacity. Sponsored-only providers can use 0.", "reserves", 0, 0, 16},
        {"maximum_maintenance_fee_per_transaction_satoshis", "Maximum fee per refill transaction", "DGB ceiling for rebuilding reserves when confirmed change cannot be reused. This is separate from customer-payment fees.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_maintenance_fee_per_hour_satoshis", "Refill fees per rolling hour", "Maximum DGB for automatic reserve maintenance over 60 minutes. Must cover the per-transaction ceiling when paid refill is enabled.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_maintenance_fee_per_day_satoshis", "Refill fees per rolling day", "Maximum DGB for automatic maintenance over 24 hours. This allowance is additional to the customer-payment budget.", "DGB", 8, 0, MAX_MONEY},
        {"maximum_fee_satoshis", "Maximum fee per setup transaction", "One-time ceiling for creating initial or missing reserves. 0.50 DGB is the new-provider proposal, not a fee estimate. Core stops if it is insufficient; an increase needs your review.", "DGB", 8, 1, MAX_MONEY / 2},
        {"unlock_seconds", "Wallet unlock duration", "Timed access stops new signing when it expires. Continuous access is recommended for uninterrupted operation until manual lock or node restart.", "seconds", 0, 60, 86400},
    };
    return fields;
}

std::string SetupFormatNumber(int64_t value, int decimals)
{
    if (value < 0 || decimals < 0 || decimals > 8) throw std::runtime_error("Invalid setup amount");
    auto result = std::to_string(value);
    if (decimals) {
        if (result.size() <= size_t(decimals)) result.insert(0, size_t(decimals) + 1 - result.size(), '0');
        result.insert(result.size() - decimals, 1, '.');
    }
    return result;
}
std::string SetupPrompt(std::istream& input, std::ostream& output, const std::string& label, const std::string& current)
{
    output << label << (current.empty() ? "" : " [" + current + "]") << ": " << std::flush;
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("Setup closed. Earlier approved steps remain saved; no pending question was approved.");
    line = TrimString(line);
    return line.empty() ? current : line;
}
std::string SetupSelect(std::istream& input, std::ostream& output, const std::string& label, const std::vector<SetupMenuItem>& items, const std::string& current)
{
    if (items.empty() || std::none_of(items.begin(), items.end(), [&](const auto& item) { return item.value == current; })) throw std::runtime_error("Invalid setup menu default");
    for (;;) {
        output << '\n' << label << '\n';
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            output << "  " << i + 1 << ") " << item.title << (item.value == current ? " [selected]" : "") << "\n     " << item.explanation << '\n';
        }
        const auto answer = ToLower(SetupPrompt(input, output, "Choose a number or name; Enter keeps the selection (? repeats help)", current));
        for (size_t i = 0; i < items.size(); ++i)
            if (answer == items[i].value || answer == std::to_string(i + 1)) return items[i].value;
        if (answer != "?") output << "Please choose one of the listed options. Your earlier choices are retained.\n";
    }
}
bool SetupConfirm(std::istream& input, std::ostream& output, const std::string& label)
{
    for (;;) {
        const auto answer = ToLower(SetupPrompt(input, output, label + " (type yes to approve; Enter declines)", "no"));
        if (answer == "yes") return true;
        if (answer == "no") return false;
        output << "Type yes to approve or no to decline. Nothing has been approved by this answer.\n";
    }
}
int64_t SetupReadNumber(std::istream& input, std::ostream& output, const SetupField& field, int64_t current)
{
    for (;;) {
        output << field.explanation << '\n';
        auto answer = SetupPrompt(input, output, field.title + " (" + field.unit + ")", SetupFormatNumber(current, field.decimals));
        if (answer == "?") continue;
        // Accept either decimal separator, but never guess thousands grouping.
        if (answer.find('.') == std::string::npos && std::count(answer.begin(), answer.end(), ',') == 1) std::replace(answer.begin(), answer.end(), ',', '.');
        // Parse the full int64 range exactly. ParseFixedPoint is limited to
        // 10^18-1 subunits, below DigiByte's MAX_MONEY of 2.1 * 10^18.
        const auto point = answer.find('.');
        const auto whole = answer.substr(0, point);
        auto fraction = point == std::string::npos ? std::string{} : answer.substr(point + 1);
        while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
        const bool plain = !whole.empty() && whole.find_first_not_of("0123456789") == std::string::npos &&
            fraction.find_first_not_of("0123456789") == std::string::npos && fraction.size() <= size_t(field.decimals);
        uint64_t parsed{0};
        if (plain) {
            fraction.append(size_t(field.decimals) - fraction.size(), '0');
            if (ParseUInt64(whole + fraction, &parsed) && parsed <= uint64_t(field.maximum) && parsed >= uint64_t(field.minimum) && parsed % field.increment == 0)
                return int64_t(parsed);
        }
        output << "Enter " << SetupFormatNumber(field.minimum, field.decimals) << " to " << SetupFormatNumber(field.maximum, field.decimals)
               << ' ' << field.unit << " in steps of " << SetupFormatNumber(field.increment, field.decimals) << ". Use no thousands separators. Please try again.\n";
    }
}

UniValue SetupNodePrerequisites(const UniValue& snapshot)
{
    const auto& current = snapshot.find_value("node_settings");
    if (!current.isObject()) throw std::runtime_error("PAYMASTER_NODE_SETTINGS_UNKNOWN");
    UniValue changes{UniValue::VOBJ};
    for (const auto& key : {"digidollar", "paymaster", "txindex", "v2transport"})
        if (current.find_value(key).getInt<int>() != 1) changes.pushKV(key, 1);
    if (current.find_value("prune").getInt<int64_t>() != 0) changes.pushKV("prune", 0);
    const int outgoing = current.find_value("paymastermaxoutbound").getInt<int>();
    const int incoming = current.find_value("paymastermaxinbound").getInt<int>();
    if (outgoing < 1 || outgoing > 4) changes.pushKV("paymastermaxoutbound", 1);
    if (incoming < 1 || incoming > 16) changes.pushKV("paymastermaxinbound", 16);
    const int requested_out = outgoing >= 1 && outgoing <= 4 ? outgoing : 1;
    const int requested_in = incoming >= 1 && incoming <= 16 ? incoming : 16;
    const int64_t required = current.find_value("ordinary_outbound_target").getInt<int64_t>() + requested_out + requested_in + std::min(8, requested_in) + 1;
    if (current.find_value("maxconnections").getInt<int64_t>() < required) changes.pushKV("maxconnections", std::max<int64_t>(125, required));
    return changes;
}
int64_t SetupFundingFee(const UniValue& snapshot)
{
    const auto& records = snapshot.find_value("provider").find_value("preparation");
    int64_t fee{0};
    if (records.isArray())
        for (const auto& record : records.getValues()) {
            const auto value = record.find_value("maximum_fee_satoshis").getInt<int64_t>();
            if (value <= 0 || (fee && value != fee)) throw std::runtime_error("PAYMASTER_SETUP_CONFLICTING_APPROVALS");
            fee = value;
        }
    return fee ? fee : 20000000;
}
bool SetupMatches(const UniValue& expected, const UniValue& actual)
{
    if (expected.getType() != actual.getType()) return false;
    if (expected.isObject()) {
        for (const auto& key : expected.getKeys())
            if (!SetupMatches(expected.find_value(key), actual.find_value(key))) return false;
        return true;
    }
    return expected.write() == actual.write();
}
UniValue SetupSafetyBridge(const UniValue& previous, const UniValue& target, int64_t old_fee, int64_t new_fee)
{
    UniValue bridge{UniValue::VOBJ};
    for (const auto& key : target.getKeys())
        if (!target.find_value(key).isObject()) bridge.pushKV(key, previous.find_value(key));
    for (const auto& name : {"user_paid", "public_sponsored", "restricted_sponsored"}) {
        UniValue limits = target.find_value(name);
        if (limits.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>() == 0) limits = previous.find_value(name);
        auto cap = limits.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>();
        limits.pushKV("maximum_network_fee_per_transaction_satoshis", std::min({cap, old_fee, new_fee}));
        bridge.pushKV(name, limits);
    }
    return bridge;
}
std::string OperatorWorkPhase(const UniValue& provider)
{
    const auto& activity = provider.find_value("pool").find_value("activity");
    if (activity.isNull()) return "idle"; // Additive field; retain older snapshots.
    if (!activity.isObject()) throw std::runtime_error("PAYMASTER_ACTIVITY_STATUS_INCOMPLETE");
    const auto count = [&](const char* key) {
        const auto& value = activity.find_value(key);
        if (!value.isNum() || value.getInt<int64_t>() < 0)
            throw std::runtime_error("PAYMASTER_ACTIVITY_STATUS_INCOMPLETE");
        return value.getInt<int64_t>();
    };
    const auto payments = count("active_payments");
    const auto capacity = count("capacity_requests");
    const auto reserved = count("reserved_outputs");
    const auto confirmations = count("pending_confirmations");
    if (payments > 0) return "payment";
    if (capacity > 0 || reserved > 0) return "capacity";
    if (confirmations > 0) return "confirmation";
    return "idle";
}

UniValue OperatorDiagnostics(const UniValue& provider, int64_t unlocked_until, int64_t now)
{
    UniValue result{UniValue::VARR};
    std::set<std::string> seen;
    const auto add = [&](const std::string& code, const std::string& state, const std::string& area, const std::string& action) {
        if (!seen.insert(code).second) return;
        UniValue item{UniValue::VOBJ};
        item.pushKV("code", code);
        item.pushKV("state", state);
        item.pushKV("severity", state == "error" ? "error" : state == "ready" || state == "waiting" ? "info" :
                                                                                                      "warning");
        item.pushKV("area", area);
        item.pushKV("action", action);
        result.push_back(item);
    };
    const UniValue& errors = provider.find_value("readiness_errors");
    const std::string work_phase = OperatorWorkPhase(provider);
    bool temporary_capacity = provider.find_value("running").isTrue() && work_phase != "idle";
    for (const char* asset : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
        const auto& missing = provider.find_value("liquidity").find_value(asset).find_value("missing");
        temporary_capacity &= missing.isNum() && missing.getInt<int64_t>() == 0;
    }
    if (!errors.isArray()) {
        add("PAYMASTER_STATUS_INCOMPLETE", "unknown", "service", "refresh");
        return result;
    }
    // A finite setup can wait while a provider is already running. Its durable
    // step diagnostic is more useful than the generic missing-slot symptom.
    bool preparation_pending{false};
    const auto& preparation = provider.find_value("preparation");
    if (preparation.isArray()) {
        for (const auto& step : preparation.getValues()) {
            const auto& state = step.find_value("state");
            const auto& error = step.find_value("error");
            if (state.isStr() && (state.get_str() == "complete" || state.get_str() == "cancelled")) continue;
            preparation_pending = true;
            if (!state.isStr() || !error.isStr() ||
                (state.get_str() != "pending_creation" && state.get_str() != "pending_confirmation" && state.get_str() != "conflict")) {
                add("PAYMASTER_POOL_PREPARATION_STATUS_UNKNOWN", "error", "liquidity", "inspect_error");
                continue;
            }
            const std::string code = error.get_str().substr(0, error.get_str().find(':'));
            if (state.get_str() == "conflict")
                add("PAYMASTER_POOL_TRANSACTION_CONFLICT", "error", "liquidity", "inspect_error");
            else if (code == "PAYMASTER_WALLET_LOCKED")
                add(code, "action_required", "wallet", "unlock");
            else if (code == "PAYMASTER_PROVIDER_DISABLED")
                add(code, "action_required", "service", "enable");
            else if (code == "PAYMASTER_POOL_WAITING_CONFIRMATION" || (code.empty() && state.get_str() == "pending_confirmation"))
                add("PAYMASTER_POOL_WAITING_CONFIRMATION", "waiting", "liquidity", "wait");
            else if (code == "PAYMASTER_POOL_WAITING_DGB" || code == "PAYMASTER_POOL_WAITING_FUNDS" ||
                     code == "PAYMASTER_POOL_FEE_LIMIT" || code == "PAYMASTER_POOL_POLICY_CHANGED")
                add(code, "action_required", "liquidity", "review_liquidity");
            else if (code.empty())
                add("PAYMASTER_POOL_PREPARATION_PENDING", "waiting", "liquidity", "wait");
            else
                add(code, "error", "liquidity", "inspect_error");
        }
    }
    for (const auto& error : errors.getValues()) {
        const std::string code = error.get_str();
        if (code == "PAYMASTER_WALLET_LOCKED")
            add(code, "action_required", "wallet", "unlock");
        else if (code == "PAYMASTER_NODE_NOT_READY" || code == "PAYMASTER_REQUIRES_READY_TXINDEX" || code == "PAYMASTER_DIGIDOLLAR_NOT_ACTIVE" ||
                 code == "PAYMASTER_PROVIDER_SYNCING" || code == "PAYMASTER_TXINDEX_NOT_READY")
            add(code, "waiting", "node", "wait");
        else if (code == "PAYMASTER_OPERATIONAL_SLOT_MISSING" && temporary_capacity)
            add(code, "waiting", work_phase == "confirmation" ? "liquidity" : "payments", "wait");
        else if (code == "PAYMASTER_PROVIDER_NOT_ENABLED")
            add(code, "action_required", "service", "enable");
        else if (code.find("LISTENER") != std::string::npos || code.find("ENDPOINT") != std::string::npos || code.find("INBOUND_CAPACITY") != std::string::npos || code.find("REQUIRES_") != std::string::npos || code == "PAYMASTER_DISABLED" || code == "PAYMASTER_MESSAGE_CAPTURE_ENABLED")
            add(code, "action_required", "connection", "configure_node");
        else if (code.find("CONFIRMATION") != std::string::npos)
            add(code, "waiting", "liquidity", "wait");
        else if (code.find("LIMIT_EXHAUSTED") != std::string::npos)
            add(code, "action_required", "budgets", "review_budget");
        else if (code.find("POOL") != std::string::npos || code.find("ADMISSION_") != std::string::npos || code.find("OPERATIONAL_SLOT") != std::string::npos || code.find("LIQUIDITY") != std::string::npos)
            add(code, "action_required", "liquidity", "review_liquidity");
        else if (code.find("NOT_FOUND") != std::string::npos || code.find("POLICY_NOT_") != std::string::npos)
            add(code, "action_required", "configuration", "setup");
        else
            add(code, "error", "service", "inspect_error");
    }
    if (provider.find_value("budget_errors").isArray())
        for (const auto& error : provider.find_value("budget_errors").getValues())
            add(error.get_str(), "action_required", "budgets", "review_budget");
    if (provider.find_value("maintenance_budget_exhausted").isTrue()) add("PAYMASTER_MAINTENANCE_BUDGET_EXHAUSTED", "action_required", "budgets", "review_budget");
    if (unlocked_until > now && unlocked_until - now <= 300) add("PAYMASTER_WALLET_LOCKING_SOON", "action_required", "wallet", "review_unlock");
    if (provider.find_value("backup_status").find_value("required").isTrue()) add("PAYMASTER_BACKUP_REQUIRED", "action_required", "backup", "backup_wallet");
    const auto& service_error = provider.find_value("last_service_error");
    if (service_error.isStr() && !service_error.get_str().empty()) {
        if (service_error.get_str() == "PAYMASTER_POOL_PREPARATION_PENDING") {
            if (!preparation_pending) add(service_error.get_str(), "action_required", "liquidity", "review_liquidity");
        } else if (provider.find_value("service_state").isStr() && provider.find_value("service_state").get_str() != "error" &&
                  (service_error.get_str() == "PAYMASTER_PROVIDER_SYNCING" ||
                   service_error.get_str() == "PAYMASTER_REQUIRES_READY_TXINDEX" ||
                   service_error.get_str() == "PAYMASTER_TXINDEX_NOT_READY" ||
                   service_error.get_str() == "PAYMASTER_NODE_NOT_READY")) {
            add(service_error.get_str(), "waiting", "node", "wait");
        } else if (service_error.get_str() == "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING" &&
                   provider.find_value("running").isTrue() &&
                   provider.find_value("service_state").isStr() &&
                   provider.find_value("service_state").get_str() != "error") {
            // This scheduler reason also covers capacity reserved by an ongoing
            // payment. It does not imply a failure or a broadcast refill.
            if (!provider.find_value("ready").isTrue() || work_phase != "idle")
                add(service_error.get_str(), "waiting", work_phase == "payment" || work_phase == "capacity" ? "payments" : "liquidity", "wait");
        } else if (service_error.get_str() == "PAYMASTER_PROVIDER_DRAIN_ONLY") {
            add(service_error.get_str(), "waiting", "service", "wait");
        } else if (service_error.get_str() == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" ||
                   service_error.get_str() == "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED" ||
                   service_error.get_str() == "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED") {
            add(service_error.get_str(), "action_required", "budgets", "review_budget");
        } else if (service_error.get_str() == "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED" ||
                   service_error.get_str() == "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE" ||
                   service_error.get_str() == "PAYMASTER_MAINTENANCE_FEE_EXCEEDED") {
            add(service_error.get_str(), "action_required", "liquidity", "review_liquidity");
        } else {
            add(service_error.get_str(), "error", "service", "inspect_error");
        }
    }
    if (provider.find_value("service_state").isStr() && provider.find_value("service_state").get_str() == "drain_only" &&
        (!service_error.isStr() || service_error.get_str().empty()))
        add("PAYMASTER_PROVIDER_DRAIN_ONLY", "waiting", "service", "wait");
    if (provider.find_value("service_state").isStr() && provider.find_value("service_state").get_str() == "error")
        add("PAYMASTER_PROVIDER_SERVICE_FAULT", "error", "service", "inspect_error");
    if (provider.find_value("running").isTrue() && work_phase != "idle")
        add(work_phase == "confirmation" ? "PAYMASTER_RESERVE_CONFIRMATION_PENDING" : "PAYMASTER_PAYMENT_IN_PROGRESS",
            "waiting", work_phase == "confirmation" ? "liquidity" : "payments", "wait");
    if (errors.empty() && !preparation_pending) add("PAYMASTER_LOCAL_READY", "ready", "service", provider.find_value("running").isTrue() ? "none" : "start");
    add("PAYMASTER_EXTERNAL_REACHABILITY_UNKNOWN", "unknown", "connection", "check_external");
    auto items = result.getValues();
    const auto priority = [](const UniValue& item) {
        const auto action = item.find_value("action").get_str();
        if (action == "inspect_error" || action == "configure_node") return 0;
        if (action == "setup") return 1;
        if ((action == "wait" && item.find_value("area").get_str() != "payments") || action == "unlock" || action == "review_unlock") return 2;
        if (action == "wait") return 3;
        if (action == "review_liquidity" || action == "review_budget") return 3;
        if (action == "enable" || action == "start") return 4;
        if (action == "check_external") return 9;
        return 5;
    };
    std::stable_sort(items.begin(), items.end(), [&](const UniValue& a, const UniValue& b) { return priority(a) < priority(b); });
    result = UniValue{UniValue::VARR};
    for (const auto& item : items)
        result.push_back(item);
    return result;
}
SetupProgress InspectSetupProgress(const UniValue& snapshot, bool require_running)
{
    SetupProgress result;
    const auto& provider = snapshot.find_value("provider");
    const auto& operations = provider.find_value("active_operations");
    const auto& diagnostics = snapshot.find_value("diagnostics");
    const auto& service = provider.find_value("service_state");
    for (const auto* key : {"enabled", "wallet_locked", "ready", "pool_ready", "running"}) {
        if (!provider.find_value(key).isBool()) throw std::runtime_error("PAYMASTER_SETUP_STATUS_INCOMPLETE");
    }
    if (!operations.isArray() || !diagnostics.isArray() || !service.isStr()) throw std::runtime_error("PAYMASTER_SETUP_STATUS_INCOMPLETE");
    const std::set<std::string> known_states{"stopped", "active", "manual", "waiting_for_unlock", "waiting_for_readiness", "waiting_for_maintenance_approval", "replenishing_liquidity", "waiting_for_liquidity_confirmation", "drain_only", "error"};
    if (!known_states.count(service.get_str())) return {false, false, true, "Unknown provider service state; inspect diagnostics before continuing."};
    result.needs_unlock = provider.find_value("wallet_locked").isTrue();
    if (!provider.find_value("enabled").isTrue()) return {false, false, true, "Provider configuration is disabled. Explicitly enable it before continuing."};
    if (service.get_str() == "error") return {false, false, true, "Provider operation requires diagnostic review."};
    bool waiting_funds{false};
    bool waiting_confirmation{false};
    int64_t confirmed{0}, required{0};
    for (const auto& step : operations.getValues()) {
        const auto state = step.find_value("state").get_str();
        const auto error = step.find_value("error").get_str();
        const auto code = error.substr(0, error.find(':'));
        if (state != "pending_creation" && state != "pending_confirmation") return {false, false, true, "Setup requires review: " + state + " " + error};
        // The journal can still contain the last locked/disabled observation
        // until the next scheduler pass after explicit unlock/enable.
        if (code == "PAYMASTER_POOL_WAITING_FUNDS" || code == "PAYMASTER_POOL_WAITING_DGB") waiting_funds = true;
        else if (code == "PAYMASTER_POOL_FEE_LIMIT") return {false, false, true, "The approved setup fee ceiling is insufficient. Review the funding preview and explicitly approve any changed limit; setup has not completed."};
        else if (code == "PAYMASTER_POOL_POLICY_CHANGED") return {false, false, true, "The provider policy changed. Review the saved setup before authorizing further preparation."};
        else if (!code.empty() && code != "PAYMASTER_POOL_WAITING_CONFIRMATION" &&
                 code != "PAYMASTER_WALLET_LOCKED" && code != "PAYMASTER_PROVIDER_DISABLED") return {false, false, true, "Setup requires review: " + error + ". Spending limits were not changed."};
        if (state == "pending_confirmation") {
            waiting_confirmation = true;
            const auto have = step.find_value("confirmations").getInt<int64_t>();
            const auto need = step.find_value("required_confirmations").getInt<int64_t>();
            if (have < 0 || need < 1 || need > 1000000 || required > 1000000 - need) throw std::runtime_error("PAYMASTER_SETUP_CONFIRMATIONS_INVALID");
            confirmed += std::min(have, need);
            required += need;
        }
    }
    for (const auto& diagnostic : diagnostics.getValues()) {
        const auto action = diagnostic.find_value("action").get_str();
        const auto code = diagnostic.find_value("code").get_str();
        if (action == "enable" && code == "PAYMASTER_PROVIDER_DISABLED" && !operations.empty()) continue;
        if (action == "inspect_error" || action == "configure_node" || action == "review_budget" || action == "setup" || action == "enable" ||
            (action == "review_liquidity" && operations.empty() && !provider.find_value("pool_ready").isTrue())) {
            return {false, false, true, "Action required: " + code + ". Review this requirement; setup has not completed."};
        }
    }
    result.complete = !result.needs_unlock && provider.find_value("ready").isTrue() &&
        provider.find_value("pool_ready").isTrue() && operations.empty() &&
        (!require_running || (provider.find_value("running").isTrue() && (service.get_str() == "active" || service.get_str() == "manual")));
    if (result.complete) result.message = require_running ? "Setup complete: provider is running and locally ready." : "Funding complete: reserves are confirmed and the provider is locally ready.";
    else if (result.needs_unlock) result.message = "Waiting for wallet unlock. Approved setup remains saved.";
    else if (waiting_funds) result.message = "Waiting for usable incoming DGB/DD funding. Confirmed funds are required; Core retries automatically within the approved limits.";
    else if (waiting_confirmation) result.message = "Waiting for reserve confirmations: " + std::to_string(confirmed) + "/" + std::to_string(required) + ". Continuation is automatic.";
    else if (!operations.empty()) result.message = "Core is creating the approved reserves or waiting for funding inputs to confirm. Continuation is automatic.";
    else result.message = "Waiting for node readiness and the requested provider start. No new spending approval is issued.";
    return result;
}
std::string OperatorAmount(int64_t satoshis)
{
    if (satoshis < 0) throw std::runtime_error("PAYMASTER_STATUS_INVALID_AMOUNT");
    const auto fraction = std::to_string(satoshis % 100000000);
    return std::to_string(satoshis / 100000000) + "." + std::string(8 - fraction.size(), '0') + fraction;
}
int64_t OperatorRemaining(int64_t limit, int64_t spent, int64_t reserved)
{
    if (limit < 0 || spent < 0 || reserved < 0) throw std::runtime_error("PAYMASTER_STATUS_INVALID_AMOUNT");
    return std::max<int64_t>(0, std::max<int64_t>(0, limit - spent) - reserved);
}
UniValue OperatorBudgets(const UniValue& snapshot)
{
    UniValue rows{UniValue::VARR};
    const auto add = [&](const std::string& name, const UniValue& usage, const UniValue& limits, bool refill) {
        const int64_t reserved = usage.find_value(refill ? "maintenance_fee_reserved_satoshis" : "reserved_network_fee_satoshis").getInt<int64_t>();
        UniValue row{UniValue::VOBJ};
        row.pushKV("name", name);
        row.pushKV("reserved", reserved);
        row.pushKV("approved", refill ? limits.find_value("paid_maintenance_approved").isTrue() : limits.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>() > 0);
        row.pushKV("transaction_limit", limits.find_value(refill ? "maximum_maintenance_fee_per_transaction_satoshis" : "maximum_network_fee_per_transaction_satoshis"));
        for (const auto& period : {std::string{"hour"}, std::string{"day"}}) {
            const auto limit = limits.find_value((refill ? "maximum_maintenance_fee_per_" : "maximum_network_fee_per_") + period + "_satoshis").getInt<int64_t>();
            const auto spent = usage.find_value((refill ? "maintenance_fee_spent_last_" : "spent_network_fee_last_") + period + "_satoshis").getInt<int64_t>();
            row.pushKV(period + "_limit", limit);
            row.pushKV(period + "_spent", spent);
            row.pushKV(period + "_remaining", OperatorRemaining(limit, spent, reserved));
        }
        rows.push_back(row);
    };
    const auto& safety = snapshot.find_value("safety");
    if (safety.find_value("configured").isTrue())
        for (const auto& name : {"user_paid", "public_sponsored", "restricted_sponsored"})
            add(name, safety.find_value(name), safety.find_value("policy").find_value(name), false);
    const auto& liquidity = snapshot.find_value("provider").find_value("liquidity");
    add("refill", liquidity, liquidity.find_value("policy"), true);
    return rows;
}
std::string OperatorSummary(const UniValue& snapshot)
{
    if (!snapshot.find_value("schema_version").isNum() || snapshot.find_value("schema_version").getInt<int>() != 1) throw std::runtime_error("PAYMASTER_STATUS_VERSION_UNKNOWN");
    const auto& provider = snapshot.find_value("provider");
    std::string result = "Network: " + snapshot.find_value("network").get_str() + "\nWallet: " + snapshot.find_value("wallet").write();
    const auto& diagnostics = snapshot.find_value("diagnostics");
    const auto explain = [](const UniValue& diagnostic) -> std::string {
        const auto action = diagnostic.find_value("action").get_str();
        if (action == "configure_node") return "Run -paymastersetup and review node configuration and routing";
        if (action == "setup") return "Continue -paymastersetup in this wallet";
        if (action == "wait" && diagnostic.find_value("area").get_str() == "service")
            return "New requests are paused; Core automatically rechecks readiness within the saved limits";
        if (action == "wait") return diagnostic.find_value("area").get_str() == "payments"
            ? "Wait for the current payment or capacity reservation; review Activity"
            : "Wait for synchronization, activation or confirmation";
        if (action == "unlock" || action == "review_unlock") return "Review wallet-wide unlock with -paymastersetup (unlock)";
        if (action == "start" || action == "enable") return "Review the saved policy, then use -paymastersetup (resume)";
        if (action == "review_liquidity") return "Review confirmed funds and pool preparation in -paymastersetup";
        if (action == "review_budget") return "Wait for rolling budget capacity or explicitly review finite limits";
        if (action == "backup_wallet") return "Create a full-wallet backup with -paymastersetup (backup)";
        if (action == "check_external") return "Run checkpaymasterendpoint from your own independent node";
        if (action == "none") return "Operation is locally ready";
        return "Inspect the diagnostic; do not treat unknown status as success";
    };
    if (!diagnostics.empty()) result += "\nNext action: " + explain(diagnostics[0]) + " (" + diagnostics[0].find_value("code").get_str() + ")";
    result += "\nService: " + provider.find_value("service_state").get_str();
    result += "\nPayment work: " + OperatorWorkPhase(provider);
    result += "\nLocal ready: " + provider.find_value("ready").write();
    const auto& transport = provider.find_value("transport");
    result += "\nConnection: local listener=" + transport.find_value("listener_ready").write() + "; outgoing=" + transport.find_value("outbound_in_use").write() + "/" + transport.find_value("outbound_limit").write() + "; incoming=" + transport.find_value("inbound_in_use").write() + "/" + transport.find_value("inbound_limit").write() + "; queued=" + transport.find_value("queued").write();
    const auto& liquidity = provider.find_value("liquidity");
    result += "\nLiquidity: " + liquidity.find_value("maintenance_state").get_str();
    for (const auto& asset : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
        const auto& counts = liquidity.find_value(asset);
        result += "\n  " + std::string(asset) + ": available=" + counts.find_value("ready").write() + ", reserved=" + counts.find_value("reserved").write() + ", pending=" + counts.find_value("pending").write() + ", missing=" + counts.find_value("missing").write();
    }
    const auto budgets = OperatorBudgets(snapshot);
    for (const auto& budget : budgets.getValues()) {
        result += "\nBudget " + budget.find_value("name").get_str() + " (DGB): approved=" + budget.find_value("approved").write();
        for (const auto& field : {"transaction_limit", "reserved", "hour_spent", "hour_limit", "hour_remaining", "day_spent", "day_limit", "day_remaining"})
            result += "; " + std::string{field} + "=" + OperatorAmount(budget.find_value(field).getInt<int64_t>());
    }
    result += "\nOpen work: " + provider.find_value("service_queue").write();
    if (provider.find_value("preparation").isArray()) result += "\nApproved setup steps: " + provider.find_value("preparation").write();
    result += "\nWallet locked: " + provider.find_value("wallet_locked").write();
    result += snapshot.find_value("unlocked_until").isNum() && snapshot.find_value("unlocked_until").getInt<int64_t>() == -1
        ? "\nOperating unlock: until manual lock, wallet unload or node restart (wallet-wide)."
        : "\nUnlock deadline (epoch seconds, 0=no timed unlock): " + snapshot.find_value("unlocked_until").write();
    result += "\nFull-wallet backup reminder: " + provider.find_value("backup_status").find_value("required").write();
    for (const auto& item : diagnostics.getValues())
        result += "\n" + item.find_value("state").get_str() + ": " + item.find_value("code").get_str() + " -> " + explain(item);
    return result + "\nExternal reachability and payment operation are not established by local readiness.\n";
}
void CheckSetupContext(const UniValue& expected, const UniValue& current)
{
    if (!current.find_value("schema_version").isNum() || current.find_value("schema_version").getInt<int>() != 1) throw std::runtime_error("PAYMASTER_SETUP_STATUS_VERSION");
    for (const auto& key : {"wallet_eligible", "wallet_locked", "settings_present", "enabled", "running", "autostart", "ready", "pool_ready"}) {
        if (!current.find_value("provider").find_value(key).isBool()) throw std::runtime_error("PAYMASTER_SETUP_STATUS_INCOMPLETE");
    }
    for (const auto& key : {"network", "wallet", "wallet_generation"}) {
        if (!expected.find_value(key).isStr() || !SetupMatches(expected.find_value(key), current.find_value(key))) throw std::runtime_error("PAYMASTER_SETUP_CONTEXT_CHANGED");
    }
    if (!current.find_value("provider").find_value("wallet_eligible").isTrue()) throw std::runtime_error("PAYMASTER_SETUP_WALLET_INELIGIBLE");
    const auto& id = expected.find_value("provider").find_value("provider_id");
    if (!id.isNull() && !SetupMatches(id, current.find_value("provider").find_value("provider_id"))) throw std::runtime_error("PAYMASTER_SETUP_IDENTITY_CHANGED");
}
void CheckSetupReply(const SetupStep& step, const UniValue& result)
{
    if (step.method == "createpaymasteridentity") {
        for (const auto& key : {"provider_id", "identity_key"}) {
            const auto& field = result.find_value(key);
            if (!field.isStr() || field.get_str().size() != 64 || !std::all_of(field.get_str().begin(), field.get_str().end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) throw std::runtime_error("PAYMASTER_SETUP_IDENTITY_REPLY_INVALID");
        }
        if (!result.find_value("created_at").isNum() || result.find_value("created_at").getInt<int64_t>() <= 0) throw std::runtime_error("PAYMASTER_SETUP_IDENTITY_REPLY_INVALID");
    }
    const auto& value = step.result_field.empty() ? result : result.find_value(step.result_field);
    if (!result.isObject() || !SetupMatches(step.expected, value)) throw std::runtime_error("PAYMASTER_SETUP_REPLY_MISMATCH: " + step.method);
}
void CheckSetupSaved(const SetupStep& step, const UniValue& snapshot)
{
    const auto& provider = snapshot.find_value("provider");
    UniValue saved;
    if (step.method == "setpaymasterpolicy")
        saved = provider.find_value("policy");
    else if (step.method == "setpaymastersafetypolicy")
        saved = snapshot.find_value("safety").find_value("policy");
    else if (step.method == "setpaymasterliquiditypolicy")
        saved = provider.find_value("liquidity").find_value("policy");
    else if (step.method == "setpaymasterenabled" || step.method == "setpaymasterruntimesettings")
        saved = provider;
    else if (step.method == "stoppaymaster") {
        if (!provider.find_value("enabled").isFalse() || !provider.find_value("autostart").isFalse() || !provider.find_value("running").isFalse()) throw std::runtime_error("PAYMASTER_SETUP_PAUSE_NOT_SAVED");
        return;
    } else
        return;
    if (!SetupMatches(step.expected, saved)) throw std::runtime_error("PAYMASTER_SETUP_STATE_CHANGED: " + step.method);
}
bool SetupPoolNeeded(const UniValue& preview)
{
    const auto& id = preview.find_value("plan_id");
    if (!id.isStr() || id.get_str().size() != 64 || !std::all_of(id.get_str().begin(), id.get_str().end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) throw std::runtime_error("PAYMASTER_SETUP_POOL_PREVIEW_INCOMPLETE");
    for (const auto& key : {"total_output_satoshis", "total_carrier_cents", "maximum_fee_satoshis", "maximum_total_fee_satoshis"})
        if (preview.find_value(key).getInt<int64_t>() < 0) throw std::runtime_error("PAYMASTER_SETUP_POOL_PREVIEW_INVALID");
    bool needed{false};
    for (const auto& key : {"missing_admission_dgb_slots", "missing_operational_dgb_slots", "missing_admission_carrier_slots", "missing_operational_carrier_slots"}) {
        const auto count = preview.find_value(key).getInt<int64_t>();
        if (count < 0) throw std::runtime_error("PAYMASTER_SETUP_POOL_PREVIEW_INVALID");
        needed |= count != 0;
    }
    return needed;
}
void CheckSetupChoices(const SetupChoices& choices)
{
    ProviderPolicy policy;
    for (const auto& model : choices.policy.find_value("funding_models").get_array().getValues()) {
        const auto name = model.get_str();
        const uint8_t bit = name == "user_paid" ? FUNDING_MODEL_USER_PAID : name == "sponsored" ? FUNDING_MODEL_SPONSORED : 0;
        if (!bit || (policy.funding_models & bit)) throw std::runtime_error("PAYMASTER_INVALID_FUNDING_MODELS");
        policy.funding_models |= bit;
    }
    const auto scope = choices.policy.find_value("sponsorship_scope").get_str();
    if (scope != "public" && scope != "restricted") throw std::runtime_error("PAYMASTER_INVALID_SPONSORSHIP_SCOPE");
    policy.sponsorship_scope = scope == "public" ? SponsorshipScope::PUBLIC : SponsorshipScope::RESTRICTED;
    policy.fee_rate_bps = choices.policy.find_value("fee_rate_bps").getInt<uint32_t>();
    policy.maximum_user_paid_service_fee = DDCents{
        choices.policy.find_value(
            "maximum_user_paid_service_fee_cents").getInt<int64_t>()};
    policy.min_payment = DDCents{choices.policy.find_value("min_amount_cents").getInt<int64_t>()};
    policy.max_payment = DDCents{choices.policy.find_value("max_amount_cents").getInt<int64_t>()};
    policy.quote_ttl = choices.policy.find_value("quote_ttl").getInt<int64_t>();
    policy.maximum_network_fee = DGBSatoshis{choices.policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>()};
    std::string error;
    if (!ValidateProviderPolicy(policy, error)) throw std::runtime_error(error);

    const auto funding_limits = [](const UniValue& value) {
        FundingSafetyLimits limits;
        limits.maximum_network_fee_per_transaction = DGBSatoshis{value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>()};
        limits.maximum_reserved_network_fee = DGBSatoshis{value.find_value("maximum_reserved_network_fee_satoshis").getInt<int64_t>()};
        limits.maximum_network_fee_per_hour = DGBSatoshis{value.find_value("maximum_network_fee_per_hour_satoshis").getInt<int64_t>()};
        limits.maximum_network_fee_per_day = DGBSatoshis{value.find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>()};
        limits.maximum_completed_per_hour = value.find_value("maximum_completed_per_hour").getInt<uint32_t>();
        limits.maximum_completed_per_day = value.find_value("maximum_completed_per_day").getInt<uint32_t>();
        return limits;
    };
    ProviderSafetyPolicy safety;
    safety.user_paid = funding_limits(choices.safety.find_value("user_paid"));
    safety.public_sponsored = funding_limits(choices.safety.find_value("public_sponsored"));
    safety.restricted_sponsored = funding_limits(choices.safety.find_value("restricted_sponsored"));
    safety.maximum_active_quotes_total = choices.safety.find_value("maximum_active_quotes_total").getInt<uint32_t>();
    safety.maximum_active_quotes_per_netgroup = choices.safety.find_value("maximum_active_quotes_per_netgroup").getInt<uint32_t>();
    safety.maximum_active_quotes_per_recipient = choices.safety.find_value("maximum_active_quotes_per_recipient").getInt<uint32_t>();
    safety.maximum_quote_requests_per_netgroup_per_minute = choices.safety.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<uint32_t>();
    safety.updated_at = 1; // Presence required by Core; no persisted timestamp is changed.
    if (!ValidateProviderSafetyPolicy(safety, policy, error)) throw std::runtime_error(error);

    ProviderLiquidityPolicy liquidity;
    liquidity.automatic_replenishment = choices.liquidity.find_value("automatic_replenishment").get_bool();
    liquidity.paid_maintenance_approved = choices.liquidity.find_value("paid_maintenance_approved").get_bool();
    liquidity.target_admission_dgb = choices.liquidity.find_value("target_admission_dgb").getInt<uint16_t>();
    liquidity.target_operational_dgb = choices.liquidity.find_value("target_operational_dgb").getInt<uint16_t>();
    liquidity.target_admission_carriers = choices.liquidity.find_value("target_admission_carriers").getInt<uint16_t>();
    liquidity.target_operational_carriers = choices.liquidity.find_value("target_operational_carriers").getInt<uint16_t>();
    liquidity.maximum_maintenance_fee_per_transaction = DGBSatoshis{choices.liquidity.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>()};
    liquidity.maximum_maintenance_fee_per_hour = DGBSatoshis{choices.liquidity.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>()};
    liquidity.maximum_maintenance_fee_per_day = DGBSatoshis{choices.liquidity.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>()};
    liquidity.updated_at = 1;
    if (!ValidateProviderLiquidityPolicy(liquidity, error)) throw std::runtime_error(error);
    if (!ProviderLiquidityTargetsSatisfyPolicy(liquidity, policy)) throw std::runtime_error("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");

    const int admission = choices.pool.find_value("admission_dgb_slots").getInt<int>();
    const int operational = choices.pool.find_value("operational_dgb_slots").getInt<int>();
    const int carriers = choices.pool.find_value("admission_carrier_slots").getInt<int>();
    const int operational_carriers = choices.pool.find_value("operational_carrier_slots").getInt<int>();
    if (admission < 3 || admission > 16 || operational < 1 || operational > 16 || carriers < 0 || carriers > 16 || operational_carriers < 0 || operational_carriers > 16)
        throw std::runtime_error("PAYMASTER_INVALID_POOL_TARGET");
    if (PolicyAllowsFundingModel(policy, FundingModel::USER_PAID)) {
        if (carriers < 3 || operational_carriers < 1) throw std::runtime_error("PAYMASTER_USER_PAID_REQUIRES_CARRIER_POOL");
    } else if (carriers || operational_carriers) {
        throw std::runtime_error("PAYMASTER_SPONSORED_POOL_HAS_CARRIERS");
    }
    if (!choices.pool.find_value("maximum_fee_satoshis").isNull()) {
        const auto fee = choices.pool.find_value("maximum_fee_satoshis").getInt<int64_t>();
        if (fee <= 0 || fee > MAX_MONEY / 2) throw std::runtime_error("PAYMASTER_POOL_INVALID_FEE_LIMIT");
    }
    if (choices.operation_mode != "automatic" && choices.operation_mode != "manual") throw std::runtime_error("PAYMASTER_INVALID_OPERATION_MODE");
}
std::vector<SetupStep> BuildSetupPlan(const UniValue& snapshot, const SetupChoices& choices)
{
    CheckSetupContext(snapshot, snapshot);
    CheckSetupChoices(choices);
    const auto& provider = snapshot.find_value("provider");
    if (provider.find_value("provider_id").isNull() && !IsValidPaymasterDisplayName(choices.display_name))
        throw std::runtime_error("PAYMASTER_INVALID_DISPLAY_NAME");
    std::vector<SetupStep> steps;
    auto add = [&](std::string method, const UniValue& input, const UniValue& expected, std::string field = {}, bool unlock = false) {
        SetupStep step;
        step.method = std::move(method);
        step.params.push_back(input);
        step.expected = expected;
        step.result_field = std::move(field);
        step.unlock = unlock;
        steps.push_back(std::move(step));
    };
    if (provider.find_value("settings_present").isTrue()) {
        UniValue stop{UniValue::VOBJ}, expected{UniValue::VOBJ};
        stop.pushKV("persistent", true);
        stop.pushKV("pause_setup", true);
        expected.pushKV("running", false);
        add("stoppaymaster", stop, expected);
    }
    if (provider.find_value("provider_id").isNull()) {
        UniValue expected{UniValue::VOBJ};
        expected.pushKV("display_name", choices.display_name);
        add("createpaymasteridentity", UniValue{choices.display_name}, expected, {}, true);
    }
    const auto& old_safety = snapshot.find_value("safety").find_value("policy");
    if (old_safety.isObject() && provider.find_value("policy").isObject()) {
        const auto bridge = SetupSafetyBridge(old_safety, choices.safety,
                                              provider.find_value("policy").find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>(),
                                              choices.policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>());
        add("setpaymastersafetypolicy", bridge, bridge);
    }
    add("setpaymasterpolicy", choices.policy, choices.policy);
    add("setpaymastersafetypolicy", choices.safety, choices.safety);
    add("setpaymasterliquiditypolicy", choices.liquidity, choices.liquidity);
    // The caller inserts exact preview/approval/execution before these final steps.
    SetupStep pool;
    pool.method = "preparepaymasterpool";
    auto pool_options = choices.pool;
    if (pool_options.find_value("maximum_fee_satoshis").isNull()) pool_options.pushKV("maximum_fee_satoshis", SetupFundingFee(snapshot));
    pool.params.push_back(pool_options);
    steps.push_back(pool);
    UniValue runtime{UniValue::VOBJ};
    runtime.pushKV("operation_mode", choices.operation_mode);
    runtime.pushKV("autostart", choices.autostart.value_or(provider.find_value("autostart").isTrue()));
    add("setpaymasterruntimesettings", runtime, runtime);
    UniValue enabled{UniValue::VOBJ};
    enabled.pushKV("enabled", choices.enabled);
    add("setpaymasterenabled", UniValue{choices.enabled}, enabled);
    return steps;
}
} // namespace DigiDollar::Paymaster
