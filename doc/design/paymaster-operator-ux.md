# Paymaster: operator experience

Current implementation candidate: 2026-10-01. Consensus and payment authority
are unchanged. See the testing runbook for verification and remaining acceptance.

## Find the task

| Destination | Responsibility |
| --- | --- |
| Overview | Operational state, Start/Pause, immediate autostart and refill switches, compact capital/budget/results summaries. |
| Funds & reserves | Balances, four visible capital actions, reserve targets and refill costs; separate stop/release/backup completion. |
| Activity | Active work and reservations with next steps; raw records in disclosures; manual processing only in manual mode. |
| Income & costs | Period DD revenue, DGB costs, bookings and complete CSV export; link to capital management. |
| Settings | Offer, Spending limits, Operation & automation, Node connection, Wallet & backup. |

The sidebar becomes a selector below 960 logical pixels. Current task progress
survives navigation. Navigation stays available during an RPC; competing changes
remain disabled. Backup reminders do not set the operational hero's state,
color or primary action. Backup timestamps and the separately labelled manual
external confirmation belong to Wallet & backup.

The header and operational hero use one presentation of the current snapshot.
Live payment-budget reservations, unexpired Capacity admissions and reserved
operational outputs show payment work even while other payment slots remain
available. Historical committed outputs do not keep a payment active. Pending
successor outputs show confirmation waits. Known node/index synchronization
waits and genuine faults retain priority; the scheduler's reason matches its
current service state. CLI uses the same work-phase classification.

Visible running providers refresh through the asynchronous operator read every
two seconds, stopped idle providers every thirty seconds. Finance summaries
refresh at thirty seconds or when capital/budgets change; an explicit refresh
reads both. Open offer drafts are preserved and do not freeze Overview polling.
Temporary capacity use pauses refill execution without changing saved consent.

## Three independent automation choices

Autostart and automatic reserve refill have immediate-save checkboxes on
Overview and Operation & automation. Request processing has its own form.
Switches show the saved intent separately from current execution and never
save another form's draft. Pause disables autostart but retains refill policy.
Wallet locking, missing funds, budget exhaustion and manual processing do not
clear saved refill intent. Full retirement revokes refill and paid consent.

Enabling refill reuses valid approved finite limits. Otherwise a cancellable
review requires positive transaction/hour/day caps before any write. Disabling
refill preserves those caps and existing signed work. A lost reply reads the
saved state before permitting another write. Wallet/privacy changes invalidate
open reviews and late callbacks.

The confirmed liquidity snapshot is separate from editable targets/costs and
the revision captured when editing begins. An acknowledged local quick change
rebases that draft while preserving edited targets/costs; external revisions
still require explicit reload/review. setpaymasterliquiditypolicy accepts
optional expected_updated_at; Core checks it under the wallet lock with the
write. Zero expects no saved policy. Older callers remain compatible.
automation_status on operator/liquidity reads reports configured/enabled intent,
paid consent, actual state/reason and pending transactions. The older
maintenance_state remains a demand summary, not proof of running work.

## Capital and configuration reviews

Funds & reserves visibly explains Prepare reserves, Withdraw DD service fees,
Release one DD reserve and Release excess DGB. All use current plan review,
explicit approval and the shared wallet-bound task controller. DGB-only
rebalance preserves every DD reserve and binds maximum_fee_satoshis into the
plan hash. Full retirement follows pause, open-work checks, separate exact
release approval and backup. An uncertain reply never triggers automatic
re-execution. Technical forms do not offer a second independent execution path.

Targets and refill costs share one Save/Discard form. Offer, spending limits
and processing mode have independent drafts and discard actions. The seven-step
setup loads existing values; amount widgets use exact DD cents/DGB satoshis
internally and human currency units on screen.

getpaymasternodeconfig is read-only and nodewide. It distinguishes effective
loaded values/sources from current main-file values and pending restart changes.
Command-line, settings.json and forced values are locked per field. Included,
negated, duplicate and conflicting entries are identified before editing.
The inline editor and setup use the same component. Saving still requires the
existing file-bound preview, conflict recheck, backup and atomic replacement.
Saving does not restart or report a file change as effective runtime state.

## Presentation and verification

Native strings remain English tr() source text. Both themes apply to pages,
wizard and dialogs. Narrow-window, scaling, keyboard and privacy tests include
late replies and exact monetary approvals. The HTML preview is an example-only
interaction model with no RPC or wallet access. It is not runtime evidence.

## Historical evidence

The following results describe the previous candidate only.

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
