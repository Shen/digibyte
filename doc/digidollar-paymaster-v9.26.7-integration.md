# Paymaster integration with DigiByte v9.26.7

## Source checkpoint (2026-10-06)

Branch: `integration/paymaster-v9.26.7`.
Merge: `e5cfa2da3e42dcc5aa1ee316771da10fafad0960`.

| Role | Commit |
| --- | --- |
| Preserved Paymaster source | `2f44ac43c24f99bff7a7388dc6d01d00eef52bcf` |
| Official `release/v9.26.7` | `d7265fb05e26e1e799876bcd526225e4826c3ba8` |
| Common ancestor | `92330d952625e20aef2ee40671a179ef03872ac1` |

The merge incorporates all 13 official commits since the common ancestor.
The preceding `integration/paymaster-v9.26.6rc2` branch remains at its original
tip. This is source integration with targeted checks, not release acceptance.
The official [v9.26.7 release document](../RELEASE_v9.26.7.md) is unchanged;
its upstream test results do not certify this fork.

## Official behavior and fork adaptations

Official product changes take precedence. The integrated source retains:

- Tip-specific `getblockchaininfo` / `getchainstates` difficulty without a
  scalar-field scan through retired Groestl history. Every `GetDifficulty`
  caller supplies the explicit arguments required by the official signature.
- Unused Qt DD receive addresses and labels in `listdigidollaraddresses` when
  `include_empty` is true. Ordinary DGB addresses, foreign contacts and legacy
  watch-only entries remain excluded from that addition.
- The Send DD minimum-change tooltip and the overview's build-derived version
  artwork, including its blank-background resource.
- Statistics-index shutdown handling: queued block/reorg supply checks finish,
  while startup verification remains interruptible.
- The official block-map, functional-test, CI-log and Mac-package-check changes.

Git merged the source without text conflicts. The official product/non-Qt-test
patch also passes a read-only reverse-application check against the resulting
tree, proving its hunks remain present. The existing MSVC `util::int128_t`
adaptation retains the upstream arithmetic expressions in the stats index.
Paymaster's DD reservation exclusions, funded transfer planning, fee approvals,
session ownership validation, background Send/Receive snapshots and provider
interface remain intact. No new consensus, Paymaster wire, record-format or
financial authorization change is added.

Only small fork-specific adaptations were needed:

- Regenerate the tracked Windows config header from `configure.ac`, setting the
  build and package version to 9.26.7.
- Expose `DigiDollarTest::SyncUpWallet` from the shared test helpers so the new
  official legacy watch-only regression can use its original rescan setup.
- Finish RPC warmup in both new address tests when they run in isolation. Their
  original assertions are retained. The isolated run first reproduced a
  warmup error; this test setup change does not alter the production RPC server.

## Completed verification

Relevant guidance: repository/src instructions, CLAUDE's DigiDollar reading
order, architecture/maps, contribution and developer GUI/locking rules, Qt
translation policy, Windows builds, functional tests and release workflow.

Selected MSVC 14.43 / static Qt 5.15.10 compiles covered 12 translation units:
three node product files, Send DD, overview branding, the regenerated resource,
the Qt fixture/helper/MOC and three Core test files. Separate incremental Core
and Qt test executables were linked, reusing unchanged objects and libraries.
The normal application, daemon and CLI executables were not linked or installed
for this checkpoint. A full build is still required.

**26 targeted cases passed; none failed or skipped:** six Core/RPC cases and
20 Qt cases across ten selected Qt functions. They cover the official difficulty
regression, both queued-index shutdown cases, pooled block-map storage, standard
Paymaster locks and unsigned cancellation, both new address RPC regressions,
busy-wallet Send/Receive navigation, wallet-bound balance replies, receive
selection, direct-send input validation, cleared new-transfer offers, inbox
failure handling and validated terminal reservation owners. In the controlled
busy-wallet run, Send DD opened in 6 ms and Receive DD in 9 ms with the wallet
lock still held. These are regression results, not live-wallet benchmarks.

Python syntax checks for both incoming functional tests and `git diff --check`
also passed. Helpers, test logs and isolated test binaries are local build
artifacts under `build_msvc/paymaster-refresh-check/v9.26.7/` and
`build_msvc/x64/Release/test_digibyte[-qt]-v9.26.7.exe`.

Full builds, complete suites, functional execution and cross-platform checks
have not run at this checkpoint. Earlier broad Qt dropdown-highlight failures
were not retested here. Historical reference-binary tests do not certify the
new release baseline.

## Operator build and acceptance

Working directory: `D:\Digibyte\digibyte-fork`. Prerequisites are the existing
MSVC v143 14.43 tools, static Qt 5.15.10 at `D:\Qt51510\install`, vcpkg static
dependencies and Python. Close wallets/daemons normally and stop automatic CLI
polling before linking the normal executables; an open EXE can produce LNK1104.
Allow minutes to tens of minutes for the solution build, depending on cached
objects, and minutes or longer for complete suites. No forced shutdown is needed.

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$ErrorActionPreference = 'Stop'
git branch --show-current
python .\build_msvc\msvc-autogen.py
if ($LASTEXITCODE -ne 0) { throw 'Project generation failed' }
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\build_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:VCToolsVersion=14.43.34808 /p:QtBaseDir=D:\Qt51510\install /p:VcpkgInstalledDir=D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/ /p:VcpkgManifestInstall=false /m:1 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'v9.26.7 integration build failed' }
& .\build_msvc\x64\Release\digibyted.exe --version
& .\build_msvc\x64\Release\digibyte-cli.exe --version
```

Require the intended branch, build exit code zero and version 9.26.7. After that
build, run the normal binaries' complete unit and affected Qt suites:

```powershell
& .\build_msvc\x64\Release\test_digibyte.exe
if ($LASTEXITCODE -ne 0) { throw 'Core unit tests failed' }
$env:QT_QPA_PLATFORM = 'windows'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
foreach ($suite in @('DigiDollarWidgetTests', 'PaymasterWidgetTests')) {
    $env:DIGIBYTE_QT_TEST_SUITE = $suite
    $env:DIGIBYTE_QT_TEST_OUTPUT = "$PWD\build_msvc\x64\Release\v9.26.7-$suite.txt"
    & .\build_msvc\x64\Release\test_digibyte-qt.exe
    if ($LASTEXITCODE -ne 0) { throw "$suite failed" }
}
Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
```

Run the affected lifecycle/RPC/failover/reorg and official Thaw Day functional
checks sequentially with the freshly built Windows binaries (minutes or longer):

```powershell
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:DIGIBYTED = "$PWD\build_msvc\x64\Release\digibyted.exe"
$env:DIGIBYTECLI = "$PWD\build_msvc\x64\Release\digibyte-cli.exe"
$env:DIGIBYTEUTIL = "$PWD\build_msvc\x64\Release\digibyte-util.exe"
$env:DIGIBYTEWALLET = "$PWD\build_msvc\x64\Release\digibyte-wallet.exe"
python .\test\functional\test_runner.py wallet_paymaster_rpc.py wallet_paymaster_lifecycle.py wallet_paymaster_failover.py wallet_paymaster_reorg.py digidollar_thawday_height.py digidollar_thawday_node_matrix.py -j1
if ($LASTEXITCODE -ne 0) { throw 'v9.26.7 integration functional checks failed' }
```

Require exit codes zero and no failed cases; report skips separately. Retain
complete logs and return the failing output for review. In a test wallet, also
check version branding and the changed tooltip in both themes, unused DD receive
addresses and labels after restart, Send/Receive responsiveness during Paymaster
work, an ordinary direct send, exact Paymaster approvals and durable recovery.
Package signature/launch, wider functional coverage and Linux/macOS acceptance
remain separate release gates.
