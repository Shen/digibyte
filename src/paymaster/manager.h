// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Node-level bounded queues, replay protection, and provider runtime coordination. */

#ifndef DIGIBYTE_PAYMASTER_MANAGER_H
#define DIGIBYTE_PAYMASTER_MANAGER_H

#include <paymaster/directory.h>
#include <paymaster/recovery.h>
#include <paymaster/wire.h>
#include <sync.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace DigiDollar::Paymaster {

static constexpr size_t MAX_DIRECT_INBOX_MESSAGES{8};
static constexpr size_t MAX_DIRECT_INBOX_BYTES{2 * 1024 * 1024};
static constexpr size_t MAX_DIRECT_INBOX_MESSAGES_PER_PEER{MAX_DIRECT_INBOX_MESSAGES / 2};
static constexpr size_t MAX_DIRECT_INBOX_BYTES_PER_PEER{MAX_DIRECT_INBOX_BYTES / 2};
static constexpr size_t MAX_DIRECT_INBOX_MESSAGES_PER_NETGROUP{MAX_DIRECT_INBOX_MESSAGES / 2};
static constexpr size_t MAX_DIRECT_INBOX_BYTES_PER_NETGROUP{MAX_DIRECT_INBOX_BYTES / 2};
static constexpr size_t MAX_DIRECT_INBOX_MESSAGES_PER_SESSION{2};
static constexpr size_t MAX_DIRECT_INBOX_BYTES_PER_SESSION{MAX_DIRECT_MESSAGE_BYTES};
static constexpr size_t MAX_DIRECT_REPLAY_ENTRIES{1024};
static constexpr int64_t DIRECT_REPLAY_TTL_SECONDS{10 * 60};
static constexpr int64_t PAYMASTER_RATE_WINDOW_SECONDS{60};
// One Paymaster transfer legitimately spans Capacity, quote, submit and result
// artifacts, including idempotent retries. Let one authenticated direct peer
// use the already bounded netgroup allowance without forcing a reconnect in
// the middle of a short burst; aggregate netgroup/global exposure is unchanged.
static constexpr size_t MAX_DIRECT_TRANSPORT_MESSAGES_PER_PEER{32};
static constexpr size_t MAX_DIRECT_TRANSPORT_MESSAGES_PER_NETGROUP{32};
static constexpr size_t MAX_DIRECT_TRANSPORT_MESSAGES_GLOBAL{256};
// Decoded artifacts still pass a smoother, stricter peer bucket after the
// cheap wire-level window. Keep this independent from reconnect policy.
static constexpr size_t MAX_DIRECT_PAYLOAD_MESSAGES_PER_PEER{16};
static constexpr size_t MAX_DIRECT_PAYLOAD_MESSAGES_PER_NETGROUP{32};
static constexpr size_t MAX_DIRECT_MESSAGES_PER_PROVIDER_PER_WINDOW{64};
static constexpr size_t MAX_DIRECT_MESSAGES_PER_SESSION_PER_WINDOW{16};
/** Suppress an exact retransmission on the same live direct peer briefly.
 * Status polling may race a response that is already in flight; sending the
 * same request again in that interval needlessly consumes the peer's inbound
 * transport budget. A replacement peer is deliberately not suppressed. */
static constexpr int64_t DIRECT_OUTBOUND_RETRY_DELAY_SECONDS{1};
/** Decoded direct-message token buckets use fixed-point units where one
 * complete token is DIRECT_TOKEN_BUCKET_PERIOD_SECONDS units. This permits a
 * smooth bounded refill without floating-point arithmetic. */
static constexpr uint64_t DIRECT_TOKEN_BUCKET_PERIOD_SECONDS{60};
static constexpr int64_t DIRECT_TOKEN_BUCKET_TTL_SECONDS{10 * 60};
static constexpr size_t MAX_DIRECT_PEER_TOKEN_BUCKETS{256};
static constexpr size_t MAX_DIRECT_NETGROUP_TOKEN_BUCKETS{256};
static constexpr size_t MAX_DIRECT_PROVIDER_TOKEN_BUCKETS{256};
static constexpr size_t MAX_DIRECT_SESSION_TOKEN_BUCKETS{1024};
// GETPMASTERS can legitimately return a 16-entry directory batch over one
// negotiated peer. Keep the cheap pre-deserialization peer window large
// enough for that protocol maximum; provider identity limits below still stop
// one signer from monopolizing the expensive validation path.
static constexpr size_t MAX_ANNOUNCEMENTS_PER_PEER_PER_WINDOW{16};
static constexpr size_t MAX_ANNOUNCEMENTS_PER_NETGROUP_PER_WINDOW{16};
static constexpr size_t MAX_ANNOUNCEMENTS_GLOBAL_PER_WINDOW{128};
static constexpr size_t MAX_ANNOUNCEMENTS_PER_PROVIDER_PER_WINDOW{4};

using DirectPayload = std::variant<PaymasterCapacityRequest,
                                   PaymasterCapacityProof,
                                   PaymasterQuoteRequest,
                                   PaymasterQuoteResponse,
                                   PaymasterSubmit,
                                   PaymasterResultMessage,
                                   AlternativeRecoveryRequest,
                                   AlternativeRecoveryResponse,
                                   AlternativeRecoverySubmit,
                                   AlternativeRecoveryResultMessage>;

enum class DirectEnqueueResult : uint8_t {
    ACCEPTED,
    DUPLICATE,
    CONFLICT,
    FULL,
    RATE_LIMITED,
    INVALID,
    DISABLED,
};

/** Ephemeral state of one wallet's provider worker. Persistent runtime
 * preferences live in ProviderSettings; no passphrase or peer metadata is
 * retained here. */
enum class ProviderServiceState : uint8_t {
    STOPPED,
    WAITING_FOR_UNLOCK,
    WAITING_FOR_READINESS,
    WAITING_FOR_MAINTENANCE_APPROVAL,
    REPLENISHING_LIQUIDITY,
    WAITING_FOR_LIQUIDITY_CONFIRMATION,
    ACTIVE,
    MANUAL,
    DRAIN_ONLY,
    FAULT,
};

struct ProviderServiceStatus {
    ProviderServiceState state{ProviderServiceState::STOPPED};
    std::string last_error;
};

/** Read-only provider-addressed queue depth for operator status displays.
 * Counts are process-local and contain no peer or network identity data. */
struct ProviderQueueStatus {
    size_t waiting_requests{0};
    size_t waiting_submits{0};
};

/** Network admission policy for an already decoded direct message. An exact
 * semantic retry is benign even when the manager suppressed a concurrently
 * queued copy; every other non-accepting result must close the direct channel.
 */
constexpr bool IsBenignDirectEnqueueResult(DirectEnqueueResult result) noexcept
{
    return result == DirectEnqueueResult::ACCEPTED ||
           result == DirectEnqueueResult::DUPLICATE;
}

struct DirectMessage {
    int64_t peer_id{-1};
    uint256 message_id;
    int64_t received_at{0};
    size_t serialized_size{0};
    DirectPayload payload;
    /** Process-local keyed group used only for in-memory admission/fairness. */
    std::optional<uint64_t> keyed_netgroup;
    /** Process-independent NetGroupManager::GetGroup() bytes. This is
     * transient and must be wallet-HMACed before durable use. */
    std::vector<unsigned char> canonical_netgroup;
    /** Monotonic queue incarnation. This prevents an old RAII scope from
     * releasing a newly enqueued exact replay with the same content hash. */
    uint64_t queue_sequence{0};
};

struct OutboundDirectMessage {
    int64_t peer_id{-1};
    uint256 message_id;
    int64_t enqueued_at{0};
    size_t serialized_size{0};
    DirectPayload payload;
};

class Manager;

/** Exception-safe ownership of one Peek* lease set. The messages stay queued
 * while this object is alive; destroying or explicitly releasing the scope
 * drops only this scope's lease references. Acknowledging or consuming a
 * message remains terminal and makes the later scope cleanup a no-op. */
class DirectMessageLease final
{
public:
    DirectMessageLease(const DirectMessageLease&) = delete;
    DirectMessageLease& operator=(const DirectMessageLease&) = delete;
    DirectMessageLease(DirectMessageLease&& other) noexcept;
    DirectMessageLease& operator=(DirectMessageLease&& other) noexcept;
    ~DirectMessageLease();

    const std::vector<DirectMessage>& Messages() const noexcept
    {
        return m_messages;
    }
    void Release() noexcept;

private:
    friend class Manager;
    DirectMessageLease(Manager& manager,
                       std::vector<DirectMessage>&& messages) noexcept;

    Manager* m_manager;
    std::vector<DirectMessage> m_messages;
};

class Manager
{
public:
    explicit Manager(bool enabled) : m_enabled{enabled} {}

    bool Enabled() const { return m_enabled.load(); }
    void SetEnabled(bool enabled)
    {
        m_enabled.store(enabled);
        if (!enabled) {
            m_directory.Clear();
            ClearDirectMessages();
            LOCK(m_provider_mutex);
            m_running_providers.clear();
            m_provider_work.clear();
            m_provider_service_status.clear();
        }
    }

    Directory& GetDirectory() { return m_directory; }
    const Directory& GetDirectory() const { return m_directory; }

    /** Consume a cheap transport-level admission token before deserializing or
     * cryptographically validating a direct message. keyed_netgroup is the
     * process-local CNode netgroup identifier, never a raw or persisted IP.
     */
    bool AdmitDirectTransport(int64_t peer_id, uint64_t keyed_netgroup, int64_t now);
    /** Apply discovery flood limits before announcement signature and UTXO
     * validation. Provider admission is split out because its identity is only
     * available after the bounded message has been decoded.
     */
    bool AdmitAnnouncementTransport(int64_t peer_id, uint64_t keyed_netgroup, int64_t now);
    bool AdmitAnnouncementProvider(const PaymasterId& provider_id, int64_t now);
    /** Apply the wallet-configured quote-request ceiling after the target
     * provider is known but before chainstate, signature, or UTXO work. The
     * keyed netgroup is process-local and is never persisted as an address. */
    bool AdmitProviderQuoteRequest(const PaymasterId& provider_id,
                                   uint64_t keyed_netgroup,
                                   uint32_t maximum_per_minute,
                                   int64_t now);

    /** Enqueue an already shape-validated direct message. Secrets remain only
     * in this small in-memory queue and are never written or logged here.
     */
    bool EnqueueDirectMessage(int64_t peer_id,
                              const uint256& message_id,
                              size_t serialized_size,
                              DirectPayload payload,
                              int64_t now,
                              std::optional<uint64_t> keyed_netgroup = std::nullopt,
                              std::vector<unsigned char> canonical_netgroup = {});
    /** Return the exact replay/queue outcome. Exact retries never add a second
     * concurrently queued message, but may be delivered again after the first
     * copy was consumed so durable handlers can reproduce their response. A
     * different message occupying the same protocol slot is admitted once,
     * remains available to the durable handler as equivocation evidence, and
     * returns CONFLICT so the network layer still closes the direct channel.
     * Redelivered retries and retained conflicts remain subject to every
     * admission bucket. */
    DirectEnqueueResult EnqueueDirectMessageResult(int64_t peer_id,
                                                   const uint256& message_id,
                                                   size_t serialized_size,
                                                   DirectPayload payload,
                                                   int64_t now,
                                                   std::optional<uint64_t> keyed_netgroup = std::nullopt,
                                                   std::vector<unsigned char> canonical_netgroup = {});
    std::vector<DirectMessage> TakeDirectMessages(size_t maximum);
    /** Remove only capacity requests addressed to one provider identity. */
    std::vector<DirectMessage> TakeCapacityRequests(const PaymasterId& provider_id,
                                                    size_t maximum);
    /** Lease provider-addressed capacity requests without removing them. */
    DirectMessageLease LeaseCapacityRequests(const PaymasterId& provider_id,
                                              size_t maximum);
    /** Remove only capacity proofs for the exact selected provider/client
     * nonce pair. Other wallets and concurrent handshakes remain queued. */
    std::vector<DirectMessage> TakeCapacityProofs(const PaymasterId& provider_id,
                                                  const uint256& client_nonce,
                                                  size_t maximum);
    /** Return a stable fair selection of capacity proofs without removing them
     * or advancing the shared fairness cursors. Observation is unleased by
     * default. Production consumers that cross a durable boundary must use
     * LeaseCapacityProofs(); lease=true is retained only as its low-level
     * implementation hook. The returned message ids are acknowledgement
     * tokens.
     */
    std::vector<DirectMessage> PeekCapacityProofs(const PaymasterId& provider_id,
                                                  const uint256& client_nonce,
                                                  size_t maximum,
                                                  bool lease = false);
    /** Return capacity proofs together with an exception-safe prune lease. */
    DirectMessageLease LeaseCapacityProofs(const PaymasterId& provider_id,
                                           const uint256& client_nonce,
                                           size_t maximum);
    /** Remove only quote responses belonging to the exact client attempt.
     * Multiple attempts can use the same provider inside one session, so the
     * signed intent hash is part of this ownership filter. */
    std::vector<DirectMessage> TakeQuoteResponses(const std::string& request_id,
                                                  const uint256& session_id,
                                                  const PaymasterId& provider_id,
                                                  const uint256& intent_hash,
                                                  size_t maximum);
    /** Return a stable fair selection of quote responses without removing them
     * or advancing the shared fairness cursors. See PeekCapacityProofs() for
     * observation semantics; durable consumers use LeaseQuoteResponses().
     */
    std::vector<DirectMessage> PeekQuoteResponses(const std::string& request_id,
                                                  const uint256& session_id,
                                                  const PaymasterId& provider_id,
                                                  const uint256& intent_hash,
                                                  size_t maximum,
                                                  bool lease = false);
    /** Return quote responses together with an exception-safe prune lease. */
    DirectMessageLease LeaseQuoteResponses(const std::string& request_id,
                                           const uint256& session_id,
                                           const PaymasterId& provider_id,
                                           const uint256& intent_hash,
                                           size_t maximum);
    /** Remove only quote requests addressed to one running provider wallet. */
    std::vector<DirectMessage> TakeQuoteRequests(const PaymasterId& provider_id,
                                                 size_t maximum);
    DirectMessageLease LeaseQuoteRequests(const PaymasterId& provider_id,
                                           size_t maximum);
    std::vector<DirectMessage> TakeSubmits(const PaymasterId& provider_id,
                                           size_t maximum);
    DirectMessageLease LeaseSubmits(const PaymasterId& provider_id,
                                    size_t maximum);
    std::vector<DirectMessage> TakeResults(const std::string& request_id,
                                           const uint256& session_id,
                                           const PaymasterId& provider_id,
                                           size_t maximum);
    std::vector<DirectMessage> TakeRecoveryRequests(
        const PaymasterId& provider_id, size_t maximum);
    DirectMessageLease LeaseRecoveryRequests(const PaymasterId& provider_id,
                                              size_t maximum);
    std::vector<DirectMessage> TakeRecoveryResponses(
        const std::string& request_id, const uint256& session_id,
        const PaymasterId& provider_id, size_t maximum);
    std::vector<DirectMessage> TakeRecoverySubmits(
        const PaymasterId& provider_id, size_t maximum);
    DirectMessageLease LeaseRecoverySubmits(const PaymasterId& provider_id,
                                             size_t maximum);
    std::vector<DirectMessage> TakeRecoveryResults(
        const std::string& request_id, const uint256& session_id,
        const PaymasterId& provider_id, size_t maximum);
    bool QueueOutboundDirectMessage(int64_t peer_id,
                                    const uint256& message_id,
                                    size_t serialized_size,
                                    DirectPayload payload,
                                    int64_t now);
    bool QueueCapacityRequest(int64_t peer_id,
                              const uint256& message_id,
                              size_t serialized_size,
                              PaymasterCapacityRequest request,
                              int64_t now);
    bool QueueCapacityProof(int64_t peer_id,
                            const uint256& message_id,
                            size_t serialized_size,
                            PaymasterCapacityProof proof,
                            int64_t now);
    std::vector<OutboundDirectMessage> TakeOutboundDirectMessages(
        int64_t peer_id,
        size_t maximum,
        int64_t now);
    /** Remove expired inbound and outbound direct messages using the supplied
     * observation time. The caller must first let every loaded wallet scan
     * signed evidence candidates. Queue byte accounting, replay state and
     * fairness cursors are updated under the direct-message lock. */
    void PruneDirectMessages(int64_t now);
    void ClearDirectMessages();
    /** Atomically remove exactly the currently queued inbound messages whose
     * content hashes are listed. If any id is null, duplicated, or absent, no
     * message is removed and false is returned.
     */
    bool AcknowledgeDirectMessages(const std::vector<uint256>& message_ids);
    /** Idempotently remove every listed inbound message that is still queued.
     * Missing ids are accepted so two durable consumers, or a concurrent TTL
     * prune, cannot turn an already committed result into a retry failure.
     * Null or duplicated ids remain invalid and remove nothing.
     */
    bool AcknowledgeDirectMessagesIfPresent(
        const std::vector<uint256>& message_ids);
    bool HasDirectMessages() const;
    /** Return whether the inbound queue contains a signed artifact that can
     * conflict with a wallet-persisted Capacity proof or quote response.
     */
    bool HasEquivocationCandidates() const;
    size_t DirectMessageCount() const;
    size_t OutboundDirectMessageCount() const;
    bool HasOutboundDirectMessage(int64_t peer_id, const uint256& message_id) const;

    /** Provider runtime state is intentionally ephemeral. Persistent
     * enablement and policy remain owned by the provider wallet. */
    bool StartProvider(const std::string& wallet_name, const PaymasterId& provider_id);
    /** Atomically turn a caller-owned provider-work slot into the running
     * state while retaining that slot. The caller releases it only after
     * service status and announcement publication are complete, so disable
     * cannot split the start transition. */
    bool CompleteProviderStart(const std::string& wallet_name,
                               const PaymasterId& provider_id);
    void StopProvider(const std::string& wallet_name);
    bool IsProviderRunning(const std::string& wallet_name,
                           const PaymasterId& provider_id) const;
    size_t RunningProviderCount() const;
    /** Serialize wallet-local provider work across the scheduler and expert
     * RPCs. Callers that operate only while stopped (for example releasing a
     * carrier slot) may explicitly waive the runtime requirement. */
    bool TryBeginProviderWork(const std::string& wallet_name,
                              const PaymasterId& provider_id,
                              bool require_running = true);
    void EndProviderWork(const std::string& wallet_name);
    void SetProviderServiceStatus(const std::string& wallet_name,
                                  ProviderServiceState state,
                                  std::string last_error = {});
    ProviderServiceStatus GetProviderServiceStatus(
        const std::string& wallet_name) const;
    ProviderQueueStatus GetProviderQueueStatus(
        const PaymasterId& provider_id) const;

private:
    friend class DirectMessageLease;
    using RateWindow = std::deque<int64_t>;

    struct TokenBucket {
        uint64_t available_units{0};
        int64_t last_refill{0};
        int64_t last_access{0};
    };

    struct DirectReplayRecord {
        uint256 message_id;
        int64_t expires_at{0};
    };

    void PruneReplays(int64_t now, bool retain_equivocation_candidates)
        EXCLUSIVE_LOCKS_REQUIRED(m_direct_mutex);
    void PruneInboundMessages(int64_t now,
                              bool retain_equivocation_candidates)
        EXCLUSIVE_LOCKS_REQUIRED(m_direct_mutex);
    void PruneOutboundMessages(int64_t now)
        EXCLUSIVE_LOCKS_REQUIRED(m_direct_mutex);
    bool AcknowledgeDirectMessagesInternal(
        const std::vector<uint256>& message_ids,
        bool allow_absent);
    void ReleaseDirectMessageLeases(
        const std::vector<DirectMessage>& messages) noexcept;
    bool AdmitDirectPayload(int64_t peer_id,
                            std::optional<uint64_t> keyed_netgroup,
                            const DirectPayload& payload,
                            int64_t now);
    int64_t PruneRateWindows(int64_t now) EXCLUSIVE_LOCKS_REQUIRED(m_rate_mutex);
    void ClearRateLimits();

    std::atomic<bool> m_enabled;
    Directory m_directory;
    mutable Mutex m_direct_mutex;
    std::deque<DirectMessage> m_direct_messages GUARDED_BY(m_direct_mutex);
    // Multiple RPC scopes may inspect the same stable candidate concurrently.
    // Pruning is allowed only after every scope has released its reference.
    // The pair is (queue incarnation, reference count).
    std::map<uint256, std::pair<uint64_t, size_t>> m_leased_direct_ids GUARDED_BY(m_direct_mutex);
    uint64_t m_next_direct_sequence GUARDED_BY(m_direct_mutex){0};
    // Replay identity is derived from the protocol session rather than the
    // short-lived peer id, so reconnecting cannot bypass idempotency checks.
    std::map<uint256, DirectReplayRecord> m_direct_replays GUARDED_BY(m_direct_mutex);
    size_t m_direct_bytes GUARDED_BY(m_direct_mutex){0};
    // Direct work rotates first between netgroups and then between semantic
    // protocol sessions inside the selected netgroup. The session cursor is
    // independent of peer ids so reconnecting cannot gain scheduling priority.
    std::optional<uint64_t> m_last_inbound_group GUARDED_BY(m_direct_mutex);
    std::map<uint64_t, uint256> m_last_inbound_session_by_group GUARDED_BY(m_direct_mutex);
    std::deque<OutboundDirectMessage> m_outbound_direct_messages GUARDED_BY(m_direct_mutex);
    std::set<std::pair<int64_t, uint256>> m_outbound_direct_ids GUARDED_BY(m_direct_mutex);
    std::map<std::pair<int64_t, uint256>, int64_t> m_recent_outbound_direct_ids GUARDED_BY(m_direct_mutex);
    size_t m_outbound_direct_bytes GUARDED_BY(m_direct_mutex){0};
    mutable Mutex m_rate_mutex;
    int64_t m_rate_high_water GUARDED_BY(m_rate_mutex){0};
    RateWindow m_direct_transport_events GUARDED_BY(m_rate_mutex);
    std::map<int64_t, RateWindow> m_direct_peer_events GUARDED_BY(m_rate_mutex);
    std::map<uint64_t, RateWindow> m_direct_netgroup_events GUARDED_BY(m_rate_mutex);
    // Decoded-message buckets are distinct from the cheap pre-deserialization
    // transport windows. They bind semantic provider/session identity, and
    // every payload admitted for delivery is charged, including an exact replay
    // after the prior copy has been consumed.
    std::map<int64_t, TokenBucket> m_direct_peer_buckets GUARDED_BY(m_rate_mutex);
    std::map<uint64_t, TokenBucket> m_direct_netgroup_buckets GUARDED_BY(m_rate_mutex);
    std::map<PaymasterId, TokenBucket> m_direct_provider_buckets GUARDED_BY(m_rate_mutex);
    std::map<uint256, TokenBucket> m_direct_session_buckets GUARDED_BY(m_rate_mutex);
    RateWindow m_announcement_transport_events GUARDED_BY(m_rate_mutex);
    std::map<int64_t, RateWindow> m_announcement_peer_events GUARDED_BY(m_rate_mutex);
    std::map<uint64_t, RateWindow> m_announcement_netgroup_events GUARDED_BY(m_rate_mutex);
    std::map<PaymasterId, RateWindow> m_announcement_provider_events GUARDED_BY(m_rate_mutex);
    std::map<std::pair<PaymasterId, uint64_t>, RateWindow> m_provider_quote_netgroup_events GUARDED_BY(m_rate_mutex);
    mutable Mutex m_provider_mutex;
    std::map<std::string, PaymasterId> m_running_providers GUARDED_BY(m_provider_mutex);
    /** Provider work ownership is bound to both wallet and identity. A caller
     * holding a wallet slot must not complete a start for another provider. */
    std::map<std::string, PaymasterId> m_provider_work GUARDED_BY(m_provider_mutex);
    std::map<std::string, ProviderServiceStatus> m_provider_service_status GUARDED_BY(m_provider_mutex);
};

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_MANAGER_H
