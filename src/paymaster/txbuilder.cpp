// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Deterministic construction of the unsigned collaborative DD transfer.
 * The builder accepts already classified inputs and outputs, enforces value
 * conservation and standardness, and never signs or performs wallet lookup.
 */

#include <paymaster/txbuilder.h>

#include <coins.h>
#include <consensus/consensus.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <kernel/chainparams.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <sync.h>
#include <util/overflow.h>
#include <validation.h>

#include <algorithm>
#include <optional>
#include <set>

namespace DigiDollar::Paymaster {
namespace {

constexpr CAmount MAX_DD_OUTPUT_CENTS{10000000};
constexpr CAmount MIN_PAYMASTER_TX_FEE{COIN / 10};

bool IsCanonicalP2TR(const CScript& script)
{
    int witness_version{-1};
    std::vector<unsigned char> witness_program;
    return script.IsWitnessProgram(witness_version, witness_program) &&
           witness_version == 1 && witness_program.size() == WITNESS_V1_TAPROOT_SIZE;
}

std::optional<XOnlyPubKey> GetP2TROutputKey(const CScript& script)
{
    int witness_version{-1};
    std::vector<unsigned char> witness_program;
    if (!script.IsWitnessProgram(witness_version, witness_program) ||
        witness_version != 1 || witness_program.size() != WITNESS_V1_TAPROOT_SIZE) {
        return std::nullopt;
    }
    XOnlyPubKey output_key{witness_program};
    if (!output_key.IsFullyValid()) return std::nullopt;
    return output_key;
}

CollaborativeTransferResult Failure(std::string error)
{
    CollaborativeTransferResult result;
    result.error = std::move(error);
    return result;
}

bool AddAmount(CAmount& total, CAmount amount)
{
    const auto sum = CheckedAdd(total, amount);
    if (!sum) return false;
    total = *sum;
    return true;
}

bool IsDDRole(InputRole role)
{
    return role == InputRole::USER_DD || role == InputRole::PROVIDER_CARRIER;
}

bool IsKnownInputRole(InputRole role)
{
    switch (role) {
    case InputRole::USER_DD:
    case InputRole::USER_DGB:
    case InputRole::PROVIDER_CARRIER:
    case InputRole::PROVIDER_DGB:
        return true;
    }
    return false;
}

bool IsKnownDDOutputRole(DDOutputRole role)
{
    switch (role) {
    case DDOutputRole::RECIPIENT:
    case DDOutputRole::USER_CHANGE:
    case DDOutputRole::PROVIDER_FEE:
    case DDOutputRole::CARRIER_SUCCESSOR:
        return true;
    }
    return false;
}

struct ResolvedCollaborativeInput {
    CAmount dd_amount{0};
    CAmount dgb_amount{0};
    std::optional<XOnlyPubKey> user_output_key;
};

bool ResolveCollaborativeInput(const CollaborativeInput& input,
                               const CCoinsViewCache& coins,
                               ResolvedCollaborativeInput& resolved,
                               std::string& error)
{
    resolved = {};
    if (!IsKnownInputRole(input.role)) {
        error = "PAYMASTER_INVALID_INPUT_ROLE";
        return false;
    }
    if (input.outpoint.IsNull() || !input.creating_tx ||
        input.creating_tx->GetHash() != input.outpoint.hash ||
        input.outpoint.n >= input.creating_tx->vout.size()) {
        error = "PAYMASTER_INVALID_CREATING_TRANSACTION";
        return false;
    }

    const Coin& coin = coins.AccessCoin(input.outpoint);
    if (coin.IsSpent() || coin.IsCoinBase() || input.creating_tx->IsCoinBase() ||
        coin.out != input.creating_tx->vout[input.outpoint.n]) {
        error = "PAYMASTER_PREVOUT_MISMATCH";
        return false;
    }

    CAmount dd_amount{0};
    const bool is_dd = ExtractDDAmountFromTransaction(*input.creating_tx,
                                                      input.outpoint, dd_amount);
    if (IsDDRole(input.role)) {
        if (!is_dd || dd_amount <= 0) {
            error = "PAYMASTER_INVALID_DD_INPUT";
            return false;
        }
        resolved.dd_amount = dd_amount;
        if (input.role == InputRole::USER_DD) {
            resolved.user_output_key =
                GetP2TROutputKey(coin.out.scriptPubKey);
            if (!resolved.user_output_key) {
                error = "PAYMASTER_INVALID_DD_INPUT";
                return false;
            }
        }
    } else {
        if (is_dd || coin.out.nValue <= 0 || !MoneyRange(coin.out.nValue)) {
            error = "PAYMASTER_INVALID_DGB_INPUT";
            return false;
        }
        resolved.dgb_amount = coin.out.nValue;
    }
    return true;
}

} // namespace

bool ValidateCollaborativeUserDDInputs(
    const std::vector<CollaborativeInput>& inputs,
    const CCoinsViewCache& coins,
    ValidatedUserDDInputs& validated,
    std::string& error)
{
    validated = {};
    if (inputs.empty()) {
        error = "PAYMASTER_NO_INPUTS";
        return false;
    }

    ValidatedUserDDInputs candidate;
    candidate.output_keys.reserve(inputs.size());
    std::set<COutPoint> unique_inputs;
    for (const CollaborativeInput& input : inputs) {
        if (input.role != InputRole::USER_DD) {
            error = "PAYMASTER_INVALID_INPUT_ROLE";
            return false;
        }
        if (!unique_inputs.insert(input.outpoint).second) {
            error = "PAYMASTER_DUPLICATE_INPUT";
            return false;
        }
        ResolvedCollaborativeInput resolved;
        if (!ResolveCollaborativeInput(input, coins, resolved, error)) return false;
        if (!AddAmount(candidate.total_dd_amount, resolved.dd_amount) ||
            !resolved.user_output_key) {
            error = "PAYMASTER_INVALID_DD_INPUT";
            return false;
        }
        candidate.output_keys.push_back(*resolved.user_output_key);
    }

    validated = std::move(candidate);
    error.clear();
    return true;
}

CollaborativeTransferResult BuildUnsignedCollaborativeTransfer(
    const CollaborativeTransferParams& params,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins)
{
    if (params.inputs.empty()) return Failure("PAYMASTER_NO_INPUTS");
    if (params.dd_outputs.empty()) return Failure("PAYMASTER_NO_DD_OUTPUTS");
    if (params.fee_rate <= 0) return Failure("PAYMASTER_INVALID_FEE_RATE");

    size_t recipient_count{0};
    CAmount total_dd_in{0};
    CAmount total_dgb_in{0};
    std::set<COutPoint> unique_inputs;
    std::vector<CollaborativeInput> user_dd_inputs;
    CMutableTransaction tx;
    tx.SetDigiDollarType(::DD_TX_TRANSFER);

    for (const CollaborativeInput& input : params.inputs) {
        if (!IsKnownInputRole(input.role)) return Failure("PAYMASTER_INVALID_INPUT_ROLE");
        if (!unique_inputs.insert(input.outpoint).second) return Failure("PAYMASTER_DUPLICATE_INPUT");
        if (input.role == InputRole::USER_DD) user_dd_inputs.push_back(input);
    }

    if (!user_dd_inputs.empty()) {
        ValidatedUserDDInputs validated;
        std::string error;
        if (!ValidateCollaborativeUserDDInputs(user_dd_inputs, coins,
                                               validated, error)) {
            return Failure(std::move(error));
        }
        total_dd_in = validated.total_dd_amount;
    }

    for (const CollaborativeInput& input : params.inputs) {
        if (input.role != InputRole::USER_DD) {
            ResolvedCollaborativeInput resolved;
            std::string error;
            if (!ResolveCollaborativeInput(input, coins, resolved, error)) {
                return Failure(std::move(error));
            }
            if (IsDDRole(input.role)) {
                if (!AddAmount(total_dd_in, resolved.dd_amount)) {
                    return Failure("PAYMASTER_INVALID_DD_INPUT");
                }
            } else if (!AddAmount(total_dgb_in, resolved.dgb_amount)) {
                return Failure("PAYMASTER_INVALID_DGB_INPUT");
            }
        }
        tx.vin.emplace_back(input.outpoint);
    }

    const CAmount minimum_dd = GetMinimumDDOutput(chain_params.GetDigiDollarParams());
    CAmount total_dd_out{0};
    std::vector<CAmount> metadata_amounts;
    metadata_amounts.reserve(params.dd_outputs.size());
    for (const CollaborativeDDOutput& output : params.dd_outputs) {
        if (!IsKnownDDOutputRole(output.role)) return Failure("PAYMASTER_INVALID_DD_OUTPUT_ROLE");
        if (output.role == DDOutputRole::RECIPIENT) ++recipient_count;
        if (output.amount < minimum_dd || output.amount > MAX_DD_OUTPUT_CENTS ||
            !IsCanonicalP2TR(output.script_pub_key) || !AddAmount(total_dd_out, output.amount)) {
            return Failure("PAYMASTER_INVALID_DD_OUTPUT");
        }
        tx.vout.emplace_back(0, output.script_pub_key);
        metadata_amounts.push_back(output.amount);
    }
    if (recipient_count != 1) return Failure("PAYMASTER_RECIPIENT_COUNT");
    if (total_dd_in != total_dd_out) return Failure("PAYMASTER_DD_CONSERVATION");

    CAmount total_dgb_out{0};
    const CFeeRate dust_rate{DUST_RELAY_TX_FEE};
    for (const CTxOut& output : params.dgb_outputs) {
        if (!MoneyRange(output.nValue) || output.nValue <= 0 ||
            output.nValue < GetDustThreshold(output, dust_rate) ||
            !AddAmount(total_dgb_out, output.nValue)) {
            return Failure("PAYMASTER_INVALID_DGB_OUTPUT");
        }
        tx.vout.push_back(output);
    }

    CScript metadata;
    metadata << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(DD_TX_TRANSFER);
    for (const CAmount amount : metadata_amounts)
        metadata << CScriptNum(amount);
    if (metadata.size() > MAX_OP_RETURN_RELAY) return Failure("PAYMASTER_METADATA_TOO_LARGE");
    tx.vout.emplace_back(0, metadata);

    if (total_dgb_in < total_dgb_out) return Failure("PAYMASTER_DGB_CONSERVATION");
    const CAmount miner_fee = total_dgb_in - total_dgb_out;
    const CAmount required_fee = std::max(CFeeRate{params.fee_rate}.GetFee(EstimateTransactionVSize(tx)), MIN_PAYMASTER_TX_FEE);
    if (miner_fee < required_fee) return Failure("PAYMASTER_INSUFFICIENT_MINER_FEE");

    tx.nVersion = 0x02000770;
    tx.nLockTime = 0;
    if (GetTransactionWeight(CTransaction{tx}) > MAX_STANDARD_TX_WEIGHT) {
        return Failure("PAYMASTER_TRANSACTION_TOO_LARGE");
    }

    CollaborativeTransferResult result;
    result.success = true;
    result.tx = std::move(tx);
    result.miner_fee = miner_fee;
    return result;
}

CollaborativeTransferResult BuildUnsignedCollaborativeTransfer(
    const CollaborativeTransferParams& params,
    const CChainParams& chain_params,
    const ChainstateManager& chainman)
{
    LOCK(cs_main);
    return BuildUnsignedCollaborativeTransfer(params, chain_params, chainman.ActiveChainstate().CoinsTip());
}

} // namespace DigiDollar::Paymaster
