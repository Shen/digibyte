// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Bounded fee arithmetic, rounding, and overflow tests. */

#include <boost/test/unit_test.hpp>

#include <paymaster/types.h>

#include <limits>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_types_tests)

BOOST_AUTO_TEST_CASE(fee_rounds_up_to_whole_cent)
{
    const auto fee = ComputePaymasterFee(DDCents{100}, 50);
    BOOST_REQUIRE(fee.has_value());
    BOOST_CHECK_EQUAL(fee->value, 1);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(DDCents{1000}, 120)->value, 12);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(DDCents{1000}, 0)->value, 0);
}

BOOST_AUTO_TEST_CASE(fee_rejects_invalid_rate_and_total)
{
    BOOST_CHECK(!ComputePaymasterFee(DDCents{0}, 10));
    BOOST_CHECK(!ComputePaymasterFee(DDCents{100}, 11));
    BOOST_CHECK(!ComputePaymasterFee(DDCents{100}, MAX_RATE_BPS + 10));
    BOOST_CHECK(!ComputePaymasterFee(DDCents{MAX_DD_OUTPUT_CENTS}, 10));
}

BOOST_AUTO_TEST_CASE(gross_amount_inversion_is_exact_and_fail_closed)
{
    const auto fifty_dd = ComputePaymasterPaymentFromGross(DDCents{5000}, 50);
    BOOST_REQUIRE(fifty_dd);
    BOOST_CHECK_EQUAL(fifty_dd->value, 4975);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(*fifty_dd, 50)->value, 25);

    BOOST_REQUIRE(ComputePaymasterPaymentFromGross(DDCents{201}, 50));
    BOOST_CHECK(!ComputePaymasterPaymentFromGross(DDCents{202}, 50));
    BOOST_REQUIRE(ComputePaymasterPaymentFromGross(DDCents{203}, 50));

    const auto sponsored =
        ComputePaymasterPaymentFromGross(DDCents{5000}, 0);
    BOOST_REQUIRE(sponsored);
    BOOST_CHECK_EQUAL(sponsored->value, 5000);

    // Regression: upper binary-search probes may exceed the global output
    // ceiling even though a lower exact solution exists.
    const auto boundary = ComputePaymasterPaymentFromGross(
        DDCents{MAX_DD_OUTPUT_CENTS}, 9990);
    BOOST_REQUIRE(boundary);
    BOOST_CHECK_EQUAL(boundary->value, 5002501);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(*boundary, 9990)->value, 4997499);

    BOOST_CHECK(!ComputePaymasterPaymentFromGross(DDCents{0}, 50));
    BOOST_CHECK(!ComputePaymasterPaymentFromGross(
        DDCents{MAX_DD_OUTPUT_CENTS + 1}, 50));
    BOOST_CHECK(!ComputePaymasterPaymentFromGross(DDCents{5000}, 51));
}

BOOST_AUTO_TEST_CASE(request_id_requires_canonical_lowercase_uuid)
{
    BOOST_CHECK(IsCanonicalRequestId("550e8400-e29b-41d4-a716-446655440000"));
    BOOST_CHECK(!IsCanonicalRequestId("550E8400-e29b-41d4-a716-446655440000"));
    BOOST_CHECK(!IsCanonicalRequestId("550e8400e29b41d4a716446655440000"));
    BOOST_CHECK(!IsCanonicalRequestId("550e8400-e29b-41d4-a716-44665544000g"));
}

BOOST_AUTO_TEST_CASE(time_arithmetic_handles_int64_boundaries)
{
    constexpr int64_t min_time{std::numeric_limits<int64_t>::min()};
    constexpr int64_t max_time{std::numeric_limits<int64_t>::max()};

    BOOST_CHECK(TimeDeltaExceeds(max_time, min_time, ANNOUNCEMENT_TTL_SECONDS));
    BOOST_CHECK(!TimeDeltaExceeds(min_time, max_time, ANNOUNCEMENT_TTL_SECONDS));
    BOOST_CHECK(!TimeDeltaExceeds(max_time, max_time - 60, 60));
    BOOST_CHECK(TimeDeltaExceeds(max_time, max_time - 60, 59));
    BOOST_CHECK(TimeDeltaExceeds(1, 0, -1));

    BOOST_CHECK_EQUAL(SaturatingAddSeconds(max_time - 1, 60), max_time);
    BOOST_CHECK_EQUAL(SaturatingAddSeconds(min_time + 1, -60), min_time);
    BOOST_CHECK_EQUAL(SaturatingAddSeconds(100, 60), 160);
}

BOOST_AUTO_TEST_SUITE_END()
