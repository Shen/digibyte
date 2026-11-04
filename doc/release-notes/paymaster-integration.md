# Paymaster integration notes

These notes describe the Paymaster development branch. They are not an upstream
release announcement. The [release gate](../digidollar-paymaster-release-gate.md)
records candidate verification and unfinished acceptance work.

## Implemented behavior

- Single-recipient DD transfers with provider-funded DGB fees, either user-paid
  in DD or publicly sponsored. Exact approval and durable same-request recovery
  precede any claim of locally confirmed recipient payment. See the
  [user guide](../digidollar-paymaster-user.md) and
  [client contract](../digidollar-paymaster-integration.md).
- Bounded Direct transport with provider/session authentication before payment
  metadata. Endpoint diagnostics separately test V2/capability negotiation;
  they do not authenticate a provider or establish payment readiness. See
  [node configuration](../digidollar-paymaster.md).
- Shared GUI/CLI setup and daemon-owned operation: reviewed identity, offer,
  budgets, reserves, autostart and unlock. Status distinguishes saved settings,
  current service and confirmed usable capacity. Configuration warnings compare
  offer ceilings with each model's saved budget; they never raise limits. See
  the [operator guide](../digidollar-paymaster-operator.md).
- Finite reserve funding and separately approved recurring refill, with complete
  slots usable below the target. Uncreated work can be cancelled after review;
  signed or ambiguous work remains protected. See
  [pool setup](../digidollar-paymaster-pool-setup.md).
- Reviewed pause, retirement and excess-capital release; backup guards and
  independent checkpoints protect retained provider authority. See
  [capital release](../paymaster-capital-release.md).
- Available/reserved/total balances, reasoned offer exclusions and exact fee
  displays. Wallet-bound background work limits concurrency, rejects stale
  display replies and preserves started mutations through UI cancellation.
  Offer ordering, derived session indexes and finance storage are described in
  the [implementation](../digidollar-paymaster-implementation.md).

## Compatibility and migration

Transactions remain ordinary DD transfers under existing validation. Minting,
redemption and batch sponsorship are not added. New restricted sponsorship is
disabled; legacy accepted artifacts remain readable for recovery without
conversion to public sponsorship.

Protocol V6 requires current discovery/negotiation and accepts unchanged legacy
V5 Direct layouts only where supported. Wallet formats are independently
versioned; a legacy reader does not establish general downgrade safety. See
[protocol compatibility](../digidollar-paymaster-implementation.md#protocol-sequence).

Finance history migrates atomically to v2 event/index records. Older binaries
cannot read the v2 header. Back up the full wallet before upgrade; a failed
migration retains the previous records. Cursor/revision changes require a page
restart. See [finance migration and pagination](../digidollar-paymaster-operator.md#finance-storage-and-page-consistency).

RPC failures can carry `data.paymaster_code` and `data.disposition`. Clients must
follow the [structured failure contract](../digidollar-paymaster-integration.md#structured-execution-failures)
and treat legacy/unknown failures conservatively. Human-readable text cannot
authorize a retry or release a request.

All binaries and test libraries need rebuilding against the updated database
interface; follow the [build runbook](../digidollar-paymaster-testing.md#build-entry-points).
Full-wallet backups preserve provider metadata; seed/descriptor recovery alone
does not. Restore guards and checkpoints do not protect a coordinated rollback
of both wallet and checkpoint or independently operated clones.

## Operational changes to understand

Use the [operator guide](../digidollar-paymaster-operator.md) for the distinctions
between Start/Pause/Stop/Retire, finite versus recurring approvals and the
wallet-wide operating unlock. A restart needs fresh unlocking; autostart stores
no passphrase. Timeout, announcement expiry and UI cancellation do not revoke
signatures. Follow the same durable order after an ambiguous reply.

The [documentation index](../../PAYMASTER.md) selects the relevant guide.
Operational examples target Mainnet; [Testnet26 and Regtest](../digidollar-paymaster-test-networks.md)
use separate wallets, configuration and funds. The
[threat model](../paymaster-threat-model.md) records remaining trust limits.
