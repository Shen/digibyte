# Paymaster release gate and evidence

Financial review checkpoint: source `e7c2c8c4bd`, 2026-10-15, on
`integration/paymaster-v9.26.7`. The [documentation index](../PAYMASTER.md) records
its later documentation/source review baseline. There is **no complete
current-revision release certification** in this record. Implemented behavior and selected regression
results must not be confused with release acceptance.

## Required candidate gates

| Gate | Required evidence |
|---|---|
| Product build | Clean app, daemon, CLI and test builds for supported platforms; exact revision/toolchain. |
| Core invariants | Exact transaction/fee/signature, budgets, replay, recovery and persistence regression groups. |
| Lifecycle | Encryption/unlock, startup/history readiness, shutdown/restart, backup/adoption/rollback and retirement. |
| Liquidity | Finite versus recurring approvals, whole-slot refill for both assets, live capacity below target and fee caps. |
| End-to-end clients | Exact authorization, cancellation, response loss, failover, reorg and confirmed-recipient-only success. |
| GUI and CLI | Shared settings/approval semantics, async readiness, consistent balances/offers and headless maintenance. |
| Compatibility | Ordinary DGB/DD paths, feature disabled, historical wallet/node interoperability. |
| Adversarial operation | Sanitizers/fuzz/load, real Tor reachability and prolonged operation, hostile relay. |
| Durability and review | Physical storage failure coverage and independent review of financial/lifecycle boundaries. |

Use the [runbook](digidollar-paymaster-testing.md) for commands and the
[threat model](paymaster-threat-model.md) for residual risks. Record a pass only
against the candidate and built executable. Remaining tests are not waived by
successful manual regtest payments.

## Earlier acceptance IDs: current interpretation

The former V1 list is retained here as a review mapping, not a passed matrix.
Items superseded by later implementation decisions are identified explicitly.

| Earlier IDs | Current requirement / disposition |
|---|---|
| 1, 2, 6 | Confirmed DD payment without client DGB; no key transfer; each party signs only authorized owned inputs. |
| 3 | Separate admission/operational resources. The old three-admission-slot readiness minimum is superseded: confirmed usable capacity remains serviceable below the refill target. |
| 4, 5, 24 | Valid carrier outputs for small fees, bounded integer fee/exact-total arithmetic and one recipient with valid DD output bounds. |
| 7, 8, 12, 21, 23 | Durable explicit authorization, same-input retry, fallback only before user PSBT, protected signed ambiguity and exact crash recovery. |
| 9, 10, 13 | Existing validators/consensus remain authoritative; existing five-argument send retains wallet-funded DGB behavior. Review arithmetic portability separately. |
| 11 | Local reputation cannot override identity, liquidity, privacy or authorization checks. |
| 14, 15 | Public sponsorship is implemented; fresh restricted sponsorship is disabled. Legacy restricted bindings/consumption remain for accepted work and recovery, not new availability. |
| 16, 17, 18, 20 | Fresh internal change, metadata-safe logging, selected-provider disclosure and high-privacy onion isolation/one attempt/no silent downgrade; no anonymity promise. |
| 19 | Eligible detail pruning retains permanent request/conflict protection; missing evidence must not be reconstructed as new authority. |
| 22, 26 | Exact final bytes and authority/accounting persist atomically before broadcast; signed-result authority is monotonic. |
| 25 | Unpruned synchronized index/history and creating transactions; ordinary validation needs no Paymaster feature, discovery needs upgraded peers. |
| 27 | Only validated locally confirmed recipient payment is success; terminal/mempool/recovery state alone is insufficient. |
| 28 | Locking pauses the same session; active models require finite safety limits and positive absolute network-fee ceilings. |

## Historical checkpoints

These summarize previous records retained in Git. They are deliberately not
summed into a current test total. Counts can overlap and some runs used isolated
executables rather than normal product builds.

| Checkpoint | Recorded evidence | Limit |
|---|---|---|
| 2026-09-27, `1789c803be` | RC2 selected unit group: 284 cases / 10,767 assertions; nine functional tests. | Older branch/baseline; does not certify later setup, security or v9.26.7 work. |
| 2026-10-06, `e5cfa2da3e` | Twelve translation units; 26 selected Core/Qt cases. | Incremental/isolated test executables; no normal app/daemon/CLI link or functional execution at that checkpoint. |
| 2026-10-10 retention corrections | Focused 3 cases / 8,539 assertions; related selection 48 / 10,012 including the focused cases. | Isolated MSVC executable; quota fixture accounts for many assertions. |
| 2026-10-10 recovery-fee correction | Focused 5 cases / 384 assertions; related 28 / 1,542 including them. | Selected tests, not full product release verification. |
| 2026-10-15, `01aaf13df9` | Seven translation units and busy-wallet Qt regression. | Selected build/test; not complete GUI acceptance. |
| 2026-10-15, `e7c2c8c4bd` | Two translation units, targeted Qt regression, German translations and two screenshot sizes. | Sync-presentation checks only; no Paymaster release certification. |

The detailed security record also contains real SQLite stale-backup and channel
binding reproductions, simulated DB transaction failures and SQLite page-cap/
commit-denial reopen tests. See TM-001 through TM-009 in the threat model.
Do not describe those as physical power-loss/ENOSPC coverage.

## Open boundaries

A matching wallet/checkpoint rollback, independent cloned stores, public-service
Sybil drain and traffic analysis remain explicit trust/operational limits.
Current full builds/suites, broad Qt regression results, independent review,
physical durability, real Tor and prolonged hostile-load acceptance need their
own evidence. Old upstream release results do not certify this integration.

## Traceability

Full earlier reports, test commands and work logs remain at the consolidation
parent; for example:

```sh
git show e7c2c8c4bd:doc/digidollar-paymaster-release-gate.md
git show e7c2c8c4bd:doc/digidollar-paymaster-testing.md
git show e7c2c8c4bd:doc/paymaster-threat-model.md
```

The former numbered design acceptance list is consolidated into the candidate
gates above; it is not silently marked complete. Update this document with
bounded revision-specific evidence rather than appending every local work log.
