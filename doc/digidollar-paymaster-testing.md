# Paymaster build and test runbook

This is a reproducible verification index, not a running work log. The
[release gate](digidollar-paymaster-release-gate.md) separates historical evidence
from checks still required for a candidate.

Operational documentation primarily targets Mainnet. Run the tests here in
isolated test environments, not with a Mainnet wallet. The
[Testnet/Regtest supplement](digidollar-paymaster-test-networks.md) provides
manual test configurations; a test pass does not establish public Mainnet
reachability or replace the candidate's release gates.

## Preparation and result recording

Read repository instructions, [test guidance](../test/README.md),
[functional guidance](../test/functional/README.md) and applicable build docs.
Use a clean test data directory, never an operator wallet. Summarize actual
results in the [release evidence](digidollar-paymaster-release-gate.md), with
full logs and binary hashes retained as build/CI artifacts. Record:

- Source commit and dirty diff, compiler/dependencies and build configuration.
- Exact executable provenance, command, selected cases and exit status.
- Failed/skipped tests and environmental limitations; keep full logs privately.
- Whether this was a normal product build or isolated/incremental compilation.

Do not count registered tests as passing, or transfer results from an old
binary to current source. A quota fixture's many assertions are not thousands
of independent security cases. Run narrow regressions first; broad builds,
sanitizers and complete matrices are release/operator jobs.

## Build entry points

Linux/WSL: follow [Unix build instructions](build-unix.md) and the repository's
actual dependency configuration. Normal build entry points are `./autogen.sh`,
`./configure` with the required wallet/Qt options, then `make`. Do not reuse a
sanitizer-only configuration that disables the daemon for functional tests.

### Windows build from PowerShell

Follow the [MSVC prerequisites](../build_msvc/README.md), including the static
Qt build/patch and vcpkg dependencies. Run from the repository root. Stop
processes using the executables that will be replaced. A normal build can take
minutes to hours depending on how much is already built; use its exit code and
final error summary, not the existence of an old executable, as the result.

`msbuild` is often absent from an ordinary PowerShell PATH even when Visual
Studio is installed. The following discovers the installed executable. First
set these **installation-specific example paths** to your actual Qt prefix and
vcpkg installed root (the directory containing `x64-windows-static/lib`). A
nested local vcpkg layout may require a different root than this example.

```powershell
$qtBase = 'C:/Qt_static'
$vcpkgInstalledDir = 'C:/dev/digibyte/build_msvc/vcpkg_installed/'
```

Then run this block in the same PowerShell. It assumes dependencies already
exist and disables automatic manifest installation for this build:

```powershell
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is missing; follow the MSVC prerequisites.' }
$msbuildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuildPath) { throw 'MSBuild was not found in the installed Visual Studio products.' }
if (-not (Test-Path -LiteralPath "$qtBase/lib/Qt5Core.lib")) { throw 'Set qtBase to the installed static Qt prefix.' }
if (-not (Test-Path -LiteralPath "$vcpkgInstalledDir/x64-windows-static/lib")) { throw 'Set vcpkgInstalledDir to the parent of the installed triplet directory.' }
if (-not (Test-Path -LiteralPath './build_msvc/msvc-autogen.py')) { throw 'Run this from the repository root.' }

python ./build_msvc/msvc-autogen.py
if ($LASTEXITCODE -ne 0) { throw 'Project generation failed' }
$buildArgs = @(
    './build_msvc/digibyte.sln', '/t:Rebuild',
    '/p:Configuration=Release', '/p:Platform=x64',
    "/p:QtBaseDir=$qtBase", "/p:VcpkgInstalledDir=$vcpkgInstalledDir",
    '/p:VcpkgManifestInstall=false', '/m:1', '/verbosity:minimal'
)
& $msbuildPath @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
```

If dependencies are absent, complete the normal prerequisite installation first;
do not treat this block as a toolchain installer. Preserve the project's selected
toolset. `/t:Rebuild` is required because database interfaces and Qt wallet/send
object layouts changed: all binaries, allocators and test libraries must use the
same headers. Prior isolated-check provenance is in the release evidence.

After success, expected Release outputs are under `build_msvc/x64/Release/`.
Start the intended GUI/daemon from that directory with the original network and
data-directory arguments; do not accidentally reopen an older shortcut target.
Report source commit, first build error (if any), final summary and exit code.
A newly built GUI feature cannot be judged from a process still running old code.

## Unit and Qt selection

In a configured Unix build:

```sh
./src/test/test_digibyte '--run_test=paymaster_*' --report_level=short
```

The prefix includes split wallet suites. Windows equivalent:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster unit tests failed' }
```

For the inbound-message and pending-balance corrections, use the freshly built
test executable from the repository root. This focused selection normally takes
seconds to a few minutes; success requires exit zero and no failed or unexpectedly
skipped selected cases:

```powershell
$focusedCases = @(
    'paymaster_recovery_tests',
    'paymaster_wallet_identity_tests/unknown_recovery_submit_is_consumed_before_normal_work,stale_submit_is_consumed_without_faulting_provider,automatic_submit_defers_before_wallet_or_index_wait',
    'paymaster_wallet_security_tests/normal_submit_classifies_peer_payload_and_local_authority,provider_recovery_pre_signature_firewall_reloads_pool_and_budget,expired_unsigned_provider_history_is_compacted_atomically,quote_prevout_lookup_failure_preserves_local_authority',
    'digidollar_mint_cleanup_tests/pending_balance_excludes_expired_mints_without_position_records',
    'digidollar_wave17_spendability_tests',
    'paymaster_wallet_identity_tests/paymaster_dd_reservations_are_owned_but_not_spendable,unreadable_paymaster_locks_fail_closed_for_coin_selection,paymaster_input_scan_reuses_pool_without_cross_scan_cache'
)
& ./build_msvc/x64/Release/test_digibyte.exe "--run_test=$($focusedCases -join ':')" --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Inbound and pending-balance regressions failed' }
```

Then run `wallet_paymaster_reorg.py` using the [functional runner](#functional-selection)
with freshly built daemon and CLI binaries. It must reject the unrelated recovery
and finish the real payment/reorganization sequence with RPC contract checking
enabled. A failure during fixture setup is not a pass for that regression.

For the wallet-lock ownership and reorganization-readiness corrections, rebuild
the changed sources and start with this focused selection:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_wallet_store_tests/unsigned_client_cleanup*:paymaster_wallet_store_tests/unsigned_client_cancel_failures_and_lost_reply_preserve_input_ownership:paymaster_wallet_store_tests/ambiguous_authorization_is_protected_and_prunes_to_tombstone:paymaster_application_tests/*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Wallet safety regressions failed' }
```

These cases preserve later manual locks through repeated cancellation, pruning
and actual wallet reload; reject unreadable reservations; and check transaction
rollback. Readiness cases capture a real locator before invalidating its tip and
reject stale hashes, including replacement at the same height. They also exercise
both readiness functions with a real in-memory transaction index. Mock database
failures do not simulate physical I/O failure or individual `EraseKey` failures.
Then run the complete store/load suites and ordinary wallet regressions as an
operator check; isolated links are not clean product-build evidence.

Qt uses `DIGIBYTE_QT_TEST_SUITE` and optional `DIGIBYTE_QT_TEST_FUNCTION`
filters implemented by `src/qt/test/test_main.cpp`. Select `PaymasterWidgetTests`
and relevant `DigiDollarWidgetTests` cases from the current fixture. Clear filters
before claiming a complete Qt run. A targeted result including init/cleanup
is not that many independent feature cases.

Direct mint/send fixtures must await the model's asynchronous DGB balance cache
with `DigiDollarTest::SyncDGBBalance`; one `pollBalanceChanged()` call is not a
completed read. Standalone post-Thaw-Day mint fixtures also finish RPC warmup
after preparing their test chain/quote, rather than relying on `AppTests` having
run first. Native dropdown render checks sample the visible row/viewport
intersection and verify image bounds; read-only offer previews are not selectable
rows. These fixture rules do not change production balances or safety checks.

For an operator-run full Paymaster GUI suite on the configured Windows build:

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_STYLE_OVERRIDE = 'Fusion'
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
& ./build_msvc/x64/Release/test_digibyte-qt.exe
if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt tests failed' }
```

Use the `windows` plugin for Windows modal-dialog tests; the local Qt 5.15.10
`minimal` plugin lacks the native Windows interface used by some `QMessageBox`
dialogs. Pure controller/queue tests do not construct those dialogs. Run from the
repository root after a successful current-revision build with tests enabled. Allow several minutes or longer; some fixtures build chains.
Success requires exit code zero and no failed tests. This suite still does not
replace the functional matrix, ordinary-wallet regressions or release gates.

## Architecture checks

From the repository root:

```powershell
python test/lint/test_paymaster_boundaries.py
if ($LASTEXITCODE -ne 0) { throw 'Boundary fixtures failed' }
python test/lint/lint-paymaster-boundaries.py
if ($LASTEXITCODE -ne 0) { throw 'Dependency/cycle checks failed' }
```

Local scans do not establish trusted-base change approval; see the
[boundary contract](design/paymaster-integration-boundaries.md#enforcement-and-trusted-policy-bootstrap)
for committed candidates. Required Core-change reasons stay in the
[change record](design/paymaster-change-record.json).

For review, compare new work against the task base and the complete fork against
the official base. Keep the normal diff; `-w` additionally exposes changes without
historical whitespace noise:

```powershell
$paymasterBases = Get-Content doc/design/paymaster-change-record.json -Raw | ConvertFrom-Json
git diff $paymasterBases.task_baseline HEAD
git diff -w $paymasterBases.task_baseline HEAD
git diff -w $paymasterBases.official_baseline HEAD
```

## Coverage map

Paths below are relative to `src/test/`, `src/wallet/test/`, `src/qt/test/` or
`test/functional/` as applicable. Qt coverage is in `paymasterwidgettests.cpp`
unless another suite is named. Test names describe coverage, not executed passes.

| Invariant / behavior | Main test locations |
|---|---|
| Amounts, exact fees, manifests and signatures | `paymaster_types_tests.cpp`, `paymaster_txbuilder_tests.cpp`, `paymaster_psbt_tests.cpp`; wallet PSBT/security suites. |
| Channel proof and bounded admission | Protocol/wire/transport/admission suites; `p2p_paymaster.py`, `p2p_paymaster_connection_capacity.py`. |
| Nonfinancial diagnostics | Transport `diagnostic_*`; `rpc_paymaster_endpoint.py` tests TCP/SOCKS5 isolation and no authentication; Qt `paymasterDiagnosticCommands`, `paymasterDiagnosticRefreshAndPrivacy`, `paymasterNodeConnectionEditor`. Local proxy coverage is not public Tor evidence. |
| Selection, fee/privacy limits and failover | Client comparator cycle permutations/strict-order properties; directory/reputation suites; `wallet_paymaster_offer_selection.py`, `wallet_paymaster_failover.py`. |
| Exact authorization and same-ID retry | Wallet service/store/security suites; `wallet_paymaster_rpc.py`, `wallet_paymaster_provider.py`: lost replies, replay, unsigned/signed cancellation, work exclusion. |
| Inbound rejection versus local uncertainty | Identity `unknown_recovery_submit_is_consumed_before_normal_work`; security submit/recovery/quote fixtures; recovery `submit_payload_firewall_preserves_full_chain_validation`. `wallet_paymaster_reorg.py` rejects an unknown recovery on an authenticated local channel, verifies unchanged balances/pools/budgets, then completes and reconfirms a real payment. |
| Pending-mint balance consistency | Mint cleanup `pending_balance_excludes_expired_mints_without_position_records` checks both pending APIs across expiry, reorg, mempool and unknown metadata/height; run ordinary DD spendability and Paymaster reservation-balance cases alongside it. |
| Signed recovery and reorg accounting | Recovery/reservation/store suites; `wallet_paymaster_reorg.py`, `wallet_paymaster_lifecycle.py`; provider tests retain exposure at 1/239 confirmations and release at safety depth 240 without double charging. |
| Finite setup and whole-slot refill | Setup/identity/load/store suites; `wallet_paymaster_pool_setup.py`, `wallet_paymaster_partial_capacity.py`, `wallet_paymaster_readiness.py`, `wallet_paymaster_operator.py`; both assets and usable capacity below target. |
| Monotonic maintenance time | `maintenance_reconciliation_preserves_accounting_time`; pool/lifecycle/provider functional tests cover confirmed stages, clock rollback, restart and absence of duplicates. |
| Setup parity and funding observations | `paymaster_setup_tests`; pool setup `--funding-diagnostics-only`; Qt `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep`, `paymasterPoolPreparationDiagnostics`, `paymasterPoolPreparationStoredFeeDiagnostics`: approvals, shortfalls, malformed/missing observations and read-only refresh. |
| Backup, adoption and stale copies | `wallet_paymaster_backup.py`, `wallet_paymaster_backup_adoption.py`, `wallet_paymaster_backup_exit.py`, `wallet_paymaster_checkpoint_rollback.py`. |
| Cancellation of uncreated work | Identity `maintenance_cancellation*`; Qt `paymasterMaintenanceCancellation`, `paymasterSavedTaskTitles`: exact preview, lost-reply signatures, rollback, checkpoint protection and retained signed work. |
| Excess reserve release | Identity `rebalance_saved_targets_*`; Qt `paymasterGuidedCapitalTasks`, `paymasterReserveReduction`: missing/underfilled assets, saved revisions, DGB-only scope and exact receipts. |
| Stop and retirement | Wire `manager_provider_stop_*`; identity `provider_stop_waits_for_work_without_bypassing_exclusion`, `capital_release_cancels_only_reviewed_uncreated_funding`; Qt `paymasterStoppedOverview`, `paymasterStopAndRelease`, `paymasterRetirementLateResults`: priority, atomic release and uncertain replies. |
| Configuration and signing review | Setup suite; Qt `paymasterConfigurationConflicts`, `paymasterSafetyInputValidation`, `paymasterConfigurationSaveRefresh`, `paymasterManualSubmitConfirmation`, `paymasterOperatorConfirmationWalletBinding`: saved versus draft policy, current wallet/privacy/readiness and exact approval. |
| GUI state, finance and navigation | Paymaster Qt suite covers unavailable/loading states, affordability, cancellation, two reads with mutation barriers, explicit coalescing, wallet switch/shutdown, unlock lifetime, untrusted error text, finance pages/export, responsive themes and preserved drafts. Controller cases inject time and replies; ordinary `WalletTests/async*` cover generic scheduling/start failure/worker shutdown. Mocked RPC fixtures do not replace backend tests. |
| Session index | Wallet store tests at 100/1,000/8,192 sessions: one reconstruction, point reads, commit/abort, reopen and invalidation. |
| Finance v2 | Wallet identity/DB and `paymaster_wallet_finance_tests`: 100,000 events; 100/1,000/8,192 sessions; bounded/filter-bound pages, 256-job limit, unchanged-query scan counts, stale revisions, UTC periods, replay/reorg/abandon/delete, commit failure, atomic migration and restore. `bench` logs measure actual reconciliation lock duration. |
| Native service/RPC contract | `paymaster_application_tests` retains amount units, optional false/zero values, artifacts and structured errors. Identity `background_*` and load `periodic_paymaster_maintenance_persists_prebaseline_capacity_equivocation` cover malformed records, wallet/scheduler isolation and retained evidence. |
| Checkpoint concurrency | Wallet identity/security tests: independent identities, shared clone locks, read/replace, rollback/restore and checkpoint-ahead failures. |
| Architecture controls | Boundary fixtures reject forbidden paths/dependencies, missing Core records, new cycles and candidate attempts to weaken trusted checks. |
| Ordinary behavior and migration | DGB/DD wallet/DB/Qt tests with Paymaster disabled, both DB backends, sanitizers/platform builds and historical compatibility below. |

## Functional selection

Run the current-current matrix with built daemons/CLI and configured Python
dependencies from the repository root. This is an operator check taking minutes
or longer; require exit code zero and no failed variants:

```powershell
python ./test/functional/test_runner.py --jobs=1 --extended '--filter=^(wallet|rpc|p2p)_paymaster' --exclude=p2p_paymaster_v9_26_5_bridge.py
if ($LASTEXITCODE -ne 0) { throw 'Paymaster functional matrix failed' }
```

Example focused functional run from repository root with built daemons and
configured Python dependencies:

```sh
python test/functional/test_runner.py --jobs=1 wallet_paymaster_pool_setup.py wallet_paymaster_partial_capacity.py wallet_paymaster_readiness.py
```

Use the registered variants in `test_runner.py`; descriptor/legacy coverage
is intentional. Do not run every test under an arbitrary wallet flag. The
failover scenario uses four daemons/two providers; reorg scenarios must verify
actual transaction confirmation and reversal, not only mocked status.

### RPC startup failures

If a variant fails in `wait_for_rpc_connection` before its scenarios run, inspect
that variant's `nodeN/regtest/debug.log` and `nodeN/stderr` first. The default
framework RPC client uses IPv4 loopback. A node can finish startup with only its
IPv6 RPC listener if binding `127.0.0.1` failed; `Done loading` alone therefore
does not prove the test client can connect. Increasing the timeout cannot fix a
failed bind. On Windows, inspect the port printed in the failed bind message:

```powershell
Get-NetTCPConnection -LocalPort 20072 -ErrorAction SilentlyContinue
netsh interface ipv4 show excludedportrange protocol=tcp
```

These are read-only diagnostics; do not terminate unrelated nodes or change OS
port reservations. For a standalone retry, keep the reported `--randomseed`
and choose an unused `--portseed` with a fresh `--tmpdir`. `--portseed` selects
test ports independently of the scenario's random seed. A later successful run
does not establish what held or blocked the port during the failed run.

## Historical compatibility

Follow [test/README.md](../test/README.md) to obtain the official v9.26.5 daemon.
`V9_26_5_DIGIBYTED` selects it; `PRE_PAYMASTER_DIGIBYTED` is a compatibility
fallback. It is an old-version interoperability input, not the current baseline.
The selection below expands registered variants for v9.26.5 and v9.26.6rc2,
including Thaw Day. Supply both official reference binaries; skipped variants do
not establish compatibility.

```sh
V9_26_5_DIGIBYTED=/path/to/v9.26.5/bin/digibyted \
V9_26_6RC2_DIGIBYTED=/path/to/v9.26.6rc2/bin/digibyted \
  python test/functional/test_runner.py --jobs=1 wallet_v9_26_5_compatibility.py wallet_v9_26_5_inplace_upgrade.py p2p_paymaster_v9_26_5_bridge.py
```

Cover ordinary DGB/DD relay and receiving-version confirmation, unchanged-datadir
upgrade, wallet/history continuity, and fail-closed Paymaster discovery through
an old bridge. A direct current-current link should restore discovery.

## Security and acceptance checks

Functional manual-provider fixtures may wait for the exact pre-execution
`PAYMASTER_PROVIDER_BUSY` / `PAYMASTER_PROVIDER_SERVICE_BUSY` guards. The quote
harness permits at most two explicit same-request transport retries after the
real-time localhost start budget refills; financial/protocol errors propagate.
In the provider lifecycle fixture, guarded configuration, pool and start/stop
calls use `provider_rpc`, including calls expected to fail. Put the idle helper
inside `assert_raises_rpc_error`: otherwise an early busy response is converted
to an assertion failure before it can be retried. The running-policy regression
injects one busy response before checking the live RPC's `PROVIDER_RUNNING`
rejection. Shared pool-confirmation retries use the same idle guard.
Offer selection checks zero fee reservation before client acceptance and the
exact reserved fee afterwards. A second unsigned provider choice needs its own
durable request, no inherited authorization, and enough remaining DD funds.

`checkpoint_readers_do_not_block_atomic_replacement` runs a read-only stale
snapshot concurrently with repeated live checkpoint updates, using separate mock
row maps to avoid a fixture data race. Every replacement must succeed and the
stale snapshot must remain rejected. This is concurrency regression coverage;
the old implementation may also pass a run that misses the Windows handle race.

Database-failure fixtures must distinguish wallet transaction rollback from
the independent provider checkpoint. A rejected row write leaves both unchanged;
a rejected DB commit can leave the external checkpoint ahead, requiring review
instead of an immediate retry. Capacity-expiry and provider-quote tests cover
successful completion and commit-failure quarantine in separate fixtures.
Inject malformed copied records through the mock database, without performing a
legitimate provider mutation that advances the original identity's checkpoint.
Client-only fixtures retain the remote provider key in their artifacts, rather
than enrolling that identity as the client's local provider.

[Paymaster Security CI](../.github/workflows/paymaster-security.yml) is the
executable reference for architecture, sanitizer and fuzz checks. Functional
tests use the [runner selection above](#functional-selection). Run the workflow's
current configuration rather than copying stale package lists. Inspect code
analysis/dependency workflows as complementary checks, not proof of correctness.

Acceptance must include locked/encrypted wallets, restart, history/index waits,
response loss, safe unsigned cancellation, signed ambiguity, fee-limit changes,
whole-slot low-funds refill for both assets, no-free-capacity discovery, restored
backups and independent checkpoint mismatch. Verify CLI/headless operation as
well as GUI behavior and ordinary sends with Paymaster disabled.

Real Tor reachability/24-hour operation, hostile transport/load, physical
storage failure and full current-revision cross-platform builds are separate
gates. Simulated DB failure and real SQLite page caps do not simulate every
fsync/power-loss failure. See [threat model](paymaster-threat-model.md).

## Documentation-only checks

Check relative links/anchors, referenced RPC/test/source names, current branch
and version references, and `git diff --check`. Parse JSON/PowerShell examples,
check configuration flags against registration, and reject hidden control
characters in copied commands (valid Markdown alone is insufficient). Documentation maintenance does
not require claiming a product-build rerun; report exactly which checks ran.
