#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Paymaster client and provider archival readiness boundaries.

These failures are intentionally checked at the public RPC boundary. A caller
must not bypass node-wide privacy, indexing, pruning, activation, or transport
requirements merely by selecting a different wallet or RPC entry point.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_raises_rpc_error


class PaymasterReadinessTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        common_args = [
            "-dandelion=0",
            "-v2transport=1",
        ]
        self.extra_args = [
            common_args + [
                "-digidollar=1", "-paymaster=0", "-prune=0", "-txindex=1",
            ],
            # On regtest, omitting an explicit -digidollar request lets the
            # node boot without txindex so the Paymaster RPC boundary itself
            # can be tested. Mainnet/testnet reject this configuration during
            # node initialization before any Paymaster RPC can run.
            common_args + ["-paymaster=1", "-prune=0", "-txindex=0"],
            common_args + [
                "-digidollar=1", "-paymaster=1", "-prune=1", "-txindex=0",
            ],
        ]

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        wallets = []
        for node in self.nodes:
            node.createwallet(
                wallet_name="readiness",
                descriptors=True,
                load_on_startup=True,
            )
            wallets.append(node.get_wallet_rpc("readiness"))

        self.generatetoaddress(
            self.nodes[0], 1, wallets[0].getnewaddress(address_type="bech32m"))
        self.sync_blocks()

        provider_id = "00" * 32
        self.log.info("Reject client operation when Paymaster support is disabled")
        assert_raises_rpc_error(
            -1,
            "DigiDollar Paymaster support is disabled",
            wallets[0].requestpaymasterquote,
            provider_id,
            {},
        )
        assert_raises_rpc_error(
            -1,
            "DigiDollar Paymaster support is disabled",
            self.nodes[0].listpaymasters,
        )
        assert "PAYMASTER_DISABLED" in (
            wallets[0].getpaymasterinfo()["readiness_errors"]
        )

        self.log.info("Reject client operation without txindex")
        assert_raises_rpc_error(
            -1,
            "PAYMASTER_REQUIRES_TXINDEX",
            wallets[1].requestpaymasterquote,
            provider_id,
            {},
        )
        assert "PAYMASTER_REQUIRES_TXINDEX" in (
            wallets[1].getpaymasterinfo()["readiness_errors"]
        )

        self.log.info("Reject client operation in pruning mode")
        assert_raises_rpc_error(
            -1,
            "PAYMASTER_REQUIRES_PRUNE_0",
            wallets[2].requestpaymasterquote,
            provider_id,
            {},
        )
        assert "PAYMASTER_REQUIRES_PRUNE_0" in (
            wallets[2].getpaymasterinfo()["readiness_errors"]
        )

        safe_client_args = [
            "-dandelion=0",
            "-digidollar=1",
            "-paymaster=1",
            "-prune=0",
            "-txindex=1",
        ]

        def restart_client(extra_args):
            self.restart_node(0, extra_args=safe_client_args + extra_args)
            self.connect_nodes(0, 1)

        self.log.info("Reject standard Paymaster operation without BIP324")
        restart_client(["-v2transport=0"])
        wallet = self.nodes[0].get_wallet_rpc("readiness")
        assert_raises_rpc_error(
            -1,
            "PAYMASTER_REQUIRES_V2_TRANSPORT",
            wallet.requestpaymasterquote,
            provider_id,
            {},
        )
        assert "PAYMASTER_REQUIRES_V2_TRANSPORT" in (
            wallet.getpaymasterinfo()["readiness_errors"]
        )

        recipient = wallet.getdigidollaraddress()

        def high_privacy_intent(request_id):
            return {
                "request_id": request_id,
                "address": recipient,
                "amount_cents": 100,
                "maximum_paymaster_fee_cents": 0,
                "privacy": "high",
                "maximum_provider_attempts": 1,
                "selected_inputs": [],
            }

        def assert_high_privacy_rejected(expected_error, request_id):
            current_wallet = self.nodes[0].get_wallet_rpc("readiness")
            assert_raises_rpc_error(
                -1,
                expected_error,
                current_wallet.requestpaymasterquote,
                provider_id,
                high_privacy_intent(request_id),
            )
            assert_raises_rpc_error(
                -4,
                "Paymaster session not found",
                current_wallet.getdigidollarsendsession,
                {"request_id": request_id},
            )

        self.log.info("Reject high privacy while IP logging is enabled")
        restart_client([
            "-v2transport=1",
            "-logips=1",
        ])
        assert_high_privacy_rejected(
            "PAYMASTER_HIGH_PRIVACY_REQUIRES_LOGIPS_0",
            "550e8400-e29b-41d4-a716-446655440201",
        )

        self.log.info("Reject high privacy without an onion proxy")
        restart_client(["-v2transport=1"])
        assert_high_privacy_rejected(
            "PAYMASTER_HIGH_PRIVACY_REQUIRES_ONION_PROXY",
            "550e8400-e29b-41d4-a716-446655440202",
        )

        self.log.info("Reject high privacy without Tor stream isolation")
        restart_client([
            "-v2transport=1",
            "-onion=127.0.0.1:9050",
            "-proxyrandomize=0",
        ])
        assert_high_privacy_rejected(
            "PAYMASTER_HIGH_PRIVACY_REQUIRES_PROXY_ISOLATION",
            "550e8400-e29b-41d4-a716-446655440203",
        )

        self.log.info("Reject high privacy while message capture is enabled")
        restart_client([
            "-v2transport=1",
            "-onion=127.0.0.1:9050",
            "-proxyrandomize=1",
            "-capturemessages=1",
        ])
        assert_high_privacy_rejected(
            "PAYMASTER_HIGH_PRIVACY_REQUIRES_CAPTUREMESSAGES_0",
            "550e8400-e29b-41d4-a716-446655440204",
        )

        self.log.info("Reject Paymaster operation before DigiDollar activation")
        restart_client([
            "-v2transport=1",
            "-digidollaractivationheight=100",
        ])
        wallet = self.nodes[0].get_wallet_rpc("readiness")
        assert_raises_rpc_error(
            -1,
            "PAYMASTER_DIGIDOLLAR_NOT_ACTIVE",
            wallet.requestpaymasterquote,
            provider_id,
            {},
        )
        assert "PAYMASTER_DIGIDOLLAR_NOT_ACTIVE" in (
            wallet.getpaymasterinfo()["readiness_errors"]
        )


if __name__ == "__main__":
    PaymasterReadinessTest().main()
