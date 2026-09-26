# Paymaster Direct connection capacity

Source: `fix/paymaster-connection-capacity`, originally based on
`12787a2b121f933a69191d1810dc9dd1acb84cae` and updated through
`913daff217f93ac0db87d2774a435e23dfe38313` (2026-09-26).
This describes the working-tree patch, not a released binary. It supersedes
no historical verification evidence.

## Design and resources

Clients default to **one outgoing Direct channel per node**, configurable with
`-paymastermaxoutbound=0..4`. Wallets share a bounded fair queue, never a socket.
Ownership binds a random wallet-load generation, session/provider operation and
privacy profile. Alternative recovery has a separate ownership key.
`src/paymaster/transport.h` owns process-local budgets, RAII permits and queueing;
`net.cpp` owns sockets/workers. Wallet RPCs request a lease without waiting
for network I/O and retain the existing durable financial authority.

| Resource | Bound |
|---|---:|
| Outgoing connecting/handshaking/active channels combined | 1 default; 0â€“4 |
| Active incoming channels across all provider wallets/listeners | 16 default; 0â€“16 |
| Sockets awaiting routed application work | min(8, requested incoming limit) |
| Clearnet connections per keyed group, pending plus active | 4 |
| Starts before v2 allocation | global burst 16, refill 1/second; group burst 4, refill 1/5 seconds |
| Source admission history | 128 keyed groups; idle records expire after 60 seconds |
| Unadmitted handshake replacement under pressure | oldest after 5-second grace |
| Negotiated inbound without admitted request | 10 seconds |
| Waiting payment / recovery operations | 24 / 8 |
| Waiting operations per wallet, both classes combined | 8 |
| Queue tickets plus active entries plus remembered failures | 256, with 8 reserved for recovery |
| Queue wait from original enqueue | 30 seconds |
| Direct TCP connect | at most 10 seconds or shorter configured timeout |
| TCP/SOCKS plus v2/application handshake | 30-second monotonic deadline |
| No accepted application progress | 30 seconds |
| Socket lifetime | 120 seconds |
| Received bytes before application negotiation | 16 KiB |
| Negotiated socket lifetime received bytes | 8 MiB |
| Received bytes, including v2 decoys, per Direct connection | burst 2 MiB, refill 256 KiB/second |
| Processed messages, including controls, per Direct connection | burst 64, refill 8/second |
| Send backlog / receive pause threshold | 2 MiB each |
| Manager inbox and outbox | each 40 messages / 20 MiB |
| Inbox: requests / expected replies / recovery replies | 24 / 8 / 8 messages; 12 / 4 / 4 MiB |
| Outbox: provider replies / local requests / local recovery requests | 24 / 8 / 8 messages; 12 / 4 / 4 MiB |
| Per-peer and per-clearnet-group queue, within each class | each 4 messages / 1 MiB |
| Per-session inbox, within each class | 2 messages / 1 MiB |
| Incoming queue per provider | 8 requests |
| Transport messages per 60 seconds by inbox class | 128 / 96 / 32; total 256 |
| Transport messages per peer/group, within each class | 32 / 32 |
| Expected response registrations | 128; 120-second TTL; removed on peer finalization |
| Scheduled provider processing | 2 wallet cycles / second, up to 4 request/submit handlers |

A complete message can cross the receive pause threshold. Socket/parser storage
is bounded in addition to these application queues; these are not whole-process
RSS promises. Explicit local provider RPC processing remains operator driven.

Wallets rotate round-robin, FIFO within a wallet. Fresh work and recovery
alternate when both classes have work. Active work is not preempted. On the
default single channel a slow request can delay other requests until its
bounded timeout. Four channels reduce this blocking but consume more sockets,
memory and concurrent provider work. Neither setting guarantees payment throughput.

The 30-second connect deadline applies to TCP/SOCKS. I2P's separate SAM setup
retains its existing timing and has not been validated by this patch.

The dedicated listener prevents ordinary inbound saturation from occupying
the Direct handshake reserve. v2 and the Direct marker negotiate transport
only. A shape-valid request to a running local provider is required for
promotion; inbox limits still apply. A Direct marker on the ordinary P2P
listener is rejected, preventing admission bypass. Direct listeners never
enter Dandelion bookkeeping. Admitted work is protected from handshake eviction.
The marker grants no financial authority.

Tor transport grouping uses ephemeral peer identifiers because forwarded users
all appear as loopback. Global start limits and replacement of old pending
handshakes apply. Only accepted nonduplicate application payloads count as
progress; pings do not extend the idle deadline. Durable financial grouping
and authorization are unchanged.

Under pressure, the oldest unadmitted connection may be replaced after five
seconds. Its socket descriptor closes before its permit is reused. A slow
honest Tor handshake may therefore need retry, and shared clearnet networks
have the four-connection group ceiling. New inbound work remains best effort.
These controls do not identify arbitrary Sybil clients or prevent upstream
bandwidth/SYN floods; changing a wire message type cannot claim a reply reserve.

## Total connection budget

Direct reservations are subtracted from effective `-maxconnections` after the
existing ordinary full/block-relay and feeler target. Outgoing permits are
independent of the ordinary semaphore, including its additional/feeler users.
Requested outgoing capacity either fits in full or is disabled. Provider
capacity requires its complete incoming/handshake reserve plus one ordinary
inbound slot. For the usual full-relay 16, block-relay 2, feeler 1 targets:

| Effective maxconnections / configuration | Direct out | Active in | Handshakes | Ordinary in |
|---|---:|---:|---:|---:|
| 32, provider configured | 1 | 0 | 0 | 12 |
| 44, provider configured | 1 | 0 | 0 | 24 |
| 45, provider configured | 1 | 16 | 8 | 1 |
| 48, out explicitly 4, provider configured | 4 | 16 | 8 | 1 |
| 125, provider configured | 1 | 16 | 8 | 81 |
| 125, client only | 1 | 0 | 0 | 105 |
| 125, Paymaster disabled | 0 | 0 | 0 | 106 |

At 16 the ordinary target leaves no Direct reserve. Existing startup logic can
adjust ordinary block/full targets at low limits; calculation uses those actual
targets. FD reduction happens first. Dedicated listeners count in reserved FDs.
Existing manual `-connect` and separately limited addnode behavior is preserved;
account for those exceptions when planning total process FDs.

If any Direct bind fails, provider readiness is false, admission is rejected
and the planned reserve remains conservative until restart.

## Provider configuration and migration

Existing client prerequisites still apply, including v2, index/archive and
privacy checks. Clients need no new setting to use the default one channel.

Providers now require an explicit dedicated bind and public announcement
endpoint. Choose a port separate from ordinary P2P:

```ini
paymaster=1
v2transport=1
maxconnections=125
paymastermaxoutbound=1
paymastermaxinbound=16
paymasterbind=127.0.0.1:12033=onion
paymasterendpoint=YOUR_V3_SERVICE.onion:12033
```

Replace the placeholder with the actual operator-owned onion service. Configure
Tor to forward its port 12033 to 127.0.0.1:12033. The new bind does not create or
reconfigure Tor services. For clearnet, bind the intended local interface and
announce its reachable address/port. Numeric bind addresses and explicit nonzero
ports are required. Multiple binds share one admission pool.

`getpaymasterinfo` and `startpaymaster` report missing capacity or listener
readiness. `listener_ready=true` proves local binds only;
`external_reachability` stays `unknown`. Verify firewall/Tor forwarding
separately. Ordinary gossip continues over ordinary P2P.

Providers without `paymasterbind` fail readiness after upgrading. Inspect open
signed sessions, drain and shut down normally before migration. Keep the old
advertised route available for pending signed operations: stored endpoints and
signed artifacts are not silently rewritten. Wire V5, wallet journals,
manifests, consensus and exact payment artifacts retain their formats. This
does not validate arbitrary older wallet backups for rollback. Do not operate
restored copies of one provider identity concurrently.

## Wallet behavior and diagnosis

A new session can return `CREATED`, zero provider attempts, empty
`reserved_user_inputs`, `connection_pending=true` and `transport`.
Inputs are reserved only after a matching ready lease is acquired. This
reserves local transport, not remote liquidity. A socket can still fail during
or after a database transaction; its failure cannot release financial locks.

Quote acceptance releases the channel before human authorization. Final-result
persistence releases it before confirmation. Recovery releases it after
response validation and final commit. Explicit abandon/fallback cancels only
that session's transport; unload cancels only that wallet-load generation.
RAII destruction releases permits after their owners stop using them.

`getpaymasterclientinfo`, `getpaymasterinfo`, `startpaymaster` and pending quote
responses expose effective limits, in-use counts, waiting count and listener
status. Generic `available` means configured outgoing capacity, not an unused
permit or remote liquidity. Qt distinguishes waiting from connecting.

| State or error | Meaning |
|---|---|
| `waiting_capacity` | Poll same request; original deadline remains. |
| `connecting` / `handshaking` | Permit held while connection progresses. |
| `direct_ready` | Matching v2/application negotiation completed. |
| `PAYMASTER_NO_LOCAL_DIRECT_CAPACITY` | Effective outgoing reserve unavailable. |
| `PAYMASTER_DIRECT_QUEUE_FULL` | Bounded queue/status storage full. |
| `PAYMASTER_DIRECT_WAIT_EXPIRED` | Local queue ticket expired. |
| `PAYMASTER_NETWORK_INACTIVE` | Local networking disabled. |
| `PAYMASTER_DIRECT_PRIVACY_REJECTED` | v2/capture/onion/isolation gate rejected. |
| `PAYMASTER_PROXY_OR_ENDPOINT_UNREACHABLE` | Connect failed; existing socket API does not distinguish all proxy/endpoint causes. |
| `PAYMASTER_DIRECT_CONNECTION_FAILED` | Socket/handshake failed or ownership binding changed. |

After inspection, `requestpaymasterquote` accepts `intent.retry_transport=true`
for explicit local retry. `resolvepaymastersession ... retry_same` clears only
failed transport before resending the exact persisted signed submit. Alternative
recovery accepts `options.retry_transport=true` with `cancel_to_self`.
Do not include these flags in automatic status polling. They renew no signed
TTL, budget or authority. Financial expiry still requires the existing explicit
unsigned-abandon or signed-recovery actions.

There is no automatic replacement payment, new request ID, signature or
downgrade. Failure outcomes remain bounded until explicit retry/abandon or
wallet unload. High Privacy remains onion-only, v2-only, with required randomized
SOCKS authentication and no shared cross-wallet socket.

## Findings and verification, 2026-09-26

All findings concern the source baseline above plus this patch. No public peer
load or live-wallet migration was performed.

| Finding | Source finding / impact | Fix / targeted evidence | Remaining proof |
|---|---|---|---|
| PC-A | Confirmed shared outbound semaphore can block Direct setup | Separate permits/workers; concurrency unit test | Normal outbound/feeler saturation |
| PC-B | Confirmed shared inbound admission rejects clients at ordinary saturation | Dedicated reserve; budget/promotion units | Admission fixture, active 16-peer load, relay progress |
| PC-C | Confirmed endpoint-only reuse lacked wallet/session isolation | Four call sites use scoped leases; isolation units | Concurrent funded wallets and unload/reload |
| PC-D | Confirmed reservation preceded available channel | Lease before PreparePaymentIntent; functional assertions | Fresh financial regression matrix |
| PC-E | Confirmed SOCKS could wait again per response step | Shared monotonic deadline; new netbase test | Integrated netbase tests and Tor failures |
| PC-F | Confirmed readiness lacked transport-budget/listener gates | Effective budgets and additive diagnosis | FD reduction and route verification |

Executed locally:
- **PASS:** fresh isolated MSVC transport executable, 9 cases, 357 assertions:
  budgets, 24 concurrent permit requests, promotion, fairness, owner/privacy
  isolation, expiry, explicit retry and bounded outcomes.
- **PASS:** targeted MSVC `/Zs` checks of changed networking, initialization,
  wallet/RPC, manager and test translation units. Existing generated config
  and dependency headers were reused. These are syntax/type checks, not a
  complete link or release-build validation.
- **PASS:** MSVC syntax/type checks for the Qt wait-state display and shared
  senddigidollar result schema, using the installed Qt and complete Boost headers.
- **PASS:** Python bytecode compilation for all seven modified/new functional-test
  files and helpers; this does not execute daemon scenarios.
- **PASS:** final whitespace/diff check. The original integration worktree was
  left unchanged; all implementation changes are in the isolated worktree.

**NOT_RUN:** fresh full daemon/Qt build, integrated unit/functional suites, real
16-client v2 load, full ordinary outbound saturation, FD exhaustion, all socket
error branches, sustained CPU/RSS tests, real Tor payment/restart, I2P timing,
ASan/TSan and cross-platform builds. These remain operator/release gates.
PC-01/04/08 have targeted unit evidence; PC-02/03/05/06/07/09â€“15 still need
integrated/runtime evidence. New tests cover parts, not every scenario.

## Availability / DoS review, 2026-09-26

**Initial review, before hardening:** the following findings and reproductions
describe the pre-fix snapshot. Their source-level causes are now addressed by
the [implemented fixes](#implemented-dos-fixes-2026-09-26), with targeted regression
evidence. Integrated socket, payment and Tor/load tests remain release gates.
One outgoing channel alone is not remote flood protection.

### PM-DOS-01 — High: unauthenticated admission can occupy every provider slot

The dedicated listener acquires a concurrent handshake permit before creating
the node (`src/net.cpp`, `CreateNodeFromAcceptedSocket`). `DirectPermits::Acquire`
and `Permit::Promote` enforce 8 pending / 16 active slots, but keep no connection
rate or source history. Negotiating v2 and `CAP_DIRECT_CONNECTION` permits
promotion without proof that the peer owns an existing authorized operation
(`src/net_processing.cpp`, `SENDPMASTERS`).

Consequently, peers can occupy all pending slots without application progress,
or all active slots after negotiation. The 30-second handshake/idle and
120-second lifetime deadlines release individual connections; a party can
compete for each freed slot again. A local permit probe confirmed both caps
and 10,000 immediate acquire/release cycles with no admission-rate rejection.
This is an in-memory behavior check, not a measured network connection rate.

The separate socket budgets protect ordinary P2P connection capacity, but
network processing threads are still shared. Repeated v2 setup/packet processing
also performs cryptographic work (`V2Transport::ProcessReceivedKeyBytes` and
`ProcessReceivedPacketBytes`). PING/PONG and v2 decoys do not consume the
Paymaster payload-message window. Lifetime byte caps bound each connection,
not aggregate work per second across replacement connections. Actual CPU and
block-relay latency impact has not been measured.

Required mitigation: bounded global connection-start/work admission before
expensive setup, additional clearnet source/group limits, and fair admission
for established operations. Onion forwarding hides client addresses; a
per-socket key is not a Sybil-resistant identity, and grouping all clients by
localhost is not an adequate replacement. Any stronger admission/challenge
design needs explicit compatibility and legitimate-client starvation tests.

### PM-DOS-02 — High: incoming retries exhaust the budget for unrelated replies

`Manager::AdmitDirectTransport` uses a single 256-message / 60-second window,
with 32 per peer/group (`src/paymaster/manager.h` and `manager.cpp`).
The network handler applies it to both requests and responses and disconnects
the receiving connection on rejection. The patch groups inbound onion streams
by peer ID rather than their shared forwarding address.

The local manager probe admitted 32 copies of the same capacity request from
each of 8 distinct groups. The first artifact was queued; the other 255 were
benign duplicates, bypassing decoded-payload buckets as intended, while all
256 consumed transport admission. The next message from an unrelated ninth
peer was rejected despite spare connection capacity. Admission returned after
the window expired. No signature, funded request or provider response was
needed for the manager-level reproduction.

This window existed before the capacity patch; the new per-stream onion
grouping makes its exhaustion by one party straightforward. It can reject
expected replies on outgoing client/recovery channels as well as new inbound
work. The local recovery queue's eight reserved places do not reserve this
network budget.

Required mitigation: isolate bounded admission for unsolicited incoming work
from locally expected responses and ongoing/recovery operations, retaining a
hard overall ceiling. Response priority must be bound to a local lease/session;
an attacker-supplied message type must not grant privileged admission.

### PM-DOS-03 — High: requests for unknown providers fill the shared inbox

`ValidateCapacityRequestEnvelope` checks the chain, shape and time window,
but not whether the destination provider is served locally. The handler then
queues the request. `Manager::LeaseCapacityRequests` selects only the exact
provider ID; unknown-provider requests have no consumer. The common inbox
allows 40 messages, 4 per peer/group, with no response/recovery reserve
(`Manager::EnqueueDirectMessageResult`).

The local probe filled it with 40 distinct capacity requests from 10 groups,
well within the 16 active-channel and transport-rate limits. A known provider
had nothing to consume; its next request returned `FULL`, even though its
transport admission succeeded. The entries still blocked admission at 59
seconds and were pruned by the probe at 61 seconds, after the 60-second inbox
TTL. Queue classification also
confirmed that a response variant receives no reserved space. In the network
handler `FULL` disconnects the legitimate peer. Thus even low-bandwidth
requests can deny service across provider wallets and client response traffic.

Required mitigation: cheaply reject requests with no locally serviced route
before queueing, preserve bounded capacity for responses to locally outstanding
operations, and apply per-provider/work-class fairness. Recovery routes must
include draining/persisted operations; filtering solely on a currently active
provider flag would break legitimate recovery. Do not discard signed evidence
or release financial reservations as an overload response.

### Review evidence and limits

- **PASS, reproduction of open defects:** fresh MSVC standalone probe using the
  current `src/paymaster/transport.h` and freshly compiled `manager.cpp`;
  all three manager/permit scenarios above completed with exit 0. This confirms
  vulnerable admission behavior, not successful attack prevention.
- The local-only harness is `.ai/paymaster_admission_audit.cpp`; rerun from this
  worktree with `cmd /c .ai\check-admission-audit.cmd` (seconds). It uses existing
  generated configuration/dependency headers and prebuilt utility, crypto,
  consensus, secp256k1 and UniValue libraries. It does not use the prebuilt
  Paymaster manager or a prebuilt daemon. These ignored verification artifacts
  are not release tests.
- Request fields were checked against the current envelope validator by source
  inspection. The isolated executable does not run the wire validators, socket
  handler, wallet worker, signatures or Tor. Its response-variant check isolates
  queue classification and does not validate a signed recovery response.
- The original transport suite's 9 cases / 357 assertions prove bounded local
  resource behavior only. Their earlier pass does not close these DoS findings.
- **NOT_RUN:** end-to-end reproductions with fresh daemon binaries, sustained
  connection churn, control-frame/decoy CPU load, legitimate recovery completion
  during flooding, ordinary block/transaction relay latency, or real Tor.
  Large builds/load campaigns remain operator work under repository instructions.
- Followed repository `AGENTS.md`, `CLAUDE.md` reading guidance, `src/AGENTS.md`,
  contribution/developer notes, current architecture/maps and Paymaster
  implementation/testing/release documents. Consensus and financial authority
  were not changed by this review.

Before release, add bounded regtest regressions for each finding and demonstrate
legitimate progress while hostile traffic runs, including 8/16 occupied slots,
8 x 32 duplicate messages, and 10 x 4 unknown-provider requests. Check CPU/RSS,
queue counters, wallet reservations and normal relay. Existing capacity tests
alone are insufficient. Raw link saturation and SYN floods also require
deployment/network controls; this application review establishes no protection
against them.

## Pull reconciliation, 2026-09-26

Fetched `origin/integration/paymaster-v9.26.6rc2` and fast-forwarded both the
integration checkout and this capacity worktree to
`913daff217f93ac0db87d2774a435e23dfe38313`. The three incoming commits are
`c2dcc753a2` (maintenance ledger boundaries), `ff64de45e4` (client readiness and
CLI contracts) and `913daff217` (CLI/header-fork fixtures). All six affected
files are tests or test helpers; no production code changed.

The 46 local patch files were backed up before the update. Content comparisons,
normalizing line endings, verified preservation of the local patch. The sole
overlap, `test/functional/test_framework/paymaster.py`, contains both the
dedicated-listener/port changes and the incoming `deliver_committed_result`
helper, with no additional differences. All seven untracked operator files in
the integration checkout retained their exact hashes.

At this intermediate pull-only checkpoint, **PM-DOS-01/02/03 remained open**.
The pull changed tests only. The subsequent hardening below addresses those
findings on top of the pulled source baseline.

Post-pull checks:
- **PASS:** Python bytecode compilation of all five changed Python files.
- **PASS:** MSVC syntax/type check of `src/test/paymaster_provider_tests.cpp`.
- **PASS:** preservation/merge checks and whitespace check.
- **NOT_RUN:** execution of the new unit cases, updated daemon-backed functional
  tests, full build or network/Tor load tests. The existing operator commands
  below still apply to this updated worktree; earlier binary results are not
  evidence for the pulled tests.

## Implemented DoS fixes, 2026-09-26

The fixes for PM-DOS-01/02/03 are implemented on top of `913daff217`. The
initial review above is retained as historical reproduction evidence.

- **PM-DOS-01:** `DirectAdmission` bounds starts before CNode/v2 allocation;
  `DirectPermits` caps concurrent clearnet groups. Negotiation retains the
  handshake permit until locally routed application work. Under pressure, old
  unadmitted handshakes can be replaced; the socket descriptor is explicitly
  closed before releasing its permit, even if a poll snapshot retains a shared
  socket reference. Admitted work is protected. Capability retries and pings
  renew no deadlines. Byte/message buckets also bound decoy/control work.
- **PM-DOS-02:** raw windows and decoded peer/group/provider/session buckets are
  partitioned together. Incoming retries cannot consume either reply reserve.
  The aggregate transport ceiling remains 256 messages/minute. The content
  hash is computed after transport admission.
- **PM-DOS-03:** `CanReceiveDirectMessage` rejects unserved provider requests
  before promotion/queue allocation. MANUAL, DRAIN_ONLY and temporarily locked
  running providers retain their route. Stop/unload removes it; persisted
  recovery resumes after provider start, as required by the existing processing
  RPCs. Inbox count/byte and per-provider bounds isolate incoming work. Outbox
  partitions protect local payment/recovery requests from slow provider clients.

Only locally queued outgoing requests register reply admission for the exact
socket, provider, request/session and protocol phase. The live local transport
lease selects its class; the decoded reply must match the registration.
All five request/response phases are tested. Registrations have bounded storage,
expiry and disconnect cleanup. Queued signed evidence, financial reservations
and wallet spending/signature authority are not discarded or changed.

Verification:
- **PASS:** fresh isolated MSVC build using the current transport header and
  freshly compiled manager plus the registered transport/admission suites:
  **22 test cases / 3,328 assertions**. Existing generated configuration and
  utility/crypto/consensus/secp256k1/UniValue libraries were reused.
- **PASS:** seven existing manager queue/rate boundary cases, taken verbatim
  from the updated wire-test source and compiled in a separate local harness:
  **7 cases / 731 assertions**. This exercises the affected count/byte, expiry,
  refill and bounded-map behavior without claiming a full wire-suite run.
- Tests cover reconnect/source limits, byte/control refill, permit release,
  unknown-provider rejection, draining/manual routes, all five reply phases,
  spoofed peer/session/provider/phase rejection, raw and decoded budget
  exhaustion, inbox count/byte reserves, provider fairness, outbox reserves
  and expected-response storage/cleanup.
- **PASS:** targeted MSVC syntax/type checks of `manager.cpp`, `net.cpp`,
  `net_processing.cpp`, the modified wire tests and the main wallet processing,
  discovery and common Paymaster RPC units.
- **PASS:** Python compilation of the modified socket regression. It tests the
  clearnet group ceiling and replacement of a full silent forwarding-listener
  pool while ordinary ping traffic progresses.
- The old local `.ai/check-admission-audit.cmd` reproduces vulnerable pre-fix
  behavior; it is no longer an acceptance test. The hardened local helper is
  `cmd /c .ai\check-dos-tests.cmd` (seconds). Both are ignored local artifacts;
  regression sources are registered in `src/Makefile.test.include`.
- **NOT_RUN:** fresh full daemon/Qt link, execution of the socket regression,
  integrated wire/wallet suites, real Tor, sustained CPU/RSS/relay measurements,
  sanitizers or the wider release matrix. The isolated tests exercise admission
  and accounting, not signed wire validation or funded wallet flows.

The operator commands below select the new registered suite. Run the fresh
socket and wallet cases before release. Public-service admission remains
subject to Sybil contention and deployment bandwidth limits; protected local
reply capacity and bounded resources are the properties implemented here.

## Next operator checks

Working directory:

```powershell
Set-Location 'D:\Digibyte\digibyte-fork\.ai\worktrees\paymaster-connection-capacity'
git status --short
git rev-parse HEAD
```

Use the [existing Windows build runbook](digidollar-paymaster-testing.md#windows-build)
with this worktree as working directory. Keep the documented absolute installed
dependency path pointing to the original checkout and `QTBASEDIR=D:\Qt51510\install`.
Run `python build_msvc\msvc-autogen.py` here, then its MSBuild command with `/m:1`.
Fresh outputs must be built here; do not copy older executables.

Prerequisites: installed v143/MSVC, matching static Qt/dependencies, Python
dependencies including `digibyte_scrypt`, and `test/config.ini` with SRCDIR and
BUILDDIR both pointing here and EXEEXT=.exe. Full build: tens of minutes or
longer. Focused test group: minutes to tens of minutes. Stop on errors or skips.

```powershell
.\src\test_digibyte.exe '--run_test=paymaster_*,netbase_tests' --report_level=short
if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed' }
python test/functional/test_runner.py p2p_paymaster_connection_capacity.py p2p_paymaster.py wallet_paymaster_readiness.py wallet_paymaster_provider.py wallet_paymaster_failover.py wallet_paymaster_offer_selection.py wallet_paymaster_lifecycle.py wallet_paymaster_rpc.py wallet_paymaster_reorg.py -j1
if ($LASTEXITCODE -ne 0) { throw 'Functional tests failed' }
Get-FileHash .\src\digibyted.exe, .\src\test_digibyte.exe -Algorithm SHA256
```

Then run the existing Qt, compatibility/bridge and release matrix. Success means
all expected cases executed, no required skips and exit 0. Retain source diff,
config, executable hashes and logs. Deployment and Tor routing changes remain
separate operator work.

### Operator validation after the MSVC fix (2026-09-26)

The operator built `7075ca046a` with the accompanying lrelease Exec correction
in the capacity worktree: Windows Release build exit **0**, empty error log.
MSBuild no longer parses translation source text such as `Error: %1` as a
compiler diagnostic; lrelease nonzero exit codes still fail the task. A short
isolated check verified both the Welsh catalog success and missing-input failure.

The fresh integrated `paymaster_*,netbase_tests` run passed **283 cases / 10,755
assertions**, exit **0**. The other 3,742 cases were excluded by the selection.
`p2p_paymaster_connection_capacity.py --descriptors` passed in **22 seconds**,
exit **0**, along with all **18** test-framework unit tests. The first socket
run completed its checks but failed runner acceptance due to CP1252 logging of
the Unicode temporary path; the successful rerun used `PYTHONUTF8=1` and
`PYTHONIOENCODING=utf-8`, inherited by child Python processes.

This updates the earlier integrated-build/unit/socket NOT_RUN status. Broader
wallet/integration, interactive Qt, real Tor, sustained load and the remaining
release matrix still require separate evidence. No full clean rebuild or
complete-suite pass is claimed.
