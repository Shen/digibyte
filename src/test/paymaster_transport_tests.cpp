// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <paymaster/transport.h>
#include <paymaster/manager.h>
#include <paymaster/wire.h>

#include <bip324.h>
#include <chainparams.h>
#include <key.h>
#include <random.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DigiDollar::Paymaster;

BOOST_AUTO_TEST_SUITE(paymaster_transport_tests)

BOOST_AUTO_TEST_CASE(effective_budgets_preserve_relay_and_disable_impossible_roles)
{
    const auto small = DirectBudget::Calculate(16, 17, 1, 16, true);
    BOOST_CHECK_EQUAL(small.outbound, 0);
    BOOST_CHECK_EQUAL(small.inbound, 0);
    BOOST_CHECK_EQUAL(small.ordinary_inbound, 0);
    const auto client = DirectBudget::Calculate(32, 19, 1, 16, true);
    BOOST_CHECK_EQUAL(client.outbound, 1);
    BOOST_CHECK_EQUAL(client.inbound, 0);
    BOOST_CHECK_EQUAL(client.ordinary_inbound, 12);
    const auto provider = DirectBudget::Calculate(45, 19, 1, 16, true);
    BOOST_CHECK_EQUAL(provider.inbound, 16);
    BOOST_CHECK_EQUAL(provider.handshakes, 8);
    BOOST_CHECK_EQUAL(provider.ordinary_inbound, 1);
    BOOST_CHECK_EQUAL(DirectBudget::Calculate(44, 19, 1, 16, true).inbound, 0);
    const auto ordinary = DirectBudget::Calculate(125, 19, 0, 0, false);
    BOOST_CHECK_EQUAL(ordinary.ordinary_inbound, 106);
    const auto parallel = DirectBudget::Calculate(125, 19, 4, 16, true);
    BOOST_CHECK_EQUAL(parallel.ordinary_inbound, 78);
}

BOOST_AUTO_TEST_CASE(permits_are_atomic_and_include_concurrent_connects)
{
    DirectPermits permits;
    permits.Configure(DirectBudget{4, 16, 8, 78});
    std::atomic<int> attempted{0}, acquired{0};
    std::atomic_bool release{false};
    std::vector<std::thread> contenders;
    for (int i = 0; i < 24; ++i) {
        contenders.emplace_back([&] {
            auto grant = permits.Acquire(DirectPermits::Kind::OUTBOUND);
            if (grant) ++acquired;
            ++attempted;
            while (!release) std::this_thread::yield();
        });
    }
    while (attempted != 24) std::this_thread::yield();
    BOOST_CHECK_EQUAL(acquired.load(), 4);
    BOOST_CHECK_EQUAL(permits.GetUsage().outgoing, 4);
    release = true;
    for (auto& worker : contenders) worker.join();
    BOOST_CHECK_EQUAL(permits.GetUsage().outgoing, 0);
}

BOOST_AUTO_TEST_CASE(provider_promotion_is_bounded_and_releases_exactly_once)
{
    DirectPermits permits;
    permits.Configure(DirectBudget{1, 2, 2, 1});
    auto first = permits.Acquire(DirectPermits::Kind::HANDSHAKE);
    auto second = permits.Acquire(DirectPermits::Kind::HANDSHAKE);
    BOOST_REQUIRE(first && second);
    BOOST_CHECK(!permits.Acquire(DirectPermits::Kind::HANDSHAKE));
    BOOST_CHECK(first->Promote());
    BOOST_CHECK(first->Promote());
    BOOST_CHECK(second->Promote());
    auto waiting = permits.Acquire(DirectPermits::Kind::HANDSHAKE);
    BOOST_REQUIRE(waiting);
    BOOST_CHECK(!waiting->Promote());
    BOOST_CHECK_EQUAL(permits.GetUsage().incoming, 2);
    first.reset();
    BOOST_CHECK(waiting->Promote());
    second.reset();
    waiting.reset();
    BOOST_CHECK_EQUAL(permits.GetUsage().incoming, 0);
    BOOST_CHECK_EQUAL(permits.GetUsage().handshakes, 0);
}

BOOST_AUTO_TEST_CASE(queue_is_idempotent_fair_and_reserves_recovery_admission)
{
    DirectPermits permits;
    permits.Configure(DirectBudget{1, 0, 0, 0});
    DirectQueue queue;
    for (const auto& key : std::vector<DirectKey>{{"a", "1"}, {"a", "2"}, {"b", "1"}, {"r", "1", true}}) {
        BOOST_CHECK(queue.Request(key, "endpoint", false, 1).state == DirectState::QUEUED);
    }
    BOOST_CHECK(queue.Request({"a", "1"}, "endpoint", false, 2).state == DirectState::QUEUED);
    BOOST_CHECK_EQUAL(queue.Size(), 4U);
    auto first = queue.Claim(permits, 2);
    BOOST_REQUIRE(first);
    BOOST_CHECK_EQUAL(first->key.owner, "a");
    BOOST_CHECK(!queue.Claim(permits, 2));
    first.reset();
    auto recovery = queue.Claim(permits, 3);
    BOOST_REQUIRE(recovery);
    BOOST_CHECK(recovery->key.recovery);
    recovery.reset();
    auto second_wallet = queue.Claim(permits, 4);
    BOOST_REQUIRE(second_wallet);
    BOOST_CHECK_EQUAL(second_wallet->key.owner, "b");
}

BOOST_AUTO_TEST_CASE(polling_cannot_extend_deadlines_or_change_privacy)
{
    DirectQueue queue;
    const DirectKey key{"wallet", "request"};
    BOOST_CHECK(queue.Request(key, "endpoint", true, 1).state == DirectState::QUEUED);
    BOOST_CHECK(queue.Request(key, "endpoint", false, 2).state == DirectState::FAILED);
    BOOST_CHECK(queue.Request(key, "endpoint", true, DIRECT_WAIT_MS).state == DirectState::QUEUED);
    BOOST_CHECK(queue.Request(key, "endpoint", true, DIRECT_WAIT_MS + 1).state == DirectState::EXPIRED);
    BOOST_CHECK(queue.Request(key, "endpoint", true, 10 * DIRECT_WAIT_MS).state == DirectState::EXPIRED);
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
    queue.Release(key); // Explicit caller action, never status polling.
    BOOST_CHECK(queue.Request(key, "endpoint", true, 10 * DIRECT_WAIT_MS).state == DirectState::QUEUED);
}

BOOST_AUTO_TEST_CASE(wallets_and_operations_never_share_a_channel)
{
    DirectQueue queue;
    DirectPermits permits;
    permits.Configure(DirectBudget{1, 0, 0, 0});
    const DirectKey a{"wallet-generation-a", "request"};
    const DirectKey b{"wallet-generation-b", "request"};
    queue.Request(a, "same-endpoint", true, 1);
    auto lease = queue.Claim(permits, 2);
    BOOST_REQUIRE(lease);
    lease->state = DirectState::READY;
    BOOST_CHECK(queue.Request(a, "same-endpoint", true, 3).lease == lease);
    BOOST_CHECK(queue.Request(b, "same-endpoint", true, 3).state == DirectState::QUEUED);
    queue.CancelOwner(a.owner);
    BOOST_CHECK(lease->canceled);
    BOOST_CHECK(!queue.Claim(permits, 4)); // Live socket still owns its permit.
    lease.reset();
    auto next = queue.Claim(permits, 5);
    BOOST_REQUIRE(next);
    BOOST_CHECK(next->key == b);
    queue.Stop();
    BOOST_CHECK(next->canceled);
    BOOST_CHECK(queue.Request(a, "same-endpoint", true, 6).state == DirectState::CANCELED);
}

BOOST_AUTO_TEST_CASE(queue_limits_do_not_exclude_recovery)
{
    DirectQueue queue;
    for (int i = 0; i < 24; ++i) {
        BOOST_CHECK(queue.Request({std::to_string(i / 8), std::to_string(i)}, "endpoint", false, 1).state == DirectState::QUEUED);
    }
    BOOST_CHECK(queue.Request({"new", "overflow"}, "endpoint", false, 1).state == DirectState::FULL);
    for (int i = 0; i < 8; ++i) {
        BOOST_CHECK(queue.Request({"recovery", std::to_string(i), true}, "endpoint", true, 1).state == DirectState::QUEUED);
    }
    BOOST_CHECK_EQUAL(queue.Size(), 32U);
    BOOST_CHECK(queue.Request({"recovery2", "overflow", true}, "endpoint", true, 1).state == DirectState::FULL);
}


BOOST_AUTO_TEST_CASE(retry_and_abandon_do_not_touch_other_sessions)
{
    DirectQueue queue;
    DirectPermits permits;
    permits.Configure(DirectBudget{1, 0, 0, 0});
    const DirectKey first{"wallet", "session-a:provider"};
    const DirectKey second{"wallet", "session-b:provider"};
    queue.Request(first, "endpoint", true, 1);
    auto lease = queue.Claim(permits, 2);
    BOOST_REQUIRE(lease);
    lease->state = DirectState::READY;
    queue.Retry(first);
    BOOST_CHECK(!lease->canceled);
    queue.Request(second, "endpoint", true, 2);
    queue.Finish(lease, DirectState::CONNECT_FAILED);
    queue.Retry(first);
    queue.Request(first, "endpoint", true, 3);
    BOOST_CHECK(lease->state == DirectState::CONNECT_FAILED);
    BOOST_CHECK(!queue.Claim(permits, 3)); // Retry cannot release a live socket.
    lease.reset();
    queue.CancelOperation(first.owner, "session-a:");
    auto next = queue.Claim(permits, 4);
    BOOST_REQUIRE(next);
    BOOST_CHECK(next->key == second);
    queue.Request(first, "endpoint", true, 4);
    BOOST_CHECK(queue.Request(first, "endpoint", true, 4 + DIRECT_WAIT_MS).state == DirectState::EXPIRED);
    queue.Retry(first);
    BOOST_CHECK(queue.Request(first, "endpoint", true, 5 + DIRECT_WAIT_MS).state == DirectState::QUEUED);
}

BOOST_AUTO_TEST_CASE(expired_outcomes_are_bounded_and_not_silently_replaced)
{
    DirectQueue queue;
    for (int batch = 0; batch < 31; ++batch) {
        const int64_t now = batch * (DIRECT_WAIT_MS + 1);
        for (int i = 0; i < 8; ++i) {
            BOOST_REQUIRE(queue.Request({"wallet", std::to_string(batch * 8 + i)},
                                        "endpoint", false, now).state == DirectState::QUEUED);
        }
    }
    const int64_t now = 31 * (DIRECT_WAIT_MS + 1);
    BOOST_CHECK(queue.Request({"wallet", "fresh"}, "endpoint", false, now).state == DirectState::FULL);
    BOOST_CHECK(queue.Request({"wallet", "0"}, "endpoint", false, now).state == DirectState::EXPIRED);
    // Failed fresh work cannot consume the reserved recovery admission.
    for (int i = 0; i < 8; ++i) {
        BOOST_CHECK(queue.Request({"recovery", std::to_string(i), true}, "endpoint", true, now).state == DirectState::QUEUED);
    }
    queue.CancelOwner("wallet");
    BOOST_CHECK(queue.Request({"reloaded-wallet", "fresh"}, "endpoint", false, now).state == DirectState::QUEUED);
}


BOOST_AUTO_TEST_CASE(connection_admission_bounds_reconnects_without_charging_other_groups)
{
    DirectAdmission admission;
    for (int i = 0; i < 4; ++i) BOOST_REQUIRE(admission.Admit(1, 1000));
    for (int i = 0; i < 1000; ++i) BOOST_CHECK(!admission.Admit(1, 1000));
    BOOST_CHECK(admission.Admit(2, 1000));
    BOOST_CHECK(!admission.Admit(1, 999)); // Backward time cannot refill.
    BOOST_CHECK(admission.Admit(1, 6000));

    DirectAdmission onion;
    for (int i = 0; i < 16; ++i) BOOST_REQUIRE(onion.Admit(std::nullopt, 1000));
    for (int i = 0; i < 1000; ++i) BOOST_CHECK(!onion.Admit(uint64_t(i), 1000));
    BOOST_CHECK(!onion.Admit(std::nullopt, 1999));
    // Failed source churn above must not fill the bounded source table.
    BOOST_CHECK(onion.Admit(10000, 2000));
    BOOST_CHECK(!onion.Admit(std::nullopt, 2000));
    BOOST_CHECK(onion.Admit(std::nullopt, 3000));
}

BOOST_AUTO_TEST_CASE(clearnet_permits_cannot_fill_the_shared_pool_and_revocation_is_exact)
{
    DirectPermits permits;
    permits.Configure(DirectBudget{1, 16, 8, 1});
    std::vector<std::shared_ptr<DirectPermits::Permit>> peers;
    for (int i = 0; i < 4; ++i) {
        peers.push_back(permits.Acquire(DirectPermits::Kind::HANDSHAKE, 1));
        BOOST_REQUIRE(peers.back());
    }
    BOOST_CHECK(permits.AtGroupLimit(1));
    BOOST_CHECK(!permits.Acquire(DirectPermits::Kind::HANDSHAKE, 1));
    auto other = permits.Acquire(DirectPermits::Kind::HANDSHAKE, 2);
    BOOST_REQUIRE(other);
    BOOST_CHECK(other->Promote());
    BOOST_CHECK(peers.front()->Promote()); // Same group still owns four.
    BOOST_CHECK(!permits.Acquire(DirectPermits::Kind::HANDSHAKE, 1));
    peers.back()->Release(); // Socket has closed; later destruction is inert.
    BOOST_CHECK(!peers.back()->Promote());
    peers.back()->Release();
    peers.pop_back();
    BOOST_CHECK_EQUAL(permits.GetUsage().incoming, 2);
    BOOST_CHECK_EQUAL(permits.GetUsage().handshakes, 2);
    BOOST_CHECK(permits.Acquire(DirectPermits::Kind::HANDSHAKE, 1));
}

BOOST_AUTO_TEST_CASE(raw_byte_and_control_message_work_has_bounded_refill)
{
    DirectRateBucket bytes{2 * 1024 * 1024, 8000};
    BOOST_CHECK(bytes.Consume(2 * 1024 * 1024, 1000));
    BOOST_CHECK(!bytes.Consume(1, 1000));
    BOOST_CHECK(!bytes.Consume(1, 999));
    BOOST_CHECK(bytes.Consume(256 * 1024, 2000));
    BOOST_CHECK(!bytes.Consume(1, 2000));
    DirectRateBucket messages{64, 8000};
    for (int i = 0; i < 64; ++i) BOOST_REQUIRE(messages.Consume(1, 1000));
    BOOST_CHECK(!messages.Consume(1, 1124));
    BOOST_CHECK(messages.Consume(1, 1125));
}

BOOST_FIXTURE_TEST_CASE(channel_authentication_rejects_two_leg_bip324_relay, BasicTestingSetup)
{
    CKey client_key, relay_front_key, relay_back_key, provider_key;
    client_key.MakeNewKey(true);
    relay_front_key.MakeNewKey(true);
    relay_back_key.MakeNewKey(true);
    provider_key.MakeNewKey(true);
    const auto entropy = GetRandHash();
    BIP324Cipher client{client_key, MakeByteSpan(entropy)};
    BIP324Cipher relay_front{relay_front_key, MakeByteSpan(entropy)};
    BIP324Cipher relay_back{relay_back_key, MakeByteSpan(entropy)};
    BIP324Cipher provider{provider_key, MakeByteSpan(entropy)};
    client.Initialize(relay_front.GetOurPubKey(), true);
    relay_front.Initialize(client.GetOurPubKey(), false);
    relay_back.Initialize(provider.GetOurPubKey(), true);
    provider.Initialize(relay_back.GetOurPubKey(), false);
    const uint256 client_session{MakeUCharSpan(client.GetSessionID())};
    const uint256 provider_session{MakeUCharSpan(provider.GetSessionID())};
    BOOST_REQUIRE(client_session == uint256{MakeUCharSpan(relay_front.GetSessionID())});
    BOOST_REQUIRE(provider_session == uint256{MakeUCharSpan(relay_back.GetSessionID())});
    BOOST_REQUIRE(client_session != provider_session);

    ChannelChallenge challenge;
    challenge.genesis_hash = Params().GenesisBlock().GetHash();
    challenge.provider_id = GetPaymasterId(XOnlyPubKey{provider_key.GetPubKey()});
    challenge.transport_session_id = client_session;
    challenge.nonce = GetRandHash();
    const auto transit = [](BIP324Cipher& sender, BIP324Cipher& receiver, const auto& message) {
        CDataStream encoded{SER_NETWORK, ::PROTOCOL_VERSION};
        encoded << message;
        std::vector<std::byte> ciphertext(encoded.size() + BIP324Cipher::EXPANSION);
        sender.Encrypt(MakeByteSpan(encoded), {}, false, ciphertext);
        const auto length = receiver.DecryptLength(Span{ciphertext}.first(BIP324Cipher::LENGTH_LEN));
        std::vector<std::byte> plaintext(length);
        bool ignore{true};
        BOOST_REQUIRE(receiver.Decrypt(Span{ciphertext}.subspan(BIP324Cipher::LENGTH_LEN), {}, ignore, plaintext));
        BOOST_REQUIRE(!ignore);
        CDataStream decoded{MakeUCharSpan(plaintext), SER_NETWORK, ::PROTOCOL_VERSION};
        std::remove_cvref_t<decltype(message)> received;
        decoded >> received;
        BOOST_REQUIRE(decoded.empty());
        return received;
    };
    ChannelChallenge forwarded = transit(relay_back, provider, transit(client, relay_front, challenge));
    BOOST_REQUIRE(GetChannelAuthHash(forwarded) == GetChannelAuthHash(challenge));
    // Forwarding authentic bytes fails at the provider's own channel check.
    BOOST_CHECK(!ValidateChannelChallenge(forwarded, challenge.genesis_hash, provider_session));
    // Rewriting the id permits a genuine proof on the second leg, but that
    // signature is not authority for the client's original channel/challenge.
    forwarded.transport_session_id = provider_session;
    BOOST_REQUIRE(ValidateChannelChallenge(forwarded, challenge.genesis_hash, provider_session));
    ChannelProof proof{forwarded, XOnlyPubKey{provider_key.GetPubKey()}, {}};
    BOOST_REQUIRE(provider_key.SignSchnorr(GetChannelAuthHash(forwarded), proof.signature, nullptr, GetRandHash()));
    const auto relayed_proof = transit(relay_front, client, transit(provider, relay_back, proof));
    BOOST_CHECK(!ValidateChannelProof(relayed_proof, challenge));
    proof.challenge.transport_session_id = client_session;
    BOOST_CHECK(!ValidateChannelProof(proof, challenge));
    // A direct encrypted channel with the selected identity succeeds.
    BOOST_REQUIRE(provider_key.SignSchnorr(GetChannelAuthHash(challenge), proof.signature, nullptr, GetRandHash()));
    BOOST_REQUIRE(ValidateChannelProof(proof, challenge));
    auto changed = challenge;
    changed.nonce = GetRandHash();
    BOOST_CHECK(!ValidateChannelProof(proof, changed));
    changed = challenge;
    changed.genesis_hash = GetRandHash();
    BOOST_CHECK(!ValidateChannelProof(proof, changed));
    changed = challenge;
    changed.provider_id = GetPaymasterId(XOnlyPubKey{client_key.GetPubKey()});
    BOOST_CHECK(!ValidateChannelProof(proof, changed));
    changed = challenge;
    ++changed.version;
    BOOST_CHECK(!ValidateChannelProof(proof, changed));
    proof.signature[0] ^= 1;
    BOOST_CHECK(!ValidateChannelProof(proof, challenge));
}

BOOST_FIXTURE_TEST_CASE(channel_auth_queue_bounds_disconnect_and_expiry, BasicTestingSetup)
{
    Manager manager{true};
    CKey identity;
    identity.MakeNewKey(true);
    ChannelChallenge challenge;
    challenge.genesis_hash = Params().GenesisBlock().GetHash();
    challenge.provider_id = GetPaymasterId(XOnlyPubKey{identity.GetPubKey()});
    challenge.transport_session_id = GetRandHash();
    challenge.nonce = GetRandHash();
    for (int64_t peer = 0; peer < MAX_DIRECT_HANDSHAKES; ++peer)
        BOOST_REQUIRE(manager.QueueChannelChallenge(peer, challenge, 1000));
    BOOST_CHECK(!manager.QueueChannelChallenge(0, challenge, 1001));
    BOOST_CHECK(!manager.QueueChannelChallenge(MAX_DIRECT_HANDSHAKES, challenge, 1001));
    BOOST_CHECK(manager.HasChannelChallenges(challenge.provider_id, 1001));
    BOOST_CHECK(!manager.HasChannelChallenges(GetRandHash(), 1001));
    BOOST_CHECK(manager.TakeChannelChallenges(GetRandHash(), 1, 1001).empty());
    auto work = manager.TakeChannelChallenges(challenge.provider_id, 1, 1001);
    BOOST_REQUIRE_EQUAL(work.size(), 1);
    ChannelProof proof{challenge, XOnlyPubKey{identity.GetPubKey()}, {}};
    BOOST_REQUIRE(identity.SignSchnorr(GetChannelAuthHash(challenge), proof.signature, nullptr, GetRandHash()));
    manager.ForgetDirectPeer(work.front().first);
    BOOST_CHECK(!manager.QueueChannelProof(work.front().first, proof, 1002));
    BOOST_CHECK(!manager.TakeChannelProof(work.front().first, 1002));
    work = manager.TakeChannelChallenges(challenge.provider_id, 1, 1002);
    BOOST_REQUIRE_EQUAL(work.size(), 1);
    BOOST_REQUIRE(manager.QueueChannelProof(work.front().first, proof, 1002));
    BOOST_CHECK(!manager.QueueChannelProof(work.front().first, proof, 1003));
    const auto received = manager.TakeChannelProof(work.front().first, 1003);
    BOOST_REQUIRE(received);
    BOOST_CHECK(ValidateChannelProof(*received, challenge));
    BOOST_CHECK(!manager.TakeChannelProof(work.front().first, 1003));
    BOOST_CHECK(manager.TakeChannelChallenges(challenge.provider_id, MAX_DIRECT_HANDSHAKES, 31'001).empty());
    BOOST_REQUIRE(manager.QueueChannelChallenge(0, challenge, 31'001));
    work = manager.TakeChannelChallenges(challenge.provider_id, 1, 31'001);
    BOOST_REQUIRE_EQUAL(work.size(), 1);
    manager.SetEnabled(false);
    BOOST_CHECK(!manager.QueueChannelProof(0, proof, 31'002));
    BOOST_CHECK(!manager.TakeChannelProof(0, 31'002));
    BOOST_CHECK(!manager.HasChannelChallenges(challenge.provider_id, 31'002));
}

BOOST_AUTO_TEST_SUITE_END()
