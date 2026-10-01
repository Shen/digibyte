# Stop a provider and release its operating capital

Start/resume and Pause belong to Qt **Overview**. Pause retains prepared pool
capital and disables provider operation and autostart until an explicit resume.
Under **Funds & reserves → Stop operation and release capital**, **Stop Paymaster…** offers an
unchecked **Also review release of all operating capital** option. Stopping is
confirmed first. A second review shows the exact DGB and DD amounts and pool
output count before releasing anything. Declining this review leaves the
provider stopped and its capital prepared.
The node, its listeners and other wallets are not shut down by this wallet-local action.

## Settings and retirement

Settings separates Offer, Spending limits, Operation & automation,
Node connection, and Wallet & backup. Capital actions and retirement are visible
in Funds & reserves. Its shared target/cost form has Save and Discard controls.
Autostart and refill switches are immediate and mirrored on Overview; request
processing remains a separate saved choice. Automatic request processing is not automatic startup,
and neither is recurring paid reserve maintenance.

**Retire Paymaster…** is a guided, non-destructive completion of the same
stop/release workflow. The first confirmation stops the provider; the separate
exact-amount review authorizes release. Open or signed work blocks release.
After a matching execution receipt, wallet balances are read with `getbalances`
and `getdigidollarbalance "" 0` (including pending DD). DGB trusted, untrusted
pending, immature and optionally reuse-excluded amounts are listed separately;
trusted is not labelled confirmed or wholly spendable. Balance observations are
timestamped and are not a live or atomic snapshot. Malformed/failed reads never
imply zero funds or completed retirement.

The completion panel opens the existing full-wallet backup workflow and points
to **File → Close Wallet** as an optional manual step for a dedicated wallet.
It never transfers funds, unloads the wallet, deletes files or erases identity
records. A cancelled backup does not acknowledge success. A cleared Core backup
reminder is not proof of an offline, recoverable archive. Preserve the wallet
and backup for future incoming funds and recovery; never run an archived copy
of the same identity alongside the original.

Wallet changes discard old RPC replies. Privacy mode halts further guided steps
and hides/clears balance details. A mutation already dispatched may still finish;
no blind retry or claim of rollback is made. After reopening, inspect persisted
Core state; the archive checklist itself is not a persistent retirement record.

`releasepaymastercapital` is wallet-local: it changes eligible pool records to
`released`, returning their outputs to ordinary wallet coin selection. It does
not send coins to another address, sign a transaction, charge a network fee,
remove the wallet, delete the provider identity or abandon a payment.

## Releasing one DD reserve

**Funds & reserves → Release one DD reserve** reduces the saved DD payment
capacity by one. This differs from releasing all capital during retirement.
The GUI first refreshes the available confirmed reserves, binds the user's
choice to its output, and obtains a fresh preview before approval. Core still
rechecks availability when executing the reviewed plan.

If the reserve is no longer available, the task offers **Refresh reserves**
and explains that it may be in use, unconfirmed, spent or already released.
Refresh only reads status. It does not release a replacement reserve, bypass
reservations, or resume a provider that was paused during the approved action.
Technical error codes remain in an initially collapsed disclosure. A new
release always needs another explicit selection and review.

## CLI workflow

Use the correct network, data directory and wallet on every invocation. For an
isolated Regtest wallet, for example:

```sh
digibyte-cli -regtest -rpcwallet=PROVIDER stoppaymaster '{"persistent":true,"pause_setup":true}'
digibyte-cli -regtest -rpcwallet=PROVIDER releasepaymastercapital
```

Review `pool_entries`, `dgb_satoshis`, `dd_cents` and the zero network fee. Only
then submit the exact returned plan identifier:

```sh
digibyte-cli -regtest -rpcwallet=PROVIDER releasepaymastercapital '{"execute":true,"plan_id":"REVIEWED_PLAN_ID"}'
```

No preview creates an authorization record. Execution rechecks the current
state while holding both the provider work guard and wallet lock. It atomically
releases the whole eligible pool, disables automatic replenishment and revokes
paid-maintenance approval, and updates the wallet-backup reminder. Targets,
safety budgets, identity, accounting and recovery evidence remain stored.
Restart does not reenable provider operation. A future restart of the *service*
requires explicit setup/replenishment and approval again.

## Fail-closed conditions and recovery

- The runtime must be stopped; saved `enabled` and `autostart` must both be false.
- Initial block download must be complete and the wallet caught up to the node.
- Any open provider payment/capacity work, reserved budget/input, pending or
  ambiguous pool entry, or unfinished maintenance blocks the **whole** release.
- Signed payment and maintenance transactions must be confirmed. Current wallet
  confirmation and spent state are checked, not only cached pool heights.
- Manual coin locks are respected; this RPC does not override them.
- An unreadable database record, changed policy/state or wrong plan rejects the
  operation. Database-write/commit failure must not leave a partial release.

Inspect Activity/recovery and pool-preparation status when blocked. Cancel only
truly unsigned setup through the existing reviewed cancellation workflow; allow
signed transactions to confirm or use their established recovery paths. A
stopped provider does **not** imply it is safe to discard signatures or unlock
reservations. Never delete the wallet to force a shutdown.

A lost execution response is not an invitation to repeat blindly. The old plan
changes after a successful release, and replay returns `PAYMASTER_CAPITAL_PLAN_CHANGED`.
Inspect `getpaymasterpoolinfo`, settings and a fresh preview: zero remaining pool
outputs is distinguishable from an unsuccessful release. Historical records
remain; they are not active funds. Create a full-wallet backup after completion.

## Tests

- `paymaster_wallet_identity_tests/capital_release_is_reviewed_atomic_and_fail_closed`:
  pure preview, exact binding, unsafe states, pending outputs, stopped settings,
  database-write/commit rollback, wallet-local release, no new transaction and replay.
- `wallet_paymaster_pool_setup.py`, native and `--usecli`: rejects unfinished
  Dandelion setup, exercises USER_PAID and sponsored pools via actual CLI,
  changes a policy between preview/execute, checks values and zero fees,
  rejects replay and verifies released pool/settings/identity after restart.
- `PaymasterWidgetTests::paymasterStopAndRelease`: cancelled stop, stop only,
  rejected release, success, Core blocker, mismatched receipt, privacy and wallet change.

Test source is not a runtime PASS. Build and execution evidence must be recorded
for the exact source snapshot before using the feature with a funded wallet.

## Windows integration check (2026-09-30)

Integration of `7ad04ea744` with the local guided-restoration changes exposed
three Qt compile defects: duplicate retirement-layout locals, a local named
`slots` colliding with Qt's keyword macro, and deleted counters still used by
the advanced capacity editor. The integration fixes rename the locals and keep
editor counters separate from the overview's saved per-pool readiness check.
The existing local restoration and withdrawal changes remain in place.
A complete build and execution of the new Core/RPC and Qt tests remain required;
older binary test results do not validate this combined source revision.
