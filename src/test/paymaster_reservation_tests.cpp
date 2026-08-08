// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Reservation expiry, authorization-risk, and release invariants. */

#include <boost/test/unit_test.hpp>

#include <paymaster/reservation.h>
#include <streams.h>
#include <version.h>

#include <array>
#include <vector>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_reservation_tests)

namespace {

constexpr std::array SESSION_STATES{
    SessionState::CREATED,
    SessionState::INPUTS_RESERVED,
    SessionState::AWAITING_WALLET_UNLOCK,
    SessionState::AWAITING_USER_SIGNATURE,
    SessionState::AUTHORIZED,
    SessionState::DGB_COMMITTING,
    SessionState::STEMPOOL,
    SessionState::MEMPOOL,
    SessionState::CONFIRMED,
    SessionState::PENDING_PROVIDER,
    SessionState::FAILED,
    SessionState::CANCELED_SAFE,
    SessionState::CONFLICTED,
};

constexpr std::array ATTEMPT_STATES{
    AttemptState::CANDIDATE,
    AttemptState::QUOTED,
    AttemptState::USER_SIGNED,
    AttemptState::USER_PSBT_ACCEPTED,
    AttemptState::PROVIDER_SIGNED,
    AttemptState::FINAL_COMMITTED,
    AttemptState::BROADCAST,
    AttemptState::STEMPOOL,
    AttemptState::MEMPOOL,
    AttemptState::REJECTED,
    AttemptState::QUOTE_EXPIRED,
    AttemptState::AMBIGUOUS,
    AttemptState::CONFLICTED,
};

bool ExpectedSessionTransition(SessionState from, SessionState to)
{
    if (from == to) return true;
    if (from == SessionState::CONFIRMED || from == SessionState::FAILED ||
        from == SessionState::CANCELED_SAFE ||
        from == SessionState::CONFLICTED) {
        return false;
    }
    if (to == SessionState::CONFLICTED) return true;
    switch (from) {
    case SessionState::CREATED:
        return to == SessionState::INPUTS_RESERVED ||
               to == SessionState::FAILED;
    case SessionState::INPUTS_RESERVED:
        return to == SessionState::AWAITING_WALLET_UNLOCK ||
               to == SessionState::AWAITING_USER_SIGNATURE ||
               to == SessionState::AUTHORIZED ||
               to == SessionState::FAILED;
    case SessionState::AWAITING_WALLET_UNLOCK:
        return to == SessionState::AWAITING_USER_SIGNATURE ||
               to == SessionState::AUTHORIZED;
    case SessionState::AWAITING_USER_SIGNATURE:
        return to == SessionState::AWAITING_WALLET_UNLOCK ||
               to == SessionState::AUTHORIZED ||
               to == SessionState::FAILED;
    case SessionState::AUTHORIZED:
        return to == SessionState::DGB_COMMITTING ||
               to == SessionState::PENDING_PROVIDER;
    case SessionState::DGB_COMMITTING:
        return to == SessionState::STEMPOOL ||
               to == SessionState::MEMPOOL ||
               to == SessionState::PENDING_PROVIDER;
    case SessionState::STEMPOOL:
        return to == SessionState::MEMPOOL ||
               to == SessionState::CONFIRMED ||
               to == SessionState::PENDING_PROVIDER;
    case SessionState::MEMPOOL:
        return to == SessionState::CONFIRMED ||
               to == SessionState::PENDING_PROVIDER;
    case SessionState::PENDING_PROVIDER:
        return to == SessionState::STEMPOOL ||
               to == SessionState::MEMPOOL ||
               to == SessionState::CONFIRMED ||
               to == SessionState::CANCELED_SAFE;
    case SessionState::CONFIRMED:
    case SessionState::FAILED:
    case SessionState::CANCELED_SAFE:
    case SessionState::CONFLICTED:
        return false;
    }
    return false;
}

bool ExpectedAttemptTransition(AttemptState from, AttemptState to)
{
    if (from == to) return true;
    if (to == AttemptState::CONFLICTED) return true;
    if (from == AttemptState::REJECTED ||
        from == AttemptState::QUOTE_EXPIRED ||
        from == AttemptState::CONFLICTED) {
        return false;
    }
    if ((from == AttemptState::FINAL_COMMITTED ||
         from == AttemptState::BROADCAST ||
         from == AttemptState::STEMPOOL ||
         from == AttemptState::MEMPOOL) &&
        (to == AttemptState::REJECTED ||
         to == AttemptState::AMBIGUOUS)) {
        return false;
    }
    if (to == AttemptState::REJECTED || to == AttemptState::AMBIGUOUS) {
        return true;
    }
    switch (from) {
    case AttemptState::CANDIDATE:
        return to == AttemptState::QUOTED;
    case AttemptState::QUOTED:
        return to == AttemptState::USER_SIGNED ||
               to == AttemptState::QUOTE_EXPIRED;
    case AttemptState::USER_SIGNED:
        return to == AttemptState::USER_PSBT_ACCEPTED;
    case AttemptState::USER_PSBT_ACCEPTED:
        return to == AttemptState::PROVIDER_SIGNED;
    case AttemptState::PROVIDER_SIGNED:
        return to == AttemptState::FINAL_COMMITTED;
    case AttemptState::FINAL_COMMITTED:
        return to == AttemptState::BROADCAST;
    case AttemptState::BROADCAST:
        return to == AttemptState::STEMPOOL || to == AttemptState::MEMPOOL;
    case AttemptState::STEMPOOL:
        return to == AttemptState::MEMPOOL;
    case AttemptState::MEMPOOL:
    case AttemptState::REJECTED:
    case AttemptState::QUOTE_EXPIRED:
    case AttemptState::AMBIGUOUS:
    case AttemptState::CONFLICTED:
        return false;
    }
    return false;
}

} // namespace

BOOST_AUTO_TEST_CASE(state_transition_matrices_reject_unknown_values)
{
    for (const SessionState from : SESSION_STATES) {
        for (const SessionState to : SESSION_STATES) {
            BOOST_CHECK_EQUAL(CanTransition(from, to),
                              ExpectedSessionTransition(from, to));
        }
    }
    const auto unknown_session = static_cast<SessionState>(0xff);
    BOOST_CHECK(!CanTransition(unknown_session, unknown_session));
    for (const SessionState state : SESSION_STATES) {
        BOOST_CHECK(!CanTransition(unknown_session, state));
        BOOST_CHECK(!CanTransition(state, unknown_session));
    }

    for (const AttemptState from : ATTEMPT_STATES) {
        for (const AttemptState to : ATTEMPT_STATES) {
            BOOST_CHECK_EQUAL(CanTransition(from, to),
                              ExpectedAttemptTransition(from, to));
        }
    }
    const auto unknown_attempt = static_cast<AttemptState>(0xff);
    BOOST_CHECK(!CanTransition(unknown_attempt, unknown_attempt));
    for (const AttemptState state : ATTEMPT_STATES) {
        BOOST_CHECK(!CanTransition(unknown_attempt, state));
        BOOST_CHECK(!CanTransition(state, unknown_attempt));
    }
}

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
