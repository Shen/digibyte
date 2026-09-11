# DigiDollar Paymaster design specification

**Source:** `feature/digidollar-paymaster-v1` at `bd270044c1`.
**Reconciled:** 2026-09-11. **Feature milestone:** V1. **Wire protocol:** V5.

This document replaces the earlier concept draft with the design implemented
in the inspected branch. The filename is retained for existing references and
the 28 acceptance-criterion identifiers are preserved. It describes the
current feature, not an unimplemented proposal. It does not claim release
approval or independent security certification.

Read [PAYMASTER.md](PAYMASTER.md) for context and review order, the
[implementation reference](doc/digidollar-paymaster-implementation.md) for
protocol/state details and source locations, the
[implementation plan](DIGIDOLLAR_PAYMASTER_PLAN.md) for work packages, and the
[release gate](doc/digidollar-paymaster-release-gate.md) for verification status.

## 1. Purpose and scope

A client holding confirmed DigiDollar but no DGB can transfer DD by asking a
provider to contribute the DGB miner-fee inputs. The client and provider
authorize and sign only their respective inputs in one ordinary
`DD_TX_TRANSFER`. The provider signs last, commits the final transaction
durably, and broadcasts it.

The funding models are user-paid percentage fees, public zero-fee sponsorship,
and restricted zero-fee sponsorship with a signed descriptor and payment-bound
capability. There is no privileged provider roster, new transaction type,
special sighash, DD output exemption, prepaid client balance at the provider,
or transfer of user private keys.

Client/provider operation requires DigiDollar activation, an unpruned node,
synchronized transaction indexing and BIP324 v2. One session has one recipient.
Paymaster mint/redeem sponsorship and multi-recipient batching are outside this
implementation. Existing five-argument `senddigidollar` calls retain direct
wallet-funded DGB fees.

## 2. Architecture decisions

| Decision | Rationale and current consequence |
|---|---|
| One normal DD transfer | Existing validators check the final transaction; the provider has no consensus privilege. |
| Dedicated deterministic builder | All inputs/outputs are fixed before signing, including explicit wallet-owned change and carrier returns. |
| DD-capable PSBT with roles | Each wallet signs only locally authorized inputs; unknown fields/roles and non-default sighashes fail closed. |
| Public admission, private operational exchange | Announcements prove separate admission liquidity. Only the selected provider/client exchange reveals operational payment resources. |
| Mandatory V5 Capacity | Provider identity and resources are authenticated before client payment details are disclosed. |
| Atomic wallet state | Sessions, attempts, reservations, authorizations and exact commits survive lost messages/restarts without fresh authority. |
| Separate authority and chain observation | A final commit, mempool acceptance, confirmation and reorganization have distinct meanings. |
| Shared RPC/Qt authority | The GUI uses the same wallet checks, snapshots and allowed actions. |
| Finite local budgets | Client fees, provider losses, concurrent reservations and maintenance spending are bounded explicitly. |
| Current-only experimental formats | Old Paymaster protocol/record versions fail closed without implicit migration or downgrade. |

These are Paymaster design decisions. The broader branch also changes DCA,
ERR and volatility source to use portable 128-bit arithmetic. Review their
equivalence independently; absence of a new Paymaster consensus rule does not
mean every consensus source file is unchanged.

## 3. Transaction and fee contract

DD uses integer cents; DGB uses integer satoshis. User-paid fees use checked
integer arithmetic, rounded up to one cent:

```text
fee_cents = ceil(recipient_cents * fee_rate_bps / 10000)
user_DD_in = recipient_DD + user_DD_change + service_fee_DD
provider_DGB_in = provider_DGB_change + miner_fee_DGB
```

Every DD output remains between 100 and 10,000,000 cents and total DD is
conserved. A 1-to-99-cent fee uses a confirmed provider-owned DD carrier whose
replacement output returns principal plus fee. A fee of at least 100 cents can
use a normal provider-fee output. Zero-fee sponsorship needs no unused carrier.
See the [worked carrier example](doc/digidollar-paymaster-implementation.md#why-a-carrier-is-needed).

Creating transactions, outpoints, scripts and values are checked locally.
Client change belongs to the client; carrier/DGB returns belong to the provider.
Exact-outflow options support fee deduction from a fixed gross DD amount and
all ordinary confirmed spendable DD. Core rejects cent-rounding gaps, excludes
reserved/provider-pool inputs and rejects a changed sweep snapshot before
committing reservations.

## 4. Discovery and signing contract

`sendpmasters`, `getpmasters` and `pmannounce` operate on upgraded ordinary peers
after DigiDollar activation. Eligibility requires identity/admission checks
and disjoint admission/operational resources. Isolated directional Paymaster
connections carry bounded Capacity, quote, submit, result and recovery messages.

Before disclosure of DD inputs, intent or restricted capability, V5 Capacity
authenticates the network, provider, request/session, nonce, reference block,
expiry and exact operational resources. The quote must use the validated
resource set. Both wallets persist their local authorization manifest and
independently reconstruct the full transaction immediately before signing.

The high-level Paymaster call first prepares an exact authorization. The
caller must return its `authorization_commitment` with the same request before
Core can create a new user signature; `prepare_only=false` does not bypass this
guard. Headless clients implement the same explicit acceptance step as Qt.

The client signs only its DD inputs; the provider signs only its own inputs.
Provider policy and budget reservations must match local signing authority.
One exact final raw transaction and related authority/accounting are committed
before broadcast. Client result processing checks all transaction/witness
bytes against the trusted template and verifies signatures with actual prevouts.

Standard direct transport requires v2 without v1 fallback. High privacy adds
onion transport, stream isolation, one provider attempt and no clearnet fallback,
plus capture/logging readiness guards. The provider still sees its co-signed
transaction and confirmed transactions remain public.

## 5. State, recovery and operations contract

`request_id` is the durable idempotency key, bound to a canonical request hash.
Each attempt binds one provider, offer, capacity, quote and template. Exact
retries cannot create replacement authority or reconstruct missing budget rows.

Ordinary `fallback` is allowed only before a user PSBT exists.
`abandon_unsigned` requires that no transaction authorization can exist. After
signing, timeout never releases inputs: use inspection, `retry_same`, or
explicitly authorized `cancel_to_self`. A distinct eligible user-paid provider
can supply recovery DGB. Recovery uses the original client inputs, fresh
client-owned DD returns and only the bounded recovery service fee. Cancellation
is final after confirmation, not after mempool acceptance.

Provider start, automatic servicing, optional autostart and paid maintenance
have separate settings/authorizations. Pool successors, maintenance, budgets
and finance reconcile through confirmation, conflict, restart and reorganization.
Pending successors are not confirmed usable liquidity.

Durable records can contain metadata and signed artifacts without field-level
encryption. Eligible completed detail is compacted after the safety depth into
idempotency tombstones. Ambiguous, unconfirmed or unreadable authority is not
automatically erased.

## 6. V1 acceptance criteria

These requirements preserve the IDs in the
[release matrix](doc/digidollar-paymaster-release-gate.md). A requirement or
test being present does not establish a current release pass.

| ID | Required behavior |
|---:|---|
| 1 | A client without DGB completes a confirmed DD payment using provider DGB inputs. |
| 2 | Neither party transfers private keys or seed. |
| 3 | Automatic eligibility requires at least three disjoint confirmed admission DGB slots and, for user-paid offers, admission carriers. Operational resources are separate and not gossiped. |
| 4 | Fees from 0.01 through 0.99 DD use valid carriers without sub-1-DD outputs. |
| 5 | Percentage fees and exact total charges are calculated and ordered locally with bounded integer arithmetic. |
| 6 | Each wallet signs only its own locally authorized inputs. |
| 7 | A signed attempt rejects changes to recipient, amount, fee, change, inputs or template. Ordinary fallback is pre-user-PSBT only and preserves client DD inputs. |
| 8 | Provider failure cannot create duplicate confirmed payment through fresh-input retry; signed ambiguity retains original inputs and permitted recovery actions. |
| 9 | Existing DD validators validate the ordinary final transaction without Paymaster configuration. |
| 10 | The Paymaster protocol adds no consensus parameter, script rule, transaction type or DD conservation rule. Review separate arithmetic portability changes for equivalence. |
| 11 | Local reputation distinguishes neutral failures and never overrides identity, liquidity or authorization checks. |
| 12 | Headless selection uses durable sessions and hard DD fee caps, requires explicit acceptance of the prepared authorization commitment before signing, and does not create a second payment on identical request retry. |
| 13 | Existing five-argument `senddigidollar` calls retain the wallet-funded DGB path. |
| 14 | Public/restricted sponsorship use the common transaction protocol, charge zero DD service fee and require no prepaid provider account. |
| 15 | Restricted capabilities are payment-bound, not gossiped and not persisted as plaintext secrets; required cryptographic bindings and consumption state are retained. |
| 16 | Change uses explicit fresh internal destinations; production Paymaster logging does not expose payment artifacts or restricted secrets. |
| 17 | High privacy enforces onion transport, stream isolation, one attempt and no silent clearnet/v1 fallback. |
| 18 | Only the selected provider receives the necessary payment intent and discloses the operational resource set for that attempt. Local fee/privacy tolerances are not globally broadcast. |
| 19 | Eligible completed detail is reduced after the safety depth while permanent idempotency and conflicting-request protection remain. |
| 20 | Documentation and UI distinguish transport privacy/pseudonymity from anonymity against the provider or public chain. |
| 21 | Signed ambiguity exposes safe inspection, exact retry and same-input self-return. Ordinary fallback is pre-signature, timeout does not free signed inputs and cancellation requires confirmation. |
| 22 | The provider commits the exact final transaction and related authority/accounting before broadcast and restores that same commit on recovery. |
| 23 | A crash after client signing resumes the persisted exact PSBT without a new quote, reservation, authorization, signature or attempt. |
| 24 | One recipient is accepted per session; every DD output, including carrier and recovery returns, respects the 100-to-10,000,000-cent bounds. |
| 25 | Client/provider readiness requires unpruned operation, synchronized txindex and complete creating-transaction data. Existing validators need no Paymaster feature; only upgraded peers relay discovery. |
| 26 | Signed result authority is monotonic; final commitment binds exact raw bytes. Related wallet records, indexes and reservation/consumption transitions commit atomically. |
| 27 | Mempool/stempool is not confirmation; cancellation remains pending until confirmed. RPC/Qt share authoritative session, phase, broadcast and confirmation state; local chain reconciliation handles reorgs. |
| 28 | Wallet locking pauses the same session without fallback. Provider policy enforces a positive absolute DGB fee ceiling and finite safety limits for active funding models. |

## 7. Hardening and changes from the initial design

The initial criteria are supplemented by mandatory V5 Capacity, independent
local manifests, full witness validation, finite provider/client budgets,
durable replay/equivocation handling and authorized independent-provider recovery.
These map to H1-H8 in the release gate and
[hardening plan](DIGIDOLLAR_PAYMASTER_HARDENING_PLAN.md).

The earlier concept's broader post-signature fallback is superseded by the
implemented pre-user-PSBT boundary. Current functionality also includes pool
successor reuse, paid maintenance, provider finance/backup workflows, exact
outflow and current-only persistence. The current source definitions, linked
from the implementation reference, define message bounds, record schemas,
RPC parameters and allowed transitions. No obsolete wire schema or implicit
migration is specified here.
