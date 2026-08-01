// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Deterministic unsigned Paymaster transaction construction. */

#ifndef DIGIBYTE_PAYMASTER_TXBUILDER_H
#define DIGIBYTE_PAYMASTER_TXBUILDER_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>

#include <cstdint>
#include <string>
#include <vector>

class CChainParams;
class CCoinsViewCache;
class ChainstateManager;

namespace DigiDollar::Paymaster {

enum class InputRole : uint8_t {
    USER_DD,
    USER_DGB,
    PROVIDER_CARRIER,
    PROVIDER_DGB,
};

enum class DDOutputRole : uint8_t {
    RECIPIENT,
    USER_CHANGE,
    PROVIDER_FEE,
    CARRIER_SUCCESSOR,
};

/** An input together with the transaction that created it.
 *
 * The creating transaction is untrusted transport data. The builder verifies
 * its hash, output index, and output against the active UTXO set before using
 * it and derives any DD amount from its canonical DD metadata.
 */
struct CollaborativeInput {
    COutPoint outpoint;
    CTransactionRef creating_tx;
    InputRole role{InputRole::USER_DD};
};

struct CollaborativeDDOutput {
    CScript script_pub_key;
    CAmount amount{0}; // DigiDollar cents
    DDOutputRole role{DDOutputRole::RECIPIENT};
};

struct CollaborativeTransferParams {
    std::vector<CollaborativeInput> inputs;
    std::vector<CollaborativeDDOutput> dd_outputs;
    std::vector<CTxOut> dgb_outputs;
    CAmount fee_rate{0}; // sat/kvB
};

struct CollaborativeTransferResult {
    bool success{false};
    CMutableTransaction tx;
    CAmount miner_fee{0};
    std::string error;
};

/** Chainstate-validated USER_DD inputs and the ordered output keys needed to
 * verify their intent or recovery control proofs. */
struct ValidatedUserDDInputs {
    CAmount total_dd_amount{0};
    std::vector<XOnlyPubKey> output_keys;
};

/** Validate fully resolved USER_DD inputs without using or mutating a wallet.
 *
 * Each input must have the USER_DD role and a unique outpoint. Its supplied
 * creating transaction, active unspent non-coinbase coin, canonical DD amount,
 * and fully valid P2TR output key must all agree exactly.
 */
bool ValidateCollaborativeUserDDInputs(
    const std::vector<CollaborativeInput>& inputs,
    const CCoinsViewCache& coins,
    ValidatedUserDDInputs& validated,
    std::string& error);

/** Build the fixed unsigned transaction after validating inputs against the
 * active chainstate. No wallet mutation, signing, commit, or broadcast occurs.
 */
CollaborativeTransferResult BuildUnsignedCollaborativeTransfer(
    const CollaborativeTransferParams& params,
    const CChainParams& chain_params,
    const ChainstateManager& chainman);

/** Validation-view overload used by focused tests and callers that already
 * hold a stable coins view. The creating transactions remain independently
 * verified against this view.
 */
CollaborativeTransferResult BuildUnsignedCollaborativeTransfer(
    const CollaborativeTransferParams& params,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_TXBUILDER_H
