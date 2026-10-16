# Paymaster integration notes

These notes describe `integration/paymaster-v9.26.7` at the documentation baseline
in [PAYMASTER.md](../../PAYMASTER.md). They are not an upstream release announcement
or a security certification. See the [release gate](../digidollar-paymaster-release-gate.md).

## Implemented behavior

- `checkpaymasterendpoint` again checks listener reachability without a provider
  identity. Its diagnostic connection stops after V2/capability negotiation and
  cannot carry payments or authenticate a provider. Success returns
  `state="transport_ready"`; stage observations and a result explanation are
  included. Tor v3 endpoints use isolated SOCKS5 connections with explicit local
  prerequisite errors and no clearnet fallback. The GUI JSON-report import is
  removed; operators read the diagnostic result directly in CLI/RPC.
- **Settings > Node connection > Connection test** provides copyable connection-test commands
  for a second node using the running announced IP/Tor endpoint and network.
  It explains Tor proxy requirements and local-only regtest checks, links to
  connection settings, and clears commands for missing/invalid addresses, failed
  reads, wallet changes or privacy. It does not discover external addresses or
  run a probe automatically.
- Node connection groups addresses, required features and connection capacity
  into responsive forms with prominent values and readable source badges.
  Pending restart notices distinguish running values from saved changes.
  The connection test is a nested view instead of a separate Settings entry;
  reviewed writes and externally controlled read-only settings are preserved.

- Collaborative single-recipient DD transfers with provider-funded DGB fees;
  user-paid DD service fees or public zero-DD-fee sponsorship.
- Exact provider/recipient/fee authorization before client payment signing,
  durable same-request retry and locally observed recipient confirmation.
- Dedicated bounded Direct transport and provider/session-bound channel proof;
  public announcement discovery is separate from reserved payment capacity.
- Wallet-scoped safety budgets, protected DGB/DD reserves, recovery accounting
  and explicit unsigned cancellation without releasing ambiguous signed work.
- Shared GUI/interactive CLI setup, status and reviewed policy changes. Automatic
  maintenance runs headlessly without an open frontend.
- Finite pool funding and separately approved recurring refill. Limited funds
  create fewer complete slots for DGB/DD; usable confirmed slots remain available
  while the remaining target waits for funds or confirmation.
- Uncreated pool preparation reports required, available and missing DD/DGB
  capital consistently through pool/operator RPCs and the GUI. Both outstanding
  steps remain visible, with funding/confirmation guidance and a link to reserve
  settings. Fee ceilings are distinguished from actual fees; status reads do not
  execute preparation or alter its approval.
- Separate available/reserved/total balances, reasoned non-selectable offer
  cards, recommendation and announcement-expiry countdown. An offer can become
  unavailable before its displayed expiry.
- Wallet-wide operating unlock until locking/unload/shutdown, with fresh unlock
  required after restart; no saved passphrase for automatic restart.
- Guarded backup restoration, independent rollback checkpoint, reviewed adoption
  and safe retirement/capital release.
- Management task titles follow saved DGB/DD funding or withdrawal work and
  clear stale waiting states after cancellation. GUI and
  `cancelpaymastermaintenance` provide reviewed cancellation of uncreated
  reserve funding, atomically disabling refill while preserving signed work.
- Saved-target excess release retains underfilled reserve categories and returns
  a no-op when nothing is available to release. DGB-only execution receipts use
  the same exact amount/fee checks as combined release. Manual submission review
  is invalidated by wallet, privacy or provider-readiness changes.
- Stop and retirement take priority over new automatic/provider work while
  allowing the current work owner to finish. Priority is scoped to the request;
  timeout or failed persistence cannot silently confirm a stop or leave a block.
- The overview identifies a confirmed provider stop independently of missing
  reserves or restart budgets. Saved offer models are labelled as stopped;
  disabled refill is not presented as an immediate approval task. Unknown errors
  and restore/checkpoint diagnostics remain prominent.
- Retirement after an unfunded start waits briefly for competing provider work
  and can explicitly cancel uncreated reserve-funding tasks in the reviewed
  release. Signed/ambiguous work and restored-wallet guards remain enforced.
- Settings starts with **Start, pause & retire**, with per-action checklists and
  the retirement balance/backup review. Reserves & refill links there. Pause and
  Stop share a persistent stop; Stop optionally reviews full reserve release,
  while Retire guides the release and final review. Overview and Settings use
  the same Start/Pause guards; the stopped hint no longer implies that released
  reserves were retained. A confirmed restart clears old successful stop/release
  messages while preserving failed or uncertain results.
- A stopped provider can request Resume with saved settings while known
  budget/liquidity/node prerequisites remain unmet. The prerequisite review
  stays visible beside Resume; a pending start is identified and cancellable.
  No readiness checks, spending limits or refill approvals are bypassed.
  Stop is disabled after a confirmed persistent stop; Retire remains available
  for release and backup review.
- Configuration warnings identify mismatched offer network-fee ceilings and
  per-payment safety limits by model and exact DGB amounts. Overview shows
  saved values; both settings forms compare edits against the other saved
  policy and link to the affected page. GUI/CLI setup review, CLI status and
  the operator RPC `configuration_conflicts` field expose the same comparison.
  Readiness and spending checks remain unchanged; limits are never auto-raised.
- Spending-limit forms prevent known invalid saves with inline explanations
  and retain edits after RPC rejection. Confirmed offer/limit saves immediately
  refresh the combined provider status, replacing obsolete Overview warnings
  with a loading state until readback succeeds. Failed reads remain unavailable.
- Operator navigation separates Overview, Activity, Income & costs and Settings.
  Settings groups operation, service/cost configuration and node/wallet tools.
  Reserves & refill now contains capital targets, current reserves and automatic
  refill, with continued status refresh that preserves drafts. Offer & pricing
  and Customer-payment budgets link to related configuration and distinguish DD
  income, DGB payment fees, wallet-owned capital and separate refill fees. Each
  payment model and refill retains its independent budget; no shared wallet cap
  or new spending authorization is introduced.

The built-in node/GUI help groups Paymaster startup flags, node/wallet RPCs and
terminal-only CLI assistant examples separately. RPC names and assistant calls
are help references, not additional node startup arguments.

## Compatibility and migration

The result remains an ordinary DD transfer under existing validation. Minting,
redemption and batch sponsorship are not added. Ordinary history/index checks
remain prerequisites; UI responsiveness changes must not bypass them.

New restricted sponsorship is disabled, including new descriptor/first-acceptance
paths. Legacy records remain readable for accepted transactions/recovery; there
is no implicit conversion to public sponsorship.

Protocol version is 6, with explicitly supported unchanged legacy V5 Direct
layouts. Wallet formats have independent versions/readers. Do not infer general
wire or wallet downgrade safety from a single legacy reader. Back up the full
wallet before upgrades and use historical compatibility tests where applicable.

Restoring old provider state can lose spending authority history. Guards and
independent checkpoints intentionally block unsafe new work. A matching rollback
of wallet and checkpoint together remains outside that protection. Do not clone
an active provider into independently running instances.

## Operational changes to understand

Expired announcements and timeouts are not proof that no payment was signed.
Inspect the same request before retrying; do not generate replacement IDs
blindly. Unsigned closure may leave provider reservation until expiry. Signed
recovery can race the original transaction and needs chain confirmation.

The full refill target is not a readiness gate when confirmed free capacity
exists. Available funds outside reserves may still be too small to fund another
whole slot plus fee. Unknown statistics/readiness must be shown as unknown,
not zero funds or completed synchronization. Confirmation depends on validated
network blocks; local test mining is covered in the test-network supplement.

See [operator guide](../digidollar-paymaster-operator.md),
[client contract](../digidollar-paymaster-integration.md),
[threat model](../paymaster-threat-model.md) and
[v9.26.7 checkpoint](../digidollar-paymaster-v9.26.7-integration.md).

## Documentation navigation

The [task index](../../PAYMASTER.md#choose-your-task) separates wallet-user,
provider, application-developer and Core-review workflows. The
[user guide](../digidollar-paymaster-user.md) explains payment/fee/cancellation
states. Operator setup/restart examples, client response handling and the
Windows build recipe supplement the concise references; they do not change
runtime behavior or renew historical test evidence.

Operational guides now use Mainnet as their primary context, with public-provider
and client configurations, explicit network selection, dedicated Direct versus
local RPC endpoints, and startup checks. [Testnet26 and Regtest](../digidollar-paymaster-test-networks.md)
are separate supplementary instructions. This changes documentation, not
network defaults, validation rules or release acceptance.
