#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""AUTO preparation must never authorize an ordinary direct DD payment."""

from concurrent.futures import ThreadPoolExecutor

from test_framework.authproxy import JSONRPCException
from test_framework.paymaster import paymaster_node_args
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, get_rpc_proxy, rpc_url


class PaymasterAutomaticFundingTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [paymaster_node_args()]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="sender", descriptors=True, load_on_startup=True)
        node.createwallet(wallet_name="recipient", descriptors=True, load_on_startup=True)
        sender = node.get_wallet_rpc("sender")
        recipient = node.get_wallet_rpc("recipient")
        self.generatetoaddress(node, 110, sender.getnewaddress(address_type="bech32m"))
        node.setmockoracleprice(500_000)
        assert_equal(sender.mintdigidollar(3000, 0)["dd_minted"], 3000)
        self.generatetoaddress(node, 1, sender.getnewaddress())
        address = recipient.getdigidollaraddress()
        options = {
            "fee_mode": "auto",
            "request_id": "550e8400-e29b-41d4-a716-446655440010",
            "maximum_paymaster_fee_cents": 100,
        }

        def snapshot():
            return (sender.getwalletinfo()["txcount"], node.getrawmempool(),
                    sender.getdigidollarbalance()["total"],
                    recipient.getdigidollarbalance()["total"])

        def reject_direct(wallet, flags, structured=True):
            try:
                wallet.senddigidollar(address, 200, "", 0, None, "cents", dict(options, **flags))
            except JSONRPCException as error:
                assert_equal(error.error["code"], -8)
                assert "PAYMASTER_AUTO_DIRECT_REQUIRES_EXPLICIT_DGB" in error.error["message"]
                if structured:
                    data = error.error["data"]
                    assert_equal(data["paymaster_code"], "PAYMASTER_AUTO_DIRECT_REQUIRES_EXPLICIT_DGB")
                    assert_equal(data["disposition"], "retry_before_execution")
                    # A preflight read is no proof of absence under concurrency.
                    assert data.get("session_absent") is not True
            else:
                raise AssertionError("AUTO preparation unexpectedly executed a direct payment")

        self.log.info("Own DGB must not bypass preparation or exact authorization")
        before = snapshot()
        for flags in ({"prepare_only": True}, {"prepare_only": False}, {},
                      {"authorization_commitment": "01" * 32}):
            reject_direct(sender, flags)
            assert_equal(snapshot(), before)
        reject_direct(node.cli("-rpcwallet=sender"), {"prepare_only": True}, structured=False)
        assert_equal(snapshot(), before)

        # Independent HTTP clients avoid sharing an RPC connection across threads.
        self.log.info("Concurrent same-UUID preparation cannot create two payments")
        base_url = node.url or rpc_url(node.datadir_path, node.index, node.chain, node.rpchost)
        clients = [get_rpc_proxy(base_url.rstrip("/") + "/wallet/sender", node.index) for _ in range(2)]
        with ThreadPoolExecutor(max_workers=2) as workers:
            pending = [workers.submit(reject_direct, wallet, {"prepare_only": True}) for wallet in clients]
            for result in pending:
                result.result()
        assert_equal(snapshot(), before)
        assert_equal(sender.listdigidollarsendsessions({"active_only": False})["count"], 0)

        self.log.info("Explicit ordinary DGB sending remains available")
        sent = sender.senddigidollar(address, 200, "", 0, None, "cents", {"fee_mode": "dgb"})
        assert sent["txid"] in node.getrawmempool()
        self.generatetoaddress(node, 1, sender.getnewaddress())
        assert_equal(recipient.getdigidollarbalance()["total"], 200)

        # The new confirmed change must not turn the earlier UUID into a payment.
        before = snapshot()
        reject_direct(sender, {"prepare_only": True})
        assert_equal(snapshot(), before)

        self.log.info("Ordinary DD sending remains available with Paymaster disabled")
        self.restart_node(0, [arg for arg in self.extra_args[0] if arg != "-paymaster=1"] + ["-paymaster=0"])
        sender = node.get_wallet_rpc("sender")
        recipient = node.get_wallet_rpc("recipient")
        sent = sender.senddigidollar(address, 200)
        assert sent["txid"] in node.getrawmempool()
        self.generatetoaddress(node, 1, sender.getnewaddress())
        assert_equal(recipient.getdigidollarbalance()["total"], 400)


if __name__ == "__main__":
    PaymasterAutomaticFundingTest().main()
