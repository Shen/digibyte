# Paymaster release gate and evidence

There is **no complete current-revision release certification** in this record.
The candidate below and historical checkpoints identify actual verification;
implemented behavior alone does not establish release acceptance.

## Unsigned authority, signing deadlines and executable service

Corrections A–G based on `d698e6328bbde2f8b30ac9f6a941832c1d6dfdce`,
checked in the dirty worktree on 2026-11-08. Current contracts are in the
[implementation](digidollar-paymaster-implementation.md); focused commands are
in the [test runbook](digidollar-paymaster-testing.md). No consensus, wire or
persisted record format changed. The explicit A–G request includes E's narrow
shared TCP logging integration, recorded in the change record and boundary policy.

Twenty-eight C++ units (20 production, eight test) were freshly compiled using
MSVC 19.43.34810 / toolset 14.43.34808, `/W3 /WX /Od /MT /std:c++20`; Qt is
5.15.10. Isolated native/Qt links reuse remaining objects/libraries and existing
MOC. These are targeted builds, not clean products or complete transitive-header
provenance. Commands, source/object/header/binary hashes and final logs are under
`.ai/correct-ag-20261108/` (`runs.jsonl`, `compiled.jsonl`, `verification.json`,
response files and native/Qt test logs).

| Executed check | Bounded result |
|---|---|
| Native | 130 selected cases / 164,799 assertions pass on the final native link. Includes independent valid/corrupt/future authority and idempotent release denial with unchanged DB/locks, accepted unsigned cancellation and authenticated negative results, deadline expiry while waiting for the signing lock with unchanged PSBT, per-model readiness/announcement/budget intersection, fee-cap boundaries, timely durable normal-payment retries after capacity TTL, strict recovery expiry, saved-signature decoding and ordinary DD reservation/network/transport controls. |
| Qt | Four selected functions, 11 feature/data rows pass, zero failed/skipped: offer-response bounds 100/101/1024/1025 with excluded entries before the eligible offer, eight fee-warning configurations, signed recovery after quote expiry and wallet snapshot binding. Eight init/cleanup rows are excluded from the feature count. Windows platform/Fusion. |
| C++17 | All 20 modified production units pass current-source `/Zs /std:c++17` with the same warning policy. No linked C++17 product is claimed. |
| Commit snapshots | 38 targeted `/Zs` checks pass against the prepared intermediate source/header snapshots: 26 production checks in C++17 and 12 test checks in C++20, retaining `/W3 /WX`. The index is compared with these snapshots during staging. Artifacts are under `.ai/commit-ag-20261108/`; no complete intermediate product build is claimed. |
| Local controls | Working-tree dependency/cycle checks, six boundary fixtures, changed paths/shared-Core records under the current explicitly extended policy, JSON and whitespace checks pass. This does not certify the historical task/official-baseline changed-path comparison. |

Initial compilation/link, fixture and unsupported offscreen-platform attempts
are separate from the final successful checks. The restart fixture now imports
the same keys/scripts through extended-key descriptors: this Core's literal
`tr(WIF)` public serialization/constant-key lookup loses odd-Y private-key
availability after reload. The new fixture regression explicitly verifies both
parities, locked-state denial and encrypted reload; the general descriptor
reader is outside these corrections. No full product rebuild, daemon
functional run, fuzz campaign, sanitizer or complete suite was executed. Those
operator/CI checks remain required in the test runbook.

## Checkpoint fences, stop exclusion and exact restart

Review follow-up based on `002375aa29993dd387294ce3e7e05e39f2a9f1cc`,
checked on 2026-11-07. Contracts and migration behavior are in the implementation
and integration guides; focused selectors are in the test runbook. This adds
independent client-order metadata and terminal recovery archival keys, while
preserving wire, PaymentSession and tombstone formats and request-hash bytes.

Thirty-six C++ source units were freshly compiled (26 production, ten test/fuzz),
plus regenerated Qt test MOC. MSVC 19.43.34810 / toolset 14.43.34808 uses
`/W3 /WX /Od /MT /std:c++20`; Qt is 5.15.10. Isolated native/Qt links reuse
remaining objects/libraries. These are not clean product builds or complete
transitive-header provenance. Commands, source/object/binary hashes and complete
logs are retained under `temp/paymaster-implement-002375aa/` in `compiled.jsonl`,
`runs.jsonl`, the native/Qt provenance JSON files and `verification.json`.

| Executed check | Bounded result |
|---|---|
| Native | 37 distinct cases / 27,890 assertions pass across the isolated native links. Covers pre-commit rejection without a checkpoint fence, retained post-commit uncertainty, immediate no-broadcast DD tracking/history, actual Stop/work exclusion, exact original orders including a pre-reservation sweep restart, opaque historical proofs, real 239/240-confirmation recovery archival/reorgs, exact replay, finance and ordinary DD/DB cursor regressions. Repeated cases count once. The final direct-facade replay case uses `native-v5`: stopped, absent and disabled managers with a locked wallet and unchanged complete DB/accounting. |
| Qt | Six selected functions pass on `qt.exe`, zero failures/skips: native-record/RPC/Qt exact-order roundtrip and related restore, fallback, action, signed-recovery and snapshot cases. Windows platform/Fusion; 22 lifecycle/data rows include 12 init/cleanup rows. |
| C++17 and persistence fuzz codec | All 20 modified production translation units and the extended persistence-fuzz codec pass current-source `/Zs /std:c++17`; fuzz uses `/Zc:preprocessor`. The codec also compiles in C++20. No fuzz campaign or linked C++17 product is claimed. |
| Local controls | Working-tree dependency/cycle checks, six boundary fixtures, changed-path/shared-Core records against HEAD policy, JSON and whitespace checks pass. |

Initial compile, selector and fixture failures remain in the logs; final fixtures
preserve wallet-height, key-lock, financial and independent-checkpoint assertions.
Archival bounds active recovery processing, not total disk history or existing
complete commit scans. No new daemon, full suite, functional, sanitizer,
Tidy-plugin or clean supported-platform product result is claimed. Those
operator/CI checks and commands remain in the test runbook.

## Exact execution, authorization commits and unavailable balances

Corrections based on `9bd53997af884691940a696d1c4030120419a887`,
checked on 2026-11-07. Thirty translation units were freshly compiled
(18 production, 12 test), including direct consumers of changed balance and
reservation layouts. MSVC 19.43.34810 / toolset 14.43.34808 uses
`/W3 /WX /Od /MT /std:c++20`; Qt is 5.15.10. Isolated links reuse remaining
objects/libraries and existing MOC, so they are not clean product builds or
complete transitive-header provenance.

| Executed check | Bounded result |
|---|---|
| Native | 37 distinct cases / 2,756 assertions pass on `temp/paymaster-commit-20261107/native.exe`: eight direct regressions, 27 related DD/store/recovery/backup cases and two transport cases. Covers exact stale-input vetoes, unknown authority before release, first provider recovery flag-write/commit rollback, reload, exact replay repair and existing ordinary DD accounting. |
| Qt | Six functions pass on `temp/paymaster-commit-20261107/qt.exe`, zero failed/skipped feature cases: unknown availability with retained owned total, watch-only, send/overview wallet binding, privacy and ordinary DGB balance. Windows platform/Fusion; init/cleanup are excluded. |
| C++17 | All 16 modified production translation units, the transport test and four Paymaster fuzz registrations pass `/Zs /std:c++17`. Fuzz checks use `/Zc:preprocessor` in both language modes. The additional provider-RPC/fuzz aggregate changes also compile in C++20; existing designated initializers elsewhere in Core remain outside this correction. No complete Linux/C++17 product build is claimed. |
| Intermediate commits | Forty targeted C++20 syntax checks pass against the exact source and changed-header snapshots of the ten-step series, including reached ordinary callers. This does not establish clean full builds of each commit. |
| Recovery negative control | Recompile only the original HEAD recovery translation unit against the current remaining objects. The unchanged new test fails with Exit 201 and ten assertions at missing flag/atomic rollback/reload/replay checks. Current code passes; this is a causal single-unit control, not a historical full build. |
| Architecture/local controls | Working-tree dependency/cycle checks, six positive/negative fixtures, approved-policy changed-path/shared-record checks, JSON and whitespace checks pass. The operator explicitly approved four additional shared header/test paths; dependency/forbidden-path rules and fixtures remain unchanged. |

The transport follow-up `4edb3a7f42`, checked on 2026-11-07, replaces five
designated aggregate blocks in `net.cpp` using the operator-approved scope
recorded in `6e8efb8908`. The actual `net.cpp` and unchanged `net_tests.cpp`
each compile in C++17 and C++20 with the MSVC toolchain above. An isolated link
of their C++17 objects with reused remaining objects/libraries passes 12 selected
cases / 144,663 assertions on `temp/paymaster-cxx17-transport-20261107/native-v3.exe`
with `--random=487619`: seven Paymaster network cases, ordinary node/listen-port
checks, BIP324 transport and two channel authentication cases. A token scan finds
no C++20 candidates in 148 Paymaster-owned C++ files or the shared adapter.
Exact commands, source/object/binary hashes and logs are retained in that
directory's `applied-verification.json` and adjacent artifacts. These targeted
results do not replace clean product builds, Linux/C++17 or the release gates below.

Commands, hashes, complete logs and failed attempts are retained under
`temp/paymaster-af-fixes-20261107/` (`compiled.jsonl`, `runs.jsonl`,
`native-v4.json`, `qt-v2.json`, `negative-recovery.json`, `verification.json`).
The additional C++17 adapter compiles, final relinks and repeated native/Qt
checks are under `temp/paymaster-commit-20261107/` (`compiled.jsonl`,
`runs.jsonl`, `native.json`, `qt.json`, `verification.json`).
Initial compile errors and Windows test teardown failures with 263-character
checkpoint paths remain in the logs. Final runs use a short workspace-local
temporary directory and retain every checkpoint assertion.
No full-product, functional, sanitizer or complete-suite result is claimed;
the operator/CI commands and prerequisites remain in the test runbook.

## Pool atomicity, recovery and derived-history integrity

Working-tree corrections based on `343cf789e8204762d9b768b9b79dccb8959a1896`,
checked on 2026-11-07. The implementation/operator guides own behavior and the
test runbook owns selectors. No consensus, wire or persisted record format
changed. Shared-Core additions are recorded in the change record.

Eighteen translation units (11 production, seven test) were freshly compiled
with MSVC 19.43.34810 / toolset 14.43.34808, `/W3 /WX /Od /MT /std:c++20`.
Qt 5.15.10 MOC was regenerated and compiled for the new test slots. Isolated
native/Qt links reuse remaining objects/libraries; they establish neither a clean
product build nor complete transitive-header provenance. Commands, hashes,
full logs and failed attempts are retained under `temp/paymaster-af-20261107/`
(`compiled.jsonl`, `runs.jsonl`, `native-v3.json`, `qt.json`, `verification.json`).

| Executed check | Bounded result |
|---|---|
| Native | 64 distinct cases / 23,304 assertions pass after removing overlap: 16 new cases plus related fee/selection, recovery authorization, whole-slot refill, conflict/restart, notification/rollback, 100,000-event warm finance and ordinary database cursor regressions. Bulk comparator/fixture assertions are not separate security scenarios. |
| Qt | Six selected functions / nine feature cases pass, zero failed or skipped feature cases, Windows platform/Fusion. Includes structured uncertain recovery errors, explicit Own DGB selection, unresolved maintenance, and existing live send/recovery/timeout paths. Init/cleanup are excluded; existing style-property warnings remain in the logs. |
| Pool negative control | The unchanged new stale-snapshot test fails with prior application/wallet objects matching HEAD (Exit 201): the newly appended pool entry is lost. Current objects pass. This is a narrow causal check, not a historical complete-build result. |
| C++17 and local controls | All 11 modified production translation units pass `/Zs /std:c++17`. Working-tree dependency/cycle checks, six boundary fixtures, changed-path/shared-Core checks against HEAD policy, JSON parsing and whitespace checks pass. |

Initial compile and fixture failures remain in the logs. Fixture corrections
retain confirmation, budget and bounded-scan assertions; no validation, lint or
compiler policy was weakened. No new daemon, functional, sanitizer, complete
suite or supported clean-product build is claimed. Those operator/CI checks
and their copyable commands remain in the test runbook.

## Maintenance, approval and finance corrections

Working-tree corrections based on `c055dd7a9cdd64b248424724ebeb9c04b2fbb1bf`,
checked on 2026-11-07. Contracts are in the implementation/operator guides;
the test runbook owns selectors. No consensus or wire rules changed. Recovery
finance adds event kind 5; `SendController::View` adds an observation flag.

Thirty-three translation units (25 production, eight test) were freshly compiled
with MSVC 19.43.34810 / toolset 14.43.34808, `/W3 /WX /Od /MT /std:c++20`.
This includes unchanged `wallet/paymasterdb.cpp` for the recovery-notification
overload and all five direct consumers of the changed send-controller layout.
Isolated native/Qt links reuse remaining objects/libraries and existing Qt
5.15.10/MOC; they are not clean product builds or complete header provenance.
Commands, source/object/binary hashes, failures and full logs are retained under
`temp/paymaster-packages-1-7/` (`compiled.jsonl`, `runs.jsonl`,
`native-v5.json`, `qt-v5.json`, `verification.json`).

| Executed check | Bounded result |
|---|---|
| Direct native corrections | Exit 0 in disjoint selections of 14 cases / 950 assertions and two cases / 17 assertions on `native-v5`. Covers maintenance conflict/restart and stale pool reuse, old-quote signing veto, preview entry points, atomic policy binding, recovery accounting/replay, exact maintenance fees and atomic historical repair, unavailable evidence, reputation read statuses and quote-template parity. |
| Related native regressions | Exit 0: 21 additional cases / 2,028 assertions on `native-v5`, including role-limited signing, V5 durable authorization, provider/recovery pre-signature guards, financial migration/notifications/rollback, setup parity and budget retention. The native selections are disjoint: 37 cases / 2,995 assertions. |
| Qt | Seven selected functions / 49 feature cases pass, zero skips; Windows platform/Fusion. Final six functions use `qt-v4`; reviewed reserve restoration uses `qt-v5` after a test-fixture-only revision correction. Init/cleanup are excluded. Existing style-property warnings remain in logs. |
| C++17 syntax | 22 modified production translation units pass `/Zs /std:c++17`. `wallet/rpc/paymaster_provider.cpp` fails at the pre-existing `RPCArgOptions{.skip_type_check = true}` initializer (also present in HEAD); its C++20 compilation passes. No complete C++17 build is claimed. |
| Preview negative control | The same ten preview assertions also pass with only pre-fix `paymasterproviderops.cpp` substituted from HEAD. This release configuration omits `DEBUG_LOCKORDER`; the control does not reproduce the lock assertion and is not causal runtime evidence for the lock fix. A complete lock-debug build remains required. |
| Local controls | Working-tree dependency/cycle checks, six boundary fixtures, changed-path/shared-Core-record checks against HEAD policy and whitespace checks pass. |

Initial include/fixture failures and isolated-link response mapping failures
remain in the logs. No validation, test or lint policy was weakened to resolve
them. No new daemon, functional, sanitizer, full-suite or complete supported
product-build result is claimed. The operator build/matrix commands remain in
the test runbook.

## Scheduler progress and local result authority

Product and test sources at `33624ebae6e265ac2beae97333968aab75335e2f`, checked
on 2026-11-07. The implementation guide owns the nonblocking finalization
and authenticated-local-authority contracts; the test runbook owns selectors.
No wire, persisted record format or shared class layout changed.

Five translation units (three production, two test) were freshly compiled with
MSVC 19.43.34810 / toolset 14.43.34808, `/W3 /WX /Od /MT /std:c++20`, and linked
into an isolated native test executable. Remaining objects/libraries are reused;
this is not a clean product build or complete transitive-header provenance.
Commands, source/object/binary hashes, full logs and failed attempts are retained
under `temp/implement-ap1-ap3-2100dd9/`, including `native-v2-provenance.json`,
`compiled.jsonl` and `runs.jsonl`.

| Executed check | Bounded result |
|---|---|
| Direct regressions | Exit 0: six cases / 771 assertions. Covers index lag after durable commit, unchanged spent budget/exact replay, readable corrupt predecessor and recovery authority, retained messages across TTL pruning and repair, forged remote rejection, and precise native/RPC error contracts. |
| Related regressions | Exit 0: 33 additional cases / 2,286 assertions across PSBT, application results, local finalization failures, confirmed-payment observation, manual-lock ownership, automatic sync deferral, retained inbox messages and ordinary database tests. The selections are disjoint: 39 cases / 3,057 assertions. |
| Negative control | All three selected scheduler/predecessor/recovery regressions fail with the original `2100dd9e77` production objects and unchanged new test objects (Exit 201). The scheduler test releases its frozen queue after the bounded failure so teardown terminates. |
| C++17 syntax | The three modified production translation units pass MSVC `/Zs /std:c++17`. This is not a linked C++17 product or Linux/compiler-matrix result. |
| Local controls | Working-tree dependency/cycle checks, six boundary fixtures and whitespace checks pass. |

Initial fixture failures are retained separately from the final successful run.
No new daemon, Qt, functional, sanitizer or complete-suite result is claimed.
Clean supported builds and the remaining release matrix are still operator/CI work.

## Final witnesses, recovery history and explicit transaction completion

Working-tree corrections based on `83c8effe233406e4b09b36f0c5d3810e089f48eb`,
checked on 2026-11-07. The implementation guide owns the signature, combined
history-limit, observation and transaction-lifecycle contracts; the test runbook
owns the new regression selectors. No wire or persisted record format changed.

Twelve distinct translation units (six production, six test) were freshly
compiled with MSVC 19.43.34810 / toolset 14.43.34808,
`/W3 /WX /Od /MT /std:c++20`. Native, daemon and Qt links are isolated and reuse
remaining objects/libraries; Qt uses the existing static 5.15.10 and MOC.
They do not establish complete transitive-header provenance or clean product
builds. Commands, hashes, full diagnostics and source snapshots are retained in
`.ai/implement-ap1-ap4-83c8eff/` (`source-freeze-final.json`, `compiled.jsonl`,
product provenance and `runs.jsonl`).

| Executed check | Bounded result |
|---|---|
| New and related native regressions | Exit 0: selections of 11 cases / 9,413 assertions and 35 cases / 9,539 assertions. Six new cases cover real-key final-witness hash modes/roles/metadata, combined normal/recovery admission and exact replay at quota, bounded recovery cleanup and failure/restart protection, targeted finance read errors, and batch destruction with reused owner addresses. Database rollback and the independent checkpoint-ahead fence are asserted separately. |
| Final native consumer/ordinary-DB selection | Exit 0: 13 cases / 38,125 assertions on `native-v2`, including the six new cases, session-index commit/abort/failure/reconstruction, native finance capture, checkpoint concurrency and ordinary prefix/range cursors on the configured BDB, SQLite and mock backends. The three native selections cover 53 distinct cases / 47,897 assertions after removing overlap; bulk quota/index assertions are not separate security scenarios. |
| Negative control | The three selected signature, finance-read and recovery-pruning regressions all fail with prior implementation objects (Exit 201, 105 failed assertions). New test objects are unchanged; this is a narrow causal check, not historical-release interoperability. |
| Descriptor functionals | Exit 0: AUTO (10.219 seconds), complete provider scenario (81.656 seconds) and reorg (27.484 seconds), using isolated Regtest directories, seed 1015350342 and port seeds 3381/3391/3401. Covers explicit ordinary DGB/DD sends, paid/public transfers, exact gross/sweep, lost results, alternative V5 recovery, safety depth, finance/backup, encryption/restart/unload and reconfirmation without duplicate accounting. The daemon is freshly linked; the existing CLI is reused. |
| Qt | Five selected feature functions exit 0 with no skipped feature cases: finance/backup, manual Activity results, ordinary wallet transactions, DGB balance breakdown and node-RPC model teardown. Windows platform/Fusion; init/cleanup are excluded. No full Qt pass is claimed. Existing style-property and QSignalSpy metadata warnings remain in the logs. |
| Local controls | Working-tree dependency/cycle checks, six boundary fixtures, shared-path/change-record checks against the unchanged HEAD policy, JSON parsing and whitespace checks pass. |

An initial isolated daemon link used the wrong project working directory and
failed before any test (LNK1181); the corrected link and all current-source test
selections above succeeded without changing compiler warnings or validation.
History/fault-injection fixtures do not simulate physical disk failure or a
measured remote denial of service. Clean supported product builds, Linux/C++17,
Tidy17 with the DigiByte plugin, sanitizers, full test/compatibility matrices and
operational release gates remain operator/CI checks.

## AUTO authorization, disconnect lifetime and retained financial work

Working-tree corrections based on `8cd998047fac1acf71c7e5dc18ff11de21861712`,
checked on 2026-11-06. Fresh AUTO requests with suitable own DGB require an
explicit ordinary `fee_mode=dgb` send; they cannot bypass Paymaster preparation,
approval or retry semantics by falling through to direct signing. Collaborative
retries retain their saved authority. Published connection leases remain owned
until node destruction. Signed unresolved maintenance retains its fee exposure
and reconciliation history, and authenticated client results survive local
failure and TTL pruning until acknowledgement. Typed session lookup distinguishes
absence, unreadable authority and present but ineligible sessions.

Quote, automatic-payment and submit coordinators now delegate named phases;
durable authority remains in the existing store and scoped work/message leases
span the complete operation. The implementation guide owns these behaviors,
and the test runbook owns repeatable selectors.

Forty-seven distinct production/test translation units were freshly compiled
with MSVC 19.43.34810 / toolset 14.43.34808, `/W3 /WX /Od /MT /std:c++20`,
including 40 daemon/native/Qt consumers reached by `paymaster/manager.h` and
the changed network, domain, RPC, wallet and Qt units. Qt uses static 5.15.10.
Native, daemon and Qt links reuse remaining objects/libraries and unchanged
MOC; they are isolated links, not clean product builds or complete transitive
provenance. Commands, inputs, hashes, logs and failed attempts are retained in
`.ai/implement-r1-r5-8cd/`; `source-freeze.json` identifies the final sources.

| Executed check | Bounded result |
|---|---|
| New failure regressions | Exit 0: nine cases / 666 assertions, repeated on final `native-v3`, plus the new alternative-recovery DB-commit/TTL/exact-retry case (77 assertions). Covers real disconnect/message-reader lifetime, all recurring maintenance kinds, both result types, delayed enqueue/replay pruning, session classifications and local read/write/preflight failures. Recovery uses mined inputs, genuine signatures and ordinary mempool validation, starting from saved recovery authority. |
| Related native regressions | Exit 0 on `native-v1`: 53 additional cases / 2,715 assertions across application, PSBT/builder, fairness, maintenance, checkpoint, provider/recovery firewalls and ordinary lock ownership. Together the disjoint selections cover 63 cases / 3,458 assertions. Final production changes after this run only move returned optional payment results and remove unused adapter includes; they were freshly compiled and linked. |
| Qt | Four selected functions / 38 feature rows exit 0 on `qt-v2`: session action matrix, asynchronous send phases, manual submit confirmation and wallet-model destruction with pending RPC reads. Init/cleanup excluded. Action matrix and submit confirmation additionally pass on final `qt-v3`; no full Qt pass is claimed. |
| Descriptor functionals | Five scenarios exit 0 on `daemon-v1`: AUTO authorization, reorg, provider paid/public/recovery, backup, and RPC client protection. Final `daemon-v2` additionally passes AUTO including ordinary DD sends with Paymaster disabled (11.843 seconds), and the full provider scenario (85.000 seconds). All runs use isolated Regtest directories and random seed 1015350342; final port seeds are 3251/3261. The CLI is reused. |
| Local controls | Dependency/cycle checks, all six architecture fixtures, Python AST/registration, JSON parsing and whitespace checks pass; 28 current-current functional variants are registered. |

Initial extraction/test compiles and a Qt link exposed missing declarations,
ambiguous test types and a duplicate older object. The first new AUTO functional
attempt shared an HTTP connection between concurrent test calls; independent
connections corrected the fixture. Failed attempts remain separate from the
successful runs. No product validation, limits, checkpoint or test assertions
were weakened.

No persisted maintenance format changed. An unresolved signed transaction has
no proven irreversible-conflict release rule in the existing ledger, so its fee
exposure is retained conservatively; records already pruned by an earlier binary
cannot be reconstructed by this correction. Retained inbound results remain
process-local and subject to the existing count/byte quotas.

Clean supported product builds, Linux/C++17, Tidy17 with the repository plugin,
ASan/UBSan/TSan and full Qt/functional/compatibility matrices remain operator/CI
work. Flake8 6.1.0 is unavailable locally; AST checks are not a lint pass.
The build/test runbook provides the commands, prerequisites and acceptance
criteria for those separate release checks.

## Provider locks, client result durability and native disposition

Working-tree corrections based on `853cbc6719659000894d230ae97212e3efbc5ac7`,
checked on 2026-11-06. New provider capacity and signatures respect ordinary
coin locks; exact durable replay retains its existing authority. Normal and
alternative-recovery client results use the existing scoped lease/ACK model.
Provider processing uses one native disposition with compatible RPC encoding.
The implementation guide owns these behaviors and the test runbook owns coverage.

Twenty-one selected production/test translation units were freshly compiled
with MSVC 19.43.34810 / toolset 14.43.34808, `/W3 /WX /Od /MT /std:c++20`;
Qt uses static 5.15.10. The rebuilt consumers include the background service
that destroys returned processing results. Native, daemon and Qt test links
reuse remaining objects/libraries and unchanged Qt MOC. These are isolated
links, not clean product builds or complete transitive-header provenance.
Commands, hashes, full logs and retained failed attempts are under
`.ai/impl-ap12-853/`; the final product/test links use the `-v4` suffix.

| Executed check | Bounded result |
|---|---|
| Focused native selection | Exit 0: 85 cases / 2,921 assertions. Covers both provider slot-reservation APIs, denied signing without PSBT mutation, independent lock reasons, both result lease types, begin/write/commit and final-preflight failures, typed disposition/JSON and the existing Wire/application cases. |
| Related authority and ordinary locks | Exit 0: 11 additional cases / 1,347 assertions. Existing cleanup/unlock ownership, exact-final conflict protection, checkpoint concurrency and provider/recovery/firewall/automatic dispatch regressions. The two selections are disjoint: 96 cases / 4,268 assertions. |
| Qt | Four selected functions exit 0: manual activity serialization, submit confirmation, ordinary wallet transactions and DGB balance breakdown. Init/cleanup are excluded; this is not a full Qt pass. |
| New provider-lock functional | Final `locks-v8` exits 0, about 44 seconds: transient/persistent DGB and carrier locks, reload, first-signature veto, retained reservations and a completed payment. Existing DD-token RPC unlock protection remains enforced; finite funding supplies the next whole slot without reducing its fee ceiling. |
| Existing provider functional | Exit 0, 100.344 seconds: paid/public transfers, lost results, alternate recovery and safety depth, finance/backup, encrypted restart and unload. |
| Existing payment/reorg functional | Exit 0, 17.860 seconds: authenticated rejection, real payment, reorganization and reconfirmation without duplicate accounting. All three successful functional runs use seed 1015350342, separate Regtest directories and port seeds 2901/2861/2881 respectively. The current daemon is freshly linked; the existing CLI is reused. |
| Local controls | Dependency/cycle checks and all six architecture fixtures pass. Python AST/registration and whitespace checks pass; 27 current-current functional variants are registered. |

Initial diagnostic links included duplicate older objects, followed by a stale
background-service consumer of the changed result layout. Those link/test
failures are retained separately from the successful links with that consumer
freshly compiled. Earlier new-functional attempts exercised invalid fixture
assumptions about DD unlock, delivery, minimum change and subsequent slot
funding; no product rule, limit or existing assertion was relaxed.

Flake8 6.1.0 was unavailable; an isolated workspace installation could not
obtain it from the available package source. AST checks are not a lint pass.
Clean supported product builds, Linux/C++17, Tidy17 with the repository plugin,
sanitizers and complete Qt/functional/compatibility/release matrices remain
operator/CI work.

## Request classification, dispatch and native observations

Working-tree changes based on `095e41ca147929dd39972047e20e2dbd3211ad36`,
checked on 2026-11-06. Expired/divergent normal submits are consumed only after
validating local authority; exact retained finals remain recoverable after expiry.
Request types now rotate with independent provider/type/group/session cursors.
Finance and automation use native typed snapshots; Qt and CLI share policy defaults.
The implementation guide owns behavior, and the test runbook owns selectors.

Eighteen selected native/Qt translation units, including affected tests and
Manager allocation/destruction consumers, were freshly compiled with MSVC
14.43.34808 (compiler 19.43.34810), `/W3 /WX /Od /MT /std:c++20` and the existing
project flags. Qt uses static 5.15.10. Native, Qt and daemon executables are
isolated links with reused remaining objects/libraries, not clean product builds.
Commands, source/header/object/binary hashes and full logs are retained under
`.ai/implement-u1-u3-095e-20261106/`; `source-freeze.json` identifies the final
sources and active link inputs. Earlier setup/link attempts remain separate.
Queue traffic uses rejected or replayed markers: it proves dispatch and retained
authority, not fresh successful payments under hostile load. The narrow
deadline-crossing exception/reload path was additionally reviewed statically.

| Executed check | Bounded result |
|---|---|
| Request/submit and wire selection | Exit 0: 72 cases / 3,098 assertions. Includes sustained admitted competing requests in both directions, capacity deferral, provider/type/group/session fairness, three submit expiry variants, valid divergent signatures, protected private/final provider evidence and local-corruption retention. Rejected traffic leaves DB/checkpoint unchanged. |
| Native observations and encoding | Exit 0: 14 cases / 267 assertions. Complete application suite plus selected activity, automation, finance and error cases; one observation time, known zero/false, optional valuation, capital categories and existing RPC failure semantics. |
| Related authority and checkpoints | Exit 0: nine cases / 890 assertions. Exact durable final/budget recovery, pre-signature firewall, retained read failure, stale-work handling and both checkpoint concurrency cases. These three native selections are disjoint: 95 cases / 4,255 assertions. |
| Qt | Five selections exit 0: configuration workflow, two guided-default rows, save/refresh and finance/backup. Init/cleanup are not feature cases; no full-Qt pass is claimed. |
| Descriptor provider functional | Exit 0, 85.234 seconds; random seed 1015350342, port seed 2461. Fresh isolated daemon and reused CLI; real paid/public transfers, finance/backup, exact gross/sweep, lost replies, alternative recovery, encrypted restart/unload and log privacy. Separate Regtest directories only. |
| Local controls | Working-tree dependency/cycle check, six architecture fixtures and whitespace checks pass. Existing policy, workflow and operator changes were preserved. |

Full supported product builds, Linux/C++17, Tidy17 with the repository plugin,
sanitizers, full functional/Qt/compatibility matrices and operational release
gates remain operator/CI work. These isolated results do not certify them.

## Fair dispatch of provider submissions

Working-tree correction based on `095e41ca147929dd39972047e20e2dbd3211ad36`,
checked on 2026-11-05. Submit processing alternates normal and recovery work
per provider, with one message per eligible call. Independent type, netgroup and
session cursors advance on lease acquisition; another type's acknowledgement
cannot reset their rotation. Local uncertainty still retains the message and
financial authority. Canonical behavior and repeatable commands
are in the implementation guide and test runbook.

The manager, processing, wire/security/identity test units and the reached
`Manager` allocation/destruction consumers (`init.cpp`, `node/context.cpp`)
were freshly compiled with MSVC 14.43.34808, `/Od /MT /WX /std:c++20`.
The private manager layout changed. This is an isolated link with reused
product/harness objects, not a clean build; old test suites were excluded.
Commands, source/object hashes, seeds and full logs are retained under
`.ai/fix-ab-095e41ca-20261105/build-a-final/`; earlier diagnostic logs remain in
the adjacent `build-a/` directory. Final test binary SHA256:
`10cfa3a3b22c825d79e1d2eab0d101dac2f46f2f499a790e1a6b2fa98532e749`.

| Executed check | Bounded result |
|---|---|
| New regressions | Exit 0: six cases / 428 assertions. Twelve eligible service cycles per traffic direction, opposite-type progress within two calls, unchanged database/checkpoint, retained same-session messages, provider isolation, fallback and lease exception/prune behavior. Interleaved acknowledgements and retained messages preserve group/session rotation in both classes. Stale markers prove dispatch/classification, not completed payments under flood. |
| Existing queue and authority coverage | Exit 0: 71 cases / 2,649 assertions, including the complete wire suite and selected readiness, normal-submit and recovery authority/replay tests. Four new queue cases overlap; the two successful selections cover 73 distinct cases / 2,933 assertions. |
| Negative control | The same new traffic tests linked with the pre-fix processing object fail on the normal-submit starvation assertion (Exit 201); the symmetric recovery case passes. |
| Independent-cursor negative control | An isolated manager variant using the shared acknowledgement cursors fails eight group/session-order assertions (Exit 201); the final independent dispatch cursors pass all four ACK/retained and normal/recovery contexts. |
| Local controls | Working-tree dependency/cycle checks, five boundary fixtures and whitespace checks pass. No policy or workflow was changed. |

Two initial test-fixture failures are retained: repeated markers overloaded a
netgroup queue, and sharing one fixture across both traffic directions crossed
its saved quote's expiry. Final tests use one marker and one competing arrival
per cycle in independent fixtures. No queue limit, expiry rule or assertion of
unchanged authority was relaxed. Clean supported builds of all consumers,
Linux/C++17, repository Tidy17/plugin, sanitizers and full functional/Qt matrices
remain operator/CI checks; earlier binaries do not certify this layout change.

### Local architecture checks

On 2026-11-06 the operator removed the Paymaster-specific hosting and separate
owner-approval requirements. The dedicated GitHub boundary workflow and the
checker veto on control-file changes are removed. Dependency, cycle, scope and
shared-Core record checks remain, as does the existing Paymaster Security CI.
Earlier hosting proposals and observations remain historical artifacts under
`.ai/fix-b-target-20261106/`; they were never installed. Historical boundary
rejections below describe the checks at those revisions, not current hosting
requirements. No remote settings, refs or PRs were changed.

Verification on 2026-11-06: all six architecture fixtures and the working-tree
dependency/cycle check pass. The new fixture permits check-file changes while
retaining consensus-scope and shared-Core record failures; candidate policy
replacement and cycle-helper execution protections still pass. JSON parsing
and whitespace checks pass. Flake8 is unavailable locally. The five A source
and test files are byte-identical to the tested snapshot; no native rebuild
or runtime test was needed for this tooling/documentation change.

## Request identity, carrier arithmetic and finance paging

Source candidate `bfad9a900a83a50491f278e8b55e65186bc8983a`,
based on `e019ec279b850974e1ea42c5b712b91faa4a23ff`, checked on 2026-11-05. Both accepted client-resume paths now compare the complete
canonical order; preferred-offer checks reuse the first-attempt binding.
Historical Restricted bindings remain readable without renewing authority.
Client reconstruction reuses `ComputeCarrierSuccessor`, moved unchanged into
the existing amount module. Finance paging uses revision-bound event identities
and stores each page's events once. Tidy CI retains complete diagnostics and
the actual pipeline status, including a silent successful analysis.

Changed native translation units and the affected provider Qt units, test unit
and regenerated MOC were freshly compiled with MSVC 14.43.34808,
`/Od /MT /WX /std:c++20`; Qt uses static 5.15.10. These are isolated links with
reused objects/libraries, not clean product builds. Commands, source/header/object
hashes, seeds, failures and full outputs are retained under
`.ai/implement-e019-c/`, `.ai/verify-a-e019-20261105-r1/`,
`.ai/implement-e019-qt/` and `.ai/implement-e019-ci/`.

| Executed check | Bounded result |
|---|---|
| Native amount/PSBT and historical request binding | Exit 0: 14 cases / 540 assertions, including carrier minimum/maximum and 0/1/99/100-cent fee outputs, exact transaction reconstruction and historical Restricted request identity. |
| Checkpoint concurrency | Both unchanged cases pass twice with the same isolated binary and short workspace TEMP/TMP: 2 cases / 15 assertions per run; repeats are not additional coverage. Long artifact paths still abort. The captured stack resolves through filesystem `remove_all` in `BasicTestingSetup` teardown and `terminate`/`abort`; no checkpoint assertion or product guard was relaxed. Earlier write-assertion/abort results and incomplete object provenance remain separate from these successful runs. |
| Qt | Four selected functions exit 0: new finance paging, existing finance/backup workflow, explicit queue effects/start failure and ordinary DGB balance. Existing stylesheet and QSignalSpy warnings remain; no full-Qt pass is claimed. |
| Descriptor provider functional | Exit 0, 94.875 seconds; seed 1015350342, port seed 1861. Covers both new RPC retry regressions, unchanged resume after tighter policy, user-paid/public payments, exact gross/sweep, recovery and encrypted restart/unload. Freshly linked daemon and existing CLI. |
| Tidy pipeline regression | Seven isolated Bash executions verify old/new silent/informational success, stderr-only analyzer failure and failed log output. Bash syntax passes. This uses a simulated analyzer and is not a Tidy/plugin analysis. |

The first full provider run reached the new regressions and then exposed an
existing comparison of a quote replay with the status endpoint's additional
`observed_at`. The correction checks that observation separately as a positive
integer and preserves exact comparison of every other field; the full rerun
passes. Initial diagnostic links with missing test-main inputs, failed long-path
runs and a debugger-only reader pass are retained and are not passing evidence.

The shared CI-line correction was explicitly requested as review work package F;
its ordinary callers and regressions are recorded in the change record. Boundary
policy and hosting settings were unchanged at that checkpoint. Complete clean
builds, Linux/C++17, supported Tidy17/plugin, sanitizers and full
matrices remain operator/CI work. The short-path checkpoint runs do not certify
arbitrary Windows path lengths or a complete transitive build provenance chain.

## Manifest amounts, authoritative status and inbound classification

Working-tree corrections based on `3c850446b45f865de5bab494ab03fd3283203085`,
checked on 2026-11-05. This review's R3–R5 cover bounded manifest arithmetic,
latest-attempt status bindings and exact local-authority error regressions.
Three changed production and three changed test translation units, plus the
wallet PSBT test unit, were freshly compiled with MSVC 14.43.34808,
`/Od /MT /WX /std:c++20`. Native and daemon links reuse other objects and
libraries; they are not clean product builds. Commands, source/object/binary
hashes, seeds, patches and full logs are in `.ai/fix-r3-r5-3c850446/`;
`source-freeze.json` binds the final sources and executables.

| Executed check | Bounded result |
|---|---|
| Native final selection | `native-fixed-tests` exits 0: 20 distinct cases / 1,752 assertions. Covers serialized/rehashed extreme amounts and valid boundaries, 14 current-attempt corruption scenarios through capture/get/list/resolve, unquoted fallback, reorg observations, retained authority and exact error classification. The earlier six-case selection overlaps and is not additional coverage. |
| Status regression before correction | The two new snapshot cases fail against the previous implementation: 93 of 340 assertions fail. Both pass in the final selection; old provider/fee terms are no longer substituted. |
| Functional provider fallback | Descriptor `wallet_paymaster_failover.py` exits 0, 84.70 seconds: contended capacity, a different provider with the same reserved DD inputs, signed fallback exclusion and unsigned closure. |
| Functional payment/reorg | Descriptor `wallet_paymaster_reorg.py` exits 0, 39.05 seconds: authenticated conflict/recovery rejection, real payment, reorg and reconfirmation without duplicate accounting. Both runs use seed `1015350342`, port seed `732`, isolated datadirs, freshly linked daemon and reused CLI. |
| Local controls | Five boundary fixtures, working-tree dependency/cycle checks and whitespace checks pass. Separate policy/product patches and an unapproved policy-first fixture are retained with `boundary-package.json`; that fixture's product comparison passes. The task-baseline comparison still rejects the control/scope changes; the official baseline still requires bootstrap. |

The initial new amount test needed named default-initialized objects for MSVC;
the initial daemon link duplicated the old client-view object. Both were fixed
before the successful runs, with their failed logs retained. Neither failure
is counted as a product runtime regression.

The former package D hosting requirement was removed on 2026-11-06. Its local
proposal was not installed, and no hosting settings were changed. Clean supported builds,
Linux/C++17, repository Tidy, sanitizers (including UBSan for the new amount
vectors), Qt and complete matrices remain operator/CI checks in the
[runbook](digidollar-paymaster-testing.md).

## Recovery, coin-lock ownership and Qt continuation corrections

Working-tree candidate based on `988bb8e621c42b33d1da337fc2aead4c19608026`,
checked on 2026-11-05. All changed C++ translation units and the reached
wallet/model allocators were freshly compiled with MSVC 14.43.34808,
`/Od /MT /WX /std:c++20`; Qt uses static 5.15.10, Windows/Fusion.
These are selected compiles and isolated Native/Qt/daemon links with reused
dependency libraries, not clean product builds. Commands, response files,
source/object/executable hashes, seeds, exit codes and full logs are retained
under `.ai/implement-988bb8e6/`; `source-freeze.json` binds the final sources.

| Executed check | Bounded result |
|---|---|
| Native ownership and recovery selection | Final `native-test-attempt3` exits 0: 18 distinct cases / 1,511 assertions. Covers active manual locks through cleanup/expiry/reload, corrupt and legacy lock records, write/commit failure, capacity absence versus local damage and automatic quote/stale-submit progress. The five-case intermediate repeat overlaps and is not additional coverage. |
| Qt selections | Final reruns exit 0 for ten functions, including 13 setup data rows, exact CSV amount round-trips, blocked node RPC during model destruction, generic queue shutdown, ordinary wallet send/balance and DD Overview error/polling cases. Init/cleanup are excluded from the function count. Existing stylesheet-property warnings remain; this is not a warning-free/full-suite claim. |
| Functional backup adoption | The registered descriptor test exits 0, 31.22 seconds: independent null-plan and changed-plan rejection, adoption, old-copy fencing, restart and a new signed payment. |
| Functional payment/reorg | Descriptor test exits 0, 28.39 seconds: authenticated conflict/recovery rejection, real payment, reorg and reconfirmation without duplicate accounting. Both functional runs use seed `2559293670`, port seed `512`, separate datadirs, freshly linked daemon and reused CLI. |
| Local controls/source checks | Five boundary fixtures and working-tree dependency/cycle scan pass. Changed Python AST, JSON and whitespace checks pass. Existing forbidden/control paths and checker are unchanged; exact ClientModel/Overview/shared-project scope and stronger CI triggers are separately reviewable in `control-review.patch`. |

The first isolated native link retained one duplicate old object; the first
daemon link used the wrong working directory. Those harness errors were corrected
without product changes. Initial Qt setup/ordinary-wallet runs then failed with
heap corruption: the link map selected old `make_shared<CWallet>` and
`make_unique<ClientModel>` COMDAT allocators alongside the new constructors.
Recompiling the remaining explicit Qt allocation consumers and verifying the
new map removed that mismatch; all ten final Qt selections passed. Initial
failures and final reruns remain separate in the artifacts.

Final Native/Qt/daemon SHA256 respectively:
`eecee7ff6e04abcd54375eafefbd02a2138217e8a408bc719ea4943039cd084b`,
`2e8a16d4a8d6e16ccd78328239767367d7d3ee01fe0712ef8ab43dd3d801ef7e`,
`198baa40b5068cb72a2e659de80bdadb6e53cd9180bb44f31ca479574c7090b3`.
CLI SHA256: `ca6fff5d506907f59a30a78a8d7e5ee0b6da44bf8518fb0a6c2d2e6c2c1954ae`.

At this checkpoint the pinned pre-correction policy rejected control changes
and the new shared paths; the proposed product-only scope check passed.
No hosting settings were changed. Clean complete builds (including Qt without
wallet), Linux/C++17, Tidy with the repository plugin, sanitizers, full suites and
release/operator gates remain required under the existing test runbook.

## R1–R6 follow-up verification

Working-tree corrections based on `204d67b03be00a77eccb3875fe414a48291555b6`,
checked on 2026-11-05. Changed translation units, tests and affected Qt layout
consumers were freshly compiled with MSVC 14.43.34808, `/Od /MT /WX /std:c++20`,
and static Qt 5.15.10. These are isolated links with reused dependencies, not
clean product builds. Commands, patches, compile/object hashes, seeds, exit
codes, full logs and `source-freeze.json` are retained in
`.ai/fix-r1-r6-204d67b0/`. This follow-up uses its own R1–R6 numbering.

| Executed check | Bounded result |
|---|---|
| R1 C++17 header | Actual `paymaster/error.h` compiles with MinGW GCC 13.2, `-std=c++17 -Wall -Wextra -Werror`; runtime prefix/fallback/length checks pass. This is not a Linux or full C++17 build. |
| R3/R5 native regressions | Baseline exposes the rollback display error and unauthenticated saved quote. Corrected final selection passes 4 cases / 89 assertions (`r35-final`); related budget, signing, persistence, replay and lock selection passes 23 / 1,348 (`native-related-final`). Total: 27 distinct cases / 1,437 assertions. |
| R2/R4 Qt | Seven selected functions pass, including nine setup rows, seven live-recovery rows, signed/unsigned expiry, ordinary DD send and shared wallet queue checks. A delayed privacy reply crashes the old setup implementation (`r4-red`, exit `0xc0000374`); all four new privacy rows pass with the guard. Qt schema consumers were tested before the separate RPC help correction below. |
| R6 functional registration | Runner lists 26 current-current variants. All seven newly registered descriptor variants pass individually. The no-change-client and reserved-capacity runs first expose a `resolvepaymastersession` result-schema error; its correction retains strict checking and both pass on retry. The other five runs also pass. Full 26-variant execution remains operator work. |
| Local boundaries and source checks | Working-tree dependencies/cycles and changed-path/shared-record checks against the existing HEAD policy pass; Python AST and `git diff --check` pass. These checks do not establish remote enforcement or final owner acceptance. |

Final native executable `native-final.exe` SHA256:
`30210de7276e496e75a2a8c8f89ea5d443fcf98440d54c1b209206c1cdcea658`.
Qt executable `qt.exe` SHA256:
`4d99fa17c93e688e1e4175d5c9490d0f50a025e6160e83b1f48575fe638f28d5`.
Functional runs use `daemon-before-schema.exe` SHA256
`5f30dd384563dacf19cb55c85e7b0595bd5220736b17a7d850ac113296f5ca14`
for announcement refresh, legacy policy and offer models; corrected `daemon.exe`
SHA256 `4e25a5b7a84647a4f39d198129d023bff7f1e8ce63263a281cd77ac76293925b`
for the other four. CLI reuse and exact per-run selections are recorded with
the artifacts. No clean build, full suite, sanitizer or release-gate pass is
inferred from these checks; the operator commands remain in the
[runbook](digidollar-paymaster-testing.md).

## Earlier review corrections and extraction candidate

Starting source: `98c717f738e765cf1dbbbb94ed95afbda118b382`.
Code candidate checked on 2026-11-04: `f28c2dcdc041d27d9799cf9da93da5b346b685e0`.
The task and official comparison bases
remain those in the [change record](design/paymaster-change-record.json).

Changed translation units and tests were freshly compiled with MSVC 14.43.34808 x64
and `/Od /MT /WX /std:c++20`; Qt checks used static Qt 5.15.10, Windows/Fusion.
Executables were isolated links with reused objects/libraries, not clean product
builds. Exact selections, source diffs, response files, executable provenance,
exit codes and full logs are retained under `.ai/paymaster-implementation-98c717f7/`.
Failed baseline/fixture attempts remain separate from successful runs.
`source-freeze-f28c2dcdc0.json` verifies the committed source against those
compile/run manifests. Unchanged dependency libraries and header dependencies
are not a complete clean-build provenance chain. Native/Qt runs from O2 onward
record the fixed `RANDOM_CTX_SEED` in their JSON; earlier runs did not explicitly
set that seed. Initial R1 runs hit a Windows long-temporary-path failure also
reproduced with the unchanged executable; subsequent runs use a short test path.

| Executed check | Bounded result |
|---|---|
| Quote conflict/retry correction (R1) and saved-quote signature follow-up | Regressions fail against the preceding product and pass after correction: initial selection 7 cases / 550 assertions; signature/identity follow-up 9 / 700. Logs: `r1-baseline-test-shorttmp`, `r1-fixed-test`, `r1-followup-{baseline,fixed}-test`. These selections overlap. |
| Persisted client reads and action gating (R2) | Baseline list/resolve/view regressions fail; corrected getter, view, confirmed-payment and application selection passes 14 cases / 597 assertions (`r2-{baseline,fixed}-test`). Corrupt/future state is not treated as absence. |
| Manual Activity presentation (R3) | The existing Qt function fails on deferred-result presentation before correction, then passes with native result serialization and malformed/conflicting-result coverage (`r3-{baseline,fixed}-test`). One feature function; init/cleanup are not extra cases. |
| Related status fallbacks (O2) | Baseline regressions fail; corrected recovery/result selection passes 13 cases / 1,033 assertions (`o2-{baseline,fixed}-test`). The retained-message and stored-replay fixtures do not establish a complete fresh recovery payment. |
| Client/provider phase extraction (O3/O4) | Client capacity and quote stages each pass the same 8 cases / 414 assertions. Provider capacity, recovery and quote stages each pass the same 15 / 994. Logs: `o3-{capacity,quote}-test`, `o4-{capacity,recovery,quote}-test`; repeated stages are not additional coverage. |
| Setup extraction and separate guided-task correction (O5) | Five setup-boundary rows, theme and layout selections pass, including wallet removal during an open wizard/pending preview. A separate local-error baseline still fails after correcting RPC mocks to the real structured format; its fix passes 34 capital-task rows, 13 restoration rows and one structured-error function. Logs: `o5-switch-test`, `o5-layout`, `o5-paymasterGuidedSetupRendersConsistentTheme`, `qt-local-error-*`. Counts exclude init/cleanup. |
| Authenticated conflict/payment/reorg scenario (O1) | `pm98-reorg1` exits zero: conflicting request ID over authenticated v2 transport is consumed, an unrelated recovery is rejected, and the genuine payment is included, reorganized and reconfirmed without duplicate accounting. Binary/source details are in `pm98-reorg1-binaries.json` and its run record. This is the recorded intermediate candidate, not the final-source rerun. |

The closing checks below use source matching the code candidate. Their native
selections are disjoint; all selected tests passed. Unselected native tests are
reported as skipped by Boost and are not counted as coverage.

| Final-source verification | Actual result |
|---|---|
| Native selections | Exit 0: 15 cases / 994 assertions in `o4-quote-test`, plus 29 / 2,114 in `final-native-complement`; 44 distinct cases / 3,108 assertions, including three ordinary DB cursor cases. Executable: `o4-quote.exe`. |
| Qt selections | Exit 0: six sender functions (`o6-*`, including 17 live-send and 19 cancellation rows), manual Activity, six manual-submit confirmation rows, and four ordinary `WalletTests` functions (`final-*`). Twelve functions, with init/cleanup excluded. Executable: `o6-send.exe`. Existing layout/property and QSignalSpy warnings remain in logs; these are not warning-free/full-suite claims. |
| Functional selection | Exit 0 in `pm98-reorg2.log`, 24.36 seconds: real v2 conflict, subsequent payment, reorg and reconfirmation; RPC contract checking retained. Seed `2563184631`, port seed `267`. `final-daemon.exe` was freshly relinked; the existing CLI was reused. Exact test/CLI hashes are in `pm98-reorg2-binaries.json`. |
| Boundary checks and comparison bases | Code candidate passes the pinned `98c717f7` checker/policy and five extracted fixtures. The `33c680cba1` comparison rejects historical control changes and shared files outside its older policy; the official `d7265fb05e26e1e799876bcd526225e4826c3ba8` base rejects missing policy/bootstrap. Evidence and separate diffs are under `.ai/ap5/runs/f28c2dcdc041d27d9799cf9da93da5b346b685e0/`. Neither historical rejection is converted into a pass. |

Executable SHA256 fingerprints:

| Executable | SHA256 |
|---|---|
| `o4-quote.exe` | `6b0c2103eeaa8b313281ae775739121cafddd1a925660c090bb4e9c53db2c091` |
| `o6-send.exe` | `6c8225c9efecfff6595bcc6b664b95c7312fb6c969d5d03b88e63d96c50d71a7` |
| `final-daemon.exe` | `9c6258233423397e884fdf2368492e62f75757eda37c285048ccbf3078d067b8` |

Do not sum overlapping selections or infer a protocol/compatibility pass from
source searches. Protocol-definition/diff review and executed behavioral tests
are separate evidence. The earlier potential `_Rep` leak diagnostic remains
unresolved; successful targeted assertions neither confirm nor disprove it.
Functional Python syntax and whitespace checks passed. `flake8` was unavailable;
the workspace-only installation attempt failed, so no flake8 pass is claimed.

On 2026-11-05 the operator approved and the implementation applied the bounded
MSVC project change: Release-only `/bigobj` for `paymasterwidgettests.cpp`, with
its integration need recorded in the shared change record. Two runs of the
existing Qt project synchronizer preserve the option. MSBuild item evaluation
confirms that Release retains its inherited flags and Debug remains unchanged.
The prior isolated compile and Qt executable above already use `/bigobj`; their
source/object hashes still match. Those compiles/tests were not rerun for the
project metadata change. Evidence is retained in
`.ai/paymaster-implementation-98c717f7/r3-msvc-bigobj/applied-verification.json`.

The project path was outside that checkpoint's pinned boundary policy; scope
approval did not turn its strict check into a pass. The policy was not expanded.
Complete Windows/Linux product builds,
Linux/C++17, Tidy with the repository
plugin, sanitizer runs and the release gates below remain operator work using
the [runbook](digidollar-paymaster-testing.md). These isolated results do not
establish those checks.

The checker, policy, fixture tests and former workflow at `98c717f7`, their
extracted hashes and comparison runner are retained under `.ai/ap5/` as
historical local evidence. The full-fork MSVC
`int128` adaptation remains subject to its separate platform/equivalence proof.

## Required candidate gates

| Gate | Required evidence |
|---|---|
| Product build | Clean app, daemon, CLI and test builds for supported platforms; exact revision/toolchain. |
| Core invariants | Exact transaction/fee/signature, budgets, replay, recovery and persistence regression groups. |
| Lifecycle | Encryption/unlock, startup/history readiness, shutdown/restart, backup/adoption/rollback and retirement. |
| Liquidity | Finite versus recurring approvals, whole-slot refill for both assets, live capacity below target and fee caps. |
| End-to-end clients | Exact authorization, cancellation, response loss, failover, reorg and confirmed-recipient-only success. |
| GUI and CLI | Shared settings/approval semantics, async readiness, consistent balances/offers and headless maintenance. |
| Compatibility | Ordinary DGB/DD paths, feature disabled, historical wallet/node interoperability. |
| Adversarial operation | Sanitizers/fuzz/load, real Tor reachability and prolonged operation, hostile relay. |
| Durability and review | Physical storage failure coverage and independent review of financial/lifecycle boundaries. |

Use the [runbook](digidollar-paymaster-testing.md) for commands and the
[threat model](paymaster-threat-model.md) for residual risks. Record a pass only
against the candidate and built executable. Remaining tests are not waived by
successful manual regtest payments.

## Historical checkpoints

These results apply only to their recorded source/build. They do not establish
passes for the current candidate and must not be summed: selections overlap.
Local native checks used MSVC 14.43/static Qt 5.15.10 and isolated links against
existing dependencies unless stated otherwise. Missing binary hashes or logs
are not reconstructed from the test counts.

| Source / date | Retained evidence | Scope or limitation |
|---|---|---|
| `7f30be59f7` through `ef213fb746`, 2026-11-04 | Inbound/pending-balance corrections: red/green regressions, 121 distinct selected native cases / 3,936 assertions, focused payment/reorg and local boundary checks passed. | Isolated MSVC links; overlapping F2 reruns excluded. Detailed provenance and limitations remain in this file at `98c717f7`. |
| Corrections based on `b3b26ccbda5a`, 2026-11-04 | Wallet safety: 18 cases / 449 assertions and a separate 72 / 4,012 selection passed, covering lock ownership, readiness, reload and selected ordinary-wallet paths. | Recorded working-tree corrections and isolated links; no complete store suite or clean product build. Details remain at `98c717f7`. |
| `6ad3fa7242` through `75d9351e46`, 2026-11-04 | Optimization: 21 native cases / 8,673 assertions, seven ordinary DB cases / 935 assertions, 43 selected Qt functions and local boundary checks passed. | Finance reruns overlap; operation-count tests are not throughput evidence. Isolated links and no remote control approval; details remain at `98c717f7`. |
| `33c680cba1`, 2026-11-04 | Prior optimization: targeted identity, selection/index, finance and security checks plus selected Qt workflows passed; four boundary fixtures passed. | Earlier source and isolated links; detailed evidence remains in that revision of this file. |
| `1789c803be`, 2026-09-27 | 284 selected unit cases / 10,767 assertions; nine functional tests. | Older RC2 branch. |
| `e5cfa2da3e`, 2026-10-06 | Twelve translation units; 26 selected Core/Qt cases. | No normal product link or functional run. |
| Retention and recovery corrections, 2026-10-10 | Related selections: 48 cases / 10,012 assertions and 28 / 1,542. | Overlapping focused tests; many assertions come from quota fixtures. |
| `01aaf13df9`, `e7c2c8c4bd`, 2026-10-15 | Selected compiles, busy-wallet and sync-presentation Qt checks. | No full GUI or release acceptance. |
| `685a6b4f2c`, `b66320e4cf`, 2026-10-16 | Funding-diagnostics functional variant; selected Qt checks; setup suite 29 cases / 605 assertions. | Isolated links. The then-failing full pool release was corrected by the next row. |
| `c7750a18a1`, `0ab4289107`, 2026-10-17 | Maintenance regression reproduced before the fix; 10 wallet cases / 455 assertions; full descriptor pool-setup, lifecycle and provider scenarios passed. | Isolated unit/daemon links and existing CLI. |
| `e4195962a0`, 2026-10-17 | Corrected checkpoint fixtures and commit-failure quarantine: 7 cases / 992 assertions. | Test-only change; checkpoint protection retained. |
| `e17c96415d`, `fe6536afc3`, 2026-10-17 | Checkpoint/read replacement and guarded functional retries: 3 wallet cases / 270 assertions; four formerly failing functional variants passed. | Windows handle failure reproduced separately; concurrent regression is scheduling-dependent. |
| `31d36a6b7a`, 2026-10-17 | Operator reported all 19 current-current functional variants passing, 709 seconds wall time, after provider-guard fixture corrections. | Normal Release daemon/CLI; summary omitted executable hashes, complete logs and seeds. Historical interoperability was excluded. |
| `6cea92c461`, 2026-10-18 | Three corrected Paymaster Qt functions passed locally; operator subsequently reported the full `PaymasterWidgetTests` selection passed. | Isolated `gui-followup-20261018/qt.exe`; full-repeat totals and warning details were not supplied. |
| `628321a743`, 2026-10-18 | Overview teardown, reservation balance and privacy-mask checks passed. | Three focused functions; earlier teardown crash was not deterministically reproduced. |
| `c231521234`, `82c0b31493`, 2026-10-18 | Eleven focused DD widget functions, mint-record (9 Qt rows), transaction-record (12 rows) and two related Paymaster functions passed; operator reported complete `DigiDollarWidgetTests` passed. | Isolated `build_msvc/paymaster-refresh-check/dd-gui-regressions-20261018/qt.exe`, Windows/Fusion. Wave 19/table passed in the earlier operator selection; no combined all-Qt or clean product build. |

Intermittent Windows bind failures did not recur in the completed functional
matrix; their cause was not established. Qt row totals include fixture/data
rows. Historical warnings and separate component passes do not certify a
current full suite. Current commands belong in the [runbook](digidollar-paymaster-testing.md).

## Open boundaries

The [threat model](paymaster-threat-model.md#residual-operational-risks) records
rollback, cloned-provider, public-sponsorship and privacy limits. Physical
storage failures, real Tor/hostile load, independent review and current full
builds/suites still need the evidence required above. SQLite page-limit and
commit-denial checks do not establish power-loss or fsync coverage.

## Traceability

The full earlier evidence, seeds, failure reproductions and former acceptance-ID
mapping remain in Git:

```sh
git show 98c717f7:doc/digidollar-paymaster-release-gate.md
git show 5ddc3e268e:doc/digidollar-paymaster-release-gate.md
git show e7c2c8c4bd:doc/paymaster-threat-model.md
```

Record only bounded candidate evidence here. Keep complete logs/fingerprints
with build artifacts, commands in the runbook and resolved investigation details
in Git. Never promote a historical pass to a current-revision release pass.
