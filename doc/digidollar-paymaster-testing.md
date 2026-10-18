# Paymaster build and test runbook

This is a reproducible verification index, not a running work log. The
[release gate](digidollar-paymaster-release-gate.md) separates historical evidence
from checks still required for a candidate. Full pre-consolidation command/log
history is available with `git show e7c2c8c4bd:doc/digidollar-paymaster-testing.md`.

Operational documentation primarily targets Mainnet. Run the tests here in
isolated test environments, not with a Mainnet wallet. The
[Testnet/Regtest supplement](digidollar-paymaster-test-networks.md) provides
manual test configurations; a test pass does not establish public Mainnet
reachability or replace the candidate's release gates.

## Preparation and result recording

Read repository instructions, [test guidance](../test/README.md),
[functional guidance](../test/functional/README.md) and applicable build docs.
Use a clean test data directory, never an operator wallet. Record:

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
    './build_msvc/digibyte.sln', '/t:Build',
    '/p:Configuration=Release', '/p:Platform=x64',
    "/p:QtBaseDir=$qtBase", "/p:VcpkgInstalledDir=$vcpkgInstalledDir",
    '/p:VcpkgManifestInstall=false', '/m:1', '/verbosity:minimal'
)
& $msbuildPath @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
```

If dependencies are absent, complete the normal prerequisite installation first;
do not treat this block as a toolchain installer. The recorded local checks used
MSVC 14.43 and static Qt 5.15.10. That is provenance, not a claim that an arbitrary
Visual Studio/Qt combination is compatible. Preserve the project's selected
toolset and consult build guidance for changes.

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

Run from the repository root after a successful current-revision build with
tests enabled. Allow several minutes or longer; some fixtures build chains.
Success requires exit code zero and no failed tests. This suite still does not
replace the functional matrix, ordinary-wallet regressions or release gates.

## Coverage map

| Invariant / behavior | Main test locations |
|---|---|
| Amounts, exact fees, manifests and signatures | `src/test/paymaster_types_tests.cpp`, `paymaster_txbuilder_tests.cpp`, `paymaster_psbt_tests.cpp`; wallet PSBT/security tests. |
| Wire/channel proof and bounded admission | Protocol, wire, transport, admission unit suites; `p2p_paymaster.py`, `p2p_paymaster_connection_capacity.py`. |
| Nonfinancial endpoint diagnostics | `paymaster_transport_tests/diagnostic_*` and `payment_without_selected_identity_is_not_a_diagnostic`: isolated keys, negotiation without authentication, suppressed metadata and cleanup. `rpc_paymaster_endpoint.py`: wallet-free TCP listener, rate limit, invalid/closed endpoint, Tor prerequisites, SOCKS5 forwarding, credential isolation and no-auth rejection. Uses a local proxy fixture, not public Tor. `wallet_paymaster_operator.py` retains the operator probe regression. Qt `paymasterOperatorNavigationAndVisibleTasks` checks RPC guidance and absence of the removed report-import control. `paymasterDiagnosticCommands` and `paymasterDiagnosticRefreshAndPrivacy` cover exact copyable IPv4/IPv6/Tor commands, network selection, loopback restrictions, malformed input, missing announcements, read failures, refresh and privacy/wallet clearing; they never run a probe. `paymasterConnectionLayout` includes the connection-test view with a long onion address in both themes and sizes. `paymasterNodeConnectionEditor` covers grouped addresses/features/capacity, readable mixed sources, pending restart values, incomplete metadata, read-only layouts at narrow/wide sizes, reviewed saves and stale/privacy guards. |
| Selection, fee limits, model binding, failover | Directory/client/reputation suites; `wallet_paymaster_offer_selection.py`, `wallet_paymaster_failover.py`. |
| Explicit authorization and same-ID retry | Wallet store/security suites; `wallet_paymaster_rpc.py`, `wallet_paymaster_provider.py`. |
| Signed recovery and budget retention | Recovery/reservation/store suites; `wallet_paymaster_reorg.py`, `wallet_paymaster_lifecycle.py`. `wallet_paymaster_provider.py` retains the original authorization at 1 and 239 recovery confirmations and releases it at the 240-confirmation safety depth without charging the service fee twice. |
| Finite setup, whole-slot refill, below-target readiness | Setup/identity/load/store suites; `wallet_paymaster_pool_setup.py`, `wallet_paymaster_partial_capacity.py`, `wallet_paymaster_readiness.py`, `wallet_paymaster_operator.py`. |
| Maintenance time and confirmation sequencing | `paymaster_wallet_identity_tests/maintenance_reconciliation_preserves_accounting_time` covers future policy accounting time, clock rollback, lost-reply recovery, confirmation and conflict transitions. `wallet_paymaster_pool_setup.py` checks completed maintenance accounting and capital release. `wallet_paymaster_lifecycle.py` confirms carrier and DGB batches separately across restart and keeps existing capacity active during refill. `wallet_paymaster_provider.py` confirms a payment successor before expecting the next DGB replacement, with no duplicate maintenance or carrier. |
| Preparation funding diagnostics | `wallet_paymaster_pool_setup.py --funding-diagnostics-only`: matching pool/operator DD and DGB capital snapshots, incoming confirmed funds reflected before another attempt, no status-read transactions and no new requirements for cancelled steps. The full test also checks the signed-step snapshot. Qt `paymasterPoolPreparationDiagnostics`: simultaneous DD/DGB shortfalls, pending confirmations, funded capital with fee checks remaining, missing/malformed observations, task/overview consistency, reserve navigation, two themes/sizes and privacy. `paymasterPoolPreparationStoredFeeDiagnostics` reads real wallet records without changing the saved approval or error. |
| Restore/adoption/retirement and stale copies | `wallet_paymaster_backup.py`, `wallet_paymaster_backup_adoption.py`, `wallet_paymaster_backup_exit.py`, `wallet_paymaster_checkpoint_rollback.py`. |
| Selected uncreated funding cancellation | `paymaster_wallet_identity_tests/maintenance_cancellation*`: DGB/DD setup/refill, exact selection/preview, lost-reply signature evidence, write rollback, commit-failure checkpoint protection, provider-work exclusion and retained signed work. Qt `paymasterMaintenanceCancellation`, `paymasterSavedTaskTitles`: review/decline, wallet/privacy switches, malformed receipts, unrelated old titles and zero-capacity cancellation. |
| Excess release with missing reserves | `paymaster_wallet_identity_tests/rebalance_saved_targets_*`: empty/mixed DGB/DD pools, saved revision/target binding, previously released DD slots/model changes, no-op execution and retained reservation guards. Qt `paymasterGuidedCapitalTasks`, `paymasterReserveReduction`: DGB-only scope, exact amount/fee receipts and no-excess feedback. |
| Manual provider signing approval | Qt `paymasterManualSubmitConfirmation`: approve/decline and wallet, privacy, stale-snapshot or paused-runtime changes during review. Core authorization and signing checks remain authoritative. |
| Stopped service versus restart readiness | Qt `paymasterStoppedOverview`: missing/retained reserves, budgets, locked wallets, saved payment models, open payments, unknown/restore/checkpoint errors and absent/contradictory stop observations. Known prerequisites permit a reviewed one-shot start request, not payment readiness; missing settings/policy and unknown/restore/checkpoint errors do not. Pending requests disable repeat starts and remain cancellable. Rendering never dispatches a mutation. `paymasterOperatorConfirmationWalletBinding` checks the exact enable/start-with-readiness-wait sequence on both GUI surfaces, including budget/liquidity waits, cancellation and wallet/privacy changes. |
| Operation settings and retirement display | Qt `paymasterOperatorNavigationAndVisibleTasks`, `paymasterConnectionLayout`: four main destinations, seven settings destinations and the nested connection-test view, lifecycle first, nested reserve links, back navigation, light/dark and narrow/wide layouts. `paymasterReserveSettingsRefresh`: periodic reserve observations preserve fee drafts after the move into Settings, cross-links remain read-only and privacy stops refresh. `paymasterHomeStartIsSeparateFromSettings` and `paymasterOperatorConfirmationWalletBinding`: shared Start/Pause guards, approval/cancellation, wallet/privacy changes. `paymasterStopAndRelease`, `paymasterRetirementLateResults`: stopped state after successful, empty or incomplete retirement, exact-release review and stale-result protection. These GUI fixtures mock RPC responses; they do not replace Core release tests. |
| Cross-policy fee configuration | `paymaster_setup_tests`: model-specific fee conflicts, both values, inactive budgets, malformed amounts and read-only CLI status. Qt `paymasterConfigurationConflicts`: saved versus draft warnings, navigation, zero/matching/above-ceiling caps, mixed models and privacy clearing. `paymasterSafetyInputValidation`: inline amount/limit errors and no RPC for invalid edits. `paymasterConfigurationSaveRefresh`: immediate combined readback, delayed/failed/incomplete reads, rejected saves, exact acknowledgements, retained edits, privacy and wallet changes. |
| Setup parity and funding guidance | `paymaster_setup_tests`: streamed capacity selection preserving approvals, required/available/missing amounts, pending funds, fee ceilings, malformed observations and signed/terminal steps. Qt `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep`: editable/persisted autostart, timer/manual funding refresh, missing amounts, confirmations, failed-read clearing, readiness transition and no extra mutation on refresh. |
| GUI cancellation, affordability, async snapshots, setup | `src/qt/test/paymasterwidgettests.cpp`, DigiDollar/overview Qt fixtures. |
| Ordinary behavior and historical migration | DGB/DD wallet tests plus the historical compatibility group below. |

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

```sh
V9_26_5_DIGIBYTED=/path/to/v9.26.5/bin/digibyted \
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
executable reference for sanitizer, fuzz and functional selections. Run its
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
