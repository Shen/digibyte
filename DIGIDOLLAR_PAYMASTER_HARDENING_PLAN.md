# DigiDollar Paymaster hardening plan and review requirements

**Source:** `bd270044c1`, `feature/digidollar-paymaster-v1`.
**Reconciled:** 2026-09-11. **Status:** controls implemented in this branch;
independent review and release acceptance remain open.

This English plan replaces the July hardening notes and outdated compatibility
status. It is a current control/verification map, not a new security audit or
formal proof. Read the [implementation reference](doc/digidollar-paymaster-implementation.md)
for flows and the [release gate](doc/digidollar-paymaster-release-gate.md) for
dated evidence and remaining acceptance.

## 1. Threat model and invariant

The honest party's local software, wallet and keys are trusted. The remote
client/provider can modify messages, lie about resources, reconnect, replay,
equivocate, delay or disappear. Peers can flood discovery/direct paths and a
crash can interrupt any persistence, signing or broadcast boundary.

Before a wallet signs, every movement of its DD/DGB must match local persisted
authorization. Retry/result processing cannot create fresh spending authority.
An identity signature and encrypted transport do not substitute for validation.
Public sponsorship is financially bounded, not Sybil-fair. Local compromise,
key theft and malicious release artifacts are outside this counterparty model.

## 2. H1: authenticated Capacity before disclosure

**Implemented:** V5 `PMCAPREQ`/`PMCAPRESP` verifies genesis, provider identity,
request/session, nonce, reference block, expiry, BIP86 control proofs, creating
transactions and live resources before intent, DD outpoints or restricted
capability are disclosed. The later quote must match the persisted snapshot's
exact resource set and funding roles.

**Review:** foreign/spent/stale resources, invalid proofs, changed resources,
cross-session replay, old-version downgrade and early disclosure.

**Source:** [wire.cpp](src/paymaster/wire.cpp),
[protocol.cpp](src/paymaster/protocol.cpp),
[validation.cpp](src/paymaster/validation.cpp),
[paymaster_discovery.cpp](src/wallet/rpc/paymaster_discovery.cpp).

## 3. H2: independent local authorization

**Implemented:** client/provider manifests bind exact inputs, destinations,
fees, offer/policy, capacity and template. Client authority includes the
canonical order and requested fee/privacy/selection modes. Provider authority
names the exact budget reservation/commit key. Signing is role-scoped and
requires `SIGHASH_DEFAULT`; unknown fields and substituted outputs fail closed.

New provider signatures require current local safety policy. Already durable
exact commits retain their historical non-null policy/SPENT-ledger binding for
safe completion. Retry never reconstructs missing ledger authority. RPC, Qt,
manual processing and automatic service share these checks.

**Review:** mutate inputs/outputs/fees/change, roles, PSBT fields, sighash,
request options, offers, policies and commitments; test policy changes before
signing and after final commit. High-level Paymaster calls must remain
prepare-only without an exact `authorization_commitment`, regardless of the
`prepare_only` flag. Verify the explicit second-step acceptance for RPC and Qt.

**Source:** [psbt.cpp](src/paymaster/psbt.cpp),
[reservation.h](src/paymaster/reservation.h),
[paymasterpsbt.cpp](src/wallet/paymasterpsbt.cpp),
[paymaster_processing.cpp](src/wallet/rpc/paymaster_processing.cpp).

## 4. H3: exact final validation and atomic commit

**Implemented:** provider final bytes, authority, budget/result and pool state
commit before broadcast. The client validates raw/non-witness transaction,
txid, wtxid, every input/output/witness and actual-prevout script execution
against the trusted PSBT, with mempool preflight when applicable.

Late negative results cannot replace a final commit or release its authority.
Local chain reconciliation separately rolls confirmation, finance and pool
availability back on reorg. Mempool/stempool is not confirmation.

**Review:** invalid witnesses, same txid/different witness, conflicts, late
results, every atomic write/commit failure, restart around wallet insertion and
broadcast, exact commit restoration and reorg without duplicate finance events.

**Source:** [paymasterstore_finalization.cpp](src/wallet/paymasterstore_finalization.cpp),
[paymasterstore_reconciliation.cpp](src/wallet/paymasterstore_reconciliation.cpp),
[paymaster_processing.cpp](src/wallet/rpc/paymaster_processing.cpp).

## 5. H4 and H5: finite economic exposure

**Implemented:** provider policies bound transaction fees, concurrently reserved
fees, rolling hour/day spending/completions and active-quote/request counts.
Quote reservation and final expenditure use atomic durable accounting and a
monotonic accounting time. Network-group and pseudonymous recipient buckets
limit repeated demand without raw IPs as persistent rate-limit keys.

Client policy bounds per-transfer/rolling-day service fees; RPC cannot raise
wallet limits. Fee reservations survive concurrency/restart. A valid all-zero
configuration disables an unused provider model; missing or partial limits do
not mean unlimited service. Paid maintenance has separate finite budgets and
explicit approval. Pool principal is distinct from profit.

**Review:** last-slot/budget races, clock rollback, concurrent quotes, policy
changes, rollback/expiry/restart, public-budget consumption, restricted
capability reuse and duplicate accounting.

**Source:** [provider.cpp](src/paymaster/provider.cpp),
[paymasterstore_provider.cpp](src/wallet/paymasterstore_provider.cpp),
[paymasterprovider.cpp](src/wallet/paymasterprovider.cpp),
[paymaster_provider.cpp](src/wallet/rpc/paymaster_provider.cpp).

## 6. H6: work bounds, replay and evidence

**Implemented:** cheap size/count/time/rate checks precede costly work. Queues
are bounded/directional and semantic replay identities survive peer changes.
Exact retries are idempotent; contradictory signed Capacity/quote claims cause
durable evidence and local provider blocking.

The first valid signed claim is staged atomically as evidence before deeper
validation/acknowledgement; it is not spending authority. Database or local
authority failures leave work unacknowledged and distinct from invalid remote
artifacts. Generation-bound leases protect queue ownership across database
operations. Time arithmetic saturates at integer bounds.

**Review:** reconnect/restart replay, contradictory concurrent claims, expiry
overflow, starvation, overlapping leases/ABA, failed persistence and retry,
cross-session bindings and floods before signature validation.

**Source:** [manager.cpp](src/paymaster/manager.cpp),
[wire.cpp](src/paymaster/wire.cpp),
[paymaster_integration.cpp](src/wallet/rpc/paymaster_integration.cpp),
[paymasterstore_reputation.cpp](src/wallet/paymasterstore_reputation.cpp).

## 7. H7: recovery without client DGB

**Implemented:** ordinary fallback is allowed only before a user PSBT exists.
Signed sessions retain inputs across timeouts. `retry_same` creates no quote,
attempt or signature. `cancel_to_self` can use a distinct eligible user-paid
provider passing V5 Capacity and inherited privacy requirements.

The recovery manifest binds original client inputs, fresh wallet-owned returns,
bounded service fee and validated provider resources. The client must explicitly
authorize the exact recovery commitment before signing. Confirmation of the
original or recovery spend determines the outcome; mempool cancellation does
not erase already signed authority.

**Review:** post-signature fallback, original-provider reuse, wrong return
scripts, excessive fees, changed inputs, original/recovery races, restart,
exact retry and lack of an available no-client-DGB recovery path.

**Source:** [recovery.cpp](src/paymaster/recovery.cpp),
[paymaster_client.cpp](src/wallet/rpc/paymaster_client.cpp),
[paymasterstore_recovery.cpp](src/wallet/paymasterstore_recovery.cpp).

## 8. H8 and current lifecycle coverage

Unit/wallet tests cover protocol, builder, PSBT, authority and persistence
failure injection. Functional tests cover payments, discovery, readiness,
maintenance, fallback/contention and reorg; Qt tests cover shared workflow
authority. The [security workflow](.github/workflows/paymaster-security.yml)
defines wire, stateful-security, pool-lifecycle and persistence fuzz targets.

Test/workflow presence is coverage, not a successful run on the release head.
July broad-matrix and August compatibility runs remain dated evidence. Repeat
affected surfaces after source/build/dependency changes with exact revision and
binary identity retained.

Current review must include preview-bound pool operations/withdrawals,
successor provenance/pending capacity, maintenance budgets, finance rollback,
authoritative GUI snapshots and current-only record decode errors. Ordinary
coin selection must protect Paymaster reservations and provider pools even
when safety state is unreadable.

## 9. Privacy, retention and unavoidable limits

The provider can retain the transaction it signs; confirmed transactions are
public. High privacy requires Tor/onion isolation but its real deployment
acceptance remains open. Wallet encryption protects keys, not every persisted
endpoint, request, manifest, PSBT, final transaction or timestamp. Restricted
capability secrets are removed before request persistence.

Retention compacts eligible completed records after the safety depth while
preserving idempotency and unresolved authority. There is no destructive reset
or implicit record-format migration. Ordinary old-wallet compatibility is a
separate question from older experimental Paymaster records.

A provider can censor/delay, double-promise resources to isolated clients,
consume allowed sponsorship budgets or later broadcast already authorized
bytes. Those risks do not permit different client outflows. Without client DGB
or a reachable eligible independent provider, recovery may be delayed.

## 10. Remaining acceptance

1. Bind results to the selected source/binaries and repeat affected local,
   functional, Qt and sanitizer/fuzz checks after later changes.
2. Retain and, where affected, repeat official-v9.26.5 compatibility evidence
   from 2026-08-08/09; it is no longer accurate to call compatibility untested.
3. Complete real-Tor checks with isolation, capture/logging guards, one attempt
   and no clearnet/v1 downgrade.
4. Complete or explicitly disposition independent review of authority,
   concurrent persistence and recovery, including the wider branch's separate
   arithmetic portability changes.

The release gate is the status record. The supported claim is “implemented
with dated test evidence; release acceptance open,” not formal or audited safety.
