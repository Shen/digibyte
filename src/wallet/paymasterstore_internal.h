// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Internal helpers shared by the domain-specific PaymasterStore units. */

#ifndef DIGIBYTE_WALLET_PAYMASTERSTORE_INTERNAL_H
#define DIGIBYTE_WALLET_PAYMASTERSTORE_INTERNAL_H

#include <paymaster/psbt.h>
#include <streams.h>
#include <version.h>
#include <wallet/paymasterstore.h>
#include <wallet/walletdb.h>

#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace wallet::paymaster_store::internal {
using namespace DigiDollar::Paymaster;

inline constexpr size_t MAX_RECOVERY_ENDPOINT_BYTES{512};
inline constexpr int64_t CAPACITY_REPLAY_RETENTION_SECONDS{24 * 60 * 60};

std::string PersistedVersionError(std::string_view record_type,
                                  uint16_t found,
                                  uint16_t expected,
                                  std::string_view invalid_error);

template <typename T>
std::string PersistedReadError(DatabaseReadStatus status,
                               std::string_view record_type,
                               const T& record,
                               std::string_view missing_error,
                               std::string_view invalid_error)
{
    if (status == DatabaseReadStatus::NOT_FOUND) {
        return std::string{missing_error};
    }
    if (status == DatabaseReadStatus::UNSUPPORTED_VERSION) {
        return PersistedVersionError(record_type, record.version,
                                     T::CURRENT_VERSION, invalid_error);
    }
    return std::string{invalid_error};
}

std::string ProviderPoolReadError(const std::vector<ProviderPoolEntry>& entries);
bool Abort(WalletBatch& batch, std::string& error, const char* code);
bool RegisterProviderPoolSuccessors(const ProviderAttempt& attempt,
                                    const ProviderCommitRecord& commit,
                                    const CTransaction& transaction,
                                    const std::optional<ProviderPolicy>& policy,
                                    std::vector<ProviderPoolEntry>& entries,
                                    bool& changed,
                                    std::string& error);
bool PrepareProviderTransferFinanceEvent(WalletBatch& batch,
                                         const ProviderIdentityRecord& identity,
                                         const ProviderAttempt& attempt,
                                         const ProviderCommitRecord& commit,
                                         const uint256& expected_genesis,
                                         ProviderFinanceLedger& ledger,
                                         bool& changed,
                                         std::string& error);
bool HasAuthorizationRisk(SessionState state);
bool AttemptHasReached(AttemptState state, AttemptState threshold);
bool HasTimelyDurableProviderSignature(const ProviderAttempt& attempt);
bool ValidateProviderBudgetState(const ProviderAttempt& attempt,
                                 const ProviderSafetyPolicy& policy,
                                 const ProviderBudgetLedger& ledger,
                                 BudgetReservationState expected_state,
                                 bool allow_historical_policy,
                                 std::string& error);
bool ValidateProviderAlternativeRecoveryBudgetAuthorizationImpl(
    const AlternativeRecoveryRecord& recovery,
    const ProviderSafetyPolicy* policy,
    const ProviderBudgetLedger& ledger,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error);
bool SameCommit(const ProviderCommitRecord& lhs, const ProviderCommitRecord& rhs);

template <typename T>
std::vector<unsigned char> CanonicalBytes(const T& value)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << value;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool DecodeProviderSignedResultEnvelope(const ProviderAttempt& attempt,
                                        PaymasterResult& result,
                                        std::string& error);
bool SameAlternativeRecoveryRecord(const AlternativeRecoveryRecord& lhs,
                                   const AlternativeRecoveryRecord& rhs);
bool SameAlternativeRecoveryProgression(const AlternativeRecoveryRecord& current,
                                        const AlternativeRecoveryRecord& next);
bool ValidateAlternativeRecoveryRecordShape(const AlternativeRecoveryRecord& recovery,
                                            std::string& error);
bool ValidateProviderAlternativeRecoveryCommitBinding(
    const AlternativeRecoveryRecord& recovery,
    const ProviderCommitRecord& commit,
    CMutableTransaction& transaction,
    std::vector<COutPoint>& provider_inputs,
    std::string& error);

struct ExactFinalArtifact {
    std::vector<unsigned char> bytes;
    CMutableTransaction transaction;
    uint256 txid;
    uint256 wtxid;
};

bool DecodeExactFinalArtifact(const std::vector<unsigned char>& bytes,
                              const uint256& expected_txid,
                              ExactFinalArtifact& artifact,
                              std::string& error);
bool LoadPaymentFinalArtifact(WalletBatch& batch,
                              const PaymentSession& session,
                              ExactFinalArtifact& artifact,
                              std::optional<ProviderAttempt>& observed_attempt,
                              std::string& error);
bool LoadRecoveryFinalArtifact(WalletBatch& batch,
                               const PaymentSession& session,
                               ExactFinalArtifact& artifact,
                               std::string& error);

struct ExactFinalObservation {
    int confirmation_depth{0};
    bool in_mempool{false};
};

bool ObserveExactWalletArtifact(const CWallet& wallet,
                                const ExactFinalArtifact& artifact,
                                ExactFinalObservation& observation,
                                std::string& error);
bool BindExactObservation(const CTransaction& transaction,
                          const ExactFinalArtifact& artifact,
                          int confirmation_depth,
                          bool in_mempool,
                          ExactFinalObservation& observation,
                          std::string& error);
bool SameCapacitySnapshot(const ValidatedCapacitySnapshot& lhs,
                          const ValidatedCapacitySnapshot& rhs);
bool StagePendingEquivocationLocked(WalletBatch& batch,
                                    const PaymasterEquivocationEvidence& evidence,
                                    std::string& error);
bool PromotePendingEquivocationLocked(WalletBatch& batch,
                                      const PaymasterId& provider_id,
                                      std::string& error);
bool DecodeCanonicalCapacityProof(const std::vector<unsigned char>& bytes,
                                  PaymasterCapacityProof& proof);
uint256 GetPersistedCapacityRequestHash(const PaymasterCapacityProof& proof);
uint256 GetPersistedCapacitySessionKey(const PaymasterCapacityProof& proof);
bool ReadProviderCapacityRequestForContinuation(
    WalletBatch& batch,
    const ProviderIdentityRecord& identity,
    const uint256& expected_genesis,
    const PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce,
    int64_t now,
    PaymasterCapacityRequest& request,
    PaymasterCapacityProof& proof,
    uint256& request_hash,
    std::string& error);
bool CapacityProofMatchesPoolEntry(const CapacityDGBInput& input,
                                   const ProviderPoolEntry& entry);
bool CapacityProofMatchesPoolEntry(const CapacityDDCarrier& carrier,
                                   const ProviderPoolEntry& entry);
bool ValidateProviderCapacityAdmissionForCommit(
    const ProviderBudgetLedger& ledger,
    const PaymasterCapacityRequest& request,
    const uint256& request_hash,
    const uint256& quote_request_hash,
    const uint256& commit_key,
    const uint256& netgroup_bucket,
    CapacityAdmissionState expected_state,
    int64_t effective_now,
    std::string& error);
bool ValidateQuoteCapacityResources(const PaymasterCapacityProof& proof,
                                    const PaymasterQuote& quote,
                                    const ProviderAuthorizationManifest& manifest,
                                    const std::vector<ProviderPoolEntry>& pool,
                                    const uint256& client_nonce,
                                    const uint256& active_reservation_id,
                                    std::string& error);
bool ValidateRecoveryCapacityResources(
    const AlternativeRecoveryRecord& recovery,
    const PaymasterCapacityRequest& capacity_request,
    const PaymasterCapacityProof& proof,
    const std::vector<unsigned char>& persisted_proof,
    const std::vector<ProviderPoolEntry>& pool,
    const uint256& active_reservation_id,
    std::string& error);
bool ValidatePersistedCapacityClaimCandidate(
    const std::vector<unsigned char>& candidate,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& identity_key,
    const char* corruption_error,
    std::string& error);
bool VerifyCapacityEvidenceArtifact(const std::vector<unsigned char>& bytes,
                                    const PaymasterCapacityRequest& request,
                                    const XOnlyPubKey& identity_key,
                                    PaymasterCapacityProof& proof);
bool CapacityProofContainsOutpoint(const PaymasterCapacityProof& proof,
                                   const COutPoint& outpoint);
bool RecordCapacityEquivocationLocked(
    WalletBatch& batch,
    const PaymasterCapacityRequest& request,
    const XOnlyPubKey& identity_key,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& second_capacity_proof,
    const uint256& semantic_key,
    int64_t now,
    std::string& error);
bool RecordCapacityResourceEquivocationLocked(
    WalletBatch& batch,
    const PaymasterId& provider_id,
    const XOnlyPubKey& identity_key,
    const COutPoint& outpoint,
    const std::vector<unsigned char>& first_capacity_proof,
    const std::vector<unsigned char>& second_capacity_proof,
    int64_t now,
    std::string& error);
bool SameCapacityResourceBinding(const CapacityResourceBinding& lhs,
                                 const CapacityResourceBinding& rhs);
bool BuildCapacityResourceBindings(const ValidatedCapacitySnapshot& snapshot,
                                   const PaymasterCapacityProof& proof,
                                   std::vector<CapacityResourceBinding>& bindings,
                                   std::string& error);
bool DecodeCanonicalQuoteRequest(const std::vector<unsigned char>& bytes,
                                 PaymasterQuoteRequest& request);
bool DecodeCanonicalQuoteResponse(const std::vector<unsigned char>& bytes,
                                  PaymasterQuoteResponse& response);
bool ValidatePersistedCapacityRequestAuthority(const PaymentSession& session,
                                               const ProviderAttempt& attempt,
                                               PaymasterCapacityRequest& request,
                                               std::string& error);
bool ValidatePersistedRecoveryCapacityRequestAuthority(
    const AlternativeRecoveryRecord& recovery,
    std::string& error);
bool ValidatePersistedQuoteRequestAuthority(const PaymentSession& session,
                                            const ProviderAttempt& attempt,
                                            PaymasterQuoteRequest& request,
                                            std::string& error);
bool ValidatePersistedQuoteClaimCandidate(
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    const PaymasterQuoteRequest& authoritative_request,
    std::string& error);
bool RecordQuoteEquivocationPairLocked(
    WalletBatch& batch,
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    const std::vector<unsigned char>& first_signed_quote,
    const std::vector<unsigned char>& second_signed_quote,
    int64_t now,
    std::string& error);
bool RecordQuoteEquivocationLocked(
    WalletBatch& batch,
    const PaymentSession& session,
    const ProviderAttempt& attempt,
    const std::vector<unsigned char>& conflicting_signed_quote,
    int64_t now,
    std::string& error);
bool IsFinalResultStatus(PaymasterResultStatus status);
bool IsNegativeTerminalResultStatus(PaymasterResultStatus status);
std::vector<unsigned char> SerializeResultTransaction(
    const CMutableTransaction& transaction);
bool SameResult(const PaymasterResult& lhs, const PaymasterResult& rhs);
bool ValidateResultProgression(const PaymasterResult& previous,
                               const PaymasterResult& next,
                               bool allow_negative_to_final,
                               std::string& error);
bool ValidateQuotedTemplate(const ProviderAttempt& attempt, std::string& error);
bool LoadAttemptAuthorizationArtifacts(const ProviderAttempt& attempt,
                                       PaymasterQuoteRequest& request,
                                       PaymasterQuoteResponse& response,
                                       CollaborativePSBTTemplate& trusted,
                                       std::string& error);
bool ValidateAttemptAuthorizationManifest(const ProviderAttempt& attempt,
                                          bool provider_side,
                                          std::string& error);
bool DecodeUnsignedIntent(const std::vector<unsigned char>& bytes,
                          PaymentIntent& intent,
                          int64_t now,
                          std::string& error);

} // namespace wallet::paymaster_store::internal

#endif // DIGIBYTE_WALLET_PAYMASTERSTORE_INTERNAL_H
