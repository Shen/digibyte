# Paymaster release gate and evidence

There is **no complete current-revision release certification** in this record.
The candidate below and historical checkpoints identify actual verification;
implemented behavior alone does not establish release acceptance.

## Optimization candidate (2026-11-04)

The optimization series is based on `5ddc3e268e`; the official comparison base and
required Core-change reasons are in the [change record](design/paymaster-change-record.json).
Checks used MSVC 14.43 Release and static Qt 5.15.10, with isolated executables
`build_msvc/paymaster-optimization-check/tests.exe` and `qt-tests.exe` linked against
existing objects. These results do not establish a clean build or release approval.

| Executed checks | Result |
|---|---|
| Wallet identity suite, before final object relink | 49 cases / 2,668 assertions passed. |
| Final client/comparator, session-index scale/commit, finance-cache and saved-target rebalance selection | 15 cases / 39,777 assertions passed. |
| Quote commit/restart, concurrent budget, SQLite failure and stale-final security checks | 4 cases / 277 assertions passed. |
| Qt reserve reduction and maintenance cancellation (`windows` plugin) | 40/40 and 10/10 passed; totals include init/cleanup. |
| Other targeted Qt checks (`minimal` plugin) | Offers/cancellation, finance/backup, wallet/privacy binding, controllers, queue/shutdown and unlock lifetime passed. |
| Architecture fixtures and local dependency/cycle scan | 4 fixtures and the scan passed. |

Finance migration/reorg/backend checks and 100,000-event bounded reads also passed;
the session-index checks cover 100, 1,000 and 8,192 sessions. Detailed local output
remains with the check executables. These are operation-count checks, not production
throughput measurements. No new consensus, PoW, activation or general script
validation change was introduced.

Review blocker: a corrupt maintenance ledger raises `ApplicationError` out of
`RunPaymasterProviderServiceCycle`, whose local catch still expects `UniValue`.
The scheduler path can terminate the node. An isolated test with freshly compiled
service/pool units failed both containment/status assertions. Fix and regression
coverage are required before release; full-history finance reconciliation also
remains a scaling concern.

Open: the complete build, functional, Qt, ordinary DGB/DD, sanitizer and platform
gates below. Server-side required statuses and bypass protection are not configured
or verified; owner-reviewed installation of the
[boundary controls](design/paymaster-integration-boundaries.md#enforcement-and-trusted-policy-bootstrap)
is required. Commands are in the [test runbook](digidollar-paymaster-testing.md).

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

## Historical checkpoints

These results apply only to their recorded source/build. They do not establish
passes for the current candidate and must not be summed: selections overlap.
Local native checks used MSVC 14.43/static Qt 5.15.10 and isolated links against
existing dependencies unless stated otherwise. Missing binary hashes or logs
are not reconstructed from the test counts.

| Source / date | Retained evidence | Scope or limitation |
|---|---|---|
| `1789c803be`, 2026-09-27 | 284 selected unit cases / 10,767 assertions; nine functional tests. | Older RC2 branch. |
| `e5cfa2da3e`, 2026-10-06 | Twelve translation units; 26 selected Core/Qt cases. | No normal product link or functional run. |
| Retention and recovery corrections, 2026-10-10 | Related selections: 48 cases / 10,012 assertions and 28 / 1,542. | Overlapping focused tests; many assertions come from quota fixtures. |
| `01aaf13df9`, `e7c2c8c4bd`, 2026-10-15 | Selected compiles, busy-wallet and sync-presentation Qt checks. | No full GUI or release acceptance. |
| `685a6b4f2c`, `b66320e4cf`, 2026-10-16 | Funding-diagnostics functional variant; selected Qt checks; setup suite 29 cases / 605 assertions. | Isolated links. The then-failing full pool release was corrected by the next row. |
| `c7750a18a1`, `0ab4289107`, 2026-10-17 | Maintenance regression reproduced before the fix; 10 wallet cases / 455 assertions; full descriptor pool-setup, lifecycle and provider scenarios passed. | Isolated unit/daemon links and existing CLI. |
| `e4195962a0`, 2026-10-17 | Corrected checkpoint fixtures and commit-failure quarantine: 7 cases / 992 assertions. | Test-only change; checkpoint protection retained. |
| `e17c96415d`, `fe6536afc3`, 2026-10-17 | Checkpoint/read replacement and guarded functional retries: 3 wallet cases / 270 assertions; four formerly failing functional variants passed. | Windows handle failure reproduced separately; concurrent regression is scheduling-dependent. |
| `31d36a6b7a`, 2026-10-17 | Operator reported all 19 current-current functional variants passing, 709 seconds wall time, after provider-guard fixture corrections. | Normal Release daemon/CLI; summary omitted executable hashes, complete logs and seeds. Historical interoperability was excluded. |
| `6cea92c461`, 2026-10-18 | Three corrected Paymaster Qt functions passed locally; operator subsequently reported the full `PaymasterWidgetTests` selection passed. | Isolated `gui-followup-20261018/qt.exe`; full-repeat totals and warning details were not supplied. |
| `628321a743`, 2026-10-18 | Overview teardown, reservation balance and privacy-mask checks passed. | Three focused functions; earlier teardown crash was not deterministically reproduced. |
| `c231521234`, `82c0b31493`, 2026-10-18 | Eleven focused DD widget functions, mint-record (9 Qt rows), transaction-record (12 rows) and two related Paymaster functions passed; operator reported complete `DigiDollarWidgetTests` passed. | Isolated `build_msvc/paymaster-refresh-check/dd-gui-regressions-20261018/qt.exe`, Windows/Fusion. Wave 19/table passed in the earlier operator selection; no combined all-Qt or clean product build. |

Intermittent Windows bind failures did not recur in the completed functional
matrix; their cause was not established. Qt row totals include fixture/data
rows. Historical warnings and separate component passes do not certify a
current full suite. Current commands belong in the [runbook](digidollar-paymaster-testing.md).

## Open boundaries

The [threat model](paymaster-threat-model.md#residual-operational-risks) records
rollback, cloned-provider, public-sponsorship and privacy limits. Physical
storage failures, real Tor/hostile load, independent review and current full
builds/suites still need the evidence required above. SQLite page-limit and
commit-denial checks do not establish power-loss or fsync coverage.

## Traceability

The full earlier evidence, seeds, failure reproductions and former acceptance-ID
mapping remain in Git:

```sh
git show 5ddc3e268e:doc/digidollar-paymaster-release-gate.md
git show e7c2c8c4bd:doc/paymaster-threat-model.md
```

Record only bounded candidate evidence here. Keep complete logs/fingerprints
with build artifacts, commands in the runbook and resolved investigation details
in Git. Never promote a historical pass to a current-revision release pass.
