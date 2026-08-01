// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_PAYMASTERCONFIRMATION_H
#define DIGIBYTE_QT_PAYMASTERCONFIRMATION_H

#include <QString>
#include <QStringList>

#include <limits>

/** Return true only for a response that identifies an exact transaction and
 * reports a Core-validated completion state. This check deliberately does not
 * depend on the response echo of the pre-signing commitment: Core has already
 * enforced that durable manifest before returning either Paymaster result. */
inline bool IsValidatedPaymasterCompletion(bool has_txid,
                                           const QString& session_state,
                                           const QString& direct_status,
                                           const QString& result_status,
                                           bool final)
{
    if (!has_txid) return false;
    const bool direct_success = session_state.isEmpty() &&
        direct_status == QStringLiteral("success");
    const bool result_state_is_final =
        session_state == QStringLiteral("MEMPOOL") ||
        session_state == QStringLiteral("CONFIRMED");
    const bool validated_result = result_state_is_final &&
        (result_status == QStringLiteral("final_committed") ||
         result_status == QStringLiteral("broadcast_attempted"));
    return direct_success || validated_result || final;
}

/** The exact user-visible fields that must be reviewed before a Paymaster
 * authorization. Core remains authoritative and independently validates its
 * persisted authorization manifest; this value only controls the Qt prompt. */
struct PaymasterConfirmationSelection {
    QString provider_id;
    QString offer_id;
    QString policy_hash;
    QString funding_model;
    QString recipient;
    QString authorization_commitment;
    qint64 payment_cents{-1};
    qint64 service_fee_cents{-1};
    qint64 user_total_cents{-1};

    bool IsComplete() const
    {
        const bool known_model = funding_model == QStringLiteral("user_paid") ||
                                 funding_model == QStringLiteral("sponsored");
        const bool addition_is_safe = payment_cents >= 0 && service_fee_cents >= 0 &&
            payment_cents <= std::numeric_limits<qint64>::max() - service_fee_cents;
        return !provider_id.isEmpty() && !offer_id.isEmpty() &&
               !policy_hash.isEmpty() && known_model && !recipient.isEmpty() &&
               !authorization_commitment.isEmpty() && addition_is_safe &&
               user_total_cents == payment_cents + service_fee_cents &&
               (funding_model != QStringLiteral("sponsored") ||
                service_fee_cents == 0);
    }

    friend bool operator==(const PaymasterConfirmationSelection& lhs,
                           const PaymasterConfirmationSelection& rhs)
    {
        return lhs.provider_id == rhs.provider_id &&
               lhs.offer_id == rhs.offer_id &&
               lhs.policy_hash == rhs.policy_hash &&
               lhs.funding_model == rhs.funding_model &&
               lhs.recipient == rhs.recipient &&
               lhs.authorization_commitment == rhs.authorization_commitment &&
               lhs.payment_cents == rhs.payment_cents &&
               lhs.service_fee_cents == rhs.service_fee_cents &&
               lhs.user_total_cents == rhs.user_total_cents;
    }
};

/** Stateful, deterministic prompt guard shared by initial authorization,
 * retries and provider fallback. An incomplete candidate always requires a
 * fresh decision but can never be recorded as accepted. */
class PaymasterConfirmationGuard
{
public:
    bool RequiresConfirmation(const PaymasterConfirmationSelection& candidate) const
    {
        return !candidate.IsComplete() || !m_have_accepted ||
               !(candidate == m_accepted);
    }

    bool Accept(const PaymasterConfirmationSelection& candidate)
    {
        if (!candidate.IsComplete()) return false;
        m_accepted = candidate;
        m_have_accepted = true;
        return true;
    }

    void Reset()
    {
        m_accepted = {};
        m_have_accepted = false;
    }

    bool HasAcceptedSelection() const { return m_have_accepted; }

    QStringList ChangedFields(const PaymasterConfirmationSelection& candidate) const
    {
        if (!candidate.IsComplete()) return {QStringLiteral("incomplete")};
        if (!m_have_accepted) {
            return {QStringLiteral("provider"), QStringLiteral("offer"),
                    QStringLiteral("policy"), QStringLiteral("funding_model"),
                    QStringLiteral("recipient"), QStringLiteral("amount"),
                    QStringLiteral("service_fee"),
                    QStringLiteral("total"),
                    QStringLiteral("authorization_commitment")};
        }
        QStringList changed;
        if (candidate.provider_id != m_accepted.provider_id) changed.push_back(QStringLiteral("provider"));
        if (candidate.offer_id != m_accepted.offer_id) changed.push_back(QStringLiteral("offer"));
        if (candidate.policy_hash != m_accepted.policy_hash) changed.push_back(QStringLiteral("policy"));
        if (candidate.funding_model != m_accepted.funding_model) changed.push_back(QStringLiteral("funding_model"));
        if (candidate.recipient != m_accepted.recipient) changed.push_back(QStringLiteral("recipient"));
        if (candidate.payment_cents != m_accepted.payment_cents) changed.push_back(QStringLiteral("amount"));
        if (candidate.service_fee_cents != m_accepted.service_fee_cents) changed.push_back(QStringLiteral("service_fee"));
        if (candidate.user_total_cents != m_accepted.user_total_cents) changed.push_back(QStringLiteral("total"));
        if (candidate.authorization_commitment != m_accepted.authorization_commitment) {
            changed.push_back(QStringLiteral("authorization_commitment"));
        }
        return changed;
    }

private:
    PaymasterConfirmationSelection m_accepted;
    bool m_have_accepted{false};
};

/** Exact user-visible fields returned from Core's validated alternative
 * recovery authorization manifest. Qt deliberately treats every value as
 * opaque presentation data: construction and validation of the manifest stay
 * in the wallet RPC implementation. The ordered wallet return descriptions
 * must come from that manifest, never from UI-side transaction inference. */
struct PaymasterRecoveryConfirmationSelection {
    QString recovery_provider_id;
    QString privacy_profile;
    QString offer_id;
    QString policy_hash;
    QString original_commit_key;
    QString original_template_commitment;
    QStringList wallet_returns;
    QString authorization_commitment;
    qint64 maximum_service_fee_cents{-1};
    qint64 service_fee_cents{-1};
    qint64 network_fee_satoshis{-1};
    qint64 expires_at{-1};

    static bool WalletReturnsEqual(const QStringList& lhs,
                                   const QStringList& rhs)
    {
        if (lhs.size() != rhs.size()) return false;
        for (int index = 0; index < lhs.size(); ++index) {
            if (lhs.at(index) != rhs.at(index)) return false;
        }
        return true;
    }

    bool IsComplete() const
    {
        if (recovery_provider_id.isEmpty() || privacy_profile.isEmpty() ||
            offer_id.isEmpty() || policy_hash.isEmpty() ||
            original_commit_key.isEmpty() ||
            original_template_commitment.isEmpty() || wallet_returns.isEmpty() ||
            authorization_commitment.isEmpty() ||
            maximum_service_fee_cents < 0 || service_fee_cents < 0 ||
            network_fee_satoshis < 0 || expires_at <= 0) {
            return false;
        }
        for (const QString& wallet_return : wallet_returns) {
            if (wallet_return.isEmpty()) return false;
        }
        return true;
    }

    friend bool operator==(const PaymasterRecoveryConfirmationSelection& lhs,
                           const PaymasterRecoveryConfirmationSelection& rhs)
    {
        return lhs.recovery_provider_id == rhs.recovery_provider_id &&
               lhs.privacy_profile == rhs.privacy_profile &&
               lhs.offer_id == rhs.offer_id &&
               lhs.policy_hash == rhs.policy_hash &&
               lhs.original_commit_key == rhs.original_commit_key &&
               lhs.original_template_commitment == rhs.original_template_commitment &&
               WalletReturnsEqual(lhs.wallet_returns, rhs.wallet_returns) &&
               lhs.authorization_commitment == rhs.authorization_commitment &&
               lhs.maximum_service_fee_cents == rhs.maximum_service_fee_cents &&
               lhs.service_fee_cents == rhs.service_fee_cents &&
               lhs.network_fee_satoshis == rhs.network_fee_satoshis &&
               lhs.expires_at == rhs.expires_at;
    }
};

/** Deterministic prompt guard for the second stage of alternative-provider
 * cancel-to-self recovery. A changed or incomplete Core manifest can never
 * inherit a previous user acceptance. */
class PaymasterRecoveryConfirmationGuard
{
public:
    bool RequiresConfirmation(
        const PaymasterRecoveryConfirmationSelection& candidate) const
    {
        return !candidate.IsComplete() || !m_have_accepted ||
               !(candidate == m_accepted);
    }

    bool Accept(const PaymasterRecoveryConfirmationSelection& candidate)
    {
        if (!candidate.IsComplete()) return false;
        m_accepted = candidate;
        m_have_accepted = true;
        return true;
    }

    void Reset()
    {
        m_accepted = {};
        m_have_accepted = false;
    }

    bool HasAcceptedSelection() const { return m_have_accepted; }

    QStringList ChangedFields(
        const PaymasterRecoveryConfirmationSelection& candidate) const
    {
        if (!candidate.IsComplete()) return {QStringLiteral("incomplete")};
        if (!m_have_accepted) {
            return {QStringLiteral("recovery_provider"),
                    QStringLiteral("privacy"), QStringLiteral("offer"),
                    QStringLiteral("policy"),
                    QStringLiteral("original_commit"),
                    QStringLiteral("original_template_commitment"),
                    QStringLiteral("wallet_returns"),
                    QStringLiteral("maximum_service_fee"),
                    QStringLiteral("service_fee"),
                    QStringLiteral("network_fee"), QStringLiteral("expiry"),
                    QStringLiteral("authorization_commitment")};
        }

        QStringList changed;
        if (candidate.recovery_provider_id != m_accepted.recovery_provider_id) {
            changed.push_back(QStringLiteral("recovery_provider"));
        }
        if (candidate.privacy_profile != m_accepted.privacy_profile) {
            changed.push_back(QStringLiteral("privacy"));
        }
        if (candidate.offer_id != m_accepted.offer_id) {
            changed.push_back(QStringLiteral("offer"));
        }
        if (candidate.policy_hash != m_accepted.policy_hash) {
            changed.push_back(QStringLiteral("policy"));
        }
        if (candidate.original_commit_key != m_accepted.original_commit_key) {
            changed.push_back(QStringLiteral("original_commit"));
        }
        if (candidate.original_template_commitment !=
            m_accepted.original_template_commitment) {
            changed.push_back(QStringLiteral("original_template_commitment"));
        }
        if (!PaymasterRecoveryConfirmationSelection::WalletReturnsEqual(
                candidate.wallet_returns, m_accepted.wallet_returns)) {
            changed.push_back(QStringLiteral("wallet_returns"));
        }
        if (candidate.maximum_service_fee_cents !=
            m_accepted.maximum_service_fee_cents) {
            changed.push_back(QStringLiteral("maximum_service_fee"));
        }
        if (candidate.service_fee_cents != m_accepted.service_fee_cents) {
            changed.push_back(QStringLiteral("service_fee"));
        }
        if (candidate.network_fee_satoshis != m_accepted.network_fee_satoshis) {
            changed.push_back(QStringLiteral("network_fee"));
        }
        if (candidate.expires_at != m_accepted.expires_at) {
            changed.push_back(QStringLiteral("expiry"));
        }
        if (candidate.authorization_commitment !=
            m_accepted.authorization_commitment) {
            changed.push_back(QStringLiteral("authorization_commitment"));
        }
        return changed;
    }

private:
    PaymasterRecoveryConfirmationSelection m_accepted;
    bool m_have_accepted{false};
};

#endif // DIGIBYTE_QT_PAYMASTERCONFIRMATION_H
