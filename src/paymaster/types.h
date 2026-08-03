// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Strong integer types and bounded arithmetic shared by Paymaster. */

#ifndef DIGIBYTE_PAYMASTER_TYPES_H
#define DIGIBYTE_PAYMASTER_TYPES_H

#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <ios>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace DigiDollar::Paymaster {

// Version 2 makes the provider-authenticated capacity handshake mandatory
// before payment intents or restricted capabilities may be disclosed.
// Version 3 adds the authenticated alternative-provider recovery exchange.
// Version 4 binds the selected funding model and the exact carrier role into
// the pre-intent capacity exchange. A v4 quote may therefore use neither
// fewer nor different provider resources than its validated snapshot.
// Version 5 binds the complete local client order (canonical request,
// requested fee mode, privacy profile, and provider-selection mode) into the
// signed payment intent and every derived authorization artifact.
// Recovery messages are never downgraded to the regular quote flow.
static constexpr uint16_t PROTOCOL_VERSION{5};
/** SENDPMASTERS capability used by the outbound half of a short-lived
 * paymaster connection. The accepting peer uses it to classify its locally
 * INBOUND half as the same isolated direct channel.
 */
static constexpr uint32_t CAP_DIRECT_CONNECTION{1U << 0};
static constexpr uint32_t MAX_RATE_BPS{10000};
static constexpr int64_t MAX_DD_OUTPUT_CENTS{10000000};
static constexpr int64_t ANNOUNCEMENT_TTL_SECONDS{600};
static constexpr int64_t DEFAULT_QUOTE_TTL_SECONDS{60};
static constexpr int64_t DEFAULT_RETRY_SECONDS{24 * 60 * 60};
static constexpr int DEFAULT_REORG_SAFETY_DEPTH{240};

/** Return whether later is more than max_delta seconds after earlier without
 * overflowing for untrusted timestamps near the int64_t limits.
 */
constexpr bool TimeDeltaExceeds(int64_t later, int64_t earlier, int64_t max_delta)
{
    if (later <= earlier) return false;
    if (max_delta < 0) return true;
    return static_cast<uint64_t>(later) - static_cast<uint64_t>(earlier) >
           static_cast<uint64_t>(max_delta);
}

/** Add seconds to a timestamp, clamping at the int64_t limits. */
constexpr int64_t SaturatingAddSeconds(int64_t timestamp, int64_t seconds)
{
    if (seconds > 0 && timestamp > std::numeric_limits<int64_t>::max() - seconds) {
        return std::numeric_limits<int64_t>::max();
    }
    if (seconds < 0 && timestamp < std::numeric_limits<int64_t>::min() - seconds) {
        return std::numeric_limits<int64_t>::min();
    }
    return timestamp + seconds;
}

struct DDCents {
    int64_t value{0};
    SERIALIZE_METHODS(DDCents, obj) { READWRITE(obj.value); }
    friend bool operator==(const DDCents& a, const DDCents& b) { return a.value == b.value; }
    friend bool operator!=(const DDCents& a, const DDCents& b) { return !(a == b); }
};

struct DGBSatoshis {
    int64_t value{0};
    SERIALIZE_METHODS(DGBSatoshis, obj) { READWRITE(obj.value); }
    friend bool operator==(const DGBSatoshis& a, const DGBSatoshis& b) { return a.value == b.value; }
    friend bool operator!=(const DGBSatoshis& a, const DGBSatoshis& b) { return !(a == b); }
};

using PaymasterId = uint256;

enum class FundingModel : uint8_t { USER_PAID, SPONSORED };
enum class SponsorshipScope : uint8_t { PUBLIC, RESTRICTED };
enum class FeeMode : uint8_t { DGB, PAYMASTER, AUTO };
enum class PrivacyProfile : uint8_t { STANDARD, HIGH };
enum class SelectionMode : uint8_t { LOWEST_TOTAL_COST, PRIVACY_WEIGHTED };

/** Stable one-byte enum encoding with fail-closed range validation. */
template <uint8_t MaxValue>
struct EnumByteFormatter {
    template <typename Stream, typename Enum>
    void Ser(Stream& s, const Enum& value)
    {
        static_assert(std::is_enum_v<Enum>);
        const uint8_t encoded{static_cast<uint8_t>(value)};
        if (encoded > MaxValue) throw std::ios_base::failure("Invalid paymaster enum value");
        Serialize(s, encoded);
    }

    template <typename Stream, typename Enum>
    void Unser(Stream& s, Enum& value)
    {
        static_assert(std::is_enum_v<Enum>);
        uint8_t encoded;
        Unserialize(s, encoded);
        if (encoded > MaxValue) throw std::ios_base::failure("Invalid paymaster enum value");
        value = static_cast<Enum>(encoded);
    }
};

/** Return the rounded-up percentage fee, or nullopt for an invalid payment,
 * rate, or arithmetic overflow.
 */
std::optional<DDCents> ComputePaymasterFee(DDCents payment, uint32_t fee_rate_bps);

/** Invert the rounded Paymaster fee formula for a fixed total wallet
 * outflow. Returns the unique recipient payment whose rounded service fee
 * makes payment + fee exactly equal gross_amount, or nullopt when the
 * cent-granular rounding leaves no exact solution. */
std::optional<DDCents> ComputePaymasterPaymentFromGross(
    DDCents gross_amount,
    uint32_t fee_rate_bps);

/** V1 request IDs are lowercase RFC 4122 textual UUIDs (8-4-4-4-12). */
bool IsCanonicalRequestId(std::string_view request_id);
bool IsValidPaymasterDisplayName(std::string_view display_name);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_TYPES_H
