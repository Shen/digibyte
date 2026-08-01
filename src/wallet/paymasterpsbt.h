// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Wallet signing entry points guarded by Paymaster authorization manifests. */

#ifndef DIGIBYTE_WALLET_PAYMASTERPSBT_H
#define DIGIBYTE_WALLET_PAYMASTERPSBT_H

#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/recovery.h>

#include <string>

class ChainstateManager;

namespace wallet {

class CWallet;

/** Sign and finalize only inputs assigned to party. The untrusted PSBT is
 * validated before signing, and the result is reconstructed from the trusted
 * input so FillPSBT cannot add or alter fields outside the permitted signature
 * slots.
 */
bool SignCollaborativePSBTForParty(
    CWallet& wallet,
    PartiallySignedTransaction& psbt,
    const DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted_template,
    DigiDollar::Paymaster::SigningParty party,
    std::string& error);

/** Resolve each USER_DD prevout to its wallet-owned BIP86 key and attach a
 * domain-separated control proof. No partial proof set is returned on error. */
bool SignPaymentIntentInputs(
    CWallet& wallet,
    DigiDollar::Paymaster::PaymentIntent& intent,
    int64_t now,
    std::vector<XOnlyPubKey>& output_keys,
    std::string& error);

/** Fail closed unless every client input and the optional DD change script in
 * the exact manifest are currently spendable by this wallet. This is the
 * wallet-local half of the client spend firewall and must run immediately
 * before signing, retry, recovery, and broadcast execution. */
bool ValidateClientAuthorizationOwnership(
    CWallet& wallet,
    const DigiDollar::Paymaster::ClientAuthorizationManifest& manifest,
    std::string& error);

/** Validate an already durable legacy client final without creating new
 * authority. The caller must supply the exact PaymasterResult loaded from the
 * wallet database; live result ingestion, signing, submit retry, and first
 * persistence must continue to use the current manifest path. Only a V1/V2
 * client manifest (or the exact empty pre-manifest value) is projected into an
 * ephemeral current manifest. The projection is never persisted and the
 * normal current-manifest acceptance boundary is not weakened.
 *
 * The signed result, attempt artifacts, user-signed PSBT, final transaction,
 * all witnesses/scripts, and wallet ownership are revalidated before the
 * returned transaction may be considered for recovery broadcast. The caller
 * must still bind the attempt/result to its durable session, revalidate any
 * required chainstate conditions, and perform the usual mempool preflight. */
bool ValidatePersistedLegacyClientFinalForRecovery(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    const DigiDollar::Paymaster::PaymasterResult& persisted_result,
    const uint256& expected_genesis,
    CMutableTransaction& final_transaction,
    std::string& error);

/** Backward-compatible narrow wrapper used by older focused tests. New
 * authorization boundaries must call ValidateClientAuthorizationOwnership. */
bool ValidateClientChangeScriptOwnership(
    CWallet& wallet,
    const DigiDollar::Paymaster::ClientAuthorizationManifest& manifest,
    std::string& error);

/** Fail closed unless every provider pool input and every provider fee,
 * carrier-return, and DGB-change script in the exact current or V1 legacy
 * manifest are currently spendable by this wallet. Manifest-less legacy
 * commits are handled only by their narrow, already-durable recovery path.
 * This is the wallet-local half of the provider spend firewall and is
 * independent from the pure transaction validator. */
bool ValidateProviderAuthorizationOwnership(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderAuthorizationManifest& manifest,
    std::string& error);

bool SignAlternativeRecoveryRequestInputs(
    CWallet& wallet,
    DigiDollar::Paymaster::AlternativeRecoveryRequest& request,
    int64_t now,
    std::vector<XOnlyPubKey>& output_keys,
    std::string& error);

/** Reconstruct an untrusted provider quote from wallet-owned USER_DD prevouts
 * and provider-supplied prevouts, verify it against chainstate, and create the
 * only PSBT template that may subsequently be persisted or signed. */
bool BuildTrustedQuoteTemplate(
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted_template,
    std::string& error);

/** Reconstruct the complete wallet-local authority used by every alternative
 * recovery validator. Client records additionally prove ownership of each DD
 * input and cancel-to-self return script. Provider records may resolve remote
 * user prevouts through txindex, but every recovery-provider input and its DD
 * fee and DGB change scripts must remain spendable by the local wallet.
 * Keeping this outside RPC ensures
 * restart and retry paths cannot drift from the signing path's authority
 * model. */
bool BuildAlternativeRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error);

} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERPSBT_H
