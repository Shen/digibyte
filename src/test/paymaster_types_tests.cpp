// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Bounded fee arithmetic, rounding, and overflow tests. */

#include <boost/test/unit_test.hpp>

#include <netbase.h>
#include <paymaster/types.h>
#include <streams.h>
#include <util/strencodings.h>

#include <limits>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_types_tests)

BOOST_AUTO_TEST_CASE(direct_endpoints_require_supported_network_and_explicit_port)
{
    BOOST_CHECK(IsSupportedPaymasterEndpoint(LookupNumeric("8.8.8.8", 12025)));
    BOOST_CHECK(IsSupportedPaymasterEndpoint(LookupNumeric("2001:4860:4860::8888", 12025)));
    CNetAddr onion;
    BOOST_REQUIRE(onion.SetSpecial("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"));
    BOOST_CHECK(IsSupportedPaymasterEndpoint(CService{onion, 12025}));
    CNetAddr i2p;
    BOOST_REQUIRE(i2p.SetSpecial("udhDrtrcetjm5sxzskjyr5ztpeszydbh4dpl3pl4utgqqw2v4jna.b32.i2p"));
    BOOST_REQUIRE(i2p.IsValid());
    BOOST_REQUIRE(i2p.IsRoutable());
    BOOST_CHECK(!IsSupportedPaymasterEndpoint(CService{i2p, 12025}));
    BOOST_CHECK(!IsSupportedPaymasterEndpoint(CService{i2p, 12025}, true));
    DataStream encoded;
    encoded << Span{ParseHex("0610fc000001000200030004000500060007")};
    CNetAddr cjdns;
    encoded >> WithParams(CNetAddr::V2, cjdns);
    BOOST_REQUIRE(cjdns.IsCJDNS());
    BOOST_REQUIRE(cjdns.IsValid());
    BOOST_CHECK(!IsSupportedPaymasterEndpoint(CService{cjdns, 12025}, true));
    for (const auto& endpoint : {CService{}, LookupNumeric("8.8.8.8", 0), CService{onion, 0},
                                LookupNumeric("192.168.1.1", 12025)}) {
        BOOST_CHECK(!IsSupportedPaymasterEndpoint(endpoint));
        BOOST_CHECK(!IsSupportedPaymasterEndpoint(endpoint, true));
    }
    for (const auto& local : {LookupNumeric("127.0.0.1", 12025), LookupNumeric("::1", 12025)}) {
        BOOST_CHECK(!IsSupportedPaymasterEndpoint(local));
        BOOST_CHECK(IsSupportedPaymasterEndpoint(local, true));
    }
}

BOOST_AUTO_TEST_CASE(recovery_fee_is_deducted_from_aggregate_inputs)
{
    BOOST_CHECK(!ComputePaymasterFee(DDCents{MAX_DD_OUTPUT_CENTS}, 10));
    const auto at_limit = ComputePaymasterRecoveryFee(DDCents{MAX_DD_OUTPUT_CENTS}, 10);
    BOOST_REQUIRE(at_limit);
    BOOST_CHECK_EQUAL(at_limit->value, 10000);
    const auto multiple_outputs = ComputePaymasterRecoveryFee(DDCents{12000000}, 10);
    BOOST_REQUIRE(multiple_outputs);
    BOOST_CHECK_EQUAL(multiple_outputs->value, 12000);
    BOOST_CHECK_EQUAL(ComputePaymasterRecoveryFee(DDCents{12000000}, 10, DDCents{99})->value, 99);
    BOOST_CHECK_EQUAL(ComputePaymasterRecoveryFee(DDCents{12000000}, 0)->value, 0);
    BOOST_CHECK_EQUAL(ComputePaymasterRecoveryFee(DDCents{1001}, 10)->value,
                      ComputePaymasterFee(DDCents{1001}, 10)->value);
    BOOST_CHECK(!ComputePaymasterRecoveryFee(DDCents{99}, 0));
    BOOST_CHECK(!ComputePaymasterRecoveryFee(DDCents{100}, 10));
    BOOST_CHECK(!ComputePaymasterRecoveryFee(DDCents{12000000}, MAX_RATE_BPS));
    BOOST_CHECK(!ComputePaymasterRecoveryFee(DDCents{std::numeric_limits<int64_t>::max()}, MAX_RATE_BPS));
    BOOST_CHECK(!ComputePaymasterRecoveryFee(DDCents{1000}, 1));
    BOOST_CHECK(!ComputePaymasterRecoveryFee(DDCents{1000}, 10, DDCents{-1}));
}

BOOST_AUTO_TEST_CASE(fee_rounds_up_to_whole_cent)
{
    const auto fee = ComputePaymasterFee(DDCents{100}, 50);
    BOOST_REQUIRE(fee.has_value());
    BOOST_CHECK_EQUAL(fee->value, 1);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(DDCents{1000}, 120)->value, 12);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(DDCents{1000}, 0)->value, 0);
}

BOOST_AUTO_TEST_CASE(user_paid_fee_obeys_provider_ceiling)
{
    const auto capped_fee =
        ComputePaymasterFee(DDCents{50000}, 50, DDCents{100});
    BOOST_REQUIRE(capped_fee);
    BOOST_CHECK_EQUAL(capped_fee->value, 100);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(DDCents{50000}, 50)->value, 250);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(DDCents{50000}, 50,
                                          DDCents{0})->value, 250);

    const auto capped_payment = ComputePaymasterPaymentFromGross(
        DDCents{50100}, 50, DDCents{100});
    BOOST_REQUIRE(capped_payment);
    BOOST_CHECK_EQUAL(capped_payment->value, 50000);
    BOOST_CHECK_EQUAL(ComputePaymasterFee(*capped_payment, 50,
                                          DDCents{100})->value, 100);
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
