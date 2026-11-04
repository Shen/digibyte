# Paymaster release gate and evidence

There is **no complete current-revision release certification** in this record.
The candidate below and historical checkpoints identify actual verification;
implemented behavior alone does not establish release acceptance.

## Wallet safety candidate (2026-11-04)

Source: working-tree corrections based on `b3b26ccbda5a`. Five changed C++
translation units, including both test files, compiled with the existing MSVC
14.43 x64 Release configuration and linked against existing objects/libraries.
This was not a clean product build. Local artifacts, source hashes, exact commands
and per-selection executable hashes are under `.ai/paymaster-wallet-safety-74ee9bccf034/`.

| Executed selection | Result |
|---|---|
| Manual-lock cancellation/pruning, corrupt/versioned reservations, actual reload, deterministic tip invalidation/replacement, complete application and wallet-load suites | 18 cases / 449 assertions passed. |
| Selected PSBT/signature, transaction, reservation, recovery, admission, transport, database and ordinary DGB/DD wallet regressions | 72 cases / 4,012 assertions passed. |

These selections are disjoint and use `tests.exe` with SHA256
`9c7197b67cd67de8fa1b5c6aee7b86d9fe8f61bbbc9b95323d21b635a51396a9`.
The complete store suite, clean builds, functional/Qt matrices and remaining
candidate gates below remain open. Operator commands use the runbook and the
local `operator-checks.ps1`; no operator stage was run by the agent.

## Optimization candidate (2026-11-04)

The review-fix series starts at `33c680cba1`; comparison bases and required
Core-change reasons are in the [change record](design/paymaster-change-record.json).
Local checks use MSVC 14.43 Release flags, static Qt 5.15.10 and isolated links
against existing dependencies, not a clean product build.
Local artifacts are under `build_msvc/paymaster-optimization-check/`; exact
selections, output paths and executable hashes are in `verified-checks.json` there.

| Source / executable | Executed selection and result |
|---|---|
| `6ad3fa7242`, `tests-6ad3fa7242.exe` | 21 cases / 8,673 assertions passed: native service/RPC contracts, finance migration/pagination/incremental work, actual finance RPC backlog, commit failure/restore, background wallet/scheduler isolation, session views, cancellation and stale submits. |
| Same native executable, `db_tests/*` | Seven ordinary database cases / 935 assertions passed, including prefix and bounded range cursors on the available backends. |
| `24172ca820`, `tests.exe`, `paymaster_wallet_finance_tests/*` | Six cases / 8,353 assertions passed again after placing timing inside the wallet lock. These overlap the native selection above. |
| `4c8c60d00c`, `qt-tests.exe`, Windows/Fusion | 32 functions passed: 13 provider and 19 sender/controller/queue functions, including finance read retries, exact confirmations, lost replies, cancellation, recovery and shutdown. The prior executable was replaced by the following relink; its hash and results remain recorded. |
| `75d9351e46`, relinked `qt-tests.exe`, Windows/Fusion | Eleven functions passed: two sender fixtures strengthened to require fresh offers and validated completion, plus nine ordinary wallet/DD queue, balance, validation and view-lifetime regressions. Together the Qt selections cover 43 distinct functions. |
| `75d9351e46`, local architecture checks | Five positive/negative boundary fixtures passed; committed dependency, path, Core-record and cycle checks passed against policy base `d298087a9a`. This is local evidence, not server approval of the separately committed controls. |

The native selection covers 100, 1,000 and 8,192 sessions plus 100,000 finance
events. Unchanged warm finance reads scanned no history; one transaction at the
three session sizes used one bounded index query. These are operation-count
checks with local timing, not production throughput measurements. After a failed
commit the ahead checkpoint remains binding: tests require explicit restore
quarantine before observation-only reconciliation, without restoring signing
permission. No consensus, PoW, activation or signed-format change is introduced.

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
| `33c680cba1`, 2026-11-04 | Prior optimization: targeted identity, selection/index, finance and security checks plus selected Qt workflows passed; four boundary fixtures passed. | Earlier source and isolated links; detailed evidence remains in that revision of this file. |
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
