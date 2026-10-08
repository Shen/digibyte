# Paymaster Qt flow audit — 2026-09-28

Scope: the working `feature/paymaster-ux-navigation` candidate, including the
subsequent client fixes. This is a regression review of GUI/Core transitions,
not release acceptance or proof that every Paymaster workflow is defect-free.

## Operator finance presentation (2026-10-08)

The old period cards repeated totals while the valuation paragraph mixed payment
models with the current-price estimate. Reserve fees were included in operating
costs but absent from that model breakdown, leaving the difference unexplained.
The new view leads with selected-period metrics and a source grid; reserve
maintenance is the bounded remainder after transfer-model costs. Native monetary
units remain authoritative. Comparison, optional USD valuation and current
reserve capital have separate sections. Period/wallet reset clears every metric.

Only Paymaster presentation/theme code changed; the existing finance RPC, ledger,
CSV workflow and polling remain shared. Eleven targeted Qt cases passed and
native dark/light captures were inspected. See the
[checkpoint](../digidollar-paymaster-testing.md#operator-finance-presentation-2026-10-08)
for scope and remaining normal-build checks. The HTML preview uses example data.

## DD Vault first-open contention (2026-10-08)

The Vault still reconciled positions and read wallet signing state, oracle/chain
state, transactions and mint status synchronously on Qt. It also queried signing
state again for every rendered row. A held-`cs_wallet` regression reproduced a
525-ms navigation stall, ended only by the test's 500-ms watchdog. This proves
lock contention can freeze the view; it does not identify the lock holder in an
operator's live session.

`DigiDollarPositionsWidget` now performs those existing backend calls in a single
retained-wallet worker. Refresh bursts coalesce, wallet/privacy generations reject
late results, and row rendering uses the captured signing state. Empty/active-only
vaults skip the pending-redemption transaction scan. No Core locking, reconciliation,
financial authorization, RPC or consensus rules were changed. The change belongs
in this native DD view: moving only Paymaster polling cannot prevent the Vault's
own synchronous reads from waiting, and a second Paymaster Vault would duplicate it.

The same navigation test now returns in 13 ms while the wallet remains locked.
Lifecycle coverage includes refresh bursts, masking, hiding, wallet replacement,
model/widget destruction, chain-lock contention and row painting under a wallet
lock. These are controlled regression measurements, not a full UI benchmark.
See the [test checkpoint](../digidollar-paymaster-testing.md#dd-vault-first-open-contention-2026-10-08).

## Original-code boundary review (2026-10-07)

Comparison: local `upstream/release/v9.26.7` at `d7265fb05e` against the
integration tree after `e05f4aa3ce`. This is a source-boundary review; upstream
release/portability changes are not presumed to be Paymaster changes. In
particular `init.cpp` has different line endings in that upstream blob: use
`git diff --ignore-space-at-eol` to distinguish line-ending churn from logic.

Three self-contained implementations now live in Paymaster-owned files:

| Previous location | Current boundary | Preserved behavior |
|---|---|---|
| `digibyte-cli.cpp`: operator menus, setup, status and monitoring | `paymaster/cli.cpp/h`, invoked with explicit arguments, RPC host and a per-invocation transport callback | Existing HTTP transport, argument conversion, secret cleansing, wallet/node binding, confirmations, finite budgets and monitoring. No second RPC client or global callback. |
| `qt/walletmodel.cpp`: Paymaster signing orchestration | `qt/paymasterwallet.cpp/h`, shared by the provider and client | Worker lock inspection/relocking, native unlock dialog, generation checks and shared `UnlockContext`. The generic model exposes only a retained-interface accessor; its DD async readers remain shared. |
| `wallet/load.cpp/h`: Paymaster startup reconciliation and recurring service implementation | Existing `wallet/rpc/paymaster_integration.cpp` and `paymaster.h`; loader retains initialization and scheduler calls | Same per-wallet startup order, 30-second maintenance and 1-second provider tick, two-wallet/four-message service bound and evidence pruning only after all wallets succeed. |

The CLI helper/workflow bodies were compared after accounting for indentation
and explicit instance context; the four maintenance/scheduler function bodies
were also compared unchanged. These are moves with narrow adapters, not copied
implementations. The generic `wallet/load.h` matches the upstream base again.
Two unused synchronous balance convenience getters were removed from
`WalletModel`; current views already consume the shared balance snapshot.

The remaining shared-code modifications were reviewed by boundary:

- **Coin selection, balance and rebroadcast** (`wallet/spend.cpp`,
  `digidollarwallet.cpp`, `wallet.cpp`): ordinary sends must respect Paymaster
  reservations and durable commits too. A check only in the Paymaster UI/RPC
  would leave ordinary wallet callers unprotected.
- **Wallet lifetime and database** (`walletmodel`, `walletdb`, database backends):
  shared ownership keeps asynchronous reads/unlock leases alive after model
  closure; wallet-batch hooks preserve atomic ordinary-wallet/Paymaster writes.
  Record implementations are already in `paymasterdb.cpp`. Duplicating batch or
  unlock implementations would increase drift and risk.
- **DD read/display paths** (Overview, Send, Receive, history): their coalesced
  background reads prevent the demonstrated lock waits even when the Paymaster
  page is hidden. Moving one copy into each Paymaster widget would reintroduce
  scans, blocked original pages or inconsistent wallet snapshots.
- **Transport and registration** (`net*`, protocol, init, RPC registration):
  dedicated connection admission, dispatch, privacy/deadline policy and lifecycle
  need hooks into the existing transport. Reusing it avoids a second SOCKS/P2P
  implementation. Wire versions, limits and authorization were not changed.
- **Native UI integration** (options, wallet view, RPC console, theme): existing
  settings/backup paths, connection labels and secret-history filtering remain
  centralized. A separate Paymaster copy would behave differently from the app.
- **Compiler/release integration**: existing platform compatibility and upstream
  DD/consensus/oracle changes are outside this extraction; no blanket reversion
  was made based only on filenames or diff size.

Targeted verification and the operator's full-build command are recorded in the
[test runbook](../digidollar-paymaster-testing.md). No live wallet was modified.

## UI blocking audit (2026-10-07)

This follow-up reviews `integration/paymaster-v9.26.7` after `b5f9395301`,
covering all provider destinations, setup and client Paymaster paths. Scope is
UI-thread lock/I/O waits, not a new payment-protocol audit or proof that every
live pause is gone. Changes concentrate on Paymaster Qt code; the signing bridge
originally added to `WalletModel` now lives in `qt/paymasterwallet.cpp` (see the
boundary review above). No Core lock, consensus,
payment authorization or RPC/CLI semantics change.

| Surface | Inspection/result |
|---|---|
| Provider Overview, Start/Pause and automation | RPCs use the serialized asynchronous dispatcher. Rendering uses saved snapshots, without wallet reads. |
| Funds & reserves, preparation, withdrawal, release and retirement | Preview/execution RPCs already run off Qt. Signing preflight/relocking used to wait on Qt; they now use the dedicated worker bridge. |
| Activity, manual processing and recovery | Queries/commands use the same dispatcher. Busy retries use bounded timers, not sleeps or thread joins. Reservation rendering remains local. |
| Income & costs | RPC history is paged at 250 rows. Final CSV serialization and `QSaveFile::commit()` previously ran synchronously; both now run on a worker. |
| Settings and setup assistant | Identity, policy, safety, refill and node-config operations use asynchronous calls. Exact reviews and wallet-bound continuation remain mandatory. |
| Client offers, safety and saved sessions | Reads use the asynchronous client wrapper, generation checks and cached balances. Visible offers are bounded to 100. `getAvailableDGBBalance()` is a cached read. |
| Client exact send and alternative recovery | The new signing bridge keeps lock-state inspection and temporary relocking off Qt. The ordinary modal unlock dialog is retained. |
| DD Overview / Send / Receive / transaction history | Existing worker reads and cached snapshots avoid automatic wallet-lock waits. Hidden page refresh guards remain. The preceding overview regression is recorded in the runbook. |
| Confirmations and progress | Modal Qt dialogs process events; disabled competing actions are deliberate. No production Paymaster `wait()`, blocking queued connection or sleeping retry loop was found. |

Remaining shared/native synchronous boundaries, retained to avoid a broad
rewrite of original DigiByte/DD behavior:

- `WalletView::backupWallet()` performs the full backup synchronously. Paymaster
  Wallet & backup currently delegates to it. The native unlock dialog also
  checks encryption status and decrypts synchronously; the new bridge does not
  replace that dialog. Both can still wait for Core or slow storage.
- Creating/persisting DD receive requests, ordinary own-DGB sending, coin-control
  queries and visible Mint/Redeem operations still contain synchronous
  wallet/chain reads or writes. Hidden-page guards protect Paymaster navigation,
  but explicitly using those native actions can encounter Paymaster-held locks.
  Vault reads were subsequently moved off Qt as recorded above.
- Initial DD activation reads `cs_main`; initial-download checks can also take
  it before their completed-sync latch is set. These are separate startup/sync
  boundaries, not normal provider polling.
- The explicitly selected operator-report import reads at most 64 KiB on Qt.
  Native file pickers and this small read can wait on a slow/disconnected file
  location. Large list rendering remains GUI work; its worst-case latency has
  not been benchmarked across all supported machines.

The encrypted-wallet preflight regression reproduced **502 ms** of blocking
before the correction, until a 500-ms watchdog released `cs_wallet`. The worker
version returned in **0 ms** in the same controlled test. Disposable encrypted
Core-wallet tests also cover unlock, cancellation, RPC error, wallet switch and model close;
results are delivered only after temporary relocking. The 10,251-booking export
checks both complete CSV rows and Qt heartbeats during final writing. See the
[test runbook](../digidollar-paymaster-testing.md) for targeted results and
operator build/full-suite commands. The normal EXE and live workload acceptance
remain separate from these checks.

## Corrected transitions

| Trigger | Defect | Corrected behavior / regression |
|---|---|---|
| Explicit unsigned provider fallback | The GUI switched to read-only polling and never requested the next offer. | Refresh Core capabilities, close the attempt, then continue the same unsigned request only if Core still permits resume. `paymasterClientFallbackContinues` also covers RPC failure, signed artifacts and a withdrawn resume capability. |
| Alternative recovery is waiting for its provider | A timer only observed the record and never advanced the next recovery transport phase. | The current explicitly started recovery repeats its bound prepare-only call, then requires exact approval before finishing the signed artifact. A two-minute bound, expiry/error/unknown-phase checks, stop and wallet changes end continuation. `paymasterClientLiveRecoveryProgresses`. |
| Explicit resume after transport failure | The retry did not request a fresh local transport attempt. | Only the explicitly initiated call gets `retry_transport`; subsequent polls do not repeat it. `paymasterClientSessionRpcActionsAreBound`. |
| Exact offer review followed by signing | RPC arguments were reconstructed from the compose controls. | Keep the original request arguments in memory across review, unlock, retry and fallback. A restored request cannot inherit Own DGB from the new form; Core-provided fee mode, privacy and gross/sweep semantics take precedence. |
| Wallet closes/reopens during review or unlock | RPC callback guards did not cover the nested modal event loop. | Check wallet generation again before continuing. Tests cover returning Yes after closing/reopening the same model, switching during a real encrypted-wallet unlock, unsigned cancellation, recovery preparation and the client fee-limit dialog. |
| Session list fails, is malformed, or has more pages | The failure was silently treated like an empty list. | Show an inline loading/retry state. Preparing another payment requires a complete successful list. Wallet changes discard old list replies. `paymasterClientSessionDiscoveryFailures`. |
| First preparation reply is lost after persistence | The GUI did not know a session existed and could strand the request. | Query the existing request UUID read-only before proceeding. `paymasterClientCoreCancellationRoundTrip` exercises actual Core listing, resolution and unsigned cancellation against a test wallet, including a lost reply after durable request creation. |
| Operator starts/pauses from the overview | The confirmation could outlive its wallet or accept another operation during the dialog. | Disable mutation controls during review, bind the confirmation to the wallet generation and validate the start response through the existing start-result path. `paymasterOperatorConfirmationWalletBinding`. |

No consensus, network protocol, Core authorization or financial journal changes
are part of this audit. The new tests use isolated test wallets; the operator's
running regtest wallets are not mutated.

## Verification

Targeted MSVC builds and test linking completed successfully. **72 scenarios
in 17 selected Qt test functions passed (zero failures/skips)** across the
current audit runs. The final recovery change was followed by reruns of live
sending, the action matrix, recovery expiry and the real Core round trip.
`git diff --check` also passes. This is not a full Qt/unit/functional suite run.
The GUI executable itself still has the older 12:50 build timestamp and must
be rebuilt before the operator walkthrough.


Targeted MSVC compilation and Qt regression results are recorded in the current
handoff. Each Qt test process reports two extra fixture cases (`initTestCase`
and `cleanupTestCase`); these are excluded from the reported scenario count.
The real Core round-trip test registers the wallet RPCs, ends RPC warmup without
relying on another test suite, and reads/writes the normal Paymaster store. Network replies in the asynchronous preparation tests
remain injected; they are not a substitute for an actual two-node payment.

## Remaining acceptance

Follow [the test runbook](../digidollar-paymaster-testing.md) with a freshly built
GUI and test executables. Full builds/suites, the live two-node walkthrough,
real Tor and the 24-hour operation test remain operator checks. Existing results
from earlier binaries do not cover this working candidate.

A resumed request remains bound by Core's canonical order. Some original
nondefault selection/fee-cap/attempt-limit inputs are not exported by the status
RPC, so their reconstruction after a process restart is not established by
these tests. Do not infer permission to change them or increase a budget.
Core rejects conflicting orders; use its safe unsigned cancellation before
preparing a fresh order when the original inputs cannot be reproduced. Signed
requests continue to use Core's exact-artifact retry/recovery actions.

## Repository guidance applied

The implementation follows `CLAUDE.md`, `CONTRIBUTING.md`, the root and `src/`
working instructions, the architecture/repository maps, developer notes, Qt
component/test guidance, C++ formatting rules and the translation-string policy.
New user-facing messages remain translatable in the existing Qt contexts.
Full builds and suites remain operator-run as required by the workspace guidance.

## Initial audit regression results (before the follow-up below)

| Test function | Scenarios passed |
|---|---:|
| `paymasterClientAuthorizationIsTwoStageAndFailClosed` | 1 |
| `paymasterClientCoreCancellationRoundTrip` | 2 |
| `paymasterClientFallbackContinues` | 5 |
| `paymasterClientLiveRecoveryProgresses` | 6 |
| `paymasterClientLiveSendProgressesAcrossAsyncPhases` | 11 |
| `paymasterClientMultipleRestartSessionsRequireSelection` | 1 |
| `paymasterClientMutationDialogWalletBinding` | 3 |
| `paymasterClientRecipientlessCancellation` | 12 |
| `paymasterClientRecoveryExpiryIsFailClosed` | 1 |
| `paymasterClientSessionActionMatrix` | 14 |
| `paymasterClientSessionDiscoveryFailures` | 6 |
| `paymasterClientSessionRpcActionsAreBound` | 1 |
| `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep` | 2 |
| `paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows` | 1 |
| `paymasterLiquidityPolicyDefaultsAndApprovalGuard` | 1 |
| `paymasterOperatorConfirmationWalletBinding` | 4 |
| `paymasterOperatorOverviewGuidesAndFailsClosed` | 1 |

## Follow-up: leaving exact-offer review and late discovery (2026-09-28)

Canceling exact-offer approval previously left an unsigned request paused. With
one provider, resuming after quote expiry could exhaust that request's provider
choices. Cancel now explicitly requests safe unsigned cancellation: refresh the
same request, require Core's permission, cancel once, validate terminal unsigned
closure, and return to payment settings with recipient and amount intact. A new
preparation gets a new UUID. Declining malformed/expired details or a wallet
change is not treated as cancellation. Signed or ambiguous outcomes remain
protected, and a lost cancellation reply is reconciled read-only.

The compose view previously retained the last directory snapshot even when a
new provider announcement arrived. A ten-second timer now reads known offers
while the Paymaster/Automatic compose view is visible, valid and idle. It does
not contact providers or reserve funds. Overlapping checks are coalesced, old
amount/wallet responses discarded, and background failures shown inline.

`paymasterClientReviewCancellation` covers cancellation, already-closed requests,
lost replies, refusal, a concurrent signature, malformed closure and
malformed/expired review details (eight rows). It checks input preservation,
fresh UUIDs and the absence of signature authorization.
`paymasterClientOfferAutomaticRefresh` covers delayed announcements, concurrent
reads, RPC/schema failures, hidden forms, privacy, Own DGB, invalid amounts,
changed amounts, wallet switches and active sessions.

These GUI checks do not prove why a particular P2P announcement arrived late.
The operator's nodes were shut down before read-only RPC inspection; their logs
also show transient provider synchronization/liquidity waits. An actual two-node
rerun remains necessary to measure discovery and payment behavior after rebuild.

Follow-up validation: all 53 selected scenarios passed with zero failures or
skips, excluding Qt init/cleanup fixtures. These comprise the nine new scenarios
and reruns of two-stage authorization (1), live sending (11), recipientless
cancellation (12), actual Core cancellation (2), mutation-dialog wallet binding
(3), offer-preview invalidation (1), and the session action matrix (14).
The changed Qt units and generated MOC files compiled with MSVC; the Qt library,
test executable and local `build_msvc/x64/Release/digibyte-qt.exe` linked
successfully. `git diff --check` passed. This was targeted incremental validation,
not a clean/full build or the complete functional/Qt suites.

## Follow-up: restart readiness and delayed status reads (2026-09-28)

An existing provider could show **Ready to start** while both overview buttons
were disabled. The initial refresh serializes provider info, liquidity,
operator info, pools, provider safety and client safety. The operator snapshot
arrives before the queue drains; the shared busy guard correctly keeps mutation
controls disabled until the remaining handlers finish. The intermediate
presentation previously did not explain that guard.

The overview now displays **Reading provider status…** with **Please wait…**
throughout pending reads, and restores the validated action once the queue
finishes. Mutations show a distinct operation-in-progress message. Saved
configuration text now points directly to Overview start/resume and describes
repeating setup as optional. Wallet-generation, privacy, schema validation and
Core start checks remain in force.

The operator reported about 30 seconds of waiting. There is no fixed 30-second
start-button timer in this GUI path. Read-only RPC inspection after that wait
found the live provider ready and stopped; seven relevant individual status
queries took 46–90 ms each. These later measurements do not identify which
startup query or callback caused the reported delay. This change fixes the
misleading presentation, not a measured startup-performance bottleneck.

A delayed test adapter now holds actual queue callbacks across Qt event
processing. `paymasterOperatorDelayedStartup` covers readiness, operator RPC
failure, malformed operator data, unrelated client-status failure and wallet
closure. It checks disabled controls until completion, absence of mutations,
late-response rejection and direct start availability without setup navigation.
An older readiness test mistakenly selected the client's session label; it now
selects the provider's `paymasterNextStep` label and retains its original
readiness assertions.

Targeted incremental MSVC compilation and Qt-library/test-executable linking
passed. Sixteen selected scenarios passed without failures or skips, excluding
Qt init/cleanup: delayed startup (5), overview diagnostics (1), operator
confirmation wallet binding (4), configuration workflows (1), liquidity/runtime
workflows (1), guided setup (2), external prerequisites (1), and current-status
start confirmation (1). `git diff --check` passed. The running operator GUI was
not stopped or replaced; its executable still needs rebuilding/relinking after
exit to include this follow-up. Full builds, full suites and a fresh interactive
restart timing remain operator acceptance checks.

## Follow-up: one provider control area (2026-09-28)

The overview now uses **Start provider**, **Resume provider** and **Pause
provider** consistently, including confirmation titles and instructions to
pause before editing a running provider. The duplicate technical-detail Start,
Stop and Enable/Disable configuration buttons and their obsolete handlers were
removed. This supersedes the earlier design's expert controls in that section.
The technical operation card links to liquidity diagnostics instead of starting
service; the operating-capital card retains its guarded, explicitly approved
start-and-refill action. Its eligibility no longer depends on an invisible
start button.

The main pause continues to call `stoppaymaster` with `persistent=true` and
`pause_setup=true`, preserving reservations and signed transactions. The old
argumentless temporary stop remains available through RPC. Core policy and
financial authority are unchanged.

Existing tests now exercise the remaining controls and assert that the removed
buttons are absent even when technical details are expanded. The overview test
checks Start/Resume/Pause wording and ownership by the top card. The pause
confirmation test includes an enabled provider with autostart enabled and still
requires one persistent stop operation; wallet changes during confirmation
prevent mutations. Obsolete shortcut fixtures were removed; safety, runtime
settings and pool-preview assertions remain.

Validation for this follow-up: targeted MSVC compilation, Qt-library linking
and test-executable linking passed. All 17 selected Qt scenarios passed without
failures or skips (excluding init/cleanup): overview (1), wallet-bound operator
confirmations (4), external readiness (1), configuration workflows (1),
liquidity/runtime workflows (1), safety defaults (1), delayed startup (5),
guided setup (2), and manual activity (1). `git diff --check` passed. The full
build/full suites and interactive GUI acceptance remain operator checks; the
running GUI executable was not replaced.

## Follow-up: Connection & wallet sizing (2026-09-28)

The new Connection & wallet scroll area omitted `setWidgetResizable(true)` and
installed its content before all cards were added. The first rendered content
could remain only 235 pixels wide inside an 862-pixel viewport, clipping and
compressing the later sections. The page and its content were also missing from
the explicit dark/light Paymaster theme selectors.

The page now resizes its content with the viewport and installs the complete
layout after all sections are added. Both themes include the connection page,
its form controls and the nested Settings pane. No RPC, wallet or provider
operation changes are involved.

`paymasterConnectionLayout` exercises initial display, expansion/collapse,
resizing and revisiting the page in both themes at two window sizes. It checks
actual content width, nonoverlapping sections, wrapped-text height, scroll
reachability and the themed background. The dark/narrow row failed against the
previous implementation with the 235/862 width mismatch before applying the fix.

Validation: the changed Qt implementation, test/MOC units and regenerated Qt
resources compiled; Qt-library and test-executable linking passed. All four
layout rows passed at both 100% and 150% Qt scaling (eight row executions, no
failures/skips). Native dark/light screenshots were inspected. `git diff
--check` passed. Repository C++/Qt, theme and test guidance was followed; full
builds/suites remain operator checks. The running GUI executable was not
replaced and requires rebuilding/relinking after normal exit.

## Follow-up: visible status-loading activity (2026-09-28)

Pending provider reads now show a themed indeterminate progress bar and the
current check (settings, operating capital, service/wallet status, prepared
reserves, spending limits or client fee limits). A monotonic elapsed-time label
updates once per second throughout the consecutive reads. This is activity,
not an estimated percentage: the follow-up query count can vary on errors and
partial refreshes. No display tick sends an RPC or grants an action.

The indication stops and clears when the queue completes or the wallet changes,
including late replies from the previous wallet. Privacy hides the text and
stops its display timer; leaving privacy during the same pending read resumes
it. Mutations and confirmation dialogs retain their separate busy message.
The asynchronous startup regression checks each stage, a delayed reply with
changing elapsed time but no extra RPC, success, early/late RPC errors,
malformed status, privacy and wallet closure. Qt/C++ and translation guidance
applies; new UI strings use `tr()` and translation catalogs are unchanged.

Validation: targeted MSVC compilation of the implementation, test unit and
regenerated Qt resources passed, as did Qt-library and test-executable linking.
All 11 selected Qt scenarios passed without failures or skips (excluding
init/cleanup): delayed startup (6), overview (1), and wallet-bound operator
confirmations (4). Native dark/light loading screenshots were inspected.
`git diff --check` passed. Full builds/suites remain operator checks; the GUI
executable was not replaced and requires rebuilding/relinking to show this change.

## Follow-up: compact client offer status (2026-09-28)

The compose view separates a short public-offer count from the local check time
and automatic-refresh notice. Refresh uses its natural button width; secondary
copy uses normal text weight in both themes. One short preparation hint replaces
the repeated process explanation, with details in its tooltip. Client monetary
labels display exact two-decimal DD values without duplicate cent values; RPC
parameters and all financial limits are unchanged.

A positive directory snapshot is informational, not proof that fee, privacy or
transport checks succeeded. `getpaymasteroffers` reads public candidates for
the amount with the general maximum fee; it does not apply the request's fee
cap or privacy profile and does not open Direct transport. Preparing a payment
rechecks current state and may reserve DD, but still requires explicit exact
provider/fee approval before signing. A missing or empty optional preview is
therefore not used as a preparation gate. The live-send regression adds an
empty-preview case followed by a newly available provider and canceled exact
approval; every preparation RPC must remain `prepare_only` with no signing
commitment.

Repository Qt/C++, translation, architecture/map and test guidance applies.
The targeted checks also cover status layout at narrow/wide sizes in both
themes, preview expiry, wallet/input invalidation and privacy masking of the
separate timestamp.

Validation: targeted MSVC compilation and Qt-library/test-executable linking
passed. All 18 scenarios across seven selected Qt functions passed with no
failures/skips (excluding init/cleanup): offer preview (1), preview invalidation
and errors (1), automatic refresh (1), live-send phases (12), two-stage
authorization (1), accessible client controls (1), and DD amount presentation
(1). The preview test covers both themes at 900/1700-pixel widths; native
screenshots were inspected. `git diff --check` passed. The GUI executable was
not replaced; a GUI rebuild/relink is required. Full builds/suites and live
network acceptance remain operator checks.

## Follow-up: visible discovery outcomes (2026-09-28)

The client directory preview now has a dedicated status card: a 16-point
headline and large checkmark for found public offers, the existing rotating
status-bar animation for a pending check, and distinct neutral/refresh/error
symbols for empty, expired and failed checks. Preparation information and the
check time remain normal-weight secondary text. The success styling denotes
finding an announcement, not authentication, eligibility or payment approval.

The spinner uses a local presentation timer and the existing theme-tinted
animation frames. It starts only while the pending check is visible, pauses
when hidden or in privacy mode, and stops on completion, changed inputs or
wallet closure. It neither adds RPCs nor changes send permissions. Privacy
hides the outcome icon and its accessible name as well as the existing text.

The preview/layout and delayed-refresh tests cover the hierarchy in both
themes at narrow/wide sizes, actual frame changes, no extra RPCs, all outcome
symbols, and animation cleanup. Existing repository Qt/C++, translation,
architecture/map and test guidance applies; no translation catalogs or Core
protocol/financial behavior were changed.

Validation: targeted MSVC compilation of the client widget, its embedding form,
tests and regenerated Qt resources succeeded; Qt-library and test-executable
linking succeeded. All 16 scenarios in five selected Qt functions passed with
no failures/skips (excluding init/cleanup): preview/layout (1), invalidation
and errors (1), asynchronous automatic refresh/animation (1), accessible client
controls (1), and live payment phases (12). Layout checks cover 900/1700-pixel
widths in dark/light themes. Native found/searching screenshots were inspected.
`git diff --check` passed. Full builds/suites remain operator checks; the GUI
executable needs rebuilding/relinking to display the changes.

## Follow-up: preparation requires a current offer (2026-09-28)

This supersedes the earlier optional-preview behavior for new requests in
explicit Paymaster mode. Prepare payment now stays disabled until a complete,
nonempty directory reply is available for the current inputs and has not
expired. Checking, empty/error/malformed replies, input changes and wallet
changes revoke that readiness immediately. A guarded send entry also prevents
a direct invocation from bypassing the disabled button or reusing a preview
for a different amount. It checks expiry even before the queued timer callback
has run. The existing ten-second refresh can enable preparation when a public
offer appears; it never prepares or reserves funds automatically.

The gate is a compose-view prerequisite, not proof of provider eligibility or
payment authorization. Core still validates fee, privacy, transport and the
exact offer; explicit approval remains required before signing. Automatic
mode may use own DGB first. Existing-session resume, cancellation and recovery
remain governed by Core capabilities, independently of the public preview.

A focused asynchronous regression exercises no result, pending/empty/failed/
malformed checks, a newly found offer, changed inputs, late replies, expiry
before callback delivery, wallet closure and the Automatic/Own DGB paths.
Live-send and cancellation tests explicitly obtain a preview before starting
a new request. Qt/C++, translation, architecture/map and test guidance applies;
Core policy, RPC formats and translation catalogs remain unchanged.

Validation: targeted MSVC compilation of the client, embedding form, tests and
regenerated test MOC succeeded, as did Qt-library/test-executable linking. All
33 scenarios across nine selected Qt functions passed without failures/skips
(excluding init/cleanup): preparation gate (1), preview/layout (1), preview
invalidation/errors (1), automatic refresh/animation (1), live-send phases
(12), review cancellation (8), actual Core cancellation round trip (2),
session-discovery failures (6), and two-stage authorization (1).
`git diff --check` passed. Full builds/suites and live-network acceptance remain
operator checks. The running GUI was not replaced; a normal incremental Build
is required to relink it with this change. Clean/Rebuild is not required.

## Follow-up: DD/percentage comparisons and stopped providers (2026-09-28)

Implemented the fee presentation first: the client edits an absolute DD ceiling
with unchanged integer-cent value/signals, plus an informational percentage of
the recipient amount. Changing the amount never increases that ceiling. When
deducting fees, comparison waits for the exact recipient amount. Actual offer
fees show both DD and effective percentage. Settings and guided setup retain
the provider percentage tariff and share a local recipient-amount example
using `ComputePaymasterFee`, including cent rounding and Core range validation.
The example neither marks configuration dirty nor changes budgets.

The new locale-aware amount control parses cents exactly, rejects extra decimal
places/grouping/exponents, and disables QSpinBox's default grouping fixup. A
regression initially exposed that Qt repair behavior converting malformed text
into a larger amount; the correction preserves the previous ceiling.

The reported lingering provider is explained by the current signed announcement
TTL of 600 seconds. `Directory::List(now)` filters expiry; re-reading the list
does not refresh it. Stopping a provider only stops its local service, without
a withdrawal message. This behavior is unchanged. The GUI labels the timestamp
List updated and the expiry Announcement expires, states that the connection
is not checked yet, and explains the ten-minute lifetime in the tooltip.
Transport failures now have specific endpoint/proxy or secure-connection
messages, retaining the read-only persisted-session refresh and all signing
guards. No extra probes, automatic retries, financial rules or wire changes.

Relevant guidance: CLAUDE reading order and current Paymaster architecture/map,
CONTRIBUTING, src/AGENTS.md, developer notes, Qt/test READMEs, translation policy,
EditorConfig/clang configuration and Windows build guidance. New UI text uses
translation markers; catalogs are unchanged. The new header is listed in the
Qt build manifest and repository map.

Validation: targeted MSVC compilation and Qt-library/test-executable linking
passed. Across the scoped runs, all 29 selected Qt scenarios passed: fee input
and comparisons (1), preview/layout (1), preview errors/invalidation (1),
preparation gate (1), automatic refresh (1), live-send phases including the
reported unreachable-endpoint error (13), unsigned review cancellation (8),
guided setup (2), and accessibility (1). Two new error rows initially expected
one status read but explicitly performed a second read-only poll; the corrected
assertion and both rows passed on rerun. The final provider translation-context
change was followed by successful fee-comparison and guided-setup reruns.

The unchanged Core directory suite additionally passed all 10 selected cases
and 80 assertions using the existing local Core test binary, including expiry
and replay handling. No Core source changed. `git diff --check` passed.
The tests use isolated wallets and injected network outcomes, not the operator's
running nodes. Full suites and the actual two-node stop/restart/payment check
remain operator acceptance. Normal incremental Build is needed to relink the
GUI; no Clean/Rebuild or dependency rebuild is required.


## Follow-up: funding availability in all fee modes (2026-09-28)

This supersedes the earlier Automatic exemption from the compose offer gate.
Own DGB requires a positive cached spendable DGB balance. Automatic requires
that balance or a current offer. Its action is Send payment with own DGB,
otherwise Prepare payment; with neither funding source it is disabled. The
explicit Paymaster gate is unchanged. Wallet balance notifications update the
button and hints immediately, and old-wallet connections are removed on switch.
The send entry points recheck availability; restoring the direct-send controls
also reapplies the gate instead of unconditionally enabling the button.

The cached balance is a preliminary UI check, not a fee quote or proof of
suitable fee inputs. Core's exact funding preflight remains authoritative.
Automatic preparation without own DGB freezes fee_mode=paymaster in the
existing request template: DGB arriving during preparation cannot turn a
Prepare payment click into direct-spend authorization. Automatic with own DGB
retains its initial send confirmation. Exact Paymaster approval, reservations,
recovery capabilities and existing-session actions remain Core-controlled.

The offer-gate regression matrix now runs for explicit Paymaster and Automatic
without DGB. A separate test empties only an isolated fixture wallet ledger and
uses real balance polling/signals to verify both balance directions, labels,
Own DGB with an available provider, direct-call guards and wallet detachment.
The live Automatic preparation row adds DGB after preparation starts and proves
the exact request and Paymaster mode remain unchanged through offer review.

Relevant guidance: CLAUDE required reading and current DigiDollar/Paymaster
architecture and map, CONTRIBUTING, src/AGENTS.md, developer notes, Qt/test
READMEs, translation policy, EditorConfig/clang configuration and Windows build
guidance. New strings use translation markers; translation catalogs are unchanged.

Validation: targeted MSVC compilation of both affected widgets, the Qt tests
and generated test MOC, followed by Qt-library/test-executable linking, passed.
After the final source change, the final run passed all 24 selected Qt scenarios
with no failures/skips (excluding init/cleanup): offer gate in both modes (2),
funding balance changes (1), live-send phases (14), preview/layout (1), automatic
offer refresh (1), and existing-session fallback (5). Logs are workspace-local
`.ai/funding-final-*.log`. `git diff --check` passed.

No full suite, full GUI build or live two-node acceptance was run. Tests use
isolated fixture wallets and injected transport outcomes. The operator's nodes
were not touched. Normal incremental Build is required to relink the GUI; no
Clean/Rebuild or dependency rebuild is needed.


## Follow-up: saved-session loading and reused inputs (2026-09-29)

The operator's read-only RPC returned PAYMASTER_RESERVATION_SESSION_CONFLICT.
A new regression uses real wallet/store/RPC calls: create a session, reserve an
input, abandon it unsigned, create a second session and reserve the released
input again. The old wallet library reproduced the exact RPC error (-4). The
old FAILED record retains historical user_inputs, and the old ownership query
mistook the new reservation for inconsistent ownership of that old request.

ClientSessionHasLiveReservations now recognizes the historical binding only
when the old session is FAILED with no pending phase, final/recovery txid,
recovery records, signed attempt evidence, authorization or provider commit.
The current owner must be a separate nonterminal client session with matching
request/session IDs, input membership, USER_DD role and session index. Unreadable
evidence and partial/malformed ownership still fail closed. This inspection
never erases reservations, unlocks coins, changes sessions or starts a payment.
No database migration or consensus/protocol change is involved.

The Qt inbox now shows an indeterminate progress bar and Loading transfers
while pending. Failures retain the detailed RPC error or a specific schema/page
reason as selectable plain text. Explicit retries report repeated failures,
whereas automatic startup errors stay inline. Privacy masks error details and
suppresses retry dialogs if enabled while a request is in flight; wallet-bound
callbacks continue to reject stale replies. Genuine reservation conflicts
explain why retrying unchanged data cannot repair the cause.

Guidance: CLAUDE required reading, current DigiDollar/Paymaster architecture and
repository map, wallet integration, CONTRIBUTING, src/AGENTS.md, developer notes,
Qt and test READMEs, translation policy, EditorConfig/clang configuration and
Windows build guidance. Diagnostics use the established Qt translation context;
raw RPC errors are plain text. The operator's actual wallet data was not changed.

Validation: the new real-Core regression first failed against the previous
wallet library with the operator's exact PAYMASTER_RESERVATION_SESSION_CONFLICT
(-4), then passed with the ownership correction. It also verifies rejection of
nonterminal/signed/incomplete historical state, partial identity matches, wrong
roles and owners without input membership, and preservation of still-owned
reservations, the new owner's record and its wallet coin lock.

Targeted MSVC compilation/linking of the wallet source, both affected Qt widgets,
the Qt regression tests and test MOC passed. The final run passed all 17 selected
scenarios across eight functions, with no failures/skips (excluding init/cleanup):
real-Core input reuse (1), discovery/retry/privacy/stale-reply failures (8), real
Core unsigned cancellation including a lost first reply (2), multiple restored
sessions (1), recipientless restart (1), preparation gates (2), and live exact
review cancellation/approval (2). Logs: `.ai/session-discovery-repro-before.log`
and `.ai/session-discovery-final-*.log`. `git diff --check` passed.

No complete suite, full application build or test against the operator's live
wallets was run. Normal incremental Build must relink the application executables
with the changed wallet and Qt libraries. Re-running msvc-autogen.py, Clean or
Rebuild is not needed for these existing-source changes.


## Follow-up: finite pool restoration and fee diagnostics (2026-09-29)

Operator evidence: after releasing an operational carrier, its replacement was
accepted but remained `pending_creation` with `PAYMASTER_POOL_FEE_LIMIT`, no
transaction ID, a 20,000,000-satoshi approval and zero recorded actual fee. Three
admission carriers and one adequate operational DGB slot remained available.
This is not evidence of a transaction waiting for confirmation. The older error
combined fee validity, the approved ceiling and the planner estimate; it could
not establish the exact failing bound from the supplied response.

Corrections:

- Shared operator diagnostics use durable preparation steps before missing-slot
  symptoms. Fee/funding/policy problems lead to operating capital; confirmation
  waits are distinct; unknown states and conflicts remain diagnostic failures.
- Qt shows active preparation details on Operating capital and the overview,
  including approved setup limits, saved transaction IDs and exact diagnostics.
  The service text no longer infers autostart from `waiting_for_readiness`.
- Explicit cancellation is offered only for a single wholly uncreated plan.
  Core's existing reconciliation/cancellation path retains a transaction saved
  concurrently. Confirmation decline, privacy changes and wallet switching
  prevent the RPC. No automatic fee increase, new approval or replacement follows.
- The manual preparation fee field uses exact satoshi parsing and is part of
  preview binding. Empty/out-of-range input or any edit revokes execution;
  the returned preview must echo the requested fee ceiling.
- Finite setup retains its existing fee checks, but distinguishes invalid fees
  and exceeded planner estimates from exceeded approval ceilings. Pool and
  operator JSON keep the stable fee-limit token and add optional `diagnostic`
  text with the checked phase and amounts. Journal format, signing and commit
  conditions, recurring budgets and consensus rules are unchanged.

Guidance: CLAUDE/DigiDollar architecture and maps, contribution/developer notes,
src/AGENTS, Qt and test guidance, formatting/translation policy, and the current
finite pool setup/operator specifications. No live operator wallet was changed.

Validation: targeted MSVC compilation of shared setup, provider RPC, Qt provider
and Qt tests plus the generated test moc, followed by static-library and Qt-test
linking. The final targeted test executable passed 32 scenarios in eight test
functions, with zero failures/skips:

- `paymasterPoolPreparationStoredFeeDiagnostics` (real isolated wallet journal,
  legacy and detailed fee errors, read-only round trip into Qt);
- `paymasterPoolPreparationDiagnostics` (13 rows, actual shared Core mapper);
- `paymasterPoolPreparationCancellation` (5 rows, injected RPC including a
  concurrent saved-transaction result);
- `paymasterInjectedRpcCoversLiquidityAndRuntimeWorkflows` (fee edits and exact
  preview binding, alongside existing preparation/retirement/runtime contracts);
- `paymasterOperatorOverviewGuidesAndFailsClosed`;
- `paymasterLiquidityMaintenanceStatesAreReadable`;
- `paymasterOperatorDelayedStartup` (6 rows);
- `paymasterOperatorConfirmationWalletBinding` (4 rows).

Logs: `.ai/pool-recovery-final-*.log`. The initial test compile needed a missing
QPlainTextEdit include; the expanded preview test also exposed an obsolete fixed
RPC-history index, corrected to assert the latest retirement call. Final scoped
runs passed. Existing theme-property warnings remain in the overview test.
`git diff --check` passed.

The full application build, complete suites and funded end-to-end withdrawal /
restoration scenario were not run locally. Rebuild normally (no Clean/Rebuild or
routine msvc-autogen), restart the provider, and allow its next 30-second
maintenance check to update the old fee diagnostic. Review that result before
changing approval. The existing `wallet_paymaster_pool_setup.py` functional
scenario remains the operator's broader check using freshly built binaries and
the documented test runbook; a higher limit must never be presumed necessary
from the old generic code alone.

## Failed first preparation and paused providers (2026-10-01)

A DD coin-selection failure could occur before Core created a session. Qt then
looked up its local UUID, treated the old generic missing-session error like an
unreadable wallet and left only Check current status available indefinitely.

The shared RPC lookup now preserves missing/read/version distinctions for both
request and session IDs. A dangling index remains a read failure; an unreadable
live session never falls back to its completion tombstone. After an exact
confirmed-absence response, Qt clears an unpersisted local attempt and keeps the
recipient and amount editable. A previously observed session is never cleared
this way. An old generic missing-session response remains protected. The
Paymaster automatic quote path also forwards upstream's precise DD selection
error, with a stable GUI/CLI category.

`paymasterClientUncreatedRequestReturnsToCompose` covers the reported early
input failure, detailed selection errors, unreachable providers, legacy absence,
read failure and unsupported versions. It uses real Core lookups for confirmed
absence and database read failure, verifies no durable mutation and requires a
new UUID only after an explicit new send. The live-send regression adds a known
session disappearing; existing lost-reply and unsigned-cancellation round trips
remain required. Store tests cover request and session-ID lookup, corrupted
records, future versions, completion retention and dangling indexes. The RPC
functional test uses both HTTP RPC and actual CLI calls with a paused provider.

Targeted compilation succeeded; rebuilt C++/Qt and functional execution remains
pending as recorded in the test runbook. No running operator wallet was changed.
