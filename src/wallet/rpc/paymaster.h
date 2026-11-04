// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Wallet Paymaster RPC registration and automatic-service entry points. */

#ifndef DIGIBYTE_WALLET_RPC_PAYMASTER_H
#define DIGIBYTE_WALLET_RPC_PAYMASTER_H

#include <consensus/amount.h>
#include <paymaster/types.h>
#include <wallet/paymasterintegration.h>
#include <primitives/transaction.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CScheduler;
class JSONRPCRequest;
class RPCHelpMan;
struct RPCResult;
class UniValue;

namespace wallet {
class CWallet;
class PaymasterStore;
struct WalletContext;

UniValue RequestAutomaticPaymasterQuote(const JSONRPCRequest& request,
                                        const std::string& address,
                                        CAmount amount,
                                        const UniValue& options,
                                        const std::vector<COutPoint>* preset_inputs);
/** Shared additive RPC schema, independent of wire and wallet record versions. */
RPCResult PaymasterPaymentViewResult();
RPCResult PaymasterTransportResult();
RPCHelpMan getpaymasterclientinfo();
RPCHelpMan getdigidollarsendsession();
RPCHelpMan listdigidollarsendsessions();
RPCHelpMan getpaymasterreputation();
RPCHelpMan clearpaymasterreputation();
RPCHelpMan getpaymasteroffers();
RPCHelpMan requestpaymasterquote();
RPCHelpMan resolvepaymastersession();
RPCHelpMan walletprocesspaymasterpsbt();
RPCHelpMan submitpaymasterdigidollar();
RPCHelpMan createpaymasteridentity();
RPCHelpMan createrestrictedpaymasterdescriptor();
RPCHelpMan setpaymasterpolicy();
RPCHelpMan setpaymastersafetypolicy();
RPCHelpMan getpaymastersafetystatus();
RPCHelpMan setpaymasterclientsafetypolicy();
RPCHelpMan getpaymasterclientsafetystatus();
RPCHelpMan setpaymasterenabled();
RPCHelpMan setpaymasterruntimesettings();
RPCHelpMan setpaymasterliquiditypolicy();
RPCHelpMan getpaymasterliquiditystatus();
RPCHelpMan getpaymasterfinancestatus();
RPCHelpMan acknowledgepaymasterproviderbackup();
RPCHelpMan withdrawpaymastercarrier();
RPCHelpMan releasepaymastercapital();
RPCHelpMan cancelpaymastermaintenance();
RPCHelpMan adoptpaymasterbackup();
RPCHelpMan preparepaymasterpool();
RPCHelpMan rebalancepaymasterpool();
RPCHelpMan getpaymasterinfo();
RPCHelpMan getpaymasteroperatorinfo();
RPCHelpMan getpaymasterpoolinfo();
RPCHelpMan listpaymasterreservations();
RPCHelpMan cancelpaymasterquote();
RPCHelpMan processpaymasterrequests();
RPCHelpMan processpaymastersubmits();
RPCHelpMan processpaymasterresult();
RPCHelpMan startpaymaster();
RPCHelpMan stoppaymaster();
} // namespace wallet

#endif // DIGIBYTE_WALLET_RPC_PAYMASTER_H
