#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see COPYING.
"""Operator status, persistent pause, config binding and nonfinancial Direct probe."""
from pathlib import Path
import subprocess
import json

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

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("operator")
        wallet = node.get_wallet_rpc("operator")
        self.generate(node, 1)
        self.sync_all()
        status = wallet.getpaymasteroperatorinfo()
        assert_equal(status["schema_version"], 1)
        assert_equal(status["network"], "regtest")
        assert_equal(status["wallet"], "operator")
        assert_equal(status["provider"]["settings_present"], False)
        assert any(item["code"] == "PAYMASTER_EXTERNAL_REACHABILITY_UNKNOWN" for item in status["diagnostics"])

        self.log.info("Persistent pause survives reload and preserves identity and policy")
        identity = wallet.createpaymasteridentity("Operator")
        policy = {
            "funding_models": ["sponsored"], "sponsorship_scope": "public",
            "fee_rate_bps": 0, "min_amount_cents": 100, "max_amount_cents": 100000,
            "quote_ttl": 60, "maximum_network_fee_dgb_satoshis": 20000000,
        }
        wallet.setpaymasterpolicy(policy)
        wallet.setpaymastersafetypolicy(provider_safety_policy(["sponsored"]))
        wallet.setpaymasterenabled(True)
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
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["transport"]["outbound_limit"], 1)
        self.restart_node(0)
        if "operator" not in node.listwallets():
            node.loadwallet("operator")
        # Restart rotates the RPC cookie; wallet proxies keep their old credentials.
        wallet = node.get_wallet_rpc("operator")
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["transport"]["outbound_limit"], 2)
        assert_equal(wallet.getpaymasteroperatorinfo()["provider"]["enabled"], False)
        updated = config.read_bytes()
        config.write_bytes(updated + b"\n[regtest]\npaymastermaxoutbound=3\n")
        assert_raises_rpc_error(-8, "duplicate", node.preparepaymasternodeconfig, {"paymastermaxoutbound": 1})
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
        config.write_bytes(updated)
        settings = node.chain_path / "settings.json"
        settings_original = settings.read_bytes() if settings.exists() else None
        try:
            prior = json.loads(settings_original or "{}")
            prior["paymastermaxoutbound"] = 3
            settings.write_text(json.dumps(prior), encoding="utf-8")
            assert_raises_rpc_error(-8, "settings.json", node.preparepaymasternodeconfig, {"paymastermaxoutbound": 1})
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
