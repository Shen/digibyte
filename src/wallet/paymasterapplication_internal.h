// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_PAYMASTERAPPLICATION_INTERNAL_H
#define DIGIBYTE_WALLET_PAYMASTERAPPLICATION_INTERNAL_H
#include <wallet/paymasterapplication.h>
#include <wallet/paymasterresults.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/walletdb.h>
#include <paymaster/validation.h>
#include <interfaces/chain.h>
#include <streams.h>
#include <version.h>
#include <logging.h>
#include <optional>
#include <string_view>
class CConnman;
namespace wallet::paymaster_app {
struct ClientPaymentOptions;
/** Resolve only the captured locator tip while it remains on the active chain.
 * Callers compare a fresh tip hash after inspecting wallet/index snapshots. */
std::optional<interfaces::BlockKey> ResolvePaymasterActiveTip(
    interfaces::Chain& chain, const CBlockLocator& locator);
SessionSnapshot CaptureSessionSnapshot(const DigiDollar::Paymaster::PaymentSession& session, const PaymasterStore* store = nullptr);
PaymentResult CaptureSessionPayment(const DigiDollar::Paymaster::PaymentSession& session, const PaymasterStore* store = nullptr);
/** Validate persisted client display terms without renewing expired authority. */
bool LoadClientIntentDisplayBinding(
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& recipient_address, std::string& error);

template <typename T>
bool DeserializePaymasterBytes(const std::vector<unsigned char>& bytes, const char* field, T& decoded, std::string& error)
{
    try {
        CDataStream stream{bytes, SER_NETWORK, ::PROTOCOL_VERSION};
        stream >> decoded;
        if (!stream.empty()) throw std::ios_base::failure("trailing paymaster data");
    } catch (const std::ios_base::failure&) {
        error = strprintf("PAYMASTER_INVALID_%s_ENCODING", field);
        return false;
    }
    error.clear();
    return true;
}

uint256 CanonicalClientRequestHash(
    const std::string& request_id, const CScript& recipient_script,
    DigiDollar::Paymaster::DDCents order_amount, DigiDollar::Paymaster::DDCents fee_cap, const std::vector<COutPoint>& inputs,
    DigiDollar::Paymaster::FeeMode mode, DigiDollar::Paymaster::PrivacyProfile privacy, DigiDollar::Paymaster::SelectionMode selection,
    int maximum_attempts, bool subtract_fee, bool send_all,
    const std::optional<XOnlyPubKey>& restricted_key,
    const std::optional<DigiDollar::Paymaster::RestrictedServiceDescriptor>& descriptor,
    const std::optional<DigiDollar::Paymaster::SponsorshipCapability>& capability);
/** Reconstruct retry identity without renewing historical sponsorship authority. */
uint256 CanonicalClientRequestHash(
    const ClientPaymentOptions& input, DigiDollar::Paymaster::DDCents order_amount,
    const std::vector<COutPoint>& inputs);
DigiDollar::Paymaster::DDCents PreviewMaximumServiceFee(CWallet& wallet, DigiDollar::Paymaster::DDCents requested_limit,
                                                      int64_t now, std::string& error);
void RecordUnavailableClientProvider(PaymasterStore& store, const DigiDollar::Paymaster::PaymentSession& session,
                                     const DigiDollar::Paymaster::ProviderAttempt& attempt, int64_t now);
using FinalTransactionPresence = ExactFinalTransactionPresence;
TransportSnapshot CapturePaymasterTransport(const CConnman& connman,
    std::optional<DigiDollar::Paymaster::DirectState> state = std::nullopt);
class ProviderInboundMessageGuard final
{
public:
    ProviderInboundMessageGuard(
        DigiDollar::Paymaster::Manager& manager,
        const DigiDollar::Paymaster::DirectMessage& message) noexcept
        : m_manager{manager},
          m_message_id{message.message_id},
          m_uncaught_exceptions{std::uncaught_exceptions()}
    {
    }

    ProviderInboundMessageGuard(const ProviderInboundMessageGuard&) = delete;
    ProviderInboundMessageGuard& operator=(
        const ProviderInboundMessageGuard&) = delete;

    // A temporary admission limit leaves this exact request in the fair inbox.
    // No response, reservation release, or extension of its expiry is implied.
    void Defer() noexcept { m_deferred = true; }

    ~ProviderInboundMessageGuard()
    {
        if (m_deferred || std::uncaught_exceptions() != m_uncaught_exceptions) return;
        if (!m_manager.AcknowledgeDirectMessagesIfPresent({m_message_id})) {
            LogPrintf("Paymaster provider inbox acknowledgement failed\n");
        }
    }

private:
    DigiDollar::Paymaster::Manager& m_manager;
    const uint256 m_message_id;
    const int m_uncaught_exceptions;
    bool m_deferred{false};
};

struct RecoveryQueueState {
    bool queued{false};
    bool connection_pending{false};
    bool route_available{false};
};

struct PaymasterSubmitQueueState {
    bool queued{false};
    bool connection_pending{false};
    bool route_available{false};
};

struct ProviderCommitRecoveryResult {
    bool broadcast{false};
    bool already_confirmed{false};
    DigiDollar::Paymaster::PaymasterResult provider_result;
    std::string error;
};

DigiDollar::Paymaster::DirectKey PaymentChannelKey(
    const CWallet& wallet, const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::PaymasterId& provider);
const char* PaymasterTransportError(DigiDollar::Paymaster::DirectState state);
std::optional<std::string> DigiDollarAddressForScript(const CScript& script);
std::vector<unsigned char> SerializeQuoteRequest(
    const DigiDollar::Paymaster::PaymasterQuoteRequest& quote_request);
std::vector<unsigned char> SerializeCapacityRequest(
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request);
std::vector<unsigned char> SerializeCapacityProof(
    const DigiDollar::Paymaster::PaymasterCapacityProof& proof);
std::vector<DigiDollar::Paymaster::DirectMessage> SelectCapacityProofMessages(
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce);

std::vector<unsigned char> SerializeRedactedQuoteRequest(
    DigiDollar::Paymaster::PaymasterQuoteRequest quote_request);
std::vector<unsigned char> SerializeQuoteResponse(
    const DigiDollar::Paymaster::PaymasterQuoteResponse& quote_response);
bool AcknowledgeEquivocationMessages(
    DigiDollar::Paymaster::Manager& manager,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    std::string& error);
bool AcknowledgeRejectedDirectMessage(
    DigiDollar::Paymaster::Manager& manager,
    const DigiDollar::Paymaster::DirectMessage& message,
    std::string& error);

// Drain conflicting signed messages into durable evidence before acknowledging
// their inbox leases, so a crash cannot erase an observed equivocation.
bool IsPermanentCapacityContinuationError(const std::string& error);
bool PersistPendingCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error);
bool PersistPendingQuoteEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& quotes,
    bool& found,
    std::string& error);
bool PersistPendingAlternativeRecoveryCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& recovery_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error);
bool DrainClientAttemptEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error,
    EquivocationBlockPolicy block_policy = EquivocationBlockPolicy::ENFORCE,
    bool lease_messages = true);
bool DrainClientSessionEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    std::string& error,
    EquivocationBlockPolicy active_block_policy =
        EquivocationBlockPolicy::OBSERVE_ONLY);
bool DrainAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    std::string& error,
    EquivocationBlockPolicy block_policy = EquivocationBlockPolicy::ENFORCE,
    bool lease_messages = true);
bool DrainPendingAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const std::vector<unsigned char>& first_capacity_proof,
    std::string& error);

// Alternative-recovery messages are persisted and retried byte-for-byte;
// decoding therefore rejects both trailing bytes and non-canonical encodings.
template <typename T>
std::vector<unsigned char> SerializeRecoveryMessage(const T& message)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << message;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename T>
bool DeserializeCanonicalRecoveryMessage(
    const std::vector<unsigned char>& encoded,
    T& message,
    std::string& error)
{
    try {
        SpanReader stream{::PROTOCOL_VERSION, encoded};
        stream >> message;
        if (!stream.empty() || SerializeRecoveryMessage(message) != encoded) {
            throw std::ios_base::failure("non-canonical recovery message");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PERSISTED_RECOVERY_CORRUPT";
        return false;
    }
    error.clear();
    return true;
}

template <typename T>
bool QueueRecoveryMessage(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const T& message,
    int64_t now,
    RecoveryQueueState& state,
    std::string& error);

bool BuildRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error);
bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);
bool HasDurableClientAuthorization(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t observation_time);
/** Observe an already authorized exact payment; never sign, submit, or broadcast.
 * Returns true on completion and copies the full records before pruning can run.
 * Throws an ApplicationError on failed validation or persistence. */
bool ReconcileConfirmedClientPayment(
    CWallet& wallet, DigiDollar::Paymaster::PaymentSession& session,
    DigiDollar::Paymaster::ProviderAttempt& attempt);
bool QueuePersistedPaymasterSubmit(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    PaymasterSubmitQueueState& state,
    std::string& error);

// Durable authorization reconstruction and the final pre-broadcast firewall.
// These helpers must reload persisted artifacts instead of trusting earlier
// RPC-local objects across a database commit or wallet-signing boundary.
std::vector<unsigned char> SerializePaymentIntent(
    const DigiDollar::Paymaster::PaymentIntent& intent);
bool LoadTrustedTemplate(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);
bool ValidateDurableProviderUserAuthorization(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error);
bool FindDurableClientAuthorizationAttempt(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    int64_t observation_time,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    std::string& error);
PaymentResult CaptureDurableClientAuthorization(
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote);
bool GetValidatedClientAuthorizationCommitment(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    uint256& commitment,
    std::string& error);
std::vector<unsigned char> SerializePSBT(
    const PartiallySignedTransaction& psbt);
std::vector<unsigned char> SerializeTransaction(
    const CMutableTransaction& transaction);
bool PreflightFinalPaymasterTransaction(
    CWallet& wallet,
    const CTransactionRef& transaction,
    FinalTransactionPresence& presence,
    std::string& error,
    ExactFinalTxIndexMode txindex_mode = ExactFinalTxIndexMode::WAIT_FOR_SYNC,
    std::optional<DigiDollar::Paymaster::ExactFinalTransactionProof>* final_proof = nullptr);
bool MarkFinalValidationFailureRecoverable(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    std::string& error);
std::vector<COutPoint> ProviderInputs(
    const CMutableTransaction& transaction,
    const std::vector<DigiDollar::Paymaster::ReservationRole>& roles);
bool TemplateInputsAvailable(CWallet& wallet,
                             const CMutableTransaction& transaction);
bool RejectUnavailableTemplateInputs(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    const CMutableTransaction& transaction,
    int64_t now,
    bool& rejected,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error);
bool BuildFinalProviderResult(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error);
bool InsertAndBroadcastPaymasterTransaction(CWallet& wallet,
                                            const CTransactionRef& transaction,
                                            bool& already_confirmed,
                                            std::string& error);
bool InsertAndBroadcastProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    bool& already_confirmed,
    std::string& error,
    CTransactionRef* exact_transaction = nullptr);
ProviderCommitRecoveryResult RecoverProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now);

} // namespace wallet::paymaster_app
#endif
