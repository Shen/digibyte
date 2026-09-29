// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_QT_PAYMASTEROPERATION_H
#define DIGIBYTE_QT_PAYMASTEROPERATION_H

#include <univalue.h>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <exception>

/** Presentation/controller state only. Core journals own all financial work.
 * A wallet switch invalidates every outstanding UI approval and reply. */
class PaymasterOperationController
{
public:
    enum class Phase { Idle, Checking, Review, Executing, Waiting, Complete, Blocked };
    Phase phase{Phase::Idle};
    QString title;
    QString error;
    UniValue snapshot;
    UniValue reviewed;
    uint64_t generation{0};
    int64_t received_at{0};
    int completed_confirmations{0};
    int required_confirmations{0};
    bool observed_work{false};

    void reset(uint64_t wallet_generation)
    {
        *this = PaymasterOperationController{};
        generation = wallet_generation;
    }
    bool begin(const QString& name, uint64_t wallet_generation)
    {
        if (phase == Phase::Checking || phase == Phase::Review || phase == Phase::Executing || phase == Phase::Waiting) return false;
        reset(wallet_generation);
        title = name;
        phase = Phase::Checking;
        return true;
    }
    void review(const UniValue& value) { reviewed = value; phase = Phase::Review; }
    void execute() { phase = Phase::Executing; error.clear(); }
    void accepted() { phase = Phase::Waiting; }
    void fail(const QString& reason) { error = reason; phase = Phase::Blocked; }
    bool stale(int64_t now) const { return received_at == 0 || now - received_at > 30; }
    bool busy() const { return phase == Phase::Checking || phase == Phase::Review || phase == Phase::Executing; }

    /** Reject an old wallet's result before touching any visible state. */
    bool observe(const UniValue& value, uint64_t wallet_generation, int64_t now)
    {
        if (wallet_generation != generation || !value.isObject() ||
            !value.find_value("provider").isObject() || !value.find_value("diagnostics").isArray()) return false;
        snapshot = value;
        received_at = now;
        const auto& provider = value.find_value("provider");
        const auto& operations = provider.find_value("active_operations");
        completed_confirmations = required_confirmations = 0;
        bool blocked{false};
        bool needs_creation{false};
        if (operations.isArray()) for (const auto& item : operations.getValues()) {
            if (item.find_value("state").isStr() && item.find_value("state").get_str() == "pending_confirmation" &&
                item.find_value("confirmations").isNum() && item.find_value("required_confirmations").isNum()) {
                const int required = item.find_value("required_confirmations").getInt<int>();
                required_confirmations += std::max(0, required);
                completed_confirmations += std::clamp(item.find_value("confirmations").getInt<int>(), 0, std::max(0, required));
            }
            needs_creation |= item.find_value("state").isStr() && item.find_value("state").get_str() == "pending_creation";
            blocked |= item.find_value("state").isStr() && item.find_value("state").get_str() == "conflict";
            const auto& code = item.find_value("error");
            if (code.isStr() && !code.get_str().empty() &&
                code.get_str() != "PAYMASTER_POOL_WAITING_CONFIRMATION" &&
                code.get_str() != "PAYMASTER_POOL_WAITING_PARENT_CONFIRMATION") {
                blocked = true;
                error = QString::fromStdString(code.get_str());
            }
        }
        if (!busy() && operations.isArray()) {
            if (!operations.empty() || provider.find_value("start_requested").isTrue()) {
                if (phase == Phase::Complete) title.clear();
                observed_work = true;
                const bool needs_processing = needs_creation || provider.find_value("start_requested").isTrue();
                if (needs_processing && provider.find_value("enabled").isFalse()) {
                    blocked = true;
                    error = QStringLiteral("PAYMASTER_PROVIDER_DISABLED");
                } else if (needs_processing && provider.find_value("wallet_locked").isTrue()) {
                    blocked = true;
                    error = QStringLiteral("PAYMASTER_WALLET_LOCKED");
                }
                phase = blocked ? Phase::Blocked : Phase::Waiting;
                if (!blocked) error.clear();
            } else if ((phase == Phase::Waiting || (phase == Phase::Blocked && observed_work)) &&
                       provider.find_value("pool_ready").isTrue()) {
                phase = Phase::Complete;
                error.clear();
            }
        }
        return true;
    }

    /** A post-configuration executable preview may shrink, never expand the
     * previously approved financial scope. No float arithmetic or UI defaults. */
    static bool fundingWithinApproval(const UniValue& approved, const UniValue& current)
    {
        try {
            for (const char* key : {"admission_dgb_satoshis_each", "operational_dgb_satoshis_each", "carrier_cents_each", "maximum_fee_satoshis",
                                   "admission_dgb_slots", "operational_dgb_slots", "admission_carrier_slots", "operational_carrier_slots"}) {
                if (!approved.find_value(key).isNum() || approved.find_value(key).getInt<int64_t>() != current.find_value(key).getInt<int64_t>()) return false;
            }
            for (const char* key : {"total_output_satoshis", "total_carrier_cents", "maximum_total_fee_satoshis",
                                   "missing_admission_dgb_slots", "missing_operational_dgb_slots", "missing_admission_carrier_slots", "missing_operational_carrier_slots"}) {
                const auto actual = current.find_value(key).getInt<int64_t>();
                if (actual < 0 || actual > approved.find_value(key).getInt<int64_t>()) return false;
            }
            return true;
        } catch (const std::exception&) { return false; }
    }
};
#endif
