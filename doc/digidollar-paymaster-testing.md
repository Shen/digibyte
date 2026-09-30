# Paymaster build and test runbook

## Guided provider tasks: current acceptance (2026-09-29)

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
vcpkg installation. No new source-list generation is needed for this change.
Allow several minutes for the incremental solution build; require exit code 0:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
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
  read-only status. Restricted sponsorship must stay restricted when editing
  unrelated offer amounts, including a mixed customer-paid/restricted offer.
- Verify the funding monitor reports changes immediately and a heartbeat every
  ten seconds while polling Core every two seconds, without flooding the screen.
- Distinguish a one-time start request from saved autostart. Continuous operating
  access still requires manual password entry after a node restart.
