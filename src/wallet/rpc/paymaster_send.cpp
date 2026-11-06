// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/rpc/paymaster_send.h>

#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <sync.h>
#include <tinyformat.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>
#include <wallet/wallet.h>

namespace wallet {
PaymasterSendOptions ParsePaymasterSendOptions(const JSONRPCRequest& request)
{
    // Preserve upstream amount_unit at index 5; Paymaster options follow it.
    const bool has_send_options = request.params.size() > 6 && !request.params[6].isNull();
    PaymasterSendOptions options;
    auto& [fee_mode, subtract_paymaster_fee_from_amount, send_all_spendable_dd, send_options] = options;
    if (has_send_options) {
        send_options = request.params[6].get_obj();
        const bool fee_cap_supplied = !send_options.find_value("maximum_paymaster_fee_cents").isNull();
        const bool attempts_supplied = !send_options.find_value("maximum_provider_attempts").isNull();
        const bool privacy_supplied = !send_options.find_value("privacy").isNull();
        const bool selection_supplied = !send_options.find_value("selection").isNull();
        const bool preferred_provider_supplied = !send_options.find_value("preferred_provider_id").isNull();
        const bool preferred_offer_supplied = !send_options.find_value("preferred_offer_id").isNull();
        const bool restricted_key_supplied = !send_options.find_value("provider_identity_key").isNull();
        const bool restricted_descriptor_supplied =
            !send_options.find_value("restricted_service_descriptor").isNull();
        const bool restricted_capability_supplied =
            !send_options.find_value("sponsorship_capability").isNull();
        const bool prepare_only_supplied = !send_options.find_value("prepare_only").isNull();
        const bool authorization_commitment_supplied =
            !send_options.find_value("authorization_commitment").isNull();
        const bool subtract_fee_supplied =
            !send_options.find_value("subtract_paymaster_fee_from_amount").isNull();
        const bool send_all_supplied =
            !send_options.find_value("send_all_spendable_dd").isNull();
        const bool restricted_context_supplied = restricted_key_supplied ||
            restricted_descriptor_supplied || restricted_capability_supplied;
        RPCTypeCheckObj(send_options,
                        {{"fee_mode", UniValueType(UniValue::VSTR)},
                         {"request_id", UniValueType(UniValue::VSTR)},
                         {"maximum_paymaster_fee_cents", UniValueType(UniValue::VNUM)},
                         {"maximum_provider_attempts", UniValueType(UniValue::VNUM)},
                          {"privacy", UniValueType(UniValue::VSTR)},
                          {"selection", UniValueType(UniValue::VSTR)},
                          {"preferred_provider_id", UniValueType(UniValue::VSTR)},
                          {"preferred_offer_id", UniValueType(UniValue::VSTR)},
                          {"provider_identity_key", UniValueType(UniValue::VSTR)},
                          {"restricted_service_descriptor", UniValueType(UniValue::VSTR)},
                          {"sponsorship_capability", UniValueType(UniValue::VSTR)},
                          {"prepare_only", UniValueType(UniValue::VBOOL)},
                          {"retry_transport", UniValueType(UniValue::VBOOL)},
                          {"authorization_commitment", UniValueType(UniValue::VSTR)},
                          {"subtract_paymaster_fee_from_amount", UniValueType(UniValue::VBOOL)},
                          {"send_all_spendable_dd", UniValueType(UniValue::VBOOL)}},
                        /*fAllowNull=*/true, /*fStrict=*/true);
        const std::string mode = send_options.find_value("fee_mode").isNull()
            ? "auto" : send_options.find_value("fee_mode").get_str();
        if (mode == "dgb") fee_mode = DigiDollar::Paymaster::FeeMode::DGB;
        else if (mode == "paymaster") fee_mode = DigiDollar::Paymaster::FeeMode::PAYMASTER;
        else if (mode == "auto") fee_mode = DigiDollar::Paymaster::FeeMode::AUTO;
        else throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_mode must be dgb, paymaster, or auto");
        if (fee_mode == DigiDollar::Paymaster::FeeMode::DGB &&
            (fee_cap_supplied || attempts_supplied || privacy_supplied || selection_supplied ||
             preferred_provider_supplied || preferred_offer_supplied ||
             restricted_key_supplied || restricted_descriptor_supplied ||
             restricted_capability_supplied || prepare_only_supplied ||
             authorization_commitment_supplied || subtract_fee_supplied ||
             send_all_supplied || !send_options.find_value("retry_transport").isNull())) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "fee_mode dgb does not accept Paymaster authorization or selection fields");
        }
        if (send_options.find_value("fee_mode").isNull()) send_options.pushKV("fee_mode", mode);

        const std::string privacy = send_options.find_value("privacy").isNull()
            ? "standard" : send_options.find_value("privacy").get_str();
        if (privacy != "standard" && privacy != "high") {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "privacy must be standard or high");
        }
        if (send_options.find_value("privacy").isNull()) send_options.pushKV("privacy", privacy);
        const int maximum_attempts = send_options.find_value("maximum_provider_attempts").isNull()
            ? ((privacy == "high" || restricted_context_supplied) ? 1 : 3)
            : send_options.find_value("maximum_provider_attempts").getInt<int>();
        if (maximum_attempts < 1 || maximum_attempts > 16 ||
            (privacy == "high" && maximum_attempts != 1)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "maximum_provider_attempts must be 1..16 and exactly 1 for high privacy");
        }
        if (send_options.find_value("maximum_provider_attempts").isNull()) {
            send_options.pushKV("maximum_provider_attempts", maximum_attempts);
        }
        const std::string selection = send_options.find_value("selection").isNull()
            ? "lowest_total_cost" : send_options.find_value("selection").get_str();
        if (selection != "lowest_total_cost" && selection != "privacy_weighted") {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "selection must be lowest_total_cost or privacy_weighted");
        }
        if (send_options.find_value("selection").isNull()) send_options.pushKV("selection", selection);
        subtract_paymaster_fee_from_amount = subtract_fee_supplied &&
            send_options.find_value("subtract_paymaster_fee_from_amount").get_bool();
        send_all_spendable_dd = send_all_supplied &&
            send_options.find_value("send_all_spendable_dd").get_bool();
        if (send_all_spendable_dd &&
            !subtract_paymaster_fee_from_amount) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "send_all_spendable_dd requires subtract_paymaster_fee_from_amount");
        }
        if (send_options.find_value("subtract_paymaster_fee_from_amount").isNull()) {
            send_options.pushKV("subtract_paymaster_fee_from_amount", false);
        }
        if (send_options.find_value("send_all_spendable_dd").isNull()) {
            send_options.pushKV("send_all_spendable_dd", false);
        }

        const bool restricted = restricted_context_supplied;
        if (preferred_provider_supplied != preferred_offer_supplied ||
            (restricted && preferred_provider_supplied)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "preferred_provider_id and preferred_offer_id must be supplied together for a public offer");
        }
        if (preferred_provider_supplied) {
            if (ParseHashO(send_options, "preferred_provider_id").IsNull() ||
                ParseHashO(send_options, "preferred_offer_id").IsNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Preferred provider and offer must be non-null");
            }
        }
        if (restricted && !(restricted_key_supplied && restricted_descriptor_supplied &&
                            restricted_capability_supplied)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "provider_identity_key, restricted_service_descriptor, and sponsorship_capability "
                "must be supplied together");
        }
        if (prepare_only_supplied && send_options.find_value("prepare_only").get_bool() &&
            authorization_commitment_supplied) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "prepare_only and authorization_commitment are mutually exclusive");
        }

        if (fee_mode != DigiDollar::Paymaster::FeeMode::DGB) {
            const UniValue& request_id = send_options.find_value("request_id");
            const UniValue& fee_cap = send_options.find_value("maximum_paymaster_fee_cents");
            if (request_id.isNull() || fee_cap.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "request_id and maximum_paymaster_fee_cents are required for paymaster or auto");
            }
            if (!DigiDollar::Paymaster::IsCanonicalRequestId(request_id.get_str())) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "request_id must be a canonical lowercase UUID");
            }
            const int64_t maximum_fee = fee_cap.getInt<int64_t>();
            if (maximum_fee < 0 || maximum_fee > DigiDollar::Paymaster::MAX_DD_OUTPUT_CENTS) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "maximum_paymaster_fee_cents is out of range");
            }
        }
    }
    return options;
}

std::optional<UniValue> PrepareDigiDollarFeeFunding(
    const JSONRPCRequest& request, CWallet& wallet, DigiDollarWallet& dd_wallet,
    const std::string& addressStr, CAmount amount,
    const PaymasterSendOptions& options, const std::vector<COutPoint>* preset_dd_inputs)
{
    const auto& [fee_mode, subtract_paymaster_fee_from_amount, send_all_spendable_dd, send_options] = options;
    const auto finalize_paymaster_result =
        [&](UniValue result) -> UniValue {
            // Low-level signing/result RPCs return different shapes. Always
            // attach the same current read-only session view at this boundary.
            LOCK(wallet.cs_wallet);
            PaymasterStore store{wallet};
            DigiDollar::Paymaster::PaymentSession session;
            const UniValue& request_id = result.find_value("request_id");
            if (!request_id.isStr() ||
                store.GetSessionByRequestIdWithStatus(request_id.get_str(), session) !=
                    DatabaseReadStatus::FOUND) {
                throw JSONRPCError(RPC_WALLET_ERROR, "PAYMASTER_SESSION_READ_FAILED");
            }
            const UniValue snapshot = paymaster_rpc::internal::SessionToJSON(session, &store);
            for (const auto& key : snapshot.getKeys()) result.pushKV(key, snapshot.find_value(key));
            result.pushKV("to_address", addressStr);
            const UniValue& payment =
                result.find_value("payment_cents");
            result.pushKV("amount", payment.isNull()
                                        ? amount
                                        : payment.getInt<int64_t>());
            result.pushKV("requested_amount_cents", amount);
            result.pushKV("subtract_paymaster_fee_from_amount",
                          subtract_paymaster_fee_from_amount);
            result.pushKV("send_all_spendable_dd",
                          send_all_spendable_dd);
            return result;
        };

    if (fee_mode != DigiDollar::Paymaster::FeeMode::DGB) {
        // Preserve the persisted-read error contract before interpreting a
        // new order. Unreadable authority cannot be treated as an absent send.
        PaymasterStore store{wallet};
        DigiDollar::Paymaster::PaymentSession session;
        const auto status = store.GetSessionByRequestIdWithStatus(
            send_options.find_value("request_id").get_str(), session);
        if (status != DatabaseReadStatus::FOUND && status != DatabaseReadStatus::NOT_FOUND) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                status == DatabaseReadStatus::UNSUPPORTED_VERSION
                    ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION"
                    : "PAYMASTER_SESSION_READ_FAILED");
        }
        // The native service owns funding decisions, durable retries and all
        // authorization gates. Only an explicit ordinary DGB send may fall
        // through to TransferDigiDollar below this adapter.
        return finalize_paymaster_result(RequestAutomaticPaymasterQuote(
            request, addressStr, amount, send_options, preset_dd_inputs));
    }
    const CAmount balance = dd_wallet.GetSpendableDDBalance();
    if (amount > balance) {
        if (amount <= balance + dd_wallet.GetPendingDDBalance()) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                "Insufficient confirmed DD balance; please wait for prior DigiDollar transfer confirmation and try again.");
        }
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
            strprintf("Insufficient DD balance (have %d cents, need %d cents)", balance, amount));
    }
    return std::nullopt;
}
} // namespace wallet
