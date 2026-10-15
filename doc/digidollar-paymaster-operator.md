# Paymaster operator guide

Read [node prerequisites](digidollar-paymaster.md) first. GUI and interactive CLI
use shared setup decisions and persisted Core policy. Automatic provider work
runs in the daemon; neither a GUI nor CLI watch must remain open.

## Setup and routine operation

The GUI separates Overview, Funds & reserves, Activity, Income & costs and
Settings. Configure identity/offer, budgets, liquidity and startup behavior,
review the resulting plan, then approve it. User-paid and public-sponsored
services can be enabled independently. New restricted sponsorship is disabled
and is not silently converted to public.

```sh
digibyte-cli -rpcwallet=provider -paymastersetup
digibyte-cli -rpcwallet=provider -paymasterstatus
digibyte-cli -rpcwallet=provider -paymasterstatus -watch
digibyte-cli -rpcwallet=provider getpaymasteroperatorinfo
```

Without an explicit wallet the assistant offers selection or encrypted
descriptor-wallet creation. An explicitly selected unloaded wallet is loaded
first. Setup requires a terminal; noninteractive use is rejected before setup
mutations. Use individual RPCs for automation.

Stages are wallet, connection, offer, costs/reserves, startup, review/apply and
funding completion. Existing-provider entry defaults to status, with explicit
pause/resume, unlock, backup and settings actions. Saved values are preserved
until reviewed changes are approved. Enter declines final mutation/funding
review; type the requested confirmation to approve. Proposed wizard values are
suggestions, not network constants or estimates of future fees.

Node connection changes are separately previewed, backed up and written to
configuration. Applying the plan does not restart the node; restart is required
for those settings. Running-provider offer changes use the reviewed save/restart
path. Changing tabs must not be a prerequisite for saving.

## Encrypted wallets and headless service

Continuous operating unlock uses `walletpassphrase` with timeout 0 and the
`paymaster_until_shutdown` option; `unlocked_until=-1` identifies that state.
Consult same-build help for argument names. Prefer the assistant's secret
input instead of exposing passphrases in process arguments or shell history.
It requires numeric loopback RPC or a configured local tunnel.

This unlock applies to the **whole wallet**, not only Paymaster signatures.
It ends on manual locking, wallet unload or shutdown. The passphrase is not
persisted for restart: encrypted wallets need fresh unlocking afterwards.
Timed unlock is also supported.

| Control | Meaning |
|---|---|
| Provider enabled | Wallet policy permits provider work. |
| Running service | Requests can be accepted when their prerequisites hold. |
| Autostart | Resume service after wallet load when ready. |
| Automatic refill | Maintain saved targets under separately approved limits. |
| Wallet unlock | Keys may sign; this alone neither enables service nor approves spending. |

For durable pause use the GUI pause action or persistent `stoppaymaster`
options, including `pause_setup` if finite setup must stop. Disabling autostart
does not itself stop an already running service.

## Reserves and availability

Protected DGB/DD reserves are excluded from available balance and shown
separately; wallet total includes them. Funds outside the pool may still be
insufficient for refill plus transaction fees. Manual locks, unconfirmed funds
and locked DD collateral are separate constraints.

The target is desired capacity, not an all-or-nothing service requirement.
Confirmed free complete slots can serve while further refill waits for funds.
Refill creates fewer **whole slots** when necessary, waits for confirmation,
and continues toward the unchanged target. This applies to DGB and required
DD carrier slots. See [pool setup](digidollar-paymaster-pool-setup.md).

No usable free slot means no new capacity. Cached announcements may remain
visible until expiry and become unavailable sooner. Exact offers reserve
capacity; listings do not. An open offer is not a completed payment. Activity
counters overlap stages and must not be summed into a payment count.

## Diagnosis

| Observation | Interpretation and next check |
|---|---|
| Reading provider status | Local snapshot loading; autostart may already be serving. Check Core status. |
| Waiting for history/index | Canonical checks remain necessary; do not disable scans. |
| Refill needs wallet funds | Inspect confirmed funds outside reserves, complete missing slots and fee allowance. Existing capacity may still work. |
| Waiting for confirmation | Inspect retained txid, mempool and block progress. Regtest requires mining. |
| Saved task without transaction | No confirmation is pending for that step; inspect prerequisites/funding. |
| Offer exchange failed | Inspect the same client request and provider Activity/logs before retrying. |
| Network statistics unavailable | Unknown snapshot, not zero supply or proof of lost funds; check synchronization/index diagnostics. |
| Restore guard | Use supported reviewed adoption/recovery; do not clear records to bypass it. |

Use operator/provider, liquidity, safety and finance status plus client session
inspection and node logs. Correlate time/wallet/request without publishing
secrets or unnecessary payment metadata. Temporary debug categories should be
restored to their prior settings after diagnosis.

## Backup and retirement

Back up the full wallet after identity and material policy/reserve changes.
Acknowledging a reminder does not create a backup. Restoration is guarded before
autostart; independent checkpoints detect stale wallet copies. Only supported
matching/quiescent states can be adopted after review. Do not operate cloned
stores as independent instances of one identity.

To retire, stop service, disable provider/autostart and resolve protected work,
then preview and approve [capital release](paymaster-capital-release.md). That
returns eligible reserves to ordinary selection without sending funds or
erasing history.
