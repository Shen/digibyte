# Releasing Paymaster capital

Protected reserves are excluded from ordinary coin selection. Wallet total
includes them; available balance does not. Release makes eligible pool entries
available to ordinary wallet use. It does **not** transfer funds, pay a fee,
sign a transaction, delete identity/history or delete the wallet.

## Whole-provider retirement

1. Stop the provider, disable provider enablement and autostart.
2. Resolve outstanding financial, maintenance and signed/ambiguous work. Ensure
   chain/wallet readiness; unconfirmed signed authority cannot be ignored.
3. Preview `releasepaymastercapital`. Inspect returned `plan_id`, `pool_entries`,
   `dgb_satoshis` and `dd_cents`.
4. Execute the exact reviewed plan using same-build RPC help for its options.

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
