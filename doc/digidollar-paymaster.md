# Paymaster node configuration and RPC reference

Start with the [documentation guide](../PAYMASTER.md), then the
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
server=1
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

## Local regtest example

This is a loopback-only test configuration, not a public provider deployment.
Use new dedicated directories, or deliberately adapt existing configurations;
do not replace a live node's configuration blindly. Create each directory before
starting its node and put that node's settings in its own `digibyte.conf`.

For the provider, combine the common settings above with:

```ini
regtest=1
[regtest]
port=18444
rpcport=18443
listen=1
bind=127.0.0.1
paymastermaxinbound=16
paymasterbind=127.0.0.1:18450
paymasterendpoint=127.0.0.1:18450
```

For a client, use the same common settings with:

```ini
regtest=1
[regtest]
port=18445
rpcport=18446
listen=1
bind=127.0.0.1
connect=127.0.0.1:18444
```

Keep common settings before the `[regtest]` section. Do not copy provider
Direct-listener settings onto clients. `connect` establishes ordinary P2P for
blocks/discovery; the advertised Direct endpoint is used for provider requests.
This does not create wallets, fund reserves, approve spending or mine blocks.

For four local nodes, one possible collision-free allocation is:

| Node | P2P port | RPC port | Provider Direct endpoint | Ordinary P2P connection |
|---|---:|---:|---|---|
| Provider 1 | 18444 | 18443 | 127.0.0.1:18450 | Listens for the others |
| Client 1 | 18445 | 18446 | None | `connect=127.0.0.1:18444` |
| Provider 2 | 18447 | 18448 | 127.0.0.1:18451 | `connect=127.0.0.1:18444` |
| Client 2 | 18449 | 18452 | None | `connect=127.0.0.1:18444` |

Give all four distinct data directories; each provider creates its own identity.
Replace the corresponding ports in the two templates. This topology depends
on Provider 1 for ordinary P2P connectivity; it is a local test example, not a
resilient public topology. Confirm matching chain and progress with
`getblockchaininfo`/`getpeerinfo` before investigating payment discovery.

From the repository root in PowerShell, after building, these are alternative
ways to run the configured provider (replace the example directory):

```powershell
& ./build_msvc/x64/Release/digibyte-qt.exe -regtest -datadir='D:/Digibyte/Regtest-Paymaster'
```

For headless operation, run `digibyted.exe` with the same arguments instead of
`digibyte-qt.exe`; do not open both on one data directory. Use another terminal
for the assistant/status. With an existing wallet named `paymaster`:

```powershell
& ./build_msvc/x64/Release/digibyte-cli.exe -regtest -datadir='D:/Digibyte/Regtest-Paymaster' -rpcport=18443 -rpcwallet=paymaster -paymastersetup
```

For Unix, use installed `digibyted`/`digibyte-cli` paths and an appropriate local
data directory with the same arguments. CLI reads configuration/authentication
from that directory; mismatching it, the network or RPC port can reach the wrong
node or fail authentication. These examples use the normal local cookie rather
than putting RPC passwords in a script. Public RPC exposure is not needed for
a public Paymaster endpoint; see [RPC security](JSON-RPC-interface.md).

## RPC scope and side effects

| Operation | Scope / effect |
|---|---|
| Node config and endpoint checks | Node-wide. Preview is distinct from applying configuration; applying does not restart. |
| Identity, provider/client policy, status and sessions | Selected wallet. Use `-rpcwallet` or `/wallet/<name>`; a provider wallet and client wallet can have different policies. |
| Offer directory preview | Discovery information, not an exact quote or funds reservation. |
| Payment preparation | Can reserve inputs/contact providers and sign an input-control proof; not a payment authorization. |
| Exact commitment acceptance | Can sign and submit; a lost reply does not prove no payment exists. |
| Finite pool setup, recurring refill and capital release | Separate state-changing operations with separate review/approval conditions. |

Not every `get...` method is a mutation-free snapshot. In particular,
`getpaymasterfinancestatus` reconciles provider finance records; the documented
`getpaymasteroperatorinfo` and `getpaymasterclientinfo` snapshots do not perform
financial reconciliation. Inspect each method's contract before classifying it
as read-only automation.

Parameter names that look similar do not make operations interchangeable.
Start with the high-level [client contract](digidollar-paymaster-integration.md)
or [operator workflow](digidollar-paymaster-operator.md); low-level protocol
methods are not an alternative way to bypass their authorization boundaries.

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
