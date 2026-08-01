#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise the Paymaster P2P negotiation and transport firewall."""

from test_framework.messages import (
    msg_pmcapreq,
    msg_pmcapresp,
    msg_pmquotereq,
    msg_pmquoteresp,
    msg_pmresult,
    msg_pmsubmit,
    msg_sendpmasters,
)
from test_framework.p2p import P2PInterface
from test_framework.paymaster import paymaster_node_args
from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal


PAYMASTER_PROTOCOL_VERSION = 5


class PaymasterP2PTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            paymaster_node_args(),
            ["-dandelion=0", "-digidollar=1", "-paymaster=0",
             "-prune=0", "-txindex=1", "-v2transport=1"],
        ]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("A node advertises support only when Paymaster is enabled")
        compatible = node.add_p2p_connection(P2PInterface())
        compatible.wait_until(
            lambda: compatible.message_count["sendpmasters"] == 1)
        assert_equal(
            compatible.last_message["sendpmasters"].version,
            PAYMASTER_PROTOCOL_VERSION)

        incompatible = self.nodes[1].add_p2p_connection(P2PInterface())
        incompatible.sync_with_ping()
        assert_equal(incompatible.message_count["sendpmasters"], 0)

        self.log.info("Invalid negotiation versions and capabilities fail closed")
        wrong_version = node.add_p2p_connection(P2PInterface())
        wrong_version.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION - 1, capabilities=0))
        assert_equal(wrong_version.message_count["getpmasters"], 0)

        unknown_capability = node.add_p2p_connection(P2PInterface())
        unknown_capability.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=1 << 31))
        assert_equal(unknown_capability.message_count["getpmasters"], 0)

        negotiated = node.add_p2p_connection(P2PInterface())
        negotiated.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=0))
        negotiated.wait_until(
            lambda: negotiated.message_count["getpmasters"] == 1)

        self.log.info("Payment-bearing messages always reject plaintext V1")
        # Every command gets an independent connection so the first expected
        # disconnect cannot mask a less strict handler for a later command.
        direct_messages = (
            msg_pmcapreq(b"\x00"),
            msg_pmcapresp(b"\x00"),
            msg_pmquotereq(b"\x00"),
            msg_pmquoteresp(b"\x00"),
            msg_pmsubmit(b"\x00"),
            msg_pmresult(b"\x00"),
        )
        for message in direct_messages:
            peer = node.add_p2p_connection(P2PInterface())
            peer.send_message(message)
            peer.wait_for_disconnect()

        self.log.info("Negotiation never upgrades an ordinary V1 connection")
        plaintext = node.add_p2p_connection(P2PInterface())
        plaintext.send_and_ping(msg_sendpmasters(
            version=PAYMASTER_PROTOCOL_VERSION, capabilities=0))
        plaintext.send_message(msg_pmcapreq(b"\x01"))
        plaintext.wait_for_disconnect()

        self.log.info("Oversized direct payloads are rejected before decoding")
        oversized = node.add_p2p_connection(P2PInterface())
        oversized.send_message(msg_pmsubmit(b"x" * (4 * 1024 * 1024 + 1)))
        oversized.wait_for_disconnect()


if __name__ == "__main__":
    PaymasterP2PTest().main()
