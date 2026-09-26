# DigiByte Core v9.26.6 release notes

2026-09-26 working-tree update: [Direct connection capacity](digidollar-paymaster-connection-capacity.md)
documents the default one-channel client queue, required provider Direct
bind, additive status, connection-start limits, routed request admission and
protected reply/recovery quotas. Targeted DoS tests pass; fresh daemon,
payment-flow and Tor/load verification is still required. Wire, consensus
and journal formats are unchanged; historical results do not validate this patch.

The combined [v9.26.6 release notes](../RELEASE_v9.26.6.md) contain the RC2
changes above the preserved RC1 notes, the change counts, activation heights,
upgrade information and verification limits.

That document is the single release-note source for v9.26.6. RC2 is a test
release. Release packages must be verified before distribution.

Notes for earlier releases are under [release-notes/](release-notes/).

## Paymaster integration branch

The fork-specific [Paymaster integration notes](digidollar-paymaster-v9.26.6rc2-integration.md)
and [client contract](digidollar-paymaster-integration.md) describe the additions
on `integration/paymaster-v9.26.6rc2`. They do not change the upstream release
notes or establish release acceptance. The interfaces support a possible x402
extension without committing to its implementation.
