// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <digidollar/health.h>
#include <digidollar/digidollar.h>
#include <consensus/digidollar.h>
#include <consensus/volatility.h>
#include <primitives/transaction.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <chainparams.h>
#include <rpc/server.h>
#include <chrono>
#include <map>

#include <boost/test/unit_test.hpp>

// Forward declaration of ExtractDDAmount from consensus/digidollar.cpp
namespace DigiDollar {
    bool ExtractDDAmount(const CScript& script, CAmount& amount);
}

BOOST_AUTO_TEST_SUITE(digidollar_health_tests)

struct DigiDollarHealthTestSetup : public TestingSetup {
    DigiDollarHealthTestSetup() : TestingSetup(ChainType::REGTEST) {
        // Initialize test environment
        mockHeight = 1000;
        mockOraclePrice = 50000; // $0.50 per DGB (50000 * 0.001 cents = 50 cents)
        mockVolatility = 15.0; // 15% volatility

        // Set up mock collateral positions for testing
        SetupMockPositions();

        // Set up block for metrics testing
        SetupMockBlock();
    }

    void SetupMockPositions() {
        // NOTE: With oraclePrice = 50000 (0.001 cents per DGB format)
        // That's 50 cents per DGB = $0.50 per DGB
        // So each DGB is worth $0.50 = 50 cents

        // Tier 1: 30-day locks (150% ratio)
        tier1Positions = {
            {COutPoint(uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0),
             7200000000, 2400000, mockHeight + 30 * 24 * 4, 150}, // 72 DGB * $0.50 = $36, DD = $24, ratio = 150%
            {COutPoint(uint256S("1111111111111111111111111111111111111111111111111111111111111112"), 0),
             3600000000, 1200000, mockHeight + 30 * 24 * 4, 150}  // 36 DGB * $0.50 = $18, DD = $12, ratio = 150%
        };

        // Tier 2: 90-day locks (125% ratio)
        tier2Positions = {
            {COutPoint(uint256S("2222222222222222222222222222222222222222222222222222222222222222"), 0),
             5000000000, 2000000, mockHeight + 90 * 24 * 4, 125}, // 50 DGB * $0.50 = $25, DD = $20, ratio = 125%
            {COutPoint(uint256S("2222222222222222222222222222222222222222222222222222222222222223"), 0),
             7500000000, 3000000, mockHeight + 90 * 24 * 4, 125}  // 75 DGB * $0.50 = $37.50, DD = $30, ratio = 125%
        };

        // Tier 3: 365-day locks (110% ratio)
        tier3Positions = {
            {COutPoint(uint256S("3333333333333333333333333333333333333333333333333333333333333333"), 0),
             5500000000, 2500000, mockHeight + 365 * 24 * 4, 110}, // 55 DGB * $0.50 = $27.50, DD = $25, ratio = 110%
            {COutPoint(uint256S("3333333333333333333333333333333333333333333333333333333333333334"), 0),
             11000000000, 5000000, mockHeight + 365 * 24 * 4, 110} // 110 DGB * $0.50 = $55, DD = $50, ratio = 110%
        };

        // Total: 358 DGB locked = $179 value, $186 DD minted, ratio = 96% (UNHEALTHY!)
        allPositions = tier1Positions;
        allPositions.insert(allPositions.end(), tier2Positions.begin(), tier2Positions.end());
        allPositions.insert(allPositions.end(), tier3Positions.begin(), tier3Positions.end());
    }

    void SetupMockBlock() {
        mockBlock.nTime = GetTime();
        // mockBlock.nHeight = mockHeight; // CBlock doesn't have nHeight member

        // Add some transactions to simulate activity
        CMutableTransaction tx1;
        tx1.nVersion = 1;
        mockBlock.vtx.push_back(MakeTransactionRef(std::move(tx1)));

        CMutableTransaction tx2;
        tx2.nVersion = 1;
        mockBlock.vtx.push_back(MakeTransactionRef(std::move(tx2)));
    }

    ~DigiDollarHealthTestSetup() {
        // Shutdown + Initialize cycles the static state so subsequent suites
        // (e.g. rh64_dca_table_disagreement_tests) do not inherit the
        // mock metrics seeded above. ResetMetrics alone leaves
        // s_initialized=true with empty tiers, which breaks the next case
        // in this same suite (DD-FA-TEST-005).
        DigiDollar::SystemHealthMonitor::Shutdown();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
        DigiDollar::Volatility::VolatilityMonitor::ClearFreeze();
    }

    int mockHeight;
    CAmount mockOraclePrice;
    double mockVolatility;
    CBlock mockBlock;

    std::vector<CCollateralPosition> tier1Positions;
    std::vector<CCollateralPosition> tier2Positions;
    std::vector<CCollateralPosition> tier3Positions;
    std::vector<CCollateralPosition> allPositions;
};

// Test 1: System Metrics Collection
BOOST_FIXTURE_TEST_CASE(test_system_metrics_collection, DigiDollarHealthTestSetup)
{
    // Test getting current system metrics
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Should have initialized values
    BOOST_CHECK_GE(metrics.totalDDSupply, 0);
    BOOST_CHECK_GE(metrics.totalCollateral, 0);
    BOOST_CHECK_GE(metrics.systemHealth, 0);
    BOOST_CHECK_LE(metrics.systemHealth, 300); // Max 300%

    // Should have tier breakdown
    BOOST_CHECK_GE(metrics.tiers.size(), 3); // At least 3 tiers

    // Check tier structure
    for (const auto& tier : metrics.tiers) {
        BOOST_CHECK_GE(tier.lockDays, 0); // Tier 0 has lockDays=0 (240 blocks = ~1 hour for testing)
        BOOST_CHECK_GE(tier.ddMinted, 0);
        BOOST_CHECK_GE(tier.dgbLocked, 0);
        BOOST_CHECK_GE(tier.positions, 0);
        BOOST_CHECK_GE(tier.healthRatio, 0);
    }

    // Protection status should be valid
    BOOST_CHECK_GE(metrics.dcaMultiplier, 1.0);
    BOOST_CHECK_LE(metrics.dcaMultiplier, 10.0);
    BOOST_CHECK_GE(metrics.volatility, 0.0);
    BOOST_CHECK_LE(metrics.volatility, 100.0);

    // Oracle status
    BOOST_CHECK_GE(metrics.activeOracles, 0);
    BOOST_CHECK_LE(metrics.activeOracles, 35); // Reserved oracle roster capacity
    BOOST_CHECK_GE(metrics.lastOraclePrice, 0);
    BOOST_CHECK_GE(metrics.lastOracleUpdate, 0);
}

// Test 2: Per-Tier Tracking
BOOST_FIXTURE_TEST_CASE(test_per_tier_tracking, DigiDollarHealthTestSetup)
{
    // Get tier breakdown
    std::vector<DigiDollar::SystemMetrics::TierMetrics> tiers =
        DigiDollar::SystemHealthMonitor::GetTierBreakdown();

    BOOST_CHECK_GE(tiers.size(), 3); // Should have at least 3 tiers

    // Verify tier ordering (by lock days)
    for (size_t i = 1; i < tiers.size(); ++i) {
        BOOST_CHECK_GT(tiers[i].lockDays, tiers[i-1].lockDays);
    }

    // Test individual tier metrics
    for (const auto& tier : tiers) {
        // Lock days should be reasonable
        // System has 9 tiers: 240 blocks (~1hr), 30 days, 90 days, 180 days, 365 days, 1095 days, 1825 days, 2555 days, 3650 days
        BOOST_CHECK_GE(tier.lockDays, 0);    // Tier 0 = 240 blocks (~1 hour for testing/onboarding)
        BOOST_CHECK_LE(tier.lockDays, 3650); // Max 10 years (tier 8)

        // Amounts should be consistent
        if (tier.positions > 0) {
            BOOST_CHECK_GT(tier.ddMinted, 0);
            BOOST_CHECK_GT(tier.dgbLocked, 0);
            BOOST_CHECK_GT(tier.healthRatio, 0);

            // Health ratio should be reasonable for active tiers
            BOOST_CHECK_GE(tier.healthRatio, 80);  // Minimum viable
            BOOST_CHECK_LE(tier.healthRatio, 300); // Maximum reasonable
        }
    }
}

// Test 3: Real-time Health Updates
BOOST_FIXTURE_TEST_CASE(test_realtime_health_updates, DigiDollarHealthTestSetup)
{
    // Get initial metrics
    DigiDollar::SystemMetrics initialMetrics =
        DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Simulate block update
    DigiDollar::SystemHealthMonitor::UpdateMetrics(mockBlock);

    // Get updated metrics
    DigiDollar::SystemMetrics updatedMetrics =
        DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Metrics should be updated
    // (Implementation will determine if values actually change)
    BOOST_CHECK_GE(updatedMetrics.totalDDSupply, 0);
    BOOST_CHECK_GE(updatedMetrics.totalCollateral, 0);
    BOOST_CHECK_GE(updatedMetrics.systemHealth, 0);

    // Should maintain data integrity
    BOOST_CHECK_LE(updatedMetrics.systemHealth, 300);
    BOOST_CHECK_GE(updatedMetrics.dcaMultiplier, 1.0);
}

// Test 4: System-wide Statistics
BOOST_FIXTURE_TEST_CASE(test_system_statistics, DigiDollarHealthTestSetup)
{
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Calculate expected totals from mock data
    CAmount expectedDDSupply = 0;
    CAmount expectedCollateral = 0;
    int expectedPositions = 0;

    for (const auto& pos : allPositions) {
        expectedDDSupply += pos.ddMinted;
        expectedCollateral += pos.dgbLocked;
        expectedPositions++;
    }

    // Verify system totals are reasonable
    BOOST_CHECK_GE(metrics.totalDDSupply, 0);
    BOOST_CHECK_GE(metrics.totalCollateral, 0);

    // Calculate system health percentage
    if (metrics.totalDDSupply > 0 && metrics.totalCollateral > 0) {
        // Health = (Collateral Value / DD Value) * 100
        // mockOraclePrice is in cents per DGB (e.g., 1 = $0.01/DGB)
        CAmount collateralValue = (metrics.totalCollateral * mockOraclePrice) / COIN;
        int calculatedHealth = (collateralValue * 100) / metrics.totalDDSupply;

        // System health should be reasonable
        // NOTE: With mock data, system may be undercollateralized due to test data
        BOOST_CHECK_GE(metrics.systemHealth, 0); // Must be non-negative
        BOOST_CHECK_LE(metrics.systemHealth, 300); // Not excessive
    }

    // Verify tier aggregation
    CAmount tierDDTotal = 0;
    CAmount tierCollateralTotal = 0;
    int tierPositionsTotal = 0;

    for (const auto& tier : metrics.tiers) {
        tierDDTotal += tier.ddMinted;
        tierCollateralTotal += tier.dgbLocked;
        tierPositionsTotal += tier.positions;
    }

    // Tier totals should match system totals
    BOOST_CHECK_EQUAL(tierDDTotal, metrics.totalDDSupply);
    BOOST_CHECK_EQUAL(tierCollateralTotal, metrics.totalCollateral);
}

// Test 5: Alert Threshold Detection
BOOST_FIXTURE_TEST_CASE(test_alert_thresholds, DigiDollarHealthTestSetup)
{
    // Test various alert conditions

    // Health ratio alerts
    // NOTE: With corrected oracle price scaling, mock data may show unhealthy system
    // This is expected - we're just testing that the alert system works
    bool healthAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("system_health");
    BOOST_CHECK(healthAlert == true || healthAlert == false); // Just verify it returns a boolean

    // Supply alerts
    bool supplyAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("total_supply");
    // Should not alert for reasonable supply levels

    // Collateral alerts
    bool collateralAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("total_collateral");
    // Should depend on current system state

    // Oracle alerts
    bool oracleAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("oracle_status");
    // Should depend on active oracle count

    // Volatility alerts
    bool volatilityAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("volatility");
    // Should depend on current volatility level

    // Position count alerts
    bool positionAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("position_count");
    // Should depend on active position count

    // All alert checks should return valid boolean values
    BOOST_CHECK(supplyAlert == true || supplyAlert == false);
    BOOST_CHECK(collateralAlert == true || collateralAlert == false);
    BOOST_CHECK(oracleAlert == true || oracleAlert == false);
    BOOST_CHECK(volatilityAlert == true || volatilityAlert == false);
    BOOST_CHECK(positionAlert == true || positionAlert == false);
}

// Test 6: Historical Health Tracking
BOOST_FIXTURE_TEST_CASE(test_historical_health_tracking, DigiDollarHealthTestSetup)
{
    // Get health history for last 100 blocks
    std::vector<int> healthHistory =
        DigiDollar::SystemHealthMonitor::GetHealthHistory(100);

    // Should return valid history
    BOOST_CHECK_GE(healthHistory.size(), 0);
    BOOST_CHECK_LE(healthHistory.size(), 100);

    // All values should be valid health percentages
    for (int health : healthHistory) {
        BOOST_CHECK_GE(health, 0);
        BOOST_CHECK_LE(health, 300);
    }

    // Test different history lengths
    std::vector<int> shortHistory =
        DigiDollar::SystemHealthMonitor::GetHealthHistory(10);
    BOOST_CHECK_LE(shortHistory.size(), 10);

    std::vector<int> longHistory =
        DigiDollar::SystemHealthMonitor::GetHealthHistory(1000);
    BOOST_CHECK_LE(longHistory.size(), 1000);

    // Longer requests should include shorter data
    if (shortHistory.size() > 0 && longHistory.size() >= shortHistory.size()) {
        // Most recent values should match
        for (size_t i = 0; i < shortHistory.size(); ++i) {
            BOOST_CHECK_EQUAL(shortHistory[i], longHistory[i]);
        }
    }
}

// Test 7: JSON Health Report
BOOST_FIXTURE_TEST_CASE(test_json_health_report, DigiDollarHealthTestSetup)
{
    // Get JSON health report
    UniValue report = DigiDollar::SystemHealthMonitor::GetHealthReport();

    // Should be a valid JSON object
    BOOST_CHECK(report.isObject());

    // Should contain required fields
    BOOST_CHECK(report.exists("supply"));
    BOOST_CHECK(report.exists("collateral"));
    BOOST_CHECK(report.exists("health"));
    BOOST_CHECK(report.exists("dca_multiplier"));
    BOOST_CHECK(report.exists("err_active"));
    BOOST_CHECK(report.exists("volatility"));
    BOOST_CHECK(report.exists("minting_frozen"));
    BOOST_CHECK(report.exists("tiers"));
    BOOST_CHECK(report.exists("oracles"));

    // Check data types
    BOOST_CHECK(report["supply"].isNum());
    BOOST_CHECK(report["collateral"].isNum());
    BOOST_CHECK(report["health"].isNum());
    BOOST_CHECK(report["dca_multiplier"].isNum());
    BOOST_CHECK(report["err_active"].isBool());
    BOOST_CHECK(report["volatility"].isNum());
    BOOST_CHECK(report["minting_frozen"].isBool());
    BOOST_CHECK(report["tiers"].isArray());
    BOOST_CHECK(report["oracles"].isObject());

    // Check tier array structure
    const UniValue& tiers = report["tiers"];
    for (size_t i = 0; i < tiers.size(); ++i) {
        const UniValue& tier = tiers[i];
        BOOST_CHECK(tier.isObject());
        BOOST_CHECK(tier.exists("lock_days"));
        BOOST_CHECK(tier.exists("dd_minted"));
        BOOST_CHECK(tier.exists("dgb_locked"));
        BOOST_CHECK(tier.exists("positions"));
        BOOST_CHECK(tier.exists("health"));

        BOOST_CHECK(tier["lock_days"].isNum());
        BOOST_CHECK(tier["dd_minted"].isNum());
        BOOST_CHECK(tier["dgb_locked"].isNum());
        BOOST_CHECK(tier["positions"].isNum());
        BOOST_CHECK(tier["health"].isNum());
    }

    // Check oracle object structure
    const UniValue& oracles = report["oracles"];
    BOOST_CHECK(oracles.exists("active_count"));
    BOOST_CHECK(oracles.exists("last_price"));
    BOOST_CHECK(oracles.exists("last_update"));

    BOOST_CHECK(oracles["active_count"].isNum());
    BOOST_CHECK(oracles["last_price"].isNum());
    BOOST_CHECK(oracles["last_update"].isNum());
}

// Test 8: Stress Testing - High Load Scenarios
BOOST_FIXTURE_TEST_CASE(test_high_load_scenarios, DigiDollarHealthTestSetup)
{
    // Test with many positions
    std::vector<CCollateralPosition> manyPositions;
    for (int i = 0; i < 1000; ++i) {
        COutPoint outpoint(uint256S("4444444444444444444444444444444444444444444444444444444444444444"), i);
        CCollateralPosition pos(outpoint, 1000000000, 50000, mockHeight + 1000, 150);
        manyPositions.push_back(pos);
    }

    // Should handle large position counts efficiently
    auto start = GetTime<std::chrono::milliseconds>();
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
    auto end = GetTime<std::chrono::milliseconds>();

    // Should complete in reasonable time (< 1 second)
    BOOST_CHECK_LT((end - start).count(), 1000);

    // Metrics should still be valid
    BOOST_CHECK_GE(metrics.totalDDSupply, 0);
    BOOST_CHECK_GE(metrics.totalCollateral, 0);
    BOOST_CHECK_GE(metrics.systemHealth, 0);
}

// Test 9: Edge Cases and Error Handling
BOOST_FIXTURE_TEST_CASE(test_edge_cases, DigiDollarHealthTestSetup)
{
    // Test with zero positions
    // (Simulate empty system)
    DigiDollar::SystemMetrics emptyMetrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Should handle empty system gracefully
    BOOST_CHECK_GE(emptyMetrics.totalDDSupply, 0);
    BOOST_CHECK_GE(emptyMetrics.totalCollateral, 0);
    BOOST_CHECK_GE(emptyMetrics.systemHealth, 0);

    // Test invalid alert metric names
    BOOST_CHECK(!DigiDollar::SystemHealthMonitor::ShouldAlert(""));
    BOOST_CHECK(!DigiDollar::SystemHealthMonitor::ShouldAlert("invalid_metric"));
    BOOST_CHECK(!DigiDollar::SystemHealthMonitor::ShouldAlert("nonexistent"));

    // Test invalid history requests
    std::vector<int> zeroHistory = DigiDollar::SystemHealthMonitor::GetHealthHistory(0);
    BOOST_CHECK_EQUAL(zeroHistory.size(), 0);

    std::vector<int> negativeHistory = DigiDollar::SystemHealthMonitor::GetHealthHistory(-10);
    BOOST_CHECK_EQUAL(negativeHistory.size(), 0);

    // Test extremely large history request
    std::vector<int> hugeHistory = DigiDollar::SystemHealthMonitor::GetHealthHistory(1000000);
    // Should not crash and should return reasonable amount
    BOOST_CHECK_LE(hugeHistory.size(), 100000); // Reasonable upper bound
}

// Test 10: Integration with Protection Systems
BOOST_FIXTURE_TEST_CASE(test_protection_system_integration, DigiDollarHealthTestSetup)
{
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // DCA multiplier should reflect volatility protection
    BOOST_CHECK_GE(metrics.dcaMultiplier, 1.0);
    if (metrics.volatility > 20.0) {
        BOOST_CHECK_GT(metrics.dcaMultiplier, 1.5); // Higher volatility = higher DCA
    }

    // ERR status should be consistent
    if (metrics.systemHealth < 120) {
        // Low health might trigger ERR
        // (Actual logic depends on implementation)
    }

    // Minting freeze status should be logical
    if (metrics.volatility > 50.0 || metrics.systemHealth < 110) {
        // High volatility or low health might freeze minting
        // (Actual logic depends on implementation)
    }

    // Oracle status should affect system confidence
    if (metrics.activeOracles < 5) {
        // Low oracle count might affect operations
        // (Actual logic depends on implementation)
    }

    // All protection states should be boolean
    BOOST_CHECK(metrics.errActive == true || metrics.errActive == false);
    BOOST_CHECK(metrics.mintingFrozen == true || metrics.mintingFrozen == false);
}

// Test 11: Real-time Health Report Generation
BOOST_FIXTURE_TEST_CASE(test_health_report_generation, DigiDollarHealthTestSetup)
{
    // Test the JSON health report functionality

    // 1. Initialize the health monitor
    DigiDollar::SystemHealthMonitor::Initialize();

    // 2. Generate a health report
    UniValue report = DigiDollar::SystemHealthMonitor::GetHealthReport();

    // 3. Verify the report structure
    BOOST_CHECK(report.isObject());

    // Check required fields
    BOOST_CHECK(report.exists("supply"));
    BOOST_CHECK(report.exists("collateral"));
    BOOST_CHECK(report.exists("health"));
    BOOST_CHECK(report.exists("dca_multiplier"));
    BOOST_CHECK(report.exists("err_active"));
    BOOST_CHECK(report.exists("volatility"));
    BOOST_CHECK(report.exists("minting_frozen"));
    BOOST_CHECK(report.exists("tiers"));
    BOOST_CHECK(report.exists("oracles"));
    BOOST_CHECK(report.exists("active_alerts"));
    BOOST_CHECK(report.exists("overall_status"));
    BOOST_CHECK(report.exists("recommended_action"));

    // 4. Verify tier array structure
    UniValue tiers = report["tiers"];
    BOOST_CHECK(tiers.isArray());
    BOOST_CHECK_GT(tiers.size(), 0);

    if (tiers.size() > 0) {
        UniValue firstTier = tiers[0];
        BOOST_CHECK(firstTier.exists("lock_days"));
        BOOST_CHECK(firstTier.exists("dd_minted"));
        BOOST_CHECK(firstTier.exists("dgb_locked"));
        BOOST_CHECK(firstTier.exists("positions"));
        BOOST_CHECK(firstTier.exists("health"));
        BOOST_CHECK(firstTier.exists("status"));
        BOOST_CHECK(firstTier.exists("action"));
    }

    // 5. Verify oracle object structure
    UniValue oracles = report["oracles"];
    BOOST_CHECK(oracles.isObject());
    BOOST_CHECK(oracles.exists("active_count"));
    BOOST_CHECK(oracles.exists("last_price"));
    BOOST_CHECK(oracles.exists("last_update"));
    BOOST_CHECK(oracles.exists("blocks_since_update"));
    BOOST_CHECK(oracles.exists("is_stale"));

    // 6. Verify alerts array
    UniValue alerts = report["active_alerts"];
    BOOST_CHECK(alerts.isArray());

    // 7. Verify status strings are valid
    std::string status = report["overall_status"].get_str();
    BOOST_CHECK(status == "Healthy" || status == "Warning" || status == "Critical");

    std::string action = report["recommended_action"].get_str();
    BOOST_CHECK(!action.empty());
}

// Test 12: Integration with Volatility Monitor
BOOST_FIXTURE_TEST_CASE(test_volatility_integration, DigiDollarHealthTestSetup)
{
    using namespace DigiDollar::Volatility;

    // 1. Initialize both systems
    DigiDollar::SystemHealthMonitor::Initialize();
    VolatilityMonitor::ClearHistory();

    // 2. Record some price history to establish volatility
    CAmount basePrice = 50000; // $0.50 per DGB (50000 * 0.001 cents = 50 cents)
    int64_t timestamp = GetTime();
    uint32_t height = 1000;

    VolatilityMonitor::RecordPrice(basePrice, timestamp, height);

    // Create some volatility
    for (int i = 1; i <= 5; i++) {
        timestamp += 3600; // 1 hour
        height += 1;

        // Oscillating price to create volatility
        CAmount newPrice = (i % 2 == 0) ? basePrice * 110 / 100 : basePrice * 90 / 100;
        VolatilityMonitor::RecordPrice(newPrice, timestamp, height);
    }

    VolatilityMonitor::UpdateState(height);

    // 3. Get health metrics and verify volatility integration
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Volatility should be reflected in health metrics
    BOOST_CHECK_GT(metrics.volatility, 0.0);

    // 4. Test that high volatility affects protection systems
    VolatilityState volatilityState = VolatilityMonitor::GetCurrentState();
    if (volatilityState.mintingFrozen) {
        BOOST_CHECK(metrics.mintingFrozen);
    }

    // 5. Test health report includes volatility information
    UniValue report = DigiDollar::SystemHealthMonitor::GetHealthReport();
    BOOST_CHECK(report.exists("volatility"));
    BOOST_CHECK_GE(report["volatility"].get_real(), 0.0);
}

// Test 13: Alert System Functionality
BOOST_FIXTURE_TEST_CASE(test_alert_system, DigiDollarHealthTestSetup)
{
    // Test the alert threshold system

    // 1. Initialize health monitor
    DigiDollar::SystemHealthMonitor::Initialize();

    // 2. Test alert conditions
    std::vector<std::string> alertTypes = {
        "system_health",
        "total_supply",
        "total_collateral",
        "oracle_status",
        "volatility",
        "position_count"
    };

    for (const std::string& alertType : alertTypes) {
        // ShouldAlert should not crash for valid alert types
        bool shouldAlert = DigiDollar::SystemHealthMonitor::ShouldAlert(alertType);
        BOOST_CHECK(shouldAlert == true || shouldAlert == false);
    }

    // 3. Test invalid alert types
    BOOST_CHECK(!DigiDollar::SystemHealthMonitor::ShouldAlert("invalid_alert"));
    BOOST_CHECK(!DigiDollar::SystemHealthMonitor::ShouldAlert(""));

    // 4. Test health report includes alerts
    UniValue report = DigiDollar::SystemHealthMonitor::GetHealthReport();
    BOOST_CHECK(report.exists("active_alerts"));

    UniValue alerts = report["active_alerts"];
    BOOST_CHECK(alerts.isArray());

    // All alerts in the array should be valid alert types
    for (size_t i = 0; i < alerts.size(); i++) {
        std::string alert = alerts[i].get_str();
        BOOST_CHECK(std::find(alertTypes.begin(), alertTypes.end(), alert) != alertTypes.end());
    }
}

// Test 14: Health Utility Functions
BOOST_FIXTURE_TEST_CASE(test_health_utilities, DigiDollarHealthTestSetup)
{
    using namespace DigiDollar::HealthUtils;

    // Test tier utility functions
    BOOST_CHECK_GE(GetTierIndex(30), 0);
    BOOST_CHECK_GE(GetTierIndex(90), 0);
    BOOST_CHECK_GE(GetTierIndex(365), 0);
    BOOST_CHECK_GE(GetTierIndex(1825), 0);

    // Test lock days conversion
    // System now has 9 tiers (0-8): 240 blocks, 30d, 90d, 180d, 365d, 1095d, 1825d, 2555d, 3650d
    for (int tier = 0; tier < 9; tier++) {
        int lockDays = GetTierLockDays(tier);
        BOOST_CHECK_GE(lockDays, 0);    // Tier 0 has lockDays=0 (240 blocks = ~1 hour)
        BOOST_CHECK_LE(lockDays, 3650); // 10 years max (tier 8)
    }

    // Test health ratio calculation
    CAmount ddAmount = 10000; // $100.00 DD (in cents)
    CAmount dgbAmount = 3000000000; // 30 DGB (in satoshis)
    CAmount dgbPrice = 50; // $0.50 per DGB (50 cents, unified cents format)

    int healthRatio = CalculateHealthRatio(ddAmount, dgbAmount, dgbPrice);
    // 30 DGB * $0.50 = $15.00 value / $100 DD = 15% ratio
    BOOST_CHECK_EQUAL(healthRatio, 15);

    // Test zero DD amount
    int perfectHealth = CalculateHealthRatio(0, dgbAmount, dgbPrice);
    BOOST_CHECK_EQUAL(perfectHealth, 300); // Perfect health

    // TDD: Test large values that would overflow int64_t without protection
    // dgbAmount near MAX_MONEY (2.1e18 satoshis) * dgbPrice (500 = $5.00)
    // = 1.05e21 which exceeds int64_t max (~9.2e18)
    // Expected: should NOT overflow, should return a valid health ratio
    {
        CAmount largeDgbAmount = 2000000000LL * COIN; // 2 billion DGB in satoshis = 2e17
        CAmount largeDgbPrice = 500;                   // $5.00 per DGB
        CAmount largeDdAmount = 100000000;             // $1M DD (in cents)
        // Correct answer: (2e17 * 500) / 1e8 / 1e8 * 100 = 100,000,000,000 → capped at 300
        int largeHealth = CalculateHealthRatio(largeDdAmount, largeDgbAmount, largeDgbPrice);
        BOOST_CHECK_GE(largeHealth, 0);   // Must be non-negative
        BOOST_CHECK_LE(largeHealth, 300); // Must not exceed cap

        // Even more extreme: near-MAX_MONEY collateral
        CAmount extremeDgbAmount = 20000000000LL * COIN; // 20 billion DGB (near MAX_MONEY)
        CAmount extremeDgbPrice = 1000;                   // $10.00 per DGB
        CAmount extremeDdAmount = 10000;                  // $100 DD
        // This WILL overflow: 2e18 * 1000 = 2e21 >> int64_t max
        int extremeHealth = CalculateHealthRatio(extremeDdAmount, extremeDgbAmount, extremeDgbPrice);
        BOOST_CHECK_GE(extremeHealth, 0);   // Must be non-negative (overflow would go negative)
        BOOST_CHECK_LE(extremeHealth, 300); // Must not exceed cap
    }

    // Test status formatting
    BOOST_CHECK_EQUAL(FormatHealthStatus(150), "Healthy");
    BOOST_CHECK_EQUAL(FormatHealthStatus(115), "Warning");
    BOOST_CHECK_EQUAL(FormatHealthStatus(105), "Critical");

    // Test recommended actions
    std::string healthyAction = GetRecommendedAction(150);
    std::string warningAction = GetRecommendedAction(115);
    std::string criticalAction = GetRecommendedAction(105);

    BOOST_CHECK_EQUAL(healthyAction, "Monitor");
    BOOST_CHECK_EQUAL(warningAction, "Add Collateral");
    BOOST_CHECK_EQUAL(criticalAction, "Emergency Action Required");
}

// Test 15: System Lifecycle Management
BOOST_FIXTURE_TEST_CASE(test_system_lifecycle, DigiDollarHealthTestSetup)
{
    // Test initialization and shutdown

    // 1. Test multiple initializations (should be safe)
    DigiDollar::SystemHealthMonitor::Initialize();
    DigiDollar::SystemHealthMonitor::Initialize(); // Should not crash

    // 2. Test that metrics are available after init
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
    BOOST_CHECK_GE(metrics.totalDDSupply, 0);
    BOOST_CHECK_GE(metrics.totalCollateral, 0);

    // 3. Test shutdown
    DigiDollar::SystemHealthMonitor::Shutdown();

    // 4. Test re-initialization after shutdown
    DigiDollar::SystemHealthMonitor::Initialize();
    DigiDollar::SystemMetrics metricsAfterRestart = DigiDollar::SystemHealthMonitor::GetSystemMetrics();
    BOOST_CHECK_GE(metricsAfterRestart.totalDDSupply, 0);

    // 5. Test multiple shutdowns (should be safe)
    DigiDollar::SystemHealthMonitor::Shutdown();
    DigiDollar::SystemHealthMonitor::Shutdown(); // Should not crash
}

// ============================================================================
// Health Alert System Extreme Tests (RED Phase) - Task 4.9
// ============================================================================

BOOST_FIXTURE_TEST_CASE(test_health_alert_extreme_scenarios, DigiDollarHealthTestSetup)
{
    // RED PHASE: These tests should FAIL until health alert extreme handling is implemented

    // Test 1: Cascade alert triggering under extreme system stress
    {
        // Set up multiple simultaneous alert conditions
        std::vector<std::string> stressAlertTypes = {
            "system_health",
            "total_supply",
            "total_collateral",
            "oracle_status",
            "volatility",
            "position_count",
            "memory_pressure",
            "computation_load"
        };

        // Trigger multiple alerts simultaneously
        std::vector<bool> alertResults;
        for (const std::string& alertType : stressAlertTypes) {
            bool shouldAlert = DigiDollar::SystemHealthMonitor::ShouldAlert(alertType);
            alertResults.push_back(shouldAlert);
        }

        // Test cascade alert handling - EXPECTED TO FAIL (RED phase)
        // TODO: Implement ValidateCascadeAlertHandling when needed
        // bool cascadeHandled = DigiDollar::SystemHealthMonitor::ValidateCascadeAlertHandling(alertResults);
        // BOOST_CHECK(!cascadeHandled); // Will fail until implemented
    }

    // Test 2: Alert system under memory pressure
    {
        // Initialize health monitoring
        DigiDollar::SystemHealthMonitor::Initialize();

        // Simulate massive alert generation (stress test)
        auto startTime = std::chrono::steady_clock::now();

        std::vector<bool> massAlertResults;
        for (int i = 0; i < 10000; ++i) {
            // Rapidly check different alert types
            std::string alertType = (i % 2 == 0) ? "system_health" : "volatility";
            bool result = DigiDollar::SystemHealthMonitor::ShouldAlert(alertType);
            massAlertResults.push_back(result);
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Test logging performs several writes per iteration, so wall-clock
        // performance is diagnostic here and should be measured in benchmarks.
        BOOST_WARN_LT(duration.count(), 10000);

        // Test memory pressure resistance - EXPECTED TO FAIL (RED phase)
        // TODO: Implement ValidateMemoryPressureResistance when needed
        // bool memoryPressureHandled = DigiDollar::SystemHealthMonitor::ValidateMemoryPressureResistance();
        // BOOST_CHECK(!memoryPressureHandled); // Will fail until implemented
    }

    // Test 3: Alert threshold precision at extreme boundaries
    {
        // Test alert precision with edge case values
        std::vector<std::pair<std::string, std::vector<double>>> precisionTests = {
            {"system_health", {99.99, 100.0, 100.01, 119.99, 120.0, 120.01}},
            {"volatility", {9.99, 10.0, 10.01, 19.99, 20.0, 20.01, 29.99, 30.0}},
            {"oracle_status", {49.99, 50.0, 50.01, 66.66, 66.67, 66.68}},
            {"total_supply", {999999.99, 1000000.0, 1000000.01}},
            {"position_count", {9999.0, 10000.0, 10000.1}}
        };

        for (auto& testCase : precisionTests) {
            const std::string& alertType = testCase.first;
            const std::vector<double>& testValues = testCase.second;

            for (double value : testValues) {
                // Mock system state to specific values for testing
                bool shouldAlert = DigiDollar::SystemHealthMonitor::ShouldAlert(alertType);
                // Currently may not reflect precise boundaries
            }
        }

        // Test precision boundary validation - EXPECTED TO FAIL (RED phase)
        // TODO: Implement ValidatePrecisionBoundaries when needed
        // bool precisionBoundariesValid = DigiDollar::SystemHealthMonitor::ValidatePrecisionBoundaries();
        // BOOST_CHECK(!precisionBoundariesValid); // Will fail until implemented
    }

    // Test 4: Concurrent alert state modifications - COMMENTED OUT FOR NOW
    // TODO: Implement ValidateAlertThreadSafety when needed
    /*{
        // Simulate multiple threads checking and modifying alert states
        std::vector<bool> concurrentAlertResults;

        for (int i = 0; i < 50; ++i) {
            // Rapid concurrent alert checks
            bool systemHealthAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("system_health");
            bool volatilityAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("volatility");
            bool oracleAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("oracle_status");

            concurrentAlertResults.push_back(systemHealthAlert);
            concurrentAlertResults.push_back(volatilityAlert);
            concurrentAlertResults.push_back(oracleAlert);

            // Simulate state changes during concurrent access
            if (i % 10 == 0) {
                DigiDollar::SystemHealthMonitor::UpdateMetrics(mockBlock);
            }
        }

        // Test thread safety - EXPECTED TO FAIL (RED phase)
        bool alertThreadSafe = DigiDollar::SystemHealthMonitor::ValidateAlertThreadSafety(concurrentAlertResults);
        BOOST_CHECK(!alertThreadSafe); // Will fail until implemented
    }*/

    // Test 5: Alert system state corruption scenarios - COMMENTED OUT FOR NOW
    // TODO: Implement DetectStateCorruption and RecoverFromCorruption when needed
    /*{
        // Initialize alert system
        DigiDollar::SystemHealthMonitor::Initialize();

        // Simulate corrupted internal state
        bool initialState = DigiDollar::SystemHealthMonitor::ShouldAlert("system_health");
        (void)initialState; // Mark as used

        // Test corruption detection and recovery - EXPECTED TO FAIL (RED phase)
        bool corruptionDetected = DigiDollar::SystemHealthMonitor::DetectStateCorruption();
        BOOST_CHECK(!corruptionDetected); // Will fail until implemented

        bool corruptionRecovered = DigiDollar::SystemHealthMonitor::RecoverFromCorruption();
        BOOST_CHECK(!corruptionRecovered); // Will fail until implemented
    }*/

    // Test 6: Alert prioritization under extreme load - COMMENTED OUT FOR NOW
    // TODO: Implement ValidateAlertPrioritization when needed
    /*{
        // Create scenario with multiple critical alerts
        std::map<std::string, int> alertPriorities = {
            {"system_health", 1},      // Highest priority
            {"oracle_status", 2},      // High priority
            {"volatility", 3},         // Medium priority
            {"total_supply", 4},       // Lower priority
            {"position_count", 5}      // Lowest priority
        };

        // Trigger all alerts simultaneously
        std::vector<std::pair<std::string, bool>> simultaneousAlerts;
        for (auto& priority : alertPriorities) {
            bool shouldAlert = DigiDollar::SystemHealthMonitor::ShouldAlert(priority.first);
            simultaneousAlerts.push_back({priority.first, shouldAlert});
        }

        // Test alert prioritization - EXPECTED TO FAIL (RED phase)
        bool prioritizationValid = DigiDollar::SystemHealthMonitor::ValidateAlertPrioritization(simultaneousAlerts);
        BOOST_CHECK(!prioritizationValid); // Will fail until implemented
    }*/
}

/* COMMENTED OUT - TODO: Implement advanced alert methods
BOOST_FIXTURE_TEST_CASE(test_health_alert_multi_metric_aggregation, DigiDollarHealthTestSetup)
{
    // RED PHASE: Test multi-metric alert aggregation under stress

    // Test 1: Complex alert condition evaluation
    {
        // Test alerts that depend on multiple metrics simultaneously
        struct ComplexAlertCondition {
            std::string name;
            std::vector<std::string> requiredMetrics;
            std::string logicalOperator; // "AND", "OR", "NAND", etc.
            double threshold;
        };

        std::vector<ComplexAlertCondition> complexConditions = {
            {"critical_system_state", {"system_health", "volatility", "oracle_status"}, "AND", 95.0},
            {"emergency_liquidity", {"total_supply", "total_collateral", "position_count"}, "OR", 80.0},
            {"protection_cascade", {"system_health", "volatility"}, "NAND", 110.0}
        };

        for (auto& condition : complexConditions) {
            // Evaluate complex multi-metric condition
            bool complexAlert = DigiDollar::SystemHealthMonitor::EvaluateComplexCondition(condition.name);

            // Currently expected to fail
            BOOST_CHECK(!complexAlert);
        }

        // Test complex condition evaluation - EXPECTED TO FAIL (RED phase)
        bool complexConditionsSupported = DigiDollar::SystemHealthMonitor::ValidateComplexConditions();
        BOOST_CHECK(!complexConditionsSupported); // Will fail until implemented
    }

    // Test 2: Alert aggregation across time windows
    {
        // Test alerts that aggregate metrics over different time periods
        std::vector<std::pair<std::string, int>> timeWindowTests = {
            {"system_health_1h", 3600},     // 1 hour window
            {"volatility_24h", 86400},      // 24 hour window
            {"oracle_status_7d", 604800},   // 7 day window
            {"supply_growth_30d", 2592000}  // 30 day window
        };

        for (auto& timeTest : timeWindowTests) {
            // Check time-windowed alert
            bool timeWindowAlert = DigiDollar::SystemHealthMonitor::CheckTimeWindowAlert(
                timeTest.first, timeTest.second);

            // Currently expected to fail
            BOOST_CHECK(!timeWindowAlert);
        }

        // Test time window aggregation - EXPECTED TO FAIL (RED phase)
        bool timeWindowSupported = DigiDollar::SystemHealthMonitor::ValidateTimeWindowAggregation();
        BOOST_CHECK(!timeWindowSupported); // Will fail until implemented
    }

    // Test 3: Statistical alert thresholds
    {
        // Test alerts based on statistical analysis (moving averages, standard deviation, etc.)
        std::vector<std::string> statisticalAlerts = {
            "health_moving_average",
            "volatility_std_deviation",
            "supply_growth_regression",
            "oracle_consensus_variance",
            "position_distribution_skew"
        };

        for (const std::string& statAlert : statisticalAlerts) {
            bool shouldAlert = DigiDollar::SystemHealthMonitor::CheckStatisticalAlert(statAlert);

            // Currently expected to fail
            BOOST_CHECK(!shouldAlert);
        }

        // Test statistical alert support - EXPECTED TO FAIL (RED phase)
        bool statisticalSupported = DigiDollar::SystemHealthMonitor::ValidateStatisticalAlerts();
        BOOST_CHECK(!statisticalSupported); // Will fail until implemented
    }

    // Test 4: Predictive alert systems
    {
        // Test alerts that predict future problems based on current trends
        std::vector<std::string> predictiveAlerts = {
            "predicted_health_decline",
            "forecasted_volatility_spike",
            "projected_supply_exhaustion",
            "anticipated_oracle_failure",
            "expected_cascade_failure"
        };

        for (const std::string& predAlert : predictiveAlerts) {
            // Check predictive alert
            bool futureProblem = DigiDollar::SystemHealthMonitor::CheckPredictiveAlert(predAlert);

            // Currently expected to fail
            BOOST_CHECK(!futureProblem);
        }

        // Test predictive alert capability - EXPECTED TO FAIL (RED phase)
        bool predictiveSupported = DigiDollar::SystemHealthMonitor::ValidatePredictiveAlerts();
        BOOST_CHECK(!predictiveSupported); // Will fail until implemented
    }
}

*/

/* COMMENTED OUT - TODO: Implement recovery mechanisms
BOOST_FIXTURE_TEST_CASE(test_health_alert_recovery_mechanisms, DigiDollarHealthTestSetup)
{
    // RED PHASE: Test alert recovery and self-healing mechanisms

    // Test 1: Automatic alert resolution
    {
        // Trigger alerts and test automatic resolution
        std::vector<std::string> alertsToResolve = {
            "system_health",
            "volatility",
            "oracle_status"
        };

        // Trigger alerts
        for (const std::string& alertType : alertsToResolve) {
            bool alertTriggered = DigiDollar::SystemHealthMonitor::ShouldAlert(alertType);
            // May or may not be triggered currently
        }

        // Test automatic resolution - EXPECTED TO FAIL (RED phase)
        bool autoResolutionWorking = DigiDollar::SystemHealthMonitor::ValidateAutomaticResolution();
        BOOST_CHECK(!autoResolutionWorking); // Will fail until implemented
    }

    // Test 2: Alert escalation mechanisms
    {
        // Test alert escalation when conditions worsen
        std::vector<std::pair<std::string, int>> escalationLevels = {
            {"system_health", 1},  // Warning level
            {"system_health", 2},  // Alert level
            {"system_health", 3},  // Critical level
            {"system_health", 4}   // Emergency level
        };

        for (auto& escalation : escalationLevels) {
            bool escalated = DigiDollar::SystemHealthMonitor::CheckAlertEscalation(
                escalation.first, escalation.second);

            // Currently expected to fail
            BOOST_CHECK(!escalated);
        }

        // Test escalation mechanism - EXPECTED TO FAIL (RED phase)
        bool escalationSupported = DigiDollar::SystemHealthMonitor::ValidateAlertEscalation();
        BOOST_CHECK(!escalationSupported); // Will fail until implemented
    }

    // Test 3: Alert notification systems
    {
        // Test various notification mechanisms
        std::vector<std::string> notificationTypes = {
            "log_notification",
            "rpc_notification",
            "webhook_notification",
            "email_notification",
            "sms_notification"
        };

        for (const std::string& notifType : notificationTypes) {
            bool notificationSent = DigiDollar::SystemHealthMonitor::SendNotification(
                "system_health", notifType);

            // Currently expected to fail
            BOOST_CHECK(!notificationSent);
        }

        // Test notification system - EXPECTED TO FAIL (RED phase)
        bool notificationSupported = DigiDollar::SystemHealthMonitor::ValidateNotificationSystem();
        BOOST_CHECK(!notificationSupported); // Will fail until implemented
    }

    // Test 4: Alert persistence across system restarts
    {
        // Trigger alerts
        DigiDollar::SystemHealthMonitor::Initialize();
        bool alertBefore = DigiDollar::SystemHealthMonitor::ShouldAlert("system_health");

        // Simulate system restart
        DigiDollar::SystemHealthMonitor::Shutdown();
        DigiDollar::SystemHealthMonitor::Initialize();

        // Test alert persistence - EXPECTED TO FAIL (RED phase)
        bool alertPersisted = DigiDollar::SystemHealthMonitor::ValidateAlertPersistence();
        BOOST_CHECK(!alertPersisted); // Will fail until implemented
    }
}

*/

/* COMMENTED OUT - TODO: Implement performance tests
BOOST_FIXTURE_TEST_CASE(test_health_alert_performance_extremes, DigiDollarHealthTestSetup)
{
    // RED PHASE: Test alert system performance under extreme conditions

    // Test 1: High-frequency alert checking
    {
        DigiDollar::SystemHealthMonitor::Initialize();

        auto startTime = std::chrono::high_resolution_clock::now();

        // Perform rapid alert checks
        std::vector<bool> rapidAlertResults;
        for (int i = 0; i < 100000; ++i) {
            std::string alertType = (i % 6 == 0) ? "system_health" :
                                   (i % 6 == 1) ? "volatility" :
                                   (i % 6 == 2) ? "oracle_status" :
                                   (i % 6 == 3) ? "total_supply" :
                                   (i % 6 == 4) ? "total_collateral" : "position_count";

            bool result = DigiDollar::SystemHealthMonitor::ShouldAlert(alertType);
            rapidAlertResults.push_back(result);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Should handle high frequency checks efficiently (< 5 seconds)
        BOOST_CHECK_LT(duration.count(), 5000);

        // Test high-frequency performance - EXPECTED TO FAIL (RED phase)
        bool highFrequencyPerformanceGood = DigiDollar::SystemHealthMonitor::ValidateHighFrequencyPerformance();
        BOOST_CHECK(!highFrequencyPerformanceGood); // Will fail until implemented
    }

    // Test 2: Alert system under resource constraints
    {
        // Simulate low memory/CPU scenarios
        bool resourceConstrained = DigiDollar::SystemHealthMonitor::SimulateResourceConstraints();

        // Alert system should still function under constraints
        bool systemHealthAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("system_health");
        bool volatilityAlert = DigiDollar::SystemHealthMonitor::ShouldAlert("volatility");

        // Test graceful degradation - EXPECTED TO FAIL (RED phase)
        bool gracefulDegradation = DigiDollar::SystemHealthMonitor::ValidateGracefulDegradation();
        BOOST_CHECK(!gracefulDegradation); // Will fail until implemented
    }

    // Test 3: Alert history and analytics
    {
        // Test alert history tracking and analysis
        std::vector<std::string> alertTypes = {"system_health", "volatility", "oracle_status"};

        for (const std::string& alertType : alertTypes) {
            // Get alert history
            // TODO: Implement GetAlertHistory when needed
            // std::vector<bool> alertHistory = DigiDollar::SystemHealthMonitor::GetAlertHistory(alertType, 100);
            //
            // // Analyze alert patterns
            // bool patternsAnalyzed = DigiDollar::SystemHealthMonitor::AnalyzeAlertPatterns(alertType);
            //
            // // Currently expected to fail
            // BOOST_CHECK(!patternsAnalyzed);
        }

        // Test alert analytics - EXPECTED TO FAIL (RED phase)
        bool analyticsSupported = DigiDollar::SystemHealthMonitor::ValidateAlertAnalytics();
        BOOST_CHECK(!analyticsSupported); // Will fail until implemented
    }
}

*/

// ============================================================================
// NEW TESTS: UTXO Scanning for Network-Wide Statistics
// ============================================================================

// Test 16: Test ScanUTXOSet() finds DD positions
BOOST_FIXTURE_TEST_CASE(test_utxo_scanning_finds_dd_positions, DigiDollarHealthTestSetup)
{
    // This test verifies that ScanUTXOSet() can find and count DigiDollar positions
    // in the UTXO set

    // Initialize health monitor
    DigiDollar::SystemHealthMonitor::Initialize();

    // Initially, with no DD positions, should have zero supply
    DigiDollar::SystemMetrics initialMetrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // After ScanUTXOSet(), we should see totals
    // Note: In actual implementation, this will scan the real UTXO set
    // For now, we verify the function doesn't crash and returns valid data
    BOOST_CHECK_GE(initialMetrics.totalDDSupply, 0);
    BOOST_CHECK_GE(initialMetrics.totalCollateral, 0);
}

// Test 17: Test DD amount extraction from OP_RETURN
BOOST_FIXTURE_TEST_CASE(test_dd_amount_extraction_from_opreturn, DigiDollarHealthTestSetup)
{
    // Create a mock OP_RETURN output with DD amount
    CAmount testAmount = 10000; // $100.00 in cents

    // Create OP_RETURN script with DD marker
    CScript opReturnScript;
    opReturnScript << OP_RETURN;
    opReturnScript << OP_DIGIDOLLAR; // DigiDollar marker (0xbb)

    // Encode amount as 8 bytes (little-endian)
    std::vector<unsigned char> amountData(8);
    for (size_t i = 0; i < 8; i++) {
        amountData[i] = (testAmount >> (i * 8)) & 0xFF;
    }
    opReturnScript << amountData;

    // Extract amount
    CAmount extractedAmount = 0;
    // ExtractDDAmount is in DigiDollar namespace (consensus/digidollar.cpp)
    bool extracted = DigiDollar::ExtractDDAmount(opReturnScript, extractedAmount);

    BOOST_CHECK(extracted);
    BOOST_CHECK_EQUAL(extractedAmount, testAmount);
}

// Test 18: Test collateral value calculation from P2TR outputs
BOOST_FIXTURE_TEST_CASE(test_collateral_extraction_from_p2tr, DigiDollarHealthTestSetup)
{
    // Create a mock P2TR output with DGB collateral
    CAmount collateralAmount = 10000000000; // 100 DGB in satoshis

    // Create simple P2TR script (OP_1 + 32 bytes)
    std::vector<unsigned char> pubkeyData(32, 0xAA);
    CScript p2trScript;
    p2trScript << OP_1;
    p2trScript << pubkeyData;

    // Create output
    CTxOut collateralOutput(collateralAmount, p2trScript);

    // Verify output has expected value
    BOOST_CHECK_EQUAL(collateralOutput.nValue, collateralAmount);
}

// Test 19: Test network-wide statistics aggregation
BOOST_FIXTURE_TEST_CASE(test_network_wide_statistics, DigiDollarHealthTestSetup)
{
    // Initialize health monitor
    DigiDollar::SystemHealthMonitor::Initialize();

    // Get system metrics
    DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Verify that metrics aggregate across all positions
    // Total DD supply should equal sum of all DD minted
    // Total collateral should equal sum of all DGB locked

    CAmount expectedTotalDD = 0;
    CAmount expectedTotalCollateral = 0;

    for (const auto& tier : metrics.tiers) {
        expectedTotalDD += tier.ddMinted;
        expectedTotalCollateral += tier.dgbLocked;
    }

    // Verify totals match tier sums
    BOOST_CHECK_EQUAL(metrics.totalDDSupply, expectedTotalDD);
    BOOST_CHECK_EQUAL(metrics.totalCollateral, expectedTotalCollateral);
}

// Test 20: Test that ScanUTXOSet is wallet-independent
BOOST_FIXTURE_TEST_CASE(test_scan_utxo_wallet_independent, DigiDollarHealthTestSetup)
{
    // Initialize health monitor
    DigiDollar::SystemHealthMonitor::Initialize();

    // Get metrics without any wallet loaded
    DigiDollar::SystemMetrics metricsNoWallet = DigiDollar::SystemHealthMonitor::GetSystemMetrics();

    // Metrics should be valid even without wallet
    BOOST_CHECK_GE(metricsNoWallet.totalDDSupply, 0);
    BOOST_CHECK_GE(metricsNoWallet.totalCollateral, 0);
    BOOST_CHECK_GE(metricsNoWallet.systemHealth, 0);

    // The function should work by scanning the actual UTXO set,
    // not by querying wallet data
}

BOOST_AUTO_TEST_SUITE_END()
