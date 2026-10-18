# Paymaster release gate and evidence

Financial review checkpoint: source `e7c2c8c4bd`, 2026-10-15, on
`integration/paymaster-v9.26.7`. The [documentation index](../PAYMASTER.md) records
its later documentation/source review baseline. There is **no complete
current-revision release certification** in this record. Implemented behavior and selected regression
results must not be confused with release acceptance.

## Required candidate gates

| Gate | Required evidence |
|---|---|
| Product build | Clean app, daemon, CLI and test builds for supported platforms; exact revision/toolchain. |
| Core invariants | Exact transaction/fee/signature, budgets, replay, recovery and persistence regression groups. |
| Lifecycle | Encryption/unlock, startup/history readiness, shutdown/restart, backup/adoption/rollback and retirement. |
| Liquidity | Finite versus recurring approvals, whole-slot refill for both assets, live capacity below target and fee caps. |
| End-to-end clients | Exact authorization, cancellation, response loss, failover, reorg and confirmed-recipient-only success. |
| GUI and CLI | Shared settings/approval semantics, async readiness, consistent balances/offers and headless maintenance. |
| Compatibility | Ordinary DGB/DD paths, feature disabled, historical wallet/node interoperability. |
| Adversarial operation | Sanitizers/fuzz/load, real Tor reachability and prolonged operation, hostile relay. |
| Durability and review | Physical storage failure coverage and independent review of financial/lifecycle boundaries. |

Use the [runbook](digidollar-paymaster-testing.md) for commands and the
[threat model](paymaster-threat-model.md) for residual risks. Record a pass only
against the candidate and built executable. Remaining tests are not waived by
successful manual regtest payments.

## Focused regression follow-up, 2026-10-17

Source changes: `c7750a18a1` (maintenance accounting time) and `0ab4289107`
(functional expectations and coverage). The three failures reported from another
node were reproduced locally before correction:

- Full pool setup/capital release: maintenance reconciliation used wall time
  below a saved record's creation time after policy revisions. Preserve the
  record/ledger accounting floor so confirmed work no longer remains open.
  The strict guard for genuinely unfinished or signed work stays in place.
- Lifecycle target expansion and provider replacement: tests waited for later
  batches without confirming pending reserve outputs. They now verify the
  separate confirmation stages, restart idempotency and absence of duplicates.
  Further stale expectations were corrected for active service below its refill
  target and recovery exposure retained until 240 confirmations. A policy-change
  test retries only the transient `PAYMASTER_PROVIDER_BUSY` response.

Verification: two changed translation units compiled with MSVC 14.43; the new
wallet regression failed against the previous implementation and passed after
the fix. Ten selected wallet cases passed (455 assertions). Full descriptor
runs of `wallet_paymaster_pool_setup.py`, `wallet_paymaster_lifecycle.py` and
`wallet_paymaster_provider.py` passed with the reported seeds
`6185203042326809576`, `5495766456681726476` and `8132640900451697906`, respectively.
The previously open pool-setup failure in the historical rows below is resolved
by this correction.

These were isolated unit/daemon links with existing dependencies and the local
CLI, not a new complete product build or full test suite. Python syntax and
Git whitespace checks passed; Flake8 was unavailable in the local environment.
Cross-platform, full-suite and release-acceptance gates remain separate.

## Checkpoint fixture follow-up, 2026-10-17

Source `e4195962a0` corrects four locally reproduced failures reported from a
broader Windows unit run: current/V5 capacity reservation lifecycle, append-only
client authorization and atomic provider-quote commit. Synthetic clone mutations
had advanced the source provider checkpoint; a client-only fixture enrolled the
remote provider identity locally; provider tests expected immediate retry after
a DB commit failure despite the independent checkpoint requiring review.

The four corrected cases and three explicit commit-failure quarantine cases
passed (7 cases / 992 assertions). Successful paths, row-write rollback and
post-commit-failure protection are separate scenarios. Production code and
checkpoint enforcement were not changed. Two test translation units compiled
and linked in isolation with MSVC 14.43 using existing dependencies; Git
whitespace checks passed. The complete Paymaster unit group and functional
matrix remain operator-run checks and were not rerun for this test-only change.

## Windows functional matrix follow-up, 2026-10-17

The operator's extended current-current matrix passed 15 of 19 variants. Four
reported failures were investigated against their saved logs and random seeds.
Source `e17c96415d` serializes checkpoint file reads with replacement: Windows
CRT readers can deny `MoveFileExW` replacement while their handles remain open.
A direct Windows API probe reproduced error 5 with an open reader and succeeded
after closing it. The new concurrent unit stress test can also pass on the old
implementation depending on scheduling; it is not a deterministic red/green
reproduction. Checkpoint enforcement and stale-copy rejection remain enabled.

Source `fe6536afc3` corrects functional fixtures: retry only exact pre-execution
busy guards, bound explicit same-request transport retries, reserve client fees
only after authorization, and establish a funded unsigned order and quote before
testing immutable provider selection. Financial/protocol failures still fail
the tests; production transport and spending limits were not increased.

Verification: three related wallet cases passed (270 assertions). All four
previously failing functional variants passed individually with their reported
seeds: checkpoint rollback (`5648792314745756954`), RPC
(`7865158726139777058`), provider `--drain-recovery-only`
(`5055654902687554025`) and offer selection (`3385301776452823645`). Two changed
C++ translation units compiled with MSVC 14.43; unit/daemon executables were
linked in isolation with existing dependencies and CLI. Python syntax and Git
whitespace checks passed. Flake8 was unavailable. The complete 19-variant matrix
and normal product builds were not repeated; these focused passes are not a
new complete-matrix or release certification.

## Remaining provider variants, 2026-10-17

The next operator-run matrix passed 17 of 19 variants. The full provider test
reached a remaining unguarded manual quote replay and failed with
`PAYMASTER_PROVIDER_SERVICE_BUSY`. Source `69efb3665f` applies the existing
exact pre-execution busy helper consistently to manual request, replay, submit
and alternative-recovery processing in this fixture. Other RPC failures and
all financial/provider-isolation assertions remain effective.

The drain-only variant failed before its scenarios: node 0's IPv4 RPC bind on
`127.0.0.1:20072` failed while its IPv6 listener succeeded. The node finished
startup, but the framework uses IPv4. The retained log does not establish why
that port was unavailable. A repeat with the same seed and port seed succeeded
without a startup-code change; no OS reservations or other processes were changed.

Verification used the operator's normal Release daemon/CLI already built in the
workspace. The full provider scenario passed with seed `7101818818119886812`
and port seed 7; drain-only passed with seed `8618233438433012274` and port seed
6. The drain-only repeat ran before this additional fixture change; the full
scenario ran afterwards. Python syntax and scoped Git whitespace checks passed.
No new C++ build or unit run was needed for this Python-only change. The full
19-variant matrix was not repeated; the intermittent IPv4 bind failure was not
reproduced and is not claimed as fixed.

## Provider configuration assertions, 2026-10-17

A further operator run failed before a full provider scenario because Windows
denied its P2P bind on `127.0.0.1:15012` with WSAEACCES (10013). Drain-only
reached a policy-change rejection test, but a transient `PROVIDER_BUSY` response
arrived before the expected `PROVIDER_RUNNING` response. Previous busy fixes
covered processing calls, not every configuration/assertion path.

Source `bfc3ee5b6a` consistently waits for pre-execution exclusion on guarded
configuration, pool and start/stop calls. Expected-error assertions enclose the
idle helper, so permanent errors remain mandatory rather than accepting busy
as a substitute. A deterministic fixture injection rejects the first policy
call as busy, then checks the real running-provider rejection. Shared finite
pool-confirmation retries also wait for the guard. Production RPC behavior,
spending limits and transport limits are unchanged.

Verification against the normal Release daemon/CLI: full provider passed with
seed `2954881066833722793`, port seed 1; drain-only passed with seed
`6701264833899060309`, port seed 0. Both final runs included the injected busy
regression. Python syntax and scoped Git whitespace checks passed; Flake8 was
unavailable. The original P2P port was usable in the repeated full test without
changing OS permissions, reservations or other processes. The earlier bind
denial is not reproduced or claimed as fixed. No new C++ build/unit run or
complete functional matrix was performed for this test-only correction.

## Operator matrix completion, 2026-10-17

After the configuration-guard corrections at checkout `31d36a6b7a`, the operator
reported all 19 current-current Paymaster functional variants passing on Windows:

```powershell
python ./test/functional/test_runner.py --jobs=1 --extended '--filter=^(wallet|rpc|p2p)_paymaster' --exclude=p2p_paymaster_v9_26_5_bridge.py
```

The supplied summary records 704 seconds accumulated and 709 seconds wall time,
with no failed variants. This completes the previously failing 19-variant
selection, including full provider and drain-only, checkpoint rollback, RPC,
offer selection, lifecycle, pool setup, backup/adoption/exit, failover and reorg.
The preceding standalone provider selection also passed both variants in the
operator's environment (101 and 22 seconds).

This is operator-supplied functional evidence; exact executable hashes, complete
logs and random seeds were not included with the successful matrix summary.
It does not certify the full Qt suite, ordinary-wallet regressions, historical
node interoperability, real Tor operation or a clean cross-platform product
build. The intermittent Windows bind failures were absent in this run; their
underlying cause remains unestablished. The next separate GUI gate is the full
`PaymasterWidgetTests` suite from the runbook.

## Operator Qt follow-up, 2026-10-18

The supplied full `PaymasterWidgetTests` output reports 686 passed, 3 failed,
0 skipped and 0 blacklisted in 1,245,421 ms, using static Qt 5.15.10/MSVC 2022
on Windows 11. Other Qt suites were excluded by the suite filter. The failures
were `paymasterSafetyControlsDefaultFailClosed`,
`paymasterInjectedRpcCoversConfigurationWorkflows` and
`paymasterClientOfferAutomaticRefresh`.

Source `6cea92c461` updates the test fixtures only. The safety notice now explains
customer-payment budgets separately from reserve capital/refill fees. The
configuration workflow explicitly supplies a fresh mutation snapshot after an
intentionally incomplete identity refresh and expects the complete operator
refresh after saving safety limits. The countdown test requires the offer radio
button to be the form's remembered focus and checks its preservation, rather
than depending on Windows retaining global application activation. Card
identity, geometry, selection, expiry and read-count assertions remain intact.

The first two failures were reproduced against the existing Release executable;
the global-focus failure did not recur in a standalone baseline run. All three
corrected functions passed separately after compiling one test translation unit
with MSVC 14.43 and linking an isolated Qt executable against the current normal
build's objects/libraries. Each reports 3 passed including init/cleanup, so this
is three feature checks, not nine independent scenarios. Scoped whitespace
checks passed. No production GUI code changed. The complete Qt suite was not
rerun; the full-suite GUI gate remains open. Style-property and Windows geometry
warnings in the supplied output have not been classified as resolved.

The subsequent operator repeat of the full `PaymasterWidgetTests` selection
using `gui-followup-20261018/qt.exe` reported `All tests passed`; the captured
PowerShell exit-code guard did not throw. This completes that Paymaster GUI
selection on the isolated Windows executable after the test corrections. The
successful follow-up did not include per-case totals, duration or warning
details, so those are not inferred from the earlier failed run. It is not a
complete all-Qt, cross-platform or normal-product-build certification. The next
regression selection covers the remaining ordinary wallet, address-book,
DigiDollar, transaction and app/options/URI/RPC Qt suites without repeating the
Paymaster suite.

## Remaining Qt suites: overview teardown, 2026-10-18

The operator's remaining-Qt run passed `AppTests`, `OptionTests`, `URITests`,
`RPCNestedTests`, `WalletTests` and `AddressBookTests`, then crashed in the first
`DigiDollarWidgetTests` scenario. Windows reported access violation `0xc0000005`
with `getdigidollarstats`' chainstate flush reaching the validation-signal
scheduler from an overview RPC worker. The later suites did not complete;
this run is not an all-Qt pass.

The overview smoke fixture started real asynchronous balance/history/oracle/
health reads, then immediately destroyed its models and `TestChain100Setup`.
An invalidated callback guard does not stop a Core RPC already running against
that temporary node. Source `628321a743` moves the smoke check into the existing
friend test method and waits for its in-flight updates to complete before
teardown, keeping the validation scheduler alive through those reads. No
production RPC or widget behavior is changed.

The standalone old smoke test passed locally, so a deterministic crash
reproduction is not claimed. The corrected smoke test, reservation-balance
overview and privacy-mask overview each passed separately (three feature checks;
each Qt summary includes init/cleanup). Verification compiled one test
translation unit with MSVC 14.43 and linked an isolated executable with the
current normal-build dependencies and previously corrected Paymaster test
object. Scoped whitespace checks passed. The complete remaining-Qt selection
must still be rerun by the operator; other asynchronous fixture lifetimes are
not certified by these focused results.

## DigiDollar Qt regression follow-up, 2026-10-18

The operator's next five-suite selection completed without the earlier crash:
`DigiDollarWidgetTests` reported 120 passed / 11 failed; Wave 19, 16 / 0;
mint records, 2 / 7; transaction records, 7 / 5; transaction table, 10 / 0.
These are Qt result rows, including fixture and data rows, not independent
security scenarios. The 23 failures were investigated as shared causes.

Source `c231521234` explicitly awaits the asynchronous DGB cache before direct
mint/send actions, finishes RPC warmup for standalone active-Thaw-Day mint
fixtures, reads CSV in text mode for Windows newlines, and updates menu, setup
and balance-format expectations to the current UI. The cache helper is opt-in;
default shared MiniGUI initialization is unchanged. The overview teardown check
also completes RPC warmup so a filtered run exercises real status reads instead
of only a warmup rejection. Theme assertions scope Paymaster rules rather than
unrelated later file-dialog CSS. Native render checks exercise visible enabled
controls, expand advanced Paymaster settings, sample the clipped popup viewport
with an image-bounds assertion, and require selectable table items to actually
be selected; read-only offer previews are not selection controls.

Source `82c0b31493` fixes a real presentation issue: advanced privacy/selection
dropdowns and numeric editors inherited blue or low-contrast styles. Both DD
send themes now style native combo items, item views and numeric editors in
the DD palette. Core monetary rules, wallet balances and RPC behavior are
unchanged; palette and rendered-selection contrast checks remain in place.

Local verification passed eleven focused DigiDollar widget functions, the full
mint-record component selection (9 Qt passed rows) and full transaction-record
component selection (12 passed rows). The related Paymaster offer-theme and
automatic-offer-refresh functions also passed. Counts for individual functions
include init/cleanup and must not be added as independent feature cases.
Four test translation units and the generated Qt resource were compiled with
MSVC 14.43/Qt 5.15.10 and linked in isolation with existing normal-build
dependencies. Source/executable fingerprints and logs are retained locally;
scoped whitespace checks passed. No complete product build or new full Qt run
was performed. The large `DigiDollarWidgetTests` suite still needs an operator
repeat; the already green Wave 19/table selections were not rerun locally.

## Earlier acceptance IDs: current interpretation

The former V1 list is retained here as a review mapping, not a passed matrix.
Items superseded by later implementation decisions are identified explicitly.

| Earlier IDs | Current requirement / disposition |
|---|---|
| 1, 2, 6 | Confirmed DD payment without client DGB; no key transfer; each party signs only authorized owned inputs. |
| 3 | Separate admission/operational resources. The old three-admission-slot readiness minimum is superseded: confirmed usable capacity remains serviceable below the refill target. |
| 4, 5, 24 | Valid carrier outputs for small fees, bounded integer fee/exact-total arithmetic and one recipient with valid DD output bounds. |
| 7, 8, 12, 21, 23 | Durable explicit authorization, same-input retry, fallback only before user PSBT, protected signed ambiguity and exact crash recovery. |
| 9, 10, 13 | Existing validators/consensus remain authoritative; existing five-argument send retains wallet-funded DGB behavior. Review arithmetic portability separately. |
| 11 | Local reputation cannot override identity, liquidity, privacy or authorization checks. |
| 14, 15 | Public sponsorship is implemented; fresh restricted sponsorship is disabled. Legacy restricted bindings/consumption remain for accepted work and recovery, not new availability. |
| 16, 17, 18, 20 | Fresh internal change, metadata-safe logging, selected-provider disclosure and high-privacy onion isolation/one attempt/no silent downgrade; no anonymity promise. |
| 19 | Eligible detail pruning retains permanent request/conflict protection; missing evidence must not be reconstructed as new authority. |
| 22, 26 | Exact final bytes and authority/accounting persist atomically before broadcast; signed-result authority is monotonic. |
| 25 | Unpruned synchronized index/history and creating transactions; ordinary validation needs no Paymaster feature, discovery needs upgraded peers. |
| 27 | Only validated locally confirmed recipient payment is success; terminal/mempool/recovery state alone is insufficient. |
| 28 | Locking pauses the same session; active models require finite safety limits and positive absolute network-fee ceilings. |

## Historical checkpoints

These summarize previous records retained in Git. They are deliberately not
summed into a current test total. Counts can overlap and some runs used isolated
executables rather than normal product builds.

| Checkpoint | Recorded evidence | Limit |
|---|---|---|
| 2026-09-27, `1789c803be` | RC2 selected unit group: 284 cases / 10,767 assertions; nine functional tests. | Older branch/baseline; does not certify later setup, security or v9.26.7 work. |
| 2026-10-06, `e5cfa2da3e` | Twelve translation units; 26 selected Core/Qt cases. | Incremental/isolated test executables; no normal app/daemon/CLI link or functional execution at that checkpoint. |
| 2026-10-10 retention corrections | Focused 3 cases / 8,539 assertions; related selection 48 / 10,012 including the focused cases. | Isolated MSVC executable; quota fixture accounts for many assertions. |
| 2026-10-10 recovery-fee correction | Focused 5 cases / 384 assertions; related 28 / 1,542 including them. | Selected tests, not full product release verification. |
| 2026-10-15, `01aaf13df9` | Seven translation units and busy-wallet Qt regression. | Selected build/test; not complete GUI acceptance. |
| 2026-10-15, `e7c2c8c4bd` | Two translation units, targeted Qt regression, German translations and two screenshot sizes. | Sync-presentation checks only; no Paymaster release certification. |
| 2026-10-16, `685a6b4f2c` | Three changed translation units compiled with MSVC 14.43/Qt 5.15.10; 41 selected Qt cases and `wallet_paymaster_pool_setup.py --funding-diagnostics-only` passed. Funding display checked in two themes at 900/1400 px. | Isolated incremental Qt/daemon links. Full pool-setup test remains failing at `check_full_capital_release` with `PAYMASTER_CAPITAL_OPEN_MAINTENANCE`, reproduced with the unchanged test and previous daemon. Full product builds/suites were not run. |
| 2026-10-16, `b66320e4cf` (includes `6ea86beba5`) | Five setup/CLI/Qt translation units compiled with MSVC 14.43/Qt 5.15.10; `paymaster_setup_tests` passed 29 cases / 605 assertions. Six selected Qt functions passed 25 cases: guided setup/retry (3), themes (1), preparation diagnostics (18), fee recovery (1), start approval (1), diagnostic refresh/privacy (1). Reserve setup reviewed in both themes. | Isolated incremental test links using existing dependencies. No normal product build, new daemon-based end-to-end setup run or full suite. The earlier pool-setup failure remains open; these frontend changes do not claim to resolve it. |

The detailed security record also contains real SQLite stale-backup and channel
binding reproductions, simulated DB transaction failures and SQLite page-cap/
commit-denial reopen tests. See TM-001 through TM-009 in the threat model.
Do not describe those as physical power-loss/ENOSPC coverage.

## Open boundaries

A matching wallet/checkpoint rollback, independent cloned stores, public-service
Sybil drain and traffic analysis remain explicit trust/operational limits.
Current full builds/suites, broad Qt regression results, independent review,
physical durability, real Tor and prolonged hostile-load acceptance need their
own evidence. Old upstream release results do not certify this integration.

## Traceability

Full earlier reports, test commands and work logs remain at the consolidation
parent; for example:

```sh
git show e7c2c8c4bd:doc/digidollar-paymaster-release-gate.md
git show e7c2c8c4bd:doc/digidollar-paymaster-testing.md
git show e7c2c8c4bd:doc/paymaster-threat-model.md
```

The former numbered design acceptance list is consolidated into the candidate
gates above; it is not silently marked complete. Update this document with
bounded revision-specific evidence rather than appending every local work log.
