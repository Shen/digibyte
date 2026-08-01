// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Restricted-capability binding and single-use authorization tests. */

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <netbase.h>
#include <paymaster/sponsorship.h>
#include <script/script.h>
#include <test/util/setup_common.h>

using namespace DigiDollar::Paymaster;

namespace {

RestrictedServiceDescriptor SignedDescriptor(const CKey& provider_key,
                                              const CKey& sponsor_key,
                                              const uint256& genesis,
                                              int64_t now)
{
    RestrictedServiceDescriptor descriptor;
    descriptor.genesis_hash = genesis;
    descriptor.provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    descriptor.p2p_endpoint = LookupNumeric("8.8.8.8", 12024);
    descriptor.offer_id = uint256::ONE;
    descriptor.policy_hash = uint256S("02");
    descriptor.sponsor_authorization_key = XOnlyPubKey{sponsor_key.GetPubKey()};
    descriptor.sponsor_display_name = "Test Sponsor";
    descriptor.expires_at = now + 300;
    descriptor.admission_sequence = 1;
    descriptor.min_confirmations = 1;
    for (uint32_t i = 0; i < REQUIRED_ADMISSION_SLOTS; ++i) {
        AdmissionSlotProof slot;
        slot.dgb_outpoint = COutPoint{uint256::ONE, i};
        slot.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        slot.reference_block = uint256::ONE;
        slot.expires_at = descriptor.expires_at;
        slot.dgb_control_signature.resize(64);
        descriptor.minimum_liquidity_proof.push_back(std::move(slot));
    }
    descriptor.provider_identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(GetRestrictedDescriptorSignatureHash(descriptor),
                                           descriptor.provider_identity_signature,
                                           nullptr, uint256{}));
    return descriptor;
}

SponsorshipCapability SignedCapability(const CKey& sponsor_key,
                                       const RestrictedServiceDescriptor& descriptor,
                                       const CScript& recipient,
                                       DDCents amount,
                                       const uint256& nonce,
                                       int64_t now)
{
    SponsorshipCapability capability;
    capability.genesis_hash = descriptor.genesis_hash;
    capability.provider_id = descriptor.provider_id;
    capability.offer_id = descriptor.offer_id;
    capability.policy_hash = descriptor.policy_hash;
    capability.recipient_script = recipient;
    capability.amount = amount;
    capability.payment_request_nonce = nonce;
    capability.expires_at = now + 120;
    capability.sponsor_signature.resize(64);
    BOOST_REQUIRE(sponsor_key.SignSchnorr(GetSponsorshipCapabilitySignatureHash(capability),
                                          capability.sponsor_signature, nullptr, uint256{}));
    return capability;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_sponsorship_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(descriptor_binds_provider_endpoint_policy_and_sponsor_key)
{
    CKey provider_key;
    CKey sponsor_key;
    provider_key.MakeNewKey(true);
    sponsor_key.MakeNewKey(true);
    const int64_t now{100000};
    const uint256 genesis = uint256S("03");
    auto descriptor = SignedDescriptor(provider_key, sponsor_key, genesis, now);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateRestrictedServiceDescriptor(
        descriptor, XOnlyPubKey{provider_key.GetPubKey()}, genesis, now, error), error);

    descriptor.offer_id = uint256S("04");
    BOOST_CHECK(!ValidateRestrictedServiceDescriptor(
        descriptor, XOnlyPubKey{provider_key.GetPubKey()}, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_DESCRIPTOR_SIGNATURE");
}

BOOST_AUTO_TEST_CASE(capability_is_bound_to_one_payment_and_descriptor)
{
    CKey provider_key;
    CKey sponsor_key;
    provider_key.MakeNewKey(true);
    sponsor_key.MakeNewKey(true);
    const int64_t now{100000};
    const uint256 genesis = uint256S("03");
    const uint256 nonce = uint256S("05");
    const CScript recipient = CScript{} << OP_TRUE;
    const DDCents amount{2500};
    const auto descriptor = SignedDescriptor(provider_key, sponsor_key, genesis, now);
    auto capability = SignedCapability(sponsor_key, descriptor, recipient, amount, nonce, now);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateSponsorshipCapability(
        capability, descriptor, recipient, amount, nonce, genesis, now, error), error);
    BOOST_CHECK(!GetSponsorshipCapabilityHash(capability).IsNull());

    BOOST_CHECK(!ValidateSponsorshipCapability(
        capability, descriptor, recipient, DDCents{2501}, nonce, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPABILITY_BINDING_MISMATCH");
    BOOST_CHECK(!ValidateSponsorshipCapability(
        capability, descriptor, recipient, amount, uint256S("06"), genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPABILITY_BINDING_MISMATCH");

    capability.amount = DDCents{2501};
    BOOST_CHECK(!ValidateSponsorshipCapability(
        capability, descriptor, recipient, capability.amount, nonce, genesis, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_CAPABILITY_SIGNATURE");
}

BOOST_AUTO_TEST_CASE(scope_fee_and_authorization_rules_fail_closed)
{
    std::string error;
    BOOST_CHECK(ValidateSponsorshipBinding(FundingModel::SPONSORED,
                                           SponsorshipScope::PUBLIC, 0, DDCents{0},
                                           uint256{}, error));
    BOOST_CHECK(ValidateSponsorshipBinding(FundingModel::SPONSORED,
                                           SponsorshipScope::RESTRICTED, 0, DDCents{0},
                                           uint256::ONE, error));
    BOOST_CHECK(!ValidateSponsorshipBinding(FundingModel::SPONSORED,
                                            SponsorshipScope::RESTRICTED, 0, DDCents{0},
                                            uint256{}, error));
    BOOST_CHECK(!ValidateSponsorshipBinding(FundingModel::SPONSORED,
                                            SponsorshipScope::PUBLIC, 10, DDCents{1},
                                            uint256{}, error));
    BOOST_CHECK(ValidateSponsorshipBinding(FundingModel::USER_PAID,
                                           SponsorshipScope::PUBLIC, 50, DDCents{1},
                                           uint256{}, error));
    BOOST_CHECK(!ValidateSponsorshipBinding(FundingModel::USER_PAID,
                                            SponsorshipScope::RESTRICTED, 50, DDCents{1},
                                            uint256::ONE, error));
}

BOOST_AUTO_TEST_CASE(durable_record_contains_only_hashes_and_enforces_transition_times)
{
    SponsorshipAuthorizationRecord record;
    record.capability_hash = uint256::ONE;
    record.payment_binding_hash = uint256S("02");
    record.reservation_id = uint256S("03");
    record.reserved_at = 100;
    record.expires_at = 200;
    std::string error;
    BOOST_CHECK(ValidateSponsorshipAuthorizationRecord(record, error));

    record.state = SponsorshipAuthorizationState::CONSUMED;
    record.consumed_at = 150;
    BOOST_CHECK(ValidateSponsorshipAuthorizationRecord(record, error));
    record.consumed_at = 201;
    BOOST_CHECK(!ValidateSponsorshipAuthorizationRecord(record, error));
}

BOOST_AUTO_TEST_SUITE_END()
