# Paymaster client integration contract

Integration version is 1. Use authenticated local wallet RPC and inspect
`getpaymasterclientinfo` from the selected build. There is one DD recipient;
this document does not establish an HTTP adapter or batch-payment contract.

## Entry point and units

```text
senddigidollar address amount comment fee_rate selected_inputs amount_unit options
```

Unit is argument 5 and options argument 6, zero-based. Prefer integer cents
with `amount_unit="cents"`. Explicit dollars are supported; ambiguous decimal
requests without a unit are rejected. One DD is 100 cents. Recipient bounds
are 100..10,000,000 cents; user total is at most 10,000,000 cents.

For an invoice, leave fee deduction/sweep disabled: `payment_cents` is received
by the recipient, `service_fee_cents` is additional, and `user_total_cents` is
the sum. `subtract_paymaster_fee_from_amount` instead makes the entered amount
the exact total; unrepresentable rounded totals fail.
`send_all_spendable_dd` uses spendable inputs, excluding protected reserves.

## Preparation, explicit authorization, execution and resumption

1. Persist a canonical lowercase UUID `request_id` and complete order before
   calling. Retain returned `session_id` and ordered `reserved_user_inputs`.
2. Prepare with `prepare_only=true`. This may reserve inputs, contact providers
   and sign input-control proofs when unlocked. It is not a read-only preview
   and does not authorize the payment signature.
3. Continue the same order until `authorization_required=true` and the exact
   commitment is available. Review recipient, provider, model, amount, fee,
   total and expiry.
4. Repeat without `prepare_only`, supplying the explicitly accepted
   `authorization_commitment`. The two options are mutually exclusive. This
   may sign and automatically submit; there is no deferred-execution promise.
   Changed terms require another explicit acceptance.
5. On response loss, inspect the existing session. Never automatically allocate
   a new UUID or assume an RPC error means nothing was signed.

Example JSON-RPC preparation parameters (replace recipient and UUID):

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

Identical concurrent retries join the durable order; material changes using
its ID fail. After detailed session pruning, replay needs the original ordered
inputs or fails with `PAYMASTER_SESSION_DETAILS_UNAVAILABLE`. Identity status
reads remain possible. Sweeps forbid explicit selected inputs; inspect pruned
sweeps through status rather than reconstructing a send.

## Worked request and authorization sequence

The preparation array above is the `params` value for `senddigidollar` at the
selected wallet endpoint, not a shell command. Replace its address and generate
a fresh UUID once for a genuinely new order. All subsequent calls for that
order keep the same UUID and material arguments. The illustrated amount is
500 cents = 5.00 DD; the 100-cent fee ceiling is a limit, not a price quote.

When preparation returns an exact commitment, store the result and compare its
recipient/provider/model/amount/fee/total with the intended order. Ask for
explicit acceptance of those exact terms. Only after acceptance send the same
parameters with `prepare_only` removed and the returned commitment inserted:

```json
["<same DD recipient>", 500, "", 0, null, "cents", {
  "fee_mode": "paymaster",
  "request_id": "550e8400-e29b-41d4-a716-446655440001",
  "maximum_paymaster_fee_cents": 100,
  "privacy": "standard",
  "selection": "lowest_total_cost",
  "maximum_provider_attempts": 1,
  "authorization_commitment": "<exact accepted commitment returned by Core>"
}]
```

Do not manufacture the placeholder commitment or recompute it from displayed
rounded amounts. This call can sign and submit. If the application crashes or
loses its response, read the same order first:

```json
{"method":"getdigidollarsendsession","params":[{"request_id":"550e8400-e29b-41d4-a716-446655440001"}]}
```

These method/params objects omit the transport envelope/authentication for
clarity; use your RPC client's normal JSON-RPC version and request ID. Wallet
selection and authentication belong to the transport, not the payment options.

Persist the order before RPC, and persist accepted terms/commitment before the
authorizing call. Keep request/session IDs, original material options, exact
amounts, ordered reserved inputs and known transaction IDs. Read/modify this
application record atomically so concurrent workers cannot create replacement
orders for the same invoice. This application responsibility does not replace
Core's own durable idempotency and authorization checks.

## Response handling for applications

The table describes the normalized send/session view, not every low-level RPC
response. Do not deserialize a quote or a recovery envelope as a send result.
`getdigidollarsendsession` returns the session object; `resolvepaymastersession`
returns an action envelope containing `session` and current permitted actions.

| Observation | Application behavior |
|---|---|
| Local readiness false / wallet locked | Show the actual prerequisite and retain order identity. Readiness failure is not payment failure or new-request permission. |
| Preparation without an exact commitment | Continue the same supported preparation flow; do not show a signable approval based only on an announcement. |
| `authorization_required=true` with exact terms | Request explicit acceptance; do not treat a previously accepted fee ceiling as acceptance of the commitment. |
| `status=pending`, or a txid/mempool observation | Track the existing order and known transaction. This is not confirmed recipient payment. |
| `status=success`, `payment_confirmed=true` | Verify the returned order/recipient/amount binding and local confirmation evidence. Apply any application-required additional confirmation policy separately. |
| `confirmation_state=recovery_confirmed` | Funds recovered to the wallet, not payment to the recipient; do not mark an invoice paid. |
| `final=true` with canceled/failed/conflicted status | Terminal order, not success. Inspect the financial result before deciding what the user should do next. |
| Read failure, unsupported persisted version or unexpectedly missing known order | Treat outcome as unknown. Preserve IDs/evidence and investigate; do not release application obligations or send a new payment automatically. |

Confirmation can change on reorg. Re-read the current view instead of treating
a previously observed success string as irrevocable settlement. Missing optional
fields are unavailable evidence, not zero-valued amounts or a free service fee.

Core derives `allowed_actions`; a frontend may remove an action but cannot add
financial authority. Use only actions documented for the method being called.
In particular, a `resume` capability means resume the existing high-level send
flow; it is not a `resolvepaymastersession` action string. That RPC accepts
`refresh`, `retry_same`, `fallback`, `abandon_unsigned` and `cancel_to_self` under
the current guards. State may change after inspection, so handle rejection of
an action that was previously allowed.

Recovery approval uses its own `recovery_authorization_commitment` and exact
recovery terms. A payment commitment cannot authorize self-recovery, and a
recovery confirmation cannot be substituted for recipient-payment evidence.

## Status and recovery

| RPC | Contract |
|---|---|
| `getpaymasterclientinfo` | Local capability/readiness snapshot; no provider traffic, finance reconciliation, reservation, signature or index wait. |
| `getpaymasteroffers` | Local directory preview; no reservation, authorization or availability guarantee. |
| `getdigidollarsendsession {"request_id":"..."}` | Read the existing order; `session_id` can also identify it. |
| `listdigidollarsendsessions` | Bounded pages; follow the cursor, use `active_only=false` for completed history. |
| `resolvepaymastersession` | Follow `allowed_actions`. `refresh` can persist received evidence, but grants no new payment authority. Other actions can mutate, communicate or sign. |

High-level send/session responses include `status`, `final`,
`payment_confirmed` and, when available, `payment_view`. `final=true` alone
is not successful recipient payment: cancellation/recovery can also be final.
Require local confirmation of the intended payment. Low-level quote/PSBT/submit
responses need not use this envelope; consult same-build RPC help.

`retry_transport=true` explicitly retries transport while preserving order,
commitment, inputs and deadlines. It is accepted by Paymaster send/quote and
supported recovery actions, rejected in own-DGB mode. It grants no new signing
authority and does not extend signed deadlines.

Ordinary provider fallback is allowed only before a user PSBT exists. Signed
work retains the original inputs; exact retry cannot create another attempt.

Unsigned preparation can be abandoned only after Core confirms it is safe.
Qt waits for durable read-back; late quotes cannot revive approval. Lost replies,
unknown state, an unexpectedly missing known session or a signature from another
caller keep the transfer protected. Provider reservations may remain until
expiry after local unsigned closure. Signed/ambiguous work needs the supported
recovery flow, not UI reset. Cancellation does not interrupt canonical scans.

## Capability and readiness query

`supported` describes binary support. `ready` and `readiness_errors` describe
local prerequisites: feature enablement, unpruned storage, history/index
readiness, activation, transport, keys/unlock and client policy. They do not
promise funds, provider capacity or additional Tor prerequisites. A changing
tip can produce temporary not-ready snapshots. Locked wallets can perform
only unsigned steps that do not need input-control proofs.

Available sponsorship scope is public. New restricted policies, descriptors,
requests and first authorization fail with
`PAYMASTER_RESTRICTED_SPONSORSHIP_DISABLED`. Legacy records remain readable
for already accepted transfers/recovery.

## Offer selection and presentation

`getpaymasteroffers(amount_cents, options)` considers requested and wallet fee
limits, remaining rolling-day budget and privacy. High privacy excludes
non-Tor endpoints and requires the supported onion isolation/one-attempt path,
without silent clearnet or V1 downgrade. `include_fee_rejected=true` returns otherwise matching
fee-rejected entries with `eligible=false`, `fee_limit_reason` (`percentage`
or `absolute_or_daily`), `maximum_service_fee_bps` and
`available_service_fee_cents`. An unconfigured legacy percentage limit is -1.
This diagnostic does not bypass amount, privacy or announcement-validity checks.

`preferred_provider_id` and `preferred_offer_id` bind a public offer as a pair.
An unavailable selection fails with `PAYMASTER_SELECTED_OFFER_UNAVAILABLE`;
it is not silently replaced by different terms. Preparation/signing recheck
current policy and capacity.

`recommendation_deprioritized` reflects wallet-local failure without known
success; eligible alternatives rank ahead, and cooldowns still apply. Local
confirmed success updates reliability idempotently. Announcements/rankings
do not establish current capacity.

The send form pauses offer checks while DD balance is unknown or below the
entered amount. Unknown is not zero. Actual fee affordability is separate:
the entire maximum fee ceiling need not be held for a cheaper offer. Excluded
cards show their reason; counts, warnings and send eligibility must agree.
Own-DGB and automatic fallback retain their respective funding semantics.

Recommendation identifies the cheapest eligible offer within selection policy,
subject to local reliability handling. The countdown is announcement expiry,
not reserved capacity: displayed offers can become unavailable earlier. Exact
provider and fee are confirmed before authorization. Recipient/amount survive
safe unsigned cancellation.
