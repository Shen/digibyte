# Paymaster threat model and security review index

Scope: the implementation described in [PAYMASTER.md](../PAYMASTER.md).
This is a source-grounded review index, not independent certification or a
claim that all tests passed at HEAD. See [release evidence](digidollar-paymaster-release-gate.md).

## Assets, actors and trust boundaries

Protect client DD, provider DGB/DD, private keys, explicit fee authority,
spending budgets, durable request identity and recovery evidence. Either remote
party, directory peer or network relay can be malicious. Treat RPC clients,
wallet backups and local storage as separate boundaries; authenticated local
RPC is not a public payment API.

The client trusts local validation, not provider claims of payment success.
The provider validates exact transaction intent and its own exposure rather
than trusting client PSBT metadata. Encrypted transport is not provider identity.
The GUI cannot grant authority by displaying an offer or clearing a form.

## Required invariants and review locations

| Invariant | Implementation / verification focus |
|---|---|
| No extra client spend or recipient substitution | Exact manifests, commitments, `txbuilder`, `psbt`, wallet PSBT/finalization tests. |
| No provider signature outside approved terms | Provider policy, admission, input ownership and exact final transaction validation. |
| Fee and budget enforcement survives retry | Client/provider ledgers, retained commit authority, store/security tests. |
| Capacity proof before sensitive intent | Protocol, channel authentication, transport/admission tests. |
| Replays do not create another payment | Canonical request IDs, atomic store transitions, send/recovery tests. |
| Timeout does not release signed exposure | Reservations, retained bytes, chain reconciliation, reorg/lifecycle tests. |
| Work and storage are bounded | Wire size, inbox/queue limits, session cap, eligible unsigned pruning. |
| Wallet lifecycle does not reset authority | Restore guard, independent checkpoint, backup/adoption/rollback tests. |
| Setup/refill spends only approved funds/fees | Shared setup, finite commands, maintenance limits, whole-slot refill tests. |
| Ordinary DigiByte checks remain intact | Integration boundaries and ordinary DGB/DD regression coverage. |

These consolidate the earlier H1-H8 hardening work packages. Consensus/script
checks, wallet history/index readiness and final transaction validation are
mandatory. Performance or UI changes must not bypass them.

## Signing, accounting and recovery details

Capacity validation binds genesis, provider/session/request, nonce, reference
block, expiry, resource-control proofs and creating transactions to live inputs.
The later quote must match the persisted resource snapshot and funding roles.
Channel authentication is an additional transport binding, not a replacement
for financial capacity validation.

Local manifests bind exact inputs/destinations, fees, policy, capacity and
transaction template. Signing is role-scoped and requires `SIGHASH_DEFAULT`;
unknown/substituted fields fail closed. Without an accepted exact commitment,
high-level calls remain preparation-only regardless of the `prepare_only` flag.
Provider final bytes, authority, accounting and pool state commit before broadcast.
Clients validate txid/wtxid, all inputs/outputs/witnesses and actual-prevout
script execution against the trusted PSBT, with applicable mempool preflight.
Late negative results cannot overwrite a final commit or release its authority.

Provider limits bound per-transaction, reserved and rolling spending/completions
as well as request counts. Paid maintenance has separate explicit approval.
Client outstanding fee reservations continue to count even after 24 hours;
spent fees age out of the daily window. RPC options cannot raise wallet limits.
These limits do not cap recipient spending or implement an agent-wide budget.
Accounting uses durable atomic transitions and monotonic accounting time.

Ordinary provider fallback ends before a user PSBT exists and preserves client
inputs. `retry_same` creates no new quote, attempt or signature. Authorized
`cancel_to_self` can use a distinct eligible user-paid provider, with inherited
privacy requirements and exact original client inputs/fresh wallet-owned returns.
It needs explicit acceptance of its own recovery commitment. Without suitable
DGB or a reachable eligible provider, recovery can remain delayed.

Conflicting signed capacity/quote claims produce durable evidence and local
provider blocking. A staged first signed claim is evidence, not spending
authority. Database/local-authority failure is distinct from an invalid remote
artifact and must not acknowledge work as successfully consumed. Queue leases
protect ownership across persistence operations. Ordinary coin selection still
protects pool/session inputs when safety state cannot be read.

## Recorded findings and disposition

The identifiers below preserve traceability to the detailed pre-consolidation
review at `e7c2c8c4bd:doc/paymaster-threat-model.md`. Corrections are source
changes with selected regression evidence, not proof against every variant.

| ID | Finding / result | Current handling and remaining limit |
|---|---|---|
| TM-001 | Rolling budget pruning invalidated evidence needed by retained signed commits. | Retained exact commit authority supports historical execution/retry without new approval or recreated budget. |
| TM-002 | Expired unsigned provider history could grow without bound. | 8,192 retained-session admission cap; bounded eligible pruning (64/pass), protected signed/ambiguous history retained. |
| TM-003 | Stale backup could forget signed liabilities and reset spending budgets; reproduced using SQLite/regtest. | Restore guard before load callbacks and independent checkpoint; reviewed adoption only. Rolling back wallet and matching checkpoint together remains outside protection. |
| TM-004 | Two encrypted relay legs could forward authentic messages while exposing payment metadata. | Identity/session/genesis/nonce-bound channel proof before metadata. Real encrypted two-leg test exists; full hostile TCP relay and traffic-analysis coverage remain open. |
| TM-005 | Mutation and budget-race scenarios. | Selected defensive tests; no general claim that all races are excluded. |
| TM-006 | Lost replies, replay and reorganization. | Durable exact replay and local reconciliation; signed recovery exposure retained through 240 confirmations. Deeper/pruned history is not an unlimited guarantee. |
| TM-007 | Storage/durability failures. | Simulated transaction failures and real SQLite page-cap/commit-denial reopen checks; physical ENOSPC, fsync, torn-write and power-loss coverage remain open. |
| TM-008 | Public sponsorship budget drain. | Public callers may consume intentionally authorized budgets; caps/admission bound exposure, not Sybil abuse. |
| TM-009 | Parser/injection robustness. | Selected mutation checks found no additional injection; fresh sanitizer, fuzz and load evidence still required. |

A related recovery correction retains fee exposure after shallow cancel-to-self
confirmation until reorg safety depth, repairs retained liabilities before new
approvals and accounts once for an original payment that wins. Do not release
budget solely because a competing recovery briefly confirmed.

## Residual operational risks

- The operating unlock is wallet-wide and lasts until locking/unload/shutdown;
  it is not a Paymaster-only key permission. RPC access must remain protected.
- Wallet encryption protects signing keys, not every payment-intent record or
  log field. Backups/logs can expose metadata.
- Independent checkpoints do not defeat rollback of the whole data directory
  or coordinated wallet/checkpoint snapshots. Cloned provider stores must not
  operate independently under one identity.
- Announcements can outlive capacity. Transport failure or quote expiry does
  not establish that no transaction was signed/broadcast.
- Public sponsorship intentionally spends provider DGB. Fee limits and request
  limits are economic controls, not identity or anti-Sybil guarantees.
- Tor hides some routing information; it does not guarantee anonymity against
  traffic analysis or a compromised endpoint.
- Recovery transactions can race the original and need confirmations. A local
  cancellation action cannot revoke an already valid remote signature.

New restricted sponsorship is disabled. Legacy artifact readers/recovery support
must not be confused with permission for fresh restricted signing.

## Evidence discipline

For each security change record commit, compiler/build configuration, exact test
selection, result and remaining limits. A selected translation-unit build or
isolated test executable is not a full product build. Assertions from seeded
quota fixtures are not independent security cases. Preserve failure reproductions
and compare them with the corrected behavior. Follow the
[test runbook](digidollar-paymaster-testing.md) and outstanding release gates.
