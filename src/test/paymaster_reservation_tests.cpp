// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Reservation expiry, authorization-risk, and release invariants. */

#include <boost/test/unit_test.hpp>

#include <paymaster/reservation.h>
#include <streams.h>
#include <version.h>

#include <vector>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_reservation_tests)

BOOST_AUTO_TEST_CASE(session_does_not_release_ambiguous_authorization)
{
    BOOST_CHECK(CanTransition(SessionState::AUTHORIZED, SessionState::PENDING_PROVIDER));
    BOOST_CHECK(!CanTransition(SessionState::PENDING_PROVIDER, SessionState::FAILED));
    BOOST_CHECK(!CanTransition(SessionState::PENDING_PROVIDER, SessionState::INPUTS_RESERVED));
}

BOOST_AUTO_TEST_CASE(relocking_before_user_signature_pauses_same_session)
{
    BOOST_CHECK(CanTransition(SessionState::AWAITING_USER_SIGNATURE,
                              SessionState::AWAITING_WALLET_UNLOCK));
    BOOST_CHECK(CanTransition(SessionState::AWAITING_WALLET_UNLOCK,
                              SessionState::AWAITING_USER_SIGNATURE));
}

BOOST_AUTO_TEST_CASE(mempool_and_stempool_are_not_terminal)
{
    BOOST_CHECK(!IsTerminal(SessionState::STEMPOOL));
    BOOST_CHECK(!IsTerminal(SessionState::MEMPOOL));
    BOOST_CHECK(CanTransition(SessionState::MEMPOOL, SessionState::PENDING_PROVIDER));
    BOOST_CHECK(CanTransition(SessionState::MEMPOOL, SessionState::CONFIRMED));
}

BOOST_AUTO_TEST_CASE(terminal_sessions_are_immutable_except_idempotent_replay)
{
    BOOST_CHECK(CanTransition(SessionState::CONFIRMED, SessionState::CONFIRMED));
    BOOST_CHECK(!CanTransition(SessionState::CONFIRMED, SessionState::MEMPOOL));
    BOOST_CHECK(!CanTransition(SessionState::CANCELED_SAFE, SessionState::CONFIRMED));
}

BOOST_AUTO_TEST_CASE(provider_signs_and_commits_in_order)
{
    BOOST_CHECK(CanTransition(AttemptState::USER_PSBT_ACCEPTED, AttemptState::PROVIDER_SIGNED));
    BOOST_CHECK(CanTransition(AttemptState::PROVIDER_SIGNED, AttemptState::FINAL_COMMITTED));
    BOOST_CHECK(!CanTransition(AttemptState::USER_SIGNED, AttemptState::PROVIDER_SIGNED));
    BOOST_CHECK_EQUAL(AttemptStateName(AttemptState::USER_PSBT_ACCEPTED), "USER_PSBT_ACCEPTED");
}

BOOST_AUTO_TEST_CASE(final_attempt_states_cannot_be_overwritten_by_negative_results)
{
    for (const AttemptState state : {AttemptState::FINAL_COMMITTED,
                                     AttemptState::BROADCAST,
                                     AttemptState::STEMPOOL,
                                     AttemptState::MEMPOOL}) {
        BOOST_CHECK(!CanTransition(state, AttemptState::REJECTED));
        BOOST_CHECK(!CanTransition(state, AttemptState::AMBIGUOUS));
        BOOST_CHECK(CanTransition(state, state));
    }
    BOOST_CHECK(CanTransition(AttemptState::USER_PSBT_ACCEPTED,
                              AttemptState::AMBIGUOUS));
}

BOOST_AUTO_TEST_CASE(absent_capacity_snapshot_is_serializable_but_unknown_model_is_rejected)
{
    ProviderAttempt attempt;
    CDataStream attempt_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    BOOST_CHECK_NO_THROW(attempt_stream << attempt);

    ValidatedCapacitySnapshot snapshot;
    CDataStream snapshot_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    snapshot_stream << snapshot;
    const auto span = MakeUCharSpan(snapshot_stream);
    std::vector<unsigned char> encoded{span.begin(), span.end()};
    BOOST_REQUIRE_GE(encoded.size(), 2U);
    // The V2 funding model is the penultimate field (before the bool). An
    // unknown value received from disk or a peer must remain fail-closed.
    encoded[encoded.size() - 2] = 0xff;
    ValidatedCapacitySnapshot decoded;
    SpanReader reader{::PROTOCOL_VERSION, encoded};
    BOOST_CHECK_THROW(reader >> decoded, std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
