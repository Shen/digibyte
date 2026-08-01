// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Chainstate-backed validation for provider capacity and input control. */

#ifndef DIGIBYTE_PAYMASTER_VALIDATION_H
#define DIGIBYTE_PAYMASTER_VALIDATION_H

#include <paymaster/directory.h>
#include <paymaster/recovery.h>
#include <paymaster/sponsorship.h>
#include <paymaster/wire.h>

#include <cstdint>
#include <string>

class ChainstateManager;
class CTransaction;

namespace DigiDollar::Paymaster {

/** Resource-spend policy for revalidating an already authorized Capacity
 * proof. A locally persisted unsigned transaction is never sufficient to use
 * EXACT_FINAL_ALREADY_KNOWN; the caller must have independently observed the
 * exact fully signed transaction in the mempool or active chain. */
enum class AuthorizedCapacityResourceMode : uint8_t {
    REQUIRE_UNSPENT,
    EXACT_FINAL_ALREADY_KNOWN,
};

uint256 GetAdmissionControlHash(const PaymasterId& provider_id,
                                uint64_t sequence,
                                const uint256& reference_block,
                                const COutPoint& outpoint,
                                int64_t expires_at,
                                bool carrier);

/** Validate current, confirmed, disjoint admission UTXOs and their control
 * signatures. This is intentionally separate from cheap envelope validation.
 */
bool ValidateAdmissionProofs(const Announcement& announcement,
                             const ChainstateManager& chainman,
                             std::string& error);

/** Validate the non-gossiped admission reserve in a restricted descriptor
 * with the same chainstate and control-signature rules as public discovery. */
bool ValidateRestrictedAdmissionProofs(const RestrictedServiceDescriptor& descriptor,
                                       const XOnlyPubKey& provider_identity_key,
                                       const ChainstateManager& chainman,
                                       std::string& error);

/** Complete node-side capacity validation against one coherent active-chain
 * view, including creating transactions, exact outputs/amounts, unspentness,
 * mempool conflicts, P2TR output keys and all provider signatures. */
bool ValidateCapacityProofAgainstChainstate(
    const PaymasterCapacityProof& proof,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const ChainstateManager& chainman,
    int64_t now,
    std::string& error,
    const CTransaction* exact_known_spender = nullptr,
    bool exact_spender_is_known = false);

/** Revalidate the Capacity authority behind an exact, already signed retry.
 * Cryptographic envelopes are checked at authorization_time, while reference
 * block membership, creating outputs, values, scripts, output keys,
 * unspentness and mempool conflicts are checked against the current
 * chainstate at observation_time. An old but still-active reference block is
 * accepted here because it was recent when the manifest was authorized. */
bool ValidateAuthorizedCapacityRetryAgainstChainstate(
    const PaymasterCapacityProof& proof,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    std::string& error,
    const CTransaction* exact_final_transaction = nullptr);

bool BuildAlternativeRecoveryTemplateAgainstChainstate(
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    AlternativeRecoveryTemplate& result,
    std::string& error);

bool ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    AlternativeRecoveryTemplate& trusted,
    PartiallySignedTransaction& unsigned_psbt,
    std::string& error,
    const CTransaction* exact_known_spender = nullptr,
    bool exact_spender_is_known = false);

/** Durable counterpart for an already authorized alternative-recovery
 * response. No new signature authority is created at observation_time. */
bool ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryRequest& request,
    const XOnlyPubKey& provider_identity_key,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    AlternativeRecoveryTemplate& trusted,
    PartiallySignedTransaction& unsigned_psbt,
    std::string& error,
    const CTransaction* exact_final_transaction = nullptr);

bool ValidateAlternativeRecoveryPSBTAgainstChainstate(
    const PartiallySignedTransaction& candidate,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    CollaborativeSignatureStage expected_stage,
    std::string& error);

bool ValidateAlternativeRecoverySubmitAgainstChainstate(
    const AlternativeRecoverySubmit& submit,
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    PartiallySignedTransaction& user_psbt,
    std::string& error,
    const CTransaction* exact_known_spender = nullptr,
    bool exact_spender_is_known = false);

/** Durable counterpart for an exact already-user-signed recovery submit. */
bool ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
    const AlternativeRecoverySubmit& submit,
    const AlternativeRecoveryResponse& response,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    PartiallySignedTransaction& user_psbt,
    std::string& error,
    const CTransaction* exact_final_transaction = nullptr);

bool ValidateFinalAlternativeRecoveryAgainstChainstate(
    const CMutableTransaction& final_transaction,
    const uint256& expected_wtxid,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    bool exact_final_already_known,
    std::string& error);

/** Full final-transaction firewall for an already authorized alternative
 * recovery, with current chainstate and historical envelope authority. */
bool ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
    const CMutableTransaction& final_transaction,
    const uint256& expected_wtxid,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    AuthorizedCapacityResourceMode resource_mode,
    std::string& error);

/** Complete client-side final-result firewall: authenticate and bind the
 * result envelope, then validate the exact transaction/witness against current
 * chainstate and the freshly rebuilt recovery authority. */
bool ValidateFinalAlternativeRecoveryResultAgainstChainstate(
    const AlternativeRecoveryResultMessage& message,
    const AlternativeRecoveryResponse& response,
    const XOnlyPubKey& provider_identity_key,
    uint64_t minimum_sequence,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t now,
    bool exact_final_already_known,
    std::string& error);

/** Validate a durable recovery result at the current observation time while
 * rebuilding its immutable signing authority at the earlier instant at which
 * it was explicitly accepted. This split is required for safe restart/retry
 * after the response TTL without treating expiry as fresh authorization. */
bool ValidateDurableAlternativeRecoveryResultAgainstChainstate(
    const AlternativeRecoveryResultMessage& message,
    const AlternativeRecoveryResponse& response,
    const XOnlyPubKey& provider_identity_key,
    uint64_t minimum_sequence,
    const AlternativeRecoveryTemplate& trusted,
    const AlternativeRecoveryParameters& parameters,
    const CChainParams& chain_params,
    const ChainstateManager& chainman,
    int64_t authorization_time,
    int64_t observation_time,
    bool exact_final_already_known,
    std::string& error);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_VALIDATION_H
