// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <paymaster/cli.h>
#include <paymaster/setup.h>
#include <paymaster/types.h>

#include <common/args.h>
#include <compat/stdin.h>
#include <support/cleanse.h>
#include <tinyformat.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace DigiDollar::Paymaster {
namespace {
class OperatorCli
{
    const ArgsManager& m_args;
    const std::string& m_rpc_host;
    const CliRpc& m_rpc;

    UniValue OperatorCall(const std::string& method, const UniValue& params, const std::optional<std::string>& wallet)
    {
        return m_rpc(method, params, wallet);
    }
    UniValue OperatorParams(std::initializer_list<UniValue> values)
    {
        UniValue result{UniValue::VARR};
        for (const auto& value : values)
            result.push_back(value);
        return result;
    }
    std::string OperatorPrompt(const std::string& label, const std::string& current = {})
    {
        return SetupPrompt(std::cin, std::cout, label, current);
    }
    bool OperatorConfirm(const std::string& label)
    {
        return SetupConfirm(std::cin, std::cout, label);
    }
    std::string OperatorSelect(const std::string& label, const std::vector<SetupMenuItem>& items, const std::string& current)
    {
        return SetupSelect(std::cin, std::cout, label, items, current);
    }
    bool OperatorSetting(const std::string& label, bool current, const std::string& enabled, const std::string& disabled)
    {
        return OperatorSelect(label, {{"yes", "Yes", enabled}, {"no", "No", disabled}}, current ? "yes" : "no") == "yes";
    }
    void OperatorStep(int step, const std::string& title)
    {
        std::cout << "\n[" << step << "/7] " << title << "\n" << std::string(64, '-') << '\n';
    }
    void OperatorFields(UniValue& values, bool edit, const std::set<std::string>& skip = {})
    {
        for (const auto& field : SetupFields()) {
            if (skip.count(field.key) || !values.find_value(field.key).isNum()) continue;
            const auto value = values.find_value(field.key).getInt<int64_t>();
            if (edit) values.pushKV(field.key, SetupReadNumber(std::cin, std::cout, field, value));
            else std::cout << "  " << field.title << ": " << SetupFormatNumber(value, field.decimals) << ' ' << field.unit << '\n';
        }
    }
    bool OperatorCustomize(const std::string& title)
    {
        return OperatorSelect(title, {{"keep", "Use the displayed values", "Keep these values for the final review."},
                                      {"custom", "Customize", "Change each value with an explanation of its effect."}}, "keep") == "custom";
    }
    bool OperatorAllowsModel(const UniValue& policy, const std::string& model)
    {
        const auto& models = policy.find_value("funding_models");
        return std::any_of(models.getValues().begin(), models.getValues().end(), [&](const UniValue& value) { return value.get_str() == model; });
    }
    std::vector<std::pair<std::string, std::string>> OperatorBudgetModels(const UniValue& policy)
    {
        std::vector<std::pair<std::string, std::string>> result;
        if (OperatorAllowsModel(policy, "user_paid")) result.emplace_back("user_paid", "Customer-paid payments");
        if (OperatorAllowsModel(policy, "sponsored")) {
            const bool restricted = policy.find_value("sponsorship_scope").get_str() == "restricted";
            result.emplace_back(restricted ? "restricted_sponsored" : "public_sponsored", restricted ? "Restricted sponsored payments" : "Public sponsored payments");
        }
        return result;
    }
    std::vector<std::pair<std::string, std::string>> OperatorStoredBudgetModels(const UniValue& policy, const UniValue& safety)
    {
        auto models = OperatorBudgetModels(policy);
        for (const auto& model : {std::pair<std::string, std::string>{"user_paid", "Customer-paid"}, {"public_sponsored", "Public sponsored"}, {"restricted_sponsored", "Restricted sponsored"}}) {
            if (std::any_of(models.begin(), models.end(), [&](const auto& active) { return active.first == model.first; })) continue;
            if (safety.find_value(model.first).find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>() > 0)
                models.emplace_back(model.first, model.second + " (inactive saved limits)");
        }
        return models;
    }
    void OperatorShowOffer(UniValue& policy)
    {
        std::cout << "Payment model:";
        for (const auto& model : OperatorBudgetModels(policy)) std::cout << ' ' << model.second;
        std::cout << '\n';
        OperatorFields(policy, false);
    }
    void OperatorShowSafety(const UniValue& policy, const UniValue& safety)
    {
        for (const auto& model : OperatorStoredBudgetModels(policy, safety)) {
            std::cout << model.second << " - DGB spending limits\n";
            auto limits = safety.find_value(model.first);
            OperatorFields(limits, false);
        }
    }
    std::string OperatorStepName(const std::string& method)
    {
        if (method == "stoppaymaster") return "Pause the existing provider for configuration";
        if (method == "createpaymasteridentity") return "Create the provider identity in this wallet";
        if (method == "setpaymasterpolicy") return "Save the customer offer";
        if (method == "setpaymastersafetypolicy") return "Save the reviewed payment limits";
        if (method == "setpaymasterliquiditypolicy") return "Save reserve targets and refill limits";
        if (method == "preparepaymasterpool") return "Prepare the reviewed operating capital";
        if (method == "setpaymasterruntimesettings") return "Save processing mode and autostart";
        if (method == "setpaymasterenabled") return "Save provider enablement";
        return method;
    }
    std::string OperatorSecret(const std::string& label)
    {
        const auto host = m_rpc_host;
        if (host != "127.0.0.1" && host != "::1" && host != "[::1]") throw std::runtime_error("Interactive wallet secrets require numeric loopback RPC or a separately configured local tunnel.");
        NO_STDIN_ECHO();
        std::cout << label << ": " << std::flush;
        std::string value;
        if (!std::getline(std::cin, value)) throw std::runtime_error("Passphrase entry cancelled");
        std::cout << '\n';
        return value;
    }
    void OperatorUnlock(const std::string& wallet, int64_t seconds, bool until_shutdown = false, const UniValue* context = nullptr)
    {
        if (until_shutdown ? seconds != 0 : (seconds < 60 || seconds > 86400)) throw std::runtime_error("Choose an unlock duration between 60 and 86400 seconds, or continuous operation");
        auto pass = OperatorSecret("Wallet passphrase (wallet-wide unlock; never saved)");
        auto params = OperatorParams({UniValue{pass}, UniValue{seconds}});
        if (until_shutdown) params.push_back(true);
        try {
            if (context) CheckSetupContext(*context, OperatorCall("getpaymasteroperatorinfo", UniValue{UniValue::VARR}, wallet));
            OperatorCall("walletpassphrase", params, wallet);
        } catch (...) {
            memory_cleanse(pass.data(), pass.size());
            throw;
        }
        memory_cleanse(pass.data(), pass.size());
    }
    void OperatorOperatingUnlock(const std::string& wallet, const UniValue& snapshot)
    {
        if (!snapshot.find_value("encrypted").isBool()) throw std::runtime_error("PAYMASTER_SETUP_STATUS_INCOMPLETE");
        if (!snapshot.find_value("encrypted").isTrue()) return;
        if (!snapshot.find_value("provider").find_value("wallet_locked").isTrue() &&
            snapshot.find_value("unlocked_until").getInt<int64_t>() == -1) {
            std::cout << "Continuous operating unlock is already active.\n";
            return;
        }
        std::cout << "Operating access unlocks the ENTIRE selected wallet. The passphrase is never saved.\n"
                  << "Continuous access ends at manual wallet lock, wallet unload or node restart. Spending limits stay unchanged.\n";
        const auto mode = OperatorSelect("Wallet access while the provider operates", {
            {"continuous", "Continuous operation (recommended for 24/7)", "No timer interrupts the running node. Enter the password again after restart or wallet reload."},
            {"timed", "Limited operating session", "New signing stops when the timer expires; you must unlock again to continue."}}, "continuous");
        int64_t seconds{0};
        if (mode == "timed") {
            const auto& fields = SetupFields();
            seconds = SetupReadNumber(std::cin, std::cout, *std::find_if(fields.begin(), fields.end(), [](const auto& field) { return field.key == "unlock_seconds"; }), 3600);
        }
        OperatorUnlock(wallet, seconds, mode == "continuous", &snapshot);
    }
    void OperatorFundingAddresses(const std::string& wallet, const UniValue& context)
    {
        for (const auto* method : {"getnewaddress", "getdigidollaraddress"}) {
            CheckSetupContext(context, OperatorCall("getpaymasteroperatorinfo", UniValue{UniValue::VARR}, wallet));
            const bool dgb = std::string{method} == "getnewaddress";
            auto params = OperatorParams({UniValue{"Paymaster setup funding"}});
            if (dgb) params.push_back("bech32m");
            const auto address = OperatorCall(method, params, wallet);
            if (!address.isStr() || address.get_str().empty()) throw std::runtime_error("Funding address was not returned");
            CheckSetupContext(context, OperatorCall("getpaymasteroperatorinfo", UniValue{UniValue::VARR}, wallet));
            std::cout << (dgb ? "DGB receiving address: " : "DD receiving address: ") << address.get_str() << '\n' << std::flush;
        }
        std::cout << "Send funds from your funding wallet to the matching address. This setup does not send from another wallet.\n";
    }
    bool OperatorAwaitSetup(const std::string& wallet, const UniValue& context, bool require_running)
    {
        std::cout << "Checking incoming funds, approved reserve preparation and readiness every 2 seconds.\n"
                  << "Ctrl+C closes this monitor; accepted funding and a requested start remain active in Core.\n";
        std::string last_message, last_balances;
        const auto started = std::chrono::steady_clock::now();
        auto last_report = started;
        for (;;) {
            const auto current = OperatorCall("getpaymasteroperatorinfo", UniValue{UniValue::VARR}, wallet);
            CheckSetupContext(context, current);
            const auto progress = InspectSetupProgress(current, require_running);
            if (progress.message != last_message) {
                std::cout << "\n" << progress.message << '\n' << std::flush;
                last_message = progress.message;
            }
            if (progress.blocked) return false;
            if (progress.complete) return true;
            const auto& provider = current.find_value("provider");
            if (require_running && !provider.find_value("running").isTrue() && !provider.find_value("start_requested").isTrue()) {
                std::cout << "The requested start is no longer active. Review provider status before requesting another start.\n";
                return false;
            }
            if (progress.needs_unlock) {
                OperatorOperatingUnlock(wallet, current);
                continue;
            }
            const auto balances = OperatorCall("getbalances", UniValue{UniValue::VARR}, wallet).find_value("mine");
            // minconf=0 retains the separate confirmed bucket and also includes
            // incoming unconfirmed DD, so funding becomes visible before mining.
            const auto dd = OperatorCall("getdigidollarbalance", OperatorParams({UniValue{""}, UniValue{0}}), wallet);
            const auto dd_amount = [](int64_t cents) {
                if (cents < 0) throw std::runtime_error("Invalid DD funding balance");
                return strprintf("%d.%02d", cents / 100, cents % 100);
            };
            CheckSetupContext(context, OperatorCall("getpaymasteroperatorinfo", UniValue{UniValue::VARR}, wallet));
            std::ostringstream balance_text;
            balance_text << "DGB available/trusted: " << balances.find_value("trusted").write()
                         << " | incoming unconfirmed: " << balances.find_value("untrusted_pending").write()
                         << " | immature mining rewards: " << balances.find_value("immature").write() << '\n'
                         << "DD confirmed: " << dd_amount(dd.find_value("confirmed").getInt<int64_t>())
                         << " | incoming unconfirmed: " << dd_amount(dd.find_value("unconfirmed").getInt<int64_t>()) << '\n';
            if (last_balances != balance_text.str()) {
                last_balances = balance_text.str();
                std::cout << last_balances << "Core checks usable inputs; displayed balances alone do not complete setup.\n";
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds{10}) {
                std::cout << "Still monitoring automatically (" << std::chrono::duration_cast<std::chrono::seconds>(now - started).count()
                          << " seconds elapsed). No new approval is needed.\n" << std::flush;
                last_report = now;
            }
            std::this_thread::sleep_for(std::chrono::seconds{2});
        }
    }
    void OperatorBriefStatus(const UniValue& snapshot)
    {
        const auto& provider = snapshot.find_value("provider");
        std::cout << "\nProvider: " << (provider.find_value("running").isTrue() ? "running" : "stopped")
                  << " | Locally ready: " << (provider.find_value("ready").isTrue() ? "yes" : "no")
                  << " | Wallet: " << (provider.find_value("wallet_locked").isTrue() ? "locked" : "unlocked")
                  << " | Autostart: " << (provider.find_value("autostart").isTrue() ? "on" : "off") << '\n';
        if (!provider.find_value("ready").isTrue()) std::cout << InspectSetupProgress(snapshot, false).message << '\n';
        std::cout << "Use -paymasterstatus with this wallet for detailed diagnostics.\n";
    }
public:
    OperatorCli(const ArgsManager& args, const std::string& rpc_host, const CliRpc& rpc)
        : m_args(args), m_rpc_host(rpc_host), m_rpc(rpc) {}

    int Run(bool setup)
    {
        if (m_args.GetBoolArg("-named", false) || m_args.GetBoolArg("-stdin", false) || m_args.GetBoolArg("-stdinwalletpassphrase", false)) throw std::runtime_error("Paymaster interactive modes cannot be combined with -named/-stdin/-stdinwalletpassphrase");
        if (setup && !StdinTerminal()) throw std::runtime_error("-paymastersetup requires an interactive terminal; use explicit RPCs for automation");
        const auto empty = UniValue{UniValue::VARR};
        if (setup) {
            std::cout << "Paymaster setup - from provider wallet to running service\n"
                      << "Enter keeps the displayed selection. Numbers or option names both work; ? repeats menu help.\n"
                      << "DGB pays network costs; DD is your customer service-fee income. Amounts use DGB/DD, not subunits.\n"
                      << "Choices are proposals until a review asks you to type yes. Blank confirmation never authorizes spending.\n";
            OperatorStep(1, "Choose the provider wallet");
            std::cout << "Use a dedicated provider wallet so its operating balance is easy to manage.\n"
                      << "A new wallet is encrypted. Keep its password: a node restart requires manual unlock.\n";
        }
        std::string wallet = m_args.GetArg("-rpcwallet", "");
        if (!m_args.IsArgSet("-rpcwallet") && !setup) throw std::runtime_error("Select the wallet explicitly with -rpcwallet");
        if (!m_args.IsArgSet("-rpcwallet")) {
            const auto wallets = OperatorCall("listwallets", empty, {});
            std::cout << "Loaded wallets:\n";
            for (const auto& loaded : wallets.getValues()) std::cout << "  - " << loaded.get_str() << '\n';
            wallet = OperatorPrompt("Provider wallet name (enter a new name to create one)");
            if (wallet.empty()) throw std::runtime_error("An explicit non-empty wallet name is required");
            bool exists{false};
            for (const auto& item : wallets.getValues())
                exists |= item.get_str() == wallet;
            if (!exists) {
                if (!OperatorConfirm("Create a new encrypted descriptor wallet named " + wallet + "?")) return EXIT_FAILURE;
                auto pass = OperatorSecret("New wallet passphrase");
                auto repeat = OperatorSecret("Repeat new wallet passphrase");
                if (pass.empty() || pass != repeat) {
                    memory_cleanse(pass.data(), pass.size());
                    memory_cleanse(repeat.data(), repeat.size());
                    throw std::runtime_error("Passphrases must match and must not be empty");
                }
                memory_cleanse(repeat.data(), repeat.size());
                try {
                    OperatorCall("createwallet", OperatorParams({UniValue{wallet}, UniValue{false}, UniValue{false}, UniValue{pass}, UniValue{false}, UniValue{true}, UniValue{true}}), {});
                } catch (...) {
                    memory_cleanse(pass.data(), pass.size());
                    throw;
                }
                memory_cleanse(pass.data(), pass.size());
            }
        }
        auto snapshot = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
        std::cout << "RPC node: " << m_rpc_host << " (configured RPC port)\n";
        if (setup) std::cout << "Selected wallet: " << wallet << " | Network: " << snapshot.find_value("network").get_str() << '\n';
        else std::cout << OperatorSummary(snapshot);
        if (!setup) {
            while (m_args.GetBoolArg("-watch", false)) {
                std::this_thread::sleep_for(std::chrono::seconds{10});
                const auto current = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
                CheckSetupContext(snapshot, current);
                std::cout << "\n"
                          << OperatorSummary(current) << std::flush;
            }
            return EXIT_SUCCESS;
        }
        CheckSetupContext(snapshot, snapshot);
        if (!OperatorConfirm("Use this node, network and provider wallet?")) return EXIT_FAILURE;
        if (snapshot.find_value("provider").find_value("settings_present").isTrue()) {
            const auto action = OperatorSelect("This wallet already has provider settings. What would you like to do?", {
                {"status", "Show current status", "Read the saved state without changing operation or spending permissions."},
                {"resume", "Resume using saved settings", "Keep your offer, identity and limits; explicitly enable and start the provider."},
                {"setup", "Review or change setup", "Walk through saved values. Applying changes pauses the provider during setup."},
                {"pause", "Pause provider", "Disable operation and autostart persistently. Already signed transactions can still confirm."},
                {"unlock", "Unlock for operation", "Choose continuous or timed access to the entire provider wallet."},
                {"backup", "Back up the complete provider wallet", "Protect the identity, settings, pool records and finance history together."}}, "status");
            if (action != "setup") {
                if (action == "pause") {
                    if (!OperatorConfirm("Persistently pause provider and new pool signatures? Signed transactions can still confirm.")) return EXIT_FAILURE;
                    UniValue options{UniValue::VOBJ};
                    options.pushKV("persistent", true);
                    options.pushKV("pause_setup", true);
                    OperatorCall("stoppaymaster", OperatorParams({options}), wallet);
                } else if (action == "resume" || action == "unlock") {
                    OperatorOperatingUnlock(wallet, snapshot);
                    if (action == "resume") {
                        if (!OperatorConfirm("Enable and start within the saved budgets?")) return EXIT_FAILURE;
                        CheckSetupContext(snapshot, OperatorCall("getpaymasteroperatorinfo", empty, wallet));
                        OperatorCall("setpaymasterenabled", OperatorParams({UniValue{true}}), wallet);
                        UniValue options{UniValue::VOBJ};
                        options.pushKV("wait_for_readiness", true);
                        OperatorCall("startpaymaster", OperatorParams({options}), wallet);
                        if (!OperatorAwaitSetup(wallet, snapshot, true)) return EXIT_FAILURE;
                    }
                } else if (action == "backup")
                    OperatorCall("backupwallet", OperatorParams({UniValue{OperatorPrompt("Full-wallet backup destination on the node")}}), wallet);
                else if (action != "status")
                    throw std::runtime_error("Unknown operator action");
                const auto current = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
                CheckSetupContext(snapshot, current);
                OperatorBriefStatus(current);
                return EXIT_SUCCESS;
            }
        }
        OperatorStep(2, "Check how customers reach the node");
        std::cout << "The public endpoint is the address customers use. The bind address is where this node listens locally.\n"
                  << "For 24/7 service, the node must stay online with a working inbound route. Local readiness alone does not prove reachability.\n";
        const auto repair = SetupNodePrerequisites(snapshot);
        std::cout << (repair.empty() ? "Required node options are already configured.\n" : "Some required node options need correction; configuration changes require a node restart.\n");
        if (OperatorSetting("Review connection settings?", !repair.empty(), "Prepare a node-config preview; nothing is written until you approve it.", "Keep the existing bind/endpoint and node options.")) {
            UniValue settings{UniValue::VOBJ};
            const auto& endpoint = snapshot.find_value("provider").find_value("endpoint");
            const auto route = OperatorSelect("Connection type", {
                {"clearnet", "Public IP connection", "Use a reachable public IP and forward/open the dedicated provider port in your router and firewall."},
                {"onion", "Tor onion service", "Choose this if you already operate or will configure a Tor hidden service. Setup does not install or configure Tor."}}, endpoint.isStr() && endpoint.get_str().find(".onion:") != std::string::npos ? "onion" : "clearnet");
            std::cout << "Use a dedicated port, separate from RPC and ordinary peer connections. Enter IPv6 as [address]:port.\n"
                      << "Example local bind: 127.0.0.1:18450 for Tor forwarding; 0.0.0.0:18450 listens on all IPv4 interfaces.\n";
            const auto bind = OperatorPrompt("Local listening IP:port");
            UniValue binds{UniValue::VARR};
            binds.push_back(bind + (route == "onion" ? "=onion" : ""));
            settings.pushKV("paymasterbind", binds);
            settings.pushKV("paymasterendpoint", OperatorPrompt("Public numeric IP:port or onion:port", snapshot.find_value("provider").find_value("endpoint").isStr() ? snapshot.find_value("provider").find_value("endpoint").get_str() : ""));
            if (OperatorSetting("Include missing node prerequisites in the preview?", true,
                "Enable DigiDollar, Paymaster, txindex and v2 transport, disable pruning and reserve sufficient connections. Suitable existing values stay unchanged.",
                "Keep node options unchanged. Missing prerequisites may prevent provider operation.")) {
                const auto defaults = SetupNodePrerequisites(snapshot);
                for (const auto& key : defaults.getKeys())
                    settings.pushKV(key, defaults.find_value(key));
            }
            const auto preview = OperatorCall("preparepaymasternodeconfig", OperatorParams({settings}), {});
            std::cout << preview.write(2) << '\n';
            if (route == "onion") std::cout << "Tor: beneath your service's HiddenServiceDir add HiddenServicePort <announced-port> " << bind << ". Reload Tor yourself. No service is created here.\n";
            if (OperatorConfirm("Back up and apply these node settings?")) {
                UniValue plan{UniValue::VOBJ};
                plan.pushKV("plan_id", preview.find_value("plan_id"));
                plan.pushKV("settings", preview.find_value("settings"));
                std::cout << OperatorCall("applypaymasternodeconfig", OperatorParams({plan}), {}).write(2) << "\nRestart the node explicitly, verify routing, then run -paymastersetup again.\n";
                return EXIT_SUCCESS;
            }
        }
        auto choices = SetupCliDefaults(snapshot);
        const auto provider = snapshot.find_value("provider");
        const bool existing = provider.find_value("settings_present").isTrue();
        std::cout << (existing ? "Saved settings are preselected below; no recommendation overwrites them.\n" : "The selections below propose a small 24/7 provider. Review its finite budgets before approval.\n");
        OperatorStep(3, "Choose your customer offer");
        if (provider.find_value("provider_id").isNull()) {
            do {
                choices.display_name = OperatorPrompt("Optional public name (up to 32 printable ASCII characters; no / or @)", choices.display_name);
                if (IsValidPaymasterDisplayName(choices.display_name)) break;
                std::cout << "Invalid provider name. Leave it empty or use up to 32 printable ASCII characters without / or @.\n";
                choices.display_name.clear();
            } while (true);
        } else {
            std::cout << "Existing provider identity and name remain unchanged.\n";
        }
        OperatorShowOffer(choices.policy);
        if (OperatorCustomize("Customer offer")) {
            const bool paid_before = OperatorAllowsModel(choices.policy, "user_paid");
            const bool sponsored_before = OperatorAllowsModel(choices.policy, "sponsored");
            const auto selected = paid_before ? (sponsored_before ? "both" : "user_paid") :
                choices.policy.find_value("sponsorship_scope").get_str() == "restricted" ? "restricted" : "sponsored";
            const auto mode = OperatorSelect("Who pays for this service?", {
                {"user_paid", "Customer pays a DD service fee (recommended)", "You supply DGB network fees and receive the configured percentage in DD. Income and costs use different assets."},
                {"sponsored", "You sponsor public payments", "You pay the DGB network fee and receive no DD service fee. Anyone may request service within your limits."},
                {"both", "Customer-paid and public sponsored payments", "Keep separate budgets for customer-paid payments and sponsorship open to anyone."},
                {"restricted", "Restricted sponsorship only (advanced)", "Sponsor only authorized requests, with no DD service fee. Cannot be combined with customer-paid service. Separate authorization grants are required; this assistant does not issue them."}}, selected);
            UniValue models{UniValue::VARR};
            if (mode == "user_paid" || mode == "both") models.push_back("user_paid");
            if (mode != "user_paid") models.push_back("sponsored");
            choices.policy.pushKV("funding_models", models);
            choices.policy.pushKV("sponsorship_scope", mode == "restricted" ? "restricted" : "public");
            if (!OperatorAllowsModel(choices.policy, "user_paid")) choices.policy.pushKV("fee_rate_bps", 0);
            do {
                OperatorFields(choices.policy, true, OperatorAllowsModel(choices.policy, "user_paid") ? std::set<std::string>{} : std::set<std::string>{"fee_rate_bps"});
                if (choices.policy.find_value("min_amount_cents").getInt<int64_t>() <= choices.policy.find_value("max_amount_cents").getInt<int64_t>()) break;
                std::cout << "Smallest payment exceeds largest payment. Correct the range; the other choices are retained.\n";
            } while (true);
        }
        const bool paid = OperatorAllowsModel(choices.policy, "user_paid");
        OperatorStep(4, "Limit costs and keep reserves ready");
        std::cout << "Payment fees and reserve-refill fees have SEPARATE budgets. Both can be spent.\n"
                  << "Limits are rolling hour/day ceilings, not a daily target or a fixed fee. Reaching a limit interrupts new work.\n";
        const auto recommended = SetupDefaultSafety(choices.policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>(), paid,
            OperatorAllowsModel(choices.policy, "sponsored"), choices.policy.find_value("sponsorship_scope").get_str() == "restricted");
        const auto active_models = OperatorBudgetModels(choices.policy);
        for (const auto& model : OperatorStoredBudgetModels(choices.policy, choices.safety)) {
            const bool active = std::any_of(active_models.begin(), active_models.end(), [&](const auto& item) { return item.first == model.first; });
            if (!active) std::cout << "Saved inactive limits must also fit the new advertised fee ceiling. Adjust them below if needed; this does not enable the model.\n";
            auto limits = choices.safety.find_value(model.first);
            std::cout << '\n' << model.second << '\n';
            if (limits.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>() == 0) {
                std::cout << "This model has no usable budget yet. The following proposal requires the final approval.\n";
                limits = recommended.find_value(model.first);
            }
            OperatorFields(limits, false);
            bool edit = OperatorCustomize("Payment limits");
            for (;;) {
                if (edit) OperatorFields(limits, true);
                bool disabled = true;
                for (const auto& key : limits.getKeys()) disabled &= limits.find_value(key).getInt<int64_t>() == 0;
                if (!active && disabled) break;
                const auto per = limits.find_value("maximum_network_fee_per_transaction_satoshis").getInt<int64_t>();
                const auto hour = limits.find_value("maximum_network_fee_per_hour_satoshis").getInt<int64_t>();
                const auto count = limits.find_value("maximum_completed_per_hour").getInt<int64_t>();
                if (per > 0 && per <= choices.policy.find_value("maximum_network_fee_dgb_satoshis").getInt<int64_t>() &&
                    limits.find_value("maximum_reserved_network_fee_satoshis").getInt<int64_t>() >= per && hour >= per &&
                    limits.find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>() >= hour && count > 0 &&
                    limits.find_value("maximum_completed_per_day").getInt<int64_t>() >= count) break;
                std::cout << "Correct these limits: per-payment must be positive and within the advertised ceiling; reserved and hourly budgets must cover it; daily limits must cover hourly limits. Payment counts must be positive.\n";
                edit = true;
            }
            choices.safety.pushKV(model.first, limits);
        }
        if (OperatorSetting("Customize advanced offer-request limits?", false,
                "Adjust concurrent offers and per-customer rate limits. Existing limits are displayed before editing.",
                "Keep the saved limits, or the finite defaults for a new provider.")) {
            do {
                OperatorFields(choices.safety, true);
                const auto total = choices.safety.find_value("maximum_active_quotes_total").getInt<int64_t>();
                if (choices.safety.find_value("maximum_active_quotes_per_netgroup").getInt<int64_t>() <= total &&
                    choices.safety.find_value("maximum_active_quotes_per_recipient").getInt<int64_t>() <= total) break;
                std::cout << "Per-network and per-recipient offer limits must not exceed the total. Please correct them.\n";
            } while (true);
        }
        std::cout << "\nReserves are your operating capital, not an expense. Payment reserves determine concurrent capacity.\n"
                  << "Capacity-check reserves prove availability before an offer; payment reserves fund actual transfers.\n";
        if (!paid) {
            std::cout << "Sponsored-only setup uses zero DD reserve targets. Existing DD outputs are not withdrawn by this change.\n";
            choices.liquidity.pushKV("target_admission_carriers", 0);
            choices.liquidity.pushKV("target_operational_carriers", 0);
        }
        OperatorFields(choices.liquidity, false);
        if (OperatorCustomize("Reserve counts and refill fee ceilings")) OperatorFields(choices.liquidity, true);
        for (;;) {
            const int admission = choices.liquidity.find_value("target_admission_carriers").getInt<int>();
            const int operational = choices.liquidity.find_value("target_operational_carriers").getInt<int>();
            if ((paid && admission >= 3 && operational >= 1) ||
                (!paid && admission == 0 && operational == 0)) break;
            std::cout << "Customer-paid service needs at least 3 DD capacity-check reserves and 1 DD payment reserve. Sponsored-only pool preparation requires zero for both DD counts. Review the targets.\n";
            OperatorFields(choices.liquidity, true);
        }
        choices.liquidity.pushKV("automatic_replenishment", OperatorSetting("Maintain reserves automatically? (recommended for 24/7)",
            choices.liquidity.find_value("automatic_replenishment").get_bool(),
            "Reuse confirmed change and maintain the selected targets. Paid replacements additionally require the allowance below.",
            "Handle missing reserves manually; service can stop after its available payment capacity is used."));
        if (choices.liquidity.find_value("automatic_replenishment").isTrue()) {
            choices.liquidity.pushKV("paid_maintenance_approved", OperatorSetting("Include paid refill within the displayed limits?",
                choices.liquidity.find_value("paid_maintenance_approved").get_bool(),
                "The final review will authorize automatic refill transactions within these finite DGB limits.",
                "Only reuse existing confirmed outputs. When this is insufficient, wait for manual approval."));
        }
        while (choices.liquidity.find_value("paid_maintenance_approved").isTrue()) {
            const auto per = choices.liquidity.find_value("maximum_maintenance_fee_per_transaction_satoshis").getInt<int64_t>();
            const auto hour = choices.liquidity.find_value("maximum_maintenance_fee_per_hour_satoshis").getInt<int64_t>();
            if (per > 0 && hour >= per && choices.liquidity.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>() >= hour) break;
            std::cout << "Paid refill needs positive transaction/hour/day limits, with day >= hour >= transaction.\n";
            OperatorFields(choices.liquidity, true);
        }
        for (const auto& names : {std::pair{"admission_dgb_slots", "target_admission_dgb"}, {"operational_dgb_slots", "target_operational_dgb"}, {"admission_carrier_slots", "target_admission_carriers"}, {"operational_carrier_slots", "target_operational_carriers"}})
            choices.pool.pushKV(names.first, choices.liquidity.find_value(names.second));
        choices.pool.pushKV("execute", false);
        OperatorFields(choices.pool, false);
        if (OperatorCustomize("One-time setup fee ceiling")) OperatorFields(choices.pool, true);
        OperatorStep(5, "Choose how the service starts and stays online");
        choices.operation_mode = OperatorSelect("Payment processing", {
            {"automatic", "Automatic (recommended for 24/7)", "Core processes requests and accepted transfers while this node and wallet are available."},
            {"manual", "Manual processing (advanced)", "You must process request and submission queues yourself. Unsuitable for unattended service."}}, choices.operation_mode);
        choices.enabled = OperatorSetting("Enable this provider after saving?", choices.enabled,
            "Allow the approved setup and provider work within the saved limits.", "Keep the provider disabled. New setup signatures and operation remain paused.");
        if (choices.enabled) choices.autostart = OperatorSetting("Autostart whenever this wallet becomes ready? (recommended for 24/7)", *choices.autostart,
            "Start in this node session and after restarts when prerequisites are ready. An encrypted wallet still needs one manual password entry after every restart.",
            "Start the provider manually after each restart. A start requested below applies only to this node session.");
        const bool start_requested = choices.enabled && OperatorSetting("Start when this setup is ready?", !existing || provider.find_value("running").isTrue(),
            "Wait for usable funding and confirmed reserves, then start and verify local readiness.", "Save the configuration without a one-time start request. Saved autostart remains independently effective.");
        CheckSetupChoices(choices);
        OperatorStep(6, "Review and approve the complete configuration");
        std::cout << "Wallet: " << wallet << " | Network: " << snapshot.find_value("network").get_str() << '\n';
        OperatorShowOffer(choices.policy);
        OperatorShowSafety(choices.policy, choices.safety);
        OperatorFields(choices.safety, false);
        int64_t daily_ceiling{0};
        for (const auto& model : OperatorBudgetModels(choices.policy)) daily_ceiling += choices.safety.find_value(model.first).find_value("maximum_network_fee_per_day_satoshis").getInt<int64_t>();
        if (choices.liquidity.find_value("automatic_replenishment").isTrue() && choices.liquidity.find_value("paid_maintenance_approved").isTrue())
            daily_ceiling += choices.liquidity.find_value("maximum_maintenance_fee_per_day_satoshis").getInt<int64_t>();
        std::cout << "Combined rolling-day fee ceilings: " << OperatorAmount(daily_ceiling) << " DGB (payment plus enabled refill budgets; not expected cost).\n";
        std::cout << "Reserve targets and separate maintenance allowance:\n";
        OperatorFields(choices.liquidity, false);
        std::cout << "Automatic reserve maintenance: " << (choices.liquidity.find_value("automatic_replenishment").isTrue() ? "yes" : "no")
                  << " | Paid refill approved: " << (choices.liquidity.find_value("paid_maintenance_approved").isTrue() ? "yes" : "no") << '\n';
        OperatorFields(choices.pool, false);
        std::cout << "Processing: " << choices.operation_mode << " | Enabled: " << (choices.enabled ? "yes" : "no")
                  << " | Autostart: " << (*choices.autostart ? "yes" : "no") << " | Start when ready: " << (start_requested ? "yes" : "no") << '\n'
                  << "Applying settings pauses an existing provider temporarily. Its identity and approved work are retained.\n"
                  << "This approves the displayed recurring budgets. Creating pool capital requires its exact preview approval next.\n";
        if (!OperatorConfirm("Apply this reviewed provider configuration and its displayed permissions?")) return EXIT_FAILURE;
        const auto steps = BuildSetupPlan(snapshot, choices);
        size_t setup_step{0};
        for (const auto& step : steps) {
            std::cout << "\nApplying " << ++setup_step << '/' << steps.size() << ": " << OperatorStepName(step.method) << "...\n" << std::flush;
            const auto current = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
            CheckSetupContext(snapshot, current);
            bool relock{false};
            try {
                if (step.method == "preparepaymasterpool") {
                    const auto preview = OperatorCall(step.method, step.params, wallet);
                    if (!SetupPoolNeeded(preview)) {
                        std::cout << "Pool targets are already covered; no additional funding approval.\n";
                        continue;
                    }
                    std::cout << "Capital to reserve: " << OperatorAmount(preview.find_value("total_output_satoshis").getInt<int64_t>())
                              << " DGB + " << SetupFormatNumber(preview.find_value("total_carrier_cents").getInt<int64_t>(), 2)
                              << " DD. This remains wallet-owned.\nMaximum total setup network fees: "
                              << OperatorAmount(preview.find_value("maximum_total_fee_satoshis").getInt<int64_t>())
                              << " DGB. This is the separate spending ceiling, not a fee estimate.\n"
                              << "Confirmed and already pending reserves are included; only missing reserves are proposed.\n";
                    if (!OperatorConfirm("Authorize exactly this finite setup? Already accepted work keeps its existing authority.")) throw std::runtime_error("Funding not approved. Provider remains safely paused; saved policy changes are retained.");
                    auto options = step.params[0];
                    options.pushKV("plan_id", preview.find_value("plan_id"));
                    options.pushKV("execute", true);
                    if (current.find_value("provider").find_value("wallet_locked").isTrue()) {
                        OperatorUnlock(wallet, 300, false, &current);
                        relock = true;
                    }
                    const auto result = OperatorCall(step.method, OperatorParams({options}), wallet);
                    if (!result.find_value("accepted").isTrue() || result.find_value("plan_id").write() != preview.find_value("plan_id").write()) throw std::runtime_error("Core did not confirm the exact funding approval");
                    std::cout << "Funding accepted; confirmation and readiness are separate.\n";
                } else {
                    if (step.unlock && current.find_value("provider").find_value("wallet_locked").isTrue()) {
                        OperatorUnlock(wallet, 300, false, &current);
                        relock = true;
                    }
                    const auto result = OperatorCall(step.method, step.params, wallet);
                    CheckSetupReply(step, result);
                    if (step.method == "createpaymasteridentity") {
                        auto bound_provider = snapshot.find_value("provider");
                        bound_provider.pushKV("provider_id", result.find_value("provider_id"));
                        snapshot.pushKV("provider", bound_provider);
                    }
                }
                if (relock) OperatorCall("walletlock", empty, wallet);
            } catch (...) {
                if (relock) {
                    try {
                        OperatorCall("walletlock", empty, wallet);
                    } catch (...) {
                    }
                }
                throw;
            }
            const auto after = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
            CheckSetupContext(snapshot, after);
            CheckSetupSaved(step, after);
            snapshot = after;
            std::cout << "Saved and verified: " << OperatorStepName(step.method) << '\n';
        }
        OperatorStep(7, "Fund, unlock and verify completion");
        std::cout << "DGB funds network fees and reserves. Customer-paid service also needs DD reserve capital.\n"
                  << "Send funds from a separate wallet. This assistant only receives; it never transfers funds from another wallet.\n";
        const auto funding = OperatorSelect("Funding assistance", {
            {"addresses", "Show new DGB and DD receiving addresses", "Generate both addresses and monitor incoming funds and reserve confirmations."},
            {"watch", "Monitor existing funding", "Use addresses you already have and wait for approved setup to finish."},
            {"skip", "Skip receiving-address assistance", "No addresses are generated. A requested start still waits for usable funding and readiness."}},
            choices.enabled ? (snapshot.find_value("provider").find_value("pool_ready").isTrue() ? "watch" : "addresses") : "skip");
        if (funding != "skip" || start_requested) {
            const auto current = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
            CheckSetupContext(snapshot, current);
            snapshot = current;
            if (!snapshot.find_value("provider").find_value("enabled").isTrue()) {
                if (!OperatorConfirm("Enable the saved provider configuration so approved setup can continue?")) return EXIT_FAILURE;
                CheckSetupContext(snapshot, OperatorCall("getpaymasteroperatorinfo", empty, wallet));
                OperatorCall("setpaymasterenabled", OperatorParams({UniValue{true}}), wallet);
            }
            OperatorOperatingUnlock(wallet, snapshot);
        }
        if (funding == "addresses") OperatorFundingAddresses(wallet, snapshot);
        if (OperatorConfirm("Create a full-wallet backup now? (path is on the node host)")) OperatorCall("backupwallet", OperatorParams({UniValue{OperatorPrompt("Backup destination")}}), wallet);
        if (start_requested) {
            CheckSetupContext(snapshot, OperatorCall("getpaymasteroperatorinfo", empty, wallet));
            UniValue options{UniValue::VOBJ};
            options.pushKV("wait_for_readiness", true);
            try {
                OperatorCall("startpaymaster", OperatorParams({options}), wallet);
            } catch (const std::exception&) {
                // A lost reply must not issue a second start. Reconcile the accepted
                // volatile request or running provider through the read-only view.
                const auto reconciled = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
                CheckSetupContext(snapshot, reconciled);
                const auto& state = reconciled.find_value("provider");
                if (!state.find_value("start_requested").isTrue() && !state.find_value("running").isTrue()) throw;
            }
        }
        if ((funding != "skip" || start_requested) && !OperatorAwaitSetup(wallet, snapshot, start_requested)) return EXIT_FAILURE;
        const auto final_status = OperatorCall("getpaymasteroperatorinfo", empty, wallet);
        CheckSetupContext(snapshot, final_status);
        OperatorBriefStatus(final_status);
        if (!start_requested) std::cout << "Configuration saved. No one-time start was requested; the saved autostart setting shown above remains effective.\n";
        std::cout << "Verify external reachability from an independent node; local readiness is not an external payment test.\n";
        return EXIT_SUCCESS;
    }
};
} // namespace

int RunCli(bool setup, const ArgsManager& args, const std::string& rpc_host, const CliRpc& rpc)
{
    return OperatorCli(args, rpc_host, rpc).Run(setup);
}
} // namespace DigiDollar::Paymaster
