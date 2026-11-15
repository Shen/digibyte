// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_TEST_PAYMASTER_CHECKPOINT_H
#define DIGIBYTE_WALLET_TEST_PAYMASTER_CHECKPOINT_H

#include <wallet/test/util.h>

namespace wallet {
/** Fail the financial commit, after its separate immutable-intent commit.
 * Uses the ordinary mock write callback; no production fault-injection hook. */
class PaymasterFinalCommitFailure {
    MockableDatabase& m_database;
    std::function<void(Span<const std::byte>)> m_previous;
    bool m_previous_failure;
    bool m_intent_written{false};
    bool m_armed{false};
public:
    explicit PaymasterFinalCommitFailure(MockableDatabase& database)
        : m_database(database), m_previous(std::move(database.m_on_write)), m_previous_failure(database.m_fail_commit)
    {
        database.m_fail_commit = false;
        database.m_on_write = [this](Span<const std::byte> key) {
            if (m_previous) m_previous(key);
            DataStream encoded;
            encoded.write(key);
            std::string type;
            encoded >> type;
            if (type == "pmcheckpointjournal") m_intent_written = true;
            else if (m_intent_written) m_database.m_fail_commit = m_armed = true;
        };
    }
    ~PaymasterFinalCommitFailure()
    {
        m_database.m_on_write = std::move(m_previous);
        m_database.m_fail_commit = m_previous_failure;
    }
    PaymasterFinalCommitFailure(const PaymasterFinalCommitFailure&) = delete;
    PaymasterFinalCommitFailure& operator=(const PaymasterFinalCommitFailure&) = delete;
    bool Armed() const { return m_armed; }
};

inline SerializeData PaymasterCheckpointIntentKey()
{
    DataStream key;
    key << std::string{"pmcheckpointjournal"};
    return {key.begin(), key.end()};
}

inline MockableData WithoutPaymasterCheckpointIntent(const MockableData& records)
{
    auto previous = records;
    previous.erase(PaymasterCheckpointIntentKey());
    return previous;
}
} // namespace wallet
#endif // DIGIBYTE_WALLET_TEST_PAYMASTER_CHECKPOINT_H
