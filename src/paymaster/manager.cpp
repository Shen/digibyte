// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Bounded Paymaster discovery, direct-message queues, replay handling, and
 * provider-service coordination. Network queues own no wallet authority:
 * leased messages remain present until wallet processing durably acknowledges
 * them, which makes retries safe across temporary local failures.
 */

#include <paymaster/manager.h>

#include <hash.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace DigiDollar::Paymaster {

namespace {

uint256 GetDirectSessionKey(const DirectPayload& payload)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Direct Session v1");
    std::visit([&](const auto& message) {
        using Message = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<Message, PaymasterCapacityRequest> ||
                      std::is_same_v<Message, PaymasterCapacityProof>) {
            // The nonce is signed payload, not semantic identity: changing it
            // for the same protocol request/session must conflict rather than create
            // a second capacity handshake.
            hasher << message.provider_id << message.request_id << message.session_id;
        } else if constexpr (std::is_same_v<Message, PaymasterQuoteRequest>) {
            hasher << message.intent.provider_id << message.intent.request_id
                   << message.intent.session_id;
        } else if constexpr (std::is_same_v<Message, PaymasterQuoteResponse>) {
            hasher << message.quote.provider_id << message.request_id << message.session_id;
        } else if constexpr (std::is_same_v<Message, PaymasterSubmit>) {
            hasher << message.provider_id << message.request_id << message.session_id;
        } else if constexpr (std::is_same_v<Message, PaymasterResultMessage>) {
            hasher << message.result.provider_id << message.request_id << message.session_id;
        } else if constexpr (std::is_same_v<Message, AlternativeRecoveryRequest>) {
            hasher << message.recovery_provider_id << message.request_id
                   << message.session_id;
        } else if constexpr (std::is_same_v<Message, AlternativeRecoveryResponse>) {
            hasher << message.recovery_provider_id << message.request_id
                   << message.session_id;
        } else if constexpr (std::is_same_v<Message, AlternativeRecoverySubmit>) {
            hasher << message.recovery_provider_id << message.request_id
                   << message.session_id;
        } else {
            hasher << message.result.provider_id << message.request_id
                   << message.session_id;
        }
    },
               payload);
    return hasher.GetSHA256();
}

uint256 GetDirectReplayKey(const DirectPayload& payload)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Direct Replay v1");
    hasher << static_cast<uint8_t>(payload.index()) << GetDirectSessionKey(payload);
    if (const auto* response = std::get_if<PaymasterQuoteResponse>(&payload)) {
        hasher << response->quote.quote_id
               << response->quote.template_commitment;
    } else if (const auto* submit = std::get_if<PaymasterSubmit>(&payload)) {
        hasher << submit->quote_id << submit->commit_key
               << submit->template_commitment;
    } else if (const auto* result = std::get_if<PaymasterResultMessage>(&payload)) {
        // Multiple monotonic provider results belong to one session, but one
        // sequence number has exactly one canonical meaning.
        hasher << result->result.commit_key
               << result->result.result_sequence;
    } else if (const auto* request =
                   std::get_if<AlternativeRecoveryRequest>(&payload)) {
        hasher << request->original_commit_key;
    } else if (const auto* response =
                   std::get_if<AlternativeRecoveryResponse>(&payload)) {
        hasher << response->recovery_id << response->recovery_request_hash
               << response->recovery_commit_key;
    } else if (const auto* submit =
                   std::get_if<AlternativeRecoverySubmit>(&payload)) {
        hasher << submit->recovery_id << submit->recovery_request_hash
               << submit->recovery_commit_key
               << submit->template_commitment;
    } else if (const auto* result =
                   std::get_if<AlternativeRecoveryResultMessage>(&payload)) {
        hasher << result->recovery_id << result->recovery_request_hash
               << result->result.commit_key
               << result->result.result_sequence;
    }
    return hasher.GetSHA256();
}

PaymasterId GetDirectProviderId(const DirectPayload& payload)
{
    return std::visit([](const auto& message) -> PaymasterId {
        using Message = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<Message, PaymasterCapacityRequest> ||
                      std::is_same_v<Message, PaymasterCapacityProof> ||
                      std::is_same_v<Message, PaymasterSubmit>) {
            return message.provider_id;
        } else if constexpr (std::is_same_v<Message, PaymasterQuoteRequest>) {
            return message.intent.provider_id;
        } else if constexpr (std::is_same_v<Message, PaymasterQuoteResponse>) {
            return message.quote.provider_id;
        } else if constexpr (std::is_same_v<Message, PaymasterResultMessage>) {
            return message.result.provider_id;
        } else if constexpr (std::is_same_v<Message, AlternativeRecoveryRequest> ||
                             std::is_same_v<Message, AlternativeRecoveryResponse> ||
                             std::is_same_v<Message, AlternativeRecoverySubmit>) {
            return message.recovery_provider_id;
        } else {
            return message.result.provider_id;
        }
    },
                      payload);
}

void PruneRateWindow(std::deque<int64_t>& window, int64_t cutoff)
{
    while (!window.empty() && window.front() <= cutoff)
        window.pop_front();
}

template <typename Map>
void PruneRateMap(Map& windows, int64_t cutoff)
{
    for (auto it = windows.begin(); it != windows.end();) {
        PruneRateWindow(it->second, cutoff);
        if (it->second.empty())
            it = windows.erase(it);
        else
            ++it;
    }
}

template <typename Map, typename Key>
size_t RateWindowSize(const Map& windows, const Key& key)
{
    const auto it = windows.find(key);
    return it == windows.end() ? 0 : it->second.size();
}

uint64_t SaturatingMultiply(uint64_t lhs, uint64_t rhs)
{
    if (lhs == 0 || rhs == 0) return 0;
    if (lhs > std::numeric_limits<uint64_t>::max() / rhs) {
        return std::numeric_limits<uint64_t>::max();
    }
    return lhs * rhs;
}

uint64_t GetDirectFairGroup(const DirectMessage& message)
{
    return message.keyed_netgroup.value_or(
        static_cast<uint64_t>(message.peer_id));
}

template <typename Predicate>
std::vector<DirectMessage> TakeMatchingFair(std::deque<DirectMessage>& messages,
                                            size_t& total_bytes,
                                            std::map<uint256, std::pair<uint64_t, size_t>>& leased_ids,
                                            std::optional<uint64_t>& last_group,
                                            std::map<uint64_t, uint256>& last_session_by_group,
                                            size_t maximum,
                                            Predicate&& matches)
{
    const auto available = [&](const DirectMessage& message) {
        if (!matches(message)) return false;
        const auto lease = leased_ids.find(message.message_id);
        return lease == leased_ids.end() ||
               lease->second.first != message.queue_sequence ||
               lease->second.second == 0;
    };
    std::vector<DirectMessage> result;
    result.reserve(std::min(maximum, messages.size()));
    while (result.size() < maximum) {
        std::optional<uint64_t> selected_group;
        for (const DirectMessage& message : messages) {
            if (!available(message)) continue;
            const uint64_t group{GetDirectFairGroup(message)};
            if (last_group && group <= *last_group) continue;
            if (!selected_group || group < *selected_group) selected_group = group;
        }
        if (!selected_group) {
            for (const DirectMessage& message : messages) {
                if (!available(message)) continue;
                const uint64_t group{GetDirectFairGroup(message)};
                if (!selected_group || group < *selected_group) selected_group = group;
            }
        }
        if (!selected_group) break;

        std::vector<uint256> sessions;
        for (const DirectMessage& message : messages) {
            if (!available(message) ||
                GetDirectFairGroup(message) != *selected_group) {
                continue;
            }
            const uint256 session{GetDirectSessionKey(message.payload)};
            if (std::find(sessions.begin(), sessions.end(), session) == sessions.end()) {
                sessions.push_back(session);
            }
        }
        if (sessions.empty()) break;

        size_t selected_session_index{0};
        const auto last_session = last_session_by_group.find(*selected_group);
        if (last_session != last_session_by_group.end()) {
            const auto cursor = std::find(sessions.begin(), sessions.end(),
                                          last_session->second);
            if (cursor != sessions.end()) {
                selected_session_index =
                    (static_cast<size_t>(std::distance(sessions.begin(), cursor)) + 1) %
                    sessions.size();
            }
        }
        const uint256 selected_session{sessions[selected_session_index]};
        const auto selected = std::find_if(
            messages.begin(), messages.end(), [&](const DirectMessage& message) {
                return available(message) &&
                       GetDirectFairGroup(message) == *selected_group &&
                       GetDirectSessionKey(message.payload) == selected_session;
            });
        if (selected == messages.end()) break;

        last_group = *selected_group;
        last_session_by_group[*selected_group] = selected_session;
        total_bytes -= selected->serialized_size;
        result.push_back(std::move(*selected));
        messages.erase(selected);
        if (std::none_of(messages.begin(), messages.end(), [&](const DirectMessage& message) {
                return GetDirectFairGroup(message) == *selected_group;
            })) {
            last_session_by_group.erase(*selected_group);
        }
    }
    return result;
}

void AddDirectMessageLeases(
    std::map<uint256, std::pair<uint64_t, size_t>>& leased_ids,
    const std::vector<DirectMessage>& messages)
{
    size_t added{0};
    try {
        for (const DirectMessage& message : messages) {
            const auto lease = leased_ids
                                   .try_emplace(
                                       message.message_id,
                                       std::pair{message.queue_sequence,
                                                 size_t{0}})
                                   .first;
            if (lease->second.first != message.queue_sequence) {
                throw std::logic_error{
                    "Paymaster direct-message lease incarnation mismatch"};
            }
            if (lease->second.second == std::numeric_limits<size_t>::max()) {
                throw std::overflow_error{"Paymaster direct-message lease overflow"};
            }
            ++lease->second.second;
            ++added;
        }
    } catch (...) {
        for (size_t index{0}; index < added; ++index) {
            const auto lease = leased_ids.find(messages[index].message_id);
            if (lease == leased_ids.end() ||
                lease->second.first != messages[index].queue_sequence) {
                continue;
            }
            if (--lease->second.second == 0) leased_ids.erase(lease);
        }
        throw;
    }
}

template <typename Predicate>
std::vector<DirectMessage> PeekMatchingFair(
    const std::deque<DirectMessage>& messages,
    const std::optional<uint64_t>& last_group,
    const std::map<uint64_t, uint256>& last_session_by_group,
    size_t maximum,
    Predicate&& matches)
{
    std::optional<uint64_t> next_group{last_group};
    std::map<uint64_t, uint256> next_session_by_group{
        last_session_by_group};
    std::vector<const DirectMessage*> selected_messages;
    std::vector<DirectMessage> result;
    selected_messages.reserve(std::min(maximum, messages.size()));
    result.reserve(std::min(maximum, messages.size()));

    const auto already_selected = [&selected_messages](
                                      const DirectMessage& message) {
        return std::find(selected_messages.begin(), selected_messages.end(),
                         &message) != selected_messages.end();
    };
    while (result.size() < maximum) {
        std::optional<uint64_t> selected_group;
        for (const DirectMessage& message : messages) {
            if (already_selected(message) || !matches(message)) continue;
            const uint64_t group{GetDirectFairGroup(message)};
            if (next_group && group <= *next_group) continue;
            if (!selected_group || group < *selected_group) {
                selected_group = group;
            }
        }
        if (!selected_group) {
            for (const DirectMessage& message : messages) {
                if (already_selected(message) || !matches(message)) continue;
                const uint64_t group{GetDirectFairGroup(message)};
                if (!selected_group || group < *selected_group) {
                    selected_group = group;
                }
            }
        }
        if (!selected_group) break;

        std::vector<uint256> sessions;
        for (const DirectMessage& message : messages) {
            if (already_selected(message) || !matches(message) ||
                GetDirectFairGroup(message) != *selected_group) {
                continue;
            }
            const uint256 session{GetDirectSessionKey(message.payload)};
            if (std::find(sessions.begin(), sessions.end(), session) ==
                sessions.end()) {
                sessions.push_back(session);
            }
        }
        if (sessions.empty()) break;

        size_t selected_session_index{0};
        const auto last_session =
            next_session_by_group.find(*selected_group);
        if (last_session != next_session_by_group.end()) {
            const auto cursor = std::find(sessions.begin(), sessions.end(),
                                          last_session->second);
            if (cursor != sessions.end()) {
                selected_session_index =
                    (static_cast<size_t>(
                         std::distance(sessions.begin(), cursor)) +
                     1) %
                    sessions.size();
            }
        }
        const uint256 selected_session{sessions[selected_session_index]};
        const auto selected = std::find_if(
            messages.begin(), messages.end(), [&](const DirectMessage& message) {
                return !already_selected(message) && matches(message) &&
                       GetDirectFairGroup(message) == *selected_group &&
                       GetDirectSessionKey(message.payload) == selected_session;
            });
        if (selected == messages.end()) break;

        next_group = *selected_group;
        next_session_by_group[*selected_group] = selected_session;
        selected_messages.push_back(&*selected);
        result.push_back(*selected);
        if (std::none_of(
                messages.begin(), messages.end(),
                [&](const DirectMessage& message) {
                    return !already_selected(message) &&
                           GetDirectFairGroup(message) == *selected_group;
                })) {
            next_session_by_group.erase(*selected_group);
        }
    }
    return result;
}

} // namespace

int64_t Manager::PruneRateWindows(int64_t now)
{
    const int64_t effective_now{std::max(now, m_rate_high_water)};
    m_rate_high_water = effective_now;
    const int64_t cutoff{effective_now > PAYMASTER_RATE_WINDOW_SECONDS ? effective_now - PAYMASTER_RATE_WINDOW_SECONDS : 0};
    PruneRateWindow(m_direct_transport_events, cutoff);
    PruneRateMap(m_direct_peer_events, cutoff);
    PruneRateMap(m_direct_netgroup_events, cutoff);
    PruneRateWindow(m_announcement_transport_events, cutoff);
    PruneRateMap(m_announcement_peer_events, cutoff);
    PruneRateMap(m_announcement_netgroup_events, cutoff);
    PruneRateMap(m_announcement_provider_events, cutoff);
    PruneRateMap(m_provider_quote_netgroup_events, cutoff);
    return effective_now;
}

bool Manager::AdmitDirectTransport(int64_t peer_id, uint64_t keyed_netgroup, int64_t now)
{
    if (!Enabled() || peer_id < 0 || now <= 0) return false;
    LOCK(m_rate_mutex);
    if (!Enabled()) return false;
    const int64_t admitted_at{PruneRateWindows(now)};
    if (m_direct_transport_events.size() >= MAX_DIRECT_TRANSPORT_MESSAGES_GLOBAL ||
        RateWindowSize(m_direct_peer_events, peer_id) >= MAX_DIRECT_TRANSPORT_MESSAGES_PER_PEER ||
        RateWindowSize(m_direct_netgroup_events, keyed_netgroup) >= MAX_DIRECT_TRANSPORT_MESSAGES_PER_NETGROUP) {
        return false;
    }
    m_direct_transport_events.push_back(admitted_at);
    m_direct_peer_events[peer_id].push_back(admitted_at);
    m_direct_netgroup_events[keyed_netgroup].push_back(admitted_at);
    return true;
}

bool Manager::AdmitAnnouncementTransport(int64_t peer_id, uint64_t keyed_netgroup, int64_t now)
{
    if (!Enabled() || peer_id < 0 || now <= 0) return false;
    LOCK(m_rate_mutex);
    if (!Enabled()) return false;
    const int64_t admitted_at{PruneRateWindows(now)};
    if (m_announcement_transport_events.size() >= MAX_ANNOUNCEMENTS_GLOBAL_PER_WINDOW ||
        RateWindowSize(m_announcement_peer_events, peer_id) >= MAX_ANNOUNCEMENTS_PER_PEER_PER_WINDOW ||
        RateWindowSize(m_announcement_netgroup_events, keyed_netgroup) >= MAX_ANNOUNCEMENTS_PER_NETGROUP_PER_WINDOW) {
        return false;
    }
    m_announcement_transport_events.push_back(admitted_at);
    m_announcement_peer_events[peer_id].push_back(admitted_at);
    m_announcement_netgroup_events[keyed_netgroup].push_back(admitted_at);
    return true;
}

bool Manager::AdmitAnnouncementProvider(const PaymasterId& provider_id, int64_t now)
{
    if (!Enabled() || provider_id.IsNull() || now <= 0) return false;
    LOCK(m_rate_mutex);
    if (!Enabled()) return false;
    const int64_t admitted_at{PruneRateWindows(now)};
    if (RateWindowSize(m_announcement_provider_events, provider_id) >=
        MAX_ANNOUNCEMENTS_PER_PROVIDER_PER_WINDOW) {
        return false;
    }
    m_announcement_provider_events[provider_id].push_back(admitted_at);
    return true;
}

bool Manager::AdmitProviderQuoteRequest(const PaymasterId& provider_id,
                                        uint64_t keyed_netgroup,
                                        uint32_t maximum_per_minute,
                                        int64_t now)
{
    if (!Enabled() || provider_id.IsNull() || maximum_per_minute == 0 || now <= 0) {
        return false;
    }
    LOCK(m_rate_mutex);
    if (!Enabled()) return false;
    const int64_t admitted_at{PruneRateWindows(now)};
    auto& events = m_provider_quote_netgroup_events[{provider_id, keyed_netgroup}];
    if (events.size() >= maximum_per_minute) return false;
    events.push_back(admitted_at);
    return true;
}

bool Manager::AdmitDirectPayload(int64_t peer_id,
                                 std::optional<uint64_t> keyed_netgroup,
                                 const DirectPayload& payload,
                                 int64_t now)
{
    const PaymasterId provider_id{GetDirectProviderId(payload)};
    const uint256 session_key{GetDirectSessionKey(payload)};
    const uint64_t netgroup_key{keyed_netgroup.value_or(static_cast<uint64_t>(peer_id))};
    LOCK(m_rate_mutex);
    const int64_t admitted_at{PruneRateWindows(now)};

    const auto capacity_units = [](size_t capacity) {
        return SaturatingMultiply(static_cast<uint64_t>(capacity),
                                  DIRECT_TOKEN_BUCKET_PERIOD_SECONDS);
    };
    const auto refill = [&](TokenBucket& bucket, size_t capacity) {
        const uint64_t maximum{capacity_units(capacity)};
        if (bucket.last_refill <= 0) {
            bucket.available_units = maximum;
            bucket.last_refill = admitted_at;
            return;
        }
        if (admitted_at > bucket.last_refill) {
            const uint64_t elapsed{static_cast<uint64_t>(admitted_at - bucket.last_refill)};
            const uint64_t added{SaturatingMultiply(elapsed, static_cast<uint64_t>(capacity))};
            bucket.available_units = added >= maximum - std::min(bucket.available_units, maximum) ?
                                         maximum :
                                         bucket.available_units + added;
            bucket.last_refill = admitted_at;
        }
    };
    const int64_t stale_cutoff{admitted_at > DIRECT_TOKEN_BUCKET_TTL_SECONDS ?
                                   admitted_at - DIRECT_TOKEN_BUCKET_TTL_SECONDS :
                                   0};
    const auto access_bucket = [&](auto& buckets,
                                   const auto& key,
                                   size_t capacity,
                                   size_t maximum_entries,
                                   bool& created) -> TokenBucket* {
        auto found = buckets.find(key);
        if (found != buckets.end()) {
            refill(found->second, capacity);
            found->second.last_access = admitted_at;
            return &found->second;
        }

        for (auto it = buckets.begin(); it != buckets.end();) {
            refill(it->second, capacity);
            if (it->second.last_access <= stale_cutoff &&
                it->second.available_units == capacity_units(capacity)) {
                it = buckets.erase(it);
            } else {
                ++it;
            }
        }
        while (buckets.size() >= maximum_entries) {
            const auto oldest_full = std::min_element(
                buckets.begin(), buckets.end(),
                [&](const auto& lhs, const auto& rhs) {
                    const bool lhs_full{lhs.second.available_units == capacity_units(capacity)};
                    const bool rhs_full{rhs.second.available_units == capacity_units(capacity)};
                    if (lhs_full != rhs_full) return lhs_full;
                    return lhs.second.last_access < rhs.second.last_access;
                });
            if (oldest_full == buckets.end() ||
                oldest_full->second.available_units != capacity_units(capacity)) {
                return nullptr;
            }
            buckets.erase(oldest_full);
        }

        TokenBucket bucket;
        bucket.available_units = capacity_units(capacity);
        bucket.last_refill = admitted_at;
        bucket.last_access = admitted_at;
        const auto [inserted, success] = buckets.emplace(key, bucket);
        if (!success) return nullptr;
        created = true;
        return &inserted->second;
    };

    bool peer_created{false};
    bool netgroup_created{false};
    bool provider_created{false};
    bool session_created{false};
    TokenBucket* const peer_bucket = access_bucket(
        m_direct_peer_buckets, peer_id, MAX_DIRECT_PAYLOAD_MESSAGES_PER_PEER,
        MAX_DIRECT_PEER_TOKEN_BUCKETS, peer_created);
    TokenBucket* const netgroup_bucket = access_bucket(
        m_direct_netgroup_buckets, netgroup_key, MAX_DIRECT_PAYLOAD_MESSAGES_PER_NETGROUP,
        MAX_DIRECT_NETGROUP_TOKEN_BUCKETS, netgroup_created);
    TokenBucket* provider_bucket{nullptr};
    if (!provider_id.IsNull()) {
        provider_bucket = access_bucket(
            m_direct_provider_buckets, provider_id,
            MAX_DIRECT_MESSAGES_PER_PROVIDER_PER_WINDOW,
            MAX_DIRECT_PROVIDER_TOKEN_BUCKETS, provider_created);
    }
    TokenBucket* const session_bucket = access_bucket(
        m_direct_session_buckets, session_key,
        MAX_DIRECT_MESSAGES_PER_SESSION_PER_WINDOW,
        MAX_DIRECT_SESSION_TOKEN_BUCKETS, session_created);

    const uint64_t one_token{DIRECT_TOKEN_BUCKET_PERIOD_SECONDS};
    if (!peer_bucket || !netgroup_bucket || (!provider_id.IsNull() && !provider_bucket) ||
        !session_bucket || peer_bucket->available_units < one_token ||
        netgroup_bucket->available_units < one_token ||
        (provider_bucket && provider_bucket->available_units < one_token) ||
        session_bucket->available_units < one_token) {
        if (peer_created) m_direct_peer_buckets.erase(peer_id);
        if (netgroup_created) m_direct_netgroup_buckets.erase(netgroup_key);
        if (provider_created) m_direct_provider_buckets.erase(provider_id);
        if (session_created) m_direct_session_buckets.erase(session_key);
        return false;
    }

    peer_bucket->available_units -= one_token;
    netgroup_bucket->available_units -= one_token;
    if (provider_bucket) provider_bucket->available_units -= one_token;
    session_bucket->available_units -= one_token;
    return true;
}

void Manager::ClearRateLimits()
{
    LOCK(m_rate_mutex);
    m_rate_high_water = 0;
    m_direct_transport_events.clear();
    m_direct_peer_events.clear();
    m_direct_netgroup_events.clear();
    m_direct_peer_buckets.clear();
    m_direct_netgroup_buckets.clear();
    m_direct_provider_buckets.clear();
    m_direct_session_buckets.clear();
    m_announcement_transport_events.clear();
    m_announcement_peer_events.clear();
    m_announcement_netgroup_events.clear();
    m_announcement_provider_events.clear();
    m_provider_quote_netgroup_events.clear();
}

void Manager::PruneReplays(int64_t now,
                           bool retain_equivocation_candidates)
{
    const auto retained_by_candidate = [&](const auto& replay) {
        return retain_equivocation_candidates &&
               std::any_of(
                   m_direct_messages.begin(), m_direct_messages.end(),
                   [&replay](const DirectMessage& message) {
                       const bool evidence_candidate =
                           std::holds_alternative<PaymasterCapacityProof>(
                               message.payload) ||
                           std::holds_alternative<PaymasterQuoteResponse>(
                               message.payload);
                       return evidence_candidate &&
                              GetDirectReplayKey(message.payload) == replay.first;
                   });
    };
    for (auto it = m_direct_replays.begin(); it != m_direct_replays.end();) {
        if (it->second.expires_at <= now && !retained_by_candidate(*it))
            it = m_direct_replays.erase(it);
        else
            ++it;
    }
    while (m_direct_replays.size() >= MAX_DIRECT_REPLAY_ENTRIES) {
        auto oldest = m_direct_replays.end();
        for (auto replay = m_direct_replays.begin();
             replay != m_direct_replays.end(); ++replay) {
            if (retained_by_candidate(*replay)) continue;
            if (oldest == m_direct_replays.end() ||
                replay->second.expires_at < oldest->second.expires_at) {
                oldest = replay;
            }
        }
        // The inbound queue is much smaller than the replay cap, but fail
        // closed if every remaining entry is nevertheless evidence-backed.
        if (oldest == m_direct_replays.end()) break;
        m_direct_replays.erase(oldest);
    }
}

void Manager::PruneInboundMessages(int64_t now,
                                   bool retain_equivocation_candidates)
{
    for (auto it = m_direct_messages.begin(); it != m_direct_messages.end();) {
        const auto lease = m_leased_direct_ids.find(it->message_id);
        if (lease != m_leased_direct_ids.end() &&
            lease->second.first == it->queue_sequence &&
            lease->second.second != 0) {
            ++it;
            continue;
        }
        const bool equivocation_candidate =
            std::holds_alternative<PaymasterCapacityProof>(it->payload) ||
            std::holds_alternative<PaymasterQuoteResponse>(it->payload);
        if (retain_equivocation_candidates && equivocation_candidate) {
            ++it;
            continue;
        }
        if (SaturatingAddSeconds(it->received_at,
                                 MAX_DIRECT_MESSAGE_TTL_SECONDS) > now) {
            ++it;
            continue;
        }
        m_direct_bytes -= it->serialized_size;
        it = m_direct_messages.erase(it);
    }
    for (auto cursor = m_last_inbound_session_by_group.begin();
         cursor != m_last_inbound_session_by_group.end();) {
        const uint64_t group{cursor->first};
        const bool group_has_messages = std::any_of(
            m_direct_messages.begin(), m_direct_messages.end(),
            [group](const DirectMessage& message) {
                return message.keyed_netgroup.value_or(
                           static_cast<uint64_t>(message.peer_id)) == group;
            });
        if (group_has_messages) {
            ++cursor;
        } else {
            cursor = m_last_inbound_session_by_group.erase(cursor);
        }
    }
    if (m_direct_messages.empty()) {
        m_last_inbound_group.reset();
        m_last_inbound_session_by_group.clear();
    }
}

void Manager::PruneOutboundMessages(int64_t now)
{
    for (auto it = m_recent_outbound_direct_ids.begin();
         it != m_recent_outbound_direct_ids.end();) {
        if (SaturatingAddSeconds(it->second,
                                 DIRECT_OUTBOUND_RETRY_DELAY_SECONDS) > now) {
            ++it;
            continue;
        }
        it = m_recent_outbound_direct_ids.erase(it);
    }
    for (auto it = m_outbound_direct_messages.begin();
         it != m_outbound_direct_messages.end();) {
        if (SaturatingAddSeconds(it->enqueued_at,
                                 MAX_DIRECT_MESSAGE_TTL_SECONDS) > now) {
            ++it;
            continue;
        }
        m_outbound_direct_bytes -= it->serialized_size;
        m_outbound_direct_ids.erase(std::pair{it->peer_id, it->message_id});
        it = m_outbound_direct_messages.erase(it);
    }
}

bool Manager::EnqueueDirectMessage(int64_t peer_id,
                                   const uint256& message_id,
                                   size_t serialized_size,
                                   DirectPayload payload,
                                   int64_t now,
                                   std::optional<uint64_t> keyed_netgroup,
                                   std::vector<unsigned char> canonical_netgroup)
{
    const DirectEnqueueResult result = EnqueueDirectMessageResult(
        peer_id, message_id, serialized_size, std::move(payload), now,
        keyed_netgroup, std::move(canonical_netgroup));
    return IsBenignDirectEnqueueResult(result);
}

DirectEnqueueResult Manager::EnqueueDirectMessageResult(int64_t peer_id,
                                                        const uint256& message_id,
                                                        size_t serialized_size,
                                                        DirectPayload payload,
                                                        int64_t now,
                                                        std::optional<uint64_t> keyed_netgroup,
                                                        std::vector<unsigned char> canonical_netgroup)
{
    if (!Enabled() || peer_id < 0 || message_id.IsNull() || serialized_size == 0 ||
        serialized_size > MAX_DIRECT_MESSAGE_BYTES || now <= 0) {
        return Enabled() ? DirectEnqueueResult::INVALID : DirectEnqueueResult::DISABLED;
    }
    const uint256 replay_key = GetDirectReplayKey(payload);
    const uint256 session_key = GetDirectSessionKey(payload);
    LOCK(m_direct_mutex);
    if (!Enabled()) return DirectEnqueueResult::DISABLED;
    PruneReplays(now, /*retain_equivocation_candidates=*/true);
    // Signed Capacity proofs and quote responses may conflict with a durable
    // wallet baseline. Retain them until periodic maintenance has scanned all
    // loaded wallets; an enqueue after suspend must not erase evidence first.
    PruneInboundMessages(now, /*retain_equivocation_candidates=*/true);
    const auto replay = m_direct_replays.find(replay_key);
    const bool exact_replay =
        replay != m_direct_replays.end() &&
        replay->second.message_id == message_id;
    const bool conflicting_replay =
        replay != m_direct_replays.end() && !exact_replay;
    // Suppress the same concurrently queued artifact. A repeated retained
    // conflict remains classified as CONFLICT so reconnecting peers still
    // take the network-layer disconnect path without multiplying queue work.
    if (std::any_of(
            m_direct_messages.begin(), m_direct_messages.end(),
            [&replay_key, &message_id](const DirectMessage& queued) {
                return queued.message_id == message_id &&
                       GetDirectReplayKey(queued.payload) == replay_key;
            })) {
        return conflicting_replay ? DirectEnqueueResult::CONFLICT : DirectEnqueueResult::DUPLICATE;
    }

    size_t peer_messages{0};
    size_t peer_bytes{0};
    size_t netgroup_messages{0};
    size_t netgroup_bytes{0};
    size_t session_messages{0};
    size_t session_bytes{0};
    for (const DirectMessage& queued : m_direct_messages) {
        if (queued.peer_id == peer_id) {
            ++peer_messages;
            peer_bytes += queued.serialized_size;
        }
        if (keyed_netgroup && queued.keyed_netgroup == keyed_netgroup) {
            ++netgroup_messages;
            netgroup_bytes += queued.serialized_size;
        }
        if (GetDirectSessionKey(queued.payload) == session_key) {
            ++session_messages;
            session_bytes += queued.serialized_size;
        }
    }
    if (m_direct_messages.size() >= MAX_DIRECT_INBOX_MESSAGES ||
        serialized_size > MAX_DIRECT_INBOX_BYTES - m_direct_bytes) {
        return DirectEnqueueResult::FULL;
    }
    if (peer_messages >= MAX_DIRECT_INBOX_MESSAGES_PER_PEER ||
        serialized_size > MAX_DIRECT_INBOX_BYTES_PER_PEER - peer_bytes ||
        (keyed_netgroup &&
         (netgroup_messages >= MAX_DIRECT_INBOX_MESSAGES_PER_NETGROUP ||
          serialized_size > MAX_DIRECT_INBOX_BYTES_PER_NETGROUP - netgroup_bytes)) ||
        session_messages >= MAX_DIRECT_INBOX_MESSAGES_PER_SESSION ||
        serialized_size > MAX_DIRECT_INBOX_BYTES_PER_SESSION - session_bytes) {
        return DirectEnqueueResult::FULL;
    }
    // Replays and retained conflicts still consume admission capacity. Their
    // durable handlers are idempotent, but signature/UTXO validation and wallet
    // database lookups are not free; allowing either to bypass these buckets
    // would let reconnecting peers amplify one protocol slot indefinitely.
    if (!AdmitDirectPayload(peer_id, keyed_netgroup, payload, now)) {
        return DirectEnqueueResult::RATE_LIMITED;
    }
    if (m_next_direct_sequence == std::numeric_limits<uint64_t>::max()) {
        return DirectEnqueueResult::FULL;
    }
    const uint64_t queue_sequence{++m_next_direct_sequence};
    if (exact_replay) {
        replay->second.expires_at = SaturatingAddSeconds(now, DIRECT_REPLAY_TTL_SECONDS);
    } else if (conflicting_replay) {
        // Keep the original semantic authority while its newly retained
        // conflict awaits durable validation and evidence persistence.
        replay->second.expires_at = SaturatingAddSeconds(now, DIRECT_REPLAY_TTL_SECONDS);
    } else {
        m_direct_replays.emplace(
            replay_key,
            DirectReplayRecord{message_id, SaturatingAddSeconds(now, DIRECT_REPLAY_TTL_SECONDS)});
    }
    m_direct_bytes += serialized_size;
    m_leased_direct_ids.erase(message_id);
    m_direct_messages.push_back(DirectMessage{peer_id, message_id, now,
                                              serialized_size, std::move(payload),
                                              keyed_netgroup,
                                              std::move(canonical_netgroup),
                                              queue_sequence});
    if (conflicting_replay) return DirectEnqueueResult::CONFLICT;
    return exact_replay ? DirectEnqueueResult::DUPLICATE : DirectEnqueueResult::ACCEPTED;
}

std::vector<DirectMessage> Manager::TakeDirectMessages(size_t maximum)
{
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [](const DirectMessage&) { return true; });
}

std::vector<DirectMessage> Manager::TakeCapacityRequests(const PaymasterId& provider_id,
                                                         size_t maximum)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || maximum == 0) return result;
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [&provider_id](const DirectMessage& message) {
                                const auto* request = std::get_if<PaymasterCapacityRequest>(&message.payload);
                                return request != nullptr && request->provider_id == provider_id;
                            });
}

DirectMessageLease Manager::LeaseCapacityRequests(
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (!provider_id.IsNull() && maximum != 0) {
        LOCK(m_direct_mutex);
        result = PeekMatchingFair(
            m_direct_messages, m_last_inbound_group,
            m_last_inbound_session_by_group, maximum,
            [&provider_id](const DirectMessage& message) {
                const auto* request =
                    std::get_if<PaymasterCapacityRequest>(&message.payload);
                return request != nullptr &&
                       request->provider_id == provider_id;
            });
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return DirectMessageLease{*this, std::move(result)};
}

std::vector<DirectMessage> Manager::TakeCapacityProofs(const PaymasterId& provider_id,
                                                       const uint256& client_nonce,
                                                       size_t maximum)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || client_nonce.IsNull() || maximum == 0) return result;
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [&provider_id, &client_nonce](const DirectMessage& message) {
                                const auto* proof = std::get_if<PaymasterCapacityProof>(&message.payload);
                                return proof != nullptr && proof->provider_id == provider_id &&
                                       proof->client_nonce == client_nonce;
                            });
}

std::vector<DirectMessage> Manager::PeekCapacityProofs(
    const PaymasterId& provider_id,
    const uint256& client_nonce,
    size_t maximum,
    bool lease)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || client_nonce.IsNull() || maximum == 0) {
        return result;
    }
    LOCK(m_direct_mutex);
    result = PeekMatchingFair(
        m_direct_messages, m_last_inbound_group,
        m_last_inbound_session_by_group, maximum,
        [&provider_id, &client_nonce](const DirectMessage& message) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            return proof != nullptr && proof->provider_id == provider_id &&
                   proof->client_nonce == client_nonce;
        });
    if (lease) {
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return result;
}

DirectMessageLease Manager::LeaseCapacityProofs(
    const PaymasterId& provider_id,
    const uint256& client_nonce,
    size_t maximum)
{
    return DirectMessageLease{
        *this,
        PeekCapacityProofs(provider_id, client_nonce, maximum, /*lease=*/true)};
}

std::vector<DirectMessage> Manager::TakeQuoteResponses(const std::string& request_id,
                                                       const uint256& session_id,
                                                       const PaymasterId& provider_id,
                                                       const uint256& intent_hash,
                                                       size_t maximum)
{
    std::vector<DirectMessage> result;
    if (request_id.empty() || session_id.IsNull() || provider_id.IsNull() ||
        intent_hash.IsNull() || maximum == 0) {
        return result;
    }
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [&request_id, &session_id, &provider_id,
                             &intent_hash](const DirectMessage& message) {
                                const auto* response = std::get_if<PaymasterQuoteResponse>(&message.payload);
                                return response != nullptr && response->request_id == request_id &&
                                       response->session_id == session_id &&
                                       response->quote.provider_id == provider_id &&
                                       response->quote.intent_hash == intent_hash;
                            });
}

std::vector<DirectMessage> Manager::PeekQuoteResponses(
    const std::string& request_id,
    const uint256& session_id,
    const PaymasterId& provider_id,
    const uint256& intent_hash,
    size_t maximum,
    bool lease)
{
    std::vector<DirectMessage> result;
    if (request_id.empty() || session_id.IsNull() || provider_id.IsNull() ||
        intent_hash.IsNull() || maximum == 0) {
        return result;
    }
    LOCK(m_direct_mutex);
    result = PeekMatchingFair(
        m_direct_messages, m_last_inbound_group,
        m_last_inbound_session_by_group, maximum,
        [&request_id, &session_id, &provider_id,
         &intent_hash](const DirectMessage& message) {
            const auto* response =
                std::get_if<PaymasterQuoteResponse>(&message.payload);
            return response != nullptr &&
                   response->request_id == request_id &&
                   response->session_id == session_id &&
                   response->quote.provider_id == provider_id &&
                   response->quote.intent_hash == intent_hash;
        });
    if (lease) {
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return result;
}

DirectMessageLease Manager::LeaseQuoteResponses(
    const std::string& request_id,
    const uint256& session_id,
    const PaymasterId& provider_id,
    const uint256& intent_hash,
    size_t maximum)
{
    return DirectMessageLease{
        *this, PeekQuoteResponses(request_id, session_id, provider_id,
                                  intent_hash, maximum, /*lease=*/true)};
}

std::vector<DirectMessage> Manager::TakeQuoteRequests(const PaymasterId& provider_id,
                                                      size_t maximum)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || maximum == 0) return result;
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [&provider_id](const DirectMessage& message) {
                                const auto* request = std::get_if<PaymasterQuoteRequest>(&message.payload);
                                return request != nullptr && request->intent.provider_id == provider_id;
                            });
}

DirectMessageLease Manager::LeaseQuoteRequests(
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (!provider_id.IsNull() && maximum != 0) {
        LOCK(m_direct_mutex);
        result = PeekMatchingFair(
            m_direct_messages, m_last_inbound_group,
            m_last_inbound_session_by_group, maximum,
            [&provider_id](const DirectMessage& message) {
                const auto* request =
                    std::get_if<PaymasterQuoteRequest>(&message.payload);
                return request != nullptr &&
                       request->intent.provider_id == provider_id;
            });
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return DirectMessageLease{*this, std::move(result)};
}

std::vector<DirectMessage> Manager::TakeSubmits(const PaymasterId& provider_id,
                                                size_t maximum)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || maximum == 0) return result;
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [&provider_id](const DirectMessage& message) {
                                const auto* submit = std::get_if<PaymasterSubmit>(&message.payload);
                                return submit != nullptr && submit->provider_id == provider_id;
                            });
}

DirectMessageLease Manager::LeaseSubmits(const PaymasterId& provider_id,
                                         size_t maximum)
{
    std::vector<DirectMessage> result;
    if (!provider_id.IsNull() && maximum != 0) {
        LOCK(m_direct_mutex);
        result = PeekMatchingFair(
            m_direct_messages, m_last_inbound_group,
            m_last_inbound_session_by_group, maximum,
            [&provider_id](const DirectMessage& message) {
                const auto* submit =
                    std::get_if<PaymasterSubmit>(&message.payload);
                return submit != nullptr &&
                       submit->provider_id == provider_id;
            });
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return DirectMessageLease{*this, std::move(result)};
}

std::vector<DirectMessage> Manager::TakeResults(const std::string& request_id,
                                                const uint256& session_id,
                                                const PaymasterId& provider_id,
                                                size_t maximum)
{
    std::vector<DirectMessage> result;
    if (request_id.empty() || session_id.IsNull() || provider_id.IsNull() || maximum == 0) {
        return result;
    }
    LOCK(m_direct_mutex);
    return TakeMatchingFair(m_direct_messages, m_direct_bytes,
                            m_leased_direct_ids, m_last_inbound_group,
                            m_last_inbound_session_by_group, maximum,
                            [&request_id, &session_id, &provider_id](const DirectMessage& direct) {
                                const auto* message = std::get_if<PaymasterResultMessage>(&direct.payload);
                                return message != nullptr && message->request_id == request_id &&
                                       message->session_id == session_id &&
                                       message->result.provider_id == provider_id;
                            });
}

std::vector<DirectMessage> Manager::TakeRecoveryRequests(
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || maximum == 0) return result;
    LOCK(m_direct_mutex);
    return TakeMatchingFair(
        m_direct_messages, m_direct_bytes, m_leased_direct_ids,
        m_last_inbound_group,
        m_last_inbound_session_by_group, maximum,
        [&provider_id](const DirectMessage& direct) {
            const auto* message =
                std::get_if<AlternativeRecoveryRequest>(&direct.payload);
            return message != nullptr &&
                   message->recovery_provider_id == provider_id;
        });
}

DirectMessageLease Manager::LeaseRecoveryRequests(
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (!provider_id.IsNull() && maximum != 0) {
        LOCK(m_direct_mutex);
        result = PeekMatchingFair(
            m_direct_messages, m_last_inbound_group,
            m_last_inbound_session_by_group, maximum,
            [&provider_id](const DirectMessage& direct) {
                const auto* message =
                    std::get_if<AlternativeRecoveryRequest>(&direct.payload);
                return message != nullptr &&
                       message->recovery_provider_id == provider_id;
            });
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return DirectMessageLease{*this, std::move(result)};
}

std::vector<DirectMessage> Manager::TakeRecoveryResponses(
    const std::string& request_id, const uint256& session_id,
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (request_id.empty() || session_id.IsNull() || provider_id.IsNull() ||
        maximum == 0) {
        return result;
    }
    LOCK(m_direct_mutex);
    return TakeMatchingFair(
        m_direct_messages, m_direct_bytes, m_leased_direct_ids,
        m_last_inbound_group,
        m_last_inbound_session_by_group, maximum,
        [&](const DirectMessage& direct) {
            const auto* message =
                std::get_if<AlternativeRecoveryResponse>(&direct.payload);
            return message != nullptr && message->request_id == request_id &&
                   message->session_id == session_id &&
                   message->recovery_provider_id == provider_id;
        });
}

std::vector<DirectMessage> Manager::TakeRecoverySubmits(
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (provider_id.IsNull() || maximum == 0) return result;
    LOCK(m_direct_mutex);
    return TakeMatchingFair(
        m_direct_messages, m_direct_bytes, m_leased_direct_ids,
        m_last_inbound_group,
        m_last_inbound_session_by_group, maximum,
        [&](const DirectMessage& direct) {
            const auto* message =
                std::get_if<AlternativeRecoverySubmit>(&direct.payload);
            return message != nullptr &&
                   message->recovery_provider_id == provider_id;
        });
}

DirectMessageLease Manager::LeaseRecoverySubmits(
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (!provider_id.IsNull() && maximum != 0) {
        LOCK(m_direct_mutex);
        result = PeekMatchingFair(
            m_direct_messages, m_last_inbound_group,
            m_last_inbound_session_by_group, maximum,
            [&provider_id](const DirectMessage& direct) {
                const auto* message =
                    std::get_if<AlternativeRecoverySubmit>(&direct.payload);
                return message != nullptr &&
                       message->recovery_provider_id == provider_id;
            });
        AddDirectMessageLeases(m_leased_direct_ids, result);
    }
    return DirectMessageLease{*this, std::move(result)};
}

std::vector<DirectMessage> Manager::TakeRecoveryResults(
    const std::string& request_id, const uint256& session_id,
    const PaymasterId& provider_id, size_t maximum)
{
    std::vector<DirectMessage> result;
    if (request_id.empty() || session_id.IsNull() || provider_id.IsNull() ||
        maximum == 0) {
        return result;
    }
    LOCK(m_direct_mutex);
    return TakeMatchingFair(
        m_direct_messages, m_direct_bytes, m_leased_direct_ids,
        m_last_inbound_group,
        m_last_inbound_session_by_group, maximum,
        [&](const DirectMessage& direct) {
            const auto* message =
                std::get_if<AlternativeRecoveryResultMessage>(&direct.payload);
            return message != nullptr && message->request_id == request_id &&
                   message->session_id == session_id &&
                   message->result.provider_id == provider_id;
        });
}

bool Manager::QueueOutboundDirectMessage(int64_t peer_id,
                                         const uint256& message_id,
                                         size_t serialized_size,
                                         DirectPayload payload,
                                         int64_t now)
{
    if (!Enabled() || peer_id < 0 || message_id.IsNull() || serialized_size == 0 ||
        serialized_size > MAX_DIRECT_MESSAGE_BYTES || now <= 0) return false;
    LOCK(m_direct_mutex);
    if (!Enabled()) return false;
    PruneOutboundMessages(now);
    for (auto it = m_outbound_direct_messages.begin(); it != m_outbound_direct_messages.end();) {
        const bool superseded = it->message_id == message_id && it->peer_id != peer_id;
        if (superseded) {
            m_outbound_direct_bytes -= it->serialized_size;
            m_outbound_direct_ids.erase(std::pair{it->peer_id, it->message_id});
            it = m_outbound_direct_messages.erase(it);
        } else {
            ++it;
        }
    }
    const auto outbound_key = std::pair{peer_id, message_id};
    // Returning success here means the exact artifact is already in flight.
    // The caller may continue reporting "queued" without multiplying network
    // work. A retry after the short delay, or on a replacement peer, is still
    // enqueued normally.
    if (m_recent_outbound_direct_ids.count(outbound_key) != 0) return true;
    size_t peer_messages{0};
    size_t peer_bytes{0};
    for (const OutboundDirectMessage& queued : m_outbound_direct_messages) {
        if (queued.peer_id == peer_id) {
            ++peer_messages;
            peer_bytes += queued.serialized_size;
        }
    }
    if (m_outbound_direct_ids.count(outbound_key) != 0 ||
        m_outbound_direct_messages.size() >= MAX_DIRECT_INBOX_MESSAGES ||
        serialized_size > MAX_DIRECT_INBOX_BYTES - m_outbound_direct_bytes ||
        peer_messages >= MAX_DIRECT_INBOX_MESSAGES_PER_PEER ||
        serialized_size > MAX_DIRECT_INBOX_BYTES_PER_PEER - peer_bytes) {
        return false;
    }
    m_outbound_direct_ids.insert(outbound_key);
    m_outbound_direct_bytes += serialized_size;
    m_outbound_direct_messages.push_back(
        OutboundDirectMessage{peer_id, message_id, now, serialized_size, std::move(payload)});
    return true;
}

bool Manager::QueueCapacityRequest(int64_t peer_id,
                                   const uint256& message_id,
                                   size_t serialized_size,
                                   PaymasterCapacityRequest request,
                                   int64_t now)
{
    return QueueOutboundDirectMessage(peer_id, message_id, serialized_size,
                                      DirectPayload{std::move(request)}, now);
}

bool Manager::QueueCapacityProof(int64_t peer_id,
                                 const uint256& message_id,
                                 size_t serialized_size,
                                 PaymasterCapacityProof proof,
                                 int64_t now)
{
    return QueueOutboundDirectMessage(peer_id, message_id, serialized_size,
                                      DirectPayload{std::move(proof)}, now);
}

std::vector<OutboundDirectMessage> Manager::TakeOutboundDirectMessages(
    int64_t peer_id,
    size_t maximum,
    int64_t now)
{
    std::vector<OutboundDirectMessage> result;
    if (peer_id < 0 || maximum == 0 || now <= 0) return result;
    LOCK(m_direct_mutex);
    PruneOutboundMessages(now);
    result.reserve(std::min(maximum, m_outbound_direct_messages.size()));
    for (auto it = m_outbound_direct_messages.begin(); it != m_outbound_direct_messages.end();) {
        const bool selected = it->peer_id == peer_id && result.size() < maximum;
        if (selected) {
            m_outbound_direct_bytes -= it->serialized_size;
            m_outbound_direct_ids.erase(std::pair{it->peer_id, it->message_id});
            m_recent_outbound_direct_ids[std::pair{it->peer_id,
                                                   it->message_id}] = now;
            result.push_back(std::move(*it));
            it = m_outbound_direct_messages.erase(it);
        } else {
            ++it;
        }
    }
    return result;
}

void Manager::PruneDirectMessages(int64_t now)
{
    if (now <= 0) return;
    LOCK(m_direct_mutex);
    PruneReplays(now, /*retain_equivocation_candidates=*/false);
    PruneInboundMessages(now, /*retain_equivocation_candidates=*/false);
    PruneOutboundMessages(now);
}

void Manager::ClearDirectMessages()
{
    {
        LOCK(m_direct_mutex);
        m_direct_messages.clear();
        m_direct_replays.clear();
        m_leased_direct_ids.clear();
        m_direct_bytes = 0;
        m_last_inbound_group.reset();
        m_last_inbound_session_by_group.clear();
        m_outbound_direct_messages.clear();
        m_outbound_direct_ids.clear();
        m_recent_outbound_direct_ids.clear();
        m_outbound_direct_bytes = 0;
    }
    ClearRateLimits();
}

bool Manager::AcknowledgeDirectMessages(
    const std::vector<uint256>& message_ids)
{
    return AcknowledgeDirectMessagesInternal(message_ids,
                                             /*allow_absent=*/false);
}

bool Manager::AcknowledgeDirectMessagesIfPresent(
    const std::vector<uint256>& message_ids)
{
    return AcknowledgeDirectMessagesInternal(message_ids,
                                             /*allow_absent=*/true);
}

bool Manager::AcknowledgeDirectMessagesInternal(
    const std::vector<uint256>& message_ids,
    bool allow_absent)
{
    if (message_ids.empty() ||
        message_ids.size() > MAX_DIRECT_INBOX_MESSAGES ||
        std::any_of(message_ids.begin(), message_ids.end(),
                    [](const uint256& message_id) {
                        return message_id.IsNull();
                    })) {
        return false;
    }

    LOCK(m_direct_mutex);
    const std::set<uint256> unique_ids{message_ids.begin(), message_ids.end()};
    if (unique_ids.size() != message_ids.size()) return false;

    struct AcknowledgedCursor {
        uint64_t group;
        uint256 session;
    };
    std::vector<AcknowledgedCursor> cursors;
    cursors.reserve(message_ids.size());
    for (const uint256& message_id : message_ids) {
        const auto message = std::find_if(
            m_direct_messages.begin(), m_direct_messages.end(),
            [&message_id](const DirectMessage& queued) {
                return queued.message_id == message_id;
            });
        if (message == m_direct_messages.end()) {
            if (allow_absent) continue;
            return false;
        }
        if (std::find_if(std::next(message), m_direct_messages.end(),
                         [&message_id](const DirectMessage& queued) {
                             return queued.message_id == message_id;
                         }) != m_direct_messages.end()) {
            return false;
        }
        cursors.push_back({GetDirectFairGroup(*message),
                           GetDirectSessionKey(message->payload)});
    }

    for (const AcknowledgedCursor& cursor : cursors) {
        m_last_inbound_group = cursor.group;
        m_last_inbound_session_by_group[cursor.group] = cursor.session;
    }
    for (auto message = m_direct_messages.begin();
         message != m_direct_messages.end();) {
        if (unique_ids.count(message->message_id) == 0) {
            ++message;
            continue;
        }
        m_direct_bytes -= message->serialized_size;
        m_leased_direct_ids.erase(message->message_id);
        message = m_direct_messages.erase(message);
    }
    for (const uint256& message_id : unique_ids) {
        m_leased_direct_ids.erase(message_id);
    }
    for (const AcknowledgedCursor& cursor : cursors) {
        if (std::none_of(
                m_direct_messages.begin(), m_direct_messages.end(),
                [&cursor](const DirectMessage& message) {
                    return GetDirectFairGroup(message) == cursor.group;
                })) {
            m_last_inbound_session_by_group.erase(cursor.group);
        }
    }
    if (m_direct_messages.empty()) {
        m_last_inbound_group.reset();
        m_last_inbound_session_by_group.clear();
    }
    return true;
}

void Manager::ReleaseDirectMessageLeases(
    const std::vector<DirectMessage>& messages) noexcept
{
    LOCK(m_direct_mutex);
    for (const DirectMessage& message : messages) {
        const auto lease = m_leased_direct_ids.find(message.message_id);
        if (lease == m_leased_direct_ids.end() ||
            lease->second.first != message.queue_sequence) {
            continue;
        }
        if (--lease->second.second == 0) m_leased_direct_ids.erase(lease);
    }
}

DirectMessageLease::DirectMessageLease(
    Manager& manager,
    std::vector<DirectMessage>&& messages) noexcept
    : m_manager{&manager}, m_messages{std::move(messages)}
{
}

DirectMessageLease::DirectMessageLease(DirectMessageLease&& other) noexcept
    : m_manager{std::exchange(other.m_manager, nullptr)},
      m_messages{std::move(other.m_messages)}
{
}

DirectMessageLease& DirectMessageLease::operator=(
    DirectMessageLease&& other) noexcept
{
    if (this == &other) return *this;
    Release();
    m_manager = std::exchange(other.m_manager, nullptr);
    m_messages = std::move(other.m_messages);
    return *this;
}

DirectMessageLease::~DirectMessageLease()
{
    Release();
}

void DirectMessageLease::Release() noexcept
{
    if (!m_manager) return;
    m_manager->ReleaseDirectMessageLeases(m_messages);
    m_manager = nullptr;
}

bool Manager::HasDirectMessages() const
{
    LOCK(m_direct_mutex);
    return !m_direct_messages.empty();
}

bool Manager::HasEquivocationCandidates() const
{
    LOCK(m_direct_mutex);
    return std::any_of(
        m_direct_messages.begin(), m_direct_messages.end(),
        [](const DirectMessage& message) {
            return std::holds_alternative<PaymasterCapacityProof>(
                       message.payload) ||
                   std::holds_alternative<PaymasterQuoteResponse>(
                       message.payload);
        });
}

size_t Manager::DirectMessageCount() const
{
    LOCK(m_direct_mutex);
    return m_direct_messages.size();
}

size_t Manager::OutboundDirectMessageCount() const
{
    LOCK(m_direct_mutex);
    return m_outbound_direct_messages.size();
}

bool Manager::HasOutboundDirectMessage(int64_t peer_id, const uint256& message_id) const
{
    LOCK(m_direct_mutex);
    return m_outbound_direct_ids.count(std::pair{peer_id, message_id}) != 0;
}

bool Manager::StartProvider(const std::string& wallet_name, const PaymasterId& provider_id)
{
    if (!Enabled() || provider_id.IsNull()) return false;
    LOCK(m_provider_mutex);
    const auto existing = m_running_providers.find(wallet_name);
    if (existing != m_running_providers.end()) {
        return existing->second == provider_id;
    }
    if (m_provider_work.count(wallet_name) != 0) return false;
    return m_running_providers.emplace(wallet_name, provider_id).second;
}

void Manager::StopProvider(const std::string& wallet_name)
{
    LOCK(m_provider_mutex);
    m_running_providers.erase(wallet_name);
    m_provider_service_status[wallet_name] =
        ProviderServiceStatus{ProviderServiceState::STOPPED, {}};
}

bool Manager::IsProviderRunning(const std::string& wallet_name,
                                const PaymasterId& provider_id) const
{
    LOCK(m_provider_mutex);
    const auto it = m_running_providers.find(wallet_name);
    return it != m_running_providers.end() && it->second == provider_id;
}

size_t Manager::RunningProviderCount() const
{
    LOCK(m_provider_mutex);
    return m_running_providers.size();
}

bool Manager::TryBeginProviderWork(const std::string& wallet_name,
                                   const PaymasterId& provider_id,
                                   bool require_running)
{
    if (!Enabled() || wallet_name.empty() || provider_id.IsNull()) return false;
    LOCK(m_provider_mutex);
    const auto running = m_running_providers.find(wallet_name);
    if ((require_running && running == m_running_providers.end()) ||
        (running != m_running_providers.end() &&
         running->second != provider_id)) {
        return false;
    }
    return m_provider_work.insert(wallet_name).second;
}

void Manager::EndProviderWork(const std::string& wallet_name)
{
    LOCK(m_provider_mutex);
    m_provider_work.erase(wallet_name);
}

void Manager::SetProviderServiceStatus(const std::string& wallet_name,
                                       ProviderServiceState state,
                                       std::string last_error)
{
    if (wallet_name.empty()) return;
    LOCK(m_provider_mutex);
    m_provider_service_status[wallet_name] =
        ProviderServiceStatus{state, std::move(last_error)};
}

ProviderServiceStatus Manager::GetProviderServiceStatus(
    const std::string& wallet_name) const
{
    LOCK(m_provider_mutex);
    const auto status = m_provider_service_status.find(wallet_name);
    return status == m_provider_service_status.end()
               ? ProviderServiceStatus{}
               : status->second;
}

ProviderQueueStatus Manager::GetProviderQueueStatus(
    const PaymasterId& provider_id) const
{
    ProviderQueueStatus status;
    if (provider_id.IsNull()) return status;

    LOCK(m_direct_mutex);
    for (const DirectMessage& direct : m_direct_messages) {
        if (const auto* request =
                std::get_if<PaymasterCapacityRequest>(&direct.payload)) {
            status.waiting_requests += request->provider_id == provider_id;
        } else if (const auto* request =
                       std::get_if<PaymasterQuoteRequest>(&direct.payload)) {
            status.waiting_requests +=
                request->intent.provider_id == provider_id;
        } else if (const auto* request =
                       std::get_if<AlternativeRecoveryRequest>(
                           &direct.payload)) {
            status.waiting_requests +=
                request->recovery_provider_id == provider_id;
        } else if (const auto* submit =
                       std::get_if<PaymasterSubmit>(&direct.payload)) {
            status.waiting_submits += submit->provider_id == provider_id;
        } else if (const auto* submit =
                       std::get_if<AlternativeRecoverySubmit>(
                           &direct.payload)) {
            status.waiting_submits +=
                submit->recovery_provider_id == provider_id;
        }
    }
    return status;
}

} // namespace DigiDollar::Paymaster
