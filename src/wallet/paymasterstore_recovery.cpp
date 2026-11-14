// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Self-recovery and alternative-provider recovery state. */

#include <wallet/paymasterstore.h>
#include <wallet/paymastersessionindex.h>
#include <wallet/paymasterstore_internal.h>

#include <chainparams.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <kernel/cs_main.h>
#include <node/context.h>
#include <paymaster/finance.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/validation.h>
#include <paymaster/wire.h>
#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/time.h>
#include <version.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterprovider.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_store::internal;

namespace {
bool ObserveProviderRecoveryHistoryCompletion(
    CWallet& wallet, WalletBatch& batch, const AlternativeRecoveryRecord& recovery,
    std::optional<ExactFinalTransactionProof>& confirmation_proof, std::string& error)
{
    AssertLockHeld(wallet.cs_wallet);
    confirmation_proof.reset();
    if (!recovery.provider_side || recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED) return true;

    // Keep the complete signed recovery, commit and result: they remain the
    // authority for exact replay and accounting after the rolling ledger ages
    // out. Archival uses the same reorg safety depth as normal session pruning;
    // it grants no signature, budget or release permission.
    ProviderCommitRecord commit;
    const auto& commit_key = recovery.recovery_response.recovery_commit_key;
    const auto commit_status = batch.ReadPaymasterProviderCommitWithStatus(commit_key, commit);
    if (commit_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(commit_status, "ProviderCommitRecord", commit,
            "PAYMASTER_PROVIDER_COMMIT_NOT_FOUND", "PAYMASTER_INVALID_PERSISTED_PROVIDER_COMMIT");
        return false;
    }
    CMutableTransaction transaction;
    std::vector<COutPoint> provider_inputs;
    if (!ValidateProviderAlternativeRecoveryCommitBinding(recovery, commit, transaction, provider_inputs, error)) return false;
    PaymasterResult result;
    const auto result_status = batch.ReadPaymasterResultWithStatus(commit_key, result);
    if (result_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(result_status, "PaymasterResult", result,
            "PAYMASTER_RESULT_NOT_FOUND", "PAYMASTER_PERSISTED_RESULT_CORRUPT");
        return false;
    }
    if (CanonicalBytes(result) != CanonicalBytes(recovery.signed_result)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT";
        return false;
    }
    const auto& manifest = recovery.recovery_response.manifest;
    CollaborativePSBTTemplate trusted;
    std::string decode_error;
    if (!DecodeRawPSBT(trusted.psbt, MakeByteSpan(recovery.recovery_response.unsigned_psbt), decode_error) ||
        CanonicalBytes(trusted.psbt) != recovery.recovery_response.unsigned_psbt) {
        error = "PAYMASTER_RECOVERY_RESPONSE_PSBT_NONCANONICAL";
        return false;
    }
    const auto append_roles = [&](const std::vector<COutPoint>& outpoints, InputRole role) {
        for (const COutPoint& outpoint : outpoints) {
            const size_t index = trusted.input_roles.size();
            if (!trusted.psbt.tx || index >= trusted.psbt.tx->vin.size() ||
                trusted.psbt.tx->vin[index].prevout != outpoint) return false;
            trusted.input_roles.push_back(role);
        }
        return true;
    };
    if (!append_roles(manifest.user_dd_inputs, InputRole::USER_DD) ||
        !append_roles(manifest.recovery_provider_carrier_inputs, InputRole::PROVIDER_CARRIER) ||
        !append_roles(manifest.recovery_provider_dgb_inputs, InputRole::PROVIDER_DGB) ||
        manifest.template_commitment != GetAlternativeRecoveryTemplateCommitment(manifest, trusted)) {
        error = "PAYMASTER_RECOVERY_TEMPLATE_MISMATCH";
        return false;
    }
    if (!ValidateCollaborativePSBT(trusted.psbt, trusted, CollaborativeSignatureStage::UNSIGNED, error) ||
        !ValidateFinalCollaborativeTransaction(transaction, commit.final_txid,
            recovery.expected_wtxid, trusted, error)) return false;
    ExactFinalArtifact artifact;
    ExactFinalObservation observation;
    if (!DecodeExactFinalArtifact(recovery.final_transaction, commit.final_txid, artifact, error) ||
        !ObserveExactWalletArtifact(wallet, artifact, observation, error)) return false;
    if (observation.confirmation_depth < DEFAULT_REORG_SAFETY_DEPTH) return true;
    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(wallet, MakeTransactionRef(transaction),
            ExactFinalTxIndexMode::NONBLOCKING, preflight, error)) return false;
    if (preflight.presence == ExactFinalTransactionPresence::CONFIRMED &&
        preflight.confirmation_depth >= DEFAULT_REORG_SAFETY_DEPTH) {
        confirmation_proof = std::move(preflight.final_proof);
    }
    return true;
}
} // namespace

bool PaymasterStore::CommitSelfRecovery(const std::string& request_id,
                                        const SelfRecoveryRecord& recovery,
                                        std::string& error)
{
    error.clear();
    CMutableTransaction transaction;
    try {
        SpanReader stream{::PROTOCOL_VERSION, recovery.final_transaction};
        stream >> transaction;
        if (!stream.empty()) throw std::ios_base::failure("trailing transaction data");
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_SELF_RECOVERY";
        return false;
    }
    if (recovery.version != SelfRecoveryRecord::CURRENT_VERSION ||
        recovery.request_id != request_id || recovery.session_id.IsNull() ||
        recovery.user_inputs.empty() || recovery.recovery_txid.IsNull() ||
        recovery.raw_transaction_hash.IsNull() || recovery.final_transaction.empty() ||
        recovery.created_at <= 0 ||
        CTransaction{transaction}.GetHash() != recovery.recovery_txid ||
        Hash(recovery.final_transaction) != recovery.raw_transaction_hash ||
        GetDigiDollarTxType(CTransaction{transaction}) != DD_TX_TRANSFER ||
        transaction.vin.size() < recovery.user_inputs.size()) {
        error = "PAYMASTER_INVALID_SELF_RECOVERY";
        return false;
    }
    for (size_t index = 0; index < recovery.user_inputs.size(); ++index) {
        if (transaction.vin[index].prevout != recovery.user_inputs[index]) {
            error = "PAYMASTER_SELF_RECOVERY_INPUT_CONFLICT";
            return false;
        }
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        recovery.session_id != session.session_id ||
        recovery.user_inputs != session.user_inputs) {
        error = "PAYMASTER_SELF_RECOVERY_SESSION_CONFLICT";
        return false;
    }
    SelfRecoveryRecord existing;
    if (batch.ReadPaymasterRecovery(request_id, existing)) {
        if (existing.session_id == recovery.session_id &&
            existing.user_inputs == recovery.user_inputs &&
            existing.recovery_txid == recovery.recovery_txid &&
            existing.raw_transaction_hash == recovery.raw_transaction_hash &&
            existing.final_transaction == recovery.final_transaction) {
            return true;
        }
        error = "PAYMASTER_SELF_RECOVERY_CONFLICT";
        return false;
    }
    if (session.state != SessionState::PENDING_PROVIDER ||
        session.pending_phase == PendingPhase::CANCEL_MEMPOOL) {
        error = "PAYMASTER_SELF_RECOVERY_NOT_ALLOWED";
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD ||
            !reservation.authorization_may_exist) {
            error = "PAYMASTER_SELF_RECOVERY_RESERVATION_MISSING";
            return false;
        }
    }
    if (transaction.vout.empty() ||
        !(m_wallet.IsMine(transaction.vout.front()) & ISMINE_SPENDABLE)) {
        error = "PAYMASTER_SELF_RECOVERY_DESTINATION_NOT_OWNED";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterRecovery(recovery, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.recovery_txid = recovery.recovery_txid;
    session.pending_phase = PendingPhase::PENDING_NETWORK;
    session.updated_at = std::max(session.updated_at, recovery.created_at);
    if (!batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    return CommitWithPaymasterAuthorizationProtection(m_wallet, batch, true, error);
}

bool PaymasterStore::GetSelfRecovery(const std::string& request_id,
                                     SelfRecoveryRecord& recovery) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterRecovery(request_id, recovery);
}

bool PaymasterStore::PrepareAlternativeRecovery(
    const AlternativeRecoveryRecord& recovery,
    std::string& error)
{
    error.clear();
    if (recovery.provider_side ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING ||
        !recovery.capacity_proof_claim_candidate.empty() ||
        !ValidateAlternativeRecoveryRecordShape(recovery, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_ALTERNATIVE_RECOVERY";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    uint256 attempt_id;
    if (!batch.ReadPaymasterSession(recovery.request_id, session) ||
        session.provider_side || session.session_id != recovery.session_id ||
        session.state != SessionState::PENDING_PROVIDER ||
        !batch.ReadPaymasterTemplate(recovery.original_template_commitment,
                                     attempt_id) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt)) {
        error = "PAYMASTER_RECOVERY_ORIGINAL_SESSION_MISSING";
        return false;
    }
    if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt.attempt_id) == session.attempt_ids.end() ||
        attempt.provider_id != recovery.original_provider_id ||
        attempt.commit_key != recovery.original_commit_key ||
        attempt.template_commitment != recovery.original_template_commitment ||
        !(AttemptHasReached(attempt.state, AttemptState::USER_SIGNED) ||
          attempt.state == AttemptState::AMBIGUOUS)) {
        error = "PAYMASTER_RECOVERY_ORIGINAL_AUTHORIZATION_MISSING";
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD ||
            !reservation.authorization_may_exist) {
            error = "PAYMASTER_RECOVERY_RESERVATION_MISSING";
            return false;
        }
    }

    uint256 indexed_id;
    if (batch.ReadPaymasterAlternativeRecoveryRequest(recovery.request_id,
                                                      indexed_id)) {
        AlternativeRecoveryRecord existing;
        if (indexed_id == recovery.recovery_id &&
            batch.ReadPaymasterAlternativeRecovery(indexed_id, existing) &&
            SameAlternativeRecoveryRecord(existing, recovery)) {
            return true;
        }
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT";
        return false;
    }
    AlternativeRecoveryRecord existing;
    if (batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id, existing)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAlternativeRecovery(recovery, false) ||
        !batch.WritePaymasterAlternativeRecoveryRequest(
            recovery.request_id, recovery.recovery_id, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::UpdateAlternativeRecovery(
    const AlternativeRecoveryRecord& recovery,
    std::string& error)
{
    error.clear();
    if (!ValidateAlternativeRecoveryRecordShape(recovery, error)) return false;

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord current;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id, current)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }
    if (!current.capacity_proof_claim_candidate.empty() &&
        (!ValidatePersistedRecoveryCapacityRequestAuthority(current, error) ||
         !ValidatePersistedCapacityClaimCandidate(
             current.capacity_proof_claim_candidate,
             current.capacity_request,
             current.recovery_provider_identity_key,
             "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
             error))) {
        return false;
    }
    if (SameAlternativeRecoveryRecord(current, recovery)) return true;
    if (current.expired || recovery.expired) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_EXPIRED";
        return false;
    }
    if (current.capacity_proof_claim_candidate !=
        recovery.capacity_proof_claim_candidate) {
        error = "PAYMASTER_SIGNED_CLAIM_CANDIDATE_CONFLICT";
        return false;
    }
    if (!SameAlternativeRecoveryProgression(current, recovery)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_TRANSITION_CONFLICT";
        return false;
    }

    bool capacity_snapshot_changed{false};
    CapacitySnapshotMutation capacity_mutation;
    if (!recovery.provider_side &&
        current.phase == AlternativeRecoveryPhase::CAPACITY_PENDING &&
        recovery.phase == AlternativeRecoveryPhase::REQUEST_READY) {
        if (current.capacity_proof_claim_candidate.empty()) {
            error = "PAYMASTER_RECOVERY_CAPACITY_CLAIM_CANDIDATE_MISSING";
            return false;
        }
        if (!ValidatePersistedRecoveryCapacityRequestAuthority(current,
                                                               error) ||
            !ValidatePersistedCapacityClaimCandidate(
                current.capacity_proof_claim_candidate,
                current.capacity_request,
                current.recovery_provider_identity_key,
                "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
                error)) {
            return false;
        }
        const ValidatedCapacitySnapshot& snapshot =
            recovery.capacity_snapshot;
        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(snapshot.capacity_proof, proof) ||
            snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
            snapshot.snapshot_id.IsNull() ||
            snapshot.resource_commitment.IsNull() ||
            snapshot.session_id != recovery.session_id ||
            snapshot.attempt_id != recovery.recovery_id ||
            snapshot.provider_id != recovery.recovery_provider_id ||
            snapshot.client_nonce != recovery.client_nonce ||
            snapshot.request_hash !=
                Hash(CanonicalBytes(recovery.capacity_request)) ||
            snapshot.funding_model != FundingModel::USER_PAID ||
            snapshot.funding_model != proof.funding_model ||
            snapshot.requires_carrier != proof.requires_carrier ||
            snapshot.snapshot_id != proof.snapshot_id ||
            snapshot.resource_commitment !=
                GetCapacityResourceCommitment(proof) ||
            snapshot.created_at != proof.created_at ||
            snapshot.expires_at != proof.expires_at ||
            snapshot.validated_at <= 0 ||
            snapshot.validated_at != recovery.updated_at ||
            snapshot.expires_at <= snapshot.validated_at ||
            !ValidateCapacityProofEnvelope(
                proof, recovery.capacity_request, snapshot.validated_at,
                error) ||
            !recovery.recovery_provider_identity_key.IsFullyValid() ||
            GetPaymasterId(recovery.recovery_provider_identity_key) !=
                proof.provider_id ||
            !recovery.recovery_provider_identity_key.VerifySchnorr(
                GetCapacityProofSignatureHash(proof),
                proof.identity_signature) ||
            recovery.recovery_request.capacity_snapshot_id !=
                snapshot.snapshot_id ||
            recovery.recovery_request.capacity_resource_commitment !=
                snapshot.resource_commitment) {
            if (error.empty()) {
                error = "PAYMASTER_RECOVERY_CAPACITY_BINDING_MISMATCH";
            }
            return false;
        }

        if (!PrepareCapacitySnapshotMutation(batch, recovery.capacity_request,
                recovery.recovery_provider_identity_key, current.capacity_proof_claim_candidate,
                snapshot, proof, snapshot.validated_at, capacity_mutation, error)) return false;
        capacity_snapshot_changed = true;
    }

    bool client_fee_changed{false};
    ClientFeeLedger client_fee_ledger;
    if (!recovery.provider_side) {
        PaymentSession session;
        if (!batch.ReadPaymasterSession(recovery.request_id, session) ||
            session.provider_side || session.session_id != recovery.session_id ||
            session.user_inputs != recovery.recovery_request.user_dd_inputs) {
            error = "PAYMASTER_RECOVERY_ORIGINAL_SESSION_MISSING";
            return false;
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            InputReservation reservation;
            if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
                reservation.session_id != session.session_id ||
                reservation.role != ReservationRole::USER_DD ||
                !reservation.authorization_may_exist) {
                error = "PAYMASTER_RECOVERY_RESERVATION_MISSING";
                return false;
            }
        }
        if (recovery.phase >= AlternativeRecoveryPhase::REQUEST_READY) {
            for (const AlternativeRecoveryReturn& output :
                 recovery.recovery_request.wallet_returns) {
                if (!(m_wallet.IsMine(output.script_pub_key) & ISMINE_SPENDABLE)) {
                    error = "PAYMASTER_RECOVERY_DESTINATION_NOT_OWNED";
                    return false;
                }
            }
        }
        // The exact authorization and fee must already be durably reserved
        // before the wallet is ever asked to produce a USER signature.
        if (current.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED &&
            recovery.phase == AlternativeRecoveryPhase::USER_SIGNED) {
            if (current.accepted_recovery_authorization_commitment !=
                    current.recovery_authorization.authorization_commitment ||
                current.recovery_authorization_accepted_at <= 0 ||
                recovery.accepted_recovery_authorization_commitment !=
                    current.accepted_recovery_authorization_commitment ||
                recovery.recovery_authorization_accepted_at !=
                    current.recovery_authorization_accepted_at) {
                error = "PAYMASTER_RECOVERY_AUTHORIZATION_NOT_ACCEPTED";
                return false;
            }
            ClientSafetyPolicy client_policy;
            const bool have_policy =
                batch.ReadPaymasterClientSafetyPolicy(client_policy);
            const bool have_ledger =
                batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(),
                client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key ==
                           recovery.recovery_response.recovery_commit_key;
                });
            if (!have_policy || !have_ledger ||
                reservation == client_fee_ledger.reservations.end() ||
                reservation->service_fee !=
                    recovery.recovery_response.manifest.service_fee ||
                reservation->state != BudgetReservationState::RESERVED) {
                error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                return false;
            }
        }
    } else if (current.phase ==
                   AlternativeRecoveryPhase::RESPONSE_VALIDATED &&
               recovery.phase == AlternativeRecoveryPhase::USER_SIGNED) {
        // Persisting the first USER-signed recovery submit is the provider's
        // authorization boundary. A response-valid quote alone must not gain
        // historical authority after the operator changes either advertised
        // terms or the wallet-local safety policy.
        ProviderPolicy advertised_policy;
        ProviderSettings settings;
        ProviderSafetyPolicy safety_policy;
        ProviderBudgetLedger budget_ledger;
        if (!batch.ReadPaymasterPolicy(advertised_policy) ||
            !batch.ReadPaymasterSettings(settings) ||
            !batch.ReadPaymasterProviderSafetyPolicy(safety_policy) ||
            !batch.ReadPaymasterProviderBudgetLedger(budget_ledger)) {
            error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
            return false;
        }
        const uint256 advertised_policy_hash{
            GetProviderPolicyHash(advertised_policy)};
        if (!settings.enabled) {
            error = "PAYMASTER_PROVIDER_NOT_RUNNING";
            return false;
        }
        if (settings.policy_hash != advertised_policy_hash ||
            current.policy_hash != advertised_policy_hash ||
            !ValidateProviderSafetyPolicy(
                safety_policy, advertised_policy, error) ||
            !ValidateProviderAlternativeRecoveryBudgetAuthorization(
                current, &safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/false, error)) {
            if (error.empty()) {
                error =
                    "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
            }
            return false;
        }
    }

    const bool client_authorization = !recovery.provider_side && recovery.phase >= AlternativeRecoveryPhase::USER_SIGNED;
    if (client_fee_changed || capacity_snapshot_changed || client_authorization) {
        if (capacity_snapshot_changed) {
            if (!BeginCapacitySnapshotMutation(batch, recovery.capacity_snapshot, capacity_mutation, error)) return false;
        } else if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterAlternativeRecovery(recovery, true) ||
            (client_fee_changed && !batch.WritePaymasterClientFeeLedger(client_fee_ledger))) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!CommitWithPaymasterAuthorizationProtection(m_wallet, batch, client_authorization, error)) return false;
    } else if (!batch.WritePaymasterAlternativeRecovery(recovery, true)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::AcceptAlternativeRecoveryAuthorization(
    const std::string& request_id,
    const uint256& authorization_commitment,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) ||
        authorization_commitment.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_RECOVERY_AUTHORIZATION_ACCEPTANCE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 recovery_id;
    AlternativeRecoveryRecord recovery;
    PaymentSession session;
    if (!batch.ReadPaymasterAlternativeRecoveryRequest(request_id,
                                                       recovery_id) ||
        !batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        recovery.provider_side || recovery.request_id != request_id ||
        recovery.session_id != session.session_id || session.provider_side ||
        session.user_inputs != recovery.recovery_request.user_dd_inputs) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }
    if (recovery.expired ||
        recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_NOT_AWAITING_ACCEPTANCE";
        return false;
    }
    if (authorization_commitment !=
        recovery.recovery_authorization.authorization_commitment) {
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH";
        return false;
    }

    const int64_t effective_now =
        std::max(now, std::max(session.updated_at, recovery.updated_at));
    if (!ValidateRecoveryAuthorizationManifest(
            recovery.recovery_authorization, recovery.recovery_request,
            recovery.recovery_response, effective_now, error)) {
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD ||
            !reservation.authorization_may_exist) {
            error = "PAYMASTER_RECOVERY_RESERVATION_MISSING";
            return false;
        }
    }
    for (const AlternativeRecoveryReturn& output :
         recovery.recovery_request.wallet_returns) {
        if (!(m_wallet.IsMine(output.script_pub_key) & ISMINE_SPENDABLE)) {
            error = "PAYMASTER_RECOVERY_DESTINATION_NOT_OWNED";
            return false;
        }
    }

    const bool already_accepted =
        !recovery.accepted_recovery_authorization_commitment.IsNull() ||
        recovery.recovery_authorization_accepted_at != 0;
    if (already_accepted &&
        (recovery.accepted_recovery_authorization_commitment !=
             authorization_commitment ||
         recovery.recovery_authorization_accepted_at <= 0)) {
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_CONFLICT";
        return false;
    }

    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    if (!batch.ReadPaymasterClientSafetyPolicy(client_policy) ||
        !batch.ReadPaymasterClientFeeLedger(client_fee_ledger)) {
        error = "PAYMASTER_CLIENT_SAFETY_POLICY_REQUIRED";
        return false;
    }
    bool client_fee_changed{false};
    if (!RestoreClientRecoveryFeeReservations(batch, client_fee_ledger,
            effective_now, client_fee_changed, error)) return false;
    const auto existing_fee = std::find_if(
        client_fee_ledger.reservations.begin(),
        client_fee_ledger.reservations.end(),
        [&](const ClientFeeReservation& reservation) {
            return reservation.commit_key ==
                   recovery.recovery_response.recovery_commit_key;
        });
    if (!already_accepted && !CheckPaymasterRecoveryServiceFee(
            m_wallet, recovery.recovery_response.manifest, error)) return false;
    const bool exact_reserved_fee =
        existing_fee != client_fee_ledger.reservations.end() &&
        existing_fee->service_fee ==
            recovery.recovery_response.manifest.service_fee &&
        existing_fee->state == BudgetReservationState::RESERVED;
    if (!already_accepted && exact_reserved_fee) {
        // Only accepted_recovery_authorization_commitment turns the exact fee
        // reservation into historical authority. An orphan reservation must
        // fail closed rather than being silently repaired into acceptance.
        error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_ORPHAN";
        return false;
    }
    if (!exact_reserved_fee) {
        if (!ReserveClientFee(
                client_fee_ledger, client_policy,
                recovery.recovery_response.recovery_commit_key,
                recovery.recovery_response.manifest.service_fee,
                effective_now, error)) {
            return false;
        }
        client_fee_changed = true;
    }
    if (already_accepted && !client_fee_changed) return true;

    AlternativeRecoveryRecord accepted{recovery};
    if (!already_accepted) {
        accepted.accepted_recovery_authorization_commitment =
            authorization_commitment;
        accepted.recovery_authorization_accepted_at = effective_now;
        accepted.updated_at = effective_now;
    }
    if (!ValidateAlternativeRecoveryRecordShape(accepted, error)) {
        return false;
    }
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((!already_accepted &&
         !batch.WritePaymasterAlternativeRecovery(accepted, true)) ||
        (client_fee_changed &&
         !batch.WritePaymasterClientFeeLedger(client_fee_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetAlternativeRecovery(
    const std::string& request_id,
    AlternativeRecoveryRecord& recovery) const
{
    return GetAlternativeRecoveryWithStatus(request_id, recovery) ==
           DatabaseReadStatus::FOUND;
}

DatabaseReadStatus PaymasterStore::GetAlternativeRecoveryWithStatus(
    const std::string& request_id,
    AlternativeRecoveryRecord& recovery) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 recovery_id;
    const DatabaseReadStatus index_status =
        batch.ReadPaymasterAlternativeRecoveryRequestWithStatus(request_id, recovery_id);
    if (index_status != DatabaseReadStatus::FOUND) return index_status;
    const DatabaseReadStatus status =
        batch.ReadPaymasterAlternativeRecoveryWithStatus(recovery_id, recovery);
    // An existing index must resolve to its durable record, not authorize
    // the original payment path by treating broken recovery state as absent.
    if (status == DatabaseReadStatus::NOT_FOUND) return DatabaseReadStatus::READ_ERROR;
    if (status != DatabaseReadStatus::FOUND) return status;
    return !recovery.provider_side && recovery.request_id == request_id
               ? DatabaseReadStatus::FOUND
               : DatabaseReadStatus::READ_ERROR;
}

bool PaymasterStore::GetAlternativeRecoveryById(
    const uint256& recovery_id,
    AlternativeRecoveryRecord& recovery) const
{
    return GetAlternativeRecoveryByIdWithStatus(recovery_id, recovery) ==
           DatabaseReadStatus::FOUND;
}

DatabaseReadStatus PaymasterStore::GetAlternativeRecoveryByIdWithStatus(
    const uint256& recovery_id,
    AlternativeRecoveryRecord& recovery) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterAlternativeRecoveryWithStatus(
        recovery_id, recovery);
}

bool PaymasterStore::ListClientAlternativeRecoveries(
    std::vector<AlternativeRecoveryRecord>& recoveries,
    std::string& error) const
{
    recoveries.clear();
    error.clear();
    LOCK(m_wallet.cs_wallet);
    if (!WalletBatch{m_wallet.GetDatabase()}
             .ListPaymasterAlternativeRecoveries(recoveries)) {
        error = "PAYMASTER_RECOVERY_DATABASE_READ";
        return false;
    }
    recoveries.erase(
        std::remove_if(recoveries.begin(), recoveries.end(),
                       [](const AlternativeRecoveryRecord& recovery) {
                           return recovery.provider_side;
                       }),
        recoveries.end());
    return true;
}

bool PaymasterStore::GetProviderAlternativeRecoveryByCommit(
    const ProviderCommitRecord& commit,
    AlternativeRecoveryRecord& recovery,
    std::string& error) const
{
    error.clear();
    recovery = {};
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    const auto status = batch.ReadPaymasterAlternativeRecoveryWithStatus(commit.quote_id, recovery);
    if (status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(status, "AlternativeRecoveryRecord", recovery,
            "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND", "PAYMASTER_RECOVERY_DATABASE_READ");
        recovery = {};
        return false;
    }
    CMutableTransaction transaction;
    std::vector<COutPoint> provider_inputs;
    if (!ValidateProviderAlternativeRecoveryCommitBinding(
            recovery, commit, transaction, provider_inputs, error)) {
        recovery = {};
        return false;
    }
    return true;
}

bool PaymasterStore::HasProviderDrainWork(
    const PaymasterId& provider_id,
    bool& has_work,
    std::string& error) const
{
    has_work = false;
    error.clear();
    if (provider_id.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_ID";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymentSession> sessions;
    std::vector<AlternativeRecoveryRecord> recoveries;
    if (!batch.ListPaymasterSessions(sessions) ||
        !batch.ListPaymasterAlternativeRecoveries(recoveries)) {
        error = "PAYMASTER_DATABASE_READ";
        return false;
    }
    for (const PaymentSession& session : sessions) {
        if (!session.provider_side) continue;
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) {
                error = "PAYMASTER_ATTEMPT_DATABASE_READ";
                return false;
            }
            if (attempt.provider_id != provider_id) continue;
            if (attempt.state == AttemptState::QUOTED ||
                attempt.state == AttemptState::USER_SIGNED ||
                attempt.state == AttemptState::USER_PSBT_ACCEPTED ||
                attempt.state == AttemptState::PROVIDER_SIGNED) {
                has_work = true;
                return true;
            }
        }
    }
    has_work = std::any_of(
        recoveries.begin(), recoveries.end(),
        [&](const AlternativeRecoveryRecord& recovery) {
            return recovery.provider_side && !recovery.expired &&
                   recovery.recovery_provider_id == provider_id &&
                   (recovery.phase ==
                        AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
                    recovery.phase ==
                        AlternativeRecoveryPhase::USER_SIGNED);
        });
    if (has_work) return true;

    // A signed capacity proof is durable provider work too. The proof has
    // already committed exact operational pool resources to one client nonce,
    // so a restart must keep the provider online long enough to accept only
    // that bound quote continuation even when no unreserved pool slot remains.
    const int64_t now{GetTime()};
    if (now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EXPIRY_TIME";
        return false;
    }
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != provider_id) {
        error = "PAYMASTER_CAPACITY_IDENTITY_MISSING";
        return false;
    }
    ProviderBudgetLedger budget_ledger;
    const auto budget_status = batch.ReadPaymasterProviderBudgetLedgerWithStatus(budget_ledger);
    if (budget_status == DatabaseReadStatus::NOT_FOUND) return true;
    if (budget_status != DatabaseReadStatus::FOUND ||
        !ValidateProviderBudgetLedger(budget_ledger, error)) {
        if (error.empty()) {
            error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        }
        return false;
    }
    // The bounded authoritative ledger decides which capacity admissions can
    // still continue. Expired replay caches grant no work; do not rescan their
    // possibly large historical response bytes on every readiness observation.
    if (std::none_of(budget_ledger.capacity_admissions.begin(), budget_ledger.capacity_admissions.end(),
            [now](const auto& admission) {
                return admission.state == CapacityAdmissionState::RESERVED && admission.expires_at > now;
            })) return true;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterProviderPool(pool) ||
        !ValidateProviderPoolEntries(pool, error)) {
        if (error.empty()) error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }
    std::map<COutPoint, const ProviderPoolEntry*> pool_by_outpoint;
    for (const ProviderPoolEntry& entry : pool) {
        pool_by_outpoint.emplace(entry.outpoint, &entry);
    }

    for (const auto& active_admission : budget_ledger.capacity_admissions) {
        if (active_admission.state != CapacityAdmissionState::RESERVED || active_admission.expires_at <= now) continue;
        const auto& request_hash = active_admission.request_hash;
        std::vector<unsigned char> response;
        if (batch.ReadPaymasterCapacityResponseWithStatus(request_hash, response) != DatabaseReadStatus::FOUND) {
            error = "PAYMASTER_CAPACITY_DATABASE_READ";
            return false;
        }
        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(response, proof)) {
            error = "PAYMASTER_CAPACITY_RESPONSE_ENCODING";
            return false;
        }
        if (!DigiDollar::Paymaster::IsSupportedDirectProtocolVersion(proof.version)) {
            error = PersistedVersionError(
                "PaymasterCapacityProof", proof.version,
                DigiDollar::Paymaster::PROTOCOL_VERSION,
                "PAYMASTER_INVALID_PERSISTED_CAPACITY_PROOF");
            return false;
        }

        PaymasterCapacityRequest request;
        request.version = proof.version;
        request.genesis_hash = proof.genesis_hash;
        request.provider_id = proof.provider_id;
        request.request_id = proof.request_id;
        request.session_id = proof.session_id;
        request.client_nonce = proof.client_nonce;
        request.funding_model = proof.funding_model;
        request.requires_carrier = proof.requires_carrier;
        request.requested_slots =
            static_cast<uint16_t>(proof.liquidity_slots.size());
        request.created_at = proof.created_at;
        request.expires_at = proof.expires_at;
        std::string validation_error;
        if (GetPersistedCapacityRequestHash(proof) != request_hash ||
            proof.genesis_hash != Params().GenesisBlock().GetHash() ||
            proof.provider_id != provider_id ||
            !ValidateCapacityProofEnvelope(proof, request, proof.created_at,
                                           validation_error) ||
            !identity.identity_key.VerifySchnorr(
                GetCapacityProofSignatureHash(proof),
                proof.identity_signature)) {
            error = validation_error.empty() ? "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH" : std::move(validation_error);
            return false;
        }

        uint256 indexed_request_hash;
        if (!batch.ReadPaymasterCapacityNonce(proof.client_nonce,
                                              indexed_request_hash) ||
            indexed_request_hash != request_hash ||
            !batch.ReadPaymasterCapacitySession(
                GetPersistedCapacitySessionKey(proof),
                indexed_request_hash) ||
            indexed_request_hash != request_hash) {
            error = "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH";
            return false;
        }

        const uint256 admission_key = GetProviderRequestSlotKey(
            proof.provider_id, proof.request_id, proof.session_id);
        const auto* admission = &active_admission;
        if (admission->request_key != admission_key || admission->request_hash != request_hash ||
            admission->funding_model != proof.funding_model ||
            admission->requires_carrier != proof.requires_carrier ||
            admission->expires_at != proof.expires_at) {
            error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
            return false;
        }

        ProviderCapacityReleaseRecord release;
        const auto release_status = batch.ReadPaymasterCapacityReleaseWithStatus(request_hash, release);
        const bool have_release = release_status == DatabaseReadStatus::FOUND;
        if (release_status != DatabaseReadStatus::FOUND && release_status != DatabaseReadStatus::NOT_FOUND) {
            error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
            return false;
        }
        if (have_release) {
            if (release.request_hash != request_hash ||
                release.client_nonce != proof.client_nonce ||
                admission->state != CapacityAdmissionState::RELEASED ||
                std::any_of(pool.begin(), pool.end(), [&](const auto& entry) {
                    return entry.reservation_id == proof.client_nonce;
                })) {
                error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
                return false;
            }
            continue;
        }
        if (admission->state == CapacityAdmissionState::RELEASED) {
            error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
            return false;
        }
        if (admission->state != CapacityAdmissionState::RESERVED ||
            proof.expires_at <= now) {
            continue;
        }

        std::set<COutPoint> proof_outpoints;
        bool exact_capacity_reservation{true};
        for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
            for (const CapacityDGBInput& input : slot.dgb_inputs) {
                proof_outpoints.insert(input.input.outpoint);
                const auto found = pool_by_outpoint.find(input.input.outpoint);
                if (found == pool_by_outpoint.end() ||
                    found->second->state != PoolEntryState::RESERVED ||
                    found->second->reservation_id != proof.client_nonce ||
                    !CapacityProofMatchesPoolEntry(input, *found->second)) {
                    exact_capacity_reservation = false;
                }
            }
            if (slot.carrier) {
                proof_outpoints.insert(slot.carrier->carrier.outpoint);
                const auto found =
                    pool_by_outpoint.find(slot.carrier->carrier.outpoint);
                if (found == pool_by_outpoint.end() ||
                    found->second->state != PoolEntryState::RESERVED ||
                    found->second->reservation_id != proof.client_nonce ||
                    !CapacityProofMatchesPoolEntry(*slot.carrier,
                                                   *found->second)) {
                    exact_capacity_reservation = false;
                }
            }
        }
        if (std::any_of(pool.begin(), pool.end(), [&](const auto& entry) {
                return entry.reservation_id == proof.client_nonce &&
                       proof_outpoints.count(entry.outpoint) == 0;
            })) {
            exact_capacity_reservation = false;
        }
        if (!exact_capacity_reservation) {
            error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
            return false;
        }
        has_work = true;
        return true;
    }
    return true;
}

bool PaymasterStore::ExpireAlternativeRecoveries(
    int64_t now,
    size_t& expired_recoveries,
    std::string& error)
{
    expired_recoveries = 0;
    error.clear();
    if (now <= 0) {
        error = "PAYMASTER_INVALID_RECOVERY_EXPIRY_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymasterScanPage page{m_wallet.GetDatabase(), PaymasterScan::RECOVERIES};
    uint256 next;
    std::vector<AlternativeRecoveryRecord> recoveries;
    if (!batch.ListPaymasterAlternativeRecoveriesPage(page.after.record, recoveries, next)) {
        error = "PAYMASTER_RECOVERY_DATABASE_READ";
        return false;
    }
    std::vector<AlternativeRecoveryRecord> expiring;
    std::vector<AlternativeRecoveryRecord> pruning;
    std::vector<std::pair<AlternativeRecoveryRecord, ExactFinalTransactionProof>> archiving;
    for (AlternativeRecoveryRecord& recovery : recoveries) {
        if (recovery.provider_side && recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED &&
            archiving.size() < MAX_PROVIDER_SESSION_PRUNES_PER_PASS) {
            std::optional<ExactFinalTransactionProof> confirmation_proof;
            if (!ObserveProviderRecoveryHistoryCompletion(m_wallet, batch, recovery, confirmation_proof, error)) return false;
            if (confirmation_proof) archiving.emplace_back(recovery, std::move(*confirmation_proof));
        }
        // Expired unsigned records remain replay barriers through every saved
        // protocol deadline and the retention interval. Never prune a signed
        // phase merely because its quote is old.
        if (recovery.expired &&
            pruning.size() < MAX_PROVIDER_SESSION_PRUNES_PER_PASS &&
            TimeDeltaExceeds(now, std::max({recovery.updated_at,
                recovery.capacity_request.expires_at, recovery.capacity_snapshot.expires_at,
                recovery.recovery_response.expires_at}), CAPACITY_REPLAY_RETENTION_SECONDS)) {
            if (!ValidateAlternativeRecoveryRecordShape(recovery, error) ||
                recovery.phase > AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
                !recovery.user_signed_psbt.empty() || !recovery.final_transaction.empty() ||
                CanonicalBytes(recovery.signed_result) != CanonicalBytes(PaymasterResult{})) {
                if (error.empty()) error = "PAYMASTER_UNSIGNED_RECOVERY_PRUNE_UNSAFE";
                return false;
            }
            pruning.push_back(recovery);
        }
        if (recovery.expired ||
            recovery.phase > AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
            continue;
        }
        const int64_t expires_at =
            recovery.phase >= AlternativeRecoveryPhase::RESPONSE_VALIDATED ? recovery.recovery_response.expires_at : recovery.capacity_request.expires_at;
        if (expires_at > now) continue;
        if (recovery.provider_side &&
            recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
            continue;
        }
        recovery.expired = true;
        recovery.updated_at = std::max(recovery.updated_at, now);
        std::string validation_error;
        if (!ValidateAlternativeRecoveryRecordShape(recovery,
                                                    validation_error)) {
            error = validation_error.empty() ? "PAYMASTER_INVALID_EXPIRED_RECOVERY" : validation_error;
            return false;
        }
        expiring.push_back(std::move(recovery));
    }
    if (expiring.empty() && pruning.empty() && archiving.empty()) {
        page.Complete(next);
        return true;
    }

    // Expiry releases financial bindings before the later pruning pass. Prove
    // absence of independent authority here too, not only before deletion.
    for (const auto& recovery : expiring) {
        if (recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED) continue;
        const auto& key = recovery.recovery_response.recovery_commit_key;
        if (!RequireNoAuthorizationEvidence(batch, key, error)) return false;
    }

    std::vector<ProviderPoolEntry> pool;
    ProviderBudgetLedger provider_ledger;
    const bool expire_provider = std::any_of(
        expiring.begin(), expiring.end(),
        [](const AlternativeRecoveryRecord& recovery) {
            return recovery.provider_side;
        });
    const bool prune_provider = std::any_of(pruning.begin(), pruning.end(),
        [](const auto& recovery) { return recovery.provider_side; });
    if ((expire_provider || prune_provider) &&
        (!batch.ReadPaymasterProviderPool(pool) ||
         !batch.ReadPaymasterProviderBudgetLedger(provider_ledger))) {
        error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    ClientFeeLedger client_fee_ledger;
    const bool expire_client_fee = std::any_of(
        expiring.begin(), expiring.end(),
        [](const AlternativeRecoveryRecord& recovery) {
            return !recovery.provider_side &&
                   !recovery.accepted_recovery_authorization_commitment.IsNull();
        });
    const bool prune_client = std::any_of(pruning.begin(), pruning.end(),
        [](const auto& recovery) { return !recovery.provider_side; });
    const auto client_fee_status = (expire_client_fee || prune_client)
        ? batch.ReadPaymasterClientFeeLedgerWithStatus(client_fee_ledger)
        : DatabaseReadStatus::NOT_FOUND;
    if ((expire_client_fee && client_fee_status != DatabaseReadStatus::FOUND) ||
        (prune_client && client_fee_status != DatabaseReadStatus::FOUND &&
         client_fee_status != DatabaseReadStatus::NOT_FOUND)) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }

    // A label is insufficient proof that signing authority is gone. Check
    // independent durable evidence and released financial bindings before
    // deleting any replay barrier; unreadable evidence also vetoes pruning.
    for (const auto& recovery : pruning) {
        const auto& commit_key = recovery.recovery_response.recovery_commit_key;
        if (!commit_key.IsNull() && !RequireNoAuthorizationEvidence(batch, commit_key, error)) return false;
        if (!recovery.provider_side) {
            if ((!recovery.accepted_recovery_authorization_commitment.IsNull() &&
                 client_fee_status != DatabaseReadStatus::FOUND) ||
                std::any_of(client_fee_ledger.reservations.begin(), client_fee_ledger.reservations.end(),
                    [&](const ClientFeeReservation& entry) {
                        return entry.commit_key == commit_key && entry.state != BudgetReservationState::RELEASED;
                    })) {
                error = "PAYMASTER_UNSIGNED_RECOVERY_PRUNE_UNSAFE";
                return false;
            }
            continue;
        }
        if (
            std::any_of(pool.begin(), pool.end(), [&](const ProviderPoolEntry& entry) {
                return entry.reservation_id == commit_key || entry.origin_commit_key == commit_key;
            }) ||
            std::any_of(provider_ledger.reservations.begin(), provider_ledger.reservations.end(),
                [&](const ProviderBudgetReservation& entry) {
                    return entry.commit_key == commit_key && entry.state != BudgetReservationState::RELEASED;
                }) ||
            std::any_of(provider_ledger.capacity_admissions.begin(), provider_ledger.capacity_admissions.end(),
                [&](const ProviderCapacityAdmission& entry) {
                    return entry.commit_key == commit_key && entry.state == CapacityAdmissionState::RESERVED;
                })) {
            error = "PAYMASTER_UNSIGNED_RECOVERY_PRUNE_UNSAFE";
            return false;
        }
    }

    for (const AlternativeRecoveryRecord& recovery : expiring) {
        if (recovery.provider_side) {
            const uint256& commit_key =
                recovery.recovery_response.recovery_commit_key;
            std::set<COutPoint> remaining{
                recovery.recovery_response.manifest
                    .recovery_provider_carrier_inputs.begin(),
                recovery.recovery_response.manifest
                    .recovery_provider_carrier_inputs.end()};
            remaining.insert(
                recovery.recovery_response.manifest
                    .recovery_provider_dgb_inputs.begin(),
                recovery.recovery_response.manifest
                    .recovery_provider_dgb_inputs.end());
            for (ProviderPoolEntry& entry : pool) {
                if (entry.reservation_id != commit_key) continue;
                if (entry.state != PoolEntryState::RESERVED ||
                    remaining.erase(entry.outpoint) != 1) {
                    error = "PAYMASTER_RECOVERY_PROVIDER_POOL_MISMATCH";
                    return false;
                }
                entry.state = PoolEntryState::AVAILABLE;
                entry.reservation_id.SetNull();
                entry.updated_at = now;
            }
            if (!remaining.empty() ||
                !ReleaseProviderBudget(provider_ledger, commit_key, now,
                                       error)) {
                if (error.empty()) {
                    error = "PAYMASTER_RECOVERY_PROVIDER_POOL_MISMATCH";
                }
                return false;
            }
        } else if (!recovery.accepted_recovery_authorization_commitment.IsNull()) {
            const uint256& commit_key =
                recovery.recovery_response.recovery_commit_key;
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(),
                client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key == commit_key;
                });
            if (reservation == client_fee_ledger.reservations.end() ||
                reservation->service_fee !=
                    recovery.recovery_response.manifest.service_fee ||
                reservation->state != BudgetReservationState::RESERVED ||
                !ReleaseClientFee(client_fee_ledger, commit_key, now,
                                  error)) {
                if (error.empty()) {
                    error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                }
                return false;
            }
        }
    }

    // The wallet's confirmation cache can lag a reorg notification. Canonical
    // nonblocking preflight supplied every archival proof; keep its exact tip
    // current at the decision immediately before the atomic wallet batch.
    // Subsequent reorgs cannot lose evidence: archive point reads retain it.
    {
        LOCK(cs_main);
        for (const auto& candidate : archiving) {
            auto* node = m_wallet.chain().context();
            if (!node || !node->chainman || !candidate.second.Matches(*node->chainman, error)) {
                if (error.empty()) error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
                return false;
            }
        }
    }
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    // Expired client records can be detached from a later recovery of the
    // same payment. Never erase that replacement's locator during cleanup.
    const auto erase_owned_locator = [&](const AlternativeRecoveryRecord& recovery) {
        if (recovery.provider_side) return true;
        uint256 current;
        const auto status = batch.ReadPaymasterAlternativeRecoveryRequestWithStatus(recovery.request_id, current);
        if (status == DatabaseReadStatus::NOT_FOUND) return true;
        return status == DatabaseReadStatus::FOUND &&
            (current != recovery.recovery_id || batch.ErasePaymasterAlternativeRecoveryRequest(recovery.request_id));
    };
    for (const auto& candidate : archiving) {
        if (!batch.ArchivePaymasterAlternativeRecovery(candidate.first)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    for (const auto& recovery : pruning) {
        if (!erase_owned_locator(recovery) || !batch.ErasePaymasterAlternativeRecovery(recovery.recovery_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    for (const AlternativeRecoveryRecord& recovery : expiring) {
        if (!batch.WritePaymasterAlternativeRecovery(recovery, true) || !erase_owned_locator(recovery)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (expire_provider &&
        (!batch.WritePaymasterProviderPool(pool) ||
         !batch.WritePaymasterProviderBudgetLedger(provider_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (expire_client_fee &&
        !batch.WritePaymasterClientFeeLedger(client_fee_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    expired_recoveries = expiring.size();
    page.Complete(next);
    return true;
}

bool PaymasterStore::CommitProviderAlternativeRecoveryQuote(
    const AlternativeRecoveryRecord& recovery,
    const uint256& netgroup_bucket,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (!CheckPaymasterProviderRestoreGuard(m_wallet, error)) return false;
    if (expected_genesis.IsNull() ||
        recovery.recovery_request.genesis_hash != expected_genesis ||
        recovery.recovery_response.genesis_hash != expected_genesis ||
        recovery.capacity_request.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (!recovery.provider_side || recovery.expired ||
        recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
        !recovery.capacity_proof_claim_candidate.empty() ||
        netgroup_bucket.IsNull() || now <= 0 ||
        !ValidateAlternativeRecoveryRecordShape(recovery, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_QUOTE";
        return false;
    }
    const AlternativeRecoveryManifest& manifest =
        recovery.recovery_response.manifest;
    if (manifest.network_fee.value <= 0 || manifest.wallet_returns.empty()) {
        error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_QUOTE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord existing;
    const auto existing_status = batch.ReadPaymasterAlternativeRecoveryWithStatus(recovery.recovery_id, existing);
    if (existing_status != DatabaseReadStatus::FOUND && existing_status != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_RECOVERY_DATABASE_READ";
        return false;
    }
    const bool have_existing = existing_status == DatabaseReadStatus::FOUND;
    if (have_existing &&
        !SameAlternativeRecoveryRecord(existing, recovery)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT";
        return false;
    }
    if (!CheckProviderHistoryCapacity(batch, recovery.request_id, error, recovery.recovery_id)) return false;
    ProviderIdentityRecord identity;
    ProviderPolicy advertised_policy;
    ProviderSettings settings;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterIdentity(identity) ||
        !batch.ReadPaymasterPolicy(advertised_policy) ||
        !batch.ReadPaymasterSettings(settings) || !settings.enabled ||
        !batch.ReadPaymasterProviderSafetyPolicy(safety_policy) ||
        !batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !ValidateProviderBudgetLedger(budget_ledger, error) ||
        !batch.ReadPaymasterProviderPool(pool) ||
        !ValidateProviderPoolEntries(pool, error) ||
        identity.provider_id != recovery.recovery_provider_id ||
        identity.identity_key != recovery.recovery_provider_identity_key) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(
            safety_policy, advertised_policy, error)) {
        return false;
    }
    const int64_t effective_now =
        std::max(now, budget_ledger.accounting_time_high_water);
    if (effective_now <= 0 ||
        !ValidateAlternativeRecoveryRequestEnvelope(
            recovery.recovery_request, expected_genesis, effective_now,
            error) ||
        !ValidateAlternativeRecoveryResponse(
            recovery.recovery_response, recovery.recovery_request,
            identity.identity_key, effective_now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_QUOTE";
        return false;
    }
    if (recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        settings.policy_hash != GetProviderPolicyHash(advertised_policy) ||
        recovery.policy_hash != settings.policy_hash ||
        (!have_existing && recovery.provider_safety_policy_hash !=
                               GetProviderSafetyPolicyHash(safety_policy)) ||
        recovery.provider_budget_reservation_id !=
            recovery.recovery_response.recovery_commit_key ||
        recovery.provider_netgroup_bucket != netgroup_bucket ||
        !(recovery.provider_maximum_network_fee == manifest.network_fee)) {
        error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, expected_genesis,
            recovery.recovery_provider_id, recovery.request_id,
            recovery.session_id, recovery.client_nonce, effective_now,
            capacity_request, capacity_proof, capacity_request_hash, error) ||
        capacity_request.funding_model != FundingModel::USER_PAID ||
        capacity_request.requires_carrier !=
            !manifest.recovery_provider_carrier_inputs.empty() ||
        recovery.recovery_request_hash !=
            GetAlternativeRecoveryRequestHash(recovery.recovery_request) ||
        !ValidateRecoveryCapacityResources(
            recovery, capacity_request, capacity_proof,
            CanonicalBytes(capacity_proof), pool,
            have_existing ? recovery.recovery_response.recovery_commit_key : recovery.client_nonce,
            error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        return false;
    }
    const uint256 capacity_admission_key = GetProviderRequestSlotKey(
        recovery.recovery_provider_id, recovery.request_id,
        recovery.session_id);
    if (capacity_admission_key.IsNull() ||
        recovery.recovery_request_hash.IsNull() ||
        recovery.recovery_response.recovery_commit_key.IsNull()) {
        error = "PAYMASTER_INVALID_CAPACITY_PROMOTION";
        return false;
    }
    if (!ValidateProviderCapacityAdmissionForCommit(
            budget_ledger, capacity_request, capacity_request_hash,
            recovery.recovery_request_hash,
            recovery.recovery_response.recovery_commit_key, netgroup_bucket,
            have_existing ? CapacityAdmissionState::PROMOTED : CapacityAdmissionState::RESERVED,
            effective_now, error)) {
        return false;
    }

    if (have_existing) {
        if (!ValidateProviderAlternativeRecoveryBudgetAuthorization(
                recovery, &safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true, error)) {
            return false;
        }
        const std::vector<unsigned char> budget_before =
            CanonicalBytes(budget_ledger);
        const uint256 recipient_bucket = GetRecipientBudgetBucket(
            budget_ledger, manifest.wallet_returns.front().script_pub_key);
        if (recipient_bucket.IsNull() ||
            !BindProviderCapacityQuote(
                budget_ledger, capacity_admission_key,
                recovery.recovery_request_hash, netgroup_bucket,
                effective_now,
                error) ||
            !PromoteProviderCapacityAdmission(
                budget_ledger, capacity_admission_key,
                recovery.recovery_request_hash,
                recovery.recovery_response.recovery_commit_key,
                effective_now,
                error) ||
            !ReserveProviderBudget(
                budget_ledger, safety_policy, FundingModel::USER_PAID,
                SponsorshipScope::PUBLIC,
                recovery.recovery_response.recovery_commit_key,
                manifest.network_fee, recipient_bucket, effective_now, error,
                netgroup_bucket) ||
            !ValidateProviderAlternativeRecoveryBudgetAuthorization(
                recovery, &safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true, error)) {
            return false;
        }
        if (budget_before != CanonicalBytes(budget_ledger)) {
            error = "PAYMASTER_PROVIDER_BUDGET_ATOMICITY_CONFLICT";
            return false;
        }
        return true;
    }

    std::set<COutPoint> expected_inputs{
        manifest.recovery_provider_carrier_inputs.begin(),
        manifest.recovery_provider_carrier_inputs.end()};
    expected_inputs.insert(manifest.recovery_provider_dgb_inputs.begin(),
                           manifest.recovery_provider_dgb_inputs.end());
    size_t rebound{0};
    for (ProviderPoolEntry& entry : pool) {
        if (expected_inputs.count(entry.outpoint) == 0) continue;
        // ValidateRecoveryCapacityResources has already matched every role,
        // creating transaction, amount, script, state and nonce reservation.
        entry.reservation_id = recovery.recovery_response.recovery_commit_key;
        entry.updated_at = effective_now;
        ++rebound;
    }
    if (rebound != expected_inputs.size()) {
        error = "PAYMASTER_RECOVERY_PROVIDER_POOL_MISMATCH";
        return false;
    }

    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, manifest.wallet_returns.front().script_pub_key);
    if (recipient_bucket.IsNull() ||
        !BindProviderCapacityQuote(
            budget_ledger, capacity_admission_key,
            recovery.recovery_request_hash, netgroup_bucket, effective_now,
            error) ||
        !PromoteProviderCapacityAdmission(
            budget_ledger, capacity_admission_key,
            recovery.recovery_request_hash,
            recovery.recovery_response.recovery_commit_key, effective_now,
            error) ||
        !ReserveProviderBudget(
            budget_ledger, safety_policy, FundingModel::USER_PAID,
            SponsorshipScope::PUBLIC,
            recovery.recovery_response.recovery_commit_key,
            manifest.network_fee, recipient_bucket, effective_now, error,
            netgroup_bucket) ||
        !ValidateProviderAlternativeRecoveryBudgetAuthorization(
            recovery, &safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false, error)) {
        if (error.empty()) error = "PAYMASTER_SAFETY_LIMIT_EXHAUSTED";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAlternativeRecovery(recovery, false) ||
        !batch.WritePaymasterProviderPool(pool) ||
        !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
    const AlternativeRecoveryRecord& recovery,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error) const
{
    error.clear();
    if (!CheckPaymasterProviderRestoreGuard(m_wallet, error)) return false;
    if (expected_genesis.IsNull() || now <= 0 ||
        recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        !recovery.provider_side || recovery.expired ||
        recovery.phase != AlternativeRecoveryPhase::USER_SIGNED ||
        recovery.user_signed_psbt.empty() || recovery.created_at <= 0 ||
        now < recovery.created_at ||
        recovery.capacity_request.genesis_hash != expected_genesis ||
        recovery.recovery_request.genesis_hash != expected_genesis ||
        recovery.recovery_response.genesis_hash != expected_genesis ||
        !ValidateAlternativeRecoveryRecordShape(recovery, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_RECOVERY_AUTHORIZATION_INVALID";
        }
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord persisted;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id,
                                                persisted) ||
        !SameAlternativeRecoveryRecord(persisted, recovery)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT";
        return false;
    }

    ProviderIdentityRecord identity;
    ProviderPolicy advertised_policy;
    ProviderSettings settings;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterIdentity(identity) ||
        !batch.ReadPaymasterPolicy(advertised_policy) ||
        !batch.ReadPaymasterSettings(settings) || !settings.enabled ||
        !batch.ReadPaymasterProviderSafetyPolicy(safety_policy) ||
        !batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !batch.ReadPaymasterProviderPool(pool) ||
        identity.provider_id != persisted.recovery_provider_id ||
        identity.identity_key !=
            persisted.recovery_provider_identity_key ||
        settings.policy_hash != persisted.policy_hash ||
        settings.policy_hash != GetProviderPolicyHash(advertised_policy)) {
        error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(
            safety_policy, advertised_policy, error) ||
        !ValidateProviderBudgetLedger(budget_ledger, error) ||
        !ValidateProviderPoolEntries(pool, error) ||
        !ValidateAlternativeRecoveryResponse(
            persisted.recovery_response, persisted.recovery_request,
            identity.identity_key, now, error) ||
        !ValidateProviderAlternativeRecoveryBudgetAuthorization(
            persisted, &safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, expected_genesis,
            persisted.recovery_provider_id, persisted.request_id,
            persisted.session_id, persisted.client_nonce, now,
            capacity_request, capacity_proof, capacity_request_hash, error) ||
        capacity_request.funding_model != FundingModel::USER_PAID ||
        capacity_request.requires_carrier !=
            !persisted.recovery_response.manifest
                 .recovery_provider_carrier_inputs.empty() ||
        persisted.recovery_request_hash !=
            GetAlternativeRecoveryRequestHash(
                persisted.recovery_request) ||
        !ValidateProviderCapacityAdmissionForCommit(
            budget_ledger, capacity_request, capacity_request_hash,
            persisted.recovery_request_hash,
            persisted.recovery_response.recovery_commit_key,
            persisted.provider_netgroup_bucket,
            CapacityAdmissionState::PROMOTED, now, error) ||
        !ValidateRecoveryCapacityResources(
            persisted, capacity_request, capacity_proof,
            CanonicalBytes(capacity_proof), pool,
            persisted.recovery_response.recovery_commit_key, error)) {
        if (error.empty()) {
            error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        }
        return false;
    }
    return true;
}

bool PaymasterStore::CommitProviderAlternativeRecoveryFinal(
    const AlternativeRecoveryRecord& recovery,
    const ProviderCommitRecord& commit,
    const PaymasterResult& result,
    const uint256& expected_genesis,
    std::string& error)
{
    error.clear();
    CMutableTransaction transaction;
    std::vector<COutPoint> provider_inputs;
    if (!ValidateProviderAlternativeRecoveryCommitBinding(
            recovery, commit, transaction, provider_inputs, error)) {
        return false;
    }
    AlternativeRecoveryParameters ownership_parameters;
    if (!BuildAlternativeRecoveryParametersFromRecord(
            m_wallet, recovery, ownership_parameters, error)) {
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord current;
    ProviderIdentityRecord identity;
    ProviderBudgetLedger budget_ledger;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id, current) ||
        !batch.ReadPaymasterIdentity(identity) ||
        !batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !batch.ReadPaymasterProviderPool(pool) ||
        identity.provider_id != recovery.recovery_provider_id ||
        !ValidatePaymasterResult(
            result, expected_genesis, recovery.recovery_provider_id,
            commit.commit_key, identity.identity_key, 1, error) ||
        !result.txid || *result.txid != commit.final_txid ||
        !result.raw_transaction_hash ||
        *result.raw_transaction_hash != commit.raw_transaction_hash ||
        !result.final_transaction ||
        CanonicalBytes(result) != CanonicalBytes(recovery.signed_result) ||
        SerializeResultTransaction(*result.final_transaction) !=
            commit.final_transaction) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_RESULT";
        return false;
    }

    ProviderCommitRecord existing_commit;
    PaymasterResult existing_result;
    const bool have_commit = batch.ReadPaymasterProviderCommit(
        commit.commit_key, existing_commit);
    const bool have_result = batch.ReadPaymasterResult(
        commit.commit_key, existing_result);
    ProviderFinanceMetadata finance;
    ProviderFinanceEvent finance_event;
    bool finance_changed{false};
    if (!PrepareProviderRecoveryFinanceEvent(batch, identity, recovery, commit,
                                            finance, finance_event, finance_changed, error)) return false;
    if (current.phase == AlternativeRecoveryPhase::FINAL_COMMITTED ||
        have_commit || have_result) {
        if (have_commit && have_result && SameCommit(existing_commit, commit) &&
            SameResult(existing_result, result) &&
            SameAlternativeRecoveryRecord(current, recovery) &&
            ValidateRetainedProviderRecoveryBudget(batch, current, budget_ledger, error)) {
            if (!finance_changed && m_wallet.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION)) return true;
            if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
            if (finance_changed && !batch.WritePaymasterFinanceEvents(finance, {finance_event}, error))
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            return CommitWithPaymasterAuthorizationProtection(m_wallet, batch, true, error);
        }
        if (!error.empty()) return false;
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT";
        return false;
    }
    if (current.phase != AlternativeRecoveryPhase::USER_SIGNED) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_TRANSITION_CONFLICT";
        return false;
    }
    if (!SameAlternativeRecoveryProgression(current, recovery)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_TRANSITION_CONFLICT";
        return false;
    }
    // USER_SIGNED is the durable provider-authorization boundary. Finalizing
    // may therefore use the exact historical reservation after a later policy
    // change, but the current-format reservation must still be present and
    // RESERVED.
    if (!ValidateProviderAlternativeRecoveryBudgetAuthorization(
            current, /*policy=*/nullptr, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }

    std::set<COutPoint> remaining{provider_inputs.begin(), provider_inputs.end()};
    for (ProviderPoolEntry& entry : pool) {
        if (entry.reservation_id != commit.commit_key) continue;
        if (entry.state != PoolEntryState::RESERVED ||
            remaining.erase(entry.outpoint) != 1) {
            error = "PAYMASTER_RECOVERY_PROVIDER_POOL_MISMATCH";
            return false;
        }
        entry.state = PoolEntryState::COMMITTED;
        entry.updated_at = commit.committed_at;
    }
    if (!remaining.empty() ||
        !SpendProviderBudget(budget_ledger, commit.commit_key,
                             commit.committed_at, error)) {
        if (error.empty()) error = "PAYMASTER_RECOVERY_PROVIDER_POOL_MISMATCH";
        return false;
    }
    // Keep the final recovery record and the atomic ledger transition bound to
    // the same SPENT reservation before committing either one.
    if (!ValidateProviderAlternativeRecoveryBudgetAuthorization(
            recovery, /*policy=*/nullptr, budget_ledger,
            BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAlternativeRecovery(recovery, true) ||
        !batch.WritePaymasterProviderCommit(commit, false) ||
        !batch.WritePaymasterResult(result, false) ||
        !batch.WritePaymasterProviderPool(pool) ||
        !batch.WritePaymasterProviderBudgetLedger(budget_ledger) ||
        (finance_changed && !batch.WritePaymasterFinanceEvents(finance, {finance_event}, error))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    return CommitWithPaymasterAuthorizationProtection(m_wallet, batch, true, error);
}

bool PaymasterStore::CommitClientAlternativeRecoveryFinal(
    const AlternativeRecoveryRecord& recovery,
    const SelfRecoveryRecord& final_recovery,
    std::string& error)
{
    error.clear();
    if (recovery.provider_side || recovery.expired ||
        recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
        !ValidateAlternativeRecoveryRecordShape(recovery, error) ||
        final_recovery.version != SelfRecoveryRecord::CURRENT_VERSION ||
        final_recovery.request_id != recovery.request_id ||
        final_recovery.session_id != recovery.session_id ||
        final_recovery.user_inputs != recovery.recovery_request.user_dd_inputs ||
        final_recovery.final_transaction != recovery.final_transaction ||
        final_recovery.recovery_txid !=
            recovery.recovery_response.manifest.unsigned_txid ||
        final_recovery.raw_transaction_hash !=
            Hash(final_recovery.final_transaction)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CLIENT_RECOVERY_COMMIT";
        return false;
    }
    AlternativeRecoveryParameters ownership_parameters;
    if (!BuildAlternativeRecoveryParametersFromRecord(
            m_wallet, recovery, ownership_parameters, error)) {
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord current;
    PaymentSession session;
    ClientFeeLedger client_fee_ledger;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id, current) ||
        !batch.ReadPaymasterSession(recovery.request_id, session) ||
        !batch.ReadPaymasterClientFeeLedger(client_fee_ledger) ||
        session.provider_side || session.session_id != recovery.session_id ||
        session.user_inputs != final_recovery.user_inputs) {
        error = "PAYMASTER_RECOVERY_ORIGINAL_SESSION_MISSING";
        return false;
    }
    for (const AlternativeRecoveryReturn& output :
         recovery.recovery_request.wallet_returns) {
        if (!(m_wallet.IsMine(output.script_pub_key) & ISMINE_SPENDABLE)) {
            error = "PAYMASTER_RECOVERY_DESTINATION_NOT_OWNED";
            return false;
        }
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD ||
            !reservation.authorization_may_exist) {
            error = "PAYMASTER_RECOVERY_RESERVATION_MISSING";
            return false;
        }
    }

    SelfRecoveryRecord existing;
    if (batch.ReadPaymasterRecovery(recovery.request_id, existing)) {
        if (existing.session_id == final_recovery.session_id &&
            existing.user_inputs == final_recovery.user_inputs &&
            existing.recovery_txid == final_recovery.recovery_txid &&
            existing.raw_transaction_hash ==
                final_recovery.raw_transaction_hash &&
            existing.final_transaction == final_recovery.final_transaction &&
            SameAlternativeRecoveryRecord(current, recovery)) {
            return true;
        }
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT";
        return false;
    }
    if (current.phase != AlternativeRecoveryPhase::USER_SIGNED) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_TRANSITION_CONFLICT";
        return false;
    }
    if (!SameAlternativeRecoveryProgression(current, recovery)) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_TRANSITION_CONFLICT";
        return false;
    }
    if (!SpendClientFee(
            client_fee_ledger,
            recovery.recovery_response.recovery_commit_key,
            final_recovery.created_at, error)) {
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAlternativeRecovery(recovery, true) ||
        !batch.WritePaymasterRecovery(final_recovery, false) ||
        !batch.WritePaymasterClientFeeLedger(client_fee_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.recovery_txid = final_recovery.recovery_txid;
    session.pending_phase = PendingPhase::PENDING_NETWORK;
    session.updated_at = std::max(session.updated_at, final_recovery.created_at);
    if (!batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    return CommitWithPaymasterAuthorizationProtection(m_wallet, batch, true, error);
}

bool PaymasterStore::RecordClientAlternativeRecoveryConflict(
    const std::string& request_id,
    const uint256& recovery_id,
    const uint256& expected_txid,
    const uint256& expected_wtxid,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || recovery_id.IsNull() ||
        expected_txid.IsNull() || expected_wtxid.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_RECOVERY_FINAL_CONFLICT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    AlternativeRecoveryRecord recovery;
    SelfRecoveryRecord final_recovery;
    uint256 indexed_recovery_id;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side ||
        !batch.ReadPaymasterAlternativeRecoveryRequest(
            request_id, indexed_recovery_id) ||
        indexed_recovery_id != recovery_id ||
        !batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.expired ||
        recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
        recovery.request_id != request_id ||
        recovery.session_id != session.session_id ||
        !batch.ReadPaymasterRecovery(request_id, final_recovery) ||
        final_recovery.session_id != session.session_id ||
        final_recovery.recovery_txid != expected_txid ||
        session.recovery_txid != expected_txid ||
        final_recovery.final_transaction != recovery.final_transaction) {
        error = "PAYMASTER_RECOVERY_FINAL_CONFLICT_BINDING_MISMATCH";
        return false;
    }

    ExactFinalArtifact artifact;
    if (!DecodeExactFinalArtifact(final_recovery.final_transaction,
                                  expected_txid, artifact, error)) {
        return false;
    }
    if (artifact.wtxid != expected_wtxid ||
        recovery.expected_wtxid != expected_wtxid ||
        final_recovery.raw_transaction_hash !=
            Hash(final_recovery.final_transaction) ||
        final_recovery.user_inputs != session.user_inputs ||
        !IsFinalResultStatus(recovery.signed_result.status) ||
        !recovery.signed_result.txid ||
        *recovery.signed_result.txid != expected_txid ||
        !recovery.signed_result.raw_transaction_hash ||
        *recovery.signed_result.raw_transaction_hash != expected_wtxid ||
        !recovery.signed_result.final_transaction ||
        SerializeResultTransaction(
            *recovery.signed_result.final_transaction) != artifact.bytes) {
        error = "PAYMASTER_RECOVERY_FINAL_CONFLICT_BINDING_MISMATCH";
        return false;
    }

    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.request_id != request_id ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD ||
            !reservation.authorization_may_exist) {
            error = "PAYMASTER_RECOVERY_RESERVATION_MISSING";
            return false;
        }
    }

    if (session.state == SessionState::CONFIRMED ||
        session.state == SessionState::CANCELED_SAFE ||
        session.state == SessionState::CONFLICTED) {
        return true;
    }
    if (IsTerminal(session.state) ||
        !CanTransition(session.state, SessionState::CONFLICTED)) {
        error = "PAYMASTER_INVALID_RECOVERY_FINAL_CONFLICT_TRANSITION";
        return false;
    }

    session.state = SessionState::CONFLICTED;
    session.pending_phase = PendingPhase::NONE;
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

} // namespace wallet
