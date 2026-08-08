// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Saturating, advisory provider-reliability accounting tests. */

#include <boost/test/unit_test.hpp>

#include <paymaster/reputation.h>

#include <limits>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_reputation_tests)

BOOST_AUTO_TEST_CASE(outcomes_are_bucketed_without_penalizing_neutral_events)
{
    PaymasterReliabilityRecord record;
    record.provider_id = uint256::ONE;
    std::string error;
    constexpr int64_t day = 20000;
    const int64_t start = day * 86400 + 100;
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::SUCCESS,
                                          start, 100, error));
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::NEUTRAL_FAILURE,
                                          start + 1, 0, error));
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::AVAILABILITY_TIMEOUT,
                                          start + 2, 0, error));
    ReliabilitySummary summary = SummarizeReliability(record, start + 3);
    BOOST_CHECK_EQUAL(summary.successful_attempts, 1U);
    BOOST_CHECK_EQUAL(summary.provider_failures, 0U);
    BOOST_CHECK_EQUAL(summary.neutral_failures, 1U);
    BOOST_CHECK_EQUAL(summary.availability_timeouts, 1U);
    BOOST_CHECK_EQUAL(summary.success_rate_basis_points, 10000U);
    BOOST_CHECK(!summary.sufficient_data);
    BOOST_CHECK_EQUAL(record.latency_ewma_ms, 100);
    BOOST_CHECK_EQUAL(record.cooldown_until, start + 2 + AVAILABILITY_COOLDOWN_SECONDS);
}

BOOST_AUTO_TEST_CASE(provider_failures_back_off_and_success_resets_the_streak)
{
    PaymasterReliabilityRecord record;
    record.provider_id = uint256::ONE;
    std::string error;
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::PROVIDER_FAILURE, 1000, 0, error));
    BOOST_CHECK_EQUAL(record.cooldown_until, 1060);
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::PROVIDER_FAILURE, 1061, 0, error));
    BOOST_CHECK_EQUAL(record.cooldown_until, 1181);
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::SUCCESS, 1182, 200, error));
    BOOST_CHECK_EQUAL(record.consecutive_provider_failures, 0U);
    BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::SUCCESS, 1183, 100, error));
    BOOST_CHECK_EQUAL(record.latency_ewma_ms, 175);
}

BOOST_AUTO_TEST_CASE(only_the_latest_thirty_utc_days_are_summarized)
{
    PaymasterReliabilityRecord record;
    record.provider_id = uint256::ONE;
    std::string error;
    for (int day = 1; day <= 31; ++day) {
        BOOST_REQUIRE(ApplyReliabilityOutcome(record, ReliabilityOutcome::SUCCESS,
                                              static_cast<int64_t>(day) * 86400, 1, error));
    }
    const ReliabilitySummary summary = SummarizeReliability(record, 31LL * 86400);
    BOOST_CHECK_EQUAL(summary.successful_attempts, 30U);
    BOOST_CHECK(summary.sufficient_data);
}

BOOST_AUTO_TEST_CASE(latency_ewma_does_not_overflow)
{
    PaymasterReliabilityRecord record;
    record.provider_id = uint256::ONE;
    std::string error;
    const int64_t maximum = std::numeric_limits<int64_t>::max();

    BOOST_REQUIRE(ApplyReliabilityOutcome(
        record, ReliabilityOutcome::SUCCESS, 1000, maximum, error));
    BOOST_CHECK_EQUAL(record.latency_ewma_ms, maximum);
    BOOST_REQUIRE(ApplyReliabilityOutcome(
        record, ReliabilityOutcome::SUCCESS, 1001, maximum, error));
    BOOST_CHECK_EQUAL(record.latency_ewma_ms, maximum);

    BOOST_REQUIRE(ApplyReliabilityOutcome(
        record, ReliabilityOutcome::SUCCESS, 1002, 0, error));
    BOOST_CHECK_EQUAL(
        record.latency_ewma_ms, maximum - maximum / 4 - 1);
}

BOOST_AUTO_TEST_SUITE_END()
