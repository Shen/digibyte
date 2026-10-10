#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A stale provider SQLite backup must not renew forgotten spending authority.

Use real signed payments, a confirmed spend and a mempool spend, and an unused
operational slot. A rescan which only notices spent pool inputs is insufficient:
the remaining slot must not become permission to spend the daily budget again.
All wallets and nodes are disposable regtest fixtures.
"""

from decimal import Decimal
import json
from shutil import copyfile

from test_framework.authproxy import JSONRPCException
from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    confirm_pool_preparation,
    default_liquidity_policy,
    paymaster_node_args,
    provider_safety_policy,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PaymasterProviderBackupTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [paymaster_node_args(provider_node=0), paymaster_node_args()]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)
        parser.add_argument("--pending-offline", action="store_true",
                            help="Restore a backup before the second payment with its signed transaction absent from both mempools")

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

    def commit_payment(self, harness, request_id, recipient):
        options, _, send = harness.wait_for_quote(request_id, recipient, 100)
        authorized = harness.authorize_quote(options, send)
        self.wait_until(lambda: send().get("queued", False))
        committed = harness.provider.submitpaymasterdigidollar(authorized["psbt"])
        assert_equal(committed["broadcast"], True)
        harness.deliver_committed_result(request_id, committed["txid"])
        raw = self.nodes[0].getrawtransaction(committed["txid"], True)
        inputs = [{"txid": vin["txid"], "vout": vin["vout"]} for vin in raw["vin"]]
        # Compute the actual DGB fee independently of the rolled-back ledger.
        input_value = sum(self.nodes[0].getrawtransaction(
            vin["txid"], True)["vout"][vin["vout"]]["value"] for vin in inputs)
        output_value = sum(vout["value"] for vout in raw["vout"])
        fee = int((input_value - output_value) * Decimal(100_000_000))
        assert_equal(fee, 10_000_000)
        return {"request_id": request_id, "psbt": authorized["psbt"],
                "txid": committed["txid"], "hex": committed["hex"],
                "fee_satoshis": fee, "inputs": inputs}

    def stop_provider(self, provider):
        def stopped():
            try:
                result = provider.stoppaymaster()
            except JSONRPCException as error:
                if (error.error.get("code") == -4 and
                        error.error.get("message") == "PAYMASTER_PROVIDER_BUSY"):
                    return False
                raise
            assert_equal(result["running"], False)
            return True

        self.wait_until(stopped, timeout=30)

    def call_review_gated(self, function, *args):
        """Permit only the explicit restore gate, not arbitrary errors."""
        try:
            return function(*args)
        except JSONRPCException as error:
            if (error.error.get("code") == -4 and
                    error.error.get("message") == "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"):
                return {"ready": False, "restore_review_required": True}
            raise

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        provider, _ = harness.create_wallets()
        harness.fund_and_configure(operation_mode="manual", carrier_cents=2_000)
        targets = {"admission_dgb_slots": 3, "operational_dgb_slots": 3,
                   "admission_carrier_slots": 3, "operational_carrier_slots": 3}
        preview = provider.preparepaymasterpool(targets)
        execute = dict(targets, execute=True, plan_id=preview["plan_id"])
        prepared = provider.preparepaymasterpool(execute)
        confirm_pool_preparation(self, self.nodes[0], provider, execute, prepared)
        liquidity = default_liquidity_policy()
        liquidity.update(target_operational_dgb=3, target_operational_carriers=3)
        provider.setpaymasterliquiditypolicy(liquidity)
        harness.fund_client_dd(500)
        self.nodes[1].createwallet("second_client", descriptors=True)
        second_client = self.nodes[1].get_wallet_rpc("second_client")
        second_client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10_000,
        })
        provider.senddigidollar(second_client.getdigidollaraddress(), 500)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.wait_chain_ready()

        # The two actual 0.1-DGB payments exhaust this explicit allowance.
        # Three unused slots existed at backup time, so liquidity cannot mask
        # an accounting rollback merely by running out of operational inputs.
        policy = dict(harness.policy, maximum_network_fee_dgb_satoshis=10_000_000)
        safety = provider_safety_policy(["user_paid"])
        safety["user_paid"].update(
            maximum_network_fee_per_transaction_satoshis=10_000_000,
            maximum_network_fee_per_hour_satoshis=20_000_000,
            maximum_network_fee_per_day_satoshis=20_000_000,
            maximum_completed_per_hour=2, maximum_completed_per_day=2)
        provider.setpaymastersafetypolicy(safety)
        provider.setpaymasterpolicy(policy)
        provider.setpaymasterruntimesettings({"operation_mode": "manual", "autostart": True})
        recipient = provider.getdigidollaraddress()
        backup = self.nodes[0].datadir_path / "provider-before-payments.bak"
        provider.backupwallet(backup)
        assert_equal(provider.getpaymastersafetystatus()["user_paid"][
            "spent_network_fee_last_day_satoshis"], 0)
        assert_equal(provider.startpaymaster()["ready"], True)

        self.log.info("Commit one confirmed and one unconfirmed payment after the provider backup")
        confirmed = self.commit_payment(harness, "83b57224-72d2-4c79-8bde-043181e0af01", recipient)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.wait_chain_ready()
        assert self.nodes[0].getrawtransaction(confirmed["txid"], True)["confirmations"] > 0
        if self.options.pending_offline:
            # This image contains the first payment and its free successors,
            # but predates the second signed payment using those successors.
            backup = self.nodes[0].datadir_path / "provider-before-pending.bak"
            provider.backupwallet(backup)
        backup_pool = provider.getpaymasterpoolinfo()
        harness.client = second_client
        pending = self.commit_payment(harness, "83b57224-72d2-4c79-8bde-043181e0af02", recipient)
        self.sync_mempools()
        assert pending["txid"] in self.nodes[0].getrawmempool()
        assert pending["txid"] in self.nodes[1].getrawmempool()
        original_budget = provider.getpaymastersafetystatus()["user_paid"]
        assert_equal(original_budget["spent_network_fee_last_day_satoshis"], 20_000_000)
        assert_equal(original_budget["completed_last_day"], 2)
        self.stop_provider(provider)
        assert_equal(provider.startpaymaster()["ready"], False)
        assert_equal(provider.getpaymasterinfo()["ready"], False)
        original_pool = provider.getpaymasterpoolinfo()
        mempool_before = set(self.nodes[0].getrawmempool())
        self.stop_provider(provider)
        self.nodes[0].unloadwallet("provider", load_on_startup=False)

        self.log.info("Restore the older SQLite image without loading a second copy of its keys")
        provider_args = paymaster_node_args(provider_node=0)
        client_args = paymaster_node_args()
        if self.options.pending_offline:
            # Remove only these disposable nodes' volatile observation of the
            # still-valid signed payment. Its bytes remain with the test, like
            # a counterparty withholding publication. Do not abandon it or
            # introduce a conflicting spend to manufacture the condition.
            self.stop_nodes()
            provider_args += ["-persistmempool=0", "-walletbroadcast=0"]
            client_args += ["-persistmempool=0", "-walletbroadcast=0"]
            self.start_node(0, provider_args)
            self.start_node(1, client_args)
            self.connect_nodes(0, 1)
            self.wait_chain_ready()
            assert pending["txid"] not in self.nodes[0].getrawmempool()
            assert pending["txid"] not in self.nodes[1].getrawmempool()
            mempool_before = set(self.nodes[0].getrawmempool())
        self.nodes[0].restorewallet("restored_provider", backup, load_on_startup=True)
        # Also discard volatile announcement replay caches on both sides. An
        # old sequence rejected by the still-running directory is not durable
        # backup protection and must not conceal a fresh-process failure.
        self.restart_node(0, provider_args)
        self.restart_node(1, client_args)
        self.connect_nodes(0, 1)
        self.wait_chain_ready()
        loaded = self.nodes[0].listwallets()
        assert "restored_provider" in loaded
        assert "provider" not in loaded
        assert_equal(pending["txid"] in self.nodes[0].getrawmempool(), not self.options.pending_offline)
        restored = self.nodes[0].get_wallet_rpc("restored_provider")
        restored_cli = self.nodes[0].cli("-rpcwallet=restored_provider")
        harness.provider = restored
        harness.provider_cli = restored_cli
        harness.client = self.nodes[1].get_wallet_rpc("client")
        self.wait_chain_ready()
        observations = {"pending_offline": self.options.pending_offline,
                        "original_budget": original_budget, "original_pool": original_pool,
                        "after_load": restored.getpaymasterinfo(),
                        "restored_budget": restored.getpaymastersafetystatus()["user_paid"]}
        failures = []

        def check(condition, message):
            if not condition:
                failures.append(message)
                self.log.error(message)

        # A lost manifest must never be re-created from untrusted replay data.
        # Check both transport-independent entry points and keep their rejection
        # separate from the stronger requirement to block new authority.
        for api in (restored, restored_cli):
            for payment in (confirmed, pending):
                if self.options.pending_offline and payment is confirmed:
                    assert_equal(api.submitpaymasterdigidollar(payment["psbt"])["txid"], payment["txid"])
                else:
                    try:
                        api.submitpaymasterdigidollar(payment["psbt"])
                    except JSONRPCException as error:
                        assert_equal(error.error.get("code"), -4)
                        assert error.error.get("message") in (
                            "No persistent Paymaster template matches this PSBT",
                            "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"), error.error
                    else:
                        raise AssertionError("Lost template replay unexpectedly signed or committed a payment")
        assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)

        for payment in (confirmed, pending):
            for prevout in payment["inputs"]:
                spent = self.nodes[0].gettxout(prevout["txid"], prevout["vout"], True) is None
                assert_equal(spent, payment is confirmed or not self.options.pending_offline)

        start = self.call_review_gated(restored_cli.startpaymaster)
        observations["explicit_start"] = start
        check(not observations["after_load"]["ready"],
              "Restored autostart reported ready before missing financial history was reconciled")
        check(not start["ready"], "Stale backup regained payment authority with forgotten daily costs")
        restored_budget = observations["restored_budget"]
        check(restored_budget["spent_network_fee_last_day_satoshis"] >= 20_000_000
              or not start["ready"], "Restored provider reset an exhausted rolling-day budget")
        restored_pool = restored.getpaymasterpoolinfo()
        observations["restored_pool"] = restored_pool
        pending_inputs = {(vin["txid"], vin["vout"]) for vin in pending["inputs"]}
        protected_pool_inputs = {(entry["txid"], entry["vout"])
                                 for entry in backup_pool["pool"]
                                 if (entry["txid"], entry["vout"]) in pending_inputs}
        if self.options.pending_offline:
            assert protected_pool_inputs, "Offline fixture must protect a reserve present in the backup"
            exposed_inputs = {(entry["txid"], entry["vout"])
                              for entry in restored_pool["pool"]
                              if entry["state"] == "available"} & protected_pool_inputs
            observations["exposed_signed_pool_inputs"] = sorted(exposed_inputs)
            check(not exposed_inputs or not start["ready"],
                  "Restored provider exposed inputs of a withheld signed payment as available")
        else:
            assert not any(entry["state"] == "available"
                           and (entry["txid"], entry["vout"]) in pending_inputs
                           for entry in restored_pool["pool"])

        # A reserve-maintenance preview must be read-only; merely opening it
        # cannot spend fees or change payment authority.
        before_preview = restored.getpaymastersafetystatus()["user_paid"]
        observations["maintenance_preview"] = self.call_review_gated(restored_cli.preparepaymasterpool, targets)
        assert_equal(restored.getpaymastersafetystatus()["user_paid"], before_preview)
        assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)

        # Probe new signing only if the restore incorrectly reopened authority.
        # This is a real third payment in disposable wallets, not a mocked row:
        # the independent sum proves an actual breach of approved expenditure.
        if start["ready"]:
            self.log.info("Probe whether the rolled-back provider actually signs a third payment")
            third = self.commit_payment(harness, "83b57224-72d2-4c79-8bde-043181e0af03", recipient)
            actual_fee = sum(payment["fee_satoshis"] for payment in (confirmed, pending, third))
            observations["signed_total_fee_satoshis"] = actual_fee
            observations["third_txid"] = third["txid"]
            if self.options.pending_offline:
                third_inputs = {(vin["txid"], vin["vout"]) for vin in third["inputs"]}
                overlap = third_inputs & pending_inputs
                observations["conflicting_signed_inputs"] = sorted(overlap)
                check(not overlap, "Provider signed a conflicting spend of a withheld payment's inputs after restore")
                if not overlap:
                    self.nodes[0].sendrawtransaction(pending["hex"])
            if not self.options.pending_offline or not overlap:
                self.generatetoaddress(self.nodes[0], 1, restored.getnewaddress())
                self.wait_chain_ready()
                for payment in (confirmed, pending, third):
                    assert self.nodes[0].getrawtransaction(payment["txid"], True)["confirmations"] > 0
                observations["actual_total_fee_satoshis"] = actual_fee
                check(actual_fee <= 20_000_000,
                      "Provider spent 0.3 DGB under a 0.2-DGB daily approval after restore")
        else:
            gate = "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED"
            restored_info = restored.getpaymasterinfo()
            assert gate in restored_info["readiness_errors"]
            assert_equal(restored_info["wallet_eligible"], True)
            # Exercise both local entry points; neither previews nor explicit
            # execution may create a fee reservation or release protected coins.
            for api in (restored, restored_cli):
                for function, options in (
                        (api.preparepaymasterpool, targets),
                        (api.preparepaymasterpool, dict(targets, execute=True, plan_id=preview["plan_id"])),
                        (api.rebalancepaymasterpool, targets),
                        (api.withdrawpaymastercarrier, {"mode": "all_excess"}),
                        (api.releasepaymastercapital, {})):
                    assert_raises_rpc_error(-4, gate, function, options)
            # Toggling provider settings, approved limits, and backup reminders
            # cannot constitute evidence about a withheld signed transaction.
            restored.setpaymasterenabled(False)
            restored.setpaymasterenabled(True)
            restored.setpaymastersafetypolicy(safety)
            restored.acknowledgepaymasterproviderbackup({"external_backup": True})
            restored.setpaymasterruntimesettings({"operation_mode": "automatic", "autostart": True})
            assert_raises_rpc_error(-4, gate, restored_cli.startpaymaster)
            self.wait_until(lambda: restored.getpaymasterinfo().get("last_service_error") == gate)
            assert_equal(restored.getpaymasterinfo()["running"], False)
            assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)
            observations["after_settings_changes"] = restored.getpaymasterinfo()

            # Copy an already quarantined image without calling restorewallet.
            # The guard belongs to the database, not its name or restore RPC.
            # An older unmarked image cannot be recognized this way; detecting
            # that rollback requires independent state outside the copied file.
            guarded_backup = self.nodes[0].datadir_path / "provider-guarded.bak"
            restored.backupwallet(guarded_backup)
            self.nodes[0].unloadwallet("restored_provider", load_on_startup=False)
            copied_name = "copied_guarded_provider"
            copied_directory = self.nodes[0].wallets_path / copied_name
            copied_directory.mkdir()
            copyfile(guarded_backup, copied_directory / self.wallet_data_filename)
            self.nodes[0].loadwallet(copied_name, load_on_startup=True)
            self.restart_node(0, provider_args)
            self.connect_nodes(0, 1)
            self.wait_chain_ready()
            copied = self.nodes[0].get_wallet_rpc(copied_name)
            copied_cli = self.nodes[0].cli(f"-rpcwallet={copied_name}")
            copied_info = copied.getpaymasterinfo()
            assert gate in copied_info["readiness_errors"]
            assert_equal(copied_info["ready"], False)
            assert_equal(copied_info["running"], False)
            for api in (copied, copied_cli):
                assert_raises_rpc_error(-4, gate, api.startpaymaster)
                assert_raises_rpc_error(-4, gate, api.preparepaymasterpool, targets)
                assert_raises_rpc_error(-4, gate, api.releasepaymastercapital, {})
            assert_equal(set(self.nodes[0].getrawmempool()), mempool_before)
            observations["guarded_file_copy_restart_protected"] = True

        # The generic restore hook must not quarantine a client-only wallet.
        client_backup = self.nodes[1].datadir_path / "client-backup.bak"
        client_total = harness.client.getdigidollarbalance()["total"]
        client_safety = harness.client.getpaymasterclientsafetystatus()
        harness.client.backupwallet(client_backup)
        self.nodes[1].unloadwallet("client", load_on_startup=False)
        self.nodes[1].restorewallet("restored_client", client_backup, load_on_startup=False)
        restored_client = self.nodes[1].get_wallet_rpc("restored_client")
        self.wait_chain_ready()
        assert_equal(restored_client.getdigidollarbalance()["total"], client_total)
        assert_equal(restored_client.getpaymasterclientsafetystatus()["policy"], client_safety["policy"])
        assert "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED" not in restored_client.getpaymasterinfo()["readiness_errors"]
        observations["client_only_restore_unchanged"] = True
        observations["failures"] = failures
        # Save even a failing diagnostic before the final assertion; never save
        # private keys or PSBTs in this public summary.
        report = self.nodes[0].datadir_path.parent / "provider-backup-observations.json"
        report.write_text(json.dumps(observations, indent=2, default=str) + "\n", encoding="utf-8")
        assert not failures, "; ".join(failures)


if __name__ == "__main__":
    PaymasterProviderBackupTest().main()
