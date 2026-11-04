// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTERSENDCONTROLLER_H
#define DIGIBYTE_QT_PAYMASTERSENDCONTROLLER_H
#include <qt/paymasterasync.h>
#include <QStringList>
namespace PaymasterQt {
struct PaymasterSessionSnapshot {
    bool authoritative{false};
    bool reported_final{false};
    bool unsigned_closed{false};
    bool offer_ready_for_review{false};
    QString request_id;
    QString session_id;
    QString state;
    QString pending_phase;
    QString broadcast_state;
    QString confirmation_state;
    QString provider_id;
    QString policy_hash;
    QString artifact;
    QString attempt_state;
    QString recipient;
    QString privacy_profile;
    UniValue restored_options{UniValue::VOBJ};
    QString transaction_id;
    QString recovery_transaction_id;
    QString result_status;
    qint64 result_sequence{-1};
    qint64 recovery_expires_at{-1};
    qint64 created_at{0};
    QStringList allowed_actions;
    qint64 payment_cents{-1};
    qint64 service_fee_cents{-1};
    qint64 total_cents{-1};
    qint64 expires_at{-1};
    qint64 requested_amount_cents{-1};
    bool has_costs{false};
    bool has_expiry{false};
    bool allowed_actions_known{false};
};

bool ReadInt64(const UniValue& object, const char* key, qint64& value);
bool IsKnownSessionState(const QString& state);
bool IsTerminalPaymasterState(const QString& state);

/** Pure decisions shared by the send view and tests. Backend snapshots own
 * permissions; neither diagnostic prose nor an error code grants execution. */
class SendController {
public:
    struct FailureDecision {
        bool invalidate_review{false};
        bool discard_uncreated_request{false};
        bool allow_unsigned_cancel{false};
    };
    static bool decodeSnapshot(const UniValue& result, PaymasterSessionSnapshot& snapshot, QString& error);
    static FailureDecision failure(const RpcError& error, bool persisted, bool known_unsigned);
};
} // namespace PaymasterQt
#endif // DIGIBYTE_QT_PAYMASTERSENDCONTROLLER_H
