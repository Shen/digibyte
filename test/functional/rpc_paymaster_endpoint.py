#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Nonfinancial endpoint diagnostics over TCP and a local SOCKS5 onion route.

The proxy forwards a fixture onion name to a local Paymaster listener. This
exercises real V2 negotiation through SOCKS5, not the public Tor network.
"""

import queue
import select
import socket
import socketserver
import threading

from test_framework.paymaster import paymaster_node_args, paymaster_port
from test_framework.socks5 import recvall
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port


ONION = "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"


class OnionProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, target):
        super().__init__(address, OnionProxyHandler)
        self.target = target
        self.observations = queue.Queue()
        self.require_auth = True


class OnionProxyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(10)
            assert_equal(recvall(self.request, 1), b"\x05")
            methods = recvall(self.request, recvall(self.request, 1)[0])
            if not self.server.require_auth:
                # A malicious/misconfigured proxy selects no-auth even though
                # this diagnostic requires isolated SOCKS credentials.
                self.request.sendall(b"\x05\x00")
                self.server.observations.put(("noauth", self.request.recv(1)))
                return
            assert 2 in methods
            self.request.sendall(b"\x05\x02")
            assert_equal(recvall(self.request, 1), b"\x01")
            username = recvall(self.request, recvall(self.request, 1)[0])
            password = recvall(self.request, recvall(self.request, 1)[0])
            self.request.sendall(b"\x01\x00")
            assert_equal(recvall(self.request, 4), b"\x05\x01\x00\x03")
            hostname = recvall(self.request, recvall(self.request, 1)[0]).decode("ascii")
            port = int.from_bytes(recvall(self.request, 2), "big")
            assert_equal(hostname, ONION)
            assert_equal(port, self.server.target[1])
            with socket.create_connection(self.server.target, timeout=10) as target:
                self.request.sendall(b"\x05\x00\x00\x01" + b"\x00" * 6)
                self.server.observations.put((hostname, port, username, password))
                while True:
                    readable, _, _ = select.select([self.request, target], [], [], 10)
                    if not readable:
                        return
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            return
                        (target if source is self.request else self.request).sendall(data)
        except (ConnectionResetError, BrokenPipeError):
            pass  # The bounded diagnostic closes its socket after observation.
        except Exception as error:
            self.server.observations.put(error)


class PaymasterEndpointTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [self.node_args(), self.node_args(1)]

    @staticmethod
    def node_args(provider=None):
        return [arg for arg in paymaster_node_args(provider) if not arg.startswith("-onion=")] + [
            "-digidollaractivationheight=0", "-disablewallet=1", "-logips=0", "-listenonion=0",
        ]

    def setup_network(self):
        self.setup_nodes()  # No relay peers or wallets are needed for the probe.

    def check_success(self, endpoint):
        node, provider = self.nodes
        before = [n.getrawmempool() for n in self.nodes]
        result = node.checkpaymasterendpoint(endpoint)
        assert_equal(result["endpoint"], endpoint)
        assert_equal(result["network"], "regtest")
        assert_equal(result["transport_ready"], True)
        assert_equal(result["state"], "transport_ready")
        assert_equal(result["stages"], {"v2_transport": "passed", "paymaster_negotiation": "passed"})
        assert_equal(result["payment_verified"], False)
        assert_equal(result["identity_verified"], False)
        assert "not checked" in result["message"]
        assert_equal(before, [n.getrawmempool() for n in self.nodes])
        self.wait_until(lambda: not node.getpeerinfo() and not provider.getpeerinfo())

    def run_test(self):
        node = self.nodes[0]
        endpoint = f"127.0.0.1:{paymaster_port(1)}"
        onion_endpoint = f"{ONION}:{paymaster_port(1)}"
        self.log.info("A listener without a wallet, identity or reserves is reachable")
        for invalid in ["example.org:12025", "127.0.0.1:0", "invalid.onion:12025"]:
            assert_raises_rpc_error(-8, "ENDPOINT_INVALID", node.checkpaymasterendpoint, invalid)
        self.check_success(endpoint)
        assert_raises_rpc_error(-1, "RATE_LIMIT", node.checkpaymasterendpoint, endpoint)

        self.log.info("Missing Tor prerequisites explain why no check was attempted")
        self.restart_node(0, self.node_args() + ["-onion=0"])
        result = node.checkpaymasterendpoint(onion_endpoint)
        assert_equal(result["state"], "privacy_rejected")
        assert_equal(result["transport_ready"], False)
        assert "SOCKS5 proxy" in result["message"]
        assert_equal(node.getpeerinfo(), [])

        with OnionProxy(("127.0.0.1", p2p_port(2)), ("127.0.0.1", paymaster_port(1))) as proxy:
            worker = threading.Thread(target=proxy.serve_forever, daemon=True)
            worker.start()
            try:
                args = self.node_args() + [f"-onion=127.0.0.1:{p2p_port(2)}"]
                for option, explanation in [("-proxyrandomize=0", "-proxyrandomize=1"), ("-logips=1", "-logips=0")]:
                    self.restart_node(0, args + [option])
                    result = node.checkpaymasterendpoint(onion_endpoint)
                    assert_equal(result["state"], "privacy_rejected")
                    assert explanation in result["message"]
                    assert proxy.observations.empty()

                self.log.info("Onion diagnostics use SOCKS5 auth and negotiate real V2/Paymaster")
                credentials = []
                for _ in range(2):
                    self.restart_node(0, args)
                    self.check_success(onion_endpoint)
                    hostname, port, username, password = proxy.observations.get(timeout=10)
                    assert_equal((hostname, port), (ONION, paymaster_port(1)))
                    assert username and password
                    credentials.append((username, password))
                assert credentials[0] != credentials[1]

                self.log.info("A proxy selecting no-auth cannot receive the onion destination")
                proxy.require_auth = False
                self.restart_node(0, args)
                result = node.checkpaymasterendpoint(onion_endpoint)
                assert_equal(result["transport_ready"], False)
                assert_equal(result["state"], "connect_failed")
                assert_equal(proxy.observations.get(timeout=10), ("noauth", b""))
            finally:
                proxy.shutdown()
                worker.join(timeout=10)

        self.log.info("A closed port fails without claiming failed identity or payment verification")
        self.restart_node(0, self.node_args())
        result = node.checkpaymasterendpoint(f"127.0.0.1:{p2p_port(2)}")
        assert_equal(result["state"], "connect_failed")
        assert_equal(result["transport_ready"], False)
        assert_equal(result["stages"], {"v2_transport": "unknown", "paymaster_negotiation": "unknown"})
        self.wait_until(lambda: not node.getpeerinfo())


if __name__ == "__main__":
    PaymasterEndpointTest().main()
