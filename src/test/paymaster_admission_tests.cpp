// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <paymaster/manager.h>

#include <boost/test/unit_test.hpp>

using namespace DigiDollar::Paymaster;

namespace {
constexpr int64_t NOW{100000};
uint256 Id(uint32_t value)
{
    uint256 id;
    for (int i = 0; i < 4; ++i) id.begin()[i] = (value >> (8 * i)) & 255;
    return id;
}

PaymasterCapacityRequest Request(uint32_t session, uint32_t provider = 100)
{
    PaymasterCapacityRequest request;
    request.genesis_hash = Id(1);
    request.provider_id = Id(provider);
    request.request_id = "550e8400-e29b-41d4-a716-446655440020";
    request.session_id = Id(session);
    request.client_nonce = Id(session);
    request.funding_model = FundingModel::SPONSORED;
    request.created_at = NOW;
    request.expires_at = NOW + 60;
    return request;
}

PaymasterCapacityProof Reply(const PaymasterCapacityRequest& request)
{
    PaymasterCapacityProof proof;
    proof.provider_id = request.provider_id;
    proof.request_id = request.request_id;
    proof.session_id = request.session_id;
    proof.client_nonce = request.client_nonce;
    return proof;
}

AlternativeRecoveryRequest Recovery(uint32_t session)
{
    AlternativeRecoveryRequest request;
    request.recovery_provider_id = Id(100);
    request.request_id = "550e8400-e29b-41d4-a716-446655440020";
    request.session_id = Id(session);
    return request;
}

AlternativeRecoveryResponse Reply(const AlternativeRecoveryRequest& request)
{
    AlternativeRecoveryResponse response;
    response.recovery_provider_id = request.recovery_provider_id;
    response.request_id = request.request_id;
    response.session_id = request.session_id;
    return response;
}

// Exercise manager admission after envelope validation. Synthetic replies here
// have no financial/signature authority; wire and wallet suites test that layer.
DirectEnqueueResult Receive(Manager& manager, int64_t peer, uint32_t id,
                            DirectPayload payload, DirectAdmissionClass admission,
                            uint64_t group = 1, size_t bytes = 100)
{
    return manager.EnqueueNetworkDirectMessage(peer, Id(id), bytes,
        std::move(payload), NOW, group, {1, 2, 3}, admission);
}
} // namespace

BOOST_AUTO_TEST_SUITE(paymaster_admission_tests)

BOOST_AUTO_TEST_CASE(unknown_provider_flood_cannot_allocate_inbox_or_reply_authority)
{
    Manager manager{true};
    for (int i = 1; i <= 40; ++i) {
        BOOST_CHECK(Receive(manager, i, i, Request(i), DirectAdmissionClass::REQUEST, i)
                    == DirectEnqueueResult::INVALID);
    }
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_REQUIRE(manager.StartProvider("provider", Id(100)));
    BOOST_CHECK(Receive(manager, 99, 99, Request(99), DirectAdmissionClass::REQUEST)
                == DirectEnqueueResult::ACCEPTED);
    BOOST_CHECK(Receive(manager, 98, 98, Reply(Request(98)), DirectAdmissionClass::RESPONSE)
                == DirectEnqueueResult::INVALID);
}

BOOST_AUTO_TEST_CASE(drain_and_manual_routes_survive_but_stop_requires_explicit_restart)
{
    Manager manager{true};
    BOOST_REQUIRE(manager.StartProvider("provider", Id(100)));
    for (auto state : {ProviderServiceState::MANUAL, ProviderServiceState::DRAIN_ONLY,
                       ProviderServiceState::WAITING_FOR_UNLOCK}) {
        manager.SetProviderServiceStatus("provider", state);
        BOOST_CHECK(manager.CanReceiveDirectMessage(1, Recovery(1), DirectAdmissionClass::REQUEST, NOW));
    }
    manager.StopProvider("provider");
    BOOST_CHECK(!manager.CanReceiveDirectMessage(1, Recovery(1), DirectAdmissionClass::REQUEST, NOW));
    BOOST_REQUIRE(manager.StartProvider("provider", Id(100)));
    BOOST_CHECK(manager.CanReceiveDirectMessage(1, Recovery(1), DirectAdmissionClass::REQUEST, NOW));
}

BOOST_AUTO_TEST_CASE(reply_reserve_requires_exact_locally_queued_peer_session_provider_and_phase)
{
    Manager manager{true};
    const auto request = Request(1);
    const auto response = Reply(request);
    BOOST_CHECK(!manager.CanReceiveDirectMessage(10, response, DirectAdmissionClass::RESPONSE, NOW));
    BOOST_REQUIRE(manager.QueueCapacityRequest(10, Id(1), 100, request, NOW));
    BOOST_CHECK(manager.CanReceiveDirectMessage(10, response, DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK(!manager.CanReceiveDirectMessage(11, response, DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK(!manager.CanReceiveDirectMessage(10, Reply(Request(2)), DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK(!manager.CanReceiveDirectMessage(10, Reply(Request(1, 101)), DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK(!manager.CanReceiveDirectMessage(10, response, DirectAdmissionClass::RECOVERY, NOW));
    PaymasterResultMessage wrong_phase;
    wrong_phase.request_id = request.request_id;
    wrong_phase.session_id = request.session_id;
    wrong_phase.result.provider_id = request.provider_id;
    BOOST_CHECK(!manager.CanReceiveDirectMessage(10, wrong_phase, DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK(Receive(manager, 10, 101, response, DirectAdmissionClass::RESPONSE)
                == DirectEnqueueResult::ACCEPTED);
    manager.ForgetDirectPeer(10);
    BOOST_CHECK(!manager.CanReceiveDirectMessage(10, response, DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U); // Signed evidence is retained.
    BOOST_REQUIRE(manager.QueueCapacityRequest(12, Id(2), 100, request, NOW));
    BOOST_CHECK(!manager.CanReceiveDirectMessage(12, response, DirectAdmissionClass::RESPONSE, NOW + 120));
}

BOOST_AUTO_TEST_CASE(request_transport_and_decoded_flood_cannot_exhaust_reply_or_recovery_windows)
{
    Manager manager{true};
    BOOST_REQUIRE(manager.StartProvider("provider", Id(100)));
    for (size_t i = 0; i < DIRECT_CLASS_TRANSPORT[0]; ++i) {
        const int64_t peer = 1 + i / 32;
        BOOST_REQUIRE(manager.AdmitDirectTransport(peer, peer, NOW));
        const auto outcome = Receive(manager, peer, 1, Request(1), DirectAdmissionClass::REQUEST, peer);
        BOOST_CHECK(outcome == (i == 0 ? DirectEnqueueResult::ACCEPTED : DirectEnqueueResult::DUPLICATE));
    }
    BOOST_CHECK(!manager.AdmitDirectTransport(99, 99, NOW));
    const auto request = Request(2);
    const auto recovery = Recovery(3);
    BOOST_REQUIRE(manager.QueueCapacityRequest(100, Id(200), 100, request, NOW));
    BOOST_REQUIRE(manager.QueueOutboundDirectMessage(101, Id(201), 100, recovery, NOW));
    // Deliberately reuse the attacker's group. Both raw and decoded quotas
    // must be partitioned, not just the top-level inbox count.
    BOOST_CHECK(manager.AdmitDirectTransport(100, 1, NOW, DirectAdmissionClass::RESPONSE));
    BOOST_CHECK(manager.AdmitDirectTransport(101, 1, NOW, DirectAdmissionClass::RECOVERY));
    BOOST_CHECK(Receive(manager, 100, 202, Reply(request), DirectAdmissionClass::RESPONSE)
                == DirectEnqueueResult::ACCEPTED);
    BOOST_CHECK(Receive(manager, 101, 203, Reply(recovery), DirectAdmissionClass::RECOVERY)
                == DirectEnqueueResult::ACCEPTED);
}

BOOST_AUTO_TEST_CASE(full_request_inbox_preserves_both_response_reserves_and_provider_fairness)
{
    Manager manager{true};
    for (uint32_t p = 100; p < 104; ++p) BOOST_REQUIRE(manager.StartProvider(std::to_string(p), Id(p)));
    for (uint32_t i = 0; i < 24; ++i) {
        BOOST_REQUIRE(Receive(manager, 1 + i / 4, 1 + i, Request(1 + i, 100 + i / 8),
                              DirectAdmissionClass::REQUEST, 1 + i / 4) == DirectEnqueueResult::ACCEPTED);
        if (i == 7) {
            BOOST_CHECK(Receive(manager, 90, 90, Request(90), DirectAdmissionClass::REQUEST, 90)
                        == DirectEnqueueResult::FULL);
        }
    }
    BOOST_CHECK(Receive(manager, 91, 91, Request(91, 103), DirectAdmissionClass::REQUEST, 91)
                == DirectEnqueueResult::FULL);
    for (uint32_t i = 0; i < 8; ++i) {
        const auto request = Request(100 + i);
        BOOST_REQUIRE(manager.QueueCapacityRequest(100 + i, Id(100 + i), 100, request, NOW));
        BOOST_REQUIRE(Receive(manager, 100 + i, 200 + i, Reply(request), DirectAdmissionClass::RESPONSE,
                              1 + i / 4) == DirectEnqueueResult::ACCEPTED);
        const auto recovery = Recovery(300 + i);
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(200 + i, Id(300 + i), 100, recovery, NOW));
        BOOST_REQUIRE(Receive(manager, 200 + i, 400 + i, Reply(recovery), DirectAdmissionClass::RECOVERY,
                              1 + i / 4) == DirectEnqueueResult::ACCEPTED);
    }
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), MAX_DIRECT_INBOX_MESSAGES);
}

BOOST_AUTO_TEST_CASE(full_provider_outbox_does_not_block_local_payment_and_recovery_requests)
{
    Manager manager{true};
    for (uint32_t i = 0; i < 24; ++i) {
        BOOST_REQUIRE(manager.QueueCapacityProof(1 + i / 4, Id(i + 1), 100, Reply(Request(i + 1)), NOW));
    }
    BOOST_CHECK(!manager.QueueCapacityProof(99, Id(99), 100, Reply(Request(99)), NOW));
    BOOST_CHECK(manager.QueueCapacityRequest(100, Id(100), 100, Request(100), NOW));
    BOOST_CHECK(manager.QueueOutboundDirectMessage(101, Id(101), 100, Recovery(101), NOW));
}

BOOST_AUTO_TEST_CASE(byte_reserves_survive_maximum_sized_incoming_requests)
{
    Manager manager{true};
    for (uint32_t p = 100; p < 103; ++p) BOOST_REQUIRE(manager.StartProvider(std::to_string(p), Id(p)));
    for (uint32_t i = 0; i < 12; ++i) {
        BOOST_REQUIRE(Receive(manager, 1 + i, i + 1, Request(i + 1, 100 + i / 4),
                              DirectAdmissionClass::REQUEST, 1 + i, MAX_DIRECT_MESSAGE_BYTES)
                      == DirectEnqueueResult::ACCEPTED);
    }
    BOOST_CHECK(Receive(manager, 99, 99, Request(99, 102), DirectAdmissionClass::REQUEST, 99)
                == DirectEnqueueResult::FULL);
    for (uint32_t i = 0; i < 4; ++i) {
        const auto request = Request(100 + i);
        BOOST_REQUIRE(manager.QueueCapacityRequest(100 + i, Id(100 + i), 100, request, NOW));
        BOOST_REQUIRE(Receive(manager, 100 + i, 200 + i, Reply(request), DirectAdmissionClass::RESPONSE,
                              1 + i, MAX_DIRECT_MESSAGE_BYTES) == DirectEnqueueResult::ACCEPTED);
        const auto recovery = Recovery(300 + i);
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(200 + i, Id(300 + i), 100, recovery, NOW));
        BOOST_REQUIRE(Receive(manager, 200 + i, 400 + i, Reply(recovery), DirectAdmissionClass::RECOVERY,
                              1 + i, MAX_DIRECT_MESSAGE_BYTES) == DirectEnqueueResult::ACCEPTED);
    }
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 20U);
}


BOOST_AUTO_TEST_CASE(decoded_provider_limit_cannot_poison_expected_response_tokens)
{
    Manager manager{true};
    BOOST_REQUIRE(manager.StartProvider("provider", Id(100)));
    for (uint32_t i = 0; i < MAX_DIRECT_MESSAGES_PER_PROVIDER_PER_WINDOW; ++i) {
        BOOST_REQUIRE(Receive(manager, 1 + i / 16, i + 1, Request(i + 1),
                              DirectAdmissionClass::REQUEST, i + 1) == DirectEnqueueResult::ACCEPTED);
        BOOST_REQUIRE_EQUAL(manager.TakeCapacityRequests(Id(100), 1).size(), 1U);
    }
    BOOST_CHECK(Receive(manager, 99, 99, Request(99), DirectAdmissionClass::REQUEST, 99)
                == DirectEnqueueResult::RATE_LIMITED);
    const auto request = Request(100);
    BOOST_REQUIRE(manager.QueueCapacityRequest(100, Id(100), 100, request, NOW));
    BOOST_CHECK(Receive(manager, 100, 200, Reply(request), DirectAdmissionClass::RESPONSE)
                == DirectEnqueueResult::ACCEPTED);
}

BOOST_AUTO_TEST_CASE(expected_response_storage_is_bounded_and_disconnect_reclaims_only_its_owner)
{
    Manager manager{true};
    for (uint32_t i = 1; i <= MAX_EXPECTED_DIRECT_RESPONSES; ++i) {
        BOOST_REQUIRE(manager.QueueCapacityRequest(i, Id(i), 100, Request(i), NOW));
        BOOST_REQUIRE_EQUAL(manager.TakeOutboundDirectMessages(i, 1, NOW).size(), 1U);
    }
    BOOST_CHECK(!manager.QueueCapacityRequest(1000, Id(1000), 100, Request(1000), NOW));
    manager.ForgetDirectPeer(1);
    BOOST_REQUIRE(manager.QueueCapacityRequest(1000, Id(1000), 100, Request(1000), NOW));
    BOOST_CHECK(manager.CanReceiveDirectMessage(2, Reply(Request(2)), DirectAdmissionClass::RESPONSE, NOW));
    BOOST_CHECK(!manager.CanReceiveDirectMessage(1, Reply(Request(1)), DirectAdmissionClass::RESPONSE, NOW));
    manager.ClearDirectMessages(); // Also used by SetEnabled(false).
    BOOST_CHECK(!manager.CanReceiveDirectMessage(2, Reply(Request(2)), DirectAdmissionClass::RESPONSE, NOW));
}


BOOST_AUTO_TEST_CASE(all_five_wire_request_phases_register_only_their_matching_response)
{
    Manager manager{true};
    const auto capacity = Request(1);
    PaymasterQuoteRequest quote;
    quote.intent.provider_id = capacity.provider_id;
    quote.intent.request_id = capacity.request_id;
    quote.intent.session_id = capacity.session_id;
    PaymasterQuoteResponse quoted;
    quoted.quote.provider_id = capacity.provider_id;
    quoted.request_id = capacity.request_id;
    quoted.session_id = capacity.session_id;
    PaymasterSubmit submit;
    submit.provider_id = capacity.provider_id;
    submit.request_id = capacity.request_id;
    submit.session_id = capacity.session_id;
    PaymasterResultMessage result;
    result.result.provider_id = capacity.provider_id;
    result.request_id = capacity.request_id;
    result.session_id = capacity.session_id;
    const auto recovery = Recovery(2);
    AlternativeRecoverySubmit recovery_submit;
    recovery_submit.recovery_provider_id = recovery.recovery_provider_id;
    recovery_submit.request_id = recovery.request_id;
    recovery_submit.session_id = recovery.session_id;
    AlternativeRecoveryResultMessage recovered;
    recovered.result.provider_id = recovery.recovery_provider_id;
    recovered.request_id = recovery.request_id;
    recovered.session_id = recovery.session_id;
    const std::vector<std::pair<DirectPayload, DirectPayload>> phases{
        {capacity, Reply(capacity)}, {quote, quoted}, {submit, result},
        {recovery, Reply(recovery)}, {recovery_submit, recovered}};
    for (size_t i = 0; i < phases.size(); ++i) {
        const auto admission = i >= 3 ? DirectAdmissionClass::RECOVERY : DirectAdmissionClass::RESPONSE;
        const int64_t peer = i + 1;
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(peer, Id(peer), 100, phases[i].first, NOW));
        BOOST_CHECK(manager.CanReceiveDirectMessage(peer, phases[i].second, admission, NOW));
        BOOST_CHECK(!manager.CanReceiveDirectMessage(peer + 100, phases[i].second, admission, NOW));
    }
}

BOOST_AUTO_TEST_SUITE_END()
