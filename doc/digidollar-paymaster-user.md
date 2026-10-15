# Sending DigiDollar with a Paymaster

This guide is for someone sending DD from the GUI wallet. You do not need to
operate a provider or create a provider identity. Provider setup belongs in the
[operator guide](digidollar-paymaster-operator.md); application/RPC use belongs
in the [client contract](digidollar-paymaster-integration.md).

The steps below assume Mainnet and real DGB/DD. Use the Mainnet wallet and a
recipient address for that network; Mainnet DD addresses start with `DD`.
For practice, use separate test funds and the
[Testnet/Regtest guide](digidollar-paymaster-test-networks.md). Test wallets and
providers do not supply funds or offers to Mainnet.

A DD payment needs a DGB network fee. A Paymaster supplies that DGB and may
charge an additional DD service fee. A sponsored offer charges no DD service
fee; the provider still pays the network fee. You still need spendable DD for
the payment. An offer does not give you DD or unlock protected wallet reserves.

## Choose who pays the network fee

| Send DD choice | What it does | What you need |
|---|---|---|
| Own DGB | Your wallet pays the DGB network fee. | Spendable DD and suitable DGB inputs for the fee. |
| Automatic | Core tries your DGB first, then Paymaster if suitable DGB is insufficient. | Spendable DD; any Paymaster fallback still needs exact fee approval. |
| Paymaster | Request provider-funded DGB for this payment. | Spendable DD and a usable offer within your limits. |

An offer shown while Automatic is selected is a possible fallback, not proof
that the provider will be used. Ordinary DGB/DD checks and wallet synchronization
remain necessary in every mode.

## Make a payment

1. Confirm the Mainnet network and sending wallet, then open
   **DigiDollar > Send DD**. Wait until the wallet balance is known. Enter the DD recipient and amount; check the address
   carefully. This flow supports one recipient.
2. Choose the network-fee mode above. For Paymaster, review **service-fee limits**:
   the per-payment DD limit, rolling-day DD limit and maximum effective percentage.
   Zero fee allowance permits only free offers. Do not raise limits merely to
   dismiss a warning unless you accept the resulting cost.
3. Review the displayed offers. Eligible offers fit the current amount, balance
   and fee limits; excluded cards explain why they cannot be selected. A
   recommendation reflects local cost/selection rules, not guaranteed availability
   or an endorsement of the provider.
4. Choose an eligible offer and start preparation. The wallet requests and checks
   an exact offer and may temporarily reserve your DD inputs. This does not yet
   authorize the payment signature. Preparation can require unlocking for an
   input-control proof; unlocking alone is not acceptance of a service fee.
5. In the exact-offer confirmation, review the provider, recipient, DD received,
   service fee, total wallet outflow and expiry. Approve only if they are correct.
   Approval can lead directly to signing and submission. If terms change, review
   them again; the displayed estimate is not your final authorization.
6. Follow the current transfer status and transaction history. Submitted or
   Pending means confirmation is still needed. Success requires the intended
   recipient payment to be validated and confirmed by the local node.

A countdown on an offer card is its announcement expiry. Capacity may be used
by someone else before that time. Preparation performs the current check; a
listing is not a reservation. If no offer is currently listed, availability may
change as providers refill, confirm transactions or publish updated terms.

## Read amounts and fees

The examples below illustrate arithmetic, not promised provider prices:

| Example | Recipient receives | DD service fee | DD leaves your wallet |
|---|---:|---:|---:|
| Sponsored transfer | 2.00 DD | 0.00 DD | 2.00 DD |
| User-paid transfer, fee added | 2.00 DD | 0.02 DD | 2.02 DD |
| Same exact total with fee deduction, if an eligible exact offer exists | 1.98 DD | 0.02 DD | 2.00 DD |

With only 2.00 DD available, the second example cannot be sent as entered.
Reduce the payment, choose a cheaper eligible offer or use fee deduction and
accept the lower recipient amount. Deduction does not bypass limits; the
percentage is measured against the amount the recipient actually receives.
Some rounded totals cannot be represented exactly and are rejected.

Rounding matters: 1% of 2.34 DD rounds up to 0.03 DD, an effective fee of about
1.28%. That can exceed a 1% effective client limit even though the provider's
advertised rate is 1%. The exact confirmation, not the nominal rate, determines
what you authorize. A maximum fee ceiling is a limit, not a charge you always pay.

**Available** excludes protected reserves and other unspendable funds.
**Reserved for Paymaster** can be provider capital or protected payment inputs;
it is still part of wallet funds but cannot simultaneously fund another send.
The DGB network fee and the DD service fee are different costs. Paymaster does
not permit spending collateral or another payment's reserved inputs.

## Payment states and next actions

| What you see | Meaning | Appropriate next step |
|---|---|---|
| Balance/status loading | A reliable snapshot is not available yet. | Wait for the checks; unknown is not a zero balance. |
| No eligible offer / excluded card | Current amount, fee, balance or availability conditions are not met. | Read the reason; adjust the amount/settings deliberately or check again later. |
| Preparing / waiting for exact offer | Provider exchange or local checks are in progress; no payment fee accepted yet. | Let it continue, or use the preparation-cancel control when offered. |
| Exact-offer confirmation | Provider and exact spending terms are ready for review. | Approve those terms or decline; approval may sign and submit. |
| Cancelling preparation | Core is determining/confirming safe unsigned closure. | Wait for confirmed closure before editing or starting another attempt. |
| Submitted / Pending | The transfer may be in flight or in a mempool, not yet confirmed. | Inspect this transfer; do not send a replacement just because a reply was lost. |
| Confirmed recipient payment | The local node has validated confirmation of the intended payment. | Use its transaction/history record; a later reorg can change confirmation state. |
| Recovery / ambiguous result | Signed work may still spend the original inputs. | Use the available status/recovery actions. A recovery confirmation is not recipient-payment success. |

Closing the window or restarting does not undo signed work. An unsigned local
cancellation may leave the provider's reservation until expiry; it does not
charge a service fee or send the payment. After signing, cancellation is not a
simple undo: the original and a recovery transaction may compete for confirmation.

## If something appears stuck

First check the current transfer rather than creating another one. Wallet
history/index checks, provider availability and block confirmation are separate
waits. On Mainnet, miners include transactions in blocks and your node must
receive and validate those blocks. Check synchronization and the retained
transaction's state; a quoted offer's expiry is not a payment-confirmation
deadline. An uncreated preparation step is not fixed by waiting for blocks alone.
The [test-network supplement](digidollar-paymaster-test-networks.md) explains
public Testnet and explicit Regtest mining separately.

For diagnosis, record the build/version, network, approximate time, visible
phase and whether you approved the exact offer. Include the request/session or
transaction ID where needed for local investigation. Do not publish passwords,
private keys, recovery seeds, full PSBTs or unnecessary recipient data.
The [operator diagnosis table](digidollar-paymaster-operator.md#diagnosis) explains
provider-side waits. A GUI cannot make an unknown or signed state safe by
clearing its display.
