// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <crypto/sha256.h>
#include <oracle/node.h>
#include <span.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

namespace {

CKey GetDeterministicRegtestOracleKey(uint32_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    CKey key;
    key.Set(hash.begin(), hash.end(), true);
    return key;
}

std::string GetOracleKeyHex(const CKey& key)
{
    return HexStr(Span<const unsigned char>(key.begin(), key.end()));
}

void ResetOracleManager()
{
    OracleManager::StopOracleService();
}

class OracleWalletAutoStartSetup : public TestingSetup
{
public:
    OracleWalletAutoStartSetup() : TestingSetup(ChainType::REGTEST)
    {
        ResetOracleManager();
    }

    ~OracleWalletAutoStartSetup()
    {
        ResetOracleManager();
    }

    std::shared_ptr<wallet::CWallet> CreateWallet(std::unique_ptr<wallet::WalletDatabase> database,
                                                  const std::string& name,
                                                  bool attach_chain = true)
    {
        wallet::WalletContext context;
        context.args = m_node.args;
        context.chain = attach_chain ? m_node.chain.get() : nullptr;

        bilingual_str error;
        std::vector<bilingual_str> warnings;
        auto wallet = wallet::CWallet::Create(context, name, std::move(database), wallet::WALLET_FLAG_DESCRIPTORS, error, warnings);
        BOOST_REQUIRE_MESSAGE(wallet, error.original);
        return wallet;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(oracle_wallet_autostart_tests, OracleWalletAutoStartSetup)

BOOST_AUTO_TEST_CASE(oracle_autostart_unencrypted_wallet)
{
    static constexpr uint32_t oracle_id{0};
    const CKey oracle_key = GetDeterministicRegtestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    auto database = wallet::CreateMockableWalletDatabase();
    {
        wallet::WalletBatch batch(*database);
        BOOST_REQUIRE(batch.WriteOracleKey(oracle_id, oracle_key));
    }

    auto wallet = CreateWallet(std::move(database), "oracle-autostart-unencrypted");
    BOOST_REQUIRE(wallet);

    OracleNode* oracle = OracleManager::GetInstance().GetOracleNode(oracle_id);
    BOOST_REQUIRE(oracle != nullptr);
    BOOST_CHECK(oracle->IsEnabled());
    BOOST_CHECK(oracle->IsRunning());
}

BOOST_AUTO_TEST_CASE(oracle_no_autostart_locked_wallet)
{
    static constexpr uint32_t oracle_id{0};
    const CKey oracle_key = GetDeterministicRegtestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    auto wallet = CreateWallet(wallet::CreateMockableWalletDatabase(), "oracle-locked-wallet");
    BOOST_REQUIRE(wallet->StoreOracleKey(oracle_id, oracle_key));
    BOOST_REQUIRE(wallet->EncryptWallet("autostart-pass"));
    BOOST_REQUIRE(wallet->IsCrypted());
    BOOST_REQUIRE(wallet->IsLocked());

    {
        ASSERT_DEBUG_LOG("Oracle key found for ID 0 but wallet is locked. Run walletpassphrase then startoracle to enable.");
        wallet->TryAutoStartOracles();
    }

    BOOST_CHECK(OracleManager::GetInstance().GetOracleNode(oracle_id) == nullptr);
}

BOOST_AUTO_TEST_CASE(oracle_autostart_after_unlock)
{
    static constexpr uint32_t oracle_id{0};
    const CKey oracle_key = GetDeterministicRegtestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    auto wallet = CreateWallet(wallet::CreateMockableWalletDatabase(), "oracle-autostart-after-unlock");
    BOOST_REQUIRE(wallet->StoreOracleKey(oracle_id, oracle_key));
    BOOST_REQUIRE(wallet->EncryptWallet("autostart-pass"));
    BOOST_REQUIRE(wallet->IsLocked());

    wallet->TryAutoStartOracles();
    BOOST_CHECK(OracleManager::GetInstance().GetOracleNode(oracle_id) == nullptr);

    BOOST_REQUIRE(wallet->Unlock("autostart-pass"));
    wallet->TryAutoStartOracles();

    OracleNode* oracle = OracleManager::GetInstance().GetOracleNode(oracle_id);
    BOOST_REQUIRE(oracle != nullptr);
    BOOST_CHECK(oracle->IsEnabled());
    BOOST_CHECK(oracle->IsRunning());
}

BOOST_AUTO_TEST_CASE(oracle_no_autostart_for_detached_wallet)
{
    static constexpr uint32_t oracle_id{0};
    const CKey oracle_key = GetDeterministicRegtestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    auto database = wallet::CreateMockableWalletDatabase();
    {
        wallet::WalletBatch batch(*database);
        BOOST_REQUIRE(batch.WriteOracleKey(oracle_id, oracle_key));
    }

    auto wallet = CreateWallet(std::move(database), "oracle-detached-wallet", /*attach_chain=*/false);
    BOOST_REQUIRE(wallet);
    BOOST_CHECK(OracleManager::GetInstance().GetOracleNode(oracle_id) == nullptr);
}

BOOST_AUTO_TEST_CASE(oracle_no_key_no_start)
{
    auto wallet = CreateWallet(wallet::CreateMockableWalletDatabase(), "oracle-no-key");
    wallet->TryAutoStartOracles();

    OracleManager& manager = OracleManager::GetInstance();
    for (uint32_t oracle_id = 0; oracle_id < 7; ++oracle_id) {
        BOOST_CHECK(manager.GetOracleNode(oracle_id) == nullptr);
    }
}

BOOST_AUTO_TEST_CASE(oracle_already_running_skip)
{
    static constexpr uint32_t oracle_id{0};
    const CKey oracle_key = GetDeterministicRegtestOracleKey(oracle_id);
    BOOST_REQUIRE(oracle_key.IsValid());

    auto wallet = CreateWallet(wallet::CreateMockableWalletDatabase(), "oracle-already-running");
    BOOST_REQUIRE(wallet->StoreOracleKey(oracle_id, oracle_key));

    OracleManager& manager = OracleManager::GetInstance();
    BOOST_REQUIRE(manager.AddOracleNode(oracle_id, GetOracleKeyHex(oracle_key)));
    OracleNode* existing_node = manager.GetOracleNode(oracle_id);
    BOOST_REQUIRE(existing_node != nullptr);

    {
        ASSERT_DEBUG_LOG("Oracle: Oracle 0 already initialized in manager. Skipping auto-start.");
        wallet->TryAutoStartOracles();
    }

    BOOST_CHECK(manager.GetOracleNode(oracle_id) == existing_node);
    BOOST_CHECK(manager.RemoveOracleNode(oracle_id));
    BOOST_CHECK(!manager.RemoveOracleNode(oracle_id));
}

BOOST_AUTO_TEST_SUITE_END()
