// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Wallet-scoped Paymaster RPC orchestration and automatic provider service.
 *
 * RPC parameters are untrusted input. This layer coordinates pure protocol
 * validation with PaymasterStore transactions and wallet signing, but does not
 * bypass either authorization manifest or persisted safety accounting. Every
 * retry reuses the exact durable request binding or fails closed.
 */

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
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
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

RPCResult PaymasterTransportResult()
{
    return {RPCResult::Type::OBJ, "transport", /*optional=*/true, "Local transport only; no financial or external reachability guarantee", {
        {RPCResult::Type::STR, "state", "available, disabled, waiting_capacity, connecting, handshaking, direct_ready, queue_full, expired, failed, canceled, network_inactive, privacy_rejected, or connect_failed"},
        {RPCResult::Type::NUM, "outbound_limit", "Effective reserved client channels"},
        {RPCResult::Type::NUM, "inbound_limit", "Effective active provider channels"},
        {RPCResult::Type::NUM, "handshake_limit", "Effective provider handshake reserve"},
        {RPCResult::Type::NUM, "outbound_in_use", "Client leases, including in-flight connections"},
        {RPCResult::Type::NUM, "inbound_in_use", "Active provider channels"},
        {RPCResult::Type::NUM, "handshakes_in_use", "Unclassified Direct sockets"},
        {RPCResult::Type::NUM, "queued", "Local transport requests waiting for capacity"},
        {RPCResult::Type::BOOL, "listener_ready", "All configured Direct binds succeeded"},
        {RPCResult::Type::STR, "external_reachability", "unknown; no external reachability test is performed"},
    }};
}

RPCResult PaymasterPaymentViewResult()
{
    const auto transaction = [](const char* key, bool recipient) {
        std::vector<RPCResult> fields{
            {RPCResult::Type::BOOL, "details_available", "Validated durable transaction details are available"},
            {RPCResult::Type::BOOL, "observation_available", "Exact transaction is present in the local wallet"},
            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known transaction id; alone is not proof of payment"},
            {RPCResult::Type::NUM, "confirmations", /*optional=*/true, "Local active-chain depth; negative means conflicted"},
            {RPCResult::Type::BOOL, "in_mempool", /*optional=*/true, "Locally observed mempool membership"},
            {RPCResult::Type::STR_HEX, "block_hash", /*optional=*/true, "Confirming block on the observed chain"},
            {RPCResult::Type::NUM, "block_height", /*optional=*/true, "Confirming block height"},
        };
        if (recipient) {
            fields.emplace_back(RPCResult::Type::OBJ, "recipient", /*optional=*/true,
                                "Output bound by the validated client authorization", std::vector<RPCResult>{
                {RPCResult::Type::NUM, "vout", "Recipient output index"},
                {RPCResult::Type::STR_HEX, "script_pub_key", "Exact recipient script"},
                {RPCResult::Type::NUM, "amount_cents", "Recipient DD amount, excluding service fee"},
            });
        }
        return RPCResult{RPCResult::Type::OBJ, key, "Read-only transaction observation", std::move(fields)};
    };
    return {RPCResult::Type::OBJ, "payment_view", /*optional=*/true,
            "Local snapshot, not a transferable receipt; details may be unavailable after pruning", {
        {RPCResult::Type::STR_HEX, "observed_tip_hash", "Wallet's last processed block hash"},
        {RPCResult::Type::NUM, "observed_tip_height", "Wallet's last processed block height"},
        {RPCResult::Type::STR, "error", /*optional=*/true, "Artifact validation/read error; no positive payment claim is made"},
        transaction("payment", true),
        transaction("recovery", false),
    }};
}

namespace paymaster_rpc::internal {

using paymaster_store::internal::PersistedVersionError;
using paymaster_store::internal::PersistedReadError;
using paymaster_store::internal::ProviderPoolReadError;

// -------------------------------------------------------------------------
// Automatic provider runtime and readiness
// -------------------------------------------------------------------------
// Internal method names are never exposed as user authorization shortcuts.
// They let the scheduler reuse the same RPC orchestration while the store,
// manifests, wallet locks, and budget checks remain authoritative.


// Serialize automatic cycles, manual processing RPCs, and maintenance for one
// provider wallet. Without this guard two callers could lease the same logical
// work or independently decide that the same finite budget is available.

// -------------------------------------------------------------------------
// Liquidity reconciliation and bounded maintenance
// -------------------------------------------------------------------------
// Reconciliation is free and idempotent: it recognizes confirmed wallet-owned
// successors. Replenishment is a distinct spending operation and is permitted
// only by a persisted liquidity policy with finite transaction/hour/day caps.

/** The automatic provider service runs on the validation scheduler. It must
 * never call SyncWithValidationInterfaceQueue (directly or through a wallet
 * or index BlockUntilSyncedToCurrentChain call), because that queue is driven
 * by the same scheduler thread. Check the already-published wallet and index
 * tips instead and retry on the next bounded service cycle. */

// JSON conversion helpers below are presentation only. They must not be used to
// make authorization decisions; callers operate on the typed records above.



// Announcements describe currently admissible service but grant no spending
// authority. Every later capacity proof, quote, and final transaction is still
// independently authenticated and validated.

UniValue LiquiditySlotCountsToJSON(const LiquiditySlotCounts& counts,
                                   size_t target)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("target", target);
    result.pushKV("ready", counts.ready);
    result.pushKV("pending", counts.pending);
    result.pushKV("counted_toward_target", counts.target_counted);
    result.pushKV("missing", counts.missing);
    return result;
}


UniValue ProviderLiquidityStatusToJSON(const ProviderReadiness& readiness,
                                       int64_t now, CWallet* wallet)
{
    using namespace DigiDollar::Paymaster;
    const ProviderLiquidityPolicy policy = readiness.have_liquidity_policy
        ? readiness.liquidity_policy
        : SuggestedLiquidityPolicy(readiness, now);
    const bool targets_satisfy_provider_policy =
        !readiness.have_policy ||
        ProviderLiquidityTargetsSatisfyPolicy(policy, readiness.policy);
    const auto admission_dgb = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB,
        policy.target_admission_dgb);
    const int64_t operational_dgb_minimum = readiness.have_policy
        ? readiness.policy.maximum_network_fee.value
        : std::numeric_limits<int64_t>::max();
    const auto operational_dgb = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
        policy.target_operational_dgb, operational_dgb_minimum);
    const auto admission_carriers = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::ADMISSION,
        PoolAsset::DD_CARRIER, policy.target_admission_carriers);
    const auto operational_carriers = CountLiquiditySlots(
        readiness.pool_entries, PoolPurpose::OPERATIONAL,
        PoolAsset::DD_CARRIER, policy.target_operational_carriers, 0,
        readiness.have_policy ? &readiness.policy : nullptr);
    const size_t missing = admission_dgb.missing + operational_dgb.missing +
                           admission_carriers.missing +
                           operational_carriers.missing;
    const size_t pending = admission_dgb.pending + operational_dgb.pending +
                           admission_carriers.pending +
                           operational_carriers.pending;
    std::string state{"ready"};
    if (!targets_satisfy_provider_policy) {
        state = "waiting_for_target_configuration";
    } else if (!readiness.have_liquidity_policy ||
        (missing > 0 && !policy.paid_maintenance_approved)) {
        state = "waiting_for_maintenance_approval";
    } else if (pending > 0) {
        state = "waiting_for_liquidity_confirmation";
    } else if (missing > 0) {
        state = "replenishing_liquidity";
    }

    const auto checked_add = [](int64_t& total, int64_t value) {
        if (value < 0 ||
            total > std::numeric_limits<int64_t>::max() - value) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "PAYMASTER_LIQUIDITY_STATUS_OVERFLOW");
        }
        total += value;
    };
    int64_t carrier_base{0};
    int64_t carrier_excess{0};
    for (const ProviderPoolEntry& entry : readiness.pool_entries) {
        if (entry.asset != PoolAsset::DD_CARRIER ||
            entry.state != PoolEntryState::AVAILABLE ||
            entry.confirmation_height <= 0) {
            continue;
        }
        checked_add(
            carrier_base,
            std::min<int64_t>(100, entry.carrier_value.value));
        checked_add(
            carrier_excess,
            std::max<int64_t>(0, entry.carrier_value.value - 100));
    }

    ProviderMaintenanceBudgetUsage usage;
    if (readiness.have_maintenance_ledger) {
        std::string error;
        if (!SummarizeProviderMaintenanceBudget(readiness.maintenance_ledger, now, usage, error)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                error == "PAYMASTER_MAINTENANCE_BUDGET_OVERFLOW"
                    ? "PAYMASTER_LIQUIDITY_STATUS_OVERFLOW" : error);
        }
    }

    UniValue result{UniValue::VOBJ};
    result.pushKV("policy_configured", readiness.have_liquidity_policy);
    result.pushKV("targets_satisfy_provider_policy",
                  targets_satisfy_provider_policy);
    result.pushKV("maintenance_state", state);
    result.pushKV("policy", LiquidityPolicyToJSON(policy));
    result.pushKV("admission_dgb", LiquiditySlotCountsToJSON(
        admission_dgb, policy.target_admission_dgb));
    result.pushKV("operational_dgb", LiquiditySlotCountsToJSON(
        operational_dgb, policy.target_operational_dgb));
    result.pushKV("admission_carriers", LiquiditySlotCountsToJSON(
        admission_carriers, policy.target_admission_carriers));
    result.pushKV("operational_carriers", LiquiditySlotCountsToJSON(
        operational_carriers, policy.target_operational_carriers));
    result.pushKV("maintenance_fee_reserved_satoshis", usage.reserved_fee.value);
    result.pushKV("maintenance_fee_planned_satoshis", usage.planned_fee.value);
    result.pushKV("maintenance_fee_broadcast_satoshis", usage.broadcast_fee.value);
    result.pushKV("maintenance_fee_spent_last_hour_satoshis", usage.spent_last_hour.value);
    result.pushKV("maintenance_fee_spent_last_day_satoshis", usage.spent_last_day.value);
    result.pushKV("carrier_base_cents", carrier_base);
    result.pushKV("carrier_withdrawable_excess_cents", carrier_excess);
    if (wallet && readiness.have_policy && missing > 0) {
        // Capital only, not a fee quote or authorization. Pending replacements
        // already count toward the targets and must not be requested twice.
        int64_t required_dgb{0};
        for (size_t i = 0; i < admission_dgb.missing; ++i) {
            checked_add(required_dgb, MIN_ADMISSION_DGB_SATOSHIS);
        }
        for (size_t i = 0; i < operational_dgb.missing; ++i) {
            checked_add(required_dgb, std::max<int64_t>(MIN_ADMISSION_DGB_SATOSHIS, operational_dgb_minimum));
        }
        const int64_t required_dd = 100 * (admission_carriers.missing + operational_carriers.missing);
        LOCK(wallet->cs_wallet);
        CCoinControl control;
        control.m_min_depth = 1;
        const CAmount available_dgb = AvailableCoins(*wallet, &control).GetTotalAmount();
        const auto* dd_wallet = wallet->GetDDWallet();
        const CAmount available_dd = dd_wallet ? dd_wallet->GetSpendableDDBalance() : 0;
        UniValue funding{UniValue::VOBJ};
        funding.pushKV("required_dgb_satoshis", required_dgb);
        funding.pushKV("available_dgb_satoshis", available_dgb);
        funding.pushKV("shortfall_dgb_satoshis", std::max<int64_t>(0, required_dgb - available_dgb));
        funding.pushKV("required_dd_cents", required_dd);
        funding.pushKV("available_dd_cents", available_dd);
        funding.pushKV("shortfall_dd_cents", std::max<int64_t>(0, required_dd - available_dd));
        result.pushKV("refill_funding", std::move(funding));
    }
    result.pushKV("readiness_errors", ReadinessErrorsToJSON(readiness.errors));
    return result;
}


// The caller holds the provider work guard and has reconciled existing work.
// Replace only an unsigned plan, atomically retaining its released history and
// reserving the exact smaller output set before any transaction is signed.

// Paid DGB replenishment reserves its worst-case maintenance fee before wallet
// construction. The durable plan record makes a crash after broadcast
// distinguishable from a request to create a second replacement transaction.

// DD carriers are ordinary wallet value with a dedicated provider role. Never
// select reserved/pending carrier value, and never silently turn normal client
// DD into provider liquidity without the configured target and preview binding.

UniValue ReliabilityToJSON(const DigiDollar::Paymaster::PaymasterReliabilityRecord& record,
                           int64_t now)
{
    const auto summary = DigiDollar::Paymaster::SummarizeReliability(record, now);
    UniValue result{UniValue::VOBJ};
    result.pushKV("provider_id", record.provider_id.GetHex());
    result.pushKV("successful_attempts", summary.successful_attempts);
    result.pushKV("provider_failures", summary.provider_failures);
    result.pushKV("neutral_failures", summary.neutral_failures);
    result.pushKV("availability_timeouts", summary.availability_timeouts);
    result.pushKV("sufficient_data", summary.sufficient_data);
    if (summary.sufficient_data) result.pushKV("success_rate_basis_points", summary.success_rate_basis_points);
    result.pushKV("latency_ewma_ms", record.latency_ewma_ms);
    result.pushKV("consecutive_provider_failures", record.consecutive_provider_failures);
    result.pushKV("last_observation_at", record.last_observation_at);
    result.pushKV("cooldown_until", record.cooldown_until);
    result.pushKV("cooldown_active", record.cooldown_until > now);
    return result;
}

// -------------------------------------------------------------------------
// Client session serialization, replay, and equivocation handling
// -------------------------------------------------------------------------

ClientSessionLookup ParseClientSessionLookup(const UniValue& lookup)
{
    RPCTypeCheckObj(lookup,
                    {{"request_id", UniValueType(UniValue::VSTR)},
                     {"session_id", UniValueType(UniValue::VSTR)}},
                    /*fAllowNull=*/true, /*fStrict=*/true);
    const bool by_request = !lookup.find_value("request_id").isNull();
    const bool by_session = !lookup.find_value("session_id").isNull();
    if (by_request == by_session) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "lookup must contain exactly one of request_id or session_id");
    }
    if (by_request) return lookup.find_value("request_id").get_str();
    return ParseHashO(lookup, "session_id");
}

bool FindSession(const UniValue& lookup, PaymasterStore& store, DigiDollar::Paymaster::PaymentSession& session)
{
    return FindClientSession(ParseClientSessionLookup(lookup), store, session);
}

std::vector<COutPoint> ParsePaymasterInputs(const UniValue& value)
{
    std::vector<COutPoint> inputs;
    std::set<COutPoint> unique;
    for (const UniValue& item : value.getValues()) {
        RPCTypeCheckObj(item,
                        {{"txid", UniValueType(UniValue::VSTR)},
                         {"vout", UniValueType(UniValue::VNUM)}},
                        /*fAllowNull=*/false, /*fStrict=*/true);
        const int64_t output_index = item.find_value("vout").getInt<int64_t>();
        if (output_index < 0 || output_index > std::numeric_limits<uint32_t>::max()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "selected input vout is out of range");
        }
        COutPoint outpoint{ParseHashO(item, "txid"), static_cast<uint32_t>(output_index)};
        if (outpoint.IsNull() || !unique.insert(outpoint).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "selected inputs contain an invalid duplicate");
        }
        inputs.push_back(outpoint);
    }
    if (inputs.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "selected_inputs must not be empty");
    return inputs;
}


DigiDollar::Paymaster::ProviderPolicy ParseProviderPolicy(const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(value,
                    {{"funding_models", UniValueType(UniValue::VARR)},
                     {"sponsorship_scope", UniValueType(UniValue::VSTR)},
                     {"fee_rate_bps", UniValueType(UniValue::VNUM)},
                     {"maximum_user_paid_service_fee_cents", UniValueType(UniValue::VNUM)},
                     {"min_amount_cents", UniValueType(UniValue::VNUM)},
                     {"max_amount_cents", UniValueType(UniValue::VNUM)},
                     {"quote_ttl", UniValueType(UniValue::VNUM)},
                     {"maximum_network_fee_dgb_satoshis", UniValueType(UniValue::VNUM)}},
                    /*fAllowNull=*/false, /*fStrict=*/true);
    ProviderPolicy policy;
    for (const UniValue& model : value.find_value("funding_models").get_array().getValues()) {
        if (!model.isStr()) throw JSONRPCError(RPC_INVALID_PARAMETER, "funding_models entries must be strings");
        uint8_t bit{0};
        if (model.get_str() == "sponsored")
            bit = FUNDING_MODEL_SPONSORED;
        else if (model.get_str() == "user_paid")
            bit = FUNDING_MODEL_USER_PAID;
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown Paymaster funding model");
        if ((policy.funding_models & bit) != 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Duplicate Paymaster funding model");
        }
        policy.funding_models |= bit;
    }
    const std::string scope = value.find_value("sponsorship_scope").get_str();
    if (scope == "public")
        policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    else if (scope == "restricted")
        policy.sponsorship_scope = SponsorshipScope::RESTRICTED;
    else
        throw JSONRPCError(RPC_INVALID_PARAMETER, "sponsorship_scope must be public or restricted");
    policy.fee_rate_bps = value.find_value("fee_rate_bps").getInt<uint32_t>();
    if (!value.find_value("maximum_user_paid_service_fee_cents").isNull()) {
        policy.maximum_user_paid_service_fee = DDCents{
            value.find_value("maximum_user_paid_service_fee_cents")
                .getInt<int64_t>()};
    }
    policy.min_payment = DDCents{value.find_value("min_amount_cents").getInt<int64_t>()};
    policy.max_payment = DDCents{value.find_value("max_amount_cents").getInt<int64_t>()};
    policy.quote_ttl = value.find_value("quote_ttl").getInt<int64_t>();
    policy.maximum_network_fee = DGBSatoshis{
        value.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>()};
    std::string error;
    if (!ValidateProviderPolicy(policy, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    return policy;
}

DigiDollar::Paymaster::FundingSafetyLimits ParseFundingSafetyLimits(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"maximum_network_fee_per_transaction_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_reserved_network_fee_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_network_fee_per_hour_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_network_fee_per_day_satoshis", UniValueType(UniValue::VNUM)},
         {"maximum_completed_per_hour", UniValueType(UniValue::VNUM)},
         {"maximum_completed_per_day", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{
        value.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>()};
    limits.maximum_reserved_network_fee = DGBSatoshis{
        value.find_value("maximum_reserved_network_fee_satoshis").getInt<int64_t>()};
    limits.maximum_network_fee_per_hour = DGBSatoshis{
        value.find_value("maximum_network_fee_per_hour_satoshis").getInt<int64_t>()};
    limits.maximum_network_fee_per_day = DGBSatoshis{
        value.find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>()};
    limits.maximum_completed_per_hour =
        value.find_value("maximum_completed_per_hour").getInt<uint32_t>();
    limits.maximum_completed_per_day =
        value.find_value("maximum_completed_per_day").getInt<uint32_t>();
    return limits;
}

DigiDollar::Paymaster::ProviderSafetyPolicy ParseProviderSafetyPolicy(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(
        value,
        {{"user_paid", UniValueType(UniValue::VOBJ)},
         {"public_sponsored", UniValueType(UniValue::VOBJ)},
         {"restricted_sponsored", UniValueType(UniValue::VOBJ)},
         {"maximum_active_quotes_total", UniValueType(UniValue::VNUM)},
         {"maximum_active_quotes_per_netgroup", UniValueType(UniValue::VNUM)},
         {"maximum_active_quotes_per_recipient", UniValueType(UniValue::VNUM)},
         {"maximum_quote_requests_per_netgroup_per_minute", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/true);
    ProviderSafetyPolicy policy;
    policy.user_paid = ParseFundingSafetyLimits(value.find_value("user_paid"));
    policy.public_sponsored = ParseFundingSafetyLimits(value.find_value("public_sponsored"));
    policy.restricted_sponsored = ParseFundingSafetyLimits(value.find_value("restricted_sponsored"));
    policy.maximum_active_quotes_total =
        value.find_value("maximum_active_quotes_total").getInt<uint32_t>();
    policy.maximum_active_quotes_per_netgroup =
        value.find_value("maximum_active_quotes_per_netgroup").getInt<uint32_t>();
    policy.maximum_active_quotes_per_recipient =
        value.find_value("maximum_active_quotes_per_recipient").getInt<uint32_t>();
    policy.maximum_quote_requests_per_netgroup_per_minute =
        value.find_value("maximum_quote_requests_per_netgroup_per_minute").getInt<uint32_t>();
    return policy;
}

UniValue ProviderSafetyClassStatusToJSON(
    const DigiDollar::Paymaster::ProviderBudgetLedger& ledger,
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
    DigiDollar::Paymaster::FundingModel model,
    DigiDollar::Paymaster::SponsorshipScope scope,
    int64_t now)
{
    using namespace DigiDollar::Paymaster;
    CScript status_script;
    status_script << OP_TRUE;
    const ProviderSafetyStatus status = EvaluateProviderSafetyStatus(
        ledger, policy, model, scope, GetRecipientBudgetBucket(ledger, status_script),
        DGBSatoshis{1}, now);
    UniValue result{UniValue::VOBJ};
    result.pushKV("reserved_network_fee_satoshis", status.reserved_network_fee.value);
    result.pushKV("spent_network_fee_last_hour_satoshis",
                  status.spent_network_fee_last_hour.value);
    result.pushKV("spent_network_fee_last_day_satoshis",
                  status.spent_network_fee_last_day.value);
    result.pushKV("active_quotes", status.active_quotes);
    result.pushKV("completed_last_hour", status.completed_last_hour);
    result.pushKV("completed_last_day", status.completed_last_day);
    result.pushKV("can_accept_minimum_quote", status.can_accept_quote);
    result.pushKV("errors", ReadinessErrorsToJSON(status.errors));
    return result;
}


DigiDollar::Paymaster::ClientSafetyPolicy ParseClientSafetyPolicy(
    const UniValue& value)
{
    using namespace DigiDollar::Paymaster;
    RPCTypeCheckObj(value,
        {{"maximum_service_fee_per_transaction_cents", UniValueType(UniValue::VNUM)},
         {"maximum_service_fee_per_day_cents", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/false, /*fStrict=*/false);
    RPCTypeCheckObj(
        value,
        {{"maximum_service_fee_per_transaction_cents", UniValueType(UniValue::VNUM)},
         {"maximum_service_fee_per_day_cents", UniValueType(UniValue::VNUM)},
         {"maximum_service_fee_bps", UniValueType(UniValue::VNUM)}},
        /*fAllowNull=*/true, /*fStrict=*/true);
    ClientSafetyPolicy policy;
    policy.maximum_service_fee_per_transaction = DDCents{
        value.find_value("maximum_service_fee_per_transaction_cents").getInt<int64_t>()};
    policy.maximum_service_fee_per_day = DDCents{
        value.find_value("maximum_service_fee_per_day_cents").getInt<int64_t>()};
    if (value.exists("maximum_service_fee_bps")) {
        int64_t bps;
        try {
            bps = value.find_value("maximum_service_fee_bps").getInt<int64_t>();
        } catch (const std::exception&) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "maximum_service_fee_bps must be an integer between 0 and 10000");
        }
        if (bps < 0 || bps > 10000) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "maximum_service_fee_bps must be between 0 and 10000");
        }
        policy.maximum_service_fee_bps = static_cast<int32_t>(bps);
    }
    return policy;
}

std::vector<RPCArg> FundingSafetyLimitArgs()
{
    return {
        {"maximum_network_fee_per_transaction_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Maximum miner fee for one transaction; zero only disables an unused model"},
        {"maximum_reserved_network_fee_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Maximum miner fee reserved by concurrent quotes"},
        {"maximum_network_fee_per_hour_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-hour miner-fee ceiling"},
        {"maximum_network_fee_per_day_satoshis", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-day miner-fee ceiling"},
        {"maximum_completed_per_hour", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-hour transaction ceiling"},
        {"maximum_completed_per_day", RPCArg::Type::NUM,
         RPCArg::Optional::NO, "Rolling-day transaction ceiling"},
    };
}

std::vector<RPCResult> FundingSafetyLimitResults()
{
    return {
        {RPCResult::Type::NUM, "maximum_network_fee_per_transaction_satoshis", "Per-transaction miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_reserved_network_fee_satoshis", "Concurrent reserved miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_network_fee_per_hour_satoshis", "Rolling-hour miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_network_fee_per_day_satoshis", "Rolling-day miner-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_completed_per_hour", "Rolling-hour transaction ceiling"},
        {RPCResult::Type::NUM, "maximum_completed_per_day", "Rolling-day transaction ceiling"},
    };
}

std::vector<RPCResult> ProviderSafetyClassStatusResults()
{
    return {
        {RPCResult::Type::NUM, "reserved_network_fee_satoshis", "Miner fee held by active quotes"},
        {RPCResult::Type::NUM, "spent_network_fee_last_hour_satoshis", "Miner fee committed during the rolling hour"},
        {RPCResult::Type::NUM, "spent_network_fee_last_day_satoshis", "Miner fee committed during the rolling day"},
        {RPCResult::Type::NUM, "active_quotes", "All active provider quote reservations"},
        {RPCResult::Type::NUM, "completed_last_hour", "Completed transactions during the rolling hour"},
        {RPCResult::Type::NUM, "completed_last_day", "Completed transactions during the rolling day"},
        {RPCResult::Type::BOOL, "can_accept_minimum_quote", "Whether a one-satoshi hypothetical quote fits the limits"},
        {RPCResult::Type::ARR, "errors", "Safety failures", {{RPCResult::Type::STR, "", "Stable Paymaster safety error"}}},
    };
}

std::vector<RPCResult> ProviderSafetyPolicyResults()
{
    return {
        {RPCResult::Type::OBJ, "user_paid", "USER_PAID loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::OBJ, "public_sponsored", "Public SPONSORED loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::OBJ, "restricted_sponsored", "Restricted SPONSORED loss ceilings",
         FundingSafetyLimitResults()},
        {RPCResult::Type::NUM, "maximum_active_quotes_total", "Maximum concurrent active quotes"},
        {RPCResult::Type::NUM, "maximum_active_quotes_per_netgroup", "Maximum concurrent quotes per network group"},
        {RPCResult::Type::NUM, "maximum_active_quotes_per_recipient", "Maximum concurrent quotes per pseudonymous recipient bucket"},
        {RPCResult::Type::NUM, "maximum_quote_requests_per_netgroup_per_minute", "Quote request rate per network group"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last policy update"},
    };
}

std::vector<RPCResult> ProviderLiquidityPolicyResults()
{
    return {
        {RPCResult::Type::BOOL, "automatic_replenishment", "Whether automatic maintenance is enabled"},
        {RPCResult::Type::BOOL, "paid_maintenance_approved", "Whether finite paid maintenance was approved"},
        {RPCResult::Type::NUM, "target_admission_dgb", "Target number of admission DGB slots"},
        {RPCResult::Type::NUM, "target_operational_dgb", "Target number of operational DGB slots"},
        {RPCResult::Type::NUM, "target_admission_carriers", "Target number of admission DigiDollar carriers"},
        {RPCResult::Type::NUM, "target_operational_carriers", "Target number of operational DigiDollar carriers"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_transaction_satoshis", "Per-transaction maintenance-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_hour_satoshis", "Rolling-hour maintenance-fee ceiling"},
        {RPCResult::Type::NUM, "maximum_maintenance_fee_per_day_satoshis", "Rolling-day maintenance-fee ceiling"},
        {RPCResult::Type::NUM_TIME, "updated_at", "Last liquidity-policy update"},
    };
}

std::vector<RPCResult> ProviderLiquiditySlotResults()
{
    return {
        {RPCResult::Type::NUM, "target", "Configured slot target"},
        {RPCResult::Type::NUM, "ready", "Confirmed available slots"},
        {RPCResult::Type::NUM, "pending", "Unconfirmed successor or maintenance slots"},
        {RPCResult::Type::NUM, "counted_toward_target", "Available, reserved, or pending slots counted toward the target"},
        {RPCResult::Type::NUM, "missing", "Slots still missing from the target"},
    };
}

std::vector<RPCResult> ProviderLiquidityStatusResults()
{
    return {
        {RPCResult::Type::BOOL, "policy_configured", "Whether the operator saved the policy"},
        {RPCResult::Type::BOOL, "targets_satisfy_provider_policy", "Whether the saved targets can satisfy the active provider offer"},
        {RPCResult::Type::STR, "maintenance_state", "Reserve demand; does not imply active automatic work"},
        {RPCResult::Type::OBJ, "automation_status", /*optional=*/true, "Saved intent and observed execution", {
            {RPCResult::Type::BOOL, "configured", "Policy is saved"},
            {RPCResult::Type::BOOL, "enabled", "Saved refill choice"},
            {RPCResult::Type::BOOL, "paid_maintenance_approved", "Saved bounded spending consent"},
            {RPCResult::Type::STR, "state", "off, ready, paused, blocked, working or waiting_confirmation"},
            {RPCResult::Type::STR, "reason", "Stable reason, empty when ready"},
            {RPCResult::Type::BOOL, "pending_transaction", "Previously signed refill is still pending"},
        }},
        {RPCResult::Type::OBJ, "policy", "Saved or suggested liquidity policy",
         ProviderLiquidityPolicyResults()},
        {RPCResult::Type::OBJ, "admission_dgb", "Admission DGB slot status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "operational_dgb", "Operational DGB slot status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "admission_carriers", "Admission DigiDollar carrier status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::OBJ, "operational_carriers", "Operational DigiDollar carrier status",
         ProviderLiquiditySlotResults()},
        {RPCResult::Type::NUM, "maintenance_fee_reserved_satoshis", "Maintenance fee exposed by planned or broadcast transactions"},
        {RPCResult::Type::NUM, "maintenance_fee_planned_satoshis", "Fee budget reserved before a transaction exists"},
        {RPCResult::Type::NUM, "maintenance_fee_broadcast_satoshis", "Actual fees of saved unconfirmed transactions"},
        {RPCResult::Type::NUM, "maintenance_fee_spent_last_hour_satoshis", "Confirmed maintenance fees in the rolling hour"},
        {RPCResult::Type::NUM, "maintenance_fee_spent_last_day_satoshis", "Confirmed maintenance fees in the rolling day"},
        {RPCResult::Type::NUM, "carrier_base_cents", "DigiDollar base value retained in available carriers"},
        {RPCResult::Type::NUM, "carrier_withdrawable_excess_cents", "DigiDollar carrier value available for excess withdrawal"},
        {RPCResult::Type::OBJ, "refill_funding", /*optional=*/true, "Capital still needed to reach saved targets; network fees are additional. Smaller refills may proceed within existing approvals.", {
            {RPCResult::Type::NUM, "required_dgb_satoshis", "DGB capital for missing reserves, excluding pending replacements and network fees"},
            {RPCResult::Type::NUM, "available_dgb_satoshis", "Confirmed ordinary DGB eligible for refill; protected reserves and locked coins excluded"},
            {RPCResult::Type::NUM, "shortfall_dgb_satoshis", "Missing DGB capital before additional network fees"},
            {RPCResult::Type::NUM, "required_dd_cents", "DD capital for missing carriers, excluding pending replacements"},
            {RPCResult::Type::NUM, "available_dd_cents", "Ordinary spendable DD outside protected reserves"},
            {RPCResult::Type::NUM, "shortfall_dd_cents", "Missing DD capital"},
        }},
        {RPCResult::Type::ARR, "readiness_errors", "Outstanding liquidity readiness gates",
         {{RPCResult::Type::STR, "", "Stable Paymaster readiness error"}}},
    };
}

} // namespace paymaster_rpc::internal

} // namespace wallet
