// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider-directory expiry, sequence, and replay behavior. */

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <netbase.h>
#include <paymaster/directory.h>
#include <test/util/setup_common.h>

#include <array>
#include <atomic>
#include <limits>
#include <thread>

using namespace DigiDollar::Paymaster;

namespace {

Announcement SignedAnnouncement(const CKey& key, const uint256& genesis, uint64_t sequence, int64_t now)
{
    Announcement announcement;
    // Keep this fixture bound to the Paymaster wire version explicitly.  The
    // test binary also exposes the node-wide ::PROTOCOL_VERSION, so relying on
    // an unqualified aggregate default here can silently exercise the legacy
    // v1 envelope after a Paymaster protocol bump.
    announcement.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    const uint256 second = uint256S("02");
    announcement.genesis_hash = genesis;
    announcement.identity_key = XOnlyPubKey{key.GetPubKey()};
    announcement.display_name = "Test Paymaster";
    announcement.sequence = sequence;
    announcement.created_at = now;
    announcement.expires_at = now + ANNOUNCEMENT_TTL_SECONDS;
    announcement.endpoint = LookupNumeric("8.8.8.8", 12024);
    announcement.offers = {{uint256::ONE, second, FundingModel::USER_PAID,
                            SponsorshipScope::PUBLIC, 50, DDCents{100}, DDCents{1000000}}};
    for (uint32_t i = 0; i < REQUIRED_ADMISSION_SLOTS; ++i) {
        AdmissionSlotProof slot;
        slot.dgb_outpoint = COutPoint{uint256::ONE, i};
        slot.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        slot.carrier_outpoint = COutPoint{second, i};
        slot.carrier_value = DDCents{100};
        slot.reference_block = uint256::ONE;
        slot.expires_at = announcement.expires_at;
        slot.dgb_control_signature.resize(64);
        slot.carrier_control_signature.resize(64);
        announcement.admission_slots.push_back(std::move(slot));
    }
    announcement.identity_signature.resize(64);
    BOOST_REQUIRE(key.SignSchnorr(GetAnnouncementSignatureHash(announcement),
                                  announcement.identity_signature, nullptr, uint256{}));
    return announcement;
}

void SetAdmissionOutpoints(Announcement& announcement,
                           const CKey& key,
                           const uint256& dgb_txid,
                           const uint256& carrier_txid)
{
    for (uint32_t i = 0; i < announcement.admission_slots.size(); ++i) {
        announcement.admission_slots[i].dgb_outpoint = COutPoint{dgb_txid, i};
        announcement.admission_slots[i].carrier_outpoint = COutPoint{carrier_txid, i};
    }
    announcement.identity_signature.assign(64, 0);
    BOOST_REQUIRE(key.SignSchnorr(GetAnnouncementSignatureHash(announcement),
                                  announcement.identity_signature, nullptr, uint256{}));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_directory_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(validates_signed_envelope_and_monotonic_replacement)
{
    CKey key;
    key.MakeNewKey(true);
    const uint256 genesis = uint256::ONE;
    const int64_t now{100000};
    auto first = SignedAnnouncement(key, genesis, 1, now);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateAnnouncementEnvelope(first, genesis, now, error), error);

    Directory directory;
    const PaymasterId provider_id{GetPaymasterId(first.identity_key)};
    BOOST_CHECK(!directory.AcceptsSequence(PaymasterId{}, 1));
    BOOST_CHECK(!directory.AcceptsSequence(provider_id, 0));
    BOOST_CHECK(directory.AcceptsSequence(provider_id, 1));
    BOOST_CHECK(directory.AddValidated(first, now));
    BOOST_CHECK(!directory.AcceptsSequence(provider_id, 1));
    BOOST_CHECK(directory.AcceptsSequence(provider_id, 2));
    BOOST_CHECK(!directory.AddValidated(first, now));
    auto replacement = SignedAnnouncement(key, genesis, 2, now);
    BOOST_CHECK(directory.AddValidated(replacement, now));
    BOOST_CHECK(!directory.AcceptsSequence(provider_id, 2));
    BOOST_CHECK(directory.AcceptsSequence(provider_id, 3));
    BOOST_CHECK_EQUAL(directory.Size(), 1U);
}

BOOST_AUTO_TEST_CASE(rejects_sponsored_offer_with_fee)
{
    CKey key;
    key.MakeNewKey(true);
    auto announcement = SignedAnnouncement(key, uint256::ONE, 1, 100000);
    announcement.offers[0].funding_model = FundingModel::SPONSORED;
    std::string error;
    BOOST_CHECK(!ValidateAnnouncementEnvelope(announcement, uint256::ONE, 100000, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_SPONSORED_FEE");
}

BOOST_AUTO_TEST_CASE(local_endpoint_is_allowed_only_when_explicitly_requested)
{
    CKey key;
    key.MakeNewKey(true);
    const uint256 genesis = uint256::ONE;
    const int64_t now{100000};
    auto announcement = SignedAnnouncement(key, genesis, 1, now);
    announcement.endpoint = LookupNumeric("127.0.0.1", 12024);
    BOOST_REQUIRE(key.SignSchnorr(GetAnnouncementSignatureHash(announcement),
                                  announcement.identity_signature, nullptr, uint256{}));
    std::string error;
    BOOST_CHECK(!ValidateAnnouncementEnvelope(announcement, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_UNROUTABLE_ENDPOINT");
    BOOST_CHECK(ValidateAnnouncementEnvelope(announcement, genesis, now, error,
                                             /*allow_local_endpoint=*/true));
}

BOOST_AUTO_TEST_CASE(expired_entries_are_not_listed)
{
    CKey key;
    key.MakeNewKey(true);
    auto announcement = SignedAnnouncement(key, uint256::ONE, 1, 100000);
    Directory directory;
    BOOST_CHECK(directory.AddValidated(announcement, 100000));
    BOOST_CHECK(directory.List(100000 + ANNOUNCEMENT_TTL_SECONDS).empty());
    directory.RemoveExpired(100000 + ANNOUNCEMENT_TTL_SECONDS);
    BOOST_CHECK_EQUAL(directory.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(rejects_cross_provider_admission_outpoint_reuse)
{
    CKey first_key;
    CKey second_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    constexpr int64_t now{100000};
    Directory directory;

    BOOST_REQUIRE(directory.AddValidated(
        SignedAnnouncement(first_key, uint256::ONE, 1, now), now));
    const auto duplicate{SignedAnnouncement(second_key, uint256::ONE, 1, now)};
    BOOST_CHECK(!directory.AcceptsAdmissionOutpoints(duplicate, now));
    BOOST_CHECK(!directory.AddValidated(duplicate, now));
    BOOST_CHECK_EQUAL(directory.Size(), 1U);
}

BOOST_AUTO_TEST_CASE(rejects_partial_cross_provider_admission_overlap)
{
    CKey first_key;
    CKey second_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    constexpr int64_t now{100000};
    const auto first{SignedAnnouncement(first_key, uint256::ONE, 1, now)};

    auto dgb_overlap{SignedAnnouncement(second_key, uint256::ONE, 1, now)};
    SetAdmissionOutpoints(dgb_overlap, second_key, uint256S("03"), uint256S("04"));
    dgb_overlap.admission_slots[1].dgb_outpoint = first.admission_slots[0].dgb_outpoint;
    dgb_overlap.identity_signature.assign(64, 0);
    BOOST_REQUIRE(second_key.SignSchnorr(GetAnnouncementSignatureHash(dgb_overlap),
                                         dgb_overlap.identity_signature, nullptr, uint256{}));
    Directory dgb_directory;
    BOOST_REQUIRE(dgb_directory.AddValidated(first, now));
    BOOST_CHECK(!dgb_directory.AddValidated(dgb_overlap, now));

    auto carrier_overlap{SignedAnnouncement(second_key, uint256::ONE, 1, now)};
    SetAdmissionOutpoints(carrier_overlap, second_key, uint256S("05"), uint256S("06"));
    carrier_overlap.admission_slots[1].carrier_outpoint = first.admission_slots[0].carrier_outpoint;
    carrier_overlap.identity_signature.assign(64, 0);
    BOOST_REQUIRE(second_key.SignSchnorr(GetAnnouncementSignatureHash(carrier_overlap),
                                         carrier_overlap.identity_signature, nullptr, uint256{}));
    Directory carrier_directory;
    BOOST_REQUIRE(carrier_directory.AddValidated(first, now));
    BOOST_CHECK(!carrier_directory.AddValidated(carrier_overlap, now));
}

BOOST_AUTO_TEST_CASE(expired_provider_releases_admission_outpoints)
{
    CKey first_key;
    CKey second_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    constexpr int64_t now{100000};
    const auto first{SignedAnnouncement(first_key, uint256::ONE, 1, now)};
    Directory directory;
    BOOST_REQUIRE(directory.AddValidated(first, now));

    const int64_t after_expiry{first.expires_at};
    const auto replacement{SignedAnnouncement(second_key, uint256::ONE, 1, after_expiry)};
    BOOST_CHECK(directory.AcceptsAdmissionOutpoints(replacement, after_expiry));
    BOOST_CHECK(directory.AddValidated(replacement, after_expiry));
    BOOST_CHECK_EQUAL(directory.List(after_expiry).size(), 1U);
}

BOOST_AUTO_TEST_CASE(list_is_bounded_and_rotates_active_entries)
{
    constexpr int64_t now{100000};
    CKey first_key;
    CKey second_key;
    CKey third_key;
    CKey fourth_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    third_key.MakeNewKey(true);
    fourth_key.MakeNewKey(true);
    std::array<std::pair<CKey*, std::pair<uint256, uint256>>, 4> fixtures{{
        {&first_key, {uint256S("11"), uint256S("12")}},
        {&second_key, {uint256S("21"), uint256S("22")}},
        {&third_key, {uint256S("31"), uint256S("32")}},
        {&fourth_key, {uint256S("41"), uint256S("42")}},
    }};

    Directory directory;
    for (auto& [key, outpoints] : fixtures) {
        auto announcement{SignedAnnouncement(*key, uint256::ONE, 1, now)};
        SetAdmissionOutpoints(
            announcement, *key, outpoints.first, outpoints.second);
        BOOST_REQUIRE(directory.AddValidated(std::move(announcement), now));
    }

    const auto all{directory.List(now)};
    BOOST_REQUIRE_EQUAL(all.size(), fixtures.size());
    const auto first_two{directory.List(now, 2, 0)};
    BOOST_REQUIRE_EQUAL(first_two.size(), 2U);
    BOOST_CHECK(GetPaymasterId(first_two[0].identity_key) ==
                GetPaymasterId(all[0].identity_key));
    BOOST_CHECK(GetPaymasterId(first_two[1].identity_key) ==
                GetPaymasterId(all[1].identity_key));

    const auto wrapped{directory.List(now, 3, all.size() - 1)};
    BOOST_REQUIRE_EQUAL(wrapped.size(), 3U);
    BOOST_CHECK(GetPaymasterId(wrapped[0].identity_key) ==
                GetPaymasterId(all[3].identity_key));
    BOOST_CHECK(GetPaymasterId(wrapped[1].identity_key) ==
                GetPaymasterId(all[0].identity_key));
    BOOST_CHECK(GetPaymasterId(wrapped[2].identity_key) ==
                GetPaymasterId(all[1].identity_key));
    BOOST_CHECK(directory.List(now, 0).empty());
}

BOOST_AUTO_TEST_CASE(concurrent_cross_provider_claim_accepts_only_one_identity)
{
    CKey first_key;
    CKey second_key;
    first_key.MakeNewKey(true);
    second_key.MakeNewKey(true);
    constexpr int64_t now{100000};
    auto first{SignedAnnouncement(first_key, uint256::ONE, 1, now)};
    auto second{SignedAnnouncement(second_key, uint256::ONE, 1, now)};
    Directory directory;
    std::atomic<unsigned int> accepted{0};

    std::thread first_thread{[&] {
        if (directory.AddValidated(std::move(first), now)) accepted.fetch_add(1);
    }};
    std::thread second_thread{[&] {
        if (directory.AddValidated(std::move(second), now)) accepted.fetch_add(1);
    }};
    first_thread.join();
    second_thread.join();

    BOOST_CHECK_EQUAL(accepted.load(), 1U);
    BOOST_CHECK_EQUAL(directory.Size(), 1U);
}

BOOST_AUTO_TEST_CASE(rejects_extreme_timestamps_without_overflow)
{
    CKey key;
    key.MakeNewKey(true);
    const uint256 genesis = uint256::ONE;
    std::string error;

    auto oversized_window = SignedAnnouncement(key, genesis, 1, 100000);
    oversized_window.created_at = std::numeric_limits<int64_t>::min();
    oversized_window.expires_at = std::numeric_limits<int64_t>::max();
    BOOST_CHECK(!ValidateAnnouncementEnvelope(oversized_window, genesis, 0, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_ANNOUNCEMENT_TIME");

    auto extreme_now = SignedAnnouncement(key, genesis, 1, 100000);
    BOOST_CHECK(!ValidateAnnouncementEnvelope(
        extreme_now, genesis, std::numeric_limits<int64_t>::max(), error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_ANNOUNCEMENT_TIME");
}

BOOST_AUTO_TEST_SUITE_END()
