#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test direct wallet-file compatibility with official DigiByte Core v9.26.5.

The test creates and uses an encrypted wallet with the v9.26.5 daemon, copies
the unloaded wallet directory to the current daemon, and verifies that wallet
state and signing remain usable without an explicit wallet upgrade. It then
copies the current wallet back to v9.26.5 to catch an incompatible automatic
format or wallet-flag change.

The descriptor variant additionally covers an active DigiDollar position,
balance, UTXO, and transaction history. DigiDollar V1 intentionally does not
support those records in legacy BDB wallets. While both daemons are running,
the test also sends DGB in both directions for both wallet types and DD in both
directions for descriptor wallets. The opposite version confirms each transfer.
"""

from decimal import Decimal
import os
from pathlib import Path
import shutil

from test_framework.test_framework import DigiByteTestFramework, SkipTest
from test_framework.util import assert_equal, assert_raises_rpc_error


V9_26_5_CLIENT_VERSION = 92605
ORACLE_PRICE_MICRO_USD = 500000
FUNDING_AMOUNT_DGB = Decimal("10000")
PRE_UPGRADE_SEND_DGB = Decimal("3")
POST_UPGRADE_SEND_DGB = Decimal("1")
OLD_TO_CURRENT_DGB = Decimal("20")
CURRENT_TO_OLD_DGB = Decimal("7")
MINT_AMOUNT_CENTS = 10000
POST_UPGRADE_SEND_CENTS = 1000
OLD_TO_CURRENT_DD_CENTS = 2000
CURRENT_TO_OLD_DD_CENTS = 500

SOURCE_WALLET = "v9_26_5_source"
ACTIVE_WALLET = "v9_26_5_active"
PEER_WALLET = "current_peer"
SINK_WALLET = "current_sink"
ROUNDTRIP_WALLET = "v9_26_5_roundtrip"
PERSISTENT_LABEL = "v9.26.5 compatibility"
WALLET_PASSPHRASE = "v9.26.5-current-roundtrip"

COMMON_ARGS = [
    "-digidollar=1",
    "-txindex=1",
    "-dandelion=0",
    "-fallbackfee=0.1",
]


class V9_26_5WalletCompatibilityTest(DigiByteTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [COMMON_ARGS.copy(), COMMON_ARGS.copy()]
        # This test creates named wallets explicitly on each binary.
        self.wallet_names = []

        self.v9_binary_env = None
        self.v9_binary_override = None
        for variable in ("V9_26_5_DIGIBYTED", "PRE_PAYMASTER_DIGIBYTED"):
            if value := os.getenv(variable):
                self.v9_binary_env = variable
                self.v9_binary_override = value
                break

        self.v9_binary = None
        self.position_id = None
        self.tracked_addresses = []
        self.tracked_txids = []
        self.tracked_dd_txids = []
        self.peer_receive_address = None
        self.peer_txids = []
        self.peer_dd_txids = []
        self.expected_active_dd_balance = None

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
        binary = self._locate_v9_binary()
        if not binary.is_file():
            if self.v9_binary_env:
                raise AssertionError(
                    f"{self.v9_binary_env} does not name a file: {binary}"
                )
            raise SkipTest(
                "DigiByte Core v9.26.5 is required at "
                f"{binary}. Build it with "
                "test/get_previous_releases.py -d v9.26.5 or set "
                "V9_26_5_DIGIBYTED."
            )
        self.v9_binary = str(binary)

    def setup_nodes(self):
        assert self.v9_binary is not None
        self.add_nodes(
            self.num_nodes,
            extra_args=self.extra_args,
            binary=[self.v9_binary, self.options.digibyted],
            # The current CLI uses the unchanged JSON-RPC transport required
            # by the official v9.26.5 daemon.
            binary_cli=[self.options.digibytecli] * self.num_nodes,
            # v9.26.5 derives from the v26 code line. This value controls only
            # test-framework argument/RPC compatibility because the explicit
            # daemon path above remains authoritative.
            versions=[260000, None],
        )
        self.start_nodes()

    def _assert_official_v9_binary(self, node):
        network_info = node.getnetworkinfo()
        assert_equal(network_info["version"], V9_26_5_CLIENT_VERSION)
        assert "9.26.5" in network_info["subversion"]
        assert_raises_rpc_error(
            -32601,
            "Method not found",
            node.getpaymasterinfo,
        )

    def _copy_wallet(self, source_node, source_name, target_node, target_name):
        source = source_node.wallets_path / source_name
        target = target_node.wallets_path / target_name
        assert source.is_dir(), f"Wallet directory does not exist: {source}"
        assert not target.exists(), f"Wallet copy target already exists: {target}"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target)

    def _wallet_state(self, wallet):
        info = wallet.getwalletinfo()
        mine = wallet.getbalances()["mine"]
        state = {
            "wallet_info": {
                "format": info["format"],
                "descriptors": info["descriptors"],
                "walletversion": info["walletversion"],
                "private_keys_enabled": info["private_keys_enabled"],
                "txcount": info["txcount"],
                "unlocked_until": info["unlocked_until"],
            },
            "balance": wallet.getbalance(),
            "mine": {
                "trusted": mine["trusted"],
                "untrusted_pending": mine["untrusted_pending"],
                "immature": mine["immature"],
            },
            "labels": wallet.getaddressesbylabel(PERSISTENT_LABEL),
            "addresses": {},
            "transactions": {},
        }

        for address in sorted(self.tracked_addresses):
            address_info = wallet.getaddressinfo(address)
            state["addresses"][address] = {
                "ismine": address_info["ismine"],
                "iswatchonly": address_info["iswatchonly"],
                "solvable": address_info["solvable"],
            }

        for txid in sorted(self.tracked_txids):
            transaction = wallet.gettransaction(txid)
            state["transactions"][txid] = {
                "amount": transaction["amount"],
            }

        return state

    def _digidollar_state(self, wallet):
        balance = wallet.getdigidollarbalance()
        position = next(
            entry
            for entry in wallet.listdigidollarpositions(False)
            if entry["position_id"] == self.position_id
        )
        unspent = sorted(
            (entry["txid"], entry["vout"], entry["amount"])
            for entry in wallet.listdigidollarunspent()
        )
        history = sorted(
            (entry["txid"], entry["category"], entry["amount"])
            for entry in wallet.listdigidollartxs(1000, 0)
            if entry["txid"] in self.tracked_dd_txids
        )
        return {
            "balance": {
                "confirmed": balance["confirmed"],
                "unconfirmed": balance["unconfirmed"],
                "total": balance["total"],
            },
            "position": {
                "position_id": position["position_id"],
                "dd_minted": position["dd_minted"],
                "dgb_collateral": position["dgb_collateral"],
                "lock_tier": position["lock_tier"],
                "unlock_height": position["unlock_height"],
                "status": position["status"],
            },
            "unspent": unspent,
            "history": history,
        }

    def _state(self, wallet):
        state = {"wallet": self._wallet_state(wallet)}
        if self.options.descriptors:
            state["digidollar"] = self._digidollar_state(wallet)
        return state

    def _peer_state(self, wallet):
        info = wallet.getwalletinfo()
        assert self.peer_receive_address is not None
        state = {
            "wallet_info": {
                "format": info["format"],
                "descriptors": info["descriptors"],
                "walletversion": info["walletversion"],
                "private_keys_enabled": info["private_keys_enabled"],
                "txcount": info["txcount"],
            },
            "balance": wallet.getbalance(),
            "labels": wallet.getaddressesbylabel(PERSISTENT_LABEL),
            "receive_address_owned": wallet.getaddressinfo(
                self.peer_receive_address
            )["ismine"],
            "transactions": {
                txid: wallet.gettransaction(txid)["amount"]
                for txid in sorted(self.peer_txids)
            },
        }
        if self.options.descriptors:
            balance = wallet.getdigidollarbalance()
            state["digidollar"] = {
                "balance": {
                    "confirmed": balance["confirmed"],
                    "unconfirmed": balance["unconfirmed"],
                    "total": balance["total"],
                },
                "unspent": sorted(
                    (entry["txid"], entry["vout"], entry["amount"])
                    for entry in wallet.listdigidollarunspent()
                ),
                "history": sorted(
                    (entry["txid"], entry["category"], entry["amount"])
                    for entry in wallet.listdigidollartxs(1000, 0)
                    if entry["txid"] in self.peer_dd_txids
                ),
            }
        return state

    def _relay_and_confirm(self, txid, confirming_node):
        self.sync_mempools()
        for node in self.nodes:
            assert txid in node.getrawmempool()
        assert_equal(
            self.nodes[0].getrawtransaction(txid),
            self.nodes[1].getrawtransaction(txid),
        )

        # Confirm with the version that received the transaction. This proves
        # more than passive chain synchronization: the opposite implementation
        # must accept the transaction into its block template and mine it.
        block_hash = self.generate(confirming_node, 1)[0]
        for node in self.nodes:
            node.syncwithvalidationinterfacequeue()
            assert txid not in node.getrawmempool()
        assert_equal(
            self.nodes[0].getrawtransaction(txid, False, block_hash),
            self.nodes[1].getrawtransaction(txid, False, block_hash),
        )

    def _assert_dgb_transfer(self, txid, sender, receiver, amount, confirming_node):
        self._relay_and_confirm(txid, confirming_node)
        sender_tx = sender.gettransaction(txid)
        receiver_tx = receiver.gettransaction(txid)
        assert_equal(sender_tx["amount"], -amount)
        assert_equal(receiver_tx["amount"], amount)
        assert sender_tx["confirmations"] > 0
        assert receiver_tx["confirmations"] > 0

    def _assert_dd_transfer(self, txid, sender, receiver, amount, confirming_node):
        self._relay_and_confirm(txid, confirming_node)
        sender_rows = [
            entry
            for entry in sender.listdigidollartxs(1000, 0)
            if entry["txid"] == txid
        ]
        receiver_rows = [
            entry
            for entry in receiver.listdigidollartxs(1000, 0)
            if entry["txid"] == txid
        ]
        assert any(
            entry["category"] == "send" and entry["amount"] == -amount
            for entry in sender_rows
        )
        assert any(
            entry["category"] == "receive" and entry["amount"] == amount
            for entry in receiver_rows
        )

    def _refresh_oracle_quotes(self):
        for node in self.nodes:
            result = node.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
            assert_equal(result["price_micro_usd"], ORACLE_PRICE_MICRO_USD)

    def _mine_one(self, node):
        self.generate(node, 1, sync_fun=self.no_op)
        node.syncwithvalidationinterfacequeue()

    def run_test(self):
        old, current = self.nodes
        self._assert_official_v9_binary(old)

        wallet_type = "descriptor/SQLite" if self.options.descriptors else "legacy/BDB"
        self.log.info("Create an active encrypted %s wallet with v9.26.5", wallet_type)
        old.createwallet(
            wallet_name=SOURCE_WALLET,
            descriptors=self.options.descriptors,
            load_on_startup=True,
        )
        old.createwallet(
            wallet_name=ACTIVE_WALLET,
            descriptors=self.options.descriptors,
            load_on_startup=True,
        )
        source = old.get_wallet_rpc(SOURCE_WALLET)
        active = old.get_wallet_rpc(ACTIVE_WALLET)

        expected_format = "sqlite" if self.options.descriptors else "bdb"
        assert_equal(active.getwalletinfo()["format"], expected_format)

        self.generatetoaddress(old, 110, source.getnewaddress())
        receive_address = active.getnewaddress(PERSISTENT_LABEL)
        self.tracked_addresses.append(receive_address)
        funding_txid = source.sendtoaddress(receive_address, FUNDING_AMOUNT_DGB)
        self.generatetoaddress(old, 1, source.getnewaddress())

        outgoing_txid = active.sendtoaddress(
            source.getnewaddress(),
            PRE_UPGRADE_SEND_DGB,
        )
        self.generatetoaddress(old, 1, source.getnewaddress())
        self.tracked_txids.extend([funding_txid, outgoing_txid])

        self.log.info("Send DGB directly between v9.26.5 and current wallets")
        current.createwallet(
            wallet_name=PEER_WALLET,
            descriptors=self.options.descriptors,
            load_on_startup=True,
        )
        peer = current.get_wallet_rpc(PEER_WALLET)
        self.peer_receive_address = peer.getnewaddress(PERSISTENT_LABEL)
        assert_equal(
            active.validateaddress(self.peer_receive_address)["isvalid"],
            True,
        )
        assert_equal(peer.validateaddress(receive_address)["isvalid"], True)

        old_to_current_dgb = active.sendtoaddress(
            self.peer_receive_address,
            OLD_TO_CURRENT_DGB,
        )
        self.tracked_txids.append(old_to_current_dgb)
        self.peer_txids.append(old_to_current_dgb)
        self._assert_dgb_transfer(
            old_to_current_dgb,
            active,
            peer,
            OLD_TO_CURRENT_DGB,
            current,
        )

        current_to_old_dgb = peer.sendtoaddress(
            receive_address,
            CURRENT_TO_OLD_DGB,
        )
        self.tracked_txids.append(current_to_old_dgb)
        self.peer_txids.append(current_to_old_dgb)
        self._assert_dgb_transfer(
            current_to_old_dgb,
            peer,
            active,
            CURRENT_TO_OLD_DGB,
            old,
        )

        if self.options.descriptors:
            self._refresh_oracle_quotes()
            mint = active.mintdigidollar(MINT_AMOUNT_CENTS, 0)
            self.position_id = mint["position_id"]
            self.tracked_txids.append(mint["txid"])
            self.tracked_dd_txids.append(mint["txid"])
            self.generatetoaddress(old, 1, source.getnewaddress())

            self.log.info("Send DD directly between v9.26.5 and current wallets")
            self._refresh_oracle_quotes()
            peer_dd_address = peer.getdigidollaraddress()
            assert_equal(active.validateddaddress(peer_dd_address)["isvalid"], True)
            old_to_current_dd = active.senddigidollar(
                peer_dd_address,
                OLD_TO_CURRENT_DD_CENTS,
            )
            self.tracked_txids.append(old_to_current_dd["txid"])
            self.tracked_dd_txids.append(old_to_current_dd["txid"])
            self.peer_dd_txids.append(old_to_current_dd["txid"])
            self._assert_dd_transfer(
                old_to_current_dd["txid"],
                active,
                peer,
                OLD_TO_CURRENT_DD_CENTS,
                current,
            )

            self._refresh_oracle_quotes()
            active_dd_address = active.getdigidollaraddress()
            assert_equal(peer.validateddaddress(active_dd_address)["isvalid"], True)
            current_to_old_dd = peer.senddigidollar(
                active_dd_address,
                CURRENT_TO_OLD_DD_CENTS,
            )
            self.tracked_txids.append(current_to_old_dd["txid"])
            self.tracked_dd_txids.append(current_to_old_dd["txid"])
            self.peer_dd_txids.append(current_to_old_dd["txid"])
            self._assert_dd_transfer(
                current_to_old_dd["txid"],
                peer,
                active,
                CURRENT_TO_OLD_DD_CENTS,
                old,
            )
            self.expected_active_dd_balance = (
                MINT_AMOUNT_CENTS
                - OLD_TO_CURRENT_DD_CENTS
                + CURRENT_TO_OLD_DD_CENTS
            )
            assert_equal(
                active.getdigidollarbalance()["total"],
                self.expected_active_dd_balance,
            )
            assert_equal(
                peer.getdigidollarbalance()["total"],
                OLD_TO_CURRENT_DD_CENTS - CURRENT_TO_OLD_DD_CENTS,
            )

        peer_state = self._peer_state(peer)

        active.encryptwallet(WALLET_PASSPHRASE)
        assert_equal(active.getwalletinfo()["unlocked_until"], 0)
        old.syncwithvalidationinterfacequeue()
        v9_state = self._state(active)

        self.log.info("Load the raw v9.26.5 wallet directory with the current daemon")
        old.unloadwallet(ACTIVE_WALLET, False)
        self.stop_node(0)
        self._copy_wallet(old, ACTIVE_WALLET, current, ACTIVE_WALLET)

        current.loadwallet(ACTIVE_WALLET, True)
        current.syncwithvalidationinterfacequeue()
        active = current.get_wallet_rpc(ACTIVE_WALLET)
        paymaster_info = active.getpaymasterinfo()
        assert isinstance(paymaster_info, dict)
        assert_equal(self._state(active), v9_state)

        self.log.info("Restart the current daemon and verify automatic wallet loading")
        self.restart_node(1)
        current = self.nodes[1]
        current.syncwithvalidationinterfacequeue()
        active = current.get_wallet_rpc(ACTIVE_WALLET)
        peer = current.get_wallet_rpc(PEER_WALLET)
        assert_equal(self._state(active), v9_state)
        assert_equal(self._peer_state(peer), peer_state)

        current.createwallet(
            wallet_name=SINK_WALLET,
            descriptors=self.options.descriptors,
            load_on_startup=True,
        )
        sink = current.get_wallet_rpc(SINK_WALLET)

        self.log.info("Prove the migrated wallet remains encrypted and can still sign")
        if self.options.descriptors:
            current.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
            destination = sink.getdigidollaraddress()
            assert_raises_rpc_error(
                -13,
                "walletpassphrase",
                active.senddigidollar,
                destination,
                POST_UPGRADE_SEND_CENTS,
            )
        else:
            destination = sink.getnewaddress()
            assert_raises_rpc_error(
                -13,
                "walletpassphrase",
                active.sendtoaddress,
                destination,
                POST_UPGRADE_SEND_DGB,
            )

        active.walletpassphrase(WALLET_PASSPHRASE, 600)
        current_address = active.getnewaddress(PERSISTENT_LABEL)
        self.tracked_addresses.append(current_address)
        assert_equal(active.getaddressinfo(current_address)["ismine"], True)

        if self.options.descriptors:
            current.setmockoracleprice(ORACLE_PRICE_MICRO_USD)
            send = active.senddigidollar(destination, POST_UPGRADE_SEND_CENTS)
            self.tracked_txids.append(send["txid"])
            self.tracked_dd_txids.append(send["txid"])
            self._mine_one(current)
            assert self.expected_active_dd_balance is not None
            assert_equal(
                active.getdigidollarbalance()["total"],
                self.expected_active_dd_balance - POST_UPGRADE_SEND_CENTS,
            )
            assert_equal(
                sink.getdigidollarbalance()["total"],
                POST_UPGRADE_SEND_CENTS,
            )
        else:
            spend_txid = active.sendtoaddress(destination, POST_UPGRADE_SEND_DGB)
            self.tracked_txids.append(spend_txid)
            self._mine_one(current)
            assert_equal(sink.getbalance(), POST_UPGRADE_SEND_DGB)

        active.walletlock()
        assert_equal(active.getwalletinfo()["unlocked_until"], 0)
        current_state = self._state(active)

        self.log.info("Restart once more and verify the post-upgrade state")
        self.restart_node(1)
        current = self.nodes[1]
        current.syncwithvalidationinterfacequeue()
        active = current.get_wallet_rpc(ACTIVE_WALLET)
        assert_equal(self._state(active), current_state)
        assert_equal(active.getwalletinfo()["unlocked_until"], 0)
        peer = current.get_wallet_rpc(PEER_WALLET)
        assert_equal(self._peer_state(peer), peer_state)

        self.log.info("Round-trip the current wallet directory back to v9.26.5")
        current.unloadwallet(ACTIVE_WALLET, False)
        self._copy_wallet(current, ACTIVE_WALLET, old, ROUNDTRIP_WALLET)
        self.start_node(0)
        old = self.nodes[0]
        self.connect_nodes(0, 1)
        self.sync_blocks([old, current])
        old.syncwithvalidationinterfacequeue()
        old.loadwallet(ROUNDTRIP_WALLET, False)
        roundtrip = old.get_wallet_rpc(ROUNDTRIP_WALLET)
        assert_equal(self._state(roundtrip), current_state)
        assert_equal(roundtrip.getwalletinfo()["unlocked_until"], 0)

        self.log.info(
            "Official v9.26.5 and the current daemon preserved the %s wallet",
            wallet_type,
        )


if __name__ == "__main__":
    V9_26_5WalletCompatibilityTest().main()
