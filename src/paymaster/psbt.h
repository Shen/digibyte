// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Role-aware PSBT manifests and local authorization firewalls. */

#ifndef DIGIBYTE_PAYMASTER_PSBT_H
#define DIGIBYTE_PAYMASTER_PSBT_H

#include <paymaster/txbuilder.h>
#include <paymaster/types.h>
#include <psbt.h>

#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

struct PaymentIntent;
struct PaymasterQuote;
struct ValidatedCapacitySnapshot;
struct ClientAuthorizationManifest;
struct ProviderAuthorizationManifest;
struct ProviderAttempt;
struct ProviderCommitRecord;

enum class SigningParty : uint8_t {
    USER,
    PROVIDER,
};

enum class CollaborativeSignatureStage : uint8_t {
    UNSIGNED,
    USER_SIGNED,
    FULLY_SIGNED,
};

struct CollaborativePSBTTemplate {
    PartiallySignedTransaction psbt;
    std::vector<InputRole> input_roles;
};

/** Construct the only PSBT shape accepted for a built collaborative transfer.
 * Every input includes its fully resolved creating transaction and explicitly
 * requests SIGHASH_DEFAULT. No key paths, signatures, or proprietary fields
 * are added.
 */
bool CreateCollaborativePSBTTemplate(
    const CMutableTransaction& transaction,
    const std::vector<CollaborativeInput>& inputs,
    CollaborativePSBTTemplate& result,
    std::string& error);

/** Validate an untrusted PSBT against the exact locally committed template.
 * Non-signature fields must be byte-for-byte identical. Signature presence
 * and verification are derived solely from the explicit input roles and
 * expected protocol stage.
 */
bool ValidateCollaborativePSBT(
    const PartiallySignedTransaction& candidate,
    const CollaborativePSBTTemplate& trusted_template,
    CollaborativeSignatureStage expected_stage,
    std::string& error);

/** Reconstruct a fully signed PSBT from an untrusted final transaction and the
 * exact locally committed template. This binds the non-witness transaction,
 * txid, wtxid, every input witness, and all signatures to the trusted prevouts.
 */
bool ValidateFinalCollaborativeTransaction(
    const CMutableTransaction& final_transaction,
    const uint256& expected_txid,
    const uint256& expected_wtxid,
    const CollaborativePSBTTemplate& trusted_template,
    std::string& error);

bool IsInputOwnedBy(InputRole role, SigningParty party);

/** Domain-separated identifier for the exact unsigned transaction, resolved
 * prevouts, input roles and SIGHASH_DEFAULT-only PSBT. The quote projection
 * deliberately excludes its commitment and identity signature. */
uint256 GetCollaborativeTemplateCommitment(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted_template);

uint256 GetClientAuthorizationManifestId(const ClientAuthorizationManifest& manifest);
uint256 GetProviderAuthorizationManifestId(const ProviderAuthorizationManifest& manifest);

/** Decode the durable capacity proof and require the quote to consume exactly
 * its proven DGB inputs (and, when used, its proven carrier).  This belongs in
 * the common spend firewall rather than an RPC-only precheck so every signing
 * and replay path receives the same protection. */
bool ValidateQuoteAgainstCapacitySnapshot(
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    std::string& error);

/** Build and self-validate the immutable client authority immediately after
 * a quote and its capacity proof have been validated locally. */
bool BuildClientAuthorizationManifest(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    const CollaborativePSBTTemplate& trusted_template,
    DDCents maximum_service_fee,
    ClientAuthorizationManifest& manifest,
    std::string& error);

/** Variant which additionally binds the original wallet-local amount
 * semantics used by senddigidollar. The wire intent remains the exact
 * recipient amount. */
bool BuildClientAuthorizationManifest(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    const CollaborativePSBTTemplate& trusted_template,
    DDCents maximum_service_fee,
    DDCents requested_amount,
    bool subtract_paymaster_fee_from_amount,
    bool send_all_spendable_dd,
    ClientAuthorizationManifest& manifest,
    std::string& error);

/** Re-run the independent client spend firewall. This must immediately
 * precede every user signature, submit retry, recovery, and final acceptance. */
bool ValidateClientAuthorizationManifest(
    const ClientAuthorizationManifest& manifest,
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const ValidatedCapacitySnapshot& capacity,
    const CollaborativePSBTTemplate& trusted_template,
    std::string& error);

/** Build and validate the provider's wallet-local authority for exactly the
 * pool inputs and change/fee scripts committed by one quote. */
bool BuildProviderAuthorizationManifest(
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted_template,
    const uint256& safety_policy_hash,
    ProviderAuthorizationManifest& manifest,
    std::string& error);

/** Re-run the independent provider spend firewall before every provider
 * signature, final commit retry, recovery, and broadcast. */
bool ValidateProviderAuthorizationManifest(
    const ProviderAuthorizationManifest& manifest,
    const PaymentIntent& intent,
    const PaymasterQuote& quote,
    const CollaborativePSBTTemplate& trusted_template,
    std::string& error);

/** Decode the immutable provider-side attempt artifacts and re-run the
 * provider authorization firewall. This is intentionally independent of RPC
 * and must be called immediately before every provider signature.
 */
bool ValidateProviderAuthorizationForExecution(
    const ProviderAttempt& attempt,
    int64_t now,
    CollaborativePSBTTemplate& trusted_template,
    std::string& error);

/** Re-run the provider authorization firewall and validate every byte of a
 * durable final commit against the trusted PSBT before recovery or broadcast.
 * The returned transaction is suitable for a subsequent mempool preflight;
 * callers must not decode or execute another transaction instead.
 */
bool ValidateProviderCommitForExecution(
    const ProviderAttempt& attempt,
    const ProviderCommitRecord& commit,
    int64_t now,
    CMutableTransaction& final_transaction,
    std::string& error);

/** Reconstruct the exact client authorization and final transaction from one
 * durable attempt. Unlike provider recovery, legacy attempts are never
 * accepted here: startup/retry broadcast requires the current client manifest,
 * its explicit local acceptance, and the exact persisted user-signed PSBT.
 * Quote expiry cannot revoke an already complete signature, so no wall-clock
 * parameter is needed. The caller must still revalidate the capacity proof
 * against chainstate and perform a mempool preflight immediately before
 * broadcast. */
bool ValidateClientFinalForExecution(
    const ProviderAttempt& attempt,
    const uint256& expected_wtxid,
    CMutableTransaction& final_transaction,
    std::string& error);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_PSBT_H
