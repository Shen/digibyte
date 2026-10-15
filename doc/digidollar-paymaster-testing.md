# Paymaster build and test runbook

This is a reproducible verification index, not a running work log. The
[release gate](digidollar-paymaster-release-gate.md) separates historical evidence
from checks still required for a candidate. Full pre-consolidation command/log
history is available with `git show e7c2c8c4bd:doc/digidollar-paymaster-testing.md`.

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

Windows: follow [MSVC instructions](../build_msvc/README.md). From the repository
root in the configured MSVC environment, regenerate projects before a normal
Release build:

```powershell
python .uild_msvc\msvc-autogen.py
if ($LASTEXITCODE -ne 0) { throw 'Project generation failed' }
msbuild .uild_msvc\digibyte.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m:1
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

Supply installation-specific Qt/vcpkg properties as documented by the build
setup. The recorded local environment used MSVC 14.43 and static Qt 5.15.10;
this is evidence provenance, not a new universal prerequisite. Stop processes
using output executables before replacing them. A full build can be lengthy;
verify exit status and linked app/daemon/CLI/tests, not just compilation lines.

## Unit and Qt selection

In a configured Unix build:

```sh
./src/test/test_digibyte '--run_test=paymaster_*' --report_level=short
```

The prefix includes split wallet suites. Windows equivalent:

```powershell
& .uild_msvcd\Release	est_digibyte.exe '--run_test=paymaster_*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster unit tests failed' }
```

Qt uses `DIGIBYTE_QT_TEST_SUITE` and optional `DIGIBYTE_QT_TEST_FUNCTION`
filters implemented by `src/qt/test/test_main.cpp`. Select `PaymasterWidgetTests`
and relevant `DigiDollarWidgetTests` cases from the current fixture. Clear filters
before claiming a complete Qt run. A targeted result including init/cleanup
is not that many independent feature cases.

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
and version references, and `git diff --check`. Documentation maintenance does
not require claiming a product-build rerun; report exactly which checks ran.
