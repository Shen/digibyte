# Paymaster operator workflow

Current UI candidate: integration/paymaster-v9.26.6rc2, 2026-10-01.
The task-oriented redesign has separate verification; earlier results do not
validate this revision. This is not a release approval.

## Find the right page in Qt

The [clickable design preview](design/paymaster-operator-preview.html) uses
example data and has no wallet connection.
The [design contract](design/paymaster-operator-ux.md) describes the interface.

| Area | Use it for |
| --- | --- |
| **Overview** | Operational state, Start/Pause, autostart and automatic refill switches. |
| **Funds & reserves** | Restore reserves, release earnings into the same wallet or review capital release. |
| **Activity** | Payments, durable reservations, recovery and manual expert processing. |
| **Income & costs** | DD income, DGB costs, financial history and complete CSV export. |
| **Settings** | Offer, Spending limits, Operation & automation, Node connection, Wallet & backup. |

A sidebar is used from 960 logical pixels; a compact selector opens the same
pages in narrower windows. Settings categories have a Back to settings action.
Current task progress stays visible while navigating. Background reads preserve
the visible pages, including when the window is resized or uncovered. If a read
takes more than two seconds, its current step and elapsed time are shown.
Passive reads leave navigation and draft inputs enabled. A deliberate wallet
action waits for the in-flight read, then runs once with the usual Core checks;
further actions are gated until its callback chain completes.

### Read income and costs

Choose a reporting period in **Income & costs**. The three headline figures are
confirmed DD service-fee income, DGB operating costs and successful payments.
The average service fee belongs to those successful payments. The source table
separates customer-paid and sponsored payments from reserve maintenance; its
cost rows add up to the headline total. Maintenance includes reserve setup,
refill, fee withdrawal and release transaction fees, not the capital in reserves.

**Compare reporting periods** shows the same measures in aligned columns.
Today begins at midnight UTC; seven and thirty days are rolling periods. These
periods overlap and must not be added together. **Estimate result in USD** values
DD income at its denomination and DGB costs at the current oracle price. This is
an optional current-price estimate, not historical profit or loss. Native DD/DGB
amounts remain the accounting record; missing prices do not imply a zero result.

**Currently held in reserves** is a current wallet snapshot, independent of the
selected period. DD base capital, accumulated service fees and fees available to
withdraw are shown separately. Accumulated fees are not additional income to add
to the period totals. Use **Manage funds and reserves…** to review a withdrawal
or release. Booking details and complete CSV export still use the selected period.
Privacy masks these values, and changing the period clears old figures until the
replacement snapshot arrives. Decimal points and thin-space grouping match the app;
DGB summaries omit trailing zeroes without reducing precision.

### Temporary admission pauses

`running` means the provider runtime is registered; `ready` describes its local
prerequisites. `service_state=drain_only` still blocks new requests while allowing
existing authorized submissions to finish. The overview shows **New payments
temporarily paused**, or the specific limit/prerequisite needing attention.
**Check current status** reads the current snapshot without changing settings.

Core rechecks a temporary drain on subsequent service ticks. Once reservations
expire or are released and all current readiness and safety checks pass, it
resumes automatically and refreshes its offer. It retains the saved budgets,
refill approvals and quote-rate accounting. Do not raise limits merely to clear
an old runtime state. Persistent Pause/Stop remains an explicit operator action
and is not undone by this recovery.

### Autostart beside Start/Pause

Changing **Automatically start Paymaster when this wallet is ready** saves
immediately. The same switch is available in Operation & automation. Saving,
the confirmed value and any failure are shown beside it.
The option may start an enabled provider during the current wallet load as
well as after a future load, once Core readiness permits it. Encrypted wallets
still need manual unlock after restart; no password is stored. Turning this
option off does not pause an already running provider.

Persistent **Pause** or **Stop** turns autostart off and explains this result.
The saved automatic-refill choice and its approved limits are retained. A subsequent manual Start/Resume does not re-enable it.
New GUI setups default to off; existing settings are retained.
Autostart and automatic request processing are separate settings. Each saves
only its own field. If a save reply is lost, the interface checks the saved
state before allowing another attempt.

### Automatic reserve refill

The additional **Automatically refill missing reserves** switch saves immediately
and mirrors Operation & automation. Its checkmark is the saved choice. The
status below it explains whether work is off, ready, paused, blocked, running or
waiting for confirmation. Wallet locks and exhausted budgets do not clear it.
Turning it off prevents new automatic work but preserves approved limits and
already signed transactions. Enabling without valid finite consent opens a
cancellable fee review. Full capital release revokes both refill and paid consent.

Targets and refill cost limits share a Save/Discard form in Funds & reserves.
No quick switch submits drafts from this form. A stale policy revision is
rejected and must be refreshed before a new review.

### Seven-step setup

Confirm wallet → Check connection → Set offer → Limit spending →
Reserves and operation → Review and approve → Setup and result.
The offer step includes the optional name, payment model and price/range.
All existing approvals and exact capital/fee reviews remain in place.

**Settings → Offer** and guided setup show an example recipient amount beside
the percentage tariff. The example computes the DD service fee, effective
percentage and total using Core's cent-upward rounding. It does not change the
configured payment range or budgets. For example, a 0.50% tariff on 1.00 DD
costs 0.01 DD, effectively 1.00%. Client fee ceilings remain absolute DD limits;
the client percentage is a comparison, not a provider price.

Across Paymaster settings, setup and payment dialogs, enter percentages and
amounts using the app's decimal point (for example, `0.80 %` and `1.25 DD`,
including with German system settings). Large readouts use thin-space grouping,
for example `12 345.67 DD`; entry fields do not accept grouping. **Save policy**
commits the exact typed values to Core. Unsupported precision, mixed separators
and out-of-range values leave the draft intact and select the field to correct.
Save feedback appears on the offer form. A failed or unconfirmed reply retains
your edits; a stale status reply cannot overwrite an acknowledged save. Pause
the running provider before changing its operating policy.

The client's **Send DD** view lists public providers as selectable cards.
Each shows the recipient amount, DD service fee, effective percentage and total
wallet outflow. **Recommended** preselects the cheapest eligible offer without a
known first failed attempt when such an alternative exists. Select any other
listed card to use it, then click **Send payment**. The exact verified quote is
reviewed again before signing. Refreshing preserves a still-valid manual choice;
changing the amount, fee/privacy inputs or wallet requires a new selection.
An expired or unavailable selected offer is reported rather than silently
replaced. Offer lists remain local previews, not reachability guarantees.

Failed unsigned provider contacts and authenticated provider rejections populate
this wallet's local history. Signed transfers remain protected after rejection;
a later confirmed payment replaces its failed/advisory outcome with success.
An otherwise eligible provider with no successful payment and an earlier
failure is recommended after alternatives, even with only one observation.
Active short cooldowns may temporarily exclude it. A lone eligible provider
remains usable; a subsequently confirmed payment removes this initial-failure
demotion. User cancellation, local connection limits and wallet locking do not
penalize a provider. This local history is advisory, not a public reputation score.

Stopping a provider does not instantly withdraw already relayed announcements.
They can remain in clients' local lists until their signed expiry, at most ten
minutes after creation. A list refresh is not a reachability test. Qt identifies
the local list update and announcement expiry separately and reports endpoint/
proxy failures through a specific connection message.

The normal Overview action changes with the current cause: start/resume,
unlock, review a limit, review reserves, inspect progress or open diagnostics.
An unknown/incomplete result never shows a start action. A locked wallet can
block signing even when the service itself is running. Local listener readiness,
a historical imported external report and confirmed payment remain distinct.

During a payment, **Payment in progress** means that operating capital is
reserved for the ongoing request. It is not a provider failure and does not
prove that a maintenance transaction has been broadcast. **View payment
activity** opens the existing activity view without changing the request.
After reserve outputs have been created, **Waiting for reserve confirmations**
explains that Core continues automatically when they confirm. Neither state
requires starting setup again. Budget/approval problems retain their own next
action; unexpected errors still lead to diagnostics.

Before quote acceptance, **Payment capacity reserved** identifies operating
capacity assigned to the request. A payment can remain in progress while other
slots are available. Historical committed outputs are not current work. After
confirmation/release the display returns to the current operating state; an
old scheduler wait does not keep a ready provider waiting. The header uses the
same state as the main card. Active providers refresh every two seconds while
the page is visible; finance summaries refresh less often or when capital and
budgets change. Manual Refresh always reads both. Unsaved offer changes remain
in the form and do not stop Overview updates. Genuine errors still take
precedence over normal payment and confirmation waits.

Unchanged observations update data freshness without reloading the forms.
Confirmed income and cost figures remain visible between finance reads;
they do not return to a loading state on every provider poll. Wallet/provider
changes clear the old figures, and failed reads still invalidate readiness.
A previous `PAYMASTER_PROVIDER_SYNCING` scheduler wait is superseded only when
current readiness passes and chain, wallet and transaction index agree.
While that wait remains, automatic refill is temporarily paused; its saved
activation and approved limits remain intact. Genuine faults are never cleared
by this display check. Every payment still performs its own synchronization checks.

After restarting with an existing provider wallet, use **Overview → Start
provider** (or **Resume provider** after a persistent pause). The setup
assistant is optional when the saved settings are unchanged. Wallet unlock,
node synchronization, confirmed liquidity and current budget checks still apply.
While the initial status queries are pending, the overview shows **Reading
provider status** with an animated activity bar, the current check and elapsed
loading time. The bar has no percentage or remaining-time estimate because
queries can take different amounts of time and follow-up steps can vary. The
local display timer makes no additional RPC calls. Controls become available
automatically when the query chain finishes; there is no fixed 30-second
startup delay. The indicator clears on completion, failure or wallet change
and is hidden in privacy mode.

**Pause provider** is the single stop action in the overview. It disables the
provider and autostart persistently, including new pool-preparation signatures.
**Show technical details** contains diagnostics and their refresh action, without
alternate start/stop or enable/disable buttons. The argumentless temporary
`stoppaymaster` remains available through RPC for expert use.
Technical IDs, complete diagnostic codes and channel counts stay in that same
disclosure. Financial previews and confirmations retain their existing guards.

## Enter setup and inspect operation

Select the provider wallet explicitly. In Qt, enable **Show Paymaster operator
controls**, select the wallet, and open **Paymaster Network → Guided setup**.
Use the normal wallet menu to create or select another wallet. The assistant
never silently switches wallets. In CLI:

```powershell
.\src\digibyte-cli.exe -paymastersetup
.\src\digibyte-cli.exe -rpcwallet=provider -paymastersetup
.\src\digibyte-cli.exe -rpcwallet=provider -paymasterstatus
.\src\digibyte-cli.exe -rpcwallet=provider -paymasterstatus -watch
.\src\digibyte-cli.exe -rpcwallet=provider getpaymasteroperatorinfo
```

Without `-rpcwallet`, interactive setup lists loaded wallets and asks for a
name. A new name offers the normal encrypted descriptor-wallet creation RPC.
An explicitly selected unloaded wallet must first be loaded. Setup refuses
nonterminal input before making RPC changes. Automation continues to use the
individual RPCs. `-watch` reads status every ten seconds; Ctrl+C ends it.

For an existing provider, `-paymastersetup` defaults to **status** and also offers
**pause**, **resume**, **unlock**, **backup**, and **setup**. These correspond to the Qt
Overview next action and Settings → Wallet & backup controls. Manual
expert mode remains available. Setup does not store
passwords, and no password belongs in a command argument, file, or log.

## CLI choices for continuous operation

The CLI assistant uses seven numbered stages: wallet, connection, customer offer,
costs/reserves, startup, review/apply, and funding/completion. Each menu explains
the consequences of its choices. Enter keeps the selected proposal, a number or
option name selects another, and `?` repeats menu help. Invalid numeric input is
corrected in place. Decimal point or comma is accepted; do not enter thousands
separators. DD amounts use DD, DGB amounts use DGB, and service fees use percent
(for example `0.50` means 0.50%, not 50%). Percent fees must use 0.10% steps.

The supported offer choices are customer-paid, public sponsored, both public
models, or restricted sponsored-only. Restricted sponsorship requires separate
authorization and zero DD service fee; it cannot be combined with customer-paid
service in one provider policy.

New-provider CLI proposals are:

| Choice | Initial selection and reason |
| --- | --- |
| Payment model | Customer-paid, 0.50% DD service fee; separate DD income and DGB costs. Sponsorship is an explicit alternative. |
| Payment range / offer lifetime | 1.00–1000.00 DD / 60 seconds, the supported maximum review time. |
| Processing / enablement | Automatic / enabled, so Core processes customer queues. Manual processing requires operator intervention. |
| Customer-payment budgets | 0.20 DGB per payment, 1 DGB reserved, 2 DGB per rolling hour and 10 DGB per rolling day; 10/100 completed payments per rolling hour/day. |
| Reserve capacity | Three capacity-check reserves and one payment reserve per required asset; a small initial provider. Additional payment reserves require more capital. |
| Reserve maintenance | Automatic and paid refill proposed, within 0.50 DGB per transaction, 2 DGB per rolling hour and 10 DGB per rolling day. |
| One-time setup fee | At most 0.50 DGB per setup transaction. Core previews total capital and the total fee ceiling before spending approval. |
| Autostart / start after setup | On / yes. Autostart can start the current session as well as later node sessions once ready. |
| Encrypted-wallet access | Continuous until manual lock, wallet unload or node shutdown. A fresh password is still required after restart. |
| Funding assistance | New receiving addresses when reserves are missing, otherwise monitor existing funding. Both remain optional. |

These are finite proposed ceilings, not expected costs or a promise of
profitability. Payment and refill budgets are separate: the initial customer-paid
profile can authorize up to 20 DGB across their rolling-day fee ceilings, plus
separately approved one-time setup fees. The review displays this combined
ceiling as well as the separate budgets. Core never increases them automatically.

Existing providers retain their saved offer, budgets, reserve policy, operating
mode, enablement and autostart as preselected values. The first menu defaults to
reading status rather than reconfiguring an existing provider. Changing the offer
to activate a model with no usable budget displays a new finite proposal for
that model; it is not authorized until the final review. Existing pool-preparation
fee approvals are retained. The GUI's existing autostart defaults are unchanged
by these CLI-specific recommendations.

Keep advanced request-rate limits unless you need them. Customize exposes
human-readable fields and their effects, including comparisons between payment,
reserved, hourly and daily caps. The final review shows the selected permissions.
**Only typing `yes` approves a mutation or spending review**; Enter declines.
Choosing a recommended option alone never authorizes it. Initial pool creation
has its own exact capital/fee approval after configuration review.

## Setup rules checked before applying changes

Both assistants check the proposed offer, all three saved budget classes,
liquidity policy and pool request using the same pure validators as Core before
returning any configuration-writing step. Core still rechecks each RPC and the
current wallet state.

- Restricted sponsorship permits only sponsored service and a zero DD service
  fee. Customer-paid service can be combined with **public** sponsorship only.
- Sponsored-only pool preparation requires zero DD carrier targets. Switching
  the setup targets to zero does not withdraw existing DD outputs.
- Inactive saved budget classes must also satisfy the advertised network-fee
  ceiling. CLI exposes these limits for correction. The GUI proposes lowering
  an inactive per-transfer ceiling when necessary and shows all resulting
  limits in review; it does not raise the saved aggregate budgets.
- Preview and execution use the same proposed values. A saved policy outside
  a GUI input's numeric range stays exact unless explicitly replaced. Restoring
  liquidity defaults retains the saved autostart choice.
- The one-time setup fee ceiling cannot exceed half of Core's monetary range,
  because pool preparation can authorize two funding transactions. DGB input is
  parsed exactly over the field's permitted range.
- A public name is optional for a new identity: at most 32 printable ASCII
  characters, excluding `/` and `@`. Existing identities keep their saved name.

## Complete the setup

1. Confirm the node, active network and selected wallet. The wallet must pass
   Core's descriptor/private-key/provider checks.
2. Review prerequisites. Disabled txindex or pruning needs configuration work;
   index synchronization and activation are waiting states. Index and chain
   heights are exposed where available. Disabling pruning may require fetching
   historical blocks again; neither reindex nor restart is automatic.
3. Choose Tor or clearnet explicitly. **Node connection** prepares dedicated
   Direct binds and the advertised endpoint. Repairing prerequisites preserves
   suitable existing values. New defaults remain Direct out=1, in=16 and
   maxconnections=125; the normal provider minimum is 45. Always inspect the
   effective transport budget after restart because OS limits can reduce it.
4. Review the offer and finite safety limits. User-paid is the default;
   sponsorship is a separate choice. DD amounts are integer cents in RPC JSON;
   DGB amounts are integer satoshis. CLI prompts convert exact DGB/DD decimal
   amounts and percentages to these internal units.
5. Review liquidity targets and the separate recurring maintenance budget.
   Automatic paid refill stays disabled until explicitly approved. Review
   transaction, rolling-hour and rolling-day ceilings; zero never means unlimited.
6. Confirm the complete configuration. Existing providers are persistently
   paused under the provider work guard before settings change. Identity is
   reused. Each step checks the response and rereads Core; a wallet reload,
   unexpected identity, incomplete reply or conflicting saved result aborts.
7. Qt loads a read-only funding preview before approval. Review capital, maximum
   setup fees, recurring limits and **Start provider when this setup is ready**
   together. One approval covers the unchanged task. A changed financial scope
   requires another review before additional spending. The CLI retains its
   separate funding confirmation.
8. Existing autostart is preselected. New CLI providers propose autostart on;
   the GUI retains its existing new-provider default of off. The reviewed choice
   is saved. The optional one-time start waits for readiness without changing
   that saved choice. Pause, wallet
   unload or node restart cancels that one-time intent. Accepted funding work
   remains in Core and can still await funds, wallet unlock or confirmations.

### Optional CLI funding and verified completion

After the exact pool preparation approval is saved, `-paymastersetup` offers:

- `addresses`: generate and display a DGB receiving address and a DD receiving
  address in the selected provider wallet, then monitor incoming funding;
- `watch`: use existing addresses and monitor funding;
- `skip`: omit this optional funding step.

Send each asset to its matching address from your funding wallet. The setup
never sends from another wallet. It checks balances and the same wallet
generation every two seconds. Changed status/balances are printed immediately;
an unchanged wait prints a ten-second heartbeat instead of repeating the screen. Confirmed balances are informational:
Core must find usable inputs, create the approved reserve outputs, and observe
their confirmations. Existing and pending reserves count toward the targets.
Limits are not raised, and a waiting or lost response does not create another
pool preparation.

If the operator approves starting when ready, the CLI requests a one-shot start
and remains open until Core reports a running, locally ready provider with no
active preparation. Saved autostart is a separate choice. Wallet locking asks
for another explicit unlock; fee limits, policy changes and unknown errors stop
with an explanation. A wallet reload invalidates the monitor. Ctrl+C closes the
CLI only: accepted funding and an accepted start request remain in Core; use
Pause provider to stop new authorized work. Local completion does not prove
external reachability.

## Guided daily tasks

**Restore reserves** automatically includes confirmed and pending reserves in
its preview. Review only the missing capital and maximum DGB fees. The main
next action also offers restoration and a one-time start when a stopped provider
needs reserves. A locked wallet or insufficient balance is a waiting condition;
limits are never raised automatically.

After releasing the last DD payment reserve, the saved payment-capacity target
is deliberately zero. A user-paid offer cannot operate with that target. Choose
**Restore reserves and start** on Overview: the single review proposes the
minimum DD capacity required for that offer, shows the old and new targets,
missing capital, maximum setup fees and unchanged recurring limits. Approving
restores this deliberately released capacity and requests a one-time start after
readiness. Declining changes nothing. Refreshing or reopening the wallet alone
never raises a reduced target. Existing higher targets, replenishment choices,
customer-payment limits and Autostart are retained.

Overview no longer duplicates the reserve and budget settings buttons below
the status cards. Those controls remain under **Settings → Automation & reserves**
and **Settings → Spending limits**. The highlighted next action runs the guided
task; it does not merely focus a configuration button.

**Withdraw earnings** previews only carrier value above the required base. The
combined confirmed excess must reach the network's minimum DD output (currently
1.00 DD). For example, 0.06 DD remains accumulated in the wallet until another
0.94 DD is available; the operating-capital base is not used to make up the
minimum. Both the guided and advanced payout controls remain disabled below
this threshold, with the current amount and minimum explained. Review
the DD payout and DGB fee once. Core journals the transaction; waiting for a block
does not require another execution. **Release one DD reserve** explicitly
reviews the selected reserve, provider pause and reduced carrier target together.
The reserve selector is refreshed from Core before each new review, even when
an older list is already visible. The choice is bound to the displayed outpoint;
a list refresh cannot silently select a different reserve. If Core reports
`PAYMASTER_CARRIER_SLOT_NOT_RELEASABLE`, the reserve may be reserved, unconfirmed,
spent or already released. The task explains this and offers **Refresh reserves**;
that action only reads status and never repeats a release. Wait for active work
to finish, then explicitly select and review an available reserve. Raw error codes
remain available under **Show technical details**. An already-approved pause is
not undone when release fails.

The released carrier becomes ordinary wallet balance; that reduced target is not
automatically rebuilt. Other individual reserve adjustments remain under advanced
automation/reserve settings.

An active task shows Checking → Review → Executing → Confirmations → Complete.
Confirmation counts come from Core. An unknown wait has an activity indicator
and a reason, not an estimated percentage. Planned fee authorization, fees of
broadcast unconfirmed transactions and confirmed costs are separate amounts.
A reserved budget without a transaction is not shown as waiting for a block.

Overview refreshes on opening and after actions, every two seconds for active
work and every ten seconds otherwise. Requests never overlap; old-wallet replies
are discarded. Financial actions require complete current snapshots. Closing the
page does not cancel accepted Core work. Cancellation can release only steps
that Core proves have no saved transaction. Technical details and past action
results do not replace the current status.

On interruption, completed settings and approved preparation remain in Core.
Reopen setup and review those values. A lost funding reply can be recovered by
another preview of the same request; Core's existing maintenance journal binds
its authorization. Do not generate another identity or manually duplicate
outputs. Failed setup does not create a new start request or additional spending approval.

## Node configuration: review, backup, apply, restart

Both UIs call the node-side RPCs. Remote CLI never writes its local configuration:

```text
preparepaymasternodeconfig {"paymastermaxoutbound":1}
applypaymasternodeconfig {"plan_id":"PREVIEW_ID","settings":{"paymastermaxoutbound":1}}
```

Use the exact canonical `settings` and `plan_id` from the preview. The only
write target is the connected node's active `digibyte.conf`. Allowed settings
are digidollar, paymaster, prune (0), txindex, v2transport, maxconnections,
paymastermaxoutbound, paymastermaxinbound, paymasterbind and paymasterendpoint.
Values belong to the active network; other sections and comments are retained.

The preview rejects targeted command-line, forced or settings.json overrides,
conflicting includes, ambiguous scalar duplicates and negations. Unrelated
includes are preserved and included in the preview fingerprint. Files are
bounded to 1 MiB and at most 16 includes. Symbolic-link/nonregular targets and
unreadable inputs are rejected. Default-section mainnet list binds require
manual relocation to `[main]` because list values merge across sections.
Negated includes require manual review. These conservative cases do not prompt
for elevated rights or rewrite other files.

Changes to the target, included files or on-disk settings invalidate the plan.
Apply creates a same-directory backup restricted to the node account (owner-only
on POSIX), writes a temporary file preserving the target configuration permissions,
and atomically replaces the configuration. A new configuration is private by default. It rechecks the plan
before replacement. A failed write preserves the old config. Backup paths
appear in the reply. A successful apply means **saved, restart required**.
Known P2P/RPC port collisions are rejected; availability against other processes
can only be established when the listener is actually opened after restart.

For Tor, below an operator-managed `HiddenServiceDir`, configure:

```text
HiddenServicePort <announced-port> 127.0.0.1:18450
```

Use `paymasterbind=127.0.0.1:18450=onion` for this forwarding target and announce
that service's onion endpoint. Reload Tor through the operator's service tools.
The assistant does not create a Tor service. For clearnet, use a numeric local
interface address, a dedicated Direct TCP port and the matching public numeric
endpoint; forward that port through firewall/NAT. Never expose RPC as the
Direct listener. There is no automatic Tor-to-clearnet fallback. Consult the
[capacity guide](digidollar-paymaster-connection-capacity.md) for exact examples.

## Daily operation and pause

The Qt Overview summarizes service, local connection, capital, approved
budgets and wallet/backup state. Detailed budget counters remain in technical
details and CLI status; open work is under Activity. Available, reserved and pending liquidity are
separate. Fee budgets report transaction ceilings, reserved exposure, hour/day
spend and remaining amounts; remaining is clamped at zero, never negative.
Per-model concurrent-reservation and completion limits still apply in addition
to these amounts. Full history and durable payment/recovery details remain in
Activity/Income & costs and the corresponding RPCs rather than every ten-second read.

`getpaymasteroperatorinfo` schema version 1 is read-only. It neither reconciles
journals nor expires records, starts service, signs, funds or probes. Pool
confirmation views are refreshed in memory. Until normal Core reconciliation,
ledger exposure can be conservative. Diagnostics carry code, state, severity,
area and action. Unknown codes remain errors/unknown information, not success.
Local readiness never establishes external reachability or a verified payment.

Normal **Pause provider** uses:

```text
stoppaymaster {"persistent":true,"pause_setup":true}
```

It disables saved autostart and provider configuration before stopping, under
the existing work guard and a wallet transaction. No new background provider
or pool-preparation signatures follow a successful pause. Reservations and
already signed transactions remain; those transactions may still broadcast or
confirm. Explicit later operator RPCs can grant new work. Resume enables and
requests start against the existing policy, without resetting any budget.
The old argumentless `stoppaymaster` remains an ephemeral expert stop and may
be undone by previously enabled autostart.

Setup signing unlocks remain short and separate from operating unlocks.
Qt and CLI default the operating choice to **continuous**, until manual
`walletlock`, wallet unload or node shutdown. The operator must enter the
passphrase; none is persisted. A timed 60–86400 second choice remains available.
A fresh password approval can replace an existing timer without first locking.
This unlocks the entire provider wallet, not only Paymaster signing keys.
It grants no new provider start, spending limits or automatic-refill authority.
Interactive CLI secrets require numeric loopback RPC or a separately configured
local tunnel. After reload/restart a new manual unlock is required even with
autostart enabled.

The underlying RPC is `walletpassphrase "<passphrase>" 0 true`; the optional
third parameter is named `paymaster_until_shutdown` and defaults to false.
Only descriptor wallets with private keys and an existing provider identity,
settings and policy can use it. Ordinary two-argument calls retain their old
semantics, including immediate relock for timeout zero. `unlocked_until = -1`
reports this volatile operating mode; it is never a persisted unlock permission.
Do not put a real passphrase in a shell command/history; use the GUI or interactive
CLI prompt.

## Check externally from your own second node

On a controlled independent node with free Direct capacity:

```powershell
.\src\digibyte-cli.exe checkpaymasterendpoint 'PUBLIC_IP:18450'
# Or use the provider's full onion:port through a configured Tor proxy.
```

The RPC allows numeric public IPv4/IPv6 and valid onion endpoints; loopback is
allowed only on regtest. No hostname resolution or public test service is used.
There is at most one active probe and one start per 30 seconds. It uses the
ordinary existing Direct queue/permits and fresh-work quotas, never a recovery
class, Capacity request, quote, payment or signature. Transport timeout is
bounded; all outcomes release the lease. Busy capacity fails rather than
launching an independent unbudgeted connection.

The report includes network, endpoint, observation time, terminal state and
positively passed v2/Paymaster stages. Unknown stages are not proof of failure
or success. `identity_verified` and `payment_verified` are always false.
Qt **Settings → Node connection → Import external check** imports this JSON as a historical operator assertion,
not a trusted Core readiness result. Reports are invalidated on wallet reload
or network/endpoint mismatch. Status reads never automatically run a probe.

## Verification and acceptance

Targeted local verification on 2026-09-27: MSVC 14.43 syntax checks passed for
the eleven changed C++ implementation/test translation units; a separately
built shared-setup test executable passed 11 cases with 57 assertions.
Python syntax and diff whitespace checks passed, and MSVC project generation
includes the new common, RPC and unit sources. This does not execute the new
wallet-persistence test, Qt tests or daemon functional tests. Those require the
fresh linked build below. Repository guidance applied: CLAUDE required reading,
root/src/test-functional AGENTS, contribution/developer notes, DigiDollar
architecture/maps, C++ formatting and Qt translation policy.

On 2026-09-28 the operator reported build exit 0 for this feature working tree.
The twelve-test Windows functional run passed nine tests. Three fixture defects
were then corrected: refresh the wallet RPC proxy after restart/cookie rotation;
keep the Dandelion stem peer off the dedicated Direct listener's port; and allow
one explicit `retry_transport` for the same signed payment after the intentional
provider crash. Other RPC errors and a second transport failure still fail the
test; the one-payment, financial persistence and duplicate-prevention assertions
remain in place. Dandelion stays enabled in the pool regression.

The targeted rerun of `wallet_paymaster_operator.py`,
`wallet_paymaster_pool_setup.py` and `wallet_paymaster_lifecycle.py` with
`--descriptors` passed in 9/15/31 seconds, respectively (runner exit 0,
55 seconds accumulated, 56 seconds runtime), with all 18 framework unit tests.
Together with the operator's nine passes this covers all twelve selected
functional tests; it is not a complete-suite run. Python syntax and diff checks
passed; flake8 was unavailable. These corrections change tests only and require
no new C++ build. Root and functional AGENTS, CLAUDE, architecture/maps,
contribution/developer notes and functional-test style guidance were applied.

The rerun used these SHA-256 artifacts from the operator's build:

- `src/digibyted.exe`: `4df3cd97d7a4d6a399d4174896f6311906638a4035b63dbd0f20d0158793b289`
- `src/digibyte-cli.exe`: `ab49cd2050c37c647d43e1d9629f4735891fb2e757b075139b3ba1c5eba09fe4`

The operator's subsequent full `PaymasterWidgetTests` run reported 45 passes
and two failures (exit 2). The observation fixture used a different recipient
from its RPC response; it now uses the matching recipient and retains its
negative assertion against unverified payment completion. The setup fixture
now clicks the actual funding confirmation button rather than supplying a
dialog return code. Its funded/pending rows model the corresponding Core
`pool_ready` status. The wizard completion page also explicitly explains the
pending pool state and directs the operator to funding/unlock status.

MSVC syntax checks and incremental builds of `libdigibyte_qt`,
`test_digibyte-qt` and `digibyte-qt` passed. With the new Qt test executable,
`paymasterClientConfirmationRequiresObservation` passed (6.2 s), as did both
rows of `paymasterGuidedSetupBoundsSafetyAndRetriesFailedStep` (13.2 s).
Both focused invocations exited 0 with no failures or skips. These checks follow
root/src AGENTS, Qt test guidance, C++ formatting and translation-string policy;
the new user-facing waiting message uses `tr()`. Diff checks passed.

The operator subsequently reported the complete Paymaster Qt group passing,
exit 0. The 297-case `paymaster_*,netbase_tests` selection then aborted on the
wallet's nonnegative processed-height assertion. An isolated run reproduced
this in `operator_readiness_and_legacy_backup_do_not_write`: `WalletTestingSetup`
loads its mock database without attaching/scanning the wallet. The fixture now
sets its processed height/hash to the existing chain tip before taking the
read-only database snapshot, and releases the setup wallet lock before calling
readiness. The production assertion and readiness checks remain unchanged.

The incrementally rebuilt `test_digibyte.exe` passes that isolated regression:
1 case, 11 assertions, exit 0. Root/src guidance, wallet test-fixture conventions
and diff checks were applied. The operator subsequently reported exit 0 for the
complete selected unit rerun (`paymaster_*,netbase_tests`, previously enumerated
as 297 cases). No final assertion count was supplied. Together with the full
Paymaster Qt pass and twelve functional passes above, this closes the focused
Windows regression checks for the feature working tree. These are separate
recorded runs, not a clean build or the full repository test suite.
Platform/failure-injection checks and practical Tor/24-hour acceptance remain
outstanding. Earlier unit counts do not cover this feature build. The daemon
and CLI artifacts above were not changed by these Qt or fixture corrections.

Run from this feature branch with newly built daemon, CLI, unit and Qt binaries.
Use the complete [Windows or Linux build/test runbook](digidollar-paymaster-testing.md).
Full builds generally take tens of minutes or longer; focused tests take
seconds to minutes and the full Paymaster group can take tens of minutes.
Record commit, dirty diff, binary hashes, commands, logs and exit codes.

After building, the first Windows check is:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
.\src\test_digibyte.exe '--run_test=paymaster_*,netbase_tests' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Paymaster unit tests failed' }
python test/functional/test_runner.py wallet_paymaster_operator.py wallet_paymaster_pool_setup.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Paymaster operator tests failed' }
```

Then run the nine previously used functional tests and the Qt group using the
runbook. The new operator test covers pause/reload persistence, preview binding,
configuration overrides and includes, direct diagnostic release/rate limits,
and unattended CLI refusal. Shared setup unit tests cover finite defaults,
wallet generation, ordering, saved-state drift, pool replies and budget math.
The Qt guided fixture exercises the common controller through the Qt adapter.
These tests are useful evidence, not a claim of complete scenario coverage.

Before integration/release, the following remain mandatory:

- Fresh full build and all focused unit, Qt, operator, pool and nine functional
  tests. Include locked/unlocked wallets, user-paid and sponsored setup,
  response loss and interruption after each write. Check no duplicate identity,
  output or approval. Test read-only/unwritable configs and write failure on
  Windows and Linux; preserve old config and verify backup permissions.
- Concurrent autostart, scheduler and pool-preparation pause tests. Verify
  persistence across node restart, outstanding signatures and unchanged budgets.
- Two controlled real Tor nodes: diagnostic, separately authorized test payment,
  provider restart, manual unlock and another payment. Record both node states;
  an external transport report alone does not satisfy payment acceptance.
- At least 24 hours on testnet: refill, budget pause, connection loss and
  recovery. Sample status, RSS/CPU, queue depths and ordinary relay peers every
  minute; preserve logs without secrets. Run repeated diagnostics during load
  and confirm recovery quotas and normal relay remain available.
- CLI/Qt practical walkthrough with wallet creation/selection, unconfirmed or
  absent funds, unlock expiration, unknown/malformed responses and wallet
  switching. Record outstanding usability or correctness defects explicitly.

No wallet-format migration, consensus change, new financial journal or
Paymaster wire-protocol change is introduced by this workflow.

### Reserve maintenance with fulfilled targets

A fee reservation for a planned refill is not a sent transaction. If confirmed
reserves already satisfy that asset's targets, the automatic runtime reconciles
the saved wallet transactions first and releases any obsolete unsigned recurring
refill job and its fee reservation. Explicit setup approvals and transactions
already committed to the wallet are retained. This runs during normal automatic
provider processing; it does not require increasing a limit or rebuilding the pool.
A saved plan without a transaction has no confirmation progress. Income and costs
are loaded by opening Finances; an unloaded summary does not mean the financial
ledger is missing.

### Wallet backup location

Backup controls live in Settings → Wallet & backup. Overview's backup
reminder opens that section; Income & costs has no second backup panel. The
central controls remain available after a successful backup. Both backup methods
refer to the complete provider wallet, including its financial records.

### Spending limits at a glance

Overview shows a separate budget block for each enabled payment model and for
reserve maintenance. Each block aligns Spent, Limit and Reserved budget in rows;
the heading identifies DGB and the rolling 24-hour period. Reserved budget is
not a confirmed expense. Detailed approvals and other time limits remain under
Settings. Privacy mode hides and clears these amounts.

The daily and booking tables follow the selected light/dark theme, including
alternating and selected rows. Amounts and counts align right; booking text and
UTC dates align left. Columns and row heights fit their content, and the table
can scroll horizontally when the window is narrow.

### A restored transfer still waits after the payment was confirmed

A client restart can leave a saved transfer waiting when the provider's signed
result was lost from the transport inbox. A previous version could report
`PAYMASTER_INVALID_CAPACITY_DGB_CHAINSTATE` on **Retry the exact provider step**
because the original payment had already spent that reserve. This error alone
is not evidence of a failed payment. Check the transaction before creating
another payment for the same purpose.

With the corrected client, the explicit retry collects the existing signed
result, or requests it again using the unchanged authorized artifact. A spent
reserve is accepted only when Core validates the exact final transaction already
present locally. No new recipient, amount, signatures or payment attempt are
created. The GUI continues this request for at most two minutes, then offers a
status check if the result has not arrived. Errors, a wallet switch or entering
privacy mode end the active continuation. Merely restoring a saved transfer
remains observational; it does not submit anything automatically.

After 240 confirmations the provider normally discards detailed payment replay
records. An older client could then cause `PAYMASTER_SUBMIT_BINDING_MISMATCH`
and leave one submission blocking the provider. The corrected client completes
an explicitly retried transfer from the deeply confirmed local transaction,
validated against its saved consent and signatures. This also works with a
stopped provider and for a payment without client change. The already charged
service fee is settled once; no transaction is signed or broadcast. The provider
consumes stale/mismatched submissions individually instead of treating them as a
persistent service fault. Genuine wallet database failures still require review.

### CLI/RPC reconciliation of an already confirmed payment

These corrections run in the wallet Core shared by `digibyted` and
`digibyte-qt`. The CLI is an RPC client; replacing only `digibyte-cli` does not
update the wallet's payment processing. The scheduler and stale-provider-submit
corrections therefore apply equally to providers operated through RPC.

For an old authorized transfer, select its client wallet explicitly and use the
existing request UUID. `processpaymasterresult "<request UUID>"` now also checks
for the exact locally confirmed payment after provider replay records have been
pruned. It can settle that observation after the signing/retry deadline, without
renewing consent. A completed observation returns `processed=true`,
`session_state="CONFIRMED"` and `txid`; it omits `result_status` and
`result_sequence` because no provider-signed receipt was received. Later calls
can return `processed=false` with the same known `txid`: this means no new result
was processed, not that the payment failed. If client details have also been
pruned, inspect the durable session through `getdigidollarsendsession`.

The same reconciliation is used by an authorized `senddigidollar` resume with
unchanged order/options and by `resolvepaymastersession` with `retry_same` while
that action remains available. An exact already-signed
`walletprocesspaymasterpsbt` retry returns the persisted signature and confirmed
`txid`, with `queued=false`. None of these observations creates another payment,
charges a second service fee or fabricates a provider receipt. Read-only status
and refresh calls do not acquire signing or retry authority.

Wallet-tip and periodic maintenance now also settle an exact original payment
already known to the client wallet after 240 confirmations, even when its provider
reply was lost and its retry deadline has expired. This closes the stale pending
session without another submit or fee charge. Normal receipt pruning still applies;
the confirmed transaction remains available through the wallet transaction history.
Qt recognizes Core's validated local confirmation even without a provider receipt;
it does not show an unproven terminal state as a successful recipient payment.

An explicit `resolvepaymastersession` `cancel_to_self` call checks the same
validated original payment before preparing recovery. If that payment is already
deeply confirmed, it returns the original confirmed session and creates no recovery.
Previously such a call could report `PAYMASTER_RESERVED_INPUT_UNAVAILABLE` because
the successful payment had spent its original input. An input spent by a different
transaction is not proof of payment: the existing recovery availability and
authorization checks remain mandatory. Inspect the exact original transaction and
session rather than starting a replacement payment based on that error alone.

### Client retry after an unsigned quote expires

`PAYMASTER_INPUT_ALREADY_RESERVED` means that a required input is already
protected by a Paymaster reservation or an ordinary wallet coin lock. Inspect
the saved transfer before retrying; this error alone does not prove whether a
previous payment was sent. Never clear all coin locks as a generic workaround.

An older client expiry path removed an unsigned expired quote's database
reservation and persistent lock but retained its in-memory coin lock. The
session became FAILED with a QUOTE_EXPIRED attempt, while a new request failed
on the same input. The corrected path releases the in-memory lock after the
database commit. No signature or payment is created by this cleanup.

For this specifically verified stale-lock case, rebuild and restart the client
wallet. The lock is not restored because its persistent record was already
removed. Inspect the current transfer before using its offered resume or safe
unsigned-cancellation action. This does not apply to signed or ambiguous
transfers, whose recovery state and reservations must remain protected.

### Offer approval countdown and automatic unsigned closure

The client approval window shows the seconds remaining until the exact offer
expires. The countdown does not contact the provider or extend the offer. At
zero, approval closes and Core is asked whether the unsigned request can be
closed safely. Live-send errors follow the same procedure. Once closure is
verified, an inline explanation replaces error popups and the recipient and
amount remain entered for a new preparation. No new payment starts by itself.

If Core reports a signature or cannot verify closure, the transfer remains
protected. **Check current status** reads the saved result; it never repeats a
cancellation after a lost reply. Technical authorization identifiers are under
the approval window's Details control. Changing wallets or enabling privacy
mode closes an open approval window without accepting or canceling that wallet's
request.

## Reviewing node connection settings

**Settings → Node connection** contains the editor for listener addresses,
the announced address, required node features and connection limits. These
settings apply to every wallet on this node. Each field shows its effective
value/source, editable saved value or lock reason, and any pending restart.
**Refresh connection settings** rereads source/conflict information. The setup
assistant embeds the same editor. Start-command/BAT,
`settings.json` and forced runtime overrides are labelled and read-only. For
example, `-paymasterbind=127.0.0.1:18450` in a Regtest launcher controls that value;
change it in the launcher and restart, or deliberately move it into the node's
configuration and remove the overriding argument before editing it here.

Only changed editable values are submitted by **Preview changes**. Unchanged
values do not cause override errors. Review the changes in the same window,
then choose **Save reviewed changes**. Editing any value invalidates the preview.
Core rechecks configuration/override conflicts; failures stay in the window and
retain your entries. Included files, duplicate/negated entries and conflicting settings.json
values are checked on opening/refresh as well as during the guarded save. No restart, Tor/firewall setup, reindex,
wallet unlock or provider start occurs automatically. Wallet switch/privacy
closes the editor; a write already dispatched may still finish.

The Overview cards separate payment capacity and pool principal from earnings.
Operating capital groups ready payment capacity, DGB network-fee reserves and
DD base capital; per-pool counts remain under Show reserve counts. Spending
limits show DGB on every amount (these are network-fee budgets, not DD service
fees). Income & costs groups confirmed lifetime income/costs and current
accumulated/withdrawable DD earnings. Funds & reserves holds the payout
actions and their minimum-payout explanation.
Lifetime income is not the currently withdrawable balance.

While a provider RPC is executing, the provider pages are disabled until its
result/error handling and queued follow-up reads complete. Review dialogs
remain usable. A blockchain confirmation wait is a separate task state;
its allowed actions are still determined by Core and the task controller.

Saving larger reserve targets does not raise the approved refill fee ceiling.
If Core reports `PAYMASTER_MAINTENANCE_FEE_EXCEEDED`, recurring refill is blocked
before a transaction is saved or broadcast. The current task stops showing a progress bar and
offers **Review refill cost limit**. Review the separate finite maintenance
limits and explicitly save any change; targets alone do not approve higher fees.
The same operator diagnostic directs RPC/CLI users to liquidity review. An
already saved transaction continues to show its confirmation progress.

For a wholly uncreated setup plan blocked by `PAYMASTER_POOL_FEE_LIMIT`, use
**Review setup fee and continue** on Overview. The dialog displays the saved
fee ceiling and diagnostics, accepts a new finite DGB ceiling and an explicit
start-after-readiness choice. Continue cancels only the uncreated plan, then
opens the existing capital/total-fee preview for separate approval. Declining
the first dialog changes nothing; declining the subsequent preview leaves the
old plan cancelled and authorizes no replacement. A saved transaction or an
unconfirmed cancellation blocks replacement. Recurring maintenance budgets
are unchanged. The GUI never invents an estimated fee when Core provides none.
The replacement review remains available across background status reads.

If a start is already requested but reserves are missing and no reserve task or
confirmation is pending, Overview shows **Restore reserves and start**. Review
the missing capital and fee ceiling there. A start request alone does not approve
reserve creation, and automatic replenishment may be disabled. After approval,
Core creates the reserves and starts when ready. Declining the review leaves the
start waiting and authorizes no spending. The saved autostart setting is unchanged.

Overview maintenance counts include planned work, so they are labelled open
maintenance tasks, not pending transactions.

Background status reads retain the last validated view until their replies
are processed. Valid snapshots update that view directly instead of hiding
and rebuilding its sections. Refresh frequency and command guards remain
unchanged. Initial loading, failed reads and actual state changes remain visible.

After a confirmed provider restart, successful stop/retirement notices are
cleared together with the retirement balance summary. Toggling privacy must
not restore those old notices. Failed or incomplete stop results retain their
error explanation.

The retirement dialog explains that reviewing full capital release is part
of retirement; it no longer shows a disabled checkbox for that fixed step.
The subsequent release still requires separate approval. In Stop Paymaster,
the optional capital-release checkbox remains editable and unchecked by default.

Connection settings supplied by startup arguments or other overriding sources
are displayed as selectable text, including explicit Yes/No values, rather
than disabled inputs. The source and startup option are shown with each fixed
value. If every setting is fixed, the dialog is explicitly read-only and hides
Preview/Save; edit the BAT/configuration source and restart to change them.

### Saving automatic refill settings

Choose an **Automatic refill target** preset in **Funds & reserves** or the setup
assistant. The same editor offers these draft targets and finite refill limits:

| Preset | Parallel payment target | DGB reserves (capacity checks / payments) | DD reserves for user-paid service | Refill ceiling per transaction / rolling hour / rolling day |
|---|---:|---|---|---|
| Small | 1 | 3 / 1 | 3 / 1 | 0.50 / 2 / 10 DGB |
| Standard | 3 | 3 / 3 | 3 / 3 | 0.75 / 3 / 15 DGB |
| Higher capacity | 6 | 3 / 6 | 3 / 6 | 1 / 4 / 20 DGB |

Sponsored-only presets need no DD reserves. Three separate admission reserves
prove capacity; extra admission reserves do not increase parallel payments.
For user-paid service, the smaller DGB/DD payment-reserve count determines
prepared capacity. The preset sets the refill goal, not a maximum payment count.
Overview and the editor show the saved refill goal separately from currently
available payment reserves. Confirmation, available payment budgets and other
operating limits still apply. **Manual** exposes the four independent reserve counts.
Saved combinations that do not match a preset open as Manual without alteration.

The editor shows minimum target capital before fees: 0.10 DGB per admission
reserve, the larger of 0.10 DGB and the offer's network-fee ceiling per DGB
payment reserve, and 1 DD per DD reserve. Existing usable reserves count toward
the target. After saving a smaller target, a separate preview offers to return
excess available DGB and DD reserves to ordinary wallet funds. Review the amounts
and maximum network fees per transaction and in total before confirming. Cancel
keeps the smaller saved target and the existing reserves. **Review excess
reserves** can reopen this flow later; extra reserves remain usable until released.
Active payments and unconfirmed pool work block retirement. Core binds the plan
to the current saved liquidity revision, targets, exact inputs and fee ceiling.
Changed targets or inputs require a fresh preview. Release fees are one-time
costs; customer-payment budgets and recurring refill approvals are unchanged.
The release preview estimates both transaction fees and proposes a separate,
finite ceiling with 10% headroom rounded up to 0.01 DGB, retaining a larger
entered one-time ceiling within the wallet-wide maximum. It does not reserve
funds, allocate receiving addresses, sign or grant approval. **Recalculate fees
and review release** offers a fresh confirmation if the one-time ceiling was
too low. Raising the automatic-refill limits does not raise this release limit.
When Core reports **PAYMASTER_PROVIDER_BUSY**, this attempt has not released
reserves. Qt waits briefly and retries up to three times, using the same
approved plan and ceiling. Core rechecks the plan on each attempt. A changed
plan requires a fresh review; wallet changes and privacy mode stop the retry.
An incomplete reply requires checking wallet activity before another reviewed
plan; it is not proof that no transaction was sent.

Selecting a preset proposes its targets **and** refill ceilings in the form;
it does not save, enable refill or grant spending permission. Existing positive
limits remain exact when opening or refreshing the editor. **Use suggested
refill limits** replaces only the three draft fee limits. The limits allow
room for several inputs/outputs and the DD fee floor, but are not fee estimates
or a guarantee for fragmented funds. Core still pauses if an actual fee exceeds
the approved transaction, hour or day allowance. Larger manual configurations
receive a larger bounded suggestion. The setup assistant keeps refill limits
independent of the customer-payment safety profile; restoring that profile
does not replace refill limits.

For wallets without saved refill rules, the liquidity RPC proposes these same
finite limits based on existing active reserve counts. It does not derive refill
limits from customer-payment budgets, save a policy or approve spending. Saved
policies always retain their original limits.

Automatic refill and permission to pay refill fees are separate saved settings.
To enable paid automatic replenishment, select **Automatically refill missing
reserves** on Overview or under Operation & automation, review the reserve
settings, select **Allow refill transactions within these fee limits**,
and choose **Save settings and approve bounded refill**. The confirmation lists
the automatic-refill choice, target capital and each DGB fee ceiling. Turning on
the first checkbox alone does not approve spending. A zero ceiling blocks paid
refill even when both checkboxes are selected.

Status refreshes preserve unsaved edits. While the approval is open and the save
is running, the page cannot submit another command. After saving, the message
states whether automatic refill and paid refill are enabled. If the reply is lost,
the GUI checks the stored policy before reporting success or offering a retry.
An unconfirmed save leaves the edits visible and explains the error beside them.
A wallet switch or privacy activation during review prevents the save.


## Diagnosing brief pauses when changing wallet tabs

Both client and provider wallets share the Paymaster reservation checks. Balance
and coin-selection scans now load and validate the provider pool once per scan;
provider finance reconciliation also avoids repeated historical event lookups.
These changes reduce work performed while the wallet lock is held. They do not
remove locks or change automatic payment/refill permissions.

To investigate remaining delays, temporarily enable existing benchmark logging
in the affected node's debug console:

```text
logging ["bench"] []
```

Reproduce the short pause and note its time, wallet and tab. The node's
`debug.log` includes `ReconcileFinalSessionsAtTip`,
`ReconcileProviderMaintenance` and `ReconcileProviderFinances` durations.
Those durations cover the whole call, including any lock wait; they are not a
measurement of GUI paint latency. These added records contain function names
and elapsed time, not payment details. Other existing benchmark records may
also be emitted. Disable the category after collecting the observation if it
was previously off:

```text
logging [] ["bench"]
```

Do not release reservations or cancel a payment to diagnose a slow tab switch.
A measured improvement in the bounded scan regression does not by itself prove
that every pause in a live wallet has been eliminated.
