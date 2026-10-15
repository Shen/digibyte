#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Confirmed payment capacity remains usable below the automatic refill target."""

import time

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    confirm_pool_preparation,
    default_liquidity_policy,
    paymaster_node_args,
)
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class PaymasterPartialCapacityTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [paymaster_node_args(provider_node=0), paymaster_node_args()]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        harness.fund_and_configure(carrier_cents=1000, operation_mode="automatic")
        provider, client = harness.provider, harness.client
        node, client_node = self.nodes
        targets = {
            "admission_dgb_slots": 3, "operational_dgb_slots": 2,
            "admission_carrier_slots": 3, "operational_carrier_slots": 2,
        }
        preview = provider.preparepaymasterpool(targets)
        targets.update(execute=True, plan_id=preview["plan_id"])
        prepared = provider.preparepaymasterpool(targets)
        confirm_pool_preparation(self, node, provider, targets, prepared)
        harness.fund_client_dd(300)

        # Make ordinary funding unavailable without touching any protected
        # reserve. The two confirmed operational slots must remain usable.
        protected = {(entry["txid"], entry["vout"])
                     for entry in provider.getpaymasterpoolinfo()["pool"]}
        ordinary = [{"txid": coin["txid"], "vout": coin["vout"]}
                    for coin in provider.listunspent()
                    if coin["spendable"] and coin["amount"] > 0
                    and (coin["txid"], coin["vout"]) not in protected]
        assert ordinary
        assert_equal(provider.lockunspent(False, ordinary), True)
        policy = default_liquidity_policy()
        policy.update(target_operational_dgb=3, target_operational_carriers=2)
        provider.setpaymasterliquiditypolicy(policy)
        saved = provider.getpaymasterliquiditystatus()["policy"]
        assert_equal(provider.startpaymaster()["running"], True)

        def accepting_with_shortfall():
            info = provider.getpaymasterinfo()
            return (info["ready"] and info["service_state"] == "active"
                    and info.get("last_service_error") == "PAYMASTER_POOL_WAITING_DGB")

        self.wait_until(accepting_with_shortfall, timeout=20)
        info = provider.getpaymasterinfo()
        assert_equal(info["pool"]["complete_operational_slots"], 2)
        assert_equal(info["liquidity"]["operational_dgb"]["missing"], 1)
        assert_equal(info["liquidity"]["operational_dgb"]["pending"], 0)
        assert_equal(info["liquidity"]["refill_funding"]["available_dgb_satoshis"], 0)
        self.wait_until(lambda: bool(client.getpaymasteroffers(100)))

        # Expire the initial advertisement: success must come from the worker's
        # renewal, not from the announcement published by startpaymaster.
        announcements = client_node.listpaymasters()
        sequence = info["announcement_sequence"]
        expiry = max(entry["expires_at"] for entry in announcements) + 1
        for test_node in self.nodes:
            test_node.setmocktime(expiry)
        self.wait_until(lambda: provider.getpaymasterinfo()["announcement_sequence"] > sequence, timeout=20)
        self.wait_until(lambda: bool(client.getpaymasteroffers(100)), timeout=20)
        assert_equal(provider.getpaymasterliquiditystatus()["policy"], saved)

        # Exercise fresh capacity and the exact unsigned offer over Direct P2P.
        # No payment signature or broadcast is needed for this regression.
        request_id = "550e8400-e29b-41d4-a716-446655447001"
        options = {"fee_mode": "paymaster", "request_id": request_id,
                   "maximum_paymaster_fee_cents": 1, "privacy": "standard",
                   "selection": "lowest_total_cost", "maximum_provider_attempts": 1}
        recipient = provider.getdigidollaraddress()
        poll_at = 0
        authorization = {}
        mempool = set(node.getrawmempool())

        def prepare():
            nonlocal poll_at, authorization
            if time.monotonic() - poll_at < 0.5:
                return False
            poll_at = time.monotonic()
            authorization = client.senddigidollar(recipient, 100, "", 0, None, "cents", options)
            return authorization.get("authorization_required", False)

        self.wait_until(prepare, timeout=30)
        assert_equal(authorization["authorization_accepted"], False)
        client.resolvepaymastersession({"request_id": request_id}, "abandon_unsigned")
        assert_equal(set(node.getrawmempool()), mempool)
        assert_equal(provider.getpaymasterliquiditystatus()["policy"], saved)

        # Explicit stop still prevents renewal, despite the remaining capacity.
        provider.stoppaymaster()
        expiry = max(entry["expires_at"] for entry in client_node.listpaymasters()) + 1
        for test_node in self.nodes:
            test_node.setmocktime(expiry)
        self.wait_until(lambda: not client.getpaymasteroffers(100), timeout=20)
        assert_equal(provider.getpaymasterinfo()["running"], False)


if __name__ == "__main__":
    PaymasterPartialCapacityTest().main()
