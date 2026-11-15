// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_PAYMASTER_CONFLICT_H
#define DIGIBYTE_PAYMASTER_CONFLICT_H

#include <serialize.h>
#include <uint256.h>
#include <cstdint>
#include <string>
#include <vector>

class CBlock;
class CTransaction;
namespace DigiDollar::Paymaster {
struct ConflictAncestorsFormatter {
    template <typename Stream> void Ser(Stream& stream, const std::vector<std::vector<unsigned char>>& values)
    {
        WriteCompactSize(stream, values.size());
        for (const auto& value : values) ::Serialize(stream, value);
    }
    template <typename Stream> void Unser(Stream& stream, std::vector<std::vector<unsigned char>>& values)
    {
        const auto count = ReadCompactSize(stream);
        if (count > 256) throw std::ios_base::failure("too many Paymaster conflict ancestors");
        values.clear();
        for (uint64_t i{0}; i < count; ++i) {
            const auto size = ReadCompactSize(stream);
            if (size > 4 * 1024 * 1024 || size > stream.size()) throw std::ios_base::failure("oversized conflict ancestor");
            values.emplace_back(size);
            stream.read(MakeWritableByteSpan(values.back()));
        }
    }
};
/** Versioned evidence; active-chain maturity and financial closure belong to
 * the wallet application. This value/interface has no wallet dependency. */
struct ProviderConflictClosure {
    static constexpr uint16_t CURRENT_VERSION{1};
    static constexpr size_t MAX_BYTES{4 * 1024 * 1024};
    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash, provider_id, authority_hash;
    uint256 transaction_id, witness_hash, shared_input_hash;
    uint32_t shared_input_index{UINT32_MAX};
    std::vector<unsigned char> original_transaction, winner_transaction, merkle_block;
    std::vector<std::vector<unsigned char>> blocked_ancestors;
    int64_t closed_at{0};
    SERIALIZE_METHODS(ProviderConflictClosure, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.authority_hash,
                  obj.transaction_id, obj.witness_hash, obj.shared_input_hash, obj.shared_input_index,
                  obj.original_transaction, obj.winner_transaction, obj.merkle_block,
                  Using<ConflictAncestorsFormatter>(obj.blocked_ancestors), obj.closed_at);
    }
};
struct ConflictInclusion { uint256 block_hash; uint32_t transactions{0}; };
bool BuildPaymasterConflictInclusion(const CBlock& block, const CTransaction& original, ProviderConflictClosure& record);
bool ExtendPaymasterConflictInclusion(const CTransaction& descendant, const ProviderConflictClosure& parent,
    ProviderConflictClosure& extended);
bool ValidatePaymasterConflictInclusion(const ProviderConflictClosure& record, ConflictInclusion& inclusion, std::string& error);
} // namespace DigiDollar::Paymaster
#endif // DIGIBYTE_PAYMASTER_CONFLICT_H
