# DigiDollar Paymaster CLI Setup Wizard

> **Historical proposal, preserved 2026-10-01.** The current integration branch
> implements `digibyte-cli -paymastersetup` in `src/digibyte-cli.cpp`.
> Use the [current operator guide](../doc/digidollar-paymaster.md) for supported
> behavior. The implementation-status statements and design below describe the
> earlier source revisions explicitly named there, not the current branch.

> **Proposal update (2026-09-23): still not implemented at `a4f17f6315`.**
> The baseline table and design below refer to the original feature branch.
> Current Qt setup lives in `src/qt/paymasterwidget.cpp`, not
> `src/qt/digidollartab.cpp`. Manual operator RPCs remain available.
> Before implementation, reconcile this proposal with
> [finite pool setup](../doc/digidollar-paymaster-pool-setup.md): `accepted`
> acknowledges durable approval while `executed` describes immediate work;
> approved setup can continue while service is stopped and autostart is off.
> Do not interpret a returned plan or accepted command as confirmed liquidity.
> The [current operator guide](../doc/digidollar-paymaster.md) is authoritative
> for existing commands; `-paymastersetup` is not a supported CLI option.

## Status

> **Target specification — not implemented yet.**
>
> This document was reconciled with
> `feature/digidollar-paymaster-v1` at commit `bd270044c1`. The wallet-scoped
> Paymaster RPCs and their ordinary manual use through `digibyte-cli` exist.
> The `-paymastersetup` client option, the shared setup-plan builder and the
> dedicated CLI-wizard tests described below do not exist at that commit.

The implementation status is deliberately stated here because this document
defines a future client workflow. It must not be read as operator guidance for
an already available command.

## Goal

Provide a guided, resumable console setup for headless Paymaster operators:

```bash
digibyte-cli -rpcwallet=paymaster -paymastersetup
```

The wizard is a client-side `digibyte-cli` feature. It asks questions, builds a
complete plan, calls existing wallet RPCs and verifies their canonical replies.
It does not add an interactive RPC to `digibyted` and does not move policy or
safety validation out of Core.

The intended result is:

- an explicitly selected provider wallet;
- a persistent provider identity;
- a valid operating, safety and liquidity configuration;
- a preview-bound pool containing only missing outputs;
- a provider configuration that is enabled with autostart disabled; and
- an explicit command the operator may run later to start the provider.

The wizard never calls `startpaymaster` or `stoppaymaster`.

## Current implementation baseline

| Capability | Status at `bd270044c1` |
|---|---|
| Wallet-scoped provider RPCs | Implemented |
| Manual calls through `digibyte-cli -rpcwallet=...` | Implemented |
| Canonical setter replies | Implemented |
| Preview-bound pool execution using `plan_id` | Implemented |
| Qt guided provider wizard | Implemented locally in Qt |
| `digibyte-cli -paymastersetup` | **Not implemented** |
| Shared Qt/CLI setup-plan builder | **Not implemented** |
| Secure interactive CLI unlock guard | **Not implemented** |
| CLI-wizard end-to-end tests | **Not implemented** |

Relevant current sources are:

- `src/digibyte-cli.cpp` for client options and request handlers;
- `src/rpc/client.cpp` for CLI JSON argument conversion;
- `src/wallet/rpc/paymaster_provider.cpp` for provider RPC contracts;
- `src/paymaster/provider.cpp` for authoritative Core validation; and
- `src/qt/digidollartab.cpp` for the existing Qt-local wizard.

## Architectural boundaries

### Client-side orchestration

`-paymastersetup` is registered by `digibyte-cli`. A non-empty
`-rpcwallet=<wallet>` is mandatory. The wizard refuses to fall back to the
default wallet because identity, liquidity, budgets and provider accounting are
wallet scoped.

The CLI handler owns only:

- terminal questions and plain-text rendering;
- secure passphrase input without echo;
- construction of a proposed setup plan;
- ordered invocation of existing RPCs;
- exact comparison of requested and persisted values; and
- resumability derived from authoritative wallet state.

### Shared pure setup planner

Qt and CLI should consume a shared pure planner:

```text
src/paymaster/setup_profiles.h
src/paymaster/setup_profiles.cpp
```

The files belong to `libdigibyte_common`, which is already linked by both
`digibyte-cli` and Qt and already contains the Paymaster core types. The planner
must not access a wallet, node context, RPC table, UI object or global mutable
state.

Its conceptual entry point is:

```cpp
PaymasterSetupPlan BuildPaymasterSetupPlan(
    const PaymasterSetupSnapshot& current,
    const PaymasterSetupChoices& requested);
```

The returned plan contains canonical JSON-compatible values and an ordered list
of required mutations. It may construct a temporary safety bridge, but it does
not decide whether Core will accept a policy. Core remains authoritative.

### Core remains authoritative

Qt and CLI may check only input shape, whole-satoshi representability,
arithmetic overflow, completeness and exact response equality. The existing
RPCs remain solely responsible for determining whether:

- funding-model and sponsorship combinations are valid;
- payment and fee limits are allowed;
- safety relationships are valid;
- liquidity targets and maintenance limits are valid;
- the wallet is eligible and unlocked where required; and
- provider operation is ready.

The wizard must display a Core rejection unchanged as plain text. It must not
silently clamp, repair or retry with different economic values.

## Command contract

The planned form is:

```bash
digibyte-cli [connection options] -rpcwallet=<wallet> -paymastersetup
```

Requirements:

- `-rpcwallet` must be explicitly present and non-empty;
- no ordinary RPC command may be supplied together with `-paymastersetup`;
- all prompts and errors use plain text;
- cancellation before the apply confirmation performs no writes;
- cancellation or failure after apply reports which durable steps completed;
- a canceled wizard exits unsuccessfully; and
- a completed wizard exits successfully even when pool outputs still require
  confirmations, provided every requested write and funding operation
  completed.

## Authoritative initial snapshot

Before asking configuration questions, load all three status surfaces:

```text
getpaymasterinfo
getpaymastersafetystatus
getpaymasterliquiditystatus
```

The minimum provider-info fields are:

```text
settings_present
wallet_eligible
wallet_locked
enabled
running
operation_mode
autostart
provider_id (optional before identity creation)
ready
pool_ready
readiness_errors
policy (optional before policy creation)
```

Safety status is required to compare the complete provider safety policy.
Liquidity status is required to compare the complete liquidity policy and to
distinguish ready, pending and missing targets. `getpaymasterinfo` alone is not
sufficient for resumability because it does not return the complete safety
policy.

Abort before any write when:

- the RPC server or selected wallet is unavailable;
- `-rpcwallet` is absent or empty;
- `wallet_eligible=false`;
- `running=true`; or
- a required status reply is malformed or incomplete.

If mutations are required and the snapshot has both `enabled=true` and
`autostart=true`, abort and instruct the operator to disable the provider or set
autostart to false manually first. This prevents an automatic start between
individually guarded RPC calls while preserving the rule that the wizard never
stops a provider itself. A read-only run whose complete plan already matches may
finish without this additional restriction.

## Provider identity

If `provider_id` is present, preserve the existing identity and its display name
unchanged. Otherwise ask for a public display name and call:

```bash
createpaymasteridentity "Community Provider"
```

The display name must satisfy the current Core rule:

- at most 32 bytes;
- printable ASCII only (`0x20` through `0x7e`); and
- neither `/` nor `@` may occur.

The CLI may reject an invalid terminal input early, but it must still treat the
Core result as authoritative. A newly returned identity is accepted only when
`provider_id`, `identity_key`, `display_name` and `created_at` are structurally
valid and the display name exactly matches the request.

## Service model normalization

Offer these choices:

```text
1) User-paid
2) Sponsored — public
3) User-paid and sponsored — public
4) Sponsored — restricted
```

The pure planner produces:

| Choice | `funding_models` | `sponsorship_scope` | `fee_rate_bps` | Carrier targets |
|---|---|---|---:|---:|
| User-paid | `user_paid` | `public` | operator value | 3 / 1 default |
| Sponsored public | `sponsored` | `public` | 0 | 0 / 0 |
| Both public | `sponsored`, `user_paid` | `public` | operator value | 3 / 1 default |
| Sponsored restricted | `sponsored` | `restricted` | 0 | 0 / 0 |

Restricted sponsorship is always sponsored-only. Sponsored-only always has a
zero service-fee rate. Existing carrier outputs are not spent, destroyed or
retired when carrier targets become zero; they remain wallet controlled until
the operator performs a separate preview-bound withdrawal or rebalance.

## Operating policy

Recommended User-paid defaults are:

```json
{
  "funding_models": ["user_paid"],
  "sponsorship_scope": "public",
  "fee_rate_bps": 50,
  "min_amount_cents": 100,
  "max_amount_cents": 100000,
  "quote_ttl": 60,
  "maximum_network_fee_dgb_satoshis": 20000000
}
```

Interpretation:

- 50 basis points = 0.50%;
- 100 cents = 1 DD;
- 100000 cents = 1000 DD; and
- 20000000 DGB satoshis = 0.20 DGB.

Core currently requires a minimum payment of at least 100 cents. The wizard
must not advertise a smaller value as configurable.

The wizard submits this object through `setpaymasterpolicy`. The RPC returns the
persisted canonical policy and `policy_hash`. The wizard compares funding
models as a set and every other submitted field exactly. A different, missing
or out-of-range reply is a failed step.

## Safety profiles

Offer:

```text
1) Conservative
2) Recommended
3) Custom
```

Let `P` be the selected
`maximum_network_fee_dgb_satoshis`. Standard profiles do not replace or lower
that operator-selected per-transfer ceiling. Instead they control repeated and
aggregate exposure:

| Limit | Conservative | Recommended |
|---|---:|---:|
| Fee per transaction | `P` | `P` |
| Concurrently reserved | `max(P, 0.20 DGB)` | `max(P, 1.00 DGB)` |
| Fee per rolling hour | `max(P, 0.50 DGB)` | `max(P, 2.00 DGB)` |
| Fee per rolling day | `max(hour, 2.00 DGB)` | `max(hour, 10.00 DGB)` |
| Completed per hour | 5 | 10 |
| Completed per day | 25 | 100 |

Both profiles use these request-admission defaults:

```text
maximum_active_quotes_total = 16
maximum_active_quotes_per_netgroup = 4
maximum_active_quotes_per_recipient = 2
maximum_quote_requests_per_netgroup_per_minute = 10
```

Every selected funding class receives the chosen finite limits. Every inactive
class is represented by all six zero values. Zero never means unlimited.

For example, Conservative with the recommended advertised cap of 0.20 DGB
produces:

```json
{
  "user_paid": {
    "maximum_network_fee_per_transaction_satoshis": 20000000,
    "maximum_reserved_network_fee_satoshis": 20000000,
    "maximum_network_fee_per_hour_satoshis": 50000000,
    "maximum_network_fee_per_day_satoshis": 200000000,
    "maximum_completed_per_hour": 5,
    "maximum_completed_per_day": 25
  },
  "public_sponsored": {
    "maximum_network_fee_per_transaction_satoshis": 0,
    "maximum_reserved_network_fee_satoshis": 0,
    "maximum_network_fee_per_hour_satoshis": 0,
    "maximum_network_fee_per_day_satoshis": 0,
    "maximum_completed_per_hour": 0,
    "maximum_completed_per_day": 0
  },
  "restricted_sponsored": {
    "maximum_network_fee_per_transaction_satoshis": 0,
    "maximum_reserved_network_fee_satoshis": 0,
    "maximum_network_fee_per_hour_satoshis": 0,
    "maximum_network_fee_per_day_satoshis": 0,
    "maximum_completed_per_hour": 0,
    "maximum_completed_per_day": 0
  },
  "maximum_active_quotes_total": 16,
  "maximum_active_quotes_per_netgroup": 4,
  "maximum_active_quotes_per_recipient": 2,
  "maximum_quote_requests_per_netgroup_per_minute": 10
}
```

Custom values are submitted through `setpaymastersafetypolicy` exactly as
entered after type, overflow and whole-satoshi checks. Qt or CLI must not
reinterpret Core's economic relationships. A successful setter reply must
match every submitted limit value.

### Safety bridge during reconfiguration

The operating-policy and safety-policy setters are independently atomic and
each validates against the other policy already persisted in the wallet.
Changing funding models or lowering the advertised fee cap can therefore need a
temporary safety bridge.

The shared planner must:

1. compare the previous and final operating policies;
2. include every funding class required by either policy in the bridge;
3. cap every non-zero bridge per-transaction value at the lower of the old and
   new advertised network-fee ceilings;
4. preserve valid aggregate ordering in the bridge;
5. retain the already persisted quote-rate limits in the bridge;
6. persist the bridge before the new operating policy; and
7. persist the exact final safety policy immediately afterward.

The bridge is a durable, reportable setup step. If a later step fails, a new
wizard run derives the next action from the actual persisted policies rather
than from a separate progress file.

## Liquidity and maintenance

Default targets are:

| Model | Admission DGB | Operational DGB | Admission carriers | Operational carriers |
|---|---:|---:|---:|---:|
| User-paid or both | 3 | 1 | 3 | 1 |
| Sponsored-only | 3 | 1 | 0 | 0 |

Default ongoing operation is automatic replenishment with paid maintenance
explicitly approved. The maintenance profiles are:

| Limit | Conservative | Recommended |
|---|---:|---:|
| Per maintenance transaction | 0.10 DGB | 0.20 DGB |
| Per rolling hour | 0.50 DGB | 2.00 DGB |
| Per rolling day | 2.00 DGB | 10.00 DGB |

Before setting `paid_maintenance_approved=true`, display the complete finite
limits and require:

```text
Type APPROVE to authorize paid automatic maintenance:
> APPROVE
```

Any other input leaves paid maintenance disabled. The approval authorizes only
future maintenance within the persisted Core limits; it does not start the
provider.

Example Recommended User-paid policy:

```json
{
  "automatic_replenishment": true,
  "paid_maintenance_approved": true,
  "target_admission_dgb": 3,
  "target_operational_dgb": 1,
  "target_admission_carriers": 3,
  "target_operational_carriers": 1,
  "maximum_maintenance_fee_per_transaction_satoshis": 20000000,
  "maximum_maintenance_fee_per_hour_satoshis": 200000000,
  "maximum_maintenance_fee_per_day_satoshis": 1000000000
}
```

The wizard persists this complete object with
`setpaymasterliquiditypolicy` and accepts success only when the canonical reply
matches every submitted target, consent flag and maintenance limit.

## Runtime and enablement

A fresh CLI setup requests:

```json
{
  "operation_mode": "automatic",
  "autostart": false
}
```

The wizard sends these values to `setpaymasterruntimesettings` and verifies the
returned `operation_mode`, `autostart` and `running` fields. It then calls
`setpaymasterenabled true` only after all required policy and funding steps
have succeeded.

After every required policy and funding step succeeds, call:

```text
setpaymasterenabled true
```

`setpaymasterenabled` does not synchronously start the provider. With autostart
false the provider remains stopped. The wizard must verify the returned
`enabled` and `running` values exactly. The runtime values come from the
preceding `setpaymasterruntimesettings` reply and are checked again in the final
`getpaymasterinfo` snapshot.

## Pool preview and funding

Always derive a fresh, side-effect-free Core preview after the final operating,
safety and liquidity policies have been persisted:

```json
{
  "admission_dgb_slots": 3,
  "operational_dgb_slots": 1,
  "admission_carrier_slots": 3,
  "operational_carrier_slots": 1,
  "execute": false
}
```

The reply includes:

- the requested targets;
- missing targets after existing wallet entries are considered;
- the value assigned to every output class;
- total DGB and DD assigned to missing outputs; and
- a non-null `plan_id`.

The preview does not promise the eventual transaction network fee. Core derives
that fee when constructing the transaction.

If all missing counts are zero, skip funding. Unconfirmed wallet-owned pool
outputs are not duplicated; final readiness may remain pending until they
confirm.

When outputs are missing, require a separate strong confirmation:

```text
Type FUND POOL to create exactly the reviewed missing outputs:
> FUND POOL
```

Execution must submit the same targets and the unchanged preview plan ID:

```json
{
  "admission_dgb_slots": 3,
  "operational_dgb_slots": 1,
  "admission_carrier_slots": 3,
  "operational_carrier_slots": 1,
  "execute": true,
  "plan_id": "<plan_id returned by the immediately preceding preview>"
}
```

Core recomputes the plan. `PAYMASTER_POOL_PLAN_CHANGED` or
`PAYMASTER_POOL_PLAN_REQUIRED` returns to a fresh preview; the wizard must never
reuse an older approval.

Carrier and DGB output creation may commit in separate transactions. If a later
part fails, do not invent a rollback. Report every returned transaction ID and
rerun the preview on resume so already wallet-owned outputs reduce the next
plan.

## Review and ordered apply

Before the first mutation, print the complete normalized plan, including:

- selected wallet and whether an identity already exists;
- funding models, scope, payment range, service fee and network-fee cap;
- all three final safety classes and request limits;
- liquidity targets and maintenance consent;
- runtime, autostart and enabled state;
- required safety bridge, if any; and
- the steps that match current state and will be skipped.

Require:

```text
Type APPLY SETUP to persist this configuration:
> APPLY SETUP
```

The ordered execution is:

```text
1.  Refresh getpaymasterinfo, getpaymastersafetystatus and
    getpaymasterliquiditystatus.
2.  Rebuild the plan; abort if the reviewed state changed materially.
3.  Create the identity only when missing.
4.  Persist a temporary safety bridge only when required.
5.  Call setpaymasterpolicy only when the final operating policy differs.
6.  Call setpaymastersafetypolicy only when the final safety policy differs.
7.  Call setpaymasterliquiditypolicy only when the final liquidity policy
    differs.
8.  Preview the exact current pool deficit with preparepaymasterpool and
    execute=false.
9.  Obtain the separate FUND POOL confirmation when outputs are missing.
10. Execute with the unchanged plan_id and exact targets.
11. Call setpaymasterruntimesettings for automatic runtime with autostart=false
    only when different.
12. Call setpaymasterenabled true only when different.
13. Reload all three authoritative status RPCs.
```

Every setter result must equal the submitted canonical values. A completed
step is not repeated merely because a later step fails.

## Resumability

Do not persist a wizard progress record. Rebuild progress from Core state:

| Authoritative state | Next behavior |
|---|---|
| `provider_id` exists | Preserve identity |
| Operating policy equals final plan | Skip operating-policy setter |
| Safety equals required bridge but operating policy is old | Continue with operating policy |
| Operating policy is final but safety is the bridge | Continue with final safety |
| Final safety equals plan | Skip final safety setter |
| Liquidity policy equals plan | Skip liquidity setter |
| Runtime equals automatic/autostart false | Skip runtime setter |
| `enabled=true` | Skip enablement setter |
| Pool preview reports no missing slots | Skip funding |
| Required pool entries are pending confirmation | Report waiting state; do not fund duplicates |

If an exact comparison cannot be made because a reply is incomplete,
unrecognized or outside supported integer ranges, fail closed and make no new
mutation.

## Wallet unlocking

Unlock only immediately before a signing operation:

- new identity creation; or
- `preparepaymasterpool execute=true`.

Policy, safety, liquidity, runtime, enablement, preview and status calls do not
hold an unlock lease.

Read the passphrase from the controlling terminal with echo disabled. Never
place it in process arguments, generated shell commands, logs, error messages or
the setup plan. Restore the original wallet lock state before handling the RPC
result or showing another prompt.

A CLI-local RAII guard may wrap the existing RPC transport:

```cpp
class WalletUnlockGuard
{
public:
    WalletUnlockGuard(RpcClient& rpc, bool initially_locked);
    bool EnsureUnlocked();
    ~WalletUnlockGuard();
};
```

The guard must relock only when the wizard performed the unlock and the wallet
was initially locked.

## Backup reminder

After creating a new identity, prominently require a complete wallet backup.
The supported built-in operation remains `backupwallet`.

Operators using another full-wallet backup mechanism may later call:

```bash
digibyte-cli -rpcwallet=paymaster \
  acknowledgepaymasterproviderbackup \
  '{"external_backup":true}'
```

The wizard must never set that acknowledgement automatically. It may offer the
RPC only after the operator confirms that a real external full-wallet backup
already exists. Seed-only, descriptor-only and finance CSV exports are not
complete provider backups.

## Completion

Always reload:

```text
getpaymasterinfo
getpaymastersafetystatus
getpaymasterliquiditystatus
```

Display the exact persisted policies, readiness errors and pending
confirmations. If `ready=true`, print only:

```text
Provider setup is ready. Start it explicitly with:

  digibyte-cli -rpcwallet=<wallet> startpaymaster
```

If readiness is pending, explain the returned Core readiness errors and still
do not start the provider.

## Known Qt/Core deviations to resolve before sharing the planner

The current Qt-local wizard and its mock tests contain assumptions that the
shared planner must not reproduce:

1. Merely toggling to Sponsored-only disables the service-fee editor but does
   not always set its value to zero unless defaults are restored. Core rejects
   a non-zero Sponsored-only fee.
2. A current Qt reconfiguration test retains 3/1 carrier targets after changing
   to Sponsored-only. The real `preparepaymasterpool` RPC requires 0/0 carrier
   targets for a Sponsored-only policy.
3. The same test retains an inactive User-paid per-transaction safety value
   above a newly lowered advertised network-fee cap. Core validates every
   non-zero safety class against that cap and rejects the final policy.
4. Qt may disable an existing enabled provider during apply and may preserve
   autostart. The CLI contract in this document instead aborts at the unsafe
   precondition and never starts or stops provider operation.

The shared planner should normalize items 1 through 3 for both frontends, and
tests must exercise the real Core validation rather than only permissive mocked
RPC replies.

## Tests

### Shared planner unit tests

Cover at least:

1. Fresh User-paid Recommended plan.
2. Fresh User-paid Conservative plan with a 0.20-DGB advertised cap.
3. Custom safety values are preserved field-for-field and value-for-value.
4. Sponsored public forces fee and carriers to zero.
5. Sponsored restricted forces sponsored-only, fee zero and carriers zero.
6. Both public models receive independent finite safety classes.
7. Inactive safety classes become all-zero.
8. Standard profiles retain the independently selected network-fee cap.
9. Lowering the cap produces a valid temporary safety bridge.
10. Existing matching values produce no mutation step.
11. Qt and CLI choices produce identical normalized plans.

### CLI interaction and functional tests

Cover at least:

1. Fresh unlocked User-paid wallet.
2. Locked wallet with hidden passphrase input and restored lock state.
3. Existing provider identity is preserved.
4. Sponsored public persists fee zero and no carrier targets.
5. Sponsored restricted persists fee zero and no carrier targets.
6. User-paid and Sponsored public configuration.
7. Cancellation before the first mutation.
8. Failure or interruption after operating policy.
9. Cancellation after pool preview.
10. Partially existing pool.
11. Unconfirmed pool outputs are not duplicated.
12. Running provider is rejected without stopping it.
13. Enabled/autostart provider requiring mutation is rejected before writes.
14. Funding confirmation is declined.
15. Partial pool-funding failure resumes from real wallet state.
16. Passphrase appears in neither process arguments nor logs.
17. Missing or empty `-rpcwallet` is rejected.
18. Changed `plan_id` requires a new preview and confirmation.
19. Policy transition requiring a safety bridge resumes at every boundary.
20. Sponsored transition with existing carrier outputs does not spend them and
    uses zero new carrier targets.
21. Lower advertised cap never leaves an invalid inactive safety class.
22. Final status reports pending confirmations without claiming readiness.
23. No execution path calls `startpaymaster` or `stoppaymaster`.

Direct RPC functional tests and Qt widget tests are useful prerequisites but do
not count as CLI-wizard end-to-end coverage. At least one functional test must
run the built `digibyte-cli` process with a real `digibyted` wallet and Core
validation.

## Acceptance criteria

The feature is complete only when:

- `digibyte-cli -rpcwallet=<wallet> -paymastersetup` is registered and
  documented by `-help`;
- every mutation uses an existing wallet RPC;
- the same pure planner supplies Qt and CLI normalized values;
- the wizard resumes from wallet state without a separate progress record;
- pool execution requires the exact current `plan_id` and targets;
- passphrases never enter command-line arguments or logs;
- the wizard neither starts nor stops a provider;
- Core rejections remain authoritative and visible;
- the dedicated unit and functional scenarios above pass; and
- existing manual RPC usage remains compatible.
