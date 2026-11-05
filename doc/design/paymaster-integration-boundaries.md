# Paymaster integration boundaries

Paymaster is adapted to DigiByte, not the reverse. This rule covers shared
wallet, validation, networking, RPC and Qt paths as well as Paymaster files.
Baseline and integration ancestry are recorded in
[the v9.26.7 integration note](../digidollar-paymaster-v9.26.7-integration.md).

## Lean documentation

This rule applies to all repository work. Document only what is needed to
understand, operate, test or safely change the current system. Update existing
documentation selectively; each fact has one authoritative location. Do not add
progress reports, work chronicles or repeated summaries. A new document requires
a concrete purpose not already covered. Remove obsolete intermediate states;
retain current interfaces, safety constraints, migration instructions and required
verification evidence. Keep handoffs to changes, checks and genuinely open items.

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

## Dependency and reuse contract

| Layer | Owns | Permitted dependencies |
|---|---|---|
| `paymaster/` | Protocol, amount arithmetic, selection and validation | Core value types/utilities; no wallet implementation, RPC handlers or Qt |
| `wallet/paymaster*` | Application operations, signing, persistence, reconciliation | Paymaster domain and existing wallet/database interfaces; no RPC handlers or Qt |
| `wallet/rpc/paymaster*`, `rpc/paymaster*` | Parameter decoding and result/error encoding | Domain/application services and existing RPC framework |
| `qt/paymaster*` | Presentation, user input and wallet-bound asynchronous work | Domain value types and existing frontend interfaces/RPC adapters; no wallet implementation |

The existing setup/CLI JSON views are adapter surfaces, not authorization
sources. Extract application logic from RPC handlers rather than calling those
handlers from a worker. Reuse wallet signing, database transactions, connection
budgets and asynchronous infrastructure. Do not copy these systems into a
Paymaster-specific replacement to reduce the apparent shared-Core diff.

The shared Qt worker bridge belongs to `qt/walletasync.*`; it must not depend
on Paymaster, wallet implementations or RPC handlers. Paymaster adapters own
error interpretation and explicit task policies. Shared wallet tests cover this
bridge alongside ordinary DGB/DD behavior. Approved integration paths include
these bridge files and `qt/test/wallettests.*`; they are not Paymaster-owned code.
`WalletModel` depends on this generic bridge, never on Paymaster views or types.

Each financial rule has one canonical implementation. Calling that implementation
again at a different trust boundary is deliberate validation, not redundant
business logic. Caches, transaction-to-session indexes and finance daily totals
are derived data: validate authoritative records before granting financial
authority, and rebuild invalid derived state without releasing reservations.

## Shared-Core change contract

`test/lint/paymaster-boundaries.json` is the executable path/dependency policy
for this pre-release optimization. Shared changes are restricted to the listed
integration points. Consensus, PoW, activation and general script validation
are outside this task. Additional changes need a concrete scope decision, not
an automatic expansion of the allowlist by the implementation agent.

For every changed shared file, `doc/design/paymaster-change-record.json` records
the purpose, existing API reused, ordinary callers affected and regression
checks. The record is review evidence, not proof that the tests passed. Record
actual results only in the [release evidence](../digidollar-paymaster-release-gate.md);
the test guide owns commands and coverage. Compare both against
the task and official baselines in the change record and the ancestry documented
in the v9.26.7 integration note; do not attribute all historical fork changes to this
optimization. There is no arbitrary line-count budget.

Keep behavior-preserving extraction separate from behavioral changes when
preparing commits. Update affected canonical guides when behavior or interfaces
change, and link to them from maps. Unknown persisted versions,
ambiguous outcomes and incomplete snapshots must remain fail-closed.

## Enforcement and trusted policy bootstrap

Run `python test/lint/lint-paymaster-boundaries.py` for dependency checks and
`python test/lint/test_paymaster_boundaries.py` for positive/negative fixtures.
The checker reuses the module mapping and graph helper in the existing
`contrib/devtools/circular-dependencies.py` linter. It rejects each newly added
edge that creates or extends a cycle against the target branch. Existing cycles
are not new exceptions; the helper itself is a protected control.
For a committed candidate, add `--base <trusted-base> --head <candidate>` to
check changed paths and shared-Core records using policy from the base commit.
The checker never executes candidate code. Computed, absolute and source-root-escaping
includes are rejected in guarded layers; changed files are read as complete source
for multiline include/cycle checks. Local default checks are advisory;
they do not claim trusted-base enforcement.

The separate `Paymaster Boundaries` workflow uses the target-branch checker and
policy, with candidate source treated only as data. Policy/checker/workflow
changes require a separate policy-only review. Initial installation requires
the repository owner to review and merge these controls before making the
`trusted-boundaries` status mandatory. A base without the controls fails with
an explicit bootstrap message; it never silently adopts candidate policy.

The repository owner must require the trusted status, protect policy changes,
and deny agent identities permission to bypass merge controls. These hosting
settings cannot be installed by editing `AGENTS.md`. Repository text and CI do
not form an operating-system sandbox or prove absence of semantic duplication.
Retain the operator's permission boundaries and review the shared-Core record.

### Owner acceptance evidence

Keep the control-only patch separate from product fixes. Include all changed
paths listed in the policy's `controls` section, including this document and
the cycle helper; do not remove them from that protected set. Review the exact
shared-path additions against their change records. A locally constructed
policy-first comparison can demonstrate the proposed split, but its commits
are fixtures, not an approved target branch or hosting evidence.

Before accepting the product PR, retain the following from the authenticated
hosting target:

1. Repository, PR number, target branch, actual base SHA and candidate SHA.
2. The separate control review and its owner-approved merge commit, with the
   installed checker, policy, fixtures, cycle helper and workflow hashes.
3. Effective branch protection and rulesets, including inherited rules: the
   `trusted-boundaries` status is mandatory, policy changes require owner
   review, and agent identities cannot bypass either requirement.
4. A successful boundary run for those exact base/candidate SHAs, using the
   target's checker. Retain the run URL and conclusion. Repeat after either
   SHA changes; a local advisory pass or historical run does not substitute.

Missing access, an absent policy, a rejected control diff or unknown rules
leaves acceptance open. Do not select a newer local base solely to turn a
rejected comparison green. Initial owner-reviewed bootstrap precedes enabling
the required status; it does not authorize product changes in the same review.
