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

## Focused regression follow-up, 2026-10-17

Source changes: `c7750a18a1` (maintenance accounting time) and `0ab4289107`
(functional expectations and coverage). The three failures reported from another
node were reproduced locally before correction:

- Full pool setup/capital release: maintenance reconciliation used wall time
  below a saved record's creation time after policy revisions. Preserve the
  record/ledger accounting floor so confirmed work no longer remains open.
  The strict guard for genuinely unfinished or signed work stays in place.
- Lifecycle target expansion and provider replacement: tests waited for later
  batches without confirming pending reserve outputs. They now verify the
  separate confirmation stages, restart idempotency and absence of duplicates.
  Further stale expectations were corrected for active service below its refill
  target and recovery exposure retained until 240 confirmations. A policy-change
  test retries only the transient `PAYMASTER_PROVIDER_BUSY` response.

Verification: two changed translation units compiled with MSVC 14.43; the new
wallet regression failed against the previous implementation and passed after
the fix. Ten selected wallet cases passed (455 assertions). Full descriptor
runs of `wallet_paymaster_pool_setup.py`, `wallet_paymaster_lifecycle.py` and
`wallet_paymaster_provider.py` passed with the reported seeds
`6185203042326809576`, `5495766456681726476` and `8132640900451697906`, respectively.
The previously open pool-setup failure in the historical rows below is resolved
by this correction.

These were isolated unit/daemon links with existing dependencies and the local
CLI, not a new complete product build or full test suite. Python syntax and
Git whitespace checks passed; Flake8 was unavailable in the local environment.
Cross-platform, full-suite and release-acceptance gates remain separate.

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
| 2026-10-16, `685a6b4f2c` | Three changed translation units compiled with MSVC 14.43/Qt 5.15.10; 41 selected Qt cases and `wallet_paymaster_pool_setup.py --funding-diagnostics-only` passed. Funding display checked in two themes at 900/1400 px. | Isolated incremental Qt/daemon links. Full pool-setup test remains failing at `check_full_capital_release` with `PAYMASTER_CAPITAL_OPEN_MAINTENANCE`, reproduced with the unchanged test and previous daemon. Full product builds/suites were not run. |
| 2026-10-16, `b66320e4cf` (includes `6ea86beba5`) | Five setup/CLI/Qt translation units compiled with MSVC 14.43/Qt 5.15.10; `paymaster_setup_tests` passed 29 cases / 605 assertions. Six selected Qt functions passed 25 cases: guided setup/retry (3), themes (1), preparation diagnostics (18), fee recovery (1), start approval (1), diagnostic refresh/privacy (1). Reserve setup reviewed in both themes. | Isolated incremental test links using existing dependencies. No normal product build, new daemon-based end-to-end setup run or full suite. The earlier pool-setup failure remains open; these frontend changes do not claim to resolve it. |

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
