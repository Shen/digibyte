# Paymaster node configuration and RPC reference

Start with the [developer guide](../PAYMASTER.md), then the
[operator guide](digidollar-paymaster-operator.md) or
[client contract](digidollar-paymaster-integration.md). Exact RPC schemas/defaults
come from `digibyte-cli help <method>` in the selected build.

## Built-in help

The GUI Command-line options dialog and node `-help` contain a dedicated
Paymaster section: startup flags, node RPCs, wallet RPCs and terminal-only CLI
assistant examples. Commands shown there are informational; RPC names and CLI
assistant invocations are not additional node startup arguments. Wallet entries
are omitted in builds without wallet support. Use console `help <command>` or
`digibyte-cli help <command>` for the full RPC schema, selecting the intended
wallet for wallet commands. `digibyte-cli -help` documents its own switches.

## Node prerequisites

Paymaster requires DigiDollar/Paymaster enablement, unpruned storage, txindex,
V2 transport and chain/wallet readiness. Example configuration:

```ini
digidollar=1
paymaster=1
prune=0
txindex=1
v2transport=1
maxconnections=125
paymastermaxoutbound=1
```

A provider also configures `paymastermaxinbound=16`, dedicated `paymasterbind`
and reachable advertised `paymasterendpoint`. Put network-specific bind/port
values in the relevant network section. Direct, P2P and RPC ports must differ.
Loopback endpoints are for local tests, not public discovery.

Multiple local regtest instances require separate data directories, P2P/RPC
ports and provider Direct ports. Connect ordinary P2P nodes to the same test
chain. Providers need independent identities/state; do not clone a live wallet.
New nodes still perform normal header/block/history checks. Mine explicitly
on regtest.

For high privacy configure Tor/proxy and reachable onion service; no silent
clearnet fallback. A ready local listener does not prove external reachability.
See [connection capacity](digidollar-paymaster-connection-capacity.md).

## RPC groups

| Purpose | Methods |
|---|---|
| Node configuration | `getpaymasternodeconfig`, `preparepaymasternodeconfig`, `applypaymasternodeconfig`, `checkpaymasterendpoint` |
| Identity and offer | `createpaymasteridentity`, `setpaymasterpolicy`, `setpaymasterenabled`, `setpaymasterruntimesettings` |
| Lifecycle | `startpaymaster`, `stoppaymaster`, `getpaymasterinfo`, `getpaymasteroperatorinfo` |
| Provider budgets | `setpaymastersafetypolicy`, `getpaymastersafetystatus` |
| Client budgets | `setpaymasterclientsafetypolicy`, `getpaymasterclientsafetystatus` |
| Liquidity | `setpaymasterliquiditypolicy`, `getpaymasterliquiditystatus`, `preparepaymasterpool`, `rebalancepaymasterpool`, `getpaymasterpoolinfo` |
| Finance/reservations | `getpaymasterfinancestatus`, `listpaymasterreservations`, `cancelpaymasterquote` |
| Retirement/backup | `releasepaymastercapital`, `withdrawpaymastercarrier`, `acknowledgepaymasterproviderbackup`, `adoptpaymasterbackup` |
| Discovery/reliability | `listpaymasters`, `getpaymasteroffers`, `getpaymasterreputation`, `clearpaymasterreputation` |
| Client | `getpaymasterclientinfo`, `senddigidollar`, `getdigidollarsendsession`, `listdigidollarsendsessions`, `resolvepaymastersession` |
| Low-level protocol | `requestpaymasterquote`, `walletprocesspaymasterpsbt`, `submitpaymasterdigidollar`, `processpaymasterresult`, `processpaymastersubmits`, `processpaymasterrequests` |

`createrestrictedpaymasterdescriptor` remains recognized but new restricted
sponsorship fails with `PAYMASTER_RESTRICTED_SPONSORSHIP_DISABLED`. Legacy
records remain usable for already accepted transfers/recovery.

Configuration apply checks reviewed `plan_id`, backs up the file and atomically
replaces intended settings. It does not restart the node or change live
settings silently. Financial methods require their own approval/authorization;
status inspection is not blanket spending permission.

## Units and limits

DD fields named cents are integers; DGB fields named satoshis are integers.
Rates use basis points. Explicit `senddigidollar` units avoid ambiguity. GUI
displays DD/DGB rather than raw storage units.

Provider and client fee limits are independent. A provider service-fee cap of
zero means uncapped by that field; a client fee limit of zero permits only free
offers. Do not transfer semantics between similarly named settings. Payments,
sponsorship and maintenance have distinct budget/accounting roles.

Status distinguishes confirmed, reserved, in-use and awaiting-confirmation
funds. Available balance excludes protected reserves. Quote expiry, rolling
budget expiry and safe release of signed inputs are different events. A UI
countdown never overrides recovery guards.
