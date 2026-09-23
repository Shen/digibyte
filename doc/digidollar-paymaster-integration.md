# Paymaster client integration contract (version 1)

This contract prepares existing Paymaster interfaces for external clients. It
is implemented on top of `46add7e4d3` on `integration/paymaster-v9.26.6rc2`.
It does not implement x402, an agent-wide spending budget, another send RPC,
or deferred execution after signing. No consensus, Paymaster wire encoding,
wallet record encoding, or wallet feature flag is introduced by this package.

## Entry point and units

Use the wallet endpoint `/wallet/<wallet-name>` and authenticated local RPC.
The entry point is:

```text
senddigidollar address amount comment fee_rate selected_inputs amount_unit options
```

Pass an integer `amount`, `amount_unit="cents"`, and `options.fee_mode="paymaster"`.
Keep the existing argument positions: the unit is argument 5 and options is
argument 6 (zero based). Named RPC arguments are also supported. The CLI JSON
conversion table already parses amount, selected inputs and options; the new
zero-argument `getpaymasterclientinfo` needs no conversion entry.

For an external invoice, leave `subtract_paymaster_fee_from_amount` and
`send_all_spendable_dd` false:

| Field | Meaning |
| --- | --- |
| `payment_cents` | DD received by the recipient; the invoice amount |
| `service_fee_cents` | Additional DD paid for the Paymaster service |
| `user_total_cents` | Recipient payment plus service fee |
| `requested_amount_cents` | Original request amount; equals the recipient amount in this mode |

One DD dollar is 100 cents. There are no floating-point cent amounts. Explicit
`dollars` requests remain supported: 200 cents and 2.00 dollars describe the
same order. Integer amounts without a unit retain the existing cent behavior;
ambiguous decimal requests without a unit are rejected. There is no new amount
mode. Recipient outputs are at least 100 cents and at most 10,000,000 cents;
the total including service fee is also bounded at 10,000,000 cents. Provider
terms and available inputs can impose tighter limits. Ordinary direct DGB
funding and the existing exact-total/sweep options keep their own behavior.

## Capability and readiness query

`getpaymasterclientinfo` has no parameters. It reports `integration_version=1`,
`network`, the full `genesis_hash`, `amount_unit`, `minimum_payment_cents`,
`maximum_payment_cents`, `maximum_user_total_cents`, `fee_modes`,
`funding_models`, and `sponsorship_scopes`.

`supported` describes the binary's integration support. `ready` and
`readiness_errors` describe the wallet/node's local authorization prerequisites:
feature enablement, unpruned operation, txindex and synchronization, v2 transport,
DD activation, wallet lock/key availability, and the existing client fee policy
and ledger. Readiness does not promise provider availability, liquidity, sufficient
wallet funds, or the additional Tor prerequisites of high privacy. A locked
wallet can still be inspected and can perform the existing unsigned preparation
steps that do not require input-control proofs.

The query reads configuration and local state. It does not run financial
reconciliation, contact a provider, reserve funds, or sign. It does not wait for
index synchronization. A changing tip can temporarily produce a not-ready
snapshot; retry the query.

## Preparation, explicit authorization, execution and resumption

1. Choose and durably save one canonical lowercase UUID `request_id` before the
   first call. Save the complete order, options, returned `session_id`, and
   `reserved_user_inputs`. Use the same ID after timeouts, response loss and
   restart. Do not automatically create a replacement ID.
2. Prepare using the same order and `prepare_only=true`. Preparation may reserve
   wallet inputs, request provider capacity/liquidity, and communicate with a
   provider. With an unlocked wallet it may sign an input-control proof. It does
   not authorize a new collaborative payment signature. It is not a read-only
   preview; `getpaymasteroffers` is the existing local offer preview.
3. Continue preparation until `authorization_required=true` and the exact
   `authorization_commitment` and fee split are available. Review recipient,
   provider, funding model, amount, service fee, total, expiry and commitment.
4. Remove `prepare_only` and repeat the order with the explicitly accepted
   `authorization_commitment`. A changed commitment requires another explicit
   acceptance. This stage may sign and automatically queue/submit the payment;
   there is no "sign now, execute later" mode. An RPC error or lost response
   after this boundary does not prove that no authorization exists.
5. Inspect with `getdigidollarsendsession {"request_id":"..."}` or
   `listdigidollarsendsessions`. `resolvepaymastersession` with action `refresh`
   provides the existing status/action envelope without new payment, signing or
   reservation operations. Its existing inbox handling can persist already-received
   provider equivocation evidence; use `getdigidollarsendsession` for a strictly
   read-only observation. Other actions can sign, transmit or recover; follow
   `allowed_actions` and the operator guide.
   Resume an authorized order using the same `senddigidollar` arguments and
   accepted commitment. Never treat a transport error as permission for a new
   payment.

Example preparation parameters (replace the address and UUID):

```json
["<DD recipient>", 500, "", 0, null, "cents", {
  "fee_mode": "paymaster",
  "request_id": "550e8400-e29b-41d4-a716-446655440001",
  "maximum_paymaster_fee_cents": 100,
  "privacy": "standard",
  "selection": "lowest_total_cost",
  "maximum_provider_attempts": 1,
  "prepare_only": true
}]
```

Parallel identical retries join the durable order. Materially different orders
using its ID are rejected. Terminal high-level retries check the same existing
canonical order hash as `requestpaymasterquote`, including recipient, amount,
fee cap, input set and selection/privacy settings. That hash and its byte format
are unchanged. After session pruning the original input set is no longer stored:
identity-only status reads still work, but a send replay without the saved
`selected_inputs` fails with `PAYMASTER_SESSION_DETAILS_UNAVAILABLE`. Supply the
original ordered input set to verify a replay, or use the status RPC. The
existing sweep option forbids explicitly selected inputs; inspect a pruned sweep
with the status RPC. Neither case creates another payment or reconstructs proof.

## Payment status and local observations

`final` only means that the persisted session state is terminal. It also applies
to failures, conflicts and safe cancellation. The shared Paymaster result fields
are used by `senddigidollar`, session queries and Qt:

| `status` | Meaning |
| --- | --- |
| `success` | A validated recipient payment has positive local confirmation depth |
| `canceled` | Safe cancellation state or an observed confirmed recovery |
| `failed` | Failed session |
| `conflicted` | Conflicted session or locally conflicted recipient transaction |
| `pending` | No confirmed recipient payment or other conclusive outcome |

Conflicted/failed session states never become success. A confirmed recovery never
becomes recipient-payment success, including when it conflicts with the original
payment. `payment_confirmed` is the explicit positive recipient-payment flag.
Mempool/stempool presence and a provider's signed final result are insufficient.
The direct DGB-funded send RPC retains its existing broadcast-success meaning;
this stricter confirmation contract applies to Paymaster session responses.

When local store context is available, `payment_view` contains:

- `observed_tip_hash` and `observed_tip_height`: the wallet's processed chain tip.
- Separate `payment` and `recovery` objects with `details_available`,
  `observation_available`, and the known `txid`, if any.
- `payment.recipient`: `vout`, `script_pub_key`, and `amount_cents`, only after
  exact final-artifact and authorization validation. The existing quote builder
  places the recipient at output index 0; that output's script and DD amount
  must match. Equal-valued change to the same address remains a separate output.
  The provider's fee output is not the recipient output.
- `confirmations` and `in_mempool` only when the exact transaction, including
  witness bytes, is observed in the wallet. Negative depth means a conflict.
- `block_hash` and `block_height` only for a locally confirmed transaction.
- An optional `error` when artifact reading/validation fails. No positive payment
  claim survives a failed validation.

The view reads existing attempts, manifests, recovery artifacts and wallet
transactions. It does not reconcile finances or persist a second payment state.
Read queries may wait for already queued wallet chain notifications. A reorg can
reduce depth to zero and change `success` back to `pending`, even before the stored
session state changes. A known txid without retained artifacts or a missing wallet
observation is not a receipt. After pruning, details are explicitly unavailable,
`payment_confirmed=false`, and even a `CONFIRMED` tombstone can return `final=true`
with `status="pending"`. Query and archive what the integration needs before the
existing 240-block retention boundary; do not assume unlimited receipt storage.
The RPC view is local evidence, not a signed, transferable proof.

## Errors and existing fee protection

Inspect JSON-RPC errors before reading result fields. Relevant identifiers include
`PAYMASTER_REQUEST_ID_CONFLICT`, `PAYMASTER_PERSISTED_CLIENT_ORDER_CONFLICT`,
`PAYMASTER_PERSISTED_INPUT_CONFLICT`, `PAYMASTER_AUTHORIZATION_COMMITMENT_MISMATCH`,
`PAYMASTER_SESSION_DETAILS_UNAVAILABLE`, `PAYMASTER_SESSION_READ_FAILED`,
`PAYMASTER_UNSUPPORTED_PERSISTED_VERSION`, `PAYMASTER_CLIENT_FEE_LIMIT_EXCEEDED`,
and `PAYMASTER_CLIENT_DAILY_FEE_LIMIT_EXCEEDED`. `getpaymasterclientinfo` lists local
readiness blockers such as `PAYMASTER_WALLET_LOCKED` and
`PAYMASTER_REQUIRES_READY_TXINDEX`. More specific artifact errors may appear in
`payment_view.error`. Unknown errors must not cause a new UUID or an inferred
payment success; inspect the existing session first. Standard invalid-parameter,
wallet and insufficient-funds RPC codes remain unchanged.

The existing service-fee daily limit counts all open reservations regardless of
age, plus spent fees in its rolling 24-hour window. Only spent entries age out of
that sum; released entries do not count. The existing accounting high-water time
prevents clock rollback from reopening a spent window. This is service-fee
protection, not a limit on recipient payments or a total agent spending budget.

## Future x402 adapter boundary

The [x402 v2 core specification](https://github.com/x402-foundation/x402/blob/main/specs/x402-specification-v2.md)
places client budget management outside its scope and specifies read-only
verification. A reserving Paymaster preparation must not implement `/verify`.

A later adapter for the [exact scheme](https://github.com/x402-foundation/x402/blob/main/specs/schemes/exact/scheme_exact.md)
using a payment already submitted by this wallet needs invoice/requirements
binding, atomic single use of the payment, and a confirmation/reorg policy in the
adapter or facilitator. A txid, amount and recipient alone do not enforce single
use across external invoices. Network/asset identifiers and interoperability need
a separate network-specific design. This is an integration boundary, not a claim
that these RPCs are already x402 compatible. No speculative `invoice_id`, x402
hash, or adapter state is added to payment manifests. The 100-cent recipient
minimum remains a boundary for external services.

## Versioning and validation

`integration_version` versions this RPC contract. Additive fields do not change
it; clients should ignore unknown fields and handle unknown enum/error values
conservatively. Changes to existing field meanings require a contract-version
change and release notes. Wire protocol V5 and each persistent record's own
version are independent. This package introduces RPC version 1; it does not bump
wire or record versions. Frozen `PaymentSession` v4 and `IdempotencyTombstone` v2
byte vectors in `paymaster_wallet_store_tests` pin the baseline encoding for later
compatibility checks. Preserve those vectors when releasing; a new version must
add fixtures and an explicit compatibility decision instead of replacing them.

Review in two packages: (1) outcome/Qt and fee-accounting corrections;
(2) capability RPC, observation schemas, retry contract and documentation.
Targeted regressions extend `paymaster_provider_tests`,
`paymaster_wallet_store_tests`, `PaymasterWidgetTests`,
`wallet_paymaster_rpc.py`, `wallet_paymaster_provider.py` and
`wallet_paymaster_reorg.py`. Existing lifecycle/failover coverage remains required.
The RPC scenario includes a zero-DGB client, explicit preparation/acceptance,
lost responses, parallel retries and wallet/node restart without new orders.

Local verification on 2026-09-23: the changed C++ translation units passed MSVC
syntax checks, the three changed functional Python files passed parsing, and
`git diff --check` passed. An isolated test executable compiled the affected
provider accounting, wallet observation/shared RPC and test objects (plus the
current session-store dependency), linking existing dependency libraries. The
three client-fee tests passed 47 assertions, the two session-observation tests
passed 83, and the frozen-format test passed 14: six tests / 144 assertions.
This is focused evidence, not a clean complete application build. The expanded
functional and Qt runtime scenarios have not been executed for this package.

Full builds and release acceptance remain operator work. In
`D:\Digibyte\digibyte-fork`, with the dependencies and Qt configuration from
[build_msvc/README.md](../build_msvc/README.md), the installed VS toolchain can be
used as follows (build: several minutes or longer; functional group: potentially
many minutes; timing depends on the machine):

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build_msvc\digibyte.sln /p:Configuration=Release /p:Platform=x64 /p:VcpkgManifestInstall=false /m:2 /v:minimal
.\src\test_digibyte.exe --run_test=paymaster_provider_tests,paymaster_wallet_store_tests --log_level=test_suite
$env:DIGIBYTE_QT_TEST_SUITE = 'PaymasterWidgetTests'
.\src\test_digibyte-qt.exe
Remove-Item Env:DIGIBYTE_QT_TEST_SUITE
python test\functional\test_runner.py wallet_paymaster_rpc.py wallet_paymaster_provider.py wallet_paymaster_lifecycle.py wallet_paymaster_reorg.py wallet_paymaster_failover.py --jobs=1
```

Use an interactive desktop for the Qt suite as required by the existing Windows
test guidance. Success means exit code zero, no Boost/Qt failures, all selected
functional tests passing (including `-rpcdoccheck`), and no regression in existing
release checks. Do not infer current-code test success from an older executable.
Independent review and a real Tor run remain open release gates; see the
[release gate](digidollar-paymaster-release-gate.md).
