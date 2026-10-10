#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Recover current provider capital without reopening restored signing authority.

Use only disposable SQLite wallets on regtest. The independent checkpoint stays
current throughout; stale/withheld-signature cases belong to wallet_paymaster_backup.
"""

from pathlib import Path

from test_framework.paymaster import PaymasterFunctionalHarness, paymaster_node_args
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PaymasterProviderBackupExitTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [paymaster_node_args(provider_node=0), paymaster_node_args()]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def wait_chain_ready(self):
        self.sync_blocks()
        for node in self.nodes:
            self.wait_until(lambda node=node: (
                node.getindexinfo().get("txindex", {}).get("synced", False)
                and node.getindexinfo()["txindex"]["best_block_height"] == node.getblockcount()))
            node.syncwithvalidationinterfacequeue()

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        provider, client = harness.create_wallets()
        harness.fund_and_configure()
        targets = {"admission_dgb_slots": 3, "operational_dgb_slots": 1,
                   "admission_carrier_slots": 3, "operational_carrier_slots": 1}
        self.wait_chain_ready()
        stopped = provider.stoppaymaster({"persistent": True, "pause_setup": True})
        assert_equal(stopped["running"], False)
        pool_before = provider.getpaymasterpoolinfo()["pool"]
        available = [entry for entry in pool_before if entry["state"] == "available"]
        assert_equal(len(available), 8)
        backup = str(Path(self.options.tmpdir) / "current-provider.sqlite")
        provider.backupwallet(backup)
        self.nodes[0].unloadwallet("provider", load_on_startup=False)
        self.nodes[0].restorewallet("restored_provider", backup, load_on_startup=True)
        self.wait_chain_ready()
        restored = self.nodes[0].get_wallet_rpc("restored_provider")
        cli = self.nodes[0].cli("-rpcwallet=restored_provider")
        gate = "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"
        before = restored.getpaymasterpoolinfo()
        transaction_count = restored.getwalletinfo()["txcount"]
        mempool_before = set(self.nodes[0].getrawmempool())

        self.log.info("Pure RPC/CLI recovery reviews retain the restore guard")
        for api in (restored, cli):
            assert_raises_rpc_error(-4, gate, api.releasepaymastercapital)
        preview = restored.releasepaymastercapital({"recovery": True})
        assert_equal(cli.releasepaymastercapital({"recovery": True}), preview)
        assert_equal(preview["executed"], False)
        assert_equal(preview["recovery_release"], True)
        assert_equal(preview["pool_entries"], len(available))
        assert preview["dgb_satoshis"] > 0
        assert_equal(preview["dd_cents"], 400)
        assert_equal(preview["network_fee_satoshis"], 0)
        assert_equal(restored.getpaymasterpoolinfo(), before)
        assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)

        dgb_entry = next(entry for entry in available if entry["asset"] == "dgb")
        outpoint = {key: dgb_entry[key] for key in ("txid", "vout")}
        assert restored.lockunspent(False, [outpoint])
        assert_raises_rpc_error(-4, "PAYMASTER_CAPITAL_COIN_MANUALLY_LOCKED",
                                cli.releasepaymastercapital, {"recovery": True})
        assert restored.lockunspent(True, [outpoint])
        execute = {"recovery": True, "execute": True, "plan_id": preview["plan_id"]}
        assert_raises_rpc_error(-4, "PAYMASTER_CAPITAL_PLAN_CHANGED", cli.releasepaymastercapital,
                                dict(execute, plan_id="00" * 32))

        self.log.info("Explicit CLI execution retires the identity without a transaction")
        receipt = cli.releasepaymastercapital(execute)
        assert_equal(receipt, dict(preview, executed=True))
        assert_equal(restored.getwalletinfo()["txcount"], transaction_count)
        assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)
        pool_after = restored.getpaymasterpoolinfo()["pool"]
        assert_equal(len(pool_after), len(pool_before))
        assert all(entry["state"] == "released" for entry in pool_after)
        assert_raises_rpc_error(-4, gate, cli.startpaymaster)
        assert_raises_rpc_error(-4, gate, cli.preparepaymasterpool, targets)
        assert_raises_rpc_error(-4, "PAYMASTER_CAPITAL_PLAN_CHANGED", cli.releasepaymastercapital, execute)
        assert_equal(restored.releasepaymastercapital({"recovery": True})["pool_entries"], 0)

        self.log.info("Restart preserves the release; an unguarded original copy cannot resume")
        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.wait_chain_ready()
        restored = self.nodes[0].get_wallet_rpc("restored_provider")
        assert all(entry["state"] == "released" for entry in restored.getpaymasterpoolinfo()["pool"])
        assert gate in restored.getpaymasterinfo()["readiness_errors"]
        self.nodes[0].loadwallet("provider", load_on_startup=False)
        original = self.nodes[0].get_wallet_rpc("provider")
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", original.startpaymaster)
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", original.preparepaymasterpool, targets)
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", original.releasepaymastercapital)
        self.nodes[0].unloadwallet("provider", load_on_startup=False)

        self.log.info("Released DD can be swept through normal Send using ordinary DGB")
        # The quarantine must still reject Paymaster authority while allowing an
        # ordinary complete DD spend. No unknown signature is force-unlocked.
        amount = restored.getdigidollarbalance()["confirmed"]
        assert_equal(amount, 800)
        sent = restored.senddigidollar(client.getdigidollaraddress(), amount)
        assert sent["txid"] in self.nodes[0].getrawmempool()
        self.generatetoaddress(self.nodes[0], 1, restored.getnewaddress())
        self.wait_chain_ready()
        assert_equal(client.getdigidollarbalance()["total"], amount)
        assert_equal(restored.getdigidollarbalance()["confirmed"], 0)
        assert gate in restored.getpaymasterinfo()["readiness_errors"]


if __name__ == "__main__":
    PaymasterProviderBackupExitTest().main()
