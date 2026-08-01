// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <consensus/digidollar.h>
#include <consensus/dca.h>
#include <digidollar/health.h>
#include <kernel/chainparams.h>
#include <chainparams.h>
#include <test/util/setup_common.h>
#include <chrono>
#include <cmath>
#include <limits>

using namespace DigiDollar;
using namespace DigiDollar::DCA;

BOOST_FIXTURE_TEST_SUITE(digidollar_dca_tests, BasicTestingSetup)

// ============================================================================
// System Health Calculation Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(system_health_calculation_basic)
{
    // Test basic system health calculation
    CAmount totalCollateral = 150000000 * COIN;  // 150M DGB
    CAmount totalDD = 50000000;                  // 50M DD (in cents, $500k)
    CAmount oraclePrice = 3333;                  // $0.03333 per DGB

    // System health = (totalCollateral * price) / totalDD * 100
    // = (150M * $0.03333) / $500k * 100 = $4,999,500 / $500k * 100 = 999.9% ≈ 999%
    int health = DynamicCollateralAdjustment::CalculateSystemHealth(
        totalCollateral, totalDD, oraclePrice);

    BOOST_CHECK_EQUAL(health, 999);
}

BOOST_AUTO_TEST_CASE(system_health_calculation_edge_cases)
{
    CAmount oraclePrice = 5000; // $0.05 per DGB

    // Test zero DD supply (system just starting)
    {
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            1000 * COIN, 0, oraclePrice);
        BOOST_CHECK_EQUAL(health, 30000); // Maximum health when no DD issued
    }

    // Test zero collateral (emergency state)
    {
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            0, 100000, oraclePrice);
        BOOST_CHECK_EQUAL(health, 0); // Zero health
    }

    // Test zero oracle price (price feed failure)
    {
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            1000 * COIN, 100000, 0);
        BOOST_CHECK_EQUAL(health, 0); // System cannot function without price
    }
}

BOOST_AUTO_TEST_CASE(system_health_various_scenarios)
{
    CAmount oraclePrice = 4000; // $0.04 per DGB

    // Healthy system (250% collateralization)
    {
        CAmount collateral = 50000000 * COIN;  // 50M DGB
        CAmount totalDD = 80000000;            // 80M DD cents ($800k)
        // Health = (50M * $0.04) / $800k * 100 = $2M / $800k * 100 = 250%
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            collateral, totalDD, oraclePrice);
        BOOST_CHECK_EQUAL(health, 250);
    }

    // Warning system (130% collateralization)
    {
        CAmount collateral = 32500000 * COIN;  // 32.5M DGB
        CAmount totalDD = 100000000;           // 100M DD cents ($1M)
        // Health = (32.5M * $0.04) / $1M * 100 = $1.3M / $1M * 100 = 130%
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            collateral, totalDD, oraclePrice);
        BOOST_CHECK_EQUAL(health, 130);
    }

    // Critical system (110% collateralization)
    {
        CAmount collateral = 27500000 * COIN;  // 27.5M DGB
        CAmount totalDD = 100000000;           // 100M DD cents ($1M)
        // Health = (27.5M * $0.04) / $1M * 100 = $1.1M / $1M * 100 = 110%
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            collateral, totalDD, oraclePrice);
        BOOST_CHECK_EQUAL(health, 110);
    }

    // Emergency system (90% collateralization)
    {
        CAmount collateral = 22500000 * COIN;  // 22.5M DGB
        CAmount totalDD = 100000000;           // 100M DD cents ($1M)
        // Health = (22.5M * $0.04) / $1M * 100 = $900k / $1M * 100 = 90%
        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            collateral, totalDD, oraclePrice);
        BOOST_CHECK_EQUAL(health, 90);
    }
}

// ============================================================================
// DCA Multiplier Calculation Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(dca_multiplier_healthy_system)
{
    // Healthy system (>150% health) should have 1.0x multiplier
    double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(200);
    BOOST_CHECK_EQUAL(multiplier, 1.0);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(300);
    BOOST_CHECK_EQUAL(multiplier, 1.0);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(151);
    BOOST_CHECK_EQUAL(multiplier, 1.0);
}

BOOST_AUTO_TEST_CASE(dca_multiplier_warning_system)
{
    // Warning system (120-149% health) should have 1.25x multiplier
    double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(149);
    BOOST_CHECK_EQUAL(multiplier, 1.25);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(135);
    BOOST_CHECK_EQUAL(multiplier, 1.25);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(120);
    BOOST_CHECK_EQUAL(multiplier, 1.25);
}

BOOST_AUTO_TEST_CASE(dca_multiplier_critical_system)
{
    // Critical system (110-119% health) should have 1.5x multiplier
    double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(119);
    BOOST_CHECK_EQUAL(multiplier, 1.5);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(110);
    BOOST_CHECK_EQUAL(multiplier, 1.5);
}

BOOST_AUTO_TEST_CASE(dca_multiplier_emergency_system)
{
    // Emergency floor (<110% health) should have 2.0x multiplier
    double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(109);
    BOOST_CHECK_EQUAL(multiplier, 2.0);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(99);
    BOOST_CHECK_EQUAL(multiplier, 2.0);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(50);
    BOOST_CHECK_EQUAL(multiplier, 2.0);

    multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(0);
    BOOST_CHECK_EQUAL(multiplier, 2.0);
}

BOOST_AUTO_TEST_CASE(dca_multiplier_boundary_conditions)
{
    // Test exact boundary conditions
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(150), 1.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(149), 1.25);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(120), 1.25);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(119), 1.5);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(110), 1.5);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(109), 2.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(100), 2.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(99), 2.0);
}

// ============================================================================
// DCA Application Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(apply_dca_to_base_ratios)
{
    // Test applying DCA to different base collateral ratios

    // Healthy system (1.0x multiplier)
    int adjustedRatio = DynamicCollateralAdjustment::ApplyDCA(300, 200); // 300% base, 200% health
    BOOST_CHECK_EQUAL(adjustedRatio, 300); // No change

    // Warning system (1.25x multiplier)
    adjustedRatio = DynamicCollateralAdjustment::ApplyDCA(300, 130); // 300% base, 130% health
    BOOST_CHECK_EQUAL(adjustedRatio, 375); // 300% * 1.25 = 375%

    // Critical system (1.5x multiplier)
    adjustedRatio = DynamicCollateralAdjustment::ApplyDCA(300, 110); // 300% base, 110% health
    BOOST_CHECK_EQUAL(adjustedRatio, 450); // 300% * 1.5 = 450%

    // Emergency system (2.0x multiplier)
    adjustedRatio = DynamicCollateralAdjustment::ApplyDCA(300, 90); // 300% base, 90% health
    BOOST_CHECK_EQUAL(adjustedRatio, 600); // 300% * 2.0 = 600%
}

BOOST_AUTO_TEST_CASE(apply_dca_all_lock_tiers)
{
    // Test DCA application across all collateral ratio tiers
    std::vector<int> baseRatios = {500, 400, 350, 300, 250, 225, 212, 200}; // All 8 tiers
    int systemHealth = 110; // Critical system (1.5x multiplier)

    for (int baseRatio : baseRatios) {
        int adjustedRatio = DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemHealth);
        int expectedRatio = static_cast<int>(std::ceil(baseRatio * 1.5)); // 1.5x multiplier for critical system
        BOOST_CHECK_EQUAL(adjustedRatio, expectedRatio);
    }
}

// ============================================================================
// Health Tier Information Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(get_current_tier_information)
{
    // Test getting tier information for different health levels

    // Healthy tier
    auto tier = DynamicCollateralAdjustment::GetCurrentTier(200);
    BOOST_CHECK_EQUAL(tier.minCollateral, 150);
    BOOST_CHECK_EQUAL(tier.maxCollateral, 30000);
    BOOST_CHECK_EQUAL(tier.multiplier, 1.0);
    BOOST_CHECK_EQUAL(tier.status, "healthy");

    // Warning tier
    tier = DynamicCollateralAdjustment::GetCurrentTier(130);
    BOOST_CHECK_EQUAL(tier.minCollateral, 120);
    BOOST_CHECK_EQUAL(tier.maxCollateral, 149);
    BOOST_CHECK_EQUAL(tier.multiplier, 1.25);
    BOOST_CHECK_EQUAL(tier.status, "warning");

    // Critical tier
    tier = DynamicCollateralAdjustment::GetCurrentTier(110);
    BOOST_CHECK_EQUAL(tier.minCollateral, 110);
    BOOST_CHECK_EQUAL(tier.maxCollateral, 119);
    BOOST_CHECK_EQUAL(tier.multiplier, 1.5);
    BOOST_CHECK_EQUAL(tier.status, "critical");

    // Emergency tier
    tier = DynamicCollateralAdjustment::GetCurrentTier(50);
    BOOST_CHECK_EQUAL(tier.minCollateral, 0);
    BOOST_CHECK_EQUAL(tier.maxCollateral, 109);
    BOOST_CHECK_EQUAL(tier.multiplier, 2.0);
    BOOST_CHECK_EQUAL(tier.status, "emergency");
}

// ============================================================================
// Emergency State Detection Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(emergency_state_detection)
{
    // Test emergency state detection
    BOOST_CHECK(!DynamicCollateralAdjustment::IsSystemEmergency(200)); // Healthy
    BOOST_CHECK(!DynamicCollateralAdjustment::IsSystemEmergency(150)); // Warning
    BOOST_CHECK(!DynamicCollateralAdjustment::IsSystemEmergency(130)); // Warning
    BOOST_CHECK(!DynamicCollateralAdjustment::IsSystemEmergency(120)); // Warning
    BOOST_CHECK(!DynamicCollateralAdjustment::IsSystemEmergency(110)); // Critical
    BOOST_CHECK(!DynamicCollateralAdjustment::IsSystemEmergency(100)); // Critical

    BOOST_CHECK(DynamicCollateralAdjustment::IsSystemEmergency(99));   // Emergency
    BOOST_CHECK(DynamicCollateralAdjustment::IsSystemEmergency(50));   // Emergency
    BOOST_CHECK(DynamicCollateralAdjustment::IsSystemEmergency(0));    // Emergency
}

// ============================================================================
// Real-time System State Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(total_system_collateral_calculation)
{
    // Test total system collateral calculation
    // This test will be implemented once we have UTXO access
    CAmount totalCollateral = DynamicCollateralAdjustment::GetTotalSystemCollateral();

    // For now, just verify the function exists and returns a reasonable value
    BOOST_CHECK(totalCollateral >= 0);
}

BOOST_AUTO_TEST_CASE(total_dd_supply_calculation)
{
    // Test total DigiDollar supply calculation
    // This test will be implemented once we have chain state access
    CAmount totalDD = DynamicCollateralAdjustment::GetTotalDDSupply();

    // For now, just verify the function exists and returns a reasonable value
    BOOST_CHECK(totalDD >= 0);
}

// ============================================================================
// Integration with Existing Collateral System Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(integration_with_consensus_params)
{
    // Test integration with existing consensus parameters
    const CChainParams& params = Params();

    // Verify DCA works with existing collateral ratios
    std::map<int64_t, int> collateralRatios = {
        {240, 1000},                 // 1 hour: 1000% (testing/onboarding)
        {30 * 24 * 60 * 4, 500},     // 30 days: 500%
        {90 * 24 * 60 * 4, 400},     // 3 months: 400%
        {180 * 24 * 60 * 4, 350},    // 6 months: 350%
        {365 * 24 * 60 * 4, 300},    // 1 year: 300%
        {3 * 365 * 24 * 60 * 4, 250}, // 3 years: 250%
        {5 * 365 * 24 * 60 * 4, 225}, // 5 years: 225%
        {7 * 365 * 24 * 60 * 4, 212}, // 7 years: 212%
        {10 * 365 * 24 * 60 * 4, 200} // 10 years: 200%
    };

    int systemHealth = 110; // Critical system
    double expectedMultiplier = 1.5;

    for (const auto& [lockBlocks, baseRatio] : collateralRatios) {
        int adjustedRatio = DynamicCollateralAdjustment::ApplyDCA(baseRatio, systemHealth);
        int expectedRatio = static_cast<int>(std::ceil(baseRatio * expectedMultiplier));
        BOOST_CHECK_EQUAL(adjustedRatio, expectedRatio);
    }
}

// ============================================================================
// Performance and Scalability Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(performance_health_calculation)
{
    // Exercise health calculation repeatedly with large numbers. Wall-clock
    // performance is benchmark territory and varies substantially in Debug
    // builds, so this unit test verifies the deterministic result instead.
    CAmount largeCollateral = 1000000000 * COIN;  // 1B DGB
    CAmount largeDD = 100000000000;               // 100B DD (1T USD)
    CAmount oraclePrice = 10000;                  // $0.10 per DGB
    constexpr int iterations = 1000;
    constexpr int expected_health = 10;
    int64_t health_sum{0};

    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        health_sum += DynamicCollateralAdjustment::CalculateSystemHealth(
            largeCollateral, largeDD, oraclePrice);
    }

    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    BOOST_CHECK_EQUAL(health_sum, static_cast<int64_t>(expected_health) * iterations);
    BOOST_WARN_LT(duration.count(), 5000000);
}

BOOST_AUTO_TEST_CASE(numerical_stability)
{
    // Test numerical stability with edge case values

    // Very small values
    {
        CAmount collateral = 1;  // 1 satoshi
        CAmount totalDD = 1;     // 1 cent
        CAmount price = 1;       // 1 cent per DGB

        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            collateral, totalDD, price);
        BOOST_CHECK(health >= 0);
    }

    // Very large values (near overflow limits)
    {
        CAmount collateral = 2000000000 * COIN;  // 2B DGB (near max supply)
        CAmount totalDD = 2100000000000;         // 21T DD
        CAmount price = 100000;                  // $1.00 per DGB

        int health = DynamicCollateralAdjustment::CalculateSystemHealth(
            collateral, totalDD, price);
        BOOST_CHECK(health >= 0);
        BOOST_CHECK(health < 1000000); // Reasonable upper bound
    }
}

// ============================================================================
// Future Enhancement Hooks Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(gradual_transition_hooks)
{
    // Test hooks for future gradual transition implementation
    // Currently tests boundary conditions, can be extended for smooth transitions

    // Test transitions at tier boundaries
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(150), 1.0);
    BOOST_CHECK_EQUAL(DynamicCollateralAdjustment::GetDCAMultiplier(149), 1.25);

    // Future: implement smooth transitions between tiers
    // For example: health 150.5 could give multiplier 1.1 instead of hard 1.25
}

BOOST_AUTO_TEST_CASE(emergency_recovery_hooks)
{
    // Test hooks for emergency recovery mechanisms
    BOOST_CHECK(DynamicCollateralAdjustment::IsSystemEmergency(99));

    // Future: implement recovery mechanisms like:
    // - Mint restrictions during emergency
    // - Gradual multiplier reduction as system recovers
    // - Emergency redemption ratio adjustments
}

// ============================================================================
// DCA Extreme Scenarios Tests (RED Phase) - Task 4.9
// ============================================================================

BOOST_AUTO_TEST_CASE(test_dca_extreme_scenarios)
{
    // RED PHASE: These tests should FAIL until implementation is complete

    // Test 1: Rapid health tier changes (10% to 90% to 150% in short time)
    {
        // Start healthy
        int health1 = 150;
        double multiplier1 = DynamicCollateralAdjustment::GetDCAMultiplier(health1);
        BOOST_CHECK_EQUAL(multiplier1, 1.0);

        // Drop to emergency
        int health2 = 10;
        double multiplier2 = DynamicCollateralAdjustment::GetDCAMultiplier(health2);
        BOOST_CHECK_EQUAL(multiplier2, 2.0);

        // Quick recovery to healthy
        int health3 = 150;
        double multiplier3 = DynamicCollateralAdjustment::GetDCAMultiplier(health3);
        BOOST_CHECK_EQUAL(multiplier3, 1.0);

        // Test rapid transition handling - GREEN phase implemented
        bool transitionHandled = DynamicCollateralAdjustment::HandleRapidTransition(health1, health2, health3);
        BOOST_CHECK(transitionHandled); // GREEN phase: now validates transitions
    }

    // Test 2: System at absolute limits (0% and 30000% health)
    {
        // Zero health scenario
        int zeroHealth = 0;
        double zeroMultiplier = DynamicCollateralAdjustment::GetDCAMultiplier(zeroHealth);
        BOOST_CHECK_EQUAL(zeroMultiplier, 2.0); // Maximum multiplier

        // Extreme overcollateralization
        int extremeHealth = 30000; // 300x collateralized
        double extremeMultiplier = DynamicCollateralAdjustment::GetDCAMultiplier(extremeHealth);
        BOOST_CHECK_EQUAL(extremeMultiplier, 1.0); // Minimum multiplier

        // Test extreme value handling - GREEN phase implemented
        bool extremesHandled = DynamicCollateralAdjustment::ValidateExtremeValues(zeroHealth, extremeHealth);
        BOOST_CHECK(extremesHandled); // GREEN phase: now validates extreme values
    }

    // Test 3: Multiplier calculations with floating point precision
    {
        // Test boundary conditions with precision
        std::vector<std::pair<int, double>> precisionTests = {
            {150, 1.0}, {149, 1.25}, // Boundary between warning and healthy
            {120, 1.25}, {119, 1.5}, // Boundary between warning and critical
            {110, 1.5}, {109, 2.0},  // Boundary between critical and emergency floor
            {100, 2.0}, {99, 2.0}
        };

        for (auto& test : precisionTests) {
            double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(test.first);
            BOOST_CHECK_EQUAL(multiplier, test.second);
        }

        // Test precision at extreme boundaries - GREEN phase implemented
        bool precisionValid = DynamicCollateralAdjustment::ValidateMultiplierPrecision();
        BOOST_CHECK(precisionValid); // GREEN phase: now validates precision
    }

    // Test 4: DCA with maximum possible collateral ratios
    {
        // Test with maximum base ratio (500%) and maximum multiplier (2.0x)
        int maxBaseRatio = 500;
        int emergencyHealth = 50;
        int maxAdjustedRatio = DynamicCollateralAdjustment::ApplyDCA(maxBaseRatio, emergencyHealth);
        BOOST_CHECK_EQUAL(maxAdjustedRatio, 1000); // 500% * 2.0 = 1000%

        // Test overflow protection - GREEN phase implemented
        bool overflowProtected = DynamicCollateralAdjustment::PreventIntegerOverflow(maxBaseRatio, 2.0);
        BOOST_CHECK(overflowProtected); // GREEN phase: now prevents overflow
    }

    // Test 5: Tier transition race conditions
    {
        // Simulate concurrent health updates during tier calculation
        std::vector<int> rapidHealthChanges = {150, 119, 100, 99, 120, 151};

        for (int health : rapidHealthChanges) {
            auto tier = DynamicCollateralAdjustment::GetCurrentTier(health);
            // Basic validation that tier is returned
            BOOST_CHECK(!tier.status.empty());
        }

        // Test race condition handling - GREEN phase implemented
        bool raceConditionHandled = DynamicCollateralAdjustment::HandleConcurrentUpdates(rapidHealthChanges);
        BOOST_CHECK(raceConditionHandled); // GREEN phase: now handles concurrent updates
    }

    // Test 6: Memory pressure under extreme scenarios
    {
        // Test DCA calculation performance under stress
        auto startTime = std::chrono::steady_clock::now();

        for (int i = 0; i < 10000; ++i) {
            int health = i % 300; // Cycle through all possible health values
            double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(health);
            (void)multiplier; // Suppress unused variable warning
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Wall-clock performance depends heavily on enabled test logging and
        // should be measured in benchmarks. Keep this as a diagnostic warning while the
        // loop continues to exercise the full health range.
        BOOST_WARN_LT(duration.count(), 5000);

        // Test memory stability under load - GREEN phase implemented
        bool memoryStable = DynamicCollateralAdjustment::VerifyMemoryStability();
        BOOST_CHECK(memoryStable); // GREEN phase: now verifies memory stability
    }

    // Test 7: Edge case calculations
    {
        // Test with negative health (error condition)
        int negativeHealth = -10;
        double negativeMultiplier = DynamicCollateralAdjustment::GetDCAMultiplier(negativeHealth);
        BOOST_CHECK_EQUAL(negativeMultiplier, 2.0); // Should treat as emergency

        // Test invalid error handling - GREEN phase implemented
        bool errorHandlingValid = DynamicCollateralAdjustment::ValidateErrorHandling(negativeHealth);
        BOOST_CHECK(errorHandlingValid); // GREEN phase: now validates error handling
    }
}

BOOST_AUTO_TEST_CASE(test_dca_system_state_transitions)
{
    // RED PHASE: Test state transitions between DCA tiers

    // Test 1: State persistence during transitions
    {
        // Initialize system in healthy state
        int currentHealth = 200;
        auto initialTier = DynamicCollateralAdjustment::GetCurrentTier(currentHealth);

        // Transition to critical
        currentHealth = 110;
        auto criticalTier = DynamicCollateralAdjustment::GetCurrentTier(currentHealth);

        // Verify state change tracking - GREEN phase implemented
        bool stateTransitionTracked = DynamicCollateralAdjustment::IsStateTransitionTracked(
            initialTier.status, criticalTier.status);
        BOOST_CHECK(stateTransitionTracked); // GREEN phase: now tracks state transitions
    }

    // Test 2: Hysteresis in tier transitions
    {
        // Test that rapid bouncing between tiers is handled smoothly
        std::vector<int> bouncingHealth = {120, 119, 120, 119, 120};

        std::vector<double> multipliers;
        for (int health : bouncingHealth) {
            multipliers.push_back(DynamicCollateralAdjustment::GetDCAMultiplier(health));
        }

        // Test hysteresis implementation - GREEN phase implemented
        bool hysteresisImplemented = DynamicCollateralAdjustment::HasHysteresis(multipliers);
        BOOST_CHECK(hysteresisImplemented); // GREEN phase: now validates hysteresis behavior
    }

    // Test 3: System recovery tracking
    {
        // Simulate system recovery from emergency to healthy
        std::vector<int> recoveryPath = {50, 80, 110, 130, 160};

        for (int health : recoveryPath) {
            double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(health);
            // Verify decreasing multiplier as health improves
            if (health >= 151) BOOST_CHECK_EQUAL(multiplier, 1.0);
            else if (health >= 120) BOOST_CHECK_EQUAL(multiplier, 1.25);
            else if (health >= 110) BOOST_CHECK_EQUAL(multiplier, 1.5);
            else BOOST_CHECK_EQUAL(multiplier, 2.0);
        }

        // Test recovery metrics tracking - GREEN phase implemented
        bool recoveryTracked = DynamicCollateralAdjustment::TrackSystemRecovery(recoveryPath);
        BOOST_CHECK(recoveryTracked); // GREEN phase: now tracks recovery
    }
}

BOOST_AUTO_TEST_CASE(test_dca_integration_stress)
{
    // RED PHASE: Test DCA integration under stress conditions

    // Test 1: Concurrent position calculations
    {
        // Simulate many positions being calculated simultaneously
        std::vector<int> baseRatios = {500, 400, 350, 300, 250, 225, 212, 200};
        int stressHealth = 105; // Emergency floor

        std::vector<int> adjustedRatios;
        for (int baseRatio : baseRatios) {
            adjustedRatios.push_back(DynamicCollateralAdjustment::ApplyDCA(baseRatio, stressHealth));
        }

        // Verify all calculations completed correctly
        for (size_t i = 0; i < baseRatios.size(); ++i) {
            int expected = static_cast<int>(std::ceil(baseRatios[i] * 2.0)); // Emergency floor multiplier
            BOOST_CHECK_EQUAL(adjustedRatios[i], expected);
        }

        // Test concurrent calculation safety - GREEN phase implemented
        bool concurrentSafe = DynamicCollateralAdjustment::ValidateConcurrentCalculations(adjustedRatios);
        BOOST_CHECK(concurrentSafe); // GREEN phase: now validates concurrent calculations
    }

    // Test 2: Resource exhaustion scenarios
    {
        // Test behavior when system resources are limited
        bool resourceLimited = DynamicCollateralAdjustment::SimulateResourceExhaustion();

        // DCA should still function under resource pressure
        int health = 110;
        double multiplier = DynamicCollateralAdjustment::GetDCAMultiplier(health);
        BOOST_CHECK_EQUAL(multiplier, 1.5);

        // Test graceful degradation - GREEN phase implemented
        BOOST_CHECK(resourceLimited); // GREEN phase: now handles resource exhaustion
    }
}

// DD-FINAL-004 / AR-CONSENSUS-1 residual regression: ApplyDCA must use the
// deterministic health supplied by the caller (recomputed from the block's
// committed oracle price), and must NOT be perturbed by the RPC-display cache
// (hasCanonicalHealth/systemHealth set by getdigidollarstats from the node-local
// last-mint price). Before the fix, ResolveCanonicalHealthForDCA returned the
// cached systemHealth and flagged staleHealth whenever the supplied (block-price)
// health differed, making ApplyDCA fail closed to INT_MAX on whichever nodes
// happened to serve a stats RPC between blocks -> consensus divergence.
BOOST_AUTO_TEST_CASE(dd_final_004_applydca_ignores_rpc_health_cache)
{
    // Poison the shared metrics cache exactly as the RPC display path would: a
    // "canonical" health in the critical DCA band (1.5x) derived from a stale price.
    SystemMetrics poisoned;
    poisoned.totalDDSupply = 100000;       // active supply so the old branch armed
    poisoned.totalCollateral = 5000000000; // non-zero
    poisoned.systemHealth = 11500;         // 115% -> critical band (1.5x)
    poisoned.hasCanonicalHealth = true;    // only ever set by UpdateTierMetrics (RPC)
    SystemHealthMonitor::SetMetricsForTesting(poisoned);

    const int baseRatio = 200; // 200%
    // Supply the deterministic block-price health in the HEALTHY band (1.0x).
    const int blockPriceHealth = 30000; // 300% -> healthy (1.0x multiplier)

    // With the fix ApplyDCA trusts the supplied health (1.0x) regardless of the
    // poisoned cache: 200% * 1.0x = 200%. Before the fix it returned INT_MAX
    // (failed closed) because supplied 300% != cached 115%.
    const int effective = DynamicCollateralAdjustment::ApplyDCA(baseRatio, blockPriceHealth);
    BOOST_CHECK_EQUAL(effective, 200);
    BOOST_CHECK(effective != std::numeric_limits<int>::max());

    // And a node that never served the RPC (clean cache) computes the same value
    // for the same supplied health -> deterministic across nodes.
    SystemHealthMonitor::ResetMetrics();
    const int effectiveClean = DynamicCollateralAdjustment::ApplyDCA(baseRatio, blockPriceHealth);
    BOOST_CHECK_EQUAL(effectiveClean, effective);

    SystemHealthMonitor::ResetMetrics();
}

BOOST_AUTO_TEST_SUITE_END()
