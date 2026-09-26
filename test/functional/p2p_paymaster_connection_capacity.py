#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check effective budgets, isolated admission, V1 rejection and permit release.

Use one daemon and cheap local sockets. Financial flows and real V2 Direct
sessions remain covered by wallet_paymaster_provider and failover; this fixture
does not claim a Tor or 16-client payment load test.
"""

import socket
import time

from test_framework.p2p import P2PInterface
from test_framework.paymaster import paymaster_node_args, paymaster_port
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


class PaymasterConnectionCapacityTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [paymaster_node_args(0) + ["-maxconnections=45"]]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("capacity", descriptors=True, load_on_startup=True)

        def transport():
            return node.get_wallet_rpc("capacity").getpaymasterclientinfo()["transport"]

        state = transport()
        assert_equal(state["outbound_limit"], 1)
        assert_equal(state["inbound_limit"], 16)
        assert_equal(state["handshake_limit"], 8)
        assert_equal(state["listener_ready"], True)

        self.log.info("One clearnet group cannot monopolize the separate handshake reserve")
        ordinary = node.add_p2p_connection(P2PInterface())
        sockets = []
        try:
            for _ in range(4):
                sockets.append(socket.create_connection(("127.0.0.1", paymaster_port(0)), timeout=5))
            self.wait_until(lambda: transport()["handshakes_in_use"] == 4)
            with socket.create_connection(("127.0.0.1", paymaster_port(0)), timeout=5) as rejected:
                assert_equal(rejected.recv(1), b"")
            assert_equal(transport()["handshakes_in_use"], 4)
            assert_equal(transport()["inbound_in_use"], 0)
            ordinary.sync_with_ping()
        finally:
            for connection in sockets:
                connection.close()
        self.wait_until(lambda: transport()["handshakes_in_use"] == 0)

        self.log.info("Forwarded onion sockets share global limits and cannot pin idle handshakes")
        node.disconnect_p2ps()
        # Model a trusted Tor forwarding listener without launching a Tor process.
        onion_args = [
            arg + "=onion" if arg.startswith("-paymasterbind=") else arg
            for arg in paymaster_node_args(0)
        ]
        self.restart_node(0, onion_args + ["-maxconnections=45"])
        ordinary = node.add_p2p_connection(P2PInterface())
        sockets = []
        try:
            for _ in range(8):
                sockets.append(socket.create_connection(("127.0.0.1", paymaster_port(0)), timeout=5))
            self.wait_until(lambda: transport()["handshakes_in_use"] == 8)
            started = time.monotonic()
            self.wait_until(lambda: time.monotonic() - started >= 5.1, timeout=10)
            replacement = socket.create_connection(("127.0.0.1", paymaster_port(0)), timeout=5)
            sockets.append(replacement)
            # New admission closes the oldest inactive socket before reusing
            # its permit. The application-work pool and ordinary relay survive.
            assert_equal(sockets[0].recv(1), b"")
            self.wait_until(lambda: transport()["handshakes_in_use"] == 8)
            assert_equal(transport()["inbound_in_use"], 0)
            ordinary.sync_with_ping()
        finally:
            for connection in sockets:
                connection.close()
        self.wait_until(lambda: transport()["handshakes_in_use"] == 0)

        self.log.info("V1 cannot promote, and disconnect releases its handshake permit")
        legacy = node.add_p2p_connection(
            P2PInterface(), dstport=paymaster_port(0), wait_for_verack=False)
        legacy.wait_for_disconnect()
        self.wait_until(lambda: transport()["handshakes_in_use"] == 0)
        assert_equal(transport()["inbound_in_use"], 0)
        ordinary.sync_with_ping()
        node.disconnect_p2ps()

        self.log.info("Impossible provider budget disables the role instead of stealing relay slots")
        self.restart_node(0, paymaster_node_args(0) + ["-maxconnections=32"])
        state = transport()
        assert_equal(state["outbound_limit"], 1)
        assert_equal(state["inbound_limit"], 0)
        assert_equal(state["handshake_limit"], 0)
        assert_equal(state["listener_ready"], False)

        self.log.info("Too-small client budget is visible, and an explicit four-channel budget works")
        self.restart_node(0, paymaster_node_args() + ["-maxconnections=16"])
        assert_equal(transport()["outbound_limit"], 0)
        assert "PAYMASTER_NO_LOCAL_DIRECT_CAPACITY" in node.get_wallet_rpc("capacity").getpaymasterclientinfo()["readiness_errors"]
        self.restart_node(0, paymaster_node_args(0) + ["-maxconnections=48", "-paymastermaxoutbound=4"])
        assert_equal(transport()["outbound_limit"], 4)
        assert_equal(transport()["inbound_limit"], 16)
        assert_equal(transport()["listener_ready"], True)

        self.log.info("Disabling Paymaster removes both Direct reservations")
        self.restart_node(0, paymaster_node_args(0) + ["-paymaster=0", "-maxconnections=45"])
        assert_equal(transport()["outbound_limit"], 0)
        assert_equal(transport()["inbound_limit"], 0)
        assert_equal(transport()["listener_ready"], False)


if __name__ == "__main__":
    PaymasterConnectionCapacityTest().main()
