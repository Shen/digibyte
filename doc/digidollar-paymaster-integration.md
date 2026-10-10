# Paymaster client integration contract (version 1)

2026-09-27 status, source `1789c803be` on `integration/paymaster-v9.26.6rc2`:
[Direct connection capacity](digidollar-paymaster-connection-capacity.md#current-verification-status)
includes the default single-channel queue, dedicated provider listener,
DoS admission limits, recovery fixes and explicit transport retry. The updated
Windows build passed 284 selected unit tests (10,767 assertions) and all nine
focused functional tests. Qt, reference compatibility, real Tor/load and the
wider release matrix remain open. Wire, consensus and journal formats are unchanged.

This contract prepares existing Paymaster interfaces for external clients. It
was committed as `a4f17f6315`, on top of `46add7e4d3`, on
`integration/paymaster-v9.26.6rc2`. The original review was on 2026-09-23; transport/retry and test-status
updates track `1789c803be` on 2026-09-27.
Its interfaces support a possible x402 extension, without committing to its
implementation. It does not include an x402 adapter, an agent-wide spending
budget, another send RPC,
or deferred execution after signing. No consensus, Paymaster wire encoding,
wallet record encoding, or wallet feature flag is introduced by this package.

## Failed preparation and session lookup

A public announcement can remain cached for up to ten minutes after its
creation even if the provider pauses. Discovery does not prove availability.
After a failed or lost preparation reply, inspect the same request UUID on the
same wallet endpoint before preparing another payment. This applies equally to
Qt, HTTP RPC and `digibyte-cli`.

`getdigidollarsendsession` and `resolvepaymastersession` distinguish:

- `PAYMASTER_SESSION_NOT_FOUND: Paymaster session not found` (`-4`): a
  status-aware lookup found neither the requested session nor its retained
  completion record. Only a fresh prepare-only attempt with no previously
  observed session or authorization may return to composition on this result.
- `PAYMASTER_SESSION_READ_FAILED` (`-4`): a read, validation or session-index
  inconsistency; the payment state remains unknown.
- `PAYMASTER_UNSUPPORTED_PERSISTED_VERSION` (`-4`): an unreadable record version;
  preserve the request and wallet. It is never evidence of absence.

A known session disappearing is an error even if the lookup reports absence.
Older generic missing-session messages do not establish the new distinction.
Existing successful result schemas and RPC argument positions are unchanged.

`PAYMASTER_DD_INPUT_SELECTION_FAILED: ...` (`-6`) preserves the upstream DD
input/change error, including the provider's service fee in the target amount.
This send error alone does not prove that no session exists; always reconcile
its request UUID. The initial recipient amount can fit the displayed balance
while the total with the service fee does not.

For a found unsigned session, read `allowed_actions` and use `abandon_unsigned`
when permitted. It releases the local unsigned reservation without requiring
the provider to resume. Inspect the same request after an uncertain cancellation
reply. For signed or otherwise protected state, retain the same payment and
use only the offered retry/recovery actions; never create a replacement payment
merely because its provider is offline. Qt clears only a confirmed uncreated
local attempt or an authoritatively closed unsigned request, preserving the
recipient and amount for the user's next decision.

## Explicit transport retry

`senddigidollar` accepts the optional boolean `retry_transport` in its Paymaster
options, as does `requestpaymasterquote`. Set it only for an explicit retry of
a failed connection, keeping the same request ID, payment options and (when
already accepted) authorization commitment. It resets local transport failure
state without extending signed deadlines, changing inputs or granting signing
authority. It is rejected with `fee_mode=dgb`. Recovery uses the same option on
`resolvepaymastersession(..., "cancel_to_self", options)`.

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
`funding_models`, and `sponsorship_scopes`. Available scopes are currently
`["public"]`. New Restricted policies, descriptor creation, requests and first
authorization are disabled with `PAYMASTER_RESTRICTED_SPONSORSHIP_DISABLED`.
Legacy artifacts remain readable for already accepted transfers and recovery;
their presence does not mean new Restricted service is available.

`supported` describes the binary's integration support. `ready` and
`readiness_errors` describe the wallet/node's local authorization prerequisites:
feature enablement, unpruned operation, txindex and synchronization, v2 transport,
DD activation, wallet lock/key availability, and the existing client fee policy
and ledger. Readiness does not promise provider availability, liquidity, sufficient
wallet funds, or the additional Tor prerequisites of high privacy. This is a
general local check, not an exhaustive eligibility check for every wallet/signing
mode; the operation still validates its own prerequisites. A locked
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
3. `prepare_only=true` and `authorization_commitment` are mutually exclusive.
   Continue preparation until `authorization_required=true` and the exact
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

### RPC effects and response shapes

| RPC | Use and possible effects |
| --- | --- |
| `getpaymasterclientinfo` | Read capabilities and local readiness; no synchronization wait, reservation, signature, provider traffic or finance reconciliation. |
| `getpaymasteroffers` | Inspect locally known offers filtered by the requested fee ceiling, wallet fee limits, remaining rolling-day budget and privacy profile; not an authorization, reservation or availability guarantee. |
| `senddigidollar` in Paymaster mode | Prepare, explicitly authorize, advance or resume the same durable order; may reserve, communicate, sign and submit according to the authorization stage. |
| `getdigidollarsendsession` | Read one wallet-scoped session by an object containing `request_id` or `session_id`; no new payment operation. |
| `listdigidollarsendsessions` | Read a bounded session page; follow its cursor and use `active_only=false` when completed sessions are needed. |
| `resolvepaymastersession` | Read/advance the existing action envelope; `refresh` can persist received equivocation evidence, other allowed actions can mutate or communicate. |

The high-level Paymaster result and session-shaped responses include `status`,
`final`, `payment_confirmed` and, when context is available, `payment_view`.
This is not a promise that every low-level quote/PSBT/submit RPC returns that
shape. Use the high-level entry point plus session inspection for integrations;
consult `help <method>` from the same build for each low-level schema.

`getpaymasteroffers(amount_cents, options)` accepts the same
`maximum_paymaster_fee_cents` and `privacy` values as preparation, plus
`subtract_paymaster_fee_from_amount`. Core further limits the preview fee by
the wallet's per-transaction ceiling and remaining rolling-day budget. The
`high` privacy profile excludes non-Tor endpoints. A later preparation still
rechecks the policy and provider state; an empty preview is not proof that no
provider could become eligible after local state changes.

Offer objects additionally contain the boolean `recommendation_deprioritized`.
It is true when wallet-local provider failure or availability observations exist
without a known successful payment, including below the general reliability
score's five-sample threshold. Eligible alternatives sort ahead of these offers;
existing cooldowns still filter candidates. Signed announcements alone do not
prove availability. Unsigned contact failures are recorded idempotently per
attempt. Authenticated `REJECTED`/`SLOT_UNAVAILABLE` results also record a failure
while signed attempts retain their protected ambiguous/recovery state. A later
locally confirmed recipient payment can atomically replace its failed/advisory
outcome marker with success; duplicate observations do not add successes and
success cannot be downgraded. The latest failure count is corrected when it can
be identified; after intervening observations or cleared history, historical
counts are retained conservatively. A known success removes initial-failure
recommendation demotion independently of those counters. Local wallet
locking, user cancellation and local connection limits are not provider failures.

`senddigidollar` public Paymaster options accept the optional paired hex IDs
`preferred_provider_id` and `preferred_offer_id` from the offer preview. Both
must be nonzero and supplied together; own-DGB mode and restricted sponsorship
reject this public choice. A new request tries that exact first provider/offer
or returns `PAYMASTER_SELECTED_OFFER_UNAVAILABLE` before creating an attempt.
A nonterminal repeat cannot change its durable first-attempt binding
(`PAYMASTER_REQUEST_ID_CONFLICT`). Explicit subsequent fallback can select a
different provider but still requires the existing fresh exact quote approval.
Terminal replays retain canonical-order checks and return the original result
after detail pruning. These preferences introduce no canonical-request-hash or
wallet record format change and grant no signing authority. CLI sends them in
the existing JSON options object; no CLI conversion change is needed.

RPC access uses the existing node authentication and authorization model. A
wallet URL selects context, not a separate security tenant. Do not expose spending
credentials to an external resource server or facilitator; see the repository's
[RPC interface guidance](JSON-RPC-interface.md#security).

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

After cancel-to-self, the original fee stays reserved until the recovery has
240 confirmations, matching signed-input retention. A shallow recovery or its
reorganization must not reopen that fee allowance. Retained sessions from older
versions repair early-released or aged-out rows from the exact accepted signed
attempt before a new payment/recovery approval or final reconciliation. Saved
limits stay unchanged; a restored obligation above the cap blocks new approvals.
This wallet-store rule is shared by GUI, RPC and CLI.

## Interface support for a possible x402 extension

The specifications below were checked on 2026-09-23. They are external, evolving
documents; any x402 adapter would need to review them at its own pinned revision.

The [x402 v2 core specification](https://github.com/x402-foundation/x402/blob/main/specs/x402-specification-v2.md)
places client budget management outside its scope and specifies read-only
verification. A reserving Paymaster preparation must not implement `/verify`.

An optional adapter for the [exact scheme](https://github.com/x402-foundation/x402/blob/main/specs/schemes/exact/scheme_exact.md)
using a payment already submitted by this wallet needs invoice/requirements
binding, atomic single use of the payment, and a confirmation/reorg policy in the
adapter or facilitator. A txid, amount and recipient alone do not enforce single
use across external invoices. Network/asset identifiers and interoperability need
a separate network-specific design. This is an integration boundary, not a claim
that these RPCs are already x402 compatible. No speculative `invoice_id`, x402
hash, or adapter state is added to payment manifests. The 100-cent recipient
minimum remains a boundary for external services.

Under the cited `exact` specification, client-submitted proofs use `upfront`
settlement before resource execution. Any such binding would also need to define proof
age/retention, non-exact-amount handling and failure disposition. Until its
finality condition is met, settlement must not consume the proof or deliver the
resource. These requirements describe the supported extension boundary, not a
planned adapter or implemented wallet features.

## Versioning and validation

`integration_version` versions this RPC contract. Additive fields do not change
it; clients should ignore unknown fields and handle unknown enum/error values
conservatively. Changes to existing field meanings require a contract-version
change and release notes. Wire protocol V6 and each persistent record's own
version are independent. This package introduces RPC version 1; V6 adds the
signed user-paid service-fee cap to public offers. Frozen `PaymentSession` v4
and `IdempotencyTombstone` v2
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

Full builds and release acceptance remain operator work. The shared
[build and test runbook](digidollar-paymaster-testing.md) provides the Windows
and Linux/WSL commands, prerequisites, expected runtime and success criteria.
It includes the current Qt/vcpkg paths instead of relying on implicit defaults.
Independent review and real Tor remain open in the
[release gate](digidollar-paymaster-release-gate.md).

### Effective client service-fee ceiling

`setpaymasterclientsafetypolicy` accepts optional `maximum_service_fee_bps`
(integer 0..10000, 100 = 1%). It is enforced together with the per-transfer and
rolling-day DD ceilings. Example policy:

```json
{"maximum_service_fee_per_transaction_cents":100,"maximum_service_fee_per_day_cents":1000,"maximum_service_fee_bps":100}
```

Omission preserves a configured percentage; absent output on a legacy policy
means no percentage has yet been approved. Zero means zero-fee only. Fractional,
negative, null and out-of-range values are rejected. GUI, CLI and RPC use the
same wallet enforcement. `getpaymasteroffers` filters the actual rounded fee
against the actual recipient amount, including net-of-fee requests. Recheck
errors use `PAYMASTER_CLIENT_PERCENTAGE_FEE_LIMIT_EXCEEDED`; a previously viewed
quote does not override a subsequently tightened cap. The final signature gate
checks again. Alternative recovery applies the percentage to its wallet returns.

Continue using `resolvepaymastersession(..., "refresh")` to observe an ambiguous
signed transfer, including after reopening the wallet. Read `allowed_actions`
for safe unsigned closure or return-to-self preparation. Never infer permission
to send a replacement, sign recovery or release inputs from a timeout. Qt's
live continuation switches to this read-only path after two minutes; CLI callers
control their own polling interval.

### Provider backup restore quarantine

The explicit wallet restore API marks a copied provider database before loading
or service callbacks. `getpaymasterinfo` reports
`PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED` and no available funding models;
the descriptor wallet remains structurally eligible and its known payment
records stay readable. Start, new provider signatures and reserve-spending
actions reject that gate in RPC/CLI; Qt explains it as missing financial history.
Already recorded exact signed finals retain read-only recovery. Client-only
backup restoration is unchanged. Do not retry configuration changes or backup
acknowledgements as a way to resume: there is no guard-clear RPC or automatic
reconstruction of omitted signatures. Manually replacing an old wallet/data
directory bypasses this restore hook and needs an independent monotonic
checkpoint. See [the threat model](paymaster-threat-model.md).
