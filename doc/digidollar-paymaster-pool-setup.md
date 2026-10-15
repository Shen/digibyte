# Paymaster pool setup and recurring refill

This guide assumes the [Mainnet operator setup](digidollar-paymaster-operator.md).
Funding and refill spend real DGB/DD; example counts and fee ceilings require
operator review. Use separate [test networks](digidollar-paymaster-test-networks.md)
for rehearsals of the same approval and recovery workflow.

Pools separate admission from operational liquidity. DGB slots fund network
fees; DD carrier slots support nonzero service fees below the minimum DD output.
Free sponsorship does not need a DD carrier for a service fee. Usable slots
require confirmed, free inputs valid for the request.

## Finite approved setup

`preparepaymasterpool` previews a finite plan. Example options:

```json
{
  "admission_dgb_slots": 3,
  "operational_dgb_slots": 1,
  "admission_carrier_slots": 3,
  "operational_carrier_slots": 1,
  "maximum_fee_satoshis": 20000000
}
```

Review the response; submit the same options with `execute=true` and `plan_id`
to approve. Counts and the per-step fee ceiling are examples, not universal
requirements. Setup can create one DD carrier transaction and one DGB
transaction; combined authorized fees depend on the missing asset classes.
Core defaults and CLI proposed ceilings need not match. A ceiling is not an
expected fee.

`accepted=true` means durably accepted, not necessarily `executed=true`,
broadcast or confirmed. A disabled provider can retain an accepted command
with `PAYMASTER_PROVIDER_DISABLED`; acceptance does not enable or sign for it.
The maintenance tick resumes approved work when feature/provider enablement,
history/index readiness and signing prerequisites permit. It can continue
with service stopped and autostart off; this does not start service or grant
recurring maintenance approval. New signatures require wallet unlock.

| State | Meaning |
|---|---|
| `pending_creation` | An approved step has no transaction; inspect funding/prerequisites. |
| `pending_confirmation` | A retained transaction needs chain confirmation. |
| `complete` | Finite command completed. |
| `conflict` | Retained transaction/input conflict needs reconciliation, not blind replacement. |
| `cancelled` | Unsigned remaining work was safely cancelled. |

Exact signed bytes survive response loss, rejection and restart. A known signed
step is not recreated merely because broadcast/confirmation failed. Children
need confirmed parents. Funding status distinguishes confirmed/unconfirmed
funds; a shortfall estimate can include the full approved fee ceiling.
On Mainnet, retained transactions need confirmation in network-mined blocks.
Confirmation delays do not authorize recreating a signed funding step.

## Recurring automatic refill

`setpaymasterliquiditypolicy` and the operator workflow configure targets and
separately approved automatic maintenance limits. `getpaymasterliquiditystatus`
reports actual state. The daemon runs this headlessly; finite setup approval
alone does not authorize indefinite paid refill.

When funds cannot fill all missing slots, create fewer **complete slots**.
This applies to DGB and DD-carrier funding. If three slots are missing but only
one plus fees is affordable, fill one; do not split money across three partial
slots. Preserve the target, wait for confirmation and re-evaluate. Transaction
and rolling maintenance fee limits still apply; no new spending authority is
created by this fallback.

Confirmed free capacity remains usable when further refill cannot reach its
target. Payment readiness and maintenance funding/confirmation state are
separate. Target count is not an all-or-nothing service gate.

## Cancellation and verification

Pause future setup/maintenance explicitly. Cancellation must not release inputs
protected by known signatures or ambiguous broadcast. Manual locks and restore
guards remain authoritative. Use [capital release](paymaster-capital-release.md)
for retirement.

The management-task card offers **Cancel uncreated reserve work** for pending
DGB/DD funding, including recurring refill and finite setup. Review the exact
operations and unused fee allowance before confirming. Core atomically closes
only those operations and turns recurring refill off to prevent recreation.
Existing reserves, provider enablement/autostart, signed work and payments stay
unchanged. Re-enable refill explicitly in its settings when wanted. The separate
setup fee-review workflow can still cancel and replace a finite setup plan.

For CLI/RPC, select `operation_id` values from
`getpaymasteroperatorinfo.provider.active_operations`, then preview:

```json
{"method":"cancelpaymastermaintenance","params":[{"operation_ids":["<operation_id>"]}]}
```

Inspect `operation_ids`, `plan_id` and `released_fee_budget_satoshis`. Submit the
same selection with `execute:true` and that exact `plan_id` to confirm. A changed
plan requires a fresh review. Withdrawal, signed and ambiguous operations cannot
be cancelled this way. Core checks wallet transactions as well as the journal;
a missing journal transaction ID alone does not establish safe cancellation.
The command signs/sends nothing and requires no wallet unlock. A lost execution
reply requires a fresh status read, not an assumption of success.

Task names follow the current saved operation kind and asset, not a preceding
GUI action. No remaining active work clears a stale waiting/blocked task even
when reserve capacity is still zero; cancellation does not imply ready reserves.

Functional coverage: `wallet_paymaster_pool_setup.py`,
`wallet_paymaster_partial_capacity.py`, `wallet_paymaster_operator.py`,
`wallet_paymaster_readiness.py`. Wallet store/identity tests cover persisted
commands, budgets and signing gates. See [testing](digidollar-paymaster-testing.md).
