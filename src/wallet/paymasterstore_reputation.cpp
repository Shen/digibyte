// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider outcomes, reputation, and durable equivocation evidence. */

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
    const DatabaseReadStatus attempt_status{
        batch.ReadPaymasterAttemptWithStatus(attempt_id, attempt)};
    if (attempt_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            attempt_status, "ProviderAttempt", attempt,
            "PAYMASTER_ATTEMPT_NOT_FOUND", "PAYMASTER_INVALID_ATTEMPT");
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
    const DatabaseReadStatus outcome_status{
        batch.ReadPaymasterOutcomeMarkerWithStatus(attempt_id, existing)};
    if (outcome_status == DatabaseReadStatus::FOUND) {
        if (existing.provider_id == attempt.provider_id && existing.outcome == outcome) return true;
        error = "PAYMASTER_OUTCOME_ALREADY_RECORDED";
        return false;
    }
    if (outcome_status != DatabaseReadStatus::NOT_FOUND) {
        error = PersistedReadError(
            outcome_status, "PaymasterOutcomeMarker", existing,
            "PAYMASTER_OUTCOME_NOT_FOUND", "PAYMASTER_INVALID_OUTCOME_MARKER");
        return false;
    }
    PaymasterReliabilityRecord record;
    const DatabaseReadStatus reliability_status{
        batch.ReadPaymasterReliabilityWithStatus(attempt.provider_id, record)};
    if (reliability_status == DatabaseReadStatus::NOT_FOUND) {
        record.provider_id = attempt.provider_id;
    } else if (reliability_status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            reliability_status, "PaymasterReliabilityRecord", record,
            "PAYMASTER_RELIABILITY_NOT_FOUND",
            "PAYMASTER_INVALID_RELIABILITY_RECORD");
        return false;
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
    const DatabaseReadStatus record_status{
        batch.ReadPaymasterReliabilityWithStatus(provider_id, record)};
    if (record_status != DatabaseReadStatus::FOUND &&
        record_status != DatabaseReadStatus::NOT_FOUND) {
        return false;
    }
    const bool have_record{record_status == DatabaseReadStatus::FOUND};
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
    const DatabaseReadStatus status{
        batch.ReadPaymasterReliabilityWithStatus(provider_id, existing)};
    if (status == DatabaseReadStatus::NOT_FOUND) return true;
    if (status != DatabaseReadStatus::FOUND) {
        error = PersistedReadError(
            status, "PaymasterReliabilityRecord", existing,
            "PAYMASTER_RELIABILITY_NOT_FOUND",
            "PAYMASTER_INVALID_RELIABILITY_RECORD");
        return false;
    }
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

} // namespace wallet
