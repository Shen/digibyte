#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Paymaster wallet, pool, and finance reconciliation across a reorg.

A provider and client complete and confirm a real USER_PAID transfer while a
third node mines an isolated longer branch that never saw the transaction.
After reconnection the payment returns to the mempool, client authority remains
bound to the same transaction, provider successor liquidity becomes pending,
and confirmed finance totals roll back. Reconfirmation must promote the same
transaction and the same single finance event again.
"""

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    paymaster_node_args,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class PaymasterReorgTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            paymaster_node_args(),
            paymaster_node_args(),
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        provider_node, client_node, fork_node = self.nodes
        harness = PaymasterFunctionalHarness(self)
        provider, client = harness.create_wallets()
        client_node.createwallet(
            wallet_name="reorg-recipient", descriptors=True,
            load_on_startup=True)
        fork_node.createwallet(
            wallet_name="fork-miner", descriptors=True,
            load_on_startup=True)
        recipient = client_node.get_wallet_rpc("reorg-recipient")
        fork_miner = fork_node.get_wallet_rpc("fork-miner")

        self.log.info("Configure and fund one manual USER_PAID provider")
        harness.fund_and_configure(
            operation_mode="manual", carrier_cents=2_000,
            fee_rate_bps=50)
        harness.fund_client_dd(1_500)
        started = provider.startpaymaster()
        assert_equal(started["running"], True)
        provider_id = harness.identity["provider_id"]
        self.wait_until(lambda: provider_id in {
            entry["provider_id"] for entry in client_node.listpaymasters()
        })

        self.log.info("Fork the third node before any private Paymaster traffic")
        self.sync_blocks()
        common_tip = provider_node.getbestblockhash()
        common_height = provider_node.getblockcount()
        assert_equal(fork_node.getbestblockhash(), common_tip)
        self.disconnect_nodes(1, 2)

        self.log.info("Complete one Paymaster transfer on the provider branch")
        request_id = "550e8400-e29b-41d4-a716-446655442201"
        options, _, send = harness.wait_for_quote(
            request_id, recipient.getdigidollaraddress(), 1_000)
        authorized = harness.authorize_quote(options, send)
        assert_equal(authorized["provider_id"], provider_id)

        commit = {}

        def submit_ready():
            nonlocal commit
            commit = provider.processpaymastersubmits()
            if commit["processed"]:
                return True
            send()
            return False

        self.wait_until(submit_ready)
        assert_equal(commit["queued"], True)
        txid = commit["txid"]
        assert txid in provider_node.getrawmempool()
        assert txid not in fork_node.getrawmempool()

        result = {}

        def result_ready():
            nonlocal result
            result = client.processpaymasterresult(request_id)
            return result["processed"]

        self.wait_until(result_ready)
        assert_equal(result["txid"], txid)
        self.wait_until(lambda: txid in client_node.getrawmempool())
        branch_a_block = self.generatetoaddress(
            provider_node, 1, provider.getnewaddress(),
            sync_fun=self.no_op)[0]
        self.sync_blocks([provider_node, client_node])
        assert txid in provider_node.getblock(branch_a_block)["tx"]
        assert_equal(fork_node.getblockcount(), common_height)

        lookup = {"request_id": request_id}
        self.wait_until(lambda: client.getdigidollarsendsession(
            lookup)["session_state"] == "CONFIRMED")
        confirmed_session = client.getdigidollarsendsession(lookup)
        assert_equal(confirmed_session["confirmation_state"],
                     "payment_confirmed")
        assert_equal(confirmed_session["txid"], txid)
        self.wait_until(lambda: provider.getpaymasterfinancestatus({
            "period": "all",
        })["successful_transfers"] == 1)
        confirmed_finance = provider.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })
        confirmed_transfer_events = [
            event for event in confirmed_finance["events"]
            if event["kind"] == "transfer"
        ]
        assert_equal(len(confirmed_transfer_events), 1)
        assert_equal(confirmed_transfer_events[0]["state"], "confirmed")
        assert_equal(confirmed_transfer_events[0]["transaction_id"], txid)

        provider.getpaymasterliquiditystatus()
        confirmed_successors = [
            entry for entry in provider.getpaymasterpoolinfo()["pool"]
            if entry["txid"] == txid and
            entry["purpose"] == "operational"
        ]
        assert confirmed_successors
        assert all(
            entry["state"] == "available"
            for entry in confirmed_successors)

        self.log.info("Reorg onto a longer branch that omitted the payment")
        self.generatetoaddress(
            fork_node, 3, fork_miner.getnewaddress(),
            sync_fun=self.no_op)
        assert fork_node.getblockcount() > common_height + 1
        competing_tip = fork_node.getbestblockhash()
        self.connect_nodes(2, 1)
        self.sync_blocks(timeout=120)
        assert_equal(provider_node.getbestblockhash(), competing_tip)
        assert_equal(client_node.getbestblockhash(), competing_tip)

        self.wait_until(lambda: txid in provider_node.getrawmempool())
        self.wait_until(lambda: txid in client_node.getrawmempool())
        assert txid not in fork_node.getrawmempool()
        self.sync_mempools([provider_node, client_node])
        self.wait_until(lambda: client.getdigidollarsendsession(
            lookup)["session_state"] == "MEMPOOL")
        reorged_session = client.getdigidollarsendsession(lookup)
        assert_equal(reorged_session["confirmation_state"], "unconfirmed")
        assert_equal(reorged_session["txid"], txid)
        assert_equal(
            reorged_session["reserved_user_inputs"],
            confirmed_session["reserved_user_inputs"])

        reorged_finance = provider.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })
        assert_equal(reorged_finance["successful_transfers"], 0)
        assert_equal(reorged_finance["service_fee_income_cents"], 0)
        reorged_transfer_events = [
            event for event in reorged_finance["events"]
            if event["kind"] == "transfer"
        ]
        assert_equal(len(reorged_transfer_events), 1)
        assert_equal(reorged_transfer_events[0]["state"], "pending")
        assert_equal(reorged_transfer_events[0]["transaction_id"], txid)

        provider.getpaymasterliquiditystatus()
        reorged_successors = [
            entry for entry in provider.getpaymasterpoolinfo()["pool"]
            if entry["txid"] == txid and
            entry["purpose"] == "operational"
        ]
        assert_equal(len(reorged_successors), len(confirmed_successors))
        assert all(
            entry["state"] == "pending_successor"
            for entry in reorged_successors)

        self.log.info("Reconfirm the same transaction without duplicate accounting")
        reconfirmation_block = self.generatetoaddress(
            provider_node, 1, provider.getnewaddress())[0]
        assert txid in provider_node.getblock(reconfirmation_block)["tx"]
        self.wait_until(lambda: client.getdigidollarsendsession(
            lookup)["session_state"] == "CONFIRMED")
        reconfirmed_session = client.getdigidollarsendsession(lookup)
        assert_equal(reconfirmed_session["txid"], txid)
        assert_equal(reconfirmed_session["confirmation_state"],
                     "payment_confirmed")

        reconfirmed_finance = provider.getpaymasterfinancestatus({
            "period": "all",
            "include_events": True,
            "limit": 10,
        })
        assert_equal(reconfirmed_finance["successful_transfers"], 1)
        assert_equal(reconfirmed_finance["service_fee_income_cents"], 5)
        final_transfer_events = [
            event for event in reconfirmed_finance["events"]
            if event["kind"] == "transfer"
        ]
        assert_equal(len(final_transfer_events), 1)
        assert_equal(final_transfer_events[0]["state"], "confirmed")
        assert_equal(final_transfer_events[0]["transaction_id"], txid)


if __name__ == "__main__":
    PaymasterReorgTest().main()
