// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <boost/test/unit_test.hpp>
#include <paymaster/setup.h>
#include <stdexcept>
#include <sstream>
#include <algorithm>

using namespace DigiDollar::Paymaster;
namespace {
UniValue Snapshot()
{
    UniValue snapshot{UniValue::VOBJ}, provider{UniValue::VOBJ};
    snapshot.pushKV("schema_version", 1);
    snapshot.pushKV("network", "regtest");
    snapshot.pushKV("wallet", "provider");
    snapshot.pushKV("wallet_generation", "first-load");
    for (const auto& key : {"wallet_eligible", "wallet_locked", "settings_present", "enabled", "running", "autostart", "ready", "pool_ready"})
        provider.pushKV(key, std::string{key} == "wallet_eligible");
    provider.pushKV("readiness_errors", UniValue{UniValue::VARR});
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("safety", UniValue{UniValue::VOBJ});
    return snapshot;
}
SetupChoices Choices()
{
    SetupChoices choices;
    choices.display_name = "Provider";
    choices.policy = SetupDefaultPolicy();
    choices.safety = SetupDefaultSafety(20000000, true, false, false);
    choices.liquidity = SetupDefaultLiquidity(true);
    choices.pool.pushKV("execute", false);
    return choices;
}
} // namespace
BOOST_AUTO_TEST_SUITE(paymaster_setup_tests)
BOOST_AUTO_TEST_CASE(finite_profiles_do_not_authorize_maintenance)
{
    const auto ordinary = SetupSafetyProfile(false, 20000000);
    const auto conservative = SetupSafetyProfile(true, 20000000);
    BOOST_CHECK_EQUAL(ordinary.per_day, 1000000000);
    BOOST_CHECK_EQUAL(conservative.per_day, 200000000);
    const auto liquidity = SetupDefaultLiquidity(true);
    BOOST_CHECK(liquidity.find_value("paid_maintenance_approved").isFalse());
    BOOST_CHECK(liquidity.find_value("automatic_replenishment").isFalse());
    BOOST_CHECK_EQUAL(liquidity.find_value("target_admission_carriers").getInt<int>(), 3);
    BOOST_CHECK_EQUAL(SetupDefaultLiquidity(false).find_value("target_operational_carriers").getInt<int>(), 0);
    const auto safety = SetupDefaultSafety(20000000, true, false, false);
    BOOST_CHECK_EQUAL(safety.find_value("public_sponsored").find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>(), 0);
}
BOOST_AUTO_TEST_CASE(cli_proposals_support_continuous_operation_without_overwriting_saved_limits)
{
    const auto snapshot = Snapshot();
    const auto defaults = SetupCliDefaults(snapshot);
    BOOST_CHECK(defaults.enabled);
    BOOST_REQUIRE(defaults.autostart.has_value());
    BOOST_CHECK(*defaults.autostart);
    BOOST_CHECK_EQUAL(defaults.operation_mode, "automatic");
    BOOST_CHECK(defaults.liquidity.find_value("automatic_replenishment").isTrue());
    BOOST_CHECK(defaults.liquidity.find_value("paid_maintenance_approved").isTrue());
    BOOST_CHECK_EQUAL(defaults.pool.find_value("maximum_fee_satoshis").getInt<int64_t>(), 50000000);
    BOOST_CHECK_EQUAL(defaults.liquidity.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>(), 50000000);
    BOOST_CHECK(SetupMatches(snapshot, Snapshot())); // A proposal has no side effects.
    auto saved = snapshot;
    auto provider = saved.find_value("provider");
    provider.pushKV("settings_present", true);
    provider.pushKV("operation_mode", "manual");
    provider.pushKV("autostart", false);
    provider.pushKV("enabled", false);
    auto policy = defaults.policy;
    policy.pushKV("fee_rate_bps", 120);
    provider.pushKV("policy", policy);
    auto liquidity = SetupDefaultLiquidity(true);
    liquidity.pushKV("maximum_maintenance_fee_per_transaction_satoshis", 1234567);
    UniValue liquidity_status{UniValue::VOBJ};
    liquidity_status.pushKV("policy_configured", true);
    liquidity_status.pushKV("policy", liquidity);
    provider.pushKV("liquidity", liquidity_status);
    saved.pushKV("provider", provider);
    UniValue safety{UniValue::VOBJ};
    auto limits = defaults.safety;
    auto paid = limits.find_value("user_paid");
    paid.pushKV("maximum_network_fee_per_day_satoshis", int64_t{9007199254740993});
    limits.pushKV("user_paid", paid);
    safety.pushKV("policy", limits);
    saved.pushKV("safety", safety);
    const auto retained = SetupCliDefaults(saved);
    BOOST_CHECK(!retained.enabled && !*retained.autostart);
    BOOST_CHECK_EQUAL(retained.operation_mode, "manual");
    BOOST_CHECK(SetupMatches(retained.policy, policy));
    BOOST_CHECK(SetupMatches(retained.safety, limits));
    BOOST_CHECK(SetupMatches(retained.liquidity, liquidity));
    const auto plan = BuildSetupPlan(snapshot, defaults);
    BOOST_CHECK(plan[plan.size() - 2].params[0].find_value("autostart").isTrue());
    auto declined = defaults;
    declined.autostart = false;
    const auto without_autostart = BuildSetupPlan(snapshot, declined);
    BOOST_CHECK(without_autostart[without_autostart.size() - 2].params[0].find_value("autostart").isFalse());
}
BOOST_AUTO_TEST_CASE(cli_menus_retry_and_confirmation_never_approves_an_empty_answer)
{
    const std::vector<SetupMenuItem> items{{"automatic", "Automatic", "Processes requests without manual queue steps."}, {"manual", "Manual", "Requires explicit queue processing."}};
    std::ostringstream output;
    std::istringstream defaults{"?\nbad selection\n\n"};
    BOOST_CHECK_EQUAL(SetupSelect(defaults, output, "Processing", items, "automatic"), "automatic");
    BOOST_CHECK(output.str().find("earlier choices are retained") != std::string::npos);
    std::istringstream number{"2\n"};
    BOOST_CHECK_EQUAL(SetupSelect(number, output, "Processing", items, "automatic"), "manual");
    std::istringstream named{" AUTOMATIC \n"};
    BOOST_CHECK_EQUAL(SetupSelect(named, output, "Processing", items, "manual"), "automatic");
    std::istringstream blank{"\n"}, typo{"yess\nno\n"}, approve{"yes\n"}, eof;
    BOOST_CHECK(!SetupConfirm(blank, output, "Apply"));
    BOOST_CHECK(!SetupConfirm(typo, output, "Apply"));
    BOOST_CHECK(SetupConfirm(approve, output, "Apply"));
    BOOST_CHECK_THROW(SetupConfirm(eof, output, "Apply"), std::runtime_error);
    BOOST_CHECK_THROW(SetupSelect(eof, output, "Processing", items, "automatic"), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(cli_currency_and_percentage_inputs_are_exact_and_retry_invalid_values)
{
    const auto field = [](const std::string& key) -> SetupField {
        const auto& fields = SetupFields();
        return *std::find_if(fields.begin(), fields.end(), [&](const auto& item) { return item.key == key; });
    };
    std::ostringstream output;
    std::istringstream dd{"1.001\n0.99\n1,25\n"};
    BOOST_CHECK_EQUAL(SetupReadNumber(dd, output, field("min_amount_cents"), 100), 125);
    std::istringstream fee{"0.55\n0.60\n"};
    BOOST_CHECK_EQUAL(SetupReadNumber(fee, output, field("fee_rate_bps"), 50), 60);
    std::istringstream dgb{"1e2\n1,000.00\n-1\n0.000000001\n0.00000001\n"};
    BOOST_CHECK_EQUAL(SetupReadNumber(dgb, output, field("maximum_fee_satoshis"), 50000000), 1);
    std::istringstream exact{"90071992.54740993\n"};
    BOOST_CHECK_EQUAL(SetupReadNumber(exact, output, field("maximum_fee_satoshis"), 50000000), int64_t{9007199254740993});
    BOOST_CHECK_EQUAL(SetupFormatNumber(9007199254740993, 8), "90071992.54740993");
    std::istringstream ttl{"61\n0\n60\n"};
    BOOST_CHECK_EQUAL(SetupReadNumber(ttl, output, field("quote_ttl"), 60), 60);
    std::istringstream slots{"17\n2\n3\n"};
    BOOST_CHECK_EQUAL(SetupReadNumber(slots, output, field("target_admission_dgb"), 3), 3);
    BOOST_CHECK(output.str().find("Please try again") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(plan_stops_before_changes_and_preserves_saved_autostart)
{
    auto snapshot = Snapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("settings_present", true);
    provider.pushKV("enabled", true);
    provider.pushKV("autostart", true);
    provider.pushKV("provider_id", "existing");
    snapshot.pushKV("provider", provider);
    const auto plan = BuildSetupPlan(snapshot, Choices());
    BOOST_CHECK_EQUAL(plan.front().method, "stoppaymaster");
    BOOST_CHECK(plan.front().params[0].find_value("persistent").isTrue());
    BOOST_CHECK(plan.front().params[0].find_value("pause_setup").isTrue());
    BOOST_CHECK_EQUAL(plan.back().method, "setpaymasterenabled");
    BOOST_CHECK(plan[plan.size() - 2].params[0].find_value("autostart").isTrue());
    for (const auto& step : plan)
        BOOST_CHECK(step.method != "createpaymasteridentity" && step.method != "startpaymaster");
    const auto fresh = BuildSetupPlan(Snapshot(), Choices());
    BOOST_CHECK(fresh[fresh.size() - 2].params[0].find_value("autostart").isFalse());
}
BOOST_AUTO_TEST_CASE(reload_and_incomplete_state_fail_before_writes)
{
    const auto initial = Snapshot();
    auto changed = initial;
    changed.pushKV("wallet_generation", "reloaded");
    BOOST_CHECK_THROW(CheckSetupContext(initial, changed), std::runtime_error);
    changed = initial;
    changed.pushKV("network", "main");
    BOOST_CHECK_THROW(CheckSetupContext(initial, changed), std::runtime_error);
    changed = initial;
    changed.pushKV("provider", UniValue{UniValue::VOBJ});
    BOOST_CHECK_THROW(BuildSetupPlan(changed, Choices()), std::runtime_error);
    changed = initial;
    changed.pushKV("schema_version", 2);
    BOOST_CHECK_THROW(BuildSetupPlan(changed, Choices()), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(safety_bridge_does_not_forward_readonly_metadata)
{
    auto prior = SetupDefaultSafety(20000000, true, false, false);
    prior.pushKV("updated_at", 123);
    const auto target = SetupDefaultSafety(10000000, false, true, false);
    const auto bridge = SetupSafetyBridge(prior, target, 20000000, 10000000);
    BOOST_CHECK(bridge.find_value("updated_at").isNull());
    BOOST_CHECK_EQUAL(bridge.find_value("user_paid").find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>(), 10000000);
    BOOST_CHECK_EQUAL(bridge.find_value("public_sponsored").find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>(), 10000000);
    BOOST_CHECK_EQUAL(bridge.find_value("restricted_sponsored").find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>(), 0);
}
BOOST_AUTO_TEST_CASE(exact_replies_and_separate_reachability)
{
    const auto plan = BuildSetupPlan(Snapshot(), Choices());
    BOOST_CHECK_EQUAL(plan.front().method, "createpaymasteridentity");
    UniValue forged{UniValue::VOBJ};
    forged.pushKV("display_name", "Provider");
    BOOST_CHECK_THROW(CheckSetupReply(plan.front(), forged), std::runtime_error);
    forged.pushKV("provider_id", std::string(64, 'a'));
    forged.pushKV("identity_key", std::string(64, 'b'));
    forged.pushKV("created_at", 1);
    BOOST_CHECK_NO_THROW(CheckSetupReply(plan.front(), forged));
    forged.pushKV("display_name", "Other");
    BOOST_CHECK_THROW(CheckSetupReply(plan.front(), forged), std::runtime_error);
    const auto diagnostics = OperatorDiagnostics(Snapshot().find_value("provider"), 0, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("code").get_str(), "PAYMASTER_LOCAL_READY");
    BOOST_CHECK_EQUAL(diagnostics[1].find_value("state").get_str(), "unknown");
    BOOST_CHECK_EQUAL(diagnostics[1].find_value("action").get_str(), "check_external");
}
BOOST_AUTO_TEST_CASE(budget_remaining_clamps_without_overflow)
{
    BOOST_CHECK_EQUAL(OperatorRemaining(100, 20, 30), 50);
    BOOST_CHECK_EQUAL(OperatorRemaining(100, 120, 30), 0);
    BOOST_CHECK_EQUAL(OperatorRemaining(INT64_MAX, INT64_MAX, INT64_MAX), 0);
    BOOST_CHECK_THROW(OperatorRemaining(1, -1, 0), std::runtime_error);
    BOOST_CHECK_EQUAL(OperatorAmount(123456789), "1.23456789");
}
BOOST_AUTO_TEST_CASE(saved_state_is_checked_after_rpc_success)
{
    const auto choices = Choices();
    const auto plan = BuildSetupPlan(Snapshot(), choices);
    const auto policy_step = plan[1];
    auto snapshot = Snapshot();
    BOOST_CHECK_THROW(CheckSetupSaved(policy_step, snapshot), std::runtime_error);
    auto provider = snapshot.find_value("provider");
    provider.pushKV("policy", choices.policy);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK_NO_THROW(CheckSetupSaved(policy_step, snapshot));
    auto policy = choices.policy;
    policy.pushKV("fee_rate_bps", 75);
    provider.pushKV("policy", policy);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK_THROW(CheckSetupSaved(policy_step, snapshot), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(unknown_errors_and_disabled_index_are_not_waiting_success)
{
    auto provider = Snapshot().find_value("provider");
    UniValue errors{UniValue::VARR};
    errors.push_back("PAYMASTER_REQUIRES_TXINDEX");
    errors.push_back("FUTURE_DIAGNOSTIC");
    provider.pushKV("readiness_errors", errors);
    const auto diagnostics = OperatorDiagnostics(provider, 140, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "configure_node");
    BOOST_CHECK_EQUAL(diagnostics[1].find_value("state").get_str(), "error");
    BOOST_CHECK_EQUAL(diagnostics[1].find_value("code").get_str(), "FUTURE_DIAGNOSTIC");
    BOOST_CHECK_EQUAL(diagnostics[2].find_value("action").get_str(), "review_unlock");
}
BOOST_AUTO_TEST_CASE(operator_distinguishes_capacity_wait_from_errors)
{
    auto provider = Snapshot().find_value("provider");
    provider.pushKV("running", true);
    provider.pushKV("service_state", "waiting_for_liquidity_confirmation");
    provider.pushKV("last_service_error", "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING");
    UniValue errors{UniValue::VARR};
    errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    provider.pushKV("readiness_errors", errors);
    auto diagnostics = OperatorDiagnostics(provider, 0, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("state").get_str(), "waiting");
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "wait");
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("severity").get_str(), "info");

    // A known wait must not mask unrelated errors or grant readiness.
    errors.push_back("PAYMASTER_INVALID_MAINTENANCE_LEDGER");
    provider.pushKV("readiness_errors", errors);
    diagnostics = OperatorDiagnostics(provider, 0, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "inspect_error");
    errors = UniValue{UniValue::VARR};
    errors.push_back("PAYMASTER_OPERATIONAL_SLOT_MISSING");
    provider.pushKV("readiness_errors", errors);
    provider.pushKV("service_state", "error");
    diagnostics = OperatorDiagnostics(provider, 0, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "inspect_error");
    provider.pushKV("service_state", "waiting_for_liquidity_confirmation");
    provider.pushKV("running", false);
    diagnostics = OperatorDiagnostics(provider, 0, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "inspect_error");
    provider.pushKV("running", true);
    provider.pushKV("last_service_error", "PAYMASTER_FUTURE_CONFIRMATION_ERROR");
    diagnostics = OperatorDiagnostics(provider, 0, 100);
    BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "inspect_error");

    for (const auto* reason : {"PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED", "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"}) {
        provider.pushKV("readiness_errors", UniValue{UniValue::VARR});
        provider.pushKV("last_service_error", reason);
        provider.pushKV("service_state", "waiting_for_maintenance_approval");
        diagnostics = OperatorDiagnostics(provider, 0, 100);
        BOOST_CHECK_EQUAL(diagnostics[0].find_value("action").get_str(), "review_budget");
    }
}
BOOST_AUTO_TEST_CASE(setup_waits_for_funding_confirmations_and_actual_start)
{
    auto snapshot = Snapshot();
    auto provider = snapshot.find_value("provider");
    provider.pushKV("enabled", true);
    provider.pushKV("service_state", "waiting_for_readiness");
    UniValue work{UniValue::VARR}, step{UniValue::VOBJ};
    step.pushKV("state", "pending_creation");
    step.pushKV("error", "PAYMASTER_POOL_WAITING_FUNDS: insufficient DD");
    work.push_back(step);
    provider.pushKV("active_operations", work);
    snapshot.pushKV("provider", provider);
    snapshot.pushKV("diagnostics", UniValue{UniValue::VARR});
    auto progress = InspectSetupProgress(snapshot, true);
    BOOST_CHECK(!progress.complete && !progress.blocked);
    BOOST_CHECK(progress.message.find("incoming") != std::string::npos);
    // Old journal lock/disable reasons can outlive an explicit unlock/enable
    // until Core's next pass. They must not terminate the funding monitor.
    for (const auto* stale : {"PAYMASTER_WALLET_LOCKED", "PAYMASTER_PROVIDER_DISABLED"}) {
        step.pushKV("error", stale);
        work = UniValue{UniValue::VARR};
        work.push_back(step);
        provider.pushKV("active_operations", work);
        snapshot.pushKV("provider", provider);
        progress = InspectSetupProgress(snapshot, true);
        BOOST_CHECK(!progress.complete && !progress.blocked && !progress.needs_unlock);
    }
    provider.pushKV("enabled", false);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK(InspectSetupProgress(snapshot, true).blocked);
    provider.pushKV("enabled", true);
    step.pushKV("state", "pending_confirmation");
    step.pushKV("error", "");
    step.pushKV("confirmations", 0);
    step.pushKV("required_confirmations", 1);
    work = UniValue{UniValue::VARR}; work.push_back(step);
    provider.pushKV("active_operations", work);
    snapshot.pushKV("provider", provider);
    progress = InspectSetupProgress(snapshot, true);
    BOOST_CHECK(!progress.complete && !progress.blocked);
    BOOST_CHECK(progress.message.find("0/1") != std::string::npos);
    step.pushKV("required_confirmations", 0);
    work = UniValue{UniValue::VARR};
    work.push_back(step);
    provider.pushKV("active_operations", work);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK_THROW(InspectSetupProgress(snapshot, true), std::runtime_error);
    step.pushKV("required_confirmations", 1);
    step.pushKV("error", "PAYMASTER_POOL_FEE_LIMIT");
    work = UniValue{UniValue::VARR}; work.push_back(step);
    provider.pushKV("active_operations", work);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK(InspectSetupProgress(snapshot, true).blocked);
    provider.pushKV("active_operations", UniValue{UniValue::VARR});
    provider.pushKV("ready", true);
    provider.pushKV("pool_ready", true);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK(!InspectSetupProgress(snapshot, true).complete);
    BOOST_CHECK(InspectSetupProgress(snapshot, false).complete);
    provider.pushKV("running", true);
    provider.pushKV("service_state", "active");
    snapshot.pushKV("provider", provider);
    BOOST_CHECK(InspectSetupProgress(snapshot, true).complete);
    provider.pushKV("wallet_locked", true);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK(InspectSetupProgress(snapshot, true).needs_unlock);
    BOOST_CHECK(!InspectSetupProgress(snapshot, true).complete);
    provider.pushKV("service_state", "future_state");
    snapshot.pushKV("provider", provider);
    BOOST_CHECK(InspectSetupProgress(snapshot, true).blocked);
}
BOOST_AUTO_TEST_CASE(pool_preview_must_be_complete_before_approval)
{
    UniValue preview{UniValue::VOBJ};
    BOOST_CHECK_THROW(SetupPoolNeeded(preview), std::runtime_error);
    preview.pushKV("plan_id", std::string(64, 'a'));
    for (const auto& key : {"total_output_satoshis", "total_carrier_cents", "maximum_fee_satoshis", "maximum_total_fee_satoshis", "missing_admission_dgb_slots", "missing_operational_dgb_slots", "missing_admission_carrier_slots", "missing_operational_carrier_slots"})
        preview.pushKV(key, 0);
    BOOST_CHECK(!SetupPoolNeeded(preview));
    preview.pushKV("missing_operational_dgb_slots", 1);
    BOOST_CHECK(SetupPoolNeeded(preview));
    preview.pushKV("maximum_total_fee_satoshis", -1);
    BOOST_CHECK_THROW(SetupPoolNeeded(preview), std::runtime_error);
}
BOOST_AUTO_TEST_CASE(node_prerequisites_preserve_suitable_existing_budgets)
{
    UniValue current{UniValue::VOBJ}, snapshot{UniValue::VOBJ};
    for (const auto& key : {"digidollar", "paymaster", "txindex", "v2transport"})
        current.pushKV(key, 1);
    current.pushKV("prune", 0);
    current.pushKV("maxconnections", 48);
    current.pushKV("paymastermaxoutbound", 4);
    current.pushKV("paymastermaxinbound", 16);
    current.pushKV("ordinary_outbound_target", 19);
    snapshot.pushKV("node_settings", current);
    BOOST_CHECK(SetupNodePrerequisites(snapshot).empty());
    current.pushKV("maxconnections", 20);
    current.pushKV("txindex", 0);
    snapshot.pushKV("node_settings", current);
    const auto repair = SetupNodePrerequisites(snapshot);
    BOOST_CHECK_EQUAL(repair.find_value("maxconnections").getInt<int>(), 125);
    BOOST_CHECK_EQUAL(repair.find_value("txindex").getInt<int>(), 1);
    BOOST_CHECK(repair.find_value("paymastermaxoutbound").isNull());
}
BOOST_AUTO_TEST_CASE(resume_preserves_approved_setup_fee)
{
    auto snapshot = Snapshot();
    UniValue record{UniValue::VOBJ}, records{UniValue::VARR};
    record.pushKV("maximum_fee_satoshis", 123456);
    records.push_back(record);
    auto provider = snapshot.find_value("provider");
    provider.pushKV("preparation", records);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK_EQUAL(SetupFundingFee(snapshot), 123456);
    const auto plan = BuildSetupPlan(snapshot, Choices());
    for (const auto& step : plan)
        if (step.method == "preparepaymasterpool") BOOST_CHECK_EQUAL(step.params[0].find_value("maximum_fee_satoshis").getInt<int64_t>(), 123456);
    record.pushKV("maximum_fee_satoshis", 222222);
    records.push_back(record);
    provider.pushKV("preparation", records);
    snapshot.pushKV("provider", provider);
    BOOST_CHECK_THROW(SetupFundingFee(snapshot), std::runtime_error);
}
BOOST_AUTO_TEST_SUITE_END()
