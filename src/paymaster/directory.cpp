// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * In-memory directory of validated provider announcements.
 * Replacement is monotonic by provider sequence and expiry; this component
 * never turns an announcement into spending authority for a wallet.
 */

#include <paymaster/directory.h>

#include <hash.h>

#include <algorithm>
#include <set>

namespace DigiDollar::Paymaster {

namespace {

std::set<COutPoint> GetAdmissionOutpoints(const Announcement& announcement)
{
    const bool needs_carriers{std::any_of(
        announcement.offers.begin(), announcement.offers.end(),
        [](const OfferTerms& offer) { return offer.funding_model == FundingModel::USER_PAID; })};
    std::set<COutPoint> outpoints;
    for (const AdmissionSlotProof& slot : announcement.admission_slots) {
        outpoints.insert(slot.dgb_outpoint);
        if (needs_carriers) outpoints.insert(slot.carrier_outpoint);
    }
    return outpoints;
}

} // namespace

PaymasterId GetPaymasterId(const XOnlyPubKey& identity_key)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Id v1");
    hasher << identity_key;
    return hasher.GetSHA256();
}

uint256 GetAnnouncementSignatureHash(const Announcement& announcement)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Announcement v1");
    hasher << announcement.version << announcement.genesis_hash << announcement.identity_key
           << announcement.display_name << announcement.sequence << announcement.created_at
           << announcement.expires_at << WithParams(CNetAddr::V2, announcement.endpoint)
           << announcement.offers
           << announcement.min_confirmations << announcement.capability_flags
           << static_cast<uint64_t>(announcement.admission_slots.size());
    for (const AdmissionSlotProof& slot : announcement.admission_slots) {
        // A creating transaction's txid commits all non-witness fields. Admission
        // validation additionally matches that transaction and output to chainstate.
        hasher << slot.dgb_outpoint << slot.dgb_creating_tx.GetHash() << slot.dgb_value
               << slot.dgb_control_signature << slot.carrier_outpoint
               << slot.carrier_creating_tx.GetHash() << slot.carrier_value
               << slot.carrier_control_signature << slot.reference_block << slot.expires_at;
    }
    return hasher.GetSHA256();
}

bool ValidateAnnouncementEnvelope(const Announcement& announcement,
                                  const uint256& expected_genesis,
                                  int64_t now,
                                  std::string& error,
                                  bool allow_local_endpoint)
{
    if (announcement.version != PROTOCOL_VERSION || announcement.genesis_hash != expected_genesis) {
        error = "PAYMASTER_WRONG_PROTOCOL_OR_CHAIN";
        return false;
    }
    if (!announcement.identity_key.IsFullyValid() || announcement.sequence == 0) {
        error = "PAYMASTER_INVALID_IDENTITY";
        return false;
    }
    if (!IsValidPaymasterDisplayName(announcement.display_name)) {
        error = "PAYMASTER_INVALID_DISPLAY_NAME";
        return false;
    }
    if (TimeDeltaExceeds(announcement.created_at, now, 60) || announcement.expires_at <= now ||
        announcement.expires_at <= announcement.created_at ||
        TimeDeltaExceeds(announcement.expires_at, announcement.created_at,
                         ANNOUNCEMENT_TTL_SECONDS)) {
        error = "PAYMASTER_INVALID_ANNOUNCEMENT_TIME";
        return false;
    }
    if (!announcement.endpoint.IsValid() ||
        (!announcement.endpoint.IsRoutable() &&
         !(allow_local_endpoint && announcement.endpoint.IsLocal()))) {
        error = "PAYMASTER_UNROUTABLE_ENDPOINT";
        return false;
    }
    if (announcement.offers.empty() || announcement.offers.size() > MAX_ANNOUNCEMENT_OFFERS ||
        announcement.admission_slots.size() != REQUIRED_ADMISSION_SLOTS) {
        error = "PAYMASTER_INVALID_ANNOUNCEMENT_COUNTS";
        return false;
    }

    bool needs_carriers{false};
    std::set<uint256> offer_ids;
    for (const OfferTerms& offer : announcement.offers) {
        if (offer.offer_id.IsNull() || offer.policy_hash.IsNull() || !offer_ids.insert(offer.offer_id).second ||
            offer.scope != SponsorshipScope::PUBLIC || offer.min_payment.value < 100 ||
            offer.max_payment.value > MAX_DD_OUTPUT_CENTS || offer.min_payment.value > offer.max_payment.value) {
            error = "PAYMASTER_INVALID_OFFER";
            return false;
        }
        if (offer.funding_model == FundingModel::SPONSORED) {
            if (offer.fee_rate_bps != 0) {
                error = "PAYMASTER_SPONSORED_FEE";
                return false;
            }
        } else {
            if (offer.fee_rate_bps > MAX_RATE_BPS || offer.fee_rate_bps % 10 != 0) {
                error = "PAYMASTER_INVALID_RATE";
                return false;
            }
            needs_carriers = true;
        }
    }

    std::set<COutPoint> outpoints;
    for (const AdmissionSlotProof& slot : announcement.admission_slots) {
        if (slot.dgb_outpoint.IsNull() || slot.dgb_value.value < MIN_ADMISSION_DGB_SATOSHIS ||
            slot.reference_block.IsNull() || slot.expires_at < announcement.expires_at ||
            !outpoints.insert(slot.dgb_outpoint).second || slot.dgb_control_signature.size() != 64) {
            error = "PAYMASTER_INVALID_ADMISSION_SLOT";
            return false;
        }
        if (needs_carriers && (slot.carrier_outpoint.IsNull() || slot.carrier_value.value < 100 ||
                               !outpoints.insert(slot.carrier_outpoint).second ||
                               slot.carrier_control_signature.size() != 64)) {
            error = "PAYMASTER_INVALID_ADMISSION_CARRIER";
            return false;
        }
    }

    if (announcement.identity_signature.size() != 64 ||
        !announcement.identity_key.VerifySchnorr(GetAnnouncementSignatureHash(announcement),
                                                 announcement.identity_signature)) {
        error = "PAYMASTER_INVALID_ANNOUNCEMENT_SIGNATURE";
        return false;
    }
    error.clear();
    return true;
}

bool Directory::AcceptsSequence(const PaymasterId& provider_id,
                                uint64_t sequence) const
{
    if (provider_id.IsNull() || sequence == 0) return false;
    LOCK(m_mutex);
    const auto existing = m_announcements.find(provider_id);
    return existing == m_announcements.end() ||
           existing->second.sequence < sequence;
}

bool Directory::HasAdmissionOutpointConflict(const Announcement& announcement,
                                             const PaymasterId& provider_id,
                                             int64_t now) const
{
    const std::set<COutPoint> claimed_outpoints{GetAdmissionOutpoints(announcement)};
    for (const auto& [other_id, other] : m_announcements) {
        if (other_id == provider_id || other.expires_at <= now) continue;
        const std::set<COutPoint> other_outpoints{GetAdmissionOutpoints(other)};
        if (std::any_of(claimed_outpoints.begin(), claimed_outpoints.end(),
                        [&](const COutPoint& outpoint) {
                            return other_outpoints.count(outpoint) != 0;
                        })) {
            return true;
        }
    }
    return false;
}

bool Directory::AcceptsAdmissionOutpoints(const Announcement& announcement,
                                          int64_t now) const
{
    if (announcement.expires_at <= now) return false;
    const PaymasterId provider_id{GetPaymasterId(announcement.identity_key)};
    LOCK(m_mutex);
    return !HasAdmissionOutpointConflict(announcement, provider_id, now);
}

bool Directory::AddValidated(Announcement announcement, int64_t now)
{
    if (announcement.expires_at <= now) return false;
    const PaymasterId id = GetPaymasterId(announcement.identity_key);
    LOCK(m_mutex);
    const auto existing = m_announcements.find(id);
    if (existing != m_announcements.end() && existing->second.sequence >= announcement.sequence) return false;

    // A UTXO controller can sign admission proofs for multiple identity keys.
    // Count each active reserve only once so one pool cannot manufacture many
    // apparent providers or fill the bounded directory. Ignore this provider's
    // prior announcement so a monotonic replacement can retain its own slots.
    if (HasAdmissionOutpointConflict(announcement, id, now)) return false;

    m_announcements[id] = std::move(announcement);
    if (m_announcements.size() > MAX_DIRECTORY_ENTRIES) {
        const auto oldest = std::min_element(m_announcements.begin(), m_announcements.end(),
            [](const auto& a, const auto& b) { return a.second.expires_at < b.second.expires_at; });
        m_announcements.erase(oldest);
    }
    return true;
}

std::vector<Announcement> Directory::List(int64_t now) const
{
    LOCK(m_mutex);
    std::vector<Announcement> result;
    for (const auto& [id, announcement] : m_announcements) {
        if (announcement.expires_at > now) result.push_back(announcement);
    }
    return result;
}

void Directory::RemoveExpired(int64_t now)
{
    LOCK(m_mutex);
    for (auto it = m_announcements.begin(); it != m_announcements.end();) {
        if (it->second.expires_at <= now) it = m_announcements.erase(it);
        else ++it;
    }
}

void Directory::Clear()
{
    LOCK(m_mutex);
    m_announcements.clear();
}

size_t Directory::Size() const
{
    LOCK(m_mutex);
    return m_announcements.size();
}

} // namespace DigiDollar::Paymaster
