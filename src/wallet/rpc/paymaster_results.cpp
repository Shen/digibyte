// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <wallet/rpc/paymaster_results.h>
#include <wallet/paymasterapplication.h>
#include <wallet/paymasterpool.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/validation.h>
#include <streams.h>
#include <key_io.h>
#include <base58.h>
#include <util/strencodings.h>
#include <version.h>

namespace wallet::paymaster_rpc::internal {
using namespace DigiDollar::Paymaster;
using namespace paymaster_app;

UniValue ToJSON(const ProviderActivitySnapshot& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("active_payments", value.active_payments);
    result.pushKV("capacity_requests", value.capacity_requests);
    result.pushKV("reserved_outputs", value.reserved_outputs);
    result.pushKV("pending_confirmations", value.pending_confirmations);
    return result;
}

UniValue ToJSON(const ProviderAutomationSnapshot& value)
{
    const char* state{"unknown"};
    switch (value.state) {
    case ProviderAutomationState::READY: state = "ready"; break;
    case ProviderAutomationState::OFF: state = "off"; break;
    case ProviderAutomationState::BLOCKED: state = "blocked"; break;
    case ProviderAutomationState::PAUSED: state = "paused"; break;
    case ProviderAutomationState::WAITING_CONFIRMATION: state = "waiting_confirmation"; break;
    case ProviderAutomationState::WORKING: state = "working"; break;
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("configured", value.configured);
    result.pushKV("enabled", value.enabled);
    result.pushKV("paid_maintenance_approved", value.paid_maintenance_approved);
    result.pushKV("state", state);
    result.pushKV("reason", value.reason);
    result.pushKV("pending_transaction", value.pending_transaction);
    return result;
}

UniValue ToJSON(const ProviderFinanceStatus& value)
{
    const char* period_name{"unknown"};
    switch (value.query.period) {
    case ProviderFinancePeriod::TODAY: period_name = "today"; break;
    case ProviderFinancePeriod::DAYS7: period_name = "7d"; break;
    case ProviderFinancePeriod::DAYS30: period_name = "30d"; break;
    case ProviderFinancePeriod::ALL: period_name = "all"; break;
    }
    const auto& report = value.report;
    const auto& ledger = report.metadata;
    const auto& totals = report.periods[size_t(value.query.period)];
    const int64_t dd_income = totals.total.dd_income.value;
    const int64_t dgb_cost = totals.total.dgb_cost.value;
    const uint64_t transfers = totals.total.successful_transfers;
    const uint64_t user_paid = totals.total.user_paid_transfers;
    const uint64_t public_sponsored = totals.total.public_sponsored_transfers;
    const uint64_t restricted_sponsored = totals.total.restricted_sponsored_transfers;
    const int64_t user_paid_income = totals.income[0], user_paid_cost = totals.cost[0];
    const int64_t public_sponsored_income = totals.income[1], public_sponsored_cost = totals.cost[1];
    const int64_t restricted_sponsored_income = totals.income[2], restricted_sponsored_cost = totals.cost[2];
    UniValue result{UniValue::VOBJ};
    result.pushKV("provider_id", value.provider_id.GetHex());
    result.pushKV("period", period_name);
    result.pushKV("revision", report.metadata.revision);
    result.pushKV("service_fee_income_cents", dd_income);
    result.pushKV("dgb_operating_cost_satoshis", dgb_cost);
    result.pushKV("successful_transfers", transfers);
    result.pushKV("average_service_fee_cents",
                  value.average_service_fee[size_t(value.query.period)].value);
    result.pushKV("user_paid_transfers", user_paid);
    result.pushKV("public_sponsored_transfers", public_sponsored);
    result.pushKV("restricted_sponsored_transfers", restricted_sponsored);
    const auto make_model_summary = [](uint64_t model_transfers,
                                       int64_t model_income,
                                       int64_t model_cost) {
        UniValue summary{UniValue::VOBJ};
        summary.pushKV("successful_transfers", model_transfers);
        summary.pushKV("service_fee_income_cents", model_income);
        summary.pushKV("dgb_operating_cost_satoshis", model_cost);
        return summary;
    };
    UniValue model_breakdown{UniValue::VOBJ};
    model_breakdown.pushKV(
        "user_paid",
        make_model_summary(user_paid, user_paid_income,
                           user_paid_cost));
    model_breakdown.pushKV(
        "public_sponsored",
        make_model_summary(public_sponsored,
                           public_sponsored_income,
                           public_sponsored_cost));
    model_breakdown.pushKV(
        "restricted_sponsored",
        make_model_summary(restricted_sponsored,
                           restricted_sponsored_income,
                           restricted_sponsored_cost));
    result.pushKV("model_breakdown", std::move(model_breakdown));

    // All cards use the same revision and checked materialized totals.
    const auto make_period_summary = [&](ProviderFinancePeriod summary_period) {
        const auto& summary_totals = report.periods[size_t(summary_period)].total;
        const auto summary_income = summary_totals.dd_income.value;
        const auto summary_cost = summary_totals.dgb_cost.value;
        const auto summary_transfers = summary_totals.successful_transfers;
        const auto summary_user_paid = summary_totals.user_paid_transfers;
        const auto summary_public_sponsored = summary_totals.public_sponsored_transfers;
        const auto summary_restricted_sponsored = summary_totals.restricted_sponsored_transfers;
        UniValue summary{UniValue::VOBJ};
        summary.pushKV("service_fee_income_cents", summary_income);
        summary.pushKV("dgb_operating_cost_satoshis", summary_cost);
        summary.pushKV("successful_transfers", summary_transfers);
        summary.pushKV(
            "average_service_fee_cents",
            value.average_service_fee[size_t(summary_period)].value);
        summary.pushKV("user_paid_transfers", summary_user_paid);
        summary.pushKV("public_sponsored_transfers", summary_public_sponsored);
        summary.pushKV("restricted_sponsored_transfers", summary_restricted_sponsored);
        return summary;
    };
    UniValue period_summaries{UniValue::VOBJ};
    period_summaries.pushKV("today", make_period_summary(ProviderFinancePeriod::TODAY));
    period_summaries.pushKV("7d", make_period_summary(ProviderFinancePeriod::DAYS7));
    period_summaries.pushKV("30d", make_period_summary(ProviderFinancePeriod::DAYS30));
    period_summaries.pushKV("all", make_period_summary(ProviderFinancePeriod::ALL));
    result.pushKV("period_summaries", std::move(period_summaries));
    result.pushKV("history_partially_reconstructable",
                  ledger.earlier_history_partial);
    result.pushKV("history_complete_from", ledger.history_complete_from);
    result.pushKV("backup_required", value.backup_required);
    result.pushKV("last_successful_backup_at",
                  value.backup.last_successful_backup_at);
    result.pushKV("external_backup_acknowledged_at",
                  value.backup.external_backup_acknowledged_at);

    if (value.valuation) {
        result.pushKV("oracle_price_micro_usd", value.valuation->oracle_price_micro_usd);
        result.pushKV("valuation_time", value.valuation->time);
        result.pushKV("estimated_result_usd", value.valuation->estimated_result_usd);
    }

    UniValue pool_capital{UniValue::VOBJ};
    pool_capital.pushKV("dgb_available_satoshis", value.pool_capital.dgb_available.value);
    pool_capital.pushKV("dgb_reserved_satoshis", value.pool_capital.dgb_reserved.value);
    pool_capital.pushKV("dgb_pending_satoshis", value.pool_capital.dgb_pending.value);
    pool_capital.pushKV("carrier_base_cents", value.pool_capital.carrier_base.value);
    pool_capital.pushKV("carrier_earned_cents", value.pool_capital.carrier_earned.value);
    pool_capital.pushKV("carrier_withdrawable_cents", value.pool_capital.carrier_withdrawable.value);
    pool_capital.pushKV("pending_maintenance_transactions", value.pool_capital.pending_maintenance_transactions);
    result.pushKV("pool_capital", std::move(pool_capital));

    if (value.query.include_events) {
        UniValue days{UniValue::VARR};
        for (const auto& entry : report.days) {
            const auto* total = &entry.total;
            UniValue day{UniValue::VOBJ};
            day.pushKV("day_start", total->day_start);
            day.pushKV("service_fee_income_cents",
                       total->dd_income.value);
            day.pushKV("dgb_operating_cost_satoshis",
                       total->dgb_cost.value);
            day.pushKV("successful_transfers",
                       total->successful_transfers);
            day.pushKV("maintenance_transactions",
                       total->maintenance_transactions);
            days.push_back(std::move(day));
        }
        result.pushKV("daily_totals", std::move(days));

        UniValue events{UniValue::VARR};
        const auto kind_name = [](ProviderFinanceEventKind kind) {
            switch (kind) {
            case ProviderFinanceEventKind::TRANSFER: return "transfer";
            case ProviderFinanceEventKind::POOL_SETUP: return "setup";
            case ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT: return "replenishment";
            case ProviderFinanceEventKind::POOL_RETIREMENT: return "retirement";
            case ProviderFinanceEventKind::CARRIER_WITHDRAWAL: return "withdrawal";
            case ProviderFinanceEventKind::RECOVERY: return "recovery";
            }
            return "invalid";
        };
        const auto state_name = [](ProviderFinanceEventState state) {
            switch (state) {
            case ProviderFinanceEventState::PENDING: return "pending";
            case ProviderFinanceEventState::CONFIRMED: return "confirmed";
            case ProviderFinanceEventState::INVALIDATED: return "invalidated";
            }
            return "invalid";
        };
        for (const auto& event : report.events) {
            UniValue item{UniValue::VOBJ};
            item.pushKV("event_id", event.event_id.GetHex());
            item.pushKV("kind", kind_name(event.kind));
            item.pushKV("state", state_name(event.state));
            item.pushKV("dd_income_cents", event.dd_income.value);
            item.pushKV("dgb_cost_satoshis", event.dgb_cost.value);
            item.pushKV("created_at", event.created_at);
            if (event.confirmed_at > 0) {
                item.pushKV("confirmed_at", event.confirmed_at);
            }
            if (event.kind == ProviderFinanceEventKind::TRANSFER) {
                item.pushKV("funding_model",
                            event.funding_model == FundingModel::USER_PAID
                                ? "user_paid" : "sponsored");
                if (event.funding_model == FundingModel::SPONSORED) {
                    item.pushKV("sponsorship_scope",
                                event.sponsorship_scope == SponsorshipScope::PUBLIC
                                    ? "public" : "restricted");
                }
            }
            item.pushKV("transaction_id", event.transaction_id.GetHex());
            events.push_back(std::move(item));
        }
        result.pushKV("events", std::move(events));
        if (!report.next_cursor.empty()) {
            result.pushKV("next_cursor", report.next_cursor);
        }
    }
    return result;
}

std::string ResultStatusName(DigiDollar::Paymaster::PaymasterResultStatus status)
{
    using DigiDollar::Paymaster::PaymasterResultStatus;
    switch (status) {
    case PaymasterResultStatus::NO_FINAL_COMMIT: return "no_final_commit";
    case PaymasterResultStatus::USER_PSBT_ACCEPTED: return "user_psbt_accepted";
    case PaymasterResultStatus::FINAL_COMMITTED: return "final_committed";
    case PaymasterResultStatus::BROADCAST_ATTEMPTED: return "broadcast_attempted";
    case PaymasterResultStatus::SLOT_UNAVAILABLE: return "slot_unavailable";
    case PaymasterResultStatus::REJECTED: return "rejected";
    }
    return "unknown";
}

std::string FeeModeName(DigiDollar::Paymaster::FeeMode mode)
{
    using DigiDollar::Paymaster::FeeMode;
    switch (mode) {
    case FeeMode::DGB: return "dgb";
    case FeeMode::PAYMASTER: return "paymaster";
    case FeeMode::AUTO: return "auto";
    }
    return "unknown";
}

std::string AlternativeRecoveryPhaseName(
    DigiDollar::Paymaster::AlternativeRecoveryPhase phase)
{
    using DigiDollar::Paymaster::AlternativeRecoveryPhase;
    switch (phase) {
    case AlternativeRecoveryPhase::CAPACITY_PENDING: return "capacity_pending";
    case AlternativeRecoveryPhase::REQUEST_READY: return "request_ready";
    case AlternativeRecoveryPhase::RESPONSE_VALIDATED: return "response_validated";
    case AlternativeRecoveryPhase::USER_SIGNED: return "user_signed";
    case AlternativeRecoveryPhase::FINAL_COMMITTED: return "final_committed";
    }
    return "unknown";
}

UniValue ProviderPolicyToJSON(const DigiDollar::Paymaster::ProviderPolicy& policy)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    UniValue models{UniValue::VARR};
    if (PolicyAllowsFundingModel(policy, FundingModel::SPONSORED)) models.push_back("sponsored");
    if (PolicyAllowsFundingModel(policy, FundingModel::USER_PAID)) models.push_back("user_paid");
    result.pushKV("funding_models", std::move(models));
    result.pushKV("sponsorship_scope",
                  policy.sponsorship_scope == SponsorshipScope::PUBLIC ? "public" : "restricted");
    result.pushKV("fee_rate_bps", policy.fee_rate_bps);
    result.pushKV("maximum_user_paid_service_fee_cents",
                  policy.maximum_user_paid_service_fee.value);
    result.pushKV("min_amount_cents", policy.min_payment.value);
    result.pushKV("max_amount_cents", policy.max_payment.value);
    result.pushKV("quote_ttl", policy.quote_ttl);
    result.pushKV("maximum_network_fee_dgb_satoshis", policy.maximum_network_fee.value);
    result.pushKV("policy_hash", GetProviderPolicyHash(policy).GetHex());
    return result;
}

UniValue ClientSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_service_fee_per_transaction_cents",
                  policy.maximum_service_fee_per_transaction.value);
    result.pushKV("maximum_service_fee_per_day_cents",
                  policy.maximum_service_fee_per_day.value);
    if (policy.maximum_service_fee_bps >= 0) {
        result.pushKV("maximum_service_fee_bps", policy.maximum_service_fee_bps);
    }
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

UniValue LiquidityPolicyToJSON(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("automatic_replenishment", policy.automatic_replenishment);
    result.pushKV("paid_maintenance_approved", policy.paid_maintenance_approved);
    result.pushKV("target_admission_dgb", policy.target_admission_dgb);
    result.pushKV("target_operational_dgb", policy.target_operational_dgb);
    result.pushKV("target_admission_carriers", policy.target_admission_carriers);
    result.pushKV("target_operational_carriers", policy.target_operational_carriers);
    result.pushKV("maximum_maintenance_fee_per_transaction_satoshis",
                  policy.maximum_maintenance_fee_per_transaction.value);
    result.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                  policy.maximum_maintenance_fee_per_hour.value);
    result.pushKV("maximum_maintenance_fee_per_day_satoshis",
                  policy.maximum_maintenance_fee_per_day.value);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

UniValue ToJSON(const ProviderIdentityRecord& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("provider_id", value.provider_id.GetHex());
    result.pushKV("identity_key", HexStr(value.identity_key));
    result.pushKV("display_name", value.display_name);
    result.pushKV("created_at", value.created_at);
    return result;
}
UniValue ToJSON(const ProviderPolicy& value) { return ProviderPolicyToJSON(value); }
UniValue ToJSON(const ProviderSafetyPolicy& value) { return ProviderSafetyPolicyToJSON(value); }
UniValue ToJSON(const ClientSafetyPolicy& value) { return ClientSafetyPolicyToJSON(value); }
UniValue ToJSON(const ProviderLiquidityPolicy& value) { return LiquidityPolicyToJSON(value); }

UniValue ToJSON(const ProviderEnabledResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("enabled", value.enabled);
    result.pushKV("running", value.running);
    if (value.policy_hash) result.pushKV("policy_hash", value.policy_hash->GetHex());
    return result;
}

UniValue ToJSON(const RestrictedDescriptorResult& value)
{
    const auto& descriptor = value.descriptor;
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << descriptor;
    UniValue result{UniValue::VOBJ};
    result.pushKV("descriptor", HexStr(MakeUCharSpan(stream)));
    result.pushKV("provider_id", descriptor.provider_id.GetHex());
    result.pushKV("provider_identity_key", HexStr(value.provider_identity_key));
    result.pushKV("offer_id", descriptor.offer_id.GetHex());
    result.pushKV("policy_hash", descriptor.policy_hash.GetHex());
    result.pushKV("endpoint", descriptor.p2p_endpoint.ToStringAddrPort());
    result.pushKV("expires_at", descriptor.expires_at);
    return result;
}

UniValue ToJSON(const ProviderRuntimeResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("operation_mode", ProviderOperationModeName(value.operation_mode));
    result.pushKV("autostart", value.autostart);
    result.pushKV("running", value.running);
    return result;
}

UniValue ToJSON(const MaintenanceCancellationResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("executed", value.executed);
    result.pushKV("plan_id", value.review.plan_id.GetHex());
    UniValue ids{UniValue::VARR};
    for (const auto& id : value.operation_ids) ids.push_back(id.GetHex());
    result.pushKV("operation_ids", std::move(ids));
    result.pushKV("released_fee_budget_satoshis", value.review.released_fee_budget);
    result.pushKV("automatic_refill_disabled", value.executed);
    return result;
}

UniValue ToJSON(const CapitalReleaseResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("executed", value.executed);
    result.pushKV("plan_id", value.release.plan_id.GetHex());
    result.pushKV("pool_entries", value.release.entries);
    result.pushKV("dgb_satoshis", value.release.dgb_satoshis);
    result.pushKV("dd_cents", value.release.dd_cents);
    result.pushKV("network_fee_satoshis", 0);
    result.pushKV("uncreated_maintenance", value.release.uncreated_maintenance);
    result.pushKV("recovery_release", value.release.recovery_release);
    return result;
}

UniValue ToJSON(const BackupAdoptionResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("executed", value.executed);
    result.pushKV("plan_id", value.review.plan_id.GetHex());
    result.pushKV("provider_id", value.provider_id.GetHex());
    result.pushKV("pool_entries", value.review.entries);
    result.pushKV("dgb_satoshis", value.review.dgb_satoshis);
    result.pushKV("dd_cents", value.review.dd_cents);
    result.pushKV("network_fee_satoshis", 0);
    result.pushKV("provider_stopped", true);
    return result;
}

UniValue ToJSON(const BackupAcknowledgementResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("acknowledged", true);
    result.pushKV("backup_required", value.backup_required);
    result.pushKV("acknowledged_at", value.acknowledged_at);
    return result;
}
UniValue FundingSafetyLimitsToJSON(
    const DigiDollar::Paymaster::FundingSafetyLimits& limits)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_network_fee_per_transaction_satoshis",
                  limits.maximum_network_fee_per_transaction.value);
    result.pushKV("maximum_reserved_network_fee_satoshis",
                  limits.maximum_reserved_network_fee.value);
    result.pushKV("maximum_network_fee_per_hour_satoshis",
                  limits.maximum_network_fee_per_hour.value);
    result.pushKV("maximum_network_fee_per_day_satoshis",
                  limits.maximum_network_fee_per_day.value);
    result.pushKV("maximum_completed_per_hour", limits.maximum_completed_per_hour);
    result.pushKV("maximum_completed_per_day", limits.maximum_completed_per_day);
    return result;
}

UniValue ProviderSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("user_paid", FundingSafetyLimitsToJSON(policy.user_paid));
    result.pushKV("public_sponsored", FundingSafetyLimitsToJSON(policy.public_sponsored));
    result.pushKV("restricted_sponsored", FundingSafetyLimitsToJSON(policy.restricted_sponsored));
    result.pushKV("maximum_active_quotes_total", policy.maximum_active_quotes_total);
    result.pushKV("maximum_active_quotes_per_netgroup",
                  policy.maximum_active_quotes_per_netgroup);
    result.pushKV("maximum_active_quotes_per_recipient",
                  policy.maximum_active_quotes_per_recipient);
    result.pushKV("maximum_quote_requests_per_netgroup_per_minute",
                  policy.maximum_quote_requests_per_netgroup_per_minute);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

UniValue ToJSON(const TransportSnapshot& value)
{
    const auto& budget = value.budget;
    const auto used = value.usage;
    UniValue result{UniValue::VOBJ};
    result.pushKV("state", value.state ? DigiDollar::Paymaster::DirectStateName(*value.state) : budget.outbound ? "available" : "disabled");
    result.pushKV("outbound_limit", budget.outbound);
    result.pushKV("inbound_limit", budget.inbound);
    result.pushKV("handshake_limit", budget.handshakes);
    result.pushKV("outbound_in_use", used.outgoing);
    result.pushKV("inbound_in_use", used.incoming);
    result.pushKV("handshakes_in_use", used.handshakes);
    result.pushKV("queued", value.queued);
    result.pushKV("listener_ready", value.listener_ready);
    result.pushKV("external_reachability", "unknown");
    return result;
}
UniValue PaymasterTransportToJSON(const CConnman& connman, std::optional<DirectState> state)
{
    return ToJSON(CapturePaymasterTransport(connman, state));
}

UniValue ReadinessErrorsToJSON(const std::vector<std::string>& errors)
{
    UniValue result{UniValue::VARR};
    for (const std::string& error : errors)
        result.push_back(error);
    return result;
}

UniValue ToJSON(const ProviderStartResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("start_requested", value.start_requested);
    result.pushKV("running", value.running);
    result.pushKV("ready", value.ready);
    result.pushKV("drain_only", value.drain_only);
    result.pushKV("operation_mode", ProviderOperationModeName(value.operation_mode));
    result.pushKV("autostart", value.autostart);
    result.pushKV("service_state", ProviderServiceStateName(value.service_state));
    if (value.safety_policy) result.pushKV("safety_policy", ProviderSafetyPolicyToJSON(*value.safety_policy));
    if (value.provider_id) result.pushKV("provider_id", value.provider_id->GetHex());
    if (value.endpoint) result.pushKV("endpoint", value.endpoint->ToStringAddrPort());
    if (value.announcement_sequence) result.pushKV("announcement_sequence", *value.announcement_sequence);
    result.pushKV("recovery_candidates", value.recovery.candidates);
    result.pushKV("recovered_broadcasts", value.recovery.recovered);
    result.pushKV("expired_commits", 0);
    result.pushKV("recovery_errors", ReadinessErrorsToJSON(value.recovery.errors));
    result.pushKV("readiness_errors", ReadinessErrorsToJSON(value.readiness_errors));
    if (value.transport) result.pushKV("transport", ToJSON(*value.transport));
    return result;
}

UniValue ToJSON(const ProviderStopResult& value)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("running", value.running);
    return result;
}
void AddSessionPaymentStatus(UniValue& result, const SessionSnapshot& snapshot)
{
    const auto& session = snapshot.session;
    const auto& observation = snapshot.observation;
    const auto& error = snapshot.observation_error;
    if (snapshot.observation_requested) {
        const auto transaction = [](const PaymasterTransactionObservation& value) {
            UniValue json{UniValue::VOBJ};
            json.pushKV("details_available", value.details_available);
            json.pushKV("observation_available", value.observation_available);
            if (!value.txid.IsNull()) json.pushKV("txid", value.txid.GetHex());
            if (value.observation_available) {
                json.pushKV("confirmations", value.confirmations);
                json.pushKV("in_mempool", value.in_mempool);
            }
            if (!value.block_hash.IsNull()) {
                json.pushKV("block_hash", value.block_hash.GetHex());
                json.pushKV("block_height", value.block_height);
            }
            if (value.recipient_vout) {
                UniValue recipient{UniValue::VOBJ};
                recipient.pushKV("vout", *value.recipient_vout);
                recipient.pushKV("script_pub_key", HexStr(value.recipient_script));
                recipient.pushKV("amount_cents", value.recipient_amount.value);
                json.pushKV("recipient", std::move(recipient));
            }
            return json;
        };
        UniValue view{UniValue::VOBJ};
        view.pushKV("observed_tip_hash", observation.tip_hash.GetHex());
        view.pushKV("observed_tip_height", observation.tip_height);
        if (!error.empty()) view.pushKV("error", error);
        view.pushKV("payment", transaction(observation.payment));
        view.pushKV("recovery", transaction(observation.recovery));
        result.pushKV("payment_view", std::move(view));
    }
    const bool paid = observation.PaymentConfirmed(session.state);
    const bool recovered = observation.recovery.observation_available &&
                           observation.recovery.confirmations > 0;
    const std::string status{observation.Status(session.state)};
    result.pushKV("status", status);
    result.pushKV("payment_confirmed", paid);
    result.pushKV("final", IsTerminal(session.state));
    result.pushKV("confirmation_state", paid ? "payment_confirmed" :
                  (recovered ? "recovery_confirmed" :
                   (status == "conflicted" ? "conflicted" : "unconfirmed")));
    std::string broadcast_state{"not_attempted"};
    if (paid || recovered) broadcast_state = "confirmed";
    else if (observation.payment.in_mempool || observation.recovery.in_mempool ||
             session.state == SessionState::MEMPOOL ||
             session.pending_phase == PendingPhase::CANCEL_MEMPOOL) broadcast_state = "accepted_mempool";
    else if (session.state == SessionState::STEMPOOL) broadcast_state = "accepted_stempool";
    else if (session.state == SessionState::PENDING_PROVIDER ||
             !session.final_txid.IsNull() || !session.recovery_txid.IsNull()) broadcast_state = "unknown";
    result.pushKV("broadcast_state", broadcast_state);
}

UniValue ToJSON(const ClientPaymentOrder& order)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", order.request_id);
    result.pushKV("session_id", order.session_id.GetHex());
    result.pushKV("canonical_request_hash", order.canonical_request_hash.GetHex());
    CTxDestination recipient;
    if (ExtractDestination(order.recipient_script, recipient)) result.pushKV("to_address", EncodeDigiDollarAddress(recipient));
    result.pushKV("requested_amount_cents", order.requested_amount.value);
    result.pushKV("maximum_paymaster_fee_cents", order.maximum_service_fee.value);
    result.pushKV("maximum_provider_attempts", order.maximum_attempts);
    result.pushKV("fee_mode", FeeModeName(order.fee_mode));
    result.pushKV("privacy", order.privacy == PrivacyProfile::HIGH ? "high" : "standard");
    result.pushKV("selection", order.selection == SelectionMode::PRIVACY_WEIGHTED ? "privacy_weighted" : "lowest_total_cost");
    result.pushKV("subtract_paymaster_fee_from_amount", order.subtract_fee);
    result.pushKV("send_all_spendable_dd", order.send_all);
    result.pushKV("selection_constraint_known", order.version >= 2);
    if (!order.preferred_provider.IsNull()) {
        result.pushKV("preferred_provider_id", order.preferred_provider.GetHex());
        result.pushKV("preferred_offer_id", order.preferred_offer.GetHex());
    }
    UniValue inputs{UniValue::VARR};
    for (const auto& input : order.user_inputs) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("txid", input.hash.GetHex());
        entry.pushKV("vout", input.n);
        inputs.push_back(std::move(entry));
    }
    result.pushKV("selected_inputs", std::move(inputs));
    return result;
}

void AddResumeOrder(UniValue& result, const SessionSnapshot& snapshot)
{
    result.pushKV("resume_order_available", snapshot.resume_order.has_value());
    if (snapshot.resume_order) result.pushKV("resume_order", ToJSON(*snapshot.resume_order));
    if (!snapshot.resume_order_error.empty()) result.pushKV("resume_order_error", snapshot.resume_order_error);
}

UniValue ToJSON(const SessionSnapshot& snapshot)
{
    const auto& session = snapshot.session;
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", session.request_id);
    result.pushKV("session_id", session.session_id.GetHex());
    result.pushKV("canonical_request_hash", session.canonical_request_hash.GetHex());
    if (session.requested_amount.value > 0) {
        result.pushKV("requested_amount_cents", session.requested_amount.value);
        result.pushKV("subtract_paymaster_fee_from_amount",
                      session.subtract_paymaster_fee_from_amount);
        result.pushKV("send_all_spendable_dd",
                      session.send_all_spendable_dd);
    }
    result.pushKV("requested_fee_mode", FeeModeName(session.fee_mode_requested));
    result.pushKV("fee_mode_used", FeeModeName(session.fee_mode_used));
    result.pushKV("session_state", std::string{SessionStateName(session.state)});
    if (session.pending_phase != PendingPhase::NONE) {
        result.pushKV("pending_phase", std::string{PendingPhaseName(session.pending_phase)});
    }
    AddSessionPaymentStatus(result, snapshot);
    result.pushKV("created_at", session.created_at);
    result.pushKV("updated_at", session.updated_at);
    if (!session.final_txid.IsNull()) result.pushKV("txid", session.final_txid.GetHex());
    if (!session.recovery_txid.IsNull()) {
        result.pushKV("recovery_txid", session.recovery_txid.GetHex());
    }

    UniValue inputs{UniValue::VARR};
    for (const auto& input : session.user_inputs) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("txid", input.hash.GetHex());
        entry.pushKV("vout", input.n);
        inputs.push_back(std::move(entry));
    }
    result.pushKV("reserved_user_inputs", std::move(inputs));
    result.pushKV("provider_attempts", static_cast<uint64_t>(session.attempt_ids.size()));

    return result;
}

UniValue AttemptToJSON(const DigiDollar::Paymaster::ProviderAttempt& attempt)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("attempt_id", attempt.attempt_id.GetHex());
    result.pushKV("provider_id", attempt.provider_id.GetHex());
    if (!attempt.provider_endpoint.empty()) {
        result.pushKV("provider_endpoint", attempt.provider_endpoint);
    }
    result.pushKV("attempt_state", std::string{AttemptStateName(attempt.state)});
    result.pushKV("privacy_profile",
                  attempt.privacy_profile == PrivacyProfile::HIGH
                      ? "high" : "standard");
    if (!attempt.quote_id.IsNull()) result.pushKV("quote_id", attempt.quote_id.GetHex());
    if (!attempt.template_commitment.IsNull()) {
        result.pushKV("template_commitment", attempt.template_commitment.GetHex());
    }
    if (!attempt.unsigned_txid.IsNull()) result.pushKV("unsigned_txid", attempt.unsigned_txid.GetHex());
    if (!attempt.commit_key.IsNull()) result.pushKV("commit_key", attempt.commit_key.GetHex());
    if (attempt.quote_expires_at > 0) result.pushKV("quote_expires_at", attempt.quote_expires_at);
    if (attempt.retry_until > 0) result.pushKV("retry_until", attempt.retry_until);
    if (!attempt.client_manifest.manifest_id.IsNull()) {
        result.pushKV("authorization_commitment",
                      attempt.client_manifest.manifest_id.GetHex());
        result.pushKV("authorization_accepted",
                      attempt.accepted_client_manifest_id ==
                              attempt.client_manifest.manifest_id &&
                          attempt.client_manifest_accepted_at > 0);
    }
    if (attempt.client_manifest_accepted_at > 0) {
        result.pushKV("authorization_accepted_at",
                      attempt.client_manifest_accepted_at);
    }
    return result;
}

UniValue AlternativeRecoveryToJSON(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    result.pushKV("recovery_id", recovery.recovery_id.GetHex());
    result.pushKV("recovery_provider_id",
                  recovery.recovery_provider_id.GetHex());
    result.pushKV("offer_id", recovery.offer_id.GetHex());
    result.pushKV("policy_hash", recovery.policy_hash.GetHex());
    result.pushKV("original_commit_key",
                  recovery.original_commit_key.GetHex());
    result.pushKV("original_template_commitment",
                  recovery.original_template_commitment.GetHex());
    result.pushKV("privacy_profile",
                  recovery.privacy_profile == PrivacyProfile::HIGH ? "high" : "standard");
    result.pushKV("phase", AlternativeRecoveryPhaseName(recovery.phase));
    result.pushKV("expired", recovery.expired);
    UniValue user_inputs{UniValue::VARR};
    for (const COutPoint& outpoint : recovery.recovery_request.user_dd_inputs) {
        UniValue input{UniValue::VOBJ};
        input.pushKV("txid", outpoint.hash.GetHex());
        input.pushKV("vout", outpoint.n);
        user_inputs.push_back(std::move(input));
    }
    result.pushKV("user_dd_inputs", std::move(user_inputs));
    UniValue wallet_returns{UniValue::VARR};
    for (const AlternativeRecoveryReturn& wallet_return :
         recovery.recovery_request.wallet_returns) {
        UniValue output{UniValue::VOBJ};
        output.pushKV("script_pub_key", HexStr(wallet_return.script_pub_key));
        CTxDestination destination;
        if (ExtractDestination(wallet_return.script_pub_key, destination)) {
            output.pushKV("address", EncodeDestination(destination));
        }
        output.pushKV("amount_cents", wallet_return.amount.value);
        wallet_returns.push_back(std::move(output));
    }
    result.pushKV("wallet_returns", std::move(wallet_returns));
    if (!recovery.capacity_snapshot.snapshot_id.IsNull()) {
        result.pushKV("capacity_snapshot_id",
                      recovery.capacity_snapshot.snapshot_id.GetHex());
        result.pushKV("capacity_resource_commitment",
                      recovery.capacity_snapshot.resource_commitment.GetHex());
    }
    if (!recovery.recovery_authorization.authorization_commitment.IsNull()) {
        result.pushKV(
            "authorization_commitment",
            recovery.recovery_authorization.authorization_commitment.GetHex());
        result.pushKV(
            "authorization_accepted",
            !recovery.provider_side &&
                recovery.accepted_recovery_authorization_commitment ==
                    recovery.recovery_authorization.authorization_commitment &&
                recovery.recovery_authorization_accepted_at > 0);
        result.pushKV("maximum_service_fee_cents",
                      recovery.recovery_authorization.maximum_service_fee.value);
        result.pushKV("service_fee_cents",
                      recovery.recovery_authorization.service_fee.value);
        result.pushKV("network_fee_satoshis",
                      recovery.recovery_authorization.network_fee.value);
    } else {
        result.pushKV("maximum_service_fee_cents",
                      recovery.selected_maximum_service_fee.value);
        result.pushKV("service_fee_cents",
                      recovery.selected_service_fee.value);
    }
    if (recovery.recovery_authorization_accepted_at > 0) {
        result.pushKV("authorization_accepted_at",
                      recovery.recovery_authorization_accepted_at);
    }
    if (!recovery.final_transaction.empty()) {
        result.pushKV("raw_transaction", HexStr(recovery.final_transaction));
        if (recovery.signed_result.txid) {
            result.pushKV("txid", recovery.signed_result.txid->GetHex());
        }
        result.pushKV("wtxid", recovery.expected_wtxid.GetHex());
    }
    result.pushKV("expires_at",
                  recovery.phase >= AlternativeRecoveryPhase::RESPONSE_VALIDATED ? recovery.recovery_response.expires_at : recovery.capacity_request.expires_at);
    return result;
}

void AddSessionPaymentStatus(UniValue& result, const PaymentSession& session, const PaymasterStore* store)
{
    AddSessionPaymentStatus(result, CaptureSessionSnapshot(session, store));
}
UniValue SessionToJSON(const PaymentSession& session, const PaymasterStore* store)
{
    const auto captured = CaptureSessionPayment(session, store);
    UniValue result = ToJSON(captured);
    if (captured.session) AddResumeOrder(result, *captured.session);
    return result;
}
UniValue ToJSON(const PaymentResult& value)
{
    UniValue result = value.session ? ToJSON(*value.session) : UniValue{UniValue::VOBJ};
    if (value.request_id) result.pushKV("request_id", *value.request_id);
    if (value.to_address) result.pushKV("to_address", *value.to_address);
    if (value.broadcast_error) result.pushKV("broadcast_error", *value.broadcast_error);
    if (value.session_id) result.pushKV("session_id", value.session_id->GetHex());
    if (value.provider_id) result.pushKV("provider_id", value.provider_id->GetHex());
    if (value.attempt_id) result.pushKV("attempt_id", value.attempt_id->GetHex());
    if (value.offer_id) result.pushKV("offer_id", value.offer_id->GetHex());
    if (value.policy_hash) result.pushKV("policy_hash", value.policy_hash->GetHex());
    if (value.txid) result.pushKV("txid", value.txid->GetHex());
    if (value.quote_id) result.pushKV("quote_id", value.quote_id->GetHex());
    if (value.unsigned_txid) result.pushKV("unsigned_txid", value.unsigned_txid->GetHex());
    if (value.template_commitment) result.pushKV("template_commitment", value.template_commitment->GetHex());
    if (value.authorization_commitment) result.pushKV("authorization_commitment", value.authorization_commitment->GetHex());
    if (value.capacity_snapshot_id) result.pushKV("capacity_snapshot_id", value.capacity_snapshot_id->GetHex());
    if (value.payment_cents) result.pushKV("payment_cents", value.payment_cents->value);
    if (value.service_fee_cents) result.pushKV("service_fee_cents", value.service_fee_cents->value);
    if (value.user_total_cents) result.pushKV("user_total_cents", value.user_total_cents->value);
    if (value.requested_amount_cents) result.pushKV("requested_amount_cents", value.requested_amount_cents->value);
    if (value.processed) result.pushKV("processed", *value.processed);
    if (value.broadcast) result.pushKV("broadcast", *value.broadcast);
    if (value.queued) result.pushKV("queued", *value.queued);
    if (value.connection_pending) result.pushKV("connection_pending", *value.connection_pending);
    if (value.capacity_pending) result.pushKV("capacity_pending", *value.capacity_pending);
    if (value.route_available) result.pushKV("route_available", *value.route_available);
    if (value.authorization_required) result.pushKV("authorization_required", *value.authorization_required);
    if (value.authorization_accepted) result.pushKV("authorization_accepted", *value.authorization_accepted);
    if (value.subtract_paymaster_fee_from_amount) result.pushKV("subtract_paymaster_fee_from_amount", *value.subtract_paymaster_fee_from_amount);
    if (value.send_all_spendable_dd) result.pushKV("send_all_spendable_dd", *value.send_all_spendable_dd);
    if (value.expires_at) result.pushKV("expires_at", *value.expires_at);
    if (value.authorization_accepted_at) result.pushKV("authorization_accepted_at", *value.authorization_accepted_at);
    if (value.result_sequence) result.pushKV("result_sequence", *value.result_sequence);
    if (value.requested_fee_mode) result.pushKV("requested_fee_mode", FeeModeName(*value.requested_fee_mode));
    if (value.fee_mode_used) result.pushKV("fee_mode_used", FeeModeName(*value.fee_mode_used));
    if (value.privacy_profile) result.pushKV("privacy_profile", *value.privacy_profile == PrivacyProfile::HIGH ? "high" : "standard");
    if (value.funding_model) result.pushKV("funding_model", *value.funding_model == FundingModel::SPONSORED ? "sponsored" : "user_paid");
    if (value.session_state) result.pushKV("session_state", std::string{SessionStateName(*value.session_state)} );
    if (value.attempt_state) result.pushKV("attempt_state", std::string{AttemptStateName(*value.attempt_state)} );
    if (value.result_status) result.pushKV("result_status", ResultStatusName(*value.result_status));
    if (value.transport) result.pushKV("transport", ToJSON(*value.transport));
    if (value.psbt) result.pushKV("psbt", EncodeBase64(*value.psbt));
    if (value.final_transaction) result.pushKV("hex", HexStr(*value.final_transaction));
    return result;
}

UniValue ToJSON(const ProviderProcessingResult& value)
{
    PaymentResult payment;
    payment.processed = value.IsProcessed();
    payment.queued = value.queued;
    payment.broadcast = value.broadcast;
    payment.broadcast_error = value.broadcast_error;
    payment.request_id = value.request_id;
    payment.session_id = value.session_id;
    payment.provider_id = value.provider_id;
    payment.attempt_id = value.attempt_id;
    payment.quote_id = value.quote_id;
    payment.template_commitment = value.template_commitment;
    payment.capacity_snapshot_id = value.capacity_snapshot_id;
    payment.txid = value.txid;
    payment.attempt_state = value.attempt_state;
    payment.result_status = value.result_status;
    UniValue result = ToJSON(payment);
    if (value.IsRejected()) result.pushKV("rejected", true);
    if (value.IsDeferred()) result.pushKV("deferred", true);
    if (value.message_type) result.pushKV("message_type", *value.message_type);
    if (value.IsRejected() && value.rejection_reason) result.pushKV("rejection_reason", *value.rejection_reason);
    if (value.rejection_code) result.pushKV("rejection_code", *value.rejection_code);
    if (value.IsDeferred() && value.deferral_reason) result.pushKV("deferral_reason", *value.deferral_reason);
    if (value.commit_key) result.pushKV("commit_key", value.commit_key->GetHex());
    if (value.client_nonce) result.pushKV("client_nonce", value.client_nonce->GetHex());
    if (value.recovery_id) result.pushKV("recovery_id", value.recovery_id->GetHex());
    if (value.recovery) result.pushKV("recovery", AlternativeRecoveryToJSON(*value.recovery));
    if (value.commit) result.pushKV("commit", ToJSON(*value.commit));
    return result;
}
UniValue AllowedActionsToJSON(const ClientSessionView& view)
{
    UniValue actions{UniValue::VARR};
    for (const std::string& action : view.allowed_actions) {
        actions.push_back(action);
    }
    return actions;
}
UniValue ToJSON(const ClientRecoveryResult& value)
{
    const auto& view = value.view;
    UniValue result{UniValue::VOBJ};
    result.pushKV("action", value.action);
    UniValue session_result = ToJSON(value.session);
    if (value.session.session) AddResumeOrder(session_result, *value.session.session);
    result.pushKV("session", std::move(session_result));
    if (view.attempt) {
        result.pushKV("attempt", AttemptToJSON(*view.attempt));
    } else {
        result.pushKV("attempt", UniValue{UniValue::VNULL});
    }
    result.pushKV("artifact", view.artifact);
    if (view.recovery) {
        result.pushKV("recovery", AlternativeRecoveryToJSON(*view.recovery));
    } else {
        result.pushKV("recovery", UniValue{UniValue::VNULL});
    }
    if (view.provider_result) {
        result.pushKV("result_status",
                      ResultStatusName(view.provider_result->status));
        result.pushKV("result_sequence",
                      view.provider_result->result_sequence);
    } else {
        result.pushKV("result_status", UniValue{UniValue::VNULL});
        result.pushKV("result_sequence", UniValue{UniValue::VNULL});
    }
    result.pushKV("requires_attention", view.requires_attention);
    result.pushKV("allowed_actions", AllowedActionsToJSON(view));
    result.pushKV("observed_at", view.observed_at);
    if (value.queued) result.pushKV("queued", *value.queued);
    if (value.connection_pending) result.pushKV("connection_pending", *value.connection_pending);
    if (value.route_available) result.pushKV("route_available", *value.route_available);
    if (value.broadcast) result.pushKV("broadcast", *value.broadcast);
    if (value.broadcast_error) result.pushKV("broadcast_error", *value.broadcast_error);
    if (value.txid) result.pushKV("txid", value.txid->GetHex());
    if (value.psbt) result.pushKV("psbt", EncodeBase64(*value.psbt));
    if (value.raw_transaction) result.pushKV("raw_transaction", HexStr(*value.raw_transaction));
    return result;
}
UniValue ClientSessionSummaryToJSON(const PaymasterStore& store,
                                    const PaymentSession& session,
                                    const ClientSessionView& view)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("request_id", session.request_id);
    result.pushKV("session_id", session.session_id.GetHex());
    if (session.requested_amount.value > 0) {
        result.pushKV("requested_amount_cents", session.requested_amount.value);
    }
    result.pushKV("requested_fee_mode", FeeModeName(session.fee_mode_requested));
    result.pushKV("fee_mode_used", FeeModeName(session.fee_mode_used));
    result.pushKV("session_state", std::string{SessionStateName(session.state)});
    if (session.pending_phase != PendingPhase::NONE) {
        result.pushKV("pending_phase", std::string{PendingPhaseName(session.pending_phase)});
    }
    result.pushKV("created_at", session.created_at);
    result.pushKV("updated_at", session.updated_at);
    result.pushKV("provider_attempts",
                  static_cast<uint64_t>(session.attempt_ids.size()));
    if (!session.final_txid.IsNull()) {
        result.pushKV("txid", session.final_txid.GetHex());
    }
    if (!session.recovery_txid.IsNull()) {
        result.pushKV("recovery_txid", session.recovery_txid.GetHex());
    }
    AddSessionPaymentStatus(result, session, &store);
    result.pushKV("artifact", view.artifact);
    result.pushKV("requires_attention", view.requires_attention);
    result.pushKV("allowed_actions", AllowedActionsToJSON(view));
    result.pushKV("observed_at", view.observed_at);
    return result;
}

UniValue ClientSessionSnapshotToJSON(PaymasterStore& store, const PaymentSession& session, std::string_view action)
{
    return ToJSON(CaptureClientSession(store, session, action));
}
void AddPreparationDiagnostic(UniValue& item, const ProviderMaintenanceRecord& record)
{
    const std::string error = record.state == ProviderMaintenanceState::CONFIRMED ? "" : record.preparation_error;
    const auto separator = error.find(':');
    // Keep the existing stable fee-limit token in error. Optional diagnostic
    // text carries amounts/phase without changing the journal format or caps.
    if (error.rfind("PAYMASTER_POOL_FEE_", 0) == 0 && separator != std::string::npos) {
        item.pushKV("error", error.substr(0, separator));
        item.pushKV("diagnostic", error.substr(separator + 2));
    } else {
        item.pushKV("error", error);
    }
}
void AddPreparationFunding(UniValue& item, const PreparationFunding& value)
{
    if (value.required_output_satoshis) item.pushKV("required_output_satoshis", *value.required_output_satoshis);
    if (value.confirmed_available_satoshis) item.pushKV("confirmed_available_satoshis", *value.confirmed_available_satoshis);
    if (value.available_including_unconfirmed_satoshis) item.pushKV("available_including_unconfirmed_satoshis", *value.available_including_unconfirmed_satoshis);
    if (value.capital_shortfall_satoshis) item.pushKV("capital_shortfall_satoshis", *value.capital_shortfall_satoshis);
    if (value.funding_shortfall_upper_bound_satoshis) item.pushKV("funding_shortfall_upper_bound_satoshis", *value.funding_shortfall_upper_bound_satoshis);
    if (value.required_output_cents) item.pushKV("required_output_cents", value.required_output_cents->value);
    if (value.confirmed_available_cents) item.pushKV("confirmed_available_cents", value.confirmed_available_cents->value);
    if (value.pending_cents) item.pushKV("pending_cents", value.pending_cents->value);
    if (value.capital_shortfall_cents) item.pushKV("capital_shortfall_cents", value.capital_shortfall_cents->value);
}
void AddPreparationFunding(UniValue& item, CWallet& wallet, const ProviderMaintenanceRecord& record)
{
    AddPreparationFunding(item, CapturePreparationFunding(wallet, record));
}
UniValue PoolPreparationToJSON(CWallet& wallet)
{
    return ToJSON(CapturePoolPreparation(wallet));
}
UniValue ToJSON(const std::vector<PreparationSnapshot>& values)
{
    UniValue result{UniValue::VARR};
    for (const auto& snapshot : values) {
        const auto& record = snapshot.record;
        if (!record.IsPreparation()) continue;
        UniValue item{UniValue::VOBJ};
        item.pushKV("plan_id", record.plan_id.GetHex());
        item.pushKV("operation_id", record.operation_id.GetHex());
        item.pushKV("asset", record.kind == ProviderMaintenanceKind::PREPARE_DGB ? "dgb" : "dd_carrier");
        item.pushKV("state", record.state == ProviderMaintenanceState::CONFIRMED ? "complete" : record.state == ProviderMaintenanceState::FAILED ? "conflict" : record.state == ProviderMaintenanceState::RELEASED ? "cancelled" : record.transaction_id.IsNull() ? "pending_creation" : "pending_confirmation");
        AddPreparationDiagnostic(item, record);
        item.pushKV("maximum_fee_satoshis", record.maximum_fee.value);
        item.pushKV("actual_fee_satoshis", record.actual_fee.value);
        if (!record.transaction_id.IsNull()) item.pushKV("txid", record.transaction_id.GetHex());
        AddPreparationFunding(item, snapshot.funding);
        result.push_back(std::move(item));
    }
    return result;
}
namespace {
std::string PoolPurposeName(DigiDollar::Paymaster::PoolPurpose purpose)
{
    return purpose == DigiDollar::Paymaster::PoolPurpose::ADMISSION ? "admission" : "operational";
}

std::string PoolAssetName(DigiDollar::Paymaster::PoolAsset asset)
{
    return asset == DigiDollar::Paymaster::PoolAsset::DGB ? "dgb" : "dd_carrier";
}

std::string PoolEntryStateName(DigiDollar::Paymaster::PoolEntryState state)
{
    using DigiDollar::Paymaster::PoolEntryState;
    switch (state) {
    case PoolEntryState::AVAILABLE: return "available";
    case PoolEntryState::RESERVED: return "reserved";
    case PoolEntryState::PENDING_SUCCESSOR: return "pending_successor";
    case PoolEntryState::SPENT: return "spent";
    case PoolEntryState::COMMITTED: return "committed";
    case PoolEntryState::RELEASED: return "released";
    case PoolEntryState::INVALIDATED: return "invalidated";
    }
    return "unknown";
}

} // namespace

UniValue PoolEntryToJSON(const DigiDollar::Paymaster::ProviderPoolEntry& entry)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("txid", entry.outpoint.hash.GetHex());
    result.pushKV("vout", entry.outpoint.n);
    result.pushKV("purpose", PoolPurposeName(entry.purpose));
    result.pushKV("asset", PoolAssetName(entry.asset));
    result.pushKV("state", PoolEntryStateName(entry.state));
    result.pushKV("dgb_satoshis", entry.dgb_value.value);
    result.pushKV("dd_cents", entry.carrier_value.value);
    result.pushKV("confirmation_height", entry.confirmation_height);
    if (!entry.reservation_id.IsNull()) result.pushKV("reservation_id", entry.reservation_id.GetHex());
    if (!entry.origin_commit_key.IsNull()) {
        result.pushKV("origin_commit_key", entry.origin_commit_key.GetHex());
    }
    result.pushKV("updated_at", entry.updated_at);
    return result;
}
UniValue ToJSON(const CarrierWithdrawalResult& value)
{
    UniValue result{UniValue::VOBJ};
    if (value.executed) result.pushKV("executed", *value.executed);
    if (value.plan_id) result.pushKV("plan_id", value.plan_id->GetHex());
    if (value.mode) result.pushKV("mode", *value.mode == CarrierWithdrawalMode::ALL_EXCESS ? "all_excess" : "release_slot");
    if (value.source_carriers) result.pushKV("source_carriers", *value.source_carriers);
    if (value.expires_at) result.pushKV("expires_at", *value.expires_at);
    if (value.withdrawable_excess_cents) result.pushKV("withdrawable_excess_cents", value.withdrawable_excess_cents->value);
    if (value.retained_carrier_cents) result.pushKV("retained_carrier_cents", value.retained_carrier_cents->value);
    if (value.estimated_network_fee_satoshis) result.pushKV("estimated_network_fee_satoshis", *value.estimated_network_fee_satoshis);
    if (value.operational_carrier_target) result.pushKV("operational_carrier_target", *value.operational_carrier_target);
    if (value.txid) result.pushKV("txid", value.txid->GetHex());
    return result;
}
UniValue ToJSON(const PoolPreparationResult& value)
{
    UniValue result{UniValue::VOBJ};
    if (value.plan_id) result.pushKV("plan_id", value.plan_id->GetHex());
    if (value.preview_only) result.pushKV("preview_only", *value.preview_only);
    if (value.admission_dgb_slots) result.pushKV("admission_dgb_slots", *value.admission_dgb_slots);
    if (value.operational_dgb_slots) result.pushKV("operational_dgb_slots", *value.operational_dgb_slots);
    if (value.admission_carrier_slots) result.pushKV("admission_carrier_slots", *value.admission_carrier_slots);
    if (value.operational_carrier_slots) result.pushKV("operational_carrier_slots", *value.operational_carrier_slots);
    if (value.missing_admission_dgb_slots) result.pushKV("missing_admission_dgb_slots", *value.missing_admission_dgb_slots);
    if (value.missing_operational_dgb_slots) result.pushKV("missing_operational_dgb_slots", *value.missing_operational_dgb_slots);
    if (value.missing_admission_carrier_slots) result.pushKV("missing_admission_carrier_slots", *value.missing_admission_carrier_slots);
    if (value.missing_operational_carrier_slots) result.pushKV("missing_operational_carrier_slots", *value.missing_operational_carrier_slots);
    if (value.admission_dgb_satoshis_each) result.pushKV("admission_dgb_satoshis_each", *value.admission_dgb_satoshis_each);
    if (value.operational_dgb_satoshis_each) result.pushKV("operational_dgb_satoshis_each", *value.operational_dgb_satoshis_each);
    if (value.carrier_cents_each) result.pushKV("carrier_cents_each", value.carrier_cents_each->value);
    if (value.total_output_satoshis) result.pushKV("total_output_satoshis", *value.total_output_satoshis);
    if (value.total_carrier_cents) result.pushKV("total_carrier_cents", value.total_carrier_cents->value);
    if (value.maximum_fee_satoshis) result.pushKV("maximum_fee_satoshis", *value.maximum_fee_satoshis);
    if (value.maximum_total_fee_satoshis) result.pushKV("maximum_total_fee_satoshis", *value.maximum_total_fee_satoshis);
    if (value.accepted) result.pushKV("accepted", *value.accepted);
    if (value.cancelled) result.pushKV("cancelled", *value.cancelled);
    if (value.preparation) result.pushKV("preparation", ToJSON(*value.preparation));
    if (value.executed) result.pushKV("executed", *value.executed);
    if (value.dgb_txid) result.pushKV("dgb_txid", value.dgb_txid->GetHex());
    if (value.txid) result.pushKV("txid", value.txid->GetHex());
    if (value.network_fee_satoshis) result.pushKV("network_fee_satoshis", *value.network_fee_satoshis);
    if (value.dd_txid) result.pushKV("dd_txid", value.dd_txid->GetHex());
    if (value.pool) {
        UniValue pool{UniValue::VARR};
        for (const auto& entry : *value.pool) pool.push_back(PoolEntryToJSON(entry));
        result.pushKV("pool", std::move(pool));
    }
    return result;
}
UniValue ToJSON(const PoolRebalanceResult& value)
{
    UniValue result{UniValue::VOBJ};
    if (value.plan_id) result.pushKV("plan_id", value.plan_id->GetHex());
    if (value.retired_admission_dgb_slots) result.pushKV("retired_admission_dgb_slots", *value.retired_admission_dgb_slots);
    if (value.retired_operational_dgb_slots) result.pushKV("retired_operational_dgb_slots", *value.retired_operational_dgb_slots);
    if (value.retired_admission_carrier_slots) result.pushKV("retired_admission_carrier_slots", *value.retired_admission_carrier_slots);
    if (value.retired_operational_carrier_slots) result.pushKV("retired_operational_carrier_slots", *value.retired_operational_carrier_slots);
    if (value.retired_dgb_satoshis) result.pushKV("retired_dgb_satoshis", *value.retired_dgb_satoshis);
    if (value.retired_carrier_cents) result.pushKV("retired_carrier_cents", value.retired_carrier_cents->value);
    if (value.estimated_dgb_fee_satoshis) result.pushKV("estimated_dgb_fee_satoshis", *value.estimated_dgb_fee_satoshis);
    if (value.estimated_dd_fee_satoshis) result.pushKV("estimated_dd_fee_satoshis", *value.estimated_dd_fee_satoshis);
    if (value.wallet_maximum_fee_satoshis) result.pushKV("wallet_maximum_fee_satoshis", *value.wallet_maximum_fee_satoshis);
    if (value.maximum_network_fee_satoshis) result.pushKV("maximum_network_fee_satoshis", *value.maximum_network_fee_satoshis);
    if (value.maximum_total_fee_satoshis) result.pushKV("maximum_total_fee_satoshis", *value.maximum_total_fee_satoshis);
    if (value.executed) result.pushKV("executed", *value.executed);
    if (value.dd_txid) result.pushKV("dd_txid", value.dd_txid->GetHex());
    if (value.dd_network_fee_satoshis) result.pushKV("dd_network_fee_satoshis", *value.dd_network_fee_satoshis);
    if (value.dgb_txid) result.pushKV("dgb_txid", value.dgb_txid->GetHex());
    if (value.network_fee_satoshis) result.pushKV("network_fee_satoshis", *value.network_fee_satoshis);
    if (value.pool) {
        UniValue pool{UniValue::VARR};
        for (const auto& entry : *value.pool) pool.push_back(PoolEntryToJSON(entry));
        result.pushKV("pool", std::move(pool));
    }
    return result;
}
} // namespace wallet::paymaster_rpc::internal
