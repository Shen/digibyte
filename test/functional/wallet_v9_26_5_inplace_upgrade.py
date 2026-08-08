#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test an in-place official-v9.26.5 to current datadir upgrade.

The historical daemon creates encrypted descriptor wallets, active DigiDollar
positions, and disconnected unconfirmed DGB/DD transfers. It shuts down cleanly
and the current daemon starts on the exact same datadir. A wallet-disabled
first start proves that mempool persistence is independent of wallet
rebroadcast; a normal second start verifies wallet state and signing. The test
then confirms both pending transfers and redeems a position created by v9.26.5.
"""

from decimal import Decimal
import os
from pathlib import Path

from test_framework.test_framework import DigiByteTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


V9_26_5_CLIENT_VERSION = 92605
ORACLE_PRICE_MICRO_USD = 500_000
WALLET_PASSPHRASE = "v9.26.5-current-in-place"
SOURCE_WALLET = "v9_inplace_source"
ACTIVE_WALLET = "v9_inplace_active"
PEER_WALLET = "current_inplace_peer"
MINT_AMOUNT_CENTS = 10_000
PENDING_DD_CENTS = 3_000
PENDING_DGB = Decimal("4")
ACTIVE_FUNDING_DGB = Decimal("10000")

COMMON_ARGS = [
    "-digidollar=1",
    "-txindex=1",
    "-dandelion=0",
    "-fallbackfee=0.1",
]


class V9_26_5InplaceUpgradeTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [COMMON_ARGS.copy(), COMMON_ARGS.copy()]
        self.wallet_names = []

        self.v9_binary_env = None
        self.v9_binary_override = None
        for variable in ("V9_26_5_DIGIBYTED", "PRE_PAYMASTER_DIGIBYTED"):
            if value := os.getenv(variable):
                self.v9_binary_env = variable
                self.v9_binary_override = value
                break
        self.v9_binary = None

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def _locate_v9_binary(self):
        if self.v9_binary_override:
            return Path(self.v9_binary_override).expanduser()
        exeext = self.config["environment"]["EXEEXT"]
        return Path(
            self.options.previous_releases_path,
            "v9.26.5",
            "bin",
            f"digibyted{exeext}",
        )

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()
        binary = self._locate_v9_binary()
        if not binary.is_file():
            if self.v9_binary_env:
                raise AssertionError(
                    f"{self.v9_binary_env} does not name a file: {binary}")
            raise SkipTest(
                "DigiByte Core v9.26.5 is required at "
                f"{binary}. Set V9_26_5_DIGIBYTED or install the previous "
                "release under the functional-test releases directory."
            )
        self.v9_binary = str(binary)

    def setup_nodes(self):
        assert self.v9_binary is not None
        self.add_nodes(
            self.num_nodes,
            extra_args=self.extra_args,
            binary=[self.v9_binary, self.options.digibyted],
            binary_cli=[self.options.digibytecli] * self.num_nodes,
            versions=[260000, None],
        )
        self.start_nodes()

    @staticmethod
    def basic_wallet_state(wallet, txid):
        info = wallet.getwalletinfo()
        mine = wallet.getbalances()["mine"]
        transaction = wallet.gettransaction(txid)
        return {
            "wallet": {
                "format": info["format"],
                "descriptors": info["descriptors"],
                "walletversion": info["walletversion"],
                "private_keys_enabled": info["private_keys_enabled"],
                # Unencrypted descriptor wallets omit this field in v9.26.5.
                # Normalize omission to the equivalent locked-timer value so
                # the state comparison remains version-independent.
                "unlocked_until": info.get("unlocked_until", 0),
            },
            "balances": {
                "trusted": mine["trusted"],
                "untrusted_pending": mine["untrusted_pending"],
                "immature": mine["immature"],
            },
            "transaction": {
                "txid": transaction["txid"],
                "amount": transaction["amount"],
                "confirmations": transaction["confirmations"],
            },
        }

    @staticmethod
    def active_wallet_state(wallet, dd_txid, position_ids):
        info = wallet.getwalletinfo()
        balance = wallet.getdigidollarbalance()
        positions = {
            position["position_id"]: {
                "dd_minted": position["dd_minted"],
                "dgb_collateral": position["dgb_collateral"],
                "lock_tier": position["lock_tier"],
                "unlock_height": position["unlock_height"],
                "status": position["status"],
            }
            for position in wallet.listdigidollarpositions(False)
            if position["position_id"] in position_ids
        }
        history = sorted(
            (row["category"], row["amount"])
            for row in wallet.listdigidollartxs(1_000, 0)
            if row["txid"] == dd_txid
        )
        unspent = sorted(
            (row["txid"], row["vout"], row["amount"])
            for row in wallet.listdigidollarunspent()
        )
        return {
            "wallet": {
                "format": info["format"],
                "descriptors": info["descriptors"],
                "walletversion": info["walletversion"],
                "private_keys_enabled": info["private_keys_enabled"],
                "unlocked_until": info.get("unlocked_until", 0),
            },
            "balance": {
                "confirmed": balance["confirmed"],
                "unconfirmed": balance["unconfirmed"],
                "total": balance["total"],
            },
            "positions": positions,
            "unspent": unspent,
            "pending_history": history,
        }

    def run_test(self):
        old, peer_node = self.nodes
        old_network = old.getnetworkinfo()
        assert_equal(old_network["version"], V9_26_5_CLIENT_VERSION)
        assert "9.26.5" in old_network["subversion"]

        self.log.info("Create v9.26.5 wallets and two active DD positions")
        old.createwallet(
            wallet_name=SOURCE_WALLET, descriptors=True,
            load_on_startup=True)
        old.createwallet(
            wallet_name=ACTIVE_WALLET, descriptors=True,
            load_on_startup=True)
        peer_node.createwallet(
            wallet_name=PEER_WALLET, descriptors=True,
            load_on_startup=True)
        source = old.get_wallet_rpc(SOURCE_WALLET)
        active = old.get_wallet_rpc(ACTIVE_WALLET)
        peer = peer_node.get_wallet_rpc(PEER_WALLET)

        self.generatetoaddress(
            old, 110, source.getnewaddress(address_type="bech32m"))
        active_funding = source.sendtoaddress(
            active.getnewaddress(address_type="bech32m"),
            ACTIVE_FUNDING_DGB)
        self.generatetoaddress(old, 1, source.getnewaddress())
        assert active.gettransaction(active_funding)["confirmations"] > 0

        for node in self.nodes:
            node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        positions = []
        for _ in range(2):
            mint = active.mintdigidollar(MINT_AMOUNT_CENTS, 0)
            positions.append(mint["position_id"])
            self.generatetoaddress(old, 1, source.getnewaddress())
        assert_equal(active.getdigidollarbalance()["total"], 20_000)

        self.log.info("Create isolated unconfirmed DGB and DD wallet state")
        peer_dgb_address = peer.getnewaddress()
        peer_dd_address = peer.getdigidollaraddress()
        self.sync_blocks()
        self.disconnect_nodes(0, 1)
        active.encryptwallet(WALLET_PASSPHRASE)
        active.walletpassphrase(WALLET_PASSPHRASE, 600)
        pending_dgb_txid = source.sendtoaddress(
            peer_dgb_address, PENDING_DGB)
        pending_dd = active.senddigidollar(
            peer_dd_address, PENDING_DD_CENTS)
        pending_dd_txid = pending_dd["txid"]
        active.walletlock()
        pending_txids = {pending_dgb_txid, pending_dd_txid}
        assert_equal(set(old.getrawmempool()), pending_txids)
        assert_equal(peer_node.getrawmempool(), [])

        source_before = self.basic_wallet_state(source, pending_dgb_txid)
        active_before = self.active_wallet_state(
            active, pending_dd_txid, set(positions))
        assert_equal(source_before["transaction"]["confirmations"], 0)
        assert_equal(active_before["wallet"]["unlocked_until"], 0)
        assert active_before["pending_history"]

        chain_before = {
            "height": old.getblockcount(),
            "tip": old.getbestblockhash(),
            "tip_raw": old.getblock(old.getbestblockhash(), 0),
        }
        self.wait_until(lambda: old.getindexinfo()["txindex"]["synced"])
        raw_pending = {
            txid: old.getrawtransaction(txid)
            for txid in pending_txids
        }

        self.log.info("Stop v9.26.5 and switch the same TestNode/datadir binary")
        self.stop_node(0)
        old.binary = self.options.digibyted
        old.args[0] = self.options.digibyted
        old.version = None

        self.log.info("Load chainstate, blocks, txindex and mempool without wallets")
        self.start_node(0, COMMON_ARGS + ["-disablewallet"])
        upgraded = self.nodes[0]
        assert "getpaymasterinfo" in upgraded.help("getpaymasterinfo")
        assert_raises_rpc_error(
            -32601, "Method not found", upgraded.listwallets)
        assert_equal(upgraded.getblockcount(), chain_before["height"])
        assert_equal(upgraded.getbestblockhash(), chain_before["tip"])
        assert_equal(
            upgraded.getblock(chain_before["tip"], 0),
            chain_before["tip_raw"])
        self.wait_until(
            lambda: upgraded.getindexinfo()["txindex"]["synced"])
        assert_equal(set(upgraded.getrawmempool()), pending_txids)
        for txid in pending_txids:
            assert_equal(upgraded.getrawtransaction(txid), raw_pending[txid])

        self.log.info("Restart normally and load the original encrypted wallets")
        self.restart_node(0)
        upgraded = self.nodes[0]
        assert_equal(
            set(upgraded.listwallets()), {SOURCE_WALLET, ACTIVE_WALLET})
        source = upgraded.get_wallet_rpc(SOURCE_WALLET)
        active = upgraded.get_wallet_rpc(ACTIVE_WALLET)
        assert isinstance(active.getpaymasterinfo(), dict)
        assert_equal(
            self.basic_wallet_state(source, pending_dgb_txid),
            source_before)
        assert_equal(
            self.active_wallet_state(
                active, pending_dd_txid, set(positions)),
            active_before)
        assert_equal(set(upgraded.getrawmempool()), pending_txids)
        assert_raises_rpc_error(
            -13, "walletpassphrase", active.senddigidollar,
            peer_dd_address, 100)

        self.log.info("Relay and confirm both inherited pending transactions")
        self.connect_nodes(1, 0)
        # Loading mempool.dat does not promise an immediate INV when a peer is
        # connected later. Explicitly resubmitting an already-loaded exact
        # transaction is the supported idempotent reannouncement path.
        for txid in pending_txids:
            assert_equal(
                upgraded.sendrawtransaction(raw_pending[txid]), txid)
        self.sync_mempools()
        for txid in pending_txids:
            assert txid in peer_node.getrawmempool()
            assert_equal(
                peer_node.getrawtransaction(txid), raw_pending[txid])
        confirmation_block = self.generatetoaddress(
            peer_node, 1, peer.getnewaddress())[0]
        block_txids = set(peer_node.getblock(confirmation_block)["tx"])
        assert pending_txids.issubset(block_txids)
        assert source.gettransaction(pending_dgb_txid)["confirmations"] > 0
        assert_equal(peer.getbalance(), PENDING_DGB)
        assert_equal(peer.getdigidollarbalance()["total"], PENDING_DD_CENTS)
        confirmed_dd_rows = [
            row for row in active.listdigidollartxs(1_000, 0)
            if row["txid"] == pending_dd_txid and
            row["category"] == "send"
        ]
        assert_equal(len(confirmed_dd_rows), 1)
        assert_equal(confirmed_dd_rows[0].get("wallet_state"), "confirmed")

        self.log.info("Redeem a v9.26.5 position with the upgraded wallet")
        position = next(
            row for row in active.listdigidollarpositions(False)
            if row["position_id"] == positions[0]
        )
        blocks_to_unlock = max(
            0, position["unlock_height"] - upgraded.getblockcount() + 1)
        if blocks_to_unlock:
            self.generatetoaddress(
                upgraded, blocks_to_unlock, source.getnewaddress())
        for node in self.nodes:
            node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
        active.walletpassphrase(WALLET_PASSPHRASE, 600)
        redeem = active.redeemdigidollar(
            positions[0], MINT_AMOUNT_CENTS)
        assert_equal(len(redeem["txid"]), 64)
        self.generatetoaddress(upgraded, 1, source.getnewaddress())
        active.walletlock()
        redeemed_position = next(
            row for row in active.listdigidollarpositions(False)
            if row["position_id"] == positions[0]
        )
        assert_equal(redeemed_position["status"], "redeemed")
        assert_equal(active.getdigidollarbalance()["total"], 7_000)

        self.log.info("Restart the upgraded datadir with final state intact")
        self.restart_node(0)
        upgraded = self.nodes[0]
        self.connect_nodes(1, 0)
        active = upgraded.get_wallet_rpc(ACTIVE_WALLET)
        assert_equal(active.getwalletinfo()["unlocked_until"], 0)
        assert_equal(active.getdigidollarbalance()["total"], 7_000)
        final_position = next(
            row for row in active.listdigidollarpositions(False)
            if row["position_id"] == positions[0]
        )
        assert_equal(final_position["status"], "redeemed")


if __name__ == "__main__":
    V9_26_5InplaceUpgradeTest().main()
