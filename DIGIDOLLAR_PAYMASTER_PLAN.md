# DigiDollar Paymaster implementation plan and delivery status

**Reviewed:** 2026-09-11. **Source:** `bd270044c1`,
`feature/digidollar-paymaster-v1`. **Status:** implemented in the branch;
release acceptance remains open.

This plan describes delivered work packages and remaining acceptance work. It
supersedes the prospective plan and July-only status. Historical test counts
remain dated evidence in the [release gate](doc/digidollar-paymaster-release-gate.md).
This documentation update does not assert a new build, regression run or audit.

Start with [PAYMASTER.md](PAYMASTER.md), then read the
[design specification](DIGIDOLLAR_PAYMASTER_NETWORK_PROPOSAL_EN.md) and
[implementation reference](doc/digidollar-paymaster-implementation.md).

## 1. Delivery objective

Enable confirmed DD payments without client DGB by collaborating with a
provider supplying miner-fee inputs. Preserve local keys, ordinary DD validity,
explicit fee authorization, durable idempotency and recovery after signing.

Current scope includes user-paid fees, public/restricted sponsorship, discovery,
automatic/manual provider operation, finite budgets, liquidity maintenance,
finance reporting, recovery, RPC and Qt. Paymaster mint/redeem sponsorship and
multi-recipient sessions are not implemented.

## 2. Delivered work packages

“Implemented” means present and wired into the inspected source. It does not
mean independently audited or freshly verified on a release binary.

| Package | Current implementation | Acceptance focus |
|---|---|---|
| Transaction | Deterministic collaborative builder, checked cents/satoshis, carriers and existing output bounds. | Conservation, exact outputs, invalid prevouts and amount boundaries. |
| Signing | Trusted PSBT, explicit roles, default sighash and local client/provider manifests. | Mutated inputs/outputs/fees/policies and unauthorized signatures. |
| Wallet state | Atomic sessions, attempts, locks, signed PSBTs, exact commits, budget/replay/recovery records. | Failure at each write, restart, exact retry, no invented authority. |
| Provider | Descriptor identity, offers/sponsorship, separate admission/operational pools and finite safety budgets. | Resource ownership, concurrent last-slot/budget races and capability binding. |
| Network | Signed directory, isolated direct channels, V5 Capacity, bounded directional queues and persistent replay evidence. | Activation/transport, malformed messages, reconnect, equivocation and disclosure ordering. |
| Client | Offer filtering/ranking, idempotency, fee policy, explicit confirmation, exact-outflow and all-spendable-DD options. | Exact order, selected-provider-only effects, rounding gaps and changed sweep balance. |
| Recovery | Unsigned abandonment/fallback, exact signed retry, same-input return using own DGB or a distinct provider. | Post-signature fallback rejection, durable locks, recovery commitment and confirmation race. |
| Lifecycle | Automatic/manual service, optional autostart, successor reuse, paid target maintenance and preview-bound withdrawals. | Restart without duplicate work, pending targets, policy races and maintenance budgets. |
| Finance/retention | DD income/DGB cost ledger, principal separation, reorg reconciliation, backup acknowledgement and tombstones. | No duplicate accounting, rollback/reconfirmation and unresolved-authority retention. |
| RPC/Qt | Shared Core authority, snapshots/allowed actions, asynchronous UI and guided setup. | Stale-response rejection, fee/recovery confirmation and lock/unlock behavior. |
| Build/tests | Automake/MSVC, arithmetic portability, unit/wallet/Qt/functional coverage and sanitizer/fuzz workflows. | Selected-revision builds, regression and compatibility evidence. |

The implementation reference and
[repository map](REPO_MAP_DIGIDOLLAR.md#paymaster-network) identify each package,
including the current split wallet-store and RPC modules.

## 3. Review sequence

### A. Transaction and authorization

Read amount types, builder, PSBT validation and manifests first. Check a carrier
transfer by hand, then malformed creating transactions, output bounds, fee
rounding, roles and changed destinations. Each wallet must authorize its exact
outflows, inputs, returns and fee exposure independently.

### B. Persistence and failure boundaries

Trace client authorization into durable signed PSBT state and provider signing
into the exact final commit. Pool, budget and authority writes must be atomic
where required. Inspect restart before/after each write, wallet insertion and
broadcast boundary. Unreadable/old-format records are failures, not missing
authority that a remote message may reconstruct.

Distinguish immutable remote authorization from local chain observation: reorgs
can restore pending state without authorizing a different spend.

### C. Provider and discovery

Check admission/operational disjointness, authenticated Capacity before intent,
and exact quote/resource binding. Review sponsorship capabilities, finite
safety budgets, replay identities, work bounds, queue ownership and equivocation.

### D. Client, runtime and interfaces

Trace high-level `senddigidollar` and explicit quote/PSBT flows through common
validation. Check that a Paymaster call without `authorization_commitment`
only prepares authority and that the identical request must echo the exact
commitment before a new user signature. Verify five-argument compatibility and the narrow insufficient-DGB
condition for automatic fallback. Check separate authorization for client
signing, recovery, provider start, autostart and paid maintenance.

Inspect Qt using current Core snapshots and allowed actions. Include pool
maintenance/withdrawal previews and finance; reviewing only the send dialog
misses durable provider operations.

### E. Compatibility and wider branch

Review final transfers with old DD validators and old-only bridge behavior
separately from upgraded direct connectivity. Old wallet-file compatibility is
not migration support for old Paymaster record formats.

The branch includes portable 128-bit arithmetic in DCA/ERR/volatility and
build/Qt integration. Those changes require their own equivalence/regression
review. [PAYMASTER.md](PAYMASTER.md) provides initial commit navigation; later
commits must also be included when reviewing each final slice.

## 4. Recorded verification

| Date | Existing record | Scope limit |
|---|---|---|
| 2026-07-30 | MSVC Debug/Release, focused Paymaster/wallet checks, full 3,605-case core suites, eight native Qt suites and sanitizer/fuzz campaigns. | Dated candidate evidence, not an automatic pass of later commits. |
| 2026-08-08 | Windows/WSL multi-provider, fallback/contention and reorg checks; official-v9.26.5 wallet/validator compatibility. | Specific tests and candidate runs. |
| 2026-08-09 | Official old-only bridge and unchanged-datadir upgrade scenarios. | Specific compatibility scenarios, not full release approval. |
| 2026-09-11 | Documentation reconciled with `bd270044c1`. | Source/document review, no new executable test pass. |

The previous plan's “mixed-version compatibility entirely open” statement is
outdated. August evidence covers specific wallet, validator, relay and upgrade
scenarios. A selected release still requires suitable reruns after affected
changes. The release gate retains details and outstanding requirements.

## 5. Remaining acceptance work

1. Select the exact release source and binaries. Record toolchain/configuration,
   binary identity, commands, exit codes and retained output.
2. Re-run affected unit/wallet, functional and Qt surfaces after later changes.
   Repeat the multi-provider/failover/reorg group on the release build and
   include lifecycle, exact-outflow and current-only persistence behavior.
3. Complete the agreed platform, arithmetic-equivalence and compatibility
   regression matrix; repeat sanitizer/fuzz checks after relevant changes.
4. Test high privacy with real Tor/onion transport, stream isolation,
   capture/logging guards, one attempt and no clearnet/v1 fallback.
5. Obtain independent review of authority, persistence, concurrency and recovery;
   record findings and their disposition before release approval.
6. Update the release gate with exact evidence and an explicit decision.

A green subset, documentation update or configured CI workflow alone does not
close release acceptance. When behavior changes, update the design/reference,
operator guide, source-map entries and affected acceptance evidence together.
