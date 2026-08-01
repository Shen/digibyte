// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Durable provider-commit recovery and wallet transaction integration. */

#ifndef DIGIBYTE_WALLET_PAYMASTERPROVIDER_H
#define DIGIBYTE_WALLET_PAYMASTERPROVIDER_H

#include <paymaster/directory.h>
#include <paymaster/provider.h>
#include <paymaster/reservation.h>
#include <paymaster/sponsorship.h>
#include <paymaster/wire.h>
#include <primitives/transaction.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wallet {

class CWallet;

/** Result of revalidating and replaying one already durable, fully signed
 * provider commit. Recovery never creates a new signature or starts the
 * provider runtime. */
struct DurablePaymasterCommitRecovery {
    bool broadcast{false};
    bool already_confirmed{false};
    std::string error;
};

/** Aggregate startup-recovery result. Errors contain stable error codes only;
 * transaction, request, session and provider identifiers are deliberately not
 * exposed to ordinary logs. */
struct DurablePaymasterRecoveryReport {
    size_t candidates{0};
    size_t recovered{0};
    size_t already_confirmed{0};
    std::vector<std::string> errors;
};

/** How the exact-final preflight may interact with txindex. Wallet-load and
 * periodic recovery must never block startup, while an explicit RPC may wait
 * for the already-required index to catch up. */
enum class ExactFinalTxIndexMode {
    NONBLOCKING,
    WAIT_FOR_SYNC,
};

enum class ExactFinalTransactionPresence {
    NONE,
    MEMPOOL,
    STEMPOOL,
    CONFIRMED,
};

/** Authoritative result of checking an exact final transaction against all
 * local transaction locations and the mempool policy engine. The full raw
 * transaction, txid and wtxid are compared whenever the txid is already
 * known. */
struct ExactFinalTransactionPreflight {
    ExactFinalTransactionPresence presence{
        ExactFinalTransactionPresence::NONE};
    uint256 confirmed_block;
    int confirmed_height{-1};
    int confirmed_position{-1};
    int confirmation_depth{0};
};

/** Shared final-transaction firewall used by RPC, recovery and background
 * processing. A same-txid/different-witness entry is always a hard conflict;
 * an otherwise unknown transaction must pass test-accept. */
bool PreflightExactPaymasterFinalTransaction(
    CWallet& wallet,
    const CTransactionRef& transaction,
    ExactFinalTxIndexMode txindex_mode,
    ExactFinalTransactionPreflight& result,
    std::string& error);

/** Return true only for local infrastructure failures that do not prove the
 * exact durable final transaction is invalid or conflicting. Callers must
 * leave the durable authorization retryable for these failures. */
bool IsTransientPaymasterFinalizationError(std::string_view error);

/** Insert and broadcast one exact, fully-authorized Paymaster transaction.
 * Existing wallet, mempool, stempool, and confirmed entries with the same
 * txid must have identical witness bytes. The mempool/stempool comparison is
 * held through broadcast under cs_main, closing the different-witness race. */
bool InsertAndBroadcastExactPaymasterTransaction(
    CWallet& wallet,
    const CTransactionRef& transaction,
    bool& already_confirmed,
    std::string& error);

/** Revalidate and insert/broadcast the exact transaction in a durable provider
 * commit. Quote/retry expiry is not an authorization revocation mechanism and
 * therefore never suppresses exact recovery. */
DurablePaymasterCommitRecovery RecoverDurablePaymasterCommit(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCommitRecord& commit,
    int64_t now);

/** Best-effort wallet-load recovery for every durable provider commit and for
 * each exact V14 PROVIDER_SIGNED attempt whose atomic commit was interrupted.
 * Promotion consumes only the already identity-signed private result envelope
 * and never accesses a signing key. This is intentionally independent from
 * startpaymaster and never publishes an announcement or changes the ephemeral
 * provider-running state. */
DurablePaymasterRecoveryReport RecoverDurablePaymasterCommits(
    CWallet& wallet,
    int64_t now);

bool SetPaymasterProviderPolicy(CWallet& wallet,
                                const DigiDollar::Paymaster::ProviderPolicy& policy,
                                int64_t now,
                                std::string& error);

bool SetPaymasterProviderEnabled(CWallet& wallet,
                                 bool enabled,
                                 int64_t now,
                                 std::string& error);

/** Persist provider runtime behavior. Runtime mode changes are policy-neutral:
 * callers are responsible for requiring a stopped provider before changing
 * the mode. Autostart never stores or obtains a wallet passphrase. */
bool SetPaymasterProviderRuntimeSettings(
    CWallet& wallet,
    DigiDollar::Paymaster::ProviderOperationMode operation_mode,
    bool autostart,
    int64_t now,
    std::string& error);

bool GetPaymasterProviderPolicy(const CWallet& wallet,
                                DigiDollar::Paymaster::ProviderPolicy& policy);

bool GetPaymasterProviderSettings(const CWallet& wallet,
                                  DigiDollar::Paymaster::ProviderSettings& settings);

/** Persist the wallet-local provider loss ceilings and initialize the
 * monotonic budget ledger. The policy is never advertised to peers. */
bool SetPaymasterProviderSafetyPolicy(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy,
    int64_t now,
    std::string& error);

bool GetPaymasterProviderSafetyPolicy(
    const CWallet& wallet,
    DigiDollar::Paymaster::ProviderSafetyPolicy& policy);

bool GetPaymasterProviderBudgetLedger(
    const CWallet& wallet,
    DigiDollar::Paymaster::ProviderBudgetLedger& ledger);

/** Persist automatic liquidity targets and finite maintenance budgets. The
 * approval bit is the operator's one-time consent to paid replenishment. */
bool SetPaymasterProviderLiquidityPolicy(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    int64_t now,
    std::string& error);

bool GetPaymasterProviderLiquidityPolicy(
    const CWallet& wallet,
    DigiDollar::Paymaster::ProviderLiquidityPolicy& policy);

bool GetPaymasterProviderMaintenanceLedger(
    const CWallet& wallet,
    DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger);

/** Persist the wallet-local client service-fee ceilings and initialize its
 * durable rolling-day ledger. */
bool SetPaymasterClientSafetyPolicy(
    CWallet& wallet,
    const DigiDollar::Paymaster::ClientSafetyPolicy& policy,
    int64_t now,
    std::string& error);

bool GetPaymasterClientSafetyPolicy(
    const CWallet& wallet,
    DigiDollar::Paymaster::ClientSafetyPolicy& policy);

bool GetPaymasterClientFeeLedger(
    const CWallet& wallet,
    DigiDollar::Paymaster::ClientFeeLedger& ledger);

bool SetPaymasterProviderPoolEntries(
    CWallet& wallet,
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error);

bool GetPaymasterProviderPoolEntries(
    const CWallet& wallet,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries);

/** Atomically reserve at most one operational DGB entry and, when required,
 * one carrier. Repeating the same reservation id returns the exact same slot.
 */
bool ReservePaymasterOperationalSlot(
    CWallet& wallet,
    const uint256& reservation_id,
    bool require_carrier,
    int32_t tip_height,
    int32_t minimum_confirmations,
    int64_t now,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& reserved,
    std::string& error);

/** Build the short-lived, provider-authenticated proof for one validated
 * PMCAPREQ. The supplied entries must be a confirmed operational slot that is
 * either AVAILABLE or already RESERVED by request.client_nonce. This helper
 * does not reserve the entries;
 * callers must persist the snapshot-to-quote binding atomically before
 * disclosing any client payment intent. */
bool BuildPaymasterCapacityProof(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderIdentityRecord& identity,
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request,
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& selected_entries,
    const uint256& expected_genesis,
    const uint256& reference_block,
    int64_t now,
    DigiDollar::Paymaster::PaymasterCapacityProof& proof,
    std::string& error);

/** Atomically bind one confirmed operational slot to the canonical request
 * hash and request.client_nonce, then persist the exact signed response in the
 * same database transaction. canonical_netgroup is transient
 * NetGroupManager output and is wallet-HMACed before any database write. An
 * exact retry returns the byte-identical stored proof; reusing a nonce for
 * different request content is a conflict. */
bool ReserveAndBuildPaymasterCapacityProof(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderIdentityRecord& identity,
    const DigiDollar::Paymaster::PaymasterCapacityRequest& request,
    const uint256& expected_genesis,
    const uint256& reference_block,
    int32_t tip_height,
    int32_t minimum_confirmations,
    const std::vector<unsigned char>& canonical_netgroup,
    int64_t now,
    DigiDollar::Paymaster::PaymasterCapacityProof& proof,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& reserved,
    std::string& error);

/** Build and sign the provider's public discovery record from confirmed,
 * wallet-owned admission pool entries. Allocating its monotonic sequence is
 * durable; a later failure may skip a sequence but can never reuse one.
 */
bool BuildPaymasterAnnouncement(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderIdentityRecord& identity,
    const DigiDollar::Paymaster::ProviderPolicy& policy,
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& pool_entries,
    const CService& endpoint,
    const uint256& genesis_hash,
    const uint256& reference_block,
    int64_t now,
    uint8_t permitted_funding_models,
    DigiDollar::Paymaster::Announcement& announcement,
    std::string& error);

/** Build a provider-signed, non-gossiped restricted service descriptor. */
bool BuildRestrictedServiceDescriptor(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderIdentityRecord& identity,
    const DigiDollar::Paymaster::ProviderPolicy& policy,
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& pool_entries,
    const CService& endpoint,
    const XOnlyPubKey& sponsor_authorization_key,
    const std::string& sponsor_display_name,
    const uint256& genesis_hash,
    const uint256& reference_block,
    int64_t now,
    int64_t expires_at,
    DigiDollar::Paymaster::RestrictedServiceDescriptor& descriptor,
    std::string& error);

} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERPROVIDER_H
