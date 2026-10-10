# Paymaster integration release notes

## Provider recovery and unsigned history (2026-10-10)

Provider recovery now remains possible when an already signed transaction's
SPENT budget entry has aged out of the rolling 24-hour ledger. The exact local
commit and authorization are independently validated; conflicting or corrupt
evidence still fails closed. This path grants no new signature, raises no limit
and never charges the fee again. It also covers exact final-commit retries.

Expired unsigned provider requests are now removed atomically after the complete
retry/capacity window plus 24 hours. Maintenance removes at most 64 sessions per
pass. Admission stops at 8,192 retained provider sessions per wallet while
allowing existing requests to continue. Signed, ambiguous and historical
restricted artifacts remain protected. Cleanup does not create permanent
tombstones for expired unsigned requests.

Changes are confined to Paymaster code, tests and documentation; consensus,
wire and database formats are unchanged. The
[security report](../paymaster-threat-model.md#follow-up-corrections-2026-10-10)
and [test runbook](../digidollar-paymaster-testing.md#provider-security-corrections-2026-10-10)
distinguish selected-test evidence from the still-required full product build
and broader release gates.

## Client recovery fee budget after reorganization (2026-10-10)

Original service-fee reservations now remain protected until cancel-to-self
reaches the existing 240-confirmation safety depth. Previously a single recovery
confirmation released the fee; a reorganization did not restore it, permitting
further approvals above the intended daily exposure limit.

Retained older recovery sessions reconstruct released or aged-out fee rows from
their exact accepted signatures before new approvals and final reconciliation.
Restoration and session/authorization updates are atomic, saved limits are not
raised, and spent fees are not duplicated. An original payment that wins after
the reorganization can settle from its validated provider result or deep local
confirmation, including stale canceled/conflicted recovery status. The correction
is confined to Paymaster wallet code and tests; consensus and record formats
are unchanged. See the testing guide for targeted validation and build scope.

## Local reputation RPC schema (2026-10-10)

`getpaymasterreputation` now declares its complete result schema, including the
optional success rate. Nonempty results no longer fail strict RPC result
validation. RPC and CLI regression coverage checks the same wallet's aggregate
records. This changes no reputation accounting, fees, reservations or consensus
behavior.

## Restricted sponsorship parked (2026-10-09)

Restricted / invitation-only sponsorship is disabled for new use in GUI, CLI
and RPC/Core. User paid and public Sponsored remain available. Existing saved
Restricted settings are shown as unavailable and are never silently converted
to public sponsorship. New descriptors, requests and first authorizations are
rejected with a stable RPC error; the GUI explains the restriction in plain text.

The implementation, serialized records, signature/replay checks and historical
accounting remain for future extensions and already accepted-transfer recovery.
This does not introduce voucher creation or redemption. Design boundaries and
re-enablement requirements are documented in the implementation guide under
"Parked restricted sponsorship". No consensus or original DigiByte fee rules
change. The older Restricted setup notes below describe the previous behavior.


These fork-specific changes are unreleased and are separate from the official
[v9.26.6 release notes](../../RELEASE_v9.26.6.md). Upstream verification results
do not validate this integration. See the [test runbook](../digidollar-paymaster-testing.md)
for the current checkpoint and remaining checks.

## DD history startup status

The fast stored-history preview no longer briefly labels already-confirmed
transactions as Pending when opening the DD history or overview. It shows
Checking… until the existing asynchronous wallet read supplies live status.
The immediate preview remains non-blocking; wallet, RPC and confirmation rules
are unchanged.

## Funds and reserves presentation

Overview now shows the saved User paid and Sponsored models with separate local
readiness/budget indicators and explicit zero-fee pricing. Concurrent customer
offers, capacity reservations and queue depths are separate from provider
management/reserve progress. Settings hides inactive model controls symmetrically
and offers a confirmed service-only restart when saving an offer or processing
mode. Autostart, budgets and protected reservations are retained; incomplete
replies stop the sequence for review. Core and RPC implementations are unchanged.

The Paymaster funds page now shares the income/cost page's responsive metric
cards and table typography. It separates current capacity/capital, daily actions,
saved reserve counts and the target/refill form. Individual DD release and
DGB-only release are under Advanced reserve management with their effects
explained. Existing approval flows, RPCs and Core spending rules are unchanged;
the layout uses already-loaded snapshots, with no additional refresh requests.

## Official v9.26.7 integration

The `integration/paymaster-v9.26.7` branch merges the official release through
`d7265fb05e26`, preserving the current Paymaster implementation. All official
product changes are retained: tip-specific difficulty RPCs, unused Qt DD receive
addresses, the minimum-change tooltip, build-derived overview branding, index
shutdown handling and the test/CI corrections. The fork's Windows version header
is regenerated as 9.26.7, and its shared Qt test helper exposes the rescan needed
by a new official address regression. Both new address regressions also finish
RPC warmup when run in isolation; their assertions remain unchanged.
No additional Paymaster product change is
required by this merge. The official release document is unchanged; its test
claims do not certify this fork. See the [integration checkpoint](../digidollar-paymaster-v9.26.7-integration.md)
for targeted results and pending operator checks.

## Recovery after interrupted Paymaster requests

First mempool acceptance and validated Paymaster completions now notify
DigiDollar displays immediately; unconfirmed payments remain shown as pending.
Repeated durable recovery of an unchanged transaction preserves wallet caches
and avoids duplicate UI events, while retaining exact-byte and chain/pool checks
and reconciliation of changed state/metadata. Opt-in `bench` timing distinguishes
durable recovery from slow DD history worker/read/Qt-delivery phases. This does
not establish that every reported live-wallet pause has been resolved.

A temporary safety-limit pause no longer leaves the automatic provider permanently
in `drain_only` after its reservations and limits recover. Core reconciles and
rechecks readiness before resuming, preserves all existing approvals and limits,
and refreshes its offer. GUI and RPC diagnostics now distinguish a drain from an
active provider even when local prerequisites are ready and no last error remains.

Routine `PAYMASTER_PROVIDER_SYNCING` waits now use opt-in `bench` logging,
avoiding repeated normal-log pause messages during frequent block arrivals.
GUI/RPC waiting status, synchronization checks, other pause reasons and drain
recovery logs are unchanged.

## Review spending limits for an offer change

Provider offer saves that conflict with spending limits now explain why and
offer a direct budget review. Disabled selected models receive finite draft
suggestions; incompatible per-transfer caps can be lowered for review. Existing
custom limits and unsaved edits are preserved. Budgets and offer activation
still require separate explicit saves; RPC/CLI validation is unchanged.
The actionable conflict notice omits the internal error code. Offer settings
also explain that User paid with a 0% tariff charges no DD fee and that switching
models does not restore a previous tariff.

## Client funding feedback

Send payment now checks the selected public offer's total against the cached DD
balance before preparation, with a visible shortfall warning. A positive remainder
below the minimum DD output also disables preparation, including with fee deduction.
The amount hint and cost summary show the same warning. Fee deduction uses
the total including the fee; Automatic with own DGB retains its direct path.
Editing a safely closed/uncreated payment draft clears its obsolete error notice.
No wallet-input, minimum-change or RPC/CLI spending rules change.

## Operator income and costs

The finance page now leads with period income, operating costs and successful
payments. An aligned breakdown explains the difference between transfer costs
and reserve-maintenance fees. Period comparison is compact, USD estimates are
explicitly optional current-price valuations, and current reserve capital and
withdrawable fees appear separately from period earnings. All views reuse the
existing finance snapshot; Core accounting, RPC/CLI and spending authorization
are unchanged. Dark/light styling, app number formatting and privacy apply to
the new readouts. Old values clear when switching wallets or reporting periods.

## Excess reserve release

Reserve reduction now previews estimated DGB/DD release fees and proposes a
finite one-time ceiling for explicit approval. A fee-limit rejection links
directly to a fresh calculation and review. Recurring refill allowances remain
separate. Busy-provider rejections receive a short bounded retry of the same
plan; the UI identifies these attempts as not started. Lost execution replies
remain uncertain and are not retried automatically.

## DD Overview responsiveness

DD Vault now loads and refreshes its positions on a single background worker.
Concurrent refresh signals coalesce and signing-state badges use the same
snapshot, so opening or repainting the Vault no longer waits on a busy wallet
on Qt's event thread. Empty vaults also avoid an unnecessary transaction-history
scan. Wallet changes, privacy and closing during a refresh reject old results.
The existing Core reconciliation and redemption rules are retained.

DD Overview no longer reads wallet balances, locked collateral or mint capability
on the GUI thread. A delayed overview refresh after switching to Paymaster Network
also returns without taking the wallet lock. Oracle and network-health reads run
in the background. Existing balances remain visible during refresh; wallet changes
discard old replies, and privacy masking applies to asynchronous results. Core
locking, financial authorization, RPC/CLI and ordinary DGB pages are unchanged.

The Paymaster signing paths now inspect wallet lock state and release temporary
unlock authority on workers as well. This covers client sending/recovery and
provider signing actions. The normal passphrase dialog, exact approval, wallet
binding and relocking before result dialogs are retained. Large accounting CSV
exports no longer serialize or write the complete file on Qt's event thread.
Shared native backup/password dialogs and other original DD actions still contain
synchronous work; see the [broader audit](../design/paymaster-flow-audit.md#ui-blocking-audit-2026-10-07).

Paymaster CLI workflows, Qt signing orchestration and wallet-maintenance
scheduling have been moved to Paymaster-owned modules. Existing RPC transport,
setup validation, unlock leases and wallet lifecycle hooks are reused; no
payment policy, wire format or wallet-data migration changes.

## Send DD responsiveness

Opening Send DD and refreshing its balance no longer wait for Paymaster wallet
locks on the GUI thread. Spendable balances are read in the background while
the last wallet-bound display and editable payment draft are retained. Pending
refreshes are coalesced; late replies cannot restore a closed or previous
wallet's balance. Loading failures are shown without treating them as zero
funds or enabling a payment. Core balance checks, signing, reservations and
RPC/CLI behavior are unchanged.

## Saved transfers after a later payment completes

The read-only client inbox accepts a validated later reservation owner even
after that owner becomes terminal. A harmless unsigned old session no longer
causes `PAYMASTER_RESERVATION_SESSION_CONFLICT` solely because a later payment
finished while its input reservations still await reconciliation. The shared
wallet lookup applies to Qt and RPC/CLI. Unsigned-history proof, ownership
bindings, signed recovery protection, reservations and coin locks are preserved.

## Fresh offers for another payment

After a completed Paymaster payment, Start a new transfer clears the previous
provider selection, offer cards, expiry and cost preview even when the entry
fields were already emptied after success. An empty entry shows an input prompt;
a new valid amount requires a fresh offer check. Saved fee limits, completed
payment history and Core financial authorization retain their existing behavior.

## Receive DD responsiveness

Opening Receive DD no longer reads saved payment requests on the GUI thread.
The DigiDollar Qt extension loads them in the background, preserving displayed
rows during refresh and rejecting late snapshots after a wallet change or local
request edit/removal. The original DGB receive page, Core wallet locks, payment
authorization and RPC/CLI are unchanged.
Selecting or opening a stored DD address, copying its request and preparing its
QR code also use the displayed snapshot. Local edits/removals update that cache
and recheck the stored request identity before writing, so an externally removed
request cannot be restored by a stale display.

## Confirmed payments with missing provider replies

Wallet-tip and periodic Paymaster maintenance now complete a signed client session
from its exact locally known payment after 240 confirmations when the provider
reply was lost. Explicit recovery performs the same validated observation before
testing spent inputs, returning the original confirmed payment instead of preparing
recovery. The fix applies to Qt and RPC/CLI through shared wallet Core. Accepted
manifests, all final signatures, atomic fee settlement, normal retention and
read-only status contracts remain required; no retry authority or payment is created.
Qt also accepts that validated local confirmation without a provider-result envelope.
Unknown result states and terminal states without current payment evidence remain
insufficient to show success.

## Provider process display

Overview and the provider header now share the current payment/capacity/
confirmation presentation. Additive RPC activity counts ignore historical
committed entries and expired admissions; CLI uses the same work classification.
Known synchronization waits remain waits, and scheduler errors report the
reason belonging to their actual state. Genuine faults still take priority.

Visible active-provider status refreshes every two seconds without repeatedly
scanning finance history. Finance summaries refresh on changed capital/budgets,
at thirty seconds or on explicit refresh. Unsaved offer drafts survive polling.
Temporary reserved capacity no longer describes saved refill consent as a
configuration failure. No spending authority or payment execution changes.

Passive status/finance reads no longer repeatedly disable and re-enable the
operator pages. Unchanged observations preserve forms, focus and confirmed
finance presentation. A deliberate action is serialized after the current read
and gates further actions. Read-only RPC/CLI observations supersede a previous
synchronization wait only after current readiness and exact chain/wallet/index
agreement; unresolved waits and genuine faults remain visible. Saved refill
consent and the strict checks before payment execution are unchanged.

## Existing V5 payment artifacts after upgrade

The V6 offer fee-cap update incorrectly rejected unchanged, retained V5
Capacity proofs during provider startup, including expired/released replay
barriers. Direct Capacity, quote, submit and result payloads now explicitly
accept exactly V5 and V6 with all existing signature, resource, consent, fee
and execution checks. Capacity/quote responses preserve the request version;
existing signed data, reservations and replay barriers are never rewritten.
GUI, RPC/CLI and automatic processing use these shared Core paths.

Announcements and connection negotiation still require V6. Pre-V5/future
direct versions and unsupported inner wallet-record versions remain rejected.
No wallet-data reset, migration, reindex or renewed spending approval is needed.

## Official v9.26.6 integration

Official PR #452 (`92330d952625`) is integrated with precedence over fork
behavior. Paymaster's full DGB funding plan is separated from upstream's DD-only
preflight, preserving detailed DD errors, direct-send checks before confirmation
and typed AUTO fallback. Official mint-consolidation tracking and private
Paymaster metadata remain hidden in transaction RPC responses. The official
v9.26.6 document is unchanged; integration runtime verification remains pending
as recorded in the test runbook.

## Failed client preparation

An early preparation error no longer strands an uncreated local request after
Core confirms its absence. Qt retains editable recipient/amount fields; known
or unreadable sessions remain protected. CLI/RPC now distinguish missing
sessions from read/version failures, and DD selection errors retain upstream's
specific reason. Existing unsigned cancellation works without a running
provider; signed payments still require their existing retry/recovery path.

## CLI/RPC recovery parity

Confirmed-payment recovery now also covers direct result processing, authorized
`senddigidollar` resumption and exact already-signed PSBT retries. These paths
share the same local transaction/signature checks as explicit GUI recovery,
including after provider replay-journal pruning. Result polling returns a known
txid even without another provider message; PSBT retry reports completion without
queueing another submit. No signed provider receipt is fabricated and no expired
spending authority is renewed. The corrections reside in the shared wallet Core;
CLI argument encoding needs no change.

## Offer selection and policy editing

Public Paymaster offers now appear as selectable main-view cards with exact
service fees, effective percentages and wallet totals. The cheapest eligible
offer without an unsuccessful first attempt is recommended/preselected when
available; users can choose a different listed provider. The paired initial
provider/offer preference is enforced by shared RPC Core. An unavailable choice
is reported, and fresh exact quote approval remains mandatory before signing.
Local unsigned contact failures and confirmed payments now populate the existing
idempotent reliability history used for recommendation demotion. Authenticated
provider rejections also count, preserving signed recovery protection. Later
confirmed completion can supersede its earlier failed/advisory outcome once;
late negative replies cannot downgrade a confirmed success.

Paymaster monetary fields and dialogs consistently use the app's decimal point,
with thin-space grouping for large readouts, also on German systems. Integer
cent/satoshi authority and machine-readable RPC/CSV formats remain unchanged.

Decimal offer inputs now validate and save the displayed percentage/DD/DGB
values exactly. Invalid precision or format retains the draft with inline
feedback. Save errors also appear on the form. Identity, offer fields and the
fee example share aligned columns; setup uses the same example form rows.

## Wallet responsiveness

Paymaster reservation checks now validate the provider pool once per balance or
coin-selection scan, reducing repeated database decoding and key validation
while holding the wallet lock. Finance reconciliation also reuses event lookups
and block times within each call. GUI and RPC/CLI share these improvements;
reservation safety, fee limits and reconciliation frequency are unchanged.
Optional `bench` logging helps diagnose remaining delays. See the
[test checkpoint](../digidollar-paymaster-testing.md#paymaster-wallet-responsiveness-2026-10-01)
for measurements and the remaining live GUI check.

## Unreleased integration follow-up

The Paymaster operator interface now separates Overview, Funds & reserves,
Activity, Income & costs, and Settings. Autostart is visible beside Start/Pause
and saves immediately; a second switch controls reserve refill with explicit
review for new paid consent. Both mirror Operation & automation. Single-reserve release refreshes the
available outputs before review, preserves the selected output through dialog
updates and explains unavailable reserves with a read-only recovery action. Its existing behavior is unchanged:
an enabled provider may start when ready, encrypted wallets need manual unlock,
and persistent pause disables autostart. Seven numbered setup steps, focused
settings pages and task progress across navigation simplify operation. This
candidate adds optional liquidity revision checks, read-only node configuration
metadata and automation status. Funds & reserves now owns capital actions,
targets/costs and retirement; backup has its own settings page and no longer
overrides operating status. DGB-only release preserves DD reserves and binds
a finite fee cap. Spending authority and consensus rules remain unchanged.
Automatic provider submission now preserves nonblocking wallet/index checks
through its nested signing and final-transaction preflights, avoiding a scheduler
self-wait when a new block arrives during payment processing. Operator pages
remain paintable during pending reads, slow reads show elapsed progress, and
provider approval dialogs use the active green theme.
Explicit client retry now reconciles a lost provider result after restart even
when the exact authorized payment has already consumed its Capacity. The spent
reserve exception requires the validated locally observed final transaction.
Qt follows the explicitly requested retry through result collection, stops on
completion/error/privacy entry, and themes client recovery message boxes in
both light and dark mode. Session restoration remains read-only.
A further retry correction covers provider journal pruning after 240
confirmations: the client can complete from validated local chain evidence,
including payments absent from its transaction cache, and settles its reserved
fee atomically. Stale/mismatched inbound submissions no longer pin the provider
queue. No provider receipt is fabricated and no new broadcast authority is added.
Verification for this UI revision is recorded separately in the test runbook.


Configured Paymaster wallets can explicitly select continuous wallet-wide
operating unlock until manual lock, wallet unload or node shutdown. GUI and CLI
setup preselect this choice; timed access remains available. Passwords are never
stored, encrypted wallets still require manual unlock after restart, and existing
wallets receive no automatic unlock or additional spending authorization.
The optional third `walletpassphrase` argument `paymaster_until_shutdown=true`
requires timeout zero; `unlocked_until=-1` reports this volatile mode.

CLI setup can optionally generate separate DGB/DD receiving addresses and watch
incoming funding, approved reserve preparation and confirmations. A requested
one-time start is verified before setup reports completion. Autostart remains
separate. Ordinary payment-capacity waits now receive a waiting explanation
instead of an error headline.

The CLI assistant now explains each choice in seven stages, accepts exact DGB,
DD and percentage inputs, retries invalid input in place and reviews readable
budgets. New providers propose automatic operation, bounded paid refill and
autostart; saved settings remain preselected for existing wallets. Explicit-yes
approval is still required, and encrypted wallets still need manual unlock after
restart. Repeated unchanged funding status is condensed into a heartbeat.
The CLI offers only supported policy combinations: restricted sponsorship is
sponsored-only with zero DD service fee. Invalid restricted combinations are
rejected by the shared setup planner before any configuration changes.

A follow-up setup audit extends shared preflight to the complete proposed
configuration using Core's existing policy validators. The Qt assistant now
prevents restricted mixed models, clears DD targets for sponsored-only pool
preparation, retains exact saved policy values in both preview and execution,
and reviews inactive budget adjustments. CLI exposes inactive saved limits,
accepts an optional valid identity name and enforces the pool setup fee bound.
The Core policy rules themselves remain unchanged.

Node connection review now uses a single window with current values and startup
source labels. Overridden values are read-only; only changed values are previewed,
errors retain the entered values, and saving requires the reviewed Core plan.

The provider's main reserve action now guides restoration after deliberately
releasing the last DD payment reserve. It reviews the required capacity change,
capital and fees together, preserves all spending limits, and can start the
provider when ready. Lost settings replies are reconciled before continuation;
changed settings or increased funding require a new review. Operation no longer
repeats the reserve/budget settings buttons below its status cards.

Earnings withdrawal now explains the network minimum output (currently 1 DD)
and disables guided and advanced payout below that amount. Smaller earnings
remain accumulated without consuming the provider's operating capital. Unknown
operator-task failures retain their actual error instead of a generic provider
requirement; no Core payment or minimum-output rule changes.

These working-branch changes are outside the published RC2 change counts and do
not move a release tag. Verification and remaining operator checks are recorded
in [the Paymaster test guide](../digidollar-paymaster-testing.md).

The provider overview groups operating capital by capacity and reserve asset,
shows DGB beside every spending-limit amount, and places accumulated earnings
and the payout threshold in Income & costs. Technical pool counts are expandable.

Provider pages now share a command-in-progress lock, preventing repeated or
competing button actions until the RPC and its follow-up processing finish.

Pool setup fee-limit errors now have a direct guided recovery action: inspect
the approved ceiling, cancel an uncreated plan safely and review a replacement.
Higher fees still need explicit approval. Overview maintenance counts are
labelled tasks, since they also include work without a saved transaction.
The replacement preview now remains in the serialized RPC chain and its approval
survives intervening status reads. A pending start without a reserve-creation
order leads to a reserve review instead of an endless automatic-progress display.

Background operator refreshes retain the last validated frame and update
valid snapshots without resetting and rebuilding the layout. This also avoids
flashing before provider startup with slower replies. Status polling and
command serialization are unchanged.

A successful retirement/stop completion notice is cleared after Core confirms
that the provider is running again, avoiding a stale stopped-state message.

The retirement confirmation replaces its non-editable capital-release
checkbox with an explanation and directs stop-only users to Stop Paymaster.

The node connection dialog presents externally controlled settings as readable
fixed values and hides edit actions when everything is read-only. Its scroll
background follows the active theme instead of the native light palette.

Liquidity settings now keep their approval and save protected from background
refreshes, preserve unconfirmed edits and reconcile lost save replies against
the wallet's stored policy. The form explicitly distinguishes automatic refill
from permission to pay refill fees and opens the fee controls when needed.

Repeated unsigned cancellations no longer turn a recipient-specific quote limit
into a provider-wide drain. Limited requests wait within their original expiry;
expired or invalid requests do not fault the provider. The overview explains
open-offer reservations and the per-recipient limit. Client notices distinguish
an incomplete exchange from an unreachable endpoint and explain that provider
reservations can remain until expiry after local cancellation. Presets, approved
fees, wire messages and signature/reservation safety rules are unchanged.

- Client wallets can approve an effective service-fee percentage alongside their
  absolute and daily DD caps. Send $DD proposes 1%, activated only by saving.
  Existing wallets retain their previous approval until review. Actual rounded
  fees and net recipient amounts are checked in discovery, acceptance and before
  signing, including alternative recovery. RPC omission cannot remove a cap.
- After a two-minute live wait, unsigned preparation is closed only when Core
  confirms it is safe. Signed transfers/recoveries switch to status observation;
  no timeout releases inputs or authorizes a replacement. Older unresolved
  sessions prominently offer reviewed return-to-wallet recovery. Transient read
  failures no longer interrupt background observation with repeated dialogs.

- Explicit GUI/RPC provider-wallet restore now commits a durable Paymaster quarantine before loading or autostart. Old backups cannot renew forgotten payment budgets or sign again with inputs of a withheld final. New provider payments and reserve spending are blocked; exact retained signed commits remain recoverable. Both real SQLite/regtest backup variants pass, including RPC/CLI checks and client-only restore compatibility. There is no acknowledgement-based override or automatic reconstruction of missing history. Unmarked wallet-only rollback is now checked against an independently flushed node-local provider checkpoint; whole-directory rollback still needs separately retained state. See `doc/paymaster-threat-model.md` and `doc/digidollar-paymaster-testing.md`.

- Provider Paymaster records now commit atomically with a network/provider-bound
  safety generation. The external checkpoint advances before wallet commit;
  missing/mismatched files, failed commits, unfinished native reserve operations
  and conflicting wallet copies block new authority. The GUI explains these
  protection states without raw error codes. Initial legacy enrollment requires
  trusted complete history; whole-datadir rollback remains a separate boundary.

- Restored current provider wallets can explicitly review recovery capital
  release when their independently retained checkpoint matches and no unfinished
  or unconfirmed work exists. Approval permanently retires the provider identity
  at that checkpoint store and releases eligible reserves into the same wallet;
  ordinary Send remains available afterwards. Signing guards/history remain;
  stale or missing checkpoints still block release. Backup reminder updates no
  longer make an otherwise current backup obsolete. This is no checkpoint reset
  or new provider-signing permission. See `doc/paymaster-capital-release.md`.

- New Paymaster direct connections authenticate the provider against the local
  BIP324 session before Capacity/payment/recovery metadata is transmitted.
  Both endpoints require the added channel-authentication capability. Retained
  V5/V6 financial artifacts keep their original signatures; there is no unsafe
  transport fallback. A real-cipher two-leg relay regression covers mismatched
  sessions and manipulated proofs.
- A new disposable-regtest diagnostic reproduces the still-open matching
  wallet/checkpoint rollback: 0.3 DGB paid under a 0.2-DGB approval. Independently
  hosted copies of one provider identity remain unsupported. Use separate
  identities or one signing authority behind transparent access points; an
  external durable witness/replicated financial ledger is future work.
