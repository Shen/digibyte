# Paymaster integration notes

These notes describe `integration/paymaster-v9.26.7` at the documentation baseline
in [PAYMASTER.md](../../PAYMASTER.md). They are not an upstream release announcement
or a security certification. See the [release gate](../digidollar-paymaster-release-gate.md).

## Implemented behavior

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
- Separate available/reserved/total balances, reasoned non-selectable offer
  cards, recommendation and announcement-expiry countdown. An offer can become
  unavailable before its displayed expiry.
- Wallet-wide operating unlock until locking/unload/shutdown, with fresh unlock
  required after restart; no saved passphrase for automatic restart.
- Guarded backup restoration, independent rollback checkpoint, reviewed adoption
  and safe retirement/capital release.

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
