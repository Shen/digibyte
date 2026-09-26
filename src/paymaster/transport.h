// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_PAYMASTER_TRANSPORT_H
#define DIGIBYTE_PAYMASTER_TRANSPORT_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace DigiDollar::Paymaster {

/** Process-local transport authority. None of these objects authorize a
 * signature, extend a protocol expiry, or release a financial reservation. */
static constexpr int DEFAULT_DIRECT_OUTBOUND{1};
static constexpr int MAX_DIRECT_OUTBOUND{4};
static constexpr int MAX_DIRECT_INBOUND{16};
static constexpr int MAX_DIRECT_HANDSHAKES{8};
static constexpr int64_t DIRECT_WAIT_MS{30'000};
static constexpr int64_t DIRECT_HANDSHAKE_MS{30'000};
static constexpr int64_t DIRECT_IDLE_MS{30'000};
static constexpr int64_t DIRECT_LIFETIME_MS{120'000};
static constexpr int64_t DIRECT_ADMISSION_GRACE_MS{5'000};
static constexpr int64_t DIRECT_FIRST_REQUEST_MS{10'000};
static constexpr int MAX_DIRECT_PER_GROUP{4};

/** Caller supplies monotonic milliseconds and synchronization. Rejected work
 * cannot refill a bucket, and a backward clock cannot renew its allowance. */
class DirectRateBucket
{
    uint64_t m_capacity;
    int64_t m_period;
    uint64_t m_units;
    int64_t m_last{-1};
public:
    DirectRateBucket(uint64_t capacity, int64_t period)
        : m_capacity(capacity), m_period(period), m_units(capacity * period) {}
    bool CanConsume(uint64_t amount, int64_t now) const
    {
        auto copy = *this;
        return copy.Consume(amount, now);
    }
    bool Consume(uint64_t amount, int64_t now)
    {
        if (now < 0 || amount > m_capacity) return false;
        const uint64_t maximum = m_capacity * m_period;
        if (m_last < 0) m_last = now;
        now = std::max(now, m_last);
        const int64_t elapsed = now - m_last;
        m_units = elapsed >= m_period ? maximum :
            std::min(maximum, m_units + uint64_t(elapsed) * m_capacity);
        m_last = now;
        const uint64_t needed = amount * m_period;
        if (needed > m_units) return false;
        m_units -= needed;
        return true;
    }
};

/** Admission happens before CNode/v2 allocation. Onion forwarding has no
 * trustworthy source identity, so it uses the global ceiling, never localhost
 * as a shared client identity. Clearnet groups retain reconnect history. */
class DirectAdmission
{
    struct Group {
        DirectRateBucket rate{4, 20'000};
        int64_t last{0};
    };
    std::mutex m_mutex;
    DirectRateBucket m_global{16, 16'000};
    std::map<uint64_t, Group> m_groups;
public:
    bool Admit(std::optional<uint64_t> group, int64_t now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Rejected global attempts allocate no attacker-chosen group state.
        if (!m_global.CanConsume(1, now)) return false;
        if (group) {
            for (auto it = m_groups.begin(); it != m_groups.end();) {
                if (now - it->second.last >= 60'000) it = m_groups.erase(it);
                else ++it;
            }
            auto it = m_groups.find(*group);
            if (it == m_groups.end()) {
                if (m_groups.size() >= 128) return false;
                it = m_groups.emplace(*group, Group{}).first;
            }
            // Only accepted source attempts consume global work capacity.
            if (!it->second.rate.Consume(1, now)) return false;
            it->second.last = std::max(now, it->second.last);
        }
        return m_global.Consume(1, now);
    }
};

inline int64_t DirectNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct DirectBudget {
    int outbound{0};
    int inbound{0};
    int handshakes{0};
    int ordinary_inbound{0};

    static DirectBudget Calculate(int total, int ordinary_outbound,
                                  int requested_outbound, int requested_inbound,
                                  bool listener)
    {
        DirectBudget result;
        int available = std::max(0, total - ordinary_outbound);
        if (requested_outbound >= 0 && requested_outbound <= MAX_DIRECT_OUTBOUND &&
            available >= requested_outbound) {
            result.outbound = requested_outbound;
            available -= result.outbound;
        }
        const int handshakes = std::min(MAX_DIRECT_HANDSHAKES, requested_inbound);
        // Leave at least one ordinary inbound slot on a provider node.
        if (listener && requested_inbound > 0 && requested_inbound <= MAX_DIRECT_INBOUND &&
            available >= requested_inbound + handshakes + 1) {
            result.inbound = requested_inbound;
            result.handshakes = handshakes;
            available -= result.inbound + result.handshakes;
        }
        result.ordinary_inbound = available;
        return result;
    }
};

enum class DirectState { DISABLED, QUEUED, CONNECTING, HANDSHAKING, READY, FULL, EXPIRED, FAILED, CANCELED, NETWORK_INACTIVE, PRIVACY_REJECTED, CONNECT_FAILED };

inline const char* DirectStateName(DirectState state)
{
    switch (state) {
    case DirectState::DISABLED: return "disabled";
    case DirectState::QUEUED: return "waiting_capacity";
    case DirectState::CONNECTING: return "connecting";
    case DirectState::HANDSHAKING: return "handshaking";
    case DirectState::READY: return "direct_ready";
    case DirectState::FULL: return "queue_full";
    case DirectState::EXPIRED: return "expired";
    case DirectState::FAILED: return "failed";
    case DirectState::CANCELED: return "canceled";
    case DirectState::NETWORK_INACTIVE: return "network_inactive";
    case DirectState::PRIVACY_REJECTED: return "privacy_rejected";
    case DirectState::CONNECT_FAILED: return "connect_failed";
    }
    return "failed";
}

/** owner is a random wallet-load identifier, never a wallet name. operation
 * binds a request/provider/phase (or recovery); endpoints alone are not keys. */
struct DirectKey {
    std::string owner;
    std::string operation;
    bool recovery{false};
    friend bool operator<(const DirectKey& a, const DirectKey& b)
    {
        return std::tie(a.owner, a.operation, a.recovery) < std::tie(b.owner, b.operation, b.recovery);
    }
    friend bool operator==(const DirectKey& a, const DirectKey& b)
    {
        return a.owner == b.owner && a.operation == b.operation && a.recovery == b.recovery;
    }
};

/** Shared lifetime keeps counters valid even after the connection manager has
 * shut down. Destruction is the only release path for an acquired permit. */
class DirectPermits
{
    struct State {
        std::mutex mutex;
        int outgoing{0}, incoming{0}, pending{0};
        DirectBudget budget;
        std::map<uint64_t, int> groups;
    };
    std::shared_ptr<State> m_state{std::make_shared<State>()};

public:
    enum class Kind { OUTBOUND, HANDSHAKE, INBOUND };
    class Permit {
        friend class DirectPermits;
        std::shared_ptr<State> m_state;
        Kind m_kind;
        std::optional<uint64_t> m_group;
        bool m_live{true};
    public:
        Permit(std::shared_ptr<State> state, Kind kind, std::optional<uint64_t> group = {})
            : m_state(std::move(state)), m_kind(kind), m_group(group) {}
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        ~Permit() { Release(); }
        /** May be called early only after the owning socket is closed. */
        void Release()
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (!m_live) return;
            m_live = false;
            if (m_kind == Kind::OUTBOUND) --m_state->outgoing;
            else if (m_kind == Kind::HANDSHAKE) --m_state->pending;
            else --m_state->incoming;
            if (m_group && --m_state->groups.at(*m_group) == 0) m_state->groups.erase(*m_group);
        }
        bool Promote()
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (!m_live) return false;
            if (m_kind == Kind::INBOUND) return true;
            if (m_kind != Kind::HANDSHAKE || m_state->incoming >= m_state->budget.inbound) return false;
            --m_state->pending;
            ++m_state->incoming;
            m_kind = Kind::INBOUND;
            return true;
        }
    };
    void Configure(DirectBudget budget)
    {
        // Called only before network threads start; old permits keep their
        // original state if a test reinitializes a stopped manager.
        m_state = std::make_shared<State>();
        m_state->budget = budget;
    }
    std::shared_ptr<Permit> Acquire(Kind kind, std::optional<uint64_t> group = {})
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        int& used = kind == Kind::OUTBOUND ? m_state->outgoing : kind == Kind::HANDSHAKE ? m_state->pending : m_state->incoming;
        const int limit = kind == Kind::OUTBOUND ? m_state->budget.outbound : kind == Kind::HANDSHAKE ? m_state->budget.handshakes : m_state->budget.inbound;
        if (used >= limit) return {};
        if (kind == Kind::OUTBOUND) group.reset();
        if (group) {
            const auto it = m_state->groups.find(*group);
            if (it != m_state->groups.end() && it->second >= MAX_DIRECT_PER_GROUP) return {};
        }
        // Any map allocation precedes permit ownership and counter increments.
        if (group) m_state->groups.try_emplace(*group, 0);
        auto permit = std::make_shared<Permit>(m_state, kind, group);
        if (group) ++m_state->groups.at(*group);
        ++used;
        return permit;
    }
    bool AtGroupLimit(std::optional<uint64_t> group) const
    {
        if (!group) return false;
        std::lock_guard<std::mutex> lock(m_state->mutex);
        const auto it = m_state->groups.find(*group);
        return it != m_state->groups.end() && it->second >= MAX_DIRECT_PER_GROUP;
    }
    struct Usage { int outgoing, incoming, handshakes; };
    Usage GetUsage() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return {m_state->outgoing, m_state->incoming, m_state->pending};
    }
};

struct DirectLease {
    DirectKey key;
    std::string endpoint;
    bool high_privacy{false};
    int64_t started{0};
    std::shared_ptr<DirectPermits::Permit> permit;
    std::atomic<DirectState> state{DirectState::CONNECTING};
    std::atomic<int64_t> peer_id{-1};
    std::atomic<int64_t> progress{0};
    std::atomic_bool canceled{false};
};

struct DirectResult {
    DirectState state{DirectState::FAILED};
    std::shared_ptr<DirectLease> lease;
    bool Pending() const
    {
        return state == DirectState::QUEUED || state == DirectState::CONNECTING || state == DirectState::HANDSHAKING;
    }
};

/** Bounded, poll-idempotent local queue. Socket workers claim permits; RPC
 * callers never block under a wallet lock. Fairness is by wallet then FIFO,
 * alternating fresh work and recovery whenever both classes have work. */
class DirectQueue
{
    struct Pending {
        DirectKey key;
        std::string endpoint;
        bool high_privacy;
        int64_t expires;
    };
    struct Outcome { DirectKey key; DirectState state; };
    mutable std::mutex m_mutex;
    std::deque<Pending> m_pending;
    std::map<DirectKey, std::weak_ptr<DirectLease>> m_active;
    // Bounded failure tombstones stop status polling from renewing a ticket.
    std::deque<Outcome> m_outcomes;
    std::string m_last_wallet[2];
    bool m_next_recovery{false};
    bool m_stopped{false};

    void Remember(const DirectKey& key, DirectState state)
    {
        m_outcomes.erase(std::remove_if(m_outcomes.begin(), m_outcomes.end(), [&](const auto& entry) { return entry.key == key; }), m_outcomes.end());
        m_outcomes.push_back({key, state});
    }
    void Prune(int64_t now)
    {
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (now >= it->expires) {
                Remember(it->key, DirectState::EXPIRED);
                it = m_pending.erase(it);
            } else ++it;
        }
        for (auto it = m_active.begin(); it != m_active.end();) {
            if (it->second.expired()) it = m_active.erase(it);
            else ++it;
        }
    }

public:
    DirectResult Request(const DirectKey& key, const std::string& endpoint, bool high_privacy, int64_t now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Prune(now);
        if (m_stopped || key.owner.empty() || key.operation.empty()) return {DirectState::CANCELED, {}};
        const auto active = m_active.find(key);
        if (active != m_active.end()) {
            auto lease = active->second.lock();
            if (lease) {
                if (lease->canceled) return {DirectState::QUEUED, {}};
                if (lease->endpoint != endpoint || lease->high_privacy != high_privacy) return {DirectState::FAILED, {}};
                return {lease->state.load(), std::move(lease)};
            }
        }
        for (const auto& entry : m_outcomes) if (entry.key == key) return {entry.state, {}};
        size_t wallet_count{0}, class_count{0};
        for (const auto& entry : m_pending) {
            if (entry.key == key) {
                return {entry.endpoint == endpoint && entry.high_privacy == high_privacy ? DirectState::QUEUED : DirectState::FAILED, {}};
            }
            wallet_count += entry.key.owner == key.owner;
            class_count += entry.key.recovery == key.recovery;
        }
        if (wallet_count >= 8 || class_count >= (key.recovery ? 8U : 24U) ||
            m_outcomes.size() + m_pending.size() + m_active.size() >= (key.recovery ? 256U : 248U)) return {DirectState::FULL, {}};
        m_pending.push_back({key, endpoint, high_privacy, now + DIRECT_WAIT_MS});
        return {DirectState::QUEUED, {}};
    }

    std::shared_ptr<DirectLease> Claim(DirectPermits& permits, int64_t now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Prune(now);
        if (m_stopped || m_pending.empty()) return {};
        auto permit = permits.Acquire(DirectPermits::Kind::OUTBOUND);
        if (!permit) return {};
        bool recovery = m_next_recovery;
        if (std::none_of(m_pending.begin(), m_pending.end(), [&](const auto& p) { return p.key.recovery == recovery; })) recovery = !recovery;
        auto selected = m_pending.end();
        for (int wrap = 0; wrap < 2 && selected == m_pending.end(); ++wrap) {
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                if (it->key.recovery != recovery || (!wrap && it->key.owner <= m_last_wallet[recovery])) continue;
                if (selected == m_pending.end() || it->key.owner < selected->key.owner) selected = it;
            }
        }
        if (selected == m_pending.end()) return {};
        auto lease = std::make_shared<DirectLease>();
        lease->key = selected->key;
        lease->endpoint = selected->endpoint;
        lease->high_privacy = selected->high_privacy;
        lease->started = now;
        lease->progress = now;
        lease->permit = std::move(permit);
        m_active[lease->key] = lease;
        m_last_wallet[recovery] = lease->key.owner;
        m_next_recovery = !recovery;
        m_pending.erase(selected);
        return lease;
    }

    void Finish(const std::shared_ptr<DirectLease>& lease, DirectState state)
    {
        if (!lease) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        // An old socket must not overwrite the status of its replacement.
        const auto it = m_active.find(lease->key);
        if (it == m_active.end() || it->second.lock() != lease) return;
        lease->state = state;
        // Finish is called after socket close or failed connect. Any RPC still
        // using the lease retains the permit; its replacement can only queue.
        m_active.erase(it);
        if (!lease->canceled) Remember(lease->key, state);
    }

    void Release(const DirectKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const auto& p) { return p.key == key; }), m_pending.end());
        const auto it = m_active.find(key);
        if (it != m_active.end()) if (auto lease = it->second.lock()) lease->canceled = true;
        m_outcomes.erase(std::remove_if(m_outcomes.begin(), m_outcomes.end(), [&](const auto& p) { return p.key == key; }), m_outcomes.end());
    }

    // Explicit user retry only: polling a healthy or pending request is inert.
    void Retry(const DirectKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outcomes.erase(std::remove_if(m_outcomes.begin(), m_outcomes.end(), [&](const auto& p) { return p.key == key; }), m_outcomes.end());
        const auto it = m_active.find(key);
        if (it != m_active.end()) {
            if (auto lease = it->second.lock()) {
                const auto state = lease->state.load();
                if (state == DirectState::FAILED || state == DirectState::EXPIRED || state == DirectState::CONNECT_FAILED) lease->canceled = true;
            }
        }
    }

    void CancelOperation(const std::string& owner, const std::string& prefix)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto matches = [&](const DirectKey& key) {
            return key.owner == owner && key.operation.compare(0, prefix.size(), prefix) == 0;
        };
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const auto& p) { return matches(p.key); }), m_pending.end());
        for (auto& [key, weak] : m_active) if (matches(key)) if (auto lease = weak.lock()) lease->canceled = true;
        m_outcomes.erase(std::remove_if(m_outcomes.begin(), m_outcomes.end(), [&](const auto& p) { return matches(p.key); }), m_outcomes.end());
    }

    void CancelOwner(const std::string& owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const auto& p) { return p.key.owner == owner; }), m_pending.end());
        for (auto& [key, weak] : m_active) if (key.owner == owner) if (auto lease = weak.lock()) lease->canceled = true;
        m_outcomes.erase(std::remove_if(m_outcomes.begin(), m_outcomes.end(), [&](const auto& p) { return p.key.owner == owner; }), m_outcomes.end());
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true;
        m_pending.clear();
        for (auto& [key, weak] : m_active) if (auto lease = weak.lock()) lease->canceled = true;
    }
    size_t Size() const { std::lock_guard<std::mutex> lock(m_mutex); return m_pending.size(); }
};
} // namespace DigiDollar::Paymaster
#endif // DIGIBYTE_PAYMASTER_TRANSPORT_H
