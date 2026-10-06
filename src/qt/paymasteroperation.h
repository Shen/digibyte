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

    /** A start intent authorizes no reserve creation. Do not present it as
     * automatic progress when no saved work or pending capacity can fill the gap. */
    bool startNeedsReserveApproval() const
    {
        const auto& provider = snapshot.find_value("provider");
        const auto& operations = provider.find_value("active_operations");
        const auto& preparation = provider.find_value("preparation");
        if (!provider.find_value("start_requested").isTrue() || !provider.find_value("pool_ready").isFalse() ||
            !operations.isArray() || !operations.empty() || !preparation.isArray()) return false;
        for (const auto& step : preparation.getValues()) {
            const auto& state = step.find_value("state");
            if (!state.isStr() || (state.get_str() != "complete" && state.get_str() != "cancelled")) return false;
        }
        const auto& liquidity = provider.find_value("liquidity");
        bool missing = liquidity.find_value("targets_satisfy_provider_policy").isFalse();
        try {
            for (const char* asset : {"admission_dgb", "operational_dgb", "admission_carriers", "operational_carriers"}) {
                const auto& counts = liquidity.find_value(asset);
                // Reserved payments and unconfirmed successor outputs are real
                // work even when no maintenance operation is in the journal.
                if (counts.find_value("pending").getInt<int64_t>() != 0 ||
                    counts.find_value("reserved").getInt<int64_t>() != 0) return false;
                missing |= counts.find_value("missing").getInt<int64_t>() > 0;
            }
        } catch (const std::exception&) { return false; }
        return missing;
    }

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
        // A planned maintenance record may have no journal error yet. The
        // current service can still be blocked before saving its transaction.
        const auto& service_error = provider.find_value("last_service_error");
        if (needs_creation && !blocked && service_error.isStr() &&
            service_error.get_str() == "PAYMASTER_MAINTENANCE_FEE_EXCEEDED") {
            blocked = true;
            error = QString::fromStdString(service_error.get_str());
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
                } else if (startNeedsReserveApproval()) {
                    blocked = true;
                    error = QStringLiteral("PAYMASTER_POOL_PREPARATION_REQUIRED");
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

    /** An explicit restore may propose repairing deliberately reduced DD
     * capacity. Merely observing a wallet must never persist this proposal. */
    static UniValue restorationPolicy(const UniValue& snapshot)
    {
        const auto& provider = snapshot.find_value("provider");
        const auto& saved = provider.find_value("liquidity").find_value("policy");
        UniValue proposed{UniValue::VOBJ};
        for (const char* key : {"automatic_replenishment", "paid_maintenance_approved",
                               "target_admission_dgb", "target_operational_dgb",
                               "target_admission_carriers", "target_operational_carriers",
                               "maximum_maintenance_fee_per_transaction_satoshis",
                               "maximum_maintenance_fee_per_hour_satoshis",
                               "maximum_maintenance_fee_per_day_satoshis"}) {
            proposed.pushKV(key, saved.find_value(key));
        }
        const auto& models = provider.find_value("policy").find_value("funding_models");
        for (const auto& model : models.getValues()) {
            if (model.get_str() != "user_paid") continue;
            proposed.pushKV("target_admission_carriers", std::max(3, saved.find_value("target_admission_carriers").getInt<int>()));
            proposed.pushKV("target_operational_carriers", std::max(1, saved.find_value("target_operational_carriers").getInt<int>()));
        }
        return proposed;
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
