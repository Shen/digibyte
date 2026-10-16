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
| Selection, fee limits, model binding, failover | Directory/client/reputation suites; `wallet_paymaster_offer_selection.py`, `wallet_paymaster_failover.py`. |
| Explicit authorization and same-ID retry | Wallet store/security suites; `wallet_paymaster_rpc.py`, `wallet_paymaster_provider.py`. |
| Signed recovery and budget retention | Recovery/reservation/store suites; `wallet_paymaster_reorg.py`, `wallet_paymaster_lifecycle.py`. |
| Finite setup, whole-slot refill, below-target readiness | Setup/identity/load/store suites; `wallet_paymaster_pool_setup.py`, `wallet_paymaster_partial_capacity.py`, `wallet_paymaster_readiness.py`, `wallet_paymaster_operator.py`. |
| Restore/adoption/retirement and stale copies | `wallet_paymaster_backup.py`, `wallet_paymaster_backup_adoption.py`, `wallet_paymaster_backup_exit.py`, `wallet_paymaster_checkpoint_rollback.py`. |
| Selected uncreated funding cancellation | `paymaster_wallet_identity_tests/maintenance_cancellation*`: DGB/DD setup/refill, exact selection/preview, lost-reply signature evidence, write rollback, commit-failure checkpoint protection, provider-work exclusion and retained signed work. Qt `paymasterMaintenanceCancellation`, `paymasterSavedTaskTitles`: review/decline, wallet/privacy switches, malformed receipts, unrelated old titles and zero-capacity cancellation. |
| Excess release with missing reserves | `paymaster_wallet_identity_tests/rebalance_saved_targets_*`: empty/mixed DGB/DD pools, saved revision/target binding, previously released DD slots/model changes, no-op execution and retained reservation guards. Qt `paymasterGuidedCapitalTasks`, `paymasterReserveReduction`: DGB-only scope, exact amount/fee receipts and no-excess feedback. |
| Manual provider signing approval | Qt `paymasterManualSubmitConfirmation`: approve/decline and wallet, privacy, stale-snapshot or paused-runtime changes during review. Core authorization and signing checks remain authoritative. |
| Stopped service versus restart readiness | Qt `paymasterStoppedOverview`: missing/retained reserves, budgets, locked wallets, saved payment models, open payments, unknown/restore/checkpoint errors and absent/contradictory stop observations. Known prerequisites permit a reviewed one-shot start request, not payment readiness; missing settings/policy and unknown/restore/checkpoint errors do not. Pending requests disable repeat starts and remain cancellable. Rendering never dispatches a mutation. `paymasterOperatorConfirmationWalletBinding` checks the exact enable/start-with-readiness-wait sequence on both GUI surfaces, including budget/liquidity waits, cancellation and wallet/privacy changes. |
| Operation settings and retirement display | Qt `paymasterFiveDestinationsAndVisibleTasks`, `paymasterConnectionLayout`: first Settings destination, Funds link, back navigation, light/dark and narrow/wide layouts. `paymasterHomeStartIsSeparateFromSettings` and `paymasterOperatorConfirmationWalletBinding`: shared Start/Pause guards, approval/cancellation, wallet/privacy changes. `paymasterStopAndRelease`, `paymasterRetirementLateResults`: stopped state after successful, empty or incomplete retirement, exact-release review and stale-result protection. These GUI fixtures mock RPC responses; they do not replace Core release tests. |
| Cross-policy fee configuration | `paymaster_setup_tests`: model-specific fee conflicts, both values, inactive budgets, malformed amounts and read-only CLI status. Qt `paymasterConfigurationConflicts`: saved versus draft warnings, navigation, zero/matching/above-ceiling caps, mixed models and privacy clearing. `paymasterSafetyInputValidation`: inline amount/limit errors and no RPC for invalid edits. `paymasterConfigurationSaveRefresh`: immediate combined readback, delayed/failed/incomplete reads, rejected saves, exact acknowledgements, retained edits, privacy and wallet changes. |
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
