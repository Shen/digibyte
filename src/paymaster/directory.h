// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Validated provider-announcement cache used for discovery only. */

#ifndef DIGIBYTE_PAYMASTER_DIRECTORY_H
#define DIGIBYTE_PAYMASTER_DIRECTORY_H

#include <netaddress.h>
#include <paymaster/types.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace DigiDollar::Paymaster {

static constexpr size_t MAX_DIRECTORY_ENTRIES{256};
static constexpr size_t MAX_ANNOUNCEMENT_OFFERS{4};
static constexpr size_t REQUIRED_ADMISSION_SLOTS{3};
static constexpr int64_t MIN_ADMISSION_DGB_SATOSHIS{10000000};

struct OfferTerms {
    uint256 offer_id;
    uint256 policy_hash;
    FundingModel funding_model{FundingModel::SPONSORED};
    SponsorshipScope scope{SponsorshipScope::PUBLIC};
    uint32_t fee_rate_bps{0};
    DDCents min_payment;
    DDCents max_payment;

    SERIALIZE_METHODS(OfferTerms, obj)
    {
        READWRITE(obj.offer_id, obj.policy_hash,
                  Using<EnumByteFormatter<static_cast<uint8_t>(FundingModel::SPONSORED)>>(obj.funding_model),
                  Using<EnumByteFormatter<static_cast<uint8_t>(SponsorshipScope::RESTRICTED)>>(obj.scope),
                  obj.fee_rate_bps, obj.min_payment, obj.max_payment);
    }
};

struct AdmissionSlotProof {
    COutPoint dgb_outpoint;
    CMutableTransaction dgb_creating_tx;
    DGBSatoshis dgb_value;
    std::vector<unsigned char> dgb_control_signature;
    COutPoint carrier_outpoint;
    CMutableTransaction carrier_creating_tx;
    DDCents carrier_value;
    std::vector<unsigned char> carrier_control_signature;
    uint256 reference_block;
    int64_t expires_at{0};

    SERIALIZE_METHODS(AdmissionSlotProof, obj)
    {
        READWRITE(obj.dgb_outpoint, obj.dgb_creating_tx, obj.dgb_value, obj.dgb_control_signature,
                  obj.carrier_outpoint, obj.carrier_creating_tx, obj.carrier_value,
                  obj.carrier_control_signature, obj.reference_block, obj.expires_at);
    }
};

struct Announcement {
    uint16_t version{PROTOCOL_VERSION};
    uint256 genesis_hash;
    XOnlyPubKey identity_key;
    std::string display_name;
    uint64_t sequence{0};
    int64_t created_at{0};
    int64_t expires_at{0};
    CService endpoint;
    std::vector<OfferTerms> offers;
    uint16_t min_confirmations{1};
    uint32_t capability_flags{0};
    std::vector<AdmissionSlotProof> admission_slots;
    std::vector<unsigned char> identity_signature;

    SERIALIZE_METHODS(Announcement, obj)
    {
        READWRITE(obj.version, obj.genesis_hash, obj.identity_key, obj.display_name,
                  obj.sequence, obj.created_at, obj.expires_at,
                  WithParams(CNetAddr::V2, obj.endpoint), obj.offers,
                  obj.min_confirmations, obj.capability_flags, obj.admission_slots,
                  obj.identity_signature);
    }
};

PaymasterId GetPaymasterId(const XOnlyPubKey& identity_key);
uint256 GetAnnouncementSignatureHash(const Announcement& announcement);
bool ValidateAnnouncementEnvelope(const Announcement& announcement,
                                  const uint256& expected_genesis,
                                  int64_t now,
                                  std::string& error,
                                  bool allow_local_endpoint = false);

/** Bounded cache for announcements whose envelope and UTXO proofs have already
 * been validated by the caller. Sequence replacement is monotonic, and active
 * provider identities cannot claim the same admission outpoint.
 */
class Directory {
public:
    /** Cheap monotonicity precheck for a signature-validated announcement.
     * Stale and exact relay duplicates must not consume provider-scoped rate
     * capacity or trigger chainstate proof validation. AddValidated remains
     * authoritative and repeats this check under the same lock. */
    bool AcceptsSequence(const PaymasterId& provider_id, uint64_t sequence) const;
    /** Cheap admission-outpoint conflict precheck before chainstate proof
     * validation. AddValidated repeats it atomically with insertion. */
    bool AcceptsAdmissionOutpoints(const Announcement& announcement, int64_t now) const;
    bool AddValidated(Announcement announcement, int64_t now);
    /** Return at most maximum active announcements, rotating the first entry
     * without copying the remainder of the bounded directory. */
    std::vector<Announcement> List(
        int64_t now,
        size_t maximum = MAX_DIRECTORY_ENTRIES,
        uint64_t rotation = 0) const;
    void RemoveExpired(int64_t now);
    void Clear();
    size_t Size() const;

private:
    bool HasAdmissionOutpointConflict(const Announcement& announcement,
                                      const PaymasterId& provider_id,
                                      int64_t now) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    mutable Mutex m_mutex;
    std::map<PaymasterId, Announcement> m_announcements GUARDED_BY(m_mutex);
};

} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_DIRECTORY_H
