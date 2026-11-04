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
A conflicting quote request and an unrelated recovery submission must each be
rejected locally before the valid payment is processed, without changing either
party's financial state.
"""

import struct

from test_framework.address import base58_to_byte
from test_framework.messages import (
    COutPoint,
    ser_compact_size,
    ser_string,
    ser_uint256,
    ser_vector,
)
from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    assert_snapshot_equal,
    paymaster_node_args,
    value_snapshot,
    wait_for_paymaster_idle,
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

    def _send_direct_and_wait(self, node, peer_id, command, payload):
        peer = next(peer for peer in node.getpeerinfo() if peer["id"] == peer_id)
        pong_bytes = peer["bytesrecv_per_msg"].get("pong", 0)
        node.sendmsgtopeer(peer_id, command, payload.hex())
        # A subsequent pong on this stream proves that the provider processed
        # the injected envelope before the manual wallet queue is read.
        node.ping()
        self.wait_until(lambda: any(
            current["id"] == peer_id
            and current["bytesrecv_per_msg"].get("pong", 0) > pong_bytes
            for current in node.getpeerinfo()))

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
        recipient_address = recipient.getdigidollaraddress()
        options, quote, send = harness.wait_for_quote(
            request_id, recipient_address, 1_000)
        authorized = harness.authorize_quote(options, send)
        assert_equal(authorized["provider_id"], provider_id)

        def submit_delivered():
            send()
            return any(
                peer["inbound"]
                and peer["bytesrecv_per_msg"].get("pmsubmit", 0) > 0
                for peer in provider_node.getpeerinfo())

        self.wait_until(submit_delivered)
        peer = next(peer for peer in client_node.getpeerinfo()
                    if peer["connection_type"] == "paymaster")
        assert_equal(peer["transport_protocol_type"], "v2")

        self.log.info("Drain only exact quote retries before injecting a request-ID conflict")
        before_conflict = value_snapshot(provider_node, provider, client, recipient)
        session = client.getdigidollarsendsession({"request_id": request_id})
        provider_session = provider.getdigidollarsendsession({"request_id": request_id})
        assert_equal(session["session_id"], quote["session_id"])
        assert_equal(provider_session["session_id"], quote["session_id"])
        # Quote delivery can leave an exact retransmission behind. Bound the
        # drain by the already queued work; never consume the pending submit.
        pending_requests = provider.getpaymasterinfo()["service_queue"]["waiting_requests"]
        for _ in range(pending_requests):
            replay = wait_for_paymaster_idle(self, provider.processpaymasterrequests)
            assert_equal(replay["processed"], True)
            assert_equal(replay["queued"], True)
            assert_equal(replay["message_type"], "quote")
            assert_equal(replay["request_id"], request_id)
            assert_equal(replay["session_id"], quote["session_id"])
            assert_equal(replay["quote_id"], quote["quote_id"])
            assert_equal(replay["provider_id"], provider_id)
        assert_equal(wait_for_paymaster_idle(
            self, provider.processpaymasterrequests)["processed"], False)
        assert_snapshot_equal(
            before_conflict, value_snapshot(provider_node, provider, client, recipient))

        self.log.info("Reject a different session using the existing quote request ID")
        session_id = int(session["session_id"], 16)
        conflicting_session_id = session_id ^ 1
        if conflicting_session_id == 0:
            conflicting_session_id = 2
        assert conflicting_session_id != session_id
        assert all(int(session[key], 16) != 0 for key in
                   ("session_id", "offer_id", "policy_hash", "canonical_request_hash"))
        input_info = session["reserved_user_inputs"][0]
        outpoint = COutPoint(int(input_info["txid"], 16), input_info["vout"])
        # The existing Base58Check helper separates one version byte. RD uses
        # two (a3 a4); the remaining 32 bytes are this recipient's real P2TR key.
        recipient_payload, recipient_version = base58_to_byte(recipient_address)
        assert_equal(recipient_version, 0xa3)
        assert_equal(recipient_payload[0], 0xa4)
        assert_equal(len(recipient_payload), 33)
        recipient_script = b"\x51\x20" + recipient_payload[1:]
        # Use Core's observed time, including mocktime, inside the 60-second
        # envelope window. No clock advance or payment authorization is needed.
        expires_at = provider.getdigidollarsendsession({"request_id": request_id})["observed_at"] + 60
        # PaymasterQuoteRequest v6 / PaymentIntent v2 in their canonical field
        # order. One matching 64-byte input proof satisfies the envelope shape;
        # it is deliberately not a signature. The existing request-ID conflict
        # must be discarded before input-control proof verification or signing.
        conflicting_quote = (
            struct.pack("<HH", 6, 2)
            + ser_uint256(int(client_node.getblockhash(0), 16))
            + ser_uint256(int(provider_id, 16))
            + ser_string(request_id.encode())
            + ser_uint256(conflicting_session_id)
            + ser_uint256(session_id)  # nonzero client nonce
            + ser_vector([outpoint])
            + ser_string(recipient_script)
            + struct.pack("<q", 1_000)
            + ser_string(b"")  # no DD change script
            + ser_uint256(int(session["offer_id"], 16))
            + struct.pack("<BB", 0, 0)  # USER_PAID, PUBLIC
            + ser_uint256(int(session["policy_hash"], 16))
            + ser_uint256(0)  # no sponsorship authorization
            + struct.pack("<q", expires_at)
            + ser_compact_size(1) + outpoint.serialize() + ser_string(bytes(64))
            + ser_uint256(int(session["canonical_request_hash"], 16))
            + struct.pack("<BBB", 1, 0, 0)  # PAYMASTER, STANDARD, LOWEST_TOTAL_COST
            + b"\x00\x00"  # no restricted descriptor or capability
        )
        self._send_direct_and_wait(client_node, peer["id"], "pmquotereq", conflicting_quote)
        rejected = wait_for_paymaster_idle(self, provider.processpaymasterrequests)
        assert_equal(rejected["processed"], True)
        assert_equal(rejected["rejected"], True)
        assert_equal(rejected["queued"], False)
        assert_equal(rejected["message_type"], "quote")
        assert_equal(rejected["rejection_reason"], "PAYMASTER_REQUEST_ID_CONFLICT")
        # Rejection acknowledges only the injected message, exactly once.
        assert_equal(wait_for_paymaster_idle(
            self, provider.processpaymasterrequests)["processed"], False)
        assert_snapshot_equal(
            before_conflict, value_snapshot(provider_node, provider, client, recipient))
        for wallet, before_session in ((client, session), (provider, provider_session)):
            after_session = wallet.getdigidollarsendsession({"request_id": request_id})
            for field in ("request_id", "session_id", "canonical_request_hash",
                          "reserved_user_inputs", "provider_attempts", "session_state"):
                assert_equal(after_session[field], before_session[field])

        self.log.info("Reject an unknown recovery before completing the queued payment")
        # AlternativeRecoverySubmit v2, encoded in its existing field order.
        # Use the client's authenticated regtest channel and a distinct ID;
        # this envelope does not grant spending authority.
        recovery_payload = (
            struct.pack("<H", 2)
            + ser_uint256(int(client_node.getblockhash(0), 16))
            + ser_string(b"550e8400-e29b-41d4-a716-446655442299")
            + ser_uint256(0x91) + ser_uint256(0x92)
            + ser_uint256(int(provider_id, 16))
            + ser_uint256(0x93) + ser_uint256(0x94) + ser_uint256(0x95)
            + ser_string(b"\x01")
        )
        before_rejection = value_snapshot(provider_node, provider, client, recipient)
        self._send_direct_and_wait(client_node, peer["id"], "pmrecsub", recovery_payload)
        rejected = wait_for_paymaster_idle(self, provider.processpaymastersubmits)
        assert_equal(rejected["processed"], True)
        assert_equal(rejected["rejected"], True)
        assert_equal(rejected["queued"], False)
        assert_equal(rejected["message_type"], "recovery_submit")
        assert_equal(rejected["rejection_reason"], "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH")
        assert_snapshot_equal(
            before_rejection, value_snapshot(provider_node, provider, client, recipient))

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
        assert_equal(confirmed_session["status"], "success")
        assert_equal(confirmed_session["payment_confirmed"], True)
        assert_equal(confirmed_session["payment_view"]["payment"]["block_hash"], branch_a_block)
        assert_equal(confirmed_session["payment_view"]["payment"]["recipient"]["amount_cents"], 1_000)
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
        assert_equal(reorged_session["status"], "pending")
        assert_equal(reorged_session["payment_confirmed"], False)
        assert_equal(reorged_session["payment_view"]["payment"]["confirmations"], 0)
        assert "block_hash" not in reorged_session["payment_view"]["payment"]
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
        assert_equal(reconfirmed_session["status"], "success")
        assert_equal(reconfirmed_session["payment_confirmed"], True)
        assert_equal(reconfirmed_session["payment_view"]["payment"]["block_hash"], reconfirmation_block)
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
