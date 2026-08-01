// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Wallet integration for provider policy, durable final transactions, pool
 * preparation, and restart recovery. Exact final transactions are reconciled
 * before rebroadcast so a crash cannot duplicate accounting or silently turn
 * an ambiguous commit into reusable liquidity.
 */

#include <wallet/paymasterprovider.h>

#include <chainparams.h>
#include <hash.h>
#include <index/txindex.h>
#include <key.h>
#include <node/context.h>
#include <node/transaction.h>
#include <paymaster/psbt.h>
#include <paymaster/validation.h>
#include <primitives/block.h>
#include <random.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <tinyformat.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <validation.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace wallet {
using namespace DigiDollar::Paymaster;
namespace {

bool Abort(WalletBatch& batch, std::string& error)
{
    batch.TxnAbort();
    error = "PAYMASTER_DATABASE_WRITE";
    return false;
}

uint256 GetOfferId(const ProviderIdentityRecord& identity,
                   const uint256& policy_hash,
                   FundingModel funding_model)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Offer v1");
    hasher << identity.provider_id << policy_hash << static_cast<uint8_t>(funding_model);
    return hasher.GetSHA256();
}

bool SignBip86ControlHash(CWallet& wallet,
                          const CScript& script,
                          const uint256& hash,
                          std::vector<unsigned char>& signature,
                          bool deterministic = false)
{
    CTxDestination destination;
    if (!ExtractDestination(script, destination)) return false;
    const auto* taproot = std::get_if<WitnessV1Taproot>(&destination);
    if (!taproot) return false;
    const XOnlyPubKey output_key{*taproot};
    for (ScriptPubKeyMan* script_manager : wallet.GetScriptPubKeyMans(script)) {
        auto* descriptor = dynamic_cast<DescriptorScriptPubKeyMan*>(script_manager);
        if (!descriptor) continue;
        auto provider = descriptor->GetSigningProviderWithKeys(script);
        TaprootSpendData spend_data;
        CKey internal_key;
        if (!provider || !provider->GetTaprootSpendData(output_key, spend_data) ||
            !spend_data.internal_key.IsFullyValid() || !spend_data.merkle_root.IsNull() ||
            !spend_data.scripts.empty() ||
            !provider->GetKeyByXOnly(spend_data.internal_key, internal_key) ||
            !internal_key.IsValid()) continue;
        const auto tweaked = spend_data.internal_key.CreateTapTweak(nullptr);
        if (!tweaked || tweaked->first != output_key) continue;
        signature.resize(64);
        const uint256 empty_merkle_root;
        const uint256 auxiliary_randomness{deterministic ? uint256{} : GetRandHash()};
        return internal_key.SignSchnorr(hash, signature, &empty_merkle_root,
                                        auxiliary_randomness);
    }
    return false;
}

uint256 GetCapacitySnapshotId(const PaymasterCapacityProof& proof)
{
    HashWriter hasher = TaggedHash(
        proof.version >= 4 ? "DigiByte Paymaster Capacity Snapshot v2" : "DigiByte Paymaster Capacity Snapshot v1");
    hasher << proof.version << proof.genesis_hash << proof.provider_id
           << proof.request_id << proof.session_id << proof.client_nonce;
    if (proof.version >= 4) {
        hasher << static_cast<uint8_t>(proof.funding_model)
               << static_cast<uint8_t>(proof.requires_carrier ? 1U : 0U);
    }
    hasher << proof.created_at << proof.expires_at
           << static_cast<uint64_t>(proof.liquidity_slots.size());
    for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
        hasher << static_cast<uint8_t>(slot.carrier.has_value() ? 1U : 0U);
        if (slot.carrier) {
            hasher << slot.carrier->carrier.outpoint
                   << CTransaction{slot.carrier->carrier.creating_tx}.GetHash()
                   << slot.carrier->carrier.value
                   << slot.carrier->control_proof.reference_block
                   << slot.carrier->control_proof.expires_at;
        }
        hasher << static_cast<uint64_t>(slot.dgb_inputs.size());
        for (const CapacityDGBInput& input : slot.dgb_inputs) {
            hasher << input.input.outpoint
                   << CTransaction{input.input.creating_tx}.GetHash()
                   << input.input.value << input.control_proof.reference_block
                   << input.control_proof.expires_at;
        }
    }
    return hasher.GetSHA256();
}

uint256 GetCapacitySessionKey(const PaymasterCapacityRequest& request)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Capacity Session v1");
    hasher << request.provider_id << request.request_id << request.session_id;
    return hasher.GetSHA256();
}

template <typename T>
std::vector<unsigned char> CanonicalBytes(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool DecodeStoredCapacityResponse(
    const std::vector<unsigned char>& response,
    const PaymasterCapacityRequest& request,
    const ProviderIdentityRecord& identity,
    const std::vector<ProviderPoolEntry>& pool_entries,
    const uint256& expected_genesis,
    int64_t now,
    PaymasterCapacityProof& proof,
    std::vector<ProviderPoolEntry>& selected_entries,
    std::string& error)
{
    error.clear();
    proof = {};
    selected_entries.clear();
    try {
        CDataStream stream{response, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> proof;
        if (!stream.empty()) throw std::ios_base::failure("trailing capacity response");
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_RESPONSE_ENCODING";
        return false;
    }
    if (CanonicalBytes(proof) != response ||
        !ValidateProviderIdentityRecord(identity) ||
        !ValidateCapacityProofEnvelope(proof, request, now, error) ||
        proof.genesis_hash != expected_genesis || proof.provider_id != identity.provider_id ||
        !identity.identity_key.VerifySchnorr(GetCapacityProofSignatureHash(proof),
                                             proof.identity_signature)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH";
        return false;
    }

    std::map<COutPoint, const ProviderPoolEntry*> by_outpoint;
    for (const ProviderPoolEntry& entry : pool_entries)
        by_outpoint.emplace(entry.outpoint, &entry);
    std::set<COutPoint> matched;
    const uint256 reference_block{
        proof.liquidity_slots.front().dgb_inputs.front().control_proof.reference_block};
    CapacityChainstateCallbacks callbacks;
    callbacks.validate_reference_block = [&](const uint256& block, std::string& callback_error) {
        if (block != reference_block) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_REFERENCE_MISMATCH";
            return false;
        }
        return true;
    };
    callbacks.validate_dgb_input = [&](const VerifiedDGBInput& input,
                                       XOnlyPubKey& output_key,
                                       std::string& callback_error) {
        const auto found = by_outpoint.find(input.outpoint);
        const CTransaction creating_tx{input.creating_tx};
        if (found == by_outpoint.end() || found->second->purpose != PoolPurpose::OPERATIONAL ||
            found->second->asset != PoolAsset::DGB ||
            found->second->state != PoolEntryState::RESERVED ||
            found->second->reservation_id != request.client_nonce ||
            !(found->second->dgb_value == input.value) ||
            creating_tx.GetHash() != input.outpoint.hash ||
            input.outpoint.n >= creating_tx.vout.size() ||
            creating_tx.vout[input.outpoint.n].scriptPubKey != found->second->script_pub_key ||
            creating_tx.vout[input.outpoint.n].nValue != input.value.value) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_DGB_MISMATCH";
            return false;
        }
        CTxDestination destination;
        if (!ExtractDestination(found->second->script_pub_key, destination)) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_DGB_SCRIPT";
            return false;
        }
        const auto* taproot = std::get_if<WitnessV1Taproot>(&destination);
        if (taproot == nullptr) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_DGB_SCRIPT";
            return false;
        }
        output_key = XOnlyPubKey{*taproot};
        matched.insert(input.outpoint);
        return true;
    };
    callbacks.validate_dd_carrier = [&](const VerifiedDDCarrier& carrier,
                                        XOnlyPubKey& output_key,
                                        std::string& callback_error) {
        const auto found = by_outpoint.find(carrier.outpoint);
        const CTransaction creating_tx{carrier.creating_tx};
        if (found == by_outpoint.end() || found->second->purpose != PoolPurpose::OPERATIONAL ||
            found->second->asset != PoolAsset::DD_CARRIER ||
            found->second->state != PoolEntryState::RESERVED ||
            found->second->reservation_id != request.client_nonce ||
            !(found->second->carrier_value == carrier.value) ||
            creating_tx.GetHash() != carrier.outpoint.hash ||
            carrier.outpoint.n >= creating_tx.vout.size() ||
            creating_tx.vout[carrier.outpoint.n].scriptPubKey != found->second->script_pub_key) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_CARRIER_MISMATCH";
            return false;
        }
        CTxDestination destination;
        if (!ExtractDestination(found->second->script_pub_key, destination)) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_CARRIER_SCRIPT";
            return false;
        }
        const auto* taproot = std::get_if<WitnessV1Taproot>(&destination);
        if (taproot == nullptr) {
            callback_error = "PAYMASTER_CAPACITY_RESPONSE_CARRIER_SCRIPT";
            return false;
        }
        output_key = XOnlyPubKey{*taproot};
        matched.insert(carrier.outpoint);
        return true;
    };
    if (!ValidateCapacityProof(proof, request, expected_genesis, identity.identity_key,
                               callbacks, now, error)) {
        return false;
    }
    const size_t resource_count{proof.liquidity_slots.front().dgb_inputs.size() +
                                (proof.liquidity_slots.front().carrier ? 1U : 0U)};
    if (matched.size() != resource_count) {
        error = "PAYMASTER_CAPACITY_RESPONSE_POOL_MISMATCH";
        return false;
    }
    for (const COutPoint& outpoint : matched)
        selected_entries.push_back(*by_outpoint.at(outpoint));
    std::sort(selected_entries.begin(), selected_entries.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.asset != rhs.asset) return lhs.asset == PoolAsset::DGB;
        return lhs.outpoint < rhs.outpoint;
    });
    error.clear();
    return true;
}

bool MatchDurableFinalTransaction(const CTransactionRef& known,
                                  const CTransactionRef& expected,
                                  std::string& error)
{
    if (!known || known->GetHash() != expected->GetHash()) {
        error = "PAYMASTER_FINAL_TRANSACTION_CONFLICT";
        return false;
    }
    if (known->GetWitnessHash() != expected->GetWitnessHash()) {
        error = "PAYMASTER_FINAL_WITNESS_CONFLICT";
        return false;
    }
    if (CanonicalBytes(CMutableTransaction{*known}) !=
        CanonicalBytes(CMutableTransaction{*expected})) {
        error = "PAYMASTER_FINAL_TRANSACTION_CONFLICT";
        return false;
    }
    return true;
}

bool PreflightExactPaymasterFinalTransactionImpl(
    CWallet& wallet,
    const CTransactionRef& transaction,
    ExactFinalTxIndexMode txindex_mode,
    ExactFinalTransactionPreflight& result,
    std::string& error)
{
    result = {};
    error.clear();
    if (!transaction) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISSING";
        return false;
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->chainman || !node->mempool) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }

    const CTransactionRef mempool_transaction =
        node->mempool->get(transaction->GetHash());
    if (mempool_transaction) {
        if (!MatchDurableFinalTransaction(mempool_transaction, transaction, error)) {
            return false;
        }
        result.presence = ExactFinalTransactionPresence::MEMPOOL;
        return true;
    }
    if (node->stempool) {
        const CTransactionRef stem_transaction =
            node->stempool->get(transaction->GetHash());
        if (stem_transaction) {
            if (!MatchDurableFinalTransaction(stem_transaction, transaction, error)) {
                return false;
            }
            result.presence = ExactFinalTransactionPresence::STEMPOOL;
            return true;
        }
    }

    // Wallet loading must never wait for the transaction index. The periodic
    // wallet task retries durable commits after the background index has
    // caught up.
    const bool txindex_ready =
        g_txindex &&
        (txindex_mode == ExactFinalTxIndexMode::WAIT_FOR_SYNC ? g_txindex->BlockUntilSyncedToCurrentChain() : g_txindex->GetSummary().synced);
    if (!txindex_ready) {
        error = "PAYMASTER_TXINDEX_NOT_READY";
        return false;
    }
    uint256 block_hash;
    const CTransactionRef known = node::GetTransaction(
        /*block_index=*/nullptr, /*mempool=*/nullptr, transaction->GetHash(),
        block_hash, node->chainman->m_blockman);
    if (known && !block_hash.IsNull()) {
        bool confirmed{false};
        int confirmed_height{-1};
        int confirmation_depth{0};
        const CBlockIndex* confirmed_index{nullptr};
        {
            LOCK(::cs_main);
            const CBlockIndex* block =
                node->chainman->m_blockman.LookupBlockIndex(block_hash);
            confirmed = block && node->chainman->ActiveChain().Contains(block);
            if (confirmed) {
                confirmed_index = block;
                confirmed_height = block->nHeight;
                confirmation_depth =
                    node->chainman->ActiveChain().Height() - block->nHeight + 1;
            }
        }
        if (confirmed) {
            if (!MatchDurableFinalTransaction(known, transaction, error)) {
                return false;
            }
            CBlock block;
            if (!node->chainman->m_blockman.ReadBlockFromDisk(
                    block, *confirmed_index)) {
                error = "PAYMASTER_FINAL_BLOCK_READ";
                return false;
            }
            const auto position = std::find_if(
                block.vtx.begin(), block.vtx.end(),
                [&](const CTransactionRef& candidate) {
                    return candidate->GetHash() == transaction->GetHash();
                });
            if (position == block.vtx.end() ||
                !MatchDurableFinalTransaction(*position, transaction, error)) {
                if (error.empty()) error = "PAYMASTER_FINAL_TRANSACTION_CONFLICT";
                return false;
            }
            result.presence = ExactFinalTransactionPresence::CONFIRMED;
            result.confirmed_block = block_hash;
            result.confirmed_height = confirmed_height;
            result.confirmed_position =
                static_cast<int>(std::distance(block.vtx.begin(), position));
            result.confirmation_depth = confirmation_depth;
            return true;
        }
    }

    const MempoolAcceptResult preflight = WITH_LOCK(
        ::cs_main,
        return node->chainman->ProcessTransaction(transaction, /*test_accept=*/true));
    if (preflight.m_result_type == MempoolAcceptResult::ResultType::VALID) {
        return true;
    }
    if (preflight.m_result_type == MempoolAcceptResult::ResultType::MEMPOOL_ENTRY) {
        const CTransactionRef accepted = node->mempool->get(transaction->GetHash());
        if (!MatchDurableFinalTransaction(accepted, transaction, error)) return false;
        result.presence = ExactFinalTransactionPresence::MEMPOOL;
        return true;
    }
    if (preflight.m_result_type ==
        MempoolAcceptResult::ResultType::DIFFERENT_WITNESS) {
        error = "PAYMASTER_FINAL_WITNESS_CONFLICT";
        return false;
    }
    error = strprintf("PAYMASTER_FINAL_MEMPOOL_REJECTED: %s",
                      preflight.m_state.ToString());
    return false;
}

bool AddExactDurableTransactionToWallet(CWallet& wallet,
                                        const CTransactionRef& transaction,
                                        const TxState& state,
                                        std::string& error)
{
    // cs_wallet is recursive. Keep it across the exact-byte check and both
    // AddToWallet calls so another wallet notification cannot insert a
    // same-txid/different-witness entry between validation and persistence.
    LOCK(wallet.cs_wallet);
    bool require_intermediate_state{false};
    TxState intermediate_state{TxStateInactive{}};
    const CWalletTx* existing = wallet.GetWalletTx(transaction->GetHash());
    if (existing) {
        if (!MatchDurableFinalTransaction(existing->tx, transaction, error)) {
            return false;
        }
        if (existing->m_state.index() == state.index() &&
            (TxStateSerializedBlockHash(existing->m_state) !=
                 TxStateSerializedBlockHash(state) ||
             TxStateSerializedIndex(existing->m_state) !=
                 TxStateSerializedIndex(state))) {
            require_intermediate_state = true;
            if (std::holds_alternative<TxStateInactive>(existing->m_state)) {
                intermediate_state = TxStateInMempool{};
            }
        }
    }
    if (require_intermediate_state &&
        !wallet.AddToWallet(transaction, intermediate_state)) {
        error = "PAYMASTER_WALLET_TRANSACTION_DATABASE_WRITE";
        return false;
    }
    if (!wallet.AddToWallet(transaction, state, [](CWalletTx& wtx, bool) {
            bool changed{false};
            if (!wtx.fTimeReceivedIsTxTime) {
                wtx.fTimeReceivedIsTxTime = true;
                changed = true;
            }
            if (!wtx.fFromMe) {
                wtx.fFromMe = true;
                changed = true;
            }
            if (wtx.mapValue["paymaster_durable_commit"] != "1") {
                wtx.mapValue["paymaster_durable_commit"] = "1";
                changed = true;
            }
            return changed;
        })) {
        error = "PAYMASTER_WALLET_TRANSACTION_DATABASE_WRITE";
        return false;
    }
    return true;
}

TxState DurableWalletState(const ExactFinalTransactionPreflight& preflight)
{
    if (preflight.presence == ExactFinalTransactionPresence::MEMPOOL) {
        return TxStateInMempool{};
    }
    if (preflight.presence == ExactFinalTransactionPresence::CONFIRMED) {
        return TxStateConfirmed{
            preflight.confirmed_block, preflight.confirmed_height,
            preflight.confirmed_position};
    }
    return TxStateInactive{};
}

bool ReconcileDurableFinalObservation(
    PaymasterStore& store,
    const CTransactionRef& transaction,
    const ExactFinalTransactionPreflight& observation,
    int64_t now,
    std::string& error)
{
    // Startup/periodic recovery may race a reorg after its chain snapshot.
    // It may advance to CONFIRMED, but never perform safety-depth pruning.
    // Tip reconciliation performs the authoritative depth-based prune.
    const int observed_depth =
        observation.presence == ExactFinalTransactionPresence::CONFIRMED ? 1 : 0;
    return store.ReconcileFinalTransaction(
        *transaction, observed_depth,
        observation.presence == ExactFinalTransactionPresence::MEMPOOL,
        now, error);
}

TransactionError BroadcastExactDurableTransaction(
    node::NodeContext& node,
    const CTransactionRef& transaction,
    std::string& error)
{
    if (!node.chainman || !node.mempool || !node.stempool || !node.peerman ||
        !node.connman) {
        error = "PAYMASTER_NODE_RUNTIME_UNAVAILABLE";
        return TransactionError::P2P_DISABLED;
    }

    // BroadcastTransaction reannounces a pre-existing same-txid mempool
    // entry without comparing its witness. Hold cs_main from this exact check
    // through broadcast so an adversarial witness cannot win that race.
    LOCK(::cs_main);
    if (const CTransactionRef known =
            node.mempool->get(transaction->GetHash())) {
        if (!MatchDurableFinalTransaction(known, transaction, error)) {
            return TransactionError::MEMPOOL_REJECTED;
        }
    }
    if (const CTransactionRef known =
            node.stempool->get(transaction->GetHash())) {
        if (!MatchDurableFinalTransaction(known, transaction, error)) {
            return TransactionError::MEMPOOL_REJECTED;
        }
    }
    return node::BroadcastTransaction(
        node, transaction, error, /*max_tx_fee=*/0, /*relay=*/true,
        /*wait_callback=*/false);
}

} // namespace

bool PreflightExactPaymasterFinalTransaction(
    CWallet& wallet,
    const CTransactionRef& transaction,
    ExactFinalTxIndexMode txindex_mode,
    ExactFinalTransactionPreflight& result,
    std::string& error)
{
    return PreflightExactPaymasterFinalTransactionImpl(
        wallet, transaction, txindex_mode, result, error);
}

bool IsTransientPaymasterFinalizationError(std::string_view error)
{
    return error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
           error == "PAYMASTER_NODE_RUNTIME_UNAVAILABLE" ||
           error == "PAYMASTER_TXINDEX_NOT_READY" ||
           error == "PAYMASTER_FINAL_BLOCK_READ" ||
           error == "PAYMASTER_WALLET_TRANSACTION_DATABASE_WRITE" ||
           error == "PAYMASTER_RECOVERY_USER_PREVOUT_NOT_FOUND";
}

bool InsertAndBroadcastExactPaymasterTransaction(
    CWallet& wallet,
    const CTransactionRef& transaction,
    bool& already_confirmed,
    std::string& error)
{
    already_confirmed = false;
    error.clear();
    if (!transaction) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISSING";
        return false;
    }

    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
            preflight, error) ||
        !AddExactDurableTransactionToWallet(
            wallet, transaction, DurableWalletState(preflight), error)) {
        return false;
    }
    if (preflight.presence == ExactFinalTransactionPresence::CONFIRMED) {
        already_confirmed = true;
        return true;
    }
    if (preflight.presence == ExactFinalTransactionPresence::MEMPOOL ||
        preflight.presence == ExactFinalTransactionPresence::STEMPOOL) {
        return true;
    }

    node::NodeContext* node = wallet.chain().context();
    if (!node) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    const TransactionError broadcast = BroadcastExactDurableTransaction(
        *node, transaction, error);
    if (broadcast != TransactionError::OK &&
        broadcast != TransactionError::ALREADY_IN_CHAIN) {
        return false;
    }

    ExactFinalTransactionPreflight observed;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
            observed, error) ||
        !AddExactDurableTransactionToWallet(
            wallet, transaction, DurableWalletState(observed), error)) {
        return false;
    }
    already_confirmed =
        observed.presence == ExactFinalTransactionPresence::CONFIRMED;
    return observed.presence != ExactFinalTransactionPresence::NONE;
}

static bool PromoteProviderSignedAttempt(
    CWallet& wallet,
    const ProviderAttempt& attempt,
    int64_t now,
    ProviderCommitRecord& committed,
    std::string& error)
{
    error.clear();
    PaymasterStore store{wallet};
    if (now <= 0 || attempt.state != AttemptState::PROVIDER_SIGNED ||
        attempt.provider_signed_at <= 0 ||
        attempt.provider_signed_at > attempt.retry_until ||
        attempt.provider_signed_result.empty()) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        return false;
    }

    PaymentSession session;
    if (!store.GetSessionBySessionId(attempt.session_id, session) ||
        !session.provider_side ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt.attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_PROVIDER_SIGNED_SESSION_MISSING";
        return false;
    }

    PaymasterResult signed_result;
    try {
        SpanReader result_stream{::PROTOCOL_VERSION,
                                 attempt.provider_signed_result};
        result_stream >> signed_result;
        if (!result_stream.empty() ||
            CanonicalBytes(signed_result) !=
                attempt.provider_signed_result) {
            throw std::ios_base::failure(
                "non-canonical provider-signed result");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        return false;
    }
    if (!ValidatePaymasterResult(
            signed_result, Params().GenesisBlock().GetHash(),
            attempt.provider_id, attempt.commit_key,
            attempt.provider_identity_key, 1, error) ||
        signed_result.result_sequence != 1 ||
        signed_result.status != PaymasterResultStatus::FINAL_COMMITTED ||
        signed_result.updated_at != attempt.provider_signed_at) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
        }
        return false;
    }

    CMutableTransaction final_transaction;
    try {
        SpanReader transaction_stream{::PROTOCOL_VERSION,
                                      attempt.final_transaction};
        transaction_stream >> final_transaction;
        if (!transaction_stream.empty()) {
            throw std::ios_base::failure(
                "trailing provider-signed transaction data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    if (attempt.input_roles.size() != final_transaction.vin.size()) {
        error = "PAYMASTER_PROVIDER_COMMIT_INPUT_MISMATCH";
        return false;
    }

    ProviderCommitRecord candidate;
    candidate.commit_key = attempt.commit_key;
    candidate.provider_id = attempt.provider_id;
    candidate.quote_id = attempt.quote_id;
    candidate.template_commitment = attempt.template_commitment;
    candidate.final_txid = attempt.final_txid;
    candidate.raw_transaction_hash = Hash(attempt.final_transaction);
    candidate.final_transaction = attempt.final_transaction;
    for (size_t index = 0; index < attempt.input_roles.size(); ++index) {
        const ReservationRole role = attempt.input_roles[index];
        if (role == ReservationRole::PROVIDER_CARRIER ||
            role == ReservationRole::PROVIDER_DGB) {
            candidate.provider_inputs.push_back(
                final_transaction.vin[index].prevout);
        }
    }
    candidate.committed_at = std::max(now, attempt.provider_signed_at);
    candidate.retry_until = attempt.retry_until;

    const auto same_commit = [](const ProviderCommitRecord& lhs,
                                const ProviderCommitRecord& rhs) {
        return lhs.version == rhs.version &&
               lhs.commit_key == rhs.commit_key &&
               lhs.provider_id == rhs.provider_id &&
               lhs.quote_id == rhs.quote_id &&
               lhs.template_commitment == rhs.template_commitment &&
               lhs.final_txid == rhs.final_txid &&
               lhs.raw_transaction_hash == rhs.raw_transaction_hash &&
               lhs.final_transaction == rhs.final_transaction &&
               lhs.provider_inputs == rhs.provider_inputs &&
               lhs.committed_at == rhs.committed_at &&
               lhs.retry_until == rhs.retry_until;
    };
    const auto same_durable_winner = [&](const ProviderCommitRecord& winner) {
        if (winner.committed_at < attempt.provider_signed_at) return false;
        ProviderCommitRecord candidate_at_winner_time{candidate};
        candidate_at_winner_time.committed_at = winner.committed_at;
        return same_commit(winner, candidate_at_winner_time);
    };
    // The startup scan intentionally selected an orphan, but another recovery
    // pass may have committed it before this pass acquired the database lock.
    // The winner's database commit time is authoritative and may differ from
    // this pass's wall clock. Adopt only that timestamp for comparison; every
    // signed authority, transaction, input and retry field must still match
    // byte-for-byte.
    if (store.GetProviderCommit(attempt.commit_key, committed)) {
        if (!same_durable_winner(committed)) {
            error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
            return false;
        }
        return true;
    }

    CMutableTransaction validated;
    if (!ValidateProviderCommitForExecution(
            attempt, candidate, candidate.committed_at, validated, error) ||
        !store.ValidateProviderBudgetAuthorization(
            attempt, BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true,
            /*allow_legacy_durable_commit=*/false, error)) {
        return false;
    }
    if (attempt.provider_manifest.manifest_id.IsNull() ||
        !ValidateProviderAuthorizationOwnership(
            wallet, attempt.provider_manifest, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED";
        }
        return false;
    }
    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, MakeTransactionRef(validated),
            ExactFinalTxIndexMode::NONBLOCKING, preflight, error)) {
        return false;
    }
    const std::optional<uint256> sponsorship_hash =
        attempt.sponsorship_capability_hash.IsNull() ? std::nullopt : std::optional<uint256>{attempt.sponsorship_capability_hash};
    if (!store.CommitProviderFinalTransaction(
            session.request_id, attempt.attempt_id, candidate,
            sponsorship_hash, signed_result,
            Params().GenesisBlock().GetHash(), error)) {
        // Another recovery pass may have won the atomic commit race. Accept
        // only the exact durable winner; every conflicting artifact remains a
        // hard failure.
        ProviderCommitRecord raced;
        if (!store.GetProviderCommit(attempt.commit_key, raced)) return false;
        if (!same_durable_winner(raced)) {
            error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
            return false;
        }
        committed = std::move(raced);
        error.clear();
        return true;
    }
    if (!store.GetProviderCommit(attempt.commit_key, committed)) {
        error = "PAYMASTER_PROVIDER_COMMIT_DATABASE_READ";
        return false;
    }
    return true;
}

DurablePaymasterCommitRecovery RecoverDurablePaymasterCommit(
    CWallet& wallet,
    const ProviderCommitRecord& commit,
    int64_t now)
{
    DurablePaymasterCommitRecovery result;
    if (now <= 0) {
        result.error = "PAYMASTER_INVALID_RECOVERY_TIME";
        return result;
    }

    PaymasterStore store{wallet};
    ProviderAttempt attempt;
    // A wall-clock rollback cannot revoke an already durable signature.
    const int64_t recovery_now = std::max(now, commit.committed_at);
    CMutableTransaction decoded;
    if (store.GetAttemptByTemplateCommitment(commit.template_commitment,
                                             attempt)) {
        if (!ValidateProviderCommitForExecution(
                attempt, commit, recovery_now, decoded, result.error)) {
            return result;
        }
        if (!store.ValidateProviderBudgetAuthorization(
                attempt, BudgetReservationState::SPENT,
                /*allow_historical_policy=*/true,
                /*allow_legacy_durable_commit=*/true, result.error)) {
            return result;
        }
        // Current and V1 manifests retain enough wallet-local ownership
        // authority to re-run the spend firewall on every restart broadcast.
        // Only an exact manifest-less legacy commit, which can no longer create
        // a signature, uses the narrow durable-recovery exception.
        if (!attempt.provider_manifest.manifest_id.IsNull() &&
            !ValidateProviderAuthorizationOwnership(
                wallet, attempt.provider_manifest, result.error)) {
            return result;
        }
    } else {
        // Alternative-provider recoveries deliberately do not create an
        // ordinary ProviderAttempt. Their final record is nevertheless a
        // complete, immutable authority artifact and is indexed by the same
        // durable provider commit.
        AlternativeRecoveryRecord alternative;
        if (!store.GetProviderAlternativeRecoveryByCommit(
                commit, alternative, result.error)) {
            return result;
        }
        try {
            SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
            stream >> decoded;
            if (!stream.empty()) {
                throw std::ios_base::failure(
                    "trailing alternative recovery transaction data");
            }
        } catch (const std::ios_base::failure&) {
            result.error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_COMMIT";
            return result;
        }

        const CTransaction exact_final{decoded};
        const CTransactionRef exact_final_ref = MakeTransactionRef(decoded);
        ExactFinalTransactionPreflight authority_preflight;
        if (!PreflightExactPaymasterFinalTransaction(
                wallet, exact_final_ref, ExactFinalTxIndexMode::NONBLOCKING,
                authority_preflight, result.error)) {
            return result;
        }
        const bool exact_final_already_known =
            authority_preflight.presence !=
            ExactFinalTransactionPresence::NONE;
        node::NodeContext* node = wallet.chain().context();
        if (!node || !node->chainman ||
            alternative.recovery_response.created_at <= 0 ||
            alternative.recovery_response.expires_at <=
                alternative.recovery_response.created_at ||
            commit.committed_at < alternative.recovery_response.created_at ||
            commit.committed_at >= alternative.recovery_response.expires_at) {
            result.error = "PAYMASTER_INVALID_PROVIDER_RECOVERY_AUTHORITY_TIME";
            return result;
        }
        const int64_t authorization_time = commit.committed_at;

        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildAlternativeRecoveryParametersFromRecord(
                wallet, alternative, parameters, result.error) ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                alternative.recovery_response,
                alternative.recovery_request,
                alternative.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time, recovery_now,
                exact_final_already_known ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                trusted, unsigned_psbt, result.error,
                exact_final_already_known ? &exact_final : nullptr)) {
            return result;
        }

        AlternativeRecoverySubmit submit;
        submit.genesis_hash = parameters.genesis_hash;
        submit.request_id = alternative.request_id;
        submit.session_id = alternative.session_id;
        submit.recovery_id = alternative.recovery_id;
        submit.recovery_provider_id = alternative.recovery_provider_id;
        submit.recovery_request_hash = alternative.recovery_request_hash;
        submit.recovery_commit_key =
            alternative.recovery_response.recovery_commit_key;
        submit.template_commitment =
            alternative.recovery_response.manifest.template_commitment;
        submit.user_psbt = alternative.user_signed_psbt;
        PartiallySignedTransaction user_psbt;
        if (!ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
                submit, alternative.recovery_response, trusted, parameters,
                Params(), *node->chainman, authorization_time, recovery_now,
                exact_final_already_known ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                user_psbt, result.error,
                exact_final_already_known ? &exact_final : nullptr)) {
            return result;
        }

        AlternativeRecoveryResultMessage result_message;
        result_message.request_id = alternative.request_id;
        result_message.session_id = alternative.session_id;
        result_message.recovery_id = alternative.recovery_id;
        result_message.recovery_request_hash =
            alternative.recovery_request_hash;
        result_message.result = alternative.signed_result;
        if (!ValidateDurableAlternativeRecoveryResultAgainstChainstate(
                result_message, alternative.recovery_response,
                alternative.recovery_provider_identity_key, 1, trusted,
                parameters, Params(), *node->chainman, authorization_time,
                std::max(recovery_now,
                         alternative.signed_result.updated_at),
                exact_final_already_known, result.error)) {
            return result;
        }
    }

    const CTransactionRef transaction = MakeTransactionRef(decoded);
    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
            preflight, result.error)) {
        return result;
    }

    if (!AddExactDurableTransactionToWallet(
            wallet, transaction, DurableWalletState(preflight), result.error)) {
        return result;
    }

    // Close the preflight-to-wallet race before deciding whether a broadcast
    // is needed. A reorg can turn CONFIRMED into NONE, while a peer can move
    // NONE into an exact pool entry.
    ExactFinalTransactionPreflight observed;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
            observed, result.error) ||
        !AddExactDurableTransactionToWallet(
            wallet, transaction, DurableWalletState(observed), result.error)) {
        return result;
    }
    if (observed.presence == ExactFinalTransactionPresence::CONFIRMED) {
        if (!ReconcileDurableFinalObservation(
                store, transaction, observed, recovery_now, result.error)) {
            return result;
        }
        result.broadcast = true;
        result.already_confirmed = true;
        return result;
    }
    if (observed.presence == ExactFinalTransactionPresence::MEMPOOL ||
        observed.presence == ExactFinalTransactionPresence::STEMPOOL) {
        if (!ReconcileDurableFinalObservation(
                store, transaction, observed, recovery_now, result.error)) {
            return result;
        }
        result.broadcast = true;
        return result;
    }

    node::NodeContext* node = wallet.chain().context();
    if (!node) {
        result.error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return result;
    }
    const TransactionError broadcast = BroadcastExactDurableTransaction(
        *node, transaction, result.error);
    result.already_confirmed =
        broadcast == TransactionError::ALREADY_IN_CHAIN;
    result.broadcast = broadcast == TransactionError::OK ||
                       result.already_confirmed;
    if (result.broadcast) {
        observed = {};
        std::string observed_error;
        if (!PreflightExactPaymasterFinalTransaction(
                wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
                observed, observed_error)) {
            result.error = std::move(observed_error);
            result.broadcast = false;
            return result;
        }
        if (!AddExactDurableTransactionToWallet(
                wallet, transaction, DurableWalletState(observed),
                result.error) ||
            !ReconcileDurableFinalObservation(
                store, transaction, observed, recovery_now, result.error)) {
            result.broadcast = false;
            return result;
        }
        result.already_confirmed =
            observed.presence == ExactFinalTransactionPresence::CONFIRMED;
    }
    return result;
}

DurablePaymasterRecoveryReport RecoverDurablePaymasterCommits(
    CWallet& wallet,
    int64_t now)
{
    DurablePaymasterRecoveryReport report;
    PaymasterStore store{wallet};
    std::vector<ProviderCommitRecord> commits;
    if (!store.ListProviderCommits(commits)) {
        report.errors.emplace_back("PAYMASTER_COMMIT_DATABASE_READ");
        return report;
    }
    std::vector<ProviderAttempt> provider_signed_attempts;
    if (!store.ListProviderSignedAttemptsWithoutCommit(
            provider_signed_attempts)) {
        report.errors.emplace_back(
            "PAYMASTER_PROVIDER_SIGNED_ATTEMPT_DATABASE_READ");
        return report;
    }
    std::set<uint256> provider_txids;
    report.candidates = commits.size() + provider_signed_attempts.size();
    for (const ProviderCommitRecord& commit : commits) {
        provider_txids.insert(commit.final_txid);
        DurablePaymasterCommitRecovery recovered =
            RecoverDurablePaymasterCommit(wallet, commit, now);
        if (recovered.broadcast) ++report.recovered;
        if (recovered.already_confirmed) ++report.already_confirmed;
        if (!recovered.error.empty()) {
            report.errors.push_back(std::move(recovered.error));
        }
    }
    for (const ProviderAttempt& attempt : provider_signed_attempts) {
        ProviderCommitRecord promoted;
        std::string promotion_error;
        if (!PromoteProviderSignedAttempt(
                wallet, attempt, now, promoted, promotion_error)) {
            report.errors.push_back(
                promotion_error.empty() ? "PAYMASTER_PROVIDER_SIGNED_PROMOTION_FAILED" : std::move(promotion_error));
            continue;
        }
        provider_txids.insert(promoted.final_txid);
        DurablePaymasterCommitRecovery recovered =
            RecoverDurablePaymasterCommit(wallet, promoted, now);
        if (recovered.broadcast) ++report.recovered;
        if (recovered.already_confirmed) ++report.already_confirmed;
        if (!recovered.error.empty()) {
            report.errors.push_back(std::move(recovered.error));
        }
    }

    std::vector<CTransactionRef> client_finals;
    std::string list_error;
    if (!store.ListClientDurableFinalTransactions(client_finals,
                                                  list_error)) {
        report.errors.push_back(
            list_error.empty() ? "PAYMASTER_CLIENT_FINAL_DATABASE_READ" : std::move(list_error));
        return report;
    }
    for (const CTransactionRef& transaction : client_finals) {
        if (!transaction || provider_txids.count(transaction->GetHash()) != 0) {
            continue;
        }
        ++report.candidates;
        bool already_confirmed{false};
        std::string recovery_error;
        ExactFinalTransactionPreflight authority_preflight;
        if (!PreflightExactPaymasterFinalTransaction(
                wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
                authority_preflight, recovery_error) ||
            !store.ValidateClientDurableFinalForBroadcast(
                *transaction, now,
                authority_preflight.presence !=
                    ExactFinalTransactionPresence::NONE,
                recovery_error) ||
            !InsertAndBroadcastExactPaymasterTransaction(
                wallet, transaction, already_confirmed, recovery_error)) {
            report.errors.push_back(std::move(recovery_error));
            continue;
        }
        ExactFinalTransactionPreflight observation;
        if (!PreflightExactPaymasterFinalTransaction(
                wallet, transaction, ExactFinalTxIndexMode::NONBLOCKING,
                observation, recovery_error) ||
            !ReconcileDurableFinalObservation(
                store, transaction, observation, now, recovery_error)) {
            report.errors.push_back(std::move(recovery_error));
            continue;
        }
        ++report.recovered;
        if (already_confirmed) ++report.already_confirmed;
    }
    return report;
}

bool SetPaymasterProviderPolicy(CWallet& wallet,
                                const ProviderPolicy& policy,
                                int64_t now,
                                std::string& error)
{
    error.clear();
    LOCK(wallet.cs_wallet);
    if (!CheckPaymasterProviderWallet(wallet, error)) return false;
    if (!ValidateProviderPolicy(policy, error)) return false;
    ProviderIdentityRecord identity;
    WalletBatch batch{wallet.GetDatabase()};
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_IDENTITY_NOT_FOUND";
        return false;
    }
    if (now <= 0) {
        error = "PAYMASTER_INVALID_TIME";
        return false;
    }
    if (batch.HasPaymasterProviderSafetyPolicy()) {
        ProviderSafetyPolicy safety_policy;
        // Keep the currently readable policy/safety pair readable: all
        // candidate-policy validation must finish before either DB write.
        if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
            error = "PAYMASTER_INVALID_SAFETY_POLICY";
            return false;
        }
        std::string safety_error;
        if (!ValidateProviderSafetyPolicy(safety_policy, policy,
                                          safety_error)) {
            error = "PAYMASTER_PROVIDER_SAFETY_POLICY_CONFLICT";
            return false;
        }
    }
    ProviderSettings settings;
    batch.ReadPaymasterSettings(settings);
    settings.policy_hash = GetProviderPolicyHash(policy);
    settings.updated_at = now;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterPolicy(policy) || !batch.WritePaymasterSettings(settings)) {
        return Abort(batch, error);
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool SetPaymasterProviderEnabled(CWallet& wallet, bool enabled, int64_t now, std::string& error)
{
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderSettings settings;
    batch.ReadPaymasterSettings(settings);
    if (enabled) {
        if (!CheckPaymasterProviderWallet(wallet, error)) return false;
        ProviderIdentityRecord identity;
        ProviderPolicy policy;
        if (!batch.ReadPaymasterIdentity(identity)) {
            error = "PAYMASTER_IDENTITY_NOT_FOUND";
            return false;
        }
        if (!batch.ReadPaymasterPolicy(policy)) {
            error = "PAYMASTER_POLICY_NOT_FOUND";
            return false;
        }
        ProviderSafetyPolicy safety;
        ProviderBudgetLedger ledger;
        if (!batch.ReadPaymasterProviderSafetyPolicy(safety)) {
            error = batch.HasPaymasterProviderSafetyPolicy() ? "PAYMASTER_INVALID_SAFETY_POLICY" : "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
            return false;
        }
        if (!batch.ReadPaymasterProviderBudgetLedger(ledger)) {
            error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
            return false;
        }
        const uint256 policy_hash = GetProviderPolicyHash(policy);
        if (settings.policy_hash != policy_hash) {
            error = "PAYMASTER_POLICY_BINDING_MISMATCH";
            return false;
        }
    }
    if (now <= 0) {
        error = "PAYMASTER_INVALID_TIME";
        return false;
    }
    settings.enabled = enabled;
    settings.updated_at = now;
    if (!batch.WritePaymasterSettings(settings)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool SetPaymasterProviderRuntimeSettings(CWallet& wallet,
                                         ProviderOperationMode operation_mode,
                                         bool autostart,
                                         int64_t now,
                                         std::string& error)
{
    error.clear();
    if (static_cast<uint8_t>(operation_mode) >
            static_cast<uint8_t>(ProviderOperationMode::MANUAL)) {
        error = "PAYMASTER_INVALID_OPERATION_MODE";
        return false;
    }
    if (now <= 0) {
        error = "PAYMASTER_INVALID_TIME";
        return false;
    }

    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderSettings settings;
    if (!batch.ReadPaymasterSettings(settings)) {
        error = "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND";
        return false;
    }
    settings.version = ProviderSettings::CURRENT_VERSION;
    settings.operation_mode = operation_mode;
    settings.autostart = autostart;
    settings.updated_at = now;
    if (!batch.WritePaymasterSettings(settings)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool GetPaymasterProviderPolicy(const CWallet& wallet, ProviderPolicy& policy)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterPolicy(policy);
}

bool GetPaymasterProviderSettings(const CWallet& wallet, ProviderSettings& settings)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterSettings(settings);
}

bool SetPaymasterProviderSafetyPolicy(CWallet& wallet,
                                      const ProviderSafetyPolicy& policy,
                                      int64_t now,
                                      std::string& error)
{
    error.clear();
    if (now <= 0) {
        error = "PAYMASTER_INVALID_TIME";
        return false;
    }
    LOCK(wallet.cs_wallet);
    if (!CheckPaymasterProviderWallet(wallet, error)) return false;

    WalletBatch batch{wallet.GetDatabase()};
    ProviderPolicy advertised;
    if (!batch.ReadPaymasterPolicy(advertised)) {
        error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }
    ProviderBudgetLedger ledger;
    if (!batch.ReadPaymasterProviderBudgetLedger(ledger)) {
        if (batch.HasPaymasterProviderBudgetLedger()) {
            error = "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER";
            return false;
        }
        ledger.recipient_bucket_secret = GetRandHash();
    }
    const int64_t effective_now = std::max(now, ledger.accounting_time_high_water);
    ledger.accounting_time_high_water = effective_now;

    ProviderSafetyPolicy persisted{policy};
    persisted.updated_at = effective_now;
    if (!ValidateProviderSafetyPolicy(persisted, advertised, error)) return false;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterProviderSafetyPolicy(persisted) ||
        !batch.WritePaymasterProviderBudgetLedger(ledger)) {
        return Abort(batch, error);
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool GetPaymasterProviderSafetyPolicy(const CWallet& wallet,
                                      ProviderSafetyPolicy& policy)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterProviderSafetyPolicy(policy);
}

bool GetPaymasterProviderBudgetLedger(const CWallet& wallet,
                                      ProviderBudgetLedger& ledger)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterProviderBudgetLedger(ledger);
}

bool SetPaymasterProviderLiquidityPolicy(
    CWallet& wallet,
    const ProviderLiquidityPolicy& policy,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (now <= 0 || policy.updated_at != now ||
        !ValidateProviderLiquidityPolicy(policy, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_LIQUIDITY_POLICY";
        return false;
    }
    LOCK(wallet.cs_wallet);
    if (!CheckPaymasterProviderWallet(wallet, error)) return false;
    WalletBatch batch{wallet.GetDatabase()};
    ProviderPolicy advertised;
    if (!batch.ReadPaymasterPolicy(advertised)) {
        error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }
    const bool user_paid = PolicyAllowsFundingModel(
        advertised, FundingModel::USER_PAID);
    const bool has_carrier_targets =
        policy.target_admission_carriers != 0 ||
        policy.target_operational_carriers != 0;
    if (user_paid != has_carrier_targets) {
        error = user_paid
            ? "PAYMASTER_USER_PAID_REQUIRES_CARRIER_TARGETS"
            : "PAYMASTER_SPONSORED_LIQUIDITY_HAS_CARRIER_TARGETS";
        return false;
    }

    ProviderMaintenanceLedger ledger;
    if (!batch.ReadPaymasterMaintenanceLedger(ledger)) {
        if (batch.HasPaymasterMaintenanceLedger()) {
            error = "PAYMASTER_INVALID_MAINTENANCE_LEDGER";
            return false;
        }
        ledger.accounting_time_high_water = now;
    }
    if (!ValidateProviderMaintenanceLedger(ledger, error)) return false;
    ledger.accounting_time_high_water = std::max(
        ledger.accounting_time_high_water, now);
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterLiquidityPolicy(policy) ||
        !batch.WritePaymasterMaintenanceLedger(ledger)) {
        batch.TxnAbort();
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool GetPaymasterProviderLiquidityPolicy(
    const CWallet& wallet,
    ProviderLiquidityPolicy& policy)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterLiquidityPolicy(policy);
}

bool GetPaymasterProviderMaintenanceLedger(
    const CWallet& wallet,
    ProviderMaintenanceLedger& ledger)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterMaintenanceLedger(ledger);
}

bool SetPaymasterClientSafetyPolicy(CWallet& wallet,
                                    const ClientSafetyPolicy& policy,
                                    int64_t now,
                                    std::string& error)
{
    error.clear();
    if (now <= 0) {
        error = "PAYMASTER_INVALID_TIME";
        return false;
    }
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ClientFeeLedger ledger;
    if (!batch.ReadPaymasterClientFeeLedger(ledger)) {
        if (batch.HasPaymasterClientFeeLedger()) {
            error = "PAYMASTER_INVALID_CLIENT_FEE_LEDGER";
            return false;
        }
        ledger.accounting_time_high_water = now;
    }
    const int64_t effective_now =
        std::max(now, ledger.accounting_time_high_water);
    ledger.accounting_time_high_water = effective_now;

    ClientSafetyPolicy persisted{policy};
    persisted.updated_at = effective_now;
    if (!ValidateClientSafetyPolicy(persisted, error)) return false;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterClientSafetyPolicy(persisted) ||
        !batch.WritePaymasterClientFeeLedger(ledger)) {
        return Abort(batch, error);
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool GetPaymasterClientSafetyPolicy(const CWallet& wallet,
                                    ClientSafetyPolicy& policy)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterClientSafetyPolicy(policy);
}

bool GetPaymasterClientFeeLedger(const CWallet& wallet,
                                 ClientFeeLedger& ledger)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterClientFeeLedger(ledger);
}

bool SetPaymasterProviderPoolEntries(CWallet& wallet,
                                     const std::vector<ProviderPoolEntry>& entries,
                                     std::string& error)
{
    error.clear();
    LOCK(wallet.cs_wallet);
    if (!CheckPaymasterProviderWallet(wallet, error)) return false;
    ProviderIdentityRecord identity;
    WalletBatch batch{wallet.GetDatabase()};
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_IDENTITY_NOT_FOUND";
        return false;
    }
    if (!ValidateProviderPoolEntries(entries, error)) return false;
    std::vector<ProviderPoolEntry> existing;
    if (batch.ReadPaymasterProviderPool(existing)) {
        std::map<COutPoint, const ProviderPoolEntry*> replacements;
        for (const ProviderPoolEntry& entry : entries)
            replacements.emplace(entry.outpoint, &entry);
        for (const ProviderPoolEntry& entry : existing) {
            if (entry.state != PoolEntryState::RESERVED &&
                entry.state != PoolEntryState::PENDING_SUCCESSOR &&
                entry.state != PoolEntryState::COMMITTED) continue;
            const auto replacement = replacements.find(entry.outpoint);
            if (replacement == replacements.end() || replacement->second->purpose != entry.purpose ||
                replacement->second->asset != entry.asset || replacement->second->state != entry.state ||
                replacement->second->script_pub_key != entry.script_pub_key ||
                !(replacement->second->dgb_value == entry.dgb_value) ||
                !(replacement->second->carrier_value == entry.carrier_value) ||
                replacement->second->reservation_id != entry.reservation_id) {
                error = "PAYMASTER_ACTIVE_POOL_ENTRY_CONFLICT";
                return false;
            }
        }
    }
    if (!batch.WritePaymasterProviderPool(entries)) {
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool GetPaymasterProviderPoolEntries(const CWallet& wallet,
                                     std::vector<ProviderPoolEntry>& entries)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterProviderPool(entries);
}

bool ReservePaymasterOperationalSlot(CWallet& wallet,
                                     const uint256& reservation_id,
                                     bool require_carrier,
                                     int32_t tip_height,
                                     int32_t minimum_confirmations,
                                     int64_t now,
                                     std::vector<ProviderPoolEntry>& reserved,
                                     std::string& error)
{
    error.clear();
    reserved.clear();
    if (reservation_id.IsNull() || tip_height < 0 || minimum_confirmations <= 0 || now <= 0) {
        error = "PAYMASTER_INVALID_POOL_RESERVATION";
        return false;
    }
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderPolicy policy;
    std::vector<ProviderPoolEntry> entries;
    if (!batch.ReadPaymasterPolicy(policy)) {
        error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderPool(entries)) {
        error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }

    for (const ProviderPoolEntry& entry : entries) {
        if (entry.reservation_id == reservation_id) reserved.push_back(entry);
    }
    if (!reserved.empty()) {
        const size_t expected_count = require_carrier ? 2 : 1;
        const bool exact = reserved.size() == expected_count &&
                           std::all_of(reserved.begin(), reserved.end(), [](const ProviderPoolEntry& entry) {
                               return entry.purpose == PoolPurpose::OPERATIONAL &&
                                      entry.state == PoolEntryState::RESERVED;
                           }) &&
                           std::count_if(reserved.begin(), reserved.end(), [](const ProviderPoolEntry& entry) {
                               return entry.asset == PoolAsset::DGB;
                           }) == 1 &&
                           std::count_if(reserved.begin(), reserved.end(), [](const ProviderPoolEntry& entry) {
                               return entry.asset == PoolAsset::DD_CARRIER;
                           }) == (require_carrier ? 1 : 0);
        if (exact) return true;
        reserved.clear();
        error = "PAYMASTER_POOL_RESERVATION_CONFLICT";
        return false;
    }

    const auto confirmed_available = [&](const ProviderPoolEntry& entry) {
        return entry.purpose == PoolPurpose::OPERATIONAL &&
               entry.state == PoolEntryState::AVAILABLE && entry.confirmation_height > 0 &&
               tip_height - entry.confirmation_height + 1 >= minimum_confirmations;
    };
    std::vector<size_t> candidates(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        candidates[i] = i;
    std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
        return entries[a].outpoint < entries[b].outpoint;
    });
    std::optional<size_t> dgb_index;
    std::optional<size_t> carrier_index;
    for (const size_t index : candidates) {
        const ProviderPoolEntry& entry = entries[index];
        if (!confirmed_available(entry)) continue;
        if (!dgb_index && entry.asset == PoolAsset::DGB &&
            entry.dgb_value.value >= policy.maximum_network_fee.value) {
            dgb_index = index;
        } else if (require_carrier && !carrier_index && entry.asset == PoolAsset::DD_CARRIER) {
            carrier_index = index;
        }
    }
    if (!dgb_index || (require_carrier && !carrier_index)) {
        error = "PAYMASTER_OPERATIONAL_SLOT_MISSING";
        return false;
    }
    entries[*dgb_index].state = PoolEntryState::RESERVED;
    entries[*dgb_index].reservation_id = reservation_id;
    entries[*dgb_index].updated_at = now;
    reserved.push_back(entries[*dgb_index]);
    if (carrier_index) {
        entries[*carrier_index].state = PoolEntryState::RESERVED;
        entries[*carrier_index].reservation_id = reservation_id;
        entries[*carrier_index].updated_at = now;
        reserved.push_back(entries[*carrier_index]);
    }
    if (!batch.WritePaymasterProviderPool(entries)) {
        reserved.clear();
        error = "PAYMASTER_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool BuildPaymasterCapacityProof(CWallet& wallet,
                                 const ProviderIdentityRecord& identity,
                                 const PaymasterCapacityRequest& request,
                                 const std::vector<ProviderPoolEntry>& selected_entries,
                                 const uint256& expected_genesis,
                                 const uint256& reference_block,
                                 int64_t now,
                                 PaymasterCapacityProof& proof,
                                 std::string& error)
{
    error.clear();
    proof = {};
    if (now <= 0 || expected_genesis.IsNull() || reference_block.IsNull() ||
        !ValidateProviderIdentityRecord(identity) ||
        identity.provider_id != request.provider_id) {
        error = "PAYMASTER_INVALID_CAPACITY_PARAMETERS";
        return false;
    }
    if (!ValidateCapacityRequestEnvelope(request, expected_genesis, now, error)) return false;
    if (selected_entries.empty() || selected_entries.size() > MAX_CAPACITY_DGB_INPUTS + 1 ||
        !ValidateProviderPoolEntries(selected_entries, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_SLOT";
        return false;
    }
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }

    std::vector<const ProviderPoolEntry*> dgb_entries;
    const ProviderPoolEntry* carrier_entry{nullptr};
    std::set<COutPoint> selected_outpoints;
    for (const ProviderPoolEntry& entry : selected_entries) {
        const bool selected_available{entry.state == PoolEntryState::AVAILABLE};
        const bool selected_reserved{entry.state == PoolEntryState::RESERVED &&
                                     entry.reservation_id == request.client_nonce};
        if (entry.purpose != PoolPurpose::OPERATIONAL ||
            (!selected_available && !selected_reserved) || entry.confirmation_height <= 0 ||
            !selected_outpoints.insert(entry.outpoint).second) {
            error = "PAYMASTER_CAPACITY_SLOT_NOT_AVAILABLE";
            return false;
        }
        if (entry.asset == PoolAsset::DGB) {
            dgb_entries.push_back(&entry);
        } else if (carrier_entry != nullptr) {
            error = "PAYMASTER_INVALID_CAPACITY_SLOT";
            return false;
        } else {
            carrier_entry = &entry;
        }
    }
    if (dgb_entries.empty() || dgb_entries.size() > MAX_CAPACITY_DGB_INPUTS ||
        (carrier_entry != nullptr) != request.requires_carrier) {
        error = "PAYMASTER_INVALID_CAPACITY_SLOT";
        return false;
    }
    std::sort(dgb_entries.begin(), dgb_entries.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->outpoint < rhs->outpoint;
    });

    CKey identity_key;
    ProviderIdentityRecord stored_identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, stored_identity, error) ||
        stored_identity.provider_id != identity.provider_id ||
        stored_identity.identity_key != identity.identity_key) {
        if (error.empty()) error = "PAYMASTER_IDENTITY_BINDING_MISMATCH";
        return false;
    }

    proof.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    proof.genesis_hash = expected_genesis;
    proof.provider_id = identity.provider_id;
    proof.request_id = request.request_id;
    proof.session_id = request.session_id;
    proof.client_nonce = request.client_nonce;
    proof.funding_model = request.funding_model;
    proof.requires_carrier = request.requires_carrier;
    // Request time is part of the deterministic snapshot. This makes an exact
    // retry after a crash produce the same proof from the durable reservation.
    proof.created_at = request.created_at;
    proof.expires_at = std::min(
        request.expires_at,
        SaturatingAddSeconds(proof.created_at, MAX_DIRECT_MESSAGE_TTL_SECONDS));

    PaymasterLiquiditySlot slot;
    slot.dgb_inputs.reserve(dgb_entries.size());
    {
        LOCK(wallet.cs_wallet);
        for (const ProviderPoolEntry* entry : dgb_entries) {
            const CWalletTx* creating_tx{wallet.GetWalletTx(entry->outpoint.hash)};
            if (!creating_tx || entry->outpoint.n >= creating_tx->tx->vout.size() ||
                creating_tx->tx->vout[entry->outpoint.n].scriptPubKey != entry->script_pub_key ||
                creating_tx->tx->vout[entry->outpoint.n].nValue != entry->dgb_value.value) {
                error = "PAYMASTER_CAPACITY_DGB_PREVOUT_MISMATCH";
                return false;
            }
            CapacityDGBInput input;
            input.input.outpoint = entry->outpoint;
            input.input.creating_tx = CMutableTransaction{*creating_tx->tx};
            input.input.value = entry->dgb_value;
            input.control_proof.reference_block = reference_block;
            input.control_proof.expires_at = proof.expires_at;
            slot.dgb_inputs.push_back(std::move(input));
        }
        if (carrier_entry != nullptr) {
            const CWalletTx* creating_tx{wallet.GetWalletTx(carrier_entry->outpoint.hash)};
            if (!creating_tx || carrier_entry->outpoint.n >= creating_tx->tx->vout.size() ||
                creating_tx->tx->vout[carrier_entry->outpoint.n].scriptPubKey !=
                    carrier_entry->script_pub_key) {
                error = "PAYMASTER_CAPACITY_CARRIER_PREVOUT_MISMATCH";
                return false;
            }
            CapacityDDCarrier carrier;
            carrier.carrier.outpoint = carrier_entry->outpoint;
            carrier.carrier.creating_tx = CMutableTransaction{*creating_tx->tx};
            carrier.carrier.value = carrier_entry->carrier_value;
            carrier.control_proof.reference_block = reference_block;
            carrier.control_proof.expires_at = proof.expires_at;
            slot.carrier = std::move(carrier);
        }
        proof.liquidity_slots.push_back(std::move(slot));
        proof.snapshot_id = GetCapacitySnapshotId(proof);
        if (proof.snapshot_id.IsNull()) {
            error = "PAYMASTER_INVALID_CAPACITY_SNAPSHOT";
            return false;
        }

        for (size_t index = 0; index < dgb_entries.size(); ++index) {
            CapacityDGBInput& input{proof.liquidity_slots.front().dgb_inputs[index]};
            if (!SignBip86ControlHash(
                    wallet, dgb_entries[index]->script_pub_key,
                    GetCapacityControlHash(proof, input.input.outpoint,
                                           input.control_proof.expires_at),
                    input.control_proof.signature, /*deterministic=*/true)) {
                error = "PAYMASTER_CAPACITY_DGB_SIGNING_FAILED";
                return false;
            }
        }
        if (carrier_entry != nullptr) {
            CapacityDDCarrier& carrier{*proof.liquidity_slots.front().carrier};
            if (!SignBip86ControlHash(
                    wallet, carrier_entry->script_pub_key,
                    GetCapacityControlHash(proof, carrier.carrier.outpoint,
                                           carrier.control_proof.expires_at),
                    carrier.control_proof.signature, /*deterministic=*/true)) {
                error = "PAYMASTER_CAPACITY_CARRIER_SIGNING_FAILED";
                return false;
            }
        }
    }

    proof.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetCapacityProofSignatureHash(proof),
                                  proof.identity_signature, nullptr, uint256{})) {
        error = "PAYMASTER_CAPACITY_IDENTITY_SIGNING_FAILED";
        return false;
    }
    if (!ValidateCapacityProofEnvelope(proof, request, now, error)) return false;
    error.clear();
    return true;
}

bool ReserveAndBuildPaymasterCapacityProof(
    CWallet& wallet,
    const ProviderIdentityRecord& identity,
    const PaymasterCapacityRequest& request,
    const uint256& expected_genesis,
    const uint256& reference_block,
    int32_t tip_height,
    int32_t minimum_confirmations,
    const std::vector<unsigned char>& canonical_netgroup,
    int64_t now,
    PaymasterCapacityProof& proof,
    std::vector<ProviderPoolEntry>& reserved,
    std::string& error)
{
    error.clear();
    proof = {};
    reserved.clear();
    if (tip_height < 0 || minimum_confirmations <= 0 || now <= 0 ||
        !ValidateCapacityRequestEnvelope(request, expected_genesis, now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_CAPACITY_RESERVATION";
        return false;
    }
    const std::vector<unsigned char> request_bytes{CanonicalBytes(request)};
    const uint256 request_hash{Hash(request_bytes)};
    const uint256 session_key{GetCapacitySessionKey(request)};
    if (request_hash.IsNull() || session_key.IsNull()) {
        error = "PAYMASTER_INVALID_CAPACITY_REQUEST_HASH";
        return false;
    }

    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderPolicy policy;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    std::vector<ProviderPoolEntry> entries;
    if (!batch.ReadPaymasterPolicy(policy)) {
        error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderPool(entries)) {
        error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = batch.HasPaymasterProviderSafetyPolicy() ? "PAYMASTER_INVALID_SAFETY_POLICY" : "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger)) {
        error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        return false;
    }
    if (!ValidateProviderPolicy(policy, error) ||
        !ValidateProviderSafetyPolicy(safety_policy, policy, error) ||
        !PolicyAllowsFundingModel(policy, request.funding_model) ||
        !ValidateProviderPoolEntries(entries, error)) {
        if (error.empty()) error = "PAYMASTER_FUNDING_MODEL_NOT_ALLOWED";
        return false;
    }

    uint256 nonce_request_hash;
    const bool have_nonce{batch.ReadPaymasterCapacityNonce(request.client_nonce,
                                                           nonce_request_hash)};
    if (have_nonce && nonce_request_hash != request_hash) {
        error = "PAYMASTER_CAPACITY_REQUEST_CONFLICT";
        return false;
    }
    uint256 session_request_hash;
    const bool have_session{
        batch.ReadPaymasterCapacitySession(session_key, session_request_hash)};
    if (have_session && session_request_hash != request_hash) {
        error = "PAYMASTER_CAPACITY_SESSION_CONFLICT";
        return false;
    }
    std::vector<unsigned char> persisted_response;
    const bool have_response{
        batch.ReadPaymasterCapacityResponse(request_hash, persisted_response)};
    if (have_response) {
        if (!have_nonce || nonce_request_hash != request_hash ||
            !have_session || session_request_hash != request_hash) {
            error = "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH";
            return false;
        }
        return DecodeStoredCapacityResponse(persisted_response, request, identity,
                                            entries, expected_genesis, now,
                                            proof, reserved, error);
    }
    if (have_nonce || have_session) {
        error = "PAYMASTER_CAPACITY_RESPONSE_MISSING";
        return false;
    }

    const uint256 admission_key = GetProviderRequestSlotKey(
        request.provider_id, request.request_id, request.session_id);
    const uint256 netgroup_bucket = GetNetgroupBudgetBucket(
        budget_ledger, canonical_netgroup);
    if (admission_key.IsNull() || netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE";
        return false;
    }
    if (!ReserveProviderCapacityAdmission(
            budget_ledger, safety_policy, admission_key, request_hash,
            netgroup_bucket, request.funding_model, request.requires_carrier,
            request.expires_at, now, error)) {
        return false;
    }

    const bool require_carrier{request.requires_carrier};
    std::vector<size_t> reserved_indexes;
    for (size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].reservation_id == request.client_nonce) {
            reserved_indexes.push_back(index);
        }
    }
    if (!reserved_indexes.empty()) {
        error = "PAYMASTER_CAPACITY_RESERVATION_WITHOUT_RESPONSE";
        return false;
    }
    std::vector<size_t> candidates(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
        candidates[index] = index;
    std::sort(candidates.begin(), candidates.end(), [&](size_t lhs, size_t rhs) {
        return entries[lhs].outpoint < entries[rhs].outpoint;
    });
    std::optional<size_t> dgb_index;
    std::optional<size_t> carrier_index;
    for (const size_t index : candidates) {
        const ProviderPoolEntry& entry{entries[index]};
        const bool confirmed{entry.confirmation_height > 0 &&
                             entry.confirmation_height <= tip_height &&
                             tip_height - entry.confirmation_height + 1 >= minimum_confirmations};
        if (entry.purpose != PoolPurpose::OPERATIONAL ||
            entry.state != PoolEntryState::AVAILABLE || !confirmed) {
            continue;
        }
        if (!dgb_index && entry.asset == PoolAsset::DGB &&
            entry.dgb_value.value >= policy.maximum_network_fee.value) {
            dgb_index = index;
        } else if (require_carrier && !carrier_index &&
                   entry.asset == PoolAsset::DD_CARRIER) {
            carrier_index = index;
        }
    }
    if (!dgb_index || (require_carrier && !carrier_index)) {
        error = "PAYMASTER_OPERATIONAL_SLOT_MISSING";
        return false;
    }
    reserved_indexes.push_back(*dgb_index);
    if (carrier_index) reserved_indexes.push_back(*carrier_index);
    for (const size_t index : reserved_indexes) {
        entries[index].state = PoolEntryState::RESERVED;
        entries[index].reservation_id = request.client_nonce;
        entries[index].updated_at = now;
    }

    reserved.reserve(reserved_indexes.size());
    for (const size_t index : reserved_indexes)
        reserved.push_back(entries[index]);
    std::sort(reserved.begin(), reserved.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.asset != rhs.asset) return lhs.asset == PoolAsset::DGB;
        return lhs.outpoint < rhs.outpoint;
    });

    if (!BuildPaymasterCapacityProof(wallet, identity, request, reserved,
                                     expected_genesis, reference_block, now,
                                     proof, error)) {
        proof = {};
        reserved.clear();
        return false;
    }
    const std::vector<unsigned char> response_bytes{CanonicalBytes(proof)};
    if (response_bytes.empty() || response_bytes.size() > MAX_DIRECT_MESSAGE_BYTES) {
        proof = {};
        reserved.clear();
        error = "PAYMASTER_INVALID_CAPACITY_RESPONSE_SIZE";
        return false;
    }
    if (!batch.TxnBegin()) {
        proof = {};
        reserved.clear();
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterProviderPool(entries) ||
        !batch.WritePaymasterProviderBudgetLedger(budget_ledger) ||
        !batch.WritePaymasterCapacityNonce(request.client_nonce, request_hash) ||
        !batch.WritePaymasterCapacitySession(session_key, request_hash) ||
        !batch.WritePaymasterCapacityResponse(request_hash, response_bytes)) {
        Abort(batch, error);
        proof = {};
        reserved.clear();
        return false;
    }
    if (!batch.TxnCommit()) {
        proof = {};
        reserved.clear();
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    error.clear();
    return true;
}

bool BuildPaymasterAnnouncement(CWallet& wallet,
                                const ProviderIdentityRecord& identity,
                                const ProviderPolicy& policy,
                                const std::vector<ProviderPoolEntry>& pool_entries,
                                const CService& endpoint,
                                const uint256& genesis_hash,
                                const uint256& reference_block,
                                int64_t now,
                                uint8_t permitted_funding_models,
                                Announcement& announcement,
                                std::string& error)
{
    error.clear();
    announcement = {};
    if (!ValidateProviderIdentityRecord(identity) || !ValidateProviderPolicy(policy, error) ||
        (permitted_funding_models & ~FUNDING_MODEL_ALL) != 0 ||
        !endpoint.IsValid() || genesis_hash.IsNull() || reference_block.IsNull() || now <= 0) {
        if (error.empty()) error = "PAYMASTER_INVALID_ANNOUNCEMENT_PARAMETERS";
        return false;
    }
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }

    const uint256 policy_hash{GetProviderPolicyHash(policy)};
    const uint8_t effective_models = policy.funding_models & permitted_funding_models;
    if ((effective_models & FUNDING_MODEL_USER_PAID) != 0) {
        announcement.offers.push_back({GetOfferId(identity, policy_hash, FundingModel::USER_PAID), policy_hash,
                                       FundingModel::USER_PAID, SponsorshipScope::PUBLIC, policy.fee_rate_bps,
                                       policy.min_payment, policy.max_payment});
    }
    if ((effective_models & FUNDING_MODEL_SPONSORED) != 0 &&
        policy.sponsorship_scope == SponsorshipScope::PUBLIC) {
        announcement.offers.push_back({GetOfferId(identity, policy_hash, FundingModel::SPONSORED), policy_hash,
                                       FundingModel::SPONSORED, SponsorshipScope::PUBLIC, 0,
                                       policy.min_payment, policy.max_payment});
    }
    if (announcement.offers.empty()) {
        error = "PAYMASTER_NO_PUBLIC_OFFER";
        return false;
    }

    std::vector<const ProviderPoolEntry*> dgb_entries;
    std::vector<const ProviderPoolEntry*> carrier_entries;
    for (const ProviderPoolEntry& entry : pool_entries) {
        if (entry.purpose != PoolPurpose::ADMISSION ||
            entry.state != PoolEntryState::AVAILABLE || entry.confirmation_height <= 0) continue;
        if (entry.asset == PoolAsset::DGB) dgb_entries.push_back(&entry);
        if (entry.asset == PoolAsset::DD_CARRIER) carrier_entries.push_back(&entry);
    }
    const auto by_outpoint = [](const ProviderPoolEntry* lhs, const ProviderPoolEntry* rhs) {
        return lhs->outpoint < rhs->outpoint;
    };
    std::sort(dgb_entries.begin(), dgb_entries.end(), by_outpoint);
    std::sort(carrier_entries.begin(), carrier_entries.end(), by_outpoint);
    const bool needs_carriers{(effective_models & FUNDING_MODEL_USER_PAID) != 0};
    if (dgb_entries.size() < REQUIRED_ADMISSION_SLOTS ||
        (needs_carriers && carrier_entries.size() < REQUIRED_ADMISSION_SLOTS)) {
        error = "PAYMASTER_ADMISSION_SLOTS_MISSING";
        return false;
    }

    CKey identity_key;
    ProviderIdentityRecord stored_identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, stored_identity, error) ||
        stored_identity.provider_id != identity.provider_id ||
        stored_identity.identity_key != identity.identity_key) {
        if (error.empty()) error = "PAYMASTER_IDENTITY_BINDING_MISMATCH";
        return false;
    }

    uint64_t sequence{0};
    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        uint64_t previous{0};
        batch.ReadPaymasterAnnouncementSequence(previous);
        if (previous == std::numeric_limits<uint64_t>::max()) {
            error = "PAYMASTER_ANNOUNCEMENT_SEQUENCE_EXHAUSTED";
            return false;
        }
        sequence = previous + 1;
        if (!batch.WritePaymasterAnnouncementSequence(sequence)) {
            error = "PAYMASTER_ANNOUNCEMENT_SEQUENCE_DATABASE_WRITE";
            return false;
        }
    }

    announcement.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    announcement.genesis_hash = genesis_hash;
    announcement.identity_key = identity.identity_key;
    announcement.display_name = identity.display_name;
    announcement.sequence = sequence;
    announcement.created_at = now;
    announcement.expires_at = SaturatingAddSeconds(now, ANNOUNCEMENT_TTL_SECONDS);
    announcement.endpoint = endpoint;
    announcement.min_confirmations = 1;
    announcement.capability_flags = 0;
    announcement.admission_slots.reserve(REQUIRED_ADMISSION_SLOTS);

    LOCK(wallet.cs_wallet);
    for (size_t index = 0; index < REQUIRED_ADMISSION_SLOTS; ++index) {
        const ProviderPoolEntry& dgb_entry{*dgb_entries[index]};
        const CWalletTx* dgb_tx{wallet.GetWalletTx(dgb_entry.outpoint.hash)};
        if (!dgb_tx || dgb_entry.outpoint.n >= dgb_tx->tx->vout.size() ||
            dgb_tx->tx->vout[dgb_entry.outpoint.n].scriptPubKey != dgb_entry.script_pub_key ||
            dgb_tx->tx->vout[dgb_entry.outpoint.n].nValue != dgb_entry.dgb_value.value) {
            error = "PAYMASTER_ADMISSION_PREVOUT_MISMATCH";
            return false;
        }
        AdmissionSlotProof slot;
        slot.dgb_outpoint = dgb_entry.outpoint;
        slot.dgb_creating_tx = CMutableTransaction{*dgb_tx->tx};
        slot.dgb_value = dgb_entry.dgb_value;
        slot.reference_block = reference_block;
        slot.expires_at = announcement.expires_at;
        if (!SignBip86ControlHash(wallet, dgb_entry.script_pub_key,
                                  GetAdmissionControlHash(identity.provider_id, sequence,
                                                          reference_block, slot.dgb_outpoint, slot.expires_at, false),
                                  slot.dgb_control_signature)) {
            error = "PAYMASTER_ADMISSION_DGB_SIGNING_FAILED";
            return false;
        }
        if (needs_carriers) {
            const ProviderPoolEntry& carrier_entry{*carrier_entries[index]};
            const CWalletTx* carrier_tx{wallet.GetWalletTx(carrier_entry.outpoint.hash)};
            if (!carrier_tx || carrier_entry.outpoint.n >= carrier_tx->tx->vout.size() ||
                carrier_tx->tx->vout[carrier_entry.outpoint.n].scriptPubKey != carrier_entry.script_pub_key) {
                error = "PAYMASTER_ADMISSION_CARRIER_MISMATCH";
                return false;
            }
            slot.carrier_outpoint = carrier_entry.outpoint;
            slot.carrier_creating_tx = CMutableTransaction{*carrier_tx->tx};
            slot.carrier_value = carrier_entry.carrier_value;
            if (!SignBip86ControlHash(wallet, carrier_entry.script_pub_key,
                                      GetAdmissionControlHash(identity.provider_id, sequence,
                                                              reference_block, slot.carrier_outpoint, slot.expires_at, true),
                                      slot.carrier_control_signature)) {
                error = "PAYMASTER_ADMISSION_CARRIER_SIGNING_FAILED";
                return false;
            }
        }
        announcement.admission_slots.push_back(std::move(slot));
    }
    announcement.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetAnnouncementSignatureHash(announcement),
                                  announcement.identity_signature, nullptr, GetRandHash())) {
        error = "PAYMASTER_ANNOUNCEMENT_SIGNING_FAILED";
        return false;
    }
    return true;
}

bool BuildRestrictedServiceDescriptor(
    CWallet& wallet,
    const ProviderIdentityRecord& identity,
    const ProviderPolicy& policy,
    const std::vector<ProviderPoolEntry>& pool_entries,
    const CService& endpoint,
    const XOnlyPubKey& sponsor_authorization_key,
    const std::string& sponsor_display_name,
    const uint256& genesis_hash,
    const uint256& reference_block,
    int64_t now,
    int64_t expires_at,
    RestrictedServiceDescriptor& descriptor,
    std::string& error)
{
    descriptor = {};
    if (policy.sponsorship_scope != SponsorshipScope::RESTRICTED ||
        policy.funding_models != FUNDING_MODEL_SPONSORED || policy.fee_rate_bps != 0 ||
        !sponsor_authorization_key.IsFullyValid() ||
        !IsValidPaymasterDisplayName(sponsor_display_name) || expires_at <= now ||
        TimeDeltaExceeds(expires_at, now, ANNOUNCEMENT_TTL_SECONDS)) {
        error = "PAYMASTER_INVALID_RESTRICTED_DESCRIPTOR_PARAMETERS";
        return false;
    }

    // Reuse the public admission proof builder, but never publish the
    // resulting announcement. Control signatures bind the durable sequence;
    // the descriptor signature then binds those proofs to the restricted
    // policy, endpoint, and sponsor key.
    ProviderPolicy proof_policy = policy;
    proof_policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    Announcement proof;
    if (!BuildPaymasterAnnouncement(wallet, identity, proof_policy, pool_entries, endpoint,
                                    genesis_hash, reference_block, now,
                                    proof_policy.funding_models, proof, error)) {
        return false;
    }

    descriptor.genesis_hash = genesis_hash;
    descriptor.provider_id = identity.provider_id;
    descriptor.p2p_endpoint = endpoint;
    descriptor.policy_hash = GetProviderPolicyHash(policy);
    descriptor.offer_id = GetOfferId(identity, descriptor.policy_hash, FundingModel::SPONSORED);
    descriptor.sponsor_authorization_key = sponsor_authorization_key;
    descriptor.sponsor_display_name = sponsor_display_name;
    descriptor.expires_at = expires_at;
    descriptor.admission_sequence = proof.sequence;
    descriptor.min_confirmations = proof.min_confirmations;
    descriptor.minimum_liquidity_proof = std::move(proof.admission_slots);

    CKey identity_key;
    ProviderIdentityRecord stored_identity;
    descriptor.provider_identity_signature.resize(64);
    if (!GetPaymasterIdentityKey(wallet, identity_key, stored_identity, error) ||
        stored_identity.provider_id != identity.provider_id ||
        !identity_key.SignSchnorr(GetRestrictedDescriptorSignatureHash(descriptor),
                                  descriptor.provider_identity_signature, nullptr, GetRandHash())) {
        if (error.empty()) error = "PAYMASTER_RESTRICTED_DESCRIPTOR_SIGNING_FAILED";
        return false;
    }
    error.clear();
    return true;
}

} // namespace wallet
