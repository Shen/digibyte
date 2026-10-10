// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <paymaster/transport.h>
#include <paymaster/manager.h>
#include <paymaster/wire.h>

#include <bip324.h>
#include <chainparams.h>
#include <key.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <random.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DigiDollar::Paymaster;

namespace {

/** Exercise the real message handler with genuine BIP324 transports. Ordinary
 * handshake messages enter the dispatcher directly; channel proofs and payment
 * packets also pass through encryption and the actual receive queue. */
struct PaymasterChannelSetup : TestingSetup {
    Manager manager{true};

    PaymasterChannelSetup() : TestingSetup{ChainType::REGTEST, {"-digidollaractivationheight=0"}}
    {
        PeerManager::Options options;
        options.deterministic_rng = true;
        options.paymaster = &manager;
        m_node.peerman = PeerManager::make(*m_node.connman, *m_node.addrman,
            m_node.banman.get(), *m_node.chainman, *m_node.mempool, *m_node.stempool, options);
        CConnman::Options connman_options;
        connman_options.m_msgproc = m_node.peerman.get();
        connman_options.nSendBufferMaxSize = 4 * 1024 * 1024;
        m_node.connman->Init(connman_options);
    }

    ~PaymasterChannelSetup() { m_node.peerman.reset(); }

    static std::vector<CNetMessage> Transfer(Transport& sender, Transport& receiver)
    {
        std::vector<CNetMessage> messages;
        const auto& [bytes, more, type] = sender.GetBytesToSend(false);
        if (bytes.empty()) return messages;
        std::vector<uint8_t> copied{bytes.begin(), bytes.end()};
        sender.MarkBytesSent(copied.size());
        Span<const uint8_t> remaining{copied};
        while (!remaining.empty()) {
            const size_t previous = remaining.size();
            BOOST_REQUIRE(receiver.ReceivedBytes(remaining));
            if (receiver.ReceivedMessageComplete()) {
                bool reject{false};
                messages.push_back(receiver.GetReceivedMessage({}, reject));
                BOOST_REQUIRE(!reject);
            }
            BOOST_REQUIRE(remaining.size() < previous);
        }
        return messages;
    }

    std::unique_ptr<CNode> MakePeer(NodeId id, bool inbound, V2Transport& remote, const PaymasterId& provider)
    {
        in_addr address;
        address.s_addr = htonl(0x01020304);
        auto node = std::make_unique<CNode>(id, nullptr,
            CAddress{CService{address, 12024}, NODE_NETWORK}, id, 0, CAddress{}, "",
            inbound ? ConnectionType::INBOUND : ConnectionType::PAYMASTER, false,
            CNodeOptions{.use_v2transport = true, .paymaster_listener = inbound});
        for (int round = 0; round < 8; ++round) {
            BOOST_REQUIRE(Transfer(*node->m_transport, remote).empty());
            BOOST_REQUIRE(Transfer(remote, *node->m_transport).empty());
        }
        BOOST_REQUIRE(node->m_transport->GetInfo().session_id);
        BOOST_REQUIRE(node->m_transport->GetInfo().session_id == remote.GetInfo().session_id);
        m_node.peerman->InitializeNode(*node, NODE_NETWORK);
        Drain(*node, remote);
        if (!inbound) {
            node->m_paymaster_lease = std::make_shared<DirectLease>();
            node->m_paymaster_lease->key = {"test-wallet", "test-payment", false, provider};
            node->m_paymaster_lease->peer_id = id;
        }
        const CNetMsgMaker maker{::PROTOCOL_VERSION};
        const uint64_t services{NODE_NETWORK | NODE_WITNESS | NODE_P2P_V2};
        const auto version = maker.Make(NetMsgType::VERSION, ::PROTOCOL_VERSION,
            services, int64_t{0}, services, CAddress::V1_NETWORK(static_cast<const CService&>(node->addr)));
        CDataStream version_stream{version.data, SER_NETWORK, ::PROTOCOL_VERSION};
        const std::atomic<bool> interrupt{false};
        m_node.peerman->ProcessMessage(*node, NetMsgType::VERSION, version_stream, {}, interrupt);
        CDataStream capabilities{SER_NETWORK, ::PROTOCOL_VERSION};
        capabilities << DigiDollar::Paymaster::PROTOCOL_VERSION
                     << (inbound ? CAP_DIRECT_CONNECTION | CAP_CHANNEL_AUTH : CAP_CHANNEL_AUTH);
        m_node.peerman->ProcessMessage(*node, NetMsgType::SENDPMASTERS, capabilities, {}, interrupt);
        CDataStream verack{SER_NETWORK, ::PROTOCOL_VERSION};
        m_node.peerman->ProcessMessage(*node, NetMsgType::VERACK, verack, {}, interrupt);
        Drain(*node, remote);
        BOOST_REQUIRE(!node->fDisconnect);
        BOOST_REQUIRE(node->IsPaymasterDirectConn());
        BOOST_REQUIRE(!node->m_paymaster_negotiated);
        return node;
    }

    std::vector<CNetMessage> Drain(CNode& node, V2Transport& remote)
    {
        std::vector<CNetMessage> messages;
        LOCK(node.cs_vSend);
        for (size_t round = 0; ; ++round) {
            if (round == 1024) BOOST_FAIL("V2 test send queue made no bounded progress");
            if (!node.vSendMsg.empty()) {
                const size_t memory = node.vSendMsg.front().GetMemoryUsage();
                if (node.m_transport->SetMessageToSend(node.vSendMsg.front())) {
                    node.m_send_memusage -= memory;
                    node.vSendMsg.pop_front();
                }
            }
            auto received = Transfer(*node.m_transport, remote);
            if (received.empty() && node.vSendMsg.empty() &&
                std::get<0>(node.m_transport->GetBytesToSend(false)).empty()) break;
            for (auto& message : received) messages.push_back(std::move(message));
        }
        return messages;
    }

    void Receive(CNode& node, V2Transport& remote, CSerializedNetMsg message)
    {
        BOOST_REQUIRE(remote.SetMessageToSend(message));
        const auto& [bytes, more, type] = remote.GetBytesToSend(false);
        BOOST_REQUIRE(!bytes.empty());
        bool complete{false};
        static_cast<ConnmanTestMsg&>(*m_node.connman).NodeReceiveMsgBytes(node, bytes, complete);
        remote.MarkBytesSent(bytes.size());
        BOOST_REQUIRE(complete);
        static_cast<ConnmanTestMsg&>(*m_node.connman).ProcessMessagesOnce(node);
    }

    ChannelChallenge BeginAuthentication(CNode& node, V2Transport& remote)
    {
        m_node.peerman->SendMessages(&node);
        auto messages = Drain(node, remote);
        std::optional<ChannelChallenge> challenge;
        for (auto& message : messages) {
            BOOST_REQUIRE(message.m_type == NetMsgType::PMAUTHREQ || message.m_type == NetMsgType::PING);
            if (message.m_type == NetMsgType::PMAUTHREQ) {
                BOOST_REQUIRE(!challenge);
                challenge.emplace();
                message.m_recv >> *challenge;
                BOOST_REQUIRE(message.m_recv.empty());
            }
        }
        BOOST_REQUIRE(challenge);
        BOOST_REQUIRE(challenge->provider_id == node.m_paymaster_lease->key.provider_id);
        BOOST_REQUIRE(challenge->transport_session_id == *remote.GetInfo().session_id);
        return *challenge;
    }

    static ChannelProof Sign(const ChannelChallenge& challenge, const CKey& key)
    {
        ChannelProof proof{challenge, XOnlyPubKey{key.GetPubKey()}, {}};
        BOOST_REQUIRE(key.SignSchnorr(GetChannelAuthHash(challenge), proof.signature, nullptr, GetRandHash()));
        return proof;
    }
};

} // namespace

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

BOOST_FIXTURE_TEST_CASE(channel_handler_withholds_all_requests_until_authenticated, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CKey identity;
    identity.MakeNewKey(true);
    V2Transport remote{101, false, SER_NETWORK, INIT_PROTO_VERSION};
    auto node = MakePeer(101, false, remote, GetPaymasterId(XOnlyPubKey{identity.GetPubKey()}));
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    const std::vector<DirectPayload> payloads{PaymasterCapacityRequest{}, PaymasterQuoteRequest{},
        PaymasterSubmit{}, AlternativeRecoveryRequest{}, AlternativeRecoverySubmit{}};
    const std::vector<std::string> types{NetMsgType::PMCAPREQ, NetMsgType::PMQUOTEREQ,
        NetMsgType::PMSUBMIT, NetMsgType::PMRECOVERYREQ, NetMsgType::PMRECOVERYSUBMIT};
    const auto challenge = BeginAuthentication(*node, remote);
    for (size_t index = 0; index < payloads.size(); ++index) {
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(node->GetId(), GetRandHash(), 100, payloads[index], GetTime()));
        m_node.peerman->SendMessages(node.get());
        BOOST_CHECK(Drain(*node, remote).empty());
        BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1);
        // Exercise each application variant independently within the peer's
        // bounded queue. Authentication must never dequeue any of them.
        if (index + 1 != payloads.size()) {
            BOOST_REQUIRE_EQUAL(manager.TakeOutboundDirectMessages(node->GetId(), 1, GetTime()).size(), 1);
        }
    }
    Receive(*node, remote, maker.Make(NetMsgType::PMAUTHRESP, Sign(challenge, identity)));
    BOOST_REQUIRE(node->m_paymaster_negotiated);
    BOOST_REQUIRE(!node->fDisconnect);
    m_node.peerman->SendMessages(node.get());
    auto released = Drain(*node, remote);
    BOOST_REQUIRE_EQUAL(released.size(), 1);
    BOOST_CHECK_EQUAL(released.front().m_type, types.back());
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0);
    m_node.peerman->FinalizeNode(*node);
}

BOOST_FIXTURE_TEST_CASE(channel_handler_rejects_identity_and_proof_manipulation, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CKey identity, impostor;
    identity.MakeNewKey(true);
    impostor.MakeNewKey(true);
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    for (int attack = 0; attack < 8; ++attack) {
        V2Transport remote{200 + attack, false, SER_NETWORK, INIT_PROTO_VERSION};
        auto node = MakePeer(200 + attack, false, remote, GetPaymasterId(XOnlyPubKey{identity.GetPubKey()}));
        const auto challenge = BeginAuthentication(*node, remote);
        auto proof = Sign(challenge, identity);
        switch (attack) {
        case 0: proof = Sign(challenge, impostor); break;
        case 1: proof.challenge.provider_id = GetPaymasterId(XOnlyPubKey{impostor.GetPubKey()}); break;
        case 2: proof.challenge.transport_session_id = GetRandHash(); break;
        case 3: proof.challenge.genesis_hash = GetRandHash(); break;
        case 4: proof.challenge.nonce = GetRandHash(); break;
        case 5: ++proof.challenge.version; break;
        case 6: proof.signature[0] ^= 1; break;
        case 7: node->m_paymaster_lease->canceled = true; break;
        }
        if (attack >= 1 && attack <= 5) proof = Sign(proof.challenge, identity);
        BOOST_REQUIRE(manager.QueueCapacityRequest(node->GetId(), GetRandHash(), 100, {}, GetTime()));
        Receive(*node, remote, maker.Make(NetMsgType::PMAUTHRESP, proof));
        BOOST_REQUIRE(node->fDisconnect);
        BOOST_CHECK(!node->m_paymaster_negotiated);
        m_node.peerman->SendMessages(node.get());
        BOOST_CHECK(Drain(*node, remote).empty());
        BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0);
        m_node.peerman->FinalizeNode(*node);
        // Disconnect removes channel proof authority, never signed financial
        // evidence. Unsent artifacts remain bounded and expire normally.
        BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1);
        manager.ClearDirectMessages();
    }
}

BOOST_FIXTURE_TEST_CASE(channel_handler_rejects_early_payment_traffic_on_both_halves, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    const std::vector<std::string> requests{NetMsgType::PMCAPREQ, NetMsgType::PMQUOTEREQ,
        NetMsgType::PMSUBMIT, NetMsgType::PMRECOVERYREQ, NetMsgType::PMRECOVERYSUBMIT};
    const std::vector<std::string> responses{NetMsgType::PMCAPRESP, NetMsgType::PMQUOTERESP,
        NetMsgType::PMRESULT, NetMsgType::PMRECOVERYRESP, NetMsgType::PMRECOVERYRESULT};
    for (int index = 0; index < 10; ++index) {
        const bool inbound = index < 5;
        V2Transport remote{300 + index, inbound, SER_NETWORK, INIT_PROTO_VERSION};
        auto node = MakePeer(300 + index, inbound, remote, GetRandHash());
        // Deliberately malformed bytes would throw if the financial decoder
        // were reached. The unauthenticated transport gate must stop first.
        Receive(*node, remote, maker.Make(inbound ? requests[index] : responses[index - 5], uint8_t{0xff}));
        BOOST_REQUIRE(node->fDisconnect);
        BOOST_CHECK(!node->m_paymaster_negotiated);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0);
        m_node.peerman->SendMessages(node.get());
        BOOST_CHECK(Drain(*node, remote).empty());
        m_node.peerman->FinalizeNode(*node);
    }
}

BOOST_FIXTURE_TEST_CASE(channel_handler_reauthenticates_after_disconnect, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CKey identity;
    identity.MakeNewKey(true);
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    const auto provider = GetPaymasterId(XOnlyPubKey{identity.GetPubKey()});
    V2Transport first_remote{401, false, SER_NETWORK, INIT_PROTO_VERSION};
    auto first = MakePeer(401, false, first_remote, provider);
    const auto previous = BeginAuthentication(*first, first_remote);
    Receive(*first, first_remote, maker.Make(NetMsgType::PMAUTHRESP, Sign(previous, identity)));
    BOOST_REQUIRE(first->m_paymaster_negotiated);
    BOOST_REQUIRE(manager.QueueCapacityRequest(first->GetId(), GetRandHash(), 100, {}, GetTime()));
    first->CloseSocketDisconnect();
    m_node.peerman->SendMessages(first.get());
    BOOST_CHECK(Drain(*first, first_remote).empty());
    m_node.peerman->FinalizeNode(*first);
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1);

    V2Transport next_remote{402, false, SER_NETWORK, INIT_PROTO_VERSION};
    auto next = MakePeer(402, false, next_remote, provider);
    const auto replacement = BeginAuthentication(*next, next_remote);
    BOOST_REQUIRE(replacement.transport_session_id != previous.transport_session_id);
    BOOST_REQUIRE(replacement.nonce != previous.nonce);
    Receive(*next, next_remote, maker.Make(NetMsgType::PMAUTHRESP, Sign(previous, identity)));
    BOOST_CHECK(next->fDisconnect);
    BOOST_CHECK(!next->m_paymaster_negotiated);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0);
    m_node.peerman->FinalizeNode(*next);
}

BOOST_FIXTURE_TEST_CASE(channel_handler_provider_proof_precedes_all_responses, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    CKey identity;
    identity.MakeNewKey(true);
    const auto provider = GetPaymasterId(XOnlyPubKey{identity.GetPubKey()});
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    V2Transport remote{501, true, SER_NETWORK, INIT_PROTO_VERSION};
    auto node = MakePeer(501, true, remote, provider);
    ChannelChallenge challenge;
    challenge.genesis_hash = Params().GenesisBlock().GetHash();
    challenge.provider_id = provider;
    challenge.transport_session_id = *remote.GetInfo().session_id;
    challenge.nonce = GetRandHash();
    Receive(*node, remote, maker.Make(NetMsgType::PMAUTHREQ, challenge));
    BOOST_REQUIRE(!node->fDisconnect);
    BOOST_REQUIRE(!node->m_paymaster_negotiated);
    auto work = manager.TakeChannelChallenges(provider, 1, DirectNow());
    BOOST_REQUIRE_EQUAL(work.size(), 1);
    BOOST_CHECK(manager.TakeChannelChallenges(GetRandHash(), 1, DirectNow()).empty());
    PaymasterQuoteResponse quote_response;
    const std::vector<DirectPayload> payloads{PaymasterCapacityProof{}, quote_response,
        PaymasterResultMessage{}, AlternativeRecoveryResponse{}, AlternativeRecoveryResultMessage{}};
    for (const auto& payload : payloads) {
        BOOST_REQUIRE(manager.QueueOutboundDirectMessage(node->GetId(), GetRandHash(), 100, payload, GetTime()));
        m_node.peerman->SendMessages(node.get());
        for (const auto& message : Drain(*node, remote)) BOOST_CHECK_EQUAL(message.m_type, NetMsgType::PING);
        BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1);
        BOOST_REQUIRE_EQUAL(manager.TakeOutboundDirectMessages(node->GetId(), 1, GetTime()).size(), 1);
    }
    BOOST_REQUIRE(manager.QueueOutboundDirectMessage(node->GetId(), GetRandHash(), 100,
        DirectPayload{PaymasterResultMessage{}}, GetTime()));
    BOOST_REQUIRE(manager.QueueChannelProof(node->GetId(), Sign(challenge, identity), DirectNow()));
    m_node.peerman->SendMessages(node.get());
    const auto proof_messages = Drain(*node, remote);
    BOOST_REQUIRE_EQUAL(proof_messages.size(), 1);
    BOOST_CHECK_EQUAL(proof_messages.front().m_type, NetMsgType::PMAUTHRESP);
    BOOST_REQUIRE(node->m_paymaster_negotiated);
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 1);
    m_node.peerman->SendMessages(node.get());
    const auto response_messages = Drain(*node, remote);
    BOOST_REQUIRE_EQUAL(response_messages.size(), 1);
    BOOST_CHECK_EQUAL(response_messages.front().m_type, NetMsgType::PMRESULT);
    m_node.peerman->FinalizeNode(*node);
}

BOOST_FIXTURE_TEST_CASE(channel_handler_rejects_invalid_challenges_and_frames, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    for (int attack = 0; attack < 7; ++attack) {
        V2Transport remote{600 + attack, true, SER_NETWORK, INIT_PROTO_VERSION};
        auto node = MakePeer(600 + attack, true, remote, GetRandHash());
        ChannelChallenge challenge;
        challenge.genesis_hash = Params().GenesisBlock().GetHash();
        challenge.provider_id = GetRandHash();
        challenge.transport_session_id = *remote.GetInfo().session_id;
        challenge.nonce = GetRandHash();
        switch (attack) {
        case 0: challenge.transport_session_id = GetRandHash(); break;
        case 1: challenge.genesis_hash = GetRandHash(); break;
        case 2: challenge.provider_id.SetNull(); break;
        case 3: challenge.nonce.SetNull(); break;
        case 4: ++challenge.version; break;
        }
        auto message = maker.Make(NetMsgType::PMAUTHREQ, challenge);
        if (attack == 5) message.data.pop_back();
        if (attack == 6) message.data.push_back(0);
        Receive(*node, remote, std::move(message));
        BOOST_REQUIRE(node->fDisconnect);
        BOOST_CHECK(!node->m_paymaster_negotiated);
        BOOST_CHECK(!manager.HasChannelChallenges(challenge.provider_id, DirectNow()));
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0);
        m_node.peerman->SendMessages(node.get());
        BOOST_CHECK(Drain(*node, remote).empty());
        m_node.peerman->FinalizeNode(*node);
    }
}

BOOST_FIXTURE_TEST_CASE(channel_sender_checks_authentication_for_internal_callers, PaymasterChannelSetup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    const CNetMsgMaker maker{::PROTOCOL_VERSION};
    for (const bool inbound : {false, true}) {
        CKey identity;
        identity.MakeNewKey(true);
        V2Transport remote{701 + inbound, inbound, SER_NETWORK, INIT_PROTO_VERSION};
        auto node = MakePeer(701 + inbound, inbound, remote,
            GetPaymasterId(XOnlyPubKey{identity.GetPubKey()}));
        // Bypass the normal queued sender deliberately. The last transport
        // boundary must also reject private payloads from an internal caller.
        const std::vector<std::string> types = inbound
            ? std::vector<std::string>{NetMsgType::PMCAPRESP, NetMsgType::PMQUOTERESP,
                NetMsgType::PMRESULT, NetMsgType::PMRECOVERYRESP, NetMsgType::PMRECOVERYRESULT}
            : std::vector<std::string>{NetMsgType::PMCAPREQ, NetMsgType::PMQUOTEREQ,
                NetMsgType::PMSUBMIT, NetMsgType::PMRECOVERYREQ, NetMsgType::PMRECOVERYSUBMIT};
        for (const auto& type : types) {
            m_node.connman->PushMessage(node.get(), maker.Make(type, uint8_t{0xff}));
            BOOST_CHECK(Drain(*node, remote).empty());
        }
        if (!inbound) {
            const auto challenge = BeginAuthentication(*node, remote);
            Receive(*node, remote, maker.Make(NetMsgType::PMAUTHRESP, Sign(challenge, identity)));
            BOOST_REQUIRE(node->m_paymaster_negotiated);
            m_node.connman->PushMessage(node.get(), maker.Make(NetMsgType::PMCAPREQ, uint8_t{0xff}));
            BOOST_REQUIRE_EQUAL(Drain(*node, remote).size(), 1);
            node->m_paymaster_lease->canceled = true;
            m_node.connman->PushMessage(node.get(), maker.Make(NetMsgType::PMCAPREQ, uint8_t{0xff}));
            BOOST_CHECK(Drain(*node, remote).empty());
        }
        node->CloseSocketDisconnect();
        m_node.connman->PushMessage(node.get(), maker.Make(types.front(), uint8_t{0xff}));
        BOOST_CHECK(Drain(*node, remote).empty());
        m_node.peerman->FinalizeNode(*node);
    }
}

BOOST_AUTO_TEST_SUITE_END()
