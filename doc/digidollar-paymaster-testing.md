# Paymaster build and test runbook

## Restricted new service disabled (2026-10-09)

The new-service boundary is `CheckNewSponsorshipScope`, documented in
`digidollar-paymaster-implementation.md#parked-restricted-sponsorship`.
Repository/CLAUDE reading order, DigiDollar architecture, Qt, wallet/RPC,
functional-test and formatting guidance apply. This change uses Paymaster
modules and preserves protocol/storage validation for accepted legacy work.

Verification completed:

- Selected MSVC compilation and isolated linking of the changed components,
  GUI, CLI, daemon and test executables.
- Eight Core cases: the new scope gate/legacy-policy round trip, setup rejection
  before mutation, saved wallet policy preservation and unavailable readiness,
  four existing sponsorship validation/replay cases, and the existing atomic
  client-authorization/idempotence case.
- Eighteen Qt cases, excluding setup/cleanup: `paymasterOfferSpendingLimitsReview`
  (11), `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep` (3), and
  `paymasterLiveOverview` (4). Restricted selection is disabled; a legacy value
  cannot cause policy/budget writes or offer-saving side effects.
- `wallet_paymaster_rpc.py --restricted-disabled-only` passed using isolated
  provider/client nodes. Both HTTP RPC and actual CLI calls reject new Restricted
  policy, descriptor, send and direct quote requests. Saved policy, safety state,
  reservations and sessions remain unchanged; capabilities advertise public only.
- Python syntax checks for both changed functional files and `git diff --check`.

The broad provider scenario now checks Restricted rejection and continues with
public-sponsored restart/recovery. It was **not rerun in full**; neither were the
full solution or complete Qt suite, per the operator's resource instructions.
The initial new-test failures were fixture issues (descriptor/processed-tip
initialization, funded client and nonempty input list), corrected before the
successful runs above. Legacy Restricted accepted-transfer recovery remains
enabled by code review and unchanged codecs; a complete old-wallet payment
upgrade/recovery scenario was not executed in this focused run.

Operator follow-up (MSVC 14.43, Qt 5.15.10 and existing dependencies; close GUI
and CLI first; several minutes to tens of minutes, exit 0 and no test failures):

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
python test/functional/wallet_paymaster_provider.py --configfile=test/config.ini
```

After verifying that no normal DigiByte processes were running, the GUI, CLI
and daemon EXEs were backed up, replaced and compared by SHA256. The installed
GUI hash is `F75D50C3DAB12BA8A6F2758D4C80F440AE54185B63DBF15B773CF6CAE5DA9DBE`.
The local backup/hash receipt is
`build_msvc/paymaster-refresh-check/reserve-presets/restricted-disabled-installed.json`.
No live wallet settings or payment state were edited; the applications were not
started automatically. The GUI also includes the DD-history correction below.

## DD history startup status (2026-10-09)

The operator confirmed that all-Pending rows appeared only immediately after
opening the tab. Read-only `listdigidollartxs` on both running regtest wallets
reported confirmed transactions (including a shared client/provider transaction
with 657 confirmations). The stored-history startup preview had been presenting
persisted zero counts as current status before the asynchronous read completed.

The fix is confined to the shared Qt history serializer and the two consumers:
`walletmodel.cpp`, `digidollartransactionswidget.cpp`, and
`digidollaroverviewwidget.cpp`. Stored preview rows carry a Qt-only `checking`
status; the existing worker replaces them with live status. There are no new
wallet locks, workers, RPC calls, persistent records or confirmation rules.
Repository, DigiDollar, Qt, test, formatting and translation guidance apply.

Targeted regression coverage:

- `transactionsWidgetShowsStoredHistoryWhileWalletBusy`: 50 immediate stored
  rows while another thread holds `cs_wallet`; both views show Checking, then
  a wallet-confirmed row shows one confirmation and genuinely unconfirmed rows
  show Pending. All 75 rows load, the loading banner clears and the model retains
  verified status for later views.
- `ddTabLoadsSelectedPageOnFirstShow` and
  `transactionsWidgetRefreshesOnDigiDollarSignal`.
- `transactionsWidgetShowsRpcHistorySignsAndFields` and
  `transactionsConfirmationsColumnIsAlwaysACount`.
- `failedMintsKeepTheirWalletStatus`.
- `overviewRecentTransactionDoubleClickOpensTransactionsTab` and
  `overviewRecentTransactionAmountIsRightAligned`.

The two existing tests that inspect live status now wait for it, instead of
assuming visible stored rows already contain a completed background read.
All eight targeted GUI cases passed (excluding fixture setup/cleanup), along
with selected MSVC compilation, isolated Qt/app linking and `git diff --check`.
The initially staged GUI hash was
`EAFCD3CA29B1AC4BFC056A3A5199DD9BD55E1C37FADF15550C7610B1A152ECF9`.
This correction was subsequently included in the combined Restricted-disabled
GUI build installed after regular shutdown; see the installation record above.
The full solution and complete suite remain operator checks using the command
and prerequisites below. No live wallet settings or payments are changed.

## Overview readiness and settings restart (2026-10-09)

Product changes are confined to `src/qt/paymasterwidget.cpp`; existing Core/RPC
interfaces are reused. No polling or per-payment wallet reads were added.
Repository, Qt, formatting and translation guidance apply.

Targeted checks:

- `paymasterLiveOverview` (4 theme/width cases): saved models versus unsaved
  drafts, zero-fee user-paid pricing, missing and exhausted model budgets,
  concurrent customer counts, multiple reserve operation references, drain/error
  states, hidden inactive controls, privacy/wallet clearing and layout bounds.
- `paymasterSettingsRestart` (19 cases): offer and processing-mode saves,
  unchanged autostart on/off, cancellation, busy stop, incomplete acknowledgements,
  failed save/readback/enable/start, deferred readiness, wallet switch, privacy
  interruption at several stages and privacy turned off again before a reply.
  The sequence never replays a mutation after an ambiguous result.
- Existing regression checks: `paymasterOperatorWorkTransitions` (2),
  `paymasterOfferPolicyTypedValues` (10),
  `paymasterInjectedRpcCoversConfigurationWorkflows` (1),
  `paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows` (1),
  `paymasterOfferFormAlignment` (4), and
  `paymasterOperatorOverviewGuidesAndFailsClosed` (1).

All 42 targeted GUI/mock-RPC cases passed, excluding fixture setup/cleanup.
The targeted compilation, isolated Qt/app linking and `git diff --check` passed;
screenshots were inspected in dark/wide and light/narrow layouts. The normal
`build_msvc/x64/Release/digibyte-qt.exe` was replaced after checking that no
instance used it, with a backup and matching staged/installed SHA256. Its hash is
`1220EAB3C0837B8D7019EB08436FFE072123D620264231DB9724D4A2DAD79EF3`.
Full solution/full Paymaster suite and live provider restart remain
operator checks. Use the full-build command and prerequisites below; no live
wallet settings or payments are changed by these tests.

## Funds & reserves operator layout (2026-10-09)

The production change is confined to `src/qt/paymasterwidget.cpp`. It follows
the repository/Qt guidance, existing semantic theme roles and translation policy.
Current metrics consume the existing serialized operator and overview-finance
reads. Expanding the reserve table or advanced actions issues no RPC. Current
saved targets are independent of the editable preset; missing capital data is
shown as unavailable instead of retained as current or replaced with zero.

Targeted MSVC compilation and isolated Qt/app linking passed. The four
`paymasterFundsOperatorPresentation` cases cover both themes at 760/1360 pixels,
large fonts, table/advanced disclosures without RPCs, unchanged saved targets
during edits, payout minimum, failed finance reads, privacy and wallet changes.
Screenshots were inspected in dark/wide and light/narrow configurations.

Additional passing checks:

- `paymasterCarrierWithdrawalPreviewsArePlanBound` and
  `paymasterLiquidityPolicyDefaultsAndApprovalGuard`.
- `paymasterGuidedCapitalTasks`: `release_slot-approve`, `release_slot-cancel`,
  `all_excess-approve`, `rebalancepaymasterpool-approve`.
- `paymasterReserveReduction:dark-approve`.
- All four `paymasterFinanceOperatorPresentation` rows.
- `DigiDollarWidgetTests::digiDollarAmountLabelsUseCurrencyPrefix`: existing
  page/currency structure check. Updated removed-card/text expectations and
  older fixture assumptions: unsaved sponsorship does not remove DD targets,
  and fee summaries require a valid amount/recipient before comparison.

All **16 targeted GUI cases** passed, excluding fixture setup/cleanup.
`git diff --check` passed. These are GUI/mock-RPC checks, not live financial
transactions; the approval implementations are unchanged.

Full solution build/full Paymaster Qt suite remain operator checks. From
`D:\Digibyte\digibyte-fork`, with the GUI instances and active CLI calls closed:

```powershell
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
```

Requires the existing MSVC 14.43, Qt 5.15.10 and cached static dependencies.
Allow several minutes; success means exit code 0 and no failed tests. No Core,
RPC or consensus behavior changed in this presentation update.

## DD transaction refresh after Paymaster payments (2026-10-09)

First accepted-mempool observation and validated payment completion now emit
the existing DD update signal. Repeated identical observations do not. Exact
durable recovery skips unchanged wallet updates after checking transaction
bytes, state and metadata; authorization and chain/pool preflights remain.
Opt-in `bench` logs measure durable recovery and slow shared DD history reads,
separating worker startup, wallet read/serialization and queued Qt delivery.
The latter is the only change to shared non-Paymaster product code.

Selected MSVC compilation and isolated Core/Qt/app linking passed. Three Core
cases passed with **384 assertions**:

- `paymaster_wallet_security_tests/durable_observation_skips_only_unchanged_wallet_state`
  checks unchanged confirmed transactions, changed block/height/position,
  inactive wallet state, missing durable metadata and conflicting witness bytes.
  Changed wallet states are injected; this is not an end-to-end chain reorg test.
- `paymaster_wallet_security_tests/provider_final_commit_spends_budget_atomically`
  also checks repeated real durable recovery while the exact final is in mempool.
- `paymaster_wallet_security_tests/provider_user_authorization_honors_exact_reserved_safety_binding`.

Six final targeted Qt cases passed, excluding fixture setup/cleanup:

- `paymasterClientLiveSendProgressesAcrossAsyncPhases` rows `completed`,
  `authorized`, and `review_changed_controls`: completion and first mempool
  acceptance refresh once; authorization alone does not.
- `paymasterClientSessionRpcActionsAreBound`: successful explicit retry refreshes
  once. Its old mock provider attempt incorrectly used session-only `CONFIRMED`;
  the fixture now uses the valid attempt state `MEMPOOL` and checks UI completion.
- `transactionsWidgetShowsStoredHistoryWhileWalletBusy` and
  `transactionsWidgetRefreshesOnDigiDollarSignal`.

The first async-flow matrix run passed the other cancellation/error/wallet-switch
rows; its new completion row initially used incomplete confirmation/result
metadata, correctly rejected by the unchanged response decoder. The corrected
row and affected mempool rows were rerun as listed above. Reports are local
`build_msvc/paymaster-refresh-check/reserve-presets/history-*.txt` files.

Guidance applied: repository/src instructions, CLAUDE reading order, DigiDollar
architecture/maps, contribution and Qt/test guidance, developer threading/logging
rules and C++ formatting. Full solution/full Qt suites and live-wallet reproduction
of the earlier long pause remain operator checks; these targeted results do not
prove that every source of latency is resolved. To run the full Windows check,
close the wallet applications and active CLI calls normally, then use the cached
MSVC 14.43 / Qt 5.15.10 environment from `D:\Digibyte\digibyte-fork`:

```powershell
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
```

Allow several minutes; require exit 0 and no failed tests. For a remaining live
pause, follow the opt-in benchmark instructions in the operator guide; do not
cancel payments or release reservations for diagnosis.

## Operator finance presentation (2026-10-08)

The finance page now separates selected-period results, payment/reserve costs,
optional period/current-price comparisons and current reserve capital. All values
use the existing wallet-bound finance snapshot. Core/RPC/CLI, fee authority and
polling frequency are unchanged; no additional wallet reads were introduced.

Selected MSVC compiles (widget, resources, tests and MOC) and isolated Qt linking
passed. **11 targeted Qt cases passed**, excluding fixture setup/cleanup:

- `paymasterFinanceOperatorPresentation`: four native dark/light rows at 760
  and 1360 logical pixels, with larger inherited fonts in narrow windows. Checks
  cost reconciliation (7.43065 = 1.4 + 6.03065 DGB), optional negative USD estimate,
  keyboard disclosure without extra RPC, wrapping/no horizontal scroll, pending
  period changes, missing price, zero payments, current capital, privacy and reset.
- `paymasterFinancesAndBackupWorkflow`: source/period values, missing oracle,
  partial/malformed history, stale period replies, privacy, backup and complete
  paginated CSV export, including the existing 10,251-event responsiveness case.
- `paymasterOverviewFinanceWalletAndPrivacyBinding`,
  `paymasterOperatorPollingPreservesDraftsAndThrottlesFinance`,
  `paymasterFiveDestinationsAndVisibleTasks`, `paymasterAppNumberFormat` and both
  theme rows of `paymasterOperatorBackgroundRefresh`.

Reports are workspace-local `finance-layout-*.txt` under
`build_msvc/paymaster-refresh-check/reserve-presets/`. The four
`finance-layout-{dark,light}-{760,1360}.png` files show native Qt example data;
wide dark and narrow light captures were inspected after the final style change.
The HTML design preview has corresponding example sections; duplicate IDs and
literal script targets were checked. `git diff --check` passed.

Relevant guidance: repository/src instructions and CLAUDE, contribution rules,
DigiDollar architecture/maps, Qt/test guidance, translation policy, developer
threading rules and C++ formatting. Product edits are confined to the Paymaster
widget and scoped theme rules. No original DigiByte product code was changed.

The normal EXE was not rebuilt/replaced. The complete build and full suite remain
operator checks. With MSVC 14.43, Qt 5.15.10 and cached static dependencies installed,
close Client, Paymaster and active CLI calls normally, then run:

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
```

Allow minutes for the incremental solution build and several minutes for the
full suite. Success requires exit 0 and zero failed cases. In the normal wallet,
compare period totals with booking export, open/close both comparisons, toggle
privacy and review current capital in Funds & reserves. No financial action is
required to check the new layout.

## DD Vault first-open contention (2026-10-08)

The Vault's original synchronous reads reproduced a 525-ms first-open stall
under `cs_wallet`, until the 500-ms test watchdog released the lock. With the
single-worker snapshot the same navigation returned in 13 ms while the lock
remained held. The worker reuses existing Core position reconciliation and
retains the wallet; row rendering does not query signing state again. No Core,
RPC/CLI, fee authorization or consensus behavior was changed.

Selected MSVC compiles and isolated Qt-library/test linking passed. **22 targeted
Qt cases passed**, excluding fixture initialization/cleanup:

- Seven new lifecycle rows: repeated invalidations, privacy, hiding, rebinding,
  model detachment, widget/model destruction and `cs_main` contention. They also
  verify row rendering under `cs_wallet` and unchanged worker identity during
  refresh bursts.
- One actual DD tab-navigation contention test, including Receive, Send,
  Overview and the first Vault activation.
- Twelve existing Vault/status regressions: hidden/first load, oracle units,
  watch-only and locked wallets, pending mint/redeem states, timelock tooltip,
  sorting, column layout, privacy and failed-mint status.
- Two Wave19 cases: canonical tier-zero tooltip and unavailable oracle health.

The failed-mint history assertion now waits for the live status instead of
assuming that a complete persisted row count means its background refresh has
finished. Expected statuses and financial checks are unchanged. Reports are
workspace-local `vault-*` files under
`build_msvc/paymaster-refresh-check/reserve-presets/`; `vault-before.txt` records
the deliberately reproduced pre-fix failure. `git diff --check` passed.

Guidance: repository/src instructions, CLAUDE and DigiDollar architecture/maps,
contribution rules, developer locking notes, Qt/test guidance, translation policy
and C++ formatting. The change is confined to the native Vault view and tests;
a second implementation in Paymaster would duplicate the original view without
removing its blocking reads.

The normal EXE was not rebuilt or replaced. Full builds/suites remain operator
work. With installed MSVC 14.43, Qt 5.15.10 and cached static dependencies, close
Client, Paymaster and active CLI calls normally, then run:

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
if ($LASTEXITCODE -ne 0) { throw 'Build or Paymaster Qt tests failed' }
$env:QT_QPA_PLATFORM = 'windows'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
foreach ($suite in @('DigiDollarWidgetTests', 'DigiDollarWave19WidgetTests')) {
    $env:DIGIBYTE_QT_TEST_SUITE = $suite
    $env:DIGIBYTE_QT_TEST_OUTPUT = Join-Path $PWD "build_msvc/paymaster-refresh-check/reserve-presets/full-vault-$suite.txt"
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw "Qt tests failed: $suite; report: $env:DIGIBYTE_QT_TEST_OUTPUT" }
}
```

Allow minutes for the incremental build and several minutes per full suite.
Success requires exit 0 and zero failed cases in each report. In both wallets,
open Vault for the first time during Paymaster activity, switch away/back and
toggle masking. Navigation must remain responsive while the data loads. These
targeted results do not establish that every native DD action is asynchronous.

## Original-code boundary extraction (2026-10-07)

The [boundary review](design/paymaster-flow-audit.md#original-code-boundary-review-2026-10-07)
compares the integration against local upstream `release/v9.26.7`. CLI operator
workflows, Qt signing orchestration and wallet-maintenance implementation moved
to Paymaster modules. Shared validation, transport, unlock leases and atomic
wallet protection were retained; two unused balance convenience getters were
removed. No RPC contract, fee limit, record version or consensus rule changed.

Validation on the completed implementation:

- Selected MSVC compilation of the affected CLI, Qt, wallet and test translation
  units passed, with isolated CLI/Core-test/Qt-test links. The CLI project was
  regenerated from its Makefile source list; the tracked Qt project was updated.
- `paymaster_setup_tests`: **23 cases passed**, including real CLI adapter
  dispatch, exact wallet selection, read-only status, forbidden input modes and
  no retry after a failed status reply.
- `paymaster_wallet_load_tests`: **7 cases passed**. Together the two Core groups
  passed **434 assertions**; no other Core groups were run.
- **52 targeted Qt cases passed**: signing contention/lifecycle (1), unlock lease
  after model close (1), two-stage authorization (1), review cancellation (20),
  command page locks (1), confirmation wallet binding (4), guided capital tasks
  (24). The signing preflight again returned in **0 ms** with `cs_wallet` held.
- **8 isolated CLI smoke checks passed** against a loopback fixture: Paymaster
  status over the existing HTTP path with an encoded wallet name, ordinary RPC
  dispatch, missing wallet, forbidden named mode, nonterminal setup, standalone
  watch, conflicting modes and help. No operator node or real wallet was used.
- Mechanical comparison preserved the CLI helper/workflow bodies after explicit
  context routing and indentation, and the four scheduling/maintenance bodies.
  `git diff --check` passed.

Reports are the workspace-local `original-code-*` files under
`build_msvc/paymaster-refresh-check/reserve-presets/`. The normal application EXE
was not rebuilt/replaced, and full builds/suites remain delegated to the operator.
Using the installed MSVC 14.43, Qt 5.15.10 and cached static dependencies, close
the applications and active CLI calls normally, then run:

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
if ($LASTEXITCODE -ne 0) { throw 'Build or full Paymaster Qt suite failed' }
.\build_msvc\x64\Release\test_digibyte.exe --run_test=paymaster_setup_tests,paymaster_wallet_load_tests
if ($LASTEXITCODE -ne 0) { throw 'Paymaster boundary regressions failed' }
```

Allow minutes for the incremental build and several minutes for the full Qt
group. Success requires build/test exit 0, zero Qt failures and both Core groups
passing. Repository/src, architecture/maps, contribution, developer locking,
Qt/test, build, translation and formatting guidance were applied.

## Paymaster-wide UI wait audit (2026-10-07)

The [scope and findings](design/paymaster-flow-audit.md#ui-blocking-audit-2026-10-07)
cover every provider destination, setup, client offers/send/recovery, and shared
DD/native boundaries. Two further Paymaster-owned waits were corrected: signing
preflight/temporary relocking and final accounting CSV serialization/file I/O.
The signing bridge initially added as `executePaymasterSigningRpcAsync` now
lives in `qt/paymasterwallet.cpp` as `PaymasterQt::ExecuteSigningRpcAsync`.
Existing native wallet operations, Core locks and financial authorization are unchanged.

The encrypted-wallet preflight blocked Qt for **502 ms** before the fix, until
the regression's 500-ms watchdog released `cs_wallet`. It returned in **0 ms**
afterward. The test uses disposable wallets and verifies an already unlocked
wallet, normal unlock, cancellation, RPC error, rebinding and model destruction.
Temporary relocking must finish before results reach Qt. Deterministic injected
widget transports retain their existing unlock test path; the new bridge is
tested separately with real Core RPCs and wallet locks.

Selected MSVC compiles and an isolated native Qt link passed. **61 targeted Qt
cases passed**, excluding initialization/cleanup:

| Test function | Cases |
|---|---:|
| `paymasterSigningWaitKeepsGuiResponsive` | 1 |
| `paymasterClientAuthorizationIsTwoStageAndFailClosed` | 1 |
| `paymasterClientReviewCancellation` | 20 |
| `paymasterFinancesAndBackupWorkflow` | 1 |
| `paymasterProviderCommandLocksPages` | 1 |
| `paymasterOperatorBackgroundRefresh` | 2 |
| `paymasterClientLiveRecoveryProgresses` | 7 |
| `paymasterOperatorConfirmationWalletBinding` | 4 |
| `paymasterGuidedCapitalTasks` | 24 |

The finance case validates all 10,251 exported bookings and a Qt timer heartbeat
during final writing, beyond the existing page-fetch event-loop check. The
writer retains atomic replacement. Wallet/privacy changes or widget destruction
cancel before replacement starts; a commit already in progress finishes without
waiting on Qt, and its result cannot populate a detached view.

No full build, full suite, live wallet action or normal EXE replacement was run.
Repository/src, CLAUDE/DigiDollar architecture/maps, developer locking notes,
Qt/test, translation and formatting guidance were followed. For the operator,
close Client, Paymaster and active CLI calls normally, then run the existing
workspace helper using the installed MSVC 14.43, Qt 5.15.10 and cached static
dependencies:

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
```

Allow minutes for the incremental build and several minutes for the full
Paymaster suite. Success requires exit 0 and zero failed cases in
`build_msvc/paymaster-refresh-check/reserve-presets/full-paymaster-qt.txt`.
The DD full-suite command and navigation checks below still apply. For this
follow-up also exercise an encrypted provider/client signing action, cancelled
unlock, wallet closure during unlock and a large accounting export. Shared
native backup/password work remains a separately documented possible source of
UI waits; these targeted results do not establish that all live hangs are gone.

## DD Overview contention after Paymaster actions (2026-10-07)

DD Overview still called the Paymaster-aware balance summary, locked-collateral
read and wallet mint-capability check on Qt's event thread. An incoming-transfer
timer could invoke its balance refresh after navigating to Paymaster Network.
The lock-contention regression reproduced a **529-ms** wait before the fix,
until its 500-ms watchdog released `cs_wallet`. After the correction, the visible
overview refresh returned in **19 ms** while that lock was held; the hidden
overview callback on Paymaster Network returned in **0 ms**. These are controlled
regression measurements, not a benchmark of the operator's complete workflow.

The read-only overview worker retains the shared wallet and returns values on
the GUI thread. Refreshes coalesce, retain the last successful balance, reject
detached replies and respect privacy. Oracle and network-health RPCs also run
on workers with bounded in-flight queries and client-generation guards. The
contention regression additionally holds `cs_main` during those refreshes.
Core locks, financial authorization, RPC/CLI and ordinary DGB views are unchanged.

Selected MSVC compiles and an isolated native Qt test link passed. **12 targeted
Qt cases passed**, excluding initialization and cleanup: nine DigiDollar cases
(contention, wallet rebinding/privacy, conditional reserved-balance display,
mint availability, overview smoke, masking, USD suffix, health polling and first
page activation) and three Paymaster cases (background refresh in both themes
and command/page locking). The mint-availability fixture now finishes RPC warmup
when run alone and waits for the asynchronous results; its network eligibility
assertions are preserved. No complete suite or normal application EXE was built
for this checkpoint.

Guidance: repository/src instructions, CLAUDE's DigiDollar reading order,
contribution rules, architecture/maps, developer locking/Qt notes, Qt/test
guidance, English translation strings and C++ formatting conventions.

Operator verification, after closing Client, Paymaster and active CLI calls
normally, with the existing MSVC 14.43/Qt 5.15.10/static dependencies:

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'DigiDollarWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
$env:DIGIBYTE_QT_TEST_OUTPUT = Join-Path $PWD 'build_msvc/paymaster-refresh-check/reserve-presets/full-digidollar-qt.txt'
& .\build_msvc\x64\Release\test_digibyte-qt.exe
if ($LASTEXITCODE -ne 0) { throw "DigiDollar Qt tests failed: $LASTEXITCODE; report: $env:DIGIBYTE_QT_TEST_OUTPUT" }
```

Allow minutes for the incremental build and several minutes per full suite.
Success requires build/test exit 0 and zero failed cases in both suite reports.
Then repeat the reported payment/reserve/settings actions and switch between DD
Overview and Paymaster Network in both wallets. Navigation must remain usable
while balances refresh. Also check wallet changes and masking during a refresh.

## Excess-reserve fee proposal and busy retry (2026-10-07)

Selected MSVC compiles and isolated native links passed for the Paymaster RPC,
widget and its tests. **74 targeted Qt cases passed**, excluding initialization
and cleanup: reserve reduction (36, both themes), guided capital actions (24),
setup-fee recovery (1), and guided restoration/approval (13). Reduction cases
cover increased/invalid fee proposals, a fresh confirmation after a fee-limit
rejection, a separate wallet-wide limit, bounded busy retries, changed plans,
wallet/privacy changes, cancellation, malformed receipts and lost replies.
Native release dialogs were inspected in both light and dark themes.

`wallet_paymaster_provider.py --pool-reduction-only` passed against an isolated
daemon and the real CLI in disposable regtest wallets. It checks read-only
recommendations (including keypool, pool and saved policy), exact fee rounding,
recommendation-only versus execution, plan/revision/cap binding, actual fees,
unchanged legacy calls and idempotent finance reconciliation. This is a targeted
test, not a full functional-suite result.

The normal application EXE and the full Qt suite remain operator work. In this
workspace, after closing Client, Paymaster and active CLI calls normally:

```powershell
Set-Location D:\Digibyte\digibyte-fork
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
```

The local helper uses the installed MSVC 14.43, Qt 5.15.10 and cached static
dependencies. Allow minutes for the incremental solution build and roughly
ten minutes for the complete Paymaster Qt suite. Success requires exit 0 and
zero failed cases in `build_msvc/paymaster-refresh-check/reserve-presets/full-paymaster-qt.txt`.
The helper and binary/log artifacts are workspace-local and are not committed.

## Reserve UI full-suite follow-up (2026-10-07)

At `1559fccd72`, the operator's full Windows solution build succeeded. The
Paymaster Qt suite reported 369 passed and six failed entries. Five failures
used the former 0.20-DGB setup proposal in their RPC fixtures or expectations;
the sixth still expected the old settings destination and reserve-repair text.
Those tests now check the 0.50-DGB proposal, Funds & reserves navigation and
the current requirement for explicit restoration approval.

A new regression also reproduced an actual UI inconsistency: switching wallets
reset the one-time setup draft to 0.20 DGB. Construction and wallet reset now
share the 0.50-DGB proposal. This does not approve spending or alter saved limits.

Selected MSVC compiles and a separate native Qt test link passed. All 39 focused
cases passed (excluding initialization/cleanup), covering guided restoration,
preparation across refresh, external readiness, default/approval guards,
existing fee recovery, reserve reduction and presets. The wallet-reset assertion
failed before the product fix and passed afterward. A complete suite rerun
after this correction remains operator work; the earlier six failures are not
reported as a passing full-suite result.

## Official v9.26.7 source integration (2026-10-06)

Branch `integration/paymaster-v9.26.7` merges official `d7265fb05e26` with the
current Paymaster source `2f44ac43c2`; merge commit `e5cfa2da3e`. The preceding
integration branch remains unchanged. Official product changes take precedence
and all their hunks are retained. The fork's Windows header is regenerated as
9.26.7; new official Qt address tests reuse the separated rescan helper and
finish RPC warmup for isolated execution, retaining their assertions.

Completed: 12 selected MSVC compiles with generated MOC/resources, separate
incremental Core/Qt test links, **26 targeted cases passed** (six Core/RPC and
20 Qt; no failures/skips), official product patch retention, unchanged official
release document, functional Python syntax and diff checks. These checks retain
the preceding Send/Receive/session corrections; no new Paymaster financial,
protocol or persistence change is added. Normal wallet/daemon/CLI binaries have
not been built or installed for this checkpoint.

The [v9.26.7 integration record](digidollar-paymaster-v9.26.7-integration.md)
contains exact source hashes, scope, test coverage and copyable full-build,
unit/Qt/functional acceptance commands. Those larger runs and cross-platform
checks remain operator work; upstream test claims do not certify this fork.

## Send DD balance responsiveness (2026-10-06)

Source base: `8bd9d0922f`; implementation: `8fd540b9ea`.
Send DD still read its spendable balance synchronously during initial binding
and page refresh. The existing Core summary takes `cs_wallet`, which Paymaster
session reads and provider work may also hold. An isolated regression with a
real DD backend reproduces the GUI wait: **509 ms**, until its 500-ms watchdog
releases the lock. The earlier navigation fixture lacked that backend and
therefore returned before taking the lock. The updated regression now checks
that Send DD opens while the lock remains held: **4 ms** in the final run;
Receive DD also passes (**9 ms**). These are controlled contention tests, not
measurements of the operator's reported live delay. Read-only live checks found
zero active client sessions and no multi-second RPC delay; they do not identify
the particular competing lock holder.

The fix adds a read-only WalletModel worker and a wallet-bound Send DD snapshot.
Pending refreshes are coalesced, detached replies are discarded, and recipient,
amount and note drafts survive refresh. Unknown or failed balance reads disable
new payment actions. Core spending, signing, reservations, RPC/CLI and native DGB
behavior are unchanged. Four Qt product files and two test files are modified;
no Core optimization or financial authorization change is needed here.

The preceding product changes were also reviewed for necessity: `7decec7151`
fixes a reproduced rejection of a validated terminal reservation owner in
unsigned history; `03309336e5` removes reproduced Receive DD selection waits.
The accompanying receive-cache identity checks prevent stale writes from
restoring removed requests. Their before/after evidence is recorded below.
This review covers those changes and the current fix, not every historical
change in the integration branch.

Relevant guidance: repository/src instructions, CLAUDE's DigiDollar reading
order, architecture/maps, contribution rules, developer GUI/locking notes,
Paymaster wallet contracts, Qt translation policy and Windows/test runbooks.
Completed verification: selected MSVC 14.43 / static Qt 5.15.10 product/test
compiles, regenerated MOC, separate incremental application/test links and
**27 targeted native Qt cases passed** across 15 selected functions, none failed
or skipped. Coverage includes busy-wallet initial binding/navigation, balance
loading/error/retry, wallet changes and late replies, preserved drafts, privacy,
coin control and send validation, inbox failures, new-transfer offer clearing
and wallet-bound Paymaster dialogs. Test helpers and logs remain excluded under
`build_msvc/paymaster-refresh-check/send-open/`.

The candidate `build_msvc/x64/Release/digibyte-qt-send-open.exe` has SHA-256
`91C5AC7BA0C3A3B9DCCE943365F2413B0C79C471CF041646913053301FCAF7E8`.
Its matching PDB has SHA-256
`F90C51723E93A840D355E83DFFF2992402B051B63BA57B8FB796A18AFC73A092`.
The local `ready-artifact.json` binds these hashes to the implementation and
test result. The normal EXE has not been replaced at this checkpoint: both
wallet processes still hold it open. No real wallet application or financial
RPC is started for these tests.

Full solution builds, complete Core/Qt suites, functional lifecycle tests and
cross-platform checks remain operator work. The prerequisites, full-build
command and suite commands in the next checkpoint apply; allow minutes and
require exit codes zero with no failed cases. The previously recorded broad
dropdown-highlight failures have not been retested by this targeted change.
Manual acceptance after closing both wallets and installing the verified EXE:
open Send DD during ordinary Paymaster work, keep an entered draft through
refresh, switch wallets and verify that the former balance cannot reappear.
Loading must leave the window responsive and prevent a new payment until the
balance is known. Exact payment approvals and durable recovery remain intact.

## Historical reservation owners and Receive DD selection (2026-10-06)

Source base: `471991482d`; shared ownership fix: `7decec7151`.
Receive snapshot fix: `03309336e5`.
The client inbox rejected a released unsigned session's historical input when
the later validated reservation owner became terminal. The extended
`paymasterClientReleasedInputsCanBeReservedAgain` regression reproduces the
reported `PAYMASTER_RESERVATION_SESSION_CONFLICT` before the fix. The read-only
query now accepts that later owner regardless of its state, retaining the old
unsigned-history proof and exact current request/session/input/index bindings.
Both active-only and full-history RPC lists are tested for confirmed, canceled,
conflicted and failed owners; reservations and ordinary coin locks stay intact.
This shared Core path applies to Qt and RPC/CLI; the CLI client needs no separate
logic change. Signing, recovery, pruning, fee policy and record formats are
unchanged. The concrete operator database remains unverified while its running
wallet holds the SQLite lock; no attempt is made to bypass that lock or manually
release its inputs.

Receive DD's background list refresh still left selection-related lookups on
the GUI thread calling `getAddressReceiveRequests()` again. Selection, copying
and request dialogs now use the displayed wallet-bound snapshot. Selected QR
metadata follows its request. Completed local edits/removals update the cache;
explicit writes reread the stored identity and reject a removed/replaced request.
Existing revision/generation checks still discard late worker replies.
The new busy-wallet regression reproduces the old dialog/lookup wait until its
1.5-second watchdog releases `cs_wallet` (**1525 ms**). With the fix, selection,
copying and dialog creation finish while the lock is still held (**317 ms**).
This includes native Windows clipboard/dialog startup, not a live-wallet
performance benchmark. Opening Receive DD also passes the existing tab-switch
test with the lock held (**14 ms**).

Relevant guidance: repository/src instructions, CLAUDE's DigiDollar reading
order, architecture/maps, contribution rules, developer GUI/locking notes,
Paymaster persistence/integration contracts, Qt translation policy and existing
Windows/test runbooks. Selected MSVC 14.43 / static Qt 5.15.10 product and test
compiles, regenerated test MOC and separate incremental application/test links
are used. Local helpers/logs are excluded under
`build_msvc/paymaster-refresh-check/receive-selection/`; diagnostic binaries are
`digibyte-qt-receive-selection.exe` and `test_digibyte-qt-receive-selection.exe`.
No real wallet application or live financial RPC is started by these checks.

Completed verification: **26 targeted native Qt/Core/RPC cases passed** across
16 selected functions, none failed or skipped. They cover terminal current
owners and unchanged reservation/coin locks, active-only/full-history RPC lists,
eight inbox loading/failure/wallet-change scenarios, all four new-transfer reset
variants, bound session RPC actions, busy-wallet Receive selection/dialog/copy,
local/external removals, edit/cancel persistence and DGB separation, late replies
and wallet rebinding, cross-network filtering, URI/amount validation and tab
navigation. Full Core unit/Qt suites and functional runs have not been executed
for this checkpoint.

The candidate EXE SHA-256 is
`407F54F5AEA0A52336D30080C35E69166095A7191CF642045A7653EAB14D0031`.
The local `ready-artifact.json` binds the candidate EXE/PDB hashes to the source
commits and test result. Installation is pending while both normal Qt wallet
processes still use `digibyte-qt.exe`; they are not force-terminated. The installed
application still has the preceding new-transfer checkpoint's hash until the
operator closes both windows and the verified pair can be installed.

Full solution builds, complete Core/Qt suites, functional lifecycle tests and
cross-platform checks remain operator work. The full-build prerequisites/command
in the Receive DD navigation checkpoint below apply: close wallets and stop CLI
polling before linking normal executables. After that build, from
`D:\Digibyte\digibyte-fork`, run these groups (minutes):

```powershell
& .\build_msvc\x64\Release\test_digibyte.exe --run_test=paymaster_wallet_store_tests
if ($LASTEXITCODE -ne 0) { throw 'Paymaster store tests failed' }
$env:QT_QPA_PLATFORM = 'windows'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
foreach ($suite in @('DigiDollarWidgetTests', 'PaymasterWidgetTests')) {
    $env:DIGIBYTE_QT_TEST_SUITE = $suite
    $env:DIGIBYTE_QT_TEST_OUTPUT = "$PWD\build_msvc\x64\Release\selection-$suite.txt"
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw "$suite failed" }
}
```

Require exit codes zero and no failed cases; the previously recorded broad
dropdown-highlight failures remain an outstanding issue outside this change.
Manual acceptance: reopen the existing client, inspect the session inbox and
select/open/copy a stored Receive DD request during ordinary Paymaster work.
Completed history must not become an active transfer because another session
retains a validated input reservation. A genuine broken binding must still
pause new Paymaster payments.

## Another payment clears the preceding offers (2026-10-06)

Source base: `fc65fa390d`; implementation: `cac7f766b6`. Successful sending clears
the entry fields while the durable session still protects its presentation.
Start a new transfer then
cleared those fields again, relying on input-change signals to invalidate the
old offer preview. An already-empty form emits no such signal. The new
`paymasterClientNewTransferClearsOffers` regression reproduces this with the
previous Qt library: `cleared-after-success` retains one row instead of zero;
the filled-entry control case passes.

The new-transfer action now explicitly invalidates the provider choice, offer
cards, preview amounts, expiry and callback generation. It shows the existing
valid-amount prompt and updates the fee display. An invalid or empty amount uses
an input prompt instead of a computed payment-cost summary. The test also checks
that an empty new entry performs no offer read and that a subsequently entered
valid payment gets a fresh preview. Request reset changes presentation only;
stored completed payments, wallet-local limits, Core authorization and RPC/CLI
keep their existing behavior.

Relevant guidance: repository/src instructions, CLAUDE DigiDollar reading order,
architecture/maps, contribution rules, developer GUI notes, Qt translation policy
and the existing Windows/test runbooks. The selected diagnostic application and
test runner are linked separately as `digibyte-qt-new-transfer.exe` and
`test_digibyte-qt-new-transfer.exe` under `build_msvc/x64/Release/`. Helpers and
before/after test logs remain locally excluded in
`build_msvc/paymaster-refresh-check/new-transfer/`.

Completed verification: selected MSVC 14.43 / Qt 5.15.10 compiles for
`paymastersendwidget.cpp`, the client/provider Qt tests and regenerated test MOC;
separate incremental application/test links; **15 targeted native Qt cases
passed**, none failed or skipped. The new regression covers already-cleared and
filled entries in Paymaster and Automatic modes, empty-entry refresh suppression
and a fresh preview for the next payment. Existing cases cover exact read-only
offer RPCs, invalidation and failures, automatic refresh, provider cards in both
themes at 720/1200 widths, current-offer preparation gates, funding-balance changes
and exact fee amounts/percentages. This is incremental verification; the previous
Receive DD checkpoint's unrelated dropdown-highlight failures remain recorded
below.

The verified application and matching PDB replaced the normal
`build_msvc/x64/Release/digibyte-qt.exe` / `.pdb` after confirming no Qt wallet was
running. Installed EXE SHA-256:
`01BC289A3E1D029CE17A6F0922295E162C849B76A069101147C69B4BF7690333`.
The previous pair is retained as
`build_msvc/paymaster-refresh-check/new-transfer/digibyte-qt-before-new-transfer-20261006-074410.exe`
and `.pdb`. The local `installed-artifact.json` records both pairs' hashes and
implementation commit. No live payment or real wallet application was started
for verification.

Full solution builds, complete Qt suites and cross-platform checks remain
operator work. The full-build command and prerequisites in the next checkpoint
apply. After building, run the complete client/provider Qt group (minutes) from
`D:\Digibyte\digibyte-fork`:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
$env:DIGIBYTE_QT_TEST_OUTPUT = "$PWD\build_msvc\x64\Release\new-transfer-full-qt.txt"
& .\build_msvc\x64\Release\test_digibyte-qt.exe
if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt tests failed' }
```

Require exit code zero and no failed Qt cases. For manual acceptance, complete
a payment, click Start a new transfer and verify that no previous provider,
recommended offer or price remains. Enter the next recipient and amount, then
verify the fresh public-offer check and the normal exact-fee approval.

## Receive DD navigation responsiveness (2026-10-06)

Source base: `271ae55d4f`; implementation: `5d2bc232e0`. Opening Receive DD queued
its refresh on Qt's event thread, where `getAddressReceiveRequests()` then waited
for `cs_wallet`.
Paymaster work may contend for that lock, but this checkpoint does not identify
the operator's particular live lock holder. The existing busy-wallet Qt
regression now processes the queued Receive DD refresh while a worker holds the
lock. Against the preceding Qt library it failed at **511 ms**; the corrected
tab returned in **13 ms** while the wallet remained locked. The watchdog limits
the pre-fix failure to 500 ms; it is not a live-wallet benchmark.

The DD receive widget now requests an asynchronous wallet snapshot through the
existing model-owned worker pattern. Refresh requests are coalesced; unchanged
rows, selection and the last successful display survive passive reads. Wallet
rebinding and local request revisions invalidate late replies. Request writes,
original DGB receive code, Core locking, RPC/CLI and financial authority keep
their existing behavior. Relevant guidance: repository/src instructions,
CLAUDE's DigiDollar reading order, architecture/maps, contribution and developer
GUI/locking notes, Qt translation policy and Windows/test runbooks.

The diagnostic application and Qt test runner are separately linked as
`digibyte-qt-receive.exe` and `test_digibyte-qt-receive.exe` under
`build_msvc/x64/Release/`. Local build/test helpers and the before/after logs are
under `build_msvc/paymaster-refresh-check/receive/`, excluded from Git locally.
This is an incremental diagnostic build, not full release verification.

Completed verification: selected MSVC 14.43 / Qt 5.15.10 compiles for the DD
receive widget, wallet model, tab caller, DD Qt tests and regenerated test MOC;
separate incremental Qt application/test links; **17 targeted native Qt cases
passed**, none skipped. They cover the busy-wallet tab switch, coalesced refresh,
unchanged rows/selection, late replies after removal and rebinding, model closure,
request edit/cancel/remove persistence and DGB separation, network filtering,
request dialogs/validation, computer-locale dates and existing tab/history refresh.

The additional `digiDollarControlsStayReadableInBothThemes` check failed two
dropdown-highlight assertions (light and dark). The unchanged preceding
`test_digibyte-qt-recovery.exe` reproduces both failures, recorded separately in
`theme-baseline.txt`. No stylesheet changes are part of this fix. The broad theme
check is therefore an existing outstanding issue, not a passing acceptance result.

With both wallet windows closed and no process using the target, the normal
`build_msvc/x64/Release/digibyte-qt.exe` and PDB were replaced with the candidate.
Both installed hashes match their candidate files. The EXE SHA-256 is
`55CBC3DFBA38C7032BAC868C4D20F418C0B6E1F8C2DDF74CB57A13A334112724`.
The previous EXE/PDB are retained under the local `receive/` artifact folder as
`digibyte-qt-before-receive-20261006-072646.*`; `installed-artifact.json` records
the source commit, checks and both hashes. No live wallet was force-terminated.

Full solution rebuild, complete Qt suites and cross-platform checks remain
operator work (minutes or longer). From `D:\Digibyte\digibyte-fork`, use the
existing MSVC v143, static Qt 5.15.10 and installed vcpkg dependencies, with
wallet windows closed and CLI polling stopped before linking normal binaries:

```powershell
$builder = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $builder .\build_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false /m:1 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Full solution build failed' }
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'DigiDollarWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
$env:DIGIBYTE_QT_TEST_OUTPUT = "$PWD\build_msvc\x64\Release\receive-dd-full-qt.txt"
& .\build_msvc\x64\Release\test_digibyte-qt.exe
if ($LASTEXITCODE -ne 0) { throw 'DigiDollar Qt tests failed' }
```

Require build/test exit codes zero and no failed Qt cases. For manual acceptance,
switch between Send DD and Receive DD during ordinary Paymaster processing;
navigation should respond immediately even if the request list arrives later.

## Passive operator refresh (2026-10-05)

Source base: `cc5e340030`. The previous refresh used the foreground RPC gate
for every timer read, repeatedly disabling/enabling whole pages. Provider reads
also reset an already observed finance header to loading. Passive observations
now preserve enabled pages and drafts, skip unchanged form reloads, retain
wallet/provider-bound finance presentation and serialize deliberate user actions
after the in-flight read. Read elapsed time restarts for each serialized step.
Failures still invalidate readiness. Current synchronized readiness supersedes
only the known historical scheduler sync wait; worker state, unknown faults,
saved consent and the strict automatic-submit synchronization guard are unchanged.

Selected MSVC v143 / Qt 5.15.10 compilation and separate incremental
application/CLI/test links passed. The 23 targeted Core cases passed with 282
assertions: the small setup suite, non-mutating automation observations, the
readiness/synchronization/fault matrix and automatic submit deferral. All 70
targeted native Qt cases passed. They cover asynchronous passive reads in both
themes, zero page/input enabled-state transitions during polling, preserved
focus and drafts, unchanged/changed data, throttled finance, serialized single
autostart writes, late wallet/privacy replies, read failures, foreground gates,
delayed startup, work transitions, refill, exact offer values and liquidity saves.
The separately linked CLI passed its version/start check. Logs and local build
helpers are in `build_msvc/paymaster-refresh-check/` and are not release artifacts.

Run the focused checks from `D:\Digibyte\digibyte-fork` after a full current-source
build, using the existing toolchain. Runtime is seconds to a few minutes; every
process must exit zero and Qt must report no failed cases:

```powershell
& .\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_setup_tests:paymaster_wallet_identity_tests/provider_automation_capacity_pause_preserves_approval,provider_sync_observation_preserves_unresolved_gates,automatic_submit_defers_before_wallet_or_index_wait' --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Focused Core checks failed' }
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
$env:QT_QPA_PLATFORM = 'windows'
$cases = @('paymasterOperatorBackgroundRefresh', 'paymasterProviderCommandLocksPages', 'paymasterOperatorPollingPreservesDraftsAndThrottlesFinance', 'paymasterOperatorDelayedStartup', 'paymasterOperatorOverviewGuidesAndFailsClosed', 'paymasterOperatorWorkTransitions', 'paymasterOverviewFinanceWalletAndPrivacyBinding', 'paymasterOverviewAutostart', 'paymasterOverviewRefill', 'paymasterOfferPolicyTypedValues', 'paymasterCapitalOverview', 'paymasterLiquiditySaveAcrossRefresh')
foreach ($case in $cases) {
    $env:DIGIBYTE_QT_TEST_FUNCTION = $case
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw "Qt case failed: $case" }
}
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_SUITE, Env:QT_QPA_PLATFORM
```

A full build and live regtest/provider walkthrough were not performed for this
change. The full-build and RPC commands below remain operator checks. Finish
long-running CLI calls and close Qt instances regularly before linking their
normal executables; a running target can cause `LNK1104`. For live validation,
watch multiple unchanged polls, edit a draft, change autostart during a slow
read, switch wallets/privacy, mine confirmations and check actual node waits
and unknown faults. Genuine ongoing synchronization must still show its wait;
only a recovered scheduler reason should disappear.

## Provider work presentation (2026-10-04)

Source base: `6829c063bb`. Current work is derived from the loaded budget,
Capacity-admission and pool records, independently of the last scheduler phase.
Completed/committed history and expired admissions do not imply active work.
Core service reasons match the step that selects the current state. CLI and Qt
share work classification; genuine faults keep priority. Saved refill consent,
payment validation and execution authority are unchanged.

Selected MSVC v143 / Qt 5.15.10 compilation and incremental separate
application/CLI/test links were used. The 24 focused Core/wallet/CLI cases pass
with 201 assertions: the complete small setup suite plus current-work lifetime,
non-mutating refill-state transitions, pre-index/sync submit deferral and stale
submit consumption. The existing resume-fee fixture now uses saved CLI defaults
instead of an unrelated explicit new-provider fee proposal; its exact fee
assertion is retained.

All 48 targeted native Qt cases pass. They cover work transitions in both themes, historical
committed outputs, reserved capacity with free slots, scheduler waits/faults,
confirmation and return to idle, manual processing, two-second active polling,
throttled finance, preserved offer drafts, privacy, delayed startup, wallet
binding, capital overview, refill switches and exact Save policy values. Cached
polls hide an inherited loading panel; a slow-read display timer has no RPC
side effects. Application and CLI candidates are linked from the changed files
and existing objects; this is not a full current-source rebuild.

Full rebuild and live RPC/provider walkthrough remain operator checks. Use the
existing VS v143, Qt 5.15.10 and static vcpkg installation, from
`D:\Digibyte\digibyte-fork`. Build runtime is in the tens of minutes; success
requires exit code zero. Then run the selected RPC/provider tests (minutes,
isolated regtest wallets; success requires every test to pass):

```powershell
$builder = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $builder .\build_msvc\digibyte.sln /m:2 /p:Configuration=Release /p:Platform=x64 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false
if ($LASTEXITCODE -ne 0) { throw 'Full build failed' }
$env:DIGIBYTED = (Resolve-Path .\build_msvc\x64\Release\digibyted.exe).Path
$env:DIGIBYTECLI = (Resolve-Path .\build_msvc\x64\Release\digibyte-cli.exe).Path
python -X utf8 test/functional/test_runner.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_lifecycle.py -j1 --descriptors
if ($LASTEXITCODE -ne 0) { throw 'Provider/RPC checks failed' }
```

Native acceptance: with client and provider open, request a quote, approve the
payment, broadcast, confirm the successor outputs, and observe reserved capacity,
payment work, confirmation wait and return to the current operating state.
Check that header/hero agree, manual processing never promises automatic
continuation, genuine errors remain actionable, offer drafts survive navigation,
and refill consent survives temporary capacity use. Snapshots refresh every two
seconds while active and visible; shorter phases can fall between reads. Hidden
pages/privacy and an outstanding read must not create overlapping polling.

## V5 payment artifacts after the V6 upgrade (2026-10-04)

Source base: `80ee78557f`. The V6 announcement update inadvertently rejected
unchanged V5 Capacity proofs, including discharged provider replay barriers.
Direct payloads now accept exactly V5/V6 with the existing full validation;
announcement/connection negotiation and inner-record requirements stay strict.
No signed artifact, replay key, reservation or spending approval is rewritten.

Completed using the existing MSVC v143 / Qt 5.15.10 toolchain:

- Selected compilation of six affected Core/wallet/RPC sources and four test
  sources; incremental common/wallet archive and separate application/test links.
- 28 distinct focused Core/wallet cases passed (1,244 assertions), covering
  exact V5/V6 proofs, signature mutations, rejected pre-V5/future payloads,
  original signed V5 provider commits after expiry, wallet reload, exact replay,
  atomic reservation release/fault injection, non-authorizing recovery evidence,
  client finality, budget enforcement and idempotency.
- The functional P2P test now expects V6 negotiation and rejects V0/V5/V7.
  Python AST syntax and `git diff --check` passed; the live P2P test was not run.
- The normal Qt executable was replaced with wallets closed and both candidate
  and backup SHA256 verified. Artifacts/logs are under
  `build_msvc/paymaster-v5-compatibility-check/`; the previous executable is
  `digibyte-qt-before-v5-compatibility.exe` in that directory.

Installed `build_msvc/x64/Release/digibyte-qt.exe` SHA256:
`6620D32C599F39A2516AD40A19BD61CA8076EB8B25E9AFC3171460E7559DD10D`.

This is targeted incremental verification. Full builds/suites, live upgraded
client/provider tests and cross-platform builds remain operator checks. Relevant
guidance: repository/src instructions, DigiDollar/Paymaster architecture/maps,
developer notes and Core/wallet/functional/MSVC test instructions.

Run the following from `D:\Digibyte\digibyte-fork`, with the normal wallets
closed, using the existing static Qt/vcpkg, MSVC, Python and wallet-test
prerequisites described below. Incremental builds and the broader focused
suite take minutes; clean builds and multi-node tests can take substantially
longer. Success requires exit 0 and all requested tests passing with required
components enabled. The daemon must be rebuilt before live RPC/P2P checks:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\build_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false /m:1 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'V5 compatibility build failed' }
& .\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_wire_tests:paymaster_wallet_identity_tests:paymaster_wallet_psbt_tests:paymaster_wallet_store_tests' --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'V5 compatibility Core/wallet checks failed' }
$env:DIGIBYTED = (Resolve-Path .\build_msvc\x64\Release\digibyted.exe).Path
$env:DIGIBYTECLI = (Resolve-Path .\build_msvc\x64\Release\digibyte-cli.exe).Path
python -X utf8 test/functional/test_runner.py wallet_paymaster_provider.py wallet_paymaster_lifecycle.py wallet_paymaster_rpc.py -j1 --descriptors
if ($LASTEXITCODE -ne 0) { throw 'V5 compatibility RPC/provider checks failed' }
python -X utf8 test/functional/test_runner.py p2p_paymaster.py -j1
if ($LASTEXITCODE -ne 0) { throw 'V6 negotiation checks failed' }
```

## Selectable public offers and app number format (2026-10-04)

Source base: `902b12b203`, retaining the existing uncommitted financial/protocol
and offer-policy fixes. Main-view provider cards show exact fee/effective rate
and total, recommend/preselect the cheapest suitable offer, preserve explicit
choices on refresh and send paired first-provider/offer preferences to Core.
Unsigned failed contacts, authenticated rejections and confirmed payments populate the existing
idempotent local reliability history. First unsuccessful attempts without a
known success sort after available alternatives, even below five samples.
No request-hash, wallet-record or signing-authority format changes were made.

This request supersedes the locale-dependent monetary presentation recorded in
the 2026-10-03 checkpoint below: all Paymaster money editors now use the app's
decimal point, and large readouts use `DigiByteUnits` thin-space grouping.
German system locale does not alter money parsing; dates remain locale-aware.
DD/DGB values remain integer cents/satoshis. RPC/CSV formats are unchanged.

Relevant guidance: repository/src instructions, DigiDollar architecture and
maps, Qt README/translation rules, developer notes and functional/MSVC test
guidance. Full builds/suites remain delegated operator work.

Completed with MSVC v143 / Qt 5.15.10:

- Selected compilation of the affected client, wallet RPC/store, node RPC and
  Qt sources/tests; test MOC and CSS resources regenerated. Existing libraries
  were archived with the newly compiled objects and separately linked into
  `digibyte-qt-offer-cards.exe`, `test_digibyte-qt-offer-cards.exe` and
  `test_digibyte-offer-cards.exe`. Historical alternate RPC object paths were
  explicitly replaced with these current compiled objects.
- **30 native Qt cases passed**, none skipped: four card/theme/width cases,
  app-standard formatting with C/German defaults, ten decimal/draft-save cases,
  four aligned-field layouts, fee calculations, both preparation gates, both
  preview regressions, automatic refresh, exact two-stage authority,
  configuration workflow, funding-balance changes and two real Core unsigned
  cancellation round trips (reopened/lost first reply). Card cases also cover
  keyboard selection, literal untrusted names, fresh cheaper recommendations,
  preserved manual choice, privacy masking, amount invalidation, selected-offer
  loss and a delayed reply after wallet closure.
- **18 Core/wallet cases passed**: the ten client-selection cases plus paired
  public preference validation, idempotent unsigned availability observation,
  confirmed payment without provider result (including one success observation),
  existing mempool/reorg/retention reconciliation, signed rejection followed by
  confirmed success, rejection after prior confirmation, atomic/monotonic final
  result handling and existing outcome uniqueness/local clearing. Failure/
  availability-to-success updates include write/commit failure injection and
  duplicate success checks. Signed authorization/recovery protections remain.
- Before committing this series, **10 additional fee-cap Core/wallet cases
  passed** (150 assertions), covering capped arithmetic, provider fee/output
  boundaries and policy validation/hash binding, client quote binding, V6/V5
  announcement layouts, CLI numeric input, setup validation and provider-policy
  backup round trips. The targeted executable was reused; no full solution
  build was run. Evidence: `paymaster-offer-cards-check/cap-core-tests.txt`.
- Native captures in both themes at 720/1200 pixels were visually inspected;
  no horizontal card overflow, monetary locale mismatch or blue scroll track.
- Extended `wallet_paymaster_offer_selection.py` with explicit more expensive
  provider choice, missing selected offer, request-ID conflict and unsigned
  cancellation. Python AST syntax checking passed; the real-node test was
  **not run** here. `git diff --check` passed.

Artifacts and individual test logs are in
`build_msvc/paymaster-offer-cards-check/`. This is a targeted incremental link,
not a full solution rebuild. Full suites, real-node selection/offline/restart
acceptance and cross-platform builds remain unverified.

The regular `build_msvc/x64/Release/digibyte-qt.exe` was updated with wallets
closed and verified against the final candidate. The preceding executable is
preserved as `paymaster-offer-cards-check/digibyte-qt-before-offer-cards.exe`.
Final application SHA256:
`7810FC50158B0F811FF26A946DC9295B8253902A1BDE6C1C46F34532649D9B7C`.

After the [Windows build](#windows-build), run from
`D:\Digibyte\digibyte-fork` with the wallet/SQLite, static Qt and Python
functional-test prerequisites documented there. Focused tests take seconds to
minutes; the real-node tests can take several minutes or longer. Success requires
exit 0, all requested cases passing, and no skipped required components:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    foreach ($case in @('paymasterClientOfferCards', 'paymasterAppNumberFormat', 'paymasterOfferPolicyTypedValues', 'paymasterClientAuthorizationIsTwoStageAndFailClosed')) {
        $env:DIGIBYTE_QT_TEST_FUNCTION = $case
        & .\build_msvc\x64\Release\test_digibyte-qt.exe
        if ($LASTEXITCODE -ne 0) { throw "Paymaster Qt check failed: $case" }
    }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
& .\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_client_tests:paymaster_wallet_store_tests/send_preferred_public_offer_is_an_explicit_pair:paymaster_wallet_store_tests/unavailable_unsigned_provider_observation_is_idempotent:paymaster_wallet_store_tests/client_confirmed_payment_without_provider_result:paymaster_wallet_store_tests/final_transaction_observations_handle_mempool_reorg_and_retention' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster Core checks failed' }
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python test/functional/test_runner.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster selection/failover checks failed' }
```

## Offer policy decimal editing and alignment (2026-10-03)

Source base: `902b12b203`, retaining the existing uncommitted service-fee-cap
changes. Qt's scaled QSpinBox controls overrode decimal display/parsing but kept
Qt's whole-number validator. A regression using actual keyboard entry of
German `0,80 %` reproduced a transmitted `fee_rate_bps` of 8000 instead of 80
against the preceding Qt library. See `before-fix.txt` below. No operator policy
was changed by this injected-RPC test.

The corrected controls share locale-aware validation and integer decimal
conversion. Unsupported precision, mixed separators, negative/out-of-range and
exponential inputs cannot be submitted as another value. Invalid text survives
focus loss before Save; the form retains drafts on RPC/acknowledgement failures
and ignores stale readback of a successfully saved policy. Save feedback stays
on the offer page. Identity, policy and expanded-limit label columns are shared;
fee examples participate in their parent's form in both settings and setup.

Relevant guidance: repository/src constraints, DigiDollar architecture/maps,
Qt README, translation policy, developer coding rules and Windows/test runbooks.
English UI feedback remains translatable. This fixes GUI parameter preparation;
CLI/RPC integer-value parsing is unchanged by this follow-up.

Completed verification with MSVC 14.43 / Qt 5.15.10:

- Selected compilation of `qt/paymasterwidget.cpp`,
  `qt/test/paymasterwidgettests.cpp` and regenerated test MOC.
- Incremental Qt archive and separate application/test links:
  `build_msvc/x64/Release/digibyte-qt-policy-save.exe` and
  `test_digibyte-qt-policy-save.exe`, using the existing current Core libraries.
- Native Windows Qt: all 16 selected cases passed without skips. Ten cases
  cover English/German typed percentages, DD caps/ranges, eight-decimal DGB
  ceilings, save/error/stale-readback, partial drafts across refresh and invalid-input focus loss. Four check
  matching input left/right edges at 640/1200 widths with real light/dark CSS.
  Fee calculation/percentage and configuration workflow regressions also pass.
- `git diff --check` passed. Existing uncommitted changes were retained.

After the operator closed both wallets, the normal
`build_msvc/x64/Release/digibyte-qt.exe` was replaced with the updated executable;
SHA-256 equality was verified. The preceding executable is retained at
`build_msvc/paymaster-policy-save-check/digibyte-qt-before-policy-save.exe`.
No wallet process was terminated and no live policy was changed.

Logs, link response files and full offer-content captures are in
`build_msvc/paymaster-policy-save-check/`. Final native logs have the prefix
`windows-`. Early minimal-platform geometry checks did not load the actual CSS;
final layout checks load it and run with `QT_QPA_PLATFORM=windows`. The minimal
platform also crashed in its native QMessageBox error path. The final inline
save-error handler avoids that modal path and passes on Windows. The old
fee-display fixture's local button assertion now respects its no-wallet
ancestor gate; production onboarding remains gated.

For a full build, use the exact prerequisites and commands under
[Operator verification for this integration](#operator-verification-for-this-integration).
That build may take several minutes or longer and remains operator work. After
building, run from `D:\Digibyte\digibyte-fork` in PowerShell:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
try {
    foreach ($check in @('paymasterOfferPolicyTypedValues', 'paymasterOfferFormAlignment', 'paymasterFeeAmountsAndPercentages', 'paymasterInjectedRpcCoversConfigurationWorkflows')) {
        $env:DIGIBYTE_QT_TEST_FUNCTION = $check
        & .\build_msvc\x64\Release\test_digibyte-qt.exe
        if ($LASTEXITCODE -ne 0) { throw "Offer policy regression failed: $check" }
    }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
```

These focused tests take seconds. Success requires exit 0 with all selected
cases passing. A live operator save/restart persistence check, full solution
build, full Core/RPC suites and cross-platform runs were not performed here.

## Paymaster wallet responsiveness (2026-10-01)

Source base: `07e3392177`. This follow-up reduces work under the wallet lock in
Paymaster balance/coin-selection hooks and finance reconciliation. It does not
change Qt tab handling, service frequency, reservations or spending authority.
Relevant guidance: repository/src instructions, CLAUDE's DigiDollar reading
order, contribution/developer locking rules, wallet architecture/maps, test and
Windows build guidance. Full builds remain operator work.

Selected MSVC 14.43 compilation passed for `wallet/paymasterstore.cpp`,
`wallet/digidollarwallet.cpp`, `wallet/spend.cpp`,
`wallet/paymasterstore_reconciliation.cpp`, `wallet/rpc/paymaster.cpp` and
`wallet/test/paymaster_wallet_identity_tests.cpp`. The current wallet objects
were archived and linked with the existing integration libraries into separate
`digibyted-paymaster-performance.exe`, `test_digibyte-paymaster-performance.exe`
and `digibyte-qt-paymaster-performance.exe` under `build_msvc/x64/Release/`.
With both Qt wallets closed, the normal `build_msvc/x64/Release/digibyte-qt.exe`
was replaced with the linked performance executable; SHA-256 equality was
verified. Its previous executable is retained as
`build_msvc/paymaster-performance-check/digibyte-qt-before-performance.exe`.
No running wallet process was terminated.

This is an incremental diagnostic build, not a complete rebuild. As in the
RPC/CLI checkpoint, the diagnostic Core binary excludes three unrelated old
test objects that need recompiling against the changed DD-selection signature.

Completed checks:

- The new `paymaster_input_scan_reuses_pool_without_cross_scan_cache` test checks
  512 candidate inputs against a valid 256-entry pool. Repeated single-input
  calls took 5,056,219 microseconds; one scoped scan took 9,676 microseconds on
  this machine. This compares the helper's single-input compatibility path with
  the batched path, not two separately built historical releases. Both returned
  identical results. The deterministic assertion is **512 database batches to
  one**, with no machine-dependent timing threshold.
- The same test exercises production DD balance/UTXO hooks, explicit and pool
  reservations, release, transaction abort, unreadable and unsupported records.
  Each new scan sees the current database; unreadable authority remains locked.
- All 25 selected Core cases passed, with 1,184 assertions: the full
  `paymaster_wallet_identity_tests` suite plus store regressions for shared
  coin locks, final-transaction mempool/reorg/retention observations, atomic
  provider quote expiry and idempotent carrier/DGB successor reconciliation.
- `wallet_paymaster_lifecycle.py --descriptors` passed, including provider
  restart, restored-payment RPC/CLI paths and reserve/autostart persistence.
- `wallet_paymaster_rpc.py --descriptors --client-preparation-only` passed,
  covering HTTP and actual CLI preparation/cancellation and read-only status.
- `wallet_paymaster_reorg.py --descriptors` passed: finance and pool state roll
  back on a real longer branch, then reconfirm without duplicate accounting.

Logs and response files are under `build_msvc/paymaster-performance-check/`;
selected compilation logs are `build_msvc/paymaster-performance-compile.log`
and `build_msvc/paymaster-performance-test-compile.log`. Test nodes were isolated
from operator wallets. No live payment or reserve state was changed.

### Remaining operator checks

The bounded timing test demonstrates eliminated repeated work, not measured
end-to-end GUI latency. After restarting with the updated executable, switch
between Overview, Send DD, Receive DD and Paymaster in both client and provider
wallets. Repeat while the provider is active and while status refreshes run.
Expected: the previous multi-second pauses are reduced; payments and status
updates continue. Record any remaining pause with its time, wallet and tab.
The [operator diagnostics](digidollar-paymaster-operator.md#diagnosing-brief-pauses-when-changing-wallet-tabs)
explain opt-in reconciliation timing. This native GUI check has not been run.

For a complete build, use the exact commands and prerequisites under
[Operator verification for this integration](#operator-verification-for-this-integration).
Allow minutes or longer for the build; the full functional contract may take
tens of minutes. After that build, from `D:\Digibyte\digibyte-fork`:

```powershell
& .\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_wallet_identity_tests:paymaster_wallet_store_tests/reservations_share_standard_wallet_locks:paymaster_wallet_store_tests/final_transaction_observations_handle_mempool_reorg_and_retention:paymaster_wallet_store_tests/expired_provider_quotes_release_pool_and_budget_atomically:paymaster_wallet_store_tests/provider_successor_reconciliation_recycles_carrier_and_dgb_idempotently' --log_level=message --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Wallet regression failed' }
$env:DIGIBYTED = (Resolve-Path .\build_msvc\x64\Release\digibyted.exe).Path
$env:DIGIBYTECLI = (Resolve-Path .\build_msvc\x64\Release\digibyte-cli.exe).Path
python -X utf8 test/functional/test_runner.py wallet_paymaster_rpc.py wallet_paymaster_lifecycle.py wallet_paymaster_reorg.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster regression failed' }
```

Success requires exit 0 without unexpected skipped selected cases. Full suites,
cross-platform builds and live native GUI verification are not claimed here.

## Uncreated client request and CLI status checks (2026-10-01)

Source base: PR #452 integration `44b70ddea7`. This follow-up separates confirmed
session absence from unreadable/versioned state. Qt can leave a failed initial
prepare-only attempt after authoritative absence, while known sessions remain
protected. The same lookup errors and detailed DD selection reason reach CLI
clients. See the [client contract](digidollar-paymaster-integration.md#failed-preparation-and-session-lookup).

Verification completed: selected MSVC 14.43 / Qt 5.15.10 compilation of seven
translation units (`paymasterstore_client`, wallet RPC `paymaster`,
`paymaster_client`, `paymaster_discovery`, Qt `paymastersendwidget`, wallet store
tests and Qt Paymaster tests), plus Python syntax and Git whitespace checks.
Objects/logs are isolated under `build_msvc/paymaster-absence-check/`. No
application/test executable was relinked and no operator wallet was changed.
At that checkpoint the new runtime tests below were **pending**. The later
[RPC/CLI parity checkpoint](#rpccli-recovery-parity-2026-10-01) executes the three
native checks and the focused preparation/cancellation contract with current
objects linked. Its results do not cover the pending Qt tests or full RPC suite.

Use the full build command in
[Operator verification for this integration](#operator-verification-for-this-integration)
first, including `msvc-autogen.py` to refresh generated configuration/projects.
Working directory and prerequisites remain `D:\Digibyte\digibyte-fork`, the
installed MSVC/SDK/Python/static vcpkg dependencies and Qt at
`D:\Qt51510\install`. Allow minutes for native tests and tens of minutes or
longer for the functional contract test. Success requires exit 0 and every
selected case passing without unexpected skips. Return the failing command,
exit code and complete output for investigation.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$checks = @(
    'paymaster_wallet_store_tests/session_lookup_distinguishes_absence_from_read_failure',
    'paymaster_wallet_store_tests/auto_dispatch_rejects_unreadable_session_before_funding_or_signing',
    'paymaster_wallet_store_tests/ambiguous_authorization_is_protected_and_prunes_to_tombstone'
)
foreach ($check in $checks) {
    & .\build_msvc\x64\Release\test_digibyte.exe "--run_test=$check" --report_level=short
    if ($LASTEXITCODE -ne 0) { throw "Native regression failed: $check" }
}
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    foreach ($check in @('paymasterClientUncreatedRequestReturnsToCompose', 'paymasterClientLiveSendProgressesAcrossAsyncPhases', 'paymasterClientCoreCancellationRoundTrip', 'paymasterClientSessionDiscoveryFailures')) {
        $env:DIGIBYTE_QT_TEST_FUNCTION = $check
        & .\build_msvc\x64\Release\test_digibyte-qt.exe
        if ($LASTEXITCODE -ne 0) { throw "Qt regression failed: $check" }
    }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:DIGIBYTED = (Resolve-Path .\build_msvc\x64\Release\digibyted.exe).Path
$env:DIGIBYTECLI = (Resolve-Path .\build_msvc\x64\Release\digibyte-cli.exe).Path
python -u test/functional/test_runner.py wallet_paymaster_rpc.py -j1
if ($LASTEXITCODE -ne 0) { throw 'RPC/CLI regression failed' }
```

The functional test explicitly exercises both HTTP RPC and real
`digibyte-cli` calls: failed PAYMASTER/AUTO preparation against a cached offer
from a paused provider, unchanged balances/reservations, authoritative absence,
and CLI cancellation of an existing unsigned request while the provider stays
paused. The Qt tests include real Core lookup/read-failure paths, an ambiguous
legacy missing-session response and disappearance of a known session. Existing
lost-first-reply tests ensure a created request is reconciled rather than reset.
The preceding integration's broader runtime matrix and previously observed
light-theme failure remain open; this follow-up does not certify them.

## PR #452 source integration (2026-10-01)

Source baseline: fork `504489f447cb` plus official PR #452 at
`92330d952625e20aef2ee40671a179ef03872ac1`. See the
[integration notes](digidollar-paymaster-v9.26.6rc2-integration.md#official-pr-452-integration-2026-10-01)
for conflict decisions and the upstream/funded planner split.

Checks performed on the integrated source:

- Ten selected translation units compiled with MSVC 14.43 / Qt 5.15.10:
  `wallet/digidollarwallet.cpp`, wallet RPC `paymaster.cpp`,
  `paymaster_provider.cpp`, `paymaster_send.cpp`, `transactions.cpp`,
  `qt/digidollarsendwidget.cpp`, `qt/walletmodel.cpp`, `rpc/digidollar.cpp`,
  `wallet/test/digidollar_wave17_spendability_tests.cpp` and
  `qt/test/digidollarwidgettests.cpp`.
- Compilation used `/t:ClCompile`, verified single-file selection and isolated
  objects under `build_msvc/pr452-check/`. No application or test executable was
  linked. MSBuild emitted MSB8028 intermediate-directory warnings while
  evaluating referenced test projects; the selected compiles returned zero.
  Existing generated build configuration was retained for these checks.
- `python -B contrib/thawday/test_rehearsal.py`: all seven tests passed.
- Python syntax, whitespace/conflict-marker and upstream source-preservation
  checks passed. The official release document is unchanged.

The C++/Qt regression sources compiled but have **not run against rebuilt
integrated binaries**. The planner tests cover upstream preflight without DGB,
DGB-denominated fee errors, typed AUTO-fallback boundaries and non-mutating
funded plans. The Qt regression retains rejection of unusable selected DD inputs
before confirmation. The pre-existing light-theme dialog failure in the next
checkpoint remains unresolved until checked on the rebuilt integration.
Previous dated passing results do not certify this source revision.

### Operator verification for this integration

Working directory: `D:\Digibyte\digibyte-fork`. Prerequisites: the installed
Visual Studio v143/MSVC 14.43 toolchain, Windows SDK, Python, Qt 5.15.10 at
`D:\Qt51510\install` and the existing static vcpkg dependencies. No dependency
installation is requested. Close programs running from the build outputs
before rebuilding; the repository build copies executables into `src/`.
Allow several minutes or longer for the solution build, minutes for native
regressions and tens of minutes or longer for the functional matrix. These
commands are pending operator work, not completed verification.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
python .\build_msvc\msvc-autogen.py
if ($LASTEXITCODE -ne 0) { throw 'Project/configuration generation failed' }
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\build_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false /m:1 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'PR 452 integration build failed' }

$unitChecks = @(
    'digidollar_wave17_spendability_tests',
    'wallet_tests/transaction_rpc_keeps_coin_merge_state_private',
    'paymaster_wallet_identity_tests/automatic_submit_defers_before_wallet_or_index_wait',
    'paymaster_wallet_identity_tests/stale_submit_is_consumed_without_faulting_provider',
    'paymaster_wallet_store_tests/client_confirmed_payment_without_provider_result',
    'paymaster_wallet_store_tests/client_final_observation_is_atomic_and_idempotent'
)
foreach ($unitCheck in $unitChecks) {
    & .\build_msvc\x64\Release\test_digibyte.exe "--run_test=$unitCheck" --report_level=short
    if ($LASTEXITCODE -ne 0) { throw "Unit regression failed: $unitCheck" }
}

$env:QT_QPA_PLATFORM = 'windows'
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    $env:DIGIBYTE_QT_TEST_SUITE = 'DigiDollarWidgetTests'
    $env:DIGIBYTE_QT_TEST_FUNCTION = 'sendWidgetCoinControlDialogSelectionFeedsSend'
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Selected-input Qt preflight failed' }
    Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION
    $env:DIGIBYTE_QT_TEST_SUITE = 'DigiDollarMintRecordTests,DDTransactionTableTests'
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Official mint/history Qt regressions failed' }
    $env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt regressions failed; retain complete output' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:DIGIBYTED = (Resolve-Path .\build_msvc\x64\Release\digibyted.exe).Path
$env:DIGIBYTECLI = (Resolve-Path .\build_msvc\x64\Release\digibyte-cli.exe).Path
$env:DIGIBYTEWALLET = (Resolve-Path .\build_msvc\x64\Release\digibyte-wallet.exe).Path
$env:DIGIBYTEUTIL = (Resolve-Path .\build_msvc\x64\Release\digibyte-util.exe).Path
python -u test/functional/test_runner.py digidollar_mint.py digidollar_mint_consolidation.py digidollar_rpc_addresses.py wallet_digidollar_descriptors.py wallet_digidollar_rc33_regressions.py wallet_fundrawtransaction.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_lifecycle.py wallet_paymaster_operator.py -j1
if ($LASTEXITCODE -ne 0) { throw 'PR 452 / Paymaster functional regressions failed' }
```

Success requires build exit 0, every selected native/Qt case passing and every
selected functional variant passing, without unexpected skips. Return the
failed command, its exit code and complete output if a check stops. Final release
acceptance, reference compatibility and cross-platform builds remain separate
operator gates.

## Commit checkpoint (2026-10-01)

The working changes based on `7ad04ea744` were separated into focused local
commits. Source files were preserved; no new build or full functional suite was
run for this checkpoint. Individual intermediate commits were not rebuilt.

Four focused tests passed using the existing Windows `test_digibyte.exe`
(90 assertions):

- `paymaster_wallet_identity_tests/automatic_submit_defers_before_wallet_or_index_wait`
- `paymaster_wallet_identity_tests/stale_submit_is_consumed_without_faulting_provider`
- `paymaster_wallet_store_tests/client_confirmed_payment_without_provider_result`
- `paymaster_wallet_store_tests/client_final_observation_is_atomic_and_idempotent`

The existing Windows Qt test binary ran
`PaymasterWidgetTests::paymasterClientSessionRpcActionsAreBound` with
`QT_QPA_PLATFORM=windows` and failed its light-theme dialog palette assertion:
actual `#0b2419`, expected `#eef9f2`. This checkpoint does not establish the cause
or claim that the Qt regression passes. The earlier dated results below remain
historical evidence. An initial offscreen launch did not reach the test and was
terminated; it is not counted as a test result.

Review the theme failure and rebuild the intended source revision before using
these commits as a verified integration candidate. Python syntax and Git
whitespace checks supplement, but do not replace, the build and runtime matrix.

## Pending payment, blank provider page and dialog theme (2026-10-01)

A local read-only investigation observed a queued provider submit, a transaction
index behind the chain tip and a finance status request timing out. The nested
`submitpaymasterdigidollar` call from automatic processing still performed wallet
and final-preflight index synchronization waits on the validation scheduler.
These three waits now preserve the automatic service's nonblocking context;
manual RPC synchronization and financial authorization remain unchanged.

Qt no longer disables painting for the lifetime of an asynchronous status chain.
Navigation, window exposure and resizing keep the last verified page visible;
reads lasting two seconds expose their step and elapsed time. Conflicting
commands remain gated. Provider dialogs, including refill approval, inherit
both the active surface colors and green buttons.

Targeted Windows verification (MSVC 14.43, Qt 5.15.10):

- Compiled the changed wallet RPC, Qt widget/resources and affected test sources;
  relinked wallet/Qt archives, daemon, test executables and operator GUI.
- `automatic_submit_defers_before_wallet_or_index_wait`: passed with a mismatched
  wallet tip, verifies the readiness error precedes PSBT parsing and database writes.
- `paymasterProviderCommandLocksPages`: passed navigation, resize and actual paint
  events while the asynchronous status reply is withheld, plus error/wallet guards.
- `paymasterOverviewRefill`: 15 cases passed, including native dark/light approval
  palette checks, cancellation, stale revisions, lost responses and wallet/privacy
  changes. Both dialog renderings were inspected.
- `paymasterFinancesAndBackupWorkflow`: passed.
- `wallet_paymaster_provider.py --descriptors`: passed, including three empty
  blocks while an authorized automatic payment completes, exact retries,
  capital actions, finance accounting and restart.

Both `build_msvc/x64/Release/digibyte-qt.exe` and the separately named
`digibyte-qt-operator.exe` now contain this fix. The operator terminated the old
Paymaster process after its shutdown stalled. Before that, the wallet file and
journal were copied with unchanged source hashes; SQLite integrity checking
passed on a separate copy. After verifying that no GUI process remained, the
regular executable was backed up and replaced, and its SHA256 was checked
against the tested candidate. No live payment was resent or cancelled and no
wallet was restarted by this work. Inspect the existing payment's status after
restart instead of creating a replacement request. Full builds and suites
remain operator checks using the commands below.


## Deep operator redesign (2026-10-01)

The current candidate separates operation from backup, mirrors immediate
startup/refill controls, consolidates capital approvals, uses revision-bound
liquidity writes and exposes node configuration provenance in an inline editor.
The operator/design documentation describes the actual page responsibilities.
Repository and src/ instructions, CLAUDE/DigiDollar architecture and maps,
Qt/translation and functional-test guidance apply. No consensus, payment wire
or wallet database migration is involved.

Verification on Windows, MSVC 14.43 / Qt 5.15.10:

- Targeted compilation of Qt implementation/tests/MOC, wallet RPCs, node RPCs
  and CLI conversion; affected archives and native test/daemon/GUI relinked.
- 149 selected Qt cases in 26 functions passed, excluding init/cleanup counts.
  This includes 14 refill cases, 10 autostart cases, 24 common capital cases,
  22 stop/release cases, 10 late retirement replies, 10 connection editor cases,
  16 responsive settings layouts, and 4 scaled/themed task layouts.
- The operator functional test passed revision conflicts (including CLI),
  persisted refill intent across pause/reload, config sources/conflicts and
  effective-versus-pending values. The provider functional test passed exact
  DGB-only fee-bound execution, rejected insufficient caps without pool changes,
  retained DD reserves, legacy rebalance, transfers, finance and restart.
- The provider test's manual-submit polling now retries only the explicit
  PAYMASTER_PROVIDER_SERVICE_BUSY rejection; other RPC errors still fail it.
  An initial run exposed this contention. A later insufficient-cap fixture
  correctly failed at 0.01 DGB (actual minimum fee 0.0169 DGB); the success
  fixture now approves 0.20 DGB and separately tests a one-satoshi rejection.
- Native light/dark views and the existing keyboard/layout checks were examined.
  A headless Edge interaction check passed immediate autostart, refill approval,
  cancellation and pause persistence in the example HTML. Python syntax and
  git diff --check passed. Existing generic Qt stylesheet property warnings
  do not fail the cases.

This is targeted verification, not a full solution build, complete Qt/Core
suite, cross-platform test or production acceptance. Run the existing
[Windows build](#windows-build) first for a clean complete build. Prerequisites
are the documented v143 C++ tools, static Qt 5.15.10, existing vcpkg libraries,
Python functional-test dependencies (including digibyte_scrypt), and a matching
test/config.ini. Full builds take tens of minutes or longer; the following
suite checks take minutes to tens of minutes. Every selected test must execute
with exit 0; skips or stale executables do not count.

From D:\Digibyte\digibyte-fork in PowerShell, after that build:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
& .\build_msvc\x64\Release\test_digibyte-qt.exe
if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt tests failed' }
& .\build_msvc\x64\Release\test_digibyte.exe --run_test=paymaster_wallet_identity_tests,paymaster_setup_tests,paymaster_provider_tests
if ($LASTEXITCODE -ne 0) { throw 'Paymaster Core tests failed' }
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
python -X utf8 test/functional/test_runner.py wallet_paymaster_operator.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster functional tests failed' }
```

Both the regular digibyte-qt.exe and the separately named digibyte-qt-operator.exe
contain this candidate. The regular executable was relinked only after checking
that the original GUI processes were no longer running. No operator process was
stopped, wallet opened or live configuration changed by this work. Use the usual
network/configuration/wallet arguments and run only one instance per data dir.


## Rejected single-reserve release (2026-10-01)

A guided release could reuse a previously populated carrier selector. Core
correctly rejected an output that had since become reserved, unconfirmed,
spent or released with `PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE`; the task showed
only the generic error code.

The GUI now rereads `getpaymasterpoolinfo` before every single-reserve selection,
validates it, and binds the preview to the displayed outpoint even if a nested
modal event changes the live selector. A rejected release explains the likely
states and provides **Refresh reserves**, which only rereads the pool and
operator status. The task is accurately named **Release one DD reserve**.
Technical details are initially collapsed and cleared on wallet/privacy changes.
Core's release, reservation, confirmation, policy and plan guards are unchanged.

Focused verification: 43 test cases from five selected Qt functions passed,
with no failed or skipped cases (excluding init/cleanup). The capital regression covers
nine added rows for a changed old selection, reordered selector, no eligible
reserve, malformed pool, failed pool read, preview/execution rejection, changed
plan and a zero saved target. It checks that Refresh reserves performs no
preview, pause or execution. Existing approval/cancel, wallet change, lost-reply,
restoration, fee recovery and themed task-layout checks are included.
The affected widget/tests were compiled and the Qt archive and test executable
relinked. The existing GUI executable is locked by running instances, so the
same GUI link produces `build_msvc/x64/Release/digibyte-qt-release-fix.exe`.
Close the running GUI normally before starting that build with the intended
wallet/network arguments. The native rejected-release view was captured and inspected;
`git diff --check` passed. No live wallet was queried or modified. Full builds
and complete suites remain operator work under the commands below.

## Operator navigation and visible Autostart (2026-10-01)

This presentation revision supersedes the older navigation acceptance below.
The operator has five destinations with a responsive sidebar/selector, focused
settings pages, a persistent task card and seven numbered setup steps. Autostart
is beside Start/Pause with independent saved/draft state and Save/Discard.
No Core, consensus, RPC schema or spending-policy change was made for this UI
revision; earlier uncommitted Core work remains part of the wider candidate.

Guidance applied: CLAUDE and its DigiDollar reading order, CONTRIBUTING,
ARCHITECTURE, repository maps, developer notes, src/AGENTS, C++ formatting,
Qt component guidance and the translation-string policy. New native strings
use tr(); catalog updates remain in the repository translation workflow.

Local validation used MSVC 14.43.34808 and Qt 5.15.10 static. Only the affected
widget, two test translation units, test MOC and Qt resources were compiled.
The Qt archive, test executable and GUI were explicitly relinked against the
existing dependency objects. This is targeted build evidence, not a clean or
complete solution build. The executable is
`build_msvc/x64/Release/digibyte-qt.exe`; no real wallet was opened for validation.

Focused Qt results: 25 selected functions / 135 test cases passed, with no
failed or skipped cases (excluding init/cleanup counts). Coverage includes:

- Eleven Autostart scenarios: enable, disable, already running, discard, lost
  reply after saving, lost reply without saving, malformed reply, wallet change,
  privacy, persistent pause, and an independent unsaved processing-mode edit.
  The fixture checks the exact one-field RPC and prevents a duplicate mutation.
- Five destinations, responsive selection, first-visit access, disabled pages
  while a command runs, normal and enlarged fonts, light/dark layouts, and
  task progress outside the scrollable page.
- Seven numbered setup pages, bounded approvals/retries, delayed startup,
  wallet-bound confirmations, reserve save/refresh, configuration/runtime,
  finance/backup/privacy, capital tasks, restoration, stop and late results.
- The embedding DigiDollar widget assertions, including setup and amounts.

Native narrow/wide and light/dark screenshots were captured; representative
renders were inspected. The standalone HTML preview was exercised in local
headless Edge for navigation, categories, Autostart draft/save/discard, reviewed
start/pause and all seven illustrative setup steps. It returned `passed`.
Existing non-fatal Qt table icon-property warnings remain. Formatting was
checked with `git diff --check`.

Operator acceptance remains: a full incremental/clean solution build, complete
unit/Qt/regtest groups, and a manual run in an isolated test wallet. Use
[Windows build](#windows-build) and the group commands immediately below.
Run from `D:\Digibyte\digibyte-fork` with the documented v143 toolset, static Qt,
vcpkg dependencies and Python. Expect minutes to tens of minutes for a full
build and minutes for the selected groups. Require exit code 0, no failed or
skipped requested cases, and freshly built matching node/CLI/test executables.
These expensive checks were not run during this UI revision, per the local
operator working instructions.

For the focused Autostart check after building:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
$env:DIGIBYTE_QT_TEST_FUNCTION = 'paymasterOverviewAutostart'
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Autostart regression failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
```

Manually confirm that Autostart stays adjacent to Start/Pause in both themes,
that saving it leaves an unsaved processing-mode choice untouched, and that
Pause clears it. Move between all five areas during a confirmation wait; the
task must stay visible. Check all settings return buttons, keyboard navigation
and a narrow window with enlarged text. Enabling Autostart may start an enabled
provider during the current wallet load once ready; it never unlocks a wallet.
Turning it off does not stop a provider that is already running.

## Guided provider tasks: previous acceptance (2026-09-29)

This candidate changes Qt navigation and adds optional Core operator RPC fields.
Earlier dated passes below do not validate this revision. Follow the repository
baseline (CLAUDE, CONTRIBUTING, architecture/maps, developer notes), Qt and
functional-test guidance. No consensus or address rules change.

Targeted local validation covers the changed Core/Qt translation units, the
shared setup unit tests, and the guided Qt cases listed below. The complete
solution build remains an operator acceptance gate; the focused regtest results
are recorded below.

Recorded locally: 23 selected Qt functions / 63 data cases passed, excluding
Qt init/cleanup bookkeeping. Eleven shared setup unit cases passed all 58
assertions. Changed Core and Qt translation units compiled and the Qt test
executable linked. Both modified functional scripts passed Python syntax
validation. The task layout was inspected
from a native Qt capture at 760 px width, including larger font; light-theme
runs still emit the pre-existing table icon-property warnings without failing.

Follow-up validation on 2026-09-29: the operator reported passing descriptor
regtests for pool setup, RPC, lifecycle and readiness. The operator test initially
failed because its wallet was not loaded after the node restart. The fixture now
explicitly loads that wallet before checking the volatile start request. Its
focused rerun passed locally (11 seconds, exit code 0), together with all 18
framework unit tests. UTF-8 environment variables must be inherited by child
Python processes on Windows. This test-only correction needs no C++ rebuild;
these five results are from separate runs, not a complete release suite.

Background-refresh follow-up: repeated delayed aggregate reads now preserve the
recent status instead of reopening the initial loading panel. The delayed-startup
Qt case also checks overlapping refresh suppression and error/retry recovery.
The affected widget and test sources compiled; the Qt test executable was
relinked. Delayed startup (6 rows), overview fail-closed behavior, three-area
navigation and guided restoration (4 rows) passed locally. The application GUI
still needs rebuilding and operator visual confirmation of this change.

Build from `D:\Digibyte\digibyte-fork` using the Windows batch command in
[Build on Windows](#windows-build). Prerequisites: configured v143/MSVC
14.43 toolset, Qt 5.15.10 static installation, existing static vcpkg dependencies
and Python. This revision adds a header to the Qt source list, so run
`python build_msvc\msvc-autogen.py` once before `MSBuild /t:Build`. Routine builds
afterward should omit autogen unless source lists/configuration change; autogen
can invalidate project timestamps. Allow minutes to tens of minutes for the
solution build; success requires exit code 0.

After the build, run these commands from the same directory in PowerShell.
They create isolated test wallets; never point them at an operator data directory.
The Qt group and regtests take minutes, depending on the host.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\src\test_digibyte.exe '--run_test=paymaster_*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster unit tests failed' }

$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Provider Qt tests failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python test/functional/test_runner.py wallet_paymaster_operator.py wallet_paymaster_pool_setup.py wallet_paymaster_rpc.py wallet_paymaster_lifecycle.py wallet_paymaster_readiness.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Guided provider regtests failed' }
```

Require zero failed/skipped requested cases and exit code 0. Use freshly built
node, CLI and test executables together. Return failing output with its exit code.

Focused Qt coverage:

- `paymasterGuidedRestoreHasOneApproval`: one review, cancellation, wallet
  change and privacy, durable continuation and duplicate-click rejection.
- `paymasterGuidedCapitalTasks`: earnings/base separation, reviewed pause and
  target reduction, cancellation and a lost execution reply.
- `paymasterOperationControllerRecoversWithoutDuplicateApproval`,
  `paymasterOperationFundingReviewIsBounded` and
  `paymasterDeferredStartIntentIsWalletScoped`: phases, stale/wrong-wallet
  replies, paused/locked recovery, exact financial bounds and start intent.
- `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep`,
  `paymasterOperatorDelayedStartup` and
  `paymasterOperatorConfirmationWalletBinding`: setup retry, current
  aggregate status, delayed failures and wallet-bound review.
- `paymasterGuidedTaskLayout` and `paymasterConnectionLayout`: both themes,
  narrow windows, larger font, scrolling and keyboard focus.

Regtests additionally verify proposed-policy preview before identity creation,
no preview writes, one-time start cancellation on pause/unload/restart, journal
operation fields and the separation of planned versus broadcast fee exposure.
The existing pool setup/RPC/lifecycle cases exercise real withdrawal/restoration,
fee ceilings, missing funding, lock, confirmations, restart and replay.

Interactive acceptance: follow all four ordinary tasks, close/reopen during
confirmation, switch wallets during a pending read and pause pending preparation.
Saved transactions must stay visible; no limit may increase automatically; a
reserved fee budget without a transaction must not show confirmation progress.


**Qt navigation candidate (2026-09-28):** `feature/paymaster-ux-navigation`
changes operator presentation on top of integrated RC2 `cac8e521e7`. Its
[design and verification](design/paymaster-operator-ux.md) are separate from the
older evidence below. A full current-revision build, full Qt regression and
an interactive walkthrough remain operator gates; no release approval follows
from the clickable preview. Targeted compilation/linking and twelve selected
Qt cases passed. Two general dropdown-theme assertions also fail with the
original RC2 styles; the linked verification records this open baseline issue.

**Operator-workflow feature candidate (2026-09-27):**
`feature/paymaster-operator-workflow`, based on `1a08828ca1`, adds
[shared Qt/CLI setup and operation](digidollar-paymaster-operator.md). On September 28,
the operator reported Windows build exit 0. Nine of twelve functional tests
passed initially; after three test-fixture corrections, the focused operator,
pool-setup and lifecycle rerun passed (55 s accumulated, 56 s runtime, exit 0).
All twelve selected functional tests have therefore passed across these two
runs. A subsequent Qt run reported 45 passes and two failures; corrections now
pass both focused test functions (including both setup rows) with newly built
Qt binaries, and the operator subsequently reported the full Paymaster Qt group
passing, exit 0. The 297-case unit selection exposed an uninitialized chain tip
in the new read-only fixture; after correction its isolated test passes all
11 assertions. The operator then reported exit 0 for the complete selected
unit rerun. Build, the selected unit group, Paymaster Qt and all twelve selected
functional tests are therefore green across the recorded runs. Real-Tor and
24-hour acceptance remain open. See the
[validation details](digidollar-paymaster-operator.md#verification-and-acceptance).
The dated RC2 results below cover the earlier code only.

2026-09-27 status, source `1789c803be` on `integration/paymaster-v9.26.6rc2`:
[Direct connection capacity](digidollar-paymaster-connection-capacity.md#current-verification-status)
includes the default single-channel queue, dedicated provider listener,
DoS admission limits, recovery fixes and explicit transport retry. The updated
Windows build passed 284 selected unit tests (10,767 assertions) and all nine
focused functional tests. Qt, reference compatibility, real Tor/load and the
wider release matrix remain open. Wire, consensus and journal formats are unchanged.

Updated for `1789c803be` on `integration/paymaster-v9.26.6rc2`, 2026-09-27.
The dated results above cover the selected regression group. Commands below
also include broader checks whose final-source results are still outstanding. The [release gate](digidollar-paymaster-release-gate.md) owns acceptance;
[PAYMASTER.md](../PAYMASTER.md) indexes the design and operating documentation.

## Before running

Use the repository root and freshly built binaries from the same source as the
tests. Record `git rev-parse HEAD`, `git status --short`, toolchain/dependency
versions, build configuration, commands, exit codes and logs. Record any local
changes as well as the commit; a commit hash alone does not identify a dirty tree.
Hash the tested executables after building. Use isolated regtest directories,
never an existing testnet/mainnet wallet for these tests.

Full builds and complete regression/fuzz/platform matrices are operator work.
Builds commonly take tens of minutes or longer; the focused unit/Qt groups take
seconds to minutes, and the functional group can take tens of minutes or longer.
Stop on a failed command. A successful build does not execute the tests.

Functional testing needs wallet/SQLite support, the Python dependencies described
in [the functional-test guide](../test/functional/README.md), including
`digibyte_scrypt`, and a `test/config.ini` matching the build. On this Windows
checkout, `SRCDIR` and `BUILDDIR` point to the repository and `EXEEXT=.exe`.
`msvc-autogen.py` refreshes projects and C++ configuration, not that runner file.
Do not treat missing modules, skipped components or stale binaries as a pass.

## Windows build

See [the MSVC guide](../build_msvc/README.md) for the C++ workload, v143 toolset,
static Qt and its bundled-zlib patch. The following `.bat` uses the current local
operator paths; replace them for another installation. Run it with `cmd.exe`.

```bat
@echo off
setlocal

cd /d D:\Digibyte\digibyte-fork
if errorlevel 1 exit /b %errorlevel%

set "QTBASEDIR=D:\Qt51510\install"
set "VCPKG_INSTALLED=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/"
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"

"%QTBASEDIR%\bin\rcc.exe" --version
if errorlevel 1 exit /b %errorlevel%

python build_msvc\msvc-autogen.py
if errorlevel 1 exit /b %errorlevel%

"%MSBUILD%" build_msvc\digibyte.sln ^
  /t:Build ^
  /p:Configuration=Release ^
  /p:Platform=x64 ^
  /p:QtBaseDir="%QTBASEDIR%" ^
  /p:VcpkgInstalledDir="%VCPKG_INSTALLED%" ^
  /p:VcpkgManifestInstall=false ^
  /m:1 ^
  /verbosity:minimal
if errorlevel 1 exit /b %errorlevel%

echo Build completed successfully.
endlocal
exit /b 0
```

Local read-only checks on 2026-09-23 found Qt 5.15.10, Python 3.11.1 and MSBuild
18.7.8.30822. Project evaluation selected v143 / MSVC 14.43.34808. No build was
started by those checks. Do not infer the compiler version from the MSBuild
installation directory alone.

The unusual local vcpkg layout is intentional in this example: the installed
MSBuild integration appends `x64-windows-static` to `VcpkgInstalledDir`, and
`libcurl.lib` exists under
`build_msvc/vcpkg_installed/x64-windows-static/x64-windows-static/lib`.
Do not shorten this workspace's setting, or copy it unchanged to a conventional
single-triplet installation. `VcpkgManifestInstall=false` reuses installed
packages; it does not install or verify missing dependencies.

`Build` is incremental and `/m:1` limits project concurrency. For the clean-build
release gate, use a clean checkout/build directory or explicitly run `Rebuild`
with the same configuration. Preserve its logs and distinguish that evidence
from an incremental build. The project's existing Release optimization settings
remain authoritative; this runbook does not change them.

## Qt client continuation regression (2026-09-28)

The latest `paymasterClientLiveSendProgressesAcrossAsyncPhases` test explicitly
covers the queued-quote AWAITING_USER_SIGNATURE reply without an authorization
commitment, followed by the exact quote. Its eight rows cover approval, review
cancel, transport error, offer expiry, stopping checks, wallet close, automatic
mode confirmation cancellation and preparation blocked by privacy masking.
Expiry returns an authoritative closed unsigned session with historical inputs;
the form can then be cleared without a new send/signature RPC. See
[the queued-offer correction](design/paymaster-operator-ux.md#queued-offer-and-single-preparation-action-2026-09-28).


The recipientless-cancellation regression uses
`DIGIBYTE_QT_TEST_FUNCTION=paymasterClientRecipientlessCancellation` in the
`PaymasterWidgetTests` suite. It covers direct unsigned cancellation, a lost
reply and a cancellation completed before the action preflight, plus nine
incomplete/contradictory snapshots. Successful closure permits typing a new
recipient without any send RPC; all negative cases keep the form protected.
See [the closure correction](design/paymaster-operator-ux.md#recipientless-unsigned-cancellation-2026-09-28).


The subsequent offer-status presentation check passes ten selected cases with
rebuilt Qt binaries: both offer-preview functions, five live-send rows,
recipientless restart, exact authorization, and accessible controls. Progress,
completed empty results, timestamps, stale responses, errors and local expiry
are covered. See [the offer-check evidence](design/paymaster-operator-ux.md#offer-discovery-and-refresh-presentation-2026-09-28).

The current working candidate fixes a live send stuck at `CREATED` after the
first asynchronous Direct-connection reply. Both discovery and transport may be
healthy in this state. Five new client-flow cases and 18 existing client cases
pass with targeted rebuilt Qt binaries (23 cases total, exit 0). See
[the candidate evidence](design/paymaster-operator-ux.md#client-continuation-correction-2026-09-28).
A full build and the actual operator regtest payment remain separate checks.
After that build, rerun the focused new cases in PowerShell:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
$env:DIGIBYTE_QT_TEST_FUNCTION = 'paymasterClientLiveSendProgressesAcrossAsyncPhases'
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Paymaster client continuation tests failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
```

## GUI flow audit rerun

See [the flow audit](design/paymaster-flow-audit.md) for corrected transitions and
remaining acceptance. After building the current sources, run these focused
regressions (typically a few minutes):

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    foreach ($test in @(
        'paymasterClientFallbackContinues',
        'paymasterClientSessionDiscoveryFailures',
        'paymasterClientReviewCancellation',
        'paymasterClientOfferAutomaticRefresh',
        'paymasterClientPreparationRequiresCurrentOffer',
        'paymasterFeeAmountsAndPercentages',
        'paymasterClientCoreCancellationRoundTrip',
        'paymasterClientLiveRecoveryProgresses',
        'paymasterClientMutationDialogWalletBinding',
        'paymasterClientLiveSendProgressesAcrossAsyncPhases',
        'paymasterClientSessionRpcActionsAreBound',
        'paymasterClientRecipientlessCancellation',
        'paymasterOperatorConfirmationWalletBinding',
        'paymasterOperatorDelayedStartup',
        'paymasterConnectionLayout',
        'paymasterOperatorOverviewGuidesAndFailsClosed',
        'paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep'
    )) {
        $env:DIGIBYTE_QT_TEST_FUNCTION = $test
        & .\build_msvc\x64\Release\test_digibyte-qt.exe
        if ($LASTEXITCODE -ne 0) { throw "GUI regression failed: $test" }
    }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
```

The full Qt and functional commands below remain required after a complete build.

## Windows runtime checks

The commands below include the broader pool-setup and amount-unit tests as well
as Qt; those are not covered by the September 27 nine-test pass. Keep their
results separate. The [capacity guide](digidollar-paymaster-connection-capacity.md#next-operator-checks)
contains the exact selected group that passed. Python UTF-8 settings are
required for the Unicode runner paths on this Windows installation.

After a successful build, run from the repository root in PowerShell. The node and CLI are copied to `src`; the Qt test executable is under
`build_msvc/x64/Release`. Use freshly built files consistently. An interactive desktop is required for the Windows Qt run.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\src\test_digibyte.exe '--run_test=paymaster_*,netbase_tests' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster unit tests failed' }

$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
try {
    .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt tests failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python test/functional/test_runner.py wallet_paymaster_operator.py p2p_paymaster_connection_capacity.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py wallet_paymaster_reorg.py digidollar_rpc_amount_units.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster functional tests failed' }

Get-FileHash .\src\digibyted.exe, .\src\digibyte-cli.exe, .\src\test_digibyte.exe, .\build_msvc\x64\Release\test_digibyte-qt.exe -Algorithm SHA256
```

## Linux / WSL runtime checks

Use an already configured checkout with wallet, SQLite and Qt tests enabled.
Follow [the Unix build guide](build-unix.md) for prerequisites. Run WSL builds in
the Linux filesystem and check the actual checked-out revision; an old pinned
helper script or copied executable does not select the current source.

```bash
set -e
make -j2 -C src digibyted digibyte-cli test/test_digibyte qt/test/test_digibyte-qt
./src/test/test_digibyte --run_test='paymaster_*,netbase_tests' --report_level=short
env -u DIGIBYTE_QT_TEST_FUNCTION -u DIGIBYTE_QT_TEST_OUTPUT QT_QPA_PLATFORM=offscreen DIGIBYTE_QT_TEST_SUITE=PaymasterWidgetTests ./src/qt/test/test_digibyte-qt
python3 test/functional/test_runner.py wallet_paymaster_operator.py p2p_paymaster_connection_capacity.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py wallet_paymaster_reorg.py digidollar_rpc_amount_units.py -j1
sha256sum src/digibyted src/digibyte-cli src/test/test_digibyte src/qt/test/test_digibyte-qt
```

These paths assume an in-tree configured build. For an out-of-tree build, use its
binaries and generated runner configuration. Existing compatibility/bridge tests
require their separately documented historical binaries; the commands above do
not claim to execute those external scenarios.

## Upstream merge regressions

The 2026-09-24 merge incorporates upstream RC2 through `d96d58545a` on top of
fork commit `49f7faa7d8`. The baseline at the top of this document identifies the
earlier client package; use the actual merge commit and freshly rebuilt binaries
for acceptance. Project generation, 13 changed Python files' AST parsing, CSS
brace checks and `git diff --check` passed. Targeted MSVC `/Zs` checks cover the
four changed/conflict-adapted Qt implementation units, two Qt regression units,
network processing, connection shutdown, DigiDollar RPC and wallet units, the
new Dandelion shutdown test, and regenerated MOC code for the two changed Qt
headers. These are compile-time checks, not linked binaries or runtime results.

After the Windows build and Paymaster checks above, run the additional upstream
regressions from `D:\Digibyte\digibyte-fork` in PowerShell. They use the same
prerequisites; allow seconds to minutes for unit/Qt groups and tens of minutes
or longer for functional tests. Every selected test must execute successfully;
missing dependencies, skips and stale executables do not establish acceptance.

```powershell
.\src\test_digibyte.exe '--run_test=dandelion_*,headers_sync_chainwork_tests,headers_work_*,digidollar_mint_cleanup_tests' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Upstream unit regressions failed' }

$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    foreach ($suite in @('DigiDollarWidgetTests', 'DigiDollarWave19WidgetTests', 'WalletTests')) {
        $env:DIGIBYTE_QT_TEST_SUITE = $suite
        .\build_msvc\x64\Release\test_digibyte-qt.exe
        if ($LASTEXITCODE -ne 0) { throw "Qt regression failed: $suite" }
    }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python test/functional/test_runner.py p2p_block_pow_order.py p2p_compactblock_logging.py p2p_dandelion_inventory.py p2p_headers_chainwork.py p2p_unrequested_blocks.py feature_block.py digidollar_estimate_mint_restrictions.py digidollar_redemption_closed_position.py digidollar_redemption_unlock_boundary.py digidollar_rpc_quote_readiness.py digidollar_stats_reordered_mint.py wallet_digidollar_transfer_ancestor_reorg.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Upstream functional regressions failed' }
```

On Linux/WSL use the executable paths and Qt platform from the preceding Linux
section with the same suite and script selections. The complete build, runtime
matrix, independent review and real Tor check remain operator release gates.

## Official reference-release compatibility matrix

The historical script names are retained for existing commands. Each accepts
`--reference-release=v9.26.5` (the default) or `--reference-release=v9.26.6rc2`.
RC2 additionally supports `--thaw-day`, enabling the same Thaw Day height of 1
on every test node, including wallet-disabled upgrade startup and restarts.
The final deployment RPC assertions verify that the selected rules are active
(or remain inactive in the ordinary RC2 variant). This checks operation under
the activated rules; it is not an activation-boundary or exhaustive consensus test.

| Scenario | v9.26.5 | RC2, default rules | RC2, Thaw Day active |
| --- | --- | --- | --- |
| Non-Paymaster bridge: DGB/DD relay and confirmation, gossip isolation, direct-link discovery | Yes | Yes | Yes |
| Raw wallet-file round trip and bidirectional DGB transfers, legacy BDB | Yes | Yes | Yes |
| Raw wallet-file round trip, DD positions/history and bidirectional DGB/DD transfers, descriptors | Yes | Yes | Yes |
| Same-datadir descriptor upgrade, pending transactions, wallet-disabled mempool load, redemption and restart | Yes | Yes | Yes |

All twelve variants are registered in `test_runner.py`. The v9.26.5 checks retain
their original scope; they do not establish compatibility with activated Thaw Day.
The reference daemons must not expose Paymaster RPCs. Tests check `-version`
(including `rc2`), numeric RPC version and P2P subversion. RPC subversion alone
omits the RC suffix and cannot identify the RC2 artifact.

Use official, separately extracted executables, not a Paymaster-disabled copy of
the fork. The local Windows setup contains node and CLI binaries under
`test/previous_releases/<tag>/bin`; these are ignored by Git. The installers were
not executed. Their SHA-256 hashes match the official release assets:

- v9.26.5: `880cdd2cc3cabcc838aea6045647d7fd4ac4ca95be25fd808c939641386b9325`
- v9.26.6rc2: `48f2aacd0102ff811357312a4d0204bd62e392c88fab28673a65e451fda458a5`

Each local release directory also has a `provenance.json` with the download URL,
installer hash and extracted executable hashes. Other platforms require their
own verified official packages.

Run the complete matrix from the repository root in PowerShell. This explicitly
overrides any stale placeholder environment values. Allow several minutes or
longer; every selected variant must pass, with no skipped reference release.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:PREVIOUS_RELEASES_DIR = "$PWD\test\previous_releases"
$env:V9_26_5_DIGIBYTED = "$env:PREVIOUS_RELEASES_DIR\v9.26.5\bin\digibyted.exe"
$env:V9_26_6RC2_DIGIBYTED = "$env:PREVIOUS_RELEASES_DIR\v9.26.6rc2\bin\digibyted.exe"
foreach ($binary in @($env:V9_26_5_DIGIBYTED, $env:V9_26_6RC2_DIGIBYTED)) {
    if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing reference: $binary" }
}
python -u test/functional/test_runner.py p2p_paymaster_v9_26_5_bridge.py wallet_v9_26_5_compatibility.py wallet_v9_26_5_inplace_upgrade.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Reference compatibility matrix failed' }
```

For a single diagnostic variant, invoke its script directly, for example:

```powershell
python -u test/functional/wallet_v9_26_5_inplace_upgrade.py --descriptors --reference-release=v9.26.6rc2 --thaw-day
if ($LASTEXITCODE -ne 0) { throw 'RC2 upgrade regression failed' }
```

Local targeted validation on 2026-09-24 used the existing Windows build from
merge `00e35df825` with the working-tree test changes. Passed: RC2 bridge (45 s),
RC2 same-datadir upgrade (33 s), and RC2 descriptor wallet round trip (about 40 s),
all with Thaw Day active; the default v9.26.5 legacy/BDB round trip also passed
(about 30 s). The registered two-test run passed all 18 framework unit tests.
Python syntax, twelve unique runner registrations, documentation links,
PowerShell example parsing and `git diff --check` passed. `flake8` is not
installed in this Python environment.

On 2026-09-25 the operator supplied the complete Windows runner output for
`test_runner_₿_🏃_20260925_044259`: all twelve registered compatibility
variants passed without skips or failures, together with all 18 framework unit
tests. Accumulated test duration was 500 seconds; total runtime was 503 seconds.
This validates the matrix above with the working-tree test changes on the
`00e35df825` build baseline. It does not renew the separate full Paymaster,
sanitizer/fuzz, real-Tor or independent-review release gates.

The corrected `wallet_paymaster_rpc.py --descriptors` also passed locally on
2026-09-24 (41 seconds plus 18 framework unit tests). Its fixes give concurrent
RPC workers independent HTTP connections, deliver the manual provider's result
before confirming the payment, and reload the actual submitting wallet.

These scenarios transfer ordinary DGB/DD and exercise Paymaster discovery
boundaries. Wallet round trips do not certify downgrading a wallet after it has
created Paymaster records. The complete Paymaster lifecycle, security, Tor and
upstream consensus matrices remain separate acceptance requirements.

## What the focused matrix must establish

| Surface | Required checks |
| --- | --- |
| Client contract | Explicit cents/dollars, parallel identical requests, conflicting IDs, response loss, restart, exact authorization and zero-DGB payment. |
| Observation/status | Recipient confirmation only; recovery, failure/conflict, reorg, missing artifacts and pruned tombstones never invent success. |
| Query effects | Capability and session inspection do not create payments, signatures or reservations; distinguish `refresh` evidence handling. |
| Existing fee policy | Open reservations older than 24 hours, release, identical retry and clock rollback. |
| Compatibility | Frozen PaymentSession v4 / tombstone v2 fixtures; maintenance V3/V4 reads and rejection of unsupported versions. |
| Provider lifecycle | Setup and recurring-maintenance consent, confirmed funding, Dandelion, lost responses, restart, policy/lock changes, successor/recovery/finance reconciliation. |
| Qt | Explicit acceptance, pending versus confirmed recipient amount, recovery and finite-setup acceptance versus execution. |

All selected tests must execute and pass with exit code zero, no unexpected
skips, no RPC schema-check failures and the expected accounting/state invariants.
The broad wallet, DigiDollar, consensus/Thaw Day, cross-platform/arithmetic,
compatibility, sanitizer/fuzz and real-Tor matrices remain separate release
requirements. Independent review remains open. See the
[release gate](digidollar-paymaster-release-gate.md) and dated
[edge-case review](digidollar-paymaster-edge-case-review.md) for those obligations.

### Compact client offer status follow-up (2026-09-28)

The compact status change passed 18 targeted Qt scenarios with rebuilt Qt
objects/test binary, including narrow/wide dark/light rendering and preparing
a request after an empty directory preview. That historical behavior is
superseded by the current-offer preparation gate below; exact provider/fee
approval remains required before signing. See the [flow audit](design/paymaster-flow-audit.md#follow-up-compact-client-offer-status-2026-09-28)
for the selected functions and scope. This does not replace a full build or
live-network acceptance.

### Visible discovery outcomes follow-up (2026-09-28)

The prominent result/checkmark and rotating pending-check icon passed 16
selected Qt scenarios, including actual animation-frame changes, pause/resume
on visibility/privacy changes, no additional RPCs, input/wallet invalidation,
both themes at two widths, and the twelve live-send flow cases. See the
[flow audit](design/paymaster-flow-audit.md#follow-up-visible-discovery-outcomes-2026-09-28)
for build and verification scope; full build and live acceptance remain open.

### Current-offer preparation gate follow-up (2026-09-28)

New explicit-Paymaster requests now require a current nonempty preview. Empty,
pending, failed, malformed or expired checks and changed inputs keep Prepare
payment disabled. The subsequent funding-availability follow-up below extends
this to Automatic without own DGB; existing sessions remain Core-controlled. The rebuilt targeted Qt tests passed all
33 scenarios across nine functions, including live preparation, cancellation,
session discovery and exact authorization. See the
[flow audit](design/paymaster-flow-audit.md#follow-up-preparation-requires-a-current-offer-2026-09-28)
for verification scope. Normal incremental Build relinks the GUI; the complete
suite and actual two-node walkthrough remain separate operator checks.

### Fee comparison and stopped-provider follow-up (2026-09-28)

The DD input/percentage comparison, provider calculator, connection-error
messages and directory/connection distinction passed 29 selected Qt scenarios
across the scoped runs, with affected rows rerun after correction. The existing
Core directory suite passed 10 cases/80 assertions, including announcement
expiry. See the [flow audit](design/paymaster-flow-audit.md#follow-up-ddpercentage-comparisons-and-stopped-providers-2026-09-28)
for build scope, initial test corrections and remaining live acceptance.

### Funding availability follow-up (2026-09-28)

Automatic without cached spendable DGB now requires a current offer; Own DGB
requires a positive balance regardless of offers. Button labels distinguish
Send payment from Prepare payment. The offer-gate matrix runs in explicit and
Automatic modes, and balance-change coverage uses isolated wallet balance polls.
A live Automatic preparation case verifies that receiving DGB during preparation
does not change the request's Paymaster funding or bypass exact-fee approval.
Targeted compilation/linking passed, followed by 24 passing Qt scenarios across
six functions, including existing-session fallback. Full suites and the live
two-node check remain operator acceptance. See the final validation scope in
the [flow audit](design/paymaster-flow-audit.md#follow-up-funding-availability-in-all-fee-modes-2026-09-28).


### Saved-session loading and reused-input follow-up (2026-09-29)

A real-Core regression reproduced PAYMASTER_RESERVATION_SESSION_CONFLICT after
unsigned abandonment followed by a new reservation of the released inputs.
The corrected read-only ownership check passed that sequence and the negative
ownership/signature-evidence checks without changing the new reservation or lock.
Qt now exposes pending progress and exact error reasons, gives feedback for
explicit repeated failures, and masks details under privacy mode. Targeted
compilation/linking and 17 selected scenarios passed; full application build,
complete suites and live-wallet acceptance remain separate operator checks.
See the [flow audit](design/paymaster-flow-audit.md#follow-up-saved-session-loading-and-reused-inputs-2026-09-29).


### Finite pool restoration follow-up (2026-09-29)

The targeted Windows rebuild passed 32 Qt scenarios covering actual shared Core
preparation diagnostics, a real isolated-wallet fee-diagnostic journal round
trip, cancellation safeguards, fee-bound preview invalidation, overview,
maintenance, startup and confirmation wallet binding. See the
[flow audit](design/paymaster-flow-audit.md#follow-up-finite-pool-restoration-and-fee-diagnostics-2026-09-29)
for test names and limits of this evidence. Funded end-to-end restoration and the
full pool-setup functional scenario remain separate operator acceptance checks;
the new error detail does not itself prove that a higher fee limit is needed.

### Obsolete recurring refill follow-up (2026-09-29)

A provider can have confirmed target reserves while a previous recurring refill
remains PLANNED, with a fee reservation and no transaction. Automatic runtime
cleanup now recovers wallet transactions first, then releases only obsolete
unsigned recurring jobs under its work guard. The Qt task distinguishes plans
from confirmations; unloaded finance data is no longer called an uninitialized
ledger.

Targeted local validation: five changed C++ translation units compiled, wallet
and Qt libraries/test executables linked, two wallet tests passed 70 assertions
(obsolete refill/lost reply/idempotence and setup conflict reversal), and 11 Qt
data cases passed (delayed startup/finance, guided restoration and controller
recovery). The first combined Boost invocation had an invalid filter; both
individual tests subsequently passed. Full application build and live Regtest
runtime acceptance remain with the operator. After rebuilding, start the saved
provider in automatic mode and verify that the obsolete task and planned fee
reservation disappear on a subsequent maintenance/status cycle without any new
transaction or changed limits. Re-run the five provider regtests above as the
broader integration gate; prior passes predate this Core cleanup change.

### Structured spending card (2026-09-29)

The Operation card separates enabled payment budgets from reserve maintenance
and aligns spent, rolling-day limit and reserved budget in individual rows.
Changed Qt sources compiled and the test executable linked. Eleven selected Qt
cases passed: four dark/light normal/large-font layout cases with privacy clearing,
six delayed-startup cases and the overview fail-closed case. A native dark-theme
capture was visually inspected at 760 px width. Full GUI rebuild and operator
visual acceptance remain required.

### Finance table theme follow-up (2026-09-29)

Finance tables now have explicit scoped light/dark palettes for backgrounds,
alternating rows, selection and headers. The widget and resource translation
units and updated test compiled; the Qt test executable linked. The finance
workflow passed, including both-theme Base/Text palette checks, numeric cell
alignment, privacy and backup handling. A native dark booking-table capture was
visually inspected. Full application build remains delegated to the operator.

### Client quote expiry and in-memory locks (2026-09-29)

A live client inspection found an unsigned QUOTE_EXPIRED attempt followed by
PAYMASTER_INPUT_ALREADY_RESERVED on the same input: database expiry had
succeeded while the running wallet retained its coin lock. The regression now
restores that process-local lock in the copied wallet fixture, verifies that
write and commit failures retain it, and then reserves the released input for a
new request after successful expiry without reloading the wallet.

Targeted Windows compilation and wallet-library/test-binary linking passed.
Three selected wallet tests passed 194 assertions: client artifact/authorization
atomicity (122), shared standard-wallet locks (8), and provider quote expiry
(64). Full application build and functional integration remain operator checks.

From `D:\Digibyte\digibyte-fork`, with Visual Studio and Qt 5.15.10 already
installed, close running binaries built from this output directory, then run:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
python .\build_msvc\msvc-autogen.py
if ($LASTEXITCODE -ne 0) { throw 'Project generation failed' }
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\build_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false /m:1 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Paymaster build failed' }
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python test/functional/test_runner.py wallet_paymaster_rpc.py wallet_paymaster_lifecycle.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster integration tests failed' }
```

No project regeneration is needed. Allow minutes or longer for the incremental
solution build and a few minutes for the two functional tests. Success requires
exit code 0 and both tests passing; these integration tests supplement the
specific expiry regression above. After starting the rebuilt client, inspect
its saved unsigned transfer before retrying or cancelling it.

### Exact-offer countdown and unsigned closure (2026-09-29)

The approval dialog displays the real exact-offer expiry, prevents late approval,
and closes on expiry, wallet generation changes or privacy mode. Expiry and
live-send errors use a fresh Core snapshot before at most one unsigned
cancellation, followed by durable read-back. An inline notice retains recipient
and amount after verified closure; signed, denied and unverifiable states stay
protected. Technical commitment details are collapsed by default.

The relevant Qt rules are asynchronous wallet RPCs, translated plain-text
presentation, generation-bound callbacks and Core-owned authorization and
reservations. No financial journal, protocol or spending-limit change is made.

Targeted Windows compilation rebuilt the affected client and both including test
translation units, then linked the Qt library and test binary. The focused
cases cover native countdown, light/dark styling, larger text, approval after
expiry, wallet switch, privacy, refused/denied cancellation, read failures,
lost replies, network errors and existing two-stage approval. The Core
cancellation round trip uses the real wallet store and RPC, including a lost
first send reply. Its old manual-cancellation expectation was updated to the
new refresh/cancel/read-back sequence. Initial combined runs exceeded the local
45-second helper timeout; individual data rows are used for the final results.
All 21 distinct focused Qt cases completed successfully. Native approval
captures were visually inspected in both themes and with 16-point labels and
buttons. `git diff --check` also passed.

After the application build above, the operator can run the complete affected
Qt functions from the same workspace (allow several minutes):

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
foreach ($qtCase in @('paymasterClientReviewCancellation', 'paymasterClientLiveSendProgressesAcrossAsyncPhases', 'paymasterClientCoreCancellationRoundTrip', 'paymasterClientAuthorizationIsTwoStageAndFailClosed', 'paymasterClientDelayedCallbackIgnoresWalletClose')) {
    $env:DIGIBYTE_QT_TEST_FUNCTION = $qtCase
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw "Paymaster Qt test failed: $qtCase" }
}
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION
Remove-Item Env:DIGIBYTE_QT_TEST_SUITE
```

Success requires exit code 0 for every function and no failed test cases. Full
application build, the complete Qt suite and live two-node payment acceptance
remain delegated to the operator. Existing build prerequisites and shutdown
instructions above apply; project regeneration is unnecessary.

### Operator full-run follow-up (2026-09-29)

The operator reported all 279 selected Paymaster unit cases passing (10,545
assertions), and all five guided-provider functional tests passing in 142 seconds.
The full Paymaster Qt run reported 174 passes and two failures in 486 seconds:
finance-table palette switching and the old database-read-error dialog assertion.
The countdown, automatic closure and Core cancellation cases passed in that run.

The finance fixture now polishes its hidden ancestor hierarchy before switching
stylesheets, matching the real window's initialization. Without this, a leaf
palette assertion retained the previous theme; optional screenshot rendering
had masked the fixture omission. The dark/light expectations remain unchanged.
Session-database read failures now retain their specific explanation inline and
only reconcile read-only, rather than treating them as unusable provider offers.
The regression verifies retained form focus, no popup chain, no cancellation,
and repeated read-only status checks, including privacy masking.

Targeted compilation/linking and four selected Qt cases passed: finance/backup,
actionable database errors, expiry RPC errors and denied unsigned cancellation.
`git diff --check` passed. Existing non-fatal icon-property QWARN messages remain.
The operator reran the complete PaymasterWidgetTests suite after these
corrections: 176 passed, 0 failed, 0 skipped and 0 blacklisted in 430,893 ms.
The two failures from the prior run are resolved. Other Qt suites were excluded
by the intentional suite filter; this result does not represent a full-repository
test run. Rebuilding the application remains a separate step.

### Payment-capacity wait presentation (2026-09-30)

The provider scheduler uses `PAYMASTER_LIQUIDITY_CONFIRMATION_PENDING` while
payment capacity is reserved as well as while successor reserves await
confirmation. `OperatorDiagnostics` previously classified this scheduler reason
as an error, so ordinary payments showed **Status needs review**. The shared
mapping now treats it as informational only for a running provider in the
matching service state. Unknown errors still take priority.

Qt distinguishes **Payment in progress** (reserved capacity) from **Waiting for
reserve confirmations**, with matching header/capital copy and read-only next
actions. A progress action without a visible task opens the reserves view rather
than focusing a hidden card. Maintenance approval, budget and target-configuration
reasons keep actionable diagnoses. No readiness, spending or journal rules change.

Verification: targeted MSVC compilation/linking succeeded; all 12
`paymaster_setup_tests` cases and 67 assertions passed. The selected Qt groups
`paymasterOperatorOverviewGuidesAndFailsClosed`, `paymasterPoolPreparationDiagnostics`
and `paymasterOperatorDelayedStartup` passed (20 cases, plus per-run init/cleanup).
Coverage includes actual Core diagnostics, reserved capacity, confirmations,
unknown-error precedence, no unintended mutation, and existing privacy/loading
checks. `git diff --check` passed. Existing non-fatal stylesheet icon-property
warnings remain. The running application was not replaced or rebuilt.

Operator follow-up, from `D:\Digibyte\digibyte-fork`, with the existing VS/Qt/vcpkg
installation: run the Windows incremental Release build in this guide's guided
provider verification block. Allow several minutes; success is MSBuild exit 0.
No project regeneration is needed for this change. After restarting the built
wallet, check a payment through offer review, submission and reserve confirmation.
A full test-suite run is not claimed for this follow-up.

### Provider operating unlock and CLI funding (2026-09-30)

The optional `walletpassphrase` operating mode is restricted to an eligible
wallet with a saved provider identity, settings and policy. GUI/CLI operating
prompts preselect continuous access; the password still needs explicit entry.
Timed access, manual locking, wallet unload and node restart remain independently
tested. Existing wallets receive no silent unlock or extra spending authority.

The shared `InspectSetupProgress` monitor distinguishes incoming funding,
confirmed reserves, blocked fee/policy requirements and actual provider start.
Old journal lock/disable observations do not override the current wallet state.
The CLI requests DD balances with `minconf=0` so incoming unconfirmed DD is
visible while Core continues to require confirmed usable inputs.

Focused coverage:

- `paymaster_setup_tests`: funding/confirmation/start completion, stale journal
  reasons, disabled providers, malformed confirmation counters and fee limits.
- Qt `paymasterOperatingUnlock`: default continuous choice, timed access,
  cancellation, privacy and wallet changes in both review/password dialogs.
- Qt `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep`: the encrypted-start
  row completes the assistant and verifies operating unlock before the one-shot
  start request, including the serialized RPC-handler continuation.
- `wallet_paymaster_operator.py`: ordinary-wallet rejection, CLI boolean
  conversion, superseded timers, wrong passwords, manual/timed relock, unload
  and node restart. The restart fixture reconnects its RPC and P2P links.
- `wallet_paymaster_pool_setup.py`: missing funding, encrypted-wallet waits,
  confirmed incoming funding, continuous unlock, deferred start, no readiness
  before reserve confirmation, and running/ready completion without changing
  autostart or duplicating the approved plan.
- `wallet_encryption.py`: ordinary legacy and descriptor wallet behavior.

Local verification on 2026-09-30: targeted MSVC compilation/linking passed.
All 13 `paymaster_setup_tests` cases / 82 assertions passed. The selected Qt operating-unlock,
guided-setup, overview, pool-diagnostics, delayed-startup and guided-setup-theme
groups passed 31 cases (43 including per-run init/cleanup), with no
failures or skips. Existing non-fatal table icon-property QWARNs remain.
The functional run passed provider operator (19 s), pool setup (16 s), ordinary
legacy encryption (9 s) and descriptor encryption (9 s), plus 18 framework
unit tests. After adding the explicit third-argument CLI conversion assertion,
the operator test passed again (22 s). Python syntax, PowerShell example parsing
and `git diff --check` passed. These focused checks do not constitute a full
build, the complete Qt suite, or a real-terminal CLI funding walkthrough.

Full Windows build and the complete Qt suite remain operator checks. From
`D:\Digibyte\digibyte-fork`, use the existing VS/MSVC 14.43, Qt 5.15.10 and static
vcpkg installation. The setup rule audit adds `paymaster/provider_policy.cpp`;
regenerate the MSVC projects once before building the current revision.
Allow several minutes for the incremental solution build; require exit code 0:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
python .\build_msvc\msvc-autogen.py
if ($LASTEXITCODE -ne 0) { throw 'Project generation failed' }
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\build_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false /m:1 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

Then run the complete Paymaster Qt suite and focused functional matrix (minutes;
require no failures/unexpected skips). These commands point at the same newly
built binaries and create isolated test wallets:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt tests failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
}
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:DIGIBYTED = (Resolve-Path .\build_msvc\x64\Release\digibyted.exe).Path
$env:DIGIBYTECLI = (Resolve-Path .\build_msvc\x64\Release\digibyte-cli.exe).Path
python test/functional/test_runner.py wallet_paymaster_operator.py wallet_paymaster_pool_setup.py wallet_encryption.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Operating unlock/funding regtests failed' }
```

Manual terminal acceptance remains separate from the RPC/model tests. In an
isolated regtest setup, run `digibyte-cli -regtest -rpcwallet=<provider>
-paymastersetup` in an actual terminal (add the test node's datadir/RPC options).
Choose `addresses`, approve the desired one-shot start and select continuous
operating access. Fund the two displayed addresses from a separate test wallet;
observe partial/unconfirmed balances, then confirm funding and reserve creation.
Require the same saved plan throughout and a final running/ready status. Repeat
with `watch`, declined start, a timed unlock that expires while waiting, and a
fee limit too small to create reserves. Closing the monitor must not create a
second plan or erase accepted work. After reload/restart the encrypted wallet
must be locked. No real operator funds are needed for these checks.

### CLI setup guidance and continuous-operation proposals (2026-09-30)

The console now has seven named stages, explained numbered choices, `?` help,
retryable numeric input and DGB/DD/percentage formatting. New providers propose
automatic processing, autostart and finite paid-refill budgets; saved selections
remain intact. GUI setup continues to preserve its saved autostart contract.
Configuration and exact pool funding each retain explicit review: pressing Enter
at an approval prompt declines, regardless of the preceding menu selection.

Focused coverage in `paymaster_setup_tests` includes pure new-provider proposals,
exact preservation of existing limits above double precision, explicit autostart
selection, invalid menus, blank/EOF confirmations, DD and percentage precision,
DGB decimal-comma input, rejected exponents/grouped amounts and numeric bounds.

Verification for this CLI follow-up: targeted MSVC compilation/linking passed;
16 setup unit cases with 115 assertions passed. The Qt guided-setup and operating-
unlock groups passed 10 cases (14 including init/cleanup), no failures or skips.
The operator functional test passed in 22 seconds, along with all 18 framework
unit tests. These checks cover helpers, shared setup and RPC integration; they do
not substitute for the complete interactive terminal walkthrough.

After the Windows build command in the preceding section, rerun the shared model
checks from the repository root (seconds; require exit code 0):

```powershell
.\build_msvc\x64\Release\test_digibyte.exe --run_test=paymaster_setup_tests --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'CLI setup unit tests failed' }
```

For terminal acceptance, use the isolated regtest instructions above. In addition:

- Follow all seven stages with a new provider; verify the selected continuous-
  operation proposals and the separate payment/refill/setup ceilings. Empty
  approval must not apply configuration or pool funding.
- Enter `?`, an invalid menu number, an invalid DD fraction and a decimal-comma
  amount. Earlier valid choices must remain intact while the prompt repeats.
- Review an existing provider with autostart/refill off and customized limits;
  the saved values must be preselected. The existing-provider menu defaults to
  read-only status. Restricted sponsorship must stay sponsored-only with zero
  DD fee when editing unrelated offer amounts. Only public sponsorship can be
  combined with customer-paid service; the CLI must not offer a restricted mix.
- Verify the funding monitor reports changes immediately and a heartbeat every
  ten seconds while polling Core every two seconds, without flooding the screen.
- Distinguish a one-time start request from saved autostart. Continuous operating
  access still requires manual password entry after a node restart.

Restricted-sponsorship correction (2026-09-30): remove the invalid mixed CLI
choice. Core already rejects restricted policies unless they are sponsored-only
with zero DD fee. The shared setup planner now rejects this combination before
returning any mutating step. The regression covers restricted customer-paid,
restricted mixed and nonzero sponsored fees, plus valid restricted-only and
public mixed policies. Run `paymaster_setup_tests` and
`paymaster_provider_tests/sponsored_policy_always_charges_zero` after rebuilding.

Correction verification: targeted MSVC compilation/linking passed. All 17 setup
unit cases / 120 assertions passed, as did the existing Core sponsored-policy
case / 6 assertions. The Qt guided setup group, including reconfiguration to
restricted sponsored-only, passed 3 cases (5 including init/cleanup) with no
failures or skips. `git diff --check` passed. Full build and terminal acceptance
remain the operator checks described above.

### CLI/GUI setup rule audit (2026-09-30)

The common preflight calls the existing Core offer/safety/liquidity validators
from the new pure `paymaster/provider_policy.cpp` translation unit. It also
matches pool RPC target and one-time fee bounds before constructing any write.
The validator bodies were compared with the previous implementation: no policy
rule changed. The Qt wizard now builds preview and execution from one choices
collector; inactive saved limits and exact policy values are part of review.

Targeted regression coverage includes invalid rates/ranges/expiry, inactive
budgets, quote limits, refill limits, sponsored-only carrier counts, setup fee
bounds, optional identity names, exact maximum DGB parsing, restricted GUI
reconfiguration, retained large policy values and saved autostart.

Recorded for this audit: all changed translation units compiled; common/Qt
libraries, CLI, daemon and both test binaries linked with MSVC. The two focused
unit groups (`paymaster_setup_tests:paymaster_provider_tests`) passed 59 cases /
1,211 assertions. Three Qt functions passed 11 actual data cases (17 including
init/cleanup) across the final focused runs, with no failures/skips: guided setup,
setup theme and operating unlock. The new large-policy/autostart test initially
omitted a status refresh after changing its simulated Core snapshot; correcting
the fixture made all three setup rows pass. Operator and pool-setup descriptor
regtests passed in 43 seconds, together with all 18 framework unit tests.
`git diff --check` passed. This is not a full solution build or release approval.

Operator acceptance: regenerate project files and use the full Windows build
and complete Paymaster Qt commands above. Full build/Qt runs take minutes to
tens of minutes and must return exit code 0 with no requested failures/skips.
The terminal funding walkthrough above remains required; these targeted checks
do not exercise a real interactive console from first prompt through funding.
Also reconfigure an existing provider from user-paid to public sponsored,
then back to public mixed service. Restricted is disabled for new use as of
2026-10-09; the earlier Restricted setup results below are historical. Verify that inactive saved budgets are shown,
invalid edits can be corrected before approval, sponsored DD targets become
zero without withdrawing funds, and the exact reviewed values are persisted.

### Earnings below the withdrawal minimum (2026-09-30)

Diagnosis against the running operator regtest used `getpaymasteroperatorinfo`
and a non-executing `withdrawpaymastercarrier` preview. The wallet had 6 cents of
confirmed carrier excess; Core rejected the preview with
`PAYMASTER_CARRIER_WITHDRAWAL_PREVIEW_FAILED: Amount below minimum DigiDollar
output. Minimum: 100 cents`. No withdrawal was executed. Readiness and the
backup recommendation were not the cause.

Qt now applies `Params().GetDigiDollarParams().minOutputAmount` to both payout
entry points and to accepted preview replies. Below the minimum, a visible hint
reports accumulated earnings and the threshold. Wallet changes and
privacy clear the hint; pending tasks retain their own progress presentation.
Specific minimum-output failures explain the wait and preserve the earnings;
unknown task errors retain their actual diagnostic.

Verification: affected Qt source/test files compiled with MSVC and the Qt test
binary linked. All 18 data cases (26 including init/cleanup) passed across
`paymasterCarrierWithdrawalActionsFailClosed`,
`paymasterCarrierWithdrawalPreviewsArePlanBound`, `paymasterGuidedCapitalTasks`
and `paymasterGuidedTaskLayout`. Cases cover 0/6/99/100 cents, late preview
rejection, unknown error detail, approval/cancellation/lost reply, wallet
changes, privacy and the existing light/dark 11/17-point layout checks.
Earlier successful withdrawal fixtures used an impossible 6-cent payout; their
valid cases now use 100 cents. Existing light-theme icon-property warnings
remain unrelated. `git diff --check` passed. Core transaction rules are unchanged.

The running GUI was not replaced. Close both Qt regtest applications before the
operator Windows build above; this change adds no source files. The complete
Paymaster Qt suite remains the larger operator check. For manual acceptance,
open Operation with 0.06 DD earnings: both payout buttons must be disabled and
the hint must explain the 1.00 DD minimum. At exactly 1.00 DD, a fresh preview
and explicit payout review must be possible within the existing DGB fee limits.
Neither repeated clicks nor a failed preview may send a withdrawal.

## Guided restoration after intentional capital release

`paymasterGuidedRestoreHasOneApproval` also models a stopped USER_PAID provider
with saved operational DD target zero. The main Operation action must produce
one review containing 0 → 1, capital, bounded setup fees, unchanged maintenance
limits and the requested start. Before approval there are no writes. Decline,
privacy activation and wallet switch perform no write or execution. Approval
preserves the other saved fields, reads the target write back, executes only a
fresh Core plan within the reviewed scope, enables continuation if necessary,
and requests `wait_for_readiness` without changing Autostart. A lost target-save
reply is reconciled without a second write. Changed policy, increased funding
and a failed write must not execute funding or start the provider.

Run the focused Qt tests after building the current test binary (PowerShell,
repository root; seconds, not the full suite):

```powershell
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
$env:DIGIBYTE_QT_TEST_FUNCTION = 'paymasterGuidedRestoreHasOneApproval'
try {
    .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Guided restoration failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION
}
```

Manual acceptance with an isolated regtest wallet: release its last operational
DD reserve, then use the main restore-and-start action. Check the one approval,
confirm the resulting transactions and verify readiness/start. Also decline a
review and reload the wallet: the saved zero target must remain zero. Both themes,
a narrow window, larger text and keyboard access must retain readable task and
status text. Reserve and spending settings remain reachable via Settings; the
Operation cards must not duplicate those buttons.

Verification for this follow-up: targeted MSVC compilation of the widget and
Qt tests, library/test linking and 32 focused cases passed (44 including suite
init/cleanup, no failures or skips). Coverage comprises guided restoration,
capital tasks, funding bounds, operator diagnostics, liquidity-state text and
the two-theme/font layout matrix. Generated dark/light repair views were also
inspected. A read-only preview against the operator's regtest node confirmed
one missing 100-cent payment reserve, no extra DGB output capital and a
20,000,000-satoshi setup fee ceiling; `accepted` and `executed` were false.
No live targets, transactions or provider start state were changed. The full
solution build, full Qt suite and real-transaction release/restore walkthrough
remain operator checks using the commands above.

## Single-window node connection review

`paymasterNodeConnectionEditor` covers unchanged values (no preview/write RPC),
command-line overrides, exact preview/apply, invalidation after editing, inline
Core override errors with retained entries, wallet switch, privacy and closing
while a preview is running. `wallet_paymaster_operator.py` checks live argument
provenance and the effective announced endpoint in the operator snapshot.
Build the updated Core and GUI before using source labels. The full build and
functional commands above remain operator checks; the focused Qt function can
be selected using `DIGIBYTE_QT_TEST_FUNCTION=paymasterNodeConnectionEditor`.

Verification: targeted MSVC compilation of the widget, Qt test, generated moc
and provider RPC translation units succeeded. All eight connection-editor cases
passed (10 including suite init/cleanup). The dialog capture was inspected for
readable labels, and the functional test passed Python syntax validation. No live
node configuration was changed. A full application build and the functional
operator test with the rebuilt daemon remain outstanding operator checks.

## Overview card presentation follow-up

Targeted widget/test compilation and test-binary linking succeeded.
`paymasterCapitalOverview`, `paymasterOverviewFinanceWalletAndPrivacyBinding`
and `paymasterGuidedTaskLayout` passed: 17 cases, 23 including suite
init/cleanup, no failures or skips. Existing checks now verify DGB suffixes,
finance-card ownership of earnings and payout guidance, and privacy of the
relocated metrics. The layout matrix uses dark/light themes and 11/17-point
font settings; generated card captures were inspected. Existing light-theme
QTableWidget iconColor property warnings remain. `git diff --check` passed.
The full application build and full Qt suite remain operator checks; no Core
accounting, financial limits or payment authorization were changed.

## Provider command-in-progress gate

Targeted MSVC compilation (widget, tests, moc) and test linking passed.
`paymasterProviderCommandLocksPages` covers every page button during delayed
RPC replies, chained finance reads, errors and stale-wallet completion.
`paymasterRetirementLateResults` also attempts repeated clicks during each
stop/preview/execute/balance phase. These and `paymasterNodeConnectionEditor`
and `paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows` passed: 20 cases
(28 including init/cleanup), no failures or skips in the final runs.
`git diff --check` passed. Full application build and full suites remain
operator checks; the running GUI executable was not replaced.

## Setup fee recovery

Targeted widget/test/moc compilation passed. Fee recovery, preparation
diagnostics, existing cancellation and page-lock tests passed (20 test
entries plus eight init/cleanup entries). The fee-recovery entry exercises
five outcomes: fresh preview, preview failure, refusal, a saved-transaction
race and wallet change; the fresh preview is declined and authorizes no
replacement. Full application build and live regtest remain operator checks.

## Background refresh paint batching

The widget/test targeted compilation and test linking passed. The page-lock
test initially asserted a 250-ms paint deferral; this proved insufficient
for slower responses and is superseded by the stable-status regression below. Together with
finance wallet/privacy binding, fee recovery and the eight connection-editor
cases, 11 test entries passed (19 including init/cleanup), without failures
or skips. Polling intervals are unchanged. `git diff --check` passed. Full
application build/full suites remain operator checks.

## Stable operator view before startup

Valid snapshots no longer call the full reset before rendering. The page-lock
regression uses a stopped, ready provider, delays a reply beyond the former
250-ms boundary, and observes visibility events across the status/finance
reply chain. Unchanged budget sections must not hide/show, command locks must
remain active, and completion/failure must restore painting.
Targeted widget/test compilation and linking passed. The page-lock, operator
fail-closed, capital overview, wallet/privacy and theme/font layout checks
passed: 19 cases (29 including init/cleanup), no failures or skips. Existing
light-theme table iconColor property warnings remain. Formatting checks passed.
The running GUI was not relinked; close both wallet instances, perform a normal
incremental application build and restart for the operator's live verification.
A clean rebuild is not required.

## Retirement notice after restart

The stop/retirement workflow test now completes retirement, supplies a current
running snapshot and verifies that the successful completion notice is cleared
and hidden, including after toggling privacy. Its backup fixture includes the
three required Core timestamps. Targeted widget/test compilation and linking
passed; `paymasterStopAndRelease` passed 22 cases and
`paymasterRetirementLateResults` passed 10 (36 including init/cleanup), without
failures or skips in the final runs. `git diff --check` passed. The application
executable still needs the operator's normal incremental build and restart.

The retirement-dialog follow-up replaces the fixed checkbox with a release
notice. `paymasterStopAndRelease` now clicks the optional Stop checkbox both
on and off and checks the retirement notice and separate release approval.
Targeted widget/test compilation and linking passed; all 22 cases passed
(24 including init/cleanup), no failures or skips. Formatting checks passed.
An incremental application build and restart are required to use the change.

The node connection read-only presentation adds dark/light all-overridden
cases to `paymasterNodeConnectionEditor`. Fixed values must be selectable
labels with explicit Yes/No values; Preview/Save are hidden when no setting
is editable. Scroll contents and viewport must not restore native background
fills. All 10 cases passed (12 including init/cleanup), following targeted
widget/test compilation and linking. The dark read-only capture was inspected.
Existing light-theme table iconColor warnings remain. The first screenshot
run used an invalid extension; the final PNG run passed. An incremental
application build and restart are still required.

## Reserve restoration after a pending start or fee correction

The read-only operator snapshot reproduced a pending start with no preparation
or active operation, missing DD reserves, and automatic/paid replenishment off.
This is an approval requirement, not a confirmation wait. No live-wallet write
was used for diagnosis or verification.

`paymasterStartIntentNeedsReserveApproval` covers that distinction, incomplete
status, saved work, pending/reserved capacity, wallet lock and disabled provider.
`paymasterPreparationContinuesAfterRefresh` covers confirmed cancellation followed
by a replacement preview, a status read arriving before the approval dialog,
approval through confirmation and running state, decline, wallet switch, privacy,
and restoration when only the start intent remains. Preview and execution retain
the chosen fee and exact plan; duplicate clicks cannot add an execution.

Targeted MSVC 14.43 / Qt 5.15.10 widget/test compilation and test linking passed.
Together with `paymasterPreparationFeeRecovery`,
`paymasterOperationControllerRecoversWithoutDuplicateApproval`,
`paymasterGuidedRestoreHasOneApproval`, `paymasterProviderCommandLocksPages`,
`paymasterGuidedTaskLayout` and `paymasterOperatorOverviewGuidesAndFailsClosed`,
27 cases passed (43 including per-run init/cleanup), no failures or skips.
The final rerun used consistent missing, unconfirmed and ready reserve snapshots.
Existing light-theme table iconColor warnings remain. `git diff --check` passed.

The running application was not relinked. Close both wallet instances and use
the normal incremental `/t:Build` command in the Native Windows section above;
a clean rebuild is unnecessary. The configured MSVC/vcpkg/static Qt prerequisites
remain the same. Allow minutes for the incremental build and several minutes
for the full Paymaster Qt group; require exit code zero and no failed tests.
The full application build and full regression group remain operator checks.

After restart, Operation must offer an actionable reserve review when capital
is missing. Approve the displayed capital and fee limit; if a fee-limit check
blocks creation, use **Review setup fee and continue**, choose the ceiling and
approve the replacement plan. The next view must show the saved creation step
or real confirmation count. On Regtest, mine a block only once a transaction
actually exists. After confirmation the explicitly requested start should finish.
Declining approval must show an actionable state without automatic-progress text.
Recurring limits and the saved autostart setting must remain unchanged.

## Saving liquidity settings during background refresh

`paymasterLiquiditySaveAcrossRefresh` adds nine UI/RPC cases: approved save,
automatic refill selected without paid approval, declined review, wallet switch,
privacy activation, lost reply, malformed reply, rejected write and failed
readback. The test clicks the controls, keeps the edit across an old status
snapshot, checks the command lock during review, verifies exact DGB-to-satoshi
values and reloads confirmed values into a fresh view. Uncertain saves trigger
one readback and never an automatic duplicate write; rejected writes keep edits.

Targeted widget/test/moc compilation and Qt library/test linking passed with
MSVC 14.43 and Qt 5.15.10. The nine new cases plus the five existing liquidity
save/default/status, command-lock and RPC-workflow cases passed: 14 cases,
26 including init/cleanup, no failures or skips. `git diff --check` passed.
Core's wallet policy was inspected read-only; no live setting was changed.

A normal incremental application build and restart are still required. Use the
Native Windows build command above after closing both running GUI instances;
a clean rebuild is unnecessary. The full application build and full suites
remain operator checks under the repository's local collaboration rules.
For the focused rerun from the repository root after building:

```powershell
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
$env:DIGIBYTE_QT_TEST_FUNCTION = 'paymasterLiquiditySaveAcrossRefresh'
try {
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Liquidity-setting regression failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE, Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
}
```

The focused test takes seconds; require nine cases plus init/cleanup to pass.
For live acceptance, enable automatic refill, review and explicitly approve the
finite paid-refill limits, save, then reopen the page and restart the wallet.
Both saved choices and all limits must remain visible. Rejected or declined
changes must not be described as saved, and background polls must retain edits.

### Confirmed-payment retry after a lost client result (2026-10-01)

The explicit `retry_same` path first consumes the selected latest attempt's
signed result. If the inbox was lost on restart, saved USER-PSBT resubmission
validates the locally observed exact final and uses the existing exact-final
Capacity exception. Normal unspent checks remain mandatory without that proof.
Qt continues only the explicitly initiated retry, for at most two minutes, and
ends it on completion, error, privacy or wallet change. Passive restore remains
observation-only. Recovery messages follow both themes.

Targeted MSVC 14.43 / Qt 5.15.10 compilation and wallet/Qt/test/daemon/GUI linking
passed. The prepared GUI is `build_msvc/x64/Release/digibyte-qt-operator.exe`;
after the operator closed both running GUI instances, that tested artifact was
installed as `digibyte-qt.exe`, with the previous binary backed up and SHA-256
verified. Installation details are in `build_msvc/retry-artifact.json`.

Verification:

- `paymasterClientSessionRpcActionsAreBound`: restore is read-only; explicit
  same-request retry progresses to confirmed, does not repeat after completion,
  stops after privacy/error, and exposes uncertain outcome plus technical error.
  Actual QMessageBox surfaces were checked and captured in dark/light themes.
- `paymasterClientSessionActionMatrix`,
  `paymasterClientRestartCreatedSessionWithoutRecipientIsReadOnly`, and
  `paymasterClientDelayedCallbackIgnoresWalletClose`: action gates and stale
  callback/restore protections.
- `paymaster_wire_tests/capacity_full_validation*` (3),
  `paymaster_psbt_tests` (4), and
  `paymaster_wallet_store_tests/client_final_observation_is_atomic_and_idempotent`
  (1): validation, signature/template binding and idempotent final observation.
- `wallet_paymaster_provider.py --descriptors`: a payment is mined before the
  client consumes its result; restart drops the inbox; repeated explicit retry
  reconnects, collects the signed result and reports CONFIRMED with the same
  transaction and attempt, unchanged attempt count and no extra mempool payment.
  The full targeted provider script passed with the earlier automatic-submit,
  reserve-recycling, recovery, restart and production-log checks included.
- `git diff --check` passed. Full repository suites and cross-platform builds
  were not run, in accordance with local operator build guidance.

One intermediate functional run hit the existing setup-time
`PAYMASTER_PROVIDER_BUSY` race at `setpaymasterenabled(false)` before reaching
this regression; the subsequent complete run passed. Qt retains unrelated
QTableWidget icon-property warnings. Test logs and both dialog images are under
`build_msvc/retry-*`.

### Resultless client observation before recovery (2026-10-05)

The live operator report concerned an old `PENDING_PROVIDER / PENDING_NETWORK`
session whose exact original transaction was already in the client wallet and
deeply confirmed. Recovery incorrectly reached `PAYMASTER_RESERVED_INPUT_UNAVAILABLE`.
Read-only diagnosis confirmed the transaction; no live retry, recovery, signing
or broadcast was used during investigation.

Targeted MSVC compilation of the two changed wallet files and the store test file
passed, as did isolated Core-test and Qt-application links. Twelve focused Core
cases passed with 530 assertions:

- `client_tip_reconciles_payment_without_provider_reply`: absent, different-spender,
  mempool and shallow observations do not complete; a modified final witness fails
  without DB writes; the exact deeply confirmed transaction completes after retry
  expiry, creates no recovery/provider receipt/rebroadcast candidate and follows
  normal subsequent tombstone retention.
- `client_recovery_reconciles_confirmed_original_payment`: direct `resolvepaymastersession`
  refresh stays read-only; explicit recovery returns the original confirmed payment
  without a recovery object or fabricated result; repeated recovery is not allowed.
- Existing resultless completion, atomic result persistence/observation, read-only
  recipient evidence and reorg checks, manifestless replay rejection, and pre/post
  final conflict/negative-result authority protections.

Full builds, complete suites and the multi-process lifecycle test were not run
for this checkpoint. Use the existing Windows build/run commands in this runbook
for operator verification; this result is not a release gate completion.

The follow-up Qt completion guard accepts Core-validated local confirmation without
requiring a provider-result envelope. Eleven focused native Qt cases passed:
confirmation observation, material-change guard, seven recovery outcomes (including
`original_confirmed`), bound session RPC actions and fail-closed recovery expiry.
The existing RPC-action theme test initially failed in both the previous and new
test executable. Its fixture now switches the application stylesheet consistently,
polishes the parent and inspects an exposed dialog in both themes. Both theme
assertions pass; the application theme implementation was not changed.

Run these Qt entries with `DIGIBYTE_QT_TEST_SUITE=PaymasterWidgetTests`,
`QT_QPA_PLATFORM=windows` and `DIGIBYTE_QT_TEST_FUNCTION` set to the selected name:
`paymasterClientConfirmationRequiresObservation`,
`paymasterConfirmationGuardDetectsMaterialChanges`,
`paymasterClientLiveRecoveryProgresses`,
`paymasterClientSessionRpcActionsAreBound` or
`paymasterClientRecoveryExpiryIsFailClosed`. The isolated application candidate
was installed only after the operator closed both live wallets and its SHA-256
was verified against the tested candidate; the previous EXE/PDB were backed up.

### Retry after provider replay-journal pruning (2026-10-01)

The preceding regression covered a retained provider reply, not its removal at
240 confirmations. The follow-up adds explicit client completion from the exact
locally confirmed transaction and accepted USER authority, atomic fee settlement,
and index recovery for payments missing from mapWallet. Observed finals without
provider results are excluded from rebroadcast; existing observation/reorg and
retention paths remain active. Stale/mismatched inbound submissions are consumed
individually, while corrupt/unreadable database bindings still fail closed.

Targeted MSVC compilation and Core/daemon/GUI links passed. Checks:

- `paymaster_wallet_store_tests/client_confirmed_payment_without_provider_result`:
  absent/shallow observations do nothing, altered witnesses fail unchanged,
  all three DB writes roll back on injected failure, historical consent permits
  observation after expiry, the fee reservation becomes spent, no provider result
  is invented, and no rebroadcast candidate is created.
- `paymaster_wallet_identity_tests/stale_submit_is_consumed_without_faulting_provider`:
  only the addressed stale message is consumed, a different provider's message
  survives, the next call is idle, the provider stays running and the DB is unchanged.
- Existing final-observation and atomic/monotonic result-persistence unit tests.
- `wallet_paymaster_lifecycle.py --descriptors`: after provider pruning, restore
  an older authorized client backup, complete while the provider is stopped,
  verify the exact transaction, one-cent fee settled once, unchanged provider
  finances/mempools, and reload the completed wallet. The default run also covers
  the existing provider crash, replenishment and locked-autostart lifecycle.
- `wallet_paymaster_lifecycle.py --descriptors --client-without-change`: focused
  payment/restart variant using all 1.01 DD for a 1.00 DD payment plus fee; the
  subsequent reserve-only scenarios remain covered by the default variant.

An intermediate attempt to put 240-block pruning into the general provider
scenario disrupted its later offer-discovery setup; that test retains its prior
one-confirmation retry case. The pruning regression belongs to the lifecycle
fixture above. A no-change variant initially also ran the reserve-only fixture
with a different funding layout; it now intentionally runs the payment/restart
scenario only. Logs are `build_msvc/binding-*`. Full repository suites and
cross-platform builds remain operator-run, per repository instructions.

### RPC/CLI recovery parity (2026-10-01)

The 240-confirmation reconciliation is now a shared RPC helper. The lifecycle
regression restores a separate copy of the pre-result client backup for each of
`resolvepaymastersession retry_same`, `processpaymasterresult`, authorized
`senddigidollar` resumption and `walletprocesspaymasterpsbt`, first through RPC
and then the real `digibyte-cli` executable. Only one copy is loaded at a time.
The provider is stopped and its replay journal has been pruned. Checks cover:

- exact original txid and no fabricated provider receipt;
- unchanged mempools and provider finance records;
- one fee settlement, idempotent result polling and durable wallet reload;
- read-only refresh preserving the pending fee reservation;
- rejection of a wrong PSBT authorization commitment;
- result/send/PSBT reconciliation after authorization expiry through CLI,
  without a new signature or submit.

Targeted MSVC compilation of the two changed RPC units and wallet/Core-test/daemon
links passed. Both lifecycle variants passed (16 independently restored
RPC/CLI cases in total):

```powershell
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
python -X utf8 test/functional/wallet_paymaster_lifecycle.py --descriptors
python -X utf8 test/functional/wallet_paymaster_lifecycle.py --descriptors --client-without-change
```

Run from the repository root with the current MSVC binaries and the functional
Python dependencies. Each targeted script takes about one minute on this
workstation and must end with `Tests successful`/exit 0. The default variant also
retains the provider crash, reserve replenishment and locked-autostart checks.
Seven Core regressions passed: resultless confirmed payment, atomic final
observation, monotonic immutable result persistence, isolated stale submit
rejection, missing/read-failed session distinction, unreadable AUTO session
rejection before funding, and protected ambiguous authorization/tombstone pruning.
The focused preparation contract also passed:

```powershell
python -X utf8 test/functional/wallet_paymaster_rpc.py --descriptors --client-preparation-only
```

This optional subset covers capabilities/access checks, failed PAYMASTER/AUTO
preparation over HTTP and actual CLI, authoritative session absence and unsigned
CLI cancellation while the provider is paused. The default full test is unchanged.
Its existing cancellation assertion used `state` instead of the documented
`session_state`; the runtime check exposed and corrected that test error.

Final logs are under `build_msvc/`: `paymaster-cli-current-core-tests.log`,
`paymaster-cli-current-default.log`, `paymaster-cli-current-no-change.log` and
`paymaster-cli-preparation2.log`. The executable contains the current PR #452
and absence-check objects as well as the two new RPC units. An initial link used
older default objects because the prior integration compiles were isolated;
those first passing runs are not the current-source checkpoint. The final daemon
was relinked with the selected current objects and all checks above rerun.
A separate `test_digibyte-paymaster-cli.exe` diagnostic link includes current
store/planner tests and omits three unrelated old test units requiring recompilation
for the changed DD-selection signature. This is not a full test-binary rebuild;
full builds and the complete contract suite remain operator work.

An initial test draft incorrectly obtained the unsigned PSBT from the provider
quote response; it now captures the client preparation reply. The initial
multi-file MSBuild argument was also rejected before compilation; selecting the
two units separately succeeded. No full build, complete suite or cross-platform
run was performed; those remain operator-run under repository guidance.


## 2026-10-08: Recover temporary provider admission drains

A provider could retain `service_state=drain_only` after a safety-limit failure
while reporting local `ready=true`, losing its last error and never processing
new requests. The service now reconciles and rechecks current prerequisites under
its existing work guard, resumes only when fully ready and synchronized, and
refreshes the validated offer. Saved limits and per-request checks are unchanged.
Shared RPC diagnostics, CLI status prose and the Qt overview expose admission
pauses independently of local readiness.

Targeted Windows/MSVC verification:

- `wallet_paymaster_provider.py --descriptors --drain-recovery-only`: passed
  against the isolated corrected daemon. Two unsigned preparations are abandoned;
  provider reservations expire, automatic admission resumes without another
  start, and safety/refill policies, client balance and mempool remain unchanged.
  The prior normal daemon fails the same regression: readiness returns, but
  `service_state` never becomes active within the bounded wait.
- Three Core cases passed: `operator_work_transitions_preserve_real_errors`,
  `provider_automation_capacity_pause_preserves_approval` and
  `provider_sync_observation_preserves_unresolved_gates`.
- `PaymasterWidgetTests::paymasterOperatorWorkTransitions` passed in dark and
  light themes (four Qt results including fixture setup/cleanup), including the
  read-only status button, safety-limit action and unknown-error precedence.
- Changed translation units compiled; diagnostic Qt/daemon/CLI links succeeded.
  `git diff --check` passed. No consensus or ordinary DigiByte product source changed.

Local reports are under `build_msvc/paymaster-refresh-check/reserve-presets/`:
`drain-before-final.log`, `drain-after-final.log`, `drain-core-tests-final.log`
and `drain-qt-tests-final.log`. The functional case is registered separately in
`test_runner.py`; it does not run the complete provider contract suite.

Full build, complete Paymaster suites and cross-platform checks remain operator
work. From the repository root, with the existing MSVC/Qt dependencies installed
and applications/CLI calls closed, the local operator helper runs the incremental
solution build plus all Paymaster Qt cases (minutes; build exit 0 and zero failures):

```powershell
.\build_msvc\paymaster-refresh-check\reserve-presets\build-and-check.ps1 -FullQtTests
```

After that build, the focused two-node RPC regression is also directly runnable
(typically under a minute; success ends with `Tests successful`):

```powershell
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
python -X utf8 test/functional/wallet_paymaster_provider.py --descriptors --drain-recovery-only
```

## 2026-10-08: Routine provider synchronization logging

Only `WAITING_FOR_READINESS` with `PAYMASTER_PROVIDER_SYNCING` moves from the
normal pause log to opt-in `bench` logging. No service-state or payment checks
change. The provider-runtime translation unit compiled and the isolated daemon,
Qt application and Core test executable linked successfully. The same three Core
cases and focused two-node `--drain-recovery-only` regression above passed.
Other pause messages and automatic drain recovery remained present in the test
log; this run did not force the short synchronization window itself.

Local reports: `build_msvc/paymaster-refresh-check/reserve-presets/`
`sync-log-core-tests.txt` and `sync-log-functional.txt`. `git diff --check`
passed. No full build, complete suite or cross-platform run was performed;
the operator commands and prerequisites above still apply.

## 2026-10-08: Offer funding and obsolete client errors

The GUI gates new preparation on the selected offer's total and cached DD balance.
Editing a safely closed/uncreated draft clears its previous attempt's notice.
The fee-deduction flag and Core's minimum-change rule are unchanged.

Targeted Windows/MSVC verification:

- Changed Qt units and resource compiled; isolated Qt test/application links passed.
- Four targeted Qt functions passed all 14 data cases (22 results including
  fixture setup/cleanup): `paymasterClientPreparationRequiresCurrentOffer`,
  `paymasterClientFundingBalanceChanges`, `paymasterClientOfferCards` and
  `paymasterClientUncreatedRequestReturnsToCompose`. Cases cover exact coverage,
  one-cent shortage, subtraction, own-DGB Automatic, provider changes, privacy,
  expiry and obsolete error notices. Dark/light previews were inspected.
- `wallet_paymaster_rpc.py --descriptors --client-preparation-only` passed using
  actual RPC and CLI in both fee modes: extra-fee shortage and 1-cent change are
  rejected without creating a session; subsequent exact-total subtraction
  prepares an unsigned request, preserves its flag and can be safely canceled.
  Balances, budgets and mempool are unchanged. This is a preparation test, not
  a new complete-payment run.
- Python compilation and `git diff --check` passed. The shared test fixture now
  includes the already-required zero/no-cap `maximum_user_paid_service_fee_cents`.
  The first test attempts exposed that missing fixture field and the existing
  distinct AUTO/Paymaster error codes; the final assertions cover both.

Reports are in `build_msvc/paymaster-refresh-check/reserve-presets/`:
`offer-funding-final-*.txt` and `offer-funding-rpc3.txt`. The installed Qt build
uses the Windows platform plugin; an initial `offscreen` attempt was unsupported.
No full build, complete suite or cross-platform run was performed. The operator
build command above applies; after building, run the focused RPC/CLI check from
the repository root (under a minute; exit 0 and `Tests successful`):

```powershell
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
python -X utf8 test/functional/wallet_paymaster_rpc.py --descriptors --client-preparation-only
```

## 2026-10-08: Minimum-change compose feedback

The Paymaster compose check now also rejects a positive remaining DD balance
below the network's minimum output. The amount hint, cost summary and send gate
share that check, including fee deduction. This uses the existing cached balance;
Core's input-selection and RPC/CLI rules are unchanged.

Targeted MSVC compilation and isolated application/test linking passed. The same
four Qt functions above passed all 14 data cases, including the added 28.49-DD
gross / 28.50-DD balance regression, zero/1/99/100-cent remainder boundaries,
wallet emptying, matching amount warnings, privacy and provider changes.
The focused RPC/CLI preparation test passed again in both fee modes. Reports:
`change-gate-{preparation,balances,cards,uncreated,rpc}.txt` under the same local
check directory. `git diff --check` passed. No full build, complete suite or
cross-platform run was performed; the operator commands above still apply.

## 2026-10-08: Cancellation bursts and request admission

`wallet_paymaster_provider.py --descriptors --cancel-burst-only` passed using
isolated Windows nodes and actual RPC/P2P/CLI. It prepares three operational
slots, keeps the per-recipient limit at two, cancels two unsigned requests, then
holds the third request at the limit across multiple service ticks. Expiring
the first offer allows that same third request to proceed without a provider
restart. Final expiry restores all three slots. Saved safety/refill policies,
client balance and mempool are unchanged. Mock time advances protocol expiry;
this does not test an arbitrarily long live transport wait.

Targeted MSVC compilation and isolated wallet/daemon linking passed. Three
existing Core cases also passed (62 assertions):
`stale_submit_is_consumed_without_faulting_provider`,
`provider_automation_capacity_pause_preserves_approval`, and
`unsigned_quote_cancellation_atomically_releases_provider_pool`.
Python compilation and `git diff --check` passed. The functional report is
`build_msvc/paymaster-refresh-check/reserve-presets/cancel-burst-functional.txt`.
An initial test attempted a runtime-mode change while running and correctly
hit `PAYMASTER_STOP_PROVIDER_BEFORE_MODE_CHANGE`; the final test keeps automatic
mode throughout and observes its unchanged limits through the CLI.

No full build, complete suite or cross-platform run was performed. The operator
can run the focused check after the full build from the repository root
(existing MSVC/Qt prerequisites; typically under a minute; exit 0 and
`Tests successful`):

```powershell
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
python -X utf8 test/functional/wallet_paymaster_provider.py --descriptors --cancel-burst-only
```

The companion Qt change passed four focused functions with 13 data cases
(21 results including setup/cleanup): `paymasterOperatorWorkTransitions`,
`paymasterOperatorOverviewGuidesAndFailsClosed`,
`paymasterClientCoreCancellationRoundTrip`, and
`paymasterClientUncreatedRequestReturnsToCompose`. They cover both themes,
reserved offers versus node synchronization, the per-recipient limit, preserved
fault precedence, safe cancellation with a lost reply, and incomplete exchange
wording. Updated Qt units and isolated test/application links passed. Reports
are `cancel-<function>.txt` in the same local check directory. These are targeted
checks, not the complete Qt suite.

### Offer and spending-limit review (2026-10-08)

The Paymaster Qt editor now provides a draft-only review after
`PAYMASTER_PROVIDER_SAFETY_POLICY_CONFLICT`. Targeted MSVC compilation, test MOC
generation and isolated Qt application/test linking passed.
`paymasterOfferSpendingLimitsReview` passed 11 data cases, including public and
restricted sponsorship, lower/higher advertised fee caps, preserved manual
edits, discard, failed/malformed acknowledgements, wallet change and privacy.
Its RPC adapter uses the real RPC parsers and Core policy validator to check
each proposed budget against the saved offer before validating the replacement
offer. No write occurs on review and no offer activation occurs on budget save.
The public review was also rendered and inspected in both themes.

The existing `paymasterOfferPolicyTypedValues` passed all 10 English/German cases.
Reports are `<function>-final.txt` under
`build_msvc/paymaster-refresh-check/reserve-presets/`. The existing Core cases
`provider_policy_update_preserves_safety_policy_liveness` and
`public_sponsorship_requires_explicit_finite_safety_limits` passed 58 assertions.
`git diff --check` passed. Core/RPC product code was unchanged; no live wallet
settings were written. No full build, full suite, new end-to-end node test or
cross-platform run was performed for this UI correction.

### Offer notice clarity (2026-10-09)

Targeted Qt compilation and isolated application/test links passed.
`paymasterOfferSpendingLimitsReview` passed its 11 cases with assertions that
the known conflict code is absent from the visible notice and provider status.
`paymasterFeeAmountsAndPercentages` passed, including the 0%-tariff explanation.
The existing Core case
`provider_user_authorization_honors_exact_reserved_safety_binding` passed all
48 assertions, including rejection of a new authorization under a replaced
advertised policy. Reports use the `-notices-20261009.txt` suffix in the local
reserve-presets check directory. No Core/RPC behavior changed; no full build or
complete suite was run.

### Effective percentage limits and timeout observation

Focused coverage: `paymaster_provider_tests/client_percentage_fee_limits_use_actual_amounts_and_preserve_legacy_encoding`
checks cent rounding, net recipient semantics, zero, absolute limits, v1/v2
serialization, unknown versions and truncation.
`paymaster_wallet_store_tests/client_percentage_policy_update_preserves_approval_and_recovery_bounds`
checks persisted approval, omission and recovery return aggregation.
Qt `paymasterClientPercentageLimits` and `paymasterClientTimeoutKeepsObserving`
cover an unapproved 1% suggestion, exact saved values, restart-age guidance,
read-only polling after timeout, and transient status failures.

Run the isolated RPC/CLI integration case with the newly built daemon/CLI:
`python test/functional/wallet_paymaster_rpc.py --client-protection-only --configfile=test/config.ini`.
It checks invalid values, omission, offer filtering, gross/net semantics,
rejection without input reservations, tightened limits before signing and
restart persistence. Existing same-input recovery and non-expiring signed
reservation tests remain required regressions. Full suites remain separate
operator-run release checks.

## Additional wallet-store failure tests (2026-10-10)

The four affected cases passed after targeted MSVC compilation and isolated
linking on 2026-10-10: 655 assertions (191, 58, 254 and 152 in table order).
Compilation exposed ambiguous unqualified `CWallet` names and a duplicate local
variable in the additions; both were corrected in the Paymaster test source.
Reports are in `build_msvc/paymaster-refresh-check/functional-fixes/`.
The operator's earlier 320-case run used a binary built before these additions
and does not cover them. No production source, shared wallet test infrastructure,
database format or consensus rule changed for these failure-test additions.

All four cases are in `paymaster_wallet_store_tests`:

| Case | Additional checks |
| --- | --- |
| `client_fee_policy_write_failures_preserve_approval_and_open_exposure` | First approval, tightening and legacy omission fail atomically at begin, each write and commit; saved percentage/absolute limits, open/spent fees and monotonic accounting time survive. Reopening persisted rows and retrying a lost reply preserve the approval. |
| `unsigned_client_cancel_failures_and_lost_reply_preserve_input_ownership` | Failed cancellation preserves both inputs and in-memory locks; commit failure rolls back deletion of reservations/locks. A lost-reply retry is harmless and cannot unlock inputs subsequently reserved by a new request. |
| `client_attempt_artifacts_and_authorization_are_append_only_and_atomic` | Existing fixture extended with failures during fallback and signature persistence, including wallet-flag atomicity. A persisted signature survives a failed retry and quote expiry after reopening; unsigned cancellation remains forbidden. |
| `self_recovery_is_same_input_idempotent_and_reorg_safe` | Existing fixture extended with failures while storing a return and rolling back a confirmation. Lost-reply retry uses the identical durable return, rejects a conflicting replacement and retains signed input reservations. |

The local `CheckPaymasterWriteFailures` helper checks the error category, exact
number of attempted writes, byte-identical database records, unchanged wallet
flags, and operation-specific safety invariants. Failure injection is cleared
before assertions so later checks do not accidentally inherit it. It reuses the
existing `MockableDatabase`; no new production fault switches are introduced.

Scope limits: reopening here constructs another wallet over copied mock records;
it does not kill a process or reopen a real SQLite file. The mock cannot inject
individual `EraseKey` failures; the cancellation commit-failure case covers
rollback after deletions, not failure of each deletion. Real storage exhaustion,
power loss, older provider backups and adversarial original-payment/recovery
network races remain separate integration requirements. Reorg persistence checks
do not establish complete fee-accounting correctness across every reorg.

For a subsequent normal-binary rerun, incrementally rebuild `test_digibyte`
with the matching libraries. In this workspace, the local helper is:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\build_msvc\paymaster-refresh-check\fault-tests\build-and-check.ps1
```

This workspace-local helper is not a tracked release artifact. It requires the
existing MSVC v143/14.43.34808 installation, cached static dependencies and a
successful preceding solution build. It builds only the unit-test project with
project-reference builds disabled, then runs the four cases separately and
records their logs. Expect a few minutes for compilation/linking, then short
unit-test runs. Require exit code 0 from every command; stop on the first failure.
No complete solution build or GUI/daemon/CLI replacement is performed.

The workspace helper explicitly passes `SolutionDir=.../build_msvc/` when
building the test project directly. Without it, referenced libraries are
resolved below each project directory instead of the existing solution output
directory, causing LNK1181 despite the libraries being present. After correcting
this helper on 2026-10-10, its normal `test_digibyte.exe` build and all four cases
passed (655 assertions, exit 0). Reports are in
`build_msvc/paymaster-refresh-check/fault-tests/20261010-065506-151Z/`.
This follow-up rebuilds the unit-test executable only; it does not incorporate
the separate RPC-schema source correction into the normal GUI/daemon binaries.

On another supported build, rebuild the unit binary through that platform's
normal build system and select each case with
`--run_test=paymaster_wallet_store_tests/<case>`.

## Functional matrix follow-up (2026-10-10)

The operator's normal build succeeded, as did the Paymaster Qt group (443 results)
and the selected 320 Core cases (12,364 assertions). Of seven functional scenarios,
provider drain recovery, readiness and reorg passed; RPC, full provider, lifecycle
and failover initially failed. These results predate the additional wallet-store
failure tests above.

The four failing scripts were corrected and rerun individually:

- RPC: the preparation/cancellation cases exhausted the listener's four initial
  admissions from one local netgroup. The fixture now allows one admission to
  replenish on the real monotonic clock before its next independent channel;
  production limits and error checks remain intact. Continuing the test exposed
  a genuinely incomplete `getpaymasterreputation` result schema. The Paymaster
  RPC now declares all returned fields. The test requires nonempty records and
  compares RPC and CLI responses using the same client wallet.
- Provider: the insufficient-funds assertion now matches the stable
  `PAYMASTER_DD_INPUT_SELECTION_FAILED` category and current balance message;
  no-session and no-financial-side-effect assertions remain.
- Lifecycle: a restored backup with wallet change is already reconciled by
  startup maintenance. The test requires confirmed state, the exact txid, one
  fee charged and zero open fee reservations, and rejects another `retry_same`.
  The no-change variant still requires a protected pending authorization until
  exact txindex observation. Both variants test four RPC and four CLI entry
  points, unchanged mempools/provider accounting, and persistence after reload.
- Failover: provider setup uses the shared policy fixture, including the required
  service-fee cap field, while retaining its distinct provider fee rates.

Successful isolated runs under
`build_msvc/paymaster-refresh-check/functional-fixes/` are `rpc-3`, `provider-1`,
`failover-1`, `lifecycle-2` and `lifecycle-no-change-1`. Each has exit code 0 and
its own `test_framework.log`. The schema and wallet-store test source passed
targeted MSVC compilation; separate daemon/unit executables were linked against
the operator-built libraries. The normal executables were not replaced.
An initial wallet-library Build target scheduled unrelated recompilation and
was stopped before replacing that library; validation used selected-file
compilation and isolated links instead. Python syntax and `git diff --check`
passed. No complete build, full Qt/Core rerun, cross-platform, real-Tor or fuzz
campaign was performed in this follow-up. The normal binaries still require a
regular operator build before these source changes are included in them.

## Recovery fee reorg regression (2026-10-10)

The review first reproduced two failed invariants in an isolated copy of the
wallet-store recovery test: after a one-block recovery was disconnected, its
3-cent original fee stayed released; with 4 cents already spent, another 16
cents could be reserved against a 20-cent daily limit. The diagnostic artifacts
are under `build_msvc/paymaster-refresh-check/security-review/`. This is a
wallet-store simulation, not a demonstrated live-chain theft.

The fix changes only Paymaster wallet code. Fees remain reserved through the
existing 240-block safety depth. A common helper repairs retained historical
liabilities under the wallet lock before new payment/recovery acceptance and
final reconciliation; changes share the caller's atomic database transaction.
Routine final observations inspect only their session, avoiding repeated
wallet-wide scans. Limits and already-spent accounting dates are preserved.

Four new cases plus the extended self-recovery case passed **384 assertions**
using selected MSVC compilation and an isolated link. The related selection
passed **28 cases / 1,542 assertions**, including those five. Coverage includes:

- A shallow confirmation, disconnection, 239/240-confirmation boundary and
  begin/write/commit failures without lost fee or input protection.
- Released and aged-out fee rows, lowered saved limits, reopened persisted
  records, invalid authorization evidence and idempotent reconciliation.
- Denying new approval before periodic repair has run.
- A winning original payment with a stale canceled recovery status, exact
  deep local confirmation, late provider results and duplicate replies.
- Existing signing, ownership, concurrent budget/pool, percentage-limit,
  unsigned-cancel and persistence-failure tests.

Reports and isolated artifacts are under
`build_msvc/paymaster-refresh-check/reorg-fee-fix/`. The normal application
executables were not replaced. Full product/Qt/functional/sanitizer/fuzz and
real storage-failure verification were not run in this targeted follow-up.
The correction retains the existing 240-block pruning horizon; it does not
reconstruct already-pruned history after deeper reorganizations.

Workspace-local operator command, from `D:\Digibyte\digibyte-fork`, after
closing Client and Paymaster normally:

```powershell
.\build_msvc\paymaster-refresh-check\reorg-fee-fix\build-and-check.ps1
```

Requires the previously installed MSVC 14.43, Qt 5.15.10 and cached vcpkg
dependencies. It builds the regular Release solution, then runs every registered
`paymaster_*` Core suite, recording source/binary hashes and timestamped reports.
Runtime is normally minutes; success requires build/test exit code 0 and zero
failed tests. It does not replace the separate full release matrix or independent
security review.

## Provider security corrections (2026-10-10)

The two reproduced findings in the [threat model](paymaster-threat-model.md) now
have source corrections. Exact provider commit recovery can outlive the rolling
24-hour expenditure ledger without recharging fees or allowing a new signature.
Expired unsigned provider history is pruned atomically after its complete replay
window, with a 64-session cleanup bound and an 8,192-session admission bound.
Signed/ambiguous records and historical restricted capabilities stay protected.

Selected MSVC compilation of seven translation units and an isolated link passed.
Two new tests and an extended durable-commit case pass **3 cases / 8,539 assertions**:

| Case in `paymaster_wallet_security_tests` | Coverage |
| --- | --- |
| `provider_final_commit_spends_budget_atomically` | Real signed final, exact 24-hour boundary, aged-out SPENT row, restart and policy change, successful durable recovery, no duplicate budget write/charge, rejection of conflicting and corrupt evidence, unchanged RESERVED checks. |
| `expired_unsigned_provider_history_is_compacted_atomically` | Full replay boundary, atomic begin/commit failure rollback, protected signature/budget/capability records, reopened mock database, removal of all associated session/attempt indices, no permanent unsigned tombstone, stale replay rejection and idempotent maintenance. |
| `provider_history_quota_preserves_existing_requests` | Actual 8,192-session boundary, existing-request continuation and new-request admission after space is freed. Its 8,197 assertions mostly insert fixture records. |

The related selection, including these three cases, passes **48 cases / 10,012
assertions**, exit 0. It covers protocol bindings, signatures, malformed messages,
transport isolation, recovery firewalls, concurrent budget/pool ownership and
prior client recovery-fee/reorg fixes. Artifacts are workspace-local under
`build_msvc/paymaster-refresh-check/security-fixes/`: `focused-report.txt`,
`related-report.txt`, `related-filter.txt`, `results.json`, source hashes/diff and
the isolated test executable. Existing operator-built libraries and the previous
client-fee test object are reused; this is not a full rebuild of every dependency.
The normal application executables have not been replaced.

For the operator's full current-source build and all Paymaster Core suites, close
Client/Paymaster normally and reuse the existing helper; its older folder name
does not pin an older source revision:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\build_msvc\paymaster-refresh-check\reorg-fee-fix\build-and-check.ps1
```

Prerequisites: the existing MSVC 14.43, Qt 5.15.10 and cached vcpkg installation.
Runtime class: minutes for the incremental full Release solution build and tests.
Success requires build/test exit code 0 and no failed tests; preserve the printed
report directory. This command builds the normal GUI, daemon, CLI and test binary.

Then run the focused functional matrix against those freshly built binaries:

```powershell
$env:DIGIBYTED = (Resolve-Path '.\build_msvc\x64\Release\digibyted.exe').Path
$env:DIGIBYTECLI = (Resolve-Path '.\build_msvc\x64\Release\digibyte-cli.exe').Path
python -X utf8 test/functional/test_runner.py wallet_paymaster_lifecycle.py wallet_paymaster_reorg.py wallet_paymaster_failover.py wallet_paymaster_provider.py wallet_paymaster_rpc.py -j1
```

This requires the existing Python test environment and `test/config.ini` pointing
to this checkout. It uses temporary test wallets/regtest, takes minutes to tens
of minutes and must report every selected scenario passed with exit code 0.
No live wallet is needed. These operator commands were not executed in this
follow-up. Full Qt/release, sanitizer/fuzz, real SQLite disk-full/power-loss,
stale-provider-backup and adversarial network/load checks remained open at that
follow-up; the backup result below supersedes that test gap. The new
historical alternative-provider commit retry also needs a full functional rerun;
the new aged-commit regression directly exercises the normal provider path.

## Stale provider backup regression (2026-10-10)

`wallet_paymaster_backup.py` is registered with two variants in the functional
runner. It uses only temporary descriptor SQLite wallets and two regtest nodes,
real quotes, client authorization, provider signing and independently computed
DGB transaction fees. It unloads the original provider before restore, then
restarts both processes so volatile announcement sequence caches cannot mask
the rollback. **Both variants reproduced TM-003 before its correction and now
pass with the durable explicit-restore quarantine**, described in
[the threat model](paymaster-threat-model.md).

| Variant | Fixture and pre-fix observed failure |
| --- | --- |
| Default | Backup before two payments; first confirmed, second in both mempools. The 0.2-DGB daily allowance was exhausted before restore. Restored autostart reports ready with zero spent; it signs another payment. All three payments confirm, proving 0.3-DGB actual fees under the unchanged 0.2-DGB approval. |
| `--pending-offline` | Backup after the first confirmation, before a second signed final; restart both disposable nodes with `-persistmempool=0 -walletbroadcast=0`, retaining the valid final outside them. Restore forgets its cost and exposes its pool inputs; a new provider-signed transaction reuses an input. This is conflicting authority, not proof that both competing transactions settle. |

The probes also verify the original provider was blocked by its exhausted
budget; old missing templates are rejected via RPC and the real CLI; retained
exact commits replay idempotently; chain/mempool-visible spent inputs stay
unavailable; a reserve-preparation preview changes neither payment budget nor
mempool. Their JSON summary contains public status/transaction identifiers,
not keys or signed PSBTs. Execution of paid refill/withdrawal/retirement after
restore remains additional coverage to implement with the correction.

Tested against normal Release daemon SHA-256
`a7c37549e2b15c8cbe018e8e6621ffb8911ac184bb19c95777e4248d1cb763d8`
on branch `integration/paymaster-v9.26.7`, HEAD
`9478107c93fc4e8ac6a8bd337b67c25680c7a9be`, plus the new uncommitted test.
No product build or binary replacement was performed. Reports are workspace-local:

- `build_msvc/paymaster-refresh-check/provider-backup/visible-20261010-170704/`
- `build_msvc/paymaster-refresh-check/provider-backup/offline-20261010-170511/`

Both exit 1 at the final security assertion, after the payment probes complete.
The default reports three failed protection conditions; offline reports four.
The following receipts document the original failing product. Preserve each run's
`test_framework.log`, node logs and `provider-backup-observations.json`.

The completed test was then run through both registered runner entries: 25 and
32 seconds, both exit 1 at the same reproduced defects. The additional autostart
check gives four and five failed conditions, respectively; all 18 framework
unit checks pass. Final test source SHA-256:
`832d85d4a5bb3ab58bf25dbfae973fbb04b1d0233f0cf3e114f7a17cbb0b3db6`.
`build_msvc/paymaster-refresh-check/provider-backup/run-receipt.json` records
checkout/binary hashes and final results. `final-visible-observations.json`,
`final-offline-observations.json` and corresponding `final-*-test.log` copies
provide accessible report paths outside the runner's Unicode directory.

Reproduce from the repository root with the existing Python environment,
SQLite/wallet/txindex-enabled Release binaries and `test/config.ini`. Each
variant takes roughly 30–60 seconds; no full rebuild is needed merely to run
these Python tests against the existing executable:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:DIGIBYTED = (Resolve-Path '.\build_msvc\x64\Release\digibyted.exe').Path
$env:DIGIBYTECLI = (Resolve-Path '.\build_msvc\x64\Release\digibyte-cli.exe').Path
$env:PYTHONUTF8 = '1'
python -X utf8 test/functional/wallet_paymaster_backup.py --configfile=test/config.ini --descriptors --nocleanup
python -X utf8 test/functional/wallet_paymaster_backup.py --configfile=test/config.ini --descriptors --pending-offline --nocleanup
```

After rebuilding the corrected Paymaster code, require
**exit 0 from each command**: missing history must keep new authority paused or
be safely reconstructed, the actual old fees must not disappear from the
effective allowance, and hidden signed inputs must not be re-signed. Keep
exact known-commit replay and read-only diagnostics safe. Reserve previews and
execution may instead reject the explicit restore guard without spending or
releasing inputs. Do not replace these
assertions with expected-success checks for the vulnerable behavior.

No live wallet was touched. This scenario does not simulate physical SQLite
write/checkpoint failure, whole-datadir rollback, deep reorgs or hostile transport.

Python AST syntax checks and `git diff --check` pass. The prescribed Python lint
script was invoked but skipped because `flake8` is absent; its exit 0 is **not**
a completed lint check. No package was installed. To run the repository's
selected flake8 rules without modifying the operator's global Python environment:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
python -m venv build_msvc/paymaster-refresh-check/provider-backup/lint-env
$backupLintPython = (Resolve-Path 'build_msvc/paymaster-refresh-check/provider-backup/lint-env/Scripts/python.exe').Path
& $backupLintPython -m pip install flake8
& $backupLintPython -m flake8 --version
$backupLintRules = python -c "import runpy; print(runpy.run_path('test/lint/lint-python.py')['ENABLED'])"
& $backupLintPython -m flake8 --ignore=B,C,E,F,I,N,W "--select=$backupLintRules" test/functional/wallet_paymaster_backup.py
```

This optional local lint environment needs package-download access; installation
and lint take seconds to a few minutes. Success is a printed flake8 version and
exit 0 without lint diagnostics. Set `PYTHONUTF8=1` when using the functional
runner on Windows: `python -X utf8` on its parent alone does not propagate UTF-8
to child scripts. The final run's console emitted encoding diagnostics for the
runner's Unicode directory; file reports and financial probes completed normally.

### Correction and current verification

Explicit GUI/RPC restore now commits `pmrestoreguard` before `LoadWallet`,
callbacks, autostart or reserve preparation. New provider signing and spending
fail closed; no configuration, backup acknowledgement or expired clock clears
the record. No automatic history reconstruction or guard-clear RPC is provided.
Client-only restore keeps its DD balance and fee policy. Known exact committed
payments still replay without new signing.

Both variants pass using the isolated, selected-object MSVC daemon in
`build_msvc/paymaster-refresh-check/provider-backup-fix/`. They cover RPC and the
real CLI, restored autostart/manual start, withheld final transactions,
preparation preview/execution, reserve retirement, withdrawal, capital release,
settings/backup-acknowledgement changes and automatic runtime selection. They
also manually copy an already guarded SQLite image under a new wallet name,
load it without the restore RPC, restart the node and require RPC/CLI startup
and reserve actions to remain blocked. This proves the guard survives copying;
it does not detect rollback to an unmarked image. The wallet tests additionally
call both refill builders and the shared signing
boundaries directly, cover marker write/commit rollback, restart/idempotence,
append-only writes and malformed/future records. The GUI test checks a clear
restore instruction in both themes, including simultaneous liquidity errors.
Final selected unit results: 22 cases / 9,348 assertions (16 security cases /
8,971 assertions, 5 PSBT cases / 311 assertions, one backup-metadata case /
66 assertions). Both real backup variants exit 0, as does the targeted Qt check.
Hashes, build/link logs and exact test reports are recorded in the local
`provider-backup-fix/run-receipt.json` and adjacent files.

The normal product executables have not been rebuilt/replaced by this selected
verification. Delegate the full MSVC solution build and wider Paymaster Qt suite
to the operator. From the repository root, with the existing MSVC v143 toolchain,
static Qt 5.15.10, vcpkg dependencies, Python and closed normal client/provider
processes, run:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\build_msvc\paymaster-refresh-check\provider-backup-fix\build-and-check.ps1 -Build -FullQtTests
```

This workspace-local helper builds the full solution (avoiding mismatched
project library paths), runs the focused wallet security/PSBT/backup-metadata
checks, both registered backup variants and the complete Paymaster Qt suite.
Runtime is minutes to tens of minutes depending on the build cache. Success is
exit 0, both functional variants passing, no failed unit/Qt tests and a report
directory printed at the end. It records source and normal-binary hashes before
testing and restores its temporary environment variables afterward. For another
checkout, use the equivalent full solution build, the two Python commands above,
`test_digibyte.exe --run_test=paymaster_wallet_security_tests`,
`--run_test=paymaster_wallet_psbt_tests`, and the `PaymasterWidgetTests` Qt filter.

At that earlier checkpoint, physical SQLite write/checkpoint failure, unmarked
wallet-file/whole-datadir rollback, safe migration, deep reorgs and sanitizer/fuzz
campaigns were unexecuted. The independent checkpoint section below now covers
unmarked wallet-only rollback; the other scenarios remain open. Do not claim
general stale-backup recovery or release approval from these passing cases.

### SQLite engine failures after the backup correction

Two additional cases in `paymaster_wallet_security_tests.cpp` use actual,
temporary SQLite databases with the normal wallet backend:

- `sqlite_full_quote_preserves_budget_pool_and_retry` caps `max_page_count` at
  the database's current size and independently verifies an actual `SQLITE_FULL`.
  A subsequent provider quote must fail without altering any stored record,
  fee reservation, pool binding or request index. The database must return to
  autocommit, reopen with the original record digest, and then accept exactly
  one reservation for the same request. A further exact retry changes no record.
- `sqlite_failed_restore_commit_cannot_leave_partial_guard` uses SQLite's
  connection-local authorizer to deny only `COMMIT`. Marker creation must fail
  with `guarded=false`, roll back, and reopen with every record unchanged.
  After removing the fault, marker creation succeeds and blocks provider
  authority after another close/reopen.

The focused run passes two cases / 123 assertions using the selected-object
MSVC test binary. The full focused wallet security suite, including these two
cases, passes 18 cases / 9,094 assertions. The connection-local page cap does
not fill the filesystem;
commit denial is deliberate engine-level fault injection. Neither test proves
physical ENOSPC, failed journal/database writes or fsync, torn writes, device
cache durability or power-loss behavior. Those TM-007 scenarios remain open.
No production code or base SQLite implementation was changed for these tests.

After rebuilding `test_digibyte` with SQLite support, run from the repository root:

```powershell
& .\build_msvc\x64\Release\test_digibyte.exe '--run_test=paymaster_wallet_security_tests/sqlite_*' --report_level=short
```

The two cases run in seconds. Success means exit 0, two passing cases and no
failed assertions. The full-build helper above includes them automatically
when it runs `paymaster_wallet_security_tests`; the SQLite-disabled build does
not register them. Local evidence is in `provider-backup-fix/unit-sqlite-faults.txt`
and `sqlite-faults-receipt.json`, with source/object/binary/report hashes.

### Independent provider checkpoints (2026-10-10)

The provider-wallet generation commits with every Paymaster mutation. A bounded
node-local checkpoint is flushed/replaced before wallet commit. Native reserve
operations carry a durable pending fence until wallet transaction and accounting
records are saved. Missing/mismatching state, invalid binding/version, exhausted
generations and interrupted fences reject fresh authority; there is no reset RPC.
Normal wallet writes keep their original transaction semantics. Paymaster logic
lives in `wallet/paymastercheckpoint.*` and `paymasterdb.cpp`; WalletBatch supplies
only transaction/lifetime notifications.

Focused MSVC verification passes 33 cases / 9,923 assertions (25 security,
five PSBT, one backup-metadata and two original wallet DB cases). The seven new
`checkpoint_*` cases include actual SQLite commit denial and matching invalid
pairs so semantic checks cannot be masked by a simple generation mismatch.
Both `wallet_paymaster_backup.py` variants pass after copying an unmarked stale
image under another wallet name and restarting; RPC and CLI must refuse new
start/preparation/release without a mempool change. Client-only restore remains
compatible. The readable GUI protection case passes in dark and light themes.
Python syntax parsing and `git diff --check` pass; flake8 is not installed.

The isolated binaries and receipt are in the workspace-local
`build_msvc/paymaster-refresh-check/provider-backup-fix/`. The receipt pins
source, object, binary and report hashes. These are selected-object checks;
normal application executables still require a full build. Following the
repository/operator workflow, close the normal client/provider instances and
run from `D:\Digibyte\digibyte-fork`:

```powershell
.\build_msvc\paymaster-refresh-check\provider-backup-fix\build-and-check.ps1 -Build -FullQtTests
```

The helper regenerates MSVC projects from `src/Makefile.am`, builds the whole
solution with the existing MSVC v143/Qt 5.15.10/vcpkg environment, runs focused
wallet checks, both regtest variants and the complete Paymaster Qt suite.
Prerequisites: configured test/config.ini, Python, existing MSVC 14.43.34808,
Windows SDK 10.0.26100.0 and static Qt at D:\Qt51510\install. Runtime: minutes
to tens of minutes. Success: exit 0, both functional variants passed, no failed
unit/Qt cases and an operator report directory. The helper restores its process
environment after running. On other checkouts, regenerate/build using
build_msvc/README.md and run the same suite/functional/Qt filters.

Full fresh builds/full Qt were not run locally. POSIX synchronization rejects
unsupported or failed fsync; its execution on Linux/macOS, real physical storage
failures, power loss/torn writes and sanitizer/fuzz campaigns remain open. A
wallet/checkpoint pair restored together or operated on multiple independent
hosts is outside the node-local detection guarantee; initial legacy enrollment
is a trusted complete-history baseline. Do not erase marker files to resume.
