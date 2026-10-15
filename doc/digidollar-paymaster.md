# Paymaster Mainnet node configuration and RPC reference

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

The operational examples target Mainnet. Paymaster requires DigiDollar/Paymaster
enablement, unpruned storage, a synchronized txindex, V2 transport and current
chain/wallet readiness. Use the build selected for your network and follow the
[DigiDollar upgrade and activation guidance](digidollar-operations.md).
Enabling Paymaster does not bypass activation, history or signature checks.

A previously pruned node needs the missing block data again before it can meet
the unpruned/txindex requirements. Allow initial synchronization and index work
to finish; changing `prune=0` does not recreate deleted blocks immediately.

## Mainnet provider configuration

Create or deliberately update `digibyte.conf` in the intended data directory;
see [configuration locations and precedence](digibyte-conf.md). Keep global
settings before `[main]`. Back up an existing configuration before editing and
review command-line overrides. Do not combine `chain=main` with a test-network
selector.

This is a public IPv4 provider template. Before starting, replace
**`LOCAL_NUMERIC_IP`** with an IPv4 address assigned to the provider host and
**`PUBLIC_NUMERIC_IP`** with its routable public IPv4 address. These are
intentionally invalid placeholders, not working endpoints.

```ini
chain=main
server=1
digidollar=1
paymaster=1
prune=0
txindex=1
v2transport=1
maxconnections=125
paymastermaxoutbound=1

[main]
port=12024
rpcport=14022
listen=1
paymastermaxinbound=16
paymasterbind=LOCAL_NUMERIC_IP:12025
paymasterendpoint=PUBLIC_NUMERIC_IP:12025
```

The three interfaces have different purposes:

| Interface | Mainnet configuration above | Reachability |
|---|---|---|
| Ordinary DigiByte P2P | TCP 12024, network default | Peers exchange blocks and discovery announcements. Allow inbound if serving peers. |
| Paymaster Direct | TCP 12025, an operator-chosen example | Clients must reach this separate listener. Open/forward this TCP port to the provider. |
| Wallet/node RPC | TCP 14022, network default | Keep local with normal cookie authentication; do not publish it for Paymaster clients. |

`paymasterbind` is the **local** listener. Use a concrete interface address;
the current Paymaster validation rejects wildcard binds such as `0.0.0.0` and
`::`. On a host with a directly assigned public address, the local and public
addresses can be the same; behind NAT, the local address is usually private.
`paymasterendpoint` is the **externally reachable** address clients use. Behind
NAT, forward that public endpoint to the local Direct listener. A private LAN
address, loopback address, wildcard or DNS hostname is not a public advertised
endpoint: the current interface accepts a numeric routable IP or an onion
address with an explicit port. Use `[PUBLIC_IPV6]:12025` and a suitable IPv6
bind for IPv6. Port 12025 is not a protocol default; choose another unused port
if necessary, distinct from P2P and RPC, and update the bind/advertisement/routing
together.

Do not carry a local test `connect=127.0.0.1:...` or loopback P2P bind into this
configuration. Use normal Mainnet peer discovery. Neither the public endpoint
nor these flags create a provider identity, enable its wallet policy, fund pools
or authorize signatures. Continue with [provider setup](digidollar-paymaster-operator.md#first-provider-setup).

This template leaves RPC on its default local-only interface and uses the
normal cookie; it contains no RPC credentials. Keep that restriction when
adapting an existing configuration. Public RPC exposure is unnecessary for
Paymaster; see [RPC security](JSON-RPC-interface.md#security). Use an appropriately
secured local tunnel for remote administration.

For high privacy, configure Tor/proxy and a separate reachable onion service
for Direct instead of reusing this clearnet template. A local Tor forwarding
bind uses the documented `=onion` suffix. See
[connection capacity](digidollar-paymaster-connection-capacity.md#deployment-and-retry)
and same-build help. High privacy must not silently fall back to clearnet.

## Mainnet client configuration

A wallet that only sends through providers does not need a public Direct
listener or provider identity. Its node still needs the Paymaster prerequisites
and ordinary P2P connectivity. A separate client's minimal configuration is:

```ini
chain=main
server=1
digidollar=1
paymaster=1
prune=0
txindex=1
v2transport=1
maxconnections=125
paymastermaxoutbound=1

[main]
port=12024
rpcport=14022
```

Those default ports assume a separate host or no competing local node.
Multiple nodes on one host need distinct data directories and listening ports;
every provider also needs its own Direct endpoint and identity. A provider and
client wallet can share a node, but select the intended wallet for every RPC.
`server=1` permits external CLI use with the GUI; it does not enable provider
service.

## Start and verify Mainnet operation

The examples below assume installed binaries on PATH. Replace the absolute
data-directory path with an existing directory containing the reviewed
configuration. Start either the daemon or GUI, never both on one data directory.

```sh
digibyted -chain=main -datadir=/absolute/path/to/mainnet-data
```

Use another terminal for CLI access. On Windows, an alternative GUI start from
the repository root after building is:

```powershell
& ./build_msvc/x64/Release/digibyte-qt.exe -chain=main -datadir='C:/DigiByte/Mainnet-Paymaster'
```

Replace that example directory first. For Windows headless operation substitute
`digibyted.exe`; for RPC use `digibyte-cli.exe` with the same `-chain`/`-datadir`
arguments. This PowerShell example reads node status without starting setup:

```powershell
& ./build_msvc/x64/Release/digibyte-cli.exe -chain=main -datadir='C:/DigiByte/Mainnet-Paymaster' getblockchaininfo
```

With installed CLI tools, run these non-spending checks against the same node:

```sh
digibyte-cli -chain=main -datadir=/absolute/path/to/mainnet-data getblockchaininfo
digibyte-cli -chain=main -datadir=/absolute/path/to/mainnet-data getindexinfo
digibyte-cli -chain=main -datadir=/absolute/path/to/mainnet-data getdigidollardeploymentinfo
digibyte-cli -chain=main -datadir=/absolute/path/to/mainnet-data getpaymasternodeconfig
```

Confirm `chain` is `main`, initial block download has finished, txindex is
synchronized, and DigiDollar is active for this chain. After wallet setup,
inspect `getpaymasteroperatorinfo` with the intended `-rpcwallet`: node readiness
alone does not establish wallet/provider readiness or available reserves.

From an independent Mainnet node with Paymaster/V2 enabled, replace the endpoint
in this explicit network probe:

```sh
digibyte-cli -chain=main checkpaymasterendpoint 'PUBLIC_NUMERIC_IP:12025'
```

`transport_ready=true` observes V2/Paymaster transport at that time. The probe
does not authenticate the provider identity, reserve capacity or verify a
payment. An actual payment still requires the normal exact-offer checks and
authorization. A local listener status alone is not an external reachability
test.

Use the [GUI/CLI operator workflow](digidollar-paymaster-operator.md) to review
identity, finite limits, reserve funding, autostart and unlock behavior. Mainnet
funding and payments spend real funds; sample slot counts and fee ceilings are
examples to review, not fixed recommended operating costs. Reserve creation,
refill and payments confirm in network-mined blocks. A wallet timeout or an
announcement countdown does not determine confirmation or safe input release.

## Supplementary test environments

For public Testnet26 and isolated local Regtest configurations, see
[Testnet and Regtest](digidollar-paymaster-test-networks.md). Keep their
directories, wallets and credentials separate from Mainnet.

### Local regtest example

The [local Regtest example](digidollar-paymaster-test-networks.md#local-regtest-example)
includes the two-node configuration, optional four-node port allocation and
explicit mining requirement. It is supplementary test guidance, not the
provider deployment template above.

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
