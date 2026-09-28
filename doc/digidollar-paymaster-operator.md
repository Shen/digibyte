# Paymaster operator workflow

Implementation branch: `feature/paymaster-operator-workflow`, based on RC2
`1a08828ca17539d2983fb307c7e33cf857f6562d`, 2026-09-27. This is a feature
candidate, not a release approval. Build and runtime acceptance is still open.
Historical RC2 results do not cover these changes.

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

For an existing provider, `-paymastersetup` also offers **pause**, **resume**,
**unlock**, and **backup** before the setup flow. These correspond to the Qt
Overview actions. Manual expert mode remains available. Setup does not store
passwords, and no password belongs in a command argument, file, or log.

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
   DGB amounts are integer satoshis. CLI amount prompts accept exact DGB decimals.
5. Review liquidity targets and the separate recurring maintenance budget.
   Automatic paid refill stays disabled until explicitly approved. Review
   transaction, rolling-hour and rolling-day ceilings; zero never means unlimited.
6. Confirm the complete configuration. Existing providers are persistently
   paused under the provider work guard before settings change. Identity is
   reused. Each step checks the response and rereads Core; a wallet reload,
   unexpected identity, incomplete reply or conflicting saved result aborts.
7. Confirm the fresh exact pool preview separately. It reports wallet-owned
   principal, DD carrier capital and the maximum total one-time fees. Existing
   covered targets need no second funding approval. Accepted preparation is
   durable; accepted does not mean funded, confirmed, locally ready or running.
8. Offer a full-wallet backup, then explicitly start from Overview or the CLI
   final prompt. Setup always saves autostart off. Choose autostart only after
   the first explicit start. A locked wallet still waits for manual unlock after
   restart. Missing confirmations may leave a completed configuration waiting.

On interruption, completed settings and approved preparation remain in Core.
Reopen setup and review those values. A lost funding reply can be recovered by
another preview of the same request; Core's existing maintenance journal binds
its authorization. Do not generate another identity or manually duplicate
outputs. Failed setup never automatically restarts the provider.

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
HiddenServicePort <announced-port> 127.0.0.1:12033
```

Use `paymasterbind=127.0.0.1:12033=onion` for this forwarding target and announce
that service's onion endpoint. Reload Tor through the operator's service tools.
The assistant does not create a Tor service. For clearnet, use a numeric local
interface address, a dedicated Direct TCP port and the matching public numeric
endpoint; forward that port through firewall/NAT. Never expose RPC as the
Direct listener. There is no automatic Tor-to-clearnet fallback. Consult the
[capacity guide](digidollar-paymaster-connection-capacity.md) for exact examples.

## Daily operation and pause

Overview and CLI status show service, connection, liquidity, budgets, open
work, and wallet/backup state. Available, reserved and pending liquidity are
separate. Fee budgets report transaction ceilings, reserved exposure, hour/day
spend and remaining amounts; remaining is clamped at zero, never negative.
Per-model concurrent-reservation and completion limits still apply in addition
to these amounts. Full history and durable payment/recovery details remain in
Activity/Finances and the corresponding RPCs rather than every ten-second read.

`getpaymasteroperatorinfo` schema version 1 is read-only. It neither reconciles
journals nor expires records, starts service, signs, funds or probes. Pool
confirmation views are refreshed in memory. Until normal Core reconciliation,
ledger exposure can be conservative. Diagnostics carry code, state, severity,
area and action. Unknown codes remain errors/unknown information, not success.
Local readiness never establishes external reachability or a verified payment.

Normal **Pause operation** uses:

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

Setup unlocks are short and separate from operating unlocks. Qt/CLI operating
unlock prompts allow 60–86400 seconds and never renew automatically. An already
unlocked wallet keeps its existing unlock duration. This is wallet-wide
unlocking, not a new Paymaster-only key permission. Interactive CLI secrets
are restricted to numeric loopback RPC or a separately configured local tunnel.

## Check externally from your own second node

On a controlled independent node with free Direct capacity:

```powershell
.\src\digibyte-cli.exe checkpaymasterendpoint 'PUBLIC_IP:12033'
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
Qt **External check** imports this JSON as a historical operator assertion,
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
