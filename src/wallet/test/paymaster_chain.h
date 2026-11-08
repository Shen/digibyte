// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_TEST_PAYMASTER_CHAIN_H
#define DIGIBYTE_WALLET_TEST_PAYMASTER_CHAIN_H

#include <coins.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <policy/feerate.h>
#include <primitives/block.h>
#include <univalue.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wallet {

/** Accounting fixtures may use synthetic wallet transactions while exercising
 * the real wallet construction, signing, persistence and submission sequence.
 * This adapter mocks only network acceptance; it does not establish production
 * mempool/consensus validity. All other chain operations retain their ordinary
 * implementation. The underlying chain must outlive the adapter and wallet. */
class PaymasterBroadcastChain final : public interfaces::Chain
{
public:
    explicit PaymasterBroadcastChain(interfaces::Chain& chain) : m_chain{chain} {}

    struct Submission {
        CTransactionRef tx;
        CAmount max_tx_fee;
        bool relay;
    };
    std::vector<Submission> submissions;
    bool broadcast_result{true};
    std::string rejection{"PAYMASTER_TEST_BROADCAST_REJECTED"};
    std::function<void(const Submission&)> on_broadcast;
    std::set<uint256> accepted_txids;

    bool broadcastTransaction(const CTransactionRef& tx, const CAmount& max_tx_fee,
                              bool relay, std::string& error) override
    {
        const Submission submission{tx, max_tx_fee, relay};
        submissions.push_back(submission);
        if (on_broadcast) on_broadcast(submission);
        if (!broadcast_result) {
            error = rejection;
            return false;
        }
        accepted_txids.insert(tx->GetHash());
        error.clear();
        return true;
    }

    bool isInMempool(const uint256& txid) override
    {
        return accepted_txids.count(txid) != 0 || m_chain.isInMempool(txid);
    }

    std::optional<int> getHeight() override { return m_chain.getHeight(); }
    uint256 getBlockHash(int height) override { return m_chain.getBlockHash(height); }
    bool haveBlockOnDisk(int height) override { return m_chain.haveBlockOnDisk(height); }
    CBlockLocator getTipLocator() override { return m_chain.getTipLocator(); }
    CBlockLocator getActiveChainLocator(const uint256& hash) override { return m_chain.getActiveChainLocator(hash); }
    std::optional<int> findLocatorFork(const CBlockLocator& locator) override { return m_chain.findLocatorFork(locator); }
    bool hasBlockFilterIndex(BlockFilterType type) override { return m_chain.hasBlockFilterIndex(type); }
    std::optional<bool> blockFilterMatchesAny(BlockFilterType type, const uint256& hash,
                                             const GCSFilter::ElementSet& elements) override
    {
        return m_chain.blockFilterMatchesAny(type, hash, elements);
    }
    bool findBlock(const uint256& hash, const interfaces::FoundBlock& block = {}) override
    {
        return m_chain.findBlock(hash, block);
    }
    bool findFirstBlockWithTimeAndHeight(int64_t time, int height, const interfaces::FoundBlock& block = {}) override
    {
        return m_chain.findFirstBlockWithTimeAndHeight(time, height, block);
    }
    bool findAncestorByHeight(const uint256& hash, int height, const interfaces::FoundBlock& ancestor = {}) override
    {
        return m_chain.findAncestorByHeight(hash, height, ancestor);
    }
    bool findAncestorByHash(const uint256& hash, const uint256& ancestor_hash,
                            const interfaces::FoundBlock& ancestor = {}) override
    {
        return m_chain.findAncestorByHash(hash, ancestor_hash, ancestor);
    }
    bool findCommonAncestor(const uint256& first, const uint256& second,
                            const interfaces::FoundBlock& ancestor = {},
                            const interfaces::FoundBlock& first_block = {},
                            const interfaces::FoundBlock& second_block = {}) override
    {
        return m_chain.findCommonAncestor(first, second, ancestor, first_block, second_block);
    }
    void findCoins(std::map<COutPoint, Coin>& coins) override { m_chain.findCoins(coins); }
    double guessVerificationProgress(const uint256& hash) override { return m_chain.guessVerificationProgress(hash); }
    bool hasBlocks(const uint256& hash, int min_height = 0, std::optional<int> max_height = {}) override
    {
        return m_chain.hasBlocks(hash, min_height, max_height);
    }
    RBFTransactionState isRBFOptIn(const CTransaction& tx) override { return m_chain.isRBFOptIn(tx); }
    bool isInStempool(const uint256& txid) override { return m_chain.isInStempool(txid); }
    bool hasDescendantsInMempool(const uint256& txid) override { return m_chain.hasDescendantsInMempool(txid); }
    void getTransactionAncestry(const uint256& txid, size_t& ancestors, size_t& descendants,
                                size_t* ancestor_size = nullptr, CAmount* ancestor_fees = nullptr) override
    {
        m_chain.getTransactionAncestry(txid, ancestors, descendants, ancestor_size, ancestor_fees);
    }
    std::map<COutPoint, CAmount> CalculateIndividualBumpFees(const std::vector<COutPoint>& outpoints,
                                                          const CFeeRate& rate) override
    {
        return m_chain.CalculateIndividualBumpFees(outpoints, rate);
    }
    std::optional<CAmount> CalculateCombinedBumpFee(const std::vector<COutPoint>& outpoints, const CFeeRate& rate) override
    {
        return m_chain.CalculateCombinedBumpFee(outpoints, rate);
    }
    void getPackageLimits(unsigned int& ancestors, unsigned int& descendants) override
    {
        m_chain.getPackageLimits(ancestors, descendants);
    }
    bool checkChainLimits(const CTransactionRef& tx) override { return m_chain.checkChainLimits(tx); }
    CFeeRate estimateSmartFee(int blocks, bool conservative, FeeCalculation* calculation = nullptr) override
    {
        return m_chain.estimateSmartFee(blocks, conservative, calculation);
    }
    unsigned int estimateMaxBlocks() override { return m_chain.estimateMaxBlocks(); }
    CFeeRate mempoolMinFee() override { return m_chain.mempoolMinFee(); }
    CFeeRate relayMinFee() override { return m_chain.relayMinFee(); }
    CFeeRate relayIncrementalFee() override { return m_chain.relayIncrementalFee(); }
    CFeeRate relayDustFee() override { return m_chain.relayDustFee(); }
    bool havePruned() override { return m_chain.havePruned(); }
    bool isReadyToBroadcast() override { return m_chain.isReadyToBroadcast(); }
    bool isInitialBlockDownload() override { return m_chain.isInitialBlockDownload(); }
    bool shutdownRequested() override { return m_chain.shutdownRequested(); }
    void initMessage(const std::string& message) override { m_chain.initMessage(message); }
    void initWarning(const bilingual_str& message) override { m_chain.initWarning(message); }
    void initError(const bilingual_str& message) override { m_chain.initError(message); }
    void showProgress(const std::string& title, int progress, bool resume_possible) override
    {
        m_chain.showProgress(title, progress, resume_possible);
    }
    std::unique_ptr<interfaces::Handler> handleNotifications(std::shared_ptr<Notifications> notifications) override
    {
        return m_chain.handleNotifications(std::move(notifications));
    }
    void waitForNotificationsIfTipChanged(const uint256& hash) override { m_chain.waitForNotificationsIfTipChanged(hash); }
    std::unique_ptr<interfaces::Handler> handleRpc(const CRPCCommand& command) override { return m_chain.handleRpc(command); }
    bool rpcEnableDeprecated(const std::string& method) override { return m_chain.rpcEnableDeprecated(method); }
    void rpcRunLater(const std::string& name, std::function<void()> function, int64_t seconds) override
    {
        m_chain.rpcRunLater(name, std::move(function), seconds);
    }
    int rpcSerializationFlags() override { return m_chain.rpcSerializationFlags(); }
    common::SettingsValue getSetting(const std::string& arg) override { return m_chain.getSetting(arg); }
    std::vector<common::SettingsValue> getSettingsList(const std::string& arg) override { return m_chain.getSettingsList(arg); }
    common::SettingsValue getRwSetting(const std::string& name) override { return m_chain.getRwSetting(name); }
    bool updateRwSetting(const std::string& name, const common::SettingsValue& value, bool write = true) override
    {
        return m_chain.updateRwSetting(name, value, write);
    }
    void requestMempoolTransactions(Notifications& notifications) override { m_chain.requestMempoolTransactions(notifications); }
    bool hasAssumedValidChain() override { return m_chain.hasAssumedValidChain(); }
    node::NodeContext* context() override { return m_chain.context(); }

private:
    interfaces::Chain& m_chain;
};

} // namespace wallet

#endif // DIGIBYTE_WALLET_TEST_PAYMASTER_CHAIN_H
