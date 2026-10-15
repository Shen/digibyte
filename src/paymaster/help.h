// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_PAYMASTER_HELP_H
#define DIGIBYTE_PAYMASTER_HELP_H

#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif
#include <util/system.h>

#include <string>

namespace DigiDollar::Paymaster {
/** Informational commands only: never register RPCs or CLI modes as node arguments. */
inline std::string GetCommandHelp()
{
    std::string usage = HelpMessageGroup("Paymaster node RPC commands (console or digibyte-cli; help <command> for arguments):");
    usage += HelpMessageOpt("getpaymasternodeconfig", "Inspect node connection settings and prerequisites");
    usage += HelpMessageOpt("preparepaymasternodeconfig", "Preview a node configuration change");
    usage += HelpMessageOpt("applypaymasternodeconfig", "Apply a reviewed configuration plan; node restart is separate");
    usage += HelpMessageOpt("checkpaymasterendpoint", "Check provider endpoint reachability");
    usage += HelpMessageOpt("listpaymasters", "List published providers; listings do not reserve capacity");
#ifdef ENABLE_WALLET
    usage += HelpMessageGroup("Paymaster wallet RPC commands (select the wallet; help <command> for arguments):");
    usage += HelpMessageOpt("createpaymasteridentity", "Create the persistent provider identity");
    usage += HelpMessageOpt("setpaymasterpolicy", "Configure the provider offer and service fee");
    usage += HelpMessageOpt("setpaymastersafetypolicy", "Configure provider spending and admission limits");
    usage += HelpMessageOpt("getpaymastersafetystatus", "Inspect provider budgets and limits");
    usage += HelpMessageOpt("setpaymasterclientsafetypolicy", "Configure client service-fee limits");
    usage += HelpMessageOpt("getpaymasterclientsafetystatus", "Inspect client service-fee limits and budget");
    usage += HelpMessageOpt("setpaymasterenabled", "Enable or disable provider policy");
    usage += HelpMessageOpt("setpaymasterruntimesettings", "Configure provider startup and runtime settings");
    usage += HelpMessageOpt("startpaymaster", "Start provider service when prerequisites permit");
    usage += HelpMessageOpt("stoppaymaster", "Stop provider service; see help for persistent pause options");
    usage += HelpMessageOpt("getpaymasterinfo", "Inspect provider state");
    usage += HelpMessageOpt("getpaymasteroperatorinfo", "Inspect operator readiness and required actions");
    usage += HelpMessageOpt("setpaymasterliquiditypolicy", "Configure reserve targets and automatic refill policy");
    usage += HelpMessageOpt("getpaymasterliquiditystatus", "Inspect reserve maintenance and funding status");
    usage += HelpMessageOpt("preparepaymasterpool", "Preview or approve finite reserve funding");
    usage += HelpMessageOpt("rebalancepaymasterpool", "Rebalance provider reserve liquidity");
    usage += HelpMessageOpt("getpaymasterpoolinfo", "Inspect provider reserve slots");
    usage += HelpMessageOpt("getpaymasterfinancestatus", "Inspect provider income and costs");
    usage += HelpMessageOpt("listpaymasterreservations", "List retained provider reservations");
    usage += HelpMessageOpt("cancelpaymasterquote", "Cancel an eligible quote under Core safety checks");
    usage += HelpMessageOpt("releasepaymastercapital", "Preview or execute guarded release of provider reserves");
    usage += HelpMessageOpt("withdrawpaymastercarrier", "Review and release an eligible DD carrier");
    usage += HelpMessageOpt("acknowledgepaymasterproviderbackup", "Acknowledge a backup reminder; does not create a backup");
    usage += HelpMessageOpt("adoptpaymasterbackup", "Review or adopt an eligible restored provider wallet");
    usage += HelpMessageOpt("getpaymasterclientinfo", "Inspect client capabilities and local readiness");
    usage += HelpMessageOpt("getpaymasteroffers", "Preview locally known offers against client limits");
    usage += HelpMessageOpt("getpaymasterreputation", "Inspect wallet-local provider reliability");
    usage += HelpMessageOpt("clearpaymasterreputation", "Clear wallet-local provider reliability records");
    usage += HelpMessageOpt("senddigidollar", "Prepare and explicitly authorize a Paymaster-funded DD transfer using fee_mode in options");
    usage += HelpMessageOpt("getdigidollarsendsession", "Inspect an existing send request or session");
    usage += HelpMessageOpt("listdigidollarsendsessions", "List retained send sessions");
    usage += HelpMessageOpt("resolvepaymastersession", "Inspect or perform a permitted session/recovery action");
    usage += HelpMessageOpt("requestpaymasterquote", "Request or resume an exact provider quote");
    usage += HelpMessageOpt("walletprocesspaymasterpsbt", "Process an authorized collaborative PSBT");
    usage += HelpMessageOpt("submitpaymasterdigidollar", "Submit the collaborative transfer");
    usage += HelpMessageOpt("processpaymasterresult", "Process a provider result for an existing session");
    usage += HelpMessageOpt("processpaymastersubmits", "Process pending provider submissions");
    usage += HelpMessageOpt("processpaymasterrequests", "Process pending provider requests");
    usage += HelpMessageOpt("createrestrictedpaymasterdescriptor", "Legacy interface; new restricted sponsorship is disabled");
    usage += HelpMessageGroup("Paymaster CLI assistants (run in a terminal, not the GUI console or node startup):");
    usage += HelpMessageOpt("digibyte-cli -rpcwallet=<wallet> -paymastersetup", "Interactive provider setup and operation; requires a terminal and explicit approval");
    usage += HelpMessageOpt("digibyte-cli -rpcwallet=<wallet> -paymasterstatus", "Display provider status without starting setup");
    usage += HelpMessageOpt("digibyte-cli -rpcwallet=<wallet> -paymasterstatus -watch", "Refresh status every ten seconds; stop with Ctrl+C");
#endif
    return usage;
}
} // namespace DigiDollar::Paymaster

#endif // DIGIBYTE_PAYMASTER_HELP_H
