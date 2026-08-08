// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Client selection, fee caps, and session-transition invariants. */

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <netbase.h>
#include <paymaster/client.h>
#include <paymaster/provider.h>
#include <script/standard.h>
#include <test/util/setup_common.h>

using namespace DigiDollar::Paymaster;

namespace {
Announcement MakeAnnouncement(CKey& key, uint256 offer_id, FundingModel model,
                                uint32_t rate, int64_t now)
{
    key.MakeNewKey(true);
    Announcement announcement;
    announcement.identity_key = XOnlyPubKey{key.GetPubKey()};
    announcement.display_name = "Provider";
    announcement.sequence = 1;
    announcement.created_at = now;
    announcement.expires_at = now + 60;
    announcement.offers.push_back(OfferTerms{offer_id, uint256::ONE, model,
                                              SponsorshipScope::PUBLIC, rate,
                                              DDCents{100}, DDCents{100000}});
    return announcement;
}

CScript TaprootScript(const CKey& key)
{
    return GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_client_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(privacy_profile_routes_fail_closed)
{
    const CService clearnet{LookupNumeric("8.8.8.8", 12024)};
    CNetAddr onion_address;
    BOOST_REQUIRE(onion_address.SetSpecial(
        "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"));
    const CService onion{onion_address, 12024};
    const CService invalid;
    const CService local{LookupNumeric("127.0.0.1", 12024)};
    const CService private_network{LookupNumeric("192.168.1.1", 12024)};

    BOOST_CHECK(IsEndpointAllowedForPrivacyProfile(
        clearnet, PrivacyProfile::STANDARD));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        clearnet, PrivacyProfile::HIGH));
    BOOST_CHECK(IsEndpointAllowedForPrivacyProfile(
        onion, PrivacyProfile::STANDARD));
    BOOST_CHECK(IsEndpointAllowedForPrivacyProfile(
        onion, PrivacyProfile::HIGH));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        invalid, PrivacyProfile::STANDARD));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        invalid, PrivacyProfile::HIGH));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        clearnet, static_cast<PrivacyProfile>(255)));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        local, PrivacyProfile::STANDARD));
    BOOST_CHECK(IsEndpointAllowedForPrivacyProfile(
        local, PrivacyProfile::STANDARD, /*allow_local_endpoint=*/true));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        local, PrivacyProfile::HIGH, /*allow_local_endpoint=*/true));
    BOOST_CHECK(!IsEndpointAllowedForPrivacyProfile(
        private_network, PrivacyProfile::STANDARD,
        /*allow_local_endpoint=*/true));
}

BOOST_AUTO_TEST_CASE(offers_are_sorted_by_exact_rounded_total_cost)
{
    constexpr int64_t now = 200000;
    CKey paid_high_key, sponsored_key, paid_low_key;
    std::vector<Announcement> announcements{
        MakeAnnouncement(paid_high_key, uint256S("11"), FundingModel::USER_PAID, 50, now),
        MakeAnnouncement(sponsored_key, uint256S("12"), FundingModel::SPONSORED, 0, now),
        MakeAnnouncement(paid_low_key, uint256S("13"), FundingModel::USER_PAID, 40, now),
    };
    std::string error;
    const auto candidates = BuildOfferCandidates(announcements, DDCents{1001},
                                                  FUNDING_MODEL_ALL, DDCents{100},
                                                  {}, now, error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_REQUIRE_EQUAL(candidates.size(), 3U);
    BOOST_CHECK(candidates[0].terms.funding_model == FundingModel::SPONSORED);
    BOOST_CHECK(candidates[0].identity_key == announcements[1].identity_key);
    BOOST_CHECK_EQUAL(candidates[0].service_fee.value, 0);
    BOOST_CHECK_EQUAL(candidates[1].service_fee.value, 5);
    BOOST_CHECK_EQUAL(candidates[2].service_fee.value, 6);
}

BOOST_AUTO_TEST_CASE(gross_offers_maximize_recipient_and_require_exact_split)
{
    constexpr int64_t now = 250000;
    CKey paid_high_key, sponsored_key, paid_low_key;
    std::vector<Announcement> announcements{
        MakeAnnouncement(paid_high_key, uint256S("14"),
                         FundingModel::USER_PAID, 100, now),
        MakeAnnouncement(sponsored_key, uint256S("15"),
                         FundingModel::SPONSORED, 0, now),
        MakeAnnouncement(paid_low_key, uint256S("16"),
                         FundingModel::USER_PAID, 50, now),
    };
    std::string error;
    const auto candidates = BuildGrossOfferCandidates(
        announcements, DDCents{5000}, FUNDING_MODEL_ALL, DDCents{100},
        {}, now, error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_REQUIRE_EQUAL(candidates.size(), 3U);
    BOOST_CHECK(candidates.front().terms.funding_model ==
                FundingModel::SPONSORED);
    BOOST_CHECK_EQUAL(candidates.front().payment.value, 5000);
    BOOST_CHECK_EQUAL(candidates[1].payment.value, 4975);
    BOOST_CHECK_EQUAL(candidates[1].service_fee.value, 25);
    BOOST_CHECK_EQUAL(candidates[1].user_total.value, 5000);
    BOOST_CHECK_EQUAL(candidates[2].payment.value, 4950);
    BOOST_CHECK_EQUAL(candidates[2].service_fee.value, 50);

    FastRandomContext rng{true};
    const auto lowest = SelectOfferCandidate(
        candidates, SelectionMode::LOWEST_TOTAL_COST, DDCents{100}, rng,
        error, /*fixed_user_total=*/true);
    BOOST_REQUIRE(lowest);
    BOOST_CHECK_EQUAL(lowest->service_fee.value, 0);

    const auto gap = BuildGrossOfferCandidates(
        {announcements[2]}, DDCents{202}, FUNDING_MODEL_USER_PAID,
        DDCents{100}, {}, now, error);
    BOOST_CHECK(error.empty());
    BOOST_CHECK(gap.empty());
}

BOOST_AUTO_TEST_CASE(one_dd_without_dgb_requires_sponsorship)
{
    constexpr int64_t now = 275000;
    CKey paid_key, sponsored_key;
    const Announcement paid = MakeAnnouncement(
        paid_key, uint256S("17"), FundingModel::USER_PAID, 50, now);
    const Announcement sponsored = MakeAnnouncement(
        sponsored_key, uint256S("18"), FundingModel::SPONSORED, 0, now);
    std::string error;

    // An additive user-paid transfer preserves the 1.00 DD recipient amount,
    // but cent rounding makes even this 50-bps offer cost one additional cent.
    const auto additive = BuildOfferCandidates(
        {paid}, DDCents{100}, FUNDING_MODEL_USER_PAID, DDCents{100}, {}, now,
        error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_REQUIRE_EQUAL(additive.size(), 1U);
    BOOST_CHECK_EQUAL(additive.front().payment.value, 100);
    BOOST_CHECK_EQUAL(additive.front().service_fee.value, 1);
    BOOST_CHECK_EQUAL(additive.front().user_total.value, 101);

    // Deducting that fee from a 1.00 DD gross amount would leave a recipient
    // output below the Paymaster's mandatory 1.00 DD payment floor. No
    // user-paid candidate may therefore be exposed to the wallet.
    const auto paid_gross = BuildGrossOfferCandidates(
        {paid}, DDCents{100}, FUNDING_MODEL_USER_PAID, DDCents{100}, {}, now,
        error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_CHECK(paid_gross.empty());

    // A sponsored offer charges no DD service fee, so it remains the sole
    // exact solution and transfers the complete 1.00 DD amount.
    const auto sponsored_gross = BuildGrossOfferCandidates(
        {paid, sponsored}, DDCents{100}, FUNDING_MODEL_ALL, DDCents{100}, {},
        now, error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_REQUIRE_EQUAL(sponsored_gross.size(), 1U);
    BOOST_CHECK(sponsored_gross.front().terms.funding_model ==
                FundingModel::SPONSORED);
    BOOST_CHECK_EQUAL(sponsored_gross.front().payment.value, 100);
    BOOST_CHECK_EQUAL(sponsored_gross.front().service_fee.value, 0);
    BOOST_CHECK_EQUAL(sponsored_gross.front().user_total.value, 100);
}

BOOST_AUTO_TEST_CASE(cooldown_filters_and_reputation_only_breaks_cost_ties)
{
    constexpr int64_t now = 300000;
    CKey reliable_key, failing_key;
    Announcement reliable = MakeAnnouncement(reliable_key, uint256S("21"), FundingModel::USER_PAID, 100, now);
    Announcement failing = MakeAnnouncement(failing_key, uint256S("22"), FundingModel::USER_PAID, 100, now);
    PaymasterReliabilityRecord reliable_record;
    reliable_record.provider_id = GetPaymasterId(reliable.identity_key);
    PaymasterReliabilityRecord failing_record;
    failing_record.provider_id = GetPaymasterId(failing.identity_key);
    std::string error;
    for (int i = 0; i < 5; ++i) {
        BOOST_REQUIRE(ApplyReliabilityOutcome(reliable_record, ReliabilityOutcome::SUCCESS,
                                              now - 10 + i, 100, error));
        BOOST_REQUIRE(ApplyReliabilityOutcome(failing_record, ReliabilityOutcome::PROVIDER_FAILURE,
                                              now - 10 + i, 0, error));
    }
    failing_record.cooldown_until = 0;
    std::map<PaymasterId, PaymasterReliabilityRecord> reputation{
        {reliable_record.provider_id, reliable_record},
        {failing_record.provider_id, failing_record},
    };
    auto candidates = BuildOfferCandidates({failing, reliable}, DDCents{1000},
                                            FUNDING_MODEL_USER_PAID, DDCents{100},
                                            reputation, now, error);
    BOOST_REQUIRE_EQUAL(candidates.size(), 2U);
    BOOST_CHECK(candidates.front().provider_id == reliable_record.provider_id);

    reliable_record.cooldown_until = now + 1;
    reputation[reliable_record.provider_id] = reliable_record;
    candidates = BuildOfferCandidates({failing, reliable}, DDCents{1000},
                                       FUNDING_MODEL_USER_PAID, DDCents{100},
                                       reputation, now, error);
    BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
    BOOST_CHECK(candidates.front().provider_id == failing_record.provider_id);
}

BOOST_AUTO_TEST_CASE(privacy_weighted_never_exceeds_the_authorized_tolerance)
{
    constexpr int64_t now = 400000;
    CKey first_key, second_key, expensive_key;
    const auto first = MakeAnnouncement(first_key, uint256S("31"), FundingModel::SPONSORED, 0, now);
    const auto second = MakeAnnouncement(second_key, uint256S("32"), FundingModel::USER_PAID, 10, now);
    const auto expensive = MakeAnnouncement(expensive_key, uint256S("33"), FundingModel::USER_PAID, 100, now);
    std::string error;
    const auto candidates = BuildOfferCandidates({first, second, expensive}, DDCents{1000},
                                                  FUNDING_MODEL_ALL, DDCents{100}, {}, now, error);
    BOOST_REQUIRE_EQUAL(candidates.size(), 3U);
    FastRandomContext rng{true};
    for (int i = 0; i < 20; ++i) {
        const auto selected = SelectOfferCandidate(candidates, SelectionMode::PRIVACY_WEIGHTED,
                                                   DDCents{1}, rng, error);
        BOOST_REQUIRE(selected);
        BOOST_CHECK_LE(selected->user_total.value, candidates.front().user_total.value + 1);
    }
    const auto exact = SelectOfferCandidate(candidates, SelectionMode::PRIVACY_WEIGHTED,
                                            DDCents{0}, rng, error);
    BOOST_REQUIRE(exact);
    BOOST_CHECK(exact->provider_id == candidates.front().provider_id);
}

BOOST_AUTO_TEST_CASE(quote_request_is_bound_to_persistent_session_and_user_inputs)
{
    constexpr int64_t now = 500000;
    CKey provider_key, user_key, recipient_key, change_key;
    provider_key.MakeNewKey(true);
    user_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    change_key.MakeNewKey(true);
    const XOnlyPubKey provider_xonly{provider_key.GetPubKey()};
    OfferCandidate offer;
    offer.provider_id = GetPaymasterId(provider_xonly);
    offer.announcement_expires_at = now + 60;
    offer.terms = OfferTerms{uint256S("41"), uint256S("42"), FundingModel::USER_PAID,
                             SponsorshipScope::PUBLIC, 100, DDCents{100}, DDCents{100000}};
    offer.payment = DDCents{1000};
    offer.service_fee = DDCents{10};
    offer.user_total = DDCents{1010};

    PaymentIntentParameters parameters;
    parameters.genesis_hash = uint256S("43");
    parameters.request_id = "550e8400-e29b-41d4-a716-446655440000";
    parameters.session_id = uint256S("44");
    parameters.client_nonce = uint256S("45");
    parameters.canonical_request_hash = uint256S("4501");
    parameters.requested_fee_mode = FeeMode::AUTO;
    parameters.privacy_profile = PrivacyProfile::HIGH;
    parameters.selection_mode = SelectionMode::PRIVACY_WEIGHTED;
    parameters.user_dd_inputs = {COutPoint{uint256S("46"), 1}};
    parameters.recipient_script = TaprootScript(recipient_key);
    parameters.user_dd_change_script = TaprootScript(change_key);
    parameters.expires_at = now + 30;
    std::string error;
    auto intent = BuildUnsignedPaymentIntent(offer, parameters, now, error);
    BOOST_REQUIRE_MESSAGE(intent, error);

    std::vector<unsigned char> signature(64);
    BOOST_REQUIRE(user_key.SignSchnorr(GetUserInputControlHash(*intent, parameters.user_dd_inputs[0]),
                                      signature, nullptr, uint256{}));
    auto request = FinalizeQuoteRequest(*intent, {XOnlyPubKey{user_key.GetPubKey()}},
                                        {signature}, std::nullopt, std::nullopt, now, error);
    BOOST_REQUIRE_MESSAGE(request, error);
    BOOST_CHECK_EQUAL(request->intent.request_id, parameters.request_id);
    BOOST_CHECK(request->intent.session_id == parameters.session_id);
    BOOST_CHECK(request->intent.provider_id == offer.provider_id);
    BOOST_CHECK(request->intent.canonical_request_hash ==
                parameters.canonical_request_hash);
    BOOST_CHECK(request->intent.requested_fee_mode == FeeMode::AUTO);
    BOOST_CHECK(request->intent.privacy_profile == PrivacyProfile::HIGH);
    BOOST_CHECK(request->intent.selection_mode ==
                SelectionMode::PRIVACY_WEIGHTED);

    signature[0] ^= 1;
    BOOST_CHECK(!FinalizeQuoteRequest(*intent, {XOnlyPubKey{user_key.GetPubKey()}},
                                      {signature}, std::nullopt, std::nullopt, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_USER_INPUT_SIGNATURE");
}

BOOST_AUTO_TEST_CASE(unsigned_intent_rejects_replay_unsafe_and_expired_parameters)
{
    constexpr int64_t now = 600000;
    CKey provider_key, recipient_key;
    provider_key.MakeNewKey(true);
    recipient_key.MakeNewKey(true);
    OfferCandidate offer;
    offer.provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    offer.announcement_expires_at = now + 60;
    offer.terms = OfferTerms{uint256S("51"), uint256S("52"), FundingModel::SPONSORED,
                             SponsorshipScope::PUBLIC, 0, DDCents{100}, DDCents{100000}};
    offer.payment = DDCents{1000};

    PaymentIntentParameters parameters;
    parameters.genesis_hash = uint256S("53");
    parameters.request_id = "550e8400-e29b-41d4-a716-446655440001";
    parameters.session_id = uint256S("54");
    parameters.client_nonce = uint256S("55");
    parameters.canonical_request_hash = uint256S("5501");
    const COutPoint input{uint256S("56"), 0};
    parameters.user_dd_inputs = {input, input};
    parameters.recipient_script = TaprootScript(recipient_key);
    parameters.expires_at = now + 30;
    std::string error;
    BOOST_CHECK(!BuildUnsignedPaymentIntent(offer, parameters, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_INTENT_INPUTS");

    parameters.user_dd_inputs = {input};
    parameters.expires_at = now + 61;
    BOOST_CHECK(!BuildUnsignedPaymentIntent(offer, parameters, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_INTENT_PARAMETERS");
}

BOOST_AUTO_TEST_SUITE_END()
