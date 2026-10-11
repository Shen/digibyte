#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Adopt a current SQLite provider backup without resetting financial authority.

The independently retained node checkpoint stays current. RPC/CLI previews,
explicit confirmation, old-copy fencing, restart and a new payment are checked.
"""

from pathlib import Path

from test_framework.paymaster import PaymasterFunctionalHarness
from test_framework.util import assert_equal, assert_raises_rpc_error
from wallet_paymaster_backup_exit import PaymasterProviderBackupExitTest


class PaymasterBackupAdoptionTest(PaymasterProviderBackupExitTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        provider, client = harness.create_wallets()
        harness.fund_and_configure()
        harness.fund_client_dd(300)
        self.wait_chain_ready()
        assert_equal(provider.stoppaymaster({"persistent": True, "pause_setup": True})["running"], False)
        backup = str(Path(self.options.tmpdir) / "current-provider.sqlite")
        provider.backupwallet(backup)
        safety_before = provider.getpaymastersafetystatus()
        self.nodes[0].unloadwallet("provider", load_on_startup=False)
        self.nodes[0].restorewallet("adopted", backup, load_on_startup=True)
        self.wait_chain_ready()
        restored = self.nodes[0].get_wallet_rpc("adopted")
        cli = self.nodes[0].cli("-rpcwallet=adopted")
        gate = "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"
        assert_raises_rpc_error(-4, gate, restored.startpaymaster)
        pool_before = restored.getpaymasterpoolinfo()["pool"]
        tx_count = restored.getwalletinfo()["txcount"]
        mempool_before = set(self.nodes[0].getrawmempool())
        preview = restored.adoptpaymasterbackup()
        assert_equal(cli.adoptpaymasterbackup(), preview)
        assert_equal(preview["executed"], False)
        assert_equal(preview["provider_stopped"], True)
        assert_equal(preview["network_fee_satoshis"], 0)
        assert_equal(restored.getpaymasterpoolinfo()["pool"], pool_before)
        execute = {"execute": True, "plan_id": preview["plan_id"]}
        assert_raises_rpc_error(-8, "PAYMASTER_BACKUP_INDEPENDENT_CHECKPOINT_REQUIRED",
                                cli.adoptpaymasterbackup, execute)
        execute["independent_checkpoint"] = True
        assert_raises_rpc_error(-4, "PAYMASTER_BACKUP_PLAN_CHANGED", cli.adoptpaymasterbackup,
                                dict(execute, plan_id="00" * 32))
        entry = next(entry for entry in pool_before if entry["state"] == "available" and entry["asset"] == "dgb")
        outpoint = {key: entry[key] for key in ("txid", "vout")}
        assert restored.lockunspent(False, [outpoint])
        assert_raises_rpc_error(-4, "PAYMASTER_CAPITAL_COIN_MANUALLY_LOCKED", restored.adoptpaymasterbackup)
        assert restored.lockunspent(True, [outpoint])
        receipt = cli.adoptpaymasterbackup(execute)
        assert_equal(receipt, dict(preview, executed=True))
        assert_equal(restored.getpaymasterpoolinfo()["pool"], pool_before)
        assert_equal(restored.getpaymastersafetystatus(), safety_before)
        assert_equal(restored.getwalletinfo()["txcount"], tx_count)
        assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)
        assert_raises_rpc_error(-4, "PAYMASTER_CAPITAL_RECOVERY_REQUIRES_RESTORED_WALLET",
                                cli.adoptpaymasterbackup, execute)

        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.wait_chain_ready()
        restored = self.nodes[0].get_wallet_rpc("adopted")
        assert gate not in restored.getpaymasterinfo()["readiness_errors"]
        assert_equal(restored.getpaymasterinfo()["running"], False)
        self.nodes[0].loadwallet("provider", load_on_startup=False)
        original = self.nodes[0].get_wallet_rpc("provider")
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", original.startpaymaster)
        self.nodes[0].unloadwallet("provider", load_on_startup=False)
        # A stale explicitly restored copy also cannot enter this adoption path.
        self.nodes[0].restorewallet("stale", backup, load_on_startup=False)
        stale = self.nodes[0].get_wallet_rpc("stale")
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED", stale.adoptpaymasterbackup)
        self.nodes[0].unloadwallet("stale", load_on_startup=False)

        self.log.info("Separate start and a new signed payment succeed after adoption")
        harness.provider = restored
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=adopted")
        restored.setpaymasterenabled(True)
        assert_equal(restored.startpaymaster()["running"], True)
        recipient = client.getdigidollaraddress()
        options, _, send = harness.wait_for_quote("550e8400-e29b-41d4-a716-446655440181", recipient, 100)
        authorized = harness.authorize_quote(options, send)
        self.wait_until(lambda: send().get("queued", False))
        committed = restored.submitpaymasterdigidollar(authorized["psbt"])
        assert_equal(committed["broadcast"], True)
        harness.deliver_committed_result(options["request_id"], committed["txid"])
        self.generatetoaddress(self.nodes[0], 1, restored.getnewaddress())
        self.wait_chain_ready()
        assert gate not in restored.getpaymasterinfo()["readiness_errors"]


if __name__ == "__main__":
    PaymasterBackupAdoptionTest().main()
