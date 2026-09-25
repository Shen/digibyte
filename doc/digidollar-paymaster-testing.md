# Paymaster build and test runbook

Reviewed against `a4f17f6315` on `integration/paymaster-v9.26.6rc2`, 2026-09-23.
This runbook describes operator commands, not a newly completed build or test
run. The [release gate](digidollar-paymaster-release-gate.md) owns acceptance;
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

## Windows runtime checks

After a successful build, run from the repository root in PowerShell. The node and CLI are copied to `src`; the Qt test executable is under
`build_msvc/x64/Release`. Use freshly built files consistently. An interactive desktop is required for the Windows Qt run.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\src\test_digibyte.exe '--run_test=paymaster_*' --report_level=short
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
python test/functional/test_runner.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py wallet_paymaster_reorg.py digidollar_rpc_amount_units.py -j1
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
./src/test/test_digibyte --run_test='paymaster_*' --report_level=short
env -u DIGIBYTE_QT_TEST_FUNCTION -u DIGIBYTE_QT_TEST_OUTPUT QT_QPA_PLATFORM=offscreen DIGIBYTE_QT_TEST_SUITE=PaymasterWidgetTests ./src/qt/test/test_digibyte-qt
python3 test/functional/test_runner.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py wallet_paymaster_reorg.py digidollar_rpc_amount_units.py -j1
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
