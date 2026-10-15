# Paymaster integration boundaries

Paymaster is adapted to DigiByte, not the reverse. This rule covers shared
wallet, validation, networking, RPC and Qt paths as well as Paymaster files.
Baseline and integration ancestry are recorded in
[the v9.26.7 integration note](../digidollar-paymaster-v9.26.7-integration.md).

## Unchanged authority

- Ordinary DigiDollar transfer consensus/script validation, activation and
  amount/output rules remain authoritative.
- Existing wallet history scans and index synchronization remain required.
  Paymaster defers work when they are incomplete; it does not declare them done.
- Ordinary DGB/DD sending, key ownership, wallet encryption and manual coin
  locks retain their meaning. Protected Paymaster reserves are excluded from
  ordinary spendable balance, but remain in wallet total.
- Transport/GUI/RPC success is not chain confirmation. Signed/ambiguous work
  remains protected until Core reconciliation establishes a safe state.

Necessary integrations include wallet persistence/signing, coin-selection
reservation guards, RPC commands, CLI setup, dedicated transport and GUI
presentation. Keep those changes narrow and test ordinary paths alongside them.
New consensus rules or weakened validation are not justified by service UX.

## Shared changes that require particular review

| Area | Intended integration | Review obligation |
|---|---|---|
| Wallet available balance/selection | Exclude protected DGB/DD pool and active reservations. | No loss of funds or double counting; manual locks/collateral respected. |
| Wallet load/backup | Restore guard before callbacks; independent checkpoint. | No autostart/signing from stale state; normal wallet loading remains compatible. |
| Unlock | Explicit operating-unlock option until shutdown. | Wallet-wide scope visible; no persisted passphrase or implicit post-restart unlock. |
| History/readiness | Defer provider/client work while canonical scans/index work finish. | Preserve scan coverage and retry from explicit waiting states. |
| Networking | Dedicated Direct listener and bounded capacity. | Preserve ordinary P2P budgets; authenticate channel before metadata. |
| GUI background reads | Avoid blocking GUI on wallet/chain work; represent unavailable snapshots. | Do not turn unknown into zero, completed, ready or successful. |
| Sync presentation | Header/block stage-aware progress. | No invented 100% progress; display fix does not change validation. |
| RPC result/status | Durable request identity and explicit authorization. | Retry cannot create new payment authority; legacy ordinary send behavior preserved. |

## Review procedure

Inspect all call sites and compare the candidate with its upstream baseline.
Separate necessary extension, independently justified upstream bug fix and
Paymaster-only behavior in commits/review descriptions. For an apparent upstream
bug, identify the original behavior and reproduction rather than attributing it
from UI symptoms alone. Do not remove expensive checks merely to make a test
node appear faster; regtest and production defaults can differ.

Use the [source map](../../REPO_MAP_DIGIDOLLAR.md#paymaster-network) and
[test matrix](../digidollar-paymaster-testing.md). Test feature-disabled and
ordinary DGB/DD paths, history/index waiting, encrypted wallets, startup/restore,
reorg and malformed/lost responses. Changes to shared code require broader
coverage than isolated Paymaster happy-path tests.

## GUI and CLI consistency

Both frontends use Core policies and shared setup decisions. Review exact plans
before mutation, distinguish one-time setup from recurring refill approval,
and retain entered payment data after safe cancellation. Offer listings are
not readiness promises. Confirmed usable slots remain available below the
maintenance target. Service status, maintenance status and network-statistics
availability are separate concepts.

Canonical operator behavior belongs in the
[operator guide](../digidollar-paymaster-operator.md), not duplicated dated UI
work logs. The [client contract](../digidollar-paymaster-integration.md) defines
preparation, authorization and safe cancellation.
