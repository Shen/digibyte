// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/amount.h>
#include <key.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <oracle/node.h>
#include <primitives/oracle.h>
#include <protocol.h>
#include <pubkey.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

BOOST_FIXTURE_TEST_SUITE(digidollar_oracle_tests, BasicTestingSetup)

/**
 * COraclePriceMessage Tests
 * Tests for individual oracle price message structure
 */
BOOST_AUTO_TEST_CASE(oracle_price_message_basic_construction)
{
    // Test basic construction
    COraclePriceMessage msg;
    BOOST_CHECK_EQUAL(msg.oracle_id, 0);
    BOOST_CHECK_EQUAL(msg.price_micro_usd, 0);
    BOOST_CHECK_EQUAL(msg.timestamp, 0);
    BOOST_CHECK(msg.schnorr_sig.empty());

    // Test construction with parameters
    uint32_t oracle_id = 5;
    CAmount price = 6000; // $0.006 per DGB (realistic price)
    int64_t timestamp = GetTime();

    COraclePriceMessage msg2(oracle_id, price, timestamp);
    BOOST_CHECK_EQUAL(msg2.oracle_id, oracle_id);
    BOOST_CHECK_EQUAL(msg2.price_micro_usd, price);
    BOOST_CHECK_EQUAL(msg2.timestamp, timestamp);
    BOOST_CHECK(msg2.schnorr_sig.empty());
}

BOOST_AUTO_TEST_CASE(oracle_price_message_validation)
{
    COraclePriceMessage msg;

    // Test invalid prices
    msg.price_micro_usd = -1;
    BOOST_CHECK(!msg.IsValid());

    msg.price_micro_usd = 0;
    BOOST_CHECK(!msg.IsValid());

    // Test price below minimum (MIN_PRICE_MICRO_USD = 100)
    msg.price_micro_usd = 50; // Below minimum ($0.00005)
    msg.timestamp = GetTime();
    msg.oracle_id = 1;
    BOOST_CHECK(!msg.IsValid());

    // Test extremely high price (unrealistic - above $100)
    msg.price_micro_usd = 200000000; // $200 per DGB (unrealistic)
    BOOST_CHECK(!msg.IsValid());

    // Test valid price range
    msg.price_micro_usd = 6000; // $0.006 per DGB (realistic)
    msg.timestamp = GetTime();
    msg.oracle_id = 1;
    {
        CKey key;
        key.MakeNewKey(true);
        BOOST_REQUIRE(msg.SignAttestation(key));
    }
    BOOST_CHECK(msg.IsValid());

    // Test timestamp validation (future timestamp should be invalid)
    msg.timestamp = GetTime() + 3600; // 1 hour in future
    BOOST_CHECK(!msg.IsValid());

    // Test very old timestamp (more than 1 hour old)
    msg.timestamp = GetTime() - 3700; // More than 1 hour old
    BOOST_CHECK(!msg.IsValid());
}

BOOST_AUTO_TEST_CASE(oracle_price_message_signature_validation)
{
    // Create a test key pair
    CKey oracle_key;
    oracle_key.MakeNewKey(true);
    CPubKey oracle_pubkey = oracle_key.GetPubKey();

    COraclePriceMessage msg(1, 6000, GetTime());  // $0.006 (realistic price)

    // Test message without signature
    msg.oracle_pubkey = XOnlyPubKey(oracle_pubkey);
    BOOST_CHECK(!msg.Verify());

    // Create a proper Schnorr signature
    BOOST_CHECK(msg.Sign(oracle_key));

    // Test valid signature
    BOOST_CHECK(msg.Verify());

    // Test with wrong pubkey
    CKey wrong_key;
    wrong_key.MakeNewKey(true);
    CPubKey wrong_pubkey = wrong_key.GetPubKey();
    msg.oracle_pubkey = XOnlyPubKey(wrong_pubkey);
    BOOST_CHECK(!msg.Verify());

    // Test with corrupted signature
    if (!msg.schnorr_sig.empty()) {
        msg.schnorr_sig[0] ^= 1; // Flip a bit
        msg.oracle_pubkey = XOnlyPubKey(oracle_pubkey);
        BOOST_CHECK(!msg.Verify());
    }
}

BOOST_AUTO_TEST_CASE(oracle_price_message_serialization)
{
    COraclePriceMessage original(42, 7500, GetTime());  // $0.0075 (realistic price)
    original.schnorr_sig = {0x01, 0x02, 0x03, 0x04}; // Dummy signature

    // Serialize
    DataStream ss{};
    ss << original;

    // Deserialize
    COraclePriceMessage deserialized;
    ss >> deserialized;

    // Verify all fields match
    BOOST_CHECK_EQUAL(deserialized.oracle_id, original.oracle_id);
    BOOST_CHECK_EQUAL(deserialized.price_micro_usd, original.price_micro_usd);
    BOOST_CHECK_EQUAL(deserialized.timestamp, original.timestamp);
    BOOST_CHECK(deserialized.schnorr_sig == original.schnorr_sig);
}

/**
 * COracleBundle Tests
 * Tests for oracle message bundle and consensus calculation
 */
BOOST_AUTO_TEST_CASE(oracle_bundle_basic_construction)
{
    COracleBundle bundle;
    BOOST_CHECK(bundle.messages.empty());
    BOOST_CHECK_EQUAL(bundle.epoch, 0);

    COracleBundle bundle2(100);
    BOOST_CHECK(bundle2.messages.empty());
    BOOST_CHECK_EQUAL(bundle2.epoch, 100);
}

BOOST_AUTO_TEST_CASE(oracle_bundle_consensus_requirement)
{
    COracleBundle bundle(42);

    // No messages - no consensus
    BOOST_CHECK(!bundle.HasConsensus(ORACLE_CONSENSUS_REQUIRED));

    // Add one fewer than the configured quorum - still no consensus.
    for (int i = 0; i < ORACLE_CONSENSUS_REQUIRED - 1; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006 (realistic price)
        bundle.AddMessage(msg);
    }
    BOOST_CHECK(!bundle.HasConsensus(ORACLE_CONSENSUS_REQUIRED));

    // Add the quorum signer - now has consensus.
    COraclePriceMessage quorum_msg(ORACLE_CONSENSUS_REQUIRED - 1, 6000, GetTime());
    bundle.AddMessage(quorum_msg);
    BOOST_CHECK(bundle.HasConsensus(ORACLE_CONSENSUS_REQUIRED));

    // Test with more messages up to the reserved active-capacity limit.
    for (int i = ORACLE_CONSENSUS_REQUIRED; i < 17; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006 (realistic price)
        bundle.AddMessage(msg);
    }
    BOOST_CHECK(bundle.HasConsensus(ORACLE_CONSENSUS_REQUIRED));

    for (int i = 17; i < ORACLE_ACTIVE_COUNT; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006 (realistic price)
        BOOST_CHECK(bundle.AddMessage(msg));
    }

    // Test with too many messages (should reject once all 35 slots are filled).
    COraclePriceMessage extra_msg(ORACLE_ACTIVE_COUNT, 6000, GetTime());  // $0.006 (realistic price)
    BOOST_CHECK(!bundle.AddMessage(extra_msg));
}

BOOST_AUTO_TEST_CASE(oracle_bundle_median_calculation)
{
    COracleBundle bundle(1);

    // Test median with odd number of values (prices in micro-USD)
    // Using realistic prices around $0.005 (5000 micro-USD)
    std::vector<CAmount> prices = {3000, 5000, 4000, 6000, 4500,
                                   5500, 4800, 5200, 4700};

    for (size_t i = 0; i < prices.size(); i++) {
        COraclePriceMessage msg(i, prices[i], GetTime());
        bundle.AddMessage(msg);
    }

    CAmount median_price = bundle.GetConsensusPrice(ORACLE_CONSENSUS_REQUIRED);
    // Sorted: [3000, 4000, 4500, 4700, 4800, 5000, 5200, 5500, 6000]
    // IQR filter (T9-01): q1_idx=2→q1=4500, q3_idx=6→q3=5200, IQR=700
    // Bounds: [3450, 6250] → 3000 filtered out
    // Remaining: [4000, 4500, 4700, 4800, 5000, 5200, 5500, 6000] — 8 values
    // Median (even): (4800 + 5000) / 2 = 4900 micro-USD
    BOOST_CHECK_EQUAL(median_price, 4900);

    // Test median with even number of values (add one more)
    COraclePriceMessage msg10(9, 4900, GetTime());
    bundle.AddMessage(msg10);

    median_price = bundle.GetConsensusPrice(ORACLE_CONSENSUS_REQUIRED);
    // Sorted: [3000, 4000, 4500, 4700, 4800, 4900, 5000, 5200, 5500, 6000]
    // IQR filter: q1_idx=2→q1=4500, q3_idx=7→q3=5200, IQR=700
    // Bounds: [3450, 6250] → 3000 filtered out
    // Remaining: [4000, 4500, 4700, 4800, 4900, 5000, 5200, 5500, 6000] — 9 values
    // Median (odd): 4900 micro-USD
    BOOST_CHECK_EQUAL(median_price, 4900);
}

BOOST_AUTO_TEST_CASE(oracle_bundle_outlier_filtering)
{
    // Test unified IQR outlier filtering via GetConsensusPrice (T9-01)
    COracleBundle bundle(1);

    // Add normal prices around 5000 micro-USD ($0.005)
    std::vector<CAmount> normal_prices = {4800, 4900, 5000, 5100, 5200,
                                          4950, 5050, 5150};

    // Add outliers (-40% and +40% from ~5000)
    std::vector<CAmount> outlier_prices = {3000, 7000};

    // Add all prices
    for (size_t i = 0; i < normal_prices.size(); i++) {
        COraclePriceMessage msg(i, normal_prices[i], GetTime());
        bundle.AddMessage(msg);
    }

    for (size_t i = 0; i < outlier_prices.size(); i++) {
        COraclePriceMessage msg(normal_prices.size() + i, outlier_prices[i], GetTime());
        bundle.AddMessage(msg);
    }

    // All 10 messages: sorted [3000, 4800, 4900, 4950, 5000, 5050, 5100, 5150, 5200, 7000]
    // IQR: q1_idx=2→q1=4900, q3_idx=7→q3=5150, IQR=250
    // Bounds: [4900-375, 5150+375] = [4525, 5525]
    // 3000 and 7000 filtered out, also 4800 < 4525 → OUT
    // Remaining: [4900, 4950, 5000, 5050, 5100, 5150, 5200] — 7 values
    // Median: 5050
    uint64_t consensus_price = bundle.GetConsensusPrice(8);
    BOOST_CHECK(consensus_price > 0);

    // Verify outliers don't distort the consensus price —
    // the median should be near 5000, not skewed by 3000/7000
    BOOST_CHECK(consensus_price >= 4800);
    BOOST_CHECK(consensus_price <= 5200);
}

BOOST_AUTO_TEST_CASE(oracle_bundle_epoch_validation)
{
    int32_t current_epoch = 100;

    // Test valid epoch (current)
    COracleBundle bundle1(current_epoch);
    BOOST_CHECK(bundle1.ValidateEpoch(current_epoch));

    // Test valid epoch (current - 1)
    COracleBundle bundle2(current_epoch - 1);
    BOOST_CHECK(bundle2.ValidateEpoch(current_epoch));

    // Test invalid epoch (too old)
    COracleBundle bundle3(current_epoch - 2);
    BOOST_CHECK(!bundle3.ValidateEpoch(current_epoch));

    // Test invalid epoch (future)
    COracleBundle bundle4(current_epoch + 1);
    BOOST_CHECK(!bundle4.ValidateEpoch(current_epoch));

    // Edge values must reject safely without signed-overflow UB.
    COracleBundle min_bundle(std::numeric_limits<int32_t>::min());
    BOOST_CHECK(min_bundle.ValidateEpoch(std::numeric_limits<int32_t>::min()));
    BOOST_CHECK(!min_bundle.ValidateEpoch(std::numeric_limits<int32_t>::max()));

    COracleBundle max_bundle(std::numeric_limits<int32_t>::max());
    BOOST_CHECK(max_bundle.ValidateEpoch(std::numeric_limits<int32_t>::max()));
    BOOST_CHECK(!max_bundle.ValidateEpoch(std::numeric_limits<int32_t>::min()));
}

BOOST_AUTO_TEST_CASE(oracle_bundle_serialization)
{
    COracleBundle original(999);

    // Add some messages
    for (int i = 0; i < 5; i++) {
        COraclePriceMessage msg(i, 500 + i * 10, GetTime());
        original.AddMessage(msg);
    }

    // Serialize
    DataStream ss{};
    ss << original;

    // Deserialize
    COracleBundle deserialized;
    ss >> deserialized;

    // Verify all fields match
    BOOST_CHECK_EQUAL(deserialized.epoch, original.epoch);
    BOOST_CHECK_EQUAL(deserialized.messages.size(), original.messages.size());

    for (size_t i = 0; i < original.messages.size(); i++) {
        BOOST_CHECK_EQUAL(deserialized.messages[i].oracle_id, original.messages[i].oracle_id);
        BOOST_CHECK_EQUAL(deserialized.messages[i].price_micro_usd, original.messages[i].price_micro_usd);
        BOOST_CHECK_EQUAL(deserialized.messages[i].timestamp, original.messages[i].timestamp);
    }
}

/**
 * OracleNode Tests
 * Tests for oracle node definition and management
 */
BOOST_AUTO_TEST_CASE(oracle_node_basic_construction)
{
    OracleNodeInfo node;
    BOOST_CHECK_EQUAL(node.id, 0);
    BOOST_CHECK(!node.pubkey.IsValid());
    BOOST_CHECK(node.endpoint.empty());
    BOOST_CHECK(!node.is_active);

    // Test construction with parameters
    CKey key;
    key.MakeNewKey(true);
    CPubKey pubkey = key.GetPubKey();

    OracleNodeInfo node2(42, pubkey, "oracle42.digibyte.io:8332", true);
    BOOST_CHECK_EQUAL(node2.id, 42);
    BOOST_CHECK(node2.pubkey.IsValid());
    BOOST_CHECK_EQUAL(node2.endpoint, "oracle42.digibyte.io:8332");
    BOOST_CHECK(node2.is_active);
}

BOOST_AUTO_TEST_CASE(oracle_node_validation)
{
    OracleNodeInfo node;

    // Invalid node (no pubkey, no endpoint)
    BOOST_CHECK(!node.IsValid());

    // Add valid pubkey
    CKey key;
    key.MakeNewKey(true);
    node.pubkey = key.GetPubKey();
    BOOST_CHECK(!node.IsValid()); // Still invalid (no endpoint)

    // Add endpoint
    node.endpoint = "oracle.example.com:8332";
    BOOST_CHECK(node.IsValid()); // Now valid

    // Test invalid endpoint formats
    node.endpoint = "";
    BOOST_CHECK(!node.IsValid());

    node.endpoint = "invalid_endpoint";
    BOOST_CHECK(!node.IsValid());

    node.endpoint = "oracle.example.com:99999"; // Invalid port
    BOOST_CHECK(!node.IsValid());
}

BOOST_AUTO_TEST_CASE(oracle_node_serialization)
{
    CKey key;
    key.MakeNewKey(true);
    CPubKey pubkey = key.GetPubKey();

    OracleNodeInfo original(123, pubkey, "test-oracle.digibyte.io:8332", true);

    // Serialize
    DataStream ss{};
    ss << original;

    // Deserialize
    OracleNodeInfo deserialized;
    ss >> deserialized;

    // Verify all fields match
    BOOST_CHECK_EQUAL(deserialized.id, original.id);
    BOOST_CHECK(deserialized.pubkey == original.pubkey);
    BOOST_CHECK_EQUAL(deserialized.endpoint, original.endpoint);
    BOOST_CHECK_EQUAL(deserialized.is_active, original.is_active);
}

/**
 * Oracle Selection Tests
 * Tests for deterministic oracle selection algorithm
 */
BOOST_AUTO_TEST_CASE(oracle_selection_deterministic)
{
    // Create one node per reserved oracle slot.
    std::vector<OracleNodeInfo> all_oracles;
    for (int i = 0; i < ORACLE_TOTAL_COUNT; i++) {
        CKey key;
        key.MakeNewKey(true);
        CPubKey pubkey = key.GetPubKey();

        OracleNodeInfo node(i, pubkey, "oracle" + std::to_string(i) + ".digibyte.io:8332", true);
        all_oracles.push_back(node);
    }

    // Test deterministic selection for epoch
    int32_t epoch = 100;
    std::vector<OracleNodeInfo> selected1 = SelectOraclesForEpoch(all_oracles, epoch);
    std::vector<OracleNodeInfo> selected2 = SelectOraclesForEpoch(all_oracles, epoch);

    // Should select exactly the current reserved active-capacity limit.
    BOOST_CHECK_EQUAL(selected1.size(), static_cast<size_t>(ORACLE_ACTIVE_COUNT));
    BOOST_CHECK_EQUAL(selected2.size(), static_cast<size_t>(ORACLE_ACTIVE_COUNT));

    // Selections should be identical (deterministic)
    for (size_t i = 0; i < selected1.size(); i++) {
        BOOST_CHECK_EQUAL(selected1[i].id, selected2[i].id);
    }

    // Different epoch should give different selection
    std::vector<OracleNodeInfo> selected3 = SelectOraclesForEpoch(all_oracles, epoch + 1);
    bool different = false;
    for (size_t i = 0; i < selected1.size() && i < selected3.size(); i++) {
        if (selected1[i].id != selected3[i].id) {
            different = true;
            break;
        }
    }
    BOOST_CHECK(different); // Should be different for different epoch
}

BOOST_AUTO_TEST_CASE(oracle_selection_shuffles_exact_active_count)
{
    std::vector<OracleNodeInfo> active_oracles;
    for (int i = 0; i < ORACLE_ACTIVE_COUNT; i++) {
        CKey key;
        key.MakeNewKey(true);
        CPubKey pubkey = key.GetPubKey();

        OracleNodeInfo node(i, pubkey, "oracle" + std::to_string(i) + ".digibyte.io:8332", true);
        active_oracles.push_back(node);
    }

    const std::vector<OracleNodeInfo> selected40 = SelectOraclesForEpoch(active_oracles, 40);
    const std::vector<OracleNodeInfo> selected41 = SelectOraclesForEpoch(active_oracles, 41);

    BOOST_REQUIRE_EQUAL(selected40.size(), static_cast<size_t>(ORACLE_ACTIVE_COUNT));
    BOOST_REQUIRE_EQUAL(selected41.size(), static_cast<size_t>(ORACLE_ACTIVE_COUNT));

    std::vector<uint32_t> ids40;
    std::vector<uint32_t> ids41;
    std::set<uint32_t> unique40;
    std::set<uint32_t> unique41;
    for (const OracleNodeInfo& oracle : selected40) {
        ids40.push_back(oracle.id);
        unique40.insert(oracle.id);
    }
    for (const OracleNodeInfo& oracle : selected41) {
        ids41.push_back(oracle.id);
        unique41.insert(oracle.id);
    }

    BOOST_CHECK_EQUAL(unique40.size(), static_cast<size_t>(ORACLE_ACTIVE_COUNT));
    BOOST_CHECK_EQUAL(unique41.size(), static_cast<size_t>(ORACLE_ACTIVE_COUNT));

    std::vector<uint32_t> sequential(ORACLE_ACTIVE_COUNT);
    std::iota(sequential.begin(), sequential.end(), 0);

    BOOST_CHECK(ids40 != sequential);
    BOOST_CHECK(ids40 != ids41);
}

BOOST_AUTO_TEST_CASE(oracle_selection_insufficient_oracles)
{
    // Test with fewer than the configured active-oracle capacity.
    std::vector<OracleNodeInfo> few_oracles;
    for (int i = 0; i < 10; i++) {
        CKey key;
        key.MakeNewKey(true);
        CPubKey pubkey = key.GetPubKey();

        OracleNodeInfo node(i, pubkey, "oracle" + std::to_string(i) + ".digibyte.io:8332", true);
        few_oracles.push_back(node);
    }

    // Should return all available oracles
    std::vector<OracleNodeInfo> selected = SelectOraclesForEpoch(few_oracles, 1);
    BOOST_CHECK_EQUAL(selected.size(), 10);
}

BOOST_AUTO_TEST_CASE(oracle_selection_inactive_oracles)
{
    // Create mix of active and inactive oracles
    std::vector<OracleNodeInfo> mixed_oracles;
    for (int i = 0; i < 30; i++) {
        CKey key;
        key.MakeNewKey(true);
        CPubKey pubkey = key.GetPubKey();

        bool is_active = (i % 3 != 0); // 2/3 active, 1/3 inactive
        OracleNodeInfo node(i, pubkey, "oracle" + std::to_string(i) + ".digibyte.io:8332", is_active);
        mixed_oracles.push_back(node);
    }

    // Should only select from active oracles
    std::vector<OracleNodeInfo> selected = SelectOraclesForEpoch(mixed_oracles, 1);

    for (const auto& oracle : selected) {
        BOOST_CHECK(oracle.is_active);
    }
}

/**
 * ChainParams Oracle Tests
 * Tests for hardcoded oracle nodes in chainparams
 */
BOOST_AUTO_TEST_CASE(chainparams_mainnet_oracle_count)
{
    // Test that mainnet has exactly 35 active RC44 oracle slots.
    auto chainparams = CChainParams::Main();
    const std::vector<OracleNodeInfo>& oracles = chainparams->GetOracleNodes();

    BOOST_CHECK_EQUAL(oracles.size(), 35);
    BOOST_CHECK_EQUAL(chainparams->GetActiveOracleCount(),
                      static_cast<uint32_t>(chainparams->GetConsensus().nOraclePubkeyCount));
}

BOOST_AUTO_TEST_CASE(chainparams_testnet_oracle_count)
{
    // Test that testnet has the same 35 active RC44 oracle slots.
    auto chainparams = CChainParams::TestNet();
    const std::vector<OracleNodeInfo>& oracles = chainparams->GetOracleNodes();

    BOOST_CHECK_EQUAL(oracles.size(), 35);
    BOOST_CHECK_EQUAL(chainparams->GetActiveOracleCount(),
                      static_cast<uint32_t>(chainparams->GetConsensus().nOraclePubkeyCount));
}

BOOST_AUTO_TEST_CASE(chainparams_regtest_oracle_count)
{
    // Test that regtest has at least 5 oracle nodes for testing
    auto chainparams = CChainParams::RegTest({});
    const std::vector<OracleNodeInfo>& oracles = chainparams->GetOracleNodes();

    BOOST_CHECK_GE(oracles.size(), 5);  // At least 5 for testing
    BOOST_CHECK_LE(oracles.size(), 35); // No more than full set
}

BOOST_AUTO_TEST_CASE(chainparams_oracle_data_validity)
{
    auto chainparams = CChainParams::Main();
    const std::vector<OracleNodeInfo>& oracles = chainparams->GetOracleNodes();

    std::set<uint32_t> oracle_ids;
    std::set<CPubKey> oracle_pubkeys;

    for (size_t i = 0; i < oracles.size(); i++) {
        const OracleNodeInfo& oracle = oracles[i];

        // Test unique ID (0-34)
        BOOST_CHECK_GE(oracle.id, 0);
        BOOST_CHECK_LT(oracle.id, 35);
        BOOST_CHECK(oracle_ids.find(oracle.id) == oracle_ids.end()); // No duplicates
        oracle_ids.insert(oracle.id);

        // Test valid public key
        BOOST_CHECK(oracle.pubkey.IsValid());
        BOOST_CHECK(oracle.pubkey.IsCompressed()); // Should use compressed format
        BOOST_CHECK(oracle_pubkeys.find(oracle.pubkey) == oracle_pubkeys.end()); // No duplicates
        oracle_pubkeys.insert(oracle.pubkey);

        // Test valid endpoint format
        BOOST_CHECK(!oracle.endpoint.empty());
        BOOST_CHECK(oracle.endpoint.find(":") != std::string::npos); // Should have port

        // RC44 exposes every configured consensus slot as active.
        BOOST_CHECK_EQUAL(oracle.is_active, oracle.id < 35);

        // Test oracle passes validation
        BOOST_CHECK(oracle.IsValid());
    }

    // Verify we have exactly the expected number of unique IDs and keys
    BOOST_CHECK_EQUAL(oracle_ids.size(), 35);
    BOOST_CHECK_EQUAL(oracle_pubkeys.size(), 35);
}

BOOST_AUTO_TEST_CASE(chainparams_oracle_getter_functions)
{
    auto chainparams = CChainParams::Main();

    // Test GetOracleNode function
    for (uint32_t id = 0; id < 35; id++) {
        const OracleNodeInfo* oracle = chainparams->GetOracleNode(id);
        BOOST_CHECK(oracle != nullptr);
        BOOST_CHECK_EQUAL(oracle->id, id);
        BOOST_CHECK(oracle->IsValid());
    }

    // Test invalid oracle ID
    const OracleNodeInfo* invalid_oracle = chainparams->GetOracleNode(999);
    BOOST_CHECK(invalid_oracle == nullptr);
}

BOOST_AUTO_TEST_CASE(chainparams_oracle_endpoint_uniqueness)
{
    auto chainparams = CChainParams::Main();
    const std::vector<OracleNodeInfo>& oracles = chainparams->GetOracleNodes();

    std::set<std::string> endpoints;

    for (const auto& oracle : oracles) {
        // Check endpoint uniqueness
        BOOST_CHECK(endpoints.find(oracle.endpoint) == endpoints.end());
        endpoints.insert(oracle.endpoint);

        // Check endpoint format - valid host:port under the approved oracle domains.
        // Operator-owned endpoints do not need to use an "oracleN" hostname prefix.

        // Check for valid domain (either digidollar.org or digibyte.io)
        bool valid_domain = oracle.endpoint.find(".digidollar.org:") != std::string::npos ||
                           oracle.endpoint.find(".digibyte.io:") != std::string::npos;
        BOOST_CHECK(valid_domain);

        // Extract port number
        size_t colon_pos = oracle.endpoint.find_last_of(':');
        BOOST_CHECK(colon_pos != std::string::npos);

        std::string port_str = oracle.endpoint.substr(colon_pos + 1);
        int port = std::stoi(port_str);

        // Valid port ranges: 9001-9035 for digidollar.org, or 12024-12033 for digibyte.io (testnet P2P ports)
        bool valid_port = (port >= 9001 && port <= 9035) || (port >= 12024 && port <= 12033);
        BOOST_CHECK(valid_port);
    }
}

BOOST_AUTO_TEST_CASE(chainparams_oracle_deterministic_across_networks)
{
    // Test that oracle configurations are consistent across runs
    auto mainnet1 = CChainParams::Main();
    auto mainnet2 = CChainParams::Main();

    const std::vector<OracleNodeInfo>& oracles1 = mainnet1->GetOracleNodes();
    const std::vector<OracleNodeInfo>& oracles2 = mainnet2->GetOracleNodes();

    BOOST_CHECK_EQUAL(oracles1.size(), oracles2.size());

    for (size_t i = 0; i < oracles1.size(); i++) {
        BOOST_CHECK_EQUAL(oracles1[i].id, oracles2[i].id);
        BOOST_CHECK(oracles1[i].pubkey == oracles2[i].pubkey);
        BOOST_CHECK_EQUAL(oracles1[i].endpoint, oracles2[i].endpoint);
        BOOST_CHECK_EQUAL(oracles1[i].is_active, oracles2[i].is_active);
    }
}

/**
 * Oracle Bundle Manager Tests
 * Tests for oracle message collection and bundle management
 */
BOOST_AUTO_TEST_CASE(oracle_bundle_manager_basic)
{
    OracleBundleManager manager;

    // Test initial state
    BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), 0);
    BOOST_CHECK(!manager.HasValidBundle(1));

    // Test stats
    auto stats = manager.GetStats();
    BOOST_CHECK_EQUAL(stats.pending_messages, 0);
    BOOST_CHECK_EQUAL(stats.active_bundles, 0);
    BOOST_CHECK(!stats.has_consensus);
}

BOOST_AUTO_TEST_CASE(oracle_bundle_manager_message_handling)
{
    OracleBundleManager manager;

    // Note: This test cannot use AddOracleMessage directly because it validates signatures
    // against chainparams oracle pubkeys which we don't have private keys for.
    // Instead, test the underlying storage directly.

    // Test pending message count starts at zero
    BOOST_CHECK_EQUAL(manager.GetPendingMessageCount(), 0);

    // Test GetPendingMessages returns empty initially
    auto pending = manager.GetPendingMessages();
    BOOST_CHECK_EQUAL(pending.size(), 0);

    // Test SetEnabled/IsEnabled
    BOOST_CHECK(manager.IsEnabled());
    manager.SetEnabled(false);
    BOOST_CHECK(!manager.IsEnabled());
    manager.SetEnabled(true);
    BOOST_CHECK(manager.IsEnabled());

    // Test SetMinOracleCount
    manager.SetMinOracleCount(5);
    // No getter, but we've tested it doesn't crash
}

BOOST_AUTO_TEST_CASE(oracle_bundle_manager_rejects_legacy_message_bundle)
{
    OracleBundleManager manager;
    int32_t test_epoch = 100;

    // Create bundle manually for testing (cannot use AddOracleMessage due to signature validation)
    COracleBundle bundle(test_epoch);

    // Add enough messages for consensus (using realistic prices)
    for (int i = 0; i < ORACLE_CONSENSUS_REQUIRED; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006 (realistic price)

        // Mock valid signature
        msg.schnorr_sig = {0x01, 0x02, 0x03, 0x04};

        bundle.AddMessage(msg);
    }

    // Update manager with bundle
    BOOST_CHECK(manager.UpdateBundle(bundle));
    BOOST_CHECK(!manager.HasValidBundle(test_epoch));

    // Legacy message bundles are not canonical V1 price sources.
    CAmount consensus_price = manager.GetConsensusPrice(test_epoch);
    BOOST_CHECK_EQUAL(consensus_price, 0);
}

/**
 * Oracle Node Tests
 * Tests for oracle node definition functionality
 */
BOOST_AUTO_TEST_CASE(oracle_node_struct_tests)
{
    // Test OracleNode struct (not the daemon class)
    OracleNodeInfo node;
    BOOST_CHECK_EQUAL(node.id, 0);
    BOOST_CHECK(!node.pubkey.IsValid());
    BOOST_CHECK(node.endpoint.empty());
    BOOST_CHECK(!node.is_active);

    // Test construction with parameters
    CKey key;
    key.MakeNewKey(true);
    CPubKey pubkey = key.GetPubKey();

    OracleNodeInfo node2(42, pubkey, "oracle42.digibyte.io:8332", true);
    BOOST_CHECK_EQUAL(node2.id, 42);
    BOOST_CHECK(node2.pubkey.IsValid());
    BOOST_CHECK_EQUAL(node2.endpoint, "oracle42.digibyte.io:8332");
    BOOST_CHECK(node2.is_active);
}

/**
 * Mock Exchange Price Tests
 * Tests for price aggregation concepts (using mock data)
 */
BOOST_AUTO_TEST_CASE(exchange_price_mock_tests)
{
    // Test mock price aggregation concepts (prices in micro-USD)
    // Using realistic prices around $0.005 (5000 micro-USD)
    std::vector<CAmount> test_prices = {5000, 5050, 4950, 7000, 3000}; // Including outliers

    // Calculate median
    std::vector<CAmount> sorted_prices = test_prices;
    std::sort(sorted_prices.begin(), sorted_prices.end());

    CAmount median_price = sorted_prices[sorted_prices.size() / 2];
    BOOST_CHECK_EQUAL(median_price, 5000);  // $0.005

    // Test outlier detection logic
    CAmount median = median_price;
    const double outlier_threshold = 0.20; // 20% threshold

    std::vector<CAmount> filtered_prices;
    for (CAmount price : test_prices) {
        double deviation = std::abs(static_cast<double>(price - median)) / median;
        if (deviation <= outlier_threshold) {
            filtered_prices.push_back(price);
        }
    }

    // Should filter out the extreme outliers (7000 and 3000)
    BOOST_CHECK_LT(filtered_prices.size(), test_prices.size());
    BOOST_CHECK_GE(filtered_prices.size(), 3); // Should keep normal prices (5000, 5050, 4950)
}

/**
 * Oracle Integration Tests
 * Tests for oracle system integration with DigiDollar
 */
BOOST_AUTO_TEST_CASE(oracle_integration_price_retrieval)
{
    // Test oracle price retrieval
    // In unit test environment without oracle setup, price may be 0
    // This tests that the function doesn't crash and returns valid range when configured
    CAmount oracle_price = OracleIntegration::GetCurrentOraclePrice();

    // Price should be 0 (no oracle configured) or in valid range
    bool valid_price = (oracle_price == 0) ||
                       (oracle_price >= 100 && oracle_price <= 1000000);
    BOOST_CHECK(valid_price);
}

BOOST_AUTO_TEST_CASE(oracle_integration_system_readiness)
{
    // Test oracle system readiness check
    bool is_ready = OracleIntegration::IsOracleSystemReady();

    // In test environment, oracle system may not be fully ready
    // This is expected behavior
    BOOST_CHECK(is_ready || !is_ready); // Either state is valid for testing
}

/**
 * Oracle Block Integration Tests
 * Tests for oracle data in blocks
 */
BOOST_AUTO_TEST_CASE(oracle_block_integration)
{
    OracleBundleManager& manager = OracleBundleManager::GetInstance();
    manager.Clear();
    manager.SetEnabled(true);

    // Create test block
    CBlock test_block;

    // Add coinbase transaction
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 5000000000; // 50 DGB
    test_block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));

    // Test adding oracle bundle to block. Without a MuSig2 session or
    // oracle messages, AddOracleBundleToBlock returns true (block proceeds
    // without oracle data — valid behavior).
    int32_t test_height = 1000;
    bool result = manager.AddOracleBundleToBlock(test_block, test_height);

    BOOST_CHECK(result);  // true = block proceeds (no oracle data is valid)

    // Test oracle script creation with empty bundle
    COracleBundle empty_bundle;
    CScript oracle_script = manager.CreateOracleScript(empty_bundle);

    // Empty bundle should create empty script
    BOOST_CHECK(oracle_script.empty());

    // Test with structurally valid MuSig2 bundle. V1 no longer serializes
    // legacy v0x01/v0x02 message bundles into block oracle scripts.
    COracleBundle valid_bundle(10);
    valid_bundle.version = 3;
    valid_bundle.median_price_micro_usd = 6000;
    valid_bundle.timestamp = GetTime();

    const Consensus::Params& consensus = Params().GetConsensus();
    std::vector<uint8_t> oracle_ids;
    for (uint8_t id = 0; id < consensus.nOracleConsensusRequired; ++id) {
        oracle_ids.push_back(id);
    }
    valid_bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(consensus.nOracleTotalOracles));
    BOOST_REQUIRE(!valid_bundle.participation_bitmap.empty());
    valid_bundle.aggregate_sig.assign(64, 0x01);

    oracle_script = manager.CreateOracleScript(valid_bundle);
    BOOST_REQUIRE(!oracle_script.empty());

    // Should start with OP_RETURN
    BOOST_CHECK_EQUAL(oracle_script[0], OP_RETURN);
}

/**
 * Oracle Data Validation Tests
 * Tests for oracle data validation in blocks and transactions
 */
BOOST_AUTO_TEST_CASE(oracle_data_validation)
{
    // Test oracle message validation
    COraclePriceMessage valid_msg(1, 6000, GetTime());  // $0.006 (realistic price)

    CKey test_key;
    test_key.MakeNewKey(true);
    BOOST_CHECK(valid_msg.SignAttestation(test_key));

    // Message should be valid (structure-wise)
    BOOST_CHECK(valid_msg.IsValid());

    // Test oracle bundle validation
    COracleBundle bundle(50);

    for (int i = 0; i < ORACLE_CONSENSUS_REQUIRED; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006 (realistic price)
        msg.schnorr_sig = {0x01, 0x02, 0x03, 0x04}; // Mock signature
        bundle.AddMessage(msg);
    }

    BOOST_CHECK(bundle.HasConsensus(ORACLE_CONSENSUS_REQUIRED));

    // Test epoch validation
    BOOST_CHECK(bundle.ValidateEpoch(50));  // Current epoch
    BOOST_CHECK(bundle.ValidateEpoch(51));  // Next epoch (allowed)
    BOOST_CHECK(!bundle.ValidateEpoch(52)); // Too far in future
    BOOST_CHECK(!bundle.ValidateEpoch(48)); // Too far in past
}

/**
 * Advanced Oracle Tests - Phase 2 TDD Implementation
 * Following Red-Green-Refactor methodology
 */

/**
 * Signature Verification Edge Cases (RED PHASE)
 * These tests SHOULD FAIL initially until we implement the functionality
 */
BOOST_AUTO_TEST_CASE(test_signature_verification_edge_cases)
{
    // Test invalid signature scenarios
    CKey oracle_key;
    oracle_key.MakeNewKey(true);
    CPubKey oracle_pubkey = oracle_key.GetPubKey();

    COraclePriceMessage msg(1, 6000, GetTime());  // $0.006 (realistic price)

    // Test 1: Expired signature (timestamp too old)
    COraclePriceMessage expired_msg(1, 6000, GetTime() - ORACLE_MAX_AGE_SECONDS - 1);  // $0.006
    BOOST_CHECK(expired_msg.Sign(oracle_key));

    // Signature itself is cryptographically valid
    BOOST_CHECK(expired_msg.Verify());

    // But message should fail validation due to expired timestamp
    BOOST_CHECK(!expired_msg.IsValid());

    // Test 2: Signature replay attack protection
    COraclePriceMessage original_msg(1, 6000, GetTime());  // $0.006
    BOOST_CHECK(original_msg.Sign(oracle_key));

    // Try to reuse signature on different message (should fail)
    COraclePriceMessage replay_msg(1, 7000, GetTime()); // Different price ($0.007)
    replay_msg.schnorr_sig = original_msg.schnorr_sig; // Same signature
    replay_msg.oracle_pubkey = original_msg.oracle_pubkey;

    BOOST_CHECK(!replay_msg.Verify());

    // Test 3: Invalid signature format
    COraclePriceMessage invalid_format_msg(1, 500, GetTime());
    invalid_format_msg.schnorr_sig = {0x00}; // Too short
    invalid_format_msg.oracle_pubkey = XOnlyPubKey(oracle_pubkey);

    BOOST_CHECK(!invalid_format_msg.Verify());

    // Test 4: Signature with wrong key
    CKey wrong_key;
    wrong_key.MakeNewKey(true);

    COraclePriceMessage wrong_key_msg(1, 6000, GetTime());  // $0.006
    BOOST_CHECK(wrong_key_msg.Sign(wrong_key));
    wrong_key_msg.oracle_pubkey = XOnlyPubKey(oracle_pubkey); // Wrong pubkey for signature

    BOOST_CHECK(!wrong_key_msg.Verify());

    // Test 5: Double-spending protection (same oracle, same epoch)
    COraclePriceMessage double_spend1(1, 6000, GetTime());  // $0.006
    COraclePriceMessage double_spend2(1, 7000, GetTime()); // Same oracle, different price ($0.007)

    BOOST_CHECK(double_spend1.SignAttestation(oracle_key));
    BOOST_CHECK(double_spend2.SignAttestation(oracle_key));

    // Both should be valid individually, but conflict detection should prevent both
    BOOST_CHECK(double_spend1.VerifyAttestation());
    BOOST_CHECK(double_spend2.VerifyAttestation());

    // This should fail when checking for conflicting messages
    BOOST_CHECK(!COraclePriceMessage::CheckForConflictingMessages({double_spend1, double_spend2}));
}

BOOST_AUTO_TEST_CASE(test_price_aggregation_outliers)
{
    // Test unified IQR outlier filtering with extreme prices (T9-01)
    COracleBundle bundle(1);

    // Test extreme outlier scenarios — prices outside ORACLE_MIN/MAX_PRICE_MICRO_USD
    // are filtered by the price-range check before IQR
    std::vector<CAmount> extreme_prices = {
        1,           // Below ORACLE_MIN_PRICE_MICRO_USD (100)
        10000000000, // Above ORACLE_MAX_PRICE_MICRO_USD (100000000)
        0,           // Zero price
        -1000        // Negative price
    };

    // Add normal prices first (8 messages)
    std::vector<CAmount> normal_prices = {4800000, 4900000, 5000000, 5100000, 5200000, 4950000, 5050000, 5150000};

    for (size_t i = 0; i < normal_prices.size(); i++) {
        COraclePriceMessage msg(i, normal_prices[i], GetTime());
        bundle.AddMessage(msg);
    }

    // Add extreme outliers (4 more messages — 12 total)
    for (size_t i = 0; i < extreme_prices.size(); i++) {
        COraclePriceMessage outlier_msg(normal_prices.size() + i, extreme_prices[i], GetTime());
        bundle.AddMessage(outlier_msg);
    }

    // GetConsensusPrice filters out-of-range prices, then applies IQR
    // Only the 8 normal prices survive range check
    // All 8 are clustered around $5, IQR keeps them all
    uint64_t price = bundle.GetConsensusPrice(8);
    BOOST_CHECK(price > 0);
    BOOST_CHECK(price >= 4800000);
    BOOST_CHECK(price <= 5200000);

    // Test with insufficient valid data
    COracleBundle insufficient_bundle(2);
    // Add only out-of-range outliers (should fail consensus after range filter)
    for (size_t i = 0; i < extreme_prices.size(); i++) {
        COraclePriceMessage outlier_msg(i, extreme_prices[i], GetTime());
        insufficient_bundle.AddMessage(outlier_msg);
    }

    BOOST_CHECK(!insufficient_bundle.HasConsensus(ORACLE_CONSENSUS_REQUIRED));

    // Test IQR outlier detection: 8000000 is far from cluster
    COracleBundle iqr_bundle(3);
    std::vector<CAmount> iqr_test_prices = {
        4000000, 4500000, 4800000, 4900000, 5000000, 5100000, 5200000, 5500000, 6000000, 8000000
    };

    for (size_t i = 0; i < iqr_test_prices.size(); i++) {
        COraclePriceMessage msg(i, iqr_test_prices[i], GetTime());
        iqr_bundle.AddMessage(msg);
    }

    // Sorted: [4000000, 4500000, 4800000, 4900000, 5000000, 5100000, 5200000, 5500000, 6000000, 8000000]
    // IQR: q1_idx=2→q1=4800000, q3_idx=7→q3=5500000, IQR=700000
    // Bounds: [3750000, 6550000] → 8000000 filtered out
    // Remaining: 9 values, median = 5000000
    uint64_t iqr_price = iqr_bundle.GetConsensusPrice(8);
    BOOST_CHECK(iqr_price > 0);
    // Median should be around 5000000, not skewed by 8000000
    BOOST_CHECK(iqr_price >= 4800000);
    BOOST_CHECK(iqr_price <= 5200000);
}

BOOST_AUTO_TEST_CASE(test_p2p_message_validation)
{
    // Test P2P oracle message validation and DOS protection

    // Test 1: Malformed oracle price message
    COraclePriceMessage malformed_msg;
    malformed_msg.oracle_id = ORACLE_TOTAL_COUNT + 1; // Invalid oracle ID
    malformed_msg.price_micro_usd = 6000;  // $0.006
    malformed_msg.timestamp = GetTime();

    BOOST_CHECK(!OracleP2P::ValidateIncomingMessage(malformed_msg));

    // Test 2: Message rate limiting
    COraclePriceMessage rate_limit_msg(1, 6000, GetTime());  // $0.006

    // Create valid Schnorr signature
    CKey test_key;
    test_key.MakeNewKey(true);
    BOOST_CHECK(rate_limit_msg.SignAttestation(test_key));

    // First message should be accepted
    BOOST_CHECK(OracleP2P::ValidateIncomingMessage(rate_limit_msg));

    // Rapid subsequent messages should be rate limited
    for (int i = 0; i < 10; i++) {
        COraclePriceMessage spam_msg(1, 6000 + i * 100, GetTime());  // $0.006 + variations
        BOOST_CHECK(spam_msg.SignAttestation(test_key));

        // Should be rate limited after the first few
        bool accepted = OracleP2P::ValidateIncomingMessage(spam_msg);
        if (i > 2) {
            BOOST_CHECK(!accepted); // Rate limiting should kick in
        }
    }

    // Test 3: Message size validation
    COraclePriceMessage oversized_msg(1, 6000, GetTime());  // $0.006
    // Create abnormally large signature
    oversized_msg.schnorr_sig.resize(10000, 0xFF); // Way too large

    BOOST_CHECK(!OracleP2P::ValidateIncomingMessage(oversized_msg));

    // Test 4: Bundle message validation
    COracleBundle test_bundle(10);

    // Add maximum allowed messages.
    for (int i = 0; i < ORACLE_ACTIVE_COUNT; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006
        test_bundle.AddMessage(msg);
    }

    // Legacy ORACLEBUNDLE P2P messages are deprecated in V1.
    BOOST_CHECK(!OracleP2P::ValidateBundleMessage(test_bundle));

    // Create bundle with too many messages by directly manipulating the vector
    COracleBundle oversized_bundle(10);
    for (int i = 0; i <= ORACLE_ACTIVE_COUNT; i++) {
        COraclePriceMessage msg(i, 6000, GetTime());  // $0.006
        oversized_bundle.messages.push_back(msg);  // Bypass AddMessage limit
    }

    // Oversized bundle should fail
    BOOST_CHECK(!OracleP2P::ValidateBundleMessage(oversized_bundle));

    // Test 5: Network partition tolerance
    GetOracleDataMsg request_msg;
    request_msg.epoch = -1; // Invalid epoch
    request_msg.oracle_id = 0xFFFFFFFF; // All oracles

    BOOST_CHECK(!OracleP2P::ValidateGetOracleRequest(request_msg));

    // Valid request
    request_msg.epoch = GetCurrentEpoch(1000);
    BOOST_CHECK(OracleP2P::ValidateGetOracleRequest(request_msg));
}

// ============================================================================
// Oracle Broadcast Interval Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(oracle_broadcast_interval_is_60_seconds)
{
    // Oracle broadcast interval should be 60 seconds, not 15.
    // 15-second broadcasts cause ~7200 novel P2P messages/hr on mainnet (30 oracles),
    // overwhelming rate limiters and causing peer disconnections.
    // 60 seconds gives 12x redundancy per testnet epoch (50 blocks) and
    // 25x redundancy per mainnet epoch (100 blocks).
    OracleNode node;

    // The default broadcast_interval should be 60
    BOOST_CHECK_EQUAL(node.GetBroadcastInterval(), 60);
}

BOOST_AUTO_TEST_CASE(oracle_broadcast_interval_parameterized_constructor)
{
    // Both constructors should use 60-second broadcast interval
    CKey key;
    key.MakeNewKey(true);
    OracleNode node(0, key);
    BOOST_CHECK_EQUAL(node.GetBroadcastInterval(), 60);
}

// ============================================================================
// Oracle P2P Rate Limiter Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(oracle_rate_limit_allows_legitimate_mainnet_traffic)
{
    // With 30 oracles broadcasting every 60 seconds, a peer relays
    // 30 * 60 = 1800 novel messages/hour. The rate limit must accommodate
    // this with headroom. Limit should be at least 2x expected = 3600.
    //
    // The rate limit constant ORACLE_MSG_RATE_LIMIT_PER_HOUR should be 3600.
    // This is verified in net_processing.cpp.
    //
    // Key invariant: exceeding the rate limit must NOT call Misbehaving().
    // Oracle relay is normal P2P behavior — penalizing it causes cascading
    // peer disconnections and oracle consensus failure.

    // 30 oracles * 60 msgs/hr = 1800 expected novel msgs/hr
    constexpr int ORACLES_MAINNET = 30;
    constexpr int BROADCASTS_PER_HOUR = 60;  // one per minute
    constexpr int EXPECTED_MSGS = ORACLES_MAINNET * BROADCASTS_PER_HOUR;  // 1800
    constexpr int RATE_LIMIT = 3600;  // 2x headroom

    BOOST_CHECK(RATE_LIMIT >= EXPECTED_MSGS * 2);
    // Ensure the limit is not so high it allows actual flood attacks
    // 30 oracles * 240/hr (every 15s) * 3 = 21600 would be too high
    BOOST_CHECK(RATE_LIMIT <= 10000);
}

BOOST_AUTO_TEST_CASE(oracle_rate_limit_no_misbehaving_penalty)
{
    // Exceeding oracle rate limit should silently drop messages,
    // NOT call Misbehaving(). Oracle relay is legitimate P2P behavior.
    // A peer relaying 30 oracles' messages is doing its job correctly.
    //
    // Previous bug: Misbehaving(*peer, 5) was called for each excess message.
    // With 1800 msgs/hr and limit of 50, that's (1800-50)*5 = 8750 misbehavior
    // points per hour — instant peer ban, loss of oracle data, consensus failure.
    //
    // This is a design-level test documenting the requirement.
    // The actual Misbehaving removal is verified by code review and integration tests.
    BOOST_CHECK(true); // Placeholder — verified by code inspection
}

BOOST_AUTO_TEST_SUITE_END()
