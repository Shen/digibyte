// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <paymaster/conflict.h>
#include <merkleblock.h>
#include <streams.h>
#include <version.h>
#include <algorithm>
#include <set>

namespace DigiDollar::Paymaster {
namespace {
template <typename T> std::vector<unsigned char> Bytes(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}
}
bool BuildPaymasterConflictInclusion(const CBlock& block, const CTransaction& original, ProviderConflictClosure& record)
{
    std::set<COutPoint> inputs;
    for (const auto& input : original.vin) inputs.insert(input.prevout);
    for (const auto& winner : block.vtx) {
        if (winner->GetHash() == original.GetHash()) continue;
        for (const auto& input : winner->vin) {
            if (!inputs.count(input.prevout)) continue;
            record.transaction_id = original.GetHash();
            record.witness_hash = original.GetWitnessHash();
            record.shared_input_hash = input.prevout.hash;
            record.shared_input_index = input.prevout.n;
            record.original_transaction = Bytes(original);
            record.winner_transaction = Bytes(*winner);
            record.merkle_block = Bytes(CMerkleBlock{block, std::set<uint256>{winner->GetHash()}});
            record.blocked_ancestors.clear();
            return true;
        }
    }
    return false;
}
bool ValidatePaymasterConflictInclusion(const ProviderConflictClosure& record, ConflictInclusion& inclusion, std::string& error)
{
    const auto invalid = [&] { error = "PAYMASTER_CONFLICT_PROOF_INVALID"; return false; };
    if (record.version != ProviderConflictClosure::CURRENT_VERSION || record.blocked_ancestors.size() > 256 ||
        GetSerializeSize(record) > ProviderConflictClosure::MAX_BYTES) return invalid();
    CMutableTransaction original, winner;
    CMerkleBlock block;
    try {
        SpanReader original_stream{::PROTOCOL_VERSION, record.original_transaction};
        SpanReader tx_stream{::PROTOCOL_VERSION, record.winner_transaction};
        SpanReader block_stream{::PROTOCOL_VERSION, record.merkle_block};
        original_stream >> original;
        tx_stream >> winner;
        block_stream >> block;
        if (!original_stream.empty() || !tx_stream.empty() || !block_stream.empty() ||
            Bytes(original) != record.original_transaction || Bytes(winner) != record.winner_transaction ||
            Bytes(block) != record.merkle_block) return invalid();
    } catch (const std::exception&) { return invalid(); }
    const COutPoint shared{record.shared_input_hash, record.shared_input_index};
    const auto spends = [&](const auto& tx) {
        return std::any_of(tx.vin.begin(), tx.vin.end(), [&](const auto& input) { return input.prevout == shared; });
    };
    const CTransaction exact{original};
    if (shared.IsNull() || exact.GetHash() != record.transaction_id || exact.GetWitnessHash() != record.witness_hash ||
        winner.GetHash() == exact.GetHash() || !spends(winner)) return invalid();
    for (const auto& bytes : record.blocked_ancestors) {
        CMutableTransaction parent;
        try {
            SpanReader stream{::PROTOCOL_VERSION, bytes};
            stream >> parent;
            if (!stream.empty() || Bytes(parent) != bytes) return invalid();
        } catch (const std::exception&) { return invalid(); }
        if (parent.GetHash() == winner.GetHash() ||
            std::none_of(original.vin.begin(), original.vin.end(), [&](const auto& input) {
                return input.prevout.hash == parent.GetHash() && input.prevout.n < parent.vout.size();
            })) return invalid();
        original = std::move(parent);
    }
    if (!spends(original)) return invalid();
    std::vector<uint256> matches;
    std::vector<unsigned int> positions;
    if (block.txn.ExtractMatches(matches, positions) != block.header.hashMerkleRoot ||
        matches.size() != 1 || matches.front() != winner.GetHash()) return invalid();
    inclusion = {block.header.GetHash(), block.txn.GetNumTransactions()};
    error.clear();
    return true;
}
bool ExtendPaymasterConflictInclusion(const CTransaction& descendant, const ProviderConflictClosure& parent,
    ProviderConflictClosure& extended)
{
    if (parent.blocked_ancestors.size() >= 256) return false;
    auto candidate = parent;
    candidate.transaction_id = descendant.GetHash();
    candidate.witness_hash = descendant.GetWitnessHash();
    candidate.original_transaction = Bytes(descendant);
    candidate.blocked_ancestors.insert(candidate.blocked_ancestors.begin(), parent.original_transaction);
    ConflictInclusion inclusion;
    std::string error;
    if (!ValidatePaymasterConflictInclusion(candidate, inclusion, error)) return false;
    extended = std::move(candidate);
    return true;
}
} // namespace DigiDollar::Paymaster
