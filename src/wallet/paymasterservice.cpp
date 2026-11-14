// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <wallet/paymasterservice.h>
#include <wallet/paymasterpool.h>
#include <wallet/paymasterapplication_internal.h>
#include <paymaster/error.h>
#include <wallet/paymasterapplication.h>

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
#include <logging/timer.h>
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
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/spend.h>

#include <util/overflow.h>
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




namespace wallet::paymaster_app {
using namespace DigiDollar::Paymaster;
ProviderStartResult StartPaymaster(WalletContext& context, CWallet& target, bool wait_for_readiness, ExecutionMode mode)
{
    CWallet* const wallet = &target;
    const bool automatic_autostart = mode == ExecutionMode::AUTOMATIC;
    if (automatic_autostart) {
        if (!AutomaticProviderStateIsSynchronized(*wallet)) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_SYNCING");
        }
    } else {
        wallet->BlockUntilSyncedToCurrentChain();
    }
    const int64_t now = GetTime();
    const DurablePaymasterRecoveryReport recovery =
        RecoverDurablePaymasterCommits(*wallet, now, /*bounded=*/automatic_autostart);
    std::string restore_error;
    if (!CheckPaymasterProviderRestoreGuard(*wallet, restore_error)) {
        throw ApplicationError(ErrorKind::WALLET, restore_error);
    }
    ProviderIdentityRecord guarded_identity;
    std::optional<ProviderWorkGuard> start_guard;
    if (context.paymaster && context.paymaster->Enabled() &&
        GetPaymasterIdentity(*wallet, guarded_identity)) {
        // Hold the same wallet-local transition slot used by disable
        // and policy updates while taking the authoritative readiness
        // snapshot. Completion below atomically converts this slot to
        // the running state.
        start_guard.emplace(
            *context.paymaster, wallet->GetName(),
            guarded_identity.provider_id,
            /*require_running=*/false);
        if (!start_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY");
        }
    }
    const ProviderReadiness readiness =
        GetProviderReadiness(*wallet, context, !automatic_autostart);
    if (wait_for_readiness) {
        if (!readiness.have_settings || !readiness.settings.enabled || !readiness.wallet_eligible ||
            !start_guard || !context.paymaster->RequestProviderStart(wallet->GetName(), readiness.identity.provider_id)) {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_START_REQUEST_NOT_ACCEPTED");
        }
    }
    bool running{false};
    bool drain_work{false};
    if (!readiness.identity.provider_id.IsNull()) {
        std::string drain_error;
        PaymasterStore store{*wallet};
        if (!store.HasProviderDrainWork(
                readiness.identity.provider_id, drain_work,
                drain_error)) {
            throw ApplicationError(ErrorKind::WALLET, drain_error);
        }
    }
    const bool drain_only{!readiness.ready && drain_work};
    const auto is_liquidity_error = [](const std::string& value) {
        return value == "PAYMASTER_POOLS_NOT_PREPARED" ||
               value == "PAYMASTER_ADMISSION_DGB_MISSING" ||
               value == "PAYMASTER_ADMISSION_CARRIERS_MISSING" ||
               value == "PAYMASTER_OPERATIONAL_SLOT_MISSING";
    };
    const bool maintenance_start =
        !readiness.ready && !readiness.errors.empty() &&
        readiness.settings.operation_mode == ProviderOperationMode::AUTOMATIC &&
        readiness.have_policy && readiness.have_liquidity_policy &&
        readiness.liquidity_policy.automatic_replenishment &&
        ProviderLiquidityTargetsSatisfyPolicy(
            readiness.liquidity_policy, readiness.policy) &&
        std::all_of(readiness.errors.begin(), readiness.errors.end(),
                    is_liquidity_error);
    Announcement announcement;
    if (readiness.ready || drain_only || maintenance_start) {
        if (!start_guard ||
            guarded_identity.provider_id !=
                readiness.identity.provider_id) {
            throw ApplicationError(
                ErrorKind::WALLET,
                "PAYMASTER_PROVIDER_RUNTIME_CONFLICT");
        }
        node::NodeContext* node = wallet->chain().context();
        if (!node || !node->chainman) {
            throw ApplicationError(ErrorKind::INTERNAL, "Node context unavailable");
        }
        std::string error;
        const bool restricted = readiness.policy.sponsorship_scope ==
                                SponsorshipScope::RESTRICTED;
        if (readiness.ready && !restricted) {
            uint256 reference_block;
            {
                LOCK(cs_main);
                const CBlockIndex* tip =
                    node->chainman->ActiveChain().Tip();
                if (!tip) {
                    throw ApplicationError(ErrorKind::GENERAL,
                                       "PAYMASTER_NODE_NOT_READY");
                }
                reference_block = tip->GetBlockHash();
            }
            if (!BuildPaymasterAnnouncement(
                    *wallet, readiness.identity, readiness.policy,
                    readiness.pool_entries, readiness.endpoint,
                    Params().GenesisBlock().GetHash(), reference_block,
                    now, readiness.available_funding_models,
                    announcement, error) ||
                !ValidateAnnouncementEnvelope(
                    announcement, Params().GenesisBlock().GetHash(),
                    now, error,
                    Params().GetChainType() == ChainType::REGTEST) ||
                !ValidateAdmissionProofs(
                    announcement, *node->chainman, error) ||
                !context.paymaster->GetDirectory().AddValidated(
                    announcement, now)) {
                throw ApplicationError(
                    ErrorKind::WALLET,
                    error.empty() ? "PAYMASTER_ANNOUNCEMENT_REJECTED" : error);
            }
        }
        running = start_guard->CompleteProviderStart();
        if (!running) throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_RUNTIME_CONFLICT");
        ProviderServiceState initial_state{ProviderServiceState::ACTIVE};
        if (drain_only) {
            initial_state = ProviderServiceState::DRAIN_ONLY;
        } else if (maintenance_start) {
            initial_state = !readiness.have_liquidity_policy ||
                                    !readiness.liquidity_policy.paid_maintenance_approved
                ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                : ProviderServiceState::REPLENISHING_LIQUIDITY;
        } else if (readiness.settings.operation_mode ==
                   ProviderOperationMode::MANUAL) {
            initial_state = ProviderServiceState::MANUAL;
        }
        context.paymaster->SetProviderServiceStatus(
            wallet->GetName(), initial_state);
        if (readiness.ready && !restricted && node->connman) {
            node->connman->ForEachNode([&announcement, node](CNode* peer) {
                if (!peer->fSuccessfullyConnected || peer->fDisconnect) return;
                const CNetMsgMaker maker{peer->GetCommonVersion()};
                node->connman->PushMessage(peer, maker.Make(NetMsgType::PMANNOUNCE,
                                                            announcement));
            });
        }
    }
    ProviderStartResult result;
    const auto lifecycle = CaptureProviderLifecycleStatus(*wallet, context, readiness, true);
    result.start_requested = lifecycle.start_requested;
    result.running = lifecycle.running;
    result.ready = readiness.ready;
    result.drain_only = drain_only && running;
    result.operation_mode = readiness.settings.operation_mode;
    result.autostart = readiness.settings.autostart;
    if (!running && context.paymaster) {
        context.paymaster->SetProviderServiceStatus(
            wallet->GetName(), lifecycle.service.state,
            readiness.errors.empty() ||
                    lifecycle.service.state == ProviderServiceState::STOPPED
                ? std::string{}
                : StableProviderServiceError(
                      readiness.errors.front()));
    }
    result.service_state = lifecycle.service.state;
    if (readiness.have_safety_policy) {
        result.safety_policy = readiness.safety_policy;
    }
    if (!readiness.identity.provider_id.IsNull()) result.provider_id = readiness.identity.provider_id;
    if (running) {
        result.endpoint = readiness.endpoint;
        if (announcement.sequence != 0) {
            result.announcement_sequence = announcement.sequence;
        }
    }
    result.recovery = recovery;
    result.readiness_errors = readiness.errors;
    if (const auto* node = wallet->chain().context(); node && node->connman) {
        result.transport = CapturePaymasterTransport(*node->connman);
    }
    return result;
}

ProviderStopResult StopPaymaster(WalletContext& context, CWallet& target, bool persistent, bool pause_setup)
{
    if (pause_setup && !persistent) {
        throw ApplicationError(ErrorKind::INVALID_INPUT, "PAYMASTER_PAUSE_REQUIRES_PERSISTENT_STOP",
                               ErrorDisposition::USER_DECISION);
    }
    CWallet* const wallet = &target;
    std::optional<ProviderStopGuard> stop_guard;
    if (context.paymaster && context.paymaster->Enabled()) {
        // Register stop priority before waiting: no new start, refill
        // or other provider work may overtake this request. The current
        // owner still finishes under its exclusive guard. This scope
        // retains priority through persistent pause and runtime stop;
        // exceptions/timeouts release it without pretending to stop.
        AssertLockNotHeld(cs_main);
        AssertLockNotHeld(wallet->cs_wallet);
        stop_guard.emplace(*context.paymaster, wallet->GetName());
        if (!stop_guard->Requested())
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_PROVIDER_BUSY");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        do {
            if (stop_guard->TryAcquire()) break;
            if (!context.paymaster->Enabled()) break;
            UninterruptibleSleep(std::chrono::milliseconds{25});
        } while (std::chrono::steady_clock::now() < deadline);
        if (!stop_guard->Acquired()) {
            throw ApplicationError(ErrorKind::WALLET,
                               "PAYMASTER_PROVIDER_BUSY");
        }
    }
    if (persistent) {
        std::string error;
        if (!PausePaymasterProvider(*wallet, pause_setup, GetTime(), error)) throw ApplicationError(ErrorKind::WALLET, error);
    }
    if (context.paymaster) {
        context.paymaster->StopProvider(wallet->GetName());
    }
    return {};
}

} // namespace wallet::paymaster_app

namespace wallet {
using namespace paymaster_app;
namespace {
void BackgroundFailure(WalletContext& context, CWallet& wallet,
                       const DigiDollar::Paymaster::ApplicationError& failure)
{
    using namespace DigiDollar::Paymaster;
    auto* manager = context.paymaster;
    if (!manager) return;
    if (failure.code == "PAYMASTER_PROVIDER_SERVICE_BUSY") return;
    ProviderServiceState state{ProviderServiceState::FAULT};
    if (failure.kind == ErrorKind::UNLOCK || failure.code == "PAYMASTER_WALLET_LOCKED") {
        state = ProviderServiceState::WAITING_FOR_UNLOCK;
    } else if (failure.code == "PAYMASTER_INVALID_MAINTENANCE_LEDGER" ||
               failure.code == "PAYMASTER_PROVIDER_SYNCING" ||
               failure.code == "PAYMASTER_REQUIRES_READY_TXINDEX" ||
               failure.code == "PAYMASTER_TXINDEX_NOT_READY" ||
               failure.code == "PAYMASTER_NODE_NOT_READY") {
        state = ProviderServiceState::WAITING_FOR_READINESS;
    } else if (failure.code == "PAYMASTER_PROVIDER_DRAIN_ONLY" ||
               failure.code == "PAYMASTER_SAFETY_LIMIT_EXHAUSTED") {
        state = ProviderServiceState::DRAIN_ONLY;
    } else if (failure.code == "PAYMASTER_PROVIDER_NOT_RUNNING") {
        state = ProviderServiceState::STOPPED;
    }
    const auto previous = manager->GetProviderServiceStatus(wallet.GetName());
    if (previous.state != state || previous.last_error != failure.code) {
        wallet.WalletLogPrintf("Paymaster background work paused: %s\n", failure.code);
    }
    manager->SetProviderServiceStatus(wallet.GetName(), state, failure.code);
}

// One bounded unit of work per queue keeps the validation scheduler responsive
// and preserves fairness across wallets/providers. Temporary local failures
// retain their lease for retry; permanent protocol failures are durably rejected
// before the message is acknowledged.
void RunPaymasterProviderServiceCycleImpl(WalletContext& context, CWallet& wallet)
{
    using namespace DigiDollar::Paymaster;
    Manager* const manager = context.paymaster;
    if (!manager || !manager->Enabled()) return;

    std::string restore_error;
    if (!CheckPaymasterProviderRestoreGuard(wallet, restore_error)) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::WAITING_FOR_READINESS,
                                          restore_error);
        return;
    }

    ProviderSettings settings;
    ProviderIdentityRecord identity;
    if (!GetPaymasterProviderSettings(wallet, settings) ||
        !GetPaymasterIdentity(wallet, identity) || !settings.enabled) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::STOPPED);
        return;
    }

    bool running = manager->IsProviderRunning(wallet.GetName(),
                                              identity.provider_id);
    if (!running) {
        ProviderReadiness observation;
        observation.have_identity = observation.have_settings = true;
        observation.identity = identity;
        observation.settings = settings;
        const auto lifecycle = CaptureProviderLifecycleStatus(wallet, context, observation, false);
        if (lifecycle.service.state == ProviderServiceState::STOPPED) {
            manager->SetProviderServiceStatus(wallet.GetName(),
                                              ProviderServiceState::STOPPED);
            return;
        }
        if (lifecycle.service.state == ProviderServiceState::WAITING_FOR_UNLOCK) {
            manager->SetProviderServiceStatus(
                wallet.GetName(), ProviderServiceState::WAITING_FOR_UNLOCK,
                "PAYMASTER_WALLET_LOCKED");
            return;
        }

        try {
            const ProviderStartResult result = StartPaymaster(context, wallet, false, ExecutionMode::AUTOMATIC);
            running = result.running;
            if (!running) {
                std::string error{"PAYMASTER_PROVIDER_NOT_READY"};
                if (!result.readiness_errors.empty()) {
                    error = StableProviderServiceError(result.readiness_errors.front());
                }
                manager->SetProviderServiceStatus(
                    wallet.GetName(),
                    wallet.IsLocked()
                        ? ProviderServiceState::WAITING_FOR_UNLOCK
                        : ProviderServiceState::WAITING_FOR_READINESS,
                    std::move(error));
                return;
            }
        } catch (const ApplicationError& failure) {
            const std::string& error = failure.code;
            manager->SetProviderServiceStatus(
                wallet.GetName(),
                error == "PAYMASTER_WALLET_LOCKED"
                    ? ProviderServiceState::WAITING_FOR_UNLOCK
                    : ProviderServiceState::WAITING_FOR_READINESS,
                error);
            return;
        } catch (const std::exception&) {
            manager->SetProviderServiceStatus(
                wallet.GetName(), ProviderServiceState::FAULT,
                "PAYMASTER_AUTOSTART_FAILED");
            return;
        }
    }

    // Authenticate even manual and drain-only transports before accepting
    // payment metadata. This domain-separated identity proof approves no spend.
    // Keys remain wallet-owned, and restore/lock gates still apply.
    if (!wallet.IsLocked() && manager->HasChannelChallenges(identity.provider_id, DirectNow())) {
        ProviderWorkGuard work_guard{*manager, wallet.GetName(), identity.provider_id};
        if (work_guard.Acquired()) {
            const auto challenges = manager->TakeChannelChallenges(identity.provider_id, 1, DirectNow());
            for (const auto& [peer_id, challenge] : challenges) {
                CKey key;
                ProviderIdentityRecord current;
                std::string error;
                if (!GetPaymasterIdentityKey(wallet, key, current, error) ||
                    current.provider_id != challenge.provider_id) continue;
                ChannelProof proof;
                proof.challenge = challenge;
                proof.identity_key = current.identity_key;
                if (key.SignSchnorr(GetChannelAuthHash(challenge), proof.signature, nullptr, GetRandHash()) &&
                    manager->QueueChannelProof(peer_id, proof, DirectNow())) {
                    if (const auto* node = wallet.chain().context(); node && node->connman)
                        node->connman->WakeMessageHandler();
                }
            }
        }
    }

    if (settings.operation_mode == ProviderOperationMode::MANUAL) {
        manager->SetProviderServiceStatus(wallet.GetName(),
                                          ProviderServiceState::MANUAL);
        return;
    }
    if (wallet.IsLocked()) {
        // Keep the explicitly started runtime registered, but never consume or
        // sign queued work until the operator unlocks the wallet again.
        manager->SetProviderServiceStatus(
            wallet.GetName(), ProviderServiceState::WAITING_FOR_UNLOCK,
            "PAYMASTER_WALLET_LOCKED");
        return;
    }

    const ProviderServiceStatus previous =
        manager->GetProviderServiceStatus(wallet.GetName());
    ProviderServiceState state =
        previous.state == ProviderServiceState::DRAIN_ONLY
            ? ProviderServiceState::DRAIN_ONLY
            : ProviderServiceState::ACTIVE;
    std::string first_error;
    bool accept_new_requests{state != ProviderServiceState::DRAIN_ONLY};

    if (!accept_new_requests) {
        ProviderWorkGuard drain_guard{*manager, wallet.GetName(), identity.provider_id};
        if (!drain_guard.Acquired()) return;
        // Drain-only is a temporary admission gate, not an operator policy.
        // Expiry/cancellation and rolling limits can restore readiness without
        // another start RPC. Reconcile and check the full current prerequisites
        // before resuming; every request still checks its own persisted limits.
        const auto readiness = GetProviderReadiness(wallet, context, /*wait_for_sync=*/false);
        if (readiness.ready && AutomaticProviderStateIsSynchronized(wallet)) {
            state = ProviderServiceState::ACTIVE;
            accept_new_requests = true;
        } else {
            first_error = readiness.errors.empty() ? "PAYMASTER_PROVIDER_SYNCING" : readiness.errors.front();
        }
    }

    if (accept_new_requests) {
        ProviderWorkGuard maintenance_guard{
            *manager, wallet.GetName(), identity.provider_id};
        if (!maintenance_guard.Acquired()) {
            // A manual expert action or another bounded provider step owns
            // the wallet. Leave its service state untouched and retry next
            // scheduler tick.
            return;
        }
        bool setup_pending{false};
        try {
            setup_pending = HasPendingPoolPreparation(wallet);
        } catch (const ApplicationError& failure) {
            BackgroundFailure(context, wallet, failure);
            return;
        }
        // Do not replenish the same outputs twice. Existing signed payment
        // submissions below can still finish while setup waits for funding.
        if (setup_pending) {
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_READINESS;
            first_error = "PAYMASTER_POOL_PREPARATION_PENDING";
        } else {
            ProviderReadiness liquidity = GetProviderReadiness(
                wallet, context, /*wait_for_sync=*/false);
            const ProviderLiquidityPolicy liquidity_policy =
                liquidity.have_liquidity_policy
                    ? liquidity.liquidity_policy
                    : SuggestedLiquidityPolicy(liquidity, GetTime());
            const bool targets_satisfy_provider_policy =
                !liquidity.have_policy ||
                ProviderLiquidityTargetsSatisfyPolicy(
                    liquidity_policy, liquidity.policy);
            const auto admission_dgb = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DGB,
                liquidity_policy.target_admission_dgb);
            const int64_t operational_dgb_minimum = liquidity.have_policy
                ? liquidity.policy.maximum_network_fee.value
                : std::numeric_limits<int64_t>::max();
            const auto operational_dgb = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DGB,
                liquidity_policy.target_operational_dgb,
                operational_dgb_minimum);
            const auto admission_carriers = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER,
                liquidity_policy.target_admission_carriers);
            const auto operational_carriers = CountLiquiditySlots(
                liquidity.pool_entries, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER,
                liquidity_policy.target_operational_carriers, 0,
                liquidity.have_policy ? &liquidity.policy : nullptr);
            const size_t missing_dgb = admission_dgb.missing + operational_dgb.missing;
            const size_t missing_carriers = admission_carriers.missing +
                                            operational_carriers.missing;
            const bool targets_confirmed =
                admission_dgb.ready >= liquidity_policy.target_admission_dgb &&
                operational_dgb.ready >= liquidity_policy.target_operational_dgb &&
                admission_carriers.ready >= liquidity_policy.target_admission_carriers &&
                operational_carriers.ready >= liquidity_policy.target_operational_carriers;
            // A successor or explicit preparation can satisfy a target after
            // a recurring refill was planned. Close only unsigned obsolete jobs
            // under this work guard, after recovering any lost transaction reply.
            std::string cleanup_error;
            if (liquidity.errors.empty() && !ReleaseSatisfiedProviderReplenishment(
                    wallet,
                    admission_dgb.ready >= liquidity_policy.target_admission_dgb &&
                        operational_dgb.ready >= liquidity_policy.target_operational_dgb,
                    admission_carriers.ready >= liquidity_policy.target_admission_carriers &&
                        operational_carriers.ready >= liquidity_policy.target_operational_carriers,
                    cleanup_error)) {
                manager->SetProviderServiceStatus(wallet.GetName(), ProviderServiceState::WAITING_FOR_READINESS,
                                                  StableProviderServiceError(cleanup_error));
                return;
            }
            if (ProviderMaintenanceBroadcastPending(wallet, liquidity.maintenance_ledger)) {
                // Signed work remains protected, but an unsubmitted wallet
                // transaction cannot be described as waiting for confirmation.
                accept_new_requests = false;
                state = ProviderServiceState::WAITING_FOR_READINESS;
                first_error = wallet.GetBroadcastTransactions() ? "PAYMASTER_MAINTENANCE_BROADCAST_PENDING"
                                                               : "PAYMASTER_WALLET_BROADCAST_DISABLED";
            } else if (!targets_satisfy_provider_policy) {
                // A zero operational carrier target can be a deliberate result of
                // release_slot. It is valid to persist, but automatic maintenance
                // must not claim success or silently recreate capital the operator
                // explicitly released. Require an explicit target change first.
                accept_new_requests = false;
                state = ProviderServiceState::WAITING_FOR_READINESS;
                first_error = "PAYMASTER_LIQUIDITY_TARGETS_INCOMPLETE";
            } else if (missing_dgb + missing_carriers > 0 &&
                       admission_dgb.pending + operational_dgb.pending +
                       admission_carriers.pending + operational_carriers.pending > 0) {
                // A partial refill must confirm before planning the remainder.
                // Pending outputs already count toward the saved targets.
                accept_new_requests = false;
                state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
                first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
            } else if (missing_dgb + missing_carriers > 0) {
                accept_new_requests = false;
                if (!liquidity.have_liquidity_policy ||
                    !liquidity_policy.paid_maintenance_approved) {
                    state = ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL;
                    first_error = "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED";
                } else if (!liquidity_policy.automatic_replenishment) {
                    state = ProviderServiceState::WAITING_FOR_READINESS;
                    first_error = "PAYMASTER_AUTOMATIC_REPLENISHMENT_DISABLED";
                } else {
                    std::string maintenance_error;
                    const bool replenished = missing_carriers > 0
                        ? RunAutomaticCarrierReplenishment(
                              wallet, liquidity_policy,
                              admission_carriers.missing,
                              operational_carriers.missing,
                              maintenance_error)
                        : RunAutomaticDGBReplenishment(
                              wallet, liquidity_policy,
                              admission_dgb.missing,
                              operational_dgb.missing,
                              maintenance_error);
                    if (!replenished) {
                        first_error = StableProviderServiceError(
                            maintenance_error.empty()
                                ? "PAYMASTER_LIQUIDITY_REPLENISHMENT_FAILED"
                                : maintenance_error);
                        state = first_error == "PAYMASTER_WALLET_BROADCAST_DISABLED"
                            ? ProviderServiceState::WAITING_FOR_READINESS
                            : first_error == "PAYMASTER_MAINTENANCE_APPROVAL_REQUIRED" ||
                                        first_error == "PAYMASTER_MAINTENANCE_LIMIT_EXHAUSTED"
                            ? ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL
                            : ProviderServiceState::REPLENISHING_LIQUIDITY;
                    } else {
                        state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
                        first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
                    }
                }
            } else if (!targets_confirmed &&
                       (!liquidity.ready || std::any_of(
                           liquidity.pool_entries.begin(), liquidity.pool_entries.end(),
                           [](const ProviderPoolEntry& entry) {
                               return entry.state == PoolEntryState::PENDING_SUCCESSOR ||
                                      (entry.state == PoolEntryState::AVAILABLE &&
                                       entry.confirmation_height <= 0);
                           }))) {
                // Confirmed reserved slots still satisfy the refill target.
                // They must not stop independent free slots from serving new
                // requests. Pending replacements cannot themselves provide
                // payment capacity; the fresh readiness check below decides
                // whether independent confirmed slots remain usable.
                accept_new_requests = false;
                state = ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION;
                first_error = "PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING";
            }
            if (!accept_new_requests) {
                // A refill target is not a minimum payment capacity. Re-read
                // after maintenance, under the same work guard: a transaction
                // or reconciliation may have changed the available inputs.
                const auto current = GetProviderReadiness(wallet, context, /*wait_for_sync=*/false);
                if (ProviderCanServeDuringRefill({state, first_error}, current,
                                                AutomaticProviderStateIsSynchronized(wallet))) {
                    accept_new_requests = true;
                    state = ProviderServiceState::ACTIVE;
                    // Keep the refill diagnosis for its separate status card.
                    // Announcement and request handlers repeat their checks.
                }
            }
        }
    }

    const int64_t announcement_now = GetTime();
    const bool announcement_refresh_required =
        previous.state == ProviderServiceState::DRAIN_ONLY ||
        previous.state == ProviderServiceState::WAITING_FOR_UNLOCK ||
        previous.state == ProviderServiceState::WAITING_FOR_READINESS ||
        previous.state == ProviderServiceState::WAITING_FOR_MAINTENANCE_APPROVAL ||
        previous.state == ProviderServiceState::REPLENISHING_LIQUIDITY ||
        previous.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION ||
        previous.state == ProviderServiceState::FAULT ||
        manager->GetDirectory().NeedsRefresh(identity.provider_id,
                                            announcement_now);
    if (accept_new_requests && announcement_refresh_required) {
        // Refresh by wall clock even when the chain tip does not change.
        // Serialize the fresh readiness check and signing with other provider
        // work; never renew a stale readiness snapshot.
        ProviderWorkGuard announcement_guard{*manager, wallet.GetName(), identity.provider_id};
        if (!announcement_guard.Acquired()) return;
        ProviderReadiness resumed = GetProviderReadiness(
            wallet, context, /*wait_for_sync=*/false);
        std::string announcement_error;
        if (!PublishCurrentProviderAnnouncement(
                wallet, context, resumed, announcement_now, nullptr,
                announcement_error)) {
            accept_new_requests = false;
            state = ProviderServiceState::WAITING_FOR_READINESS;
            first_error = StableProviderServiceError(
                announcement_error.empty()
                    ? "PAYMASTER_ANNOUNCEMENT_REJECTED"
                    : announcement_error);
        }
    }

    // A refill wait must not strand quote/recovery continuations. Fresh capacity
    // requests always require full current readiness in the request handler.
    // Drain-only providers still finish only already-authorized submissions.
    const auto work = PlanProviderWork({state, first_error}, accept_new_requests,
                                       AutomaticProviderStateIsSynchronized(wallet));
    if (work.request_continuations) {
        ProcessPaymasterRequests(context, wallet, ExecutionMode::AUTOMATIC,
            work.new_capacity ? ProviderRequestScope::NEW_AND_CONTINUING : ProviderRequestScope::CONTINUING_ONLY);
    }
    if (work.submits) {
        ProcessPaymasterSubmits(context, wallet, ExecutionMode::AUTOMATIC);
    }
    if (!first_error.empty() &&
        (previous.state != state || previous.last_error != first_error)) {
        // New blocks briefly put wallet/index notifications behind the tip.
        // Keep this expected wait observable without flooding the normal log.
        if (state == ProviderServiceState::WAITING_FOR_READINESS &&
            first_error == "PAYMASTER_PROVIDER_SYNCING") {
            LogPrint(BCLog::BENCH, "%s Paymaster automatic provider service waiting for synchronization: %s\n",
                     wallet.GetDisplayName(), first_error);
        } else if (state == ProviderServiceState::ACTIVE) {
            wallet.WalletLogPrintf("Paymaster reserve refill waiting while provider remains ready: %s\n", first_error);
        } else {
            wallet.WalletLogPrintf(
                "Paymaster automatic provider service paused: %s\n",
                first_error);
        }
    }
    if (previous.state == ProviderServiceState::DRAIN_ONLY &&
        state == ProviderServiceState::ACTIVE && first_error.empty()) {
        wallet.WalletLogPrintf("Paymaster automatic provider service resumed after temporary drain\n");
    }
    manager->SetProviderServiceStatus(wallet.GetName(), state,
                                      std::move(first_error));
}

} // namespace

bool RunPaymasterBackgroundWork(WalletContext& context, CWallet& wallet,
                               const std::function<void()>& work)
{
    using namespace DigiDollar::Paymaster;
    try {
        work();
        return true;
    } catch (const ApplicationError& failure) {
        BackgroundFailure(context, wallet, failure);
    } catch (...) {
        // Do not let one wallet stop the shared node scheduler. No action is
        // replayed here, and no diagnostic text is interpreted as authority.
        BackgroundFailure(context, wallet, ApplicationError{
            ErrorKind::INTERNAL, "PAYMASTER_AUTOMATIC_SERVICE_ERROR"});
    }
    return false;
}

void RunPaymasterProviderServiceCycle(WalletContext& context, CWallet& wallet)
{
    RunPaymasterBackgroundWork(context, wallet, [&] {
        RunPaymasterProviderServiceCycleImpl(context, wallet);
    });
}

} // namespace wallet
