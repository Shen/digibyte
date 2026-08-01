// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider-directory expiry, sequence, and replay behavior. */

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <netbase.h>
#include <paymaster/directory.h>
#include <test/util/setup_common.h>

#include <limits>

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
