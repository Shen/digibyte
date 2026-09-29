// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <algorithm>
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
UniValue SetupDefaultLiquidity(bool user_paid)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("automatic_replenishment", false);
    result.pushKV("paid_maintenance_approved", false);
    result.pushKV("target_admission_dgb", 3);
    result.pushKV("target_operational_dgb", 1);
    result.pushKV("target_admission_carriers", user_paid ? 3 : 0);
    result.pushKV("target_operational_carriers", user_paid ? 1 : 0);
    result.pushKV("maximum_maintenance_fee_per_transaction_satoshis", 20000000);
    result.pushKV("maximum_maintenance_fee_per_hour_satoshis", 200000000);
    result.pushKV("maximum_maintenance_fee_per_day_satoshis", 1000000000);
    return result;
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
        else if (code == "PAYMASTER_NODE_NOT_READY" || code == "PAYMASTER_REQUIRES_READY_TXINDEX" || code == "PAYMASTER_DIGIDOLLAR_NOT_ACTIVE")
            add(code, "waiting", "node", "wait");
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
        } else {
            add(service_error.get_str(), "error", "service", "inspect_error");
        }
    }
    if (errors.empty() && !preparation_pending) add("PAYMASTER_LOCAL_READY", "ready", "service", provider.find_value("running").isTrue() ? "none" : "start");
    add("PAYMASTER_EXTERNAL_REACHABILITY_UNKNOWN", "unknown", "connection", "check_external");
    auto items = result.getValues();
    const auto priority = [](const UniValue& item) {
        const auto action = item.find_value("action").get_str();
        if (action == "inspect_error" || action == "configure_node") return 0;
        if (action == "setup") return 1;
        if (action == "wait" || action == "unlock" || action == "review_unlock") return 2;
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
    const auto explain = [](const std::string& action) -> std::string {
        if (action == "configure_node") return "Run -paymastersetup and review node configuration and routing";
        if (action == "setup") return "Continue -paymastersetup in this wallet";
        if (action == "wait") return "Wait for synchronization, activation or confirmation";
        if (action == "unlock" || action == "review_unlock") return "Review wallet-wide unlock with -paymastersetup (unlock)";
        if (action == "start" || action == "enable") return "Review the saved policy, then use -paymastersetup (resume)";
        if (action == "review_liquidity") return "Review confirmed funds and pool preparation in -paymastersetup";
        if (action == "review_budget") return "Wait for rolling budget capacity or explicitly review finite limits";
        if (action == "backup_wallet") return "Create a full-wallet backup with -paymastersetup (backup)";
        if (action == "check_external") return "Run checkpaymasterendpoint from your own independent node";
        if (action == "none") return "Operation is locally ready";
        return "Inspect the diagnostic; do not treat unknown status as success";
    };
    if (!diagnostics.empty()) result += "\nNext action: " + explain(diagnostics[0].find_value("action").get_str()) + " (" + diagnostics[0].find_value("code").get_str() + ")";
    result += "\nService: " + provider.find_value("service_state").get_str();
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
    result += "\nUnlock deadline (epoch seconds, 0=no timed unlock): " + snapshot.find_value("unlocked_until").write();
    result += "\nFull-wallet backup reminder: " + provider.find_value("backup_status").find_value("required").write();
    for (const auto& item : diagnostics.getValues())
        result += "\n" + item.find_value("state").get_str() + ": " + item.find_value("code").get_str() + " -> " + explain(item.find_value("action").get_str());
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
std::vector<SetupStep> BuildSetupPlan(const UniValue& snapshot, const SetupChoices& choices)
{
    CheckSetupContext(snapshot, snapshot);
    const auto& provider = snapshot.find_value("provider");
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
    runtime.pushKV("autostart", provider.find_value("autostart").isTrue());
    add("setpaymasterruntimesettings", runtime, runtime);
    UniValue enabled{UniValue::VOBJ};
    enabled.pushKV("enabled", choices.enabled);
    add("setpaymasterenabled", UniValue{choices.enabled}, enabled);
    return steps;
}
} // namespace DigiDollar::Paymaster
