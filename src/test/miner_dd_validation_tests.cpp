// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <util/int128.h>

#include <chainparams.h>
#include <util/int128.h>
#include <consensus/digidollar.h>
#include <util/int128.h>
#include <crypto/sha256.h>
#include <util/int128.h>
#include <digidollar/digidollar.h>
#include <util/int128.h>
#include <digidollar/scripts.h>
#include <util/int128.h>
#include <digidollar/txbuilder.h>
#include <util/int128.h>
#include <digidollar/validation.h>
#include <util/int128.h>
#include <hash.h>
#include <util/int128.h>
#include <key.h>
#include <util/int128.h>
#include <node/miner.h>
#include <util/int128.h>
#include <oracle/bundle_manager.h>
#include <util/int128.h>
#include <oracle/mock_oracle.h>
#include <util/int128.h>
#include <oracle/musig2_aggregator.h>
#include <util/int128.h>
#include <policy/feerate.h>
#include <util/int128.h>
#include <primitives/transaction.h>
#include <util/int128.h>
#include <random.h>
#include <util/int128.h>
#include <script/script.h>
#include <util/int128.h>
#include <test/util/setup_common.h>
#include <util/int128.h>
#include <test/util/txmempool.h>
#include <util/int128.h>
#include <validation.h>
#include <util/int128.h>

#include <secp256k1.h>
#include <util/int128.h>
#include <secp256k1_musig.h>
#include <util/int128.h>
#include <secp256k1_schnorrsig.h>
#include <util/int128.h>

#include <array>
#include <util/int128.h>
#include <cstring>
#include <util/int128.h>
#include <string>
#include <util/int128.h>
#include <vector>
#include <util/int128.h>

using node::BlockAssembler;
using node::CBlockTemplate;

namespace {

std::array<unsigned char, 32> RegtestOracleSecret(uint8_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());
    return secret;
}

bool SignRegtestV03Bundle(COracleBundle& bundle, const std::vector<uint8_t>& oracle_ids)
{
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        seckeys[i] = RegtestOracleSecret(oracle_ids[i]);
        if (!secp256k1_keypair_create(ctx, &keypairs[i], seckeys[i].data()) ||
            !secp256k1_keypair_pub(ctx, &pubkeys[i], &keypairs[i])) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_pubkey*> pubkey_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        pubkey_ptrs[i] = &pubkeys[i];
    }

    secp256k1_xonly_pubkey agg_pk{};
    secp256k1_musig_keyagg_cache cache{};
    if (!secp256k1_musig_pubkey_agg(ctx, &agg_pk, &cache, pubkey_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_secnonce> secnonces(n_signers);
    std::vector<secp256k1_musig_pubnonce> pubnonces(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        unsigned char session_rand[32];
        GetStrongRandBytes(Span{session_rand, 32});
        if (!secp256k1_musig_nonce_gen(ctx, &secnonces[i], &pubnonces[i],
                                       session_rand, seckeys[i].data(), &pubkeys[i],
                                       nullptr, &cache, nullptr)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_musig_pubnonce*> nonce_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        nonce_ptrs[i] = &pubnonces[i];
    }

    secp256k1_musig_aggnonce aggnonce{};
    if (!secp256k1_musig_nonce_agg(ctx, &aggnonce, nonce_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const uint256 msg_hash = ComputeOracleBundleHash(bundle);
    unsigned char msg32[32];
    std::memcpy(msg32, msg_hash.begin(), sizeof(msg32));

    secp256k1_musig_session session{};
    if (!secp256k1_musig_nonce_process(ctx, &session, &aggnonce, msg32, &cache)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_partial_sig> partial_sigs(n_signers);
    std::vector<const secp256k1_musig_partial_sig*> partial_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        if (!secp256k1_musig_partial_sign(ctx, &partial_sigs[i], &secnonces[i],
                                          &keypairs[i], &cache, &session)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        partial_ptrs[i] = &partial_sigs[i];
    }

    bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(Params().GetConsensus().nOracleTotalOracles));
    bundle.aggregate_sig.assign(64, 0);
    if (!secp256k1_musig_partial_sig_agg(ctx, bundle.aggregate_sig.data(),
                                         &session, partial_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const bool verifies = secp256k1_schnorrsig_verify(ctx, bundle.aggregate_sig.data(), msg32, 32, &agg_pk);
    secp256k1_context_destroy(ctx);
    return verifies;
}

bool BlockHasTx(const CBlock& block, const uint256& txid)
{
    for (const auto& tx : block.vtx) {
        if (tx->GetHash() == txid) {
            return true;
        }
    }
    return false;
}

struct MinerDDValidationSetup : public TestChain100Setup {
    size_t m_coinbase_spend_index{0};

    MinerDDValidationSetup()
    {
        MockOracleManager::GetInstance().Reset();
        MockOracleManager::GetInstance().SetEnabled(false);
        OracleBundleManager::GetInstance().Clear();
        OracleBundleManager::GetInstance().SetEnabled(true);
        EnsureDigiDollarActive();
    }

    ~MinerDDValidationSetup()
    {
        MockOracleManager::GetInstance().Reset();
        OracleBundleManager::GetInstance().Clear();
        OracleBundleManager::GetInstance().SetEnabled(true);
    }

    void EnsureDigiDollarActive()
    {
        for (int i = 0; i < 2500; ++i) {
            const bool active = WITH_LOCK(cs_main, return DigiDollar::IsDigiDollarEnabled(m_node.chainman->ActiveChain().Tip(), *m_node.chainman));
            const int next_height = NextBlockHeight();
            const bool musig2_ready = Params().GetConsensus().IsMuSig2OracleActive(next_height);
            if (active && musig2_ready) {
                return;
            }
            mineBlocks(1);
        }
        BOOST_FAIL("DigiDollar/MuSig2 activation did not activate in time");
    }

    int NextBlockHeight() const
    {
        return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height() + 1);
    }

    void InstallMuSig2OraclePrice(CAmount price_micro_usd, int block_height)
    {
        OracleBundleManager& manager = OracleBundleManager::GetInstance();
        manager.SetEnabled(true);

        COracleBundle bundle(GetCurrentEpoch(block_height));
        bundle.version = 3;
        bundle.median_price_micro_usd = static_cast<uint64_t>(price_micro_usd);
        bundle.timestamp = GetTime();

        std::vector<uint8_t> oracle_ids;
        for (int id = 0; id < Params().GetConsensus().nOracleConsensusRequired; ++id) {
            oracle_ids.push_back(static_cast<uint8_t>(id));
        }

        BOOST_REQUIRE(SignRegtestV03Bundle(bundle, oracle_ids));
        std::string error;
        BOOST_REQUIRE_MESSAGE(
            OracleBundleManager::ValidateMuSig2Bundle(bundle, block_height, Params().GetConsensus(), error),
            error);
        BOOST_REQUIRE(manager.UpdateBundle(bundle));
    }

    COutPoint ConfirmOpTrueFunding(CAmount output_value)
    {
        BOOST_REQUIRE(m_coinbase_spend_index < m_coinbase_txns.size());
        const int input_height = static_cast<int>(m_coinbase_spend_index + 1);
        CMutableTransaction funding = CreateValidMempoolTransaction(
            m_coinbase_txns[m_coinbase_spend_index], 0, input_height, coinbaseKey,
            CScript() << OP_TRUE, output_value, /*submit=*/false);
        ++m_coinbase_spend_index;

        const CPubKey coinbase_pubkey = coinbaseKey.GetPubKey();
        const CScript coinbase_script = CScript()
                                        << std::vector<unsigned char>(coinbase_pubkey.begin(), coinbase_pubkey.end())
                                        << OP_CHECKSIG;
        CBlock block = CreateAndProcessBlock({funding}, coinbase_script);
        BOOST_REQUIRE_GE(block.vtx.size(), 2U);
        return COutPoint(block.vtx[1]->GetHash(), 0);
    }

    CAmount RequiredCollateralAt(CAmount dd_amount, int lock_days, int next_height, CAmount oracle_price_micro_usd) const
    {
        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
        DigiDollar::ValidationContext ctx(next_height, oracle_price_micro_usd, 300, Params());
        return DigiDollar::CalculateRequiredCollateral(dd_amount, lock_blocks, ctx);
    }

    CTransactionRef BuildDDMint(const COutPoint& prevout, CAmount input_value, CAmount collateral_value,
                                CAmount dd_amount, int next_height, CAmount fee, int lock_days = 30)
    {
        BOOST_REQUIRE(input_value >= collateral_value + fee);

        CKey owner_key;
        owner_key.MakeNewKey(true);
        XOnlyPubKey owner_xonly(owner_key.GetPubKey());

        const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(lock_days);
        const int64_t lock_height = next_height + lock_blocks;

        DigiDollar::MintParams params;
        params.ddAmount = dd_amount;
        params.lockHeight = lock_height;
        params.ownerKey = owner_xonly;
        params.internalKey = DigiDollar::GetCollateralNUMSKey();
        params.oracleKeys = DigiDollar::GetOracleKeys(15);

        CMutableTransaction mint;
        mint.SetDigiDollarType(DD_TX_MINT);
        mint.vin.emplace_back(prevout);
        mint.vout.emplace_back(collateral_value, DigiDollar::CreateCollateralP2TR(params));
        mint.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(owner_xonly, dd_amount));

        CScript op_return = CScript() << OP_RETURN
                                      << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(1)
                                      << CScriptNum(dd_amount)
                                      << CScriptNum(lock_height)
                                      << CScriptNum(lock_days == 30 ? 1 : 0)
                                      << std::vector<unsigned char>(owner_xonly.begin(), owner_xonly.end());
        mint.vout.emplace_back(0, op_return);
        return MakeTransactionRef(mint);
    }

    CTransactionRef BuildOpTrueSpend(const COutPoint& prevout, CAmount input_value, CAmount fee)
    {
        BOOST_REQUIRE(input_value > fee);
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout.emplace_back(input_value - fee, CScript() << OP_TRUE);
        return MakeTransactionRef(tx);
    }

    CTransactionRef BuildOpTrueSpendWithDataCarrier(const COutPoint& prevout, CAmount input_value, CAmount fee,
                                                    const std::vector<unsigned char>& data)
    {
        BOOST_REQUIRE(input_value > fee);
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout.emplace_back(input_value - fee, CScript() << OP_TRUE);
        tx.vout.emplace_back(0, CScript() << OP_RETURN << data);
        return MakeTransactionRef(tx);
    }

    CTransactionRef BuildDDTransfer(const COutPoint& prevout, const XOnlyPubKey& recipient, CAmount dd_amount)
    {
        CMutableTransaction tx;
        tx.SetDigiDollarType(DD_TX_TRANSFER);
        tx.vin.emplace_back(prevout);
        tx.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(recipient, dd_amount));

        CScript op_return = CScript() << OP_RETURN
                                      << std::vector<unsigned char>{'D', 'D'}
                                      << CScriptNum(2)
                                      << CScriptNum(dd_amount);
        tx.vout.emplace_back(0, op_return);
        return MakeTransactionRef(tx);
    }

    bool ValidateMintAtPrice(const CTransaction& tx, int next_height, CAmount oracle_price_micro_usd, std::string* reject_reason = nullptr) const
    {
        DigiDollar::ValidationContext ctx(next_height, oracle_price_micro_usd, 300, Params());
        TxValidationState state;
        const bool valid = DigiDollar::ValidateDigiDollarTransaction(tx, ctx, state);
        if (reject_reason) {
            *reject_reason = state.GetRejectReason();
        }
        return valid;
    }

    void AddToMempool(const CTransactionRef& tx, CAmount fee)
    {
        LOCK2(cs_main, m_node.mempool->cs);
        TestMemPoolEntryHelper entry;
        m_node.mempool->addUnchecked(entry.Fee(fee).FromTx(tx));
    }

    std::unique_ptr<CBlockTemplate> BuildTemplate(const BlockAssembler::Options& options)
    {
        return BlockAssembler(m_node.chainman->ActiveChainstate(), m_node.mempool.get(), options)
            .CreateNewBlock(CScript() << OP_TRUE, ALGO_SHA256D);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(miner_dd_validation_tests)

BOOST_FIXTURE_TEST_CASE(block_includes_non_dd_op_return_dd_payload_without_oracle, MinerDDValidationSetup)
{
    constexpr CAmount kInput = 2 * COIN;
    constexpr CAmount kFee = 1000;

    const COutPoint funding = ConfirmOpTrueFunding(kInput);
    const CTransactionRef tx = BuildOpTrueSpendWithDataCarrier(
        funding,
        kInput,
        kFee,
        std::vector<unsigned char>{'D', 'D'});

    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*tx));
    OracleBundleManager::GetInstance().Clear();

    AddToMempool(tx, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_REQUIRE_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK_MESSAGE(BlockHasTx(block_template->block, tx->GetHash()),
        "ordinary DGB transaction with OP_RETURN \"DD\" payload must not require a DigiDollar oracle bundle");
}

BOOST_FIXTURE_TEST_CASE(block_skips_stale_dd_mint, MinerDDValidationSetup)
{
    constexpr CAmount kHighPrice = 50000; // $0.05
    constexpr CAmount kLowPrice = 45000;  // $0.045
    constexpr CAmount kDDAmount = 10000;  // $100.00
    constexpr CAmount kFee = 1000;

    const CAmount required_high = RequiredCollateralAt(kDDAmount, /*lock_days=*/30, NextBlockHeight(), kHighPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required_high + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef stale_mint = BuildDDMint(funding, required_high + kFee, required_high, kDDAmount, next_height, kFee);

    std::string reject_low;
    BOOST_REQUIRE(ValidateMintAtPrice(*stale_mint, next_height, kHighPrice));
    BOOST_REQUIRE(!ValidateMintAtPrice(*stale_mint, next_height, kLowPrice, &reject_low));
    BOOST_CHECK_EQUAL(reject_low, "insufficient-collateral");

    InstallMuSig2OraclePrice(kLowPrice, next_height);
    AddToMempool(stale_mint, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, stale_mint->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(block_succeeds_without_dd_after_failure, MinerDDValidationSetup)
{
    constexpr CAmount kHighPrice = 50000;
    constexpr CAmount kLowPrice = 45000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;
    constexpr CAmount kStdInput = 2 * COIN;

    const CAmount required_high = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kHighPrice);
    const COutPoint dd_funding = ConfirmOpTrueFunding(required_high + kFee);
    const COutPoint std_funding = ConfirmOpTrueFunding(kStdInput);

    const int next_height = NextBlockHeight();
    const CTransactionRef dd_mint = BuildDDMint(dd_funding, required_high + kFee, required_high, kDDAmount, next_height, kFee);
    const CTransactionRef std_tx = BuildOpTrueSpend(std_funding, kStdInput, kFee);

    AddToMempool(dd_mint, kFee);
    AddToMempool(std_tx, kFee);
    InstallMuSig2OraclePrice(kLowPrice, next_height);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_REQUIRE_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, std_tx->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, dd_mint->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(mint_includes_safety_margin, RegTestingSetup)
{
    constexpr CAmount kOraclePrice = 50000; // $0.05
    constexpr CAmount kDDAmount = 10000;    // $100
    constexpr int kLockDays = 30;

    DigiDollar::MintTxBuilder builder(Params(), /*height=*/1000, kOraclePrice);
    const CAmount with_margin = builder.CalculateRequiredCollateral(kDDAmount, kLockDays);

    const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(kLockDays);
    const auto& dd_params = Params().GetDigiDollarParams();
    const int base_ratio = DigiDollar::GetCollateralRatioForLockTime(lock_blocks, dd_params);

    util::int128_t numerator = static_cast<util::int128_t>(kDDAmount) *
                         static_cast<util::int128_t>(COIN) *
                         static_cast<util::int128_t>(base_ratio) * 100;
    const CAmount base_required = static_cast<CAmount>(numerator / static_cast<util::int128_t>(kOraclePrice));
    const CAmount expected_with_margin = static_cast<CAmount>((static_cast<util::int128_t>(base_required) * 101) / 100);

    BOOST_CHECK_EQUAL(with_margin, expected_with_margin);
    BOOST_CHECK(with_margin > base_required);
}

BOOST_FIXTURE_TEST_CASE(addPackageTxs_skips_invalid_dd, MinerDDValidationSetup)
{
    constexpr CAmount kPrice = 45000;   // $0.045
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    BOOST_REQUIRE(required > COIN);
    const CAmount insufficient_collateral = required - COIN;

    const COutPoint funding = ConfirmOpTrueFunding(insufficient_collateral + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef invalid_mint = BuildDDMint(funding, insufficient_collateral + kFee, insufficient_collateral, kDDAmount, next_height, kFee);
    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(invalid_mint, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, invalid_mint->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(test_block_validity_uses_musig2_price, MinerDDValidationSetup)
{
    constexpr CAmount kHighPrice = 50000;
    constexpr CAmount kLowPrice = 45000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required_high = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kHighPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required_high + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef borderline_mint = BuildDDMint(funding, required_high + kFee, required_high, kDDAmount, next_height, kFee);

    std::string reject_low;
    BOOST_REQUIRE(ValidateMintAtPrice(*borderline_mint, next_height, kHighPrice));
    BOOST_REQUIRE(!ValidateMintAtPrice(*borderline_mint, next_height, kLowPrice, &reject_low));
    BOOST_CHECK_EQUAL(reject_low, "insufficient-collateral");

    AddToMempool(borderline_mint, kFee);
    InstallMuSig2OraclePrice(kLowPrice, next_height);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_REQUIRE_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(!BlockHasTx(block_template->block, borderline_mint->GetHash()));
    BOOST_CHECK_EQUAL(block_template->block.vtx.size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(block_skips_unconfirmed_dd_transfer_chains, MinerDDValidationSetup)
{
    // DD transfers are confirmed-only (RC32): transfers spending unconfirmed
    // mempool DD outputs cannot resolve their input DD amounts and are
    // rejected with dd-input-amounts-unknown. The mint has no DD inputs so
    // it still qualifies. Only the mint should land in the block.
    //
    // Regression: addPackageTxs must erase mapModifiedTx entries whose trigger
    // tx fails ValidateDDForBlockInclusion. After the mint is selected,
    // UpdatePackagesForAdded enqueues its unconfirmed DD descendants into
    // mapModifiedTx. Each descendant fails DD validation. Without the erase,
    // the same mapModifiedTx entry is reselected forever and CreateNewBlock
    // never returns (previously observed writing ~250GB of debug.log before
    // the disk filled).
    constexpr CAmount kPrice = 50000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef mint = BuildDDMint(funding, required + kFee, required, kDDAmount, next_height, kFee);

    std::string reject_reason;
    BOOST_REQUIRE(ValidateMintAtPrice(*mint, next_height, kPrice, &reject_reason));

    CKey transfer1_key;
    transfer1_key.MakeNewKey(true);
    const XOnlyPubKey transfer1_xonly(transfer1_key.GetPubKey());

    CKey transfer2_key;
    transfer2_key.MakeNewKey(true);
    const XOnlyPubKey transfer2_xonly(transfer2_key.GetPubKey());

    const COutPoint mint_dd_out(mint->GetHash(), 1);
    const CTransactionRef transfer1 = BuildDDTransfer(mint_dd_out, transfer1_xonly, kDDAmount);
    const COutPoint transfer1_dd_out(transfer1->GetHash(), 0);
    const CTransactionRef transfer2 = BuildDDTransfer(transfer1_dd_out, transfer2_xonly, kDDAmount);

    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(mint, kFee);
    AddToMempool(transfer1, kFee);
    AddToMempool(transfer2, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, mint->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, transfer1->GetHash()));
    BOOST_CHECK(!BlockHasTx(block_template->block, transfer2->GetHash()));
}

BOOST_FIXTURE_TEST_CASE(block_skips_non_dd_child_spending_unconfirmed_mint_collateral, MinerDDValidationSetup)
{
    constexpr CAmount kPrice = 50000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef mint = BuildDDMint(funding, required + kFee, required, kDDAmount, next_height, kFee);

    CMutableTransaction child;
    child.nVersion = 2;
    child.vin.emplace_back(COutPoint(mint->GetHash(), 0));
    child.vout.emplace_back(required - kFee, CScript() << OP_TRUE);
    const CTransactionRef non_dd_child = MakeTransactionRef(child);

    BOOST_REQUIRE(ValidateMintAtPrice(*mint, next_height, kPrice));
    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*non_dd_child));

    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(mint, kFee);
    AddToMempool(non_dd_child, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = false;

    auto block_template = BuildTemplate(options);
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, mint->GetHash()));
    BOOST_CHECK_MESSAGE(!BlockHasTx(block_template->block, non_dd_child->GetHash()),
        "Miner must not include a non-DD child that spends an unconfirmed "
        "DigiDollar mint collateral output without the required redemption burn.");
}

BOOST_FIXTURE_TEST_CASE(block_skips_descendant_of_skipped_non_dd_collateral_child, MinerDDValidationSetup)
{
    constexpr CAmount kPrice = 50000;
    constexpr CAmount kDDAmount = 10000;
    constexpr CAmount kFee = 1000;

    const CAmount required = RequiredCollateralAt(kDDAmount, 30, NextBlockHeight(), kPrice);
    const COutPoint funding = ConfirmOpTrueFunding(required + kFee);
    const int next_height = NextBlockHeight();
    const CTransactionRef mint = BuildDDMint(funding, required + kFee, required, kDDAmount, next_height, kFee);

    CMutableTransaction child;
    child.nVersion = 2;
    child.vin.emplace_back(COutPoint(mint->GetHash(), 0));
    child.vout.emplace_back(required - kFee, CScript() << OP_TRUE);
    const CTransactionRef non_dd_child = MakeTransactionRef(child);

    const CTransactionRef grandchild = BuildOpTrueSpend(
        COutPoint(non_dd_child->GetHash(), 0),
        required - kFee,
        kFee);

    BOOST_REQUIRE(ValidateMintAtPrice(*mint, next_height, kPrice));
    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*non_dd_child));
    BOOST_REQUIRE(!DigiDollar::HasDigiDollarMarker(*grandchild));

    InstallMuSig2OraclePrice(kPrice, next_height);
    AddToMempool(mint, kFee);
    AddToMempool(non_dd_child, kFee);
    AddToMempool(grandchild, kFee);

    BlockAssembler::Options options;
    options.blockMinFeeRate = CFeeRate(0);
    options.test_block_validity = true;

    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_CHECK_NO_THROW(block_template = BuildTemplate(options));
    BOOST_REQUIRE(block_template);
    BOOST_CHECK(BlockHasTx(block_template->block, mint->GetHash()));
    BOOST_CHECK_MESSAGE(!BlockHasTx(block_template->block, non_dd_child->GetHash()),
        "Miner must skip the non-DD child that spends unconfirmed DigiDollar "
        "collateral without the required redemption burn.");
    BOOST_CHECK_MESSAGE(!BlockHasTx(block_template->block, grandchild->GetHash()),
        "Miner must also skip descendants of a skipped collateral-spend child; "
        "otherwise block template validity can fail with a missing in-block input.");
}

// Regression: the miner previously skipped AddOracleBundleToBlock() for blocks
// with no DigiDollar transactions. This created a bootstrapping deadlock:
//   - Minting requires an on-chain oracle price (OP_CHECKPRICE fails closed)
//   - Oracle bundles only appeared in DD-touching blocks
//   - No DD txs could form → no DD-touching blocks → no bundle → no price → loop
//
// Fix: call AddOracleBundleToBlock() for ALL DigiDollar-active blocks.
// When a completed MuSig2 session is available, the bundle stamps the price
// on-chain even in plain (non-DD) blocks, breaking the circular dependency.
BOOST_FIXTURE_TEST_CASE(oracle_bundle_stamped_in_non_dd_blocks, MinerDDValidationSetup)
{
    const int next_height = NextBlockHeight();
    InstallMuSig2OraclePrice(5000, next_height);

    // Mine a block with NO DigiDollar transactions in the mempool.
    BlockAssembler::Options opts;
    opts.test_block_validity = false;
    auto tmpl = BuildTemplate(opts);
    BOOST_REQUIRE(tmpl);
    const CTransaction& coinbase = *tmpl->block.vtx[0];

    bool found_oracle_output = false;
    for (const CTxOut& out : coinbase.vout) {
        const CScript& s = out.scriptPubKey;
        if (s.size() >= 2 && s[0] == OP_RETURN && s[1] == 0xbf) {
            found_oracle_output = true;
            break;
        }
    }

    BOOST_CHECK_MESSAGE(found_oracle_output,
        "Non-DD block must carry the oracle bundle when a completed MuSig2 "
        "session is available — required for price-cache bootstrapping");
}

BOOST_AUTO_TEST_SUITE_END()
