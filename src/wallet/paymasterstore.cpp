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
    if (batch.ReadPaymasterReservation(outpoint, reservation)) return true;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterProviderPool(pool)) return false;
    const auto entry = std::find_if(pool.begin(), pool.end(), [&](const ProviderPoolEntry& candidate) {
        return candidate.outpoint == outpoint;
    });
    // Every live provider-pool entry is dedicated Paymaster liquidity. An
    // AVAILABLE entry must not be consumed by an unrelated wallet send before
    // it can be reserved for a quote or used in an admission proof.
    return entry != pool.end() && IsActiveProviderPoolState(entry->state);
}

std::set<COutPoint> GetPaymasterProviderPoolInputs(const CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    std::vector<ProviderPoolEntry> pool;
    if (!WalletBatch{wallet.GetDatabase()}.ReadPaymasterProviderPool(pool)) return {};
    std::set<COutPoint> result;
    for (const ProviderPoolEntry& entry : pool) {
        if (IsActiveProviderPoolState(entry.state)) result.insert(entry.outpoint);
    }
    return result;
}

namespace {

constexpr size_t MAX_RECOVERY_ENDPOINT_BYTES{512};
constexpr int64_t CAPACITY_REPLAY_RETENTION_SECONDS{24 * 60 * 60};

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

/** Prepare the accounting side of a provider commit. Modern attempts bind
 * the exact service fee and network fee in the provider authorization
 * manifest, so this event can share the same database transaction as the
 * durable commit. Legacy manifest-less commits are not guessed; their ledger
 * is marked as only partially reconstructable. */
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
        if (!ledger.earlier_history_partial) {
            ledger.earlier_history_partial = true;
            changed = true;
        }
        if (ledger.updated_at < commit.committed_at) {
            ledger.updated_at = commit.committed_at;
            changed = true;
        }
        return RebuildProviderFinanceDailyTotals(ledger, error);
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

bool ProviderBudgetReservationRequired(const ProviderAttempt& attempt)
{
    // Every quote created by the mandatory safety-policy flow binds the
    // policy hash in its provider authorization manifest. A null hash is an
    // explicit legacy marker; wall-clock ordering is not safe for migration
    // decisions because the local clock may move backwards across restarts.
    return !attempt.provider_manifest.safety_policy_hash.IsNull();
}

bool IsLegacyBudgetlessProviderAttempt(const ProviderAttempt& attempt)
{
    if (attempt.provider_manifest.manifest_id.IsNull()) return true;
    return attempt.provider_manifest.version ==
               ProviderAuthorizationManifest::LEGACY_VERSION &&
           attempt.provider_manifest.manifest_id ==
               GetProviderAuthorizationManifestId(attempt.provider_manifest);
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

bool HasExactDurableProviderCommit(WalletBatch& batch,
                                   const ProviderAttempt& attempt)
{
    ProviderCommitRecord commit;
    return batch.ReadPaymasterProviderCommit(attempt.commit_key, commit) &&
           commit.version == ProviderCommitRecord::CURRENT_VERSION &&
           commit.commit_key == attempt.commit_key &&
           commit.provider_id == attempt.provider_id &&
           commit.quote_id == attempt.quote_id &&
           commit.template_commitment == attempt.template_commitment &&
           commit.final_txid == attempt.unsigned_txid &&
           commit.final_txid == attempt.final_txid &&
           commit.raw_transaction_hash == Hash(commit.final_transaction) &&
           commit.final_transaction == attempt.final_transaction &&
           !commit.provider_inputs.empty() && commit.committed_at > 0 &&
           commit.retry_until == attempt.retry_until;
}

bool ValidateProviderBudgetState(
    const ProviderAttempt& attempt,
    const ProviderSafetyPolicy& policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    bool allow_legacy_durable_commit,
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
        allow_historical_policy, allow_legacy_durable_commit, error);
}

bool ValidateProviderAlternativeRecoveryBudgetAuthorizationImpl(
    const AlternativeRecoveryRecord& recovery,
    const ProviderSafetyPolicy* policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    bool allow_legacy_authorized_recovery,
    std::string& error)
{
    error.clear();
    if (!recovery.provider_side ||
        recovery.phase < AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
        !AlternativeRecoveryRecord::IsSupportedVersion(recovery.version) ||
        !ValidateProviderBudgetLedger(ledger, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
        }
        return false;
    }

    const bool legacy =
        recovery.version == AlternativeRecoveryRecord::LEGACY_VERSION;
    if (legacy &&
        (!allow_historical_policy || !allow_legacy_authorized_recovery ||
         recovery.phase < AlternativeRecoveryPhase::USER_SIGNED)) {
        error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_LEGACY_NOT_EXECUTABLE";
        return false;
    }
    if (!legacy &&
        (recovery.provider_safety_policy_hash.IsNull() ||
         recovery.provider_budget_reservation_id !=
             recovery.recovery_response.recovery_commit_key ||
         recovery.provider_netgroup_bucket.IsNull() ||
         recovery.provider_maximum_network_fee.value <= 0 ||
         !(recovery.provider_maximum_network_fee ==
           recovery.recovery_response.manifest.network_fee) ||
         (!allow_historical_policy &&
          (!policy ||
           recovery.provider_safety_policy_hash !=
               GetProviderSafetyPolicyHash(*policy))))) {
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
        (!legacy &&
         first->netgroup_bucket != recovery.provider_netgroup_bucket) ||
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

template <typename T>
std::vector<unsigned char> CanonicalBytes(const T& value)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
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
    if (!AlternativeRecoveryRecord::IsSupportedVersion(recovery.version) ||
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
    if (recovery.version == AlternativeRecoveryRecord::LEGACY_VERSION) {
        // These fields are not serialized by v2. Reject in-memory aliases that
        // would otherwise compare equal after canonical serialization.
        if (has_provider_budget_binding) {
            error = "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
            return false;
        }
    } else if (recovery.provider_side &&
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

struct ExactFinalArtifact {
    std::vector<unsigned char> bytes;
    CMutableTransaction transaction;
    uint256 txid;
    uint256 wtxid;
};

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
        if (!batch.ReadPaymasterAttempt(attempt_id, candidate) ||
            candidate.final_txid != session.final_txid) {
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
    if (!batch.ReadPaymasterRecovery(session.request_id, recovery) ||
        recovery.session_id != session.session_id ||
        recovery.recovery_txid != session.recovery_txid ||
        recovery.raw_transaction_hash != Hash(recovery.final_transaction) ||
        !DecodeExactFinalArtifact(recovery.final_transaction,
                                  session.recovery_txid, artifact, error)) {
        if (error.empty()) error = "PAYMASTER_RECOVERY_FINAL_MISSING";
        return false;
    }

    uint256 alternative_id;
    if (batch.ReadPaymasterAlternativeRecoveryRequest(session.request_id,
                                                      alternative_id)) {
        AlternativeRecoveryRecord alternative;
        if (!batch.ReadPaymasterAlternativeRecovery(alternative_id,
                                                    alternative) ||
            alternative.provider_side ||
            alternative.session_id != session.session_id ||
            alternative.phase != AlternativeRecoveryPhase::FINAL_COMMITTED ||
            alternative.final_transaction != artifact.bytes ||
            alternative.expected_wtxid != artifact.wtxid) {
            error = "PAYMASTER_ALTERNATIVE_RECOVERY_FINAL_MISMATCH";
            return false;
        }
    }
    error.clear();
    return true;
}

struct ExactFinalObservation {
    int confirmation_depth{0};
    bool in_mempool{false};
};

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

} // namespace

bool ValidateProviderAlternativeRecoveryBudgetAuthorization(
    const AlternativeRecoveryRecord& recovery,
    const ProviderSafetyPolicy* policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    bool allow_legacy_authorized_recovery,
    std::string& error)
{
    return ValidateProviderAlternativeRecoveryBudgetAuthorizationImpl(
        recovery, policy, ledger, expected_state, allow_historical_policy,
        allow_legacy_authorized_recovery, error);
}

CreatePaymasterSessionResult PaymasterStore::CreateOrJoinSession(
    const std::string& request_id,
    const uint256& canonical_request_hash,
    DigiDollar::Paymaster::FeeMode requested_mode,
    int64_t now,
    DigiDollar::Paymaster::PaymentSession& session,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (!IsCanonicalRequestId(request_id) || canonical_request_hash.IsNull()) {
        error = "PAYMASTER_INVALID_REQUEST";
        return CreatePaymasterSessionResult::CONFLICT;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (batch.ReadPaymasterSession(request_id, session)) {
        if (session.canonical_request_hash != canonical_request_hash || session.fee_mode_requested != requested_mode) {
            error = "PAYMASTER_REQUEST_ID_CONFLICT";
            return CreatePaymasterSessionResult::CONFLICT;
        }
        return CreatePaymasterSessionResult::JOINED;
    }

    IdempotencyTombstone tombstone;
    if (batch.ReadPaymasterTombstone(request_id, tombstone)) {
        if (tombstone.canonical_request_hash != canonical_request_hash) {
            error = "PAYMASTER_REQUEST_ID_CONFLICT";
            return CreatePaymasterSessionResult::CONFLICT;
        }
        session.request_id = tombstone.request_id;
        session.session_id = tombstone.session_id;
        session.canonical_request_hash = tombstone.canonical_request_hash;
        session.fee_mode_requested = tombstone.fee_mode_requested;
        session.fee_mode_used = tombstone.fee_mode_used;
        session.requested_amount = tombstone.requested_amount;
        session.subtract_paymaster_fee_from_amount =
            tombstone.subtract_paymaster_fee_from_amount;
        session.send_all_spendable_dd = tombstone.send_all_spendable_dd;
        session.state = tombstone.final_state;
        if (tombstone.final_state == SessionState::CANCELED_SAFE) {
            session.recovery_txid = tombstone.final_txid;
        } else {
            session.final_txid = tombstone.final_txid;
        }
        return CreatePaymasterSessionResult::FINAL_TOMBSTONE;
    }

    session = {};
    session.request_id = request_id;
    do {
        session.session_id = GetRandHash();
    } while (session.session_id.IsNull());
    session.canonical_request_hash = canonical_request_hash;
    session.fee_mode_requested = requested_mode;
    session.fee_mode_used = requested_mode;
    session.created_at = now;
    session.updated_at = now;

    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }
    if (!batch.WritePaymasterSession(session, false) ||
        !batch.WritePaymasterSessionId(session.session_id, request_id, false)) {
        Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return CreatePaymasterSessionResult::DATABASE_ERROR;
    }
    return CreatePaymasterSessionResult::CREATED;
}

bool PaymasterStore::BindClientPaymentOrder(
    const std::string& request_id,
    DDCents requested_amount,
    bool subtract_paymaster_fee_from_amount,
    bool send_all_spendable_dd,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || requested_amount.value <= 0 ||
        requested_amount.value > MAX_DD_OUTPUT_CENTS ||
        (send_all_spendable_dd && !subtract_paymaster_fee_from_amount)) {
        error = "PAYMASTER_INVALID_CLIENT_PAYMENT_ORDER";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    const bool already_bound = session.requested_amount.value != 0;
    if (already_bound) {
        if (session.requested_amount != requested_amount ||
            session.subtract_paymaster_fee_from_amount !=
                subtract_paymaster_fee_from_amount ||
            session.send_all_spendable_dd != send_all_spendable_dd) {
            error = "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT";
            return false;
        }
        return true;
    }
    // A migrated additive session can be bound from its already persisted
    // intent. New subtract/sweep semantics are never grafted onto an old
    // active attempt whose original local order cannot prove those flags.
    if (!session.attempt_ids.empty() &&
        (subtract_paymaster_fee_from_amount || send_all_spendable_dd)) {
        error = "PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT";
        return false;
    }
    session.requested_amount = requested_amount;
    session.subtract_paymaster_fee_from_amount =
        subtract_paymaster_fee_from_amount;
    session.send_all_spendable_dd = send_all_spendable_dd;
    session.updated_at = std::max(session.updated_at, GetTime());
    if (!batch.WritePaymasterSession(session)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::GetSessionByRequestId(const std::string& request_id, PaymentSession& session) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (batch.ReadPaymasterSession(request_id, session)) return true;
    IdempotencyTombstone tombstone;
    if (!batch.ReadPaymasterTombstone(request_id, tombstone)) return false;
    session = {};
    session.request_id = tombstone.request_id;
    session.session_id = tombstone.session_id;
    session.canonical_request_hash = tombstone.canonical_request_hash;
    session.fee_mode_requested = tombstone.fee_mode_requested;
    session.fee_mode_used = tombstone.fee_mode_used;
    session.requested_amount = tombstone.requested_amount;
    session.subtract_paymaster_fee_from_amount =
        tombstone.subtract_paymaster_fee_from_amount;
    session.send_all_spendable_dd = tombstone.send_all_spendable_dd;
    session.state = tombstone.final_state;
    if (tombstone.final_state == SessionState::CANCELED_SAFE) {
        session.recovery_txid = tombstone.final_txid;
    } else {
        session.final_txid = tombstone.final_txid;
    }
    return true;
}

bool PaymasterStore::GetSessionBySessionId(const uint256& session_id, PaymentSession& session) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::string request_id;
    if (!batch.ReadPaymasterSessionId(session_id, request_id)) return false;
    if (batch.ReadPaymasterSession(request_id, session)) return session.session_id == session_id;
    IdempotencyTombstone tombstone;
    if (!batch.ReadPaymasterTombstone(request_id, tombstone) || tombstone.session_id != session_id) return false;
    session = {};
    session.request_id = tombstone.request_id;
    session.session_id = tombstone.session_id;
    session.canonical_request_hash = tombstone.canonical_request_hash;
    session.fee_mode_requested = tombstone.fee_mode_requested;
    session.fee_mode_used = tombstone.fee_mode_used;
    session.requested_amount = tombstone.requested_amount;
    session.subtract_paymaster_fee_from_amount =
        tombstone.subtract_paymaster_fee_from_amount;
    session.send_all_spendable_dd = tombstone.send_all_spendable_dd;
    session.state = tombstone.final_state;
    if (tombstone.final_state == SessionState::CANCELED_SAFE) {
        session.recovery_txid = tombstone.final_txid;
    } else {
        session.final_txid = tombstone.final_txid;
    }
    return true;
}

bool PaymasterStore::ListClientSessions(
    std::vector<PaymentSession>& sessions,
    std::string& error) const
{
    sessions.clear();
    error.clear();
    LOCK(m_wallet.cs_wallet);
    if (!WalletBatch{m_wallet.GetDatabase()}.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }
    sessions.erase(
        std::remove_if(sessions.begin(), sessions.end(),
                       [](const PaymentSession& session) {
                           return session.provider_side;
                       }),
        sessions.end());
    return true;
}

bool PaymasterStore::ReserveInputs(
    const std::string& request_id,
    const std::vector<std::pair<COutPoint, ReservationRole>>& inputs,
    FeeMode used_mode,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (inputs.empty()) {
        error = "PAYMASTER_NO_RESERVED_INPUTS";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }

    std::set<COutPoint> unique;
    std::vector<InputReservation> reservations;
    reservations.reserve(inputs.size());
    std::vector<COutPoint> user_inputs;
    for (const auto& [outpoint, role] : inputs) {
        if (outpoint.IsNull() || !unique.insert(outpoint).second) {
            error = "PAYMASTER_INVALID_RESERVED_INPUT";
            return false;
        }
        InputReservation existing;
        const bool occupied = batch.ReadPaymasterReservation(outpoint, existing);
        const bool ours = occupied && existing.session_id == session.session_id;
        if ((occupied && !ours) || (m_wallet.IsLockedCoin(outpoint) && !ours)) {
            error = "PAYMASTER_INPUT_ALREADY_RESERVED";
            return false;
        }
        reservations.push_back({InputReservation::CURRENT_VERSION, outpoint, request_id, session.session_id, role, false, now});
        if (role == ReservationRole::USER_DD || role == ReservationRole::USER_DGB) user_inputs.push_back(outpoint);
    }

    if (session.state == SessionState::INPUTS_RESERVED) {
        if (session.user_inputs == user_inputs && session.fee_mode_used == used_mode) return true;
        error = "PAYMASTER_SESSION_INPUT_CONFLICT";
        return false;
    }
    if (!CanTransition(session.state, SessionState::INPUTS_RESERVED)) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    for (const auto& reservation : reservations) {
        InputReservation existing;
        if (!batch.ReadPaymasterReservation(reservation.outpoint, existing) &&
            !batch.WritePaymasterReservation(reservation, false)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.WriteLockedUTXO(reservation.outpoint)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.user_inputs = std::move(user_inputs);
    session.fee_mode_used = used_mode;
    session.state = SessionState::INPUTS_RESERVED;
    session.updated_at = now;
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    for (const auto& reservation : reservations)
        m_wallet.LockCoin(reservation.outpoint);
    return true;
}

bool PaymasterStore::TransitionSession(const std::string& request_id, SessionState state,
                                       PendingPhase phase, const uint256& final_txid,
                                       int64_t now, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (!CanTransition(session.state, state)) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }
    if ((state == SessionState::PENDING_PROVIDER) != (phase != PendingPhase::NONE)) {
        error = "PAYMASTER_INVALID_PENDING_PHASE";
        return false;
    }
    if (!session.final_txid.IsNull() && !final_txid.IsNull() && session.final_txid != final_txid) {
        error = "PAYMASTER_FINAL_TX_CONFLICT";
        return false;
    }

    const bool authorization_risk = HasAuthorizationRisk(state);
    const uint64_t current_wallet_flags = m_wallet.GetWalletFlags();
    const uint64_t protected_wallet_flags =
        current_wallet_flags | WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    const bool protect_wallet = authorization_risk &&
                                protected_wallet_flags != current_wallet_flags;
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    session.state = state;
    session.pending_phase = phase;
    session.updated_at = now;
    if (!final_txid.IsNull()) session.final_txid = final_txid;
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (protect_wallet && !batch.WriteWalletFlags(protected_wallet_flags)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (authorization_risk && !session.provider_side) {
        for (const auto& outpoint : session.user_inputs) {
            InputReservation reservation;
            if (!batch.ReadPaymasterReservation(outpoint, reservation)) {
                return Abort(batch, error, "PAYMASTER_RESERVATION_MISSING");
            }
            reservation.authorization_may_exist = true;
            if (!batch.WritePaymasterReservation(reservation)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    // The durable flag was committed with the session. Publish exactly that
    // value to memory without issuing a second, fallible database write.
    if (protect_wallet && !m_wallet.LoadWalletFlags(protected_wallet_flags)) {
        assert(false);
    }
    return true;
}

bool PaymasterStore::AddAttempt(const std::string& request_id, ProviderAttempt attempt, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (attempt.version != ProviderAttempt::CURRENT_VERSION || attempt.attempt_id.IsNull() ||
        attempt.provider_id.IsNull() || attempt.state != AttemptState::CANDIDATE ||
        attempt.created_at <= 0 || attempt.updated_at != attempt.created_at ||
        !attempt.commit_key.IsNull() || !attempt.client_nonce.IsNull() ||
        !attempt.intent_hash.IsNull() || !attempt.quote_id.IsNull() || !attempt.unsigned_txid.IsNull() ||
        !attempt.template_commitment.IsNull() || !attempt.unsigned_intent.empty() ||
        !attempt.client_manifest.manifest_id.IsNull() ||
        !attempt.provider_manifest.manifest_id.IsNull() ||
        !attempt.accepted_client_manifest_id.IsNull() ||
        attempt.client_manifest_accepted_at != 0 ||
        attempt.provider_signed_at != 0 ||
        !attempt.provider_signed_result.empty() ||
        !attempt.capacity_proof_claim_candidate.empty() ||
        !attempt.quote_response_claim_candidate.empty() ||
        !attempt.quote_request.empty() ||
        !attempt.signed_quote.empty() ||
        !attempt.unsigned_transaction.empty() || !attempt.unsigned_psbt.empty() ||
        !attempt.input_roles.empty() || !attempt.user_signed_psbt.empty() ||
        !attempt.final_transaction.empty() || !attempt.final_txid.IsNull() ||
        attempt.quote_expires_at != 0 || attempt.retry_until != 0) {
        error = "PAYMASTER_INVALID_ATTEMPT";
        return false;
    }
    if (!attempt.session_id.IsNull() && attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
        return false;
    }
    if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt.attempt_id) != session.attempt_ids.end()) {
        ProviderAttempt existing;
        if (batch.ReadPaymasterAttempt(attempt.attempt_id, existing) &&
            existing.session_id == session.session_id && existing.provider_id == attempt.provider_id) return true;
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }
    ProviderAttempt collision;
    if (batch.ReadPaymasterAttempt(attempt.attempt_id, collision)) {
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }

    attempt.session_id = session.session_id;
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAttempt(attempt, false)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    session.attempt_ids.push_back(attempt.attempt_id);
    session.updated_at = std::max(session.updated_at, attempt.created_at);
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::PreparePaymentIntent(const std::string& request_id,
                                          ProviderAttempt attempt,
                                          int64_t now,
                                          std::string& error)
{
    error.clear();
    PaymentIntent intent;
    if (!DecodeUnsignedIntent(attempt.unsigned_intent, intent, now, error)) return false;
    if (attempt.version != ProviderAttempt::CURRENT_VERSION || attempt.attempt_id.IsNull() ||
        attempt.provider_id != intent.provider_id || attempt.state != AttemptState::CANDIDATE ||
        attempt.created_at <= 0 || attempt.created_at > now ||
        attempt.updated_at != attempt.created_at || attempt.client_nonce != intent.client_nonce ||
        !attempt.intent_hash.IsNull() || !attempt.commit_key.IsNull() ||
        !attempt.quote_id.IsNull() || !attempt.unsigned_txid.IsNull() ||
        !attempt.template_commitment.IsNull() || !attempt.capacity_request.empty() ||
        !attempt.capacity_snapshot.snapshot_id.IsNull() || !attempt.quote_request.empty() ||
        !attempt.client_manifest.manifest_id.IsNull() ||
        !attempt.provider_manifest.manifest_id.IsNull() ||
        !attempt.accepted_client_manifest_id.IsNull() ||
        attempt.client_manifest_accepted_at != 0 ||
        attempt.provider_signed_at != 0 ||
        !attempt.provider_signed_result.empty() ||
        !attempt.capacity_proof_claim_candidate.empty() ||
        !attempt.quote_response_claim_candidate.empty() ||
        !attempt.signed_quote.empty() || !attempt.unsigned_transaction.empty() ||
        !attempt.unsigned_psbt.empty() || !attempt.input_roles.empty() ||
        !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !attempt.final_txid.IsNull() || attempt.quote_expires_at != 0 || attempt.retry_until != 0) {
        error = "PAYMASTER_INVALID_INTENT_ATTEMPT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (intent.request_id != request_id || intent.session_id != session.session_id ||
        intent.canonical_request_hash != session.canonical_request_hash ||
        intent.requested_fee_mode != session.fee_mode_requested ||
        intent.privacy_profile != attempt.privacy_profile ||
        (!attempt.session_id.IsNull() && attempt.session_id != session.session_id)) {
        error = "PAYMASTER_INTENT_SESSION_CONFLICT";
        return false;
    }
    attempt.session_id = session.session_id;

    ProviderAttempt existing_attempt;
    if (batch.ReadPaymasterAttempt(attempt.attempt_id, existing_attempt)) {
        if (existing_attempt.session_id == attempt.session_id &&
            existing_attempt.provider_id == attempt.provider_id &&
            existing_attempt.client_nonce == attempt.client_nonce &&
            existing_attempt.unsigned_intent == attempt.unsigned_intent &&
            session.user_inputs == intent.user_dd_inputs &&
            session.fee_mode_used == FeeMode::PAYMASTER) {
            return true;
        }
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }
    const bool initial_reservation = session.state == SessionState::CREATED &&
                                     session.attempt_ids.empty();
    bool reuse_reservation = session.state == SessionState::INPUTS_RESERVED &&
                             session.user_inputs == intent.user_dd_inputs &&
                             session.fee_mode_used == FeeMode::PAYMASTER;
    if (reuse_reservation && !session.attempt_ids.empty()) {
        ProviderAttempt previous;
        reuse_reservation = batch.ReadPaymasterAttempt(session.attempt_ids.back(), previous) &&
                            (previous.state == AttemptState::REJECTED ||
                             previous.state == AttemptState::QUOTE_EXPIRED) &&
                            previous.user_signed_psbt.empty() &&
                            previous.final_transaction.empty() &&
                            previous.final_txid.IsNull();
    }
    if (!initial_reservation && !reuse_reservation) {
        error = "PAYMASTER_SESSION_INPUT_CONFLICT";
        return false;
    }

    std::vector<InputReservation> reservations;
    for (const COutPoint& outpoint : intent.user_dd_inputs) {
        InputReservation occupied;
        const bool is_occupied = batch.ReadPaymasterReservation(outpoint, occupied);
        const bool ours = is_occupied && occupied.session_id == session.session_id &&
                          occupied.role == ReservationRole::USER_DD;
        if ((is_occupied && !ours) || (m_wallet.IsLockedCoin(outpoint) && !ours)) {
            error = "PAYMASTER_INPUT_ALREADY_RESERVED";
            return false;
        }
        if (!ours) {
            reservations.push_back({InputReservation::CURRENT_VERSION, outpoint, request_id,
                                    session.session_id, ReservationRole::USER_DD, false, now});
        }
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    for (const InputReservation& reservation : reservations) {
        if (!batch.WritePaymasterReservation(reservation, false) ||
            !batch.WriteLockedUTXO(reservation.outpoint)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.WritePaymasterAttempt(attempt, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (initial_reservation) session.user_inputs = intent.user_dd_inputs;
    session.attempt_ids.push_back(attempt.attempt_id);
    session.fee_mode_used = FeeMode::PAYMASTER;
    session.state = SessionState::INPUTS_RESERVED;
    session.updated_at = now;
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    for (const InputReservation& reservation : reservations)
        m_wallet.LockCoin(reservation.outpoint);
    return true;
}

bool PaymasterStore::PrepareCapacityRequest(
    const std::string& request_id,
    const uint256& attempt_id,
    const std::vector<unsigned char>& capacity_request,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || capacity_request.empty() ||
        capacity_request.size() > MAX_DIRECT_MESSAGE_BYTES || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_REQUEST";
        return false;
    }
    PaymasterCapacityRequest decoded;
    try {
        CDataStream stream{capacity_request, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> decoded;
        if (!stream.empty()) throw std::ios_base::failure("trailing capacity request");
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_REQUEST_ENCODING";
        return false;
    }
    CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical << decoded;
    const auto canonical_bytes = MakeUCharSpan(canonical);
    if (!std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                    capacity_request.begin(), capacity_request.end()) ||
        !ValidateCapacityRequestEnvelope(decoded, decoded.genesis_hash, now, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_REQUEST_NONCANONICAL";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymentIntent intent;
    std::string intent_error;
    if (!DecodeUnsignedIntent(attempt.unsigned_intent, intent, attempt.created_at,
                              intent_error)) {
        error = "PAYMASTER_PERSISTED_INTENT_CORRUPT";
        return false;
    }
    if (attempt.state != AttemptState::CANDIDATE || !attempt.quote_request.empty() ||
        !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !attempt.capacity_snapshot.snapshot_id.IsNull() ||
        decoded.request_id != request_id || decoded.session_id != session.session_id ||
        decoded.provider_id != attempt.provider_id ||
        decoded.client_nonce != attempt.client_nonce ||
        decoded.genesis_hash != intent.genesis_hash ||
        decoded.funding_model != intent.funding_model ||
        (intent.funding_model == FundingModel::SPONSORED &&
         decoded.requires_carrier) ||
        decoded.expires_at > intent.expires_at) {
        error = "PAYMASTER_CAPACITY_REQUEST_BINDING_MISMATCH";
        return false;
    }
    if (!attempt.capacity_request.empty()) {
        if (attempt.capacity_request == capacity_request) return true;
        error = "PAYMASTER_CAPACITY_REQUEST_CONFLICT";
        return false;
    }
    attempt.capacity_request = capacity_request;
    attempt.updated_at = std::max(attempt.updated_at, now);
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAttempt(attempt) || !batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::CommitValidatedCapacitySnapshot(
    const std::string& request_id,
    const uint256& attempt_id,
    ValidatedCapacitySnapshot snapshot,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || snapshot.version != ValidatedCapacitySnapshot::CURRENT_VERSION ||
        snapshot.snapshot_id.IsNull() || snapshot.resource_commitment.IsNull() ||
        snapshot.capacity_proof.empty() || snapshot.validated_at != now || now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_SNAPSHOT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id || attempt.capacity_request.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof proof;
    try {
        CDataStream request_stream{attempt.capacity_request, SER_NETWORK, ::PROTOCOL_VERSION};
        CDataStream proof_stream{snapshot.capacity_proof, SER_NETWORK, ::PROTOCOL_VERSION};
        request_stream >> capacity_request;
        proof_stream >> proof;
        if (!request_stream.empty() || !proof_stream.empty()) {
            throw std::ios_base::failure("trailing capacity data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
        return false;
    }
    CDataStream canonical_proof{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_proof << proof;
    const auto proof_bytes = MakeUCharSpan(canonical_proof);
    if (!std::equal(proof_bytes.begin(), proof_bytes.end(),
                    snapshot.capacity_proof.begin(), snapshot.capacity_proof.end()) ||
        !ValidateCapacityProofEnvelope(proof, capacity_request, now, error) ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != proof.provider_id ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_SNAPSHOT";
        return false;
    }
    snapshot.session_id = session.session_id;
    snapshot.attempt_id = attempt.attempt_id;
    if (snapshot.snapshot_id != proof.snapshot_id ||
        snapshot.resource_commitment != GetCapacityResourceCommitment(proof) ||
        snapshot.provider_id != proof.provider_id ||
        snapshot.provider_id != attempt.provider_id ||
        snapshot.client_nonce != proof.client_nonce ||
        snapshot.client_nonce != attempt.client_nonce ||
        snapshot.funding_model != proof.funding_model ||
        snapshot.funding_model != capacity_request.funding_model ||
        snapshot.requires_carrier != proof.requires_carrier ||
        snapshot.requires_carrier != capacity_request.requires_carrier ||
        snapshot.request_hash != Hash(attempt.capacity_request) ||
        snapshot.created_at != proof.created_at || snapshot.expires_at != proof.expires_at ||
        snapshot.expires_at <= now) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
        return false;
    }
    PaymasterCapacityRequest authoritative_request;
    if (!attempt.capacity_proof_claim_candidate.empty() &&
        (!ValidatePersistedCapacityRequestAuthority(
             session, attempt, authoritative_request, error) ||
         !ValidatePersistedCapacityClaimCandidate(
             attempt.capacity_proof_claim_candidate, authoritative_request,
             attempt.provider_identity_key,
             "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
             error))) {
        return false;
    }
    if (!attempt.capacity_snapshot.snapshot_id.IsNull()) {
        if (SameCapacitySnapshot(attempt.capacity_snapshot, snapshot)) return true;
        std::string evidence_error;
        if (RecordCapacityEquivocationLocked(
                batch, capacity_request, attempt.provider_identity_key,
                attempt.capacity_snapshot.capacity_proof,
                snapshot.capacity_proof, snapshot.request_hash, now,
                evidence_error)) {
            error = std::move(evidence_error);
            return false;
        }
        if (!evidence_error.empty()) {
            error = std::move(evidence_error);
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }

    if (attempt.capacity_proof_claim_candidate.empty()) {
        error = "PAYMASTER_CAPACITY_CLAIM_CANDIDATE_MISSING";
        return false;
    }
    if (attempt.capacity_proof_claim_candidate != snapshot.capacity_proof) {
        std::string evidence_error;
        if (RecordCapacityEquivocationLocked(
                batch, authoritative_request, attempt.provider_identity_key,
                attempt.capacity_proof_claim_candidate,
                snapshot.capacity_proof, snapshot.request_hash, now,
                evidence_error)) {
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_EQUIVOCATION" : std::move(evidence_error);
            return false;
        }
        error = evidence_error.empty() ? "PAYMASTER_CAPACITY_CLAIM_CANDIDATE_CONFLICT" : std::move(evidence_error);
        return false;
    }

    ValidatedCapacitySnapshot by_id;
    if (batch.ReadPaymasterCapacitySnapshot(snapshot.snapshot_id, by_id) &&
        !SameCapacitySnapshot(by_id, snapshot)) {
        std::string evidence_error;
        if (by_id.provider_id == snapshot.provider_id &&
            by_id.request_hash == snapshot.request_hash &&
            RecordCapacityEquivocationLocked(
                batch, capacity_request, attempt.provider_identity_key,
                by_id.capacity_proof, snapshot.capacity_proof,
                snapshot.request_hash, now, evidence_error)) {
            error = std::move(evidence_error);
            return false;
        }
        if (!evidence_error.empty()) {
            error = std::move(evidence_error);
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    std::vector<CapacityResourceBinding> resource_bindings;
    if (!BuildCapacityResourceBindings(snapshot, proof, resource_bindings,
                                       error)) {
        return false;
    }

    // Compatibility scan for wallets created before the per-outpoint index.
    // It also provides a fail-closed cross-check if an index entry was lost or
    // corrupted. Only live snapshots can conflict; expired authority may be
    // replaced but remains available as historical evidence.
    std::vector<ValidatedCapacitySnapshot> persisted_snapshots;
    if (!batch.ListPaymasterCapacitySnapshots(persisted_snapshots)) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_DATABASE_READ";
        return false;
    }
    for (const ValidatedCapacitySnapshot& existing : persisted_snapshots) {
        if (existing.snapshot_id == snapshot.snapshot_id ||
            existing.provider_id != snapshot.provider_id ||
            existing.expires_at <= now) {
            continue;
        }
        PaymasterCapacityProof existing_proof;
        if (!DecodeCanonicalCapacityProof(existing.capacity_proof,
                                          existing_proof)) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
            return false;
        }
        for (const CapacityResourceBinding& binding : resource_bindings) {
            if (!CapacityProofContainsOutpoint(existing_proof,
                                               binding.outpoint)) {
                continue;
            }
            std::string evidence_error;
            if (RecordCapacityResourceEquivocationLocked(
                    batch, snapshot.provider_id,
                    attempt.provider_identity_key, binding.outpoint,
                    existing.capacity_proof, snapshot.capacity_proof, now,
                    evidence_error)) {
                error = std::move(evidence_error);
                return false;
            }
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND" : std::move(evidence_error);
            return false;
        }
    }

    std::vector<bool> replace_resources;
    replace_resources.reserve(resource_bindings.size());
    for (const CapacityResourceBinding& binding : resource_bindings) {
        CapacityResourceBinding existing;
        const bool have_existing = batch.ReadPaymasterCapacityResource(
            binding.provider_id, binding.outpoint, existing);
        if (!have_existing) {
            replace_resources.push_back(false);
            continue;
        }
        if (SameCapacityResourceBinding(existing, binding)) {
            replace_resources.push_back(true);
            continue;
        }
        if (existing.expires_at > now) {
            ValidatedCapacitySnapshot existing_snapshot;
            std::string evidence_error;
            if (batch.ReadPaymasterCapacitySnapshot(existing.snapshot_id,
                                                    existing_snapshot) &&
                RecordCapacityResourceEquivocationLocked(
                    batch, snapshot.provider_id,
                    attempt.provider_identity_key, binding.outpoint,
                    existing_snapshot.capacity_proof,
                    snapshot.capacity_proof, now, evidence_error)) {
                error = std::move(evidence_error);
                return false;
            }
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND" : std::move(evidence_error);
            return false;
        }
        replace_resources.push_back(true);
    }

    uint256 bound_snapshot_id;
    bool replace_slot{false};
    if (batch.ReadPaymasterCapacitySlot(snapshot.resource_commitment,
                                        bound_snapshot_id) &&
        bound_snapshot_id != snapshot.snapshot_id) {
        ValidatedCapacitySnapshot existing;
        if (!batch.ReadPaymasterCapacitySnapshot(bound_snapshot_id, existing)) {
            error = "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND";
            return false;
        }
        // A live aggregate collision necessarily overlaps every resource and
        // has already been rejected above. Reaching here is therefore only a
        // safe expired replacement.
        if (existing.expires_at > now) {
            error = "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND";
            return false;
        }
        replace_slot = true;
    }

    attempt.capacity_snapshot = snapshot;
    attempt.updated_at = std::max(attempt.updated_at, now);
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterCapacitySnapshot(snapshot, false) ||
        !batch.WritePaymasterCapacitySlot(snapshot.resource_commitment,
                                          snapshot.snapshot_id, replace_slot) ||
        !batch.WritePaymasterAttempt(attempt) || !batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    for (size_t index = 0; index < resource_bindings.size(); ++index) {
        if (!batch.WritePaymasterCapacityResource(
                resource_bindings[index], replace_resources[index])) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RecordCapacityEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || conflicting_capacity_proof.empty() ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt_id) == session.attempt_ids.end() ||
        attempt.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    PaymasterCapacityRequest request;
    if (!ValidatePersistedCapacityRequestAuthority(
            session, attempt, request, error)) {
        return false;
    }
    PaymasterCapacityProof persisted_proof;
    if (!VerifyCapacityEvidenceArtifact(
            attempt.capacity_snapshot.capacity_proof, request,
            attempt.provider_identity_key, persisted_proof)) {
        error = "PAYMASTER_PERSISTED_CAPACITY_SNAPSHOT_CORRUPT";
        return false;
    }
    if (!RecordCapacityEquivocationLocked(
            batch, request, attempt.provider_identity_key,
            attempt.capacity_snapshot.capacity_proof,
            conflicting_capacity_proof, Hash(attempt.capacity_request),
            observed_at, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::RecordPendingCapacityEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || first_capacity_proof.empty() ||
        conflicting_capacity_proof.empty() || observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    const ValidatedCapacitySnapshot empty_snapshot;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || session.request_id != request_id ||
        attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !SameCapacitySnapshot(attempt.capacity_snapshot, empty_snapshot)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    PaymasterCapacityRequest request;
    if (!ValidatePersistedCapacityRequestAuthority(
            session, attempt, request, error)) {
        return false;
    }
    if (!RecordCapacityEquivocationLocked(
            batch, request, attempt.provider_identity_key,
            first_capacity_proof, conflicting_capacity_proof,
            Hash(attempt.capacity_request), observed_at, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::StageCapacityProofClaimCandidate(
    const uint256& attempt_id,
    const std::vector<unsigned char>& capacity_proof,
    int64_t observed_at,
    bool& equivocation,
    std::string& error)
{
    error.clear();
    equivocation = false;
    if (attempt_id.IsNull() || capacity_proof.empty() ||
        capacity_proof.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    PaymentSession session;
    std::string request_id;
    const ValidatedCapacitySnapshot empty_snapshot;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !SameCapacitySnapshot(attempt.capacity_snapshot, empty_snapshot)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }

    PaymasterCapacityRequest request;
    PaymasterCapacityProof proof;
    std::string validation_error;
    if (!ValidatePersistedCapacityRequestAuthority(
            session, attempt, request, error)) {
        return false;
    }
    if (!attempt.capacity_proof_claim_candidate.empty() &&
        !ValidatePersistedCapacityClaimCandidate(
            attempt.capacity_proof_claim_candidate, request,
            attempt.provider_identity_key,
            "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
            error)) {
        return false;
    }
    if (!DecodeCanonicalCapacityProof(capacity_proof, proof) ||
        !ValidateCapacityProofEnvelope(
            proof, request, proof.created_at, validation_error) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof), proof.identity_signature)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    if (!attempt.capacity_proof_claim_candidate.empty()) {
        if (attempt.capacity_proof_claim_candidate == capacity_proof) {
            return true;
        }
        if (!RecordCapacityEquivocationLocked(
                batch, request, attempt.provider_identity_key,
                attempt.capacity_proof_claim_candidate, capacity_proof,
                Hash(attempt.capacity_request), observed_at, error)) {
            if (error.empty()) {
                error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
            }
            return false;
        }
        equivocation = true;
        error.clear();
        return true;
    }

    if (!ValidateCapacityRequestEnvelope(
            request, request.genesis_hash, observed_at, validation_error) ||
        !ValidateCapacityProofEnvelope(
            proof, request, observed_at, validation_error)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    attempt.version = ProviderAttempt::CURRENT_VERSION;
    attempt.capacity_proof_claim_candidate = capacity_proof;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RecordAlternativeRecoveryCapacityEquivocation(
    const uint256& recovery_id,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (recovery_id.IsNull() || conflicting_capacity_proof.empty() ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord recovery;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.recovery_id != recovery_id ||
        recovery.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }
    if (!ValidatePersistedRecoveryCapacityRequestAuthority(recovery,
                                                           error)) {
        return false;
    }
    PaymasterCapacityProof persisted_proof;
    if (!VerifyCapacityEvidenceArtifact(
            recovery.capacity_snapshot.capacity_proof,
            recovery.capacity_request,
            recovery.recovery_provider_identity_key, persisted_proof)) {
        error = "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_SNAPSHOT_CORRUPT";
        return false;
    }
    const std::vector<unsigned char> request_bytes =
        CanonicalBytes(recovery.capacity_request);
    if (!RecordCapacityEquivocationLocked(
            batch, recovery.capacity_request,
            recovery.recovery_provider_identity_key,
            recovery.capacity_snapshot.capacity_proof,
            conflicting_capacity_proof, Hash(request_bytes), observed_at,
            error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::RecordPendingAlternativeRecoveryCapacityEquivocation(
    const uint256& recovery_id,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& conflicting_capacity_proof,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (recovery_id.IsNull() || first_capacity_proof.empty() ||
        conflicting_capacity_proof.empty() || observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord recovery;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.recovery_id != recovery_id ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING ||
        !recovery.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }
    if (!ValidatePersistedRecoveryCapacityRequestAuthority(recovery,
                                                           error)) {
        return false;
    }
    const std::vector<unsigned char> request_bytes =
        CanonicalBytes(recovery.capacity_request);
    if (!RecordCapacityEquivocationLocked(
            batch, recovery.capacity_request,
            recovery.recovery_provider_identity_key, first_capacity_proof,
            conflicting_capacity_proof, Hash(request_bytes), observed_at,
            error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::StageAlternativeRecoveryCapacityProofClaimCandidate(
    const uint256& recovery_id,
    const std::vector<unsigned char>& capacity_proof,
    int64_t observed_at,
    bool& equivocation,
    std::string& error)
{
    error.clear();
    equivocation = false;
    if (recovery_id.IsNull() || capacity_proof.empty() ||
        capacity_proof.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    AlternativeRecoveryRecord recovery;
    if (!batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) ||
        recovery.provider_side || recovery.recovery_id != recovery_id ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING ||
        !recovery.capacity_snapshot.capacity_proof.empty()) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
        return false;
    }

    PaymasterCapacityProof proof;
    std::string validation_error;
    if (!ValidatePersistedRecoveryCapacityRequestAuthority(recovery,
                                                           error)) {
        return false;
    }
    if (!recovery.capacity_proof_claim_candidate.empty() &&
        !ValidatePersistedCapacityClaimCandidate(
            recovery.capacity_proof_claim_candidate,
            recovery.capacity_request,
            recovery.recovery_provider_identity_key,
            "PAYMASTER_PERSISTED_RECOVERY_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
            error)) {
        return false;
    }
    if (!DecodeCanonicalCapacityProof(capacity_proof, proof) ||
        !ValidateCapacityProofEnvelope(
            proof, recovery.capacity_request, proof.created_at,
            validation_error) ||
        !recovery.recovery_provider_identity_key.VerifySchnorr(
            GetCapacityProofSignatureHash(proof),
            proof.identity_signature)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    if (!recovery.capacity_proof_claim_candidate.empty()) {
        if (recovery.capacity_proof_claim_candidate == capacity_proof) {
            return true;
        }
        const std::vector<unsigned char> request_bytes =
            CanonicalBytes(recovery.capacity_request);
        if (!RecordCapacityEquivocationLocked(
                batch, recovery.capacity_request,
                recovery.recovery_provider_identity_key,
                recovery.capacity_proof_claim_candidate, capacity_proof,
                Hash(request_bytes), observed_at, error)) {
            if (error.empty()) {
                error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
            }
            return false;
        }
        equivocation = true;
        error.clear();
        return true;
    }

    if (!ValidateCapacityRequestEnvelope(
            recovery.capacity_request,
            recovery.capacity_request.genesis_hash, observed_at,
            validation_error) ||
        !ValidateCapacityProofEnvelope(
            proof, recovery.capacity_request, observed_at,
            validation_error)) {
        error = "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
        return false;
    }

    recovery.version = AlternativeRecoveryRecord::CURRENT_VERSION;
    recovery.capacity_proof_claim_candidate = capacity_proof;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterAlternativeRecovery(recovery)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetProviderCapacityProof(
    const PaymasterCapacityRequest& request,
    int64_t now,
    PaymasterCapacityProof& proof,
    std::string& error) const
{
    error.clear();
    proof = {};
    if (now <= 0 ||
        !ValidateCapacityRequestEnvelope(request, request.genesis_hash, now,
                                         error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_REQUEST";
        return false;
    }
    const uint256 request_hash{Hash(CanonicalBytes(request))};
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 indexed_hash;
    std::vector<unsigned char> encoded;
    ProviderCapacityReleaseRecord release;
    if (!batch.ReadPaymasterCapacityNonce(request.client_nonce, indexed_hash) ||
        indexed_hash != request_hash ||
        !batch.ReadPaymasterCapacityResponse(request_hash, encoded) ||
        batch.ReadPaymasterCapacityRelease(request_hash, release) ||
        !DecodeCanonicalCapacityProof(encoded, proof) ||
        !ValidateCapacityProofEnvelope(proof, request, now, error)) {
        proof = {};
        if (error.empty()) error = "PAYMASTER_CAPACITY_RESERVATION_MISSING";
        return false;
    }
    return true;
}

bool PaymasterStore::FinalizePaymentIntent(const std::string& request_id,
                                           const uint256& attempt_id,
                                           const std::vector<unsigned char>& quote_request,
                                           int64_t now,
                                           std::string& error)
{
    error.clear();
    if (quote_request.empty() || quote_request.size() > MAX_DIRECT_MESSAGE_BYTES) {
        error = "PAYMASTER_QUOTE_REQUEST_ENCODING";
        return false;
    }
    PaymasterQuoteRequest request;
    try {
        CDataStream stream{quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> request;
        if (!stream.empty()) {
            error = "PAYMASTER_QUOTE_REQUEST_ENCODING";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_QUOTE_REQUEST_ENCODING";
        return false;
    }
    CDataStream canonical{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical << request;
    const auto canonical_bytes = MakeUCharSpan(canonical);
    if (!std::equal(canonical_bytes.begin(), canonical_bytes.end(),
                    quote_request.begin(), quote_request.end()) ||
        !ValidateRedactedQuoteRequestEnvelope(request, request.intent.genesis_hash, now, error)) {
        if (error.empty()) error = "PAYMASTER_QUOTE_REQUEST_NONCANONICAL";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymentIntent draft;
    std::string draft_error;
    if (!DecodeUnsignedIntent(attempt.unsigned_intent, draft, attempt.created_at, draft_error)) {
        error = "PAYMASTER_PERSISTED_INTENT_CORRUPT";
        return false;
    }
    if (attempt.capacity_request.empty() || attempt.capacity_snapshot.snapshot_id.IsNull() ||
        attempt.capacity_snapshot.request_hash != Hash(attempt.capacity_request) ||
        attempt.capacity_snapshot.provider_id != attempt.provider_id ||
        attempt.capacity_snapshot.client_nonce != attempt.client_nonce ||
        attempt.capacity_snapshot.expires_at <= now) {
        error = "PAYMASTER_CAPACITY_PROOF_REQUIRED";
        return false;
    }
    if (request.intent.request_id != request_id || request.intent.session_id != session.session_id ||
        request.intent.canonical_request_hash != session.canonical_request_hash ||
        request.intent.requested_fee_mode != session.fee_mode_requested ||
        request.intent.privacy_profile != attempt.privacy_profile ||
        request.intent.provider_id != attempt.provider_id ||
        request.intent.client_nonce != attempt.client_nonce ||
        attempt.sponsorship_capability_hash != request.intent.sponsorship_authorization_hash ||
        GetPaymentIntentCoreHash(request.intent) != GetPaymentIntentCoreHash(draft) ||
        session.user_inputs != request.intent.user_dd_inputs ||
        (session.state != SessionState::INPUTS_RESERVED &&
         session.state != SessionState::AWAITING_WALLET_UNLOCK &&
         session.state != SessionState::AWAITING_USER_SIGNATURE)) {
        error = "PAYMASTER_SIGNED_INTENT_CONFLICT";
        return false;
    }
    const uint256 intent_hash = GetPaymentIntentHash(request.intent);
    if (!attempt.quote_request.empty()) {
        if (attempt.quote_request == quote_request && attempt.intent_hash == intent_hash) return true;
        error = "PAYMASTER_QUOTE_REQUEST_CONFLICT";
        return false;
    }
    if (!attempt.intent_hash.IsNull()) {
        error = "PAYMASTER_INTENT_CONFLICT";
        return false;
    }
    attempt.quote_request = quote_request;
    attempt.intent_hash = intent_hash;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::AbandonClientAttemptForFallback(const std::string& request_id,
                                                     const uint256& attempt_id,
                                                     int64_t now,
                                                     std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_FALLBACK";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) || session.provider_side ||
        session.attempt_ids.empty() || session.attempt_ids.back() != attempt_id ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (attempt.state == AttemptState::REJECTED &&
        session.state == SessionState::INPUTS_RESERVED) {
        return true;
    }
    if ((attempt.state != AttemptState::CANDIDATE &&
         attempt.state != AttemptState::QUOTED &&
         attempt.state != AttemptState::QUOTE_EXPIRED) ||
        !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !attempt.final_txid.IsNull() ||
        (session.state != SessionState::INPUTS_RESERVED &&
         session.state != SessionState::AWAITING_WALLET_UNLOCK &&
         session.state != SessionState::AWAITING_USER_SIGNATURE &&
         session.state != SessionState::AUTHORIZED)) {
        error = "PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST";
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.role != ReservationRole::USER_DD) {
            error = "PAYMASTER_RESERVATION_MISSING";
            return false;
        }
        if (reservation.authorization_may_exist) {
            error = "PAYMASTER_FALLBACK_AUTHORIZATION_MAY_EXIST";
            return false;
        }
    }

    ClientFeeLedger client_fee_ledger;
    bool client_fee_changed{false};
    if (!attempt.accepted_client_manifest_id.IsNull()) {
        ClientSafetyPolicy client_policy;
        const bool have_client_policy =
            batch.ReadPaymasterClientSafetyPolicy(client_policy);
        const bool have_client_ledger =
            batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
        if ((!have_client_policy &&
             batch.HasPaymasterClientSafetyPolicy()) ||
            (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
            have_client_policy != have_client_ledger) {
            error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
            return false;
        }
        PaymasterQuoteResponse quote_response;
        try {
            SpanReader stream{::PROTOCOL_VERSION, attempt.signed_quote};
            stream >> quote_response;
            if (!stream.empty()) {
                throw std::ios_base::failure(
                    "trailing paymaster quote data");
            }
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_QUOTE_ENCODING";
            return false;
        }
        if (have_client_ledger) {
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(),
                client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key == attempt.commit_key;
                });
            if (reservation == client_fee_ledger.reservations.end() ||
                reservation->service_fee !=
                    quote_response.quote.service_fee ||
                reservation->state != BudgetReservationState::RESERVED ||
                !ReleaseClientFee(client_fee_ledger,
                                  attempt.commit_key, now, error)) {
                if (error.empty()) {
                    error =
                        "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                }
                return false;
            }
            client_fee_changed = true;
        } else if (quote_response.quote.service_fee.value != 0) {
            error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
            return false;
        }
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    attempt.state = AttemptState::REJECTED;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.state = SessionState::INPUTS_RESERVED;
    session.pending_phase = PendingPhase::NONE;
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.WritePaymasterSession(session)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (client_fee_changed &&
        !batch.WritePaymasterClientFeeLedger(client_fee_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::AbandonUnsignedClientSession(const std::string& request_id,
                                                  int64_t now,
                                                  std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || now <= 0) {
        error = "PAYMASTER_INVALID_UNSIGNED_ABANDON";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (session.state == SessionState::FAILED) {
        for (const COutPoint& outpoint : session.user_inputs) {
            InputReservation reservation;
            if (batch.ReadPaymasterReservation(outpoint, reservation)) {
                error = "PAYMASTER_FAILED_SESSION_STILL_RESERVED";
                return false;
            }
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            m_wallet.UnlockCoin(outpoint);
        }
        return true;
    }
    if ((session.state != SessionState::CREATED &&
         session.state != SessionState::INPUTS_RESERVED &&
         session.state != SessionState::AWAITING_WALLET_UNLOCK &&
         session.state != SessionState::AWAITING_USER_SIGNATURE) ||
        session.pending_phase != PendingPhase::NONE ||
        !session.final_txid.IsNull() || !session.recovery_txid.IsNull()) {
        error = "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST";
        return false;
    }

    std::vector<ProviderAttempt> attempts;
    attempts.reserve(session.attempt_ids.size());
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
            attempt.session_id != session.session_id) {
            error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
            return false;
        }
        if ((attempt.state != AttemptState::CANDIDATE &&
             attempt.state != AttemptState::QUOTED &&
             attempt.state != AttemptState::REJECTED &&
             attempt.state != AttemptState::QUOTE_EXPIRED) ||
            !attempt.user_signed_psbt.empty() ||
            !attempt.final_transaction.empty() ||
            !attempt.final_txid.IsNull() || attempt.provider_signed_at > 0 ||
            !attempt.provider_signed_result.empty()) {
            error = "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST";
            return false;
        }
        attempts.push_back(std::move(attempt));
    }

    for (const COutPoint& outpoint : session.user_inputs) {
        InputReservation reservation;
        if (!batch.ReadPaymasterReservation(outpoint, reservation) ||
            reservation.session_id != session.session_id ||
            reservation.request_id != request_id ||
            reservation.role != ReservationRole::USER_DD ||
            reservation.authorization_may_exist) {
            error = "PAYMASTER_RESERVATION_NOT_CANCELABLE";
            return false;
        }
    }

    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    const bool have_client_policy =
        batch.ReadPaymasterClientSafetyPolicy(client_policy);
    const bool have_client_ledger =
        batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
    if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
        (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
        have_client_policy != have_client_ledger) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }

    bool client_fee_changed{false};
    for (const ProviderAttempt& attempt : attempts) {
        if (attempt.commit_key.IsNull()) continue;
        if (!have_client_ledger) {
            if (!attempt.accepted_client_manifest_id.IsNull()) {
                PaymasterQuoteResponse response;
                try {
                    SpanReader stream{::PROTOCOL_VERSION,
                                      attempt.signed_quote};
                    stream >> response;
                    if (!stream.empty()) {
                        throw std::ios_base::failure(
                            "trailing paymaster quote data");
                    }
                } catch (const std::ios_base::failure&) {
                    error = "PAYMASTER_QUOTE_ENCODING";
                    return false;
                }
                if (response.quote.service_fee.value != 0) {
                    error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                    return false;
                }
            }
            continue;
        }
        const auto reservation = std::find_if(
            client_fee_ledger.reservations.begin(),
            client_fee_ledger.reservations.end(),
            [&](const ClientFeeReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation == client_fee_ledger.reservations.end()) {
            if (!attempt.accepted_client_manifest_id.IsNull()) {
                error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                return false;
            }
            continue;
        }
        if (reservation->state == BudgetReservationState::SPENT) {
            error = "PAYMASTER_UNSIGNED_ABANDON_AUTHORIZATION_MAY_EXIST";
            return false;
        }
        if (reservation->state == BudgetReservationState::RESERVED) {
            if (!ReleaseClientFee(client_fee_ledger, attempt.commit_key,
                                  now, error)) {
                return false;
            }
            client_fee_changed = true;
        }
    }

    if (!batch.TxnBegin()) {
        return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    }
    for (ProviderAttempt& attempt : attempts) {
        if (attempt.state != AttemptState::QUOTE_EXPIRED) {
            attempt.state = AttemptState::REJECTED;
        }
        attempt.updated_at = std::max(attempt.updated_at, now);
        if (!batch.WritePaymasterAttempt(attempt)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    session.state = SessionState::FAILED;
    session.pending_phase = PendingPhase::NONE;
    session.updated_at = std::max(session.updated_at, now);
    if (!batch.WritePaymasterSession(session) ||
        (client_fee_changed &&
         !batch.WritePaymasterClientFeeLedger(client_fee_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        if (!batch.ErasePaymasterReservation(outpoint) ||
            !batch.EraseLockedUTXO(outpoint)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    for (const COutPoint& outpoint : session.user_inputs) {
        m_wallet.UnlockCoin(outpoint);
    }
    return true;
}

bool PaymasterStore::CommitProviderQuote(ProviderAttempt attempt,
                                         const uint256& expected_genesis,
                                         int64_t now,
                                         std::string& error)
{
    return CommitProviderQuote(std::move(attempt), std::nullopt,
                               expected_genesis, now, error);
}

bool PaymasterStore::CheckProviderQuoteAdmission(
    const PaymasterQuoteRequest& request,
    const uint256& request_hash,
    const std::vector<unsigned char>& canonical_netgroup,
    bool requires_carrier,
    DGBSatoshis proposed_network_fee,
    int64_t now,
    uint256& netgroup_bucket,
    std::string& error) const
{
    error.clear();
    netgroup_bucket.SetNull();
    if (request_hash.IsNull() || canonical_netgroup.empty() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_REQUEST_BUDGET_EVENT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != request.intent.provider_id) {
        error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    ProviderSafetyPolicy safety_policy;
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = batch.HasPaymasterProviderSafetyPolicy() ? "PAYMASTER_INVALID_SAFETY_POLICY" : "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    ProviderBudgetLedger budget_ledger;
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger)) {
        error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        return false;
    }
    netgroup_bucket = GetNetgroupBudgetBucket(budget_ledger,
                                              canonical_netgroup);
    if (netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, request.intent.genesis_hash,
            request.intent.provider_id,
            request.intent.request_id, request.intent.session_id,
            request.intent.client_nonce, now, capacity_request,
            capacity_proof, capacity_request_hash, error)) {
        return false;
    }
    if (capacity_proof.snapshot_id.IsNull() ||
        capacity_request_hash != Hash(CanonicalBytes(capacity_request))) {
        error = "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH";
        return false;
    }
    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, request.intent.recipient_script);
    const ProviderSafetyStatus status =
        EvaluateProviderCapacityContinuationPreflight(
            budget_ledger, safety_policy, capacity_request, request,
            request_hash, requires_carrier, recipient_bucket,
            proposed_network_fee, now, netgroup_bucket);
    if (!status.can_accept_quote) {
        error = status.errors.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : status.errors.front();
        return false;
    }
    return true;
}

bool PaymasterStore::CheckProviderRecoveryAdmission(
    const AlternativeRecoveryRequest& request,
    const uint256& request_hash,
    const std::vector<unsigned char>& canonical_netgroup,
    bool requires_carrier,
    DGBSatoshis proposed_network_fee,
    int64_t now,
    uint256& netgroup_bucket,
    std::string& error) const
{
    error.clear();
    netgroup_bucket.SetNull();
    if (request_hash.IsNull() || canonical_netgroup.empty() || now <= 0 ||
        request.wallet_returns.empty() ||
        request_hash != GetAlternativeRecoveryRequestHash(request) ||
        !ValidateAlternativeRecoveryRequestEnvelope(
            request, request.genesis_hash, now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_RECOVERY_REQUEST";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != request.recovery_provider_id) {
        error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = batch.HasPaymasterProviderSafetyPolicy() ? "PAYMASTER_INVALID_SAFETY_POLICY" : "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger)) {
        error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        return false;
    }
    netgroup_bucket = GetNetgroupBudgetBucket(
        budget_ledger, canonical_netgroup);
    if (netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 persisted_capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, request.genesis_hash,
            request.recovery_provider_id,
            request.request_id, request.session_id, request.client_nonce, now,
            capacity_request, capacity_proof,
            persisted_capacity_request_hash, error) ||
        CanonicalBytes(capacity_request) !=
            CanonicalBytes(request.capacity_request) ||
        persisted_capacity_request_hash !=
            Hash(CanonicalBytes(capacity_request)) ||
        capacity_proof.snapshot_id != request.capacity_snapshot_id ||
        capacity_request.funding_model != FundingModel::USER_PAID ||
        capacity_request.requires_carrier != requires_carrier) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        return false;
    }

    const uint256 request_key = GetProviderRequestSlotKey(
        request.recovery_provider_id, request.request_id, request.session_id);
    const uint256 capacity_request_hash{Hash(CanonicalBytes(capacity_request))};
    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, request.wallet_returns.front().script_pub_key);
    const auto admission = std::find_if(
        budget_ledger.capacity_admissions.begin(),
        budget_ledger.capacity_admissions.end(),
        [&](const ProviderCapacityAdmission& candidate) {
            return candidate.request_key == request_key;
        });
    if (admission == budget_ledger.capacity_admissions.end()) {
        error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
        return false;
    }
    const int64_t effective_now = std::max(
        now, budget_ledger.accounting_time_high_water);
    if (request_key.IsNull() || recipient_bucket.IsNull() ||
        admission->request_hash != capacity_request_hash ||
        admission->funding_model != capacity_request.funding_model ||
        admission->requires_carrier != requires_carrier ||
        admission->expires_at != capacity_request.expires_at) {
        error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
        return false;
    }

    ProviderBudgetLedger candidate{budget_ledger};
    if (!BindProviderCapacityQuote(
            candidate, request_key, request_hash, netgroup_bucket,
            effective_now, error)) {
        return false;
    }
    HashWriter simulated_commit = TaggedHash(
        "DigiByte Paymaster Recovery Capacity Preflight v1");
    simulated_commit << request_key << capacity_request_hash << request_hash;
    if (!PromoteProviderCapacityAdmission(
            candidate, request_key, request_hash,
            simulated_commit.GetSHA256(), effective_now, error)) {
        return false;
    }
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        candidate, safety_policy, FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, recipient_bucket, proposed_network_fee,
        effective_now, netgroup_bucket);
    if (!status.can_accept_quote) {
        error = status.errors.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : status.errors.front();
        return false;
    }
    return true;
}

bool PaymasterStore::CommitProviderQuote(
    ProviderAttempt attempt,
    std::optional<SponsorshipAuthorizationRecord> sponsorship,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error)
{
    error.clear();
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    try {
        CDataStream request_stream{attempt.quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
        request_stream >> request;
        SpanReader response_stream{::PROTOCOL_VERSION, attempt.signed_quote};
        response_stream >> response;
        if (!request_stream.empty() || !response_stream.empty()) {
            throw std::ios_base::failure("trailing paymaster quote data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_QUOTE_ENCODING";
        return false;
    }
    CDataStream canonical_request{SER_NETWORK, ::PROTOCOL_VERSION};
    CDataStream canonical_response{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_request << request;
    canonical_response << response;
    const auto request_bytes = MakeUCharSpan(canonical_request);
    const auto response_bytes = MakeUCharSpan(canonical_response);
    if (expected_genesis.IsNull() ||
        request.intent.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (!std::equal(request_bytes.begin(), request_bytes.end(),
                    attempt.quote_request.begin(), attempt.quote_request.end()) ||
        !std::equal(response_bytes.begin(), response_bytes.end(),
                    attempt.signed_quote.begin(), attempt.signed_quote.end()) ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.state != AttemptState::QUOTED || attempt.attempt_id.IsNull() ||
        attempt.created_at <= 0 || attempt.updated_at < attempt.created_at ||
        attempt.quote_expires_at <= attempt.created_at ||
        attempt.retry_until < attempt.quote_expires_at ||
        attempt.session_id != request.intent.session_id ||
        attempt.provider_id != request.intent.provider_id ||
        attempt.client_nonce != request.intent.client_nonce ||
        attempt.intent_hash != GetPaymentIntentHash(request.intent) ||
        response.request_id != request.intent.request_id ||
        response.session_id != request.intent.session_id ||
        response.quote.quote_id != attempt.quote_id ||
        response.quote.unsigned_txid != attempt.unsigned_txid ||
        response.quote.template_commitment != attempt.template_commitment ||
        attempt.provider_netgroup_bucket.IsNull() ||
        !attempt.accepted_client_manifest_id.IsNull() ||
        attempt.client_manifest_accepted_at != 0 ||
        attempt.provider_signed_at != 0 ||
        !attempt.provider_signed_result.empty() ||
        !attempt.capacity_proof_claim_candidate.empty() ||
        !attempt.quote_response_claim_candidate.empty() ||
        !ValidateQuotedTemplate(attempt, error) ||
        !ValidateAttemptAuthorizationManifest(attempt, /*provider_side=*/true,
                                              error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_QUOTE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    ProviderPolicy policy;
    ProviderSettings settings;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != attempt.provider_id ||
        attempt.provider_identity_key != identity.identity_key) {
        error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !ValidateProviderBudgetLedger(budget_ledger, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        }
        return false;
    }
    if (!batch.ReadPaymasterProviderPool(pool) ||
        !ValidateProviderPoolEntries(pool, error)) {
        if (error.empty()) error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }
    if (!batch.ReadPaymasterPolicy(policy) ||
        !ValidateProviderSafetyPolicy(safety_policy, policy, error)) {
        if (error.empty()) error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }
    const int64_t effective_now =
        std::max(now, budget_ledger.accounting_time_high_water);
    if (effective_now <= 0 ||
        !ValidateRedactedQuoteRequestEnvelope(
            request, expected_genesis, effective_now, error) ||
        !ValidateQuoteResponseEnvelope(
            response, expected_genesis, effective_now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_QUOTE";
        return false;
    }

    ProviderAttempt existing;
    const bool have_existing =
        batch.ReadPaymasterAttempt(attempt.attempt_id, existing);
    if (have_existing &&
        CanonicalBytes(existing) != CanonicalBytes(attempt)) {
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, expected_genesis, attempt.provider_id,
            request.intent.request_id, request.intent.session_id,
            request.intent.client_nonce, effective_now, capacity_request,
            capacity_proof, capacity_request_hash, error) ||
        capacity_request.funding_model != request.intent.funding_model ||
        capacity_request.requires_carrier !=
            response.quote.reserved_carrier.has_value()) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        return false;
    }

    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, request.intent.recipient_script);
    const uint256 capacity_admission_key = GetProviderRequestSlotKey(
        attempt.provider_id, request.intent.request_id,
        request.intent.session_id);
    const uint256 quote_request_hash = Hash(attempt.quote_request);
    if (recipient_bucket.IsNull() || capacity_admission_key.IsNull() ||
        quote_request_hash.IsNull()) {
        error = "PAYMASTER_INVALID_RECIPIENT_BUDGET_BUCKET";
        return false;
    }
    const CapacityAdmissionState expected_admission_state =
        have_existing ? CapacityAdmissionState::PROMOTED : CapacityAdmissionState::RESERVED;
    if (!ValidateProviderCapacityAdmissionForCommit(
            budget_ledger, capacity_request, capacity_request_hash,
            quote_request_hash, attempt.commit_key,
            attempt.provider_netgroup_bucket, expected_admission_state,
            effective_now, error) ||
        !ValidateQuoteCapacityResources(
            capacity_proof, response.quote, attempt.provider_manifest, pool,
            request.intent.client_nonce,
            have_existing ? attempt.commit_key : request.intent.client_nonce,
            error)) {
        return false;
    }

    if (have_existing) {
        bool exact_sponsorship =
            existing.sponsorship_capability_hash.IsNull() && !sponsorship;
        if (sponsorship) {
            SponsorshipAuthorizationRecord persisted;
            exact_sponsorship =
                existing.sponsorship_capability_hash ==
                    sponsorship->capability_hash &&
                batch.ReadPaymasterSponsorshipAuthorization(
                    sponsorship->capability_hash, persisted) &&
                CanonicalBytes(persisted) == CanonicalBytes(*sponsorship) &&
                persisted.state == SponsorshipAuthorizationState::RESERVED;
        }
        if (!exact_sponsorship) {
            error = "PAYMASTER_SPONSORSHIP_QUOTE_CONFLICT";
            return false;
        }
        if (!ValidateProviderBudgetState(
                existing, safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true,
                /*allow_legacy_durable_commit=*/false, error)) {
            return false;
        }
        const std::vector<unsigned char> budget_before =
            CanonicalBytes(budget_ledger);
        if (!BindProviderCapacityQuote(
                budget_ledger, capacity_admission_key, quote_request_hash,
                existing.provider_netgroup_bucket, effective_now, error) ||
            !PromoteProviderCapacityAdmission(
                budget_ledger, capacity_admission_key, quote_request_hash,
                existing.commit_key, effective_now, error) ||
            !ReserveProviderBudget(
                budget_ledger, safety_policy, request.intent.funding_model,
                request.intent.sponsorship_scope, existing.commit_key,
                response.quote.network_fee, recipient_bucket, effective_now,
                error, existing.provider_netgroup_bucket) ||
            !ValidateProviderBudgetState(
                existing, safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true,
                /*allow_legacy_durable_commit=*/false, error)) {
            return false;
        }
        if (budget_before != CanonicalBytes(budget_ledger)) {
            error = "PAYMASTER_PROVIDER_BUDGET_ATOMICITY_CONFLICT";
            return false;
        }
        return true;
    }

    if (attempt.provider_manifest.safety_policy_hash !=
        GetProviderSafetyPolicyHash(safety_policy)) {
        error = "PAYMASTER_PROVIDER_SAFETY_POLICY_CONFLICT";
        return false;
    }
    if (!batch.ReadPaymasterSettings(settings) || !settings.enabled ||
        settings.policy_hash != GetProviderPolicyHash(policy) ||
        !ValidatePaymasterQuote(response.quote, request.intent, policy,
                                identity.identity_key,
                                response.quote.service_fee, effective_now,
                                error)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (attempt.commit_key != GetPaymasterCommitKey(
                                  attempt.provider_id, attempt.client_nonce,
                                  attempt.intent_hash, attempt.quote_id,
                                  attempt.template_commitment)) {
        error = "PAYMASTER_INVALID_COMMIT_KEY";
        return false;
    }
    if (!BindProviderCapacityQuote(
            budget_ledger, capacity_admission_key, quote_request_hash,
            attempt.provider_netgroup_bucket, effective_now, error) ||
        !PromoteProviderCapacityAdmission(
            budget_ledger, capacity_admission_key, quote_request_hash,
            attempt.commit_key, effective_now, error) ||
        !ReserveProviderBudget(
            budget_ledger, safety_policy, request.intent.funding_model,
            request.intent.sponsorship_scope, attempt.commit_key,
            response.quote.network_fee, recipient_bucket, effective_now,
            error, attempt.provider_netgroup_bucket) ||
        !ValidateProviderBudgetState(
            attempt, safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false,
            /*allow_legacy_durable_commit=*/false, error)) {
        return false;
    }

    const bool restricted =
        request.intent.funding_model == FundingModel::SPONSORED &&
        request.intent.sponsorship_scope == SponsorshipScope::RESTRICTED;
    if (restricted) {
        std::string validation_error;
        if (!sponsorship || attempt.sponsorship_capability_hash.IsNull() ||
            attempt.sponsorship_capability_hash !=
                request.intent.sponsorship_authorization_hash ||
            sponsorship->state != SponsorshipAuthorizationState::RESERVED ||
            sponsorship->capability_hash !=
                attempt.sponsorship_capability_hash ||
            sponsorship->payment_binding_hash != attempt.intent_hash ||
            sponsorship->reservation_id != attempt.commit_key ||
            sponsorship->reserved_at != now ||
            !ValidateSponsorshipAuthorizationRecord(*sponsorship,
                                                    validation_error)) {
            error = validation_error.empty() ? "PAYMASTER_INVALID_SPONSORSHIP_RESERVATION" : validation_error;
            return false;
        }
        SponsorshipAuthorizationRecord existing_sponsorship;
        if (batch.ReadPaymasterSponsorshipAuthorization(
                sponsorship->capability_hash, existing_sponsorship)) {
            error = "PAYMASTER_SPONSORSHIP_ALREADY_USED";
            return false;
        }
    } else if (sponsorship ||
               !attempt.sponsorship_capability_hash.IsNull() ||
               !request.intent.sponsorship_authorization_hash.IsNull()) {
        error = "PAYMASTER_UNEXPECTED_SPONSORSHIP_RESERVATION";
        return false;
    }

    const uint256 canonical_request_hash = quote_request_hash;
    PaymentSession session;
    if (batch.ReadPaymasterSession(request.intent.request_id, session)) {
        error = session.session_id == request.intent.session_id &&
                        session.canonical_request_hash == canonical_request_hash ?
                    "PAYMASTER_PROVIDER_QUOTE_CONFLICT" :
                    "PAYMASTER_REQUEST_ID_CONFLICT";
        return false;
    }
    IdempotencyTombstone tombstone;
    if (batch.ReadPaymasterTombstone(request.intent.request_id, tombstone)) {
        error = "PAYMASTER_REQUEST_ID_CONFLICT";
        return false;
    }
    std::string indexed_request;
    if (batch.ReadPaymasterSessionId(request.intent.session_id,
                                     indexed_request)) {
        error = "PAYMASTER_SESSION_ID_CONFLICT";
        return false;
    }

    std::set<COutPoint> required;
    for (const VerifiedDGBInput& input : response.quote.reserved_dgb_inputs) {
        required.insert(input.outpoint);
    }
    if (response.quote.reserved_carrier) {
        required.insert(response.quote.reserved_carrier->outpoint);
    }
    size_t rebound{0};
    for (ProviderPoolEntry& entry : pool) {
        if (required.count(entry.outpoint) == 0) continue;
        // ValidateQuoteCapacityResources already proved every resource's role,
        // creating transaction, value, script, state and nonce reservation.
        entry.reservation_id = attempt.commit_key;
        entry.updated_at = effective_now;
        ++rebound;
    }
    if (required.empty() || rebound != required.size()) {
        error = "PAYMASTER_OPERATIONAL_SLOT_MISSING";
        return false;
    }

    session.request_id = request.intent.request_id;
    session.session_id = request.intent.session_id;
    session.canonical_request_hash = canonical_request_hash;
    session.fee_mode_requested = FeeMode::PAYMASTER;
    session.fee_mode_used = FeeMode::PAYMASTER;
    session.state = SessionState::INPUTS_RESERVED;
    session.user_inputs = request.intent.user_dd_inputs;
    session.attempt_ids = {attempt.attempt_id};
    session.created_at = attempt.created_at;
    session.updated_at = effective_now;
    session.provider_side = true;

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterSession(session, false) ||
        !batch.WritePaymasterSessionId(session.session_id,
                                       session.request_id, false) ||
        !batch.WritePaymasterAttempt(attempt, false) ||
        !batch.WritePaymasterTemplate(attempt.template_commitment,
                                      attempt.attempt_id, false) ||
        !batch.WritePaymasterUnsignedTx(attempt.unsigned_txid,
                                        attempt.attempt_id, false) ||
        !batch.WritePaymasterProviderPool(pool) ||
        !batch.WritePaymasterProviderBudgetLedger(budget_ledger) ||
        (sponsorship &&
         !batch.WritePaymasterSponsorshipAuthorization(*sponsorship,
                                                       false))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::UpdateAttempt(const std::string& request_id, const ProviderAttempt& update, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt current;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(update.attempt_id, current) || current.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    const bool final_artifact_known = AttemptHasReached(current.state, AttemptState::FINAL_COMMITTED) ||
                                      !current.final_txid.IsNull() ||
                                      !current.final_transaction.empty() ||
                                      session.state == SessionState::CONFIRMED;
    if (final_artifact_known &&
        (update.state == AttemptState::REJECTED || update.state == AttemptState::AMBIGUOUS)) {
        error = "PAYMASTER_INVALID_ATTEMPT_TRANSITION";
        return false;
    }
    if (update.version != current.version || update.session_id != current.session_id ||
        update.provider_id != current.provider_id || update.attempt_id != current.attempt_id ||
        update.privacy_profile != current.privacy_profile ||
        (current.provider_identity_key.IsFullyValid() &&
         current.provider_identity_key != update.provider_identity_key) ||
        update.created_at != current.created_at || update.updated_at < current.updated_at ||
        !CanTransition(current.state, update.state)) {
        error = "PAYMASTER_INVALID_ATTEMPT_TRANSITION";
        return false;
    }
    if (!current.capacity_proof_claim_candidate.empty()) {
        PaymasterCapacityRequest authoritative_capacity_request;
        if (!ValidatePersistedCapacityRequestAuthority(
                session, current, authoritative_capacity_request, error) ||
            !ValidatePersistedCapacityClaimCandidate(
                current.capacity_proof_claim_candidate,
                authoritative_capacity_request,
                current.provider_identity_key,
                "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
                error)) {
            return false;
        }
    }
    if (!current.quote_response_claim_candidate.empty()) {
        PaymasterQuoteRequest authoritative_quote_request;
        if (!ValidatePersistedQuoteRequestAuthority(
                session, current, authoritative_quote_request, error) ||
            !ValidatePersistedQuoteClaimCandidate(
                session, current, authoritative_quote_request, error)) {
            return false;
        }
    }
    if (current.capacity_request != update.capacity_request ||
        !SameCapacitySnapshot(current.capacity_snapshot, update.capacity_snapshot)) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_CONFLICT";
        return false;
    }
    // Signed claim candidates are written only by the dedicated staging
    // methods. Generic attempt updates may neither manufacture nor erase
    // evidence-bearing remote claims.
    if (current.capacity_proof_claim_candidate !=
            update.capacity_proof_claim_candidate ||
        current.quote_response_claim_candidate !=
            update.quote_response_claim_candidate) {
        error = "PAYMASTER_SIGNED_CLAIM_CANDIDATE_CONFLICT";
        return false;
    }
    // Explicit client approval can only be created by
    // AcceptClientAuthorization(). Generic/stale attempt updates must neither
    // manufacture, replace nor clear it.
    if (current.accepted_client_manifest_id !=
            update.accepted_client_manifest_id ||
        current.client_manifest_accepted_at !=
            update.client_manifest_accepted_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_CONFLICT";
        return false;
    }
    if (session.provider_side &&
        (!current.accepted_client_manifest_id.IsNull() ||
         current.client_manifest_accepted_at != 0)) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_ON_PROVIDER_ATTEMPT";
        return false;
    }
    if (current.accepted_client_manifest_id.IsNull() !=
        (current.client_manifest_accepted_at == 0)) {
        error = "PAYMASTER_INVALID_CLIENT_AUTHORIZATION_ACCEPTANCE";
        return false;
    }
    if (!session.provider_side && current.signed_quote.empty() &&
        !update.signed_quote.empty()) {
        if (current.quote_response_claim_candidate.empty()) {
            error = "PAYMASTER_QUOTE_CLAIM_CANDIDATE_MISSING";
            return false;
        }
        PaymasterQuoteRequest authoritative_request;
        if (!ValidatePersistedQuoteRequestAuthority(
                session, current, authoritative_request, error) ||
            !ValidatePersistedQuoteClaimCandidate(
                session, current, authoritative_request, error)) {
            return false;
        }
        if (current.quote_response_claim_candidate != update.signed_quote) {
            std::string evidence_error;
            if (RecordQuoteEquivocationPairLocked(
                    batch, session, current,
                    current.quote_response_claim_candidate,
                    update.signed_quote, update.updated_at, evidence_error)) {
                error = evidence_error.empty() ? "PAYMASTER_QUOTE_EQUIVOCATION" : std::move(evidence_error);
                return false;
            }
            if (!evidence_error.empty()) {
                error = std::move(evidence_error);
                return false;
            }
            error = "PAYMASTER_QUOTE_CLAIM_CANDIDATE_CONFLICT";
            return false;
        }
    }
    if (!session.provider_side && !current.signed_quote.empty() &&
        !update.signed_quote.empty() && current.signed_quote != update.signed_quote) {
        std::string evidence_error;
        if (RecordQuoteEquivocationLocked(batch, session, current,
                                          update.signed_quote, update.updated_at,
                                          evidence_error)) {
            error = std::move(evidence_error);
            return false;
        }
        if (!evidence_error.empty()) {
            error = std::move(evidence_error);
            return false;
        }
    }

    const auto immutable_hash = [&error](const uint256& old_value, const uint256& new_value, const char* code) {
        if (!old_value.IsNull() && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    const auto immutable_bytes = [&error](const std::vector<unsigned char>& old_value,
                                          const std::vector<unsigned char>& new_value,
                                          const char* code) {
        if (!old_value.empty() && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    const auto immutable_string = [&error](const std::string& old_value,
                                           const std::string& new_value,
                                           const char* code) {
        if (!old_value.empty() && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    const auto immutable_time = [&error](int64_t old_value, int64_t new_value, const char* code) {
        if (old_value != 0 && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    if (!immutable_hash(current.commit_key, update.commit_key, "PAYMASTER_COMMIT_KEY_CONFLICT") ||
        !immutable_hash(current.client_nonce, update.client_nonce, "PAYMASTER_CLIENT_NONCE_CONFLICT") ||
        !immutable_hash(current.intent_hash, update.intent_hash, "PAYMASTER_INTENT_CONFLICT") ||
        !immutable_hash(current.quote_id, update.quote_id, "PAYMASTER_QUOTE_CONFLICT") ||
        !immutable_hash(current.unsigned_txid, update.unsigned_txid, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_hash(current.template_commitment, update.template_commitment, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_hash(current.sponsorship_capability_hash, update.sponsorship_capability_hash,
                        "PAYMASTER_SPONSORSHIP_AUTHORIZATION_CONFLICT") ||
        !immutable_hash(current.client_manifest.manifest_id,
                        update.client_manifest.manifest_id,
                        "PAYMASTER_CLIENT_AUTH_MANIFEST_CONFLICT") ||
        !immutable_hash(current.provider_manifest.manifest_id,
                        update.provider_manifest.manifest_id,
                        "PAYMASTER_PROVIDER_AUTH_MANIFEST_CONFLICT") ||
        current.provider_netgroup_bucket != update.provider_netgroup_bucket ||
        !immutable_hash(current.final_txid, update.final_txid, "PAYMASTER_FINAL_TX_CONFLICT") ||
        !immutable_string(current.provider_endpoint, update.provider_endpoint,
                          "PAYMASTER_PROVIDER_ENDPOINT_CONFLICT") ||
        !immutable_bytes(current.unsigned_intent, update.unsigned_intent, "PAYMASTER_INTENT_CONFLICT") ||
        !immutable_bytes(current.quote_request, update.quote_request, "PAYMASTER_QUOTE_REQUEST_CONFLICT") ||
        !immutable_bytes(current.signed_quote, update.signed_quote, "PAYMASTER_QUOTE_CONFLICT") ||
        !immutable_bytes(current.unsigned_transaction, update.unsigned_transaction, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_bytes(current.unsigned_psbt, update.unsigned_psbt, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_bytes(current.user_signed_psbt, update.user_signed_psbt, "PAYMASTER_USER_PSBT_CONFLICT") ||
        !immutable_bytes(current.final_transaction, update.final_transaction, "PAYMASTER_FINAL_TX_CONFLICT") ||
        !immutable_time(current.provider_signed_at,
                        update.provider_signed_at,
                        "PAYMASTER_PROVIDER_SIGNATURE_TIME_CONFLICT") ||
        !immutable_bytes(current.provider_signed_result,
                         update.provider_signed_result,
                         "PAYMASTER_PROVIDER_SIGNED_RESULT_CONFLICT") ||
        !immutable_time(current.quote_expires_at, update.quote_expires_at, "PAYMASTER_QUOTE_EXPIRY_CONFLICT") ||
        !immutable_time(current.retry_until, update.retry_until, "PAYMASTER_RETRY_EXPIRY_CONFLICT")) {
        if (error.empty()) error = "PAYMASTER_NETGROUP_BUDGET_BINDING_CONFLICT";
        return false;
    }

    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        (update.commit_key.IsNull() || update.client_nonce.IsNull() || update.intent_hash.IsNull() ||
         update.quote_id.IsNull() || update.unsigned_txid.IsNull() || update.template_commitment.IsNull() ||
         update.signed_quote.empty() ||
         update.unsigned_transaction.empty() || update.unsigned_psbt.empty() || update.input_roles.empty() ||
         update.quote_expires_at <= update.created_at ||
         update.retry_until < update.quote_expires_at)) {
        error = "PAYMASTER_INCOMPLETE_QUOTE_ATTEMPT";
        return false;
    }
    if (session.provider_side && AttemptHasReached(update.state, AttemptState::QUOTED) &&
        update.provider_netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUDGET_BINDING_REQUIRED";
        return false;
    }
    if (!session.provider_side && AttemptHasReached(update.state, AttemptState::QUOTED) &&
        (update.capacity_request.empty() || update.capacity_snapshot.snapshot_id.IsNull() ||
         update.capacity_snapshot.request_hash != Hash(update.capacity_request) ||
         update.capacity_snapshot.provider_id != update.provider_id ||
         update.capacity_snapshot.client_nonce != update.client_nonce ||
         update.capacity_snapshot.expires_at <= update.updated_at)) {
        error = "PAYMASTER_CAPACITY_PROOF_REQUIRED";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        update.commit_key != GetPaymasterCommitKey(update.provider_id, update.client_nonce,
                                                   update.intent_hash, update.quote_id,
                                                   update.template_commitment)) {
        error = "PAYMASTER_INVALID_COMMIT_KEY";
        return false;
    }
    if (!current.input_roles.empty() && current.input_roles != update.input_roles) {
        error = "PAYMASTER_TEMPLATE_CONFLICT";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        !ValidateQuotedTemplate(update, error)) return false;
    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        ((session.provider_side && update.provider_manifest.manifest_id.IsNull()) ||
         (!session.provider_side && update.client_manifest.manifest_id.IsNull()) ||
         !ValidateAttemptAuthorizationManifest(update, session.provider_side,
                                               error))) {
        if (error.empty()) error = session.provider_side ? "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED" : "PAYMASTER_CLIENT_AUTH_MANIFEST_REQUIRED";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::USER_SIGNED) && update.user_signed_psbt.empty()) {
        error = "PAYMASTER_USER_PSBT_MISSING";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED) &&
        (update.final_transaction.empty() || update.final_txid.IsNull())) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISSING";
        return false;
    }
    const bool introducing_provider_signature =
        session.provider_side &&
        current.state == AttemptState::USER_PSBT_ACCEPTED &&
        update.state == AttemptState::PROVIDER_SIGNED;
    if (session.provider_side) {
        const bool provider_authority_known =
            AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED);
        if (introducing_provider_signature) {
            PaymasterResult signed_result;
            if (current.provider_signed_at != 0 ||
                !current.provider_signed_result.empty() ||
                update.provider_signed_at != update.updated_at ||
                !DecodeProviderSignedResultEnvelope(
                    update, signed_result, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
                }
                return false;
            }
        } else if (provider_authority_known &&
                   (update.provider_signed_at == 0 ||
                    update.provider_signed_result.empty()) &&
                   !HasExactDurableProviderCommit(batch, current)) {
            // V13 attempts without the signed envelope gain no new authority.
            // Only their already exact, durable ProviderCommit remains
            // recoverable through the legacy path.
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_MISSING";
            return false;
        } else if (provider_authority_known &&
                   update.provider_signed_at != 0 &&
                   !update.provider_signed_result.empty()) {
            PaymasterResult signed_result;
            if (!DecodeProviderSignedResultEnvelope(
                    update, signed_result, error)) {
                return false;
            }
        } else if (!provider_authority_known &&
                   (update.provider_signed_at != 0 ||
                    !update.provider_signed_result.empty())) {
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_PREMATURE";
            return false;
        }
    } else if (current.provider_signed_at != update.provider_signed_at ||
               current.provider_signed_result !=
                   update.provider_signed_result) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_CLIENT_CONFLICT";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::USER_PSBT_ACCEPTED)) {
        UserAuthorizationRecord authorization;
        if (!batch.ReadPaymasterUserAuthorization(update.commit_key, authorization) ||
            authorization.attempt_id != update.attempt_id ||
            authorization.canonical_psbt_hash != Hash(update.user_signed_psbt)) {
            error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
            return false;
        }
    }

    const bool newly_user_signed = !session.provider_side &&
                                   !AttemptHasReached(current.state, AttemptState::USER_SIGNED) &&
                                   AttemptHasReached(update.state, AttemptState::USER_SIGNED);
    if (newly_user_signed) {
        if (current.accepted_client_manifest_id.IsNull() ||
            current.accepted_client_manifest_id !=
                current.client_manifest.manifest_id ||
            current.client_manifest_accepted_at <= 0 ||
            current.client_manifest_accepted_at > update.updated_at) {
            error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
            return false;
        }
        PaymasterQuoteResponse quote_response;
        try {
            SpanReader quote_stream{::PROTOCOL_VERSION, update.signed_quote};
            quote_stream >> quote_response;
            if (!quote_stream.empty()) {
                throw std::ios_base::failure("trailing paymaster quote data");
            }
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_QUOTE_ENCODING";
            return false;
        }
        ClientSafetyPolicy client_policy;
        ClientFeeLedger client_fee_ledger;
        const bool have_client_policy = batch.ReadPaymasterClientSafetyPolicy(client_policy);
        const bool have_client_ledger = batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
        if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
            (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
            have_client_policy != have_client_ledger) {
            error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
            return false;
        }
        if (!have_client_policy) {
            if (quote_response.quote.service_fee.value != 0) {
                error = "PAYMASTER_CLIENT_SAFETY_POLICY_NOT_FOUND";
                return false;
            }
        } else {
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(),
                client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key == update.commit_key;
                });
            if (reservation == client_fee_ledger.reservations.end() ||
                reservation->service_fee != quote_response.quote.service_fee ||
                reservation->state != BudgetReservationState::RESERVED) {
                error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                return false;
            }
        }
    }

    const bool authorization_risk = AttemptHasReached(update.state, AttemptState::USER_SIGNED);
    const bool wallet_authorization_risk = session.provider_side ? AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED) : authorization_risk;
    const uint64_t current_wallet_flags = m_wallet.GetWalletFlags();
    const uint64_t protected_wallet_flags =
        current_wallet_flags | WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    const bool protect_wallet = wallet_authorization_risk &&
                                protected_wallet_flags != current_wallet_flags;
    if (authorization_risk && session.state != SessionState::AUTHORIZED &&
        !HasAuthorizationRisk(session.state)) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }

    uint256 indexed_attempt;
    const bool add_template_index = current.template_commitment.IsNull() &&
                                    !update.template_commitment.IsNull();
    if (add_template_index &&
        batch.ReadPaymasterTemplate(update.template_commitment, indexed_attempt) &&
        indexed_attempt != update.attempt_id) {
        error = "PAYMASTER_TEMPLATE_ALREADY_COMMITTED";
        return false;
    }
    uint256 indexed_tx_attempt;
    const bool add_unsigned_tx_index = current.unsigned_txid.IsNull() && !update.unsigned_txid.IsNull();
    if (add_unsigned_tx_index &&
        batch.ReadPaymasterUnsignedTx(update.unsigned_txid, indexed_tx_attempt) &&
        indexed_tx_attempt != update.attempt_id) {
        error = "PAYMASTER_UNSIGNED_TX_ALREADY_COMMITTED";
        return false;
    }
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAttempt(update)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (add_template_index && indexed_attempt.IsNull() &&
        !batch.WritePaymasterTemplate(update.template_commitment, update.attempt_id, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (add_unsigned_tx_index && indexed_tx_attempt.IsNull() &&
        !batch.WritePaymasterUnsignedTx(update.unsigned_txid, update.attempt_id, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    // Wallet/mempool callbacks can observe the committed transaction while the
    // provider RPC is still advancing its durable attempt. Do not regress an
    // already observed network or terminal session state back to pending.
    if (authorization_risk &&
        (session.state == SessionState::AUTHORIZED ||
         session.state == SessionState::PENDING_PROVIDER)) {
        session.state = SessionState::PENDING_PROVIDER;
        session.pending_phase = AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED) ? PendingPhase::PROVIDER_SIGNED_KNOWN : PendingPhase::USER_SIGNATURE_SENT;
        if (!session.provider_side) {
            for (const auto& outpoint : session.user_inputs) {
                InputReservation reservation;
                if (!batch.ReadPaymasterReservation(outpoint, reservation)) {
                    return Abort(batch, error, "PAYMASTER_RESERVATION_MISSING");
                }
                reservation.authorization_may_exist = true;
                if (!batch.WritePaymasterReservation(reservation)) {
                    return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
                }
            }
        }
    }
    if (protect_wallet && !batch.WriteWalletFlags(protected_wallet_flags)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.updated_at = std::max(session.updated_at, update.updated_at);
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    // The durable flag was committed with the attempt. Publish exactly that
    // value to memory without issuing a second, fallible database write.
    if (protect_wallet && !m_wallet.LoadWalletFlags(protected_wallet_flags)) {
        assert(false);
    }
    return true;
}

bool PaymasterStore::AcceptClientAuthorization(
    const std::string& request_id,
    const uint256& attempt_id,
    const uint256& manifest_id,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || attempt_id.IsNull() ||
        manifest_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CLIENT_AUTHORIZATION_ACCEPTANCE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (session.provider_side) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_ON_PROVIDER_ATTEMPT";
        return false;
    }
    if (attempt.accepted_client_manifest_id.IsNull() !=
        (attempt.client_manifest_accepted_at == 0)) {
        error = "PAYMASTER_INVALID_CLIENT_AUTHORIZATION_ACCEPTANCE";
        return false;
    }
    const bool already_accepted = !attempt.accepted_client_manifest_id.IsNull();
    if (already_accepted) {
        if (attempt.accepted_client_manifest_id != manifest_id ||
            attempt.accepted_client_manifest_id !=
                attempt.client_manifest.manifest_id) {
            error = "PAYMASTER_CLIENT_AUTHORIZATION_CONFLICT";
            return false;
        }
    }
    if (already_accepted && attempt.state != AttemptState::QUOTED) {
        // Once a signature exists, acceptance is immutable and its budget
        // reservation is checked by the signed-state transition itself.
        return true;
    }
    if (!already_accepted && attempt.state != AttemptState::QUOTED) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_AWAITING_ACCEPTANCE";
        return false;
    }
    if (attempt.version != ProviderAttempt::CURRENT_VERSION) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_AWAITING_ACCEPTANCE";
        return false;
    }
    if (attempt.client_manifest.manifest_id.IsNull() ||
        manifest_id != attempt.client_manifest.manifest_id) {
        error = "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH";
        return false;
    }
    if (attempt.client_manifest.canonical_request_hash !=
            session.canonical_request_hash ||
        attempt.client_manifest.requested_fee_mode !=
            session.fee_mode_requested ||
        attempt.client_manifest.privacy_profile != attempt.privacy_profile) {
        error = "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH";
        return false;
    }

    const int64_t effective_now =
        std::max(now, std::max(session.updated_at, attempt.updated_at));
    if (attempt.quote_expires_at <= 0 ||
        attempt.client_manifest.expires_at <= 0 ||
        attempt.capacity_snapshot.expires_at <= 0 ||
        effective_now > attempt.quote_expires_at ||
        effective_now > attempt.client_manifest.expires_at ||
        effective_now > attempt.capacity_snapshot.expires_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }
    if (!ValidateQuotedTemplate(attempt, error) ||
        !ValidateAttemptAuthorizationManifest(
            attempt, /*provider_side=*/false, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_INVALID";
        return false;
    }

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

    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    const bool have_client_policy =
        batch.ReadPaymasterClientSafetyPolicy(client_policy);
    const bool have_client_ledger =
        batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
    if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
        (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
        have_client_policy != have_client_ledger) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }
    bool client_fee_changed{false};
    if (!have_client_policy) {
        if (quote_response.quote.service_fee.value != 0) {
            error = "PAYMASTER_CLIENT_SAFETY_POLICY_NOT_FOUND";
            return false;
        }
    } else {
        const auto existing_fee = std::find_if(
            client_fee_ledger.reservations.begin(),
            client_fee_ledger.reservations.end(),
            [&](const ClientFeeReservation& reservation) {
                return reservation.commit_key == attempt.commit_key;
            });
        const bool exact_reserved_fee =
            existing_fee != client_fee_ledger.reservations.end() &&
            existing_fee->service_fee == quote_response.quote.service_fee &&
            existing_fee->state == BudgetReservationState::RESERVED;
        if (!already_accepted && exact_reserved_fee) {
            // A RESERVED row without the corresponding durable manifest
            // acceptance is an orphaned atomic half, not historical
            // authorization. Never silently repair it into authority.
            error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_ORPHAN";
            return false;
        }
        if (!exact_reserved_fee) {
            if (!ReserveClientFee(client_fee_ledger, client_policy,
                                  attempt.commit_key,
                                  quote_response.quote.service_fee,
                                  effective_now, error)) {
                return false;
            }
            client_fee_changed = true;
        }
    }

    if (already_accepted && !client_fee_changed) return true;

    if (!already_accepted) {
        attempt.accepted_client_manifest_id = manifest_id;
        attempt.client_manifest_accepted_at = effective_now;
        attempt.updated_at = effective_now;
        session.updated_at = effective_now;
    }
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((!already_accepted &&
         (!batch.WritePaymasterAttempt(attempt) ||
          !batch.WritePaymasterSession(session))) ||
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

bool PaymasterStore::GetAttempt(const uint256& attempt_id, ProviderAttempt& attempt) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterAttempt(attempt_id, attempt);
}

bool PaymasterStore::ValidateProviderBudgetAuthorization(
    const ProviderAttempt& attempt,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    bool allow_legacy_durable_commit,
    std::string& error) const
{
    error.clear();
    if (attempt.attempt_id.IsNull() || attempt.commit_key.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_BINDING";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt persisted;
    ProviderPolicy advertised_policy;
    ProviderSafetyPolicy policy;
    ProviderBudgetLedger ledger;
    if (!batch.ReadPaymasterAttempt(attempt.attempt_id, persisted) ||
        persisted.commit_key != attempt.commit_key ||
        persisted.provider_manifest.manifest_id !=
            attempt.provider_manifest.manifest_id) {
        error = "PAYMASTER_PROVIDER_BUDGET_ATTEMPT_CONFLICT";
        return false;
    }
    const bool have_advertised_policy =
        batch.ReadPaymasterPolicy(advertised_policy);
    const bool have_policy =
        batch.ReadPaymasterProviderSafetyPolicy(policy);
    const bool have_ledger =
        batch.ReadPaymasterProviderBudgetLedger(ledger);
    if (!have_advertised_policy) {
        error = "PAYMASTER_PROVIDER_POLICY_NOT_FOUND";
        return false;
    }
    if (!have_policy || !have_ledger) {
        if (allow_legacy_durable_commit &&
            expected_state == BudgetReservationState::SPENT &&
            IsLegacyBudgetlessProviderAttempt(persisted) &&
            HasExactDurableProviderCommit(batch, persisted) &&
            !batch.HasPaymasterProviderSafetyPolicy() &&
            !batch.HasPaymasterProviderBudgetLedger()) {
            return true;
        }
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(policy, advertised_policy, error)) {
        return false;
    }
    if (!ValidateProviderBudgetLedger(ledger, error)) return false;
    const bool reservation_missing = std::none_of(
        ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ProviderBudgetReservation& reservation) {
            return reservation.commit_key == persisted.commit_key;
        });
    if (allow_legacy_durable_commit &&
        expected_state == BudgetReservationState::SPENT &&
        reservation_missing && IsLegacyBudgetlessProviderAttempt(persisted) &&
        HasExactDurableProviderCommit(batch, persisted)) {
        // Safety policy and ledger may be installed after an old exact commit
        // was already durable. Such a commit predates budget rows and cannot
        // be charged retroactively, but it remains recoverable. Current
        // manifests never receive this exception.
        return true;
    }
    return ValidateProviderBudgetState(
        persisted, policy, ledger, expected_state,
        allow_historical_policy, allow_legacy_durable_commit, error);
}

bool PaymasterStore::ValidateProviderPreSignatureAuthorization(
    const ProviderAttempt& attempt,
    const uint256& expected_genesis,
    int64_t now,
    PaymasterCapacityRequest& capacity_request,
    PaymasterCapacityProof& capacity_proof,
    std::string& error) const
{
    error.clear();
    capacity_request = {};
    capacity_proof = {};
    if (expected_genesis.IsNull() || now <= 0 ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.attempt_id.IsNull() || attempt.commit_key.IsNull() ||
        attempt.provider_id.IsNull() || attempt.provider_netgroup_bucket.IsNull() ||
        attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
        attempt.user_signed_psbt.empty() || attempt.created_at <= 0 ||
        now < attempt.created_at || now > attempt.retry_until) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_STATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt persisted;
    if (!batch.ReadPaymasterAttempt(attempt.attempt_id, persisted) ||
        CanonicalBytes(persisted) != CanonicalBytes(attempt)) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_STATE";
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
        identity.provider_id != persisted.provider_id ||
        identity.identity_key != persisted.provider_identity_key ||
        !identity.identity_key.IsFullyValid() ||
        GetPaymasterId(identity.identity_key) != persisted.provider_id) {
        error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(
            safety_policy, advertised_policy, error) ||
        !ValidateProviderBudgetLedger(budget_ledger, error) ||
        !ValidateProviderPoolEntries(pool, error) ||
        !ValidateProviderBudgetState(
            persisted, safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true,
            /*allow_legacy_durable_commit=*/false, error)) {
        return false;
    }

    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    CollaborativePSBTTemplate trusted;
    if (!LoadAttemptAuthorizationArtifacts(
            persisted, request, response, trusted, error) ||
        !ValidateProviderAuthorizationManifest(
            persisted.provider_manifest, request.intent, response.quote,
            trusted, error) ||
        request.intent.genesis_hash != expected_genesis ||
        request.intent.provider_id != identity.provider_id ||
        request.intent.session_id != persisted.session_id ||
        request.intent.client_nonce != persisted.client_nonce ||
        request.intent.policy_hash != settings.policy_hash ||
        response.quote.policy_hash != settings.policy_hash ||
        settings.policy_hash != GetProviderPolicyHash(advertised_policy)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_POLICY_NOT_CURRENT";
        return false;
    }

    PaymentSession session;
    UserAuthorizationRecord authorization;
    if (!batch.ReadPaymasterSession(request.intent.request_id, session) ||
        !session.provider_side || session.session_id != persisted.session_id ||
        session.state != SessionState::PENDING_PROVIDER ||
        session.pending_phase != PendingPhase::USER_SIGNATURE_SENT ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  persisted.attempt_id) == session.attempt_ids.end() ||
        !batch.ReadPaymasterUserAuthorization(persisted.commit_key,
                                              authorization) ||
        authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key != persisted.commit_key ||
        authorization.attempt_id != persisted.attempt_id ||
        authorization.canonical_psbt_hash !=
            Hash(persisted.user_signed_psbt) ||
        authorization.accepted_at <= 0 || authorization.accepted_at > now ||
        authorization.retry_until != persisted.retry_until) {
        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
        return false;
    }

    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, expected_genesis, persisted.provider_id,
            request.intent.request_id, request.intent.session_id,
            request.intent.client_nonce, now, capacity_request,
            capacity_proof, capacity_request_hash, error) ||
        capacity_request.funding_model != request.intent.funding_model ||
        capacity_request.requires_carrier !=
            response.quote.reserved_carrier.has_value() ||
        !ValidateProviderCapacityAdmissionForCommit(
            budget_ledger, capacity_request, capacity_request_hash,
            Hash(persisted.quote_request), persisted.commit_key,
            persisted.provider_netgroup_bucket,
            CapacityAdmissionState::PROMOTED, now, error) ||
        !ValidateQuoteCapacityResources(
            capacity_proof, response.quote, persisted.provider_manifest, pool,
            request.intent.client_nonce, persisted.commit_key, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        capacity_request = {};
        capacity_proof = {};
        return false;
    }
    return true;
}

bool PaymasterStore::GetAttemptByTemplateCommitment(const uint256& template_commitment,
                                                    ProviderAttempt& attempt) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 attempt_id;
    return batch.ReadPaymasterTemplate(template_commitment, attempt_id) &&
           batch.ReadPaymasterAttempt(attempt_id, attempt) &&
           attempt.template_commitment == template_commitment;
}

bool PaymasterStore::GetAttemptByUnsignedTxid(const uint256& unsigned_txid,
                                              ProviderAttempt& attempt) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 attempt_id;
    return batch.ReadPaymasterUnsignedTx(unsigned_txid, attempt_id) &&
           batch.ReadPaymasterAttempt(attempt_id, attempt) &&
           attempt.unsigned_txid == unsigned_txid;
}

bool PaymasterStore::CancelProviderQuote(const uint256& attempt_id,
                                         int64_t now,
                                         ProviderAttempt& attempt,
                                         std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_CANCELLATION";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
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
    if (!attempt.commit_key.IsNull() && have_safety_policy && have_budget_ledger) {
        const auto reservation = std::find_if(
            budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation != budget_ledger.reservations.end()) {
            const BudgetReservationState previous_state = reservation->state;
            if (!ReleaseProviderBudget(budget_ledger, attempt.commit_key, now, error)) {
                return false;
            }
            budget_changed = previous_state != BudgetReservationState::RELEASED;
        } else if (ProviderBudgetReservationRequired(attempt)) {
            error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
            return false;
        }
    }
    if (attempt.state == AttemptState::REJECTED) {
        if (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
            error = "PAYMASTER_DATABASE_WRITE";
            return false;
        }
        return true;
    }
    if (attempt.state != AttemptState::CANDIDATE && attempt.state != AttemptState::QUOTED &&
        attempt.state != AttemptState::QUOTE_EXPIRED) {
        error = "PAYMASTER_QUOTE_AUTHORIZATION_MAY_EXIST";
        return false;
    }
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
        return false;
    }

    std::vector<ProviderPoolEntry> pool;
    const bool have_pool = batch.ReadPaymasterProviderPool(pool);
    bool pool_changed{false};
    size_t released_entries{0};
    if (!attempt.commit_key.IsNull() && have_pool) {
        for (ProviderPoolEntry& entry : pool) {
            if (entry.reservation_id != attempt.commit_key) continue;
            if (entry.state != PoolEntryState::RESERVED) {
                error = "PAYMASTER_POOL_RESERVATION_NOT_CANCELABLE";
                return false;
            }
            entry.state = PoolEntryState::AVAILABLE;
            entry.reservation_id.SetNull();
            entry.updated_at = now;
            pool_changed = true;
            ++released_entries;
        }
    }
    if (attempt.state != AttemptState::CANDIDATE &&
        (attempt.commit_key.IsNull() || released_entries == 0)) {
        error = "PAYMASTER_POOL_RESERVATION_MISSING";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (attempt.state != AttemptState::QUOTE_EXPIRED) attempt.state = AttemptState::REJECTED;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (pool_changed && !batch.WritePaymasterProviderPool(pool)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::ExpireProviderQuotes(int64_t now,
                                          size_t& expired_quotes,
                                          std::string& error)
{
    error.clear();
    expired_quotes = 0;
    if (now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_EXPIRY_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }

    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    const bool have_safety_policy = batch.ReadPaymasterProviderSafetyPolicy(safety_policy);
    const bool have_budget_ledger = batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if ((!have_safety_policy && batch.HasPaymasterProviderSafetyPolicy()) ||
        (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger()) ||
        have_safety_policy != have_budget_ledger) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    const bool have_client_policy =
        batch.ReadPaymasterClientSafetyPolicy(client_policy);
    const bool have_client_ledger =
        batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
    if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
        (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
        have_client_policy != have_client_ledger) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }

    for (PaymentSession& session : sessions) {
        if (!session.provider_side) {
            if ((session.state != SessionState::INPUTS_RESERVED &&
                 session.state != SessionState::AWAITING_WALLET_UNLOCK &&
                 session.state != SessionState::AWAITING_USER_SIGNATURE &&
                 session.state != SessionState::AUTHORIZED) ||
                !session.final_txid.IsNull() ||
                !session.recovery_txid.IsNull()) {
                continue;
            }
            for (const uint256& attempt_id : session.attempt_ids) {
                ProviderAttempt attempt;
                if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                    attempt.session_id != session.session_id) {
                    error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                    return false;
                }
                if (attempt.state != AttemptState::QUOTED ||
                    attempt.quote_expires_at <= 0 ||
                    attempt.quote_expires_at > now ||
                    attempt.commit_key.IsNull() ||
                    !attempt.user_signed_psbt.empty() ||
                    !attempt.final_transaction.empty() ||
                    !attempt.final_txid.IsNull()) {
                    continue;
                }

                bool client_fee_changed{false};
                if (have_client_ledger) {
                    const auto reservation = std::find_if(
                        client_fee_ledger.reservations.begin(),
                        client_fee_ledger.reservations.end(),
                        [&](const ClientFeeReservation& entry) {
                            return entry.commit_key == attempt.commit_key;
                        });
                    if (reservation != client_fee_ledger.reservations.end()) {
                        if (reservation->state ==
                            BudgetReservationState::SPENT) {
                            // A spent client fee is durable evidence that a
                            // final result existed even if another record was
                            // lost. Timeout must never unlock that authority.
                            continue;
                        }
                        if (reservation->state !=
                                BudgetReservationState::RESERVED ||
                            !ReleaseClientFee(client_fee_ledger,
                                              attempt.commit_key, now,
                                              error)) {
                            if (error.empty()) {
                                error =
                                    "PAYMASTER_CLIENT_FEE_RESERVATION_CONFLICT";
                            }
                            return false;
                        }
                        client_fee_changed = true;
                    } else if (!attempt.accepted_client_manifest_id.IsNull()) {
                        // A pre-authorized quote and fee reservation are one
                        // atomic record. Missing either half is corruption.
                        error =
                            "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                        return false;
                    }
                } else if (!attempt.accepted_client_manifest_id.IsNull()) {
                    PaymasterQuoteResponse response;
                    try {
                        SpanReader stream{::PROTOCOL_VERSION,
                                          attempt.signed_quote};
                        stream >> response;
                        if (!stream.empty()) {
                            throw std::ios_base::failure(
                                "trailing paymaster quote data");
                        }
                    } catch (const std::ios_base::failure&) {
                        error = "PAYMASTER_QUOTE_ENCODING";
                        return false;
                    }
                    if (response.quote.service_fee.value != 0) {
                        error =
                            "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                        return false;
                    }
                }

                bool other_live_attempt{false};
                for (const uint256& other_id : session.attempt_ids) {
                    if (other_id == attempt_id) continue;
                    ProviderAttempt other;
                    if (!batch.ReadPaymasterAttempt(other_id, other) ||
                        other.session_id != session.session_id) {
                        error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                        return false;
                    }
                    if (other.state == AttemptState::CANDIDATE ||
                        (other.state == AttemptState::QUOTED &&
                         other.quote_expires_at > now)) {
                        other_live_attempt = true;
                        break;
                    }
                }

                attempt.state = AttemptState::QUOTE_EXPIRED;
                attempt.updated_at = std::max(attempt.updated_at, now);
                session.state = other_live_attempt ? SessionState::INPUTS_RESERVED : SessionState::FAILED;
                session.pending_phase = PendingPhase::NONE;
                session.updated_at = std::max(session.updated_at, now);
                if (!batch.TxnBegin()) {
                    return Abort(batch, error,
                                 "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.WritePaymasterAttempt(attempt) ||
                    !batch.WritePaymasterSession(session) ||
                    (client_fee_changed &&
                     !batch.WritePaymasterClientFeeLedger(
                         client_fee_ledger))) {
                    return Abort(batch, error,
                                 "PAYMASTER_DATABASE_WRITE");
                }
                if (!other_live_attempt) {
                    for (const COutPoint& outpoint : session.user_inputs) {
                        InputReservation reservation;
                        if (!batch.ReadPaymasterReservation(outpoint,
                                                            reservation) ||
                            reservation.session_id != session.session_id ||
                            reservation.role != ReservationRole::USER_DD ||
                            reservation.authorization_may_exist ||
                            !batch.ErasePaymasterReservation(outpoint) ||
                            !batch.EraseLockedUTXO(outpoint)) {
                            return Abort(
                                batch, error,
                                "PAYMASTER_RESERVATION_NOT_EXPIRABLE");
                        }
                    }
                }
                if (!batch.TxnCommit()) {
                    error = "PAYMASTER_DATABASE_COMMIT";
                    return false;
                }
                ++expired_quotes;
                break;
            }
            continue;
        }
        // INPUTS_RESERVED/NONE is the only provider-side state in which no
        // authorization can have been accepted. In particular, AUTHORIZED is
        // treated as unsafe even if a crash preceded the attempt-state write.
        if (!session.provider_side || session.state != SessionState::INPUTS_RESERVED ||
            session.pending_phase != PendingPhase::NONE || !session.final_txid.IsNull()) {
            continue;
        }
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                attempt.session_id != session.session_id) {
                error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                return false;
            }
            if (attempt.state != AttemptState::QUOTED || attempt.quote_expires_at <= 0 ||
                attempt.quote_expires_at > now || attempt.commit_key.IsNull() ||
                !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
                !attempt.final_txid.IsNull()) {
                continue;
            }

            // These records are durable evidence that timing out the quote is
            // no longer safe. A result is conservatively treated the same way,
            // even when it has not yet advanced the attempt after a crash.
            UserAuthorizationRecord authorization;
            ProviderCommitRecord commit;
            PaymasterResult result;
            if (batch.ReadPaymasterUserAuthorization(attempt.commit_key, authorization) ||
                batch.ReadPaymasterProviderCommit(attempt.commit_key, commit) ||
                batch.ReadPaymasterResult(attempt.commit_key, result)) {
                continue;
            }

            std::vector<ProviderPoolEntry> pool;
            if (!batch.ReadPaymasterProviderPool(pool)) {
                error = "PAYMASTER_POOL_RESERVATION_MISSING";
                return false;
            }
            size_t released_entries{0};
            for (ProviderPoolEntry& entry : pool) {
                if (entry.reservation_id != attempt.commit_key) continue;
                if (entry.state != PoolEntryState::RESERVED) {
                    error = "PAYMASTER_POOL_RESERVATION_NOT_EXPIRABLE";
                    return false;
                }
                entry.state = PoolEntryState::AVAILABLE;
                entry.reservation_id.SetNull();
                entry.updated_at = std::max(entry.updated_at, now);
                ++released_entries;
            }
            if (released_entries == 0) {
                error = "PAYMASTER_POOL_RESERVATION_MISSING";
                return false;
            }

            bool budget_changed{false};
            if (have_safety_policy && have_budget_ledger) {
                const auto reservation = std::find_if(
                    budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
                    [&](const ProviderBudgetReservation& entry) {
                        return entry.commit_key == attempt.commit_key;
                    });
                if (reservation == budget_ledger.reservations.end()) {
                    if (ProviderBudgetReservationRequired(attempt)) {
                        error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
                        return false;
                    }
                } else if (reservation->state == BudgetReservationState::SPENT) {
                    // A spent reservation is evidence of a provider commit,
                    // even if another record was lost during local recovery.
                    continue;
                } else {
                    const BudgetReservationState previous_state = reservation->state;
                    if (!ReleaseProviderBudget(budget_ledger, attempt.commit_key, now, error)) {
                        return false;
                    }
                    budget_changed = previous_state != BudgetReservationState::RELEASED;
                }
            }

            attempt.state = AttemptState::QUOTE_EXPIRED;
            attempt.updated_at = std::max(attempt.updated_at, now);
            session.state = SessionState::FAILED;
            session.pending_phase = PendingPhase::NONE;
            session.updated_at = std::max(session.updated_at, now);

            if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
            if (!batch.WritePaymasterAttempt(attempt) ||
                !batch.WritePaymasterSession(session) ||
                !batch.WritePaymasterProviderPool(pool) ||
                (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger))) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (!batch.TxnCommit()) {
                error = "PAYMASTER_DATABASE_COMMIT";
                return false;
            }
            ++expired_quotes;
            break;
        }
    }
    return true;
}

bool PaymasterStore::ExpireProviderCapacityReservations(
    int64_t now,
    size_t& expired_reservations,
    std::string& error)
{
    error.clear();
    expired_reservations = 0;
    if (now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EXPIRY_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<std::pair<uint256, std::vector<unsigned char>>> responses;
    if (!batch.ListPaymasterCapacityResponses(responses)) {
        error = "PAYMASTER_CAPACITY_DATABASE_READ";
        return false;
    }
    if (responses.empty()) return true;

    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_CAPACITY_IDENTITY_MISSING";
        return false;
    }
    ProviderBudgetLedger budget_ledger;
    const bool have_budget_ledger =
        batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger()) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER";
        return false;
    }

    // A provider-side attempt means the payment flow has bound the capacity
    // nonce. Conservatively retain such slots at every attempt state: the
    // pool rebind and attempt transition are atomic in normal operation, but
    // this also fails closed for imported or partially recovered databases.
    std::set<uint256> bound_nonces;
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }
    for (const PaymentSession& session : sessions) {
        if (!session.provider_side) continue;
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                attempt.session_id != session.session_id ||
                attempt.provider_id != identity.provider_id) {
                error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                return false;
            }
            if (!attempt.client_nonce.IsNull()) bound_nonces.insert(attempt.client_nonce);
        }
    }

    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterProviderPool(pool)) {
        error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }
    std::map<COutPoint, size_t> pool_by_outpoint;
    for (size_t index = 0; index < pool.size(); ++index) {
        pool_by_outpoint.emplace(pool[index].outpoint, index);
    }

    for (const auto& [request_hash, response] : responses) {
        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(response, proof) ||
            proof.created_at <= 0 || proof.expires_at <= proof.created_at) {
            error = "PAYMASTER_CAPACITY_RESPONSE_ENCODING";
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
        request.requested_slots = static_cast<uint16_t>(proof.liquidity_slots.size());
        request.created_at = proof.created_at;
        request.expires_at = proof.expires_at;
        std::string validation_error;
        if (GetPersistedCapacityRequestHash(proof) != request_hash ||
            !ValidateCapacityProofEnvelope(proof, request, proof.expires_at - 1,
                                           validation_error) ||
            proof.provider_id != identity.provider_id ||
            !identity.identity_key.VerifySchnorr(GetCapacityProofSignatureHash(proof),
                                                 proof.identity_signature)) {
            error = validation_error.empty() ? "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH" : std::move(validation_error);
            return false;
        }

        uint256 indexed_request_hash;
        if (!batch.ReadPaymasterCapacityNonce(proof.client_nonce,
                                              indexed_request_hash) ||
            indexed_request_hash != request_hash ||
            !batch.ReadPaymasterCapacitySession(GetPersistedCapacitySessionKey(proof),
                                                indexed_request_hash) ||
            indexed_request_hash != request_hash) {
            error = "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH";
            return false;
        }

        ProviderCapacityReleaseRecord existing_release;
        const bool have_release = batch.ReadPaymasterCapacityRelease(
            request_hash, existing_release);
        if (!have_release && batch.HasPaymasterCapacityRelease(request_hash)) {
            error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
            return false;
        }
        const bool replay_retention_elapsed = TimeDeltaExceeds(
            now, proof.expires_at, CAPACITY_REPLAY_RETENTION_SECONDS);
        const uint256 admission_key = proof.version >= 4 ? GetProviderRequestSlotKey(proof.provider_id, proof.request_id,
                                                                                     proof.session_id) :
                                                           uint256{};
        const auto find_admission = [&](ProviderBudgetLedger& ledger) {
            return std::find_if(
                ledger.capacity_admissions.begin(),
                ledger.capacity_admissions.end(),
                [&](const ProviderCapacityAdmission& entry) {
                    return entry.request_key == admission_key;
                });
        };
        if (have_release) {
            if (existing_release.client_nonce != proof.client_nonce) {
                error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
                return false;
            }
            if (std::any_of(pool.begin(), pool.end(), [&](const auto& entry) {
                    return entry.reservation_id == proof.client_nonce;
                })) {
                error = "PAYMASTER_CAPACITY_RELEASE_POOL_CONFLICT";
                return false;
            }
            if (proof.version >= 4) {
                if (!have_budget_ledger) {
                    error = "PAYMASTER_CAPACITY_RELEASE_BUDGET_CONFLICT";
                    return false;
                }
                const auto admission = find_admission(budget_ledger);
                if (admission != budget_ledger.capacity_admissions.end() &&
                    (admission->request_hash != request_hash ||
                     admission->state != CapacityAdmissionState::RELEASED)) {
                    error = "PAYMASTER_CAPACITY_RELEASE_BUDGET_CONFLICT";
                    return false;
                }
                if (admission == budget_ledger.capacity_admissions.end() &&
                    !replay_retention_elapsed) {
                    error = "PAYMASTER_CAPACITY_RELEASE_BUDGET_CONFLICT";
                    return false;
                }
            }
            if (replay_retention_elapsed) {
                ProviderBudgetLedger compacted_ledger{budget_ledger};
                bool compact_budget{false};
                if (proof.version >= 4) {
                    const auto admission = find_admission(compacted_ledger);
                    if (admission != compacted_ledger.capacity_admissions.end()) {
                        compacted_ledger.capacity_admissions.erase(admission);
                        compact_budget = true;
                    }
                }
                if (!batch.TxnBegin()) {
                    return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.ErasePaymasterCapacityResponse(request_hash) ||
                    !batch.ErasePaymasterCapacityNonce(proof.client_nonce) ||
                    !batch.ErasePaymasterCapacitySession(
                        GetPersistedCapacitySessionKey(proof)) ||
                    !batch.ErasePaymasterCapacityRelease(request_hash) ||
                    (compact_budget &&
                     !batch.WritePaymasterProviderBudgetLedger(
                         compacted_ledger))) {
                    return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
                }
                if (!batch.TxnCommit()) {
                    error = "PAYMASTER_DATABASE_COMMIT";
                    return false;
                }
                if (compact_budget) {
                    budget_ledger = std::move(compacted_ledger);
                }
            }
            continue;
        }
        if (bound_nonces.count(proof.client_nonce) != 0) {
            if (proof.expires_at > now || !replay_retention_elapsed) {
                continue;
            }
            ProviderBudgetLedger compacted_ledger{budget_ledger};
            bool compact_budget{false};
            if (proof.version >= 4) {
                if (!have_budget_ledger) {
                    error = "PAYMASTER_CAPACITY_PROMOTION_CONFLICT";
                    return false;
                }
                const auto admission = find_admission(compacted_ledger);
                if (admission != compacted_ledger.capacity_admissions.end()) {
                    if (admission->request_hash != request_hash ||
                        admission->state !=
                            CapacityAdmissionState::PROMOTED) {
                        error = "PAYMASTER_CAPACITY_PROMOTION_CONFLICT";
                        return false;
                    }
                    compacted_ledger.capacity_admissions.erase(admission);
                    compact_budget = true;
                }
            }
            if (!batch.TxnBegin()) {
                return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
            }
            if (!batch.ErasePaymasterCapacityResponse(request_hash) ||
                !batch.ErasePaymasterCapacityNonce(proof.client_nonce) ||
                !batch.ErasePaymasterCapacitySession(
                    GetPersistedCapacitySessionKey(proof)) ||
                (compact_budget &&
                 !batch.WritePaymasterProviderBudgetLedger(
                     compacted_ledger))) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (!batch.TxnCommit()) {
                error = "PAYMASTER_DATABASE_COMMIT";
                return false;
            }
            if (compact_budget) {
                budget_ledger = std::move(compacted_ledger);
            }
            continue;
        }
        if (proof.expires_at > now) continue;

        std::set<COutPoint> proof_outpoints;
        std::vector<size_t> releasable_indexes;
        bool exact_capacity_reservation{true};
        const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
        for (const CapacityDGBInput& input : slot.dgb_inputs) {
            proof_outpoints.insert(input.input.outpoint);
            const auto found = pool_by_outpoint.find(input.input.outpoint);
            if (found == pool_by_outpoint.end()) {
                exact_capacity_reservation = false;
                continue;
            }
            const ProviderPoolEntry& entry = pool[found->second];
            if (entry.state != PoolEntryState::RESERVED ||
                entry.reservation_id != proof.client_nonce ||
                !CapacityProofMatchesPoolEntry(input, entry)) {
                exact_capacity_reservation = false;
                continue;
            }
            releasable_indexes.push_back(found->second);
        }
        if (slot.carrier) {
            proof_outpoints.insert(slot.carrier->carrier.outpoint);
            const auto found = pool_by_outpoint.find(slot.carrier->carrier.outpoint);
            if (found == pool_by_outpoint.end()) {
                exact_capacity_reservation = false;
            } else {
                const ProviderPoolEntry& entry = pool[found->second];
                if (entry.state != PoolEntryState::RESERVED ||
                    entry.reservation_id != proof.client_nonce ||
                    !CapacityProofMatchesPoolEntry(*slot.carrier, entry)) {
                    exact_capacity_reservation = false;
                } else {
                    releasable_indexes.push_back(found->second);
                }
            }
        }
        for (const ProviderPoolEntry& entry : pool) {
            if (entry.reservation_id == proof.client_nonce &&
                proof_outpoints.count(entry.outpoint) == 0) {
                error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
                return false;
            }
        }
        if (!exact_capacity_reservation ||
            releasable_indexes.size() != proof_outpoints.size()) {
            // A quote commit rebinds these exact entries to its commit key.
            // Missing, committed, spent, or otherwise changed entries are
            // likewise never made available by an expiry heuristic.
            continue;
        }

        std::vector<ProviderPoolEntry> updated_pool{pool};
        for (const size_t index : releasable_indexes) {
            updated_pool[index].state = PoolEntryState::AVAILABLE;
            updated_pool[index].reservation_id.SetNull();
            updated_pool[index].updated_at = std::max(updated_pool[index].updated_at, now);
        }
        ProviderCapacityReleaseRecord release;
        release.request_hash = request_hash;
        release.client_nonce = proof.client_nonce;
        release.released_at = now;

        if (proof.version >= 4) {
            if (!have_budget_ledger ||
                !ReleaseProviderCapacityAdmission(
                    budget_ledger,
                    GetProviderRequestSlotKey(proof.provider_id,
                                              proof.request_id,
                                              proof.session_id),
                    request_hash, now, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
                }
                return false;
            }
        }

        if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
        if (!batch.WritePaymasterProviderPool(updated_pool) ||
            !batch.WritePaymasterCapacityRelease(release, false) ||
            (proof.version >= 4 &&
             !batch.WritePaymasterProviderBudgetLedger(budget_ledger))) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
        pool = std::move(updated_pool);
        ++expired_reservations;
    }
    return true;
}

bool PaymasterStore::CompactClientCapacitySnapshots(
    int64_t now,
    size_t& compacted_snapshots,
    std::string& error)
{
    error.clear();
    compacted_snapshots = 0;
    if (now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_COMPACTION_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<ValidatedCapacitySnapshot> snapshots;
    if (!batch.ListPaymasterCapacitySnapshots(snapshots)) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_DATABASE_READ";
        return false;
    }
    for (const ValidatedCapacitySnapshot& snapshot : snapshots) {
        if (snapshot.expires_at <= 0 ||
            !TimeDeltaExceeds(now, snapshot.expires_at,
                              CAPACITY_REPLAY_RETENTION_SECONDS)) {
            continue;
        }

        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(snapshot.capacity_proof, proof) ||
            proof.snapshot_id != snapshot.snapshot_id ||
            proof.provider_id != snapshot.provider_id ||
            proof.session_id != snapshot.session_id ||
            proof.client_nonce != snapshot.client_nonce ||
            proof.funding_model != snapshot.funding_model ||
            proof.requires_carrier != snapshot.requires_carrier ||
            proof.created_at != snapshot.created_at ||
            proof.expires_at != snapshot.expires_at ||
            GetCapacityResourceCommitment(proof) !=
                snapshot.resource_commitment) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
            return false;
        }

        bool erase_slot{false};
        uint256 indexed_snapshot_id;
        if (batch.ReadPaymasterCapacitySlot(
                snapshot.resource_commitment, indexed_snapshot_id) &&
            indexed_snapshot_id == snapshot.snapshot_id) {
            erase_slot = true;
        }

        std::set<COutPoint> proof_outpoints;
        for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
            if (slot.carrier) {
                proof_outpoints.insert(slot.carrier->carrier.outpoint);
            }
            for (const CapacityDGBInput& input : slot.dgb_inputs) {
                proof_outpoints.insert(input.input.outpoint);
            }
        }
        if (proof_outpoints.empty()) {
            error = "PAYMASTER_INVALID_CAPACITY_RESOURCE_BINDING";
            return false;
        }
        std::vector<COutPoint> erase_resources;
        for (const COutPoint& outpoint : proof_outpoints) {
            CapacityResourceBinding binding;
            if (batch.ReadPaymasterCapacityResource(
                    snapshot.provider_id, outpoint, binding) &&
                binding.snapshot_id == snapshot.snapshot_id) {
                if (binding.resource_commitment !=
                        snapshot.resource_commitment ||
                    binding.session_id != snapshot.session_id ||
                    binding.attempt_id != snapshot.attempt_id ||
                    binding.proof_hash != Hash(snapshot.capacity_proof)) {
                    error = "PAYMASTER_CAPACITY_RESOURCE_BINDING_CONFLICT";
                    return false;
                }
                erase_resources.push_back(outpoint);
            }
        }

        if (!batch.TxnBegin()) {
            return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
        }
        if ((erase_slot &&
             !batch.ErasePaymasterCapacitySlot(
                 snapshot.resource_commitment)) ||
            !batch.ErasePaymasterCapacitySnapshot(snapshot.snapshot_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        for (const COutPoint& outpoint : erase_resources) {
            if (!batch.ErasePaymasterCapacityResource(
                    snapshot.provider_id, outpoint)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
        ++compacted_snapshots;
    }
    return true;
}

bool PaymasterStore::RecordQuoteEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& conflicting_signed_quote,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || conflicting_signed_quote.empty() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
            session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymasterQuoteRequest authoritative_request;
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    PaymasterQuoteResponse persisted_response;
    std::string validation_error;
    if (!DecodeCanonicalQuoteResponse(attempt.signed_quote,
                                      persisted_response) ||
        persisted_response.request_id != session.request_id ||
        persisted_response.session_id != session.session_id ||
        persisted_response.quote.provider_id != attempt.provider_id ||
        persisted_response.quote.intent_hash != attempt.intent_hash ||
        !ValidateQuoteResponseEnvelope(
            persisted_response,
            authoritative_request.intent.genesis_hash,
            persisted_response.quote.created_at, validation_error) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(persisted_response.quote),
            persisted_response.quote.identity_signature)) {
        error = "PAYMASTER_PERSISTED_SIGNED_QUOTE_CORRUPT";
        return false;
    }
    if (!RecordQuoteEquivocationLocked(batch, session, attempt,
                                       conflicting_signed_quote, now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::RecordPendingQuoteEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& first_signed_quote,
    const std::vector<unsigned char>& conflicting_signed_quote,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || first_signed_quote.empty() ||
        conflicting_signed_quote.empty() || observed_at <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || session.request_id != request_id ||
        attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !attempt.signed_quote.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymasterQuoteRequest authoritative_request;
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (!RecordQuoteEquivocationPairLocked(
            batch, session, attempt, first_signed_quote,
            conflicting_signed_quote, observed_at, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::StageQuoteResponseClaimCandidate(
    const uint256& attempt_id,
    const std::vector<unsigned char>& signed_quote,
    int64_t observed_at,
    bool& equivocation,
    std::string& error)
{
    error.clear();
    equivocation = false;
    if (attempt_id.IsNull() || signed_quote.empty() ||
        signed_quote.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    PaymentSession session;
    std::string request_id;
    PaymasterQuoteResponse response;
    PaymasterQuoteRequest authoritative_request;
    std::string validation_error;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !attempt.signed_quote.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (!attempt.quote_response_claim_candidate.empty() &&
        !ValidatePersistedQuoteClaimCandidate(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (!DecodeCanonicalQuoteResponse(signed_quote, response) ||
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
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != attempt.provider_id ||
        !ValidateQuoteResponseEnvelope(
            response, authoritative_request.intent.genesis_hash,
            response.quote.created_at,
            validation_error) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(response.quote),
            response.quote.identity_signature)) {
        error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
        return false;
    }

    if (!attempt.quote_response_claim_candidate.empty()) {
        if (attempt.quote_response_claim_candidate == signed_quote) {
            return true;
        }
        if (!RecordQuoteEquivocationPairLocked(
                batch, session, attempt,
                attempt.quote_response_claim_candidate, signed_quote,
                observed_at, error)) {
            if (error.empty()) {
                error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
            }
            return false;
        }
        equivocation = true;
        error.clear();
        return true;
    }

    if (!ValidateQuoteResponseEnvelope(
            response, authoritative_request.intent.genesis_hash, observed_at,
            validation_error)) {
        error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
        return false;
    }

    attempt.version = ProviderAttempt::CURRENT_VERSION;
    attempt.quote_response_claim_candidate = signed_quote;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RejectUnavailableProviderSubmit(
    const uint256& attempt_id,
    const PaymasterResult& result,
    const uint256& expected_genesis,
    ProviderAttempt& attempt,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull()) {
        error = "PAYMASTER_INVALID_SUBMIT_REJECTION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterIdentity(identity) ||
        attempt.provider_id != identity.provider_id ||
        attempt.provider_identity_key != identity.identity_key ||
        attempt.commit_key.IsNull()) {
        error = "PAYMASTER_SUBMIT_REJECTION_BINDING_MISSING";
        return false;
    }
    if (!ValidatePaymasterResult(result, expected_genesis, attempt.provider_id,
                                 attempt.commit_key, identity.identity_key, 1, error)) {
        return false;
    }
    if (result.status != PaymasterResultStatus::REJECTED || result.txid ||
        result.raw_transaction_hash || result.final_transaction) {
        error = "PAYMASTER_INVALID_SUBMIT_REJECTION";
        return false;
    }

    PaymasterResult existing;
    if (batch.ReadPaymasterResult(result.commit_key, existing)) {
        if (attempt.state == AttemptState::REJECTED &&
            result.result_sequence == existing.result_sequence &&
            GetPaymasterResultSignatureHash(result) == GetPaymasterResultSignatureHash(existing) &&
            result.identity_signature == existing.identity_signature) {
            return true;
        }
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    ProviderCommitRecord commit;
    if (batch.ReadPaymasterProviderCommit(attempt.commit_key, commit) ||
        (attempt.state != AttemptState::QUOTED &&
         attempt.state != AttemptState::USER_SIGNED &&
         attempt.state != AttemptState::USER_PSBT_ACCEPTED)) {
        error = "PAYMASTER_PROVIDER_SIGNATURE_MAY_EXIST";
        return false;
    }

    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterProviderPool(pool)) {
        error = "PAYMASTER_POOL_RESERVATION_MISSING";
        return false;
    }
    size_t released_entries{0};
    for (ProviderPoolEntry& entry : pool) {
        if (entry.reservation_id != attempt.commit_key) continue;
        if (entry.state != PoolEntryState::RESERVED) {
            error = "PAYMASTER_POOL_RESERVATION_NOT_REJECTABLE";
            return false;
        }
        entry.state = PoolEntryState::AVAILABLE;
        entry.reservation_id.SetNull();
        entry.updated_at = result.updated_at;
        ++released_entries;
    }
    if (released_entries == 0) {
        error = "PAYMASTER_POOL_RESERVATION_MISSING";
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
    if (have_safety_policy && have_budget_ledger) {
        const auto reservation = std::find_if(
            budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation != budget_ledger.reservations.end()) {
            const BudgetReservationState previous_state = reservation->state;
            if (!ReleaseProviderBudget(budget_ledger, attempt.commit_key,
                                       result.updated_at, error)) {
                return false;
            }
            budget_changed = previous_state != BudgetReservationState::RELEASED;
        } else if (ProviderBudgetReservationRequired(attempt)) {
            error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
            return false;
        }
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    attempt.state = AttemptState::REJECTED;
    attempt.updated_at = std::max(attempt.updated_at, result.updated_at);
    if (!batch.WritePaymasterAttempt(attempt) ||
        !batch.WritePaymasterProviderPool(pool) ||
        !batch.WritePaymasterResult(result) ||
        (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

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
            /*allow_historical_policy=*/true,
            /*allow_legacy_durable_commit=*/false, error)) {
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
    const auto spend_budget = [&](int64_t committed_at,
                                  bool allow_legacy_durable_commit) {
        if (!have_safety_policy || !have_budget_ledger) {
            if (allow_legacy_durable_commit &&
                !attempt.final_txid.IsNull() &&
                !attempt.final_transaction.empty() &&
                IsLegacyBudgetlessProviderAttempt(attempt)) {
                return true;
            }
            error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
            return false;
        }
        const auto reservation = std::find_if(
            budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation == budget_ledger.reservations.end()) {
            if (allow_legacy_durable_commit &&
                IsLegacyBudgetlessProviderAttempt(attempt)) return true;
            error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
            return false;
        }
        if ((reservation->state != BudgetReservationState::RESERVED &&
             reservation->state != BudgetReservationState::SPENT) ||
            !ValidateProviderBudgetState(
                attempt, safety_policy, budget_ledger, reservation->state,
                /*allow_historical_policy=*/true,
                allow_legacy_durable_commit, error)) {
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
    const bool legacy_durable_result =
        have_existing_commit && attempt.provider_signed_at == 0 &&
        attempt.provider_signed_result.empty() &&
        result.updated_at == existing.committed_at;
    if (!ValidatePaymasterResult(result, expected_genesis, commit.provider_id,
                                 commit.commit_key, identity.identity_key,
                                 1, error) ||
        result.result_sequence != 1 ||
        result.status != PaymasterResultStatus::FINAL_COMMITTED ||
        (!exact_signed_envelope && !legacy_durable_result) || !result.txid ||
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
    // budget is spent and the final transaction becomes durable. An exact
    // manifest-less legacy commit may be replayed, but can never enter here as
    // a newly authorized signature.
    if ((!have_existing_commit &&
         attempt.provider_manifest.manifest_id.IsNull()) ||
        (!attempt.provider_manifest.manifest_id.IsNull() &&
         !ValidateProviderAuthorizationOwnership(
             m_wallet, attempt.provider_manifest, error))) {
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
    const bool have_provider_pool = batch.ReadPaymasterProviderPool(pool_entries);
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
        if (!spend_budget(existing.committed_at,
                          /*allow_legacy_durable_commit=*/true)) return false;
        bool pool_changed{false};
        if (have_provider_pool && !RegisterProviderPoolSuccessors(
                attempt, existing, transaction, successor_policy,
                pool_entries, pool_changed, error)) {
            return false;
        }
        if (have_existing_result && !budget_changed && !pool_changed &&
            !finance_changed) return true;

        // Repair a legacy crash window (commit durable before the initial
        // result) and any legacy budget transition as one database unit.
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

    if (have_provider_pool) {
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
    if (!spend_budget(commit.committed_at,
                      /*allow_legacy_durable_commit=*/false)) return false;

    bool pool_changed{false};
    if (have_provider_pool && !RegisterProviderPoolSuccessors(
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
    if (have_provider_pool) {
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
    if (!batch.ReadPaymasterProviderPool(pool_entries)) return true;

    std::vector<ProviderCommitRecord> commits;
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterProviderCommits(commits) ||
        !batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_POOL_RECONCILIATION_READ_FAILED";
        return false;
    }
    ProviderPolicy provider_policy;
    const std::optional<ProviderPolicy> successor_policy =
        batch.ReadPaymasterPolicy(provider_policy)
            ? std::optional<ProviderPolicy>{provider_policy}
            : std::nullopt;

    std::map<uint256, ProviderAttempt> attempts_by_commit;
    for (const PaymentSession& session : sessions) {
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                attempt.commit_key.IsNull() ||
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
            if (!batch.ReadPaymasterProviderCommit(
                    attempt.commit_key, commit)) {
                attempts.push_back(std::move(attempt));
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
        IsSupportedClientAuthorizationManifestVersion(
            attempt.client_manifest.version) &&
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
        // A result received now is new remote input, even when it describes
        // the same unsigned txid as an old attempt. Only an explicitly
        // accepted current manifest may authorize state progression or create
        // client-side authorization evidence. Already durable legacy finals
        // are handled by the separate exact-artifact recovery path.
        if (!has_current_accepted_client_manifest) {
            const bool exact_durable_legacy_binding =
                exact_replay && result.txid && result.raw_transaction_hash &&
                result.final_transaction &&
                session.final_txid == *result.txid &&
                attempt.final_txid == *result.txid &&
                attempt.final_transaction == final_transaction;
            if (exact_durable_legacy_binding) {
                CMutableTransaction recovered_final;
                if (!ValidatePersistedLegacyClientFinalForRecovery(
                        m_wallet, attempt, existing, expected_genesis,
                        recovered_final, error)) {
                    return false;
                }
                if (CTransaction{recovered_final} !=
                    CTransaction{*result.final_transaction}) {
                    error = "PAYMASTER_LEGACY_CLIENT_FINAL_RESULT_MISMATCH";
                    return false;
                }
                // Recovery of an exact durable legacy result is deliberately
                // read-only. In particular it must not manufacture a current
                // manifest acceptance or UserAuthorizationRecord.
                return true;
            }
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
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
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
    bool replace_capacity_slot{false};
    std::vector<CapacityResourceBinding> capacity_resource_bindings;
    std::vector<bool> replace_capacity_resources;
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

        if (current.capacity_proof_claim_candidate !=
            snapshot.capacity_proof) {
            std::string evidence_error;
            const std::vector<unsigned char> request_bytes =
                CanonicalBytes(recovery.capacity_request);
            if (RecordCapacityEquivocationLocked(
                    batch, recovery.capacity_request,
                    recovery.recovery_provider_identity_key,
                    current.capacity_proof_claim_candidate,
                    snapshot.capacity_proof, Hash(request_bytes),
                    snapshot.validated_at, evidence_error)) {
                error = evidence_error.empty() ? "PAYMASTER_CAPACITY_EQUIVOCATION" : std::move(evidence_error);
                return false;
            }
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_CLAIM_CANDIDATE_CONFLICT" : std::move(evidence_error);
            return false;
        }

        ValidatedCapacitySnapshot by_id;
        if (batch.ReadPaymasterCapacitySnapshot(snapshot.snapshot_id, by_id) &&
            !SameCapacitySnapshot(by_id, snapshot)) {
            std::string evidence_error;
            if (by_id.provider_id == snapshot.provider_id &&
                by_id.request_hash == snapshot.request_hash &&
                RecordCapacityEquivocationLocked(
                    batch, recovery.capacity_request,
                    recovery.recovery_provider_identity_key,
                    by_id.capacity_proof, snapshot.capacity_proof,
                    snapshot.request_hash, snapshot.validated_at,
                    evidence_error)) {
                error = std::move(evidence_error);
                return false;
            }
            error = evidence_error.empty() ? "PAYMASTER_CAPACITY_EQUIVOCATION" : std::move(evidence_error);
            return false;
        }
        if (!BuildCapacityResourceBindings(
                snapshot, proof, capacity_resource_bindings, error)) {
            return false;
        }

        // Cross-check the historical aggregate index as well as the direct
        // per-outpoint index. This keeps wallets created before the resource
        // index fail-closed and produces signed equivocation evidence when a
        // provider promises a live slot to conflicting sessions.
        std::vector<ValidatedCapacitySnapshot> persisted_snapshots;
        if (!batch.ListPaymasterCapacitySnapshots(persisted_snapshots)) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_DATABASE_READ";
            return false;
        }
        for (const ValidatedCapacitySnapshot& existing :
             persisted_snapshots) {
            if (existing.snapshot_id == snapshot.snapshot_id ||
                existing.provider_id != snapshot.provider_id ||
                existing.expires_at <= snapshot.validated_at) {
                continue;
            }
            PaymasterCapacityProof existing_proof;
            if (!DecodeCanonicalCapacityProof(existing.capacity_proof,
                                              existing_proof)) {
                error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
                return false;
            }
            for (const CapacityResourceBinding& binding :
                 capacity_resource_bindings) {
                if (!CapacityProofContainsOutpoint(existing_proof,
                                                   binding.outpoint)) {
                    continue;
                }
                std::string evidence_error;
                if (RecordCapacityResourceEquivocationLocked(
                        batch, snapshot.provider_id,
                        recovery.recovery_provider_identity_key,
                        binding.outpoint, existing.capacity_proof,
                        snapshot.capacity_proof, snapshot.validated_at,
                        evidence_error)) {
                    error = std::move(evidence_error);
                    return false;
                }
                error = evidence_error.empty() ? "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND" : std::move(evidence_error);
                return false;
            }
        }

        replace_capacity_resources.reserve(
            capacity_resource_bindings.size());
        for (const CapacityResourceBinding& binding :
             capacity_resource_bindings) {
            CapacityResourceBinding existing;
            const bool have_existing =
                batch.ReadPaymasterCapacityResource(
                    binding.provider_id, binding.outpoint, existing);
            if (!have_existing) {
                replace_capacity_resources.push_back(false);
                continue;
            }
            if (SameCapacityResourceBinding(existing, binding)) {
                replace_capacity_resources.push_back(true);
                continue;
            }
            if (existing.expires_at > snapshot.validated_at) {
                ValidatedCapacitySnapshot existing_snapshot;
                std::string evidence_error;
                if (batch.ReadPaymasterCapacitySnapshot(
                        existing.snapshot_id, existing_snapshot) &&
                    RecordCapacityResourceEquivocationLocked(
                        batch, snapshot.provider_id,
                        recovery.recovery_provider_identity_key,
                        binding.outpoint,
                        existing_snapshot.capacity_proof,
                        snapshot.capacity_proof, snapshot.validated_at,
                        evidence_error)) {
                    error = std::move(evidence_error);
                    return false;
                }
                error = evidence_error.empty() ? "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND" : std::move(evidence_error);
                return false;
            }
            replace_capacity_resources.push_back(true);
        }

        uint256 bound_snapshot_id;
        if (batch.ReadPaymasterCapacitySlot(
                snapshot.resource_commitment, bound_snapshot_id) &&
            bound_snapshot_id != snapshot.snapshot_id) {
            ValidatedCapacitySnapshot existing;
            if (!batch.ReadPaymasterCapacitySnapshot(bound_snapshot_id,
                                                     existing) ||
                existing.expires_at > snapshot.validated_at) {
                error = "PAYMASTER_CAPACITY_RESOURCE_ALREADY_BOUND";
                return false;
            }
            replace_capacity_slot = true;
        }
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
                /*allow_historical_policy=*/false,
                /*allow_legacy_authorized_recovery=*/false, error)) {
            if (error.empty()) {
                error =
                    "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH";
            }
            return false;
        }
    }

    if (client_fee_changed || capacity_snapshot_changed) {
        if (!batch.TxnBegin()) {
            error = "PAYMASTER_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterAlternativeRecovery(recovery, true) ||
            (client_fee_changed &&
             !batch.WritePaymasterClientFeeLedger(client_fee_ledger)) ||
            (capacity_snapshot_changed &&
             (!batch.WritePaymasterCapacitySnapshot(
                  recovery.capacity_snapshot, false) ||
              !batch.WritePaymasterCapacitySlot(
                  recovery.capacity_snapshot.resource_commitment,
                  recovery.capacity_snapshot.snapshot_id,
                  replace_capacity_slot)))) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        for (size_t index = 0;
             index < capacity_resource_bindings.size(); ++index) {
            if (!batch.WritePaymasterCapacityResource(
                    capacity_resource_bindings[index],
                    replace_capacity_resources[index])) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
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
    const auto existing_fee = std::find_if(
        client_fee_ledger.reservations.begin(),
        client_fee_ledger.reservations.end(),
        [&](const ClientFeeReservation& reservation) {
            return reservation.commit_key ==
                   recovery.recovery_response.recovery_commit_key;
        });
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
    bool client_fee_changed{false};
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
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 recovery_id;
    return batch.ReadPaymasterAlternativeRecoveryRequest(request_id, recovery_id) &&
           batch.ReadPaymasterAlternativeRecovery(recovery_id, recovery) &&
           !recovery.provider_side && recovery.request_id == request_id;
}

bool PaymasterStore::GetAlternativeRecoveryById(
    const uint256& recovery_id,
    AlternativeRecoveryRecord& recovery) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterAlternativeRecovery(
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
    std::vector<AlternativeRecoveryRecord> recoveries;
    if (!batch.ListPaymasterAlternativeRecoveries(recoveries)) {
        error = "PAYMASTER_RECOVERY_DATABASE_READ";
        return false;
    }
    bool found{false};
    for (const AlternativeRecoveryRecord& candidate : recoveries) {
        if (!candidate.provider_side ||
            candidate.phase < AlternativeRecoveryPhase::RESPONSE_VALIDATED ||
            candidate.recovery_response.recovery_commit_key !=
                commit.commit_key) {
            continue;
        }
        if (found) {
            error = "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT";
            recovery = {};
            return false;
        }
        CMutableTransaction transaction;
        std::vector<COutPoint> provider_inputs;
        if (!ValidateProviderAlternativeRecoveryCommitBinding(
                candidate, commit, transaction, provider_inputs, error)) {
            recovery = {};
            return false;
        }
        recovery = candidate;
        found = true;
    }
    if (!found) {
        error = "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND";
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
    std::vector<std::pair<uint256, std::vector<unsigned char>>> responses;
    if (!batch.ListPaymasterCapacityResponses(responses)) {
        error = "PAYMASTER_CAPACITY_DATABASE_READ";
        return false;
    }
    if (responses.empty()) return true;

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
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !ValidateProviderBudgetLedger(budget_ledger, error)) {
        if (error.empty()) {
            error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        }
        return false;
    }
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

    for (const auto& [request_hash, response] : responses) {
        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(response, proof)) {
            error = "PAYMASTER_CAPACITY_RESPONSE_ENCODING";
            return false;
        }
        // Legacy proofs have no persistent ProviderCapacityAdmission and
        // therefore cannot grant new post-restart continuation authority.
        if (proof.version < 4) continue;

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
        const auto admission = std::find_if(
            budget_ledger.capacity_admissions.begin(),
            budget_ledger.capacity_admissions.end(),
            [&](const ProviderCapacityAdmission& candidate) {
                return candidate.request_key == admission_key;
            });
        if (admission == budget_ledger.capacity_admissions.end()) {
            // The response and its indexes are durable replay barriers, but a
            // capacity proof without its atomic budget admission grants no
            // continuation authority. This can remain after upgrading a
            // wallet written at an older crash boundary. Ignore it when
            // deciding whether the provider has work to drain so that a
            // stopped automatic provider may still start solely to restore
            // liquidity. The proof is deliberately not erased here: normal
            // expiry/compaction retains the replay barrier for its full
            // retention window.
            continue;
        }
        if (admission->request_hash != request_hash ||
            admission->funding_model != proof.funding_model ||
            admission->requires_carrier != proof.requires_carrier ||
            admission->expires_at != proof.expires_at) {
            error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
            return false;
        }

        ProviderCapacityReleaseRecord release;
        const bool have_release =
            batch.ReadPaymasterCapacityRelease(request_hash, release);
        if (!have_release && batch.HasPaymasterCapacityRelease(request_hash)) {
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
    std::vector<AlternativeRecoveryRecord> recoveries;
    if (!batch.ListPaymasterAlternativeRecoveries(recoveries)) {
        error = "PAYMASTER_RECOVERY_DATABASE_READ";
        return false;
    }
    std::vector<AlternativeRecoveryRecord> expiring;
    for (AlternativeRecoveryRecord& recovery : recoveries) {
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
    if (expiring.empty()) return true;

    std::vector<ProviderPoolEntry> pool;
    ProviderBudgetLedger provider_ledger;
    const bool expire_provider = std::any_of(
        expiring.begin(), expiring.end(),
        [](const AlternativeRecoveryRecord& recovery) {
            return recovery.provider_side;
        });
    if (expire_provider &&
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
    if (expire_client_fee &&
        !batch.ReadPaymasterClientFeeLedger(client_fee_ledger)) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
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

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    for (const AlternativeRecoveryRecord& recovery : expiring) {
        if (!batch.WritePaymasterAlternativeRecovery(recovery, true) ||
            (!recovery.provider_side &&
             !batch.ErasePaymasterAlternativeRecoveryRequest(
                 recovery.request_id))) {
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
    const bool have_existing =
        batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id,
                                               existing);
    if (have_existing &&
        !SameAlternativeRecoveryRecord(existing, recovery)) {
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
                /*allow_historical_policy=*/true,
                /*allow_legacy_authorized_recovery=*/false, error)) {
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
                /*allow_historical_policy=*/true,
                /*allow_legacy_authorized_recovery=*/false, error)) {
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
            /*allow_historical_policy=*/false,
            /*allow_legacy_authorized_recovery=*/false, error)) {
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
            /*allow_historical_policy=*/true,
            /*allow_legacy_authorized_recovery=*/false, error)) {
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
    if (current.phase == AlternativeRecoveryPhase::FINAL_COMMITTED ||
        have_commit || have_result) {
        if (have_commit && have_result && SameCommit(existing_commit, commit) &&
            SameResult(existing_result, result) &&
            SameAlternativeRecoveryRecord(current, recovery) &&
            ValidateProviderAlternativeRecoveryBudgetAuthorization(
                current, /*policy=*/nullptr, budget_ledger,
                BudgetReservationState::SPENT,
                /*allow_historical_policy=*/true,
                /*allow_legacy_authorized_recovery=*/true, error)) {
            return true;
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
    // change, but the reservation must still be present and RESERVED. V2 is
    // accepted here only because it was already durably authorized.
    if (!ValidateProviderAlternativeRecoveryBudgetAuthorization(
            current, /*policy=*/nullptr, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true,
            /*allow_legacy_authorized_recovery=*/true, error)) {
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
            /*allow_historical_policy=*/true,
            /*allow_legacy_authorized_recovery=*/true, error)) {
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAlternativeRecovery(recovery, true) ||
        !batch.WritePaymasterProviderCommit(commit, false) ||
        !batch.WritePaymasterResult(result, false) ||
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
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
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

bool PaymasterStore::RecordProviderOutcome(const uint256& attempt_id,
                                           ReliabilityOutcome outcome,
                                           int64_t observed_at,
                                           int64_t successful_latency_ms,
                                           std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    const bool compatible = [&] {
        switch (outcome) {
        case ReliabilityOutcome::SUCCESS:
            return attempt.state == AttemptState::STEMPOOL || attempt.state == AttemptState::MEMPOOL;
        case ReliabilityOutcome::PROVIDER_FAILURE:
            return attempt.state == AttemptState::REJECTED || attempt.state == AttemptState::CONFLICTED;
        case ReliabilityOutcome::NEUTRAL_FAILURE:
            return attempt.state == AttemptState::REJECTED || attempt.state == AttemptState::QUOTE_EXPIRED ||
                   attempt.state == AttemptState::AMBIGUOUS || attempt.state == AttemptState::CONFLICTED;
        case ReliabilityOutcome::AVAILABILITY_TIMEOUT:
            return attempt.state == AttemptState::QUOTE_EXPIRED || attempt.state == AttemptState::AMBIGUOUS;
        }
        return false;
    }();
    if (!compatible) {
        error = "PAYMASTER_OUTCOME_STATE_MISMATCH";
        return false;
    }
    PaymasterOutcomeMarker existing;
    if (batch.ReadPaymasterOutcomeMarker(attempt_id, existing)) {
        if (existing.provider_id == attempt.provider_id && existing.outcome == outcome) return true;
        error = "PAYMASTER_OUTCOME_ALREADY_RECORDED";
        return false;
    }
    PaymasterReliabilityRecord record;
    if (!batch.ReadPaymasterReliability(attempt.provider_id, record)) {
        record.provider_id = attempt.provider_id;
    }
    if (!ApplyReliabilityOutcome(record, outcome, observed_at, successful_latency_ms, error)) return false;
    const PaymasterOutcomeMarker marker{
        PaymasterOutcomeMarker::CURRENT_VERSION, attempt_id, attempt.provider_id, outcome, observed_at};
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterReliability(record) || !batch.WritePaymasterOutcomeMarker(marker, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetProviderReliability(const PaymasterId& provider_id,
                                            PaymasterReliabilityRecord& record) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    const bool have_record = batch.ReadPaymasterReliability(provider_id, record);
    PaymasterProviderBlock block;
    const DatabaseReadStatus block_status =
        batch.ReadPaymasterProviderBlock(provider_id, block);
    if (block_status == DatabaseReadStatus::READ_ERROR) return false;
    const bool blocked = block_status == DatabaseReadStatus::FOUND;
    if (!have_record && !blocked) return false;
    if (!have_record) {
        record = PaymasterReliabilityRecord{};
        record.provider_id = provider_id;
        record.last_observation_at = block.blocked_at;
    }
    if (blocked) {
        record.last_observation_at = std::max(record.last_observation_at, block.blocked_at);
        record.cooldown_until = std::numeric_limits<int64_t>::max();
    }
    return ValidateReliabilityRecord(record);
}

bool PaymasterStore::ListProviderReliability(
    std::vector<PaymasterReliabilityRecord>& records) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymasterProviderBlock> blocks;
    if (!batch.ListPaymasterReliability(records) ||
        !batch.ListPaymasterProviderBlocks(blocks)) {
        return false;
    }
    for (const PaymasterProviderBlock& block : blocks) {
        auto record = std::find_if(
            records.begin(), records.end(), [&](const PaymasterReliabilityRecord& candidate) {
                return candidate.provider_id == block.provider_id;
            });
        if (record == records.end()) {
            PaymasterReliabilityRecord blocked_record;
            blocked_record.provider_id = block.provider_id;
            blocked_record.last_observation_at = block.blocked_at;
            blocked_record.cooldown_until = std::numeric_limits<int64_t>::max();
            if (!ValidateReliabilityRecord(blocked_record)) return false;
            records.push_back(std::move(blocked_record));
        } else {
            record->last_observation_at = std::max(record->last_observation_at,
                                                   block.blocked_at);
            record->cooldown_until = std::numeric_limits<int64_t>::max();
            if (!ValidateReliabilityRecord(*record)) return false;
        }
    }
    return true;
}

bool PaymasterStore::ClearProviderReliability(const PaymasterId& provider_id,
                                              std::string& error)
{
    error.clear();
    if (provider_id.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_ID";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymasterReliabilityRecord existing;
    if (!batch.ReadPaymasterReliability(provider_id, existing)) return true;
    if (!batch.ErasePaymasterReliability(provider_id)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool PaymasterStore::StagePendingEquivocation(
    const PaymasterEquivocationEvidence& evidence,
    std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    return StagePendingEquivocationLocked(batch, evidence, error);
}

bool PaymasterStore::PromotePendingEquivocation(
    const PaymasterId& provider_id,
    std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    return PromotePendingEquivocationLocked(batch, provider_id, error);
}

bool PaymasterStore::PromotePendingEquivocations(std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymasterEquivocationEvidence> pending;
    const DatabaseReadStatus status =
        batch.ListPaymasterPendingEquivocations(pending);
    if (status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ";
        return false;
    }
    if (status == DatabaseReadStatus::NOT_FOUND) return true;
    for (const PaymasterEquivocationEvidence& evidence : pending) {
        if (!PromotePendingEquivocationLocked(batch, evidence.provider_id,
                                              error)) {
            return false;
        }
    }
    error.clear();
    return true;
}

DatabaseReadStatus PaymasterStore::GetPendingEquivocation(
    const PaymasterId& provider_id,
    PaymasterEquivocationEvidence& evidence,
    std::string& error) const
{
    error.clear();
    if (provider_id.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_ID";
        return DatabaseReadStatus::READ_ERROR;
    }
    LOCK(m_wallet.cs_wallet);
    const DatabaseReadStatus status =
        WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterPendingEquivocation(
            provider_id, evidence);
    if (status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_PENDING_EQUIVOCATION_DATABASE_READ";
    }
    return status;
}

bool PaymasterStore::GetEquivocationEvidence(
    const uint256& evidence_id,
    PaymasterEquivocationEvidence& evidence) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterEquivocationEvidence(
               evidence_id, evidence) == DatabaseReadStatus::FOUND;
}

DatabaseReadStatus PaymasterStore::GetProviderBlock(
    const PaymasterId& provider_id,
    PaymasterProviderBlock& block,
    std::string& error) const
{
    error.clear();
    if (provider_id.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_ID";
        return DatabaseReadStatus::READ_ERROR;
    }
    LOCK(m_wallet.cs_wallet);
    const DatabaseReadStatus status =
        WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterProviderBlock(
            provider_id, block);
    if (status == DatabaseReadStatus::READ_ERROR) {
        error = "PAYMASTER_PROVIDER_BLOCK_DATABASE_READ";
    }
    return status;
}

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
                const bool legacy_manifest =
                    observed_attempt->client_manifest.manifest_id.IsNull() ||
                    observed_attempt->client_manifest.version == 1 ||
                    observed_attempt->client_manifest.version == 2;
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
                if (!legacy_manifest) {
                    UserAuthorizationRecord authorization;
                    if (!batch.ReadPaymasterUserAuthorization(
                            observed_attempt->commit_key, authorization)) {
                        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
                        return false;
                    }
                    user_authorization = std::move(authorization);
                }
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
                !AlternativeRecoveryRecord::IsSupportedVersion(
                    recovery.version) ||
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
        const bool legacy_manifest =
            payment_attempt->client_manifest.manifest_id.IsNull() ||
            payment_attempt->client_manifest.version == 1 ||
            payment_attempt->client_manifest.version == 2;
        if (legacy_manifest) {
            CMutableTransaction validated_final;
            if (!ValidatePersistedLegacyClientFinalForRecovery(
                    m_wallet, *payment_attempt,
                    persisted_result,
                    Params().GenesisBlock().GetHash(), validated_final,
                    error) ||
                CTransaction{validated_final} != transaction) {
                if (error.empty()) {
                    error =
                        "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_MISMATCH";
                }
                return false;
            }
            PaymasterCapacityRequest capacity_request;
            PaymasterCapacityProof capacity_proof;
            try {
                SpanReader request_stream{
                    ::PROTOCOL_VERSION,
                    payment_attempt->capacity_request};
                request_stream >> capacity_request;
                SpanReader proof_stream{
                    ::PROTOCOL_VERSION,
                    payment_attempt->capacity_snapshot.capacity_proof};
                proof_stream >> capacity_proof;
                if (!request_stream.empty() || !proof_stream.empty()) {
                    throw std::ios_base::failure(
                        "trailing legacy capacity authority data");
                }
            } catch (const std::ios_base::failure&) {
                error = "PAYMASTER_CAPACITY_SNAPSHOT_ENCODING";
                return false;
            }
            if (CanonicalBytes(capacity_request) !=
                    payment_attempt->capacity_request ||
                CanonicalBytes(capacity_proof) !=
                    payment_attempt->capacity_snapshot.capacity_proof ||
                Hash(payment_attempt->capacity_request) !=
                    payment_attempt->capacity_snapshot.request_hash) {
                error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
                return false;
            }
            const int64_t authorization_time =
                payment_attempt->capacity_snapshot.validated_at;
            const AuthorizedCapacityResourceMode resource_mode{
                exact_final_already_known ? AuthorizedCapacityResourceMode::
                                                EXACT_FINAL_ALREADY_KNOWN :
                                            AuthorizedCapacityResourceMode::REQUIRE_UNSPENT};
            if (!ValidateAuthorizedCapacityRetryAgainstChainstate(
                    capacity_proof, capacity_request,
                    payment_attempt->provider_identity_key,
                    *node->chainman, authorization_time,
                    std::max(now, authorization_time), resource_mode,
                    error,
                    exact_final_already_known ? &transaction : nullptr)) {
                return false;
            }
            // The normal mempool/chain preflight performed by the caller is
            // still required. This narrow branch only recovers authority
            // already made durable by the historical exact final result.
            return true;
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
                const bool have_client_ledger =
                    batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
                if (!have_client_ledger &&
                    batch.HasPaymasterClientFeeLedger()) {
                    error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
                    return false;
                }
                if (have_client_ledger) {
                    // Confirmation of the cancel-to-self transaction proves
                    // that no original provider attempt can consume these DD
                    // inputs. Release only still-reserved original fees;
                    // already-spent fees and the (separate) recovery fee stay
                    // untouched.
                    for (const uint256& attempt_id : session.attempt_ids) {
                        ProviderAttempt original_attempt;
                        if (!batch.ReadPaymasterAttempt(attempt_id,
                                                        original_attempt) ||
                            original_attempt.commit_key.IsNull()) {
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
    if (!batch.ReadPaymasterSession(request_id, session)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (!IsTerminal(session.state)) {
        error = "PAYMASTER_SESSION_NOT_FINAL";
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
            if (!batch.ErasePaymasterReservation(outpoint) || !batch.EraseLockedUTXO(outpoint)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
    }
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) continue;
        if (!attempt.template_commitment.IsNull() &&
            !batch.ErasePaymasterTemplate(attempt.template_commitment)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!attempt.unsigned_txid.IsNull() &&
            !batch.ErasePaymasterUnsignedTx(attempt.unsigned_txid)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!attempt.commit_key.IsNull()) {
            ProviderCommitRecord commit;
            if (batch.ReadPaymasterProviderCommit(attempt.commit_key, commit) &&
                !batch.ErasePaymasterProviderCommit(attempt.commit_key)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            UserAuthorizationRecord authorization;
            if (batch.ReadPaymasterUserAuthorization(attempt.commit_key, authorization) &&
                !batch.ErasePaymasterUserAuthorization(attempt.commit_key)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            PaymasterResult result;
            if (batch.ReadPaymasterResult(attempt.commit_key, result) &&
                !batch.ErasePaymasterResult(attempt.commit_key)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
        PaymasterOutcomeMarker outcome;
        if (batch.ReadPaymasterOutcomeMarker(attempt_id, outcome) &&
            !batch.ErasePaymasterOutcomeMarker(attempt_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.ErasePaymasterAttempt(attempt_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
    }
    SelfRecoveryRecord recovery;
    if (batch.ReadPaymasterRecovery(request_id, recovery) &&
        !batch.ErasePaymasterRecovery(request_id)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    uint256 alternative_recovery_id;
    if (batch.ReadPaymasterAlternativeRecoveryRequest(
            request_id, alternative_recovery_id)) {
        AlternativeRecoveryRecord alternative;
        if (!batch.ReadPaymasterAlternativeRecovery(
                alternative_recovery_id, alternative) ||
            alternative.provider_side ||
            alternative.request_id != request_id ||
            alternative.session_id != session.session_id ||
            (session.state == SessionState::CANCELED_SAFE &&
             (alternative.phase !=
                  AlternativeRecoveryPhase::FINAL_COMMITTED ||
              alternative.final_transaction != recovery.final_transaction))) {
            return Abort(batch, error,
                         "PAYMASTER_ALTERNATIVE_RECOVERY_PRUNE_CONFLICT");
        }
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
