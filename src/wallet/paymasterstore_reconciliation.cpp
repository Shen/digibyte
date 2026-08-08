// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Durable final rebroadcast, wallet reconciliation, reorgs, and pruning. */

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

bool PaymasterStore::ListClientDurableFinalTransactions(
    std::vector<CTransactionRef>& transactions,
    std::string& error) const
{
    transactions.clear();
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }
    std::set<uint256> unique_wtxids;
    for (const PaymentSession& session : sessions) {
        if (session.provider_side || session.state == SessionState::FAILED ||
            (session.final_txid.IsNull() && session.recovery_txid.IsNull())) {
            continue;
        }
        ExactFinalArtifact payment_artifact;
        ExactFinalArtifact recovery_artifact;
        std::optional<ProviderAttempt> observed_attempt;
        if (!LoadPaymentFinalArtifact(batch, session, payment_artifact,
                                      observed_attempt, error) ||
            !LoadRecoveryFinalArtifact(batch, session, recovery_artifact,
                                       error)) {
            return false;
        }
        const ExactFinalArtifact& selected =
            !recovery_artifact.txid.IsNull() ? recovery_artifact : payment_artifact;
        if (selected.txid.IsNull() ||
            !unique_wtxids.insert(selected.wtxid).second) {
            continue;
        }
        transactions.push_back(MakeTransactionRef(selected.transaction));
    }
    return true;
}

bool PaymasterStore::ValidateClientDurableFinalForBroadcast(
    const CTransaction& transaction,
    int64_t now,
    bool exact_final_already_known,
    std::string& error) const
{
    error.clear();
    const uint256 txid{transaction.GetHash()};
    const uint256 wtxid{transaction.GetWitnessHash()};
    const std::vector<unsigned char> final_bytes{CanonicalBytes(transaction)};
    if (txid.IsNull() || wtxid.IsNull() || now <= 0) {
        error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED";
        return false;
    }

    std::optional<ProviderAttempt> payment_attempt;
    std::optional<UserAuthorizationRecord> user_authorization;
    std::optional<PaymasterResult> persisted_payment_result;
    std::optional<AlternativeRecoveryRecord> alternative_recovery;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        std::vector<PaymentSession> sessions;
        if (!batch.ListPaymasterSessions(sessions)) {
            error = "PAYMASTER_SESSION_DATABASE_READ";
            return false;
        }

        size_t matching_bindings{0};
        for (const PaymentSession& session : sessions) {
            if (session.provider_side || session.state == SessionState::FAILED ||
                (session.final_txid.IsNull() && session.recovery_txid.IsNull())) {
                continue;
            }
            ExactFinalArtifact payment_artifact;
            ExactFinalArtifact recovery_artifact;
            std::optional<ProviderAttempt> observed_attempt;
            if (!LoadPaymentFinalArtifact(batch, session, payment_artifact,
                                          observed_attempt, error) ||
                !LoadRecoveryFinalArtifact(batch, session, recovery_artifact,
                                           error)) {
                return false;
            }
            const bool selected_recovery = !recovery_artifact.txid.IsNull();
            const ExactFinalArtifact& selected =
                selected_recovery ? recovery_artifact : payment_artifact;
            if (selected.txid != txid || selected.wtxid != wtxid ||
                selected.bytes != final_bytes) {
                continue;
            }
            if (++matching_bindings != 1) {
                error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_CONFLICT";
                return false;
            }

            if (!selected_recovery) {
                if (!observed_attempt) {
                    error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED";
                    return false;
                }
                PaymasterResult persisted_result;
                if (!batch.ReadPaymasterResult(
                        observed_attempt->commit_key,
                        persisted_result)) {
                    error = batch.HasPaymasterResult(
                                observed_attempt->commit_key) ?
                                "PAYMASTER_PERSISTED_RESULT_CORRUPT" :
                                "PAYMASTER_FINAL_RESULT_MISSING";
                    return false;
                }
                persisted_payment_result = std::move(persisted_result);
                UserAuthorizationRecord authorization;
                if (!batch.ReadPaymasterUserAuthorization(
                        observed_attempt->commit_key, authorization)) {
                    error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
                    return false;
                }
                user_authorization = std::move(authorization);
                payment_attempt = std::move(*observed_attempt);
                continue;
            }

            uint256 alternative_id;
            AlternativeRecoveryRecord recovery;
            if (!batch.ReadPaymasterAlternativeRecoveryRequest(
                    session.request_id, alternative_id) ||
                !batch.ReadPaymasterAlternativeRecovery(alternative_id,
                                                        recovery) ||
                recovery.provider_side ||
                recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
                recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
                recovery.request_id != session.request_id ||
                recovery.session_id != session.session_id ||
                recovery.final_transaction != final_bytes ||
                recovery.expected_wtxid != wtxid) {
                error = "PAYMASTER_RECOVERY_AUTHORIZATION_REQUIRED";
                return false;
            }
            alternative_recovery = std::move(recovery);
        }
        if (matching_bindings != 1) {
            error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED";
            return false;
        }
    }

    node::NodeContext* node = m_wallet.chain().context();
    if (!node || !node->chainman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }

    if (payment_attempt) {
        if (!persisted_payment_result) {
            error = "PAYMASTER_FINAL_RESULT_MISSING";
            return false;
        }
        const PaymasterResult& persisted_result =
            *persisted_payment_result;
        if (!ValidatePaymasterResult(
                persisted_result, Params().GenesisBlock().GetHash(),
                payment_attempt->provider_id,
                payment_attempt->commit_key,
                payment_attempt->provider_identity_key, 1, error) ||
            (persisted_result.status !=
                 PaymasterResultStatus::FINAL_COMMITTED &&
             persisted_result.status !=
                 PaymasterResultStatus::BROADCAST_ATTEMPTED) ||
            !persisted_result.txid || *persisted_result.txid != txid ||
            !persisted_result.raw_transaction_hash ||
            *persisted_result.raw_transaction_hash != wtxid ||
            !persisted_result.final_transaction ||
            CanonicalBytes(*persisted_result.final_transaction) !=
                final_bytes) {
            if (error.empty()) {
                error = "PAYMASTER_FINAL_RESULT_BINDING_MISMATCH";
            }
            return false;
        }
        if (!user_authorization) {
            error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
            return false;
        }
        CMutableTransaction validated_final;
        if (!ValidateClientFinalForExecution(
                *payment_attempt, wtxid, validated_final, error) ||
            CTransaction{validated_final} != transaction) {
            if (error.empty()) {
                error = "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_MISMATCH";
            }
            return false;
        }
        const UserAuthorizationRecord& authorization = *user_authorization;
        if (authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
            authorization.commit_key != payment_attempt->commit_key ||
            authorization.attempt_id != payment_attempt->attempt_id ||
            authorization.canonical_psbt_hash !=
                Hash(payment_attempt->user_signed_psbt) ||
            authorization.accepted_at <= 0 ||
            authorization.accepted_at <
                payment_attempt->client_manifest_accepted_at ||
            authorization.retry_until != payment_attempt->retry_until) {
            error = "PAYMASTER_USER_AUTHORIZATION_CONFLICT";
            return false;
        }
        if (!ValidateClientAuthorizationOwnership(
                m_wallet, payment_attempt->client_manifest, error)) {
            return false;
        }

        PaymasterCapacityRequest capacity_request;
        PaymasterCapacityProof capacity_proof;
        try {
            SpanReader request_stream{::PROTOCOL_VERSION,
                                      payment_attempt->capacity_request};
            request_stream >> capacity_request;
            CDataStream proof_stream{
                payment_attempt->capacity_snapshot.capacity_proof,
                SER_NETWORK, ::PROTOCOL_VERSION};
            proof_stream >> capacity_proof;
            if (!request_stream.empty() || !proof_stream.empty()) {
                throw std::ios_base::failure(
                    "trailing client capacity authority data");
            }
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
            return false;
        }
        if (CanonicalBytes(capacity_request) !=
                payment_attempt->capacity_request ||
            CanonicalBytes(capacity_proof) !=
                payment_attempt->capacity_snapshot.capacity_proof ||
            payment_attempt->capacity_snapshot.version !=
                ValidatedCapacitySnapshot::CURRENT_VERSION ||
            Hash(payment_attempt->capacity_request) !=
                payment_attempt->capacity_snapshot.request_hash) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
            return false;
        }
        const int64_t authorization_time =
            payment_attempt->client_manifest_accepted_at;
        const int64_t observation_time{
            std::max(now, authorization_time)};
        // This exact, fully signed spender is durable even when it has not yet
        // reached a pool. Supplying it here permits inspection of its now-spent
        // prevouts after restart; the immediately preceding/following mempool
        // preflights remain authoritative for conflicts.
        const AuthorizedCapacityResourceMode resource_mode{
            exact_final_already_known ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT};
        if (!ValidateAuthorizedCapacityRetryAgainstChainstate(
                capacity_proof, capacity_request,
                payment_attempt->provider_identity_key, *node->chainman,
                authorization_time, observation_time, resource_mode, error,
                exact_final_already_known ? &transaction : nullptr)) {
            return false;
        }
        return true;
    }

    if (!alternative_recovery) {
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_REQUIRED";
        return false;
    }
    const AlternativeRecoveryRecord& recovery = *alternative_recovery;
    if (!ValidateAlternativeRecoveryRecordShape(recovery, error) ||
        recovery.accepted_recovery_authorization_commitment.IsNull() ||
        recovery.accepted_recovery_authorization_commitment !=
            recovery.recovery_authorization.authorization_commitment ||
        recovery.recovery_authorization_accepted_at <= 0) {
        if (error.empty()) {
            error = "PAYMASTER_RECOVERY_AUTHORIZATION_REQUIRED";
        }
        return false;
    }
    const int64_t authorization_time =
        recovery.recovery_authorization_accepted_at;
    const int64_t observation_time{
        std::max(now, authorization_time)};
    if (!ValidateRecoveryAuthorizationManifest(
            recovery.recovery_authorization, recovery.recovery_request,
            recovery.recovery_response, authorization_time, error)) {
        return false;
    }

    AlternativeRecoveryParameters parameters;
    AlternativeRecoveryTemplate trusted;
    PartiallySignedTransaction unsigned_psbt;
    if (!BuildAlternativeRecoveryParametersFromRecord(
            m_wallet, recovery, parameters, error) ||
        !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
            recovery.recovery_response, recovery.recovery_request,
            recovery.recovery_provider_identity_key, parameters, Params(),
            *node->chainman, authorization_time, observation_time,
            exact_final_already_known ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
            trusted, unsigned_psbt, error,
            exact_final_already_known ? &transaction : nullptr)) {
        return false;
    }

    AlternativeRecoverySubmit submit;
    submit.genesis_hash = parameters.genesis_hash;
    submit.request_id = recovery.request_id;
    submit.session_id = recovery.session_id;
    submit.recovery_id = recovery.recovery_id;
    submit.recovery_provider_id = recovery.recovery_provider_id;
    submit.recovery_request_hash = recovery.recovery_request_hash;
    submit.recovery_commit_key =
        recovery.recovery_response.recovery_commit_key;
    submit.template_commitment =
        recovery.recovery_response.manifest.template_commitment;
    submit.user_psbt = recovery.user_signed_psbt;
    PartiallySignedTransaction user_psbt;
    if (!ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
            submit, recovery.recovery_response, trusted, parameters, Params(),
            *node->chainman, authorization_time, observation_time,
            exact_final_already_known ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
            user_psbt, error,
            exact_final_already_known ? &transaction : nullptr)) {
        return false;
    }

    AlternativeRecoveryResultMessage result_message;
    result_message.request_id = recovery.request_id;
    result_message.session_id = recovery.session_id;
    result_message.recovery_id = recovery.recovery_id;
    result_message.recovery_request_hash = recovery.recovery_request_hash;
    result_message.result = recovery.signed_result;
    const PaymasterResult& result = recovery.signed_result;
    if ((result.status != PaymasterResultStatus::FINAL_COMMITTED &&
         result.status != PaymasterResultStatus::BROADCAST_ATTEMPTED) ||
        !result.txid || *result.txid != txid ||
        !result.raw_transaction_hash ||
        *result.raw_transaction_hash != wtxid ||
        !result.final_transaction ||
        CanonicalBytes(*result.final_transaction) != final_bytes) {
        if (error.empty()) error = "PAYMASTER_RECOVERY_FINAL_RESULT_REQUIRED";
        return false;
    }
    return ValidateDurableAlternativeRecoveryResultAgainstChainstate(
        result_message, recovery.recovery_response,
        recovery.recovery_provider_identity_key, 1, trusted, parameters,
        Params(), *node->chainman, authorization_time,
        std::max(now, result.updated_at), exact_final_already_known, error);
}

bool PaymasterStore::ReconcileFinalTransaction(const CTransaction& transaction,
                                               int confirmation_depth,
                                               bool in_mempool,
                                               int64_t now,
                                               std::string& error)
{
    error.clear();
    const uint256 txid{transaction.GetHash()};
    if (txid.IsNull() || now <= 0 ||
        (confirmation_depth != 0 && in_mempool)) {
        error = "PAYMASTER_INVALID_TRANSACTION_OBSERVATION";
        return false;
    }

    std::vector<std::string> prune;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        std::vector<PaymentSession> sessions;
        if (!batch.ListPaymasterSessions(sessions)) {
            error = "PAYMASTER_SESSION_DATABASE_READ";
            return false;
        }
        for (PaymentSession& session : sessions) {
            const bool observed_recovery = session.recovery_txid == txid;
            const bool observed_payment = session.final_txid == txid;
            if ((!observed_payment && !observed_recovery) ||
                session.state == SessionState::FAILED) {
                continue;
            }
            if (observed_payment && observed_recovery) {
                error = "PAYMASTER_FINAL_TRANSACTION_ROLE_CONFLICT";
                return false;
            }

            ExactFinalArtifact payment_artifact;
            ExactFinalArtifact recovery_artifact;
            std::optional<ProviderAttempt> observed_attempt;
            if (!LoadPaymentFinalArtifact(batch, session, payment_artifact,
                                          observed_attempt, error) ||
                !LoadRecoveryFinalArtifact(batch, session, recovery_artifact,
                                           error)) {
                return false;
            }
            ExactFinalObservation payment_observation;
            ExactFinalObservation recovery_observation;
            if (!ObserveExactWalletArtifact(m_wallet, payment_artifact,
                                            payment_observation, error) ||
                !ObserveExactWalletArtifact(m_wallet, recovery_artifact,
                                            recovery_observation, error)) {
                return false;
            }
            if (observed_payment &&
                !BindExactObservation(transaction, payment_artifact,
                                      confirmation_depth, in_mempool,
                                      payment_observation, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_FINAL_TRANSACTION_BINDING_MISMATCH";
                }
                return false;
            }
            if (observed_recovery &&
                !BindExactObservation(transaction, recovery_artifact,
                                      confirmation_depth, in_mempool,
                                      recovery_observation, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_RECOVERY_FINAL_MISMATCH";
                }
                return false;
            }

            SessionState state;
            PendingPhase phase{PendingPhase::NONE};
            int final_depth{0};
            if (payment_observation.confirmation_depth > 0 &&
                recovery_observation.confirmation_depth > 0) {
                error = "PAYMASTER_MUTUALLY_EXCLUSIVE_FINALS_CONFIRMED";
                return false;
            }
            if (recovery_observation.confirmation_depth > 0) {
                state = SessionState::CANCELED_SAFE;
                final_depth = recovery_observation.confirmation_depth;
            } else if (payment_observation.confirmation_depth > 0) {
                state = SessionState::CONFIRMED;
                final_depth = payment_observation.confirmation_depth;
            } else if (recovery_observation.in_mempool) {
                state = SessionState::PENDING_PROVIDER;
                phase = PendingPhase::CANCEL_MEMPOOL;
            } else if (payment_observation.in_mempool) {
                state = SessionState::MEMPOOL;
            } else if (payment_observation.confirmation_depth < 0 ||
                       recovery_observation.confirmation_depth < 0) {
                state = SessionState::CONFLICTED;
            } else {
                state = SessionState::PENDING_PROVIDER;
                phase = PendingPhase::PENDING_NETWORK;
            }

            AttemptState attempt_state{AttemptState::CANDIDATE};
            if (observed_attempt) {
                if (payment_observation.confirmation_depth < 0) {
                    attempt_state = AttemptState::CONFLICTED;
                } else if (payment_observation.confirmation_depth > 0 ||
                           payment_observation.in_mempool) {
                    attempt_state = AttemptState::MEMPOOL;
                } else {
                    // The exact signed bytes remain durable and may be retried,
                    // but the transaction is not presently observed by the node.
                    attempt_state = AttemptState::BROADCAST;
                }
            }
            ClientFeeLedger client_fee_ledger;
            bool client_fee_changed{false};
            if (state == SessionState::CANCELED_SAFE && final_depth > 0 &&
                !session.provider_side) {
                const DatabaseReadStatus client_ledger_status =
                    batch.ReadPaymasterClientFeeLedgerWithStatus(
                        client_fee_ledger);
                if (client_ledger_status != DatabaseReadStatus::FOUND &&
                    client_ledger_status != DatabaseReadStatus::NOT_FOUND) {
                    error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
                    return false;
                }
                const bool have_client_ledger{
                    client_ledger_status == DatabaseReadStatus::FOUND};
                if (have_client_ledger) {
                    // Confirmation of the cancel-to-self transaction proves
                    // that no original provider attempt can consume these DD
                    // inputs. Release only still-reserved original fees;
                    // already-spent fees and the (separate) recovery fee stay
                    // untouched.
                    for (const uint256& attempt_id : session.attempt_ids) {
                        ProviderAttempt original_attempt;
                        const DatabaseReadStatus attempt_status =
                            batch.ReadPaymasterAttemptWithStatus(
                                attempt_id, original_attempt);
                        if (attempt_status != DatabaseReadStatus::FOUND) {
                            error = PersistedReadError(
                                attempt_status, "ProviderAttempt",
                                original_attempt,
                                "PAYMASTER_CLIENT_FEE_ATTEMPT_MISSING",
                                "PAYMASTER_CLIENT_FEE_ATTEMPT_READ_FAILED");
                            return false;
                        }
                        if (original_attempt.commit_key.IsNull()) {
                            continue;
                        }
                        const auto reservation = std::find_if(
                            client_fee_ledger.reservations.begin(),
                            client_fee_ledger.reservations.end(),
                            [&](const ClientFeeReservation& entry) {
                                return entry.commit_key ==
                                       original_attempt.commit_key;
                            });
                        if (reservation ==
                                client_fee_ledger.reservations.end() ||
                            reservation->state !=
                                BudgetReservationState::RESERVED) {
                            continue;
                        }
                        if (!ReleaseClientFee(client_fee_ledger,
                                              original_attempt.commit_key,
                                              now, error)) {
                            return false;
                        }
                        client_fee_changed = true;
                    }
                }
            }
            const bool write_session = session.state != state ||
                                       session.pending_phase != phase;
            const bool write_attempt = observed_attempt &&
                                       observed_attempt->state != attempt_state;
            if (!write_session && !write_attempt && !client_fee_changed) {
                if (final_depth >= DEFAULT_REORG_SAFETY_DEPTH) {
                    prune.push_back(session.request_id);
                }
                continue;
            }
            if (write_session) {
                session.state = state;
                session.pending_phase = phase;
                session.updated_at = std::max(session.updated_at, now);
            }
            if (write_attempt) {
                observed_attempt->state = attempt_state;
                observed_attempt->updated_at =
                    std::max(observed_attempt->updated_at, now);
            }
            if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
            if ((write_attempt &&
                 !batch.WritePaymasterAttempt(*observed_attempt)) ||
                (write_session && !batch.WritePaymasterSession(session)) ||
                (client_fee_changed &&
                 !batch.WritePaymasterClientFeeLedger(client_fee_ledger))) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (!batch.TxnCommit()) {
                error = "PAYMASTER_DATABASE_COMMIT";
                return false;
            }
            if (final_depth >= DEFAULT_REORG_SAFETY_DEPTH) {
                prune.push_back(session.request_id);
            }
        }
    }
    for (const std::string& request_id : prune) {
        if (!PruneFinalSession(request_id, error)) return false;
    }
    return true;
}

bool PaymasterStore::ReconcileFinalSessionsAtTip(int64_t now,
                                                 std::string& error)
{
    error.clear();
    if (now <= 0) {
        error = "PAYMASTER_INVALID_TRANSACTION_OBSERVATION";
        return false;
    }

    // Chain-tip maintenance is the wallet-wide expiry clock. It covers both
    // client and provider wallets, so stale unsigned authority is released
    // even when no Paymaster RPC is called again.
    size_t expired{0};
    if (!ExpireProviderQuotes(now, expired, error) ||
        !ExpireProviderCapacityReservations(now, expired, error) ||
        !CompactClientCapacitySnapshots(now, expired, error) ||
        !ExpireAlternativeRecoveries(now, expired, error)) {
        return false;
    }

    struct ReconciliationCandidate {
        CTransactionRef transaction;
        int confirmation_depth{0};
        bool in_mempool{false};
    };
    std::vector<ReconciliationCandidate> candidates;
    std::set<uint256> scheduled_wtxids;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        std::vector<PaymentSession> sessions;
        if (!batch.ListPaymasterSessions(sessions)) {
            error = "PAYMASTER_SESSION_DATABASE_READ";
            return false;
        }
        for (const PaymentSession& session : sessions) {
            if (session.state == SessionState::FAILED ||
                (session.final_txid.IsNull() && session.recovery_txid.IsNull())) {
                continue;
            }
            ExactFinalArtifact payment_artifact;
            ExactFinalArtifact recovery_artifact;
            std::optional<ProviderAttempt> observed_attempt;
            if (!LoadPaymentFinalArtifact(batch, session, payment_artifact,
                                          observed_attempt, error) ||
                !LoadRecoveryFinalArtifact(batch, session, recovery_artifact,
                                           error)) {
                return false;
            }
            const ExactFinalArtifact& selected =
                !recovery_artifact.txid.IsNull() ? recovery_artifact : payment_artifact;
            if (selected.txid.IsNull() ||
                !scheduled_wtxids.insert(selected.wtxid).second) {
                continue;
            }
            ExactFinalObservation observation;
            if (!ObserveExactWalletArtifact(m_wallet, selected, observation,
                                            error)) {
                return false;
            }
            candidates.push_back({MakeTransactionRef(selected.transaction),
                                  observation.confirmation_depth,
                                  observation.in_mempool});
        }
    }

    for (const ReconciliationCandidate& candidate : candidates) {
        if (!ReconcileFinalTransaction(*candidate.transaction,
                                       candidate.confirmation_depth,
                                       candidate.in_mempool, now, error)) {
            return false;
        }
    }
    return true;
}

bool PaymasterStore::PruneFinalSession(const std::string& request_id, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    const DatabaseReadStatus session_status =
        batch.ReadPaymasterSessionWithStatus(request_id, session);
    if (session_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            session_status, "PaymentSession", session,
            "PAYMASTER_SESSION_NOT_FOUND",
            "PAYMASTER_INVALID_PERSISTED_SESSION");
        return false;
    }
    if (!IsTerminal(session.state)) {
        error = "PAYMASTER_SESSION_NOT_FINAL";
        return false;
    }

    // Pruning is destructive. Resolve and validate every referenced record
    // before opening the transaction so no unreadable child can be skipped and
    // orphaned by an otherwise successful tombstone write.
    std::string indexed_request_id;
    if (batch.ReadPaymasterSessionIdWithStatus(
            session.session_id, indexed_request_id) !=
            DatabaseReadStatus::FOUND ||
        indexed_request_id != request_id) {
        error = "PAYMASTER_SESSION_INDEX_CONFLICT";
        return false;
    }

    IdempotencyTombstone existing_tombstone;
    const DatabaseReadStatus tombstone_status =
        batch.ReadPaymasterTombstoneWithStatus(request_id,
                                               existing_tombstone);
    if (tombstone_status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_TOMBSTONE_CONFLICT";
        return false;
    }
    if (tombstone_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            tombstone_status, "IdempotencyTombstone", existing_tombstone,
            "PAYMASTER_TOMBSTONE_NOT_FOUND",
            "PAYMASTER_INVALID_PERSISTED_TOMBSTONE");
        return false;
    }

    std::set<COutPoint> reservations_to_erase;
    if (!session.provider_side) {
        for (const COutPoint& outpoint : session.user_inputs) {
            InputReservation reservation;
            const DatabaseReadStatus reservation_status =
                batch.ReadPaymasterReservationWithStatus(outpoint,
                                                         reservation);
            if (reservation_status == DatabaseReadStatus::NOT_FOUND) {
                // Failed unsigned sessions may have released their locks in
                // an earlier atomic transition.
                continue;
            }
            if (reservation_status != DatabaseReadStatus::FOUND) {
                error = PersistedReadError(
                    reservation_status, "InputReservation", reservation,
                    "PAYMASTER_RESERVATION_NOT_FOUND",
                    "PAYMASTER_INVALID_PERSISTED_RESERVATION");
                return false;
            }
            if (reservation.request_id != request_id ||
                reservation.session_id != session.session_id) {
                error = "PAYMASTER_RESERVATION_SESSION_CONFLICT";
                return false;
            }
            reservations_to_erase.insert(outpoint);
        }
    }

    struct PrunableAttempt {
        ProviderAttempt attempt;
        bool has_commit{false};
        bool has_authorization{false};
        bool has_result{false};
        bool has_outcome{false};
    };
    std::vector<PrunableAttempt> attempts;
    attempts.reserve(session.attempt_ids.size());
    std::set<uint256> unique_attempt_ids;
    for (const uint256& attempt_id : session.attempt_ids) {
        if (!unique_attempt_ids.insert(attempt_id).second) {
            error = "PAYMASTER_PRUNE_ATTEMPT_ID_CONFLICT";
            return false;
        }
        PrunableAttempt records;
        const DatabaseReadStatus attempt_status =
            batch.ReadPaymasterAttemptWithStatus(attempt_id, records.attempt);
        if (attempt_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                attempt_status, "ProviderAttempt", records.attempt,
                "PAYMASTER_PRUNE_ATTEMPT_MISSING",
                "PAYMASTER_PRUNE_ATTEMPT_READ_FAILED");
            return false;
        }
        if (records.attempt.session_id != session.session_id) {
            error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
            return false;
        }

        if (!records.attempt.template_commitment.IsNull()) {
            uint256 indexed_attempt;
            if (batch.ReadPaymasterTemplateWithStatus(
                    records.attempt.template_commitment, indexed_attempt) !=
                    DatabaseReadStatus::FOUND ||
                indexed_attempt != attempt_id) {
                error = "PAYMASTER_TEMPLATE_INDEX_CONFLICT";
                return false;
            }
        }
        if (!records.attempt.unsigned_txid.IsNull()) {
            uint256 indexed_attempt;
            if (batch.ReadPaymasterUnsignedTxWithStatus(
                    records.attempt.unsigned_txid, indexed_attempt) !=
                    DatabaseReadStatus::FOUND ||
                indexed_attempt != attempt_id) {
                error = "PAYMASTER_UNSIGNED_TX_INDEX_CONFLICT";
                return false;
            }
        }

        if (!records.attempt.commit_key.IsNull()) {
            ProviderCommitRecord commit;
            const DatabaseReadStatus commit_status =
                batch.ReadPaymasterProviderCommitWithStatus(
                    records.attempt.commit_key, commit);
            if (commit_status == DatabaseReadStatus::FOUND) {
                if (commit.provider_id != records.attempt.provider_id ||
                    commit.template_commitment !=
                        records.attempt.template_commitment) {
                    error = "PAYMASTER_PROVIDER_COMMIT_ATTEMPT_CONFLICT";
                    return false;
                }
                records.has_commit = true;
            } else if (commit_status != DatabaseReadStatus::NOT_FOUND) {
                error = PersistedReadError(
                    commit_status, "ProviderCommitRecord", commit,
                    "PAYMASTER_PROVIDER_COMMIT_NOT_FOUND",
                    "PAYMASTER_INVALID_PERSISTED_PROVIDER_COMMIT");
                return false;
            }

            UserAuthorizationRecord authorization;
            const DatabaseReadStatus authorization_status =
                batch.ReadPaymasterUserAuthorizationWithStatus(
                    records.attempt.commit_key, authorization);
            if (authorization_status == DatabaseReadStatus::FOUND) {
                if (authorization.attempt_id != attempt_id) {
                    error = "PAYMASTER_USER_AUTHORIZATION_CONFLICT";
                    return false;
                }
                records.has_authorization = true;
            } else if (authorization_status != DatabaseReadStatus::NOT_FOUND) {
                error = PersistedReadError(
                    authorization_status, "UserAuthorizationRecord",
                    authorization, "PAYMASTER_USER_AUTHORIZATION_MISSING",
                    "PAYMASTER_INVALID_PERSISTED_USER_AUTHORIZATION");
                return false;
            }

            PaymasterResult result;
            const DatabaseReadStatus result_status =
                batch.ReadPaymasterResultWithStatus(
                    records.attempt.commit_key, result);
            if (result_status == DatabaseReadStatus::FOUND) {
                records.has_result = true;
            } else if (result_status != DatabaseReadStatus::NOT_FOUND) {
                error = PersistedReadError(
                    result_status, "PaymasterResult", result,
                    "PAYMASTER_RESULT_NOT_FOUND",
                    "PAYMASTER_PERSISTED_RESULT_CORRUPT");
                return false;
            }
        }

        PaymasterOutcomeMarker outcome;
        const DatabaseReadStatus outcome_status =
            batch.ReadPaymasterOutcomeMarkerWithStatus(attempt_id, outcome);
        if (outcome_status == DatabaseReadStatus::FOUND) {
            records.has_outcome = true;
        } else if (outcome_status != DatabaseReadStatus::NOT_FOUND) {
            error = PersistedReadError(
                outcome_status, "PaymasterOutcomeMarker", outcome,
                "PAYMASTER_OUTCOME_NOT_FOUND",
                "PAYMASTER_INVALID_PERSISTED_OUTCOME");
            return false;
        }
        attempts.push_back(std::move(records));
    }

    std::optional<SelfRecoveryRecord> recovery;
    SelfRecoveryRecord loaded_recovery;
    const DatabaseReadStatus recovery_status =
        batch.ReadPaymasterRecoveryWithStatus(request_id, loaded_recovery);
    if (recovery_status == DatabaseReadStatus::FOUND) {
        if (loaded_recovery.session_id != session.session_id) {
            error = "PAYMASTER_RECOVERY_SESSION_CONFLICT";
            return false;
        }
        recovery = std::move(loaded_recovery);
    } else if (recovery_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            recovery_status, "SelfRecoveryRecord", loaded_recovery,
            "PAYMASTER_RECOVERY_NOT_FOUND",
            "PAYMASTER_INVALID_PERSISTED_RECOVERY");
        return false;
    }

    std::optional<AlternativeRecoveryRecord> alternative_recovery;
    uint256 alternative_recovery_id;
    const DatabaseReadStatus alternative_request_status =
        batch.ReadPaymasterAlternativeRecoveryRequestWithStatus(
            request_id, alternative_recovery_id);
    if (alternative_request_status == DatabaseReadStatus::FOUND) {
        AlternativeRecoveryRecord alternative;
        const DatabaseReadStatus alternative_status =
            batch.ReadPaymasterAlternativeRecoveryWithStatus(
                alternative_recovery_id, alternative);
        if (alternative_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                alternative_status, "AlternativeRecoveryRecord", alternative,
                "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND",
                "PAYMASTER_INVALID_PERSISTED_ALTERNATIVE_RECOVERY");
            return false;
        }
        if (alternative.provider_side ||
            alternative.request_id != request_id ||
            alternative.session_id != session.session_id ||
            (session.state == SessionState::CANCELED_SAFE &&
             (alternative.phase !=
                  AlternativeRecoveryPhase::FINAL_COMMITTED ||
              !recovery ||
              alternative.final_transaction !=
                  recovery->final_transaction))) {
            error = "PAYMASTER_ALTERNATIVE_RECOVERY_PRUNE_CONFLICT";
            return false;
        }
        alternative_recovery = std::move(alternative);
    } else if (alternative_request_status != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_INDEX_DATABASE_READ";
        return false;
    }

    const uint256 tombstone_txid = session.state == SessionState::CANCELED_SAFE ? session.recovery_txid : session.final_txid;
    IdempotencyTombstone tombstone{IdempotencyTombstone::CURRENT_VERSION, request_id, session.session_id,
                                   session.canonical_request_hash, session.fee_mode_requested,
                                   session.fee_mode_used, session.state, tombstone_txid,
                                   session.requested_amount,
                                   session.subtract_paymaster_fee_from_amount,
                                   session.send_all_spendable_dd};
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterTombstone(tombstone, false)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!session.provider_side) {
        for (const auto& outpoint : session.user_inputs) {
            if ((reservations_to_erase.count(outpoint) != 0 &&
                 !batch.ErasePaymasterReservation(outpoint)) ||
                !batch.EraseLockedUTXO(outpoint)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
    }
    for (const PrunableAttempt& records : attempts) {
        const ProviderAttempt& attempt = records.attempt;
        if (!attempt.template_commitment.IsNull() &&
            !batch.ErasePaymasterTemplate(attempt.template_commitment)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!attempt.unsigned_txid.IsNull() &&
            !batch.ErasePaymasterUnsignedTx(attempt.unsigned_txid)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!attempt.commit_key.IsNull()) {
            if (records.has_commit &&
                !batch.ErasePaymasterProviderCommit(attempt.commit_key)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (records.has_authorization &&
                !batch.ErasePaymasterUserAuthorization(attempt.commit_key)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (records.has_result &&
                !batch.ErasePaymasterResult(attempt.commit_key)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
        if (records.has_outcome &&
            !batch.ErasePaymasterOutcomeMarker(attempt.attempt_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.ErasePaymasterAttempt(attempt.attempt_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (recovery &&
        !batch.ErasePaymasterRecovery(request_id)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (alternative_recovery) {
        if (!batch.ErasePaymasterAlternativeRecovery(
                alternative_recovery_id) ||
            !batch.ErasePaymasterAlternativeRecoveryRequest(request_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.ErasePaymasterSession(request_id)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    if (!session.provider_side) {
        for (const auto& outpoint : session.user_inputs)
            m_wallet.UnlockCoin(outpoint);
    }
    return true;
}

} // namespace wallet
