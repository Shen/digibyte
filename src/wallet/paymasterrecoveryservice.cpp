// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <wallet/paymasterrecoveryservice.h>
#include <wallet/paymasterservice.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>
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
#include <paymaster/validation.h>
#include <random.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/spend.h>
#include <util/chaintype.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <algorithm>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;
ClientRecoveryResult ResolveAlternativePaymasterRecovery(
    WalletContext& context,
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& original_attempt,
    const ClientRecoveryOptions& options)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    int64_t now{GetTime()};
    if (!CheckPaymasterClientReadiness(wallet, context, error) ||
        !CheckPaymasterPrivacyReadiness(
            original_attempt.privacy_profile, context, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman || !node->chainman || !context.paymaster) {
        throw ApplicationError(ErrorKind::INTERNAL,
                           "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
    }

    const auto& requested_provider = options.provider_id;
    const auto& requested_offer = options.offer_id;
    const bool prepare_only = options.prepare_only;
    const auto& accepted_commitment = options.authorization_commitment;

    AlternativeRecoveryRecord recovery;
    const DatabaseReadStatus recovery_status =
        store.GetAlternativeRecoveryWithStatus(session.request_id, recovery);
    if (recovery_status != DatabaseReadStatus::FOUND && recovery_status != DatabaseReadStatus::NOT_FOUND) {
        throw ApplicationError(ErrorKind::WALLET,
            recovery_status == DatabaseReadStatus::UNSUPPORTED_VERSION
                ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_RECOVERY_DATABASE_READ");
    }
    if (recovery_status == DatabaseReadStatus::NOT_FOUND) {
        if (session.state != SessionState::PENDING_PROVIDER ||
            original_attempt.provider_id.IsNull() ||
            original_attempt.commit_key.IsNull() ||
            original_attempt.template_commitment.IsNull() ||
            !((original_attempt.state >= AttemptState::USER_SIGNED &&
               original_attempt.state <= AttemptState::MEMPOOL) ||
              original_attempt.state == AttemptState::AMBIGUOUS)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_ALLOWED");
        }
        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        if (!dd_wallet) {
            throw ApplicationError(ErrorKind::WALLET,
                               "DigiDollar wallet not initialized");
        }
        CAmount total_dd{0};
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        const auto& maximum_value = options.maximum_fee;
        const DDCents requested_maximum{
            !maximum_value.has_value() ? MAX_DD_OUTPUT_CENTS : *maximum_value};
        const DDCents effective_maximum =
            EffectiveClientServiceFeeCap(wallet, requested_maximum, error);
        if (effective_maximum.value < 0) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, error);
        }

        std::vector<PaymasterReliabilityRecord> reliability_records;
        if (!store.ListProviderReliability(reliability_records)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_REPUTATION_DATABASE_READ");
        }
        std::map<PaymasterId, PaymasterReliabilityRecord> reliability;
        for (auto& record : reliability_records) {
            reliability.emplace(record.provider_id, std::move(record));
        }
        const auto candidates = BuildRecoveryOfferCandidates(
            context.paymaster->GetDirectory().List(now), DDCents{total_dd},
            effective_maximum, reliability, now,
            error, Params().GetChainType() == ChainType::REGTEST);
        if (!error.empty()) {
            throw ApplicationError(ErrorKind::INVALID_INPUT, error);
        }
        const auto selected = std::find_if(
            candidates.begin(), candidates.end(),
            [&](const OfferCandidate& candidate) {
                return candidate.provider_id !=
                           original_attempt.provider_id &&
                       candidate.terms.funding_model ==
                           FundingModel::USER_PAID &&
                       (!requested_provider ||
                        candidate.provider_id == *requested_provider) &&
                       (!requested_offer ||
                        candidate.terms.offer_id == *requested_offer) &&
                       IsEndpointAllowedForPrivacyProfile(
                           candidate.endpoint, original_attempt.privacy_profile,
                           Params().GetChainType() == ChainType::REGTEST) &&
                       total_dd - candidate.service_fee.value >= 100;
            });
        if (selected == candidates.end()) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_NO_ELIGIBLE_RECOVERY_PROVIDER");
        }

        do {
            recovery.client_nonce = GetRandHash();
        } while (recovery.client_nonce.IsNull());
        recovery.provider_side = false;
        recovery.request_id = session.request_id;
        recovery.session_id = session.session_id;
        recovery.original_provider_id = original_attempt.provider_id;
        recovery.recovery_provider_id = selected->provider_id;
        recovery.privacy_profile = original_attempt.privacy_profile;
        recovery.offer_id = selected->terms.offer_id;
        recovery.policy_hash = selected->terms.policy_hash;
        recovery.recovery_provider_identity_key = selected->identity_key;
        recovery.recovery_provider_endpoint =
            selected->endpoint.ToStringAddrPort();
        recovery.original_commit_key = original_attempt.commit_key;
        recovery.original_template_commitment =
            original_attempt.template_commitment;
        recovery.selected_maximum_service_fee = effective_maximum;
        recovery.selected_service_fee = selected->service_fee;
        recovery.recovery_id = GetAlternativeRecoveryId(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, recovery.client_nonce);
        recovery.capacity_request.genesis_hash =
            Params().GenesisBlock().GetHash();
        recovery.capacity_request.provider_id =
            recovery.recovery_provider_id;
        recovery.capacity_request.request_id = recovery.request_id;
        recovery.capacity_request.session_id = recovery.session_id;
        recovery.capacity_request.client_nonce = recovery.client_nonce;
        recovery.capacity_request.funding_model = FundingModel::USER_PAID;
        recovery.capacity_request.requires_carrier =
            recovery.selected_service_fee.value > 0 &&
            recovery.selected_service_fee.value < 100;
        recovery.capacity_request.requested_slots = 1;
        recovery.capacity_request.created_at = now;
        recovery.capacity_request.expires_at = std::min(
            selected->announcement_expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        recovery.phase = AlternativeRecoveryPhase::CAPACITY_PENDING;
        recovery.created_at = recovery.updated_at = now;
        if (recovery.capacity_request.expires_at <= now ||
            !store.PrepareAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_CAPACITY_REQUEST_EXPIRED" : error);
        }
    } else {
        if (recovery.expired || recovery.provider_side ||
            recovery.original_provider_id != original_attempt.provider_id ||
            recovery.original_commit_key != original_attempt.commit_key ||
            recovery.original_template_commitment !=
                original_attempt.template_commitment ||
            (requested_provider &&
             recovery.recovery_provider_id != *requested_provider) ||
            (requested_offer && recovery.offer_id != *requested_offer)) {
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
        }
        const auto& maximum_value = options.maximum_fee;
        const bool durable_authorization =
            !recovery.accepted_recovery_authorization_commitment.IsNull() &&
            recovery.accepted_recovery_authorization_commitment ==
                recovery.recovery_authorization.authorization_commitment &&
            recovery.recovery_authorization_accepted_at > 0;
        const bool inconsistent_authorization =
            recovery.accepted_recovery_authorization_commitment.IsNull() !=
                (recovery.recovery_authorization_accepted_at == 0) ||
            (!recovery.accepted_recovery_authorization_commitment.IsNull() &&
             recovery.accepted_recovery_authorization_commitment !=
                 recovery.recovery_authorization.authorization_commitment);
        if (inconsistent_authorization) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_RECOVERY_AUTHORIZATION_CONFLICT");
        }
        if (maximum_value.has_value() && !durable_authorization) {
            const DDCents effective = EffectiveClientServiceFeeCap(
                wallet, DDCents{*maximum_value}, error);
            if (effective.value < 0 ||
                !(effective == recovery.selected_maximum_service_fee)) {
                throw ApplicationError(
                    ErrorKind::INVALID_INPUT,
                    "PAYMASTER_RECOVERY_FEE_LIMIT_CHANGED");
            }
        }
    }

    const EquivocationBlockPolicy recovery_block_policy =
        static_cast<uint8_t>(recovery.phase) <
                static_cast<uint8_t>(AlternativeRecoveryPhase::USER_SIGNED) ?
            EquivocationBlockPolicy::ENFORCE :
            EquivocationBlockPolicy::OBSERVE_ONLY;
    if (!DrainAlternativeRecoveryCapacityEquivocations(
            *context.paymaster, store, recovery, error,
            recovery_block_policy)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }

    // Recovery is now durable and bound to this original attempt. Its separate
    // channel must not wait behind the original submit's unanswered lease when
    // the node has only one outgoing slot. Keep signed artifacts/reservations.
    node->connman->ReleasePaymasterConnection(
        PaymentChannelKey(wallet, session, original_attempt.provider_id));

    const auto make_result = [&](const AlternativeRecoveryRecord& record,
                                 const RecoveryQueueState& queue,
                                 std::optional<bool> broadcast = std::nullopt,
                                 const std::string& broadcast_error = {}) {
        PaymentSession current_session;
        if (!store.GetSessionByRequestId(record.request_id,
                                         current_session)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_SESSION_NOT_FOUND");
        }
        // Every resolve mutation returns the same authoritative envelope as a
        // refresh. Callers can therefore replace the complete snapshot and
        // never retain an artifact from an older recovery phase.
        ClientRecoveryResult result = CaptureClientSession(
            store, current_session, "cancel_to_self");
        result.queued = queue.queued;
        result.connection_pending = queue.connection_pending;
        result.route_available = queue.route_available;
        if (broadcast) result.broadcast = *broadcast;
        if (!broadcast_error.empty()) {
            result.broadcast_error = broadcast_error;
        }
        return result;
    };
    if (recovery.phase == AlternativeRecoveryPhase::CAPACITY_PENDING) {
        auto proof_lease = context.paymaster->LeaseCapacityProofs(
            recovery.recovery_provider_id, recovery.client_nonce,
            MAX_DIRECT_INBOX_MESSAGES_PER_SESSION);
        const auto proofs = SelectCapacityProofMessages(
            proof_lease.Messages(), recovery.recovery_provider_id,
            recovery.capacity_request.request_id,
            recovery.capacity_request.session_id, recovery.client_nonce);
        if (proofs.empty()) {
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.capacity_request, now,
                    queue, error)) {
                throw ApplicationError(ErrorKind::NODE_CAPACITY, error);
            }
            return make_result(recovery, queue);
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(
                    *context.paymaster, proofs, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_CAPACITY_EQUIVOCATION");
        }
        std::optional<PaymasterCapacityProof> accepted_proof;
        std::string rejected_proof_error;
        for (const DirectMessage& direct : proofs) {
            const auto* proof =
                std::get_if<PaymasterCapacityProof>(&direct.payload);
            if (!proof) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                rejected_proof_error = "PAYMASTER_INVALID_CAPACITY_PROOF";
                continue;
            }

            bool claim_equivocation{false};
            const std::vector<unsigned char> encoded_proof =
                SerializeCapacityProof(*proof);
            if (!store.StageAlternativeRecoveryCapacityProofClaimCandidate(
                    recovery.recovery_id, encoded_proof, direct.received_at,
                    claim_equivocation, error)) {
                if (error != "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE") {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                rejected_proof_error =
                    "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(
                        *context.paymaster, proofs, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_CAPACITY_EQUIVOCATION");
            }
            if (!store.GetAlternativeRecoveryById(recovery.recovery_id,
                                                  recovery)) {
                throw ApplicationError(ErrorKind::WALLET,
                                   "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND");
            }

            std::string proof_error;
            if (!ValidateCapacityProofAgainstChainstate(
                    *proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key, *node->chainman,
                    now, proof_error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                rejected_proof_error =
                    proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
                continue;
            }
            accepted_proof = *proof;
            break;
        }
        if (!accepted_proof) {
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                rejected_proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : rejected_proof_error);
        }
        const PaymasterCapacityProof& proof = *accepted_proof;
        const std::vector<unsigned char> first_capacity_proof =
            SerializeCapacityProof(proof);
        recovery.capacity_snapshot.snapshot_id = proof.snapshot_id;
        recovery.capacity_snapshot.resource_commitment =
            GetCapacityResourceCommitment(proof);
        recovery.capacity_snapshot.session_id = recovery.session_id;
        recovery.capacity_snapshot.attempt_id = recovery.recovery_id;
        recovery.capacity_snapshot.provider_id = proof.provider_id;
        recovery.capacity_snapshot.client_nonce = proof.client_nonce;
        recovery.capacity_snapshot.funding_model = proof.funding_model;
        recovery.capacity_snapshot.requires_carrier =
            proof.requires_carrier;
        recovery.capacity_snapshot.request_hash =
            Hash(SerializeCapacityRequest(recovery.capacity_request));
        recovery.capacity_snapshot.capacity_proof = first_capacity_proof;
        recovery.capacity_snapshot.created_at = proof.created_at;
        recovery.capacity_snapshot.expires_at = proof.expires_at;
        recovery.capacity_snapshot.validated_at = now;

        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        CAmount total_dd{0};
        if (!dd_wallet) {
            throw ApplicationError(ErrorKind::WALLET,
                               "DigiDollar wallet not initialized");
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        CAmount remaining = total_dd - recovery.selected_service_fee.value;
        if (remaining < 100) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_RECOVERY_RETURN_TOO_SMALL");
        }
        std::vector<AlternativeRecoveryReturn> returns;
        while (remaining > 0) {
            CAmount chunk = std::min<CAmount>(remaining,
                                              MAX_DD_OUTPUT_CENTS);
            if (remaining > MAX_DD_OUTPUT_CENTS &&
                remaining - chunk < 100) {
                chunk = remaining - 100;
            }
            const auto destination = wallet.GetNewDestination(
                OutputType::BECH32M, "Paymaster alternative recovery");
            if (!destination || chunk < 100 ||
                chunk > MAX_DD_OUTPUT_CENTS) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    "PAYMASTER_RECOVERY_DESTINATION_UNAVAILABLE");
            }
            returns.push_back(
                {GetScriptForDestination(*destination), DDCents{chunk}});
            remaining -= chunk;
        }
        if (returns.size() > MAX_PAYMENT_INTENT_INPUTS) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_RECOVERY_TOO_MANY_RETURNS");
        }

        AlternativeRecoveryRequest recovery_request;
        recovery_request.genesis_hash =
            Params().GenesisBlock().GetHash();
        recovery_request.request_id = recovery.request_id;
        recovery_request.session_id = recovery.session_id;
        recovery_request.original_provider_id =
            recovery.original_provider_id;
        recovery_request.recovery_provider_id =
            recovery.recovery_provider_id;
        recovery_request.privacy_profile = recovery.privacy_profile;
        recovery_request.offer_id = recovery.offer_id;
        recovery_request.policy_hash = recovery.policy_hash;
        recovery_request.original_commit_key =
            recovery.original_commit_key;
        recovery_request.original_template_commitment =
            recovery.original_template_commitment;
        recovery_request.client_nonce = recovery.client_nonce;
        recovery_request.capacity_request = recovery.capacity_request;
        recovery_request.capacity_snapshot_id = proof.snapshot_id;
        recovery_request.capacity_resource_commitment =
            recovery.capacity_snapshot.resource_commitment;
        recovery_request.user_dd_inputs = session.user_inputs;
        recovery_request.wallet_returns = std::move(returns);
        recovery_request.maximum_service_fee =
            recovery.selected_maximum_service_fee;
        recovery_request.service_fee = recovery.selected_service_fee;
        recovery_request.created_at = now;
        recovery_request.expires_at = std::min(
            proof.expires_at,
            SaturatingAddSeconds(now, MAX_DIRECT_MESSAGE_TTL_SECONDS));
        // Recheck the capacity proof at the last possible point before the
        // first payment-revealing signatures are produced.
        if (wallet.IsLocked()) {
            throw ApplicationError(
                ErrorKind::UNLOCK,
                "Wallet unlock is required for alternative recovery");
        }
        if (!DrainPendingAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, first_capacity_proof,
                error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        std::vector<XOnlyPubKey> output_keys;
        if (!ValidateCapacityProofAgainstChainstate(
                proof, recovery.capacity_request,
                recovery.recovery_provider_identity_key,
                *node->chainman, now, error) ||
            !SignAlternativeRecoveryRequestInputs(
                wallet, recovery_request, now, output_keys, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        recovery.recovery_request = std::move(recovery_request);
        recovery.recovery_request_hash =
            GetAlternativeRecoveryRequestHash(recovery.recovery_request);
        recovery.phase = AlternativeRecoveryPhase::REQUEST_READY;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::REQUEST_READY) {
        // Authenticate local authority before interpreting a peer reply. A
        // damaged request must neither discard a valid quote nor grant it
        // spending authority.
        if (!paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(recovery, error) ||
            recovery.capacity_request.genesis_hash != Params().GenesisBlock().GetHash()) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RECOVERY_DATABASE_READ");
        }
        const auto response_lease = context.paymaster->LeaseRecoveryResponses(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        const auto& responses = response_lease.Messages();
        if (responses.empty()) {
            PaymasterCapacityProof proof;
            if (!DeserializeCanonicalRecoveryMessage(
                    recovery.capacity_snapshot.capacity_proof, proof, error) ||
                !ValidateCapacityProofAgainstChainstate(
                    proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key,
                    *node->chainman, now, error)) {
                throw ApplicationError(ErrorKind::INVALID_INPUT, error);
            }
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.recovery_request, now,
                    queue, error)) {
                throw ApplicationError(ErrorKind::NODE_CAPACITY, error);
            }
            return make_result(recovery, queue);
        }
        const auto* response =
            std::get_if<AlternativeRecoveryResponse>(
                &responses.front().payload);
        AlternativeRecoveryRecord candidate{recovery};
        if (!response ||
            !ValidateAlternativeRecoveryResponse(
                *response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, now, error)) {
            const std::string rejection = error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESPONSE" : error;
            if (!AcknowledgeRejectedDirectMessage(*context.paymaster, responses.front(), error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                rejection);
        }
        candidate.recovery_response = *response;
        candidate.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
        candidate.updated_at = std::max(candidate.updated_at, now);
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, candidate, parameters, error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                *response, candidate.recovery_request,
                candidate.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error) ||
            !BuildRecoveryAuthorizationManifest(
                candidate.recovery_request, *response,
                candidate.recovery_authorization, error) ||
            !store.UpdateAlternativeRecovery(candidate, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        if (!context.paymaster->AcknowledgeDirectMessagesIfPresent({responses.front().message_id})) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DIRECT_INBOX_ACK_CONFLICT");
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
        if (!CheckPaymasterRecoveryServiceFee(wallet, recovery.recovery_response.manifest, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        node->connman->ReleasePaymasterConnection({wallet.m_paymaster_transport_owner.GetHex(),
            "recovery:" + recovery.recovery_id.GetHex(), true, recovery.recovery_provider_id});
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response, now,
                error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error)) {
            throw ApplicationError(ErrorKind::REJECTED, error);
        }
        if (prepare_only) {
            return make_result(recovery, RecoveryQueueState{});
        }
        if (!accepted_commitment) {
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_REQUIRED");
        }
        if (*accepted_commitment !=
            recovery.recovery_authorization.authorization_commitment) {
            throw ApplicationError(
                ErrorKind::INVALID_INPUT,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH");
        }
        if (wallet.IsLocked()) {
            throw ApplicationError(
                ErrorKind::UNLOCK,
                "Wallet unlock is required for alternative recovery");
        }
        // Persist the exact user authorization and reserve its client fee
        // before invoking any wallet signer. Reload and revalidate the
        // immutable recovery template after that transaction commits.
        if (!store.AcceptAlternativeRecoveryAuthorization(
                session.request_id, *accepted_commitment, now, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery) ||
            !BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateRecoveryAuthorizationManifest(
                recovery.recovery_authorization,
                recovery.recovery_request, recovery.recovery_response, now,
                error) ||
            !ValidateAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, now, trusted, unsigned_psbt,
                error)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_RECOVERY_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
        {
            // Serialize policy edits with the actual signature, without holding
            // the wallet lock while querying chainstate below.
            LOCK(wallet.cs_wallet);
            now = GetTime();
            AlternativeRecoveryRecord current;
            if (!store.GetAlternativeRecovery(session.request_id, current) ||
                paymaster_store::internal::CanonicalBytes(current) != paymaster_store::internal::CanonicalBytes(recovery) ||
                current.recovery_authorization_accepted_at <= 0 ||
                now < current.updated_at ||
                !ValidateRecoveryAuthorizationManifest(current.recovery_authorization,
                    current.recovery_request, current.recovery_response, now, error) ||
                !CheckPaymasterRecoveryServiceFee(wallet, recovery.recovery_response.manifest, error) ||
                !SignCollaborativePSBTForParty(wallet, unsigned_psbt, trusted.trusted_template,
                    SigningParty::USER, std::min(recovery.recovery_response.expires_at,
                        recovery.capacity_snapshot.expires_at) - 1, error)) {
                throw ApplicationError(ErrorKind::WALLET,
                    error.empty() ? "PAYMASTER_RECOVERY_AUTHORIZATION_STATE" : error);
            }
            recovery.user_signed_psbt = SerializePSBT(unsigned_psbt);
            recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
            recovery.updated_at = now;
            if (!store.UpdateAlternativeRecovery(recovery, error) ||
                !store.GetAlternativeRecovery(session.request_id, recovery)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
        }
        if (!ValidateAlternativeRecoveryPSBTAgainstChainstate(
                unsigned_psbt, trusted, parameters, Params(),
                *node->chainman, now,
                CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw ApplicationError(ErrorKind::WALLET, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::USER_SIGNED) {
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        if (authorization_time <= 0) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_RECOVERY_AUTHORIZATION_REQUIRED");
        }
        const int64_t observation_time{
            std::max(now, authorization_time)};
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        AlternativeRecoverySubmit submit;
        submit.genesis_hash = Params().GenesisBlock().GetHash();
        submit.request_id = recovery.request_id;
        submit.session_id = recovery.session_id;
        submit.recovery_id = recovery.recovery_id;
        submit.recovery_provider_id = recovery.recovery_provider_id;
        submit.recovery_request_hash = recovery.recovery_request_hash;
        submit.recovery_commit_key =
            recovery.recovery_response.recovery_commit_key;
        submit.template_commitment =
            recovery.recovery_response.manifest.template_commitment;
        submit.user_psbt = recovery.user_signed_psbt;
        const auto result_lease = context.paymaster->LeaseRecoveryResults(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        const auto& messages = result_lease.Messages();
        const AlternativeRecoveryResultMessage* message{nullptr};
        CTransactionRef observed_final;
        FinalTransactionPresence observed_presence{
            FinalTransactionPresence::NONE};
        std::optional<ExactFinalTransactionProof> observed_proof;
        if (!messages.empty()) {
            message = std::get_if<AlternativeRecoveryResultMessage>(
                &messages.front().payload);
            // The accepted manifest can authenticate a result for retention
            // even when another local recovery artifact is damaged. This
            // grants no execution authority; the complete record and final
            // transaction still have to pass the checks below.
            const auto& authorization = recovery.recovery_authorization;
            std::string retention_error;
            if (message &&
                authorization.version == RecoveryAuthorizationManifest::CURRENT_VERSION &&
                authorization.authorization_commitment == recovery.accepted_recovery_authorization_commitment &&
                authorization.authorization_commitment == GetRecoveryAuthorizationCommitment(authorization) &&
                authorization.request_id == recovery.request_id &&
                authorization.session_id == recovery.session_id &&
                authorization.recovery_provider_id == recovery.recovery_provider_id &&
                message->recovery_id == authorization.recovery_id &&
                message->recovery_request_hash == authorization.recovery_request_hash &&
                ValidateAlternativeRecoveryResultEnvelope(*message, Params().GenesisBlock().GetHash(), now, retention_error) &&
                ValidatePaymasterResult(message->result, Params().GenesisBlock().GetHash(),
                    authorization.recovery_provider_id, authorization.recovery_commit_key,
                    recovery.recovery_provider_identity_key, 1, retention_error) &&
                !context.paymaster->RetainAuthenticatedResult(messages.front())) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DIRECT_INBOX_RETAIN_CONFLICT");
            }
            // Local corruption must not turn a valid remote result into an
            // acknowledged binding failure. Validate at historical creation
            // times so an already signed recovery survives quote expiry.
            if (!paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(recovery, error) ||
                recovery.capacity_request.genesis_hash != Params().GenesisBlock().GetHash()) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_RECOVERY_DATABASE_READ");
            }
            if (!message || !ValidateAlternativeRecoveryResult(*message, recovery.recovery_response, recovery.recovery_provider_identity_key, Params().GenesisBlock().GetHash(), 1, now, error) ||
                !message->result.final_transaction ||
                !message->result.raw_transaction_hash) {
                const std::string rejection{error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESULT" : error};
                if (!AcknowledgeRejectedDirectMessage(*context.paymaster, messages.front(), error)) {
                    throw ApplicationError(ErrorKind::WALLET, error);
                }
                throw ApplicationError(
                    ErrorKind::INVALID_INPUT,
                    rejection);
            }
            if (!context.paymaster->RetainAuthenticatedResult(messages.front())) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DIRECT_INBOX_RETAIN_CONFLICT");
            }
            observed_final = MakeTransactionRef(
                *message->result.final_transaction);
            if (!PreflightFinalPaymasterTransaction(
                    wallet, observed_final, observed_presence, error,
                    ExactFinalTxIndexMode::WAIT_FOR_SYNC, &observed_proof)) {
                throw ApplicationError(IsTransientPaymasterFinalizationError(error)
                    ? ErrorKind::WALLET : ErrorKind::REJECTED, error);
            }
        }
        if (!message) {
            // A lost result does not imply that the provider failed to finish
            // the recovery. As for an ordinary submit retry, only local exact
            // chain/mempool evidence can permit retrying already spent inputs.
            // This observation never fabricates a provider-signed result.
            observed_final = FindLocalPaymasterFinal(
                wallet, recovery.recovery_response.manifest.unsigned_txid);
            if (observed_final && !PreflightFinalPaymasterTransaction(
                    wallet, observed_final, observed_presence, error,
                    ExactFinalTxIndexMode::NONBLOCKING, &observed_proof)) {
                throw ApplicationError(IsTransientPaymasterFinalizationError(error)
                    ? ErrorKind::WALLET : ErrorKind::REJECTED, error);
            }
        }
        const auto* final_proof = observed_proof ? &*observed_proof : nullptr;
        PartiallySignedTransaction user_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                final_proof, trusted, unsigned_psbt, error) ||
            !ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
                submit, recovery.recovery_response, trusted, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                final_proof, user_psbt, error)) {
            throw ApplicationError(IsTransientPaymasterFinalizationError(error)
                ? ErrorKind::WALLET : ErrorKind::REJECTED, error);
        }

        if (!message && observed_final &&
            !ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                CMutableTransaction{*observed_final}, observed_final->GetWitnessHash(),
                trusted, parameters, Params(), *node->chainman,
                authorization_time, observation_time, final_proof, error)) {
            throw ApplicationError(IsTransientPaymasterFinalizationError(error)
                ? ErrorKind::WALLET : ErrorKind::REJECTED, error);
        }

        if (message) {
            const CMutableTransaction& final_transaction =
                *message->result.final_transaction;
            const CTransactionRef& final_ref = observed_final;
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, *message->result.raw_transaction_hash,
                    trusted, parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    final_proof, error)) {
                throw ApplicationError(IsTransientPaymasterFinalizationError(error)
                    ? ErrorKind::WALLET : ErrorKind::REJECTED, error);
            }
            recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
            recovery.final_transaction =
                SerializeTransaction(final_transaction);
            recovery.expected_wtxid =
                CTransaction{final_transaction}.GetWitnessHash();
            recovery.signed_result = message->result;
            recovery.updated_at = std::max(recovery.updated_at, now);
            SelfRecoveryRecord final_recovery;
            final_recovery.request_id = recovery.request_id;
            final_recovery.session_id = recovery.session_id;
            final_recovery.user_inputs =
                recovery.recovery_request.user_dd_inputs;
            final_recovery.recovery_txid =
                CTransaction{final_transaction}.GetHash();
            final_recovery.raw_transaction_hash =
                Hash(recovery.final_transaction);
            final_recovery.final_transaction = recovery.final_transaction;
            final_recovery.created_at = now;
            if (!store.CommitClientAlternativeRecoveryFinal(
                    recovery, final_recovery, error)) {
                throw ApplicationError(ErrorKind::WALLET, error);
            }
            if (!context.paymaster->AcknowledgeDirectMessagesIfPresent({messages.front().message_id})) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DIRECT_INBOX_ACK_CONFLICT");
            }

            node->connman->ReleasePaymasterConnection({wallet.m_paymaster_transport_owner.GetHex(),
                "recovery:" + recovery.recovery_id.GetHex(), true, recovery.recovery_provider_id});

            ExecuteDurableClientFinal(wallet, final_ref->GetHash(), now, context.paymaster);
            return make_result(recovery, RecoveryQueueState{}, true);
        }
        // Process/retain a received signed result before local completion can
        // fail. Without a result, deep local evidence replaces transport retry.
        bool completed{false};
        if (!store.CompleteClientConfirmedFinal(session.request_id, now, completed, error))
            throw ApplicationError(ErrorKind::WALLET, error);
        if (completed) {
            if (!store.GetSessionByRequestId(session.request_id, session))
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DATABASE_READ");
            return CaptureClientSession(store, session, "cancel_to_self");
        }
        RecoveryQueueState queue;
        if (!QueueRecoveryMessage(context, wallet, recovery, submit, now,
                                  queue, error)) {
            throw ApplicationError(ErrorKind::NODE_CAPACITY, error);
        }
        return make_result(recovery, queue);
    }

    if (recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED) {
        if (!recovery.signed_result.txid) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PERSISTED_RECOVERY_CORRUPT");
        }
        ExecuteDurableClientFinal(wallet, *recovery.signed_result.txid, now, context.paymaster);
        return make_result(recovery, RecoveryQueueState{}, true);
    }
    throw ApplicationError(ErrorKind::WALLET,
                       "PAYMASTER_ALTERNATIVE_RECOVERY_STATE_INVALID");
}


ClientRecoveryResult ResolvePaymasterSession(WalletContext& context, CWallet& target,
    const ClientSessionLookup& lookup, const std::string& action, const ClientRecoveryOptions& options)
{
    CWallet* const wallet = &target;
    if (action != "refresh" && action != "retry_same" && action != "fallback" &&
        action != "abandon_unsigned" && action != "cancel_to_self")
        throw ApplicationError(ErrorKind::INVALID_INPUT,
            "action must be refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self");
    PaymasterStore store{*wallet};
    DigiDollar::Paymaster::PaymentSession session;
    if (!FindClientSession(lookup, store, session)) {
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND",
            "PAYMASTER_SESSION_NOT_FOUND: Paymaster session not found", ErrorDisposition::RECONCILE, true);
    }
    if (action == "refresh") wallet->BlockUntilSyncedToCurrentChain();
    if (context.paymaster && context.paymaster->Enabled()) {
        std::string equivocation_error;
        if (!DrainClientSessionEquivocations(
                *context.paymaster, store, session,
                equivocation_error,
                EquivocationBlockPolicy::OBSERVE_ONLY)) {
            throw ApplicationError(ErrorKind::WALLET,
                               equivocation_error);
        }
    }
    {
        // Re-read under one recursive wallet lock after inbox
        // observation. The same authoritative action set exposed to
        // Qt is also the fail-closed gate for direct CLI mutations.
        LOCK(wallet->cs_wallet);
        if (!FindClientSession(lookup, store,
                         session)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND",
                "PAYMASTER_SESSION_NOT_FOUND: Paymaster session not found", ErrorDisposition::RECONCILE, true);
        }
        if (action == "refresh") {
            // Complete local chain evidence outside the wallet lock below.
        } else {
            ClientSessionView view;
            std::string view_error;
            if (!LoadClientSessionView(store, session, GetTime(), view, view_error)) {
                throw ApplicationError(ErrorKind::WALLET, view_error);
            }
            if (std::find(view.allowed_actions.begin(), view.allowed_actions.end(), action) == view.allowed_actions.end()) {
                throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_ACTION_NOT_ALLOWED");
            }
        }
    }
    if (action == "refresh") {
        bool locally_completed{false};
        std::string completion_error;
        if (!store.CompleteClientConfirmedFinal(session.request_id, GetTime(), locally_completed, completion_error))
            throw ApplicationError(ErrorKind::WALLET, completion_error);
        if (locally_completed && !store.GetSessionByRequestId(session.request_id, session))
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DATABASE_READ");
        std::string history_error;
        if (!store.ReconcileClientPaymentHistory(session.request_id, history_error))
            wallet->WalletLogPrintf("Paymaster refreshed payment history update deferred: %s\n", history_error);
        return CaptureClientSession(store, session, action);
    }
    if (action == "abandon_unsigned") {
        std::string error;
        if (!store.AbandonUnsignedClientSession(
                session.request_id, GetTime(), error) ||
            !store.GetSessionByRequestId(session.request_id, session)) {
            throw ApplicationError(
                ErrorKind::WALLET,
                error.empty() ? "PAYMASTER_UNSIGNED_ABANDON_FAILED" : error);
        }
        if (const auto* node = wallet->chain().context(); node && node->connman) {
            node->connman->CancelPaymasterOperation(wallet->m_paymaster_transport_owner.GetHex(), session.session_id.GetHex() + ":");
        }
        return CaptureClientSession(store, session, action);
    }

    if (session.attempt_ids.empty()) {
        throw ApplicationError(ErrorKind::WALLET, "Paymaster session has no provider attempt");
    }
    uint256 attempt_id = session.attempt_ids.back();
    if (options.attempt_id.has_value()) {
        attempt_id = *options.attempt_id;
        if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
            session.attempt_ids.end()) {
            throw ApplicationError(ErrorKind::INVALID_INPUT,
                               "attempt_id does not belong to this session");
        }
    }
    DigiDollar::Paymaster::ProviderAttempt attempt;
    if (!store.GetAttempt(attempt_id, attempt)) {
        throw ApplicationError(ErrorKind::WALLET, "Paymaster attempt not found");
    }
    if (action == "cancel_to_self" && attempt_id == session.attempt_ids.back() &&
        ReconcileConfirmedClientPayment(*wallet, session, attempt)) {
        // A lost provider reply does not make an already confirmed
        // payment recoverable. Validate and settle that exact payment
        // before testing its now-spent inputs or preparing recovery.
        return CaptureClientSession(store, session, action);
    }
    if (action == "retry_same" && attempt_id == session.attempt_ids.back() &&
        context.paymaster && context.paymaster->Enabled()) {
        if (!attempt.user_signed_psbt.empty() &&
            attempt.final_transaction.empty()) {
            // Collect an already received result before rechecking
            // Capacity that may have been spent by this exact payment.
            const auto received = ProcessPaymasterResult(context, *wallet, session.request_id);
            if (received.processed.value_or(false)) {
                if (!store.GetSessionByRequestId(session.request_id, session))
                    throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_DATABASE_READ");
                return CaptureClientSession(store, session, action);
            }
        }
    }
    if (action == "cancel_to_self") {
        if (options.retry_transport) {
            DigiDollar::Paymaster::AlternativeRecoveryRecord recovery;
            if (const auto* node = wallet->chain().context(); node && node->connman &&
                store.GetAlternativeRecovery(session.request_id, recovery)) {
                node->connman->RetryPaymasterConnection({wallet->m_paymaster_transport_owner.GetHex(),
                    "recovery:" + recovery.recovery_id.GetHex(), true, recovery.recovery_provider_id});
            }
        }
        return ResolveAlternativePaymasterRecovery(
            context, *wallet, store, session, attempt, options);
    }
    if (action == "fallback") {
        std::string error;
        if (!store.AbandonClientAttemptForFallback(session.request_id, attempt_id,
                                                   GetTime(), error) ||
            !store.GetSessionByRequestId(session.request_id, session) ||
            !store.GetAttempt(attempt_id, attempt)) {
            throw ApplicationError(ErrorKind::WALLET,
                               error.empty() ? "PAYMASTER_FALLBACK_FAILED" : error);
        }
        if (const auto* node = wallet->chain().context(); node && node->connman) {
            node->connman->CancelPaymasterOperation(wallet->m_paymaster_transport_owner.GetHex(), session.session_id.GetHex() + ":");
        }
        return CaptureClientSession(store, session, action);
    }

    if (!attempt.final_transaction.empty() &&
        !attempt.final_txid.IsNull()) {
        bool broadcast{false};
        std::string broadcast_error;
        if (action == "retry_same") {
            ExecuteDurableClientFinal(*wallet, attempt.final_txid, GetTime(), context.paymaster);
            broadcast = true;
        }
        if (!store.GetSessionByRequestId(session.request_id,
                                         session)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_SESSION_DATABASE_READ");
        }
        ClientRecoveryResult result = CaptureClientSession(
            store, session, action);
        std::string history_error;
        if (!store.ReconcileClientPaymentHistory(session.request_id, history_error))
            wallet->WalletLogPrintf("Paymaster retried payment history update deferred: %s\n", history_error);
        result.raw_transaction = attempt.final_transaction;
        result.txid = attempt.final_txid;
        result.broadcast = broadcast;
        if (!broadcast_error.empty()) {
            result.broadcast_error = broadcast_error;
        }
        return result;
    }
    if (attempt.retry_until > 0 && GetTime() > attempt.retry_until) {
        throw ApplicationError(ErrorKind::WALLET, "Paymaster retry window expired");
    }
    if (!attempt.user_signed_psbt.empty()) {
        PaymasterSubmitQueueState queue_state;
        if (action == "retry_same") {
            if (const auto* node = wallet->chain().context(); node && node->connman) {
                node->connman->RetryPaymasterConnection(PaymentChannelKey(*wallet, session, attempt.provider_id));
            }
            std::string error;
            if (!QueuePersistedPaymasterSubmit(context, *wallet, session, attempt,
                                               GetTime(), queue_state, error)) {
                throw ApplicationError(
                    error == "PAYMASTER_DIRECT_QUEUE_FULL" ? ErrorKind::NODE_CAPACITY : ErrorKind::WALLET,
                    error);
            }
        }
        if (!store.GetSessionByRequestId(session.request_id,
                                         session)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_SESSION_DATABASE_READ");
        }
        ClientRecoveryResult result = CaptureClientSession(
            store, session, action);
        result.psbt = attempt.user_signed_psbt;
        if (action == "retry_same") {
            result.queued = queue_state.queued;
            result.connection_pending = queue_state.connection_pending;
            result.route_available = queue_state.route_available;
        }
        return result;
    } else {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_RETRY_ARTIFACT_UNAVAILABLE");
    }
}
} // namespace wallet::paymaster_app
