// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Client session, PSBT, submit, result, and recovery RPCs. */

#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>

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
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>

#include <univalue.h>
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

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_rpc::internal;

RPCHelpMan getdigidollarsendsession()
{
    return RPCHelpMan{
        "getdigidollarsendsession",
        "Return one persistent DigiDollar send session without creating a new payment attempt.\n",
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent 256-bit session identifier"},
                                                                                                             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The authoritative persistent session", {
                                                                                         {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                         {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                         {RPCResult::Type::STR_HEX, "canonical_request_hash", "Hash of the canonical authorized request"},
                                                                                         {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original wallet-local amount; exact total outflow in subtract mode"},
                                                                                         {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the service fee is deducted from requested_amount_cents"},
                                                                                         {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the session is bound to all ordinary spendable DD"},
                                                                                         {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
                                                                                         {RPCResult::Type::STR, "fee_mode_used", "Persisted effective fee mode"},
                                                                                         {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider bound by the latest durable client authorization"},
                                                                                         {RPCResult::Type::STR_HEX, "offer_id", /*optional=*/true, "Offer bound by the latest durable client authorization"},
                                                                                         {RPCResult::Type::STR_HEX, "policy_hash", /*optional=*/true, "Provider policy bound by the latest durable client authorization"},
                                                                                         {RPCResult::Type::STR, "funding_model", /*optional=*/true, "Exact sponsored or user_paid funding model"},
                                                                                         {RPCResult::Type::NUM, "payment_cents", /*optional=*/true, "Exact recipient amount from the latest durable client authorization"},
                                                                                         {RPCResult::Type::NUM, "service_fee_cents", /*optional=*/true, "Exact rounded provider service fee"},
                                                                                         {RPCResult::Type::NUM, "user_total_cents", /*optional=*/true, "Exact recipient amount plus service fee"},
                                                                                         {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                        {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Persisted phase for PENDING_PROVIDER"},
                                                                                        {RPCResult::Type::BOOL, "final", "Whether the state is terminal"},
                                                                                        {RPCResult::Type::STR, "broadcast_state", "not_attempted, unknown, accepted_mempool, accepted_stempool, or confirmed"},
                                                                                        {RPCResult::Type::STR, "confirmation_state", "unconfirmed, payment_confirmed, recovery_confirmed, or conflicted"},
                                                                                        {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
                                                                                        {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
                                                                                        {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction id"},
                                                                                        {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known idempotent self-recovery transaction id"},
                                                                                        {RPCResult::Type::ARR, "reserved_user_inputs", "Immutable user input set", {
                                                                                                                                                                       {RPCResult::Type::OBJ, "", "A reserved user input", {
                                                                                                                                                                                                                               {RPCResult::Type::STR_HEX, "txid", "Creating transaction id"},
                                                                                                                                                                                                                               {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                           }},
                                                                                                                                                                   }},
                                                                                        {RPCResult::Type::NUM, "provider_attempts", "Number of persistent provider attempts"},
                                                                                    }},
        RPCExamples{HelpExampleCli("getdigidollarsendsession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}'")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;

            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            return SessionToJSON(session, &store);
        },
    };
}

UniValue ResolveAlternativePaymasterRecovery(
    WalletContext& context,
    CWallet& wallet,
    PaymasterStore& store,
    DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& original_attempt,
    const UniValue& options)
{
    using namespace DigiDollar::Paymaster;
    std::string error;
    const int64_t now{GetTime()};
    if (!CheckPaymasterClientReadiness(wallet, context, error) ||
        !CheckPaymasterPrivacyReadiness(
            original_attempt.privacy_profile, context, error)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }
    node::NodeContext* node = wallet.chain().context();
    if (!node || !node->connman || !node->chainman || !context.paymaster) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
    }

    const UniValue& provider_value =
        options.find_value("recovery_provider_id");
    const UniValue& offer_value = options.find_value("recovery_offer_id");
    const std::optional<PaymasterId> requested_provider =
        provider_value.isNull() ? std::nullopt : std::optional<PaymasterId>{ParseHashO(options, "recovery_provider_id")};
    const std::optional<uint256> requested_offer =
        offer_value.isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "recovery_offer_id")};
    const bool prepare_only =
        !options.find_value("prepare_only").isNull() &&
        options.find_value("prepare_only").get_bool();
    const UniValue& commitment_value =
        options.find_value("recovery_authorization_commitment");
    const std::optional<uint256> accepted_commitment =
        commitment_value.isNull() ? std::nullopt : std::optional<uint256>{ParseHashO(options, "recovery_authorization_commitment")};

    AlternativeRecoveryRecord recovery;
    if (!store.GetAlternativeRecovery(session.request_id, recovery)) {
        if (session.state != SessionState::PENDING_PROVIDER ||
            original_attempt.provider_id.IsNull() ||
            original_attempt.commit_key.IsNull() ||
            original_attempt.template_commitment.IsNull() ||
            !((original_attempt.state >= AttemptState::USER_SIGNED &&
               original_attempt.state <= AttemptState::MEMPOOL) ||
              original_attempt.state == AttemptState::AMBIGUOUS)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_ALLOWED");
        }
        DigiDollarWallet* dd_wallet = wallet.GetDDWallet();
        if (!dd_wallet) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "DigiDollar wallet not initialized");
        }
        CAmount total_dd{0};
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        const UniValue& maximum_value =
            options.find_value("maximum_recovery_service_fee_cents");
        const DDCents requested_maximum{
            maximum_value.isNull() ? MAX_DD_OUTPUT_CENTS : maximum_value.getInt<int64_t>()};
        const DDCents effective_maximum =
            EffectiveClientServiceFeeCap(wallet, requested_maximum, error);
        if (effective_maximum.value < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
        }

        std::vector<PaymasterReliabilityRecord> reliability_records;
        if (!store.ListProviderReliability(reliability_records)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_REPUTATION_DATABASE_READ");
        }
        std::map<PaymasterId, PaymasterReliabilityRecord> reliability;
        for (auto& record : reliability_records) {
            reliability.emplace(record.provider_id, std::move(record));
        }
        const auto candidates = BuildOfferCandidates(
            context.paymaster->GetDirectory().List(now), DDCents{total_dd},
            FUNDING_MODEL_USER_PAID, effective_maximum, reliability, now,
            error);
        if (!error.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, error);
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
                       (original_attempt.privacy_profile !=
                            PrivacyProfile::HIGH ||
                        candidate.endpoint.IsTor()) &&
                       total_dd - candidate.service_fee.value >= 100;
            });
        if (selected == candidates.end()) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
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
            throw JSONRPCError(
                RPC_WALLET_ERROR,
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
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT");
        }
        const UniValue& maximum_value =
            options.find_value("maximum_recovery_service_fee_cents");
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
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_RECOVERY_AUTHORIZATION_CONFLICT");
        }
        if (!maximum_value.isNull() && !durable_authorization) {
            const DDCents effective = EffectiveClientServiceFeeCap(
                wallet, DDCents{maximum_value.getInt<int64_t>()}, error);
            if (effective.value < 0 ||
                !(effective == recovery.selected_maximum_service_fee)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
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
        throw JSONRPCError(RPC_WALLET_ERROR, error);
    }

    const auto make_result = [&](const AlternativeRecoveryRecord& record,
                                 const RecoveryQueueState& queue,
                                 std::optional<bool> broadcast = std::nullopt,
                                 const std::string& broadcast_error = {}) {
        PaymentSession current_session;
        if (!store.GetSessionByRequestId(record.request_id,
                                         current_session)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_SESSION_NOT_FOUND");
        }
        UniValue result{UniValue::VOBJ};
        result.pushKV("action", "cancel_to_self");
        result.pushKV("session", SessionToJSON(current_session));
        result.pushKV("attempt", AttemptToJSON(original_attempt));
        result.pushKV("artifact", "alternative_recovery");
        result.pushKV("recovery", AlternativeRecoveryToJSON(record));
        result.pushKV("queued", queue.queued);
        result.pushKV("connection_pending", queue.connection_pending);
        result.pushKV("route_available", queue.route_available);
        if (broadcast) result.pushKV("broadcast", *broadcast);
        if (!broadcast_error.empty()) {
            result.pushKV("broadcast_error", broadcast_error);
        }
        return result;
    };
    const auto fail_after_recovery_store =
        [&](const AlternativeRecoveryRecord& durable_recovery,
            const CTransaction& exact_final, int rpc_code,
            const std::string& failure) -> void {
        std::string persistence_error;
        if (!store.RecordClientAlternativeRecoveryConflict(
                durable_recovery.request_id, durable_recovery.recovery_id,
                exact_final.GetHash(), exact_final.GetWitnessHash(), now,
                persistence_error)) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                persistence_error.empty() ? "PAYMASTER_RECOVERY_FINAL_CONFLICT_NOT_PERSISTED" : persistence_error);
        }
        throw JSONRPCError(
            rpc_code,
            failure.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : failure);
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
                throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
            }
            return make_result(recovery, queue);
        }
        bool capacity_equivocation{false};
        if (!PersistPendingAlternativeRecoveryCapacityEquivocationPair(
                store, recovery.recovery_id, proofs, capacity_equivocation,
                error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (capacity_equivocation) {
            if (!AcknowledgeEquivocationMessages(
                    *context.paymaster, proofs, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            throw JSONRPCError(RPC_WALLET_ERROR,
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
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
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
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error =
                    "PAYMASTER_INVALID_CAPACITY_CLAIM_CANDIDATE";
                continue;
            }
            if (claim_equivocation) {
                if (!AcknowledgeEquivocationMessages(
                        *context.paymaster, proofs, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_CAPACITY_EQUIVOCATION");
            }
            if (!store.GetAlternativeRecoveryById(recovery.recovery_id,
                                                  recovery)) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_FOUND");
            }

            std::string proof_error;
            if (!ValidateCapacityProofAgainstChainstate(
                    *proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key, *node->chainman,
                    now, proof_error)) {
                if (!AcknowledgeRejectedDirectMessage(
                        *context.paymaster, direct, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                rejected_proof_error =
                    proof_error.empty() ? "PAYMASTER_INVALID_CAPACITY_PROOF" : std::move(proof_error);
                continue;
            }
            accepted_proof = *proof;
            break;
        }
        if (!accepted_proof) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
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
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "DigiDollar wallet not initialized");
        }
        for (const COutPoint& outpoint : session.user_inputs) {
            const CAmount amount = dd_wallet->GetDDFromUTXO(outpoint);
            if (amount <= 0 ||
                amount > std::numeric_limits<CAmount>::max() - total_dd) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RESERVED_INPUT_UNAVAILABLE");
            }
            total_dd += amount;
        }
        CAmount remaining = total_dd - recovery.selected_service_fee.value;
        if (remaining < 100) {
            throw JSONRPCError(RPC_WALLET_ERROR,
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
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RECOVERY_DESTINATION_UNAVAILABLE");
            }
            returns.push_back(
                {GetScriptForDestination(*destination), DDCents{chunk}});
            remaining -= chunk;
        }
        if (returns.size() > MAX_PAYMENT_INTENT_INPUTS) {
            throw JSONRPCError(RPC_WALLET_ERROR,
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
            throw JSONRPCError(
                RPC_WALLET_UNLOCK_NEEDED,
                "Wallet unlock is required for alternative recovery");
        }
        if (!DrainPendingAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, first_capacity_proof,
                error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        std::vector<XOnlyPubKey> output_keys;
        if (!ValidateCapacityProofAgainstChainstate(
                proof, recovery.capacity_request,
                recovery.recovery_provider_identity_key,
                *node->chainman, now, error) ||
            !SignAlternativeRecoveryRequestInputs(
                wallet, recovery_request, now, output_keys, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        recovery.recovery_request = std::move(recovery_request);
        recovery.recovery_request_hash =
            GetAlternativeRecoveryRequestHash(recovery.recovery_request);
        recovery.phase = AlternativeRecoveryPhase::REQUEST_READY;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::REQUEST_READY) {
        const auto responses = context.paymaster->TakeRecoveryResponses(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        if (responses.empty()) {
            PaymasterCapacityProof proof;
            if (!DeserializeCanonicalRecoveryMessage(
                    recovery.capacity_snapshot.capacity_proof, proof, error) ||
                !ValidateCapacityProofAgainstChainstate(
                    proof, recovery.capacity_request,
                    recovery.recovery_provider_identity_key,
                    *node->chainman, now, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            RecoveryQueueState queue;
            if (!QueueRecoveryMessage(
                    context, wallet, recovery, recovery.recovery_request, now,
                    queue, error)) {
                throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
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
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESPONSE" : error);
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
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED) {
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
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
        }
        if (prepare_only) {
            return make_result(recovery, RecoveryQueueState{});
        }
        if (!accepted_commitment) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_REQUIRED");
        }
        if (*accepted_commitment !=
            recovery.recovery_authorization.authorization_commitment) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "PAYMASTER_RECOVERY_AUTHORIZATION_COMMITMENT_MISMATCH");
        }
        if (wallet.IsLocked()) {
            throw JSONRPCError(
                RPC_WALLET_UNLOCK_NEEDED,
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
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                error.empty() ? "PAYMASTER_RECOVERY_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
        }
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        if (!SignCollaborativePSBTForParty(
                wallet, unsigned_psbt, trusted.trusted_template,
                SigningParty::USER, error) ||
            !ValidateAlternativeRecoveryPSBTAgainstChainstate(
                unsigned_psbt, trusted, parameters, Params(),
                *node->chainman, now,
                CollaborativeSignatureStage::USER_SIGNED, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        recovery.user_signed_psbt = SerializePSBT(unsigned_psbt);
        recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
        recovery.updated_at = std::max(recovery.updated_at, now);
        if (!store.UpdateAlternativeRecovery(recovery, error) ||
            !store.GetAlternativeRecovery(session.request_id, recovery)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
    }

    if (recovery.phase == AlternativeRecoveryPhase::USER_SIGNED) {
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        if (authorization_time <= 0) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
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
        const auto messages = context.paymaster->TakeRecoveryResults(
            recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, 1);
        const AlternativeRecoveryResultMessage* message{nullptr};
        CTransactionRef observed_final;
        FinalTransactionPresence observed_presence{
            FinalTransactionPresence::NONE};
        if (!messages.empty()) {
            message = std::get_if<AlternativeRecoveryResultMessage>(
                &messages.front().payload);
            if (!message || !ValidateAlternativeRecoveryResult(*message, recovery.recovery_response, recovery.recovery_provider_identity_key, Params().GenesisBlock().GetHash(), 1, now, error) ||
                !message->result.final_transaction ||
                !message->result.raw_transaction_hash) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    error.empty() ? "PAYMASTER_INVALID_RECOVERY_RESULT" : error);
            }
            observed_final = MakeTransactionRef(
                *message->result.final_transaction);
            if (!PreflightFinalPaymasterTransaction(
                    wallet, observed_final, observed_presence, error)) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
            }
        }
        const AuthorizedCapacityResourceMode resource_mode{
            observed_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT};
        const CTransaction* const known_final{
            observed_presence != FinalTransactionPresence::NONE ? observed_final.get() : nullptr};
        PartiallySignedTransaction user_psbt;
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                resource_mode, trusted, unsigned_psbt, error, known_final) ||
            !ValidateAuthorizedAlternativeRecoverySubmitAgainstChainstate(
                submit, recovery.recovery_response, trusted, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                resource_mode, user_psbt, error, known_final)) {
            throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
        }

        if (message) {
            const CMutableTransaction& final_transaction =
                *message->result.final_transaction;
            const CTransactionRef& final_ref = observed_final;
            const FinalTransactionPresence presence{observed_presence};
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, *message->result.raw_transaction_hash,
                    trusted, parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    resource_mode, error)) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, error);
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
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }

            // The final commit above performs database work and spends the
            // client fee reservation. Rebuild every authority from the exact
            // durable bytes and repeat the chainstate/mempool firewall before
            // handing the transaction to the node.
            AlternativeRecoveryRecord broadcast_recovery;
            AlternativeRecoveryParameters broadcast_parameters;
            AlternativeRecoveryTemplate broadcast_trusted;
            PartiallySignedTransaction broadcast_unsigned_psbt;
            FinalTransactionPresence broadcast_presence{
                FinalTransactionPresence::NONE};
            if (!store.GetAlternativeRecovery(
                    recovery.request_id, broadcast_recovery)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "PAYMASTER_RECOVERY_FINAL_STATE_RELOAD_FAILED");
            }
            if (broadcast_recovery.phase !=
                    AlternativeRecoveryPhase::FINAL_COMMITTED ||
                broadcast_recovery.recovery_id != recovery.recovery_id ||
                broadcast_recovery.final_transaction !=
                    SerializeTransaction(final_transaction) ||
                broadcast_recovery.expected_wtxid !=
                    CTransaction{final_transaction}.GetWitnessHash()) {
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID");
            }
            if (!PreflightFinalPaymasterTransaction(
                    wallet, final_ref, broadcast_presence, error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED, error);
            }
            if (!BuildRecoveryParametersFromRecord(
                    wallet, broadcast_recovery, broadcast_parameters, error) ||
                !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                    broadcast_recovery.recovery_response,
                    broadcast_recovery.recovery_request,
                    broadcast_recovery.recovery_provider_identity_key,
                    broadcast_parameters, Params(), *node->chainman,
                    authorization_time, observation_time,
                    broadcast_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                    broadcast_trusted,
                    broadcast_unsigned_psbt, error,
                    broadcast_presence != FinalTransactionPresence::NONE ? final_ref.get() : nullptr)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    error.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            if (!ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                    final_transaction, broadcast_recovery.expected_wtxid,
                    broadcast_trusted, broadcast_parameters, Params(),
                    *node->chainman, authorization_time, observation_time,
                    broadcast_presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                    error)) {
                if (IsTransientPaymasterFinalizationError(error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    error.empty() ? "PAYMASTER_RECOVERY_BROADCAST_AUTHORIZATION_INVALID" : error);
            }
            recovery = std::move(broadcast_recovery);
            bool already_confirmed{false};
            std::string broadcast_error;
            const bool broadcast = InsertAndBroadcastPaymasterTransaction(
                wallet, final_ref, already_confirmed, broadcast_error);
            if (!broadcast) {
                if (IsTransientPaymasterFinalizationError(broadcast_error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
                }
                fail_after_recovery_store(
                    recovery, CTransaction{final_transaction},
                    RPC_TRANSACTION_REJECTED,
                    broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
            }
            if (broadcast && !store.ReconcileFinalTransaction(
                                 *final_ref, already_confirmed ? 1 : 0,
                                 !already_confirmed, now, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            return make_result(recovery, RecoveryQueueState{}, broadcast,
                               broadcast_error);
        }
        RecoveryQueueState queue;
        if (!QueueRecoveryMessage(context, wallet, recovery, submit, now,
                                  queue, error)) {
            throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, error);
        }
        return make_result(recovery, queue);
    }

    if (recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED) {
        CMutableTransaction final_transaction;
        try {
            SpanReader stream{::PROTOCOL_VERSION,
                              recovery.final_transaction};
            stream >> final_transaction;
            if (!stream.empty()) {
                throw std::ios_base::failure("trailing recovery transaction");
            }
        } catch (const std::ios_base::failure&) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "PAYMASTER_PERSISTED_RECOVERY_CORRUPT");
        }
        const CTransactionRef final_ref =
            MakeTransactionRef(final_transaction);
        FinalTransactionPresence presence{FinalTransactionPresence::NONE};
        if (!PreflightFinalPaymasterTransaction(
                wallet, final_ref, presence, error)) {
            if (IsTransientPaymasterFinalizationError(error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED, error);
        }
        AlternativeRecoveryParameters parameters;
        AlternativeRecoveryTemplate trusted;
        PartiallySignedTransaction unsigned_psbt;
        const int64_t authorization_time =
            recovery.recovery_authorization_accepted_at;
        const int64_t observation_time{
            std::max(now, authorization_time)};
        if (!BuildRecoveryParametersFromRecord(
                wallet, recovery, parameters, error) ||
            authorization_time <= 0 ||
            !ValidateAuthorizedAlternativeRecoveryResponseTemplateAgainstChainstate(
                recovery.recovery_response, recovery.recovery_request,
                recovery.recovery_provider_identity_key, parameters,
                Params(), *node->chainman, authorization_time,
                observation_time,
                presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                trusted,
                unsigned_psbt, error,
                presence != FinalTransactionPresence::NONE ? final_ref.get() : nullptr) ||
            !ValidateAuthorizedFinalAlternativeRecoveryAgainstChainstate(
                final_transaction, recovery.expected_wtxid, trusted,
                parameters, Params(), *node->chainman,
                authorization_time, observation_time,
                presence != FinalTransactionPresence::NONE ? AuthorizedCapacityResourceMode::EXACT_FINAL_ALREADY_KNOWN : AuthorizedCapacityResourceMode::REQUIRE_UNSPENT,
                error)) {
            if (IsTransientPaymasterFinalizationError(error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED, error);
        }
        bool already_confirmed{false};
        std::string broadcast_error;
        const bool broadcast = InsertAndBroadcastPaymasterTransaction(
            wallet, final_ref, already_confirmed, broadcast_error);
        if (!broadcast) {
            if (IsTransientPaymasterFinalizationError(broadcast_error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_UNAVAILABLE" : broadcast_error);
            }
            fail_after_recovery_store(
                recovery, CTransaction{final_transaction},
                RPC_TRANSACTION_REJECTED,
                broadcast_error.empty() ? "PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED" : strprintf("PAYMASTER_RECOVERY_FINAL_BROADCAST_REJECTED: %s", broadcast_error));
        }
        if (broadcast && !store.ReconcileFinalTransaction(
                             *final_ref, already_confirmed ? 1 : 0,
                             !already_confirmed, now, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR, error);
        }
        return make_result(recovery, RecoveryQueueState{}, broadcast,
                           broadcast_error);
    }
    throw JSONRPCError(RPC_WALLET_ERROR,
                       "PAYMASTER_ALTERNATIVE_RECOVERY_STATE_INVALID");
}

RPCHelpMan resolvepaymastersession()
{
    return RPCHelpMan{
        "resolvepaymastersession",
        "Inspect or recover an existing Paymaster session.\n"
        "retry_same never creates a quote, attempt, reservation, or signature. "
        "fallback is allowed only before a user PSBT exists. abandon_unsigned "
        "atomically releases a session for which no transaction authorization can exist. "
        "cancel_to_self durably creates or resumes one exact same-input recovery transaction.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"lookup", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Exactly one persistent session identifier", {
                                                                                                                 {"request_id", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Canonical lowercase UUID"},
                                                                                                                 {"session_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Persistent session identifier"},
                                                                                                             }},
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Exact recovery selection and authorization", {
                                                                                                                        {"attempt_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Existing original attempt identifier"},
                                                                                                                        {"recovery_provider_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact distinct recovery provider"},
                                                                                                                        {"recovery_offer_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Optional exact USER_PAID recovery offer"},
                                                                                                                        {"maximum_recovery_service_fee_cents", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Local recovery service-fee ceiling"},
                                                                                                                        {"prepare_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Stop after validating and persisting the exact authorization commitment"},
                                                                                                                        {"recovery_authorization_commitment", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Exact commitment required before signing user inputs"},
                                                                                                                    }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative local recovery state", {
                                                                                      {RPCResult::Type::STR, "action", "Performed local action"},
                                                                                      {RPCResult::Type::OBJ, "session", /*optional=*/false, "Persistent session", {
                                                                                                                                                                      {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                                                                                                      {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                                                                                                       {RPCResult::Type::STR_HEX, "canonical_request_hash", "Canonical request hash"},
                                                                                                                                                                       {RPCResult::Type::STR, "requested_fee_mode", "Requested fee mode"},
                                                                                                                                                                       {RPCResult::Type::STR, "fee_mode_used", "Effective fee mode"},
                                                                                                                                                                       {RPCResult::Type::NUM, "requested_amount_cents", /*optional=*/true, "Original recipient amount or exact total DD outflow"},
                                                                                                                                                                       {RPCResult::Type::BOOL, "subtract_paymaster_fee_from_amount", /*optional=*/true, "Whether the Paymaster fee is deducted from the requested amount"},
                                                                                                                                                                       {RPCResult::Type::BOOL, "send_all_spendable_dd", /*optional=*/true, "Whether the requested amount was bound to all spendable confirmed DD"},
                                                                                                                                                                       {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                                                                                                      {RPCResult::Type::STR, "pending_phase", /*optional=*/true, "Pending provider phase"},
                                                                                                                                                                      {RPCResult::Type::BOOL, "final", "Whether the session is terminal"},
                                                                                                                                                                      {RPCResult::Type::STR, "broadcast_state", "Authoritative broadcast state"},
                                                                                                                                                                      {RPCResult::Type::STR, "confirmation_state", "Authoritative confirmation state"},
                                                                                                                                                                      {RPCResult::Type::NUM_TIME, "created_at", "Session creation time"},
                                                                                                                                                                      {RPCResult::Type::NUM_TIME, "updated_at", "Last persisted transition time"},
                                                                                                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known payment transaction"},
                                                                                                                                                                      {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Known self-recovery transaction"},
                                                                                                                                                                      {RPCResult::Type::ARR, "reserved_user_inputs", "Immutable user input set", {
                                                                                                                                                                                                                                                     {RPCResult::Type::OBJ, "", "A reserved user input", {
                                                                                                                                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "txid", "Creating transaction id"},
                                                                                                                                                                                                                                                                                                             {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                                                                         }},
                                                                                                                                                                                                                                                 }},
                                                                                                                                                                      {RPCResult::Type::NUM, "provider_attempts", "Persistent provider attempt count"},
                                                                                                                                                                  }},
                                                                                      {RPCResult::Type::OBJ, "attempt", /*optional=*/true, "Selected persistent attempt", {
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "attempt_id", "Persistent attempt identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "provider_id", "Provider identifier"},
                                                                                                                                                                              {RPCResult::Type::STR, "provider_endpoint", /*optional=*/true, "Direct provider endpoint"},
                                                                                                                                                                              {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Provider quote identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Bound transaction template"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "unsigned_txid", /*optional=*/true, "Unsigned transaction identifier"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Provider commit key"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "quote_expires_at", /*optional=*/true, "Quote expiration time"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "retry_until", /*optional=*/true, "Exact retry deadline"},
                                                                                                                                                                              {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact client authorization"},
                                                                                                                                                                              {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                              {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                          }},
                                                                                      {RPCResult::Type::STR, "artifact", /*optional=*/true, "none, user_psbt, final_transaction, or alternative_recovery"},
                                                                                      {RPCResult::Type::OBJ, "recovery", /*optional=*/true, "Persistent alternative-recovery authority and state", {
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "recovery_id", "Stable recovery identifier"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "recovery_provider_id", "Distinct recovery provider"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "offer_id", "Exact USER_PAID offer"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "policy_hash", "Exact provider policy"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "original_commit_key", "Ambiguous original provider commit"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "original_template_commitment", "Ambiguous original transaction template"},
                                                                                                                                                                                                       {RPCResult::Type::STR, "privacy_profile", "Inherited standard or high privacy profile"},
                                                                                                                                                                                                       {RPCResult::Type::STR, "phase", "capacity_pending, request_ready, response_validated, user_signed, or final_committed"},
                                                                                                                                                                                                       {RPCResult::Type::BOOL, "expired", "Whether a safely expirable unsigned recovery expired"},
                                                                                                                                                                                                       {RPCResult::Type::ARR, "user_dd_inputs", "Canonical original user inputs", {{RPCResult::Type::OBJ, "", "User DD input", {
                                                                                                                                                                                                                                                                                                                                   {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                                                                                                                   {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                                                                                               }}}},
                                                                                                                                                                                                       {RPCResult::Type::ARR, "wallet_returns", "Canonical wallet-owned recovery outputs", {{RPCResult::Type::OBJ, "", "Wallet return", {
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::STR_HEX, "script_pub_key", "Exact return script"},
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::STR, "address", /*optional=*/true, "Display address when decodable"},
                                                                                                                                                                                                                                                                                                                                            {RPCResult::Type::NUM, "amount_cents", "Exact DD amount"},
                                                                                                                                                                                                                                                                                                                                        }}}},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Validated Capacity-v5 snapshot"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "capacity_resource_commitment", /*optional=*/true, "Exact capacity resources"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact recovery authorization"},
                                                                                                                                                                                                       {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                                                       {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "maximum_service_fee_cents", "Effective local fee ceiling"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "service_fee_cents", "Exact recovery service fee"},
                                                                                                                                                                                                       {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "Exact provider-paid network fee"},
                                                                                                                                                                                                       {RPCResult::Type::NUM_TIME, "expires_at", "Recovery authorization expiry"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact final recovery transaction"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final recovery txid"},
                                                                                                                                                                                                       {RPCResult::Type::STR_HEX, "wtxid", /*optional=*/true, "Final recovery wtxid"},
                                                                                                                                                                                                   }},
                                                                                      {RPCResult::Type::STR, "psbt", /*optional=*/true, "Exact persisted user-signed PSBT"},
                                                                                      {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact durably committed transaction"},
                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Committed transaction id"},
                                                                                      {RPCResult::Type::STR_HEX, "recovery_txid", /*optional=*/true, "Idempotent self-recovery transaction id"},
                                                                                      {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether local broadcast succeeded"},
                                                                                      {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Local broadcast error; exact retry remains available"},
                                                                                      {RPCResult::Type::BOOL, "queued", /*optional=*/true, "Whether the exact persisted submit is queued"},
                                                                                      {RPCResult::Type::BOOL, "connection_pending", /*optional=*/true, "Whether a dedicated provider reconnection was requested"},
                                                                                      {RPCResult::Type::BOOL, "route_available", /*optional=*/true, "Whether the persisted provider route is available"},
                                                                                      {RPCResult::Type::STR, "result_status", /*optional=*/true, "Latest signed provider result"},
                                                                                      {RPCResult::Type::NUM, "result_sequence", /*optional=*/true, "Latest monotonic result sequence"},
                                                                                  }},
        RPCExamples{HelpExampleCli("resolvepaymastersession", "'{\"request_id\":\"550e8400-e29b-41d4-a716-446655440000\"}' retry_same")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            const std::string action = request.params[1].get_str();
            if (action != "refresh" && action != "retry_same" && action != "fallback" &&
                action != "abandon_unsigned" && action != "cancel_to_self") {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "action must be refresh, retry_same, fallback, abandon_unsigned, or cancel_to_self");
            }

            UniValue options{UniValue::VOBJ};
            if (!request.params[2].isNull()) options = request.params[2].get_obj();
            RPCTypeCheckObj(options,
                            {{"attempt_id", UniValueType(UniValue::VSTR)},
                             {"recovery_provider_id", UniValueType(UniValue::VSTR)},
                             {"recovery_offer_id", UniValueType(UniValue::VSTR)},
                             {"maximum_recovery_service_fee_cents", UniValueType(UniValue::VNUM)},
                             {"prepare_only", UniValueType(UniValue::VBOOL)},
                             {"recovery_authorization_commitment", UniValueType(UniValue::VSTR)}},
                            /*fAllowNull=*/true, /*fStrict=*/true);

            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::PaymentSession session;
            if (!FindSession(request.params[0].get_obj(), store, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            if (context.paymaster && context.paymaster->Enabled()) {
                std::string equivocation_error;
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session,
                        equivocation_error,
                        EquivocationBlockPolicy::OBSERVE_ONLY)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       equivocation_error);
                }
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("action", action);
            result.pushKV("session", SessionToJSON(session));
            if (action == "refresh") return result;

            if (action == "abandon_unsigned") {
                std::string error;
                if (!store.AbandonUnsignedClientSession(
                        session.request_id, GetTime(), error) ||
                    !store.GetSessionByRequestId(session.request_id, session)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_UNSIGNED_ABANDON_FAILED" : error);
                }
                result.pushKV("session", SessionToJSON(session));
                result.pushKV("artifact", "none");
                return result;
            }

            if (session.attempt_ids.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session has no provider attempt");
            }
            uint256 attempt_id = session.attempt_ids.back();
            if (!options.find_value("attempt_id").isNull()) {
                attempt_id = ParseHashO(options, "attempt_id");
                if (std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
                    session.attempt_ids.end()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "attempt_id does not belong to this session");
                }
            }
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttempt(attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster attempt not found");
            }
            if (action == "cancel_to_self") {
                return ResolveAlternativePaymasterRecovery(
                    context, *wallet, store, session, attempt, options);
            }
            if (action == "fallback") {
                std::string error;
                if (!store.AbandonClientAttemptForFallback(session.request_id, attempt_id,
                                                           GetTime(), error) ||
                    !store.GetSessionByRequestId(session.request_id, session) ||
                    !store.GetAttempt(attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       error.empty() ? "PAYMASTER_FALLBACK_FAILED" : error);
                }
                result.pushKV("session", SessionToJSON(session));
                result.pushKV("attempt", AttemptToJSON(attempt));
                result.pushKV("artifact", "none");
                return result;
            }
            result.pushKV("attempt", AttemptToJSON(attempt));

            DigiDollar::Paymaster::ProviderCommitRecord commit;
            if (!attempt.commit_key.IsNull() && store.GetProviderCommit(attempt.commit_key, commit)) {
                result.pushKV("artifact", "final_transaction");
                result.pushKV("raw_transaction", HexStr(commit.final_transaction));
                result.pushKV("txid", commit.final_txid.GetHex());
                if (action == "retry_same") {
                    const int64_t now{GetTime()};
                    bool already_confirmed{false};
                    std::string broadcast_error;
                    CTransactionRef exact_transaction;
                    const bool broadcast = InsertAndBroadcastProviderCommit(
                        *wallet, store, commit, now, already_confirmed,
                        broadcast_error, &exact_transaction);
                    result.pushKV("broadcast", broadcast);
                    if (!broadcast_error.empty()) {
                        result.pushKV("broadcast_error", broadcast_error);
                    }
                    std::string reconcile_error;
                    if (broadcast && !store.ReconcileFinalTransaction(
                                         *exact_transaction, already_confirmed ? 1 : 0,
                                         !already_confirmed, now, reconcile_error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, reconcile_error);
                    }
                }
                DigiDollar::Paymaster::PaymasterResult provider_result;
                if (store.GetProviderResult(commit.commit_key, provider_result)) {
                    result.pushKV("result_status", ResultStatusName(provider_result.status));
                    result.pushKV("result_sequence", provider_result.result_sequence);
                }
                return result;
            }
            if (attempt.retry_until > 0 && GetTime() > attempt.retry_until) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
            }
            if (!attempt.user_signed_psbt.empty()) {
                result.pushKV("artifact", "user_psbt");
                result.pushKV("psbt", EncodeBase64(attempt.user_signed_psbt));
                if (action == "retry_same") {
                    PaymasterSubmitQueueState queue_state;
                    std::string error;
                    if (!QueuePersistedPaymasterSubmit(context, *wallet, session, attempt,
                                                       GetTime(), queue_state, error)) {
                        throw JSONRPCError(
                            error == "PAYMASTER_DIRECT_QUEUE_FULL" ? RPC_CLIENT_NODE_CAPACITY_REACHED : RPC_WALLET_ERROR,
                            error);
                    }
                    result.pushKV("queued", queue_state.queued);
                    result.pushKV("connection_pending", queue_state.connection_pending);
                    result.pushKV("route_available", queue_state.route_available);
                }
            } else {
                result.pushKV("artifact", "none");
            }
            return result;
        },
    };
}

RPCHelpMan walletprocesspaymasterpsbt()
{
    return RPCHelpMan{
        "walletprocesspaymasterpsbt",
        "Validate an unsigned Paymaster PSBT against its exact persistent wallet template and sign only USER inputs.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded unsigned Paymaster PSBT"},
            {"authorization_commitment", RPCArg::Type::STR_HEX,
             RPCArg::Optional::OMITTED,
             "Exact client authorization commitment. Required before a new USER signature; omitted only for an exact already-signed retry"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The durable user authorization artifact", {
                                                                                           {RPCResult::Type::STR, "psbt", "Canonical base64-encoded user-signed PSBT"},
                                                                                           {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                           {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                           {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                                           {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                                           {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                                           {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Accepted client authorization commitment"},
                                                                                           {RPCResult::Type::BOOL, "authorization_accepted", "Whether the exact commitment is durably accepted"},
                                                                                           {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Monotonic durable acceptance time"},
                                                                                           {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                           {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                           {RPCResult::Type::NUM_TIME, "expires_at", "Provider retry deadline"},
                                                                                           {RPCResult::Type::BOOL, "queued", "Whether PMSUBMIT is queued to a connected v2 peer"},
                                                                                           {RPCResult::Type::BOOL, "connection_pending", "Whether reconnecting to the provider was requested"},
                                                                                           {RPCResult::Type::BOOL, "route_available", "Whether a current provider announcement supplies the retry route"},
                                                                                       }},
        RPCExamples{HelpExampleCli("walletprocesspaymasterpsbt", "\"cHNidP8...\" \"0123...\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            std::string readiness_error;
            if (!CheckPaymasterClientReadiness(*wallet, context, readiness_error)) {
                throw JSONRPCError(RPC_MISC_ERROR, readiness_error);
            }

            PartiallySignedTransaction candidate;
            std::string error;
            if (!DecodeBase64PSBT(candidate, request.params[0].get_str(), error) || !candidate.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   strprintf("Paymaster PSBT decode failed: %s", error));
            }
            const uint256 unsigned_txid = CTransaction{*candidate.tx}.GetHash();
            std::optional<uint256> supplied_authorization_commitment;
            if (request.params.size() > 1 && !request.params[1].isNull()) {
                supplied_authorization_commitment =
                    ParseHashV(request.params[1], "authorization_commitment");
            }
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "No persistent Paymaster template matches this PSBT");
            }
            DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error) ||
                !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                    candidate, trusted,
                    DigiDollar::Paymaster::CollaborativeSignatureStage::UNSIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }

            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            if (!DrainClientSessionEquivocations(
                    *context.paymaster, store, session, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            const int64_t now = GetTime();
            PaymentIntent authorized_intent;
            PaymasterQuote authorized_quote;
            CollaborativePSBTTemplate authorized_template;
            if (!LoadAttemptAuthorizationArtifacts(
                    attempt, authorized_intent, authorized_quote,
                    authorized_template, error) ||
                attempt.client_manifest.manifest_id.IsNull() ||
                now > attempt.client_manifest.expires_at ||
                !ValidateClientAuthorizationManifest(
                    attempt.client_manifest, authorized_intent, authorized_quote,
                    attempt.capacity_snapshot, authorized_template, error)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
            }
            if (attempt.state == AttemptState::QUOTED) {
                if (!supplied_authorization_commitment) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_AUTHORIZATION_COMMITMENT_REQUIRED");
                }
                if (*supplied_authorization_commitment !=
                    attempt.client_manifest.manifest_id) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH");
                }
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
                        throw std::ios_base::failure("trailing capacity artifact data");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
                }
                node::NodeContext* node = wallet->chain().context();
                if (!node || !node->chainman ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof, capacity_request,
                        attempt.provider_identity_key, *node->chainman, now,
                        error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
                }
                if (!store.AcceptClientAuthorization(
                        session.request_id, attempt.attempt_id,
                        *supplied_authorization_commitment, now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_FAILED" : error);
                }
            }
            const int64_t operation_now = std::max(
                now, std::max(session.updated_at, attempt.updated_at));
            if (attempt.state != DigiDollar::Paymaster::AttemptState::QUOTED) {
                if (attempt.state >= DigiDollar::Paymaster::AttemptState::USER_SIGNED &&
                    attempt.state <= DigiDollar::Paymaster::AttemptState::MEMPOOL &&
                    !attempt.user_signed_psbt.empty()) {
                    if (operation_now > attempt.retry_until) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
                    }
                    candidate = PartiallySignedTransaction{};
                } else {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster attempt is not awaiting a user signature");
                }
            } else if (operation_now > attempt.quote_expires_at) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster quote expired");
            }

            if (candidate.tx &&
                session.state != DigiDollar::Paymaster::SessionState::INPUTS_RESERVED &&
                session.state != DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK &&
                session.state != DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE &&
                session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Paymaster session is not awaiting a user authorization");
            }

            if (!candidate.tx) {
                if (!DecodeRawPSBT(candidate, MakeByteSpan(attempt.user_signed_psbt), error) ||
                    !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                        candidate, trusted,
                        DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Persisted Paymaster PSBT is corrupt");
                }
            } else {
                if (wallet->IsLocked()) {
                    if ((session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                         session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                         session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) &&
                        !store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                    throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                       "Wallet unlock is required to sign Paymaster USER inputs");
                }
                if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK) {
                    if (!store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                }
                // Re-read the immutable authorization immediately before the
                // only operation that can sign client inputs. Earlier RPC
                // validation is deliberately not treated as signing authority.
                if (!DrainClientSessionEquivocations(
                        *context.paymaster, store, session, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                ProviderAttempt signing_attempt;
                PaymentIntent signing_intent;
                PaymasterQuote signing_quote;
                CollaborativePSBTTemplate signing_template;
                if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
                    signing_attempt.state != AttemptState::QUOTED ||
                    !supplied_authorization_commitment ||
                    signing_attempt.accepted_client_manifest_id !=
                        *supplied_authorization_commitment ||
                    signing_attempt.accepted_client_manifest_id !=
                        signing_attempt.client_manifest.manifest_id ||
                    signing_attempt.client_manifest_accepted_at <= 0 ||
                    std::max(operation_now, signing_attempt.updated_at) >
                        signing_attempt.client_manifest.expires_at ||
                    !LoadAttemptAuthorizationArtifacts(
                        signing_attempt, signing_intent, signing_quote,
                        signing_template, error) ||
                    !ValidateClientAuthorizationManifest(
                        signing_attempt.client_manifest, signing_intent,
                        signing_quote, signing_attempt.capacity_snapshot,
                        signing_template, error) ||
                    !ValidateCollaborativePSBT(
                        candidate, signing_template,
                        CollaborativeSignatureStage::UNSIGNED, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED" : error);
                }
                if (signing_intent.canonical_request_hash !=
                        session.canonical_request_hash ||
                    signing_intent.requested_fee_mode !=
                        session.fee_mode_requested ||
                    signing_intent.privacy_profile !=
                        signing_attempt.privacy_profile) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH");
                }
                PaymasterCapacityRequest signing_capacity_request;
                PaymasterCapacityProof signing_capacity_proof;
                try {
                    SpanReader request_stream{::PROTOCOL_VERSION,
                                              signing_attempt.capacity_request};
                    request_stream >> signing_capacity_request;
                    SpanReader proof_stream{
                        ::PROTOCOL_VERSION,
                        signing_attempt.capacity_snapshot.capacity_proof};
                    proof_stream >> signing_capacity_proof;
                    if (!request_stream.empty() || !proof_stream.empty()) {
                        throw std::ios_base::failure(
                            "trailing capacity artifact data");
                    }
                } catch (const std::ios_base::failure&) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_CAPACITY_ARTIFACT_ENCODING");
                }
                node::NodeContext* signing_node = wallet->chain().context();
                if (!signing_node || !signing_node->chainman ||
                    !ValidateCapacityProofAgainstChainstate(
                        signing_capacity_proof, signing_capacity_request,
                        signing_attempt.provider_identity_key,
                        *signing_node->chainman,
                        std::max(operation_now, signing_attempt.updated_at), error)) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        error.empty() ? "PAYMASTER_CAPACITY_NOT_CURRENT" : error);
                }
                if (!ValidateClientAuthorizationOwnership(
                        *wallet, signing_attempt.client_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!DrainClientAttemptEquivocations(
                        *context.paymaster, store, session, signing_attempt,
                        error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                attempt = std::move(signing_attempt);
                if (!SignCollaborativePSBTForParty(*wallet, candidate,
                                                   signing_template,
                                                   DigiDollar::Paymaster::SigningParty::USER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                const std::vector<unsigned char> signed_psbt = SerializePSBT(candidate);
                if (!store.TransitionSession(session.request_id,
                                             DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                             DigiDollar::Paymaster::PendingPhase::NONE, {}, operation_now, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
                attempt.user_signed_psbt = signed_psbt;
                attempt.updated_at = std::max(attempt.updated_at, operation_now);
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            DigiDollar::Paymaster::PaymentSession authoritative;
            if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
                !store.GetAttempt(attempt.attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload durable Paymaster authorization");
            }

            PaymasterSubmitQueueState queue_state;
            if (!QueuePersistedPaymasterSubmit(context, *wallet, authoritative,
                                               attempt, operation_now, queue_state, error)) {
                throw JSONRPCError(
                    error == "PAYMASTER_DIRECT_QUEUE_FULL" ? RPC_CLIENT_NODE_CAPACITY_REACHED : RPC_WALLET_ERROR,
                    error);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("psbt", EncodeBase64(attempt.user_signed_psbt));
            result.pushKV("request_id", authoritative.request_id);
            result.pushKV("session_id", authoritative.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", attempt.quote_id.GetHex());
            result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
            result.pushKV("template_commitment", attempt.template_commitment.GetHex());
            if (!attempt.client_manifest.manifest_id.IsNull()) {
                result.pushKV("authorization_commitment",
                              attempt.client_manifest.manifest_id.GetHex());
            }
            const bool authorization_accepted =
                attempt.accepted_client_manifest_id ==
                    attempt.client_manifest.manifest_id &&
                !attempt.accepted_client_manifest_id.IsNull() &&
                attempt.client_manifest_accepted_at > 0;
            result.pushKV("authorization_accepted",
                          authorization_accepted);
            if (authorization_accepted) {
                result.pushKV("authorization_accepted_at",
                              attempt.client_manifest_accepted_at);
            }
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(authoritative.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(attempt.state)});
            result.pushKV("expires_at", attempt.retry_until);
            result.pushKV("queued", queue_state.queued);
            result.pushKV("connection_pending", queue_state.connection_pending);
            result.pushKV("route_available", queue_state.route_available);
            return result;
        },
    };
}

RPCHelpMan submitpaymasterdigidollar()
{
    return RPCHelpMan{
        "submitpaymasterdigidollar",
        "Validate a user-signed Paymaster PSBT, sign only PROVIDER inputs, durably commit the final transaction, then broadcast it.\n"
        "An already committed transaction is inserted and broadcast idempotently without requiring another provider signature.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded user-signed Paymaster PSBT"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The durable provider commit", {
                                                                               {RPCResult::Type::STR, "psbt", /*optional=*/true, "Canonical fully signed PSBT when produced by this call"},
                                                                               {RPCResult::Type::STR_HEX, "hex", "Exact durably committed final transaction"},
                                                                               {RPCResult::Type::STR_HEX, "txid", "Final transaction identifier"},
                                                                               {RPCResult::Type::BOOL, "broadcast", "Whether the committed transaction is in a local pool or already confirmed"},
                                                                               {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable recovery error when the durable commit could not yet be broadcast"},
                                                                               {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                               {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                               {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                               {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                               {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                               {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                               {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                               {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                               {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                               {RPCResult::Type::STR, "result_status", "Signed provider result status"},
                                                                               {RPCResult::Type::NUM, "result_sequence", "Monotonic result sequence"},
                                                                               {RPCResult::Type::NUM_TIME, "expires_at", "Exact retry deadline"},
                                                                           }},
        RPCExamples{HelpExampleCli("submitpaymasterdigidollar", "\"cHNidP8...\"")},
        [](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            PartiallySignedTransaction user_psbt;
            std::string error;
            if (!DecodeBase64PSBT(user_psbt, request.params[0].get_str(), error) || !user_psbt.tx) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
                                   strprintf("Paymaster PSBT decode failed: %s", error));
            }
            const uint256 unsigned_txid = CTransaction{*user_psbt.tx}.GetHash();
            PaymasterStore store{*wallet};
            DigiDollar::Paymaster::ProviderAttempt attempt;
            if (!store.GetAttemptByUnsignedTxid(unsigned_txid, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "No persistent Paymaster template matches this PSBT");
            }
            DigiDollar::Paymaster::ProviderIdentityRecord provider_identity;
            if (!CheckPaymasterProviderWallet(*wallet, error) ||
                !GetPaymasterIdentity(*wallet, provider_identity) ||
                provider_identity.provider_id != attempt.provider_id) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   error.empty() ? "Paymaster provider identity does not match the quote" : error);
            }
            DigiDollar::Paymaster::CollaborativePSBTTemplate trusted;
            if (!LoadTrustedTemplate(attempt, trusted, error) ||
                !DigiDollar::Paymaster::ValidateCollaborativePSBT(
                    user_psbt, trusted,
                    DigiDollar::Paymaster::CollaborativeSignatureStage::USER_SIGNED, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            const std::vector<unsigned char> canonical_user_psbt = SerializePSBT(user_psbt);
            if (!attempt.user_signed_psbt.empty() && attempt.user_signed_psbt != canonical_user_psbt) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_USER_PSBT_CONFLICT");
            }

            DigiDollar::Paymaster::PaymentSession session;
            if (!store.GetSessionBySessionId(attempt.session_id, session)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster session not found");
            }
            const int64_t now = GetTime();
            DigiDollar::Paymaster::ProviderCommitRecord committed;
            const bool already_committed = !attempt.commit_key.IsNull() &&
                                           store.GetProviderCommit(attempt.commit_key, committed);
            const bool exact_persisted_provider_signature =
                attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
                !attempt.final_txid.IsNull() &&
                !attempt.final_transaction.empty() &&
                attempt.provider_signed_at > 0 &&
                attempt.provider_signed_at <= attempt.retry_until &&
                !attempt.provider_signed_result.empty();
            if (!already_committed && !exact_persisted_provider_signature &&
                now > attempt.retry_until) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster retry window expired");
            }
            if (!already_committed && !exact_persisted_provider_signature) {
                PaymentIntent authorized_intent;
                PaymasterQuote authorized_quote;
                CollaborativePSBTTemplate authorized_template;
                if (!LoadAttemptAuthorizationArtifacts(
                        attempt, authorized_intent, authorized_quote,
                        authorized_template, error) ||
                    attempt.provider_manifest.manifest_id.IsNull() ||
                    now > attempt.provider_manifest.expires_at ||
                    !ValidateProviderAuthorizationManifest(
                        attempt.provider_manifest, authorized_intent,
                        authorized_quote, authorized_template, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_EXPIRED" : error);
                }
                if (!ValidateProviderAuthorizationOwnership(
                        *wallet, attempt.provider_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (!already_committed &&
                attempt.state != DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED &&
                wallet->IsLocked()) {
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                   "Provider wallet unlock is required for a new Paymaster signature");
            }

            if (!already_committed) {
                bool rejected{false};
                DigiDollar::Paymaster::PaymasterResult rejection;
                if (!RejectUnavailableTemplateInputs(*wallet, store, attempt, *user_psbt.tx,
                                                     now, rejected, rejection, error)) {
                    throw JSONRPCError(
                        error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                        error);
                }
                if (rejected) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE");
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::QUOTED) {
                if (now > attempt.quote_expires_at) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Paymaster quote expired");
                }
                if (session.state == DigiDollar::Paymaster::SessionState::INPUTS_RESERVED ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_WALLET_UNLOCK ||
                    session.state == DigiDollar::Paymaster::SessionState::AWAITING_USER_SIGNATURE) {
                    if (!store.TransitionSession(session.request_id,
                                                 DigiDollar::Paymaster::SessionState::AUTHORIZED,
                                                 DigiDollar::Paymaster::PendingPhase::NONE, {}, now, error)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, error);
                    }
                } else if (session.state != DigiDollar::Paymaster::SessionState::AUTHORIZED &&
                           session.state != DigiDollar::Paymaster::SessionState::PENDING_PROVIDER) {
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       "Paymaster session cannot accept a user authorization");
                }
                attempt.state = DigiDollar::Paymaster::AttemptState::USER_SIGNED;
                attempt.user_signed_psbt = canonical_user_psbt;
                attempt.updated_at = std::max(attempt.updated_at, now);
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_SIGNED) {
                if (!store.AcceptUserAuthorization(session.request_id, attempt.attempt_id,
                                                   Hash(canonical_user_psbt), now, error) ||
                    !store.GetAttempt(attempt.attempt_id, attempt)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            PartiallySignedTransaction full_psbt{user_psbt};
            bool signed_now{false};
            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::USER_PSBT_ACCEPTED) {
                ProviderAttempt signing_attempt;
                CollaborativePSBTTemplate signing_template;
                if (!store.GetAttempt(attempt.attempt_id, signing_attempt) ||
                    signing_attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
                    !ValidateDurableProviderUserAuthorization(
                        store, signing_attempt, error) ||
                    !ValidateProviderAuthorizationForExecution(
                        signing_attempt, now, signing_template, error) ||
                    !ValidateCollaborativePSBT(
                        full_psbt, signing_template,
                        CollaborativeSignatureStage::USER_SIGNED, error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        error.empty() ? "PAYMASTER_PROVIDER_AUTHORIZATION_STATE" : error);
                }
                attempt = std::move(signing_attempt);
                node::NodeContext* node_ctx = wallet->chain().context();
                PaymasterCapacityRequest capacity_request;
                PaymasterCapacityProof capacity_proof;
                if (!node_ctx || !node_ctx->chainman) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "PAYMASTER_NODE_CONTEXT_UNAVAILABLE");
                }
                if (!store.ValidateProviderPreSignatureAuthorization(
                        attempt, Params().GenesisBlock().GetHash(), now,
                        capacity_request, capacity_proof, error) ||
                    !ValidateCapacityProofAgainstChainstate(
                        capacity_proof, capacity_request,
                        attempt.provider_identity_key, *node_ctx->chainman,
                        now, error) ||
                    !TemplateInputsAvailable(*wallet, *full_psbt.tx)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_TEMPLATE_INPUT_UNAVAILABLE" : error);
                }
                if (!ValidateProviderAuthorizationOwnership(
                        *wallet, attempt.provider_manifest, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                if (!SignCollaborativePSBTForParty(*wallet, full_psbt,
                                                   signing_template,
                                                   DigiDollar::Paymaster::SigningParty::PROVIDER, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                CMutableTransaction final_transaction;
                if (!FinalizeAndExtractPSBT(full_psbt, final_transaction)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_FINALIZATION_FAILED");
                }
                const CTransaction signed_transaction{final_transaction};
                if (!ValidateFinalCollaborativeTransaction(
                        final_transaction, signed_transaction.GetHash(),
                        signed_transaction.GetWitnessHash(), signing_template,
                        error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
                // The signed result below is private crash-recovery authority.
                // Do not persist even that envelope until the exact transaction
                // has passed the shared script, witness, chain and mempool
                // firewall. The second preflight before the atomic commit stays
                // in place to close the race after this observation.
                FinalTransactionPresence signed_presence{
                    FinalTransactionPresence::NONE};
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, MakeTransactionRef(final_transaction),
                        signed_presence, error)) {
                    const bool transient =
                        error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
                        error == "PAYMASTER_TXINDEX_NOT_READY";
                    throw JSONRPCError(
                        transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error);
                }
                signed_now = true;
                attempt.state = DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED;
                attempt.final_transaction = SerializeTransaction(final_transaction);
                attempt.final_txid = CTransaction{final_transaction}.GetHash();
                const int64_t provider_signed_at =
                    std::max(attempt.updated_at, now);
                DigiDollar::Paymaster::ProviderCommitRecord signed_commit;
                signed_commit.commit_key = attempt.commit_key;
                signed_commit.provider_id = attempt.provider_id;
                signed_commit.quote_id = attempt.quote_id;
                signed_commit.template_commitment =
                    attempt.template_commitment;
                signed_commit.final_txid = attempt.final_txid;
                signed_commit.raw_transaction_hash =
                    Hash(attempt.final_transaction);
                signed_commit.final_transaction =
                    attempt.final_transaction;
                signed_commit.provider_inputs = ProviderInputs(
                    *full_psbt.tx, attempt.input_roles);
                signed_commit.committed_at = provider_signed_at;
                signed_commit.retry_until = attempt.retry_until;
                DigiDollar::Paymaster::PaymasterResult signed_result;
                if (!BuildFinalProviderResult(
                        *wallet, signed_commit, provider_signed_at,
                        signed_result, error)) {
                    throw JSONRPCError(
                        error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR,
                        error);
                }
                attempt.provider_signed_at = provider_signed_at;
                attempt.provider_signed_result =
                    SerializeRecoveryMessage(signed_result);
                attempt.updated_at = provider_signed_at;
                if (!store.UpdateAttempt(session.request_id, attempt, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }

            if (!already_committed && attempt.state == DigiDollar::Paymaster::AttemptState::PROVIDER_SIGNED) {
                DigiDollar::Paymaster::ProviderCommitRecord commit;
                commit.commit_key = attempt.commit_key;
                commit.provider_id = attempt.provider_id;
                commit.quote_id = attempt.quote_id;
                commit.template_commitment = attempt.template_commitment;
                commit.final_txid = attempt.final_txid;
                commit.raw_transaction_hash = Hash(attempt.final_transaction);
                commit.final_transaction = attempt.final_transaction;
                commit.provider_inputs = ProviderInputs(*user_psbt.tx, attempt.input_roles);
                // A crash may leave the complete provider-signed transaction
                // durable without its atomic ProviderCommit. Preserve the
                // signature boundary while recording the actual (monotonic)
                // commit time, even when the retry window has since elapsed.
                const int64_t commit_time =
                    std::max(now, attempt.provider_signed_at);
                commit.committed_at = commit_time;
                commit.retry_until = attempt.retry_until;
                const std::optional<uint256> sponsorship_hash =
                    attempt.sponsorship_capability_hash.IsNull() ? std::nullopt : std::optional<uint256>{attempt.sponsorship_capability_hash};
                DigiDollar::Paymaster::PaymasterResult final_result;
                if (!DeserializeCanonicalRecoveryMessage(
                        attempt.provider_signed_result, final_result,
                        error)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID");
                }

                // Reload the durable PROVIDER_SIGNED attempt and re-run both
                // the provider authorization firewall and the full final
                // transaction verifier at the last possible point before the
                // atomic FINAL_COMMITTED transition.
                ProviderAttempt committing_attempt;
                CMutableTransaction validated_final;
                FinalTransactionPresence final_presence{
                    FinalTransactionPresence::NONE};
                if (!store.GetAttempt(attempt.attempt_id, committing_attempt) ||
                    committing_attempt.state != AttemptState::PROVIDER_SIGNED ||
                    !ValidateProviderCommitForExecution(
                        committing_attempt, commit, commit_time, validated_final,
                        error) ||
                    !store.ValidateProviderBudgetAuthorization(
                        committing_attempt,
                        BudgetReservationState::RESERVED,
                        /*allow_historical_policy=*/true, error)) {
                    throw JSONRPCError(
                        RPC_TRANSACTION_REJECTED,
                        error.empty() ? "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_INVALID" : error);
                }
                if (!PreflightFinalPaymasterTransaction(
                        *wallet, MakeTransactionRef(validated_final),
                        final_presence, error)) {
                    const bool transient =
                        error == "PAYMASTER_NODE_CONTEXT_UNAVAILABLE" ||
                        error == "PAYMASTER_TXINDEX_NOT_READY";
                    throw JSONRPCError(
                        transient ? RPC_WALLET_ERROR : RPC_TRANSACTION_REJECTED,
                        error);
                }
                attempt = std::move(committing_attempt);
                if (!store.CommitProviderFinalTransaction(session.request_id, attempt.attempt_id,
                                                          commit, sponsorship_hash,
                                                          final_result,
                                                          Params().GenesisBlock().GetHash(),
                                                          error) ||
                    !store.GetProviderCommit(attempt.commit_key, committed)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            if (!already_committed && committed.final_transaction.empty()) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Paymaster attempt is not recoverable by exact provider retry");
            }

            ProviderCommitRecoveryResult recovery =
                RecoverProviderCommit(
                    *wallet, store, committed,
                    std::max(now, committed.committed_at));
            if (recovery.provider_result.commit_key.IsNull()) {
                const int code = recovery.error == "PAYMASTER_WALLET_LOCKED" ? RPC_WALLET_UNLOCK_NEEDED : RPC_WALLET_ERROR;
                throw JSONRPCError(code, recovery.error);
            }

            DigiDollar::Paymaster::PaymentSession authoritative;
            if (!store.GetSessionByRequestId(session.request_id, authoritative) ||
                !store.GetAttempt(attempt.attempt_id, attempt)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to reload durable provider commit");
            }
            UniValue result{UniValue::VOBJ};
            if (signed_now) result.pushKV("psbt", EncodeBase64(SerializePSBT(full_psbt)));
            result.pushKV("hex", HexStr(committed.final_transaction));
            result.pushKV("txid", committed.final_txid.GetHex());
            result.pushKV("broadcast", recovery.broadcast);
            if (!recovery.error.empty()) result.pushKV("broadcast_error", recovery.error);
            result.pushKV("request_id", authoritative.request_id);
            result.pushKV("session_id", authoritative.session_id.GetHex());
            result.pushKV("provider_id", attempt.provider_id.GetHex());
            result.pushKV("attempt_id", attempt.attempt_id.GetHex());
            result.pushKV("quote_id", attempt.quote_id.GetHex());
            result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
            result.pushKV("template_commitment", attempt.template_commitment.GetHex());
            result.pushKV("session_state", std::string{DigiDollar::Paymaster::SessionStateName(authoritative.state)});
            result.pushKV("attempt_state", std::string{DigiDollar::Paymaster::AttemptStateName(attempt.state)});
            result.pushKV("result_status", ResultStatusName(recovery.provider_result.status));
            result.pushKV("result_sequence", recovery.provider_result.result_sequence);
            result.pushKV("expires_at", attempt.retry_until);
            return result;
        },
    };
}

} // namespace wallet
