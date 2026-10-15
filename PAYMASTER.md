# DigiDollar Paymaster: documentation guide

Documentation baseline: `integration/paymaster-v9.26.7`, source revision
`bab89065df`, reviewed on 2026-10-15. This identifies the source described here,
not a release certification. See the [release gate](doc/digidollar-paymaster-release-gate.md).

## Scope and integration rule

These guides primarily describe **Mainnet operation**. Unless a section says
otherwise, node/CLI examples select `-chain=main` and payments use real DGB/DD.
Start with the [Mainnet node configuration](doc/digidollar-paymaster.md#mainnet-provider-configuration)
and [operator setup](doc/digidollar-paymaster-operator.md#first-provider-setup).
[Testnet and Regtest](doc/digidollar-paymaster-test-networks.md) are supplementary
test environments, with separate wallets, funds, endpoints and configuration.
This documentation target does not change the release acceptance recorded above.

A Paymaster contributes DGB inputs to an ordinary DigiDollar transfer so that
the sender can pay without suitable DGB. Each party signs its own inputs after
checking the complete transaction. The provider sponsors the network fee or
receives an explicitly authorized DD service fee in that transaction.

There is one recipient per payment. Sponsored minting, redemption and batch
payments are not implemented. User-paid and public-sponsored services can be
enabled independently. New restricted sponsorship is disabled; legacy records
remain readable for recovery. Qt and interactive CLI setup are implemented;
automatic provider maintenance also runs headlessly in the daemon.

Paymaster adapts to DigiByte. Consensus, script validation, activation, wallet
history checks and ordinary DGB/DD transfers remain authoritative. Necessary
wallet, RPC, CLI, networking and GUI extensions must preserve these boundaries.

## Choose your task

Read the task guide first; the complete reference list below is not a mandatory
reading sequence for every user. UI labels are quoted in English to match the
source; translated builds may display localized labels.

| Reader and task | Start here | Continue when needed |
|---|---|---|
| Wallet user: send DD, understand fees or cancel preparation | [User guide](doc/digidollar-paymaster-user.md) | [Payment states and next actions](doc/digidollar-paymaster-user.md#payment-states-and-next-actions) |
| Provider operator: first GUI or headless setup | [Operator guide](doc/digidollar-paymaster-operator.md#first-provider-setup) | [Node configuration](doc/digidollar-paymaster.md#node-prerequisites), [pool funding](doc/digidollar-paymaster-pool-setup.md) |
| Operator: restart, pause, refill or investigate a failure | [Routine operation](doc/digidollar-paymaster-operator.md#routine-operation-and-restart) | [Diagnosis](doc/digidollar-paymaster-operator.md#diagnosis), [capital release](doc/paymaster-capital-release.md) |
| Application developer: integrate payment RPCs | [Client contract](doc/digidollar-paymaster-integration.md) | [Result handling](doc/digidollar-paymaster-integration.md#response-handling-for-applications), same-build RPC help |
| Core developer: change protocol, wallet or recovery | [Implementation](doc/digidollar-paymaster-implementation.md) | [Change ownership](doc/digidollar-paymaster-implementation.md#change-ownership-and-verification), boundaries and threat model |
| Tester: rehearse on a public or local test chain | [Testnet and Regtest](doc/digidollar-paymaster-test-networks.md) | [Test runbook](doc/digidollar-paymaster-testing.md) |
| Reviewer: establish what was actually verified | [Release gate](doc/digidollar-paymaster-release-gate.md) | [Test runbook](doc/digidollar-paymaster-testing.md), exact source/binary revision |
| Builder: compile or diagnose a Windows build | [Build entry points](doc/digidollar-paymaster-testing.md#build-entry-points) | [MSVC prerequisites](build_msvc/README.md) |
| AI assistant: locate the right sources and avoid unsupported assumptions | [Evidence and AI-assisted work](#evidence-and-ai-assisted-work) | The relevant task row above, then its source/test references |

## Evidence and AI-assisted work

The same evidence rules apply to people and AI tools:

1. Establish the checked-out commit, dirty changes and intended network. General
   operational examples target Mainnet; test fixtures do not establish Mainnet
   fees, timing, activation or availability. For a running node,
   establish the executable/build too; new source or documentation does not
   update an already running binary.
2. Read applicable repository/path instructions, this task index and the one
   relevant guide. Follow source/test references for the operation being changed;
   loading every historical report is neither necessary nor a substitute.
3. Treat same-build RPC help as the parameter/schema reference and current code
   and tests as implementation evidence. Current architecture/security documents
   state intended constraints. If those disagree, report and investigate the
   mismatch rather than silently weakening a constraint to match a bug.
4. Distinguish a documented requirement, a test that exists and a test actually
   executed on the candidate. Historical passes and selected compiles cannot be
   promoted to a full current release pass.
5. Preserve units, request identity and authorization boundaries. Status reads,
   preparation, exact approval, signing, maintenance and recovery are different
   operations. Do not infer permission for a mutation from an example or a log.
6. After a change, update the owning guide and affected links, then run the
   narrow relevant checks. Report what ran, what could not run and whether the
   executable was rebuilt/installed. Do not describe source-only changes as live.

Docs intentionally do not duplicate all RPC schemas. Examples identify their
shell or JSON-RPC context and placeholders; they are not default policies or
blanket operational approval. Unknown fields/status must not be interpreted as
zero balance, successful payment, released inputs or permission to retry under
a new ID. Use Core's returned capabilities and permitted actions.

Keep task instructions and worked examples here; keep historical evidence in
the release gate/Git. When code changes a unit, default, state, policy or public
interface, update its canonical reference and example together. Prefer a small
local explanation over requiring readers to reconstruct current behavior from
commit history.

## Reading order

| Document | Responsibility |
|---|---|
| [Implementation](doc/digidollar-paymaster-implementation.md) | Protocol, transaction construction, durable state and source ownership. |
| [Integration boundaries](doc/design/paymaster-integration-boundaries.md) | Compatibility constraints and review of shared DigiByte code. |
| [Threat model](doc/paymaster-threat-model.md) | Security invariants, corrected findings and residual risks. |
| [Client contract](doc/digidollar-paymaster-integration.md) | Prepare, authorize, resume, cancel and interpret payment status. |
| [Node and RPC reference](doc/digidollar-paymaster.md) | Configuration, units, prerequisites and method groups. |
| [Operator guide](doc/digidollar-paymaster-operator.md) | GUI/CLI setup, encrypted wallets, operation and diagnosis. |
| [User guide](doc/digidollar-paymaster-user.md) | Wallet payment steps, fee examples and waiting/cancellation states. |
| [Testnet and Regtest](doc/digidollar-paymaster-test-networks.md) | Separate public-test and local-test configurations and confirmation behavior. |
| [Pool setup and refill](doc/digidollar-paymaster-pool-setup.md) | Finite funding versus recurring whole-slot maintenance. |
| [Connection capacity](doc/digidollar-paymaster-connection-capacity.md) | Dedicated transport, admission and identity binding. |
| [Capital release](doc/paymaster-capital-release.md) | Retire protected liquidity or release an individual carrier. |
| [Testing](doc/digidollar-paymaster-testing.md) | Reproducible checks and test-to-invariant index. |
| [Release gate](doc/digidollar-paymaster-release-gate.md) | Revision-specific evidence and unfinished acceptance work. |
| [v9.26.7 integration](doc/digidollar-paymaster-v9.26.7-integration.md) | Upstream baseline and integration checkpoint. |
| [Release notes](doc/release-notes/paymaster-integration.md) | User-visible behavior and migration constraints. |

The [DigiDollar source map](REPO_MAP_DIGIDOLLAR.md#paymaster-network) locates the
implementation. Runtime `help <method>` from the selected build is the exact
RPC schema; documentation examples do not override it.

## Review sequence

1. Exact input/output manifests, fee arithmetic and signatures in the
   transaction builder, PSBT code and wallet finalization.
2. Persisted authorization, budgets, reservations and recovery together:
   a timeout must not release potentially signed spending authority.
3. Admission and authenticated channels before payment metadata disclosure.
4. Wallet load, backup restoration, refill, cancellation and shutdown.
5. RPC/CLI/Qt against the same Core state, followed by candidate-revision tests.

## Documentation history

Superseded implementation/hardening plans, network proposal, RC2 integration
report, capacity handoff, flow/UX/edge-case reviews, CLI wizard specification
and old security-audit report are removed from active guidance. Their essential
decisions are consolidated above; dated evidence is summarized in the release
gate. Full historical text remains accessible at the pre-consolidation revision:

```sh
git show e7c2c8c4bd:doc/digidollar-paymaster-testing.md
git show e7c2c8c4bd:DIGIDOLLAR_PAYMASTER_HARDENING_PLAN.md
```

Historical test counts, proposed interfaces and old branch names are not
current behavior or a pass of the selected revision.
