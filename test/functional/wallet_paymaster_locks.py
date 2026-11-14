#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Manual provider locks gate new capacity and signatures without releasing authority."""

from test_framework.paymaster import PaymasterFunctionalHarness, confirm_pool_preparation, paymaster_node_args
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PaymasterLocksTest(DigiByteTestFramework):
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
        harness.fund_and_configure(carrier_cents=1000, operation_mode="manual")
        harness.fund_client_dd(500)
        provider = harness.provider
        pool = provider.getpaymasterpoolinfo()["pool"]
        slots = [dict(txid=entry["txid"], vout=entry["vout"])
                 for entry in pool if entry["purpose"] == "operational"]
        assert_equal(len(slots), 2)
        dgb = next(dict(txid=entry["txid"], vout=entry["vout"])
                   for entry in pool if entry["purpose"] == "operational" and entry["asset"] == "dgb")
        carrier = next(slot for slot in slots if slot != dgb)
        for persistent in (False, True):
            assert provider.lockunspent(False, [dgb], persistent)
            assert_equal(provider.getpaymasterpoolinfo()["complete_operational_slots"], 0)
            assert_equal(provider.getpaymasterinfo()["ready"], False)
            if persistent:
                self.nodes[0].unloadwallet(harness.provider_wallet_name)
                self.nodes[0].loadwallet(harness.provider_wallet_name)
                assert dgb in provider.listlockunspent()
                assert_equal(provider.getpaymasterpoolinfo()["complete_operational_slots"], 0)
            assert provider.lockunspent(True, [dgb])
            assert_equal(provider.getpaymasterpoolinfo()["complete_operational_slots"], 1)

        # A transient manual carrier veto clears on reload. Paymaster's
        # independent pool protection remains; persistent vetoes are tested below.
        assert provider.lockunspent(False, [carrier])
        assert_equal(provider.getpaymasterpoolinfo()["complete_operational_slots"], 0)
        assert_equal(provider.getpaymasterinfo()["ready"], False)
        self.nodes[0].unloadwallet(harness.provider_wallet_name)
        self.nodes[0].loadwallet(harness.provider_wallet_name)
        assert carrier not in provider.listlockunspent()
        assert_equal(provider.getpaymasterpoolinfo()["complete_operational_slots"], 1)

        provider.startpaymaster()
        request_id = "550e8400-e29b-41d4-a716-446655447099"
        options, _, send = harness.wait_for_quote(request_id, provider.getdigidollaraddress(), 100)
        authorized = harness.authorize_quote(options, send)
        self.wait_until(lambda: send().get("queued", False))
        assert provider.lockunspent(False, [dgb], True)
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_INPUT_LOCKED",
                                provider.submitpaymasterdigidollar, authorized["psbt"])
        assert dgb in provider.listlockunspent()
        reserved = [entry for entry in provider.getpaymasterpoolinfo()["pool"]
                    if dict(txid=entry["txid"], vout=entry["vout"]) in slots]
        assert_equal(len(reserved), 2)
        assert all(entry["state"] == "reserved" for entry in reserved)
        assert provider.lockunspent(True, [dgb])

        committed = provider.submitpaymasterdigidollar(authorized["psbt"])
        decoded = self.nodes[0].getrawtransaction(committed["txid"], True)
        inputs = [dict(txid=txin["txid"], vout=txin["vout"]) for txin in decoded["vin"]]
        assert all(slot in inputs for slot in slots)
        harness.deliver_committed_result(request_id, committed["txid"])
        self.generate(self.nodes[0], 1)
        self.sync_blocks()
        assert_equal(harness.client.getdigidollarsendsession({"request_id": request_id})["txid"], committed["txid"])

        # The first DGB successor is below the unchanged per-slot fee ceiling.
        # Explicitly fund another whole slot for the independent carrier case.
        provider.stoppaymaster()
        targets = {"admission_dgb_slots": 3, "operational_dgb_slots": 2,
                   "admission_carrier_slots": 3, "operational_carrier_slots": 2}
        preview = provider.preparepaymasterpool(targets)
        targets.update(execute=True, plan_id=preview["plan_id"])
        prepared = provider.preparepaymasterpool(targets)
        confirm_pool_preparation(self, self.nodes[0], provider, targets, prepared)
        provider.startpaymaster()
        request_id = "550e8400-e29b-41d4-a716-446655447098"
        options, _, send = harness.wait_for_quote(request_id, provider.getdigidollaraddress(), 100)
        authorized = harness.authorize_quote(options, send)
        self.wait_until(lambda: send().get("queued", False))
        carrier = next(dict(txid=entry["txid"], vout=entry["vout"])
                       for entry in provider.getpaymasterpoolinfo()["pool"]
                       if entry["purpose"] == "operational" and entry["asset"] == "dd_carrier"
                       and entry["state"] == "reserved")
        assert provider.lockunspent(False, [carrier], True)
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_INPUT_LOCKED",
                                provider.submitpaymasterdigidollar, authorized["psbt"])
        self.nodes[0].unloadwallet(harness.provider_wallet_name)
        self.nodes[0].loadwallet(harness.provider_wallet_name)
        assert carrier in provider.listlockunspent()
        assert_raises_rpc_error(-4, "PAYMASTER_PROVIDER_INPUT_LOCKED",
                                provider.submitpaymasterdigidollar, authorized["psbt"])
        reserved = [entry for entry in provider.getpaymasterpoolinfo()["pool"]
                    if dict(txid=entry["txid"], vout=entry["vout"]) == carrier]
        assert_equal(len(reserved), 1)
        assert_equal(reserved[0]["state"], "reserved")
        # Removing the explicit manual veto never releases Paymaster's own
        # reservation. The exact authorized transaction can then continue.
        assert provider.lockunspent(True, [carrier])
        assert carrier in provider.listlockunspent()
        reserved = [entry for entry in provider.getpaymasterpoolinfo()["pool"]
                    if dict(txid=entry["txid"], vout=entry["vout"]) == carrier]
        assert_equal(reserved[0]["state"], "reserved")


if __name__ == "__main__":
    PaymasterLocksTest().main()
