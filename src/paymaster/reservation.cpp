// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Closed state-transition rules for durable client sessions and attempts.
 * Terminal states are immutable and repeated transitions are idempotent,
 * preventing delayed network results from reopening or rewriting authority.
 */

#include <paymaster/reservation.h>

namespace DigiDollar::Paymaster {

bool IsTerminal(SessionState state)
{
    return state == SessionState::CONFIRMED || state == SessionState::FAILED ||
           state == SessionState::CANCELED_SAFE || state == SessionState::CONFLICTED;
}

bool CanTransition(SessionState from, SessionState to)
{
    if (from == to) return true; // idempotent replay
    if (IsTerminal(from)) return false;
    if (to == SessionState::CONFLICTED) return true;

    switch (from) {
    case SessionState::CREATED:
        return to == SessionState::INPUTS_RESERVED || to == SessionState::FAILED;
    case SessionState::INPUTS_RESERVED:
        return to == SessionState::AWAITING_WALLET_UNLOCK ||
               to == SessionState::AWAITING_USER_SIGNATURE ||
               to == SessionState::AUTHORIZED || to == SessionState::FAILED;
    case SessionState::AWAITING_WALLET_UNLOCK:
        return to == SessionState::AWAITING_USER_SIGNATURE || to == SessionState::AUTHORIZED;
    case SessionState::AWAITING_USER_SIGNATURE:
        return to == SessionState::AWAITING_WALLET_UNLOCK ||
               to == SessionState::AUTHORIZED || to == SessionState::FAILED;
    case SessionState::AUTHORIZED:
        return to == SessionState::DGB_COMMITTING || to == SessionState::PENDING_PROVIDER;
    case SessionState::DGB_COMMITTING:
        return to == SessionState::STEMPOOL || to == SessionState::MEMPOOL ||
               to == SessionState::PENDING_PROVIDER;
    case SessionState::STEMPOOL:
        return to == SessionState::MEMPOOL || to == SessionState::CONFIRMED ||
               to == SessionState::PENDING_PROVIDER;
    case SessionState::MEMPOOL:
        return to == SessionState::CONFIRMED || to == SessionState::PENDING_PROVIDER;
    case SessionState::PENDING_PROVIDER:
        return to == SessionState::STEMPOOL || to == SessionState::MEMPOOL ||
               to == SessionState::CONFIRMED || to == SessionState::CANCELED_SAFE;
    case SessionState::CONFIRMED:
    case SessionState::FAILED:
    case SessionState::CANCELED_SAFE:
    case SessionState::CONFLICTED:
        return false;
    }
    return false;
}

bool CanTransition(AttemptState from, AttemptState to)
{
    if (from == to) return true;
    if (to == AttemptState::CONFLICTED) return true;
    if (from == AttemptState::REJECTED || from == AttemptState::QUOTE_EXPIRED ||
        from == AttemptState::CONFLICTED) return false;
    // Once an exact fully signed transaction has become durable, a negative
    // provider result or a retry ambiguity cannot revoke that authorization.
    // Network-observation states may still advance normally (or be reconciled
    // by the store), but must never regress to a state that permits releasing
    // the bound inputs or budget.
    if ((from == AttemptState::FINAL_COMMITTED ||
         from == AttemptState::BROADCAST ||
         from == AttemptState::STEMPOOL ||
         from == AttemptState::MEMPOOL) &&
        (to == AttemptState::REJECTED || to == AttemptState::AMBIGUOUS)) {
        return false;
    }
    if (to == AttemptState::REJECTED || to == AttemptState::AMBIGUOUS) return true;

    switch (from) {
    case AttemptState::CANDIDATE: return to == AttemptState::QUOTED;
    case AttemptState::QUOTED: return to == AttemptState::USER_SIGNED || to == AttemptState::QUOTE_EXPIRED;
    case AttemptState::USER_SIGNED: return to == AttemptState::USER_PSBT_ACCEPTED;
    case AttemptState::USER_PSBT_ACCEPTED: return to == AttemptState::PROVIDER_SIGNED;
    case AttemptState::PROVIDER_SIGNED: return to == AttemptState::FINAL_COMMITTED;
    case AttemptState::FINAL_COMMITTED: return to == AttemptState::BROADCAST;
    case AttemptState::BROADCAST: return to == AttemptState::STEMPOOL || to == AttemptState::MEMPOOL;
    case AttemptState::STEMPOOL: return to == AttemptState::MEMPOOL;
    case AttemptState::MEMPOOL:
    case AttemptState::AMBIGUOUS:
    case AttemptState::REJECTED:
    case AttemptState::QUOTE_EXPIRED:
    case AttemptState::CONFLICTED:
        return false;
    }
    return false;
}

std::string_view SessionStateName(SessionState state)
{
    switch (state) {
    case SessionState::CREATED: return "CREATED";
    case SessionState::INPUTS_RESERVED: return "INPUTS_RESERVED";
    case SessionState::AWAITING_WALLET_UNLOCK: return "AWAITING_WALLET_UNLOCK";
    case SessionState::AWAITING_USER_SIGNATURE: return "AWAITING_USER_SIGNATURE";
    case SessionState::AUTHORIZED: return "AUTHORIZED";
    case SessionState::DGB_COMMITTING: return "DGB_COMMITTING";
    case SessionState::STEMPOOL: return "STEMPOOL";
    case SessionState::MEMPOOL: return "MEMPOOL";
    case SessionState::CONFIRMED: return "CONFIRMED";
    case SessionState::PENDING_PROVIDER: return "PENDING_PROVIDER";
    case SessionState::FAILED: return "FAILED";
    case SessionState::CANCELED_SAFE: return "CANCELED_SAFE";
    case SessionState::CONFLICTED: return "CONFLICTED";
    }
    return "UNKNOWN";
}

std::string_view PendingPhaseName(PendingPhase phase)
{
    switch (phase) {
    case PendingPhase::NONE: return "NONE";
    case PendingPhase::USER_SIGNATURE_SENT: return "USER_SIGNATURE_SENT";
    case PendingPhase::PROVIDER_SIGNED_KNOWN: return "PROVIDER_SIGNED_KNOWN";
    case PendingPhase::PENDING_NETWORK: return "PENDING_NETWORK";
    case PendingPhase::CANCEL_MEMPOOL: return "CANCEL_MEMPOOL";
    }
    return "UNKNOWN";
}

std::string_view AttemptStateName(AttemptState state)
{
    switch (state) {
    case AttemptState::CANDIDATE: return "CANDIDATE";
    case AttemptState::QUOTED: return "QUOTED";
    case AttemptState::USER_SIGNED: return "USER_SIGNED";
    case AttemptState::USER_PSBT_ACCEPTED: return "USER_PSBT_ACCEPTED";
    case AttemptState::PROVIDER_SIGNED: return "PROVIDER_SIGNED";
    case AttemptState::FINAL_COMMITTED: return "FINAL_COMMITTED";
    case AttemptState::BROADCAST: return "BROADCAST";
    case AttemptState::STEMPOOL: return "STEMPOOL";
    case AttemptState::MEMPOOL: return "MEMPOOL";
    case AttemptState::REJECTED: return "REJECTED";
    case AttemptState::QUOTE_EXPIRED: return "QUOTE_EXPIRED";
    case AttemptState::AMBIGUOUS: return "AMBIGUOUS";
    case AttemptState::CONFLICTED: return "CONFLICTED";
    }
    return "UNKNOWN";
}

} // namespace DigiDollar::Paymaster
