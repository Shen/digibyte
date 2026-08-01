// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Local, advisory provider reliability accounting. */

#ifndef DIGIBYTE_PAYMASTER_REPUTATION_H
#define DIGIBYTE_PAYMASTER_REPUTATION_H

#include <paymaster/types.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

static constexpr size_t RELIABILITY_DAYS{30};
static constexpr uint32_t MIN_RELIABILITY_SAMPLES{5};
static constexpr int64_t PROVIDER_FAILURE_COOLDOWN_BASE_SECONDS{60};
static constexpr int64_t PROVIDER_FAILURE_COOLDOWN_MAX_SECONDS{60 * 60};
static constexpr int64_t AVAILABILITY_COOLDOWN_SECONDS{30};
static constexpr size_t MAX_EQUIVOCATION_ARTIFACT_BYTES{1024 * 1024};

enum class ReliabilityOutcome : uint8_t {
    SUCCESS,
    PROVIDER_FAILURE,
    NEUTRAL_FAILURE,
    AVAILABILITY_TIMEOUT,
};

/** A remotely signed artifact class for which two different payloads cannot
 * legitimately share one semantic binding. */
enum class EquivocationKind : uint8_t {
    CAPACITY,
    QUOTE,
};

/** Durable, locally observed cryptographic evidence. Artifacts are retained
 * so the signatures can be rechecked after restart; peer IDs, network
 * addresses and raw IP addresses are deliberately absent. */
struct PaymasterEquivocationEvidence {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 evidence_id;
    EquivocationKind kind{EquivocationKind::CAPACITY};
    PaymasterId provider_id;
    uint256 semantic_key;
    uint256 first_artifact_hash;
    uint256 second_artifact_hash;
    std::vector<unsigned char> first_artifact;
    std::vector<unsigned char> second_artifact;
    int64_t observed_at{0};

    SERIALIZE_METHODS(PaymasterEquivocationEvidence, obj)
    {
        READWRITE(obj.version, obj.evidence_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(EquivocationKind::QUOTE)>>(obj.kind),
                  obj.provider_id, obj.semantic_key, obj.first_artifact_hash,
                  obj.second_artifact_hash, obj.first_artifact,
                  obj.second_artifact, obj.observed_at);
    }
};

/** Permanent local deny-list entry created only from verified equivocation
 * evidence. Clearing ordinary reputation must not remove this record. */
struct PaymasterProviderBlock {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    PaymasterId provider_id;
    uint256 evidence_id;
    EquivocationKind kind{EquivocationKind::CAPACITY};
    int64_t blocked_at{0};

    SERIALIZE_METHODS(PaymasterProviderBlock, obj)
    {
        READWRITE(obj.version, obj.provider_id, obj.evidence_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(EquivocationKind::QUOTE)>>(obj.kind),
                  obj.blocked_at);
    }
};

struct PaymasterReliabilityBucket {
    int32_t utc_day{0};
    uint32_t successful_attempts{0};
    uint32_t provider_failures{0};
    uint32_t neutral_failures{0};
    uint32_t availability_timeouts{0};
    uint64_t successful_latency_sum_ms{0};

    SERIALIZE_METHODS(PaymasterReliabilityBucket, obj)
    {
        READWRITE(obj.utc_day, obj.successful_attempts, obj.provider_failures,
                  obj.neutral_failures, obj.availability_timeouts,
                  obj.successful_latency_sum_ms);
    }
};

struct PaymasterReliabilityRecord {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    PaymasterId provider_id;
    std::array<PaymasterReliabilityBucket, RELIABILITY_DAYS> daily_buckets{};
    uint32_t consecutive_provider_failures{0};
    int64_t latency_ewma_ms{0};
    int64_t last_observation_at{0};
    int64_t cooldown_until{0};

    SERIALIZE_METHODS(PaymasterReliabilityRecord, obj)
    {
        READWRITE(obj.version, obj.provider_id);
        for (auto& bucket : obj.daily_buckets) READWRITE(bucket);
        READWRITE(obj.consecutive_provider_failures, obj.latency_ewma_ms,
                  obj.last_observation_at, obj.cooldown_until);
    }
};

struct PaymasterOutcomeMarker {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 attempt_id;
    PaymasterId provider_id;
    ReliabilityOutcome outcome{ReliabilityOutcome::NEUTRAL_FAILURE};
    int64_t observed_at{0};

    SERIALIZE_METHODS(PaymasterOutcomeMarker, obj)
    {
        READWRITE(obj.version, obj.attempt_id, obj.provider_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(ReliabilityOutcome::AVAILABILITY_TIMEOUT)>>(obj.outcome),
                  obj.observed_at);
    }
};

struct ReliabilitySummary {
    uint32_t successful_attempts{0};
    uint32_t provider_failures{0};
    uint32_t neutral_failures{0};
    uint32_t availability_timeouts{0};
    bool sufficient_data{false};
    uint32_t success_rate_basis_points{0};
};

bool ValidateReliabilityRecord(const PaymasterReliabilityRecord& record);
uint256 GetEquivocationEvidenceId(EquivocationKind kind,
                                  const PaymasterId& provider_id,
                                  const uint256& semantic_key,
                                  const uint256& first_artifact_hash,
                                  const uint256& second_artifact_hash);
bool ValidateEquivocationEvidence(const PaymasterEquivocationEvidence& evidence);
bool ValidateProviderBlock(const PaymasterProviderBlock& block);
bool ApplyReliabilityOutcome(PaymasterReliabilityRecord& record,
                             ReliabilityOutcome outcome,
                             int64_t observed_at,
                             int64_t successful_latency_ms,
                             std::string& error);
ReliabilitySummary SummarizeReliability(const PaymasterReliabilityRecord& record,
                                        int64_t now);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_REPUTATION_H
