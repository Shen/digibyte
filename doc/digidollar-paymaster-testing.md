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
- Exact executable provenance and SHA256, command, selected cases, random/port
  seeds and exit status.
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

### Linux CI profiles and separate evidence

Use the existing [CI runner](../ci/README.md) from a clean candidate checkout in
a disposable Linux VM or Docker-capable Linux/WSL environment. Prerequisites are
`bash`, `python3`, a running Docker daemon accessible to the operator, network
access for the declared images/packages/dependencies, and enough memory/disk for
full builds. Verify the host with `bash --version`, `python3 --version`,
`docker version` and `docker info`; install missing prerequisites using the CI
guide before starting. Keep `DANGER_RUN_CI_ON_HOST` unset. The runner installs
packages, uses named caches and performs setup changes inside its disposable
environment. Each profile can take tens of minutes to hours, including downloads;
these are operator/CI jobs, separate from targeted local tests.

Select one profile below, run from the repository root, and retain its complete
console output and exit status. Repeat separately for all required profiles:

```bash
profile=native_qt5  # Also run native_tidy, native_asan and native_tsan separately.
log="../paymaster-${profile}-$(git rev-parse --short=12 HEAD)-$(date -u +%Y%m%dT%H%M%SZ).log"
set -o pipefail
env -i HOME="$HOME" PATH="$PATH" USER="$USER" MAKEJOBS="-j1" \
    FILE_ENV="./ci/test/00_setup_env_${profile}.sh" \
    bash ./ci/test_run_all.sh 2>&1 | tee "$log"
ci_status=$?
printf 'CI exit status: %s\n' "$ci_status" | tee -a "$log"
test "$ci_status" -eq 0
```

Use the versioned profiles as the source for exact package/configuration choices;
do not copy their package lists into another setup. Record the selected commit,
profile, compiler version, actual language mode, configuration, test selections
and any source patches applied by the stage.
Preserve build/test and sanitizer logs as CI artifacts before container teardown.
An installation or configure failure leaves the later checks unexecuted.

| Evidence | Existing profile and acceptance scope |
|---|---|
| Linux/C++17 | [`native_qt5`](../ci/test/00_setup_env_native_qt5.sh) uses Ubuntu 20.04, GCC 9, system Qt5 and depends. `configure.ac` selects C++17 by default; verify that mode in the compiler/configure output. This profile builds Qt, runs native unit tests sequentially and runs extended functional/previous-release tests. Its sequential path does **not** execute the Qt test binary; record Qt execution separately. |
| Tidy with repository plugin | [`native_tidy`](../ci/test/00_setup_env_native_tidy.sh) uses LLVM/Clang 17, Bear and the repository's `.clang-tidy`. Require a current compilation database, successful `digibyte-tidy` and `digibyte-tidy-tests` targets, and analysis loading `/tidy-build/libdigibyte-tidy.so`. The stage also runs IWYU; it disables unit/functional/fuzz execution. An unavailable plugin or a parser/configuration error is not a clean analysis result. |
| ASan/UBSan with Qt | [`native_asan`](../ci/test/00_setup_env_native_asan.sh) uses Clang 17, Qt5 and **C++20**, with address, float-divide-by-zero, integer and undefined sanitizers. Its `make check` includes the Qt test target when enabled; require the actual Qt log, plus the native and functional results. The local container profile requests `SYS_PTRACE` for sanitizer operation. This run does not establish C++17 compatibility. |
| Thread sanitizer | [`native_tsan`](../ci/test/00_setup_env_native_tsan.sh) uses Clang 17/libc++, depends and `--with-sanitizers=thread`, separately from ASan. Require completed native/Qt/functional selections and inspect the TSAN output directory; a build alone is not concurrency evidence. |

The [stage script](../ci/test/06_script_b.sh) supplies sanitizer options and the
versioned suppression files. Keep them in the evidence; do not add suppressions,
disable checks or reinterpret a nonzero stage result to obtain a pass. A filtered
Tidy console excerpt does not replace the tool's exit status and complete
diagnostics. The [Paymaster Security workflow](../.github/workflows/paymaster-security.yml)
has separate headless sanitizer-unit/fuzz jobs with GUI and daemon disabled;
those jobs do not satisfy the ASan/Qt or functional requirements above.

Inside the configured Tidy environment, verify the loaded check explicitly:

```bash
clang-tidy-17 --load=/tidy-build/libdigibyte-tidy.so \
    --checks='-*,digibyte-unterminated-logprintf' --list-checks
```

The output must include `digibyte-unterminated-logprintf`. Use the real build's
compilation database; previous MSVC/STL parser diagnostics need reassessment
under that supported configuration, not new blanket suppressions.

For separate Qt execution from a successfully configured Linux build directory,
use its freshly built binary and the [selection rules below](#unit-and-qt-selection).
For an instrumented build retain that profile's sanitizer environment and
suppression paths. This example runs the complete Paymaster suite with the Linux
headless platform; native-display and ordinary-wallet Qt checks remain separate:

```bash
env -u DIGIBYTE_QT_TEST_FUNCTION -u DIGIBYTE_QT_TEST_OUTPUT \
    QT_QPA_PLATFORM=minimal QT_STYLE_OVERRIDE=Fusion \
    DIGIBYTE_QT_TEST_SUITE=PaymasterWidgetTests \
    ./src/qt/test/test_digibyte-qt
```

Require exit zero, no failed cases and an explanation for skips. Clear the suite
filter as well when recording a complete Qt run. Keep every profile's result
separate; Windows/MSVC results do not replace Linux/C++17, Tidy-plugin or
sanitizer execution.

### Windows build from PowerShell

Follow the [MSVC prerequisites](../build_msvc/README.md), including the static
Qt build/patch and vcpkg dependencies. Run from the repository root of a fresh
checkout of the candidate commit. Stop
processes using the executables that will be replaced. A normal build can take
minutes to hours depending on how much is already built; use its exit code and
final error summary, not the existence of an old executable, as the result.

`msbuild` is often absent from an ordinary PowerShell PATH even when Visual
Studio is installed. The following discovers the installed executable. First
set these **installation-specific example paths** to your actual Qt prefix and
vcpkg installed root (the directory containing `x64-windows-static/lib`). A
nested local vcpkg layout may require a different root than this example.

```powershell
$qtBase = 'D:/QT51510/install'
$vcpkgInstalledDir = 'D:/Digibyte/digibyte-fork/build_msvc/vcpkg_installed/x64-windows-static/'
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
    '/p:PlatformToolset=v143', '/p:VCToolsVersion=14.43.34808',
    "/p:QtBaseDir=$qtBase", "/p:VcpkgInstalledDir=$vcpkgInstalledDir",
    '/p:VcpkgManifestInstall=false', '/m:1', '/verbosity:minimal'
)
& $msbuildPath @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
```

These paths identify the locally verified static Qt 5.15.10 and nested vcpkg
installation; the candidate Windows profile uses v143/MSVC 14.43.34808. The
fresh source checkout may reuse those dependencies. Verify the requested toolset
is installed before running; do not silently substitute another version.

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

Use a short isolated `TEMP`/`TMP` for Windows checkpoint tests. The fixture
appends a random 64-character directory and checkpoint filenames; deeply nested
artifact paths can fail in `BasicTestingSetup`'s ordinary filesystem cleanup.
The fixture destructor can then terminate the process, hiding the test result.
Keep the original assertions and record the failed long-path run separately.
For example, from this workspace after building the selected test executable:

```powershell
$checkpointTemp = 'D:\Digibyte\digibyte-fork\.ai\ctmp'
New-Item -ItemType Directory -Force -Path $checkpointTemp | Out-Null
$savedTemp, $savedTmp = $env:TEMP, $env:TMP
try {
    $env:TEMP = $env:TMP = $checkpointTemp
    & ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_wallet_identity_tests/checkpoint_readers_do_not_block_atomic_replacement,checkpoint_independent_identities_progress_while_another_write_is_paused' --report_level=detailed
    if ($LASTEXITCODE -ne 0) { throw 'Checkpoint tests failed' }
} finally {
    $env:TEMP, $env:TMP = $savedTemp, $savedTmp
}
```

For inbound classification, persisted client state and pending-balance regressions,
use the freshly built test executable from the repository root. This focused selection normally takes
seconds to a few minutes; success requires exit zero and no failed or unexpectedly
skipped selected cases:

```powershell
$focusedCases = @(
    'paymaster_recovery_tests',
    'paymaster_wallet_identity_tests/unknown_recovery_submit_is_consumed_before_normal_work,stale_submit_is_consumed_without_faulting_provider,automatic_submit_defers_before_wallet_or_index_wait',
    'paymaster_wallet_security_tests/normal_submit_classifies_peer_payload_and_local_authority,provider_recovery_pre_signature_firewall_reloads_pool_and_budget,expired_unsigned_provider_history_is_compacted_atomically,quote_prevout_lookup_failure_preserves_local_authority',
    'paymaster_wallet_security_tests/quote_request_*,expired_quote_retry_is_consumed,identical_quote_retry_reuses_signed_response,automatic_quote_rejection_allows_following_work',
    'paymaster_wallet_security_tests/client_recovery_read_failure_stops_new_preparation,provider_recovery_read_failure_retains_request_and_replay',
    'paymaster_wallet_security_tests/provider_pre_signature_firewall_reloads_capacity_pool_and_budget,provider_final_commit_spends_budget_atomically,client_recovery_result_commit_failure_survives_ttl_and_exact_retry',
    'paymaster_wallet_store_tests/session_lookup_distinguishes_absence_from_read_failure',
    'paymaster_wallet_store_tests/client_view_*,client_recovery_status_getter_preserves_absence_and_bool_compatibility,client_result_status_getter_preserves_absence_and_bool_compatibility,client_result_read_failure_retains_incoming_message,client_result_corrupt_predecessor_retains_incoming_message,confirmed_client_payment_bypasses_prior_result_read,final_result_read_failure_does_not_sign_replacement',
    'digidollar_mint_cleanup_tests/pending_balance_excludes_expired_mints_without_position_records',
    'digidollar_wave17_spendability_tests',
    'paymaster_wallet_identity_tests/paymaster_dd_reservations_are_owned_but_not_spendable,unreadable_paymaster_locks_fail_closed_for_coin_selection,paymaster_input_scan_reuses_pool_without_cross_scan_cache'
)
& ./build_msvc/x64/Release/test_digibyte.exe "--run_test=$($focusedCases -join ':')" --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Inbound, persisted-state or pending-balance regressions failed' }
```

The quote cases distinguish consumed peer conflicts/expired retries from unreadable
local authority, and preserve exact signed replay and subsequent automatic work.
The client-view cases exercise the list and resolve RPCs as well as native reads:
missing records remain distinct from corruption or unsupported versions. Result
processing must retain incoming messages until prior state is readable, while
already confirmed payments retain their existing reconciliation path. Recovery
tests check both the read-error stop and the existing absence/continuation guards;
they do not replace the complete recovery-payment scenarios.

Then run `wallet_paymaster_reorg.py` using the [functional runner](#functional-selection)
with freshly built daemon and CLI binaries. It must consume a conflicting quote
request and reject the unrelated recovery over authenticated v2 channels, preserve
the original financial state, and finish the real payment/reorganization sequence
without duplicate finance events, with RPC contract checking enabled. A failure
during fixture setup is not a pass for that regression.

The targeted Python check can be run in an unused workspace environment
with the repository CI's flake8 version
(`ci/lint/04_install.sh`); this needs package-download access and normally takes
minutes. Do not overwrite an existing environment:

```powershell
python -m venv .ai/paymaster-lint-env
& ./.ai/paymaster-lint-env/Scripts/python.exe -m pip install flake8==6.1.0
& ./.ai/paymaster-lint-env/Scripts/python.exe -m flake8 --version
& ./.ai/paymaster-lint-env/Scripts/python.exe -m flake8 test/functional/wallet_paymaster_reorg.py --select=F401,F821,F823,E9
```

This narrow check does not replace the repository Python lint selection in
`test/lint/lint-python.py` and its CI dependencies.

For the wallet-lock ownership and reorganization-readiness corrections, rebuild
the changed sources and start with this focused selection:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_wallet_store_tests/unsigned_client_cleanup*:paymaster_wallet_store_tests/unsigned_client_cancel_failures_and_lost_reply_preserve_input_ownership:paymaster_wallet_store_tests/ambiguous_authorization_is_protected_and_prunes_to_tombstone:paymaster_application_tests/*' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Wallet safety regressions failed' }
```

Add the current lock-owner and recovery-request regressions after rebuilding:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_wallet_store_tests/active_reservation_cleanup_preserves_manual_lock_ownership,manual_unlock_does_not_remove_active_reservation_lock,legacy_ambiguous_lock_record_is_preserved_as_manual,unknown_lock_owners_fail_closed_without_releasing_reservations,client_attempt_artifacts_and_authorization_are_append_only_and_atomic:paymaster_wallet_security_tests/provider_capacity_lookup_distinguishes_peer_conflicts_from_local_damage,provider_recovery_capacity_absence_rejects_but_read_failures_retain_request,automatic_recovery_rejection_allows_following_quote_and_submit,provider_recovery_read_failure_retains_request_and_replay' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Lock ownership or recovery request regressions failed' }
```

`CWallet` and `ClientModel` have appended private state: all allocation sites and
fixture definitions must use the new headers. Selected isolated links require
fresh compilation of their reached allocators and are not clean product builds.
The lock-owner cases cover manual locks installed during an active reservation,
both live and reloaded, write/commit failure, expiry, pruning and corrupt records.
The recovery cases distinguish missing peer capacity from damaged local state;
automatic progress checks a valid saved quote and consumption of a stale submit.

After changes to submit scheduling, rebuild all consumers of `paymaster/manager.h`
and run the bounded dispatch regressions:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_wire_tests/manager_provider_submit_*:paymaster_wallet_security_tests/automatic_submit_progress_under_continuous_recovery_traffic,automatic_recovery_progress_under_continuous_submit_traffic,normal_submit_classifies_peer_payload_and_local_authority,provider_recovery_pre_signature_firewall_reloads_pool_and_budget' --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Submit scheduling or authority regressions failed' }
```

These cover both directions of continuous admitted competing traffic, progress
within two eligible service cycles, retained same-session messages, independent
providers, empty-type fallback and lease cleanup after exceptions/pruning.
Interleaved acknowledgements and retained messages also exercise independent
netgroup and session rotation for each submit type.
The traffic markers are stale submissions: their rejection proves dispatch and
unchanged database/checkpoint state, not a successful payment. The existing
classification/firewall cases retain local read failures and replay exact saved
recovery results; complete recovery payments remain in the functional matrix.
A complete fresh signed payment remains a functional-test obligation.

For request rotation and permanent normal-submit rejection, add:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_wire_tests/manager_provider_request_*:paymaster_wallet_security_tests/normal_submit_classifies_peer_payload_and_local_authority,automatic_quote_progress_under_continuous_recovery_requests,automatic_recovery_request_progress_under_continuous_quotes,deferred_capacity_preserves_request_and_yields_next_turn' --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Request dispatch or submit classification regressions failed' }
```

Request tests keep the one-message dispatch budget and the existing admission
limits. They cover continuous competing types, retained local failures, provider
isolation, independent group/session rotation and a deferred capacity request
yielding the next turn. Normal-submit cases distinguish expired or divergent
peer work from damaged local user signatures, results and final-commit authority.
Rejected traffic markers must leave the wallet database and checkpoint unchanged.

For native finance/automation snapshots and their unchanged JSON contract, run:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=paymaster_application_tests/native_finance_and_automation_json_preserve_complete_observations:paymaster_wallet_identity_tests/native_finance_status_captures_capital_time_and_typed_failures,provider_activity_ignores_completed_and_expired_authority,provider_automation_capacity_pause_preserves_approval' --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Native status snapshot or JSON regressions failed' }
```

These check one observation time across expiry boundaries, capital categories,
backup failures, optional valuation, known zero/false values and event paging.
For shared setup defaults, retain the Qt configuration workflows and guided
setup reset cases; funding-model reset and numeric-policy reset remain distinct.

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
and relevant `DigiDollarWidgetTests` cases from the current fixture. For the
manual Activity result contract, select
`paymasterManualActivityUsesExpectedRpcAndReadableStates`: native result
serialization, deferred/local-rejection/signed-result states, optional delivery
status, malformed or contradictory fields and literal diagnostic text. Both
request processing and explicitly confirmed submit processing issue one processing
RPC and the existing status read; formatting must not introduce another mutation.
The [operator state table](digidollar-paymaster-operator.md#manual-activity-results)
defines the displayed meaning. Clear filters before claiming a complete Qt run.
A targeted result including init/cleanup is not that many independent feature cases.

For the private setup/send extractions, retain the existing setup-boundary,
failed-step retry, one-approval restore, capital-task, layout/theme and sender
authorization/cancellation/lost-reply selections. Setup-boundary rows also
remove and rebind a wallet while the wizard is open or its preview is pending;
neither the old dialog nor its callback may authorize work for the new binding.
`WalletTests/nodeRpcReadsJoinBeforeModelDestruction` holds a real internal RPC
across model teardown and preserves its structured error contract. Run the
ordinary DD Overview unavailable/malformed/error selections with it.
`paymasterFinancesAndBackupWorkflow` round-trips ungrouped CSV amounts including
zero, fractional units, large amounts, maximum DD and `MAX_MONEY` DGB.
The setup-boundary selection also delivers read/mutation success/error replies
after a wallet-generation change, in addition to the privacy rows below.
The same function's four `privacy-*` rows deliver success/error replies after
the wizard is destroyed, both before a read completes and after a mutation has
executed. No stale control access or subsequent setup step is permitted;
already dispatched work remains saved. For recovery expiry, select
`paymasterSignedRecoverySurvivesQuoteExpiry`,
`paymasterClientRecoveryExpiryIsFailClosed` and
`paymasterSendControllerRecoveryReview`: signed phases retain permitted actions
past the deadline while unsigned preparation stays blocked.

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

For final-signature enforcement, combined provider-history admission and native
observation/lifecycle failures, use a freshly built test binary:

```powershell
$reviewRegressions = @(
    'paymaster_psbt_tests/final_witness_requires_default_sighash_independently_of_psbt_metadata',
    'paymaster_wallet_security_tests/provider_history_quota_counts_alternative_recovery_quotes,expired_unsigned_provider_recovery_history_is_compacted_atomically,provider_recovery_pruning_is_bounded_and_preserves_signed_records',
    'paymaster_wallet_finance_tests/warm_finance_read_errors_never_become_zero_capital,wallet_batch_destruction_discards_uncommitted_finance_notifications'
)
& ./build_msvc/x64/Release/test_digibyte.exe "--run_test=$($reviewRegressions -join ':')" --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Final signatures, retained history or transaction completion failed' }
```

The signature case uses real keys and valid signatures for all supported hash
modes, both metadata forms and all four roles. History cases check admission,
exact replay at quota, expiry/pruning begin/write/commit failures, restart,
bounded cleanup and retained signing evidence. Finance failure injection targets
one unreadable key and its failed existence lookup while other records remain
readable. Add existing checkpoint, session-index, finance commit/abort, wallet
PSBT and provider/recovery functional regressions. Recompile reached consumers
of the changed inline `WalletBatch` destructor for isolated checks; only clean
product builds establish the complete consumer set.

## Architecture checks

From the repository root:

```powershell
python test/lint/test_paymaster_boundaries.py
if ($LASTEXITCODE -ne 0) { throw 'Boundary fixtures failed' }
python test/lint/lint-paymaster-boundaries.py
if ($LASTEXITCODE -ne 0) { throw 'Dependency/cycle checks failed' }
```

For AUTO authorization, disconnect lifetime and delayed-result/maintenance
recovery, rebuild the changed sources and all consumers of `paymaster/manager.h`
before using these focused regressions:

```powershell
& ./build_msvc/x64/Release/test_digibyte.exe '--run_test=net_tests/paymaster_disconnect_retains_lease_for_message_readers:paymaster_provider_tests/failed_signed_maintenance_retains_budget_and_replay_history:paymaster_wallet_identity_tests/recurring_maintenance_conflict_reversal_survives_restart_atomically:paymaster_wire_tests/manager_authenticated_results_survive_failed_scope_and_delayed_pruning:paymaster_wallet_store_tests/result_processing_distinguishes_session_absence_damage_and_ineligibility,auto_dispatch_rejects_unreadable_session_before_funding_or_signing,client_result_read_failure_retains_incoming_message,client_result_corrupt_predecessor_retains_incoming_message,client_result_local_write_failure_retains_incoming_message,client_final_preflight_failure_retains_incoming_message:paymaster_wallet_security_tests/provider_final_commit_spends_budget_atomically,client_recovery_result_commit_failure_survives_ttl_and_exact_retry' --report_level=detailed
if ($LASTEXITCODE -ne 0) { throw 'Paymaster lifetime, retention or maintenance regressions failed' }
python test/functional/test_runner.py --jobs=1 wallet_paymaster_auto.py
if ($LASTEXITCODE -ne 0) { throw 'AUTO authorization regression failed' }
```

Use the short isolated Windows temp directory described above. The AUTO test
uses descriptor Regtest wallets and no provider: preparation, absent/mismatched
approval, concurrent same-ID calls and CLI calls cannot become direct payments;
the explicit ordinary DGB send remains functional. The native cases cover both
result types beyond TTL/enqueue pruning, all recurring maintenance kinds across
conflict/abandon and reload, atomic commit failure, and exact lease/permit lifetime.
The real concurrent network case is also a targeted TSan regression; an ordinary
native pass is not sanitizer evidence.

For committed candidates, the
[architecture checks](design/paymaster-integration-boundaries.md#architecture-checks)
also compare changed paths against a selected base. Required Core-change reasons stay in the
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
| Amounts, exact fees, manifests and signatures | `paymaster_types_tests.cpp`, `paymaster_txbuilder_tests.cpp`, `paymaster_psbt_tests.cpp`; `client_authorization_rejects_invalid_amounts_before_arithmetic` covers rehashed/serialized negative and extreme operands in both fee modes. Wallet PSBT tests compare carrier boundaries and exact independently reconstructed fee outputs against the shared amount rule. Wallet security suites retain exact artifact-conflict, coherent invalid-sighash and manifest-only error cases. |
| Channel proof and bounded admission | Protocol/wire/transport/admission suites; `p2p_paymaster.py`, `p2p_paymaster_connection_capacity.py`. |
| Nonfinancial diagnostics | Transport `diagnostic_*`; `rpc_paymaster_endpoint.py` tests TCP/SOCKS5 isolation and no authentication; Qt `paymasterDiagnosticCommands`, `paymasterDiagnosticRefreshAndPrivacy`, `paymasterNodeConnectionEditor`. Local proxy coverage is not public Tor evidence. |
| Selection, fee/privacy limits and failover | Client comparator cycle permutations/strict-order properties; directory/reputation suites; `wallet_paymaster_offer_selection.py`, `wallet_paymaster_failover.py`. |
| Exact authorization and same-ID retry | Wallet service/store/security suites, including `client_saved_quote_authentication_precedes_new_authority`: corrupt persisted quote signatures cannot gain acceptance or pass the signing gate; exact retries and historical final validation remain valid. Application tests preserve historical Restricted request bindings without renewing authority. `wallet_paymaster_rpc.py`, `wallet_paymaster_provider.py`: lost replies, replay, unsigned/signed cancellation, work exclusion; accepted high-/low-level retries reject changed fee caps, attempt limits and preferred offers while unchanged retries survive stricter current policy. |
| Inbound rejection versus local uncertainty | Identity `unknown_recovery_submit_is_consumed_before_normal_work`; security submit/recovery/quote classification fixtures; recovery `submit_payload_firewall_preserves_full_chain_validation`. `wallet_paymaster_reorg.py` consumes a conflicting quote request and rejects an unknown recovery over authenticated v2 channels, verifies unchanged balances/pools/budgets, then completes and reconfirms a real payment without duplicate finance events. |
| Persisted read errors and action gating | Store `client_view_*`, `session_payment_snapshot_*`, status-getter and result-read cases cover corrupt/future records, unreadable/latest-attempt terms, valid unquoted fallback, dangling recovery indexes, full result binding/signature checks, structured get/list/resolve errors, retained result messages and unchanged protected state. Security recovery-read cases reach the application operations through their normal readiness gates. |
| Client result durability | Store `client_result_local_write_failure_retains_incoming_message` injects begin/write/commit failures and retries the same signed result; `client_final_preflight_failure_retains_incoming_message` retains exact final bytes on local preflight failure. `client_result_corrupt_predecessor_retains_incoming_message` covers readable but unauthenticated predecessor sequences, signatures and bindings. Security `client_recovery_result_commit_failure_survives_ttl_and_exact_retry` uses mined inputs and real signatures, corrupts local recovery bindings/signatures and fails the final DB commit; the original result survives both TTLs and is retried through ordinary mempool validation without a second fee charge. Forged remote signatures remain rejectable with intact local authority. Wire `manager_authenticated_results_survive_failed_scope_and_delayed_pruning` covers both result types, enqueue/replay pruning and queue-incarnation checks. |
| Provider finalization and index progress | Security `provider_final_commit_spends_budget_atomically` freezes the validation queue and advances the tip after commit. Exact commit recovery must complete before queue release, preserve the spent budget and permit accounting-neutral replay; the test releases the queue on the blocking negative path so teardown can finish. `wallet_paymaster_provider.py` also advances the tip during an automatic submit. |
| Provider manual locks | `wallet_paymaster_locks.py` checks DGB and carrier readiness, transient/persistent locks, reload and locks added after capacity reservation; DGB unlock permits the same signed client payment. The ordinary DD-token RPC unlock prohibition remains enforced; transient carrier locks clear on reload and persistent ones keep blocking new signatures. Identity capacity lifecycle covers both reservation APIs; wallet PSBT role signing preserves the unsigned provider artifact and its independent reservation lock. |
| Pending-mint balance consistency | Mint cleanup `pending_balance_excludes_expired_mints_without_position_records` checks both pending APIs across expiry, reorg, mempool and unknown metadata/height; run ordinary DD spendability and Paymaster reservation-balance cases alongside it. |
| Signed recovery and reorg accounting | Recovery/reservation/store suites; `wallet_paymaster_reorg.py`, `wallet_paymaster_lifecycle.py`; provider tests retain exposure at 1/239 confirmations and release at safety depth 240 without double charging. |
| Finite setup and whole-slot refill | Setup/identity/load/store suites; `wallet_paymaster_pool_setup.py`, `wallet_paymaster_partial_capacity.py`, `wallet_paymaster_readiness.py`, `wallet_paymaster_operator.py`; both assets and usable capacity below target. |
| Monotonic maintenance time | `maintenance_reconciliation_preserves_accounting_time`, application `maintenance_rpc_budget_matches_authorization_after_clock_rollback`, provider `maintenance_budget_usage_*`; pool/lifecycle/provider functional tests cover confirmed stages, clock rollback, restart and absence of duplicates. |
| Setup parity and funding observations | `paymaster_setup_tests`; pool setup `--funding-diagnostics-only`; Qt `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep`, `paymasterPoolPreparationDiagnostics`, `paymasterPoolPreparationStoredFeeDiagnostics`: approvals, shortfalls, malformed/missing observations and read-only refresh. |
| Backup, adoption and stale copies | `wallet_paymaster_backup.py`, `wallet_paymaster_backup_adoption.py`, `wallet_paymaster_backup_exit.py`, `wallet_paymaster_checkpoint_rollback.py`. |
| Cancellation of uncreated work | Identity `maintenance_cancellation*`; Qt `paymasterMaintenanceCancellation`, `paymasterSavedTaskTitles`: exact preview, lost-reply signatures, rollback, checkpoint protection and retained signed work. |
| Excess reserve release | Identity `rebalance_saved_targets_*`; Qt `paymasterGuidedCapitalTasks`, `paymasterReserveReduction`: missing/underfilled assets, saved revisions, DGB-only scope and exact receipts. |
| Stop and retirement | Wire `manager_provider_stop_*`; identity `provider_stop_waits_for_work_without_bypassing_exclusion`, `capital_release_cancels_only_reviewed_uncreated_funding`; Qt `paymasterStoppedOverview`, `paymasterStopAndRelease`, `paymasterRetirementLateResults`: priority, atomic release and uncertain replies. |
| Configuration and signing review | Setup suite; Qt `paymasterConfigurationConflicts`, `paymasterSafetyInputValidation`, `paymasterConfigurationSaveRefresh`, `paymasterManualSubmitConfirmation`, `paymasterOperatorConfirmationWalletBinding`: saved versus draft policy, current wallet/privacy/readiness and exact approval. |
| GUI state, finance and navigation | Paymaster Qt suite covers unavailable/loading states, affordability, cancellation, two reads with mutation barriers, explicit coalescing, wallet switch/shutdown, unlock lifetime, untrusted error text, finance pages/export, responsive themes and preserved drafts. Controller cases inject time and replies; ordinary `WalletTests/async*` cover generic scheduling/start failure/worker shutdown. Mocked RPC fixtures do not replace backend tests. |
| Session index | Wallet store tests at 100/1,000/8,192 sessions: one reconstruction, point reads, commit/abort, reopen and invalidation. |
| Finance v2 | Wallet identity/DB and `paymaster_wallet_finance_tests`: 100,000 events; 100/1,000/8,192 sessions; bounded/filter-bound pages, 256-job limit, unchanged-query scan counts, stale revisions, UTC periods, replay/reorg/abandon/delete, commit failure, atomic migration and restore. Qt `paymasterFinancePaging` covers page duplicates, rejected pages, cached navigation and provider/revision/period/wallet resets. `bench` logs measure actual reconciliation lock duration. |
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

The current-current selection includes 28 variants. In addition to the default
scenarios, it registers the RPC legacy-policy, offer-model, reserved-capacity and
client-protection cases; provider cancel-burst; and lifecycle no-client-change
and announcement-refresh cases, plus provider manual locks and the AUTO direct-authorization guard. These are independent paths, not substitutes
for the default scenarios. Historical bridge variants remain in the separate
compatibility selection below.

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
