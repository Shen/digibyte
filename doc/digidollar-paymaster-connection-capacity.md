# Paymaster Direct connection capacity

Source: `src/paymaster/transport.h`, `manager.*`, `src/net.*`, `src/netbase.*`.
See [node configuration](digidollar-paymaster.md) and [threat model](paymaster-threat-model.md).

## Connection ownership and limits

Direct connections use a dedicated provider listener, separate from ordinary
P2P. Direct markers on an ordinary listener are rejected. Limits are shared
across wallets in one node, not multiplied by wallet count.

| Limit | Current value |
|---|---|
| Default / maximum outbound Direct | 1 / 4 |
| Maximum provider inbound Direct | 16 |
| Inbound handshake reserve | 8 |
| Handshake / idle timeout | 30 seconds |
| Connection lifetime | 120 seconds |
| First-request deadline | 10 seconds |
| Pending admission grace | 5 seconds |
| Per-network-group inbound allowance | 4 |

Provider operation reserves the requested inbound/handshake capacity plus at
least one ordinary inbound connection and outbound allowances. With 16 inbound
Direct slots, eight handshake slots and ordinary outbound target 19, the minimum
effective connection limit is 45 for one Direct outbound, or 48 for four. File-descriptor-adjusted effective limits matter, not
only the configured number. Default `maxconnections=125` leaves ordinary P2P
capacity; do not obtain it by silently shrinking provider safety allowances.

Connection-start buckets are bounded globally and per network group. Wallet
queues are fair and bounded; expected replies/recovery have protected routing
quotas. A pending oldest admission can be replaced under pressure after its
grace period; accepted work remains protected. Pings do not prolong useful-work
idle deadlines. These controls are not a Sybil or bandwidth-flood guarantee.

## Channel authentication

BIP324 encryption alone does not authenticate the advertised provider. With
`CAP_CHANNEL_AUTH`, the provider signature binds its identity to the local
BIP324 session, genesis and fresh nonce. Verify before disclosing capacity,
payment or recovery metadata. A relay cannot substitute another encrypted leg
while reusing the authenticated binding.

Proof requests use a separate bounded queue (eight fixed challenges), one
challenge per socket, with expiry/disconnect cleanup. Networking does not cache
wallet signing keys. Capacity proof precedes full payment-intent processing.

## Deployment and retry

Advertise an endpoint reachable by the intended clients. Local readiness and
external reachability are different measurements. Use a separate onion service
for Direct where required; high privacy must not silently become clearnet.
A configuration preview/apply changes files, not the running listener.

Explicit `retry_transport` retries a retained request without changing financial
authority, input set or deadlines. Recovery releases the original payment lease
and uses its supported channel; expected capacity proofs are accepted on the
applicable payment/recovery paths. A transport failure does not cancel a signed
payment or imply permission to choose a new request ID.

## Verification

Unit coverage: `paymaster_transport_tests`, `paymaster_admission_tests`,
`paymaster_wire_tests`, `paymaster_protocol_tests`. Functional coverage:
`p2p_paymaster_connection_capacity.py`, `p2p_paymaster.py`, and wallet failover/
recovery tests. Real Tor, hostile TCP relay, prolonged saturation and current
sanitizer/load checks remain separate release gates. See [testing](digidollar-paymaster-testing.md).
