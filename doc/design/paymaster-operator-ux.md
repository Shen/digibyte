# Paymaster: operator experience

Status: guided three-area implementation candidate, 2026-09-29. This changes presentation, not spending authority or protocol. The HTML preview uses example data only.

## Daily operation

Use three destinations: Operation, Activity & finances, Settings.
Settings contains Offer, Spending limits, Automation & reserves, and Connection & wallet. Paying with a
Paymaster remains in the normal DigiDollar Send flow; this panel operates a provider.

Operation answers three questions: Is the provider running? What needs attention?
What should I do next? Show one context-sensitive primary action, with persistent
pause as a secondary action while running. Readiness, external reachability and
confirmed payment are independent observations. A locally ready provider is not
labelled externally verified. Unknown or stale status never enables a start.

Avoid a second dashboard below the first. Show brief capital, budget and financial
summaries; place detailed prerequisite checks and raw RPC diagnostics behind
disclosures. Keep Start provider / Resume provider and Pause provider together
at the top; do not duplicate them in technical details. Put wallet backup and
connection controls on Connection & wallet, together with runtime processing
and explicit autostart.
Retain a visible short overview reminder. Starting/resuming enables the saved
configuration; pausing disables it persistently. The temporary stop remains an
RPC expert action. Outstanding work and recovery remain accessible even if
setup is unfinished.

## Information hierarchy

- One page title, one purpose sentence, one primary task per section.
- Normal overview: human-readable state, next action, local connection status,
  wallet lock state and backup reminder. No raw diagnostic codes or provider IDs.
- Errors use text and symbols; colors are supplementary. Unknown codes remain in
  technical details, accompanied by an actionable unknown-status notice.
- Exact financial values keep their currency. Read-only compact values may trim
  trailing zeroes, never round away meaningful digits. Editable ceilings retain
  their exact value. DD revenue and DGB cost are never directly subtracted.
- Show payment-model budgets separately; no combined total implying one shared
  cap. Keep reserved exposure distinct from spent amounts. Inactive models do
  not occupy the overview. Hour/day caps remain rolling windows.
- Show per-transfer/hour/day fee limits first. Concurrent reservations and count
  limits remain available in expanded controls, with saved values preserved.
- Show one selected finance period. Comparative periods, valuation and capital
  details are secondary. Incomplete historical coverage remains visible.

## Flows and wording

New wallet: Choose wallet → Check node → Configure endpoint → Offer and finite
limits → Review capital, maximum setup fees, recurring limits and optional start
→ Approve once → Automatic execution and confirmations. Resume existing setup without another identity or
duplicate pool approval. No new defaults overwrite persisted values.

Stopped and ready: “Ready to start” → “Start provider”. Locked: “Waiting for
wallet unlock” → “Unlock wallet”. Funding pending: “Waiting for confirmations”
→ “Review operating capital”. Budget exhausted: “Spending limit reached” →
“Review limits”. Never suggest raising a budget as the automatic remedy.

Pause explicitly persists disabled provider/autostart and stops new signatures;
already signed transactions can still confirm. Unlock is wallet-wide, bounded
and never automatically renewed. Startup and financial confirmation dialogs keep
all existing Core checks and explicit consent.

Helpers explain consequences beside inputs. Tooltips supplement, never replace,
important instructions. Keyboard focus, accessible names, selectable diagnostics,
privacy mode and wallet-switch invalidation apply to all new controls. Keep
technical material collapsed initially and preserve responsiveness at narrow
window widths. New Qt source strings use tr(); translator catalogs follow the
repository translation process.

## Review and validation

The HTML preview documents the earlier five-tab candidate; the three-tab Qt implementation is authoritative. It
is not connected to a wallet. Qt regression checks must cover navigation into
nested settings, one primary action, no raw codes in the hero, unknown/stale
status, privacy, correct financial period, and existing approval guards.
Full builds and full test matrices remain operator work per AGENTS.md. Record
actual targeted results below; earlier tests do not validate this redesign.

## Operator acceptance commands

Working directory: `D:\Digibyte\digibyte-fork`, branch
`feature/paymaster-ux-navigation`. Use Qt 5.15.10 and the configured MSVC/vcpkg
installation. Run the full Windows build from
[the build runbook](../digidollar-paymaster-testing.md) first (minutes to tens
of minutes). A full build is not replaced by the targeted object compilation
used while developing this UI.

Then run both affected Qt groups on the Windows desktop (seconds to a few
minutes). Success means exit code 0 with no failed test cases for each group:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:DIGIBYTE_QT_TEST_FUNCTION -ErrorAction SilentlyContinue
Remove-Item Env:DIGIBYTE_QT_TEST_OUTPUT -ErrorAction SilentlyContinue
try {
    foreach ($suite in @('PaymasterWidgetTests', 'DigiDollarWidgetTests')) {
        $env:DIGIBYTE_QT_TEST_SUITE = $suite
        & .\build_msvc\x64\Release\test_digibyte-qt.exe
        if ($LASTEXITCODE -ne 0) { throw "Qt regression failed: $suite" }
    }
} finally {
    Remove-Item Env:DIGIBYTE_QT_TEST_SUITE -ErrorAction SilentlyContinue
}
```

Interactive review: select an existing provider wallet; open all three areas and their nested pages;
verify nested settings, a locked wallet, exhausted/pending states and privacy;
review a financial period, expand exact limits and cancel a mutation preview;
confirm that the ordinary pause explains persistent autostart disablement.
Check both themes and a narrow window. Real Tor/payment and 24-hour acceptance
remain the separate Core/operator release gates, not claims of this redesign.

## Recorded candidate verification (2026-09-28)

Base: `cac8e521e71ebce29a5afdcbb636ba417350bf05`; working branch:
`feature/paymaster-ux-navigation`.

- Recompiled the affected Qt implementation and tests with MSVC 14.43 / Qt
  5.15.10, regenerated the affected MOC and Qt resources, and relinked the Qt
  archive, test executable and GUI. Each completed successfully. This was
  targeted compilation/linking, not a completed full solution build.
- Eleven selected Qt functions passed (twelve cases including the two guided
  setup rows): operator overview; finance/backup; nested navigation and setup
  gating; liquidity maintenance; manual activity; local readiness; default
  safety; finite/disabled safety semantics; configuration RPC flows;
  liquidity/runtime RPC flows; and bounded setup with retry. Existing financial
  approval guards were exercised through injected RPCs.
- The operator overview check covers unknown/incomplete status, wallet locking,
  expired unlock time, budget routing and privacy. Its text contrast checks
  pass in both dark and light themes. Native Qt screenshots were reviewed.
- The standalone HTML preview passed browser interaction checks for navigation,
  representative states, confirmation/cancellation, setup completion, finance
  periods and sponsorship. HTML IDs are unique; it performs no wallet calls.
- The separate general `DigiDollarWidgetTests::digiDollarControlsStayReadableInBothThemes`
  test still reports two dropdown-selection color failures, one per theme:
  `highlight.green() > highlight.blue()`. The same two failures were reproduced
  after temporarily embedding the original base-revision CSS. Candidate
  resources were restored afterwards. This test does not instantiate the
  Paymaster operator panel; its failure remains an open general theme issue.
  No full Qt regression pass is claimed, and the assertions were not weakened.

The full current-source build, both complete Qt groups and the interactive
wallet walkthrough above remain pending operator verification. Expect the known
baseline dropdown test failure until that separate issue is resolved; it must
not be silently counted as a pass. Existing protocol, Tor and soak-test gates
remain applicable separately.

Repository and `src/` guidance, the DigiDollar architecture/maps, Qt developer
and test guidance, Windows build instructions and the translation-string policy
were applied. New UI strings use `tr()`; translation catalogs were not edited
manually. No consensus, wire format, financial journal or Core authority changed.

## Client continuation correction (2026-09-28)

The operator's two local regtest nodes exposed a separate pre-existing send-UI
stall: discovery and eligible offers succeeded, and the diagnostic Direct check
passed both v2 and Paymaster negotiation, but the first asynchronous send stayed
at `CREATED`. The old timer only refreshed durable state; it never completed the
pending connection/quote request. No payment was signed in the observed session.

`PaymasterSendWidget` now keeps the exact live request in memory and advances it
within a two-minute bound, retaining the separate exact-fee/signature dialog.
Approved continuation additionally requires the same commitment and a verified
signed artifact; when the send reply omits artifact metadata, Qt obtains the
read-only authoritative envelope first. Restarts, wallet changes, errors and
cancellation do not rearm this continuation. Financial journals and Core signing
checks are unchanged. The client guide explains how to cancel an old unsigned
stalled request before trying again. Rebuild and restart both GUI processes
before the operator repeats the real regtest payment.

Targeted verification: the modified send implementation, embedding form and Qt
tests compiled successfully. The five new live-send cases (cancel exact review,
authorized completion, stop, RPC error, wallet close) pass. Existing exact
approval, recipientless restart, RPC-action binding, delayed callback and
14-row action-matrix checks also pass: 23 selected cases total, exit 0. The
regtest diagnostics above were read-only apart from the bounded transport probe;
no operator payment, cancellation or signature was issued by the agent. The
operator still needs to retry the actual payment using the new GUI and perform
the full current-source build/regression gates.

## Offer discovery and refresh presentation (2026-09-28)

Provider announcements arrive automatically at the node. The visible client
preview is an explicit local snapshot, not a new network search or a connection
probe. Its status and check/refresh action now sit outside Advanced settings.
Only an in-flight check shows an indeterminate progress bar; both nonempty and
empty results explicitly finish and show the check time. Input changes and the
earliest displayed announcement expiry invalidate the preview. Errors stay
separate from an empty result. No automatic preview RPC polling was added.

Live payment preparation describes waiting for a slot, connecting and verifying
the offer separately, and says when it continues without refresh. Exact-offer
review is labelled complete; paused preparation points to the current session
actions. No preview can select another provider or advance an active payment.
New strings follow the Qt translation policy; Core and wire behavior are unchanged.

Verification: rebuilt the changed client widget, embedding form, generated MOC
and Qt tests, and relinked both test and GUI executables successfully. Ten
selected cases pass (exit 0): offer-check RPC/progress/completion/expiry, stale
inputs and error handling, five live-send outcomes, recipientless restart,
exact authorization and accessible controls. The expiry check confirms that
invalidating a preview performs no additional RPC. Complete solution builds and
a manual walkthrough with the operator's nodes remain separate acceptance work.

## Recipientless unsigned cancellation (2026-09-28)

The operator's initial request had already been canceled in Core: FAILED,
final, no provider attempt, artifact or reserved inputs, no attention required,
and only refresh permitted. Qt accepted the recipientless CREATED record but
rejected its recipientless FAILED successor. This left the old form locked.

The decoder now accepts that narrowly proven closed snapshot. A successful
unsigned cancellation releases the form; a read-only refresh after a lost
response offers Start a new transfer without repeating cancellation. A generic
FAILED label or missing recipient is never sufficient proof. Core's no-attention
classification, finality, unsigned artifact/attempt state and permitted actions
are checked, along with transaction/broadcast/confirmation consistency. A
recipientless closure additionally requires zero attempts and empty reserved
inputs. Malformed or signed snapshots retain protection. An unavailable cancel
response is described as uncertain and points to a status check, rather than
claiming cancellation was refused.

Repository C++/Qt and translation guidance applies; Core storage, signatures,
wire protocol and spending limits are unchanged. Live-wallet checks were read
only. The running operator nodes were neither stopped nor mutated by the agent.

Verification: targeted MSVC compilation of the client, embedding form, affected
MOC objects and Qt tests, followed by library/test linking, succeeded. All 35
selected test cases pass (exit 0): 12 cancellation rows, recipientless restart,
14 action-matrix rows, bound RPC actions, five live-send rows, two-stage exact
authorization, and stale callbacks after wallet close. `git diff --check` passes.
The running GUI executable has not been replaced because the operator's nodes
remain open. Close both Qt instances normally before the next GUI build. Full
solution/Qt regression and a new real payment remain operator acceptance work.

## Fee-choice radio rendering (2026-09-28)

The existing theme used a browser-style `QRadioButton::indicator:checked::after`
rule to draw an inner dot. With Qt 5.15.10 on Windows this also reduced the
indicator to the rule's six-pixel dimensions. Combined with the fee card's
six-pixel selected border, it rendered as a small square. A standalone Qt
render using the complete theme reproduced the operator screenshot.

Both themes now draw the dot with a Qt radial gradient, keep border thickness
constant across selection states, and use a circular radius. Fee cards have
explicit disabled colors while retaining selected/unselected distinction.
No payment behavior or new translation strings are involved. Repository Qt
resource/style guidance applies. Actual Qt renders were checked at 100%, 150%
and 200% scale in both themes, and the resource compiler/library/test relink
succeeded. The running GUI executable still needs rebuilding after both
operator Qt instances have been closed normally.

The existing `digiDollarAmountLabelsUseCurrencyPrefix` case passes with rebuilt
resources/tests, including fee selection and session form focus. Its outdated
preparation-text assertion was aligned with the earlier paused-session copy;
no new test or payment behavior was introduced for this cosmetic change.
`git diff --check` passes. A full application build was not run.

## Queued offer and single preparation action (2026-09-28)

Read-only inspection of the operator's second failed request showed an unsigned
REJECTED attempt, no recovery need and only refresh allowed. Core retains its
historical inputs in this view; requires_attention=false confirms that no live
reservation/authorization remains. That case uses the same safe-closure
presentation as the earlier recipientless cancellation correction.

The underlying stall was another asynchronous phase: requestpaymasterquote sets
AWAITING_USER_SIGNATURE before the quote reply arrives. Qt had stopped its live
send continuation at this phase and labelled it ready for review. A later
explicit resume could run beyond the intent deadline, exhaust the remaining
eligible provider set and end the unsigned session. The old UI then still
required a manual status refresh and could not dismiss the terminal failure.

Qt now continues the original prepare_only request through that queued phase.
Only a quoted attempt or exact commitment with current complete costs permits
review presentation. While preparation is active, the primary status action is
disabled and labelled Preparing payment, preventing an extra refresh from
interrupting it. An RPC error ends continuation and reads durable state once;
a safely closed request offers Start a new transfer, including when historical
inputs remain in its record. Signed/ambiguous states remain protected.

In explicit Paymaster mode, Prepare payment replaces Send DigiDollar and the
redundant Find offer dialog. Inline copy explains provider contact, temporary
reservations and the still-mandatory exact payment authorization. Directory
preview remains optional. Automatic funding retains its first confirmation
because it may spend DGB directly; privacy masking still blocks preparation
until the recipient/amount can be reviewed. Core/RPC/wire authority is unchanged.

Qt and translation guidance apply; no translation catalogs were edited. A full
application build and a real two-node payment remain operator acceptance work.

Verification: targeted MSVC compilation of the changed client/form and affected
MOC, Qt test compilation and library/test linking succeeded. All 28 selected
cases pass (exit 0): eight live-send outcomes, 14 action-matrix rows, exact
authorization, bound RPC actions, recipientless restart, two preview checks,
and the form/fee-selection test. Optional preview performs no prepare/send RPC.
The queued phase receives no approval until complete exact terms arrive; the
expiry case reaches Start a new transfer without signing. `git diff --check`
passes. The existing user GUI processes still use the older executable;
all changed objects are ready for the GUI relink after a normal shutdown.
