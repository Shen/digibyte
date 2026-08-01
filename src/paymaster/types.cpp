// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Strong amount helpers and canonical user-facing identifiers for Paymaster.
 * All fee arithmetic is integer-only, overflow checked, and rounded in the
 * provider's favor exactly once so every later authorization layer agrees.
 */

#include <paymaster/types.h>

#include <limits>

namespace DigiDollar::Paymaster {

std::optional<DDCents> ComputePaymasterFee(DDCents payment, uint32_t fee_rate_bps)
{
    if (payment.value <= 0 || payment.value > MAX_DD_OUTPUT_CENTS ||
        fee_rate_bps > MAX_RATE_BPS || fee_rate_bps % 10 != 0) {
        return std::nullopt;
    }

    if (fee_rate_bps != 0 && payment.value > std::numeric_limits<int64_t>::max() / fee_rate_bps) {
        return std::nullopt;
    }
    const int64_t numerator = payment.value * fee_rate_bps;
    if (numerator > std::numeric_limits<int64_t>::max() - 9999) return std::nullopt;
    const int64_t rounded = (numerator + 9999) / 10000;
    if (rounded > MAX_DD_OUTPUT_CENTS - payment.value) return std::nullopt;
    return DDCents{rounded};
}

std::optional<DDCents> ComputePaymasterPaymentFromGross(
    DDCents gross_amount,
    uint32_t fee_rate_bps)
{
    if (gross_amount.value <= 0 || gross_amount.value > MAX_DD_OUTPUT_CENTS ||
        fee_rate_bps > MAX_RATE_BPS || fee_rate_bps % 10 != 0) {
        return std::nullopt;
    }

    // payment + ceil(payment * rate / 10000) is strictly increasing for
    // positive integer-cent payments, so a binary search is exact and cannot
    // accidentally choose a best-effort amount that leaves DD behind.
    int64_t low{1};
    int64_t high{gross_amount.value};
    while (low <= high) {
        const int64_t payment = low + (high - low) / 2;
        const auto fee = ComputePaymasterFee(DDCents{payment}, fee_rate_bps);
        // Inputs and rate were prevalidated above. In this bounded domain a
        // null fee at a probe can only mean payment + fee exceeds the global
        // DD-output ceiling, so the exact solution (if any) is lower.
        if (!fee) {
            high = payment - 1;
            continue;
        }
        const int64_t total = payment + fee->value;
        if (total == gross_amount.value) return DDCents{payment};
        if (total < gross_amount.value) {
            low = payment + 1;
        } else {
            high = payment - 1;
        }
    }
    return std::nullopt;
}

bool IsCanonicalRequestId(std::string_view request_id)
{
    if (request_id.size() != 36) return false;
    for (size_t i = 0; i < request_id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (request_id[i] != '-') return false;
            continue;
        }
        const char c = request_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool IsValidPaymasterDisplayName(std::string_view display_name)
{
    if (display_name.size() > 32) return false;
    for (const unsigned char c : display_name) {
        if (c < 0x20 || c > 0x7e || c == '/' || c == '@') return false;
    }
    return true;
}

} // namespace DigiDollar::Paymaster
