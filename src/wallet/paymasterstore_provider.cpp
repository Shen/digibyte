// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider quote admission, authorization, expiration, and equivocation state. */

#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>

#include <chainparams.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <node/context.h>
#include <paymaster/protocol.h>
#include <paymaster/psbt.h>
#include <paymaster/validation.h>
#include <paymaster/wire.h>
#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/time.h>
#include <version.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_store::internal;

bool PaymasterStore::CommitProviderQuote(ProviderAttempt attempt,
                                         const uint256& expected_genesis,
                                         int64_t now,
                                         std::string& error)
{
    return CommitProviderQuote(std::move(attempt), std::nullopt,
                               expected_genesis, now, error);
}

bool PaymasterStore::CheckProviderQuoteAdmission(
    const PaymasterQuoteRequest& request,
    const uint256& request_hash,
    const std::vector<unsigned char>& canonical_netgroup,
    bool requires_carrier,
    DGBSatoshis proposed_network_fee,
    int64_t now,
    uint256& netgroup_bucket,
    std::string& error) const
{
    error.clear();
    netgroup_bucket.SetNull();
    if (request_hash.IsNull() || canonical_netgroup.empty() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_REQUEST_BUDGET_EVENT";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != request.intent.provider_id) {
        error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    ProviderSafetyPolicy safety_policy;
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = batch.HasPaymasterProviderSafetyPolicy() ? "PAYMASTER_INVALID_SAFETY_POLICY" : "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    ProviderBudgetLedger budget_ledger;
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger)) {
        error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        return false;
    }
    netgroup_bucket = GetNetgroupBudgetBucket(budget_ledger,
                                              canonical_netgroup);
    if (netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, request.intent.genesis_hash,
            request.intent.provider_id,
            request.intent.request_id, request.intent.session_id,
            request.intent.client_nonce, now, capacity_request,
            capacity_proof, capacity_request_hash, error)) {
        return false;
    }
    if (capacity_proof.snapshot_id.IsNull() ||
        capacity_request_hash != Hash(CanonicalBytes(capacity_request))) {
        error = "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH";
        return false;
    }
    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, request.intent.recipient_script);
    const ProviderSafetyStatus status =
        EvaluateProviderCapacityContinuationPreflight(
            budget_ledger, safety_policy, capacity_request, request,
            request_hash, requires_carrier, recipient_bucket,
            proposed_network_fee, now, netgroup_bucket);
    if (!status.can_accept_quote) {
        error = status.errors.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : status.errors.front();
        return false;
    }
    return true;
}

bool PaymasterStore::CheckProviderRecoveryAdmission(
    const AlternativeRecoveryRequest& request,
    const uint256& request_hash,
    const std::vector<unsigned char>& canonical_netgroup,
    bool requires_carrier,
    DGBSatoshis proposed_network_fee,
    int64_t now,
    uint256& netgroup_bucket,
    std::string& error) const
{
    error.clear();
    netgroup_bucket.SetNull();
    if (request_hash.IsNull() || canonical_netgroup.empty() || now <= 0 ||
        request.wallet_returns.empty() ||
        request_hash != GetAlternativeRecoveryRequestHash(request) ||
        !ValidateAlternativeRecoveryRequestEnvelope(
            request, request.genesis_hash, now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_RECOVERY_REQUEST";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != request.recovery_provider_id) {
        error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = batch.HasPaymasterProviderSafetyPolicy() ? "PAYMASTER_INVALID_SAFETY_POLICY" : "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger)) {
        error = batch.HasPaymasterProviderBudgetLedger() ? "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER" : "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        return false;
    }
    netgroup_bucket = GetNetgroupBudgetBucket(
        budget_ledger, canonical_netgroup);
    if (netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUCKET_UNAVAILABLE";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 persisted_capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, request.genesis_hash,
            request.recovery_provider_id,
            request.request_id, request.session_id, request.client_nonce, now,
            capacity_request, capacity_proof,
            persisted_capacity_request_hash, error) ||
        CanonicalBytes(capacity_request) !=
            CanonicalBytes(request.capacity_request) ||
        persisted_capacity_request_hash !=
            Hash(CanonicalBytes(capacity_request)) ||
        capacity_proof.snapshot_id != request.capacity_snapshot_id ||
        capacity_request.funding_model != FundingModel::USER_PAID ||
        capacity_request.requires_carrier != requires_carrier) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        return false;
    }

    const uint256 request_key = GetProviderRequestSlotKey(
        request.recovery_provider_id, request.request_id, request.session_id);
    const uint256 capacity_request_hash{Hash(CanonicalBytes(capacity_request))};
    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, request.wallet_returns.front().script_pub_key);
    const auto admission = std::find_if(
        budget_ledger.capacity_admissions.begin(),
        budget_ledger.capacity_admissions.end(),
        [&](const ProviderCapacityAdmission& candidate) {
            return candidate.request_key == request_key;
        });
    if (admission == budget_ledger.capacity_admissions.end()) {
        error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
        return false;
    }
    const int64_t effective_now = std::max(
        now, budget_ledger.accounting_time_high_water);
    if (request_key.IsNull() || recipient_bucket.IsNull() ||
        admission->request_hash != capacity_request_hash ||
        admission->funding_model != capacity_request.funding_model ||
        admission->requires_carrier != requires_carrier ||
        admission->expires_at != capacity_request.expires_at) {
        error = "PAYMASTER_CAPACITY_ADMISSION_CONFLICT";
        return false;
    }

    ProviderBudgetLedger candidate{budget_ledger};
    if (!BindProviderCapacityQuote(
            candidate, request_key, request_hash, netgroup_bucket,
            effective_now, error)) {
        return false;
    }
    HashWriter simulated_commit = TaggedHash(
        "DigiByte Paymaster Recovery Capacity Preflight v1");
    simulated_commit << request_key << capacity_request_hash << request_hash;
    if (!PromoteProviderCapacityAdmission(
            candidate, request_key, request_hash,
            simulated_commit.GetSHA256(), effective_now, error)) {
        return false;
    }
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        candidate, safety_policy, FundingModel::USER_PAID,
        SponsorshipScope::PUBLIC, recipient_bucket, proposed_network_fee,
        effective_now, netgroup_bucket);
    if (!status.can_accept_quote) {
        error = status.errors.empty() ? "PAYMASTER_SAFETY_LIMIT_EXHAUSTED" : status.errors.front();
        return false;
    }
    return true;
}

bool PaymasterStore::CommitProviderQuote(
    ProviderAttempt attempt,
    std::optional<SponsorshipAuthorizationRecord> sponsorship,
    const uint256& expected_genesis,
    int64_t now,
    std::string& error)
{
    error.clear();
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    try {
        CDataStream request_stream{attempt.quote_request, SER_NETWORK, ::PROTOCOL_VERSION};
        request_stream >> request;
        SpanReader response_stream{::PROTOCOL_VERSION, attempt.signed_quote};
        response_stream >> response;
        if (!request_stream.empty() || !response_stream.empty()) {
            throw std::ios_base::failure("trailing paymaster quote data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_PROVIDER_QUOTE_ENCODING";
        return false;
    }
    CDataStream canonical_request{SER_NETWORK, ::PROTOCOL_VERSION};
    CDataStream canonical_response{SER_NETWORK, ::PROTOCOL_VERSION};
    canonical_request << request;
    canonical_response << response;
    const auto request_bytes = MakeUCharSpan(canonical_request);
    const auto response_bytes = MakeUCharSpan(canonical_response);
    if (expected_genesis.IsNull() ||
        request.intent.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (!std::equal(request_bytes.begin(), request_bytes.end(),
                    attempt.quote_request.begin(), attempt.quote_request.end()) ||
        !std::equal(response_bytes.begin(), response_bytes.end(),
                    attempt.signed_quote.begin(), attempt.signed_quote.end()) ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.state != AttemptState::QUOTED || attempt.attempt_id.IsNull() ||
        attempt.created_at <= 0 || attempt.updated_at < attempt.created_at ||
        attempt.quote_expires_at <= attempt.created_at ||
        attempt.retry_until < attempt.quote_expires_at ||
        attempt.session_id != request.intent.session_id ||
        attempt.provider_id != request.intent.provider_id ||
        attempt.client_nonce != request.intent.client_nonce ||
        attempt.intent_hash != GetPaymentIntentHash(request.intent) ||
        response.request_id != request.intent.request_id ||
        response.session_id != request.intent.session_id ||
        response.quote.quote_id != attempt.quote_id ||
        response.quote.unsigned_txid != attempt.unsigned_txid ||
        response.quote.template_commitment != attempt.template_commitment ||
        attempt.provider_netgroup_bucket.IsNull() ||
        !attempt.accepted_client_manifest_id.IsNull() ||
        attempt.client_manifest_accepted_at != 0 ||
        attempt.provider_signed_at != 0 ||
        !attempt.provider_signed_result.empty() ||
        !attempt.capacity_proof_claim_candidate.empty() ||
        !attempt.quote_response_claim_candidate.empty() ||
        !ValidateQuotedTemplate(attempt, error) ||
        !ValidateAttemptAuthorizationManifest(attempt, /*provider_side=*/true,
                                              error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_QUOTE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    ProviderPolicy policy;
    ProviderSettings settings;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterIdentity(identity) ||
        identity.provider_id != attempt.provider_id ||
        attempt.provider_identity_key != identity.identity_key) {
        error = "PAYMASTER_PROVIDER_IDENTITY_MISMATCH";
        return false;
    }
    if (!batch.ReadPaymasterProviderSafetyPolicy(safety_policy)) {
        error = "PAYMASTER_SAFETY_POLICY_NOT_FOUND";
        return false;
    }
    if (!batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !ValidateProviderBudgetLedger(budget_ledger, error)) {
        if (error.empty()) {
            error = "PAYMASTER_PROVIDER_BUDGET_LEDGER_NOT_FOUND";
        }
        return false;
    }
    if (!batch.ReadPaymasterProviderPool(pool) ||
        !ValidateProviderPoolEntries(pool, error)) {
        if (error.empty()) error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }
    if (!batch.ReadPaymasterPolicy(policy) ||
        !ValidateProviderSafetyPolicy(safety_policy, policy, error)) {
        if (error.empty()) error = "PAYMASTER_POLICY_NOT_FOUND";
        return false;
    }
    const int64_t effective_now =
        std::max(now, budget_ledger.accounting_time_high_water);
    if (effective_now <= 0 ||
        !ValidateRedactedQuoteRequestEnvelope(
            request, expected_genesis, effective_now, error) ||
        !ValidateQuoteResponseEnvelope(
            response, expected_genesis, effective_now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_PROVIDER_QUOTE";
        return false;
    }

    ProviderAttempt existing;
    const bool have_existing =
        batch.ReadPaymasterAttempt(attempt.attempt_id, existing);
    if (!have_existing && batch.HasPaymasterAttempt(attempt.attempt_id)) {
        error = PersistedVersionError(
            "ProviderAttempt", existing.version,
            ProviderAttempt::CURRENT_VERSION,
            "PAYMASTER_INVALID_PERSISTED_ATTEMPT");
        return false;
    }
    if (have_existing &&
        CanonicalBytes(existing) != CanonicalBytes(attempt)) {
        error = "PAYMASTER_ATTEMPT_ID_CONFLICT";
        return false;
    }

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, expected_genesis, attempt.provider_id,
            request.intent.request_id, request.intent.session_id,
            request.intent.client_nonce, effective_now, capacity_request,
            capacity_proof, capacity_request_hash, error) ||
        capacity_request.funding_model != request.intent.funding_model ||
        capacity_request.requires_carrier !=
            response.quote.reserved_carrier.has_value()) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        return false;
    }

    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        budget_ledger, request.intent.recipient_script);
    const uint256 capacity_admission_key = GetProviderRequestSlotKey(
        attempt.provider_id, request.intent.request_id,
        request.intent.session_id);
    const uint256 quote_request_hash = Hash(attempt.quote_request);
    if (recipient_bucket.IsNull() || capacity_admission_key.IsNull() ||
        quote_request_hash.IsNull()) {
        error = "PAYMASTER_INVALID_RECIPIENT_BUDGET_BUCKET";
        return false;
    }
    const CapacityAdmissionState expected_admission_state =
        have_existing ? CapacityAdmissionState::PROMOTED : CapacityAdmissionState::RESERVED;
    if (!ValidateProviderCapacityAdmissionForCommit(
            budget_ledger, capacity_request, capacity_request_hash,
            quote_request_hash, attempt.commit_key,
            attempt.provider_netgroup_bucket, expected_admission_state,
            effective_now, error) ||
        !ValidateQuoteCapacityResources(
            capacity_proof, response.quote, attempt.provider_manifest, pool,
            request.intent.client_nonce,
            have_existing ? attempt.commit_key : request.intent.client_nonce,
            error)) {
        return false;
    }

    if (have_existing) {
        bool exact_sponsorship =
            existing.sponsorship_capability_hash.IsNull() && !sponsorship;
        if (sponsorship) {
            SponsorshipAuthorizationRecord persisted;
            exact_sponsorship =
                existing.sponsorship_capability_hash ==
                    sponsorship->capability_hash &&
                batch.ReadPaymasterSponsorshipAuthorization(
                    sponsorship->capability_hash, persisted) &&
                CanonicalBytes(persisted) == CanonicalBytes(*sponsorship) &&
                persisted.state == SponsorshipAuthorizationState::RESERVED;
        }
        if (!exact_sponsorship) {
            error = "PAYMASTER_SPONSORSHIP_QUOTE_CONFLICT";
            return false;
        }
        if (!ValidateProviderBudgetState(
                existing, safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true, error)) {
            return false;
        }
        const std::vector<unsigned char> budget_before =
            CanonicalBytes(budget_ledger);
        if (!BindProviderCapacityQuote(
                budget_ledger, capacity_admission_key, quote_request_hash,
                existing.provider_netgroup_bucket, effective_now, error) ||
            !PromoteProviderCapacityAdmission(
                budget_ledger, capacity_admission_key, quote_request_hash,
                existing.commit_key, effective_now, error) ||
            !ReserveProviderBudget(
                budget_ledger, safety_policy, request.intent.funding_model,
                request.intent.sponsorship_scope, existing.commit_key,
                response.quote.network_fee, recipient_bucket, effective_now,
                error, existing.provider_netgroup_bucket) ||
            !ValidateProviderBudgetState(
                existing, safety_policy, budget_ledger,
                BudgetReservationState::RESERVED,
                /*allow_historical_policy=*/true, error)) {
            return false;
        }
        if (budget_before != CanonicalBytes(budget_ledger)) {
            error = "PAYMASTER_PROVIDER_BUDGET_ATOMICITY_CONFLICT";
            return false;
        }
        return true;
    }

    if (attempt.provider_manifest.safety_policy_hash !=
        GetProviderSafetyPolicyHash(safety_policy)) {
        error = "PAYMASTER_PROVIDER_SAFETY_POLICY_CONFLICT";
        return false;
    }
    if (!batch.ReadPaymasterSettings(settings)) {
        error = batch.HasPaymasterSettings()
                    ? PersistedVersionError(
                          "ProviderSettings", settings.version,
                          ProviderSettings::CURRENT_VERSION,
                          "PAYMASTER_INVALID_PROVIDER_SETTINGS")
                    : "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (!settings.enabled ||
        settings.policy_hash != GetProviderPolicyHash(policy) ||
        !ValidatePaymasterQuote(response.quote, request.intent, policy,
                                identity.identity_key,
                                response.quote.service_fee, effective_now,
                                error)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (attempt.commit_key != GetPaymasterCommitKey(
                                  attempt.provider_id, attempt.client_nonce,
                                  attempt.intent_hash, attempt.quote_id,
                                  attempt.template_commitment)) {
        error = "PAYMASTER_INVALID_COMMIT_KEY";
        return false;
    }
    if (!BindProviderCapacityQuote(
            budget_ledger, capacity_admission_key, quote_request_hash,
            attempt.provider_netgroup_bucket, effective_now, error) ||
        !PromoteProviderCapacityAdmission(
            budget_ledger, capacity_admission_key, quote_request_hash,
            attempt.commit_key, effective_now, error) ||
        !ReserveProviderBudget(
            budget_ledger, safety_policy, request.intent.funding_model,
            request.intent.sponsorship_scope, attempt.commit_key,
            response.quote.network_fee, recipient_bucket, effective_now,
            error, attempt.provider_netgroup_bucket) ||
        !ValidateProviderBudgetState(
            attempt, safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false, error)) {
        return false;
    }

    const bool restricted =
        request.intent.funding_model == FundingModel::SPONSORED &&
        request.intent.sponsorship_scope == SponsorshipScope::RESTRICTED;
    if (restricted) {
        std::string validation_error;
        if (!sponsorship || attempt.sponsorship_capability_hash.IsNull() ||
            attempt.sponsorship_capability_hash !=
                request.intent.sponsorship_authorization_hash ||
            sponsorship->state != SponsorshipAuthorizationState::RESERVED ||
            sponsorship->capability_hash !=
                attempt.sponsorship_capability_hash ||
            sponsorship->payment_binding_hash != attempt.intent_hash ||
            sponsorship->reservation_id != attempt.commit_key ||
            sponsorship->reserved_at != now ||
            !ValidateSponsorshipAuthorizationRecord(*sponsorship,
                                                    validation_error)) {
            error = validation_error.empty() ? "PAYMASTER_INVALID_SPONSORSHIP_RESERVATION" : validation_error;
            return false;
        }
        SponsorshipAuthorizationRecord existing_sponsorship;
        if (batch.ReadPaymasterSponsorshipAuthorization(
                sponsorship->capability_hash, existing_sponsorship)) {
            error = "PAYMASTER_SPONSORSHIP_ALREADY_USED";
            return false;
        }
    } else if (sponsorship ||
               !attempt.sponsorship_capability_hash.IsNull() ||
               !request.intent.sponsorship_authorization_hash.IsNull()) {
        error = "PAYMASTER_UNEXPECTED_SPONSORSHIP_RESERVATION";
        return false;
    }

    const uint256 canonical_request_hash = quote_request_hash;
    PaymentSession session;
    if (batch.ReadPaymasterSession(request.intent.request_id, session)) {
        error = session.session_id == request.intent.session_id &&
                        session.canonical_request_hash == canonical_request_hash ?
                    "PAYMASTER_PROVIDER_QUOTE_CONFLICT" :
                    "PAYMASTER_REQUEST_ID_CONFLICT";
        return false;
    }
    if (batch.HasPaymasterSession(request.intent.request_id)) {
        error = PersistedVersionError(
            "PaymentSession", session.version,
            PaymentSession::CURRENT_VERSION,
            "PAYMASTER_INVALID_PERSISTED_SESSION");
        return false;
    }
    IdempotencyTombstone tombstone;
    if (batch.ReadPaymasterTombstone(request.intent.request_id, tombstone)) {
        error = "PAYMASTER_REQUEST_ID_CONFLICT";
        return false;
    }
    if (batch.HasPaymasterTombstone(request.intent.request_id)) {
        error = PersistedVersionError(
            "IdempotencyTombstone", tombstone.version,
            IdempotencyTombstone::CURRENT_VERSION,
            "PAYMASTER_INVALID_PERSISTED_TOMBSTONE");
        return false;
    }
    std::string indexed_request;
    if (batch.ReadPaymasterSessionId(request.intent.session_id,
                                     indexed_request)) {
        error = "PAYMASTER_SESSION_ID_CONFLICT";
        return false;
    }

    std::set<COutPoint> required;
    for (const VerifiedDGBInput& input : response.quote.reserved_dgb_inputs) {
        required.insert(input.outpoint);
    }
    if (response.quote.reserved_carrier) {
        required.insert(response.quote.reserved_carrier->outpoint);
    }
    size_t rebound{0};
    for (ProviderPoolEntry& entry : pool) {
        if (required.count(entry.outpoint) == 0) continue;
        // ValidateQuoteCapacityResources already proved every resource's role,
        // creating transaction, value, script, state and nonce reservation.
        entry.reservation_id = attempt.commit_key;
        entry.updated_at = effective_now;
        ++rebound;
    }
    if (required.empty() || rebound != required.size()) {
        error = "PAYMASTER_OPERATIONAL_SLOT_MISSING";
        return false;
    }

    session.request_id = request.intent.request_id;
    session.session_id = request.intent.session_id;
    session.canonical_request_hash = canonical_request_hash;
    session.fee_mode_requested = FeeMode::PAYMASTER;
    session.fee_mode_used = FeeMode::PAYMASTER;
    session.state = SessionState::INPUTS_RESERVED;
    session.user_inputs = request.intent.user_dd_inputs;
    session.attempt_ids = {attempt.attempt_id};
    session.created_at = attempt.created_at;
    session.updated_at = effective_now;
    session.provider_side = true;

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterSession(session, false) ||
        !batch.WritePaymasterSessionId(session.session_id,
                                       session.request_id, false) ||
        !batch.WritePaymasterAttempt(attempt, false) ||
        !batch.WritePaymasterTemplate(attempt.template_commitment,
                                      attempt.attempt_id, false) ||
        !batch.WritePaymasterUnsignedTx(attempt.unsigned_txid,
                                        attempt.attempt_id, false) ||
        !batch.WritePaymasterProviderPool(pool) ||
        !batch.WritePaymasterProviderBudgetLedger(budget_ledger) ||
        (sponsorship &&
         !batch.WritePaymasterSponsorshipAuthorization(*sponsorship,
                                                       false))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::UpdateAttempt(const std::string& request_id, const ProviderAttempt& update, std::string& error)
{
    error.clear();
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt current;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(update.attempt_id, current) || current.session_id != session.session_id) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    const bool final_artifact_known = AttemptHasReached(current.state, AttemptState::FINAL_COMMITTED) ||
                                      !current.final_txid.IsNull() ||
                                      !current.final_transaction.empty() ||
                                      session.state == SessionState::CONFIRMED;
    if (final_artifact_known &&
        (update.state == AttemptState::REJECTED || update.state == AttemptState::AMBIGUOUS)) {
        error = "PAYMASTER_INVALID_ATTEMPT_TRANSITION";
        return false;
    }
    if (update.version != current.version || update.session_id != current.session_id ||
        update.provider_id != current.provider_id || update.attempt_id != current.attempt_id ||
        update.privacy_profile != current.privacy_profile ||
        (current.provider_identity_key.IsFullyValid() &&
         current.provider_identity_key != update.provider_identity_key) ||
        update.created_at != current.created_at || update.updated_at < current.updated_at ||
        !CanTransition(current.state, update.state)) {
        error = "PAYMASTER_INVALID_ATTEMPT_TRANSITION";
        return false;
    }
    if (!current.capacity_proof_claim_candidate.empty()) {
        PaymasterCapacityRequest authoritative_capacity_request;
        if (!ValidatePersistedCapacityRequestAuthority(
                session, current, authoritative_capacity_request, error) ||
            !ValidatePersistedCapacityClaimCandidate(
                current.capacity_proof_claim_candidate,
                authoritative_capacity_request,
                current.provider_identity_key,
                "PAYMASTER_PERSISTED_CAPACITY_CLAIM_CANDIDATE_CORRUPT",
                error)) {
            return false;
        }
    }
    if (!current.quote_response_claim_candidate.empty()) {
        PaymasterQuoteRequest authoritative_quote_request;
        if (!ValidatePersistedQuoteRequestAuthority(
                session, current, authoritative_quote_request, error) ||
            !ValidatePersistedQuoteClaimCandidate(
                session, current, authoritative_quote_request, error)) {
            return false;
        }
    }
    if (current.capacity_request != update.capacity_request ||
        !SameCapacitySnapshot(current.capacity_snapshot, update.capacity_snapshot)) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_CONFLICT";
        return false;
    }
    // Signed claim candidates are written only by the dedicated staging
    // methods. Generic attempt updates may neither manufacture nor erase
    // evidence-bearing remote claims.
    if (current.capacity_proof_claim_candidate !=
            update.capacity_proof_claim_candidate ||
        current.quote_response_claim_candidate !=
            update.quote_response_claim_candidate) {
        error = "PAYMASTER_SIGNED_CLAIM_CANDIDATE_CONFLICT";
        return false;
    }
    // Explicit client approval can only be created by
    // AcceptClientAuthorization(). Generic/stale attempt updates must neither
    // manufacture, replace nor clear it.
    if (current.accepted_client_manifest_id !=
            update.accepted_client_manifest_id ||
        current.client_manifest_accepted_at !=
            update.client_manifest_accepted_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_ACCEPTANCE_CONFLICT";
        return false;
    }
    if (session.provider_side &&
        (!current.accepted_client_manifest_id.IsNull() ||
         current.client_manifest_accepted_at != 0)) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_ON_PROVIDER_ATTEMPT";
        return false;
    }
    if (current.accepted_client_manifest_id.IsNull() !=
        (current.client_manifest_accepted_at == 0)) {
        error = "PAYMASTER_INVALID_CLIENT_AUTHORIZATION_ACCEPTANCE";
        return false;
    }
    if (!session.provider_side && current.signed_quote.empty() &&
        !update.signed_quote.empty()) {
        if (current.quote_response_claim_candidate.empty()) {
            error = "PAYMASTER_QUOTE_CLAIM_CANDIDATE_MISSING";
            return false;
        }
        PaymasterQuoteRequest authoritative_request;
        if (!ValidatePersistedQuoteRequestAuthority(
                session, current, authoritative_request, error) ||
            !ValidatePersistedQuoteClaimCandidate(
                session, current, authoritative_request, error)) {
            return false;
        }
        if (current.quote_response_claim_candidate != update.signed_quote) {
            std::string evidence_error;
            if (RecordQuoteEquivocationPairLocked(
                    batch, session, current,
                    current.quote_response_claim_candidate,
                    update.signed_quote, update.updated_at, evidence_error)) {
                error = evidence_error.empty() ? "PAYMASTER_QUOTE_EQUIVOCATION" : std::move(evidence_error);
                return false;
            }
            if (!evidence_error.empty()) {
                error = std::move(evidence_error);
                return false;
            }
            error = "PAYMASTER_QUOTE_CLAIM_CANDIDATE_CONFLICT";
            return false;
        }
    }
    if (!session.provider_side && !current.signed_quote.empty() &&
        !update.signed_quote.empty() && current.signed_quote != update.signed_quote) {
        std::string evidence_error;
        if (RecordQuoteEquivocationLocked(batch, session, current,
                                          update.signed_quote, update.updated_at,
                                          evidence_error)) {
            error = std::move(evidence_error);
            return false;
        }
        if (!evidence_error.empty()) {
            error = std::move(evidence_error);
            return false;
        }
    }

    const auto immutable_hash = [&error](const uint256& old_value, const uint256& new_value, const char* code) {
        if (!old_value.IsNull() && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    const auto immutable_bytes = [&error](const std::vector<unsigned char>& old_value,
                                          const std::vector<unsigned char>& new_value,
                                          const char* code) {
        if (!old_value.empty() && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    const auto immutable_string = [&error](const std::string& old_value,
                                           const std::string& new_value,
                                           const char* code) {
        if (!old_value.empty() && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    const auto immutable_time = [&error](int64_t old_value, int64_t new_value, const char* code) {
        if (old_value != 0 && old_value != new_value) {
            error = code;
            return false;
        }
        return true;
    };
    if (!immutable_hash(current.commit_key, update.commit_key, "PAYMASTER_COMMIT_KEY_CONFLICT") ||
        !immutable_hash(current.client_nonce, update.client_nonce, "PAYMASTER_CLIENT_NONCE_CONFLICT") ||
        !immutable_hash(current.intent_hash, update.intent_hash, "PAYMASTER_INTENT_CONFLICT") ||
        !immutable_hash(current.quote_id, update.quote_id, "PAYMASTER_QUOTE_CONFLICT") ||
        !immutable_hash(current.unsigned_txid, update.unsigned_txid, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_hash(current.template_commitment, update.template_commitment, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_hash(current.sponsorship_capability_hash, update.sponsorship_capability_hash,
                        "PAYMASTER_SPONSORSHIP_AUTHORIZATION_CONFLICT") ||
        !immutable_hash(current.client_manifest.manifest_id,
                        update.client_manifest.manifest_id,
                        "PAYMASTER_CLIENT_AUTH_MANIFEST_CONFLICT") ||
        !immutable_hash(current.provider_manifest.manifest_id,
                        update.provider_manifest.manifest_id,
                        "PAYMASTER_PROVIDER_AUTH_MANIFEST_CONFLICT") ||
        current.provider_netgroup_bucket != update.provider_netgroup_bucket ||
        !immutable_hash(current.final_txid, update.final_txid, "PAYMASTER_FINAL_TX_CONFLICT") ||
        !immutable_string(current.provider_endpoint, update.provider_endpoint,
                          "PAYMASTER_PROVIDER_ENDPOINT_CONFLICT") ||
        !immutable_bytes(current.unsigned_intent, update.unsigned_intent, "PAYMASTER_INTENT_CONFLICT") ||
        !immutable_bytes(current.quote_request, update.quote_request, "PAYMASTER_QUOTE_REQUEST_CONFLICT") ||
        !immutable_bytes(current.signed_quote, update.signed_quote, "PAYMASTER_QUOTE_CONFLICT") ||
        !immutable_bytes(current.unsigned_transaction, update.unsigned_transaction, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_bytes(current.unsigned_psbt, update.unsigned_psbt, "PAYMASTER_TEMPLATE_CONFLICT") ||
        !immutable_bytes(current.user_signed_psbt, update.user_signed_psbt, "PAYMASTER_USER_PSBT_CONFLICT") ||
        !immutable_bytes(current.final_transaction, update.final_transaction, "PAYMASTER_FINAL_TX_CONFLICT") ||
        !immutable_time(current.provider_signed_at,
                        update.provider_signed_at,
                        "PAYMASTER_PROVIDER_SIGNATURE_TIME_CONFLICT") ||
        !immutable_bytes(current.provider_signed_result,
                         update.provider_signed_result,
                         "PAYMASTER_PROVIDER_SIGNED_RESULT_CONFLICT") ||
        !immutable_time(current.quote_expires_at, update.quote_expires_at, "PAYMASTER_QUOTE_EXPIRY_CONFLICT") ||
        !immutable_time(current.retry_until, update.retry_until, "PAYMASTER_RETRY_EXPIRY_CONFLICT")) {
        if (error.empty()) error = "PAYMASTER_NETGROUP_BUDGET_BINDING_CONFLICT";
        return false;
    }

    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        (update.commit_key.IsNull() || update.client_nonce.IsNull() || update.intent_hash.IsNull() ||
         update.quote_id.IsNull() || update.unsigned_txid.IsNull() || update.template_commitment.IsNull() ||
         update.signed_quote.empty() ||
         update.unsigned_transaction.empty() || update.unsigned_psbt.empty() || update.input_roles.empty() ||
         update.quote_expires_at <= update.created_at ||
         update.retry_until < update.quote_expires_at)) {
        error = "PAYMASTER_INCOMPLETE_QUOTE_ATTEMPT";
        return false;
    }
    if (session.provider_side && AttemptHasReached(update.state, AttemptState::QUOTED) &&
        update.provider_netgroup_bucket.IsNull()) {
        error = "PAYMASTER_NETGROUP_BUDGET_BINDING_REQUIRED";
        return false;
    }
    if (!session.provider_side && AttemptHasReached(update.state, AttemptState::QUOTED) &&
        (update.capacity_request.empty() || update.capacity_snapshot.snapshot_id.IsNull() ||
         update.capacity_snapshot.request_hash != Hash(update.capacity_request) ||
         update.capacity_snapshot.provider_id != update.provider_id ||
         update.capacity_snapshot.client_nonce != update.client_nonce ||
         update.capacity_snapshot.expires_at <= update.updated_at)) {
        error = "PAYMASTER_CAPACITY_PROOF_REQUIRED";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        update.commit_key != GetPaymasterCommitKey(update.provider_id, update.client_nonce,
                                                   update.intent_hash, update.quote_id,
                                                   update.template_commitment)) {
        error = "PAYMASTER_INVALID_COMMIT_KEY";
        return false;
    }
    if (!current.input_roles.empty() && current.input_roles != update.input_roles) {
        error = "PAYMASTER_TEMPLATE_CONFLICT";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        !ValidateQuotedTemplate(update, error)) return false;
    if (AttemptHasReached(update.state, AttemptState::QUOTED) &&
        ((session.provider_side && update.provider_manifest.manifest_id.IsNull()) ||
         (!session.provider_side && update.client_manifest.manifest_id.IsNull()) ||
         !ValidateAttemptAuthorizationManifest(update, session.provider_side,
                                               error))) {
        if (error.empty()) error = session.provider_side ? "PAYMASTER_PROVIDER_AUTH_MANIFEST_REQUIRED" : "PAYMASTER_CLIENT_AUTH_MANIFEST_REQUIRED";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::USER_SIGNED) && update.user_signed_psbt.empty()) {
        error = "PAYMASTER_USER_PSBT_MISSING";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED) &&
        (update.final_transaction.empty() || update.final_txid.IsNull())) {
        error = "PAYMASTER_FINAL_TRANSACTION_MISSING";
        return false;
    }
    const bool introducing_provider_signature =
        session.provider_side &&
        current.state == AttemptState::USER_PSBT_ACCEPTED &&
        update.state == AttemptState::PROVIDER_SIGNED;
    if (session.provider_side) {
        const bool provider_authority_known =
            AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED);
        if (introducing_provider_signature) {
            PaymasterResult signed_result;
            if (current.provider_signed_at != 0 ||
                !current.provider_signed_result.empty() ||
                update.provider_signed_at != update.updated_at ||
                !DecodeProviderSignedResultEnvelope(
                    update, signed_result, error)) {
                if (error.empty()) {
                    error = "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID";
                }
                return false;
            }
        } else if (provider_authority_known &&
                   (update.provider_signed_at == 0 ||
                    update.provider_signed_result.empty())) {
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_MISSING";
            return false;
        } else if (provider_authority_known &&
                   update.provider_signed_at != 0 &&
                   !update.provider_signed_result.empty()) {
            PaymasterResult signed_result;
            if (!DecodeProviderSignedResultEnvelope(
                    update, signed_result, error)) {
                return false;
            }
        } else if (!provider_authority_known &&
                   (update.provider_signed_at != 0 ||
                    !update.provider_signed_result.empty())) {
            error = "PAYMASTER_PROVIDER_SIGNED_RESULT_PREMATURE";
            return false;
        }
    } else if (current.provider_signed_at != update.provider_signed_at ||
               current.provider_signed_result !=
                   update.provider_signed_result) {
        error = "PAYMASTER_PROVIDER_SIGNED_RESULT_CLIENT_CONFLICT";
        return false;
    }
    if (AttemptHasReached(update.state, AttemptState::USER_PSBT_ACCEPTED)) {
        UserAuthorizationRecord authorization;
        if (!batch.ReadPaymasterUserAuthorization(update.commit_key, authorization) ||
            authorization.attempt_id != update.attempt_id ||
            authorization.canonical_psbt_hash != Hash(update.user_signed_psbt)) {
            error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
            return false;
        }
    }

    const bool newly_user_signed = !session.provider_side &&
                                   !AttemptHasReached(current.state, AttemptState::USER_SIGNED) &&
                                   AttemptHasReached(update.state, AttemptState::USER_SIGNED);
    if (newly_user_signed) {
        if (current.accepted_client_manifest_id.IsNull() ||
            current.accepted_client_manifest_id !=
                current.client_manifest.manifest_id ||
            current.client_manifest_accepted_at <= 0 ||
            current.client_manifest_accepted_at > update.updated_at) {
            error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_ACCEPTED";
            return false;
        }
        PaymasterQuoteResponse quote_response;
        try {
            SpanReader quote_stream{::PROTOCOL_VERSION, update.signed_quote};
            quote_stream >> quote_response;
            if (!quote_stream.empty()) {
                throw std::ios_base::failure("trailing paymaster quote data");
            }
        } catch (const std::ios_base::failure&) {
            error = "PAYMASTER_QUOTE_ENCODING";
            return false;
        }
        ClientSafetyPolicy client_policy;
        ClientFeeLedger client_fee_ledger;
        const bool have_client_policy = batch.ReadPaymasterClientSafetyPolicy(client_policy);
        const bool have_client_ledger = batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
        if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
            (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
            have_client_policy != have_client_ledger) {
            error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
            return false;
        }
        if (!have_client_policy) {
            if (quote_response.quote.service_fee.value != 0) {
                error = "PAYMASTER_CLIENT_SAFETY_POLICY_NOT_FOUND";
                return false;
            }
        } else {
            const auto reservation = std::find_if(
                client_fee_ledger.reservations.begin(),
                client_fee_ledger.reservations.end(),
                [&](const ClientFeeReservation& entry) {
                    return entry.commit_key == update.commit_key;
                });
            if (reservation == client_fee_ledger.reservations.end() ||
                reservation->service_fee != quote_response.quote.service_fee ||
                reservation->state != BudgetReservationState::RESERVED) {
                error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                return false;
            }
        }
    }

    const bool authorization_risk = AttemptHasReached(update.state, AttemptState::USER_SIGNED);
    const bool wallet_authorization_risk = session.provider_side ? AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED) : authorization_risk;
    const uint64_t current_wallet_flags = m_wallet.GetWalletFlags();
    const uint64_t protected_wallet_flags =
        current_wallet_flags | WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    const bool protect_wallet = wallet_authorization_risk &&
                                protected_wallet_flags != current_wallet_flags;
    if (authorization_risk && session.state != SessionState::AUTHORIZED &&
        !HasAuthorizationRisk(session.state)) {
        error = "PAYMASTER_INVALID_SESSION_TRANSITION";
        return false;
    }

    uint256 indexed_attempt;
    const bool add_template_index = current.template_commitment.IsNull() &&
                                    !update.template_commitment.IsNull();
    if (add_template_index &&
        batch.ReadPaymasterTemplate(update.template_commitment, indexed_attempt) &&
        indexed_attempt != update.attempt_id) {
        error = "PAYMASTER_TEMPLATE_ALREADY_COMMITTED";
        return false;
    }
    uint256 indexed_tx_attempt;
    const bool add_unsigned_tx_index = current.unsigned_txid.IsNull() && !update.unsigned_txid.IsNull();
    if (add_unsigned_tx_index &&
        batch.ReadPaymasterUnsignedTx(update.unsigned_txid, indexed_tx_attempt) &&
        indexed_tx_attempt != update.attempt_id) {
        error = "PAYMASTER_UNSIGNED_TX_ALREADY_COMMITTED";
        return false;
    }
    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (!batch.WritePaymasterAttempt(update)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (add_template_index && indexed_attempt.IsNull() &&
        !batch.WritePaymasterTemplate(update.template_commitment, update.attempt_id, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (add_unsigned_tx_index && indexed_tx_attempt.IsNull() &&
        !batch.WritePaymasterUnsignedTx(update.unsigned_txid, update.attempt_id, false)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    // Wallet/mempool callbacks can observe the committed transaction while the
    // provider RPC is still advancing its durable attempt. Do not regress an
    // already observed network or terminal session state back to pending.
    if (authorization_risk &&
        (session.state == SessionState::AUTHORIZED ||
         session.state == SessionState::PENDING_PROVIDER)) {
        session.state = SessionState::PENDING_PROVIDER;
        session.pending_phase = AttemptHasReached(update.state, AttemptState::PROVIDER_SIGNED) ? PendingPhase::PROVIDER_SIGNED_KNOWN : PendingPhase::USER_SIGNATURE_SENT;
        if (!session.provider_side) {
            for (const auto& outpoint : session.user_inputs) {
                InputReservation reservation;
                if (!batch.ReadPaymasterReservation(outpoint, reservation)) {
                    return Abort(batch, error, "PAYMASTER_RESERVATION_MISSING");
                }
                reservation.authorization_may_exist = true;
                if (!batch.WritePaymasterReservation(reservation)) {
                    return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
                }
            }
        }
    }
    if (protect_wallet && !batch.WriteWalletFlags(protected_wallet_flags)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    session.updated_at = std::max(session.updated_at, update.updated_at);
    if (!batch.WritePaymasterSession(session)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    // The durable flag was committed with the attempt. Publish exactly that
    // value to memory without issuing a second, fallible database write.
    if (protect_wallet && !m_wallet.LoadWalletFlags(protected_wallet_flags)) {
        assert(false);
    }
    return true;
}

bool PaymasterStore::AcceptClientAuthorization(
    const std::string& request_id,
    const uint256& attempt_id,
    const uint256& manifest_id,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (!IsCanonicalRequestId(request_id) || attempt_id.IsNull() ||
        manifest_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_CLIENT_AUTHORIZATION_ACCEPTANCE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    ProviderAttempt attempt;
    if (!batch.ReadPaymasterSession(request_id, session) ||
        !batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.session_id != session.session_id ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (session.provider_side) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_ON_PROVIDER_ATTEMPT";
        return false;
    }
    if (attempt.accepted_client_manifest_id.IsNull() !=
        (attempt.client_manifest_accepted_at == 0)) {
        error = "PAYMASTER_INVALID_CLIENT_AUTHORIZATION_ACCEPTANCE";
        return false;
    }
    const bool already_accepted = !attempt.accepted_client_manifest_id.IsNull();
    if (already_accepted) {
        if (attempt.accepted_client_manifest_id != manifest_id ||
            attempt.accepted_client_manifest_id !=
                attempt.client_manifest.manifest_id) {
            error = "PAYMASTER_CLIENT_AUTHORIZATION_CONFLICT";
            return false;
        }
    }
    if (already_accepted && attempt.state != AttemptState::QUOTED) {
        // Once a signature exists, acceptance is immutable and its budget
        // reservation is checked by the signed-state transition itself.
        return true;
    }
    if (!already_accepted && attempt.state != AttemptState::QUOTED) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_AWAITING_ACCEPTANCE";
        return false;
    }
    if (attempt.version != ProviderAttempt::CURRENT_VERSION) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_NOT_AWAITING_ACCEPTANCE";
        return false;
    }
    if (attempt.client_manifest.manifest_id.IsNull() ||
        manifest_id != attempt.client_manifest.manifest_id) {
        error = "PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH";
        return false;
    }
    if (attempt.client_manifest.canonical_request_hash !=
            session.canonical_request_hash ||
        attempt.client_manifest.requested_fee_mode !=
            session.fee_mode_requested ||
        attempt.client_manifest.privacy_profile != attempt.privacy_profile) {
        error = "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH";
        return false;
    }

    const int64_t effective_now =
        std::max(now, std::max(session.updated_at, attempt.updated_at));
    if (attempt.quote_expires_at <= 0 ||
        attempt.client_manifest.expires_at <= 0 ||
        attempt.capacity_snapshot.expires_at <= 0 ||
        effective_now > attempt.quote_expires_at ||
        effective_now > attempt.client_manifest.expires_at ||
        effective_now > attempt.capacity_snapshot.expires_at) {
        error = "PAYMASTER_CLIENT_AUTHORIZATION_EXPIRED";
        return false;
    }
    if (!ValidateQuotedTemplate(attempt, error) ||
        !ValidateAttemptAuthorizationManifest(
            attempt, /*provider_side=*/false, error)) {
        if (error.empty()) error = "PAYMASTER_CLIENT_AUTHORIZATION_INVALID";
        return false;
    }

    PaymasterQuoteResponse quote_response;
    try {
        SpanReader quote_stream{::PROTOCOL_VERSION, attempt.signed_quote};
        quote_stream >> quote_response;
        if (!quote_stream.empty()) {
            throw std::ios_base::failure("trailing paymaster quote data");
        }
    } catch (const std::ios_base::failure&) {
        error = "PAYMASTER_QUOTE_ENCODING";
        return false;
    }

    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    const bool have_client_policy =
        batch.ReadPaymasterClientSafetyPolicy(client_policy);
    const bool have_client_ledger =
        batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
    if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
        (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
        have_client_policy != have_client_ledger) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }
    bool client_fee_changed{false};
    if (!have_client_policy) {
        if (quote_response.quote.service_fee.value != 0) {
            error = "PAYMASTER_CLIENT_SAFETY_POLICY_NOT_FOUND";
            return false;
        }
    } else {
        const auto existing_fee = std::find_if(
            client_fee_ledger.reservations.begin(),
            client_fee_ledger.reservations.end(),
            [&](const ClientFeeReservation& reservation) {
                return reservation.commit_key == attempt.commit_key;
            });
        const bool exact_reserved_fee =
            existing_fee != client_fee_ledger.reservations.end() &&
            existing_fee->service_fee == quote_response.quote.service_fee &&
            existing_fee->state == BudgetReservationState::RESERVED;
        if (!already_accepted && exact_reserved_fee) {
            // A RESERVED row without the corresponding durable manifest
            // acceptance is an orphaned atomic half, not historical
            // authorization. Never silently repair it into authority.
            error = "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_ORPHAN";
            return false;
        }
        if (!exact_reserved_fee) {
            if (!ReserveClientFee(client_fee_ledger, client_policy,
                                  attempt.commit_key,
                                  quote_response.quote.service_fee,
                                  effective_now, error)) {
                return false;
            }
            client_fee_changed = true;
        }
    }

    if (already_accepted && !client_fee_changed) return true;

    if (!already_accepted) {
        attempt.accepted_client_manifest_id = manifest_id;
        attempt.client_manifest_accepted_at = effective_now;
        attempt.updated_at = effective_now;
        session.updated_at = effective_now;
    }
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if ((!already_accepted &&
         (!batch.WritePaymasterAttempt(attempt) ||
          !batch.WritePaymasterSession(session))) ||
        (client_fee_changed &&
         !batch.WritePaymasterClientFeeLedger(client_fee_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::GetAttempt(const uint256& attempt_id, ProviderAttempt& attempt) const
{
    LOCK(m_wallet.cs_wallet);
    return WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterAttempt(attempt_id, attempt);
}

bool PaymasterStore::ValidateProviderBudgetAuthorization(
    const ProviderAttempt& attempt,
    BudgetReservationState expected_state,
    bool allow_historical_policy,
    std::string& error) const
{
    error.clear();
    if (attempt.attempt_id.IsNull() || attempt.commit_key.IsNull()) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_BINDING";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt persisted;
    ProviderPolicy advertised_policy;
    ProviderSafetyPolicy policy;
    ProviderBudgetLedger ledger;
    if (!batch.ReadPaymasterAttempt(attempt.attempt_id, persisted) ||
        persisted.commit_key != attempt.commit_key ||
        persisted.provider_manifest.manifest_id !=
            attempt.provider_manifest.manifest_id) {
        error = "PAYMASTER_PROVIDER_BUDGET_ATTEMPT_CONFLICT";
        return false;
    }
    const bool have_advertised_policy =
        batch.ReadPaymasterPolicy(advertised_policy);
    const bool have_policy =
        batch.ReadPaymasterProviderSafetyPolicy(policy);
    const bool have_ledger =
        batch.ReadPaymasterProviderBudgetLedger(ledger);
    if (!have_advertised_policy) {
        error = "PAYMASTER_PROVIDER_POLICY_NOT_FOUND";
        return false;
    }
    if (!have_policy || !have_ledger) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(policy, advertised_policy, error)) {
        return false;
    }
    if (!ValidateProviderBudgetLedger(ledger, error)) return false;
    return ValidateProviderBudgetState(
        persisted, policy, ledger, expected_state,
        allow_historical_policy, error);
}

bool PaymasterStore::ValidateProviderPreSignatureAuthorization(
    const ProviderAttempt& attempt,
    const uint256& expected_genesis,
    int64_t now,
    PaymasterCapacityRequest& capacity_request,
    PaymasterCapacityProof& capacity_proof,
    std::string& error) const
{
    error.clear();
    capacity_request = {};
    capacity_proof = {};
    if (expected_genesis.IsNull() || now <= 0 ||
        attempt.version != ProviderAttempt::CURRENT_VERSION ||
        attempt.attempt_id.IsNull() || attempt.commit_key.IsNull() ||
        attempt.provider_id.IsNull() || attempt.provider_netgroup_bucket.IsNull() ||
        attempt.state != AttemptState::USER_PSBT_ACCEPTED ||
        attempt.user_signed_psbt.empty() || attempt.created_at <= 0 ||
        now < attempt.created_at || now > attempt.retry_until) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_STATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt persisted;
    if (!batch.ReadPaymasterAttempt(attempt.attempt_id, persisted) ||
        CanonicalBytes(persisted) != CanonicalBytes(attempt)) {
        error = "PAYMASTER_PROVIDER_AUTHORIZATION_STATE";
        return false;
    }

    ProviderIdentityRecord identity;
    ProviderPolicy advertised_policy;
    ProviderSettings settings;
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterIdentity(identity) ||
        !batch.ReadPaymasterPolicy(advertised_policy) ||
        !batch.ReadPaymasterSettings(settings) || !settings.enabled ||
        !batch.ReadPaymasterProviderSafetyPolicy(safety_policy) ||
        !batch.ReadPaymasterProviderBudgetLedger(budget_ledger) ||
        !batch.ReadPaymasterProviderPool(pool) ||
        identity.provider_id != persisted.provider_id ||
        identity.identity_key != persisted.provider_identity_key ||
        !identity.identity_key.IsFullyValid() ||
        GetPaymasterId(identity.identity_key) != persisted.provider_id) {
        error = "PAYMASTER_PROVIDER_NOT_READY";
        return false;
    }
    if (!ValidateProviderSafetyPolicy(
            safety_policy, advertised_policy, error) ||
        !ValidateProviderBudgetLedger(budget_ledger, error) ||
        !ValidateProviderPoolEntries(pool, error) ||
        !ValidateProviderBudgetState(
            persisted, safety_policy, budget_ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true, error)) {
        return false;
    }

    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    CollaborativePSBTTemplate trusted;
    if (!LoadAttemptAuthorizationArtifacts(
            persisted, request, response, trusted, error) ||
        !ValidateProviderAuthorizationManifest(
            persisted.provider_manifest, request.intent, response.quote,
            trusted, error) ||
        request.intent.genesis_hash != expected_genesis ||
        request.intent.provider_id != identity.provider_id ||
        request.intent.session_id != persisted.session_id ||
        request.intent.client_nonce != persisted.client_nonce ||
        request.intent.policy_hash != settings.policy_hash ||
        response.quote.policy_hash != settings.policy_hash ||
        settings.policy_hash != GetProviderPolicyHash(advertised_policy)) {
        if (error.empty()) error = "PAYMASTER_PROVIDER_POLICY_NOT_CURRENT";
        return false;
    }

    PaymentSession session;
    UserAuthorizationRecord authorization;
    if (!batch.ReadPaymasterSession(request.intent.request_id, session) ||
        !session.provider_side || session.session_id != persisted.session_id ||
        session.state != SessionState::PENDING_PROVIDER ||
        session.pending_phase != PendingPhase::USER_SIGNATURE_SENT ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(),
                  persisted.attempt_id) == session.attempt_ids.end() ||
        !batch.ReadPaymasterUserAuthorization(persisted.commit_key,
                                              authorization) ||
        authorization.version != UserAuthorizationRecord::CURRENT_VERSION ||
        authorization.commit_key != persisted.commit_key ||
        authorization.attempt_id != persisted.attempt_id ||
        authorization.canonical_psbt_hash !=
            Hash(persisted.user_signed_psbt) ||
        authorization.accepted_at <= 0 || authorization.accepted_at > now ||
        authorization.retry_until != persisted.retry_until) {
        error = "PAYMASTER_USER_AUTHORIZATION_MISSING";
        return false;
    }

    uint256 capacity_request_hash;
    if (!ReadProviderCapacityRequestForContinuation(
            batch, identity, expected_genesis, persisted.provider_id,
            request.intent.request_id, request.intent.session_id,
            request.intent.client_nonce, now, capacity_request,
            capacity_proof, capacity_request_hash, error) ||
        capacity_request.funding_model != request.intent.funding_model ||
        capacity_request.requires_carrier !=
            response.quote.reserved_carrier.has_value() ||
        !ValidateProviderCapacityAdmissionForCommit(
            budget_ledger, capacity_request, capacity_request_hash,
            Hash(persisted.quote_request), persisted.commit_key,
            persisted.provider_netgroup_bucket,
            CapacityAdmissionState::PROMOTED, now, error) ||
        !ValidateQuoteCapacityResources(
            capacity_proof, response.quote, persisted.provider_manifest, pool,
            request.intent.client_nonce, persisted.commit_key, error)) {
        if (error.empty()) error = "PAYMASTER_CAPACITY_CONTINUATION_MISMATCH";
        capacity_request = {};
        capacity_proof = {};
        return false;
    }
    return true;
}

bool PaymasterStore::GetAttemptByTemplateCommitment(const uint256& template_commitment,
                                                    ProviderAttempt& attempt) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 attempt_id;
    return batch.ReadPaymasterTemplate(template_commitment, attempt_id) &&
           batch.ReadPaymasterAttempt(attempt_id, attempt) &&
           attempt.template_commitment == template_commitment;
}

bool PaymasterStore::GetAttemptByUnsignedTxid(const uint256& unsigned_txid,
                                              ProviderAttempt& attempt) const
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    uint256 attempt_id;
    return batch.ReadPaymasterUnsignedTx(unsigned_txid, attempt_id) &&
           batch.ReadPaymasterAttempt(attempt_id, attempt) &&
           attempt.unsigned_txid == unsigned_txid;
}

bool PaymasterStore::CancelProviderQuote(const uint256& attempt_id,
                                         int64_t now,
                                         ProviderAttempt& attempt,
                                         std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_CANCELLATION";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt)) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    const bool have_safety_policy = batch.ReadPaymasterProviderSafetyPolicy(safety_policy);
    const bool have_budget_ledger = batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if ((!have_safety_policy && batch.HasPaymasterProviderSafetyPolicy()) ||
        (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger())) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    bool budget_changed{false};
    if (!attempt.commit_key.IsNull() && have_safety_policy && have_budget_ledger) {
        const auto reservation = std::find_if(
            budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation != budget_ledger.reservations.end()) {
            const BudgetReservationState previous_state = reservation->state;
            if (!ReleaseProviderBudget(budget_ledger, attempt.commit_key, now, error)) {
                return false;
            }
            budget_changed = previous_state != BudgetReservationState::RELEASED;
        } else {
            error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
            return false;
        }
    }
    if (attempt.state == AttemptState::REJECTED) {
        if (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
            error = "PAYMASTER_DATABASE_WRITE";
            return false;
        }
        return true;
    }
    if (attempt.state != AttemptState::CANDIDATE && attempt.state != AttemptState::QUOTED &&
        attempt.state != AttemptState::QUOTE_EXPIRED) {
        error = "PAYMASTER_QUOTE_AUTHORIZATION_MAY_EXIST";
        return false;
    }
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) == session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
        return false;
    }

    std::vector<ProviderPoolEntry> pool;
    const DatabaseReadStatus pool_status{
        batch.ReadPaymasterProviderPoolWithStatus(pool)};
    if (pool_status != DatabaseReadStatus::FOUND &&
        pool_status != DatabaseReadStatus::NOT_FOUND) {
        error = ProviderPoolReadError(pool);
        return false;
    }
    const bool have_pool{pool_status == DatabaseReadStatus::FOUND};
    bool pool_changed{false};
    size_t released_entries{0};
    if (!attempt.commit_key.IsNull() && have_pool) {
        for (ProviderPoolEntry& entry : pool) {
            if (entry.reservation_id != attempt.commit_key) continue;
            if (entry.state != PoolEntryState::RESERVED) {
                error = "PAYMASTER_POOL_RESERVATION_NOT_CANCELABLE";
                return false;
            }
            entry.state = PoolEntryState::AVAILABLE;
            entry.reservation_id.SetNull();
            entry.updated_at = now;
            pool_changed = true;
            ++released_entries;
        }
    }
    if (attempt.state != AttemptState::CANDIDATE &&
        (attempt.commit_key.IsNull() || released_entries == 0)) {
        error = "PAYMASTER_POOL_RESERVATION_MISSING";
        return false;
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    if (attempt.state != AttemptState::QUOTE_EXPIRED) attempt.state = AttemptState::REJECTED;
    attempt.updated_at = std::max(attempt.updated_at, now);
    if (!batch.WritePaymasterAttempt(attempt)) return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    if (pool_changed && !batch.WritePaymasterProviderPool(pool)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::ExpireProviderQuotes(int64_t now,
                                          size_t& expired_quotes,
                                          std::string& error)
{
    error.clear();
    expired_quotes = 0;
    if (now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_EXPIRY_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }

    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    const bool have_safety_policy = batch.ReadPaymasterProviderSafetyPolicy(safety_policy);
    const bool have_budget_ledger = batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if ((!have_safety_policy && batch.HasPaymasterProviderSafetyPolicy()) ||
        (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger()) ||
        have_safety_policy != have_budget_ledger) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    ClientSafetyPolicy client_policy;
    ClientFeeLedger client_fee_ledger;
    const bool have_client_policy =
        batch.ReadPaymasterClientSafetyPolicy(client_policy);
    const bool have_client_ledger =
        batch.ReadPaymasterClientFeeLedger(client_fee_ledger);
    if ((!have_client_policy && batch.HasPaymasterClientSafetyPolicy()) ||
        (!have_client_ledger && batch.HasPaymasterClientFeeLedger()) ||
        have_client_policy != have_client_ledger) {
        error = "PAYMASTER_INVALID_CLIENT_SAFETY_STATE";
        return false;
    }

    for (PaymentSession& session : sessions) {
        if (!session.provider_side) {
            if ((session.state != SessionState::INPUTS_RESERVED &&
                 session.state != SessionState::AWAITING_WALLET_UNLOCK &&
                 session.state != SessionState::AWAITING_USER_SIGNATURE &&
                 session.state != SessionState::AUTHORIZED) ||
                !session.final_txid.IsNull() ||
                !session.recovery_txid.IsNull()) {
                continue;
            }
            for (const uint256& attempt_id : session.attempt_ids) {
                ProviderAttempt attempt;
                if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                    attempt.session_id != session.session_id) {
                    error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                    return false;
                }
                if (attempt.state != AttemptState::QUOTED ||
                    attempt.quote_expires_at <= 0 ||
                    attempt.quote_expires_at > now ||
                    attempt.commit_key.IsNull() ||
                    !attempt.user_signed_psbt.empty() ||
                    !attempt.final_transaction.empty() ||
                    !attempt.final_txid.IsNull()) {
                    continue;
                }

                bool client_fee_changed{false};
                if (have_client_ledger) {
                    const auto reservation = std::find_if(
                        client_fee_ledger.reservations.begin(),
                        client_fee_ledger.reservations.end(),
                        [&](const ClientFeeReservation& entry) {
                            return entry.commit_key == attempt.commit_key;
                        });
                    if (reservation != client_fee_ledger.reservations.end()) {
                        if (reservation->state ==
                            BudgetReservationState::SPENT) {
                            // A spent client fee is durable evidence that a
                            // final result existed even if another record was
                            // lost. Timeout must never unlock that authority.
                            continue;
                        }
                        if (reservation->state !=
                                BudgetReservationState::RESERVED ||
                            !ReleaseClientFee(client_fee_ledger,
                                              attempt.commit_key, now,
                                              error)) {
                            if (error.empty()) {
                                error =
                                    "PAYMASTER_CLIENT_FEE_RESERVATION_CONFLICT";
                            }
                            return false;
                        }
                        client_fee_changed = true;
                    } else if (!attempt.accepted_client_manifest_id.IsNull()) {
                        // A pre-authorized quote and fee reservation are one
                        // atomic record. Missing either half is corruption.
                        error =
                            "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                        return false;
                    }
                } else if (!attempt.accepted_client_manifest_id.IsNull()) {
                    PaymasterQuoteResponse response;
                    try {
                        SpanReader stream{::PROTOCOL_VERSION,
                                          attempt.signed_quote};
                        stream >> response;
                        if (!stream.empty()) {
                            throw std::ios_base::failure(
                                "trailing paymaster quote data");
                        }
                    } catch (const std::ios_base::failure&) {
                        error = "PAYMASTER_QUOTE_ENCODING";
                        return false;
                    }
                    if (response.quote.service_fee.value != 0) {
                        error =
                            "PAYMASTER_CLIENT_FEE_PREAUTHORIZATION_MISSING";
                        return false;
                    }
                }

                bool other_live_attempt{false};
                for (const uint256& other_id : session.attempt_ids) {
                    if (other_id == attempt_id) continue;
                    ProviderAttempt other;
                    if (!batch.ReadPaymasterAttempt(other_id, other) ||
                        other.session_id != session.session_id) {
                        error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                        return false;
                    }
                    if (other.state == AttemptState::CANDIDATE ||
                        (other.state == AttemptState::QUOTED &&
                         other.quote_expires_at > now)) {
                        other_live_attempt = true;
                        break;
                    }
                }

                attempt.state = AttemptState::QUOTE_EXPIRED;
                attempt.updated_at = std::max(attempt.updated_at, now);
                session.state = other_live_attempt ? SessionState::INPUTS_RESERVED : SessionState::FAILED;
                session.pending_phase = PendingPhase::NONE;
                session.updated_at = std::max(session.updated_at, now);
                if (!batch.TxnBegin()) {
                    return Abort(batch, error,
                                 "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.WritePaymasterAttempt(attempt) ||
                    !batch.WritePaymasterSession(session) ||
                    (client_fee_changed &&
                     !batch.WritePaymasterClientFeeLedger(
                         client_fee_ledger))) {
                    return Abort(batch, error,
                                 "PAYMASTER_DATABASE_WRITE");
                }
                if (!other_live_attempt) {
                    for (const COutPoint& outpoint : session.user_inputs) {
                        InputReservation reservation;
                        if (!batch.ReadPaymasterReservation(outpoint,
                                                            reservation) ||
                            reservation.session_id != session.session_id ||
                            reservation.role != ReservationRole::USER_DD ||
                            reservation.authorization_may_exist ||
                            !batch.ErasePaymasterReservation(outpoint) ||
                            !batch.EraseLockedUTXO(outpoint)) {
                            return Abort(
                                batch, error,
                                "PAYMASTER_RESERVATION_NOT_EXPIRABLE");
                        }
                    }
                }
                if (!batch.TxnCommit()) {
                    error = "PAYMASTER_DATABASE_COMMIT";
                    return false;
                }
                ++expired_quotes;
                break;
            }
            continue;
        }
        // INPUTS_RESERVED/NONE is the only provider-side state in which no
        // authorization can have been accepted. In particular, AUTHORIZED is
        // treated as unsafe even if a crash preceded the attempt-state write.
        if (!session.provider_side || session.state != SessionState::INPUTS_RESERVED ||
            session.pending_phase != PendingPhase::NONE || !session.final_txid.IsNull()) {
            continue;
        }
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                attempt.session_id != session.session_id) {
                error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                return false;
            }
            if (attempt.state != AttemptState::QUOTED || attempt.quote_expires_at <= 0 ||
                attempt.quote_expires_at > now || attempt.commit_key.IsNull() ||
                !attempt.user_signed_psbt.empty() || !attempt.final_transaction.empty() ||
                !attempt.final_txid.IsNull()) {
                continue;
            }

            // These records are durable evidence that timing out the quote is
            // no longer safe. A result is conservatively treated the same way,
            // even when it has not yet advanced the attempt after a crash.
            UserAuthorizationRecord authorization;
            ProviderCommitRecord commit;
            PaymasterResult result;
            const DatabaseReadStatus authorization_status{
                batch.ReadPaymasterUserAuthorizationWithStatus(
                    attempt.commit_key, authorization)};
            if (authorization_status == DatabaseReadStatus::FOUND) {
                continue;
            }
            if (authorization_status != DatabaseReadStatus::NOT_FOUND) {
                error = PersistedReadError(
                    authorization_status, "UserAuthorizationRecord",
                    authorization, "PAYMASTER_USER_AUTHORIZATION_MISSING",
                    "PAYMASTER_INVALID_USER_AUTHORIZATION");
                return false;
            }
            const DatabaseReadStatus commit_status{
                batch.ReadPaymasterProviderCommitWithStatus(
                    attempt.commit_key, commit)};
            if (commit_status == DatabaseReadStatus::FOUND) continue;
            if (commit_status != DatabaseReadStatus::NOT_FOUND) {
                error = PersistedReadError(
                    commit_status, "ProviderCommitRecord", commit,
                    "PAYMASTER_PROVIDER_COMMIT_MISSING",
                    "PAYMASTER_INVALID_PROVIDER_COMMIT");
                return false;
            }
            const DatabaseReadStatus result_status{
                batch.ReadPaymasterResultWithStatus(attempt.commit_key,
                                                    result)};
            if (result_status == DatabaseReadStatus::FOUND) continue;
            if (result_status != DatabaseReadStatus::NOT_FOUND) {
                error = PersistedReadError(
                    result_status, "PaymasterResult", result,
                    "PAYMASTER_RESULT_MISSING",
                    "PAYMASTER_INVALID_PERSISTED_RESULT");
                return false;
            }

            std::vector<ProviderPoolEntry> pool;
            if (!batch.ReadPaymasterProviderPool(pool)) {
                error = "PAYMASTER_POOL_RESERVATION_MISSING";
                return false;
            }
            size_t released_entries{0};
            for (ProviderPoolEntry& entry : pool) {
                if (entry.reservation_id != attempt.commit_key) continue;
                if (entry.state != PoolEntryState::RESERVED) {
                    error = "PAYMASTER_POOL_RESERVATION_NOT_EXPIRABLE";
                    return false;
                }
                entry.state = PoolEntryState::AVAILABLE;
                entry.reservation_id.SetNull();
                entry.updated_at = std::max(entry.updated_at, now);
                ++released_entries;
            }
            if (released_entries == 0) {
                error = "PAYMASTER_POOL_RESERVATION_MISSING";
                return false;
            }

            bool budget_changed{false};
            if (have_safety_policy && have_budget_ledger) {
                const auto reservation = std::find_if(
                    budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
                    [&](const ProviderBudgetReservation& entry) {
                        return entry.commit_key == attempt.commit_key;
                    });
                if (reservation == budget_ledger.reservations.end()) {
                    error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
                    return false;
                } else if (reservation->state == BudgetReservationState::SPENT) {
                    // A spent reservation is evidence of a provider commit,
                    // even if another record was lost during local recovery.
                    continue;
                } else {
                    const BudgetReservationState previous_state = reservation->state;
                    if (!ReleaseProviderBudget(budget_ledger, attempt.commit_key, now, error)) {
                        return false;
                    }
                    budget_changed = previous_state != BudgetReservationState::RELEASED;
                }
            }

            attempt.state = AttemptState::QUOTE_EXPIRED;
            attempt.updated_at = std::max(attempt.updated_at, now);
            session.state = SessionState::FAILED;
            session.pending_phase = PendingPhase::NONE;
            session.updated_at = std::max(session.updated_at, now);

            if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
            if (!batch.WritePaymasterAttempt(attempt) ||
                !batch.WritePaymasterSession(session) ||
                !batch.WritePaymasterProviderPool(pool) ||
                (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger))) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (!batch.TxnCommit()) {
                error = "PAYMASTER_DATABASE_COMMIT";
                return false;
            }
            ++expired_quotes;
            break;
        }
    }
    return true;
}

bool PaymasterStore::ExpireProviderCapacityReservations(
    int64_t now,
    size_t& expired_reservations,
    std::string& error)
{
    error.clear();
    expired_reservations = 0;
    if (now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_EXPIRY_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<std::pair<uint256, std::vector<unsigned char>>> responses;
    if (!batch.ListPaymasterCapacityResponses(responses)) {
        error = "PAYMASTER_CAPACITY_DATABASE_READ";
        return false;
    }
    if (responses.empty()) return true;

    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_CAPACITY_IDENTITY_MISSING";
        return false;
    }
    ProviderBudgetLedger budget_ledger;
    const bool have_budget_ledger =
        batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger()) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_LEDGER";
        return false;
    }

    // A provider-side attempt means the payment flow has bound the capacity
    // nonce. Conservatively retain such slots at every attempt state: the
    // pool rebind and attempt transition are atomic in normal operation, but
    // this also fails closed for imported or partially recovered databases.
    std::set<uint256> bound_nonces;
    std::vector<PaymentSession> sessions;
    if (!batch.ListPaymasterSessions(sessions)) {
        error = "PAYMASTER_SESSION_DATABASE_READ";
        return false;
    }
    for (const PaymentSession& session : sessions) {
        if (!session.provider_side) continue;
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
                attempt.session_id != session.session_id ||
                attempt.provider_id != identity.provider_id) {
                error = "PAYMASTER_ATTEMPT_SESSION_CONFLICT";
                return false;
            }
            if (!attempt.client_nonce.IsNull()) bound_nonces.insert(attempt.client_nonce);
        }
    }

    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterProviderPool(pool)) {
        error = "PAYMASTER_POOLS_NOT_PREPARED";
        return false;
    }
    std::map<COutPoint, size_t> pool_by_outpoint;
    for (size_t index = 0; index < pool.size(); ++index) {
        pool_by_outpoint.emplace(pool[index].outpoint, index);
    }

    for (const auto& [request_hash, response] : responses) {
        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(response, proof) ||
            proof.created_at <= 0 || proof.expires_at <= proof.created_at) {
            error = "PAYMASTER_CAPACITY_RESPONSE_ENCODING";
            return false;
        }

        PaymasterCapacityRequest request;
        request.version = proof.version;
        request.genesis_hash = proof.genesis_hash;
        request.provider_id = proof.provider_id;
        request.request_id = proof.request_id;
        request.session_id = proof.session_id;
        request.client_nonce = proof.client_nonce;
        request.funding_model = proof.funding_model;
        request.requires_carrier = proof.requires_carrier;
        request.requested_slots = static_cast<uint16_t>(proof.liquidity_slots.size());
        request.created_at = proof.created_at;
        request.expires_at = proof.expires_at;
        std::string validation_error;
        if (GetPersistedCapacityRequestHash(proof) != request_hash ||
            !ValidateCapacityProofEnvelope(proof, request, proof.expires_at - 1,
                                           validation_error) ||
            proof.provider_id != identity.provider_id ||
            !identity.identity_key.VerifySchnorr(GetCapacityProofSignatureHash(proof),
                                                 proof.identity_signature)) {
            error = validation_error.empty() ? "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH" : std::move(validation_error);
            return false;
        }

        uint256 indexed_request_hash;
        if (!batch.ReadPaymasterCapacityNonce(proof.client_nonce,
                                              indexed_request_hash) ||
            indexed_request_hash != request_hash ||
            !batch.ReadPaymasterCapacitySession(GetPersistedCapacitySessionKey(proof),
                                                indexed_request_hash) ||
            indexed_request_hash != request_hash) {
            error = "PAYMASTER_CAPACITY_RESPONSE_INDEX_MISMATCH";
            return false;
        }

        ProviderCapacityReleaseRecord existing_release;
        const bool have_release = batch.ReadPaymasterCapacityRelease(
            request_hash, existing_release);
        if (!have_release && batch.HasPaymasterCapacityRelease(request_hash)) {
            error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
            return false;
        }
        const bool replay_retention_elapsed = TimeDeltaExceeds(
            now, proof.expires_at, CAPACITY_REPLAY_RETENTION_SECONDS);
        const uint256 admission_key = GetProviderRequestSlotKey(
            proof.provider_id, proof.request_id, proof.session_id);
        const auto find_admission = [&](ProviderBudgetLedger& ledger) {
            return std::find_if(
                ledger.capacity_admissions.begin(),
                ledger.capacity_admissions.end(),
                [&](const ProviderCapacityAdmission& entry) {
                    return entry.request_key == admission_key;
                });
        };
        if (have_release) {
            if (existing_release.client_nonce != proof.client_nonce) {
                error = "PAYMASTER_CAPACITY_RELEASE_CONFLICT";
                return false;
            }
            if (std::any_of(pool.begin(), pool.end(), [&](const auto& entry) {
                    return entry.reservation_id == proof.client_nonce;
                })) {
                error = "PAYMASTER_CAPACITY_RELEASE_POOL_CONFLICT";
                return false;
            }
            if (!have_budget_ledger) {
                error = "PAYMASTER_CAPACITY_RELEASE_BUDGET_CONFLICT";
                return false;
            }
            const auto admission = find_admission(budget_ledger);
            if (admission != budget_ledger.capacity_admissions.end() &&
                (admission->request_hash != request_hash ||
                 admission->state != CapacityAdmissionState::RELEASED)) {
                error = "PAYMASTER_CAPACITY_RELEASE_BUDGET_CONFLICT";
                return false;
            }
            if (admission == budget_ledger.capacity_admissions.end() &&
                !replay_retention_elapsed) {
                error = "PAYMASTER_CAPACITY_RELEASE_BUDGET_CONFLICT";
                return false;
            }
            if (replay_retention_elapsed) {
                ProviderBudgetLedger compacted_ledger{budget_ledger};
                bool compact_budget{false};
                const auto compacted_admission = find_admission(compacted_ledger);
                if (compacted_admission !=
                    compacted_ledger.capacity_admissions.end()) {
                    compacted_ledger.capacity_admissions.erase(
                        compacted_admission);
                    compact_budget = true;
                }
                if (!batch.TxnBegin()) {
                    return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
                }
                if (!batch.ErasePaymasterCapacityResponse(request_hash) ||
                    !batch.ErasePaymasterCapacityNonce(proof.client_nonce) ||
                    !batch.ErasePaymasterCapacitySession(
                        GetPersistedCapacitySessionKey(proof)) ||
                    !batch.ErasePaymasterCapacityRelease(request_hash) ||
                    (compact_budget &&
                     !batch.WritePaymasterProviderBudgetLedger(
                         compacted_ledger))) {
                    return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
                }
                if (!batch.TxnCommit()) {
                    error = "PAYMASTER_DATABASE_COMMIT";
                    return false;
                }
                if (compact_budget) {
                    budget_ledger = std::move(compacted_ledger);
                }
            }
            continue;
        }
        if (bound_nonces.count(proof.client_nonce) != 0) {
            if (proof.expires_at > now || !replay_retention_elapsed) {
                continue;
            }
            ProviderBudgetLedger compacted_ledger{budget_ledger};
            bool compact_budget{false};
            if (!have_budget_ledger) {
                error = "PAYMASTER_CAPACITY_PROMOTION_CONFLICT";
                return false;
            }
            const auto admission = find_admission(compacted_ledger);
            if (admission != compacted_ledger.capacity_admissions.end()) {
                if (admission->request_hash != request_hash ||
                    admission->state != CapacityAdmissionState::PROMOTED) {
                    error = "PAYMASTER_CAPACITY_PROMOTION_CONFLICT";
                    return false;
                }
                compacted_ledger.capacity_admissions.erase(admission);
                compact_budget = true;
            }
            if (!batch.TxnBegin()) {
                return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
            }
            if (!batch.ErasePaymasterCapacityResponse(request_hash) ||
                !batch.ErasePaymasterCapacityNonce(proof.client_nonce) ||
                !batch.ErasePaymasterCapacitySession(
                    GetPersistedCapacitySessionKey(proof)) ||
                (compact_budget &&
                 !batch.WritePaymasterProviderBudgetLedger(
                     compacted_ledger))) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
            if (!batch.TxnCommit()) {
                error = "PAYMASTER_DATABASE_COMMIT";
                return false;
            }
            if (compact_budget) {
                budget_ledger = std::move(compacted_ledger);
            }
            continue;
        }
        if (proof.expires_at > now) continue;

        std::set<COutPoint> proof_outpoints;
        std::vector<size_t> releasable_indexes;
        bool exact_capacity_reservation{true};
        const PaymasterLiquiditySlot& slot = proof.liquidity_slots.front();
        for (const CapacityDGBInput& input : slot.dgb_inputs) {
            proof_outpoints.insert(input.input.outpoint);
            const auto found = pool_by_outpoint.find(input.input.outpoint);
            if (found == pool_by_outpoint.end()) {
                exact_capacity_reservation = false;
                continue;
            }
            const ProviderPoolEntry& entry = pool[found->second];
            if (entry.state != PoolEntryState::RESERVED ||
                entry.reservation_id != proof.client_nonce ||
                !CapacityProofMatchesPoolEntry(input, entry)) {
                exact_capacity_reservation = false;
                continue;
            }
            releasable_indexes.push_back(found->second);
        }
        if (slot.carrier) {
            proof_outpoints.insert(slot.carrier->carrier.outpoint);
            const auto found = pool_by_outpoint.find(slot.carrier->carrier.outpoint);
            if (found == pool_by_outpoint.end()) {
                exact_capacity_reservation = false;
            } else {
                const ProviderPoolEntry& entry = pool[found->second];
                if (entry.state != PoolEntryState::RESERVED ||
                    entry.reservation_id != proof.client_nonce ||
                    !CapacityProofMatchesPoolEntry(*slot.carrier, entry)) {
                    exact_capacity_reservation = false;
                } else {
                    releasable_indexes.push_back(found->second);
                }
            }
        }
        for (const ProviderPoolEntry& entry : pool) {
            if (entry.reservation_id == proof.client_nonce &&
                proof_outpoints.count(entry.outpoint) == 0) {
                error = "PAYMASTER_CAPACITY_RESERVATION_CONFLICT";
                return false;
            }
        }
        if (!exact_capacity_reservation ||
            releasable_indexes.size() != proof_outpoints.size()) {
            // A quote commit rebinds these exact entries to its commit key.
            // Missing, committed, spent, or otherwise changed entries are
            // likewise never made available by an expiry heuristic.
            continue;
        }

        std::vector<ProviderPoolEntry> updated_pool{pool};
        for (const size_t index : releasable_indexes) {
            updated_pool[index].state = PoolEntryState::AVAILABLE;
            updated_pool[index].reservation_id.SetNull();
            updated_pool[index].updated_at = std::max(updated_pool[index].updated_at, now);
        }
        ProviderCapacityReleaseRecord release;
        release.request_hash = request_hash;
        release.client_nonce = proof.client_nonce;
        release.released_at = now;

        if (!have_budget_ledger ||
            !ReleaseProviderCapacityAdmission(
                budget_ledger,
                GetProviderRequestSlotKey(proof.provider_id,
                                          proof.request_id,
                                          proof.session_id),
                request_hash, now, error)) {
            if (error.empty()) {
                error = "PAYMASTER_CAPACITY_ADMISSION_MISSING";
            }
            return false;
        }

        if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
        if (!batch.WritePaymasterProviderPool(updated_pool) ||
            !batch.WritePaymasterCapacityRelease(release, false) ||
            !batch.WritePaymasterProviderBudgetLedger(budget_ledger)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
        pool = std::move(updated_pool);
        ++expired_reservations;
    }
    return true;
}

bool PaymasterStore::CompactClientCapacitySnapshots(
    int64_t now,
    size_t& compacted_snapshots,
    std::string& error)
{
    error.clear();
    compacted_snapshots = 0;
    if (now <= 0) {
        error = "PAYMASTER_INVALID_CAPACITY_COMPACTION_TIME";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::vector<ValidatedCapacitySnapshot> snapshots;
    if (!batch.ListPaymasterCapacitySnapshots(snapshots)) {
        error = "PAYMASTER_CAPACITY_SNAPSHOT_DATABASE_READ";
        return false;
    }
    for (const ValidatedCapacitySnapshot& snapshot : snapshots) {
        if (snapshot.expires_at <= 0 ||
            !TimeDeltaExceeds(now, snapshot.expires_at,
                              CAPACITY_REPLAY_RETENTION_SECONDS)) {
            continue;
        }

        PaymasterCapacityProof proof;
        if (!DecodeCanonicalCapacityProof(snapshot.capacity_proof, proof) ||
            proof.snapshot_id != snapshot.snapshot_id ||
            proof.provider_id != snapshot.provider_id ||
            proof.session_id != snapshot.session_id ||
            proof.client_nonce != snapshot.client_nonce ||
            proof.funding_model != snapshot.funding_model ||
            proof.requires_carrier != snapshot.requires_carrier ||
            proof.created_at != snapshot.created_at ||
            proof.expires_at != snapshot.expires_at ||
            GetCapacityResourceCommitment(proof) !=
                snapshot.resource_commitment) {
            error = "PAYMASTER_CAPACITY_SNAPSHOT_BINDING_MISMATCH";
            return false;
        }

        bool erase_slot{false};
        uint256 indexed_snapshot_id;
        if (batch.ReadPaymasterCapacitySlot(
                snapshot.resource_commitment, indexed_snapshot_id) &&
            indexed_snapshot_id == snapshot.snapshot_id) {
            erase_slot = true;
        }

        std::set<COutPoint> proof_outpoints;
        for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
            if (slot.carrier) {
                proof_outpoints.insert(slot.carrier->carrier.outpoint);
            }
            for (const CapacityDGBInput& input : slot.dgb_inputs) {
                proof_outpoints.insert(input.input.outpoint);
            }
        }
        if (proof_outpoints.empty()) {
            error = "PAYMASTER_INVALID_CAPACITY_RESOURCE_BINDING";
            return false;
        }
        std::vector<COutPoint> erase_resources;
        for (const COutPoint& outpoint : proof_outpoints) {
            CapacityResourceBinding binding;
            if (batch.ReadPaymasterCapacityResource(
                    snapshot.provider_id, outpoint, binding) &&
                binding.snapshot_id == snapshot.snapshot_id) {
                if (binding.resource_commitment !=
                        snapshot.resource_commitment ||
                    binding.session_id != snapshot.session_id ||
                    binding.attempt_id != snapshot.attempt_id ||
                    binding.proof_hash != Hash(snapshot.capacity_proof)) {
                    error = "PAYMASTER_CAPACITY_RESOURCE_BINDING_CONFLICT";
                    return false;
                }
                erase_resources.push_back(outpoint);
            }
        }

        if (!batch.TxnBegin()) {
            return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
        }
        if ((erase_slot &&
             !batch.ErasePaymasterCapacitySlot(
                 snapshot.resource_commitment)) ||
            !batch.ErasePaymasterCapacitySnapshot(snapshot.snapshot_id)) {
            return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
        }
        for (const COutPoint& outpoint : erase_resources) {
            if (!batch.ErasePaymasterCapacityResource(
                    snapshot.provider_id, outpoint)) {
                return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
            }
        }
        if (!batch.TxnCommit()) {
            error = "PAYMASTER_DATABASE_COMMIT";
            return false;
        }
        ++compacted_snapshots;
    }
    return true;
}

bool PaymasterStore::RecordQuoteEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& conflicting_signed_quote,
    int64_t now,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || conflicting_signed_quote.empty() || now <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        std::find(session.attempt_ids.begin(), session.attempt_ids.end(), attempt_id) ==
            session.attempt_ids.end()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymasterQuoteRequest authoritative_request;
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    PaymasterQuoteResponse persisted_response;
    std::string validation_error;
    if (!DecodeCanonicalQuoteResponse(attempt.signed_quote,
                                      persisted_response) ||
        persisted_response.request_id != session.request_id ||
        persisted_response.session_id != session.session_id ||
        persisted_response.quote.provider_id != attempt.provider_id ||
        persisted_response.quote.intent_hash != attempt.intent_hash ||
        !ValidateQuoteResponseEnvelope(
            persisted_response,
            authoritative_request.intent.genesis_hash,
            persisted_response.quote.created_at, validation_error) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(persisted_response.quote),
            persisted_response.quote.identity_signature)) {
        error = "PAYMASTER_PERSISTED_SIGNED_QUOTE_CORRUPT";
        return false;
    }
    if (!RecordQuoteEquivocationLocked(batch, session, attempt,
                                       conflicting_signed_quote, now, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::RecordPendingQuoteEquivocation(
    const uint256& attempt_id,
    const std::vector<unsigned char>& first_signed_quote,
    const std::vector<unsigned char>& conflicting_signed_quote,
    int64_t observed_at,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull() || first_signed_quote.empty() ||
        conflicting_signed_quote.empty() || observed_at <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    std::string request_id;
    PaymentSession session;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || session.request_id != request_id ||
        attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !attempt.signed_quote.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    PaymasterQuoteRequest authoritative_request;
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (!RecordQuoteEquivocationPairLocked(
            batch, session, attempt, first_signed_quote,
            conflicting_signed_quote, observed_at, error)) {
        if (error.empty()) error = "PAYMASTER_INVALID_QUOTE_EQUIVOCATION";
        return false;
    }
    error.clear();
    return true;
}

bool PaymasterStore::StageQuoteResponseClaimCandidate(
    const uint256& attempt_id,
    const std::vector<unsigned char>& signed_quote,
    int64_t observed_at,
    bool& equivocation,
    std::string& error)
{
    error.clear();
    equivocation = false;
    if (attempt_id.IsNull() || signed_quote.empty() ||
        signed_quote.size() > MAX_EQUIVOCATION_ARTIFACT_BYTES ||
        observed_at <= 0) {
        error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderAttempt attempt;
    PaymentSession session;
    std::string request_id;
    PaymasterQuoteResponse response;
    PaymasterQuoteRequest authoritative_request;
    std::string validation_error;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        attempt.attempt_id != attempt_id ||
        !batch.ReadPaymasterSessionId(attempt.session_id, request_id) ||
        !batch.ReadPaymasterSession(request_id, session) ||
        session.provider_side || attempt.session_id != session.session_id ||
        std::count(session.attempt_ids.begin(), session.attempt_ids.end(),
                   attempt_id) != 1 ||
        !attempt.signed_quote.empty()) {
        error = "PAYMASTER_ATTEMPT_NOT_FOUND";
        return false;
    }
    if (!ValidatePersistedQuoteRequestAuthority(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (!attempt.quote_response_claim_candidate.empty() &&
        !ValidatePersistedQuoteClaimCandidate(
            session, attempt, authoritative_request, error)) {
        return false;
    }
    if (!DecodeCanonicalQuoteResponse(signed_quote, response) ||
        response.request_id != session.request_id ||
        response.session_id != session.session_id ||
        response.quote.provider_id != attempt.provider_id ||
        response.quote.intent_hash != attempt.intent_hash ||
        response.quote.offer_id != authoritative_request.intent.offer_id ||
        response.quote.policy_hash !=
            authoritative_request.intent.policy_hash ||
        response.quote.funding_model !=
            authoritative_request.intent.funding_model ||
        response.quote.sponsorship_scope !=
            authoritative_request.intent.sponsorship_scope ||
        !attempt.provider_identity_key.IsFullyValid() ||
        GetPaymasterId(attempt.provider_identity_key) != attempt.provider_id ||
        !ValidateQuoteResponseEnvelope(
            response, authoritative_request.intent.genesis_hash,
            response.quote.created_at,
            validation_error) ||
        !attempt.provider_identity_key.VerifySchnorr(
            GetPaymasterQuoteSignatureHash(response.quote),
            response.quote.identity_signature)) {
        error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
        return false;
    }

    if (!attempt.quote_response_claim_candidate.empty()) {
        if (attempt.quote_response_claim_candidate == signed_quote) {
            return true;
        }
        if (!RecordQuoteEquivocationPairLocked(
                batch, session, attempt,
                attempt.quote_response_claim_candidate, signed_quote,
                observed_at, error)) {
            if (error.empty()) {
                error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
            }
            return false;
        }
        equivocation = true;
        error.clear();
        return true;
    }

    if (!ValidateQuoteResponseEnvelope(
            response, authoritative_request.intent.genesis_hash, observed_at,
            validation_error)) {
        error = "PAYMASTER_INVALID_QUOTE_CLAIM_CANDIDATE";
        return false;
    }

    attempt.version = ProviderAttempt::CURRENT_VERSION;
    attempt.quote_response_claim_candidate = signed_quote;
    if (!batch.TxnBegin()) {
        error = "PAYMASTER_DATABASE_BEGIN";
        return false;
    }
    if (!batch.WritePaymasterAttempt(attempt)) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

bool PaymasterStore::RejectUnavailableProviderSubmit(
    const uint256& attempt_id,
    const PaymasterResult& result,
    const uint256& expected_genesis,
    ProviderAttempt& attempt,
    std::string& error)
{
    error.clear();
    if (attempt_id.IsNull()) {
        error = "PAYMASTER_INVALID_SUBMIT_REJECTION";
        return false;
    }

    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterAttempt(attempt_id, attempt) ||
        !batch.ReadPaymasterIdentity(identity) ||
        attempt.provider_id != identity.provider_id ||
        attempt.provider_identity_key != identity.identity_key ||
        attempt.commit_key.IsNull()) {
        error = "PAYMASTER_SUBMIT_REJECTION_BINDING_MISSING";
        return false;
    }
    if (!ValidatePaymasterResult(result, expected_genesis, attempt.provider_id,
                                 attempt.commit_key, identity.identity_key, 1, error)) {
        return false;
    }
    if (result.status != PaymasterResultStatus::REJECTED || result.txid ||
        result.raw_transaction_hash || result.final_transaction) {
        error = "PAYMASTER_INVALID_SUBMIT_REJECTION";
        return false;
    }

    PaymasterResult existing;
    if (batch.ReadPaymasterResult(result.commit_key, existing)) {
        if (attempt.state == AttemptState::REJECTED &&
            result.result_sequence == existing.result_sequence &&
            GetPaymasterResultSignatureHash(result) == GetPaymasterResultSignatureHash(existing) &&
            result.identity_signature == existing.identity_signature) {
            return true;
        }
        error = "PAYMASTER_RESULT_SEQUENCE_REGRESSION";
        return false;
    }
    ProviderCommitRecord commit;
    if (batch.ReadPaymasterProviderCommit(attempt.commit_key, commit) ||
        (attempt.state != AttemptState::QUOTED &&
         attempt.state != AttemptState::USER_SIGNED &&
         attempt.state != AttemptState::USER_PSBT_ACCEPTED)) {
        error = "PAYMASTER_PROVIDER_SIGNATURE_MAY_EXIST";
        return false;
    }

    std::vector<ProviderPoolEntry> pool;
    if (!batch.ReadPaymasterProviderPool(pool)) {
        error = "PAYMASTER_POOL_RESERVATION_MISSING";
        return false;
    }
    size_t released_entries{0};
    for (ProviderPoolEntry& entry : pool) {
        if (entry.reservation_id != attempt.commit_key) continue;
        if (entry.state != PoolEntryState::RESERVED) {
            error = "PAYMASTER_POOL_RESERVATION_NOT_REJECTABLE";
            return false;
        }
        entry.state = PoolEntryState::AVAILABLE;
        entry.reservation_id.SetNull();
        entry.updated_at = result.updated_at;
        ++released_entries;
    }
    if (released_entries == 0) {
        error = "PAYMASTER_POOL_RESERVATION_MISSING";
        return false;
    }

    ProviderSafetyPolicy safety_policy;
    ProviderBudgetLedger budget_ledger;
    const bool have_safety_policy = batch.ReadPaymasterProviderSafetyPolicy(safety_policy);
    const bool have_budget_ledger = batch.ReadPaymasterProviderBudgetLedger(budget_ledger);
    if ((!have_safety_policy && batch.HasPaymasterProviderSafetyPolicy()) ||
        (!have_budget_ledger && batch.HasPaymasterProviderBudgetLedger())) {
        error = "PAYMASTER_INVALID_PROVIDER_BUDGET_STATE";
        return false;
    }
    bool budget_changed{false};
    if (have_safety_policy && have_budget_ledger) {
        const auto reservation = std::find_if(
            budget_ledger.reservations.begin(), budget_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == attempt.commit_key;
            });
        if (reservation != budget_ledger.reservations.end()) {
            const BudgetReservationState previous_state = reservation->state;
            if (!ReleaseProviderBudget(budget_ledger, attempt.commit_key,
                                       result.updated_at, error)) {
                return false;
            }
            budget_changed = previous_state != BudgetReservationState::RELEASED;
        } else {
            error = "PAYMASTER_BUDGET_RESERVATION_MISSING";
            return false;
        }
    }

    if (!batch.TxnBegin()) return Abort(batch, error, "PAYMASTER_DATABASE_BEGIN");
    attempt.state = AttemptState::REJECTED;
    attempt.updated_at = std::max(attempt.updated_at, result.updated_at);
    if (!batch.WritePaymasterAttempt(attempt) ||
        !batch.WritePaymasterProviderPool(pool) ||
        !batch.WritePaymasterResult(result) ||
        (budget_changed && !batch.WritePaymasterProviderBudgetLedger(budget_ledger))) {
        return Abort(batch, error, "PAYMASTER_DATABASE_WRITE");
    }
    if (!batch.TxnCommit()) {
        error = "PAYMASTER_DATABASE_COMMIT";
        return false;
    }
    return true;
}

} // namespace wallet
