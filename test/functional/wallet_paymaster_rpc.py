#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Direct contract tests for Paymaster wallet RPCs.

The broad provider scenario proves the complete product flow.  This file keeps
the public RPC boundaries independently testable, especially preview binding,
idempotency, redacted reservation output, and atomic release semantics.
"""

import base64

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    assert_snapshot_equal,
    default_liquidity_policy,
    paymaster_node_args,
    value_snapshot,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PaymasterRPCContractsTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(provider_node=0),
            paymaster_node_args(),
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        provider, client = harness.create_wallets()
        info = harness.fund_and_configure(
            operation_mode="manual", carrier_cents=4_000,
            fee_rate_bps=1_900)
        assert_equal(info["operation_mode"], "manual")
        # Keep one independent operational carrier available after the submit
        # contract spends another one.  This lets release_slot be tested
        # without depending on successor promotion timing.
        expanded_pool = harness.provider_cli.preparepaymasterpool({
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 2,
            "execute": True,
        })
        assert_equal(expanded_pool["executed"], True)
        expanded_liquidity = default_liquidity_policy(carriers=True)
        expanded_liquidity.update({
            "target_operational_dgb": 2,
            "target_operational_carriers": 2,
            "maximum_maintenance_fee_per_transaction_satoshis": 200_000_000,
            "maximum_maintenance_fee_per_hour_satoshis": 500_000_000,
            "maximum_maintenance_fee_per_day_satoshis": 1_000_000_000,
        })
        provider.setpaymasterliquiditypolicy(expanded_liquidity)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        harness.fund_client_dd(1_500)

        # Use a third wallet as an independent recipient so every value-moving
        # assertion distinguishes client change from recipient value.
        self.nodes[0].createwallet(
            wallet_name="recipient", descriptors=True, load_on_startup=True)
        recipient_wallet = self.nodes[0].get_wallet_rpc("recipient")
        recipient = recipient_wallet.getdigidollaraddress()

        started = provider.startpaymaster()
        assert_equal(started["running"], True)
        self.wait_until(lambda: len(client.getpaymasteroffers(500)) > 0)

        self.log.info("Reject malformed client PSBTs at the direct RPC boundary")
        assert_raises_rpc_error(
            -22, "Paymaster PSBT decode failed",
            client.walletprocesspaymasterpsbt, "not-a-psbt")

        self.log.info("Reject malformed inputs at the remaining public RPC boundaries")
        assert_raises_rpc_error(
            -8, "", client.requestpaymasterquote, "00", {})
        assert_raises_rpc_error(
            -8, "", client.resolvepaymastersession,
            {"request_id": "not-a-canonical-request-id"}, "refresh")
        assert_raises_rpc_error(
            -8, "", provider.createrestrictedpaymasterdescriptor,
            "00", "invalid sponsor", 300)
        assert_raises_rpc_error(
            -8, "request_id must be a canonical lowercase UUID",
            client.processpaymasterresult, "not-a-canonical-request-id")

        # Rebalancing defaults to a side-effect-free preview.  Calling the
        # boundary directly with the current targets also proves that a no-op
        # retirement cannot mutate the live pool.
        before_rebalance = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        rebalance = provider.rebalancepaymasterpool({
            "admission_dgb_slots": 3,
            "operational_dgb_slots": 2,
            "admission_carrier_slots": 3,
            "operational_carrier_slots": 2,
        })
        assert_equal(rebalance["executed"], False)
        assert_snapshot_equal(
            before_rebalance,
            value_snapshot(self.nodes[0], provider, client, recipient_wallet))

        self.log.info("Unsigned quote cancellation releases pool and budget atomically")
        _, quote, _ = harness.wait_for_quote(
            "550e8400-e29b-41d4-a716-446655441001", recipient, 500)
        assert "attempt_id" in quote
        reservations = provider.listpaymasterreservations()
        assert_equal(len(reservations) >= 1, True)
        required_fields = {
            "txid", "vout", "purpose", "asset", "state",
            "dgb_satoshis", "dd_cents", "confirmation_height", "updated_at",
        }
        for reservation in reservations:
            assert required_fields.issubset(reservation)
            assert reservation["state"] != "available"
            # This diagnostic RPC must not expose payment or network metadata.
            assert "recipient" not in reservation
            assert "amount" not in reservation
            assert "peer_id" not in reservation
            assert "netgroup" not in reservation

        canceled = provider.cancelpaymasterquote(quote["attempt_id"])
        assert_equal(canceled["attempt_state"], "REJECTED")
        assert_equal(provider.listpaymasterreservations(), [])
        safety = provider.getpaymastersafetystatus()["user_paid"]
        assert_equal(safety["reserved_network_fee_satoshis"], 0)
        # Exact cancellation retries are intentionally idempotent.
        assert_equal(provider.cancelpaymasterquote(quote["attempt_id"]), canceled)

        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        self.sync_blocks()
        provider = self.nodes[0].get_wallet_rpc("provider")
        recipient_wallet = self.nodes[0].get_wallet_rpc("recipient")
        harness.provider_node = self.nodes[0]
        harness.provider = provider
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=provider")
        assert_equal(provider.listpaymasterreservations(), [])
        assert_equal(provider.cancelpaymasterquote(quote["attempt_id"])[
            "attempt_state"], "REJECTED")

        # Keep the cancellation tombstone isolated from the independent submit
        # contract.  A fresh wallet must not inherit capacity evidence or
        # reservations from the first client.
        self.nodes[1].createwallet(
            wallet_name="submit_client", descriptors=True,
            load_on_startup=True)
        client = self.nodes[1].get_wallet_rpc("submit_client")
        client.setpaymasterclientsafetypolicy({
            "maximum_service_fee_per_transaction_cents": 100,
            "maximum_service_fee_per_day_cents": 10_000,
        })
        harness.client = client
        provider.senddigidollar(client.getdigidollaraddress(), 1_500)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        provider.startpaymaster()

        self.log.info("Manual submit rejects mutation and commits exact retries once")
        submit_request_id = "550e8400-e29b-41d4-a716-446655441002"
        options, _, send = harness.wait_for_quote(
            submit_request_id, recipient, 500)
        authorization = harness.authorize_quote(options, send)
        psbt = authorization["psbt"]

        tampered = bytearray(base64.b64decode(psbt))
        tampered[-1] ^= 1
        assert_raises_rpc_error(
            -22, "Paymaster PSBT decode failed",
            provider.submitpaymasterdigidollar,
            base64.b64encode(tampered).decode())

        before_commit = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        assert_equal(
            before_commit.provider_budget["user_paid"]
            ["reserved_network_fee_satoshis"] > 0,
            True)
        assert_raises_rpc_error(
            -4, "", provider.cancelpaymasterquote,
            authorization["attempt_id"])
        commit = provider.submitpaymasterdigidollar(psbt)
        assert_equal(len(commit["txid"]), 64)
        assert_equal(commit["broadcast"], True)
        after_commit = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        replay = provider.submitpaymasterdigidollar(psbt)
        assert_equal(replay["txid"], commit["txid"])
        assert_equal(len(self.nodes[0].getrawmempool()), 1)
        after_replay = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        assert_equal(after_replay.mempool, frozenset({commit["txid"]}))
        # Replaying an exact durable commit cannot reserve another budget or
        # create another transaction.
        assert_equal(
            after_replay.provider_budget["user_paid"]
            ["reserved_network_fee_satoshis"],
            after_commit.provider_budget["user_paid"]
            ["reserved_network_fee_satoshis"])

        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(
            lambda: recipient_wallet.getdigidollarbalance()["total"] == 500)
        assert_equal(client.getdigidollarbalance()["total"], 905)

        # A single sub-DD fee is intentionally below the minimum ordinary DD
        # output.  Recycle the successor once more so all_excess can prove its
        # intended batching contract rather than manufacture dust.
        second_options, _, second_send = harness.wait_for_quote(
            "550e8400-e29b-41d4-a716-446655441003", recipient, 500)
        second_authorization = harness.authorize_quote(
            second_options, second_send)
        second_commit = provider.submitpaymasterdigidollar(
            second_authorization["psbt"])
        assert_equal(len(second_commit["txid"]), 64)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(
            lambda: recipient_wallet.getdigidollarbalance()["total"] == 1_000)
        assert_equal(client.getdigidollarbalance()["total"], 310)

        self.log.info("Carrier release preview is inert and plan-bound")
        self.restart_node(0, paymaster_node_args(provider_node=0))
        self.connect_nodes(0, 1)
        self.sync_blocks()
        provider = self.nodes[0].get_wallet_rpc("provider")
        recipient_wallet = self.nodes[0].get_wallet_rpc("recipient")
        harness.provider = provider
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=provider")

        self.log.info("Automatic service excludes manual processing RPCs")
        provider.setpaymasterruntimesettings({
            "operation_mode": "automatic",
            "autostart": False,
        })
        provider.startpaymaster()
        assert_raises_rpc_error(
            -4, "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE",
            provider.processpaymasterrequests)
        assert_raises_rpc_error(
            -4, "PAYMASTER_AUTOMATIC_SERVICE_ACTIVE",
            provider.processpaymastersubmits)
        provider.stoppaymaster()
        provider.setpaymasterruntimesettings({
            "operation_mode": "manual",
            "autostart": False,
        })

        self.log.info("Carrier excess withdrawal preserves every 1 DD base")
        liquidity_before_excess = provider.getpaymasterliquiditystatus()
        self.log.info(
            "Carrier accounting before withdrawal: %s",
            {
                "base": liquidity_before_excess["carrier_base_cents"],
                "excess": liquidity_before_excess[
                    "carrier_withdrawable_excess_cents"],
                "pool": [
                    (entry["purpose"], entry["state"], entry["dd_cents"])
                    for entry in provider.getpaymasterpoolinfo()["pool"]
                    if entry["asset"] == "dd_carrier"
                ],
            })
        assert_equal(
            liquidity_before_excess["carrier_withdrawable_excess_cents"] > 0,
            True)
        before_excess_preview = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        available_before_excess = provider.getdigidollarbalance()["total"]
        excess_preview = provider.withdrawpaymastercarrier({
            "mode": "all_excess",
        })
        assert_equal(excess_preview["executed"], False)
        assert_equal(excess_preview["withdrawable_excess_cents"] > 0, True)
        assert_equal(excess_preview["source_carriers"] > 0, True)
        assert_equal(
            excess_preview["retained_carrier_cents"],
            excess_preview["source_carriers"] * 100)
        assert_snapshot_equal(
            before_excess_preview,
            value_snapshot(self.nodes[0], provider, client, recipient_wallet))
        execution_preview = provider.withdrawpaymastercarrier({
            "mode": "all_excess",
        })
        excess_result = provider.withdrawpaymastercarrier({
            "mode": "all_excess",
            "execute": True,
            "plan_id": execution_preview["plan_id"],
        })
        assert_equal(excess_result["executed"], True)
        assert_equal(len(excess_result["txid"]), 64)
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.sync_blocks()
        self.wait_until(lambda: provider.getpaymasterliquiditystatus()[
            "carrier_withdrawable_excess_cents"] == 0)
        assert_equal(
            provider.getdigidollarbalance()["total"],
            available_before_excess +
            excess_preview["withdrawable_excess_cents"])

        carrier = next(entry for entry in provider.getpaymasterpoolinfo()["pool"]
                       if entry["purpose"] == "operational" and
                       entry["asset"] == "dd_carrier" and
                       entry["state"] == "available")

        before_preview = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        preview = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "txid": carrier["txid"],
            "vout": carrier["vout"],
        })
        assert_equal(preview["executed"], False)
        after_preview = value_snapshot(
            self.nodes[0], provider, client, recipient_wallet)
        assert_snapshot_equal(before_preview, after_preview)

        provider.setpaymasterliquiditypolicy(default_liquidity_policy(True))
        assert_raises_rpc_error(
            -4, "PAYMASTER_CARRIER_WITHDRAWAL_PLAN_CHANGED",
            provider.withdrawpaymastercarrier,
            {"mode": "release_slot", "execute": True,
             "plan_id": preview["plan_id"]})

        release_preview = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "txid": carrier["txid"],
            "vout": carrier["vout"],
        })
        released = provider.withdrawpaymastercarrier({
            "mode": "release_slot", "execute": True,
            "plan_id": release_preview["plan_id"],
        })
        assert_equal(released["executed"], True)
        assert_equal(released["operational_carrier_target"], 0)

        self.log.info("Reputation listing and clearing have deterministic contracts")
        records = client.getpaymasterreputation()
        assert_equal(records, sorted(records, key=lambda item: item["provider_id"]))
        if records:
            provider_id = records[0]["provider_id"]
            assert_equal(len(client.getpaymasterreputation(provider_id)), 1)
            assert_equal(client.clearpaymasterreputation(provider_id)["cleared"], 1)
            assert_equal(client.getpaymasterreputation(provider_id), [])
        remaining_records = len(client.getpaymasterreputation())
        assert_equal(client.clearpaymasterreputation()["cleared"],
                     remaining_records)
        assert_equal(client.getpaymasterreputation(), [])


if __name__ == "__main__":
    PaymasterRPCContractsTest().main()
