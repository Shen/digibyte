# DigiByte Core v9.26.6 release notes

2026-09-27 status, source `1789c803be` on `integration/paymaster-v9.26.6rc2`:
[Direct connection capacity](digidollar-paymaster-connection-capacity.md#current-verification-status)
includes the default single-channel queue, dedicated provider listener,
DoS admission limits, recovery fixes and explicit transport retry. The updated
Windows build passed 284 selected unit tests (10,767 assertions) and all nine
focused functional tests. Qt, reference compatibility, real Tor/load and the
wider release matrix remain open. Wire, consensus and journal formats are unchanged.

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
