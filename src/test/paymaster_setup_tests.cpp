// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <boost/test/unit_test.hpp>
#include <paymaster/setup.h>
#include <stdexcept>

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
BOOST_AUTO_TEST_CASE(plan_stops_before_changes_and_never_autostarts)
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
    BOOST_CHECK(plan[plan.size() - 2].params[0].find_value("autostart").isFalse());
    for (const auto& step : plan)
        BOOST_CHECK(step.method != "createpaymasteridentity" && step.method != "startpaymaster");
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
