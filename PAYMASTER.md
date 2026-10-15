# DigiDollar Paymaster: developer guide

Documentation baseline: `integration/paymaster-v9.26.7`, source revision
`e7c2c8c4bd`, reviewed on 2026-10-15. This identifies the source described here,
not a release certification. See the [release gate](doc/digidollar-paymaster-release-gate.md).

## Scope and integration rule

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

## Reading order

| Document | Responsibility |
|---|---|
| [Implementation](doc/digidollar-paymaster-implementation.md) | Protocol, transaction construction, durable state and source ownership. |
| [Integration boundaries](doc/design/paymaster-integration-boundaries.md) | Compatibility constraints and review of shared DigiByte code. |
| [Threat model](doc/paymaster-threat-model.md) | Security invariants, corrected findings and residual risks. |
| [Client contract](doc/digidollar-paymaster-integration.md) | Prepare, authorize, resume, cancel and interpret payment status. |
| [Node and RPC reference](doc/digidollar-paymaster.md) | Configuration, units, prerequisites and method groups. |
| [Operator guide](doc/digidollar-paymaster-operator.md) | GUI/CLI setup, encrypted wallets, operation and diagnosis. |
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
