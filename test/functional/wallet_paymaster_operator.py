#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see COPYING.
"""Operator status, persistent pause, config binding and nonfinancial Direct probe."""
from pathlib import Path
import subprocess
import json
import time

from test_framework.paymaster import paymaster_node_args, paymaster_port, provider_safety_policy
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port


class PaymasterOperatorTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args() + ["-digidollaractivationheight=0"],
            paymaster_node_args(1) + ["-digidollaractivationheight=0"],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def check_operating_unlock(self):
        self.log.info("Paymaster-only continuous unlock remains volatile and replaces old timers")
        node = self.nodes[0]
        node.createwallet("operating-unlock", passphrase="test passphrase")
        provider = node.get_wallet_rpc("operating-unlock")
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_CONFIGURATION_REQUIRED",
                                provider.walletpassphrase, "test passphrase", 0, True)
        provider.walletpassphrase("test passphrase", 60)
        provider.createpaymasteridentity("Encrypted operator")
        policy = {
            "funding_models": ["sponsored"], "sponsorship_scope": "public",
            "fee_rate_bps": 0, "min_amount_cents": 100, "max_amount_cents": 10000,
            "quote_ttl": 60, "maximum_network_fee_dgb_satoshis": 20000000,
        }
        provider.setpaymasterpolicy(policy)
        provider.setpaymasterenabled(False)
        assert_raises_rpc_error(-8, "requires timeout=0", provider.walletpassphrase,
                                "test passphrase", 60, True)
        provider.walletpassphrase("test passphrase", 1)
        # Exercise CLI conversion of the optional third boolean as well.
        # Native and --usecli runs must not put even fixture passphrases in
        # process arguments or the framework's command-vector debug log.
        node.cli("-rpcwallet=operating-unlock", "-stdin",
                 input="test passphrase\n0\ntrue\n").walletpassphrase()
        assert_equal(provider.getwalletinfo()["unlocked_until"], -1)
        assert_equal(provider.getpaymasteroperatorinfo()["provider"]["wallet_locked"], False)
        assert_equal(provider.getpaymasteroperatorinfo()["provider"]["enabled"], False)
        time.sleep(2)  # The superseded one-second relock callback must not lock this lease.
        assert_equal(provider.getpaymasteroperatorinfo()["provider"]["wallet_locked"], False)
        assert_raises_rpc_error(-14, "incorrect", provider.walletpassphrase, "wrong", 0, True)
        assert_equal(provider.getwalletinfo()["unlocked_until"], -1)
        provider.walletlock()
        assert_equal(provider.getwalletinfo()["unlocked_until"], 0)
        provider.walletpassphrase("test passphrase", 0, True)
        provider.walletpassphrase("test passphrase", 1)
        self.wait_until(lambda: provider.getwalletinfo()["unlocked_until"] == 0)
        provider.walletpassphrase("test passphrase", 0, True)
        node.unloadwallet("operating-unlock")
        node.loadwallet("operating-unlock")
        assert_equal(provider.getpaymasteroperatorinfo()["provider"]["wallet_locked"], True)
        provider.walletpassphrase("test passphrase", 0, True)
        self.restart_node(0)
        self.connect_nodes(0, 1)
        provider = node.get_wallet_rpc("operating-unlock")
        if "operating-unlock" not in node.listwallets():
            node.loadwallet("operating-unlock")
        assert_equal(provider.getpaymasteroperatorinfo()["provider"]["wallet_locked"], True)
        provider.walletpassphrase("test passphrase", 0)
        self.wait_until(lambda: provider.getpaymasteroperatorinfo()["provider"]["wallet_locked"])
        node.unloadwallet("operating-unlock")

    def run_test(self):
        self.check_operating_unlock()
        node = self.nodes[0]
        node.createwallet("operator")
        wallet = node.get_wallet_rpc("operator")
        self.generate(node, 1)
        self.sync_all()
        status = wallet.getpaymasteroperatorinfo()
        assert_equal(status["schema_version"], 1)
        assert_equal(status["network"], "regtest")
        for option in ("digidollar", "paymaster", "prune", "txindex", "v2transport"):
            assert_equal(status["node_setting_overrides"][option], "command_line")
        assert_equal(status["node_settings"]["paymasterendpoint"], "")
        assert "paymasterbind" not in status["node_setting_overrides"]
        node_settings = node.getpaymasternodeconfig()
        assert_equal(node_settings["network"], "regtest")
        for option in ("digidollar", "paymaster", "prune", "txindex", "v2transport"):
            assert_equal(node_settings["fields"][option]["source"], "command_line")
            assert_equal(node_settings["fields"][option]["editable"], False)
        assert_equal(node_settings["fields"]["paymasterbind"]["editable"], True)

        assert_equal(status["wallet"], "operator")
        assert_equal(status["provider"]["settings_present"], False)
        assert any(item["code"] == "PAYMASTER_EXTERNAL_REACHABILITY_UNKNOWN" for item in status["diagnostics"])

        self.log.info("Persistent pause survives reload and preserves identity and policy")
        policy = {
            "funding_models": ["sponsored"], "sponsorship_scope": "public",
            "fee_rate_bps": 0, "min_amount_cents": 100, "max_amount_cents": 100000,
            "quote_ttl": 60, "maximum_network_fee_dgb_satoshis": 20000000,
        }
        self.log.info("A proposed-policy setup preview writes no identity, settings, pool or transaction")
        targets = {"admission_dgb_slots": 3, "operational_dgb_slots": 1,
                   "admission_carrier_slots": 0, "operational_carrier_slots": 0,
                   "maximum_fee_satoshis": 20_000_000}
        mempool_before = node.getrawmempool()
        preview = wallet.preparepaymasterpool(dict(targets, preview_policy=policy))
        assert_equal(preview["preview_only"], True)
        assert_equal(preview["accepted"], False)
        assert_equal(preview["total_carrier_cents"], 0)
        assert_equal(preview["maximum_total_fee_satoshis"], 20_000_000)
        unchanged = wallet.getpaymasteroperatorinfo()["provider"]
        assert_equal(unchanged["settings_present"], False)
        assert "provider_id" not in unchanged
        assert_equal(unchanged["active_operations"], [])
        assert_equal(node.getrawmempool(), mempool_before)
        assert_raises_rpc_error(-8, "PAYMASTER_SETUP_PREVIEW_ONLY", wallet.preparepaymasterpool,
                                dict(targets, preview_policy=policy, execute=True, plan_id=preview["plan_id"]))
        identity = wallet.createpaymasteridentity("Operator")
        wallet.setpaymasterpolicy(policy)
        wallet.setpaymastersafetypolicy(provider_safety_policy(["sponsored"]))
        wallet.setpaymasterenabled(True)
        wallet.setpaymasterruntimesettings({"autostart": False})
        self.log.info("Refill intent is revision-bound and survives runtime transitions")
        refill = {
            "automatic_replenishment": True, "paid_maintenance_approved": True,
            "target_admission_dgb": 3, "target_operational_dgb": 1,
            "target_admission_carriers": 0, "target_operational_carriers": 0,
            "maximum_maintenance_fee_per_transaction_satoshis": 10_000_000,
            "maximum_maintenance_fee_per_hour_satoshis": 50_000_000,
            "maximum_maintenance_fee_per_day_satoshis": 200_000_000,
        }
        saved = wallet.setpaymasterliquiditypolicy(refill, 0)
        assert_raises_rpc_error(-4, "PAYMASTER_LIQUIDITY_POLICY_CHANGED",
                                wallet.setpaymasterliquiditypolicy, dict(refill, automatic_replenishment=False), 0)
        assert_equal(wallet.getpaymasterliquiditystatus()["policy"], saved)
        # Exercise optional numeric argument conversion through the actual CLI.
        updated = node.cli("-rpcwallet=operator").setpaymasterliquiditypolicy(refill, saved["updated_at"])
        assert updated["updated_at"] > saved["updated_at"]
        assert_equal(wallet.getpaymasterliquiditystatus()["automation_status"]["enabled"], True)
        assert_equal(wallet.getpaymasterliquiditystatus()["automation_status"]["state"], "paused")

        self.log.info("One-shot start waits without changing autostart and is cleared by pause and unload")
        waiting = wallet.startpaymaster({"wait_for_readiness": True})
        assert_equal(waiting["running"], False)
        assert_equal(waiting["start_requested"], True)
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["start_requested"], True)
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["autostart"], False)
        wallet.stoppaymaster()
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["start_requested"], False)
        wallet.startpaymaster({"wait_for_readiness": True})
        node.unloadwallet("operator")
        node.loadwallet("operator")
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["start_requested"], False)
        wallet.startpaymaster({"wait_for_readiness": True})
        self.restart_node(0)
        node = self.nodes[0]
        # Wallet loading is independent of provider autostart and one-shot start requests.
        if "operator" not in node.listwallets():
            node.loadwallet("operator")
        wallet = node.get_wallet_rpc("operator")
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["start_requested"], False)
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["autostart"], False)
        wallet.setpaymasterruntimesettings({"autostart": True})
        assert_raises_rpc_error(-8, "requires persistent", wallet.stoppaymaster, {"pause_setup": True})
        assert_equal(wallet.stoppaymaster({"persistent": True, "pause_setup": True})["running"], False)
        before = wallet.getpaymasteroperatorinfo()
        assert_equal(before["provider"]["enabled"], False)
        assert_equal(before["provider"]["autostart"], False)
        node.unloadwallet("operator")
        node.loadwallet("operator")
        after = wallet.getpaymasteroperatorinfo()
        assert before["wallet_generation"] != after["wallet_generation"]
        assert_equal(after["provider"]["provider_id"], identity["provider_id"])
        assert_equal(after["provider"]["policy"], before["provider"]["policy"])
        assert_equal(after["provider"]["enabled"], False)
        assert_equal(after["provider"]["autostart"], False)
        assert_equal(after["provider"]["liquidity"]["policy"]["automatic_replenishment"], True)
        assert_equal(after["provider"]["liquidity"]["policy"]["paid_maintenance_approved"], True)
        assert_equal(after["provider"]["liquidity"]["automation_status"]["state"], "paused")


        self.log.info("An explicit transport check neither signs nor funds provider work")
        transactions = wallet.getwalletinfo()["txcount"]
        checked = node.checkpaymasterendpoint(f"127.0.0.1:{paymaster_port(1)}")
        assert_equal(checked["transport_ready"], True)
        assert_equal(checked["payment_verified"], False)
        assert_equal(checked["identity_verified"], False)
        assert_equal(wallet.getwalletinfo()["txcount"], transactions)
        self.wait_until(lambda: wallet.getpaymasteroperatorinfo()["provider"]["transport"]["outbound_in_use"] == 0)
        assert_raises_rpc_error(-1, "RATE_LIMIT", node.checkpaymasterendpoint, f"127.0.0.1:{paymaster_port(1)}")
        assert_raises_rpc_error(-8, "ENDPOINT_INVALID", node.checkpaymasterendpoint, "example.org:12033")

        self.log.info("Config preview is read-only and rejects stale files and overrides")
        config = node.datadir_path / "digibyte.conf"
        original = config.read_bytes()
        preview = node.preparepaymasternodeconfig({"paymastermaxoutbound": 2})
        assert_equal(config.read_bytes(), original)
        plan = {"settings": preview["settings"], "plan_id": preview["plan_id"]}
        config.write_bytes(original + b"\n# concurrent operator edit\n")
        assert_raises_rpc_error(-8, "PLAN_CHANGED", node.applypaymasternodeconfig, plan)
        config.write_bytes(original)
        assert_raises_rpc_error(-8, "command line", node.preparepaymasternodeconfig, {"v2transport": 1})
        assert_raises_rpc_error(-8, "UNKNOWN_OR_DUPLICATE", node.preparepaymasternodeconfig, {"rpcpassword": "secret"})
        result = node.applypaymasternodeconfig(plan)
        assert_equal(result["applied"], True)
        assert_equal(Path(result["backup"]).read_bytes(), original)
        pending = node.getpaymasternodeconfig()["fields"]["paymastermaxoutbound"]
        assert_equal(pending["value"], 1)
        assert_equal(pending["configured_value"], 2)
        assert_equal(pending["restart_required"], True)

        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["transport"]["outbound_limit"], 1)
        self.restart_node(0)
        if "operator" not in node.listwallets():
            node.loadwallet("operator")
        # Restart rotates the RPC cookie; wallet proxies keep their old credentials.
        wallet = node.get_wallet_rpc("operator")
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["transport"]["outbound_limit"], 2)
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["enabled"], False)
        updated = config.read_bytes()
        config.write_bytes(original)
        removed = node.getpaymasternodeconfig()["fields"]["paymastermaxoutbound"]
        assert_equal(removed["source"], "loaded_configuration")
        assert_equal(removed["value"], 2)
        assert_equal(removed["configured_value"], 1)
        assert_equal(removed["restart_required"], True)
        config.write_bytes(updated + b"\n[regtest]\npaymastermaxoutbound=3\n")
        assert_raises_rpc_error(-8, "duplicate", node.preparepaymasternodeconfig, {"paymastermaxoutbound": 1})
        assert_equal(node.getpaymasternodeconfig()["fields"]["paymastermaxoutbound"]["editable"], False)

        config.write_bytes(updated + b"\n[regtest]\nincludeconf=operator.conf\n")
        assert_raises_rpc_error(-8, "INCLUDED_FILE", node.preparepaymasternodeconfig, {"paymastermaxoutbound": 1})
        config.write_bytes(updated)

        self.log.info("Unrelated includes remain intact, but all preview inputs are bound")
        included = node.datadir_path / "operator.conf"
        included.write_text("[regtest]\nmaxmempool=300\n", encoding="utf-8")
        config.write_bytes(updated + b"\n[regtest]\nincludeconf=operator.conf\n")
        preview = node.preparepaymasternodeconfig({"paymastermaxoutbound": 1})
        included.write_text("[regtest]\nmaxmempool=301\n", encoding="utf-8")
        assert_raises_rpc_error(-8, "PLAN_CHANGED", node.applypaymasternodeconfig, {"plan_id": preview["plan_id"], "settings": preview["settings"]})
        included.write_text("[regtest]\npaymastermaxoutbound=3\n", encoding="utf-8")
        assert_raises_rpc_error(-8, "included file", node.preparepaymasternodeconfig, {"paymastermaxoutbound": 1})
        included_field = node.getpaymasternodeconfig()["fields"]["paymastermaxoutbound"]
        assert_equal(included_field["editable"], False)
        assert "included file" in included_field["reason"]
        included.write_text("paymasterbind=127.0.0.1:18499\n", encoding="utf-8")
        assert_equal(node.getpaymasternodeconfig()["fields"]["paymasterbind"]["editable"], False)
        assert_raises_rpc_error(-8, "included file", node.preparepaymasternodeconfig, {"paymasterbind": ["127.0.0.1:18498"]})

        config.write_bytes(updated)
        settings = node.chain_path / "settings.json"
        settings_original = settings.read_bytes() if settings.exists() else None
        try:
            prior = json.loads(settings_original or "{}")
            prior["paymastermaxoutbound"] = 3
            settings.write_text(json.dumps(prior), encoding="utf-8")
            assert_raises_rpc_error(-8, "settings.json", node.preparepaymasternodeconfig, {"paymastermaxoutbound": 1})
            disk_setting = node.getpaymasternodeconfig()["fields"]["paymastermaxoutbound"]
            assert_equal(disk_setting["value"], 2)
            assert_equal(disk_setting["source"], "loaded_configuration")
            assert_equal(disk_setting["editable"], False)
            assert "settings.json" in disk_setting["reason"]
        finally:
            if settings_original is None:
                settings.unlink()
            else:
                settings.write_bytes(settings_original)
        # An active bind from command-line configuration blocks automatic bind replacement.
        assert_raises_rpc_error(-8, "PORT_CONFLICT", node.preparepaymasternodeconfig, {"paymasterbind": [f"127.0.0.1:{p2p_port(0)}"]})

        self.log.info("CLI refuses unattended setup before making RPC mutations")
        cli = node.cli.binary
        refused = subprocess.run([cli, f"-datadir={node.datadir_path}", "-paymastersetup"], input="", text=True, capture_output=True, timeout=30)
        assert refused.returncode != 0
        assert "interactive terminal" in refused.stderr


if __name__ == "__main__":
    PaymasterOperatorTest().main()
