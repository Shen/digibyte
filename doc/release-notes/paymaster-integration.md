# Paymaster integration release notes

These fork-specific changes are unreleased and are separate from the official
[v9.26.6 release notes](../../RELEASE_v9.26.6.md). Upstream verification results
do not validate this integration. See the [test runbook](../digidollar-paymaster-testing.md)
for the current checkpoint and remaining checks.

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
