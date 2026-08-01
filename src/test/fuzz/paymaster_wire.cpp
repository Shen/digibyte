// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Paymaster wire-format and envelope-validation fuzzer.
 *
 * It exercises decoding, canonical reserialization, size bounds, and signature
 * domain separation independently of wallet state. Successfully decoded bytes
 * must have one canonical meaning; malformed or trailing data must never be
 * accepted as a different message on retry.
 */

#include <paymaster/directory.h>
#include <paymaster/provider.h>
#include <paymaster/recovery.h>
#include <paymaster/wire.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <version.h>

#include <cassert>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using namespace DigiDollar::Paymaster;

namespace {

void initialize_paymaster_wire()
{
    assert(DigiDollar::Paymaster::PROTOCOL_VERSION == 5);
    assert(PaymasterCapacityRequest{}.version == 5);
    assert(PaymentIntent::CURRENT_VERSION == 2);
    assert(AlternativeRecoveryRequest::CURRENT_VERSION == 2);
    assert(AlternativeRecoveryResultMessage::CURRENT_VERSION == 2);
}

template <typename Message>
std::vector<unsigned char> SerializeCanonical(const Message& message)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << message;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

template <typename Message>
void AssertCanonicalRoundTrip(const Message& message)
{
    const std::vector<unsigned char> encoded = SerializeCanonical(message);
    CDataStream stream{encoded, SER_NETWORK, ::PROTOCOL_VERSION};
    Message decoded;
    stream >> decoded;
    assert(stream.empty());
    assert(SerializeCanonical(decoded) == encoded);
}

void ParseHardenedV4Record(uint8_t message_type,
                           const std::vector<uint8_t>& payload,
                           int64_t now)
{
    CDataStream stream{payload, SER_NETWORK, ::PROTOCOL_VERSION};
    std::string error;
    try {
        switch (message_type) {
        case 0: {
            AlternativeRecoveryRequest message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateAlternativeRecoveryRequestEnvelope(
                message, message.genesis_hash, now, error);
            (void)GetAlternativeRecoveryRequestHash(message);
            break;
        }
        case 1: {
            AlternativeRecoveryResponse message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateAlternativeRecoveryResponseEnvelope(
                message, message.genesis_hash, now, error);
            (void)GetAlternativeRecoveryCommitKey(message);
            break;
        }
        case 2: {
            AlternativeRecoverySubmit message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateAlternativeRecoverySubmitEnvelope(
                message, message.genesis_hash, error);
            break;
        }
        case 3: {
            AlternativeRecoveryResultMessage message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateAlternativeRecoveryResultEnvelope(
                message, message.result.genesis_hash, now, error);
            break;
        }
        case 4: {
            ClientAuthorizationManifest manifest;
            stream >> manifest;
            AssertCanonicalRoundTrip(manifest);
            (void)GetClientAuthorizationManifestId(manifest);
            break;
        }
        case 5: {
            ProviderAuthorizationManifest manifest;
            stream >> manifest;
            AssertCanonicalRoundTrip(manifest);
            (void)GetProviderAuthorizationManifestId(manifest);
            break;
        }
        case 6: {
            ProviderBudgetLedger ledger;
            stream >> ledger;
            AssertCanonicalRoundTrip(ledger);
            (void)ValidateProviderBudgetLedger(ledger, error);
            break;
        }
        case 7: {
            ClientFeeLedger ledger;
            stream >> ledger;
            AssertCanonicalRoundTrip(ledger);
            (void)ValidateClientFeeLedger(ledger, error);
            break;
        }
        }
    } catch (const std::exception&) {
        // Truncated, non-canonical and fail-closed enum inputs are expected.
    }
}

} // namespace

FUZZ_TARGET(paymaster_wire_envelopes, .init = initialize_paymaster_wire)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    const uint8_t message_type = provider.ConsumeIntegralInRange<uint8_t>(0, 7);
    const int64_t now = provider.ConsumeIntegral<int64_t>();
    const bool allow_local_endpoint = provider.ConsumeBool();
    const std::vector<uint8_t> payload = provider.ConsumeRemainingBytes<uint8_t>();
    CDataStream stream{payload, SER_NETWORK, ::PROTOCOL_VERSION};
    std::string error;

    try {
        switch (message_type) {
        case 0: {
            Announcement message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateAnnouncementEnvelope(message, message.genesis_hash, now, error,
                                               allow_local_endpoint);
            break;
        }
        case 1: {
            PaymasterCapacityRequest message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateCapacityRequestEnvelope(message, message.genesis_hash, now, error);
            break;
        }
        case 2: {
            PaymasterCapacityProof message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateCapacityProofEnvelope(message, message.genesis_hash, now, error);
            break;
        }
        case 3: {
            PaymasterQuoteRequest message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateQuoteRequestEnvelope(message, message.intent.genesis_hash, now, error);
            break;
        }
        case 4: {
            PaymasterQuoteResponse message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateQuoteResponseEnvelope(message, message.quote.genesis_hash, now, error);
            break;
        }
        case 5: {
            PaymasterSubmit message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateSubmitEnvelope(message, message.genesis_hash, error);
            break;
        }
        case 6: {
            PaymasterResultMessage message;
            stream >> message;
            AssertCanonicalRoundTrip(message);
            (void)ValidateResultMessageEnvelope(message, message.result.genesis_hash, now, error);
            break;
        }
        case 7: {
            PaymasterCapacityRequest request;
            PaymasterCapacityProof proof;
            stream >> request >> proof;
            AssertCanonicalRoundTrip(request);
            AssertCanonicalRoundTrip(proof);
            (void)ValidateCapacityProofEnvelope(proof, request, now, error);
            break;
        }
        }
    } catch (const std::exception&) {
        // Truncated and non-canonical serialization is expected fuzz input.
    }

    // Keep the 0..7 selector and payload consumption exactly as before so
    // existing qa-assets corpus entries retain their original behavior. The
    // same bytes additionally exercise v4 durable authorities, budgets and
    // the alternative-recovery protocol.
    ParseHardenedV4Record(message_type, payload, now);
}
