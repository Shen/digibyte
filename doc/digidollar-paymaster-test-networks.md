# Paymaster on Testnet and Regtest

This is supplementary testing guidance. The primary operator instructions and
examples target [Mainnet](digidollar-paymaster.md#mainnet-provider-configuration).
Use separate data directories and wallets for tests. Never copy an active
Mainnet provider wallet/identity into a test environment or run cloned provider
stores independently.

## Select the test network deliberately

Choose one network selector in the global part of `digibyte.conf`, before any
section header. Use the same network, data directory, RPC port and wallet in
every daemon/GUI/CLI invocation. When adapting a Mainnet template, replace
`chain=main` and `[main]`; do not append test options to an active Mainnet file.

| Environment | Selector | Config section | Network data subdirectory | Default P2P / RPC |
|---|---|---|---|---|
| Mainnet (reference) | `-chain=main` | `[main]` | Data-directory root | 12024 / 14022 |
| Public Testnet26 | `-chain=test` or `-testnet` | `[test]` | `testnet26` | 12033 / 14026 |
| Local Regtest | `-chain=regtest` or `-regtest` | `[regtest]` | `regtest` | 18444 / 18443 |

These values come from `src/chainparamsbase.cpp` and `src/kernel/chainparams.cpp`.
The current test network is Testnet26; old testnet directories/peers are not
interchangeable. Signet is not a supported DigiDollar deployment.

Tests use separate test funds. DD addresses must match their network (`DD...`
on Mainnet, `TD...` on Testnet, `RD...` on Regtest). Do not reuse fees, timing,
mock-oracle data or activation overrides from a test as Mainnet operating
assumptions. Mainnet acceptance remains governed by the
[release gate](digidollar-paymaster-release-gate.md).

## Common test settings

Place these at the top of each test node's `digibyte.conf`, followed by exactly
one of the network-specific blocks below. These preserve the normal Paymaster
readiness requirements:

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

## Public Testnet26 example

For a public provider, add this block after the common settings. Replace
`LOCAL_NUMERIC_IP` with a concrete IPv4 address assigned to this host and
`PUBLIC_NUMERIC_IP` with your routable public IPv4 address. Wildcard Direct
binds are rejected. Port 12035 is an operator-chosen example, not a network
default.

```ini
chain=test
[test]
port=12033
rpcport=14026
listen=1
paymastermaxinbound=16
paymasterbind=LOCAL_NUMERIC_IP:12035
paymasterendpoint=PUBLIC_NUMERIC_IP:12035
```

For a client-only node, omit the three `paymastermaxinbound`/`paymasterbind`/
`paymasterendpoint` lines. Mainnet's endpoint, NAT/firewall and local RPC
separation rules also apply here: the loopback advertisement exception applies
only to Regtest, **not Testnet**. Forward the separate Direct port for a public
provider; keep RPC local. Nodes on the same host still need distinct ports.

After creating the intended data directory and configuration, start a node with
installed binaries (replace the absolute path):

```sh
digibyted -chain=test -datadir=/absolute/path/to/testnet-data
```

In another terminal:

```sh
digibyte-cli -chain=test -datadir=/absolute/path/to/testnet-data getblockchaininfo
digibyte-cli -chain=test -datadir=/absolute/path/to/testnet-data getpeerinfo
digibyte-cli -chain=test -datadir=/absolute/path/to/testnet-data getindexinfo
digibyte-cli -chain=test -datadir=/absolute/path/to/testnet-data getdigidollardeploymentinfo
```

Expect `chain` to be `test`; wait for synchronization, index and wallet checks.
Fund a dedicated test wallet with confirmed test DGB and, when needed, test DD,
then follow the same [provider setup](digidollar-paymaster-operator.md#first-provider-setup)
using `-chain=test` and this data directory on **every** command. The assistant
does not supply funds, peers or blocks. Public Testnet confirmations depend on
its miners; local Regtest generation recipes are not public Testnet procedures.
Use the [DigiDollar/oracle test guide](DIGIDOLLAR_ORACLE_TESTING_GUIDE.md) for
network-specific diagnostics. Running a Paymaster does not require becoming an
oracle operator.

## Local regtest example

This is a loopback-only test configuration, not a public provider deployment.
Use new dedicated directories, or deliberately adapt existing configurations;
do not replace a live node's configuration blindly. Create each directory before
starting its node and put that node's settings in its own `digibyte.conf`.

For the provider, combine the common test settings above with:

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

For a client, use the same common test settings with:

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

## Regtest confirmations and limits of test evidence

Regtest blocks are produced by the test operator or functional-test harness.
New nodes still validate headers/blocks and wallet history; wait for those checks.
Reserve funding and payment transactions need mined confirmations. Merely
waiting longer or restarting cannot confirm them without new blocks.

Use the [test runbook](digidollar-paymaster-testing.md) and
[DigiDollar/oracle test guide](DIGIDOLLAR_ORACLE_TESTING_GUIDE.md) for controlled
chain/funding fixtures. An approved task with no transaction yet is waiting for
creation prerequisites; mining alone does not fix it. Do not copy activation
overrides, mock-oracle commands or debug validation settings into a public node.

For each test result record the network, source/binary version, command and
observed outcome. Passing a local Regtest payment does not establish Mainnet
reachability, public-network behavior or complete release acceptance.
