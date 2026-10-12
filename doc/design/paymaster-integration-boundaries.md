# Paymaster integration boundaries — 2026-10-11

## Result and governing rule

Paymaster adapts to DigiByte's existing wallet, validation and lifecycle rules.
Necessary GUI, CLI, RPC, wallet/database and network adapters are permitted;
they must preserve existing authorization and recovery behavior. Performance
work must not skip ownership recovery, turn failing tests into warnings, narrow
existing scans or make a test selection succeed without executing its tests.
An original bug can be corrected with evidence and regression coverage.

The review identified and corrected one original-wallet behavior regression,
three weakened runtime test gates, a narrowed CodeQL build target and a Qt test
filter that could falsely report success. Necessary Paymaster reservation,
signature, backup and lifecycle protections remain. This is a focused cumulative
integration review, not a claim that every changed line is safe or that the
release acceptance matrix has passed.

## Reviewed stand and guidance

- Branch: `integration/paymaster-v9.26.7`.
- Original comparison: local `upstream/release/v9.26.7`,
  `d7265fb05e26e1e799876bcd526225e4826c3ba8`; also the merge base at review start.
  This is a pinned local release reference, not a newly fetched latest upstream.
- Review-start integration: `360a7b3f8aa7399ddba92840e18d7098e2eb2f29`, with a
  clean worktree. Corrections in this report follow that revision.
- Inventory: 168 previously existing shared changed files under source, build,
  CI and test surfaces, excluding newly added files and Paymaster-named files.
  Inventory is not a statement that all 168 received a full independent audit.
- Local evidence: `build_msvc/paymaster-refresh-check/original-boundaries/`.
  `source-review.json` records the original inventory, source SHA256 values,
  baseline blob IDs and unchanged-surface comparisons. `final-receipt.json`
  records the corrected sources, selectively compiled objects, link inputs,
  binaries and reports. These workspace artifacts are not shipped in Git.

Relevant guidance: root/src/test instructions, `CLAUDE.md`'s DigiDollar/oracle
reading order, `CONTRIBUTING.md`, architecture and repo maps, developer notes,
test/build documentation, translation policy and formatting configuration.
Full builds, broad tests and sanitizer/fuzz campaigns are operator tasks here.

## Corrected findings

| Finding | Evidence and impact | Correction |
|---|---|---|
| OB-001: history skipped original descriptor recovery | `DigiDollarWallet::GetDDTransactionHistory()` at `8930d506eb` replaced `IsDDOutputMine()` with known public-key/script checks. A DD key-only output belonging to an imported `tr(internal_key,pk(leaf))` descriptor needs the original internal-key search. With the old object, history did not reconstruct its missing address key. No theft or signature bypass was demonstrated. | Restore the entire history function to the pinned original implementation. Add `dd_history_recovers_missing_descriptor_address_key`, proving exact key recovery and persistence after database reload. Retain foreign-output rejection and locked-history coverage. |
| OB-002: runtime failures became warnings | `digidollar_dca_tests.cpp` and `digidollar_health_tests.cpp` replaced original failure bounds with larger diagnostic warnings. A slow implementation could pass despite violating the original tests. | Restore failure thresholds: health calculation <100 ms, DCA stress <100 ms, mass health alerts <2,000 ms. Retain the added deterministic result check and monotonic clock. |
| OB-003: original CodeQL build target narrowed | `.github/workflows/codeql.yml` built only `test/test_digibyte` instead of the original default targets. Manual C/C++ scanning depends on compiled translation units; narrowing targets can omit other enabled targets. No completed CodeQL database comparison is claimed. | Restore `make -j$(nproc) -C src`; retain the added wallet/tests configuration. Existing query/config exclusions remain unchanged. CI execution is still required. |
| OB-004: unknown Qt suites reported success | `DIGIBYTE_QT_TEST_SUITE=NonexistentPaymasterSuite` returned 0 in the normal prior test binary while running no suite. A mixed valid/invalid filter also needed rejection. | Track unmatched suite names in `src/qt/test/test_main.cpp`; any unmatched name causes a nonzero result. No filter still runs every compiled suite. |

The restored history function matches the original release function after
normalizing line endings. Its full recovery may again incur cold wallet-lock
contention. That cost must be addressed without discarding ownership behavior;
the previous startup speedup is withdrawn. Cached display rows are provisional,
and the GUI worker does not confer financial authority. Ordinary DD payments
already perform their own current input/ownership/fee/signature checks; history
completion was not an original global prerequisite for every send.

## Additional test/scan checks

- Extend the Paymaster security workflow's triggers to shared wallet database,
  signing, restore, lifecycle and relevant Qt adapters. Keep the existing
  sanitizer/fuzz jobs and suppressions. A Qt path trigger does **not** mean Qt
  is sanitizer-tested: the job still builds with `--with-gui=no`.
- Remove the misleading redundant `DISABLED_SCRIPTS` entry for
  `feature_assumevalid.py`. It was already registered in `BASE_SCRIPTS` and was
  **not actually disabled**. Its registration stays intact. An unregistered
  synthetic script still warns and exits 1 with `fail_on_warn=True`.
- Retain the runner fix that exits 1 on test-framework unit failure; the
  original `sys.exit(False)` incorrectly expressed success.
- Check the amended BIP324 long-packet fixture independently. Python EllSwift,
  native `cryptography` ChaCha20Poly1305 and Python length-stream calculations,
  following the [BIP324 reference](https://github.com/bitcoin/bips/blob/master/bip-0324/reference.py),
  reproduce DigiByte's existing salt `digibyte_v2_shared_secret` and mainnet
  magic `fac3b6da`, session ID, garbage terminators and 128-byte suffix. The
  original 14,988-byte suffix does not match. Add an expected SHA256 for the
  **entire** encrypted packet, including length and authentication tag, rather
  than accepting the shortened suffix as the only fixed-output check. Packet
  673's 4,000,001-byte payload gives
  `53736b18a79e0512e95686bfbe94c3c8362e5002a92bec936313ddd57ff53f7c`.
  Existing wrong-side, corruption, AAD and packet-index rejection checks remain.
  No production cipher or key derivation was changed.

## Necessary shared adapters retained

| Boundary | Review disposition |
|---|---|
| Coin selection and spendable balance (`wallet/spend.cpp`, `digidollarwallet.cpp`) | Ordinary wallet sends must also exclude durable Paymaster reservations and pool inputs. Restricting checks to Paymaster RPC/GUI would allow another wallet caller to spend them. The pool-input override remains internal, false by default, and used by exact provider maintenance plans. |
| Ordinary DD send and AUTO fallback (`rpc/digidollar.cpp`, `wallet/rpc/paymaster_send.cpp`) | Original DD-only preflight remains. The adapter classifies DGB funding shortage before AUTO fallback; arbitrary failures cannot authorize fallback. Current DD availability excludes reservations. Existing direct ownership, signing, chain and fee checks remain authoritative. |
| Signing and unlock (`qt/paymasterwallet.cpp`, shared `UnlockContext`, `rpc/encrypt.cpp`) | Paymaster workflow lives in its adapter and reuses the native dialog/relock context. Normal timed unlock stays; provider unlock-until-shutdown is explicit, provider-only authority, not an implicit zero-timeout migration. |
| Wallet load/restore/backup and database (`wallet/load.cpp`, wallet DB backends, Paymaster DB helpers) | Loader retains narrow hooks; service bodies already reside in `wallet/rpc/paymaster_integration.cpp`. Original post-init DD UTXO scan remains. NOTFOUND is distinguished from database errors; existing bool reads retain their semantics. Native backup runs before the provider reminder update. Restore/checkpoint protection necessarily touches wallet lifecycle and must not be bypassed for speed. |
| Rebroadcast (`wallet/wallet.cpp`, Paymaster recovery) | Generic rebroadcast excludes durable Paymaster commits; recovery validates their manifest and budget first. An unreadable protection record deliberately defers the wallet's rebroadcast pass, including ordinary transactions in that wallet, until evidence is readable. This is a fail-closed operational restriction, not removal of transaction validation. |
| GUI reads (`qt/walletmodel.cpp`, native DD views) | Workers retain the wallet, coalesce requests and reject replies from a replaced wallet. Preview state is not authorization. Existing backend reconciliation is retained. The history shortcut was the substantive exception and is now reverted. |
| CLI orchestration (`paymaster/cli.cpp`) | Original RPC transport remains. Paymaster workflow implementation was already moved out of the generic CLI; see the [2026-10-07 review](paymaster-flow-audit.md#original-code-boundary-review-2026-10-07). |
| Direct network transport (`net*`, `net_processing.cpp`, `dandelion.cpp`, `protocol*`) | Dedicated bounded direct connections do not carry ordinary block/tx/address relay. V1/authentication errors fail closed for that channel. Shared SOCKS parameters preserve ordinary defaults; stricter authentication/deadlines are explicit Paymaster settings. Reserved slots have a real resource cost within connection limits, not unlimited capacity. |
| DD metadata and portable arithmetic | New transaction-bound metadata helper first binds the transaction hash, then uses original extraction; original callers remain. Native GNU wide arithmetic is retained, while MSVC uses a portable alias. Domain bounds/formulas were inspected, but cross-platform arithmetic equivalence remains a dynamic verification task. |

Whitespace-insensitive comparison with the pinned original finds no changes in
`src/validation.cpp`, `src/script/`, `src/policy/`,
`src/kernel/chainparams.cpp`, `src/pow.cpp`, `src/wallet/sign.cpp`,
`src/wallet/scriptpubkeyman.cpp`, `ci/`, `test/sanitizer_suppressions/` or
`.github/codeql/`. BIP324/ChaCha/Poly1305 production implementations are also
unchanged. This does not mean the entire consensus subtree is unchanged:
portable DD arithmetic adapters elsewhere require the checks stated above.

## Executed verification and limitations

Selected MSVC `/WX`, `/sdl`, `/GS` translation-unit compiles and isolated links
passed. Existing normal-build libraries/objects supplied the remaining inputs;
this was **not** a clean or full build. No live operator wallet was changed.

| Check | Result |
|---|---|
| History, encrypted/locked keys, watch-only/foreign rejection and self-send | 11 cases, 99 assertions passed; includes the new descriptor recovery test. |
| Negative history reproduction using the prior optimized wallet object | Fails recovering the missing key (exit 201); restored source passes. |
| BIP324 packet vectors, including new full-packet hash | 1 case, 697 assertions passed; independent reference calculation also passed. |
| Qt suite filter | Unknown and mixed valid/unknown filters exit 1; valid `URITests` exits 0 (3 Qt entries, zero failures/skips). Prior normal binary's unknown filter exits 0. |
| Restored runtime gates | Initially 2 cases pass and DCA stress fails at 312 ms (<100 ms required). In the combined final-binary run, DCA stress is 311 ms and mass health alerts 2,001 ms (<2,000 ms required); both fail, exit 201. Bounds remain unchanged. |
| DCA/mass-alert diagnostic with `-- -debugexclude=digidollar` | Both cases pass (30 assertions). Enabled per-iteration DD logging is implicated; this is a diagnostic comparison, **not** a replacement for the default failing gates or proof of exclusive cause. |
| Script inventory, Python/YAML syntax, whitespace | Targeted registry rejection and syntax/diff checks recorded in local receipts. No full functional suite or CI execution is claimed. |

The combined final-binary run executes 15 cases: 13 pass, 2 runtime cases fail;
826/828 assertions pass. All wallet/history and BIP324 assertions pass. The
mass-alert bound is sensitive to this run's conditions; its failure is retained,
not replaced by the earlier isolated pass. No formatter executable was available;
formatting was inspected against the repository configuration.

The installed normal GUI at review start has SHA256
`922A161CB03D62DF75DB17D9CF9797DBAB7D327951D422CFCB2E4A49BCAE3935`
and still contains the withdrawn history optimization. No replacement was made
by this review. Rebuild before treating a live reproduction as verification of
the corrected sources; earlier backup/authentication/GUI reports are evidence
for their own hashed revisions, not the new complete release candidate.

## Remaining checks

The [runbook](../digidollar-paymaster-testing.md#original-code-boundary-verification-2026-10-11)
provides commands and success criteria for a normal build, full Core/Qt tests
and ordinary-wallet/Paymaster Regtests. The default runtime failures remain
open; investigate build/logging cost without relaxing the original bounds.

CI CodeQL extraction and scans, full sanitizer/fuzz campaigns, cross-platform
wide-arithmetic checks and a new cold-start reproduction with the restored
history path remain open. If startup contention delays Paymaster preparation,
adjust Paymaster scheduling/progress/availability around the existing wallet
work; do not reinstate the ownership shortcut. A new startup gate would need
its own lifecycle/cancellation tests and should describe a Paymaster prerequisite,
not invent an original global history-scan requirement.

## Paymaster coordination follow-up (2026-10-12)

The Paymaster GUI now waits for the existing canonical history workers on the
same node before creating a new payment request. The retained worker, not the
view, owns the activity count, so closing a wallet model cannot falsely end
the wait. The small WalletModel adapter reports activity/errors and leaves
`DigiDollarWallet::GetDDTransactionHistory()` unchanged. It adds no wallet,
consensus, network or financial persistence format.

Client and operator views explain long waits, defer their reads, and refresh
after completion. A failed selected-wallet read blocks new client preparation
until explicit retry succeeds. Fast warm reads preserve presentation. The
two-minute live preparation budget starts after the first successful Core
reply and excludes observed later unsigned history waits. Native sends,
original ownership/key recovery, synchronization, transport/quote limits,
exact financial approval and signed-artifact protection remain in place.

This addresses observed local Qt history contention; it does not promise that
unknown remote providers or unrelated RPC tasks never time out. Five new
Paymaster GUI cases use disposable wallets and the real history worker. See
the [history-wait runbook](../digidollar-paymaster-testing.md#paymaster-coordination-with-full-dd-history-2026-10-12)
for the final evidence and operator acceptance steps. The normal installed
GUI is not automatically replaced by an isolated test build.

### Safe DD ownership optimization and network-status repair (2026-10-12)

The shared DD ownership fallback performed private HD expansion for every
descriptor script even when its public internal key could not produce the
examined DD output. A foreign output can therefore hold `cs_wallet` longer
than necessary; wallet validation callbacks need that same lock, and the
original bounded validation queue can then delay other node work. Live logs
also showed an Own DGB transaction reaching processing on the peer minutes
after local acceptance. They do not establish that ownership derivation was
the only source of that delay.

The correction keeps the complete descriptor search, all earlier ownership
checks and the negative cache. It uses the existing public solving provider
to reject nonmatching candidates before private expansion. A public match
does **not** establish ownership: the original private internal-key lookup,
final DD tweak verification and recovered-key persistence still run. Original
history construction, wallet/chain synchronization, validation queue limits,
relay, confirmation rules and Paymaster financial protections are unchanged.
This is a narrow shared-wallet performance fix, rather than a Paymaster-only
shortcut around DD security checks.

Separately, the Paymaster integration's asynchronous Overview bridge had
converted structured RPC exceptions to text, redirecting them from the
original waiting handler into the generic error handler. The bridge now
preserves their type. Failure clears both the health snapshot and its old
percentage label; malformed results still fail. The statistics RPC and index
readiness requirements remain unchanged.

The [DD refresh runbook](../digidollar-paymaster-testing.md#dd-refresh-latency-and-status-errors-2026-10-12)
records the regression counterexample, targeted tests and outstanding normal
build/live acceptance. Isolated tests do not establish production throughput
or that every possible relay delay has been eliminated.
