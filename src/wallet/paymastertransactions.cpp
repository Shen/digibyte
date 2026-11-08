// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>
#include <wallet/paymasterapplication.h>

#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <logging/timer.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/setup.h>
#include <paymaster/validation.h>
#include <random.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/spend.h>

#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>


namespace wallet::paymaster_app {
TransportSnapshot CapturePaymasterTransport(const CConnman& connman,
    std::optional<DigiDollar::Paymaster::DirectState> state)
{
    return {state, connman.GetPaymasterBudget(), connman.GetPaymasterUsage(),
            static_cast<uint64_t>(connman.GetPaymasterQueueSize()), connman.PaymasterListenerReady()};
}
using namespace DigiDollar::Paymaster;
SessionSnapshot CaptureSessionSnapshot(const PaymentSession& session, const PaymasterStore* store)
{
    SessionSnapshot result;
    result.session = session;
    if (store) result.resume_order = LoadClientPaymentOrder(*store, session, result.resume_order_error);
    result.observation_requested = store != nullptr;
    auto& observation = result.observation;
    if (store && !store->GetSessionObservation(session, observation, result.observation_error)) {
        const auto tip_hash = observation.tip_hash;
        const auto tip_height = observation.tip_height;
        observation = {};
        observation.tip_hash = tip_hash;
        observation.tip_height = tip_height;
        observation.payment.txid = session.final_txid;
        observation.recovery.txid = session.recovery_txid;
    }
    return result;
}

PaymentResult CaptureSessionPayment(const PaymentSession& session, const PaymasterStore* store)
{
    PaymentResult result;
    result.session = CaptureSessionSnapshot(session, store);
    result.request_id = session.request_id;
    result.session_id = session.session_id;
    result.requested_fee_mode = session.fee_mode_requested;
    result.fee_mode_used = session.fee_mode_used;
    result.session_state = session.state;
    if (result.session->resume_order) {
        const auto& order = *result.session->resume_order;
        CTxDestination recipient;
        if (ExtractDestination(order.recipient_script, recipient)) result.to_address = EncodeDigiDollarAddress(recipient);
        result.privacy_profile = order.privacy;
    }
    if (session.requested_amount.value > 0) {
        result.requested_amount_cents = session.requested_amount;
        result.subtract_paymaster_fee_from_amount = session.subtract_paymaster_fee_from_amount;
        result.send_all_spendable_dd = session.send_all_spendable_dd;
    }
    if (!session.final_txid.IsNull()) result.txid = session.final_txid;
    // The session owns the provider-independent gross order while its latest
    // client authorization manifest owns the exact provider-dependent split.
    // An unreadable or invalid current attempt is unknown, never permission to
    // display an older provider's terms alongside the current session/txid.
    if (store && !session.provider_side && !session.attempt_ids.empty()) {
        const uint256& attempt_id = session.attempt_ids.back();
        ProviderAttempt attempt;
        if (!store->GetAttempt(attempt_id, attempt) ||
            attempt.attempt_id != attempt_id ||
            attempt.session_id != session.session_id) {
            throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_SESSION_ATTEMPT_DATABASE_READ");
        }
        std::string recipient_address;
        std::string error;
        if (!LoadClientIntentDisplayBinding(session, attempt,
                                            recipient_address, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        const ClientAuthorizationManifest& manifest = attempt.client_manifest;
        if (!manifest.manifest_id.IsNull()) {
            result.provider_id = manifest.provider_id;
            result.privacy_profile = attempt.privacy_profile;
            result.offer_id = manifest.offer_id;
            result.policy_hash = manifest.policy_hash;
            result.funding_model = manifest.funding_model;
            result.payment_cents = manifest.recipient_amount;
            result.service_fee_cents = manifest.service_fee;
            result.user_total_cents = DDCents{manifest.recipient_amount.value +
                    manifest.service_fee.value};
            result.to_address = std::move(recipient_address);
        }
    }
    return result;
}

void FillMissingPaymentTerms(PaymentResult& target, const PaymentResult& source)
{
    if (!target.requested_fee_mode) target.requested_fee_mode = source.requested_fee_mode;
    if (!target.fee_mode_used) target.fee_mode_used = source.fee_mode_used;
    if (!target.provider_id) target.provider_id = source.provider_id;
    if (!target.offer_id) target.offer_id = source.offer_id;
    if (!target.policy_hash) target.policy_hash = source.policy_hash;
    if (!target.funding_model) target.funding_model = source.funding_model;
    if (!target.payment_cents) target.payment_cents = source.payment_cents;
    if (!target.service_fee_cents) target.service_fee_cents = source.service_fee_cents;
    if (!target.user_total_cents) target.user_total_cents = source.user_total_cents;
    if (!target.requested_amount_cents) target.requested_amount_cents = source.requested_amount_cents;
    if (!target.subtract_paymaster_fee_from_amount) target.subtract_paymaster_fee_from_amount = source.subtract_paymaster_fee_from_amount;
    if (!target.send_all_spendable_dd) target.send_all_spendable_dd = source.send_all_spendable_dd;
    if (!target.authorization_commitment) target.authorization_commitment = source.authorization_commitment;
}

DigiDollar::Paymaster::DirectKey PaymentChannelKey(
    const CWallet& wallet, const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::PaymasterId& provider)
{
    return {wallet.m_paymaster_transport_owner.GetHex(),
            session.session_id.GetHex() + ":" + provider.GetHex(), false, provider};
}

const char* PaymasterTransportError(DigiDollar::Paymaster::DirectState state)
{
    using DigiDollar::Paymaster::DirectState;
    switch (state) {
    case DirectState::DISABLED: return "PAYMASTER_NO_LOCAL_DIRECT_CAPACITY";
    case DirectState::FULL: return "PAYMASTER_DIRECT_QUEUE_FULL";
    case DirectState::EXPIRED: return "PAYMASTER_DIRECT_WAIT_EXPIRED";
    case DirectState::CANCELED: return "PAYMASTER_DIRECT_CANCELED";
    case DirectState::NETWORK_INACTIVE: return "PAYMASTER_NETWORK_INACTIVE";
    case DirectState::PRIVACY_REJECTED: return "PAYMASTER_DIRECT_PRIVACY_REJECTED";
    case DirectState::CONNECT_FAILED: return "PAYMASTER_PROXY_OR_ENDPOINT_UNREACHABLE";
    default: return "PAYMASTER_DIRECT_CONNECTION_FAILED";
    }
}

std::optional<std::string> DigiDollarAddressForScript(const CScript& script)
{
    CTxDestination destination;
    if (!ExtractDestination(script, destination)) return std::nullopt;
    const std::string address = EncodeDigiDollarAddress(destination);
    const CDigiDollarAddress decoded{address};
    if (address.empty() || !decoded.IsValid()) return std::nullopt;
    return address;
}

bool ReconcileConfirmedClientPayment(
    CWallet& wallet, PaymentSession& session, ProviderAttempt& attempt)
{
    if (session.provider_side || session.attempt_ids.empty() ||
        session.attempt_ids.back() != attempt.attempt_id ||
        attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
        !session.final_txid.IsNull()) return false;
    PaymasterStore store{wallet};
    // A spend with no wallet change output may not have been
    // inserted in mapWallet before the provider result arrived.
    // Recover only a fully validated, deeply confirmed exact
    // transaction from the local index, without broadcasting it.
    CTransactionRef observed_transaction;
    ExactFinalTransactionPreflight observed;
    bool missing_wallet_transaction{false};
    {
        LOCK(wallet.cs_wallet);
        missing_wallet_transaction = !wallet.GetWalletTx(attempt.unsigned_txid);
    }
    if (missing_wallet_transaction) {
        const auto* node = wallet.chain().context();
        if (node && node->chainman) {
            uint256 block_hash;
            observed_transaction = node::GetTransaction(nullptr, node->mempool.get(),
                attempt.unsigned_txid, block_hash, node->chainman->m_blockman);
            std::string observation_error;
            if (observed_transaction &&
                PreflightExactPaymasterFinalTransaction(wallet, observed_transaction,
                    ExactFinalTxIndexMode::NONBLOCKING, observed, observation_error) &&
                observed.presence == ExactFinalTransactionPresence::CONFIRMED &&
                observed.confirmation_depth >= DEFAULT_REORG_SAFETY_DEPTH) {
                auto candidate = attempt;
                candidate.final_txid = observed_transaction->GetHash();
                CDataStream bytes{SER_NETWORK, ::PROTOCOL_VERSION};
                bytes << *observed_transaction;
                const auto span = MakeUCharSpan(bytes);
                candidate.final_transaction.assign(span.begin(), span.end());
                candidate.state = AttemptState::MEMPOOL;
                CMutableTransaction validated;
                if (!ValidateClientFinalForExecution(candidate, observed_transaction->GetWitnessHash(),
                        validated, observation_error) ||
                    !ValidateClientAuthorizationOwnership(wallet, candidate.client_manifest, observation_error))
                    throw ApplicationError(ErrorKind::WALLET, observation_error);
            } else {
                observed_transaction.reset();
            }
        }
    }
    {
        // Copy the completed full records before tip pruning can run.
        LOCK(wallet.cs_wallet);
        if (observed_transaction && !wallet.GetWalletTx(attempt.unsigned_txid) &&
            !wallet.AddToWallet(observed_transaction,
                TxStateConfirmed{observed.confirmed_block, observed.confirmed_height, observed.confirmed_position}))
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_OBSERVED_TRANSACTION_WRITE");
        bool completed{false};
        std::string observation_error;
        if (!store.CompleteClientConfirmedPayment(session.request_id,
                attempt.attempt_id, GetTime(), completed, observation_error))
            throw ApplicationError(ErrorKind::WALLET, observation_error);
        if (completed) {
            if (!store.GetSessionByRequestId(session.request_id, session) ||
                !store.GetAttempt(attempt.attempt_id, attempt))
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DATABASE_READ");
            if (auto* dd_wallet = wallet.GetDDWallet()) {
                std::string history_error;
                if (!dd_wallet->RecordPaymasterSendHistory(session.final_txid,
                        attempt.client_manifest.recipient_script,
                        attempt.client_manifest.recipient_amount.value + attempt.client_manifest.service_fee.value,
                        history_error))
                    wallet.WalletLogPrintf("Paymaster observed payment history update deferred\n");
            }
            if (const auto* node = wallet.chain().context(); node && node->connman)
                node->connman->ReleasePaymasterConnection(PaymentChannelKey(wallet, session, attempt.provider_id));
            return true;
        }
    }
    return false;
}
std::vector<unsigned char> SerializeQuoteRequest(
    const DigiDollar::Paymaster::PaymasterQuoteRequest& quote_request)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << quote_request;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeCapacityRequest(
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << request;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeCapacityProof(
    const DigiDollar::Paymaster::PaymasterCapacityProof& proof)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << proof;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<DigiDollar::Paymaster::DirectMessage>
SelectCapacityProofMessages(
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    const std::string& request_id,
    const uint256& session_id,
    const uint256& client_nonce)
{
    using namespace DigiDollar::Paymaster;
    std::vector<DirectMessage> selected;
    selected.reserve(messages.size());
    for (const DirectMessage& message : messages) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (proof && proof->provider_id == provider_id &&
            proof->request_id == request_id &&
            proof->session_id == session_id &&
            proof->client_nonce == client_nonce) {
            selected.push_back(message);
        }
    }
    return selected;
}

std::vector<unsigned char> SerializeRedactedQuoteRequest(
    DigiDollar::Paymaster::PaymasterQuoteRequest quote_request)
{
    quote_request.restricted_descriptor.reset();
    quote_request.restricted_capability.reset();
    return SerializeQuoteRequest(quote_request);
}

std::vector<unsigned char> SerializeQuoteResponse(
    const DigiDollar::Paymaster::PaymasterQuoteResponse& quote_response)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << quote_response;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

// Direct-message acknowledgement happens only after durable evidence or a
// durable rejection has been written. Removing queue entries first would lose
// the only proof of a conflicting response on crash.
bool AcknowledgeEquivocationMessages(
    DigiDollar::Paymaster::Manager& manager,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& messages,
    std::string& error)
{
    if (messages.empty()) return true;
    std::vector<uint256> message_ids;
    message_ids.reserve(messages.size());
    for (const auto& message : messages) {
        message_ids.push_back(message.message_id);
    }
    if (!manager.AcknowledgeDirectMessagesIfPresent(message_ids)) {
        error = "PAYMASTER_EQUIVOCATION_INBOX_ACK_CONFLICT";
        return false;
    }
    return true;
}

bool AcknowledgeRejectedDirectMessage(
    DigiDollar::Paymaster::Manager& manager,
    const DigiDollar::Paymaster::DirectMessage& message,
    std::string& error)
{
    if (!manager.AcknowledgeDirectMessagesIfPresent({message.message_id})) {
        error = "PAYMASTER_DIRECT_INBOX_ACK_CONFLICT";
        return false;
    }
    return true;
}

/** Return whether an immutable quote continuation can never become valid on a
 * later scheduler pass. Capacity is durably reserved before its proof is sent,
 * so a quote that cannot reload that exact reservation is not a transient
 * liquidity or synchronization failure. Keeping such a message leased would
 * let one stale or malicious peer repeatedly fault an otherwise healthy
 * automatic provider service.
 */
bool IsPermanentCapacityContinuationError(const std::string& error)
{
    return error == "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH" ||
           error == "PAYMASTER_CAPACITY_RESERVATION_MISSING" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_MISSING" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_EXPIRED" ||
           error == "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
}

/** Keep one provider-addressed inbound message leased until the durable
 * handler finishes. A normal return acknowledges the message; unwinding from
 * a local failure releases only the lease so the scheduler can retry it. The
 * caller must explicitly acknowledge permanently invalid messages before
 * throwing.
 */
bool PersistPendingCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < proofs.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterCapacityProof>(&proofs[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < proofs.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterCapacityProof>(
                &proofs[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingCapacityEquivocation(
                    attempt_id, SerializeCapacityProof(*first),
                    SerializeCapacityProof(*second),
                    std::max(proofs[first_index].received_at,
                             proofs[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

bool PersistPendingQuoteEquivocationPair(
    PaymasterStore& store,
    const uint256& attempt_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& quotes,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < quotes.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterQuoteResponse>(&quotes[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < quotes.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterQuoteResponse>(
                &quotes[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingQuoteEquivocation(
                    attempt_id, SerializeQuoteResponse(*first),
                    SerializeQuoteResponse(*second),
                    std::max(quotes[first_index].received_at,
                             quotes[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_QUOTE_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

bool PersistPendingAlternativeRecoveryCapacityEquivocationPair(
    PaymasterStore& store,
    const uint256& recovery_id,
    const std::vector<DigiDollar::Paymaster::DirectMessage>& proofs,
    bool& found,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    found = false;
    error.clear();
    for (size_t first_index = 0; first_index < proofs.size(); ++first_index) {
        const auto* first =
            std::get_if<PaymasterCapacityProof>(&proofs[first_index].payload);
        if (!first) continue;
        for (size_t second_index = first_index + 1;
             second_index < proofs.size(); ++second_index) {
            const auto* second = std::get_if<PaymasterCapacityProof>(
                &proofs[second_index].payload);
            if (!second) continue;
            std::string evidence_error;
            if (store.RecordPendingAlternativeRecoveryCapacityEquivocation(
                    recovery_id, SerializeCapacityProof(*first),
                    SerializeCapacityProof(*second),
                    std::max(proofs[first_index].received_at,
                             proofs[second_index].received_at),
                    evidence_error)) {
                found = true;
                return true;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
    }
    return true;
}

/** Drain provider-signed conflicts for one exact client attempt. Before the
 * first capacity snapshot or quote is accepted, every bounded pair is checked
 * and the first individually valid signed claim is durably staged. This keeps
 * sequential reconnect/restart conflicts provable even when neither artifact
 * passes the deeper local spend firewall. Staged claims are evidence-only and
 * never become authorization baselines. */
bool DrainClientAttemptEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error,
    EquivocationBlockPolicy block_policy,
    bool lease_messages)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (session.provider_side || attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt.attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(store, attempt.provider_id,
                                              error)) {
        return false;
    }

    std::optional<DirectMessageLease> proof_lease;
    std::vector<DirectMessage> unleased_proofs;
    if (lease_messages) {
        proof_lease.emplace(manager.LeaseCapacityProofs(
            attempt.provider_id, attempt.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_proofs = manager.PeekCapacityProofs(
            attempt.provider_id, attempt.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    }
    const std::vector<DirectMessage>& raw_proofs =
        proof_lease ? proof_lease->Messages() : unleased_proofs;
    const std::vector<DirectMessage> proofs = SelectCapacityProofMessages(
        raw_proofs, attempt.provider_id, session.request_id,
        session.session_id, attempt.client_nonce);
    if (attempt.capacity_snapshot.capacity_proof.empty()) {
        bool capacity_equivocation{false};
        if (!PersistPendingCapacityEquivocationPair(
                store, attempt.attempt_id, proofs, capacity_equivocation,
                error)) {
            return false;
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                return false;
            }
        } else {
            for (const DirectMessage& message : proofs) {
                const auto* proof =
                    std::get_if<PaymasterCapacityProof>(&message.payload);
                if (!proof) continue;
                bool claim_equivocation{false};
                if (!store.StageCapacityProofClaimCandidate(
                        attempt.attempt_id, SerializeCapacityProof(*proof),
                        message.received_at, claim_equivocation, error)) {
                    if (error !=
                        "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                        return false;
                    }
                    if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                          error)) {
                        return false;
                    }
                    continue;
                }
                if (claim_equivocation) {
                    if (!AcknowledgeEquivocationMessages(manager, proofs,
                                                         error)) {
                        return false;
                    }
                    if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                        return false;
                    }
                    break;
                }
                // Maintenance has no in-flight acceptance scope. Once the
                // signed candidate is durable it can consume the inbox copy;
                // active RPCs retain it for Chainstate validation below.
                if (!lease_messages &&
                    !AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
            }
        }
    } else {
        bool capacity_equivocation{false};
        for (const DirectMessage& message : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            if (!proof) continue;
            const std::vector<unsigned char> encoded =
                SerializeCapacityProof(*proof);
            if (encoded == attempt.capacity_snapshot.capacity_proof) continue;

            std::string evidence_error;
            if (store.RecordCapacityEquivocation(
                    attempt.attempt_id, encoded, message.received_at,
                    evidence_error)) {
                capacity_equivocation = true;
                continue;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
        if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
            return false;
        }
        if (capacity_equivocation &&
            block_policy == EquivocationBlockPolicy::ENFORCE) {
            error = "PAYMASTER_CAPACITY_EQUIVOCATION";
            return false;
        }
    }

    std::optional<DirectMessageLease> quote_lease;
    std::vector<DirectMessage> unleased_quotes;
    if (lease_messages) {
        quote_lease.emplace(manager.LeaseQuoteResponses(
            session.request_id, session.session_id, attempt.provider_id,
            attempt.intent_hash, MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_quotes = manager.PeekQuoteResponses(
            session.request_id, session.session_id, attempt.provider_id,
            attempt.intent_hash, MAX_DIRECT_INBOX_MESSAGES_PER_SESSION,
            /*lease=*/false);
    }
    const std::vector<DirectMessage>& quotes =
        quote_lease ? quote_lease->Messages() : unleased_quotes;
    if (attempt.signed_quote.empty()) {
        bool quote_equivocation{false};
        if (!PersistPendingQuoteEquivocationPair(
                store, attempt.attempt_id, quotes, quote_equivocation, error)) {
            return false;
        }
        if (quote_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, quotes, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_QUOTE_EQUIVOCATION";
                return false;
            }
        } else {
            for (const DirectMessage& message : quotes) {
                const auto* response =
                    std::get_if<PaymasterQuoteResponse>(&message.payload);
                if (!response) continue;
                bool claim_equivocation{false};
                if (!store.StageQuoteResponseClaimCandidate(
                        attempt.attempt_id,
                        SerializeQuoteResponse(*response), message.received_at,
                        claim_equivocation, error)) {
                    if (error != "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE") {
                        return false;
                    }
                    if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                          error)) {
                        return false;
                    }
                    continue;
                }
                if (claim_equivocation) {
                    if (!AcknowledgeEquivocationMessages(manager, quotes,
                                                         error)) {
                        return false;
                    }
                    if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                        error = "PAYMASTER_QUOTE_EQUIVOCATION";
                        return false;
                    }
                    break;
                }
                if (!lease_messages &&
                    !AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
            }
        }
    } else {
        bool quote_equivocation{false};
        for (const DirectMessage& message : quotes) {
            const auto* response =
                std::get_if<PaymasterQuoteResponse>(&message.payload);
            if (!response) continue;
            const std::vector<unsigned char> encoded =
                SerializeQuoteResponse(*response);
            if (encoded == attempt.signed_quote) continue;

            std::string evidence_error;
            if (store.RecordQuoteEquivocation(
                    attempt.attempt_id, encoded, message.received_at,
                    evidence_error)) {
                quote_equivocation = true;
                continue;
            }
            if (!evidence_error.empty() &&
                evidence_error != "PAYMASTER_INVALID_QUOTE_EQUIVOCATION") {
                error = std::move(evidence_error);
                return false;
            }
        }
        if (!AcknowledgeEquivocationMessages(manager, quotes, error)) {
            return false;
        }
        if (quote_equivocation &&
            block_policy == EquivocationBlockPolicy::ENFORCE) {
            error = "PAYMASTER_QUOTE_EQUIVOCATION";
            return false;
        }
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(store, attempt.provider_id,
                                              error)) {
        return false;
    }
    return true;
}

bool DrainClientSessionEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    std::string& error,
    EquivocationBlockPolicy active_block_policy)
{
    using namespace DigiDollar::Paymaster;
    if (session.provider_side) return true;
    for (const uint256& attempt_id : session.attempt_ids) {
        ProviderAttempt attempt;
        if (!store.GetAttempt(attempt_id, attempt)) {
            error = "PAYMASTER_ATTEMPT_NOT_FOUND";
            return false;
        }
        const EquivocationBlockPolicy attempt_policy =
            attempt_id == session.attempt_ids.back() ? active_block_policy : EquivocationBlockPolicy::OBSERVE_ONLY;
        if (!DrainClientAttemptEquivocations(
                manager, store, session, attempt, error, attempt_policy)) {
            return false;
        }
    }
    error.clear();
    return true;
}

bool DrainAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    std::string& error,
    EquivocationBlockPolicy block_policy,
    bool lease_messages)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (recovery.provider_side) {
        return true;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    std::optional<DirectMessageLease> proof_lease;
    std::vector<DirectMessage> unleased_proofs;
    if (lease_messages) {
        proof_lease.emplace(manager.LeaseCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION));
    } else {
        unleased_proofs = manager.PeekCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    }
    const std::vector<DirectMessage>& raw_proofs =
        proof_lease ? proof_lease->Messages() : unleased_proofs;
    const std::vector<DirectMessage> proofs = SelectCapacityProofMessages(
        raw_proofs, recovery.recovery_provider_id,
        recovery.capacity_request.request_id,
        recovery.capacity_request.session_id, recovery.client_nonce);
    if (recovery.capacity_snapshot.capacity_proof.empty()) {
        if (recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING) {
            return true;
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
            return false;
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                return false;
            }
            if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                return false;
            }
            return true;
        }
        for (const DirectMessage& message : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&message.payload);
            if (!proof) continue;
            bool claim_equivocation{false};
            if (!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
                    recovery.recovery_id, SerializeCapacityProof(*proof),
                    message.received_at, claim_equivocation, error)) {
                if (error != "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                    return false;
                }
                if (!AcknowledgeRejectedDirectMessage(manager, message,
                                                      error)) {
                    return false;
                }
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
                    return false;
                }
                if (block_policy == EquivocationBlockPolicy::ENFORCE) {
                    error = "PAYMASTER_CAPACITY_EQUIVOCATION";
                    return false;
                }
                return true;
            }
            if (!lease_messages &&
                !AcknowledgeRejectedDirectMessage(manager, message, error)) {
                return false;
            }
        }
        return true;
    }
    bool capacity_equivocation{false};
    for (const DirectMessage& message : proofs) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (!proof) continue;
        const std::vector<unsigned char> encoded =
            SerializeCapacityProof(*proof);
        if (encoded == recovery.capacity_snapshot.capacity_proof) continue;

        std::string evidence_error;
        if (store.RecordAlternativeRecoveryCapacityEquivocation(
                recovery.recovery_id, encoded, message.received_at,
                evidence_error)) {
            capacity_equivocation = true;
            continue;
        }
        if (!evidence_error.empty() &&
            evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
            error = std::move(evidence_error);
            return false;
        }
    }
    if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
        return false;
    }
    if (capacity_equivocation &&
        block_policy == EquivocationBlockPolicy::ENFORCE) {
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    if (block_policy == EquivocationBlockPolicy::ENFORCE &&
        !EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    return true;
}

bool DrainPendingAlternativeRecoveryCapacityEquivocations(
    DigiDollar::Paymaster::Manager& manager,
    PaymasterStore& store,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    const std::vector<unsigned char>& first_capacity_proof,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    if (recovery.provider_side || first_capacity_proof.empty() ||
        recovery.phase != AlternativeRecoveryPhase::CAPACITY_PENDING) {
        error = "PAYMASTER_INVALID_RECOVERY_CAPACITY_STATE";
        return false;
    }
    if (!EnsureProviderNotEquivocationBlocked(
            store, recovery.recovery_provider_id, error)) {
        return false;
    }
    const auto raw_proofs = manager.PeekCapacityProofs(
        recovery.recovery_provider_id, recovery.client_nonce,
        MAX_DIRECT_INBOX_MESSAGES_PER_SESSION, /*lease=*/false);
    const auto proofs = SelectCapacityProofMessages(
        raw_proofs, recovery.recovery_provider_id,
        recovery.capacity_request.request_id,
        recovery.capacity_request.session_id, recovery.client_nonce);
    bool capacity_equivocation{false};
    for (const DirectMessage& message : proofs) {
        const auto* proof =
            std::get_if<PaymasterCapacityProof>(&message.payload);
        if (!proof) continue;
        const std::vector<unsigned char> encoded =
            SerializeCapacityProof(*proof);
        if (encoded == first_capacity_proof) continue;
        std::string evidence_error;
        if (store.RecordPendingAlternativeRecoveryCapacityEquivocation(
                recovery.recovery_id, first_capacity_proof, encoded,
                message.received_at, evidence_error)) {
            capacity_equivocation = true;
            continue;
        }
        if (!evidence_error.empty() &&
            evidence_error != "PAYMASTER_INVALID_CAPACITY_EQUIVOCATION") {
            error = std::move(evidence_error);
            return false;
        }
    }
    if (capacity_equivocation) {
        if (!AcknowledgeEquivocationMessages(manager, proofs, error)) {
            return false;
        }
        error = "PAYMASTER_CAPACITY_EQUIVOCATION";
        return false;
    }
    // The calling acceptance scope owns the prune lease until the recovery
    // snapshot and signed request commit. This comparison must not acquire a
    // second, unscoped reference that could survive an early return.
    return EnsureProviderNotEquivocationBlocked(
        store, recovery.recovery_provider_id, error);
}

std::vector<unsigned char> SerializeSubmit(
    const DigiDollar::Paymaster::PaymasterSubmit& submit)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << submit;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename T>
// Recovery messages use the same bounded direct-message transport as normal
// Paymaster traffic, but their semantic replay key is tied to the durable
// session rather than the transient peer connection.
bool QueueRecoveryMessage(WalletContext& context,
                          CWallet& wallet,
                          const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
                          const T& message,
                          int64_t now,
                          RecoveryQueueState& state,
                          std::string& error)
{
    using namespace DigiDollar::Paymaster;
    state = {};
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "PAYMASTER_DISABLED";
        return false;
    }
    const bool allow_local_endpoint{
        Params().GetChainType() == ChainType::REGTEST};
    CService endpoint = LookupNumeric(recovery.recovery_provider_endpoint);
    state.route_available = IsEndpointAllowedForPrivacyProfile(
        endpoint, recovery.privacy_profile, allow_local_endpoint);
    if (!state.route_available) {
        for (const Announcement& announcement :
             context.paymaster->GetDirectory().List(now)) {
            if (GetPaymasterId(announcement.identity_key) ==
                    recovery.recovery_provider_id &&
                IsEndpointAllowedForPrivacyProfile(
                    announcement.endpoint, recovery.privacy_profile,
                    allow_local_endpoint)) {
                endpoint = announcement.endpoint;
                state.route_available = true;
                break;
            }
        }
    }
    if (!state.route_available) return true;
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    const DirectKey channel{wallet.m_paymaster_transport_owner.GetHex(),
                            "recovery:" + recovery.recovery_id.GetHex(), true, recovery.recovery_provider_id};
    const auto transport = node->connman->RequestPaymasterConnection(
        channel, endpoint, recovery.privacy_profile == PrivacyProfile::HIGH);
    state.connection_pending = transport.Pending();
    if (transport.state != DirectState::READY || !transport.lease) {
        if (state.connection_pending) return true;
        error = PaymasterTransportError(transport.state);
        return false;
    }
    const NodeId peer_id = transport.lease->peer_id;
    const std::vector<unsigned char> encoded =
        SerializeRecoveryMessage(message);
    const uint256 message_id{Hash(encoded)};
    state.queued =
        context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
        context.paymaster->QueueOutboundDirectMessage(
            peer_id, message_id, encoded.size(), DirectPayload{message}, now);
    if (!state.queued) {
        error = "PAYMASTER_DIRECT_QUEUE_FULL";
        return false;
    }
    node->connman->WakeMessageHandler();
    return true;
}

template bool QueueRecoveryMessage<DigiDollar::Paymaster::PaymasterCapacityRequest>(
    WalletContext&,
    CWallet&,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord&,
    const DigiDollar::Paymaster::PaymasterCapacityRequest&,
    int64_t,
    RecoveryQueueState&,
    std::string&);
template bool QueueRecoveryMessage<DigiDollar::Paymaster::AlternativeRecoveryRequest>(
    WalletContext&,
    CWallet&,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord&,
    const DigiDollar::Paymaster::AlternativeRecoveryRequest&,
    int64_t,
    RecoveryQueueState&,
    std::string&);
template bool QueueRecoveryMessage<DigiDollar::Paymaster::AlternativeRecoverySubmit>(
    WalletContext&,
    CWallet&,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord&,
    const DigiDollar::Paymaster::AlternativeRecoverySubmit&,
    int64_t,
    RecoveryQueueState&,
    std::string&);

bool BuildRecoveryParametersFromRecord(
    CWallet& wallet,
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery,
    DigiDollar::Paymaster::AlternativeRecoveryParameters& parameters,
    std::string& error)
{
    // Keep RPC, background recovery and restart recovery on the same ownership
    // and immutable-template firewall. In particular, the shared builder also
    // proves that every locally supplied provider input and every wallet-return
    // script belongs to the wallet before either side can sign.
    return BuildAlternativeRecoveryParametersFromRecord(
        wallet, recovery, parameters, error);
}

bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error);

bool HasDurableClientAuthorization(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t observation_time)
{
    return paymaster_store::internal::HasAcceptedClientManifest(attempt, observation_time);
}

bool QueuePersistedPaymasterSubmit(
    WalletContext& context,
    CWallet& wallet,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    PaymasterSubmitQueueState& state,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    state = {};
    error.clear();
    if (!context.paymaster || !context.paymaster->Enabled()) {
        error = "PAYMASTER_DISABLED";
        return false;
    }
    if (attempt.user_signed_psbt.empty() || attempt.retry_until <= 0 ||
        now > attempt.retry_until) {
        error = "PAYMASTER_RETRY_UNAVAILABLE";
        return false;
    }
    PaymentIntent authorized_intent;
    PaymasterQuote authorized_quote;
    CollaborativePSBTTemplate authorized_template;
    if (!HasDurableClientAuthorization(attempt, now) ||
        attempt.client_manifest_accepted_at > session.updated_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
        return false;
    }
    if (!LoadAttemptAuthorizationArtifacts(
            attempt, authorized_intent, authorized_quote, authorized_template, error) ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, authorized_intent, authorized_quote,
            attempt.capacity_snapshot, authorized_template, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }

    PartiallySignedTransaction persisted_user_psbt;
    if (!DecodeRawPSBT(persisted_user_psbt,
                       MakeByteSpan(attempt.user_signed_psbt), error) ||
        !ValidateCollaborativePSBT(
            persisted_user_psbt, authorized_template,
            CollaborativeSignatureStage::USER_SIGNED, error)) {
        if (error.empty()) error = "PAYMASTER_PERSISTED_USER_PSBT_INVALID";
        return false;
    }
    CDataStream canonical_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_stream << persisted_user_psbt;
    const auto canonical_span = MakeUCharSpan(canonical_stream);
    if (std::vector<unsigned char>{canonical_span.begin(), canonical_span.end()} !=
        attempt.user_signed_psbt) {
        error = "PAYMASTER_PERSISTED_USER_PSBT_NONCANONICAL";
        return false;
    }
    if (!ValidateClientAuthorizationOwnership(
            wallet, attempt.client_manifest, error)) {
        return false;
    }

    // A persisted USER signature is not authority to reuse stale or replaced
    // provider liquidity. Reconstruct the exact capacity artifacts and apply
    // the same identity/control-proof/chainstate firewall used immediately
    // before the first signature on every retry, except for inputs proven to
    // belong to the exact locally observed final transaction below.
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    try {
        SpanReader request_stream{::PROTOCOL_VERSION,
                                  attempt.capacity_request};
        request_stream >> capacity_request;
        SpanReader proof_stream{
            ::PROTOCOL_VERSION,
            attempt.capacity_snapshot.capacity_proof};
        proof_stream >> capacity_proof;
        if (!request_stream.empty() || !proof_stream.empty()) {
            throw std::ios_base::failure(
                "trailing persisted capacity artifact data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_CAPACITY_ARTIFACT_ENCODING";
        return false;
    }
    CDataStream canonical_request{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_request << capacity_request;
    CDataStream canonical_proof{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_proof << capacity_proof;
    const auto canonical_request_span = MakeUCharSpan(canonical_request);
    const auto canonical_proof_span = MakeUCharSpan(canonical_proof);
    if (std::vector<unsigned char>{canonical_request_span.begin(),
                                   canonical_request_span.end()} !=
            attempt.capacity_request ||
        std::vector<unsigned char>{canonical_proof_span.begin(),
                                   canonical_proof_span.end()} !=
            attempt.capacity_snapshot.capacity_proof ||
        Hash(attempt.capacity_request) !=
            attempt.capacity_snapshot.request_hash) {
        error = "PAYMASTER_CAPACITY_ARTIFACT_NONCANONICAL";
        return false;
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->chainman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    // A restarted client may have missed the provider's signed result even
    // though the exact payment is already mined. Only a fully validated,
    // locally observed final transaction permits retrying its spent Capacity.
    // The unsigned txid or persisted USER PSBT alone never grants this exception.
    CTransactionRef known_final;
    {
        LOCK(wallet.cs_wallet);
        if (const auto* wallet_tx = wallet.GetWalletTx(attempt.unsigned_txid))
            known_final = wallet_tx->tx;
    }
    if (!known_final) {
        uint256 block_hash;
        known_final = node::GetTransaction(nullptr, node->mempool.get(),
            attempt.unsigned_txid, block_hash, node->chainman->m_blockman);
    }
    std::optional<ExactFinalTransactionProof> final_proof;
    if (known_final) {
        FinalTransactionPresence presence{FinalTransactionPresence::NONE};
        if (!ValidateFinalCollaborativeTransaction(
                CMutableTransaction{*known_final}, known_final->GetHash(),
                known_final->GetWitnessHash(), authorized_template, error) ||
            !PreflightFinalPaymasterTransaction(wallet, known_final, presence,
                error, ExactFinalTxIndexMode::NONBLOCKING, &final_proof)) {
            return false;
        }
    }
    const int64_t observation_time =
        std::max(now, attempt.client_manifest_accepted_at);
    if (!node || !node->chainman ||
        !ValidateAuthorizedCapacityRetryAgainstChainstate(
            capacity_proof, capacity_request,
            attempt.provider_identity_key, *node->chainman,
            attempt.client_manifest_accepted_at, observation_time,
            final_proof ? &*final_proof : nullptr, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_NOT_CURRENT";
        return false;
    }

    PaymasterSubmit submit;
    submit.genesis_hash = Params().GenesisBlock().GetHash();
    submit.provider_id = attempt.provider_id;
    submit.request_id = session.request_id;
    submit.session_id = session.session_id;
    submit.quote_id = attempt.quote_id;
    submit.commit_key = attempt.commit_key;
    submit.template_commitment = attempt.template_commitment;
    submit.user_psbt = attempt.user_signed_psbt;
    if (!ValidateSubmitEnvelope(submit, submit.genesis_hash, error)) return false;

    CService endpoint;
    const bool allow_local_endpoint{
        Params().GetChainType() == ChainType::REGTEST};
    if (!attempt.provider_endpoint.empty()) {
        endpoint = LookupNumeric(attempt.provider_endpoint);
        state.route_available = IsEndpointAllowedForPrivacyProfile(
            endpoint, attempt.privacy_profile, allow_local_endpoint);
    }
    if (!state.route_available) {
        for (const Announcement& announcement : context.paymaster->GetDirectory().List(now)) {
            if (GetPaymasterId(announcement.identity_key) == attempt.provider_id &&
                IsEndpointAllowedForPrivacyProfile(
                    announcement.endpoint, attempt.privacy_profile,
                    allow_local_endpoint)) {
                endpoint = announcement.endpoint;
                state.route_available = true;
                break;
            }
        }
    }
    if (!state.route_available) return true;

    if (!node || !node->connman) {
        error = "PAYMASTER_NODE_CONTEXT_UNAVAILABLE";
        return false;
    }
    const auto transport = node->connman->RequestPaymasterConnection(
        PaymentChannelKey(wallet, session, attempt.provider_id), endpoint,
        attempt.privacy_profile == PrivacyProfile::HIGH);
    state.connection_pending = transport.Pending();
    if (transport.state != DirectState::READY || !transport.lease) {
        if (state.connection_pending) return true;
        error = PaymasterTransportError(transport.state);
        return false;
    }
    const NodeId peer_id = transport.lease->peer_id;

    const std::vector<unsigned char> submit_bytes = SerializeSubmit(submit);
    const uint256 message_id = Hash(submit_bytes);
    state.queued = context.paymaster->HasOutboundDirectMessage(peer_id, message_id) ||
                   context.paymaster->QueueOutboundDirectMessage(
                       peer_id, message_id, submit_bytes.size(), DirectPayload{submit}, now);
    if (!state.queued) {
        error = "PAYMASTER_DIRECT_QUEUE_FULL";
        return false;
    }
    node->connman->WakeMessageHandler();
    return true;
}

std::vector<unsigned char> SerializePaymentIntent(
    const DigiDollar::Paymaster::PaymentIntent& intent)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << intent;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

// -------------------------------------------------------------------------
// Final transaction validation and broadcast
// -------------------------------------------------------------------------
// The persisted PSBT/template is the trust anchor. Peer-returned raw
// transactions are inserted into that template and fully checked before the
// wallet records a terminal state or broadcasts anything.
bool LoadTrustedTemplate(const DigiDollar::Paymaster::ProviderAttempt& attempt,
                         DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
                         std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (!DecodeRawPSBT(trusted.psbt, MakeByteSpan(attempt.unsigned_psbt), error)) return false;
    if (!trusted.psbt.tx || trusted.psbt.inputs.size() != attempt.input_roles.size()) {
        error = "PAYMASTER_PERSISTED_TEMPLATE_SHAPE";
        return false;
    }
    static_assert(static_cast<uint8_t>(ReservationRole::USER_DD) == static_cast<uint8_t>(InputRole::USER_DD));
    static_assert(static_cast<uint8_t>(ReservationRole::USER_DGB) == static_cast<uint8_t>(InputRole::USER_DGB));
    static_assert(static_cast<uint8_t>(ReservationRole::PROVIDER_CARRIER) == static_cast<uint8_t>(InputRole::PROVIDER_CARRIER));
    static_assert(static_cast<uint8_t>(ReservationRole::PROVIDER_DGB) == static_cast<uint8_t>(InputRole::PROVIDER_DGB));
    trusted.input_roles.reserve(attempt.input_roles.size());
    for (const ReservationRole role : attempt.input_roles) {
        trusted.input_roles.push_back(static_cast<InputRole>(role));
    }
    return true;
}

bool LoadAttemptAuthorizationArtifacts(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    DigiDollar::Paymaster::CollaborativePSBTTemplate& trusted,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    if (!DecodePaymasterAuthorizationArtifacts(attempt, request, response, trusted, error)) {
        return false;
    }
    intent = std::move(request.intent);
    quote = std::move(response.quote);
    return true;
}

bool ValidateDurableProviderUserAuthorization(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    UserAuthorizationRecord authorization;
    if (attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
        attempt.commit_key.IsNull() || attempt.user_signed_psbt.empty() ||
        !store.GetUserAuthorization(attempt.commit_key, authorization) ||
        authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key != attempt.commit_key ||
        authorization.attempt_id != attempt.attempt_id ||
        authorization.canonical_psbt_hash !=
            Hash(attempt.user_signed_psbt) ||
        authorization.accepted_at <= 0 ||
        authorization.retry_until != attempt.retry_until) {
        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
        return false;
    }
    return true;
}

bool FindDurableClientAuthorizationAttempt(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    int64_t observation_time,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    DigiDollar::Paymaster::PaymentIntent& intent,
    DigiDollar::Paymaster::PaymasterQuote& quote,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (session.provider_side) return false;
    PaymentSession current = session;
    for (auto it = session.attempt_ids.rbegin();
         it != session.attempt_ids.rend(); ++it) {
        ProviderAttempt candidate;
        if (!store.GetAttempt(*it, candidate)) {
            error = "PAYMASTER_ATTEMPT_NOT_FOUND";
            return false;
        }
        ClientAuthorizationState authorization;
        if (!store.GetClientAuthorizationState(current, candidate, observation_time, authorization, error)) return false;
        if (authorization == ClientAuthorizationState::CLOSED_UNSIGNED) {
            if (candidate.state != AttemptState::REJECTED && *it == session.attempt_ids.back() &&
                (!store.AbandonClientAttemptForFallback(session.request_id, candidate.attempt_id, observation_time, error) ||
                 !store.GetSessionByRequestId(session.request_id, current))) return false;
            continue;
        }
        if (!HasDurableClientAuthorization(candidate, observation_time) ||
            candidate.client_manifest_accepted_at > session.updated_at) {
            const bool has_authorization_evidence =
                !candidate.accepted_client_manifest_id.IsNull() ||
                candidate.client_manifest_accepted_at != 0 ||
                !candidate.user_signed_psbt.empty() ||
                (candidate.state >= AttemptState::USER_SIGNED &&
                 candidate.state <= AttemptState::MEMPOOL) ||
                candidate.state == AttemptState::AMBIGUOUS;
            if (has_authorization_evidence) {
                error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
                return false;
            }
            continue;
        }
        CollaborativePSBTTemplate trusted;
        if (!LoadAttemptAuthorizationArtifacts(
                candidate, intent, quote, trusted, error) ||
            !ValidateClientAuthorizationManifest(
                candidate.client_manifest, intent, quote,
                candidate.capacity_snapshot, trusted, error) ||
            !ValidateClientAuthorizationOwnership(
                wallet, candidate.client_manifest, error)) {
            if (error.empty()) {
                error = "PAYMASTER_CLIENT_AUTHORIZATION_INVALID";
            }
            return false;
        }
        attempt = std::move(candidate);
        return true;
    }
    error.clear();
    return false;
}

PaymentResult CaptureDurableClientAuthorization(
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    const DigiDollar::Paymaster::PaymentIntent& intent,
    const DigiDollar::Paymaster::PaymasterQuote& quote)
{
    using namespace DigiDollar::Paymaster;
    PaymentResult result = CaptureSessionPayment(session);
    result.provider_id = attempt.provider_id;
    result.offer_id = intent.offer_id;
    result.policy_hash = intent.policy_hash;
    result.funding_model = intent.funding_model;
    result.payment_cents = intent.recipient_amount;
    result.service_fee_cents = quote.service_fee;
    result.user_total_cents = DDCents{intent.recipient_amount.value + quote.service_fee.value};
    if (const auto address = DigiDollarAddressForScript(
            intent.recipient_script)) {
        result.to_address = *address;
    }
    result.expires_at = intent.expires_at;
    result.queued = false;
    result.connection_pending = false;
    result.capacity_pending = false;
    result.capacity_snapshot_id = attempt.capacity_snapshot.snapshot_id;
    result.quote_id = attempt.quote_id;
    result.unsigned_txid = attempt.unsigned_txid;
    result.template_commitment = attempt.template_commitment;
    result.authorization_commitment = attempt.client_manifest.manifest_id;
    const bool authorization_accepted =
        attempt.accepted_client_manifest_id ==
            attempt.client_manifest.manifest_id &&
        attempt.client_manifest_accepted_at > 0;
    result.authorization_accepted = authorization_accepted;
    if (authorization_accepted) {
        result.authorization_accepted_at = attempt.client_manifest_accepted_at;
    }
    result.psbt = attempt.unsigned_psbt;
    return result;
}

bool GetValidatedClientAuthorizationCommitment(
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    uint256& commitment,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymentIntent intent;
    PaymasterQuote quote;
    CollaborativePSBTTemplate trusted;
    if (!LoadAttemptAuthorizationArtifacts(attempt, intent, quote, trusted, error) ||
        attempt.client_manifest.manifest_id.IsNull() ||
        (attempt.state == AttemptState::QUOTED && now > attempt.quote_expires_at) ||
        now > attempt.client_manifest.expires_at ||
        !ValidateClientAuthorizationManifest(
            attempt.client_manifest, intent, quote, attempt.capacity_snapshot,
            trusted, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }
    commitment = attempt.client_manifest.manifest_id;
    return true;
}

std::vector<unsigned char> SerializePSBT(const PartiallySignedTransaction& psbt)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << psbt;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

std::vector<unsigned char> SerializeTransaction(const CMutableTransaction& transaction)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << transaction;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

bool PreflightFinalPaymasterTransaction(CWallet& wallet,
                                        const CTransactionRef& transaction,
                                        FinalTransactionPresence& presence,
                                        std::string& error,
                                        ExactFinalTxIndexMode txindex_mode,
                                        std::optional<ExactFinalTransactionProof>* final_proof)
{
    if (final_proof) final_proof->reset();
    ExactFinalTransactionPreflight preflight;
    if (!PreflightExactPaymasterFinalTransaction(
            wallet, transaction, txindex_mode,
            preflight, error)) {
        presence = FinalTransactionPresence::NONE;
        return false;
    }
    presence = preflight.presence;
    if (final_proof) *final_proof = preflight.final_proof;
    return true;
}

bool MarkFinalValidationFailureRecoverable(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& attempt,
    int64_t now,
    std::string& error)
{
    return store.MarkClientFinalValidationFailureRecoverable(
        session.request_id, attempt.attempt_id, now, error);
}

std::vector<COutPoint> ProviderInputs(const CMutableTransaction& transaction,
                                      const std::vector<DigiDollar::Paymaster::ReservationRole>& roles)
{
    using namespace DigiDollar::Paymaster;
    std::vector<COutPoint> result;
    if (transaction.vin.size() != roles.size()) return result;
    for (size_t index = 0; index < roles.size(); ++index) {
        if (roles[index] == ReservationRole::PROVIDER_CARRIER ||
            roles[index] == ReservationRole::PROVIDER_DGB) {
            result.push_back(transaction.vin[index].prevout);
        }
    }
    return result;
}

bool TemplateInputsAvailable(CWallet& wallet, const CMutableTransaction& transaction)
{
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& input : transaction.vin)
        coins.emplace(input.prevout, Coin{});
    wallet.chain().findCoins(coins);
    return std::all_of(coins.begin(), coins.end(), [](const auto& entry) {
        return !entry.second.IsSpent();
    });
}

bool RejectUnavailableTemplateInputs(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    const CMutableTransaction& transaction,
    int64_t now,
    bool& rejected,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    rejected = false;
    if (attempt.state == AttemptState::REJECTED) {
        if (!store.GetProviderResult(attempt.commit_key, result) ||
            result.status != PaymasterResultStatus::REJECTED) {
            error = "PAYMASTER_REJECTED_RESULT_MISSING";
            return false;
        }
        rejected = true;
        return true;
    }
    if (attempt.state != AttemptState::QUOTED &&
        attempt.state != AttemptState::USER_SIGNED &&
        attempt.state != AttemptState::USER_PSBT_ACCEPTED) {
        return true;
    }
    if (TemplateInputsAvailable(wallet, transaction)) return true;

    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != attempt.provider_id ||
        identity.identity_key != attempt.provider_identity_key) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = attempt.provider_id;
    result.commit_key = attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::REJECTED;
    result.updated_at = now;
    result.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    if (!store.RejectUnavailableProviderSubmit(
            attempt.attempt_id, result, result.genesis_hash, attempt, error)) {
        return false;
    }
    rejected = true;
    return true;
}

bool BuildFinalProviderResult(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    DigiDollar::Paymaster::PaymasterResult& result,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    result = PaymasterResult{};
    CMutableTransaction transaction;
    try {
        SpanReader stream{::PROTOCOL_VERSION, commit.final_transaction};
        stream >> transaction;
        if (!stream.empty()) {
            error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
            return false;
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_INVALID_PROVIDER_COMMIT";
        return false;
    }
    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != commit.provider_id) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = commit.provider_id;
    result.commit_key = commit.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = CTransaction{transaction}.GetHash();
    result.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
    result.final_transaction = transaction;
    result.updated_at = now;
    result.identity_signature.resize(64);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    error.clear();
    return true;
}

bool GetOrCreateFinalProviderResult(CWallet& wallet,
                                    PaymasterStore& store,
                                    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
                                    int64_t now,
                                    DigiDollar::Paymaster::PaymasterResult& result,
                                    std::string& error)
{
    const DatabaseReadStatus status = store.GetProviderResultWithStatus(commit.commit_key, result);
    if (status == DatabaseReadStatus::FOUND) return true;
    if (status != DatabaseReadStatus::NOT_FOUND) {
        error = status == DatabaseReadStatus::UNSUPPORTED_VERSION
            ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_RESULT_DATABASE_READ";
        return false;
    }
    if (!BuildFinalProviderResult(wallet, commit, now, result, error)) return false;
    return store.StoreProviderResult(result, result.genesis_hash, error);
}

bool InsertAndBroadcastPaymasterTransaction(CWallet& wallet,
                                            const CTransactionRef& transaction,
                                            bool& already_confirmed,
                                            std::string& error)
{
    return InsertAndBroadcastExactPaymasterTransaction(
        wallet, transaction, already_confirmed, error);
}

// Wallet insertion precedes network broadcast so restart recovery can find the
// exact authorized transaction even if the process exits between the two.
bool InsertAndBroadcastProviderCommit(CWallet& wallet,
                                      PaymasterStore& store,
                                      const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now,
    bool& already_confirmed,
    std::string& error,
    CTransactionRef* exact_transaction)
{
    using namespace DigiDollar::Paymaster;
    ProviderAttempt attempt;
    if (!store.GetAttemptByTemplateCommitment(commit.template_commitment, attempt)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    CMutableTransaction decoded;
    if (!ValidateProviderCommitForExecution(attempt, commit, now, decoded, error) ||
        !store.ValidateProviderBudgetAuthorization(
            attempt, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
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
    const CTransactionRef transaction = MakeTransactionRef(decoded);
    if (exact_transaction) *exact_transaction = transaction;
    FinalTransactionPresence presence{FinalTransactionPresence::NONE};
    // Automatic submits reach this helper on the validation scheduler. A new
    // tip may have arrived since the pre-commit check, so never wait for that
    // same scheduler to drain the transaction-index notifications here.
    if (!PreflightFinalPaymasterTransaction(
            wallet, transaction, presence, error,
            ExactFinalTxIndexMode::NONBLOCKING)) {
        return false;
    }
    return InsertAndBroadcastPaymasterTransaction(
        wallet, transaction, already_confirmed, error);
}

bool MarkProviderResultBroadcastAttempted(
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymasterResult& result,
    int64_t now,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    if (result.status == PaymasterResultStatus::BROADCAST_ATTEMPTED) return true;
    CKey identity_key;
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentityKey(wallet, identity_key, identity, error) ||
        identity.provider_id != result.provider_id) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    result.status = PaymasterResultStatus::BROADCAST_ATTEMPTED;
    ++result.result_sequence;
    result.updated_at = std::max(result.updated_at, now);
    result.identity_signature.assign(64, 0);
    if (!identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
                                  result.identity_signature, nullptr,
                                  GetRandHash())) {
        error = "PAYMASTER_RESULT_SIGNING_FAILED";
        return false;
    }
    return store.StoreProviderResult(result, result.genesis_hash, error);
}

bool IsProviderNetworkStateAtLeast(
    DigiDollar::Paymaster::AttemptState state,
    DigiDollar::Paymaster::AttemptState minimum)
{
    using DigiDollar::Paymaster::AttemptState;
    switch (minimum) {
    case AttemptState::BROADCAST:
        return state == AttemptState::BROADCAST ||
               state == AttemptState::STEMPOOL ||
               state == AttemptState::MEMPOOL;
    case AttemptState::STEMPOOL:
        return state == AttemptState::STEMPOOL ||
               state == AttemptState::MEMPOOL;
    case AttemptState::MEMPOOL:
        return state == AttemptState::MEMPOOL;
    default:
        return false;
    }
}

bool IsProviderSessionNetworkStateAtLeast(
    DigiDollar::Paymaster::SessionState state,
    DigiDollar::Paymaster::SessionState minimum)
{
    using DigiDollar::Paymaster::SessionState;
    switch (minimum) {
    case SessionState::STEMPOOL:
        return state == SessionState::STEMPOOL ||
               state == SessionState::MEMPOOL ||
               state == SessionState::CONFIRMED;
    case SessionState::MEMPOOL:
        return state == SessionState::MEMPOOL ||
               state == SessionState::CONFIRMED;
    case SessionState::CONFIRMED:
        return state == SessionState::CONFIRMED;
    default:
        return false;
    }
}

bool ReloadExactProviderCommitAttempt(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    const uint256& expected_attempt_id,
    const uint256& expected_session_id,
    int64_t now,
    DigiDollar::Paymaster::ProviderAttempt& attempt,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    ProviderAttempt reloaded;
    CMutableTransaction validated;
    if (!store.GetAttemptByTemplateCommitment(
            commit.template_commitment, reloaded)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (reloaded.attempt_id != expected_attempt_id ||
        reloaded.session_id != expected_session_id ||
        !ValidateProviderCommitForExecution(
            reloaded, commit, now, validated, error) ||
        !store.ValidateProviderBudgetAuthorization(
            reloaded, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return false;
    }
    attempt = std::move(reloaded);
    return true;
}

bool ReloadExactProviderCommitSession(
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    const std::string& expected_request_id,
    const uint256& expected_session_id,
    const uint256& expected_attempt_id,
    DigiDollar::Paymaster::PaymentSession& session,
    std::string& error)
{
    using namespace DigiDollar::Paymaster;
    PaymentSession reloaded;
    if (!store.GetSessionBySessionId(expected_session_id, reloaded)) {
        error = "PAYMASTER_SESSION_NOT_FOUND";
        return false;
    }
    if (reloaded.request_id != expected_request_id ||
        reloaded.session_id != expected_session_id ||
        !reloaded.provider_side ||
        (!reloaded.final_txid.IsNull() &&
         reloaded.final_txid != commit.final_txid) ||
        std::find(reloaded.attempt_ids.begin(), reloaded.attempt_ids.end(),
                  expected_attempt_id) == reloaded.attempt_ids.end()) {
        error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return false;
    }
    session = std::move(reloaded);
    return true;
}

ProviderCommitRecoveryResult RecoverProviderCommit(
    CWallet& wallet,
    PaymasterStore& store,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    ProviderCommitRecoveryResult result;
    ProviderCommitRecord persisted_commit;
    if (commit.commit_key.IsNull() ||
        !store.GetProviderCommit(commit.commit_key, persisted_commit) ||
        persisted_commit.version != commit.version ||
        persisted_commit.commit_key != commit.commit_key ||
        persisted_commit.provider_id != commit.provider_id ||
        persisted_commit.quote_id != commit.quote_id ||
        persisted_commit.template_commitment != commit.template_commitment ||
        persisted_commit.final_txid != commit.final_txid ||
        persisted_commit.raw_transaction_hash != commit.raw_transaction_hash ||
        persisted_commit.final_transaction != commit.final_transaction ||
        persisted_commit.provider_inputs != commit.provider_inputs ||
        persisted_commit.committed_at != commit.committed_at ||
        persisted_commit.retry_until != commit.retry_until) {
        result.error = "PAYMASTER_PROVIDER_COMMIT_CONFLICT";
        return result;
    }
    ProviderAttempt attempt;
    if (!store.GetAttemptByTemplateCommitment(commit.template_commitment, attempt)) {
        result.error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return result;
    }
    PaymentSession session;
    if (!store.GetSessionBySessionId(attempt.session_id, session)) {
        result.error = "PAYMASTER_SESSION_NOT_FOUND";
        return result;
    }
    // The caller supplied the exact durable ProviderCommitRecord. The
    // broadcast helper reloads its current attempt and applies the complete
    // commit, manifest, ownership, and budget firewall.
    result.broadcast = InsertAndBroadcastProviderCommit(
        wallet, store, commit, now, result.already_confirmed, result.error);
    if (!result.broadcast) return result;

    const uint256 expected_attempt_id{attempt.attempt_id};
    const uint256 expected_session_id{attempt.session_id};
    if (!ReloadExactProviderCommitAttempt(
            store, commit, expected_attempt_id, expected_session_id, now,
            attempt, result.error)) {
        return result;
    }
    const auto advance_attempt = [&](AttemptState target) {
        if (IsProviderNetworkStateAtLeast(attempt.state, target)) return true;

        ProviderAttempt update{attempt};
        update.state = target;
        update.updated_at = std::max(update.updated_at, now);
        std::string update_error;
        if (store.UpdateAttempt(session.request_id, update, update_error)) {
            attempt = std::move(update);
            return true;
        }
        if (update_error != "PAYMASTER_INVALID_ATTEMPT_TRANSITION") {
            result.error = std::move(update_error);
            return false;
        }

        // Broadcast can synchronously notify the wallet. That callback may
        // atomically advance the durable attempt to STEMPOOL/MEMPOOL while
        // this RPC still holds its pre-callback copy. Treat the stale update
        // as idempotent only after reloading and revalidating the exact commit
        // binding, and only when the callback reached this target or a later
        // network-observation state.
        ProviderAttempt raced;
        std::string reload_error;
        if (!ReloadExactProviderCommitAttempt(
                store, commit, expected_attempt_id, expected_session_id, now,
                raced, reload_error)) {
            result.error = std::move(reload_error);
            return false;
        }
        if (!IsProviderNetworkStateAtLeast(raced.state, target)) {
            result.error = std::move(update_error);
            return false;
        }
        attempt = std::move(raced);
        return true;
    };
    if (!advance_attempt(AttemptState::BROADCAST)) return result;
    if (!result.already_confirmed &&
        !advance_attempt(gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) ? AttemptState::STEMPOOL : AttemptState::MEMPOOL)) {
        return result;
    }
    const SessionState session_state = result.already_confirmed ? SessionState::CONFIRMED : (gArgs.GetBoolArg("-dandelion", DEFAULT_DANDELION) ? SessionState::STEMPOOL : SessionState::MEMPOOL);
    if (!ReloadExactProviderCommitSession(
            store, commit, session.request_id, expected_session_id,
            expected_attempt_id, session, result.error)) {
        return result;
    }
    if (session.final_txid != commit.final_txid ||
        !IsProviderSessionNetworkStateAtLeast(session.state, session_state)) {
        std::string transition_error;
        if (!store.TransitionSession(
                session.request_id, session_state, PendingPhase::NONE,
                commit.final_txid, now, transition_error)) {
            if (transition_error != "PAYMASTER_INVALID_SESSION_TRANSITION") {
                result.error = std::move(transition_error);
                return result;
            }
            PaymentSession raced;
            std::string reload_error;
            if (!ReloadExactProviderCommitSession(
                    store, commit, session.request_id, expected_session_id,
                    expected_attempt_id, raced, reload_error)) {
                result.error = std::move(reload_error);
                return result;
            }
            if (raced.final_txid != commit.final_txid ||
                !IsProviderSessionNetworkStateAtLeast(
                    raced.state, session_state)) {
                result.error = std::move(transition_error);
                return result;
            }
            session = std::move(raced);
        }
    }
    if (!GetOrCreateFinalProviderResult(
            wallet, store, commit, now, result.provider_result, result.error)) {
        return result;
    }
    std::string result_error;
    if (!MarkProviderResultBroadcastAttempted(
            wallet, store, result.provider_result, now, result_error)) {
        result.error = result_error;
    }
    return result;
}

// -------------------------------------------------------------------------
// Strict policy parsing
// -------------------------------------------------------------------------
// Parsers reject unknown or nonsensical values rather than inheriting permissive
// defaults. In particular, zero safety budgets mean disabled, never unlimited.

} // namespace wallet::paymaster_app

namespace wallet {
bool EnsureProviderNotEquivocationBlocked(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    std::string& error)
{
    DigiDollar::Paymaster::PaymasterProviderBlock block;
    const DatabaseReadStatus status =
        store.GetProviderBlock(provider_id, block, error);
    if (status == DatabaseReadStatus::READ_ERROR) return false;
    if (status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_PROVIDER_EQUIVOCATION_BLOCKED";
        return false;
    }

    DigiDollar::Paymaster::PaymasterEquivocationEvidence pending;
    const DatabaseReadStatus pending_status =
        store.GetPendingEquivocation(provider_id, pending, error);
    if (pending_status == DatabaseReadStatus::READ_ERROR) return false;
    if (pending_status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_PROVIDER_EQUIVOCATION_PENDING";
        return false;
    }
    return true;
}
} // namespace wallet
