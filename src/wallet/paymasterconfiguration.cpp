// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include <wallet/paymasterproviderops.h>
#include <paymaster/finance.h>

#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/setup.h>
#include <paymaster/validation.h>
#include <random.h>
#include <streams.h>
#include <util/chaintype.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/fees.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/spend.h>

#include <univalue.h>
#include <util/overflow.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

#include <wallet/paymasterconfiguration.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>

namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;

UniValue ProviderPolicyToJSON(const DigiDollar::Paymaster::ProviderPolicy& policy)
{
    using namespace DigiDollar::Paymaster;
    UniValue result{UniValue::VOBJ};
    UniValue models{UniValue::VARR};
    if (PolicyAllowsFundingModel(policy, FundingModel::SPONSORED)) models.push_back("sponsored");
    if (PolicyAllowsFundingModel(policy, FundingModel::USER_PAID)) models.push_back("user_paid");
    result.pushKV("funding_models", std::move(models));
    result.pushKV("sponsorship_scope",
                  policy.sponsorship_scope == SponsorshipScope::PUBLIC ? "public" : "restricted");
    result.pushKV("fee_rate_bps", policy.fee_rate_bps);
    result.pushKV("maximum_user_paid_service_fee_cents",
                  policy.maximum_user_paid_service_fee.value);
    result.pushKV("min_amount_cents", policy.min_payment.value);
    result.pushKV("max_amount_cents", policy.max_payment.value);
    result.pushKV("quote_ttl", policy.quote_ttl);
    result.pushKV("maximum_network_fee_dgb_satoshis", policy.maximum_network_fee.value);
    result.pushKV("policy_hash", GetProviderPolicyHash(policy).GetHex());
    return result;
}

UniValue ClientSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("maximum_service_fee_per_transaction_cents",
                  policy.maximum_service_fee_per_transaction.value);
    result.pushKV("maximum_service_fee_per_day_cents",
                  policy.maximum_service_fee_per_day.value);
    if (policy.maximum_service_fee_bps >= 0) {
        result.pushKV("maximum_service_fee_bps", policy.maximum_service_fee_bps);
    }
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

UniValue LiquidityPolicyToJSON(
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("automatic_replenishment", policy.automatic_replenishment);
    result.pushKV("paid_maintenance_approved", policy.paid_maintenance_approved);
    result.pushKV("target_admission_dgb", policy.target_admission_dgb);
    result.pushKV("target_operational_dgb", policy.target_operational_dgb);
    result.pushKV("target_admission_carriers", policy.target_admission_carriers);
    result.pushKV("target_operational_carriers", policy.target_operational_carriers);
    result.pushKV("maximum_maintenance_fee_per_transaction_satoshis",
                  policy.maximum_maintenance_fee_per_transaction.value);
    result.pushKV("maximum_maintenance_fee_per_hour_satoshis",
                  policy.maximum_maintenance_fee_per_hour.value);
    result.pushKV("maximum_maintenance_fee_per_day_satoshis",
                  policy.maximum_maintenance_fee_per_day.value);
    result.pushKV("updated_at", policy.updated_at);
    return result;
}

UniValue CreateProviderIdentity(CWallet& wallet_ref, const std::string& display_name)
{
    auto* wallet = &wallet_ref;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    std::string error;
    if (!CreatePaymasterIdentity(*wallet, display_name, GetTime(), identity, error)) {
        const auto code = error == "PAYMASTER_WALLET_LOCKED" ? ErrorKind::UNLOCK : ErrorKind::WALLET;
        throw ApplicationError(code, error);
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("provider_id", identity.provider_id.GetHex());
    result.pushKV("identity_key", HexStr(identity.identity_key));
    result.pushKV("display_name", identity.display_name);
    result.pushKV("created_at", identity.created_at);
    return result;
}

UniValue SaveProviderPolicy(WalletContext& context, CWallet& wallet_ref, DigiDollar::Paymaster::ProviderPolicy policy)
{
    auto* wallet = &wallet_ref;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    std::optional<ProviderWorkGuard> policy_guard;
    if (context.paymaster && context.paymaster->Enabled() &&
        GetPaymasterIdentity(*wallet, identity)) {
        // The stopped-state check and the database write must form one
        // scheduler exclusion window. Otherwise autostart could begin
        // the provider after IsProviderRunning() returned false but
        // before the new advertised policy reached the wallet.
        policy_guard.emplace(
            *context.paymaster, wallet->GetName(), identity.provider_id,
            /*require_running=*/false);
        if (!policy_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
        }
        if (context.paymaster->IsProviderRunning(
                wallet->GetName(), identity.provider_id)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_RUNNING");
        }
    }
    std::string error;
    if (!SetPaymasterProviderPolicy(*wallet, policy, GetTime(), error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    return ProviderPolicyToJSON(policy);
}

UniValue SaveProviderSafetyPolicy(WalletContext& context, CWallet& wallet_ref, DigiDollar::Paymaster::ProviderSafetyPolicy policy)
{
    auto* wallet = &wallet_ref;
    ProviderIdentityRecord identity;
    std::optional<ProviderWorkGuard> safety_guard;
    if (context.paymaster && context.paymaster->Enabled() &&
        GetPaymasterIdentity(*wallet, identity)) {
        // Safety updates may remain available while the provider is
        // online, but the authoritative read/commit must not overlap
        // a scheduler step that is reserving against the old limits.
        safety_guard.emplace(
            *context.paymaster, wallet->GetName(),
            identity.provider_id, /*require_running=*/false);
        if (!safety_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
        }
    }
    std::string error;
    if (!SetPaymasterProviderSafetyPolicy(*wallet, policy, GetTime(), error) ||
        !GetPaymasterProviderSafetyPolicy(*wallet, policy)) {
        throw ApplicationError(ErrorKind::WALLET,
                           error.empty() ? "PAYMASTER_SAFETY_POLICY_RELOAD_FAILED" : error);
    }
    return ProviderSafetyPolicyToJSON(policy);
}

UniValue SaveClientSafetyPolicy(CWallet& wallet_ref, DigiDollar::Paymaster::ClientSafetyPolicy policy)
{
    auto* wallet = &wallet_ref;
    std::string error;
    if (!SetPaymasterClientSafetyPolicy(*wallet, policy, GetTime(), error) ||
        !GetPaymasterClientSafetyPolicy(*wallet, policy)) {
        throw ApplicationError(ErrorKind::WALLET,
                           error.empty() ? "PAYMASTER_CLIENT_SAFETY_POLICY_RELOAD_FAILED" : error);
    }
    return ClientSafetyPolicyToJSON(policy);
}

UniValue EnableProvider(WalletContext& context, CWallet& wallet_ref, bool requested_enabled)
{
    auto* wallet = &wallet_ref;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    std::optional<ProviderWorkGuard> disable_guard;
    if (!requested_enabled && context.paymaster &&
        context.paymaster->Enabled() &&
        GetPaymasterIdentity(*wallet, identity)) {
        // Serialize the persistent disable and runtime stop with the
        // guarded start transition and all provider work. Whichever
        // operation owns the slot first determines a coherent final
        // state.
        disable_guard.emplace(
            *context.paymaster, wallet->GetName(), identity.provider_id,
            /*require_running=*/false);
        if (!disable_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
        }
    }
    std::string error;
    if (!SetPaymasterProviderEnabled(
            *wallet, requested_enabled, GetTime(), error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    DigiDollar::Paymaster::ProviderSettings settings;
    if (!GetPaymasterProviderSettings(*wallet, settings)) {
        throw ApplicationError(ErrorKind::WALLET, "Failed to reload Paymaster settings");
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("enabled", settings.enabled);
    if (!settings.enabled && context.paymaster) context.paymaster->StopProvider(wallet->GetName());
    const bool running = settings.enabled && context.paymaster &&
                         GetPaymasterIdentity(*wallet, identity) &&
                         context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id);
    result.pushKV("running", running);
    if (!settings.policy_hash.IsNull()) result.pushKV("policy_hash", settings.policy_hash.GetHex());
    return result;
}

UniValue CreateRestrictedDescriptor(WalletContext& context, CWallet& wallet_ref, const XOnlyPubKey& sponsor_key, const std::string& service_name, int64_t ttl)
{
    auto* wallet = &wallet_ref;
    std::string scope_error;
    if (!CheckNewSponsorshipScope(SponsorshipScope::RESTRICTED, scope_error)) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, scope_error);
    }
    wallet->BlockUntilSyncedToCurrentChain();
    if (!sponsor_key.IsFullyValid() || ttl < 1 || ttl > ANNOUNCEMENT_TTL_SECONDS) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "Invalid sponsor key or expires_in outside 1..600");
    }
    const ProviderReadiness readiness = GetProviderReadiness(*wallet, context);
    if (!readiness.ready || readiness.policy.sponsorship_scope != SponsorshipScope::RESTRICTED) {
        throw ApplicationError(ErrorKind::WALLET,
                           readiness.ready ? "PAYMASTER_POLICY_NOT_RESTRICTED" : "PAYMASTER_PROVIDER_NOT_READY");
    }
    node::NodeContext* node = wallet->chain().context();
    if (!node || !node->chainman) {
        throw ApplicationError(ErrorKind::INTERNAL, "Node context unavailable");
    }
    uint256 reference_block;
    {
        LOCK(cs_main);
        const CBlockIndex* tip = node->chainman->ActiveChain().Tip();
        if (!tip) throw ApplicationError(ErrorKind::GENERAL, "PAYMASTER_NODE_NOT_READY");
        reference_block = tip->GetBlockHash();
    }
    const int64_t now = GetTime();
    RestrictedServiceDescriptor descriptor;
    std::string error;
    if (!BuildRestrictedServiceDescriptor(
            *wallet, readiness.identity, readiness.policy, readiness.pool_entries,
            readiness.endpoint, sponsor_key, service_name,
            Params().GenesisBlock().GetHash(), reference_block, now,
            SaturatingAddSeconds(now, ttl),
            descriptor, error) ||
        !ValidateRestrictedServiceDescriptor(
            descriptor, readiness.identity.identity_key,
            Params().GenesisBlock().GetHash(), now, error,
            Params().GetChainType() == ChainType::REGTEST) ||
        !ValidateRestrictedAdmissionProofs(descriptor, readiness.identity.identity_key,
                                           *node->chainman, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << descriptor;
    UniValue result{UniValue::VOBJ};
    result.pushKV("descriptor", HexStr(MakeUCharSpan(stream)));
    result.pushKV("provider_id", descriptor.provider_id.GetHex());
    result.pushKV("provider_identity_key", HexStr(readiness.identity.identity_key));
    result.pushKV("offer_id", descriptor.offer_id.GetHex());
    result.pushKV("policy_hash", descriptor.policy_hash.GetHex());
    result.pushKV("endpoint", descriptor.p2p_endpoint.ToStringAddrPort());
    result.pushKV("expires_at", descriptor.expires_at);
    return result;
}

UniValue SaveProviderRuntime(WalletContext& context, CWallet& wallet_ref, std::optional<DigiDollar::Paymaster::ProviderOperationMode> requested_mode, std::optional<bool> requested_autostart)
{
    auto* wallet = &wallet_ref;

    ProviderIdentityRecord identity;
    const bool have_identity =
        GetPaymasterIdentity(*wallet, identity);
    std::optional<ProviderWorkGuard> runtime_guard;
    if (context.paymaster && context.paymaster->Enabled() &&
        have_identity) {
        // A mode change and a guarded start must be ordered against
        // each other. Otherwise both could observe the stopped state
        // before the database commit makes the new mode authoritative.
        runtime_guard.emplace(
            *context.paymaster, wallet->GetName(),
            identity.provider_id, /*require_running=*/false);
        if (!runtime_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
        }
    }

    if (!requested_mode && !requested_autostart) throw ApplicationError(ErrorKind::INVALID_INPUT, "At least one runtime setting is required");
    if (requested_mode && *requested_mode != ProviderOperationMode::AUTOMATIC && *requested_mode != ProviderOperationMode::MANUAL)
        throw ApplicationError(ErrorKind::INVALID_INPUT, "operation_mode must be automatic or manual");
    ProviderSettings current;
    if (!GetPaymasterProviderSettings(*wallet, current)) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_PROVIDER_SETTINGS_NOT_FOUND");
    }
    const auto mode = requested_mode.value_or(current.operation_mode);
    const bool autostart = requested_autostart.value_or(current.autostart);

    const bool running = context.paymaster &&
                         have_identity &&
                         context.paymaster->IsProviderRunning(
                             wallet->GetName(), identity.provider_id);
    if (running && mode != current.operation_mode) {
        throw ApplicationError(ErrorKind::WALLET,
                           "PAYMASTER_STOP_PROVIDER_BEFORE_MODE_CHANGE");
    }
    std::string error;
    if (!SetPaymasterProviderRuntimeSettings(
            *wallet, mode, autostart, GetTime(), error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("operation_mode", ProviderOperationModeName(mode));
    result.pushKV("autostart", autostart);
    result.pushKV("running", running);
    return result;
}

UniValue SaveLiquidityPolicy(WalletContext& context, CWallet& wallet_ref, DigiDollar::Paymaster::ProviderLiquidityPolicy policy, std::optional<int64_t> expected_revision)
{
    auto* wallet = &wallet_ref;
    ProviderIdentityRecord identity;
    if (!context.paymaster ||
        !GetPaymasterIdentity(*wallet, identity)) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
    }
    ProviderWorkGuard work_guard{
        *context.paymaster, wallet->GetName(), identity.provider_id,
        /*require_running=*/false};
    if (!work_guard.Acquired()) {
        throw ApplicationError(
            ErrorKind::WALLET,
            "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
    }
    // Serialize the revision check with the write, including CLI edits.
    LOCK(wallet->cs_wallet);
    ProviderLiquidityPolicy saved_policy;
    const bool have_saved = GetPaymasterProviderLiquidityPolicy(*wallet, saved_policy);
    if (expected_revision.has_value()) {
        const int64_t expected = *expected_revision;
        if (expected < 0 || expected != (have_saved ? saved_policy.updated_at : 0)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_LIQUIDITY_POLICY_CHANGED");
        }
    }
    int64_t policy_time = GetTime();
    ProviderLiquidityPolicy current_policy;
    if (GetPaymasterProviderLiquidityPolicy(
            *wallet, current_policy)) {
        if (current_policy.updated_at ==
            std::numeric_limits<int64_t>::max()) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_LIQUIDITY_POLICY_TIME_EXHAUSTED");
        }
        policy_time = std::max(
            policy_time, current_policy.updated_at + 1);
    }
    policy.updated_at = policy_time;
    std::string error;
    if (!SetPaymasterProviderLiquidityPolicy(
            *wallet, policy, policy.updated_at, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    return LiquidityPolicyToJSON(policy);
}

UniValue CancelProviderMaintenance(WalletContext& context, CWallet& wallet_ref, const std::vector<uint256>& operations, bool execute, const uint256& plan)
{
    auto* wallet = &wallet_ref;
    if (!context.paymaster || !context.paymaster->Enabled())
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DISABLED");
    if (execute == plan.IsNull()) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_OPERATION_APPROVAL");
    if (operations.empty() || operations.size() > 256) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_MAINTENANCE_SELECTION");
    wallet->BlockUntilSyncedToCurrentChain();
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentity(*wallet, identity))
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
    ProviderWorkGuard guard{*context.paymaster, wallet->GetName(), identity.provider_id, false};
    if (!guard.Acquired()) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
    PaymasterMaintenanceCancellation review;
    std::string error;
    if (!CancelPaymasterMaintenance(*wallet, operations, execute, plan, GetTime(), review, error))
        throw ApplicationError(ErrorKind::WALLET, error);
    UniValue result{UniValue::VOBJ};
    result.pushKV("executed", execute);
    result.pushKV("plan_id", review.plan_id.GetHex());
    UniValue ids{UniValue::VARR};
    for (const auto& id : operations) ids.push_back(id.GetHex());
    result.pushKV("operation_ids", ids);
    result.pushKV("released_fee_budget_satoshis", review.released_fee_budget);
    result.pushKV("automatic_refill_disabled", execute);
    return result;
}

UniValue ReleaseProviderCapital(WalletContext& context, CWallet& wallet_ref, bool execute, const uint256& plan, bool recovery, bool cancel_uncreated)
{
    auto* wallet = &wallet_ref;
    if (!context.paymaster || !context.paymaster->Enabled())
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DISABLED");
    if (execute == plan.IsNull()) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_OPERATION_APPROVAL");
    wallet->BlockUntilSyncedToCurrentChain();
    if (wallet->chain().isInitialBlockDownload())
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CAPITAL_CHAIN_NOT_READY");
    ProviderIdentityRecord identity;
    if (!GetPaymasterIdentity(*wallet, identity))
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
    ProviderWorkGuard guard{*context.paymaster, wallet->GetName(), identity.provider_id, false};
    if (!guard.Acquired()) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
    if (context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id))
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_MUST_BE_STOPPED");
    PaymasterCapitalRelease release;
    std::string error;
    if (!ReleasePaymasterCapital(*wallet, execute, plan, GetTime(), release, error, recovery, cancel_uncreated))
        throw ApplicationError(ErrorKind::WALLET, error);
    UniValue result{UniValue::VOBJ};
    result.pushKV("executed", execute);
    result.pushKV("plan_id", release.plan_id.GetHex());
    result.pushKV("pool_entries", release.entries);
    result.pushKV("dgb_satoshis", release.dgb_satoshis);
    result.pushKV("dd_cents", release.dd_cents);
    result.pushKV("network_fee_satoshis", 0);
    result.pushKV("uncreated_maintenance", release.uncreated_maintenance);
    result.pushKV("recovery_release", release.recovery_release);
    return result;
}

UniValue AdoptProviderBackup(WalletContext& context, CWallet& wallet_ref, bool execute, const uint256& plan, bool independent_checkpoint)
{
    auto* wallet = &wallet_ref;
    if (!context.paymaster || !context.paymaster->Enabled())
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_DISABLED");
    if (execute == plan.IsNull()) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_INVALID_OPERATION_APPROVAL");
    if (execute && !independent_checkpoint) throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_BACKUP_INDEPENDENT_CHECKPOINT_REQUIRED");
    wallet->BlockUntilSyncedToCurrentChain();
    if (wallet->chain().isInitialBlockDownload())
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_CAPITAL_CHAIN_NOT_READY");
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    if (!GetPaymasterIdentity(*wallet, identity))
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_IDENTITY_NOT_FOUND");
    ProviderWorkGuard guard{*context.paymaster, wallet->GetName(), identity.provider_id, false};
    if (!guard.Acquired()) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_BUSY", ErrorDisposition::RETRY_BEFORE_EXECUTION);
    if (context.paymaster->IsProviderRunning(wallet->GetName(), identity.provider_id))
        throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_MUST_BE_STOPPED");
    PaymasterCapitalRelease review;
    std::string error;
    if (!AdoptPaymasterBackup(*wallet, execute, plan, GetTime(), review, error))
        throw ApplicationError(ErrorKind::WALLET, error);
    UniValue result{UniValue::VOBJ};
    result.pushKV("executed", execute);
    result.pushKV("plan_id", review.plan_id.GetHex());
    result.pushKV("provider_id", identity.provider_id.GetHex());
    result.pushKV("pool_entries", review.entries);
    result.pushKV("dgb_satoshis", review.dgb_satoshis);
    result.pushKV("dd_cents", review.dd_cents);
    result.pushKV("network_fee_satoshis", 0);
    result.pushKV("provider_stopped", true);
    return result;
}

UniValue AcknowledgeProviderBackup(CWallet& wallet_ref, bool external_backup)
{
    auto* wallet = &wallet_ref;
    if (!external_backup) {
        throw ApplicationError(ErrorKind::INVALID_INPUT,
                           "PAYMASTER_EXTERNAL_BACKUP_NOT_CONFIRMED");
    }
    const int64_t now = GetTime();
    std::string error;
    if (!AcknowledgePaymasterProviderExternalBackup(
            *wallet, now, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    DigiDollar::Paymaster::ProviderBackupStatus status;
    if (!GetPaymasterProviderBackupStatus(
            *wallet, status, now, error)) {
        throw ApplicationError(ErrorKind::WALLET, error);
    }
    UniValue result{UniValue::VOBJ};
    result.pushKV("acknowledged", true);
    result.pushKV("backup_required",
                  DigiDollar::Paymaster::ProviderBackupRequired(status));
    result.pushKV("acknowledged_at",
                  status.external_backup_acknowledged_at);
    return result;
}
} // namespace wallet::paymaster_app
