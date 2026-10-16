# Paymaster operator guide

This guide targets Mainnet operators. Read the
[Mainnet node configuration](digidollar-paymaster.md#mainnet-provider-configuration)
first. GUI and interactive CLI use shared setup decisions and persisted Core
policy. Automatic provider work runs in the daemon; neither a GUI nor CLI watch
must remain open.

## First provider setup

Before setup, confirm Mainnet (`getblockchaininfo.chain` is `main`), the node
data directory and provider wallet. The same wallet name on another node is
not the same provider. Use a fresh identity for each independent provider;
do not clone a running wallet.
The node needs a reachable advertised Direct endpoint and local authenticated
RPC for administration. Verify the listener externally as described in
[Mainnet startup checks](digidollar-paymaster.md#start-and-verify-mainnet-operation).
Rehearsals use the separate [Testnet/Regtest guide](digidollar-paymaster-test-networks.md);
select that test network and its wallet consistently instead of using the
Mainnet commands below.

Have confirmed DGB for reserve principal **and transaction fees**, and DD for
carrier reserves when offering user-paid service that needs them. Funding the
wallet is separate from authorizing the setup to allocate reserves. The preview
reports what is missing; no fixed amount in this guide guarantees every setup.
Do not move collateral or another session's reserved funds to make it pass.

### GUI sequence

1. Start the Mainnet GUI with its intended data directory, load/select the
   provider wallet, open **DigiDollar > Paymaster Network** and
   wait for current wallet/history/index readiness. A loading status does not
   create a second setup requirement.
2. Configure the node connection if needed. Review the bind address and advertised
   endpoint, apply the configuration plan, and restart the node explicitly if
   requested. Check that the correct wallet is selected again after restart.
3. Review identity and offer: display name, user-paid/public-sponsored models,
   amount range, service fee and quote/network-fee limits. Create the persistent
   identity only when needed; do not replace it for routine settings changes.
4. Review finite payment/sponsorship limits and reserve targets. Choose whether
   recurring paid refill is authorized and under which ceilings. Review the
   separate finite initial funding plan before approving transaction creation.
5. Choose autostart and unlock behavior deliberately. Review the final setup
   summary, approve intended changes, and follow funding/confirmation progress.
6. Verify current service readiness and actual free payment capacity, not only
   that setup was accepted. Back up the resulting wallet. An open offer or an
   announcement is not a successfully completed customer payment.

Existing settings are available through Overview, Funds & reserves, Activity,
Income & costs and Settings. Node configuration requires restart; running-provider
offer changes use the reviewed save/restart path. Do not use tab changes as part
of a save procedure. If a save fails, inspect its error/status and saved values
before assuming the running offer changed.

**Settings > Start, pause & retire** is the first settings destination. It groups
the operation controls and retirement review, with a checklist of each action's
effects. These checkmarks describe the action, not its completion. **Funds &
reserves** links directly to this page; Overview retains its quick Start/Pause
controls. Both entry points use the same confirmations and readiness checks.

A confirmed stopped provider with saved settings can request **Resume** while
known budget, liquidity or node prerequisites remain unmet. Overview keeps the
prerequisite review beside Resume. This records a one-shot start request for the
current wallet session; it does not mean the provider is serving payments or
approve refill, increase limits or enable autostart. The waiting state is shown
explicitly. Pause or Stop cancels the request. Locked wallets and unknown,
restore/checkpoint errors still require review before the GUI offers Resume.
When already persistently stopped, Stop is disabled; **Retire** remains the
entry point for reserve release and the final balance/backup review.

For a spending-limit warning, compare the offer's network-fee ceiling with its
payment safety limits as well as the rolling budget usage. For example, an offer
ceiling of 0.50 DGB cannot fit a per-payment limit of 0.20 DGB, even when today's
budget is unused. Review the intended offer ceiling and limits; neither changes
automatically. RPC `can_accept_minimum_quote` only checks a hypothetical
one-satoshi quote, not the saved offer ceiling or payment readiness.

| Action | Effect after confirmation |
|---|---|
| Start / resume | Enable the saved configuration and request operation subject to readiness and wallet checks. Existing budgets and automation preferences remain; autostart is unchanged. |
| Pause | Persistently disable service and autostart, including new reserve-setup signatures. Retain reserves, unfinished work and refill preferences; refill execution is blocked while paused. |
| Stop | Apply the **same persistent pause**. Optionally review full reserve release in a separate confirmation. Without release approval, its effect is the same as Pause. |
| Retire | Stop, check unfinished work, separately review full reserve release and eligible uncreated funding cancellation, then review wallet balances and a full-wallet backup. A confirmed release turns off automatic refill and revokes its fee approval. |

Approved full release through Stop also turns off refill and revokes its fee
approval. No action transfers funds or closes the wallet. Signed work remains
protected and can still confirm. Cancelling or failing a later retirement step
does not undo a confirmed stop; the remaining review stays incomplete. Ordinary
retirement retains the identity for a deliberate future setup. Restored-wallet
recovery can instead permanently retire it after a separate explicit review;
see [capital release](paymaster-capital-release.md).

### CLI sequence

The following assumes `digibyte-cli` is on PATH and the intended Mainnet node
uses the default data directory. If using a custom directory or port, add the
same `-datadir` and custom `-rpcport` to **every** command. A GUI node must run
with `server=1` for external CLI access. On Windows, use `&` and the explicit
`.exe` path when it is not on PATH; see the
[Mainnet PowerShell examples](digidollar-paymaster.md#start-and-verify-mainnet-operation).

First inspect connectivity and wallet selection (read-only):

```sh
digibyte-cli -chain=main getblockchaininfo
digibyte-cli -chain=main listwallets
digibyte-cli -chain=main -rpcwallet=provider getwalletinfo
```

`provider` is a wallet-name placeholder. If this is a new setup with no wallet,
the terminal assistant can select or create an encrypted descriptor wallet:

```sh
digibyte-cli -chain=main -paymastersetup
```

For an existing known wallet, select it explicitly:

```sh
digibyte-cli -chain=main -rpcwallet=provider -paymastersetup
digibyte-cli -chain=main -rpcwallet=provider -paymasterstatus
digibyte-cli -chain=main -rpcwallet=provider -paymasterstatus -watch
```

These are alternative entry points, not a sequence that must stay running.
The assistant covers wallet, connection, offer, costs/reserves, startup,
review/apply and funding completion. Existing-provider entry defaults to status
with explicit pause/resume, unlock, backup and settings actions. An explicitly
selected unloaded wallet is loaded first. Enter declines the final mutation/
funding review; type the requested confirmation to approve it.

Setup requires a terminal and rejects noninteractive setup before mutation.
Do not automate it by piping answers/passwords. Automation uses the documented
individual RPCs and their explicit policy/plan/authorization contracts. `-watch`
is only observation: ending it neither stops service nor disables refill.
Saved settings are preserved until reviewed changes are accepted. Proposed
wizard values are suggestions, not network constants or future fee estimates.

## Routine operation and restart

Use these status views with the same node/wallet arguments as setup:

```sh
digibyte-cli -chain=main -rpcwallet=provider getpaymasteroperatorinfo
digibyte-cli -chain=main -rpcwallet=provider getpaymasterliquiditystatus
digibyte-cli -chain=main -rpcwallet=provider getpaymastersafetystatus
digibyte-cli -chain=main -rpcwallet=provider getpaymasterfinancestatus
```

`getpaymasteroperatorinfo` is a strictly read-only operations snapshot.
`getpaymasterfinancestatus` can reconcile persisted finance records with chain
observations before returning them; it is not a mutation-free database read.
Neither is approval for a new payment, reserve-funding transaction or service
restart. Distinguish this bookkeeping from signing/spending authority in scripts.

| Situation | Action | Check afterwards |
|---|---|---|
| Restart after an upgrade | Stop normally, replace the intended binary, restart with the same data directory/network and load the wallet. | Executable version, wallet selection, synchronization and restore/readiness state. Source changes alone do not update the process. |
| Encrypted wallet after restart | Explicitly unlock through the GUI/terminal workflow. | Intended unlock duration and provider status; autostart cannot supply a saved passphrase. |
| Change offered model or fee | Review saved policy, edit the offer and approve save/restart if running. | Read back the effective policy and current service state; existing client listings may be cached. |
| Temporarily exhausted targets | Inspect free confirmed slots separately from missing refill capacity. | Existing free slots can serve; missing targets do not imply all service must stop. |
| Pause future service and setup | Use the persistent pause action. | Service stopped, autostart/provider enablement paused; signed work still protected. |
| Resume after persistent pause | Use the assistant's reviewed resume path or deliberately re-enable the configured provider and start it. | `startpaymaster` alone does not undo disabled provider policy; restore/unlock/readiness checks still apply. |

For RPC automation, the normal persistent pause is this JSON-RPC method and
parameter array; it is a **state-changing** operation, not a status check:

```json
{"method":"stoppaymaster","params":[{"persistent":true,"pause_setup":true}]}
```

Execute it on the selected wallet endpoint using your authenticated RPC client.
It disables saved autostart/provider configuration and new setup signatures,
then stops service. It does not revoke existing signatures or release protected
funds. A bare `stoppaymaster` can be undone later by saved autostart. Complete
node shutdown is a separate operation; stopping a provider does not stop the node.

The GUI shows **Paymaster is stopped** when a current snapshot confirms a
disabled, non-running provider without a pending start request. Saved offers
are marked as configured but stopped. Missing reserves or spending requirements
for a later restart do not replace that service status. If refill is off and no
work remains, the reserve card shows the retained or below-target reserves
without asking for immediate refill approval. Unknown errors and wallet
restore/checkpoint protection still take precedence; open payments remain in
Activity. These display distinctions do not change start or signing checks.
The same stopped status applies after ordinary retirement, including an empty
pool or a declined release. It does not assert that reserves were retained or
released; the release receipt and retirement review report that separately.
Once a fresh snapshot confirms running again, old successful stop/release
messages disappear. Unclear or failed release results remain available for review.

`startpaymaster` with `wait_for_readiness=true` can retain a one-shot start
request during the current wallet load when accepted. It is not persistent
autostart, a wallet unlock or permission to exceed saved limits. Inspect the
returned state and errors; acceptance is not proof of immediately serving.

## Encrypted wallets and headless service

Continuous operating unlock uses `walletpassphrase` with timeout 0 and the
`paymaster_until_shutdown` option; `unlocked_until=-1` identifies that state.
Consult same-build help for argument names. Prefer the assistant's secret
input instead of exposing passphrases in process arguments or shell history.
It requires numeric loopback RPC or a configured local tunnel.

This unlock applies to the **whole wallet**, not only Paymaster signatures.
It ends on manual locking, wallet unload or shutdown. The passphrase is not
persisted for restart: encrypted wallets need fresh unlocking afterwards.
Timed unlock is also supported.

| Control | Meaning |
|---|---|
| Provider enabled | Wallet policy permits provider work. |
| Running service | Requests can be accepted when their prerequisites hold. |
| Autostart | Resume service after wallet load when ready. |
| Automatic refill | Maintain saved targets under separately approved limits. |
| Wallet unlock | Keys may sign; this alone neither enables service nor approves spending. |

For durable pause use the GUI pause action or persistent `stoppaymaster`
options, including `pause_setup` if finite setup must stop. Disabling autostart
does not itself stop an already running service.

## Reserves and availability

Protected DGB/DD reserves are excluded from available balance and shown
separately; wallet total includes them. Funds outside the pool may still be
insufficient for refill plus transaction fees. Manual locks, unconfirmed funds
and locked DD collateral are separate constraints.

The target is desired capacity, not an all-or-nothing service requirement.
Confirmed free complete slots can serve while further refill waits for funds.
Refill creates fewer **whole slots** when necessary, waits for confirmation,
and continues toward the unchanged target. This applies to DGB and required
DD carrier slots. See [pool setup](digidollar-paymaster-pool-setup.md).

No usable free slot means no new capacity. Cached announcements may remain
visible until expiry and become unavailable sooner. Exact offers reserve
capacity; listings do not. An open offer is not a completed payment. Activity
counters overlap stages and must not be summed into a payment count.

## Diagnosis

| Observation | Interpretation and next check |
|---|---|
| Reading provider status | Local snapshot loading; autostart may already be serving. Check Core status. |
| Waiting for history/index | Canonical checks remain necessary; do not disable scans. |
| Refill needs wallet funds | Inspect confirmed funds outside reserves, complete missing slots and fee allowance. Existing capacity may still work. |
| Waiting for confirmation | Inspect retained txid, mempool, peer connectivity and validated block progress. Mainnet miners confirm transactions; offer expiry is not a confirmation deadline. |
| Saved task without transaction | No confirmation is pending for that step; inspect prerequisites/funding. |
| Offer exchange failed | Inspect the same client request and provider Activity/logs before retrying. |
| Network statistics unavailable | Unknown snapshot, not zero supply or proof of lost funds; check synchronization/index diagnostics. |
| Restore guard | Use supported reviewed adoption/recovery; do not clear records to bypass it. |

Use operator/provider, liquidity, safety and finance status plus client session
inspection and node logs. Correlate time/wallet/request without publishing
secrets or unnecessary payment metadata. Temporary debug categories should be
restored to their prior settings after diagnosis.

## Backup and retirement

Back up the full wallet after identity and material policy/reserve changes.
Acknowledging a reminder does not create a backup. Restoration is guarded before
autostart; independent checkpoints detect stale wallet copies. Only supported
matching/quiescent states can be adopted after review. Do not operate cloned
stores as independent instances of one identity.

To retire, stop service, disable provider/autostart and resolve protected work,
then preview and approve [capital release](paymaster-capital-release.md). That
returns eligible reserves to ordinary selection without sending funds or
erasing history.
If a start could not fund its reserves, GUI retirement also reviews cancellation
of funding tasks that created no transaction. CLI/RPC can request the same review
with `releasepaymastercapital {"cancel_uncreated":true}`; repeat the option with
the approved plan on execution. Signed or ambiguous work still blocks release.
Stopping immediately gives the request priority over new provider work and
briefly waits for active work. A continuing busy error means the stop has not
been confirmed, not that funds were released. Confirmed Stop/Pause blocks refill
execution while retaining its preferences; completing retirement also turns off
automatic refill and revokes its fee approval. Failed stop attempts remove their
temporary priority rather than leaving an invisible permanent pause.

To cancel only uncreated reserve funding without retiring the provider, use
**Cancel uncreated reserve work** in the management-task card. It previews the
selected DGB/DD operations and turns automatic refill off on confirmation.
Existing reserves and signed transactions remain protected. GUI and headless
RPC steps are documented under [cancellation](digidollar-paymaster-pool-setup.md#cancellation-and-verification).
