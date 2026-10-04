// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <paymaster/provider.h>

#include <consensus/amount.h>
#include <paymaster/directory.h>

namespace DigiDollar::Paymaster {
namespace {
constexpr size_t MAX_BUDGET_LEDGER_ENTRIES{8192};
constexpr size_t MAX_QUOTE_REQUEST_EVENTS{8192};
bool LimitsDisabled(const FundingSafetyLimits& limits)
{
    return limits.maximum_network_fee_per_transaction.value == 0 &&
           limits.maximum_reserved_network_fee.value == 0 &&
           limits.maximum_network_fee_per_hour.value == 0 &&
           limits.maximum_network_fee_per_day.value == 0 &&
           limits.maximum_completed_per_hour == 0 &&
           limits.maximum_completed_per_day == 0;
}
} // namespace

bool ValidateProviderFundingSafetyLimits(const FundingSafetyLimits& limits,
                                 bool required,
                                 DGBSatoshis advertised_maximum,
                                 std::string& error)
{
    if (LimitsDisabled(limits)) {
        if (required) error = "PAYMASTER_SAFETY_LIMITS_REQUIRED";
        return !required;
    }
    if (limits.maximum_network_fee_per_transaction.value <= 0 ||
        !MoneyRange(limits.maximum_network_fee_per_transaction.value) ||
        !MoneyRange(limits.maximum_reserved_network_fee.value) ||
        !MoneyRange(limits.maximum_network_fee_per_hour.value) ||
        !MoneyRange(limits.maximum_network_fee_per_day.value) ||
        limits.maximum_network_fee_per_transaction.value > advertised_maximum.value ||
        limits.maximum_reserved_network_fee.value <
            limits.maximum_network_fee_per_transaction.value ||
        limits.maximum_network_fee_per_hour.value <
            limits.maximum_network_fee_per_transaction.value ||
        limits.maximum_network_fee_per_day.value <
            limits.maximum_network_fee_per_hour.value ||
        limits.maximum_completed_per_hour == 0 ||
        limits.maximum_completed_per_day < limits.maximum_completed_per_hour) {
        error = "PAYMASTER_INVALID_SAFETY_LIMITS";
        return false;
    }
    return true;
}

bool ValidateProviderSafetyQuoteLimits(const ProviderSafetyPolicy& safety, std::string& error)
{
    if (safety.version != ProviderSafetyPolicy::CURRENT_VERSION || safety.updated_at <= 0) {
        error = "PAYMASTER_INVALID_SAFETY_POLICY";
        return false;
    }
    if (safety.maximum_active_quotes_total == 0 ||
        safety.maximum_active_quotes_total > MAX_BUDGET_LEDGER_ENTRIES ||
        safety.maximum_active_quotes_per_netgroup == 0 ||
        safety.maximum_active_quotes_per_recipient == 0 ||
        safety.maximum_quote_requests_per_netgroup_per_minute == 0 ||
        safety.maximum_quote_requests_per_netgroup_per_minute > MAX_QUOTE_REQUEST_EVENTS ||
        safety.maximum_active_quotes_per_netgroup > safety.maximum_active_quotes_total ||
        safety.maximum_active_quotes_per_recipient > safety.maximum_active_quotes_total) {
        error = "PAYMASTER_INVALID_SAFETY_QUOTE_LIMITS";
        return false;
    }
    return true;
}

bool PolicyAllowsFundingModel(const ProviderPolicy& policy, FundingModel model)
{
    if (model != FundingModel::USER_PAID && model != FundingModel::SPONSORED) return false;
    const uint8_t bit{static_cast<uint8_t>(1U << static_cast<uint8_t>(model))};
    return (policy.funding_models & bit) != 0;
}

bool ValidateProviderPolicy(const ProviderPolicy& policy, std::string& error)
{
    error.clear();
    if ((policy.version != ProviderPolicy::LEGACY_VERSION &&
         policy.version != ProviderPolicy::CURRENT_VERSION) ||
        policy.funding_models == 0 ||
        (policy.funding_models & ~FUNDING_MODEL_ALL) != 0) {
        error = "PAYMASTER_INVALID_FUNDING_MODELS";
        return false;
    }
    if (policy.sponsorship_scope != SponsorshipScope::PUBLIC &&
        policy.sponsorship_scope != SponsorshipScope::RESTRICTED) {
        error = "PAYMASTER_INVALID_SPONSORSHIP_SCOPE";
        return false;
    }
    if (policy.sponsorship_scope == SponsorshipScope::RESTRICTED &&
        (policy.funding_models != FUNDING_MODEL_SPONSORED ||
         policy.fee_rate_bps != 0 ||
         policy.maximum_user_paid_service_fee.value != 0)) {
        error = "PAYMASTER_INVALID_RESTRICTED_POLICY";
        return false;
    }
    if (!PolicyAllowsFundingModel(policy, FundingModel::USER_PAID) &&
        (policy.fee_rate_bps != 0 ||
         policy.maximum_user_paid_service_fee.value != 0)) {
        error = "PAYMASTER_SPONSORED_FEE";
        return false;
    }
    if (policy.fee_rate_bps > MAX_RATE_BPS || policy.fee_rate_bps % 10 != 0) {
        error = "PAYMASTER_INVALID_RATE";
        return false;
    }
    if (policy.min_payment.value < 100 || policy.max_payment.value > MAX_DD_OUTPUT_CENTS ||
        policy.min_payment.value > policy.max_payment.value) {
        error = "PAYMASTER_INVALID_PAYMENT_RANGE";
        return false;
    }
    if (policy.maximum_user_paid_service_fee.value < 0 ||
        policy.maximum_user_paid_service_fee.value > MAX_DD_OUTPUT_CENTS ||
        (policy.version == ProviderPolicy::LEGACY_VERSION &&
         policy.maximum_user_paid_service_fee.value != 0)) {
        error = "PAYMASTER_INVALID_SERVICE_FEE_CAP";
        return false;
    }
    if (policy.quote_ttl <= 0 || policy.quote_ttl > MAX_QUOTE_TTL_SECONDS) {
        error = "PAYMASTER_INVALID_QUOTE_TTL";
        return false;
    }
    if (policy.maximum_network_fee.value <= 0 ||
        !MoneyRange(policy.maximum_network_fee.value)) {
        error = "PAYMASTER_INVALID_NETWORK_FEE_CAP";
        return false;
    }
    return true;
}

bool ValidateProviderSafetyPolicy(const ProviderSafetyPolicy& safety,
                                  const ProviderPolicy& advertised,
                                  std::string& error)
{
    error.clear();
    std::string policy_error;
    if (!ValidateProviderPolicy(advertised, policy_error)) {
        error = policy_error.empty() ? "PAYMASTER_INVALID_SAFETY_POLICY" : policy_error;
        return false;
    }
    if (!ValidateProviderSafetyQuoteLimits(safety, error)) return false;
    const bool user_paid = PolicyAllowsFundingModel(advertised, FundingModel::USER_PAID);
    const bool sponsored = PolicyAllowsFundingModel(advertised, FundingModel::SPONSORED);
    const bool public_sponsored = sponsored &&
                                  advertised.sponsorship_scope == SponsorshipScope::PUBLIC;
    const bool restricted_sponsored = sponsored &&
                                      advertised.sponsorship_scope == SponsorshipScope::RESTRICTED;
    if (!ValidateProviderFundingSafetyLimits(safety.user_paid, user_paid,
                                     advertised.maximum_network_fee, error) ||
        !ValidateProviderFundingSafetyLimits(safety.public_sponsored, public_sponsored,
                                     advertised.maximum_network_fee, error) ||
        !ValidateProviderFundingSafetyLimits(safety.restricted_sponsored, restricted_sponsored,
                                     advertised.maximum_network_fee, error)) {
        return false;
    }
    return true;
}

bool ValidateProviderLiquidityPolicy(const ProviderLiquidityPolicy& policy,
                                     std::string& error)
{
    error.clear();
    const bool carrier_targets_valid =
        (policy.target_admission_carriers == 0
             ? policy.target_operational_carriers == 0
             : policy.target_admission_carriers >= 3) &&
        policy.target_operational_carriers <= 16;
    const bool finite_limits =
        policy.maximum_maintenance_fee_per_transaction.value > 0 &&
        policy.maximum_maintenance_fee_per_hour.value >=
            policy.maximum_maintenance_fee_per_transaction.value &&
        policy.maximum_maintenance_fee_per_day.value >=
            policy.maximum_maintenance_fee_per_hour.value;
    if (policy.version != ProviderLiquidityPolicy::CURRENT_VERSION ||
        policy.updated_at <= 0 ||
        policy.target_admission_dgb < 3 ||
        policy.target_operational_dgb < 1 ||
        policy.target_admission_dgb > 16 ||
        policy.target_operational_dgb > 16 ||
        policy.target_admission_carriers > 16 ||
        policy.target_operational_carriers > 16 ||
        !carrier_targets_valid ||
        !MoneyRange(policy.maximum_maintenance_fee_per_transaction.value) ||
        !MoneyRange(policy.maximum_maintenance_fee_per_hour.value) ||
        !MoneyRange(policy.maximum_maintenance_fee_per_day.value) ||
        (policy.paid_maintenance_approved && !finite_limits)) {
        error = "PAYMASTER_INVALID_LIQUIDITY_POLICY";
        return false;
    }
    return true;
}

bool ProviderLiquidityTargetsSatisfyPolicy(
    const ProviderLiquidityPolicy& liquidity_policy,
    const ProviderPolicy& provider_policy)
{
    std::string error;
    if (!ValidateProviderLiquidityPolicy(liquidity_policy, error) ||
        !ValidateProviderPolicy(provider_policy, error)) {
        return false;
    }

    // The generic liquidity-policy format permits zero carrier targets so an
    // operator can deliberately release the last carrier while retaining a
    // USER_PAID offer for later use. That is a valid stored state, but it
    // cannot restore provider readiness until the operator raises the targets
    // again. Keep this policy-dependent invariant separate from serialization
    // validation so sponsored-only providers still require no DD carriers.
    if (PolicyAllowsFundingModel(provider_policy, FundingModel::USER_PAID)) {
        return liquidity_policy.target_admission_carriers >=
                   REQUIRED_ADMISSION_SLOTS &&
               liquidity_policy.target_operational_carriers >= 1;
    }
    return true;
}
} // namespace DigiDollar::Paymaster
