// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider commits, exact results, and client finalization state. */

#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>

#include <chainparams.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <node/context.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/validation.h>
#include <paymaster/wire.h>
#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/time.h>
#include <version.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_store::internal;

bool PaymasterStore::AcceptUserAuthorization(
    const std::string& request_id,
    const uint256& attempt_id,
    const uint256& canonical_psbt_hash,
    int64_t now,
    std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) || attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (attempt.commit_key.IsNull() || attempt.user_signed_psbt.empty() || canonical_psbt_hash.IsNull() ||
        Hash(attempt.user_signed_psbt) != canonical_psbt_hash || now <= 0 || now > attempt.retry_until) {
        error = "PAYMASTER_INVALID_USER_AUTHORIZATION";
        return false;
    }

    UserAuthorizationRecord existing;
    if (batch.ReadPaymasterUserAuthorization(attempt.commit_key, existing)) {
        if (existing.attempt_id == attempt_id && existing.canonical_psbt_hash == canonical_psbt_hash &&
            existing.retry_until == attempt.retry_until) return true;
        error = "PAYMASTER_USER_AUTHORIZATION_CONFLICT";
        return false;
    }
    if (!session.provider_side || attempt.state != AttemptState::USER_SIGNED) {
        error = "PAYMASTER_INVALID_ATTEMPT_TRANSITION";
        return false;
    }

    // The exact quote already has a durable budget reservation. A later
    // wallet-local safety-policy change must not strand a matching user
    // authorization, but it also must not relax any other binding below.
    // Stopping the provider or replacing its advertised policy remains an
    // explicit fail-closed boundary.
    ProviderPolicy advertised_policy;
    ProviderSettings settings;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    if (!batch.ReadPaymasterPolicy(advertised_policy)) {
        error = "PAYMASTER_PROVIDER_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterSettings(settings)) {
        error = "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND";
        return false;
    }
    if (!settings.enabled) {
        error = "PAYMASTER_PROVIDER_NOT_RUNNING";
        return false;
    }
    PaymasterQuoteRequest quote_request;
    PaymasterQuoteResponse quote_response;
    const uint256 advertised_policy_hash{
        GetProviderPolicyHash(advertised_policy)};
    if (!DecodeCanonicalQuoteRequest(attempt.quote_request, quote_request) ||
        !DecodeCanonicalQuoteResponse(attempt.signed_quote, quote_response) ||
        quote_request.intent.policy_hash != advertised_policy_hash ||
        quote_response.quote.policy_hash != advertised_policy_hash ||
        settings.policy_hash != advertised_policy_hash ||
        attempt.intent_hash != GetPaymentIntentHash(quote_request.intent) ||
        quote_response.quote.intent_hash != attempt.intent_hash) {
        error = "PAYMASTER_PROVIDER_POLICY_NOT_CURRENT";
        return false;
    }
    const bool have_safety_policy =
        batch.ReadPaymasterProviderSafetyPolicy(safety_policy);
    const bool have_budget_ledger =
        batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if (!have_safety_policy || !have_budget_ledger) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(
            safety_policy, advertised_policy, error) ||
        !ValidateProviderBudgetState(
            attempt, safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }

    const UserAuthorizationRecord authorization{
        UserAuthorizationRecord::CURRENT_VERSION, attempt.commit_key, attempt.attempt_id,
        canonical_psbt_hash, now, attempt.retry_until};
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterUserAuthorization(authorization, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    attempt.state = AttemptState::USER_PSBT_ACCEPTED;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetUserAuthorization(
    const uint256& commit_key,
    UserAuthorizationRecord& authorization) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterUserAuthorization(commit_key, authorization);
}

bool PaymasterStore::ReserveSponsorshipAuthorization(
    const SponsorshipAuthorizationRecord& authorization,
    std::string& error)
{
    error.clear();
    std::string validation_error;
    if (authorization.state != SponsorshipAuthorizationState::RESERVED ||
        !ValidateSponsorshipAuthorizationRecord(authorization, validation_error)) {
        error = validation_error.empty() ? "PAYMASTER_INVALID_SPONSORSHIP_STATE" : validation_error;
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    SponsorshipAuthorizationRecord existing;
    if (batch.ReadPaymasterSponsorshipAuthorization(authorization.capability_hash, existing)) {
        if (existing.state == SponsorshipAuthorizationState::RESERVED &&
            existing.payment_binding_hash == authorization.payment_binding_hash &&
            existing.reservation_id == authorization.reservation_id &&
            existing.reserved_at == authorization.reserved_at &&
            existing.expires_at == authorization.expires_at) return true;
        error = "PAYMASTER_SPONSORSHIP_ALREADY_USED";
        return false;
    }
    if (!batch.WritePaymasterSponsorshipAuthorization(authorization, false)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::GetSponsorshipAuthorization(
    const uint256& capability_hash,
    SponsorshipAuthorizationRecord& authorization) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterSponsorshipAuthorization(
        capability_hash, authorization);
}

bool PaymasterStore::CommitProviderFinalTransaction(
    const std::string& request_id,
    const uint256& attempt_id,
    const ProviderCommitRecord& commit,
    const PaymasterResult& result,
    const uint256& expected_genesis,
    std::string& error)
{
    return CommitProviderFinalTransaction(
        request_id, attempt_id, commit, std::nullopt,
        result, expected_genesis, error);
}

bool PaymasterStore::CommitProviderFinalTransaction(
    const std::string& request_id,
    const uint256& attempt_id,
    const ProviderCommitRecord& commit,
    const std::optional<uint256>& sponsorship_capability_hash,
    const PaymasterResult& result,
    const uint256& expected_genesis,
    std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterIdentity(identity) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    UserAuthorizationRecord authorization;
    if (!batch.ReadPaymasterUserAuthorization(attempt.commit_key, authorization) ||
        authorization.attempt_id != attempt.attempt_id ||
        authorization.canonical_psbt_hash != Hash(attempt.user_signed_psbt)) {
        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
        return false;
    }

    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    const bool have_safety_policy = batch.ReadPaymasterProviderSafetyPolicy(safety_policy);
    const bool have_budget_ledger = batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if ((!have_safety_policy && batch.HasPaymasterProviderSafetyPolicy()) ||
        (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger())) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    bool budget_changed{false};
    const auto spend_budget = [&](int64_t committed_at) {
        if (!have_safety_policy || !have_budget_ledger) {
            error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
            return false;
        }
        const auto reservation = std::find_if(
            budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation == budget_ledger.reservations.end()) {
            error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
            return false;
        }
        if ((reservation->state != BudgetReservationState::RESERVED &&
             reservation->state != BudgetReservationState::SPENT) ||
            !ValidateProviderBudgetState(
                attempt, safety_policy, budget_ledger, reservation->state,
                /*allow_historical_policy=*/true, error)) {
            if (error.empty()) {
                error = "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH";
            }
            return false;
        }
        const BudgetReservationState previous_state = reservation->state;
        if (!SpendProviderBudget(budget_ledger, attempt.commit_key,
                                 committed_at, error)) {
            return false;
        }
        budget_changed = previous_state != BudgetReservationState::SPENT;
        return true;
    };

    ProviderCommitRecord existing;
    const bool have_existing_commit =
        batch.ReadPaymasterProviderCommit(commit.commit_key, existing);
    PaymasterResult signed_envelope;
    std::string envelope_error;
    const bool have_signed_envelope =
        DecodeProviderSignedResultEnvelope(
            attempt, signed_envelope, envelope_error);
    const bool exact_signed_envelope =
        have_signed_envelope &&
        CanonicalBytes(result) == attempt.provider_signed_result;
    if (!ValidatePaymasterResult(result, expected_genesis, commit.provider_id,
                                 commit.commit_key, identity.identity_key,
                                 1, error) ||
        result.result_sequence != 1 ||
        result.status != PaymasterResultStatus::FINAL_COMMITTED ||
        !exact_signed_envelope || !result.txid ||
        *result.txid != commit.final_txid || !result.raw_transaction_hash ||
        *result.raw_transaction_hash != commit.raw_transaction_hash ||
        !result.final_transaction ||
        SerializeResultTransaction(*result.final_transaction) !=
            commit.final_transaction) {
        if (error.empty()) error = "PAYMASTER_RESULT_DOES_NOT_MATCH_FINAL_COMMIT";
        return false;
    }
    // The first atomic commit is a financial authorization boundary: prove
    // every provider input and return script is still wallet-owned before the
    // budget is spent and the final transaction becomes durable.
    if (attempt.provider_manifest.manifest_id.IsNull() ||
        !ValidateProviderAuthorizationOwnership(
            m_wallet, attempt.provider_manifest, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        }
        return false;
    }

    CMutableTransaction mutable_transaction;
    try {
        SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
        stream >> mutable_transaction;
        if (!stream.empty()) {
            error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    const CTransaction transaction{mutable_transaction};

    std::vector<ProviderPoolEntry> pool_entries;
    const DatabaseReadStatus pool_status{
        batch.ReadPaymasterProviderPoolWithStatus(pool_entries)};
    if (pool_status != DatabaseReadStatus::FOUND) {
        error = pool_status == DatabaseReadStatus::NOT_FOUND
                    ? "PAYMASTER_PROVIDER_POOL_COMMIT_MISMATCH"
                    : ProviderPoolReadError(pool_entries);
        return false;
    }
    ProviderPolicy provider_policy;
    const std::optional<ProviderPolicy> successor_policy =
        batch.ReadPaymasterPolicy(provider_policy)
            ? std::optional<ProviderPolicy>{provider_policy}
            : std::nullopt;
    ProviderFinanceLedger finance_ledger;
    bool finance_changed{false};
    if (!PrepareProviderTransferFinanceEvent(
            batch, identity, attempt, commit, expected_genesis,
            finance_ledger, finance_changed, error)) {
        return false;
    }

    PaymasterResult existing_result;
    const bool have_existing_result =
        batch.ReadPaymasterResult(commit.commit_key, existing_result);
    if (!have_existing_result && batch.HasPaymasterResult(commit.commit_key)) {
        error = "PAYMASTER_PERSISTED_RESULT_CORRUPT";
        return false;
    }
    if (have_existing_commit) {
        if (!SameCommit(existing, commit)) {
            error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
            return false;
        }
        if (have_existing_result && !SameResult(existing_result, result)) {
            error = "PAYMASTER_PROVIDER_RESULT_CONFLICT";
            return false;
        }
        if (sponsorship_capability_hash) {
            SponsorshipAuthorizationRecord committed_sponsorship;
            if (!batch.ReadPaymasterSponsorshipAuthorization(
                    *sponsorship_capability_hash, committed_sponsorship) ||
                committed_sponsorship.state !=
                    SponsorshipAuthorizationState::CONSUMED ||
                committed_sponsorship.reservation_id != commit.commit_key) {
                error = "PAYMASTER_SPONSORSHIP_COMMIT_MISSING";
                return false;
            }
        }
        if (!spend_budget(existing.committed_at)) return false;
        bool pool_changed{false};
        if (!RegisterProviderPoolSuccessors(
                attempt, existing, transaction, successor_policy,
                pool_entries, pool_changed, error)) {
            return false;
        }
        if (have_existing_result && !budget_changed && !pool_changed &&
            !finance_changed) return true;

        // Repair a crash window where the commit became durable before the
        // initial result, budget, pool, or finance update completed.
        if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if ((!have_existing_result &&
             !batch.WritePaymasterResult(result, false)) ||
            (budget_changed &&
             !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) ||
            (pool_changed &&
             !batch.WritePaymasterProviderPool(pool_entries)) ||
            (finance_changed &&
             !batch.WritePaymasterFinanceLedger(finance_ledger))) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
        return true;
    }

    SponsorshipAuthorizationRecord sponsorship;
    if (sponsorship_capability_hash) {
        if (!batch.ReadPaymasterSponsorshipAuthorization(*sponsorship_capability_hash, sponsorship) ||
            sponsorship.state != SponsorshipAuthorizationState::RESERVED ||
            sponsorship.reservation_id != commit.commit_key ||
            (sponsorship.expires_at < commit.committed_at &&
             (!HasTimelyDurableProviderSignature(attempt) ||
              sponsorship.expires_at < attempt.provider_signed_at))) {
            error = "PAYMASTER_SPONSORSHIP_AUTHORIZATION_MISSING";
            return false;
        }
    }

    if (commit.version != ProviderCommitRecord::CURRENT_VERSION || commit.commit_key.IsNull() ||
        commit.provider_id.IsNull() || commit.quote_id.IsNull() || commit.template_commitment.IsNull() ||
        commit.final_txid.IsNull() || commit.raw_transaction_hash.IsNull() ||
        commit.final_transaction.empty() || commit.provider_inputs.empty() || commit.committed_at <= 0 ||
        commit.committed_at < attempt.provider_signed_at ||
        !HasTimelyDurableProviderSignature(attempt) ||
        Hash(commit.final_transaction) != commit.raw_transaction_hash ||
        transaction.GetHash() != commit.final_txid || attempt.state != AttemptState::PROVIDER_SIGNED ||
        attempt.commit_key != commit.commit_key || attempt.provider_id != commit.provider_id ||
        attempt.quote_id != commit.quote_id || attempt.template_commitment != commit.template_commitment ||
        attempt.commit_key != GetPaymasterCommitKey(attempt.provider_id, attempt.client_nonce,
                                                    attempt.intent_hash, attempt.quote_id,
                                                    attempt.template_commitment) ||
        attempt.final_txid != commit.final_txid || attempt.final_transaction != commit.final_transaction) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    std::vector<COutPoint> expected_provider_inputs;
    if (attempt.input_roles.size() != transaction.vin.size() ||
        commit.final_txid != attempt.unsigned_txid) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    for (size_t index = 0; index < attempt.input_roles.size(); ++index) {
        const ReservationRole role = attempt.input_roles[index];
        if (role == ReservationRole::PROVIDER_CARRIER || role == ReservationRole::PROVIDER_DGB) {
            expected_provider_inputs.push_back(transaction.vin[index].prevout);
        }
    }
    if (expected_provider_inputs.empty() || commit.provider_inputs != expected_provider_inputs) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }

    {
        std::set<COutPoint> committed_inputs(commit.provider_inputs.begin(), commit.provider_inputs.end());
        size_t bound_entries{0};
        for (const ProviderPoolEntry& entry : pool_entries) {
            if (entry.reservation_id != commit.commit_key) continue;
            ++bound_entries;
            if (entry.purpose != PoolPurpose::OPERATIONAL ||
                entry.state != PoolEntryState::RESERVED ||
                committed_inputs.erase(entry.outpoint) != 1) {
                error = "PAYMASTER_PROVIDER_POOL_COMMIT_MISMATCH";
                return false;
            }
        }
        if (bound_entries != commit.provider_inputs.size() || !committed_inputs.empty()) {
            error = "PAYMASTER_PROVIDER_POOL_COMMIT_MISMATCH";
            return false;
        }
    }
    if (!spend_budget(commit.committed_at)) return false;

    bool pool_changed{false};
    if (!RegisterProviderPoolSuccessors(
            attempt, commit, transaction, successor_policy,
            pool_entries, pool_changed, error)) {
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterProviderCommit(commit, false) ||
        !batch.WritePaymasterResult(result, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (sponsorship_capability_hash) {
        sponsorship.state = SponsorshipAuthorizationState::CONSUMED;
        sponsorship.consumed_at = sponsorship.expires_at < commit.committed_at ? attempt.provider_signed_at : commit.committed_at;
        if (!batch.WritePaymasterSponsorshipAuthorization(sponsorship, true)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    {
        for (ProviderPoolEntry& entry : pool_entries) {
            if (entry.reservation_id == commit.commit_key) {
                if (entry.origin_commit_key == commit.commit_key) continue;
                entry.state = PoolEntryState::COMMITTED;
                entry.updated_at = commit.committed_at;
                pool_changed = true;
            }
        }
        if (pool_changed && !batch.WritePaymasterProviderPool(pool_entries)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (finance_changed &&
        !batch.WritePaymasterFinanceLedger(finance_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    attempt.state = AttemptState::FINAL_COMMITTED;
    attempt.updated_at = std::max(attempt.updated_at, commit.committed_at);
    if (!batch.WritePaymasterAttempt(attempt)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::PENDING_NETWORK;
    session.final_txid = commit.final_txid;
    session.updated_at = std::max(session.updated_at, commit.committed_at);
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetProviderCommit(
    const uint256& commit_key,
    ProviderCommitRecord& commit) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterProviderCommit(commit_key, commit);
}

bool PaymasterStore::ListProviderCommits(
    std::vector<ProviderCommitRecord>& commits) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ListPaymasterProviderCommits(commits);
}

bool PaymasterStore::ReconcileProviderPoolSuccessors(size_t& recovered,
                                                     std::string& error)
{
    recovered = 0;
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<ProviderPoolEntry> pool_entries;
    const DatabaseReadStatus pool_status =
        batch.ReadPaymasterProviderPoolWithStatus(pool_entries);
    if (pool_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (pool_status != DatabaseReadStatus::FOUND) {
        error = ProviderPoolReadError(pool_entries);
        return false;
    }

    std::vector<ProviderCommitRecord> commits;
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterProviderCommits(commits) ||
        !batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_POOL_RECONCILIATION_READ_FAILED";
        return false;
    }
    ProviderPolicy provider_policy;
    const DatabaseReadStatus policy_status =
        batch.ReadPaymasterPolicyWithStatus(provider_policy);
    if (policy_status != DatabaseReadStatus::FOUND &&
        policy_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            policy_status, "ProviderPolicy", provider_policy,
            "PAYMASTER_POLICY_NOT_FOUND", "PAYMASTER_INVALID_PROVIDER_POLICY");
        return false;
    }
    const std::optional<ProviderPolicy> successor_policy =
        policy_status == DatabaseReadStatus::FOUND
            ? std::optional<ProviderPolicy>{provider_policy}
            : std::nullopt;

    std::map<uint256, ProviderAttempt> attempts_by_commit;
    for (const PaymentSession& session : sessions) {
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            const DatabaseReadStatus attempt_status =
                batch.ReadPaymasterAttemptWithStatus(attempt_id, attempt);
            if (attempt_status != DatabaseReadStatus::FOUND) {
                error = PersistedReadError(
                    attempt_status, "ProviderAttempt", attempt,
                    "PAYMASTER_POOL_RECONCILIATION_ATTEMPT_MISSING",
                    "PAYMASTER_POOL_RECONCILIATION_ATTEMPT_READ_FAILED");
                return false;
            }
            if (attempt.commit_key.IsNull() ||
                attempt.provider_manifest.manifest_id.IsNull() ||
                attempt.provider_manifest.manifest_id !=
                    GetProviderAuthorizationManifestId(attempt.provider_manifest)) {
                continue;
            }
            attempts_by_commit.emplace(attempt.commit_key, std::move(attempt));
        }
    }

    bool changed{false};
    for (const ProviderCommitRecord& commit : commits) {
        const auto found = attempts_by_commit.find(commit.commit_key);
        if (found == attempts_by_commit.end()) continue;
        const ProviderAttempt& attempt = found->second;
        if (attempt.final_txid != commit.final_txid ||
            attempt.final_transaction != commit.final_transaction ||
            attempt.provider_id != commit.provider_id ||
            attempt.template_commitment != commit.template_commitment) {
            continue;
        }
        const auto source_present = [&](const COutPoint& outpoint) {
            return std::any_of(pool_entries.begin(), pool_entries.end(),
                               [&](const ProviderPoolEntry& entry) {
                                   return entry.outpoint == outpoint;
                               });
        };
        if ((!attempt.provider_manifest.provider_carrier_inputs.empty() &&
             !std::all_of(attempt.provider_manifest.provider_carrier_inputs.begin(),
                          attempt.provider_manifest.provider_carrier_inputs.end(),
                          source_present)) ||
            !std::all_of(attempt.provider_manifest.provider_dgb_inputs.begin(),
                         attempt.provider_manifest.provider_dgb_inputs.end(),
                         source_present)) {
            continue;
        }

        CMutableTransaction mutable_transaction;
        try {
            SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
            stream >> mutable_transaction;
            if (!stream.empty()) continue;
        } catch (const std::ios_base::failure&) {
            continue;
        }
        if (CTransaction{mutable_transaction}.GetHash() != commit.final_txid) continue;

        bool commit_changed{false};
        std::string commit_error;
        if (!RegisterProviderPoolSuccessors(
                attempt, commit, CTransaction{mutable_transaction},
                successor_policy, pool_entries, commit_changed, commit_error)) {
            // A malformed historical record is isolated from independent
            // valid commits. Exact outpoint conflicts remain fail-closed.
            if (commit_error == "PAYMASTER_PROVIDER_SUCCESSOR_CONFLICT") {
                error = commit_error;
                return false;
            }
            continue;
        }
        if (commit_changed) {
            changed = true;
            ++recovered;
        }
    }
    if (!changed) return true;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterProviderPool(pool_entries)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::ListProviderSignedAttemptsWithoutCommit(
    std::vector<ProviderAttempt>& attempts) const
{
    attempts.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterSessions(sessions)) return false;
    for (const PaymentSession& session : sessions) {
        if (!session.provider_side) continue;
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                attempt.session_id != session.session_id) {
                // A corrupt reference must not suppress recovery of unrelated
                // provider-signed attempts. The individual entry remains
                // unusable and is skipped fail-closed.
                continue;
            }
            if (attempt.state != AttemptState::PROVIDER_SIGNED) continue;
            ProviderCommitRecord commit;
            const DatabaseReadStatus commit_status{
                batch.ReadPaymasterProviderCommitWithStatus(
                    attempt.commit_key, commit)};
            if (commit_status == DatabaseReadStatus::NOT_FOUND) {
                attempts.push_back(std::move(attempt));
            } else if (commit_status != DatabaseReadStatus::FOUND) {
                return false;
            }
        }
    }
    return true;
}

bool PaymasterStore::StoreProviderResult(const PaymasterResult& result,
                                         const uint256& expected_genesis,
                                         std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderCommitRecord commit;
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterProviderCommit(result.commit_key, commit) ||
        !batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_RESULT_COMMIT_OR_IDENTITY_MISSING";
        return false;
    }
    PaymasterResult existing;
    const bool have_existing = batch.ReadPaymasterResult(result.commit_key, existing);
    if (!have_existing && batch.HasPaymasterResult(result.commit_key)) {
        error = "PAYMASTER_PERSISTED_RESULT_CORRUPT";
        return false;
    }
    if (!ValidatePaymasterResult(result, expected_genesis, commit.provider_id,
                                 commit.commit_key, identity.identity_key,
                                 1, error)) return false;
    if (result.status != PaymasterResultStatus::FINAL_COMMITTED &&
        result.status != PaymasterResultStatus::BROADCAST_ATTEMPTED) {
        error = "PAYMASTER_RESULT_DOES_NOT_MATCH_FINAL_COMMIT";
        return false;
    }
    const CTransaction transaction{*result.final_transaction};
    const std::vector<unsigned char> final_transaction =
        SerializeResultTransaction(*result.final_transaction);
    if (*result.txid != commit.final_txid || transaction.GetHash() != commit.final_txid ||
        *result.raw_transaction_hash != commit.raw_transaction_hash ||
        final_transaction != commit.final_transaction) {
        error = "PAYMASTER_RESULT_DOES_NOT_MATCH_FINAL_COMMIT";
        return false;
    }
    if (have_existing) {
        if (SameResult(result, existing)) return true;
        if (!ValidateResultProgression(existing, result,
                                       /*allow_negative_to_final=*/false,
                                       error)) return false;
    }
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterResult(result, have_existing)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::StoreClientResult(const PaymasterResult& result,
                                       const uint256& expected_genesis,
                                       const uint256& attempt_id,
                                       int64_t now,
                                       std::string& error)
{
    error.clear();
    if (now <= 0) {
        error = "PAYMASTER_INVALID_RESULT_TIME";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    PaymentSession session;
    std::string request_id;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.provider_id.IsNull() || !attempt.provider_identity_key.IsFullyValid() ||
        attempt.commit_key.IsNull() || attempt.unsigned_txid.IsNull() ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        session.session_id != attempt.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_CLIENT_RESULT_BINDING_MISSING";
        return false;
    }
    if (!ValidatePaymasterResult(result, expected_genesis, attempt.provider_id,
                                 attempt.commit_key, attempt.provider_identity_key,
                                 1, error)) {
        return false;
    }
    const bool final_result = IsFinalResultStatus(result.status);
    const bool negative_result = IsNegativeTerminalResultStatus(result.status);
    const bool attempt_has_user_authorization =
        AttemptHasReached(attempt.state, AttemptState::USER_SIGNED) ||
        attempt.state == AttemptState::AMBIGUOUS;
    const bool has_current_accepted_client_manifest =
        attempt.client_manifest.version ==
            ClientAuthorizationManifest::CURRENT_VERSION &&
        !attempt.client_manifest.manifest_id.IsNull() &&
        attempt.accepted_client_manifest_id ==
            attempt.client_manifest.manifest_id &&
        attempt.created_at > 0 &&
        attempt.client_manifest_accepted_at > 0 &&
        attempt.client_manifest_accepted_at >= attempt.created_at &&
        attempt.client_manifest_accepted_at <= now &&
        attempt.client_manifest_accepted_at <= attempt.updated_at &&
        attempt.client_manifest_accepted_at <= session.updated_at &&
        attempt.client_manifest_accepted_at <= attempt.retry_until &&
        attempt.client_manifest_accepted_at <=
            attempt.client_manifest.expires_at &&
        attempt.capacity_snapshot.version ==
            ValidatedCapacitySnapshot::CURRENT_VERSION &&
        attempt.capacity_snapshot.validated_at > 0 &&
        attempt.capacity_snapshot.expires_at > 0 &&
        attempt.client_manifest_accepted_at >=
            attempt.capacity_snapshot.validated_at &&
        attempt.client_manifest_accepted_at <=
            attempt.capacity_snapshot.expires_at;
    const bool final_artifact_known = AttemptHasReached(attempt.state, AttemptState::FINAL_COMMITTED) ||
                                      !attempt.final_txid.IsNull() ||
                                      !attempt.final_transaction.empty();
    if ((session.state == SessionState::CONFIRMED || final_artifact_known) && !final_result) {
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    if ((attempt.state == AttemptState::REJECTED ||
         attempt.state == AttemptState::QUOTE_EXPIRED ||
         attempt.state == AttemptState::CONFLICTED ||
         (IsTerminal(session.state) && session.state != SessionState::CONFIRMED)) &&
        final_result) {
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }

    std::vector<unsigned char> final_transaction;
    if (result.final_transaction) {
        const CTransaction transaction{*result.final_transaction};
        final_transaction = SerializeResultTransaction(*result.final_transaction);
        if (!result.txid || *result.txid != attempt.unsigned_txid ||
            transaction.GetHash() != attempt.unsigned_txid) {
            error = "PAYMASTER_RESULT_TEMPLATE_MISMATCH";
            return false;
        }
        if ((!attempt.final_txid.IsNull() && attempt.final_txid != *result.txid) ||
            (!attempt.final_transaction.empty() && attempt.final_transaction != final_transaction) ||
            (!session.final_txid.IsNull() && session.final_txid != *result.txid)) {
            error = "PAYMASTER_FINAL_TX_CONFLICT";
            return false;
        }
    }

    PaymasterResult existing;
    const bool have_existing =
        batch.ReadPaymasterResult(result.commit_key, existing);
    if (!have_existing && batch.HasPaymasterResult(result.commit_key)) {
        error = "PAYMASTER_PERSISTED_RESULT_CORRUPT";
        return false;
    }
    const bool exact_replay = have_existing && SameResult(result, existing);
    if (final_result) {
        // A result received now is remote input. Only an explicitly accepted
        // current manifest may authorize state progression or create
        // client-side authorization evidence.
        if (!has_current_accepted_client_manifest) {
            error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
            return false;
        }
        PaymasterQuoteRequest authorized_request;
        PaymasterQuoteResponse authorized_response;
        CollaborativePSBTTemplate trusted_template;
        if (!LoadAttemptAuthorizationArtifacts(
                attempt, authorized_request, authorized_response,
                trusted_template, error)) {
            return false;
        }
        if (!ValidateClientAuthorizationManifest(
                attempt.client_manifest, authorized_request.intent,
                authorized_response.quote, attempt.capacity_snapshot,
                trusted_template, error) ||
            !ValidateClientAuthorizationOwnership(
                m_wallet, attempt.client_manifest, error)) {
            return false;
        }
        if (!ValidateFinalCollaborativeTransaction(
                *result.final_transaction, *result.txid,
                *result.raw_transaction_hash, trusted_template, error)) {
            if (error.empty()) error = "PAYMASTER_CLIENT_RESULT_AUTHORIZATION_INVALID";
            return false;
        }
    }

    if (!exact_replay &&
        (attempt.state == AttemptState::REJECTED ||
         attempt.state == AttemptState::QUOTE_EXPIRED ||
         attempt.state == AttemptState::CONFLICTED ||
         (IsTerminal(session.state) && session.state != SessionState::CONFIRMED))) {
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    if (have_existing && !exact_replay &&
        !ValidateResultProgression(
            existing, result,
            /*allow_negative_to_final=*/final_result &&
                attempt_has_user_authorization,
            error)) {
        return false;
    }

    UserAuthorizationRecord authorization;
    const bool have_authorization =
        batch.ReadPaymasterUserAuthorization(attempt.commit_key, authorization);
    bool write_authorization{false};
    if (final_result) {
        if (!attempt_has_user_authorization ||
            attempt.user_signed_psbt.empty()) {
            error = "PAYMASTER_RESULT_WITHOUT_USER_AUTHORIZATION";
            return false;
        }
        const uint256 canonical_psbt_hash{Hash(attempt.user_signed_psbt)};
        if (have_authorization) {
            if (authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
                authorization.commit_key != attempt.commit_key ||
                authorization.attempt_id != attempt.attempt_id ||
                authorization.canonical_psbt_hash != canonical_psbt_hash ||
                authorization.retry_until != attempt.retry_until) {
                error = "PAYMASTER_USER_AUTHORIZATION_CONFLICT";
                return false;
            }
        } else {
            if (attempt.retry_until <= 0 || attempt.updated_at <= 0) {
                error = "PAYMASTER_INVALID_USER_AUTHORIZATION";
                return false;
            }
            authorization = {
                UserAuthorizationRecord::CURRENT_VERSION,
                attempt.commit_key,
                attempt.attempt_id,
                canonical_psbt_hash,
                std::min(attempt.updated_at, attempt.retry_until),
                attempt.retry_until,
            };
            write_authorization = true;
        }
    }

    ClientFeeLedger client_fee_ledger;
    bool client_fee_changed{false};
    if (final_result) {
        ClientSafetyPolicy client_policy;
        const bool have_client_policy = batch.ReadPaymasterClientSafetyPolicy(client_policy);
        const bool have_client_ledger = batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
        if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
            (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
            have_client_policy != have_client_ledger) {
            error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
            return false;
        }
        if (have_client_policy) {
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(), client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key == attempt.commit_key;
                });
            if (reservation != client_fee_ledger.reservations.end()) {
                const BudgetReservationState previous_state = reservation->state;
                if (!SpendClientFee(client_fee_ledger, attempt.commit_key, now, error)) {
                    return false;
                }
                client_fee_changed = previous_state != BudgetReservationState::SPENT;
            } else if (attempt.created_at >= client_policy.updated_at) {
                PaymasterQuoteResponse quote_response;
                try {
                    SpanReader quote_stream{::PROTOCOL_VERSION, attempt.signed_quote};
                    quote_stream >> quote_response;
                    if (!quote_stream.empty()) {
                        throw std::ios_base::failure("trailing paymaster quote data");
                    }
                } catch (const std::ios_base::failure&) {
                    error = "PAYMASTER_QUOTE_ENCODING";
                    return false;
                }
                if (quote_response.quote.service_fee.value != 0) {
                    error = "PAYMASTER_CLIENT_FEE_RESERVATION_MISSING";
                    return false;
                }
            }
        }
    }

    bool write_attempt{false};
    bool write_session{false};
    if (final_result) {
        if (attempt.final_txid.IsNull()) {
            attempt.final_txid = *result.txid;
            write_attempt = true;
        }
        if (attempt.final_transaction.empty()) {
            attempt.final_transaction = final_transaction;
            write_attempt = true;
        }
        if (session.final_txid.IsNull()) {
            session.final_txid = *result.txid;
            write_session = true;
        }
        if (attempt.state == AttemptState::USER_SIGNED ||
            attempt.state == AttemptState::USER_PSBT_ACCEPTED ||
            attempt.state == AttemptState::AMBIGUOUS) {
            attempt.state = AttemptState::PROVIDER_SIGNED;
            write_attempt = true;
        }
        if (session.state == SessionState::AUTHORIZED ||
            session.state == SessionState::PENDING_PROVIDER) {
            if (session.state != SessionState::PENDING_PROVIDER ||
                session.pending_phase != PendingPhase::PROVIDER_SIGNED_KNOWN) {
                session.state = SessionState::PENDING_PROVIDER;
                session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
                write_session = true;
            }
        } else if (session.state != SessionState::STEMPOOL &&
                   session.state != SessionState::MEMPOOL &&
                   session.state != SessionState::CONFIRMED) {
            error = "PAYMASTER_INVALID_SESSION_TRANSITION";
            return false;
        }
    } else if (negative_result) {
        if (attempt_has_user_authorization) {
            // A hostile provider can sign a negative response and still sign
            // the already authorized transaction later. Keep the inputs and
            // session recoverable instead of treating that response as proof
            // that spending risk disappeared.
            if (attempt.state != AttemptState::AMBIGUOUS) {
                attempt.state = AttemptState::AMBIGUOUS;
                write_attempt = true;
            }
            if (session.state == SessionState::AUTHORIZED ||
                session.state == SessionState::PENDING_PROVIDER) {
                if (session.state != SessionState::PENDING_PROVIDER ||
                    session.pending_phase == PendingPhase::NONE) {
                    session.state = SessionState::PENDING_PROVIDER;
                    session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
                    write_session = true;
                }
            } else if (!HasAuthorizationRisk(session.state)) {
                error = "PAYMASTER_INVALID_SESSION_TRANSITION";
                return false;
            }
        } else if (attempt.state != AttemptState::REJECTED) {
            if (!CanTransition(attempt.state, AttemptState::REJECTED)) {
                error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
                return false;
            }
            attempt.state = AttemptState::REJECTED;
            write_attempt = true;
        }
    }

    const bool authorization_risk = final_result || attempt_has_user_authorization;
    const uint64_t current_wallet_flags = m_wallet.GetWalletFlags();
    const uint64_t protected_wallet_flags =
        current_wallet_flags | WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    const bool protect_wallet = authorization_risk &&
                                protected_wallet_flags != current_wallet_flags;
    std::vector<InputReservation> reservations;
    bool write_reservations{false};
    if (authorization_risk && !session.provider_side) {
        reservations.reserve(session.user_inputs.size());
        for (const COutPoint& outpoint : session.user_inputs) {
            InputReservation reservation;
            if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
                reservation.request_id != request_id ||
                reservation.session_id != session.session_id ||
                reservation.role != ReservationRole::USER_DD) {
                error = "PAYMASTER_RESERVATION_MISSING";
                return false;
            }
            if (!reservation.authorization_may_exist) {
                reservation.authorization_may_exist = true;
                write_reservations = true;
            }
            reservations.push_back(std::move(reservation));
        }
    }
    if (write_attempt) attempt.updated_at = std::max(attempt.updated_at, now);
    if (write_session) session.updated_at = std::max(session.updated_at, now);

    if (exact_replay && !write_authorization && !write_attempt && !write_session &&
        !write_reservations && !protect_wallet && !client_fee_changed) return true;
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!exact_replay && !batch.WritePaymasterResult(result, have_existing)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (write_authorization &&
        !batch.WritePaymasterUserAuthorization(authorization, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (write_attempt && !batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (write_session && !batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (client_fee_changed && !batch.WritePaymasterClientFeeLedger(client_fee_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (write_reservations) {
        for (const InputReservation& reservation : reservations) {
            if (!batch.WritePaymasterReservation(reservation)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
    }
    if (protect_wallet &&
        !batch.WriteWalletFlags(protected_wallet_flags)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    // The durable flag was part of the transaction above. Publish that exact
    // committed value to memory without issuing a second fallible DB write.
    if (protect_wallet && !m_wallet.LoadWalletFlags(protected_wallet_flags)) {
        assert(false);
    }
    return true;
}

bool PaymasterStore::MarkClientFinalValidationFailureRecoverable(
    const std::string& request_id,
    const uint256& attempt_id,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || attempt_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_FINAL_VALIDATION_FAILURE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_CLIENT_FINAL_FAILURE_BINDING_MISSING";
        return false;
    }

    // A stale validation failure racing with a valid final result, mempool
    // observation, confirmation or safe cancellation must never move either
    // durable state backwards. Attempts that never reached a user signature
    // likewise carry no authorization risk for this operation to record.
    const bool attempt_advanced =
        attempt.state == AttemptState::PROVIDER_SIGNED ||
        attempt.state == AttemptState::FINAL_COMMITTED ||
        attempt.state == AttemptState::BROADCAST ||
        attempt.state == AttemptState::STEMPOOL ||
        attempt.state == AttemptState::MEMPOOL;
    const bool session_advanced =
        session.state == SessionState::STEMPOOL ||
        session.state == SessionState::MEMPOOL ||
        session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE ||
        session.state == SessionState::CONFLICTED;
    if (attempt_advanced || session_advanced ||
        attempt.state == AttemptState::CANDIDATE ||
        attempt.state == AttemptState::QUOTED ||
        attempt.state == AttemptState::REJECTED ||
        attempt.state == AttemptState::QUOTE_EXPIRED ||
        attempt.state == AttemptState::CONFLICTED) {
        return true;
    }
    if (attempt.state != AttemptState::USER_SIGNED &&
        attempt.state != AttemptState::USER_PSBT_ACCEPTED &&
        attempt.state != AttemptState::AMBIGUOUS) {
        error = "PAYMASTER_INVALID_FINAL_VALIDATION_FAILURE_STATE";
        return false;
    }
    if (attempt.user_signed_psbt.empty() ||
        attempt.client_manifest.manifest_id.IsNull() ||
        attempt.accepted_client_manifest_id !=
            attempt.client_manifest.manifest_id ||
        attempt.client_manifest_accepted_at <= 0) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
        return false;
    }
    if (session.state != SessionState::AUTHORIZED &&
        session.state != SessionState::PENDING_PROVIDER) {
        error = "PAYMASTER_INVALID_FINAL_VALIDATION_FAILURE_STATE";
        return false;
    }

    std::vector<InputReservation> reservations;
    reservations.reserve(session.user_inputs.size());
    bool write_reservations{false};
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.request_id != request_id ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD) {
            error = "PAYMASTER_RESERVATION_MISSING";
            return false;
        }
        if (!reservation.authorization_may_exist) {
            reservation.authorization_may_exist = true;
            write_reservations = true;
        }
        reservations.push_back(std::move(reservation));
    }

    const uint64_t current_wallet_flags = m_wallet.GetWalletFlags();
    const uint64_t protected_wallet_flags =
        current_wallet_flags | WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    const bool protect_wallet = protected_wallet_flags != current_wallet_flags;
    const bool write_attempt = attempt.state != AttemptState::AMBIGUOUS;
    const bool write_session =
        session.state != SessionState::PENDING_PROVIDER ||
        session.pending_phase != PendingPhase::PENDING_NETWORK;
    if (!write_attempt && !write_session && !write_reservations &&
        !protect_wallet) {
        return true;
    }
    if (write_attempt) {
        attempt.state = AttemptState::AMBIGUOUS;
        attempt.updated_at = std::max(attempt.updated_at, now);
    }
    if (write_session) {
        session.state = SessionState::PENDING_PROVIDER;
        session.pending_phase = PendingPhase::PENDING_NETWORK;
        session.updated_at = std::max(session.updated_at, now);
    }

    if (!batch.TxnBegin()) {
        return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    }
    if ((write_attempt && !batch.WritePaymasterAttempt(attempt)) ||
        (write_session && !batch.WritePaymasterSession(session))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (write_reservations) {
        for (const InputReservation& reservation : reservations) {
            if (!batch.WritePaymasterReservation(reservation)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
    }
    if (protect_wallet && !batch.WriteWalletFlags(protected_wallet_flags)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    if (protect_wallet && !m_wallet.LoadWalletFlags(protected_wallet_flags)) {
        assert(false);
    }
    return true;
}

bool PaymasterStore::RecordClientFinalConflict(const std::string& request_id,
                                               const uint256& attempt_id,
                                               const uint256& expected_txid,
                                               const uint256& expected_wtxid,
                                               int64_t now,
                                               std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || attempt_id.IsNull() ||
        expected_txid.IsNull() || expected_wtxid.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_FINAL_CONFLICT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    PaymasterResult result;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
            session.attempt_ids.end() ||
        attempt.final_txid != expected_txid ||
        session.final_txid != expected_txid ||
        !batch.ReadPaymasterResult(attempt.commit_key, result)) {
        error = batch.HasPaymasterResult(attempt.commit_key) ? "PAYMASTER_FINAL_CONFLICT_BINDING_MISMATCH" : "PAYMASTER_FINAL_RESULT_MISSING";
        return false;
    }

    ExactFinalArtifact artifact;
    if (!DecodeExactFinalArtifact(attempt.final_transaction, expected_txid,
                                  artifact, error)) {
        return false;
    }
    if (artifact.wtxid != expected_wtxid || !IsFinalResultStatus(result.status) ||
        result.provider_id != attempt.provider_id ||
        result.commit_key != attempt.commit_key || !result.txid ||
        *result.txid != expected_txid || !result.raw_transaction_hash ||
        *result.raw_transaction_hash != expected_wtxid ||
        !result.final_transaction ||
        SerializeResultTransaction(*result.final_transaction) != artifact.bytes) {
        error = "PAYMASTER_FINAL_CONFLICT_BINDING_MISMATCH";
        return false;
    }

    // A conflict is recoverable only while every authorized user input stays
    // bound to this session. Merely changing the state must never unlock or
    // replace these reservations.
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.request_id != request_id ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD ||
            !reservation.authorization_may_exist) {
            error = "PAYMASTER_RESERVATION_MISSING";
            return false;
        }
    }

    // These two states are proved by chain observation and must never be
    // overwritten by a stale post-store failure racing with reconciliation.
    if (session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE) {
        return true;
    }
    if ((session.state == SessionState::CONFLICTED) &&
        attempt.state == AttemptState::CONFLICTED) {
        return true;
    }
    if (IsTerminal(session.state) ||
        !CanTransition(session.state, SessionState::CONFLICTED) ||
        !CanTransition(attempt.state, AttemptState::CONFLICTED)) {
        error = "PAYMASTER_INVALID_FINAL_CONFLICT_TRANSITION";
        return false;
    }

    const bool write_attempt = attempt.state != AttemptState::CONFLICTED;
    const bool write_session = session.state != SessionState::CONFLICTED ||
                               session.pending_phase != PendingPhase::NONE;
    if (write_attempt) {
        attempt.state = AttemptState::CONFLICTED;
        attempt.updated_at = std::max(attempt.updated_at, now);
    }
    if (write_session) {
        session.state = SessionState::CONFLICTED;
        session.pending_phase = PendingPhase::NONE;
        session.updated_at = std::max(session.updated_at, now);
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if ((write_attempt && !batch.WritePaymasterAttempt(attempt)) ||
        (write_session && !batch.WritePaymasterSession(session))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RecordClientFinalObservation(const std::string& request_id,
                                                  const uint256& attempt_id,
                                                  bool confirmed,
                                                  int64_t now,
                                                  std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || attempt_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_FINAL_OBSERVATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    PaymasterResult result;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
            session.attempt_ids.end() ||
        attempt.final_txid.IsNull() || attempt.final_transaction.empty() ||
        session.final_txid != attempt.final_txid ||
        !batch.ReadPaymasterResult(attempt.commit_key, result) ||
        !IsFinalResultStatus(result.status) || !result.txid ||
        *result.txid != attempt.final_txid) {
        error = batch.HasPaymasterResult(attempt.commit_key) ? "PAYMASTER_FINAL_OBSERVATION_BINDING_MISMATCH" : "PAYMASTER_FINAL_RESULT_MISSING";
        return false;
    }
    if (attempt.state != AttemptState::PROVIDER_SIGNED &&
        attempt.state != AttemptState::FINAL_COMMITTED &&
        attempt.state != AttemptState::BROADCAST &&
        attempt.state != AttemptState::STEMPOOL &&
        attempt.state != AttemptState::MEMPOOL) {
        error = "PAYMASTER_INVALID_FINAL_OBSERVATION_STATE";
        return false;
    }
    if (IsTerminal(session.state) && session.state != SessionState::CONFIRMED) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }

    const SessionState target_session = confirmed || session.state == SessionState::CONFIRMED ? SessionState::CONFIRMED : SessionState::MEMPOOL;
    const bool write_attempt = attempt.state != AttemptState::MEMPOOL;
    const bool write_session = session.state != target_session ||
                               session.pending_phase != PendingPhase::NONE;
    if (!write_attempt && !write_session) return true;

    if (write_attempt) {
        // A successful node submission proves the intermediate
        // FINAL_COMMITTED/BROADCAST states in one external operation. Publish
        // the highest durable observation together with the session state.
        attempt.state = AttemptState::MEMPOOL;
        attempt.updated_at = std::max(attempt.updated_at, now);
    }
    if (write_session) {
        if (!CanTransition(session.state, target_session)) {
            error = "PAYMASTER_INVALID_SESSION_TRANSITION";
            return false;
        }
        session.state = target_session;
        session.pending_phase = PendingPhase::NONE;
        session.updated_at = std::max(session.updated_at, now);
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if ((write_attempt && !batch.WritePaymasterAttempt(attempt)) ||
        (write_session && !batch.WritePaymasterSession(session))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetProviderResult(const uint256& commit_key,
                                       PaymasterResult& result) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterResult(commit_key, result);
}

} // namespace wallet
