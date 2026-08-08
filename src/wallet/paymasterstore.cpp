// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Durable Paymaster state and atomic wallet-database transitions.
 *
 * The store is the authority for sessions, attempts, manifests, reservations,
 * provider pools, and safety ledgers. Methods are designed for exact replay:
 * either the complete state transition is persisted in one WalletBatch or no
 * new signing/spending authority becomes visible after restart.
 */

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

// Wallet coin selection calls these helpers before it knows which Paymaster
// workflow owns an output. Keep this view deliberately conservative: both an
// explicit reservation and every live pool state make the outpoint unavailable
// to ordinary sends.
bool IsPaymasterInputReserved(const CWallet& wallet, const COutPoint& outpoint)
{
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    InputReservation reservation;
    const DatabaseReadStatus reservation_status{
        batch.ReadPaymasterReservationWithStatus(outpoint, reservation)};
    if (reservation_status == DatabaseReadStatus::FOUND) return true;
    if (reservation_status != DatabaseReadStatus::NOT_FOUND) return true;
    std::vector<ProviderPoolEntry> pool;
    const DatabaseReadStatus pool_status{
        batch.ReadPaymasterProviderPoolWithStatus(pool)};
    if (pool_status == DatabaseReadStatus::NOT_FOUND) return false;
    if (pool_status != DatabaseReadStatus::FOUND) return true;
    const auto entry = std::find_if(pool.begin(), pool.end(), [&](const ProviderPoolEntry& candidate) {
        return candidate.outpoint == outpoint;
    });
    // Every live provider-pool entry is dedicated Paymaster liquidity. An
    // AVAILABLE entry must not be consumed by an unrelated wallet send before
    // it can be reserved for a quote or used in an admission proof.
    return entry != pool.end() && IsActiveProviderPoolState(entry->state);
}

DatabaseReadStatus GetPaymasterProviderPoolInputs(
    const CWallet& wallet, std::set<COutPoint>& inputs)
{
    LOCK(wallet.cs_wallet);
    inputs.clear();
    std::vector<ProviderPoolEntry> pool;
    const DatabaseReadStatus status{
        WalletBatch{wallet.GetDatabase()}.ReadPaymasterProviderPoolWithStatus(
            pool)};
    if (status != DatabaseReadStatus::FOUND) return status;
    for (const ProviderPoolEntry& entry : pool) {
        if (IsActiveProviderPoolState(entry.state)) inputs.insert(entry.outpoint);
    }
    return DatabaseReadStatus::FOUND;
}

namespace paymaster_store::internal {

std::string PersistedVersionError(std::string_view record_type,
                                  uint16_t found,
                                  uint16_t expected,
                                  std::string_view invalid_error)
{
    if (found == expected) return std::string{invalid_error};
    return strprintf(
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=%s found=%u expected=%u",
        std::string{record_type}, found, expected);
}

std::string ProviderPoolReadError(
    const std::vector<ProviderPoolEntry>& entries)
{
    const auto outdated = std::find_if(
        entries.begin(), entries.end(), [](const ProviderPoolEntry& entry) {
            return entry.version != ProviderPoolEntry::CURRENT_VERSION;
        });
    if (outdated != entries.end()) {
        return PersistedVersionError(
            "ProviderPoolEntry", outdated->version,
            ProviderPoolEntry::CURRENT_VERSION,
            "PAYMASTER_INVALID_PROVIDER_POOL");
    }
    return "PAYMASTER_INVALID_PROVIDER_POOL";
}

// -------------------------------------------------------------------------
// Provider-pool successor reconstruction
// -------------------------------------------------------------------------
// A completed user-paid transfer normally consumes one DGB slot and one DD
// carrier, then creates wallet-owned replacements. Successors are accepted only
// from scripts already bound by the provider manifest; matching by amount or
// output position would let an otherwise valid transaction relabel an output as
// protected provider liquidity.

/** Value-initialize through ordinary default-initialization. Some consensus
 * transaction types intentionally expose an explicit default constructor, so
 * assigning an empty braced initializer is rejected by MSVC even though a
 * fresh object is well-defined. */
template <typename T>
void ResetDefault(T& value)
{
    T reset;
    value = std::move(reset);
}

bool Abort(WalletBatch& batch, std::string& error, const char* code)
{
    batch.TxnAbort();
    error = code;
    return false;
}

std::optional<uint32_t> FindUniqueOutputByScript(const CTransaction& transaction,
                                                 const CScript& script)
{
    std::optional<uint32_t> result;
    for (uint32_t index{0}; index < transaction.vout.size(); ++index) {
        if (transaction.vout[index].scriptPubKey != script) continue;
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}

bool SamePoolSuccessor(const ProviderPoolEntry& entry,
                       const ProviderPoolEntry& expected)
{
    return entry.outpoint == expected.outpoint &&
           entry.purpose == expected.purpose && entry.asset == expected.asset &&
           entry.script_pub_key == expected.script_pub_key &&
           entry.dgb_value == expected.dgb_value &&
           entry.carrier_value == expected.carrier_value &&
           entry.origin_commit_key == expected.origin_commit_key;
}

bool MakeProviderPoolRoom(std::vector<ProviderPoolEntry>& entries,
                          size_t additional,
                          std::string& error)
{
    while (entries.size() + additional > 256) {
        const auto oldest = std::min_element(
            entries.begin(), entries.end(),
            [](const ProviderPoolEntry& lhs, const ProviderPoolEntry& rhs) {
                if (IsActiveProviderPoolState(lhs.state) !=
                    IsActiveProviderPoolState(rhs.state)) {
                    return !IsActiveProviderPoolState(lhs.state);
                }
                return lhs.updated_at < rhs.updated_at;
            });
        if (oldest == entries.end() || IsActiveProviderPoolState(oldest->state)) {
            error = "PAYMASTER_POOL_TOO_LARGE";
            return false;
        }
        entries.erase(oldest);
    }
    return true;
}

/** Register wallet-owned outputs of an exact provider commit as durable pool
 * successors. The scripts originate in the provider authorization manifest,
 * so no output is inferred from value or position. */
bool RegisterProviderPoolSuccessors(const ProviderAttempt& attempt,
                                    const ProviderCommitRecord& commit,
                                    const CTransaction& transaction,
                                    const std::optional<ProviderPolicy>& policy,
                                    std::vector<ProviderPoolEntry>& entries,
                                    bool& changed,
                                    std::string& error)
{
    changed = false;
    const ProviderAuthorizationManifest& manifest = attempt.provider_manifest;
    if (manifest.manifest_id.IsNull()) return true;
    if (manifest.carrier_return_scripts.size() > 1 ||
        manifest.dgb_change_scripts.size() > 1) {
        error = "PAYMASTER_INVALID_PROVIDER_SUCCESSOR_MANIFEST";
        return false;
    }

    std::vector<ProviderPoolEntry> successors;
    if (!manifest.carrier_return_scripts.empty()) {
        if (manifest.provider_carrier_inputs.size() != 1) {
            error = "PAYMASTER_INVALID_PROVIDER_CARRIER_SUCCESSOR";
            return false;
        }
        const auto source = std::find_if(
            entries.begin(), entries.end(), [&](const ProviderPoolEntry& entry) {
                return entry.outpoint == manifest.provider_carrier_inputs.front() &&
                       entry.asset == PoolAsset::DD_CARRIER;
            });
        const auto output_index = FindUniqueOutputByScript(
            transaction, manifest.carrier_return_scripts.front());
        const auto successor_value = source == entries.end()
            ? std::optional<DDCents>{}
            : ComputeCarrierSuccessor(source->carrier_value,
                                      manifest.service_fee, error);
        if (source == entries.end() || !output_index || !successor_value) {
            if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_CARRIER_SUCCESSOR";
            return false;
        }
        CAmount on_chain_carrier_value{0};
        if (!DigiDollar::ExtractDDAmountFromTransaction(
                transaction,
                COutPoint{commit.final_txid, *output_index},
                on_chain_carrier_value) ||
            on_chain_carrier_value != successor_value->value) {
            error = "PAYMASTER_INVALID_PROVIDER_CARRIER_SUCCESSOR";
            return false;
        }
        ProviderPoolEntry successor;
        successor.outpoint = COutPoint{commit.final_txid, *output_index};
        successor.purpose = PoolPurpose::OPERATIONAL;
        successor.asset = PoolAsset::DD_CARRIER;
        successor.state = PoolEntryState::PENDING_SUCCESSOR;
        successor.script_pub_key = manifest.carrier_return_scripts.front();
        successor.carrier_value = *successor_value;
        successor.reservation_id = commit.commit_key;
        successor.origin_commit_key = commit.commit_key;
        successor.updated_at = commit.committed_at;
        successors.push_back(std::move(successor));
    }

    if (!manifest.dgb_change_scripts.empty()) {
        const auto output_index = FindUniqueOutputByScript(
            transaction, manifest.dgb_change_scripts.front());
        if (!output_index) {
            error = "PAYMASTER_INVALID_PROVIDER_DGB_SUCCESSOR";
            return false;
        }
        const CAmount output_value = transaction.vout[*output_index].nValue;
        const CAmount minimum_capacity = policy
            ? policy->maximum_network_fee.value
            : manifest.maximum_network_fee.value;
        if (output_value >= minimum_capacity && output_value > 0) {
            ProviderPoolEntry successor;
            successor.outpoint = COutPoint{commit.final_txid, *output_index};
            successor.purpose = PoolPurpose::OPERATIONAL;
            successor.asset = PoolAsset::DGB;
            successor.state = PoolEntryState::PENDING_SUCCESSOR;
            successor.script_pub_key = manifest.dgb_change_scripts.front();
            successor.dgb_value = DGBSatoshis{output_value};
            successor.reservation_id = commit.commit_key;
            successor.origin_commit_key = commit.commit_key;
            successor.updated_at = commit.committed_at;
            successors.push_back(std::move(successor));
        }
    }

    size_t missing{0};
    for (const ProviderPoolEntry& successor : successors) {
        const auto existing = std::find_if(
            entries.begin(), entries.end(), [&](const ProviderPoolEntry& entry) {
                return entry.outpoint == successor.outpoint;
            });
        if (existing == entries.end()) {
            ++missing;
        } else if (!SamePoolSuccessor(*existing, successor)) {
            error = "PAYMASTER_PROVIDER_SUCCESSOR_CONFLICT";
            return false;
        }
    }
    if (!MakeProviderPoolRoom(entries, missing, error)) return false;
    for (ProviderPoolEntry& successor : successors) {
        if (std::none_of(entries.begin(), entries.end(), [&](const ProviderPoolEntry& entry) {
                return entry.outpoint == successor.outpoint;
            })) {
            entries.push_back(std::move(successor));
            changed = true;
        }
    }
    return true;
}

/** Prepare the accounting side of a provider commit. The exact service fee
 * and network fee are bound in the provider authorization manifest, so this
 * event can share the same database transaction as the durable commit. */
bool PrepareProviderTransferFinanceEvent(
    WalletBatch& batch,
    const ProviderIdentityRecord& identity,
    const ProviderAttempt& attempt,
    const ProviderCommitRecord& commit,
    const uint256& expected_genesis,
    ProviderFinanceLedger& ledger,
    bool& changed,
    std::string& error)
{
    changed = false;
    const bool have_ledger = batch.ReadPaymasterFinanceLedger(ledger);
    if (!have_ledger && batch.HasPaymasterFinanceLedger()) {
        error = "PAYMASTER_INVALID_FINANCE_LEDGER";
        return false;
    }
    if (!have_ledger) {
        ledger.genesis_hash = expected_genesis;
        ledger.provider_id = identity.provider_id;
        ledger.history_complete_from = commit.committed_at;
        ledger.earlier_history_partial =
            identity.created_at < commit.committed_at;
        ledger.updated_at = commit.committed_at;
        if (!RebuildProviderFinanceDailyTotals(ledger, error)) return false;
        changed = true;
    }
    if (ledger.genesis_hash != expected_genesis ||
        ledger.provider_id != identity.provider_id) {
        error = "PAYMASTER_FINANCE_LEDGER_BINDING_MISMATCH";
        return false;
    }

    const ProviderAuthorizationManifest& manifest = attempt.provider_manifest;
    if (manifest.manifest_id.IsNull()) {
        error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        return false;
    }
    ProviderFinanceEvent event;
    event.event_id = commit.commit_key;
    event.genesis_hash = expected_genesis;
    event.provider_id = identity.provider_id;
    event.kind = ProviderFinanceEventKind::TRANSFER;
    event.state = ProviderFinanceEventState::PENDING;
    event.funding_model = manifest.funding_model;
    event.sponsorship_scope = manifest.sponsorship_scope;
    event.transaction_id = commit.final_txid;
    event.dd_income = manifest.service_fee;
    event.dgb_cost = manifest.network_fee;
    event.created_at = commit.committed_at;
    event.updated_at = commit.committed_at;

    const auto existing = std::find_if(
        ledger.events.begin(), ledger.events.end(),
        [&](const ProviderFinanceEvent& candidate) {
            return candidate.event_id == event.event_id;
        });
    if (existing != ledger.events.end()) {
        // Exact commit replay must not turn a previously confirmed event back
        // into pending after a restart or a harmless duplicate result.
        event.state = existing->state;
        event.confirmed_at = existing->confirmed_at;
        event.updated_at = std::max(existing->updated_at, event.updated_at);
    } else {
        changed = true;
    }
    return UpsertProviderFinanceEvent(ledger, event, error);
}

// -------------------------------------------------------------------------
// Durable authorization and budget invariants
// -------------------------------------------------------------------------
// Once either party may have signed, timeout alone can no longer release
// inputs or budget. These predicates centralize the distinction between a
// harmless unsigned attempt and an artifact that may still become an on-chain
// transaction after a restart or delayed peer message.
bool HasAuthorizationRisk(DigiDollar::Paymaster::SessionState state)
{
    using DigiDollar::Paymaster::SessionState;
    return state == SessionState::PENDING_PROVIDER || state == SessionState::STEMPOOL ||
           state == SessionState::MEMPOOL || state == SessionState::CONFIRMED ||
           state == SessionState::CANCELED_SAFE || state == SessionState::CONFLICTED;
}

bool AttemptHasReached(AttemptState state, AttemptState threshold)
{
    return state >= threshold && state <= AttemptState::MEMPOOL;
}

bool HasTimelyDurableProviderSignature(const ProviderAttempt& attempt)
{
    return attempt.state == AttemptState::PROVIDER_SIGNED &&
           !attempt.final_txid.IsNull() &&
           !attempt.final_transaction.empty() &&
           attempt.provider_signed_at > 0 &&
           attempt.provider_signed_at <= attempt.retry_until &&
           attempt.provider_signed_at <= attempt.updated_at &&
           !attempt.provider_signed_result.empty();
}

bool ValidateProviderBudgetState(
    const ProviderAttempt& attempt,
    const ProviderSafetyPolicy& policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error)
{
    if (!ValidateProviderBudgetLedger(ledger, error)) return false;
    const auto first = std::find_if(
        ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ProviderBudgetReservation& reservation) {
            return reservation.commit_key == attempt.commit_key;
        });
    if (first == ledger.reservations.end()) {
        error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
        return false;
    }
    if (std::find_if(std::next(first), ledger.reservations.end(),
                     [&](const ProviderBudgetReservation& reservation) {
                         return reservation.commit_key == attempt.commit_key;
                     }) != ledger.reservations.end()) {
        error = "PAYMASTER_BUDGET_RESERVATION_CONFLICT";
        return false;
    }
    return ValidateProviderBudgetReservationBinding(
        attempt.provider_manifest, attempt, *first, policy, expected_state,
        allow_historical_policy, error);
}

bool ValidateProviderAlternativeRecoveryBudgetAuthorizationImpl(
    const AlternativeRecoveryRecord& recovery,
    const ProviderSafetyPolicy* policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error)
{
    error.clear();
    if (!recovery.provider_side ||
        recovery.phase < AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
        recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        !ValidateProviderBudgetLedger(ledger, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        }
        return false;
    }

    if (recovery.provider_safety_policy_hash.IsNull() ||
        recovery.provider_budget_reservation_id !=
            recovery.recovery_response.recovery_commit_key ||
        recovery.provider_netgroup_bucket.IsNull() ||
        recovery.provider_maximum_network_fee.value <= 0 ||
        !(recovery.provider_maximum_network_fee ==
          recovery.recovery_response.manifest.network_fee) ||
        (!allow_historical_policy &&
         (!policy ||
          recovery.provider_safety_policy_hash !=
              GetProviderSafetyPolicyHash(*policy)))) {
        error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        return false;
    }

    const uint256 commit_key =
        recovery.recovery_response.recovery_commit_key;
    if (commit_key.IsNull() ||
        commit_key != GetAlternativeRecoveryCommitKey(
                          recovery.recovery_response) ||
        recovery.recovery_response.manifest.wallet_returns.empty()) {
        error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        return false;
    }
    const auto first = std::find_if(
        ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ProviderBudgetReservation& reservation) {
            return reservation.commit_key == commit_key;
        });
    if (first == ledger.reservations.end()) {
        error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
        return false;
    }
    if (std::find_if(
            std::next(first), ledger.reservations.end(),
            [&](const ProviderBudgetReservation& reservation) {
                return reservation.commit_key == commit_key;
            }) != ledger.reservations.end()) {
        error = "PAYMASTER_BUDGET_RESERVATION_CONFLICT";
        return false;
    }

    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        ledger,
        recovery.recovery_response.manifest.wallet_returns.front()
            .script_pub_key);
    if (first->version != ProviderBudgetReservation::CURRENT_VERSION ||
        first->funding_model != FundingModel::USER_PAID ||
        first->sponsorship_scope != SponsorshipScope::PUBLIC ||
        !(first->network_fee ==
          recovery.recovery_response.manifest.network_fee) ||
        first->recipient_bucket != recipient_bucket ||
        first->recipient_bucket.IsNull() || first->netgroup_bucket.IsNull() ||
        first->netgroup_bucket != recovery.provider_netgroup_bucket ||
        first->state != expected_state) {
        error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        return false;
    }
    return true;
}

// Exact equality helpers make retries idempotent. Reusing an identifier with
// different bytes is a conflict, never an update to the previous authorization.
bool SameCommit(const ProviderCommitRecord& lhs, const ProviderCommitRecord& rhs)
{
    return lhs.version == rhs.version && lhs.commit_key == rhs.commit_key &&
           lhs.provider_id == rhs.provider_id && lhs.quote_id == rhs.quote_id &&
           lhs.template_commitment == rhs.template_commitment &&
           lhs.final_txid == rhs.final_txid &&
           lhs.raw_transaction_hash == rhs.raw_transaction_hash &&
           lhs.final_transaction == rhs.final_transaction &&
           lhs.provider_inputs == rhs.provider_inputs && lhs.committed_at == rhs.committed_at &&
           lhs.retry_until == rhs.retry_until;
}

bool DecodeProviderSignedResultEnvelope(const ProviderAttempt& attempt,
                                        PaymasterResult& result,
                                        std::string& error)
{
    result = {};
    if (attempt.provider_signed_at <= 0 ||
        attempt.provider_signed_at > attempt.retry_until ||
        attempt.provider_signed_at > attempt.updated_at ||
        attempt.provider_signed_result.empty() ||
        !attempt.provider_identity_key.IsFullyValid()) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        return false;
    }
    try {
        SpanReader stream{::PROTOCOL_VERSION,
                          attempt.provider_signed_result};
        stream >> result;
        if (!stream.empty() ||
            CanonicalBytes(result) != attempt.provider_signed_result) {
            throw std::ios_base::failure(
                "non-canonical provider-signed result");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        return false;
    }
    if (!ValidatePaymasterResult(
            result, result.genesis_hash, attempt.provider_id,
            attempt.commit_key, attempt.provider_identity_key, 1, error) ||
        result.result_sequence != 1 ||
        result.status != PaymasterResultStatus::FINAL_COMMITTED ||
        result.updated_at != attempt.provider_signed_at || !result.txid ||
        *result.txid != attempt.final_txid ||
        !result.raw_transaction_hash ||
        *result.raw_transaction_hash != Hash(attempt.final_transaction) ||
        !result.final_transaction ||
        CanonicalBytes(*result.final_transaction) !=
            attempt.final_transaction) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        }
        return false;
    }
    error.clear();
    return true;
}

bool SameAlternativeRecoveryRecord(const AlternativeRecoveryRecord& lhs,
                                   const AlternativeRecoveryRecord& rhs)
{
    return CanonicalBytes(lhs) == CanonicalBytes(rhs);
}

/** Compare one adjacent durable recovery transition after erasing exactly the
 * fields that this transition is allowed to introduce. This is intentionally
 * canonical rather than field-by-field: newly added persisted authority must
 * not silently escape the immutable progression firewall. */
bool SameAlternativeRecoveryProgression(
    const AlternativeRecoveryRecord& current,
    const AlternativeRecoveryRecord& next)
{
    if (current.version != next.version || current.expired || next.expired ||
        next.updated_at < current.updated_at ||
        static_cast<uint8_t>(next.phase) !=
            static_cast<uint8_t>(current.phase) + 1) {
        return false;
    }

    AlternativeRecoveryRecord normalized{next};
    normalized.phase = current.phase;
    normalized.updated_at = current.updated_at;
    switch (current.phase) {
    case AlternativeRecoveryPhase::CAPACITY_PENDING:
        if (next.phase != AlternativeRecoveryPhase::REQUEST_READY) return false;
        normalized.capacity_snapshot = current.capacity_snapshot;
        normalized.recovery_request = current.recovery_request;
        normalized.recovery_request_hash = current.recovery_request_hash;
        break;
    case AlternativeRecoveryPhase::REQUEST_READY:
        if (next.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
            return false;
        }
        normalized.recovery_response = current.recovery_response;
        normalized.recovery_authorization = current.recovery_authorization;
        break;
    case AlternativeRecoveryPhase::RESPONSE_VALIDATED:
        if (next.phase != AlternativeRecoveryPhase::USER_SIGNED) return false;
        normalized.user_signed_psbt = current.user_signed_psbt;
        normalized.accepted_recovery_authorization_commitment =
            current.accepted_recovery_authorization_commitment;
        normalized.recovery_authorization_accepted_at =
            current.recovery_authorization_accepted_at;
        break;
    case AlternativeRecoveryPhase::USER_SIGNED:
        if (next.phase != AlternativeRecoveryPhase::FINAL_COMMITTED) return false;
        normalized.final_transaction = current.final_transaction;
        normalized.expected_wtxid = current.expected_wtxid;
        normalized.signed_result = current.signed_result;
        break;
    case AlternativeRecoveryPhase::FINAL_COMMITTED:
        return false;
    }
    return SameAlternativeRecoveryRecord(current, normalized);
}

// -------------------------------------------------------------------------
// Alternative-provider recovery
// -------------------------------------------------------------------------
// Recovery is intentionally represented as another fully bound authorization,
// not as permission to edit the original transaction. The replacement must
// return the client's DD inputs to wallet-owned scripts and has independent,
// finite provider-budget authorization.
bool ValidateAlternativeRecoveryRecordShape(
    const AlternativeRecoveryRecord& recovery,
    std::string& error)
{
    if (recovery.version != AlternativeRecoveryRecord::CURRENT_VERSION ||
        !IsCanonicalRequestId(recovery.request_id) || recovery.session_id.IsNull() ||
        recovery.recovery_id.IsNull() || recovery.original_provider_id.IsNull() ||
        recovery.recovery_provider_id.IsNull() || recovery.offer_id.IsNull() ||
        recovery.policy_hash.IsNull() ||
        recovery.original_provider_id == recovery.recovery_provider_id ||
        (recovery.privacy_profile != PrivacyProfile::STANDARD &&
         recovery.privacy_profile != PrivacyProfile::HIGH) ||
        !recovery.recovery_provider_identity_key.IsFullyValid() ||
        GetPaymasterId(recovery.recovery_provider_identity_key) !=
            recovery.recovery_provider_id ||
        recovery.recovery_provider_endpoint.empty() ||
        recovery.recovery_provider_endpoint.size() > MAX_RECOVERY_ENDPOINT_BYTES ||
        recovery.original_commit_key.IsNull() ||
        recovery.original_template_commitment.IsNull() ||
        recovery.selected_maximum_service_fee.value < 0 ||
        recovery.selected_service_fee.value < 0 ||
        recovery.selected_service_fee.value >
            recovery.selected_maximum_service_fee.value ||
        recovery.client_nonce.IsNull() || recovery.created_at <= 0 ||
        recovery.updated_at < recovery.created_at ||
        recovery.recovery_id != GetAlternativeRecoveryId(
                                    recovery.request_id, recovery.session_id,
                                    recovery.recovery_provider_id, recovery.client_nonce) ||
        recovery.capacity_request.version != DigiDollar::Paymaster::PROTOCOL_VERSION ||
        recovery.capacity_request.genesis_hash.IsNull() ||
        recovery.capacity_request.provider_id != recovery.recovery_provider_id ||
        recovery.capacity_request.request_id != recovery.request_id ||
        recovery.capacity_request.session_id != recovery.session_id ||
        recovery.capacity_request.client_nonce != recovery.client_nonce ||
        recovery.capacity_request.requested_slots != 1 ||
        recovery.capacity_request.created_at <= 0 ||
        recovery.capacity_request.expires_at <=
            recovery.capacity_request.created_at ||
        recovery.capacity_proof_claim_candidate.size() >
            MAX_EQUIVOCATION_ARTIFACT_BYTES) {
        error = recovery.original_provider_id == recovery.recovery_provider_id ? "PAYMASTER_RECOVERY_PROVIDER_MUST_DIFFER" : "PAYMASTER_INVALID_ALTERNATIVE_RECOVERY";
        return false;
    }
    if (recovery.expired &&
        ((recovery.provider_side &&
          recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED) ||
         (!recovery.provider_side &&
          recovery.phase > AlternativeRecoveryPhase::RESPONSE_VALIDATED) ||
         !recovery.user_signed_psbt.empty() ||
         !recovery.final_transaction.empty() ||
         !recovery.expected_wtxid.IsNull())) {
        error = "PAYMASTER_INVALID_EXPIRED_RECOVERY";
        return false;
    }

    if (recovery.phase >= AlternativeRecoveryPhase::REQUEST_READY) {
        const AlternativeRecoveryRequest& request = recovery.recovery_request;
        if (!ValidateAlternativeRecoveryRequestEnvelope(
                request, recovery.capacity_request.genesis_hash,
                request.created_at, error) ||
            request.request_id != recovery.request_id ||
            request.session_id != recovery.session_id ||
            request.original_provider_id != recovery.original_provider_id ||
            request.recovery_provider_id != recovery.recovery_provider_id ||
            request.privacy_profile != recovery.privacy_profile ||
            request.offer_id != recovery.offer_id ||
            request.policy_hash != recovery.policy_hash ||
            request.original_commit_key != recovery.original_commit_key ||
            request.original_template_commitment !=
                recovery.original_template_commitment ||
            !(request.maximum_service_fee ==
              recovery.selected_maximum_service_fee) ||
            !(request.service_fee == recovery.selected_service_fee) ||
            request.client_nonce != recovery.client_nonce ||
            CanonicalBytes(request.capacity_request) !=
                CanonicalBytes(recovery.capacity_request) ||
            recovery.recovery_request_hash !=
                GetAlternativeRecoveryRequestHash(request)) {
            if (error.empty()) error = "PAYMASTER_RECOVERY_REQUEST_MISMATCH";
            return false;
        }
    } else if (!recovery.recovery_request_hash.IsNull()) {
        error = "PAYMASTER_RECOVERY_PREMATURE_REQUEST_HASH";
        return false;
    }

    if (recovery.phase >= AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        const ValidatedCapacitySnapshot& snapshot = recovery.capacity_snapshot;
        if (snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
            snapshot.snapshot_id != recovery.recovery_request.capacity_snapshot_id ||
            snapshot.resource_commitment !=
                recovery.recovery_request.capacity_resource_commitment ||
            snapshot.session_id != recovery.session_id ||
            snapshot.provider_id != recovery.recovery_provider_id ||
            snapshot.client_nonce != recovery.client_nonce ||
            snapshot.funding_model != FundingModel::USER_PAID ||
            snapshot.requires_carrier !=
                (recovery.recovery_request.service_fee.value > 0 &&
                 recovery.recovery_request.service_fee.value < 100) ||
            snapshot.request_hash != Hash(CanonicalBytes(recovery.capacity_request)) ||
            snapshot.capacity_proof.empty() || snapshot.expires_at <= snapshot.created_at ||
            !ValidateAlternativeRecoveryResponse(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key,
                recovery.recovery_response.created_at, error) ||
            recovery.recovery_response.recovery_id != recovery.recovery_id) {
            if (error.empty()) error = "PAYMASTER_RECOVERY_RESPONSE_MISMATCH";
            return false;
        }
        if (!ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response,
                recovery.recovery_response.created_at, error)) {
            return false;
        }
    } else if (!recovery.recovery_authorization.authorization_commitment.IsNull() ||
               !recovery.accepted_recovery_authorization_commitment.IsNull() ||
               recovery.recovery_authorization_accepted_at != 0) {
        error = "PAYMASTER_PREMATURE_RECOVERY_AUTHORIZATION";
        return false;
    }

    const bool has_provider_budget_binding =
        !recovery.provider_safety_policy_hash.IsNull() ||
        !recovery.provider_budget_reservation_id.IsNull() ||
        !recovery.provider_netgroup_bucket.IsNull() ||
        recovery.provider_maximum_network_fee.value != 0;
    if (recovery.provider_side &&
        recovery.phase >= AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        const AlternativeRecoveryManifest& manifest =
            recovery.recovery_response.manifest;
        if (recovery.provider_safety_policy_hash.IsNull() ||
            recovery.provider_budget_reservation_id !=
                recovery.recovery_response.recovery_commit_key ||
            recovery.provider_netgroup_bucket.IsNull() ||
            recovery.provider_maximum_network_fee.value <= 0 ||
            !(recovery.provider_maximum_network_fee == manifest.network_fee)) {
            error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
            return false;
        }
    } else if (has_provider_budget_binding) {
        // Client records and pre-response provider records have no authority
        // over the provider's wallet-local budget.
        error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        return false;
    }

    const bool has_recovery_acceptance =
        !recovery.accepted_recovery_authorization_commitment.IsNull() ||
        recovery.recovery_authorization_accepted_at != 0;
    const bool complete_recovery_acceptance =
        !recovery.provider_side &&
        recovery.accepted_recovery_authorization_commitment ==
            recovery.recovery_authorization.authorization_commitment &&
        recovery.recovery_authorization_accepted_at > 0 &&
        recovery.recovery_authorization_accepted_at >=
            recovery.recovery_response.created_at &&
        recovery.recovery_authorization_accepted_at <=
            recovery.recovery_authorization.expires_at;
    if (recovery.provider_side && has_recovery_acceptance) {
        error = "PAYMASTER_INVALID_RECOVERY_USER_PSBT";
        return false;
    }
    if (!recovery.provider_side && has_recovery_acceptance &&
        !complete_recovery_acceptance) {
        error = "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH";
        return false;
    }
    if (recovery.phase >= AlternativeRecoveryPhase::USER_SIGNED) {
        if (!recovery.provider_side && !complete_recovery_acceptance) {
            error = "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_REQUIRED";
            return false;
        }
        if (recovery.user_signed_psbt.empty() ||
            recovery.user_signed_psbt.size() > MAX_DIRECT_PSBT_BYTES) {
            error = "PAYMASTER_INVALID_RECOVERY_USER_PSBT";
            return false;
        }
    } else if (!recovery.user_signed_psbt.empty() ||
               (has_recovery_acceptance &&
                recovery.phase != AlternativeRecoveryPhase::RESPONSE_VALIDATED)) {
        error = "PAYMASTER_PREMATURE_RECOVERY_USER_PSBT";
        return false;
    }

    if (recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED) {
        if (recovery.expired || recovery.final_transaction.empty() ||
            recovery.expected_wtxid.IsNull() ||
            !ValidatePaymasterResult(
                recovery.signed_result,
                recovery.capacity_request.genesis_hash,
                recovery.recovery_provider_id,
                recovery.recovery_response.recovery_commit_key,
                recovery.recovery_provider_identity_key, 1, error) ||
            !recovery.signed_result.final_transaction ||
            CanonicalBytes(*recovery.signed_result.final_transaction) !=
                recovery.final_transaction) {
            if (error.empty()) error = "PAYMASTER_INVALID_FINAL_RECOVERY";
            return false;
        }
    } else if (!recovery.final_transaction.empty() ||
               !recovery.expected_wtxid.IsNull() ||
               recovery.signed_result.result_sequence != 0) {
        if (error.empty()) {
            error = "PAYMASTER_INVALID_FINAL_RECOVERY";
        }
        return false;
    }
    error.clear();
    return true;
}

bool ValidateProviderAlternativeRecoveryCommitBinding(
    const AlternativeRecoveryRecord& recovery,
    const ProviderCommitRecord& commit,
    CMutableTransaction& transaction,
    std::vector<COutPoint>& provider_inputs,
    std::string& error)
{
    ResetDefault(transaction);
    provider_inputs.clear();
    try {
        SpanReader stream{::PROTOCOL_VERSION, recovery.final_transaction};
        stream >> transaction;
        if (!stream.empty()) {
            throw std::ios_base::failure("trailing transaction data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_FINAL_RECOVERY";
        return false;
    }

    const CTransaction final_tx{transaction};
    const AlternativeRecoveryManifest& manifest =
        recovery.recovery_response.manifest;
    provider_inputs = manifest.recovery_provider_carrier_inputs;
    provider_inputs.insert(provider_inputs.end(),
                           manifest.recovery_provider_dgb_inputs.begin(),
                           manifest.recovery_provider_dgb_inputs.end());
    const PaymasterResult& result = recovery.signed_result;
    if (!recovery.provider_side || recovery.expired ||
        recovery.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
        !ValidateAlternativeRecoveryRecordShape(recovery, error) ||
        final_tx.GetHash() != manifest.unsigned_txid ||
        final_tx.GetWitnessHash() != recovery.expected_wtxid ||
        commit.version != ProviderCommitRecord::CURRENT_VERSION ||
        commit.commit_key != recovery.recovery_response.recovery_commit_key ||
        commit.provider_id != recovery.recovery_provider_id ||
        commit.quote_id != recovery.recovery_id ||
        commit.template_commitment != manifest.template_commitment ||
        commit.final_txid != final_tx.GetHash() ||
        commit.raw_transaction_hash != Hash(recovery.final_transaction) ||
        commit.final_transaction != recovery.final_transaction ||
        commit.provider_inputs != provider_inputs || !result.txid ||
        *result.txid != commit.final_txid || !result.raw_transaction_hash ||
        *result.raw_transaction_hash != commit.raw_transaction_hash ||
        !result.final_transaction ||
        CanonicalBytes(*result.final_transaction) != commit.final_transaction) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_COMMIT";
        ResetDefault(transaction);
        provider_inputs.clear();
        return false;
    }
    error.clear();
    return true;
}

// -------------------------------------------------------------------------
// Final-artifact reconciliation
// -------------------------------------------------------------------------
// Txid alone is insufficient for witness transactions. Persist and compare the
// exact final artifact so a peer cannot turn the same non-witness transaction
// into a different finality observation by supplying altered witness data.
bool DecodeExactFinalArtifact(const std::vector<unsigned char>& bytes,
                              const uint256& expected_txid,
                              ExactFinalArtifact& artifact,
                              std::string& error)
{
    ResetDefault(artifact);
    if (bytes.empty() || expected_txid.IsNull()) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISSING";
        return false;
    }
    try {
        SpanReader stream{::PROTOCOL_VERSION, bytes};
        stream >> artifact.transaction;
        if (!stream.empty()) {
            throw std::ios_base::failure("trailing final transaction data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_FINAL_TRANSACTION_ENCODING";
        return false;
    }
    const CTransaction transaction{artifact.transaction};
    const std::vector<unsigned char> canonical = CanonicalBytes(transaction);
    if (canonical != bytes || transaction.GetHash() != expected_txid) {
        ResetDefault(artifact);
        error = "PAYMASTER_FINAL_TRANSACTION_BINDING_MISMATCH";
        return false;
    }
    artifact.bytes = bytes;
    artifact.txid = transaction.GetHash();
    artifact.wtxid = transaction.GetWitnessHash();
    error.clear();
    return true;
}

bool LoadPaymentFinalArtifact(WalletBatch& batch,
                              const PaymentSession& session,
                              ExactFinalArtifact& artifact,
                              std::optional<ProviderAttempt>& observed_attempt,
                              std::string& error)
{
    ResetDefault(artifact);
    observed_attempt.reset();
    if (session.final_txid.IsNull()) return true;

    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt candidate;
        const DatabaseReadStatus attempt_status =
            batch.ReadPaymasterAttemptWithStatus(attempt_id, candidate);
        if (attempt_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                attempt_status, "ProviderAttempt", candidate,
                "PAYMASTER_FINAL_ATTEMPT_MISSING",
                "PAYMASTER_INVALID_PERSISTED_ATTEMPT");
            return false;
        }
        if (candidate.final_txid != session.final_txid) {
            continue;
        }
        ExactFinalArtifact candidate_artifact;
        if (!DecodeExactFinalArtifact(candidate.final_transaction,
                                      session.final_txid,
                                      candidate_artifact, error)) {
            return false;
        }
        if (!artifact.txid.IsNull() &&
            artifact.bytes != candidate_artifact.bytes) {
            error = "PAYMASTER_FINAL_TRANSACTION_STORAGE_CONFLICT";
            return false;
        }
        if (observed_attempt &&
            observed_attempt->attempt_id != candidate.attempt_id) {
            error = "PAYMASTER_FINAL_TRANSACTION_STORAGE_CONFLICT";
            return false;
        }
        artifact = std::move(candidate_artifact);
        observed_attempt = std::move(candidate);
    }
    if (!observed_attempt) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISSING";
        return false;
    }
    error.clear();
    return true;
}

bool LoadRecoveryFinalArtifact(WalletBatch& batch,
                               const PaymentSession& session,
                               ExactFinalArtifact& artifact,
                               std::string& error)
{
    ResetDefault(artifact);
    if (session.recovery_txid.IsNull()) return true;

    SelfRecoveryRecord recovery;
    const DatabaseReadStatus recovery_status =
        batch.ReadPaymasterRecoveryWithStatus(session.request_id, recovery);
    if (recovery_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            recovery_status, "SelfRecoveryRecord", recovery,
            "PAYMASTER_RECOVERY_FINAL_MISSING",
            "PAYMASTER_INVALID_PERSISTED_RECOVERY");
        return false;
    }
    if (recovery.session_id != session.session_id ||
        recovery.recovery_txid != session.recovery_txid ||
        recovery.raw_transaction_hash != Hash(recovery.final_transaction) ||
        !DecodeExactFinalArtifact(recovery.final_transaction,
                                  session.recovery_txid, artifact, error)) {
        if (error.empty()) error = "PAYMASTER_RECOVERY_FINAL_MISSING";
        return false;
    }

    uint256 alternative_id;
    const DatabaseReadStatus alternative_request_status =
        batch.ReadPaymasterAlternativeRecoveryRequestWithStatus(
            session.request_id, alternative_id);
    if (alternative_request_status == DatabaseReadStatus::FOUND) {
        AlternativeRecoveryRecord alternative;
        const DatabaseReadStatus alternative_status =
            batch.ReadPaymasterAlternativeRecoveryWithStatus(alternative_id,
                                                             alternative);
        if (alternative_status != DatabaseReadStatus::FOUND) {
            error = PersistedReadError(
                alternative_status, "AlternativeRecoveryRecord", alternative,
                "PAYMASTER_ALTERNATIVE_RECOVERY_FINAL_MISSING",
                "PAYMASTER_INVALID_PERSISTED_ALTERNATIVE_RECOVERY");
            return false;
        }
        if (alternative.provider_side ||
            alternative.session_id != session.session_id ||
            alternative.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
            alternative.final_transaction != artifact.bytes ||
            alternative.expected_wtxid != artifact.wtxid) {
            error = "PAYMASTER_ALTERNATIVE_RECOVERY_FINAL_MISMATCH";
            return false;
        }
    } else if (alternative_request_status != DatabaseReadStatus::NOT_FOUND) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_INDEX_DATABASE_READ";
        return false;
    }
    error.clear();
    return true;
}

bool ObserveExactWalletArtifact(const CWallet& wallet,
                                const ExactFinalArtifact& artifact,
                                ExactFinalObservation& observation,
                                std::string& error)
{
    observation = {};
    if (artifact.txid.IsNull()) return true;
    const auto wallet_tx = wallet.mapWallet.find(artifact.txid);
    if (wallet_tx == wallet.mapWallet.end()) return true;
    if (!wallet_tx->second.tx ||
        CanonicalBytes(*wallet_tx->second.tx) != artifact.bytes ||
        wallet_tx->second.tx->GetWitnessHash() != artifact.wtxid) {
        error = "PAYMASTER_OBSERVED_FINAL_WITNESS_MISMATCH";
        return false;
    }
    observation.confirmation_depth =
        wallet.GetTxDepthInMainChain(wallet_tx->second);
    observation.in_mempool = wallet_tx->second.InMempool();
    error.clear();
    return true;
}

bool BindExactObservation(const CTransaction& transaction,
                          const ExactFinalArtifact& artifact,
                          int confirmation_depth,
                          bool in_mempool,
                          ExactFinalObservation& observation,
                          std::string& error)
{
    if (artifact.txid.IsNull() || transaction.GetHash() != artifact.txid) {
        return false;
    }
    if (CanonicalBytes(transaction) != artifact.bytes ||
        transaction.GetWitnessHash() != artifact.wtxid) {
        error = "PAYMASTER_OBSERVED_FINAL_WITNESS_MISMATCH";
        return false;
    }
    observation.confirmation_depth = confirmation_depth;
    observation.in_mempool = in_mempool;
    error.clear();
    return true;
}

// -------------------------------------------------------------------------
// Equivocation evidence and capacity admission
// -------------------------------------------------------------------------
// Evidence is staged before it is promoted to a local provider block. This
// keeps detection crash-safe and ensures an exact replay is harmless while two
// contradictory signed artifacts remain auditable.
bool SameCapacitySnapshot(const ValidatedCapacitySnapshot& lhs,
                          const ValidatedCapacitySnapshot& rhs)
{
    return lhs.version == rhs.version && lhs.snapshot_id == rhs.snapshot_id &&
           lhs.resource_commitment == rhs.resource_commitment &&
           lhs.session_id == rhs.session_id && lhs.attempt_id == rhs.attempt_id &&
           lhs.provider_id == rhs.provider_id && lhs.client_nonce == rhs.client_nonce &&
           lhs.request_hash == rhs.request_hash &&
           lhs.capacity_proof == rhs.capacity_proof && lhs.created_at == rhs.created_at &&
           lhs.expires_at == rhs.expires_at && lhs.validated_at == rhs.validated_at &&
           lhs.funding_model == rhs.funding_model &&
           lhs.requires_carrier == rhs.requires_carrier;
}

bool SameEquivocationEvidence(const PaymasterEquivocationEvidence& lhs,
                              const PaymasterEquivocationEvidence& rhs)
{
    // observed_at is local metadata rather than part of the provider's signed
    // claim. Retrying the same two canonical artifacts must therefore remain
    // idempotent even if the retry is observed at a later wall-clock time.
    return lhs.version == rhs.version && lhs.evidence_id == rhs.evidence_id &&
           lhs.kind == rhs.kind && lhs.provider_id == rhs.provider_id &&
           lhs.semantic_key == rhs.semantic_key &&
           lhs.first_artifact_hash == rhs.first_artifact_hash &&
           lhs.second_artifact_hash == rhs.second_artifact_hash &&
           lhs.first_artifact == rhs.first_artifact &&
           lhs.second_artifact == rhs.second_artifact;
}

bool StagePendingEquivocationLocked(
    WalletBatch& batch,
    const PaymasterEquivocationEvidence& evidence,
    std::string& error)
{
    if (!ValidateEquivocationEvidence(evidence)) {
        error = "PAYMASTER_INVALID_EQUIVOCATION_EVIDENCE";
        return false;
    }
    PaymasterEquivocationEvidence existing;
    const DatabaseReadStatus status =
        batch.ReadPaymasterPendingEquivocation(evidence.provider_id, existing);
    if (status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ";
        return false;
    }
    if (status == DatabaseReadStatus::FOUND) {
        if (!SameEquivocationEvidence(existing, evidence)) {
            error = "PAYMASTER_PENDING_EQUIVOCATION_CONFLICT";
            return false;
        }
        error.clear();
        return true;
    }

    // Phase 1 is deliberately its own commit. Once this succeeds, a later
    // write error or process crash cannot erase the verified candidate.
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterPendingEquivocation(evidence, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    error.clear();
    return true;
}

bool PromotePendingEquivocationLocked(WalletBatch& batch,
                                      const PaymasterId& provider_id,
                                      std::string& error)
{
    if (provider_id.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_ID";
        return false;
    }
    PaymasterEquivocationEvidence evidence;
    const DatabaseReadStatus pending_status =
        batch.ReadPaymasterPendingEquivocation(provider_id, evidence);
    if (pending_status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ";
        return false;
    }
    if (pending_status == DatabaseReadStatus::NOT_FOUND) {
        error.clear();
        return true;
    }

    PaymasterEquivocationEvidence existing_evidence;
    const DatabaseReadStatus evidence_status =
        batch.ReadPaymasterEquivocationEvidence(evidence.evidence_id,
                                                existing_evidence);
    if (evidence_status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_EQUIVOCATION_EVIDENCE_DATABASE_READ";
        return false;
    }
    if (evidence_status == DatabaseReadStatus::FOUND &&
        !SameEquivocationEvidence(existing_evidence, evidence)) {
        error = "PAYMASTER_EQUIVOCATION_EVIDENCE_CONFLICT";
        return false;
    }

    PaymasterProviderBlock existing_block;
    const DatabaseReadStatus block_status =
        batch.ReadPaymasterProviderBlock(provider_id, existing_block);
    if (block_status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ";
        return false;
    }
    if (block_status == DatabaseReadStatus::FOUND &&
        existing_block.evidence_id == evidence.evidence_id &&
        existing_block.kind != evidence.kind) {
        error = "PAYMASTER_PROVIDER_BLOCK_CONFLICT";
        return false;
    }

    PaymasterProviderBlock block;
    block.provider_id = provider_id;
    block.evidence_id = evidence.evidence_id;
    block.kind = evidence.kind;
    block.blocked_at = evidence.observed_at;
    if (!ValidateProviderBlock(block)) {
        error = "PAYMASTER_INVALID_PROVIDER_BLOCK";
        return false;
    }

    // Phase 2 makes the final evidence, permanent block (when one does not
    // already exist), and pending erasure one atomic database transition.
    // A provider already blocked by other valid evidence remains blocked; the
    // new evidence is still retained before its pending record is removed.
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if ((evidence_status == DatabaseReadStatus::NOT_FOUND &&
         !batch.WritePaymasterEquivocationEvidence(evidence, false)) ||
        (block_status == DatabaseReadStatus::NOT_FOUND &&
         !batch.WritePaymasterProviderBlock(block, false)) ||
        !batch.ErasePaymasterPendingEquivocation(provider_id)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    error.clear();
    return true;
}

bool PersistEquivocation(WalletBatch& batch,
                         EquivocationKind kind,
                         const PaymasterId& provider_id,
                         const uint256& semantic_key,
                         const std::vector<unsigned char>& first_artifact,
                         const std::vector<unsigned char>& second_artifact,
                         int64_t now,
                         std::string& error)
{
    PaymasterEquivocationEvidence evidence;
    evidence.kind = kind;
    evidence.provider_id = provider_id;
    evidence.semantic_key = semantic_key;
    evidence.first_artifact_hash = Hash(first_artifact);
    evidence.second_artifact_hash = Hash(second_artifact);
    evidence.first_artifact = first_artifact;
    evidence.second_artifact = second_artifact;
    if (evidence.second_artifact_hash < evidence.first_artifact_hash) {
        std::swap(evidence.first_artifact_hash, evidence.second_artifact_hash);
        std::swap(evidence.first_artifact, evidence.second_artifact);
    }
    evidence.observed_at = now;
    evidence.evidence_id = GetEquivocationEvidenceId(
        kind, provider_id, semantic_key, evidence.first_artifact_hash,
        evidence.second_artifact_hash);
    if (!ValidateEquivocationEvidence(evidence)) {
        error = "PAYMASTER_INVALID_EQUIVOCATION_EVIDENCE";
        return false;
    }
    return StagePendingEquivocationLocked(batch, evidence, error) &&
           PromotePendingEquivocationLocked(batch, provider_id, error);
}

bool DecodeCanonicalCapacityProof(const std::vector<unsigned char>& bytes,
                                  PaymasterCapacityProof& proof)
{
    if (bytes.empty() || bytes.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES) return false;
    try {
        SpanReader stream{::PROTOCOL_VERSION, bytes};
        stream >> proof;
        if (!stream.empty()) return false;
        CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
        canonical << proof;
        const auto canonical_bytes = MakeUCharSpan(canonical);
        return std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                          bytes.begin(), bytes.end());
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

bool DecodeCanonicalCapacityRequest(const std::vector<unsigned char>& bytes,
                                    PaymasterCapacityRequest& request)
{
    if (bytes.empty() || bytes.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES) return false;
    try {
        SpanReader stream{::PROTOCOL_VERSION, bytes};
        stream >> request;
        if (!stream.empty()) return false;
        CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
        canonical << request;
        const auto canonical_bytes = MakeUCharSpan(canonical);
        return std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                          bytes.begin(), bytes.end());
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

uint256 GetPersistedCapacityRequestHash(const PaymasterCapacityProof& proof)
{
    PaymasterCapacityRequest request;
    request.version = proof.version;
    request.genesis_hash = proof.genesis_hash;
    request.provider_id = proof.provider_id;
    request.request_id = proof.request_id;
    request.session_id = proof.session_id;
    request.client_nonce = proof.client_nonce;
    request.funding_model = proof.funding_model;
    request.requires_carrier = proof.requires_carrier;
    request.requested_slots = static_cast<uint16_t>(proof.liquidity_slots.size());
    request.created_at = proof.created_at;
    request.expires_at = proof.expires_at;
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << request;
    return Hash(MakeUCharSpan(stream));
}

PaymasterCapacityRequest GetPersistedCapacityRequest(
    const PaymasterCapacityProof& proof)
{
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
    return request;
}

uint256 GetPersistedCapacitySessionKey(const PaymasterCapacityProof& proof)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Capacity Session v1");
    hasher << proof.provider_id << proof.request_id << proof.session_id;
    return hasher.GetSHA256();
}

// A capacity proof is useful only for the request and pool resources to which
// it was signed. Continuations therefore reload the original canonical request
// and validate the current pool binding instead of trusting caller-supplied
// identifiers.
bool ReadProviderCapacityRequestForContinuation(
    WalletBatch& batch,
    const ProviderIdentityRecord& identity,
    const uint256& expected_genesis,
    const PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce,
    int64_t now,
    PaymasterCapacityRequest& request,
    PaymasterCapacityProof& proof,
    uint256& request_hash,
    std::string& error)
{
    request = {};
    proof = {};
    request_hash.SetNull();
    if (expected_genesis.IsNull() || identity.provider_id != provider_id ||
        provider_id.IsNull() ||
        !IsCanonicalRequestId(request_id) || session_id.IsNull() ||
        client_nonce.IsNull() || now <= 0) {
        error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        return false;
    }

    std::vector<unsigned char> encoded;
    if (!batch.ReadPaymasterCapacityNonce(client_nonce, request_hash) ||
        !batch.ReadPaymasterCapacityResponse(request_hash, encoded)) {
        error = "PAYMASTER_CAPACITY_RESERVATION_MISSING";
        return false;
    }
    ProviderCapacityReleaseRecord release;
    if (batch.ReadPaymasterCapacityRelease(request_hash, release)) {
        error = "PAYMASTER_CAPACITY_ADMISSION_EXPIRED";
        return false;
    }
    if (batch.HasPaymasterCapacityRelease(request_hash)) {
        error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
        return false;
    }

    if (!DecodeCanonicalCapacityProof(encoded, proof)) {
        error = "PAYMASTER_CAPACITY_RESPONSE_ENCODING";
        return false;
    }
    request = GetPersistedCapacityRequest(proof);
    std::string validation_error;
    uint256 indexed_hash;
    if (request_hash != GetPersistedCapacityRequestHash(proof) ||
        request_hash != Hash(CanonicalBytes(request)) ||
        request.genesis_hash != expected_genesis ||
        proof.provider_id != provider_id || proof.request_id != request_id ||
        proof.session_id != session_id || proof.client_nonce != client_nonce ||
        !ValidateCapacityRequestEnvelope(request, expected_genesis, now,
                                         validation_error) ||
        !ValidateCapacityProofEnvelope(proof, request, now, validation_error) ||
        !identity.identity_key.IsFullyValid() ||
        GetPaymasterId(identity.identity_key) != provider_id ||
        !identity.identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature)) {
        error = validation_error.empty() ? "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH" : std::move(validation_error);
        return false;
    }
    if (!batch.ReadPaymasterCapacitySession(
            GetPersistedCapacitySessionKey(proof), indexed_hash) ||
        indexed_hash != request_hash) {
        error = "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH";
        return false;
    }
    return true;
}

bool CapacityProofMatchesPoolEntry(const CapacityDGBInput& input,
                                   const ProviderPoolEntry& entry)
{
    const CTransaction creating_tx{input.input.creating_tx};
    return entry.purpose == PoolPurpose::OPERATIONAL &&
           entry.asset == PoolAsset::DGB && entry.outpoint == input.input.outpoint &&
           entry.dgb_value == input.input.value &&
           creating_tx.GetHash() == input.input.outpoint.hash &&
           input.input.outpoint.n < creating_tx.vout.size() &&
           creating_tx.vout[input.input.outpoint.n].nValue == input.input.value.value &&
           creating_tx.vout[input.input.outpoint.n].scriptPubKey == entry.script_pub_key;
}

bool CapacityProofMatchesPoolEntry(const CapacityDDCarrier& carrier,
                                   const ProviderPoolEntry& entry)
{
    const CTransaction creating_tx{carrier.carrier.creating_tx};
    return entry.purpose == PoolPurpose::OPERATIONAL &&
           entry.asset == PoolAsset::DD_CARRIER &&
           entry.outpoint == carrier.carrier.outpoint &&
           entry.carrier_value == carrier.carrier.value &&
           creating_tx.GetHash() == carrier.carrier.outpoint.hash &&
           carrier.carrier.outpoint.n < creating_tx.vout.size() &&
           creating_tx.vout[carrier.carrier.outpoint.n].scriptPubKey ==
               entry.script_pub_key;
}

bool ValidateProviderCapacityAdmissionForCommit(
    const ProviderBudgetLedger& ledger,
    const PaymasterCapacityRequest& request,
    const uint256& request_hash,
    const uint256& quote_request_hash,
    const uint256& commit_key,
    const uint256& netgroup_bucket,
    CapacityAdmissionState expected_state,
    int64_t effective_now,
    std::string& error)
{
    const uint256 request_key = GetProviderRequestSlotKey(
        request.provider_id, request.request_id, request.session_id);
    if (request_key.IsNull() || request_hash.IsNull() ||
        quote_request_hash.IsNull() || commit_key.IsNull() ||
        netgroup_bucket.IsNull() || effective_now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_PROMOTION";
        return false;
    }
    const auto first = std::find_if(
        ledger.capacity_admissions.begin(), ledger.capacity_admissions.end(),
        [&](const ProviderCapacityAdmission& admission) {
            return admission.request_key == request_key;
        });
    if (first == ledger.capacity_admissions.end()) {
        error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
        return false;
    }
    if (std::find_if(
            std::next(first), ledger.capacity_admissions.end(),
            [&](const ProviderCapacityAdmission& admission) {
                return admission.request_key == request_key;
            }) != ledger.capacity_admissions.end()) {
        error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
        return false;
    }
    if (first->request_hash != request_hash ||
        first->netgroup_bucket != netgroup_bucket ||
        first->funding_model != request.funding_model ||
        first->requires_carrier != request.requires_carrier ||
        first->expires_at != request.expires_at ||
        first->expires_at <= effective_now || first->state != expected_state) {
        error = first->expires_at <= effective_now ? "PAYMASTER_CAPACITY_ADMISSION_EXPIRED" : "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
        return false;
    }
    if (expected_state == CapacityAdmissionState::RESERVED) {
        if (!first->quote_request_hash.IsNull() ||
            !first->commit_key.IsNull()) {
            error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
            return false;
        }
    } else if (expected_state == CapacityAdmissionState::PROMOTED) {
        if (first->quote_request_hash != quote_request_hash ||
            first->commit_key != commit_key) {
            error = "PAYMASTER_CAPACITY_PROMOTION_CONFLICT";
            return false;
        }
    } else {
        error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
        return false;
    }
    return true;
}

bool ValidateCapacityPoolBinding(
    const PaymasterCapacityProof& proof,
    const std::set<COutPoint>& expected_dgb,
    const std::set<COutPoint>& expected_carriers,
    const std::vector<ProviderPoolEntry>& pool,
    const uint256& client_nonce,
    const uint256& active_reservation_id,
    std::string& error)
{
    if (proof.liquidity_slots.size() != 1 || client_nonce.IsNull() ||
        active_reservation_id.IsNull()) {
        error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
        return false;
    }
    const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
    std::map<COutPoint, const CapacityDGBInput*> proof_dgb;
    std::map<COutPoint, const CapacityDDCarrier*> proof_carriers;
    for (const CapacityDGBInput& input : slot.dgb_inputs) {
        if (!proof_dgb.emplace(input.input.outpoint, &input).second) {
            error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
            return false;
        }
    }
    if (slot.carrier &&
        !proof_carriers.emplace(slot.carrier->carrier.outpoint,
                                &*slot.carrier)
             .second) {
        error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
        return false;
    }
    std::set<COutPoint> proof_dgb_outpoints;
    std::set<COutPoint> proof_carrier_outpoints;
    for (const auto& [outpoint, input] : proof_dgb) {
        proof_dgb_outpoints.insert(outpoint);
    }
    for (const auto& [outpoint, carrier] : proof_carriers) {
        proof_carrier_outpoints.insert(outpoint);
    }
    if (proof_dgb_outpoints != expected_dgb ||
        proof_carrier_outpoints != expected_carriers ||
        proof.requires_carrier != !expected_carriers.empty()) {
        error = "PAYMASTER_CAPACITY_RESOURCE_ROLE_MISMATCH";
        return false;
    }

    std::set<COutPoint> matched;
    for (const ProviderPoolEntry& entry : pool) {
        const auto dgb = proof_dgb.find(entry.outpoint);
        const auto carrier = proof_carriers.find(entry.outpoint);
        if (dgb != proof_dgb.end() || carrier != proof_carriers.end()) {
            const bool exact_resource =
                dgb != proof_dgb.end() ? CapacityProofMatchesPoolEntry(*dgb->second, entry) : CapacityProofMatchesPoolEntry(*carrier->second, entry);
            if (!exact_resource || entry.state != PoolEntryState::RESERVED ||
                entry.reservation_id != active_reservation_id ||
                !matched.insert(entry.outpoint).second) {
                error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
                return false;
            }
            continue;
        }
        if (entry.reservation_id == client_nonce ||
            entry.reservation_id == active_reservation_id) {
            error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
            return false;
        }
    }
    if (matched.size() != proof_dgb.size() + proof_carriers.size()) {
        error = "PAYMASTER_OPERATIONAL_SLOT_MISSING";
        return false;
    }
    return true;
}

bool ValidateQuoteCapacityResources(
    const PaymasterCapacityProof& proof,
    const PaymasterQuote& quote,
    const ProviderAuthorizationManifest& manifest,
    const std::vector<ProviderPoolEntry>& pool,
    const uint256& client_nonce,
    const uint256& active_reservation_id,
    std::string& error)
{
    std::set<COutPoint> quoted_dgb;
    std::map<COutPoint, const VerifiedDGBInput*> quoted_dgb_inputs;
    for (const VerifiedDGBInput& input : quote.reserved_dgb_inputs) {
        if (!quoted_dgb.insert(input.outpoint).second) {
            error = "PAYMASTER_CAPACITY_RESOURCE_ROLE_MISMATCH";
            return false;
        }
        quoted_dgb_inputs.emplace(input.outpoint, &input);
    }
    std::set<COutPoint> quoted_carriers;
    if (quote.reserved_carrier) {
        quoted_carriers.insert(quote.reserved_carrier->outpoint);
    }
    if (std::set<COutPoint>{manifest.provider_dgb_inputs.begin(),
                            manifest.provider_dgb_inputs.end()} != quoted_dgb ||
        std::set<COutPoint>{manifest.provider_carrier_inputs.begin(),
                            manifest.provider_carrier_inputs.end()} !=
            quoted_carriers ||
        manifest.provider_dgb_inputs.size() != quoted_dgb.size() ||
        manifest.provider_carrier_inputs.size() != quoted_carriers.size() ||
        proof.liquidity_slots.size() != 1) {
        error = "PAYMASTER_CAPACITY_RESOURCE_ROLE_MISMATCH";
        return false;
    }
    const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
    for (const CapacityDGBInput& input : slot.dgb_inputs) {
        const auto quoted = quoted_dgb_inputs.find(input.input.outpoint);
        if (quoted == quoted_dgb_inputs.end() ||
            CanonicalBytes(input.input) != CanonicalBytes(*quoted->second)) {
            error = "PAYMASTER_CAPACITY_RESOURCE_ROLE_MISMATCH";
            return false;
        }
    }
    if (slot.carrier.has_value() != quote.reserved_carrier.has_value() ||
        (slot.carrier &&
         CanonicalBytes(slot.carrier->carrier) !=
             CanonicalBytes(*quote.reserved_carrier))) {
        error = "PAYMASTER_CAPACITY_RESOURCE_ROLE_MISMATCH";
        return false;
    }
    return ValidateCapacityPoolBinding(
        proof, quoted_dgb, quoted_carriers, pool, client_nonce,
        active_reservation_id, error);
}

bool ValidateRecoveryCapacityResources(
    const AlternativeRecoveryRecord& recovery,
    const PaymasterCapacityRequest& capacity_request,
    const PaymasterCapacityProof& proof,
    const std::vector<unsigned char>& persisted_proof,
    const std::vector<ProviderPoolEntry>& pool,
    const uint256& active_reservation_id,
    std::string& error)
{
    const AlternativeRecoveryManifest& manifest =
        recovery.recovery_response.manifest;
    const ValidatedCapacitySnapshot& snapshot = recovery.capacity_snapshot;
    if (CanonicalBytes(recovery.capacity_request) !=
            CanonicalBytes(capacity_request) ||
        CanonicalBytes(recovery.recovery_request.capacity_request) !=
            CanonicalBytes(capacity_request) ||
        snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
        snapshot.snapshot_id != proof.snapshot_id ||
        snapshot.resource_commitment != GetCapacityResourceCommitment(proof) ||
        snapshot.session_id != recovery.session_id ||
        snapshot.attempt_id != recovery.recovery_id ||
        snapshot.provider_id != recovery.recovery_provider_id ||
        snapshot.client_nonce != recovery.client_nonce ||
        snapshot.request_hash != Hash(CanonicalBytes(capacity_request)) ||
        snapshot.capacity_proof != persisted_proof ||
        snapshot.created_at != proof.created_at ||
        snapshot.expires_at != proof.expires_at ||
        snapshot.funding_model != proof.funding_model ||
        snapshot.requires_carrier != proof.requires_carrier) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
        return false;
    }
    const std::set<COutPoint> expected_dgb{
        manifest.recovery_provider_dgb_inputs.begin(),
        manifest.recovery_provider_dgb_inputs.end()};
    const std::set<COutPoint> expected_carriers{
        manifest.recovery_provider_carrier_inputs.begin(),
        manifest.recovery_provider_carrier_inputs.end()};
    if (expected_dgb.size() !=
            manifest.recovery_provider_dgb_inputs.size() ||
        expected_carriers.size() !=
            manifest.recovery_provider_carrier_inputs.size()) {
        error = "PAYMASTER_CAPACITY_RESOURCE_ROLE_MISMATCH";
        return false;
    }
    return ValidateCapacityPoolBinding(
        proof, expected_dgb, expected_carriers, pool, recovery.client_nonce,
        active_reservation_id, error);
}

bool VerifyCapacityEvidenceArtifact(const std::vector<unsigned char>& bytes,
                                    const PaymasterCapacityRequest& request,
                                    const XOnlyPubKey& identity_key,
                                    PaymasterCapacityProof& proof)
{
    std::string validation_error;
    return DecodeCanonicalCapacityProof(bytes, proof) &&
           proof.created_at > 0 &&
           ValidateCapacityProofEnvelope(proof, request, proof.created_at,
                                         validation_error) &&
           identity_key.IsFullyValid() && GetPaymasterId(identity_key) == proof.provider_id &&
           identity_key.VerifySchnorr(GetCapacityProofSignatureHash(proof),
                                      proof.identity_signature);
}

bool ValidatePersistedCapacityClaimCandidate(
    const std::vector<unsigned char>& candidate,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& identity_key,
    const char* corruption_error,
    std::string& error)
{
    PaymasterCapacityProof proof;
    if (candidate.empty() ||
        !VerifyCapacityEvidenceArtifact(candidate, request, identity_key,
                                        proof)) {
        error = corruption_error;
        return false;
    }
    error.clear();
    return true;
}

bool CapacityEvidenceWindowsOverlap(const PaymasterCapacityProof& first,
                                    const PaymasterCapacityProof& second)
{
    return std::max(first.created_at, second.created_at) <
           std::min(first.expires_at, second.expires_at);
}

bool RecordCapacityEquivocationLocked(
    WalletBatch& batch,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& identity_key,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& second_capacity_proof,
    const uint256& semantic_key,
    int64_t now,
    std::string& error)
{
    PaymasterCapacityProof first;
    PaymasterCapacityProof second;
    if (first_capacity_proof == second_capacity_proof || semantic_key.IsNull() || now <= 0 ||
        !VerifyCapacityEvidenceArtifact(first_capacity_proof, request, identity_key, first) ||
        !VerifyCapacityEvidenceArtifact(second_capacity_proof, request, identity_key, second) ||
        !CapacityEvidenceWindowsOverlap(first, second) ||
        GetCapacityProofSignatureHash(first) == GetCapacityProofSignatureHash(second)) {
        return false;
    }
    if (!PersistEquivocation(batch, EquivocationKind::CAPACITY, request.provider_id,
                             semantic_key, first_capacity_proof,
                             second_capacity_proof, now, error)) {
        return false;
    }
    error = "PAYMASTER_CAPACITY_EQUIVOCATION";
    return true;
}

uint256 GetCapacityResourceSemanticKey(const PaymasterId& provider_id,
                                       const COutPoint& outpoint)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Capacity Resource v1");
    hasher << provider_id << outpoint;
    return hasher.GetSHA256();
}

bool CapacityProofContainsOutpoint(const PaymasterCapacityProof& proof,
                                   const COutPoint& outpoint)
{
    for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
        if (slot.carrier && slot.carrier->carrier.outpoint == outpoint) {
            return true;
        }
        if (std::any_of(slot.dgb_inputs.begin(), slot.dgb_inputs.end(),
                        [&](const CapacityDGBInput& input) {
                            return input.input.outpoint == outpoint;
                        })) {
            return true;
        }
    }
    return false;
}

bool VerifyCapacityResourceEvidenceArtifact(
    const std::vector<unsigned char>& bytes,
    const PaymasterId& provider_id,
    const XOnlyPubKey& identity_key,
    const COutPoint& outpoint,
    PaymasterCapacityProof& proof)
{
    std::string validation_error;
    return DecodeCanonicalCapacityProof(bytes, proof) &&
           proof.provider_id == provider_id &&
           proof.created_at > 0 &&
           ValidateCapacityProofEnvelope(proof, proof.genesis_hash,
                                         proof.created_at,
                                         validation_error) &&
           identity_key.IsFullyValid() &&
           GetPaymasterId(identity_key) == provider_id &&
           identity_key.VerifySchnorr(GetCapacityProofSignatureHash(proof),
                                      proof.identity_signature) &&
           CapacityProofContainsOutpoint(proof, outpoint);
}

bool RecordCapacityResourceEquivocationLocked(
    WalletBatch& batch,
    const PaymasterId& provider_id,
    const XOnlyPubKey& identity_key,
    const COutPoint& outpoint,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& second_capacity_proof,
    int64_t now,
    std::string& error)
{
    PaymasterCapacityProof first;
    PaymasterCapacityProof second;
    if (first_capacity_proof == second_capacity_proof || outpoint.IsNull() || now <= 0 ||
        !VerifyCapacityResourceEvidenceArtifact(
            first_capacity_proof, provider_id, identity_key, outpoint, first) ||
        !VerifyCapacityResourceEvidenceArtifact(
            second_capacity_proof, provider_id, identity_key, outpoint, second) ||
        first.genesis_hash != second.genesis_hash ||
        !CapacityEvidenceWindowsOverlap(first, second) ||
        GetCapacityProofSignatureHash(first) ==
            GetCapacityProofSignatureHash(second)) {
        return false;
    }
    if (!PersistEquivocation(
            batch, EquivocationKind::CAPACITY, provider_id,
            GetCapacityResourceSemanticKey(provider_id, outpoint),
            first_capacity_proof, second_capacity_proof, now, error)) {
        return false;
    }
    error = "PAYMASTER_CAPACITY_EQUIVOCATION";
    return true;
}

bool SameCapacityResourceBinding(const CapacityResourceBinding& lhs,
                                 const CapacityResourceBinding& rhs)
{
    return CanonicalBytes(lhs) == CanonicalBytes(rhs);
}

bool BuildCapacityResourceBindings(
    const ValidatedCapacitySnapshot& snapshot,
    const PaymasterCapacityProof& proof,
    std::vector<CapacityResourceBinding>& bindings,
    std::string& error)
{
    bindings.clear();
    const uint256 proof_hash{Hash(snapshot.capacity_proof)};
    std::set<COutPoint> unique;
    for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
        if (slot.carrier) {
            const VerifiedDDCarrier& carrier = slot.carrier->carrier;
            const CTransaction creating_tx{carrier.creating_tx};
            if (!unique.insert(carrier.outpoint).second ||
                creating_tx.GetHash() != carrier.outpoint.hash ||
                carrier.value.value <= 0) {
                error = "PAYMASTER_INVALID_CAPACITY_RESOURCE_BINDING";
                return false;
            }
            CapacityResourceBinding binding;
            binding.provider_id = snapshot.provider_id;
            binding.outpoint = carrier.outpoint;
            binding.snapshot_id = snapshot.snapshot_id;
            binding.resource_commitment = snapshot.resource_commitment;
            binding.session_id = snapshot.session_id;
            binding.attempt_id = snapshot.attempt_id;
            binding.proof_hash = proof_hash;
            binding.carrier = true;
            binding.creating_txid = creating_tx.GetHash();
            binding.value = carrier.value.value;
            binding.expires_at = snapshot.expires_at;
            bindings.push_back(std::move(binding));
        }
        for (const CapacityDGBInput& input : slot.dgb_inputs) {
            const CTransaction creating_tx{input.input.creating_tx};
            if (!unique.insert(input.input.outpoint).second ||
                creating_tx.GetHash() != input.input.outpoint.hash ||
                input.input.value.value <= 0) {
                error = "PAYMASTER_INVALID_CAPACITY_RESOURCE_BINDING";
                return false;
            }
            CapacityResourceBinding binding;
            binding.provider_id = snapshot.provider_id;
            binding.outpoint = input.input.outpoint;
            binding.snapshot_id = snapshot.snapshot_id;
            binding.resource_commitment = snapshot.resource_commitment;
            binding.session_id = snapshot.session_id;
            binding.attempt_id = snapshot.attempt_id;
            binding.proof_hash = proof_hash;
            binding.creating_txid = creating_tx.GetHash();
            binding.value = input.input.value.value;
            binding.expires_at = snapshot.expires_at;
            bindings.push_back(std::move(binding));
        }
    }
    if (bindings.empty()) {
        error = "PAYMASTER_INVALID_CAPACITY_RESOURCE_BINDING";
        return false;
    }
    error.clear();
    return true;
}

bool DecodeCanonicalQuoteRequest(const std::vector<unsigned char>& bytes,
                                 PaymasterQuoteRequest& request)
{
    if (bytes.empty() || bytes.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES) {
        return false;
    }
    try {
        CDataStream stream{bytes, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> request;
        if (!stream.empty()) return false;
        CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
        canonical << request;
        const auto canonical_bytes = MakeUCharSpan(canonical);
        return std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                          bytes.begin(), bytes.end());
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

bool DecodeCanonicalQuoteResponse(const std::vector<unsigned char>& bytes,
                                  PaymasterQuoteResponse& response)
{
    if (bytes.empty() || bytes.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES) return false;
    try {
        CDataStream stream{bytes, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> response;
        if (!stream.empty()) return false;
        CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
        canonical << response;
        const auto canonical_bytes = MakeUCharSpan(canonical);
        return std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                          bytes.begin(), bytes.end());
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

bool ValidatePersistedCapacityRequestAuthority(
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    PaymasterCapacityRequest& request,
    std::string& error)
{
    std::string validation_error;
    if (session.provider_side ||
        !DecodeCanonicalCapacityRequest(attempt.capacity_request, request) ||
        request.genesis_hash.IsNull() ||
        !ValidateCapacityRequestEnvelope(
            request, request.genesis_hash, request.created_at,
            validation_error) ||
        attempt.session_id != session.session_id ||
        request.provider_id != attempt.provider_id ||
        request.request_id != session.request_id ||
        request.session_id != session.session_id ||
        request.client_nonce != attempt.client_nonce ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) !=
            attempt.provider_id) {
        error = "PAYMASTER_PERSISTED_CAPACITY_REQUEST_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

bool ValidatePersistedRecoveryCapacityRequestAuthority(
    const AlternativeRecoveryRecord& recovery,
    std::string& error)
{
    std::string validation_error;
    const PaymasterCapacityRequest& request = recovery.capacity_request;
    if (recovery.provider_side || request.genesis_hash.IsNull() ||
        !ValidateCapacityRequestEnvelope(
            request, request.genesis_hash, request.created_at,
            validation_error) ||
        request.provider_id != recovery.recovery_provider_id ||
        request.request_id != recovery.request_id ||
        request.session_id != recovery.session_id ||
        request.client_nonce != recovery.client_nonce ||
        !recovery.recovery_provider_identity_key.IsFullyValid() ||
        GetPaymasterId(recovery.recovery_provider_identity_key) !=
            recovery.recovery_provider_id) {
        error =
            "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_REQUEST_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

bool ValidatePersistedQuoteRequestAuthority(
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    PaymasterQuoteRequest& request,
    std::string& error)
{
    std::string validation_error;
    if (session.provider_side ||
        !DecodeCanonicalQuoteRequest(attempt.quote_request, request) ||
        request.intent.genesis_hash.IsNull() ||
        !ValidateRedactedQuoteRequestEnvelope(
            request, request.intent.genesis_hash, attempt.created_at,
            validation_error) ||
        attempt.session_id != session.session_id ||
        request.intent.request_id != session.request_id ||
        request.intent.session_id != session.session_id ||
        request.intent.provider_id != attempt.provider_id ||
        request.intent.client_nonce != attempt.client_nonce ||
        GetPaymentIntentHash(request.intent) != attempt.intent_hash ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) !=
            attempt.provider_id) {
        error = "PAYMASTER_PERSISTED_QUOTE_REQUEST_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

bool ValidatePersistedQuoteClaimCandidate(
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    const PaymasterQuoteRequest& authoritative_request,
    std::string& error)
{
    PaymasterQuoteResponse response;
    std::string validation_error;
    if (attempt.quote_response_claim_candidate.empty() ||
        !DecodeCanonicalQuoteResponse(
            attempt.quote_response_claim_candidate, response) ||
        response.request_id != session.request_id ||
        response.session_id != session.session_id ||
        response.quote.provider_id != attempt.provider_id ||
        response.quote.intent_hash != attempt.intent_hash ||
        response.quote.offer_id != authoritative_request.intent.offer_id ||
        response.quote.policy_hash !=
            authoritative_request.intent.policy_hash ||
        response.quote.funding_model !=
            authoritative_request.intent.funding_model ||
        response.quote.sponsorship_scope !=
            authoritative_request.intent.sponsorship_scope ||
        !ValidateQuoteResponseEnvelope(
            response, authoritative_request.intent.genesis_hash,
            response.quote.created_at, validation_error) ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) !=
            attempt.provider_id ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(response.quote),
            response.quote.identity_signature)) {
        error = "PAYMASTER_PERSISTED_QUOTE_CLAIM_CANDIDATE_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

uint256 GetQuoteSemanticKey(const ProviderAttempt& attempt)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Quote Semantic Binding v1");
    hasher << attempt.provider_id << attempt.session_id << attempt.client_nonce
           << attempt.intent_hash;
    return hasher.GetSHA256();
}

bool RecordQuoteEquivocationPairLocked(
    WalletBatch& batch,
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    const std::vector<unsigned char>& first_signed_quote,
    const std::vector<unsigned char>& second_signed_quote,
    int64_t now,
    std::string& error)
{
    PaymasterQuoteRequest authoritative_request;
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (attempt.provider_id.IsNull() || attempt.intent_hash.IsNull() ||
        first_signed_quote.empty() || second_signed_quote.empty() ||
        first_signed_quote == second_signed_quote || now <= 0) {
        return false;
    }
    PaymasterQuoteResponse first;
    PaymasterQuoteResponse second;
    std::string first_error;
    std::string second_error;
    if (!DecodeCanonicalQuoteResponse(first_signed_quote, first) ||
        !DecodeCanonicalQuoteResponse(second_signed_quote, second) ||
        first.quote.created_at <= 0 || second.quote.created_at <= 0 ||
        !ValidateQuoteResponseEnvelope(
            first, authoritative_request.intent.genesis_hash,
            first.quote.created_at, first_error) ||
        !ValidateQuoteResponseEnvelope(
            second, authoritative_request.intent.genesis_hash,
            second.quote.created_at, second_error) ||
        std::max(first.quote.created_at, second.quote.created_at) >=
            std::min(first.quote.expires_at, second.quote.expires_at) ||
        first.request_id != session.request_id || second.request_id != session.request_id ||
        first.session_id != session.session_id || second.session_id != session.session_id ||
        first.quote.provider_id != attempt.provider_id ||
        second.quote.provider_id != attempt.provider_id ||
        first.quote.intent_hash != attempt.intent_hash ||
        second.quote.intent_hash != attempt.intent_hash ||
        GetPaymasterQuoteSignatureHash(first.quote) ==
            GetPaymasterQuoteSignatureHash(second.quote) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(first.quote), first.quote.identity_signature) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(second.quote), second.quote.identity_signature)) {
        return false;
    }
    if (!PersistEquivocation(batch, EquivocationKind::QUOTE, attempt.provider_id,
                             GetQuoteSemanticKey(attempt), first_signed_quote,
                             second_signed_quote, now, error)) {
        return false;
    }
    error = "PAYMASTER_QUOTE_EQUIVOCATION";
    return true;
}

bool RecordQuoteEquivocationLocked(WalletBatch& batch,
                                   const PaymentSession& session,
                                   const ProviderAttempt& attempt,
                                   const std::vector<unsigned char>& conflicting_signed_quote,
                                   int64_t now,
                                   std::string& error)
{
    if (attempt.signed_quote.empty()) return false;
    return RecordQuoteEquivocationPairLocked(
        batch, session, attempt, attempt.signed_quote,
        conflicting_signed_quote, now, error);
}

bool IsFinalResultStatus(PaymasterResultStatus status)
{
    return status == PaymasterResultStatus::FINAL_COMMITTED ||
           status == PaymasterResultStatus::BROADCAST_ATTEMPTED;
}

bool IsNegativeTerminalResultStatus(PaymasterResultStatus status)
{
    return status == PaymasterResultStatus::SLOT_UNAVAILABLE ||
           status == PaymasterResultStatus::REJECTED;
}

int ResultProgressRank(PaymasterResultStatus status)
{
    switch (status) {
    case PaymasterResultStatus::NO_FINAL_COMMIT: return 0;
    case PaymasterResultStatus::USER_PSBT_ACCEPTED: return 1;
    case PaymasterResultStatus::FINAL_COMMITTED: return 2;
    case PaymasterResultStatus::BROADCAST_ATTEMPTED: return 3;
    case PaymasterResultStatus::SLOT_UNAVAILABLE:
    case PaymasterResultStatus::REJECTED: return -1;
    }
    return -1;
}

std::vector<unsigned char> SerializeResultTransaction(const CMutableTransaction& transaction)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << transaction;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool SameFinalResultArtifacts(const PaymasterResult& lhs, const PaymasterResult& rhs)
{
    if (lhs.txid != rhs.txid || lhs.raw_transaction_hash != rhs.raw_transaction_hash ||
        lhs.final_transaction.has_value() != rhs.final_transaction.has_value()) {
        return false;
    }
    return !lhs.final_transaction ||
           SerializeResultTransaction(*lhs.final_transaction) ==
               SerializeResultTransaction(*rhs.final_transaction);
}

bool SameResult(const PaymasterResult& lhs, const PaymasterResult& rhs)
{
    return lhs.result_sequence == rhs.result_sequence &&
           GetPaymasterResultSignatureHash(lhs) == GetPaymasterResultSignatureHash(rhs) &&
           lhs.identity_signature == rhs.identity_signature;
}

bool ValidateResultProgression(const PaymasterResult& previous,
                               const PaymasterResult& next,
                               bool allow_negative_to_final,
                               std::string& error)
{
    if (next.result_sequence <= previous.result_sequence) {
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    if (IsNegativeTerminalResultStatus(previous.status) &&
        !(allow_negative_to_final && IsFinalResultStatus(next.status))) {
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    if (IsFinalResultStatus(previous.status)) {
        if (!IsFinalResultStatus(next.status) ||
            (previous.status == PaymasterResultStatus::BROADCAST_ATTEMPTED &&
             next.status != PaymasterResultStatus::BROADCAST_ATTEMPTED)) {
            error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
            return false;
        }
        if (!SameFinalResultArtifacts(previous, next)) {
            error = "PAYMASTER_FINAL_TX_CONFLICT";
            return false;
        }
        return true;
    }
    const int previous_rank = ResultProgressRank(previous.status);
    const int next_rank = ResultProgressRank(next.status);
    if (next_rank >= 0 && next_rank < previous_rank) {
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    return true;
}

bool ToInputRole(ReservationRole role, InputRole& result)
{
    switch (role) {
    case ReservationRole::USER_DD: result = InputRole::USER_DD; return true;
    case ReservationRole::USER_DGB: result = InputRole::USER_DGB; return true;
    case ReservationRole::PROVIDER_CARRIER: result = InputRole::PROVIDER_CARRIER; return true;
    case ReservationRole::PROVIDER_DGB: result = InputRole::PROVIDER_DGB; return true;
    }
    return false;
}

bool ValidateQuotedTemplate(const ProviderAttempt& attempt, std::string& error)
{
    CMutableTransaction transaction;
    try {
        SpanReader stream{::PROTOCOL_VERSION, attempt.unsigned_transaction};
        stream >> transaction;
        if (!stream.empty()) {
            error = "PAYMASTER_TEMPLATE_TRANSACTION_ENCODING";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_TEMPLATE_TRANSACTION_ENCODING";
        return false;
    }
    if (transaction.vin.empty() || transaction.vin.size() != attempt.input_roles.size() ||
        CTransaction{transaction}.GetHash() != attempt.unsigned_txid) {
        error = "PAYMASTER_TEMPLATE_COMMITMENT_MISMATCH";
        return false;
    }

    PartiallySignedTransaction psbt;
    std::string decode_error;
    if (!DecodeRawPSBT(psbt, MakeByteSpan(attempt.unsigned_psbt), decode_error)) {
        error = "PAYMASTER_TEMPLATE_PSBT_ENCODING";
        return false;
    }
    CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical << psbt;
    const auto canonical_bytes = MakeUCharSpan(canonical);
    if (!std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                    attempt.unsigned_psbt.begin(), attempt.unsigned_psbt.end()) ||
        !psbt.tx || CTransaction{*psbt.tx}.GetHash() != attempt.unsigned_txid) {
        error = "PAYMASTER_TEMPLATE_PSBT_NONCANONICAL";
        return false;
    }

    CollaborativePSBTTemplate trusted;
    trusted.psbt = psbt;
    trusted.input_roles.reserve(attempt.input_roles.size());
    bool has_user{false};
    bool has_provider{false};
    for (const ReservationRole role : attempt.input_roles) {
        InputRole input_role;
        if (!ToInputRole(role, input_role)) {
            error = "PAYMASTER_TEMPLATE_INPUT_ROLE";
            return false;
        }
        trusted.input_roles.push_back(input_role);
        has_user |= IsInputOwnedBy(input_role, SigningParty::USER);
        has_provider |= IsInputOwnedBy(input_role, SigningParty::PROVIDER);
    }
    if (!has_user || !has_provider ||
        !ValidateCollaborativePSBT(psbt, trusted, CollaborativeSignatureStage::UNSIGNED, error)) {
        if (error.empty()) error = "PAYMASTER_TEMPLATE_INPUT_ROLE";
        return false;
    }
    return true;
}

bool LoadAttemptAuthorizationArtifacts(const ProviderAttempt& attempt,
                                       PaymasterQuoteRequest& request,
                                       PaymasterQuoteResponse& response,
                                       CollaborativePSBTTemplate& trusted,
                                       std::string& error)
{
    request = {};
    ResetDefault(response);
    trusted = {};
    PartiallySignedTransaction psbt;
    try {
        CDataStream request_stream{attempt.quote_request, SER_NETWORK,
                                   ::PROTOCOL_VERSION};
        request_stream >> request;
        SpanReader response_stream{::PROTOCOL_VERSION, attempt.signed_quote};
        response_stream >> response;
        if (!request_stream.empty() || !response_stream.empty()) {
            throw std::ios_base::failure("trailing authorization artifact data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_AUTHORIZATION_ARTIFACT_ENCODING";
        return false;
    }
    std::string decode_error;
    if (!DecodeRawPSBT(psbt, MakeByteSpan(attempt.unsigned_psbt), decode_error) ||
        !psbt.tx || psbt.inputs.size() != attempt.input_roles.size()) {
        error = "PAYMASTER_AUTHORIZATION_TEMPLATE_ENCODING";
        return false;
    }
    trusted.psbt = std::move(psbt);
    for (const ReservationRole role : attempt.input_roles) {
        InputRole input_role;
        if (!ToInputRole(role, input_role)) {
            error = "PAYMASTER_TEMPLATE_INPUT_ROLE";
            return false;
        }
        trusted.input_roles.push_back(input_role);
    }
    return true;
}

bool ValidateAttemptAuthorizationManifest(const ProviderAttempt& attempt,
                                          bool provider_side,
                                          std::string& error)
{
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    CollaborativePSBTTemplate trusted;
    if (!LoadAttemptAuthorizationArtifacts(attempt, request, response, trusted, error)) {
        return false;
    }
    if (provider_side) {
        return ValidateProviderAuthorizationManifest(
            attempt.provider_manifest, request.intent, response.quote,
            trusted, error);
    }
    return ValidateClientAuthorizationManifest(
        attempt.client_manifest, request.intent, response.quote,
        attempt.capacity_snapshot, trusted, error);
}

bool IsBIP86Script(const CScript& script)
{
    int version{-1};
    std::vector<unsigned char> program;
    return script.IsWitnessProgram(version, program) && version == 1 && program.size() == 32 &&
           XOnlyPubKey{program}.IsFullyValid();
}

bool DecodeUnsignedIntent(const std::vector<unsigned char>& bytes,
                          PaymentIntent& intent,
                          int64_t now,
                          std::string& error)
{
    if (bytes.empty() || bytes.size() > MAX_DIRECT_MESSAGE_BYTES) {
        error = "PAYMASTER_INTENT_ENCODING";
        return false;
    }
    try {
        SpanReader stream{::PROTOCOL_VERSION, bytes};
        stream >> intent;
        if (!stream.empty()) {
            error = "PAYMASTER_INTENT_ENCODING";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INTENT_ENCODING";
        return false;
    }
    CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical << intent;
    const auto canonical_bytes = MakeUCharSpan(canonical);
    if (!std::equal(canonical_bytes.begin(), canonical_bytes.end(), bytes.begin(), bytes.end())) {
        error = "PAYMASTER_INTENT_NONCANONICAL";
        return false;
    }
    if (intent.version != PaymentIntent::CURRENT_VERSION || intent.genesis_hash.IsNull() ||
        intent.provider_id.IsNull() || !IsCanonicalRequestId(intent.request_id) ||
        intent.session_id.IsNull() || intent.client_nonce.IsNull() || intent.offer_id.IsNull() ||
        intent.policy_hash.IsNull() || intent.canonical_request_hash.IsNull() ||
        (intent.requested_fee_mode != FeeMode::PAYMASTER &&
         intent.requested_fee_mode != FeeMode::AUTO) ||
        (intent.privacy_profile != PrivacyProfile::STANDARD &&
         intent.privacy_profile != PrivacyProfile::HIGH) ||
        (intent.selection_mode != SelectionMode::LOWEST_TOTAL_COST &&
         intent.selection_mode != SelectionMode::PRIVACY_WEIGHTED) ||
        intent.user_dd_inputs.empty() ||
        intent.user_dd_inputs.size() > MAX_PAYMENT_INTENT_INPUTS ||
        !intent.user_input_proofs.empty() || !IsBIP86Script(intent.recipient_script) ||
        (!intent.user_dd_change_script.empty() && !IsBIP86Script(intent.user_dd_change_script)) ||
        intent.recipient_amount.value < 100 || intent.recipient_amount.value > MAX_DD_OUTPUT_CENTS ||
        intent.expires_at <= now ||
        TimeDeltaExceeds(intent.expires_at, now, MAX_DIRECT_MESSAGE_TTL_SECONDS)) {
        error = "PAYMASTER_INVALID_INTENT_DRAFT";
        return false;
    }
    std::string binding_error;
    if (!ValidateSponsorshipBinding(intent.funding_model, intent.sponsorship_scope,
                                    /*fee_rate_bps=*/0, DDCents{0},
                                    intent.sponsorship_authorization_hash, binding_error)) {
        error = binding_error;
        return false;
    }
    std::set<COutPoint> unique_inputs;
    for (const COutPoint& input : intent.user_dd_inputs) {
        if (input.IsNull() || !unique_inputs.insert(input).second) {
            error = "PAYMASTER_INVALID_INTENT_INPUTS";
            return false;
        }
    }
    return true;
}

} // namespace paymaster_store::internal

bool ValidateProviderAlternativeRecoveryBudgetAuthorization(
    const AlternativeRecoveryRecord& recovery,
    const ProviderSafetyPolicy* policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error)
{
    return paymaster_store::internal::ValidateProviderAlternativeRecoveryBudgetAuthorizationImpl(
        recovery, policy, ledger, expected_state, allow_historical_policy,
        error);
}

} // namespace wallet
