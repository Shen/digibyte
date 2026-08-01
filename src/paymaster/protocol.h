// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Signed protocol records and commitments for the Paymaster lifecycle. */

#ifndef DIGIBYTE_PAYMASTER_PROTOCOL_H
#define DIGIBYTE_PAYMASTER_PROTOCOL_H

#include <paymaster/provider.h>
#include <paymaster/psbt.h>
#include <paymaster/types.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <ios>
#include <optional>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

struct OfferTerms;
struct PaymasterQuoteRequest;

static constexpr size_t MAX_PAYMENT_INTENT_INPUTS{64};

template <typename T>
struct OptionalFieldFormatter {
    template <typename Stream>
    void Ser(Stream& stream, const std::optional<T>& value)
    {
        const uint8_t present{static_cast<uint8_t>(value.has_value())};
        Serialize(stream, present);
        if (present) Serialize(stream, *value);
    }

    template <typename Stream>
    void Unser(Stream& stream, std::optional<T>& value)
    {
        uint8_t present;
        Unserialize(stream, present);
        if (present > 1) throw std::ios_base::failure("Non-canonical optional field");
        if (!present) {
            value.reset();
            return;
        }
        T decoded;
        Unserialize(stream, decoded);
        value = std::move(decoded);
    }
};

struct UserInputControlProof {
    COutPoint outpoint;
    std::vector<unsigned char> signature;

    SERIALIZE_METHODS(UserInputControlProof, obj) { READWRITE(obj.outpoint, obj.signature); }
};

struct PaymentIntent {
    static constexpr uint16_t CURRENT_VERSION{2};
    static constexpr uint16_t LEGACY_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    std::string request_id;
    uint256 session_id;
    uint256 client_nonce;
    std::vector<COutPoint> user_dd_inputs;
    CScript recipient_script;
    DDCents recipient_amount;
    CScript user_dd_change_script;
    uint256 offer_id;
    FundingModel funding_model{FundingModel::SPONSORED};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    uint256 policy_hash;
    uint256 sponsorship_authorization_hash;
    int64_t expires_at{0};
    std::vector<UserInputControlProof> user_input_proofs;
    /** Exact local order that selected this provider and offer. These fields
     * are part of the core hash and therefore every user input-control proof,
     * provider quote, PSBT template, and authorization manifest. */
    uint256 canonical_request_hash;
    FeeMode requested_fee_mode{FeeMode::PAYMASTER};
    PrivacyProfile privacy_profile{PrivacyProfile::STANDARD};
    SelectionMode selection_mode{SelectionMode::LOWEST_TOTAL_COST};

    SERIALIZE_METHODS(PaymentIntent, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.request_id,
                  obj.session_id, obj.client_nonce, obj.user_dd_inputs,
                  obj.recipient_script, obj.recipient_amount,
                  obj.user_dd_change_script, obj.offer_id,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
                  obj.policy_hash, obj.sponsorship_authorization_hash,
                  obj.expires_at, obj.user_input_proofs);
        if (obj.version >= 2) {
            READWRITE(obj.canonical_request_hash,
                      Using<EnumByteFormatter<static_cast<uint8_t>(FeeMode::AUTO)>>(obj.requested_fee_mode),
                      Using<EnumByteFormatter<static_cast<uint8_t>(PrivacyProfile::HIGH)>>(obj.privacy_profile),
                      Using<EnumByteFormatter<static_cast<uint8_t>(SelectionMode::PRIVACY_WEIGHTED)>>(obj.selection_mode));
        }
    }
};

struct VerifiedDGBInput {
    COutPoint outpoint;
    CMutableTransaction creating_tx;
    DGBSatoshis value;

    SERIALIZE_METHODS(VerifiedDGBInput, obj) { READWRITE(obj.outpoint, obj.creating_tx, obj.value); }
};

struct VerifiedDDCarrier {
    COutPoint outpoint;
    CMutableTransaction creating_tx;
    DDCents value;

    SERIALIZE_METHODS(VerifiedDDCarrier, obj) { READWRITE(obj.outpoint, obj.creating_tx, obj.value); }
};

struct PaymasterQuote {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    uint256 quote_id;
    uint256 intent_hash;
    uint256 offer_id;
    uint256 policy_hash;
    FundingModel funding_model{FundingModel::SPONSORED};
    SponsorshipScope sponsorship_scope{SponsorshipScope::PUBLIC};
    uint256 sponsorship_authorization_hash;
    uint32_t fee_rate_bps{0};
    DDCents service_fee;
    std::optional<VerifiedDDCarrier> reserved_carrier;
    std::vector<VerifiedDGBInput> reserved_dgb_inputs;
    std::optional<CScript> carrier_return_script;
    std::optional<CScript> provider_fee_script;
    std::optional<CScript> dgb_change_script;
    DGBSatoshis network_fee;
    int64_t created_at{0};
    int64_t expires_at{0};
    int64_t retry_until{0};
    CMutableTransaction unsigned_transaction;
    uint256 unsigned_txid;
    uint256 template_commitment;
    std::vector<unsigned char> identity_signature;

    SERIALIZE_METHODS(PaymasterQuote, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.quote_id,
                  obj.intent_hash, obj.offer_id, obj.policy_hash,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.sponsorship_scope),
                  obj.sponsorship_authorization_hash, obj.fee_rate_bps,
                  obj.service_fee,
                  Using<OptionalFieldFormatter<VerifiedDDCarrier>>(obj.reserved_carrier),
                  obj.reserved_dgb_inputs,
                  Using<OptionalFieldFormatter<CScript>>(obj.carrier_return_script),
                  Using<OptionalFieldFormatter<CScript>>(obj.provider_fee_script),
                  Using<OptionalFieldFormatter<CScript>>(obj.dgb_change_script),
                  obj.network_fee, obj.created_at, obj.expires_at, obj.retry_until,
                  obj.unsigned_transaction, obj.unsigned_txid,
                  obj.template_commitment, obj.identity_signature);
    }
};

struct ProviderQuoteBuildParameters {
    uint256 quote_id;
    std::vector<CollaborativeInput> user_inputs;
    std::optional<VerifiedDDCarrier> reserved_carrier;
    std::vector<VerifiedDGBInput> reserved_dgb_inputs;
    std::optional<CScript> carrier_return_script;
    std::optional<CScript> provider_fee_script;
    std::optional<CScript> dgb_change_script;
    DGBSatoshis network_fee;
    int64_t created_at{0};
    int64_t expires_at{0};
    int64_t retry_until{0};
};

struct ProviderQuoteBuildResult {
    PaymasterQuote quote;
    CollaborativePSBTTemplate trusted_template;
};

/** Wallet-independent validation that can run before deriving any provider
 * output destination. It validates the request's exact USER_DD inputs against
 * chainstate together with the intent, envelope, control proofs, and service
 * fee that do not depend on provider resources or output scripts. */
struct ProviderQuotePreflightResult {
    ValidatedUserDDInputs user_inputs;
    ServiceFeePlan fee_plan;
    CAmount user_change{0};
};

bool PreflightProviderQuoteRequest(
    const PaymasterQuoteRequest& request,
    const ProviderPolicy& policy,
    const std::vector<CollaborativeInput>& user_inputs,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    int64_t now,
    ProviderQuotePreflightResult& result,
    std::string& error);

/** Deterministically construct the unsigned quote and exact PSBT template
 * from fully resolved inputs. No wallet key is accepted or used here; the
 * caller signs quote.identity_signature only after durable reservation. */
bool BuildUnsignedProviderQuote(
    const PaymasterQuoteRequest& request,
    const ProviderPolicy& policy,
    const ProviderQuoteBuildParameters& parameters,
    const CChainParams& chain_params,
    const CCoinsViewCache& coins,
    ProviderQuoteBuildResult& result,
    std::string& error);

enum class PaymasterResultStatus : uint8_t {
    NO_FINAL_COMMIT,
    USER_PSBT_ACCEPTED,
    FINAL_COMMITTED,
    BROADCAST_ATTEMPTED,
    SLOT_UNAVAILABLE,
    REJECTED,
};

struct PaymasterResult {
    static constexpr uint16_t CURRENT_VERSION{1};

    uint16_t version{CURRENT_VERSION};
    uint256 genesis_hash;
    PaymasterId provider_id;
    uint256 commit_key;
    uint64_t result_sequence{0};
    PaymasterResultStatus status{PaymasterResultStatus::NO_FINAL_COMMIT};
    std::optional<uint256> txid;
    std::optional<uint256> raw_transaction_hash;
    std::optional<CMutableTransaction> final_transaction;
    int64_t updated_at{0};
    std::vector<unsigned char> identity_signature;

    SERIALIZE_METHODS(PaymasterResult, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.provider_id, obj.commit_key,
                  obj.result_sequence,
                  Using<EnumByteFormatter<static_cast<uint8_t>(PaymasterResultStatus::REJECTED)>>(obj.status),
                  Using<OptionalFieldFormatter<uint256>>(obj.txid),
                  Using<OptionalFieldFormatter<uint256>>(obj.raw_transaction_hash),
                  Using<OptionalFieldFormatter<CMutableTransaction>>(obj.final_transaction),
                  obj.updated_at, obj.identity_signature);
    }
};

uint256 GetPaymentIntentCoreHash(const PaymentIntent& intent);
uint256 GetPaymentIntentHash(const PaymentIntent& intent);
uint256 GetUserInputControlHash(const PaymentIntent& intent, const COutPoint& outpoint);
uint256 GetPaymasterQuoteSignatureHash(const PaymasterQuote& quote);
uint256 GetPaymasterResultSignatureHash(const PaymasterResult& result);
uint256 GetPaymasterCommitKey(const PaymasterId& provider_id,
                              const uint256& client_nonce,
                              const uint256& intent_hash,
                              const uint256& quote_id,
                              const uint256& template_commitment);

bool ValidatePaymentIntent(const PaymentIntent& intent,
                           const uint256& expected_genesis,
                           int64_t now,
                           const std::vector<XOnlyPubKey>& user_output_keys,
                           std::string& error);
bool ValidatePaymasterQuote(const PaymasterQuote& quote,
                            const PaymentIntent& intent,
                            const ProviderPolicy& policy,
                            const XOnlyPubKey& provider_identity_key,
                            DDCents maximum_service_fee,
                            int64_t now,
                            std::string& error);
bool ValidatePaymasterQuoteForClient(const PaymasterQuote& quote,
                                     const PaymentIntent& intent,
                                     const OfferTerms& offer,
                                     const XOnlyPubKey& provider_identity_key,
                                     DDCents maximum_service_fee,
                                     int64_t now,
                                     std::string& error);
bool ValidatePaymasterResult(const PaymasterResult& result,
                             const uint256& expected_genesis,
                             const PaymasterId& expected_provider,
                             const uint256& expected_commit_key,
                             const XOnlyPubKey& provider_identity_key,
                             uint64_t minimum_sequence,
                             std::string& error);
bool ValidatePaymasterResultShape(const PaymasterResult& result, std::string& error);

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_PROTOCOL_H
