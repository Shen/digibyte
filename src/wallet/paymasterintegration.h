// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERINTEGRATION_H
#define DIGIBYTE_WALLET_PAYMASTERINTEGRATION_H
#include <paymaster/types.h>
#include <wallet/paymasterpool.h>
#include <wallet/paymasterservice.h>
#include <cstddef>
#include <string>
class CScheduler;
namespace wallet {
class CWallet;
class PaymasterStore;
struct WalletContext;
/** Fail closed unless the provider-block record is definitively absent. A
 * malformed or unreadable record is a wallet database error, not evidence
 * that the provider is eligible. */
bool EnsureProviderNotEquivocationBlocked(
    PaymasterStore& store,
    const DigiDollar::Paymaster::PaymasterId& provider_id,
    std::string& error);

/** Persist every verifiable signed conflict currently waiting for this
 * wallet. This is invoked periodically so evidence does not depend on a user
 * opening an RPC or Qt flow inside the bounded network-inbox TTL. */
bool DrainPaymasterEquivocationInbox(WalletContext& context,
                                     CWallet& wallet,
                                     std::string& error);

/** Reconcile restartable paid maintenance and carrier-withdrawal records with
 * wallet transactions. Safe to invoke repeatedly at startup and at each
 * periodic wallet-maintenance tick. */
bool ReconcilePaymasterProviderMaintenance(CWallet& wallet,
                                           size_t& recovered,
                                           std::string& error);

/** Rebuild provider income and cost state from durable commits, maintenance
 * records, and current wallet transaction state. Safe across replay, reorg,
 * and restart. */
bool ReconcilePaymasterProviderFinances(CWallet& wallet,
                                        size_t& changed_events,
                                        std::string& error);

/** Restore client-facing DD send rows for successful Paymaster sessions made
 * by older wallet versions. This changes display history only; authoritative
 * session, transaction, reservation, and accounting records are untouched. */
bool ReconcilePaymasterClientHistory(CWallet& wallet, std::string& error);

/** Restore display history and previously authorized maintenance before service starts. */
void InitializePaymasterWallet(CWallet& wallet);

//! Reconcile expired Paymaster authority and durable final state for every
//! loaded wallet. Individual quote/capacity releases are atomic in
//! PaymasterStore; one wallet failure does not prevent maintenance of another.
void RunPeriodicPaymasterMaintenance(WalletContext& context, int64_t now);

//! Register the recurring Paymaster wallet-maintenance task.
void SchedulePeriodicPaymasterMaintenance(WalletContext& context,
                                          CScheduler& scheduler);

//! Run one bounded automatic provider-service cycle for every loaded wallet.
void RunPaymasterProviderServices(WalletContext& context);

//! Register the short, bounded automatic Paymaster provider worker.
void SchedulePaymasterProviderServices(WalletContext& context,
                                       CScheduler& scheduler);

} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERINTEGRATION_H
