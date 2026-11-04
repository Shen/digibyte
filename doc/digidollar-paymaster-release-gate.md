# Paymaster release gate and evidence

There is **no complete current-revision release certification** in this record.
The candidate below and historical checkpoints identify actual verification;
implemented behavior alone does not establish release acceptance.

## Current review corrections and extraction candidate

Starting source: `98c717f738e765cf1dbbbb94ed95afbda118b382`.
Code candidate checked on 2026-11-04: `f28c2dcdc041d27d9799cf9da93da5b346b685e0`.
The task and official comparison bases
remain those in the [change record](design/paymaster-change-record.json).

Changed translation units and tests were freshly compiled with MSVC 14.43.34808 x64
and `/Od /MT /WX /std:c++20`; Qt checks used static Qt 5.15.10, Windows/Fusion.
Executables were isolated links with reused objects/libraries, not clean product
builds. Exact selections, source diffs, response files, executable provenance,
exit codes and full logs are retained under `.ai/paymaster-implementation-98c717f7/`.
Failed baseline/fixture attempts remain separate from successful runs.
`source-freeze-f28c2dcdc0.json` verifies the committed source against those
compile/run manifests. Unchanged dependency libraries and header dependencies
are not a complete clean-build provenance chain. Native/Qt runs from O2 onward
record the fixed `RANDOM_CTX_SEED` in their JSON; earlier runs did not explicitly
set that seed. Initial R1 runs hit a Windows long-temporary-path failure also
reproduced with the unchanged executable; subsequent runs use a short test path.

| Executed check | Bounded result |
|---|---|
| Quote conflict/retry correction (R1) and saved-quote signature follow-up | Regressions fail against the preceding product and pass after correction: initial selection 7 cases / 550 assertions; signature/identity follow-up 9 / 700. Logs: `r1-baseline-test-shorttmp`, `r1-fixed-test`, `r1-followup-{baseline,fixed}-test`. These selections overlap. |
| Persisted client reads and action gating (R2) | Baseline list/resolve/view regressions fail; corrected getter, view, confirmed-payment and application selection passes 14 cases / 597 assertions (`r2-{baseline,fixed}-test`). Corrupt/future state is not treated as absence. |
| Manual Activity presentation (R3) | The existing Qt function fails on deferred-result presentation before correction, then passes with native result serialization and malformed/conflicting-result coverage (`r3-{baseline,fixed}-test`). One feature function; init/cleanup are not extra cases. |
| Related status fallbacks (O2) | Baseline regressions fail; corrected recovery/result selection passes 13 cases / 1,033 assertions (`o2-{baseline,fixed}-test`). The retained-message and stored-replay fixtures do not establish a complete fresh recovery payment. |
| Client/provider phase extraction (O3/O4) | Client capacity and quote stages each pass the same 8 cases / 414 assertions. Provider capacity, recovery and quote stages each pass the same 15 / 994. Logs: `o3-{capacity,quote}-test`, `o4-{capacity,recovery,quote}-test`; repeated stages are not additional coverage. |
| Setup extraction and separate guided-task correction (O5) | Five setup-boundary rows, theme and layout selections pass, including wallet removal during an open wizard/pending preview. A separate local-error baseline still fails after correcting RPC mocks to the real structured format; its fix passes 34 capital-task rows, 13 restoration rows and one structured-error function. Logs: `o5-switch-test`, `o5-layout`, `o5-paymasterGuidedSetupRendersConsistentTheme`, `qt-local-error-*`. Counts exclude init/cleanup. |
| Authenticated conflict/payment/reorg scenario (O1) | `pm98-reorg1` exits zero: conflicting request ID over authenticated v2 transport is consumed, an unrelated recovery is rejected, and the genuine payment is included, reorganized and reconfirmed without duplicate accounting. Binary/source details are in `pm98-reorg1-binaries.json` and its run record. This is the recorded intermediate candidate, not the final-source rerun. |

The closing checks below use source matching the code candidate. Their native
selections are disjoint; all selected tests passed. Unselected native tests are
reported as skipped by Boost and are not counted as coverage.

| Final-source verification | Actual result |
|---|---|
| Native selections | Exit 0: 15 cases / 994 assertions in `o4-quote-test`, plus 29 / 2,114 in `final-native-complement`; 44 distinct cases / 3,108 assertions, including three ordinary DB cursor cases. Executable: `o4-quote.exe`. |
| Qt selections | Exit 0: six sender functions (`o6-*`, including 17 live-send and 19 cancellation rows), manual Activity, six manual-submit confirmation rows, and four ordinary `WalletTests` functions (`final-*`). Twelve functions, with init/cleanup excluded. Executable: `o6-send.exe`. Existing layout/property and QSignalSpy warnings remain in logs; these are not warning-free/full-suite claims. |
| Functional selection | Exit 0 in `pm98-reorg2.log`, 24.36 seconds: real v2 conflict, subsequent payment, reorg and reconfirmation; RPC contract checking retained. Seed `2563184631`, port seed `267`. `final-daemon.exe` was freshly relinked; the existing CLI was reused. Exact test/CLI hashes are in `pm98-reorg2-binaries.json`. |
| Boundary checks and comparison bases | Code candidate passes the pinned `98c717f7` checker/policy and five extracted fixtures. The `33c680cba1` comparison rejects historical control changes and shared files outside its older policy; the official `d7265fb05e26e1e799876bcd526225e4826c3ba8` base rejects missing policy/bootstrap. Evidence and separate diffs are under `.ai/ap5/runs/f28c2dcdc041d27d9799cf9da93da5b346b685e0/`. Neither historical rejection is converted into a pass. |

Executable SHA256 fingerprints:

| Executable | SHA256 |
|---|---|
| `o4-quote.exe` | `6b0c2103eeaa8b313281ae775739121cafddd1a925660c090bb4e9c53db2c091` |
| `o6-send.exe` | `6c8225c9efecfff6595bcc6b664b95c7312fb6c969d5d03b88e63d96c50d71a7` |
| `final-daemon.exe` | `9c6258233423397e884fdf2368492e62f75757eda37c285048ccbf3078d067b8` |

Do not sum overlapping selections or infer a protocol/compatibility pass from
source searches. Protocol-definition/diff review and executed behavioral tests
are separate evidence. The earlier potential `_Rep` leak diagnostic remains
unresolved; successful targeted assertions neither confirm nor disprove it.
Functional Python syntax and whitespace checks passed. `flake8` was unavailable;
the workspace-only installation attempt failed, so no flake8 pass is claimed.

The local Qt test compile uses `/bigobj`. The proposed normal MSVC project change
is **not approved or applied**; the normal project rebuild prerequisite remains
open. Its bounded patch is retained at
`.ai/paymaster-implementation-98c717f7/r3-msvc-bigobj/project-and-record.patch`;
the trusted policy was not expanded. Complete Windows/Linux product builds,
Linux/C++17, Tidy with the repository
plugin, sanitizer runs and the release gates below remain operator work using
the [runbook](digidollar-paymaster-testing.md). These isolated results do not
establish those checks.

The operator-selected hosting target is `Shen/digibyte`, branch
`integration/paymaster-v9.26.7`. The boundary checker, policy, fixture tests and
workflow exist locally at `98c717f7`; their extracted hashes and comparison
runner are retained under `.ai/ap5/`. Remote owner approval, mandatory status
checks, bypass protection and the actual PR base have **not been verified**.
Local controls and local check results are not evidence of their installation or
enforcement on that hosting target.
Final boundary/integration acceptance therefore remains open, as requested by
the operator. If a PR exists, its verified base SHA must replace the locally
selected pre-correction base for the final hosting check. The full-fork MSVC
`int128` adaptation remains subject to its separate platform/equivalence proof.

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
| `7f30be59f7` through `ef213fb746`, 2026-11-04 | Inbound/pending-balance corrections: red/green regressions, 121 distinct selected native cases / 3,936 assertions, focused payment/reorg and local boundary checks passed. | Isolated MSVC links; overlapping F2 reruns excluded. Detailed provenance and limitations remain in this file at `98c717f7`. |
| Corrections based on `b3b26ccbda5a`, 2026-11-04 | Wallet safety: 18 cases / 449 assertions and a separate 72 / 4,012 selection passed, covering lock ownership, readiness, reload and selected ordinary-wallet paths. | Recorded working-tree corrections and isolated links; no complete store suite or clean product build. Details remain at `98c717f7`. |
| `6ad3fa7242` through `75d9351e46`, 2026-11-04 | Optimization: 21 native cases / 8,673 assertions, seven ordinary DB cases / 935 assertions, 43 selected Qt functions and local boundary checks passed. | Finance reruns overlap; operation-count tests are not throughput evidence. Isolated links and no remote control approval; details remain at `98c717f7`. |
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
git show 98c717f7:doc/digidollar-paymaster-release-gate.md
git show 5ddc3e268e:doc/digidollar-paymaster-release-gate.md
git show e7c2c8c4bd:doc/paymaster-threat-model.md
```

Record only bounded candidate evidence here. Keep complete logs/fingerprints
with build artifacts, commands in the runbook and resolved investigation details
in Git. Never promote a historical pass to a current-revision release pass.
