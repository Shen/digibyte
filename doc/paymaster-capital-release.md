# Releasing Paymaster capital

Protected reserves are excluded from ordinary coin selection. Wallet total
includes them; available balance does not. Release makes eligible pool entries
available to ordinary wallet use. It does **not** transfer funds, pay a fee,
sign a transaction, delete identity/history or delete the wallet.

## Whole-provider retirement

In the GUI, use **Settings > Start, pause & retire**, the first Settings item.
**Funds & reserves** provides an internal link. Pause and Stop use the same
persistent pause; Stop additionally offers an optional full-release review.
Retire always leads to that review and then to remaining balances and backup.
The checklists describe these effects, not completed operations.

1. Stop the provider, disable provider enablement and autostart.
2. Resolve outstanding financial, maintenance and signed/ambiguous work. Ensure
   chain/wallet readiness; unconfirmed signed authority cannot be ignored.
3. Preview `releasepaymastercapital`. Inspect returned `plan_id`, `pool_entries`,
   `dgb_satoshis` and `dd_cents`.
4. Execute the exact reviewed plan using same-build RPC help for its options.

`stoppaymaster` first registers temporary stop priority for this wallet. New
provider work, including automatic start and refill, cannot overtake that request.
It waits up to five seconds for the current work owner to finish, then acquires
exclusive ownership through persistent pause and runtime stop. Other wallets
continue independently. The wait never interrupts signing or bypasses its guard.
If work remains busy or persistence fails, stop is not confirmed and no capital
is released. Scope cleanup removes the temporary priority so a failed attempt
does not leave a hidden permanent block; an existing work owner is preserved.

A confirmed GUI Stop/Pause disables provider enablement and autostart and clears
one-shot start intent. This prevents automatic execution but retains saved refill
preferences for explicit resumption. Completing retirement additionally disables
automatic refill and revokes its fee approval. A bare RPC `stoppaymaster` without
persistent options still allows saved autostart to resume later.
Approved full release through Stop disables refill and its approval as well.
After ordinary retirement the service remains stopped; the GUI does not treat
missing restart reserves or budgets as evidence of an active provider. Release
results and incomplete balance/backup review remain separate from service state.

Starting without enough funds can leave a planned reserve-funding task with no
transaction. Normal GUI **Retire Paymaster** includes these tasks in its release
review and displays the number to cancel. Declining that review keeps both the
reserves and tasks. Ordinary **Stop** keeps pending work by default.

RPC/CLI operators can opt in explicitly. After persistent stop, review:

```json
{"method":"releasepaymastercapital","params":[{"cancel_uncreated":true}]}
```

Inspect `uncreated_maintenance` as well as the amounts and plan. To approve,
repeat `cancel_uncreated: true` with `execute: true` and that exact `plan_id`.
Omission retains the strict default: unfinished maintenance blocks release.
This option is unavailable for restored-wallet recovery.

Only planned DGB/DD reserve funding with no transaction or evidence of signing
is eligible. A wallet transaction matching even one planned output script blocks
cancellation, including a lost journal reply or a different output amount.
Signed, ambiguous, failed and withdrawal work remain protected. Cancelled tasks
stay in the maintenance history as released; the ledger and pool changes commit
atomically under the existing work guard and wallet lock.

Execution rechecks state under the work guard and wallet lock, then releases
eligible entries atomically. Manual coin locks remain respected. Automatic
refill is disabled and paid maintenance approval revoked; configured targets,
budgets and history remain. A backup reminder records the material change.

If the reply is lost, inspect actual state. Replaying an obsolete plan can fail
with `PAYMASTER_CAPITAL_PLAN_CHANGED`; it does not justify creating another
operation blindly. Review a fresh preview if further release is needed.

## Individual DD carrier

`withdrawpaymastercarrier` supports a separately reviewed selected carrier
outpoint. Releasing it reduces provider capacity; it is not permission to take
inputs reserved by an active or ambiguous payment. Inspect the exact RPC plan
and current guards. This differs from stopping/retiring the entire provider.

## Restoration

A restored provider wallet is guarded before autostart. An independent checkpoint
helps detect an unmarked stale wallet copy. `adoptpaymasterbackup` supports only
reviewed matching/quiescent states and cannot manufacture missing commit or
budget history. Do not delete guards/checkpoints to make release succeed.

See [operator guide](digidollar-paymaster-operator.md),
[threat model](paymaster-threat-model.md) and [tests](digidollar-paymaster-testing.md).
Relevant functional scenarios are `wallet_paymaster_backup_exit.py`,
`wallet_paymaster_backup_adoption.py`, `wallet_paymaster_checkpoint_rollback.py`
and `wallet_paymaster_lifecycle.py`.
Targeted unit coverage includes `provider_stop_waits_for_work_without_bypassing_exclusion`
and `capital_release_cancels_only_reviewed_uncreated_funding` in
`paymaster_wallet_identity_tests`. The `manager_provider_stop_*` cases in
`paymaster_wire_tests` cover stop priority, active-owner preservation, duplicate
requests, failure cleanup and other-wallet independence. Qt coverage is in
`paymasterStopAndRelease` and `paymasterRetirementLateResults`.
