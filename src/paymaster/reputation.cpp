// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Wallet-local reliability accounting and signed-equivocation evidence.
 * Counters saturate instead of wrapping, and reputation remains advisory;
 * it cannot weaken transaction, capacity, or authorization validation.
 */

#include <paymaster/reputation.h>

#include <hash.h>

#include <algorithm>
#include <limits>

namespace DigiDollar::Paymaster {
namespace {

template <typename T>
void SaturatingIncrement(T& value)
{
    if (value != std::numeric_limits<T>::max()) ++value;
}

void SaturatingAdd(uint64_t& value, uint64_t add)
{
    value = add > std::numeric_limits<uint64_t>::max() - value
                ? std::numeric_limits<uint64_t>::max() : value + add;
}

void SaturatingAdd(uint32_t& value, uint32_t add)
{
    value = add > std::numeric_limits<uint32_t>::max() - value
                ? std::numeric_limits<uint32_t>::max() : value + add;
}

int64_t UpdateLatencyEwma(int64_t current, int64_t sample)
{
    if (current == 0) return sample;
    // Equivalent to (3 * current + sample) / 4 for non-negative
    // measurements, without overflowing when either persisted value is near
    // INT64_MAX. Preserve the original floor rounding exactly.
    if (sample >= current) return current + (sample - current) / 4;
    const int64_t difference{current - sample};
    return current - difference / 4 - (difference % 4 != 0 ? 1 : 0);
}

PaymasterReliabilityBucket& BucketForDay(PaymasterReliabilityRecord& record, int32_t utc_day)
{
    for (auto& bucket : record.daily_buckets) {
        if (bucket.utc_day == utc_day) return bucket;
    }
    auto oldest = std::min_element(record.daily_buckets.begin(), record.daily_buckets.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.utc_day < rhs.utc_day; });
    *oldest = PaymasterReliabilityBucket{};
    oldest->utc_day = utc_day;
    return *oldest;
}

} // namespace

uint256 GetEquivocationEvidenceId(EquivocationKind kind,
                                  const PaymasterId& provider_id,
                                  const uint256& semantic_key,
                                  const uint256& first_artifact_hash,
                                  const uint256& second_artifact_hash)
{
    const uint256& low = first_artifact_hash < second_artifact_hash
                             ? first_artifact_hash : second_artifact_hash;
    const uint256& high = first_artifact_hash < second_artifact_hash
                              ? second_artifact_hash : first_artifact_hash;
    HashWriter hasher = TaggedHash("DigiByte Paymaster Equivocation Evidence v1");
    hasher << static_cast<uint8_t>(kind) << provider_id << semantic_key << low << high;
    return hasher.GetSHA256();
}

bool ValidateEquivocationEvidence(const PaymasterEquivocationEvidence& evidence)
{
    if (evidence.version != PaymasterEquivocationEvidence::CURRENT_VERSION ||
        static_cast<uint8_t>(evidence.kind) > static_cast<uint8_t>(EquivocationKind::QUOTE) ||
        evidence.evidence_id.IsNull() || evidence.provider_id.IsNull() ||
        evidence.semantic_key.IsNull() || evidence.first_artifact_hash.IsNull() ||
        evidence.second_artifact_hash.IsNull() ||
        evidence.first_artifact_hash == evidence.second_artifact_hash ||
        evidence.first_artifact.empty() || evidence.second_artifact.empty() ||
        evidence.first_artifact.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        evidence.second_artifact.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        evidence.observed_at <= 0 || Hash(evidence.first_artifact) != evidence.first_artifact_hash ||
        Hash(evidence.second_artifact) != evidence.second_artifact_hash) {
        return false;
    }
    return evidence.evidence_id == GetEquivocationEvidenceId(
        evidence.kind, evidence.provider_id, evidence.semantic_key,
        evidence.first_artifact_hash, evidence.second_artifact_hash);
}

bool ValidateProviderBlock(const PaymasterProviderBlock& block)
{
    return block.version == PaymasterProviderBlock::CURRENT_VERSION &&
           !block.provider_id.IsNull() && !block.evidence_id.IsNull() &&
           static_cast<uint8_t>(block.kind) <= static_cast<uint8_t>(EquivocationKind::QUOTE) &&
           block.blocked_at > 0;
}

bool ValidateReliabilityRecord(const PaymasterReliabilityRecord& record)
{
    if (record.version != PaymasterReliabilityRecord::CURRENT_VERSION ||
        record.provider_id.IsNull() || record.last_observation_at < 0 ||
        record.cooldown_until < 0 || record.latency_ewma_ms < 0) return false;
    for (const auto& bucket : record.daily_buckets) {
        if (bucket.utc_day < 0) return false;
    }
    return true;
}

bool ApplyReliabilityOutcome(PaymasterReliabilityRecord& record,
                             ReliabilityOutcome outcome,
                             int64_t observed_at,
                             int64_t successful_latency_ms,
                             std::string& error)
{
    if (!ValidateReliabilityRecord(record) ||
        static_cast<uint8_t>(outcome) > static_cast<uint8_t>(ReliabilityOutcome::AVAILABILITY_TIMEOUT) ||
        observed_at <= 0 || observed_at / 86400 > std::numeric_limits<int32_t>::max() ||
        observed_at < record.last_observation_at || successful_latency_ms < 0 ||
        (outcome != ReliabilityOutcome::SUCCESS && successful_latency_ms != 0)) {
        error = "PAYMASTER_INVALID_RELIABILITY_OBSERVATION";
        return false;
    }
    const int32_t utc_day = static_cast<int32_t>(observed_at / 86400);
    PaymasterReliabilityBucket& bucket = BucketForDay(record, utc_day);
    switch (outcome) {
    case ReliabilityOutcome::SUCCESS:
        SaturatingIncrement(bucket.successful_attempts);
        SaturatingAdd(bucket.successful_latency_sum_ms, static_cast<uint64_t>(successful_latency_ms));
        record.consecutive_provider_failures = 0;
        record.cooldown_until = 0;
        record.latency_ewma_ms = UpdateLatencyEwma(
            record.latency_ewma_ms, successful_latency_ms);
        break;
    case ReliabilityOutcome::PROVIDER_FAILURE: {
        SaturatingIncrement(bucket.provider_failures);
        SaturatingIncrement(record.consecutive_provider_failures);
        const uint32_t shift = std::min<uint32_t>(record.consecutive_provider_failures - 1, 6);
        const int64_t cooldown = std::min(PROVIDER_FAILURE_COOLDOWN_BASE_SECONDS << shift,
                                          PROVIDER_FAILURE_COOLDOWN_MAX_SECONDS);
        record.cooldown_until = std::max(
            record.cooldown_until, SaturatingAddSeconds(observed_at, cooldown));
        break;
    }
    case ReliabilityOutcome::NEUTRAL_FAILURE:
        SaturatingIncrement(bucket.neutral_failures);
        break;
    case ReliabilityOutcome::AVAILABILITY_TIMEOUT:
        SaturatingIncrement(bucket.availability_timeouts);
        record.cooldown_until = std::max(record.cooldown_until,
                                         SaturatingAddSeconds(
                                             observed_at,
                                             AVAILABILITY_COOLDOWN_SECONDS));
        break;
    }
    record.last_observation_at = observed_at;
    error.clear();
    return true;
}

ReliabilitySummary SummarizeReliability(const PaymasterReliabilityRecord& record, int64_t now)
{
    ReliabilitySummary result;
    if (!ValidateReliabilityRecord(record) || now <= 0) return result;
    const int64_t first_day = now / 86400 - static_cast<int64_t>(RELIABILITY_DAYS) + 1;
    for (const auto& bucket : record.daily_buckets) {
        if (bucket.utc_day < first_day || bucket.utc_day > now / 86400) continue;
        SaturatingAdd(result.successful_attempts, bucket.successful_attempts);
        SaturatingAdd(result.provider_failures, bucket.provider_failures);
        SaturatingAdd(result.neutral_failures, bucket.neutral_failures);
        SaturatingAdd(result.availability_timeouts, bucket.availability_timeouts);
    }
    const uint64_t assessable = static_cast<uint64_t>(result.successful_attempts) + result.provider_failures;
    result.sufficient_data = assessable >= MIN_RELIABILITY_SAMPLES;
    if (assessable != 0) {
        result.success_rate_basis_points = static_cast<uint32_t>(
            static_cast<uint64_t>(result.successful_attempts) * 10000 / assessable);
    }
    return result;
}

} // namespace DigiDollar::Paymaster
