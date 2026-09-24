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

After a successful build, run from the repository root in PowerShell. The MSVC
projects copy application/test executables to `src`; use those freshly copied
files consistently. An interactive desktop is required for the Windows Qt run.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
.\src\test_digibyte.exe '--run_test=paymaster_*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster unit tests failed' }

$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION, Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
try {
    .\src\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw 'Paymaster Qt tests failed' }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
}

$env:PYTHONUTF8 = '1'
python test/functional/test_runner.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_pool_setup.py wallet_paymaster_lifecycle.py wallet_paymaster_offer_selection.py wallet_paymaster_failover.py wallet_paymaster_reorg.py digidollar_rpc_amount_units.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster functional tests failed' }

Get-FileHash .\src\digibyted.exe, .\src\digibyte-cli.exe, .\src\test_digibyte.exe, .\src\test_digibyte-qt.exe -Algorithm SHA256
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
