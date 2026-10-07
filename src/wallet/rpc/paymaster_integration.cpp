// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Wallet lifecycle integration and durable equivocation maintenance. */

#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>

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
#include <paymaster/validation.h>
#include <random.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <scheduler.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>

#include <univalue.h>
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

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_rpc::internal;

namespace {
constexpr auto PAYMASTER_MAINTENANCE_INTERVAL{std::chrono::seconds{30}};
constexpr auto PAYMASTER_PROVIDER_SERVICE_INTERVAL{std::chrono::seconds{1}};
} // namespace

void InitializePaymasterWallet(CWallet& wallet)
{
    // Older Paymaster clients could persist only the wallet-owned DD
    // change row because collaborative sends bypassed the ordinary direct
    // send history path. Rebuild those display rows from successful,
    // durable sessions without changing transaction authority or funds.
    std::string client_history_error;
    if (!ReconcilePaymasterClientHistory(wallet,
                                         client_history_error)) {
        wallet.WalletLogPrintf(
            "Paymaster client-history startup reconciliation failed: %s\n",
            client_history_error);
    }
    // Reconstruct free, wallet-owned successor liquidity before automatic
    // service starts. This is idempotent and creates no transaction; paid
    // replenishment remains gated by the persisted maintenance policy.
    size_t recovered_successors{0};
    std::string successor_error;
    if (!PaymasterStore{wallet}.ReconcileProviderPoolSuccessors(
            recovered_successors, successor_error)) {
        wallet.WalletLogPrintf(
            "Paymaster pool-successor startup reconciliation failed: %s\n",
            successor_error);
    } else if (recovered_successors != 0) {
        wallet.WalletLogPrintf(
            "Recovered %u historical Paymaster pool successor(s) at wallet startup\n",
            static_cast<unsigned int>(recovered_successors));
    }
    size_t recovered_maintenance{0};
    std::string maintenance_error;
    if (!ReconcilePaymasterProviderMaintenance(
            wallet, recovered_maintenance, maintenance_error)) {
        wallet.WalletLogPrintf(
            "Paymaster maintenance startup reconciliation failed: %s\n",
            maintenance_error);
    }
}

void RunPeriodicPaymasterMaintenance(WalletContext& context, int64_t now)
{
    // Reconcile durable observations and continue explicitly authorized finite
    // setup. Elapsed time never authorizes discarding a signature, releasing an
    // ambiguous reservation, or exceeding an approved fee limit.
    bool all_equivocation_scans_succeeded{true};
    for (const std::shared_ptr<CWallet>& wallet : GetWallets(context)) {
        std::string error;
        if (!DrainPaymasterEquivocationInbox(context, *wallet, error)) {
            all_equivocation_scans_succeeded = false;
            wallet->WalletLogPrintf(
                "Paymaster equivocation-inbox maintenance failed: %s\n",
                error);
            error.clear();
        }
        if (!PaymasterStore{*wallet}.ReconcileFinalSessionsAtTip(now, error)) {
            wallet->WalletLogPrintf("Paymaster periodic maintenance failed: %s\n",
                                    error);
        }
        size_t recovered_maintenance{0};
        error.clear();
        if (!ReconcilePaymasterProviderMaintenance(
                *wallet, recovered_maintenance, error)) {
            wallet->WalletLogPrintf(
                "Paymaster liquidity maintenance reconciliation failed: %s\n",
                error);
        } else {
            RunPaymasterPoolPreparation(context, *wallet);
        }
    }
    // The manager is shared by all wallets. Expired signed artifacts can be
    // discarded only after every loaded wallet had an opportunity to persist
    // evidence; pruning inside the per-wallet loop could lose a conflict that
    // belongs to a later wallet.
    if (all_equivocation_scans_succeeded && context.paymaster) {
        context.paymaster->PruneDirectMessages(now);
    }
}

void SchedulePeriodicPaymasterMaintenance(WalletContext& context,
                                          CScheduler& scheduler)
{
    scheduler.scheduleEvery(
        [&context] { RunPeriodicPaymasterMaintenance(context, GetTime()); },
        PAYMASTER_MAINTENANCE_INTERVAL);
}

void RunPaymasterProviderServices(WalletContext& context)
{
    // Each wallet owns independent identity, liquidity, and ledgers. Iterating
    // loaded wallets here must not merge provider state merely because several
    // wallets share one node-level Paymaster transport manager.
    const auto wallets = GetWallets(context);
    if (wallets.empty()) return;
    // Each cycle processes at most one request and one submit. Bound the
    // node-wide scheduled work to four messages per tick, rotating wallets.
    const size_t count = std::min(size_t{2}, wallets.size());
    for (size_t i = 0; i < count; ++i) {
        context.paymaster_service_cursor %= wallets.size();
        RunPaymasterProviderServiceCycle(context, *wallets[context.paymaster_service_cursor++]);
    }
}

void SchedulePaymasterProviderServices(WalletContext& context,
                                       CScheduler& scheduler)
{
    scheduler.scheduleEvery(
        [&context] { RunPaymasterProviderServices(context); },
        PAYMASTER_PROVIDER_SERVICE_INTERVAL);
}

bool ReconcilePaymasterProviderMaintenance(CWallet& wallet,
                                           size_t& recovered,
                                           std::string& error)
{
    if (!ReconcileProviderMaintenance(wallet, recovered, error)) return false;
    size_t finance_changes{0};
    return ReconcileProviderFinances(wallet, finance_changes, error);
}

bool ReconcilePaymasterProviderFinances(CWallet& wallet,
                                        size_t& changed_events,
                                        std::string& error)
{
    return ReconcileProviderFinances(wallet, changed_events, error);
}

bool ReconcilePaymasterClientHistory(CWallet& wallet, std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    DigiDollarWallet* const dd_wallet = wallet.GetDDWallet();
    if (!dd_wallet) return true;

    PaymasterStore store{wallet};
    std::vector<PaymentSession> sessions;
    if (!store.ListClientSessions(sessions, error)) return false;

    for (const PaymentSession& session : sessions) {
        // Only a transaction already accepted by the wallet/node is a
        // completed send. Signed, ambiguous, failed, or safely canceled
        // sessions must not acquire an outgoing history row merely because
        // they contain durable final bytes.
        if (session.state != SessionState::STEMPOOL &&
            session.state != SessionState::MEMPOOL &&
            session.state != SessionState::CONFIRMED) {
            continue;
        }
        if (session.final_txid.IsNull()) continue;

        bool found_final_attempt{false};
        for (auto it = session.attempt_ids.rbegin();
             it != session.attempt_ids.rend(); ++it) {
            ProviderAttempt attempt;
            if (!store.GetAttempt(*it, attempt) ||
                attempt.final_txid != session.final_txid) {
                continue;
            }

            PaymentIntent intent;
            PaymasterQuote quote;
            CollaborativePSBTTemplate trusted_template;
            std::string artifact_error;
            if (!LoadAttemptAuthorizationArtifacts(
                    attempt, intent, quote, trusted_template,
                    artifact_error)) {
                error = artifact_error.empty()
                    ? "PAYMASTER_CLIENT_HISTORY_ARTIFACTS_INVALID"
                    : artifact_error;
                return false;
            }
            if (intent.recipient_amount.value <= 0 ||
                quote.service_fee.value < 0 ||
                intent.recipient_amount.value >
                    std::numeric_limits<CAmount>::max() -
                        quote.service_fee.value) {
                error = "PAYMASTER_CLIENT_HISTORY_AMOUNT_INVALID";
                return false;
            }

            if (!dd_wallet->RecordPaymasterSendHistory(
                    session.final_txid, intent.recipient_script,
                    intent.recipient_amount.value + quote.service_fee.value,
                    error)) {
                return false;
            }
            found_final_attempt = true;
            break;
        }

        if (!found_final_attempt) {
            error = "PAYMASTER_CLIENT_HISTORY_ATTEMPT_NOT_FOUND";
            return false;
        }
    }
    return true;
}

bool EnsureProviderNotEquivocationBlocked(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    std::string& error)
{
    DigiDollar::Paymaster::PaymasterProviderBlock block;
    const DatabaseReadStatus status =
        store.GetProviderBlock(provider_id, block, error);
    if (status == DatabaseReadStatus::READ_ERROR) return false;
    if (status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_PROVIDER_EQUIVOCATION_BLOCKED";
        return false;
    }

    DigiDollar::Paymaster::PaymasterEquivocationEvidence pending;
    const DatabaseReadStatus pending_status =
        store.GetPendingEquivocation(provider_id, pending, error);
    if (pending_status == DatabaseReadStatus::READ_ERROR) return false;
    if (pending_status == DatabaseReadStatus::FOUND) {
        error = "PAYMASTER_PROVIDER_EQUIVOCATION_PENDING";
        return false;
    }
    return true;
}

bool DrainPaymasterEquivocationInbox(WalletContext& context,
                                     CWallet& wallet,
                                     std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    PaymasterStore store{wallet};
    if (!store.PromotePendingEquivocations(error)) return false;
    if (!context.paymaster) return true;
    if (!context.paymaster->HasEquivocationCandidates()) {
        return true;
    }

    std::vector<PaymentSession> sessions;
    if (!store.ListClientSessions(sessions, error)) return false;
    std::string first_error;
    const auto remember_error = [&first_error](const std::string& candidate) {
        if (first_error.empty() && !candidate.empty()) {
            first_error = candidate;
        }
    };
    for (const PaymentSession& session : sessions) {
        for (const uint256& attempt_id : session.attempt_ids) {
            ProviderAttempt attempt;
            if (!store.GetAttempt(attempt_id, attempt)) {
                remember_error("PAYMASTER_ATTEMPT_NOT_FOUND");
                continue;
            }
            std::string attempt_error;
            if (!DrainClientAttemptEquivocations(
                    *context.paymaster, store, session, attempt, attempt_error,
                    EquivocationBlockPolicy::OBSERVE_ONLY,
                    /*lease_messages=*/false)) {
                remember_error(attempt_error);
            }
        }
    }

    std::vector<AlternativeRecoveryRecord> recoveries;
    if (!store.ListClientAlternativeRecoveries(recoveries, error)) {
        return false;
    }
    for (const AlternativeRecoveryRecord& recovery : recoveries) {
        std::string recovery_error;
        if (!DrainAlternativeRecoveryCapacityEquivocations(
                *context.paymaster, store, recovery, recovery_error,
                EquivocationBlockPolicy::OBSERVE_ONLY,
                /*lease_messages=*/false)) {
            remember_error(recovery_error);
        }
    }
    if (!first_error.empty()) {
        error = std::move(first_error);
        return false;
    }
    error.clear();
    return true;
}

} // namespace wallet
