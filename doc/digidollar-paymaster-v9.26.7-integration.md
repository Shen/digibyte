# Paymaster integration with DigiByte v9.26.7

## Source checkpoint

Branch: `integration/paymaster-v9.26.7`.
Integration merge (2026-10-06): `e5cfa2da3e42dcc5aa1ee316771da10fafad0960`.

| Role | Commit |
|---|---|
| Preserved Paymaster source | `2f44ac43c24f99bff7a7388dc6d01d00eef52bcf` |
| Official `release/v9.26.7` | `d7265fb05e26e1e799876bcd526225e4826c3ba8` |
| Common ancestor | `92330d952625e20aef2ee40671a179ef03872ac1` |

The merge incorporated the 13 official commits after that ancestor. Previous
RC2 history remains in Git. The [official release document](../RELEASE_v9.26.7.md)
describes upstream; its tests do not certify the Paymaster integration.

## Integration decisions

Official product behavior takes precedence: explicit tip-specific difficulty,
unused DD receive-address listing, send minimum-change tooltip/build-derived
branding, queued statistics-index shutdown checks and official block-map/test/
CI/package changes are retained. The merge had no textual conflicts; a recorded
reverse-application check confirmed official product/non-Qt-test hunks remained.

The existing MSVC `util::int128_t` adaptation preserves upstream arithmetic
expressions. Arithmetic portability changes in consensus-adjacent code require
separate equivalence review, not an assumption that all branch differences are
Paymaster-only. This merge added no new consensus, wire, record-format or
financial authorization change.

Fork adaptations regenerated the Windows version header, exposed shared
`DigiDollarTest::SyncUpWallet` for the official regression and completed RPC
warmup in isolated address tests without changing production RPC behavior.
Paymaster reservations, exact approvals, session ownership and background GUI
snapshots remain integrated with those upstream changes.

## Verification scope

The checkpoint compiled 12 selected MSVC/static-Qt translation units and linked
incremental test executables. Twenty-six selected cases passed: six Core/RPC and
20 Qt cases across ten functions. Python syntax and diff checks passed. Normal
application/daemon/CLI linking, complete suites, functional execution and
cross-platform verification did not run at this checkpoint. Earlier broad Qt
failures were not retested by that selection.

Later source corrections are described by the current [implementation](digidollar-paymaster-implementation.md),
[integration boundaries](design/paymaster-integration-boundaries.md) and
[release notes](release-notes/paymaster-integration.md). Follow the
[test runbook](digidollar-paymaster-testing.md) and [release gate](digidollar-paymaster-release-gate.md)
for a candidate build; this historical merge record is not a rolling pass.
