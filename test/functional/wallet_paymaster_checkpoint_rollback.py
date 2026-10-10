#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reproduce the limitation of restoring a matching wallet/checkpoint pair.

This is a diagnostic regression, not a claim of rollback protection: its success
means that a pair rollback renews already spent authority. Only disposable
regtest wallet files are replaced, with both processes stopped. A separately
retained high-water authority is necessary to prevent this scenario.
"""

from shutil import copyfile

from test_framework.paymaster import (
    PaymasterFunctionalHarness,
    confirm_pool_preparation,
    default_liquidity_policy,
    provider_safety_policy,
)
from test_framework.util import assert_equal
from wallet_paymaster_backup import PaymasterProviderBackupTest


class PaymasterCheckpointRollbackTest(PaymasterProviderBackupTest):
    def set_test_params(self):
        super().set_test_params()

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def run_test(self):
        harness = PaymasterFunctionalHarness(self)
        provider, _ = harness.create_wallets()
        harness.fund_and_configure(operation_mode="manual", carrier_cents=2_000)
        targets = {"admission_dgb_slots": 3, "operational_dgb_slots": 3,
                   "admission_carrier_slots": 3, "operational_carrier_slots": 3}
        preview = provider.preparepaymasterpool(targets)
        execute = dict(targets, execute=True, plan_id=preview["plan_id"])
        prepared = provider.preparepaymasterpool(execute)
        confirm_pool_preparation(self, self.nodes[0], provider, execute, prepared)
        liquidity = default_liquidity_policy()
        liquidity.update(target_operational_dgb=3, target_operational_carriers=3,
                         automatic_replenishment=False)
        provider.setpaymasterliquiditypolicy(liquidity)
        harness.fund_client_dd(500)
        policy = dict(harness.policy, maximum_network_fee_dgb_satoshis=10_000_000)
        safety = provider_safety_policy(["user_paid"])
        safety["user_paid"].update(
            maximum_network_fee_per_transaction_satoshis=10_000_000,
            maximum_network_fee_per_hour_satoshis=20_000_000,
            maximum_network_fee_per_day_satoshis=20_000_000,
            maximum_completed_per_hour=2, maximum_completed_per_day=2)
        provider.setpaymastersafetypolicy(safety)
        provider.setpaymasterpolicy(policy)
        recipient = provider.getdigidollaraddress()
        identity = harness.identity["provider_id"]
        root = self.nodes[0].datadir_path.resolve()
        wallet_image = root / "pair-wallet.bak"
        checkpoint_image = root / "pair-checkpoint.bak"
        provider.backupwallet(wallet_image)
        checkpoints = list((root / "regtest" / "paymaster-checkpoints").glob("*.checkpoint"))
        assert_equal(len(checkpoints), 1)
        checkpoint = checkpoints[0]
        copyfile(checkpoint, checkpoint_image)
        assert_equal(provider.startpaymaster()["ready"], True)

        payments = []
        for request_id in ("aa457dc0-d4e6-4c1b-8b55-808961d44401",
                           "aa457dc0-d4e6-4c1b-8b55-808961d44402"):
            payments.append(self.commit_payment(harness, request_id, recipient))
            self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
            self.wait_chain_ready()
        assert_equal(provider.getpaymastersafetystatus()["user_paid"][
            "spent_network_fee_last_day_satoshis"], 20_000_000)

        self.log.info("Restore both files without the explicit restore API; retain current chainstate")
        self.stop_nodes()
        wallet_file = root / "regtest" / "wallets" / "provider" / "wallet.dat"
        # Never replace an operator wallet or compute a target outside the
        # dedicated test datadir. No directory deletion is needed.
        assert root in wallet_file.resolve().parents
        assert root in checkpoint.resolve().parents
        assert wallet_file.is_file()
        copyfile(wallet_image, wallet_file)
        copyfile(checkpoint_image, checkpoint)
        self.start_nodes()
        self.connect_nodes(0, 1)
        self.wait_chain_ready()
        harness.provider = self.nodes[0].get_wallet_rpc("provider")
        harness.provider_cli = self.nodes[0].cli("-rpcwallet=provider")
        harness.client = self.nodes[1].get_wallet_rpc("client")
        provider = harness.provider
        assert_equal(provider.createpaymasteridentity(harness.identity["display_name"])["provider_id"], identity)
        assert_equal(provider.getpaymastersafetystatus()["user_paid"][
            "spent_network_fee_last_day_satoshis"], 0)
        assert_equal(provider.startpaymaster()["ready"], True)
        payments.append(self.commit_payment(harness, "aa457dc0-d4e6-4c1b-8b55-808961d44403", recipient))
        self.generatetoaddress(self.nodes[0], 1, provider.getnewaddress())
        self.wait_chain_ready()
        assert all(self.nodes[0].getrawtransaction(payment["txid"], True)["confirmations"] > 0
                   for payment in payments)
        actual_fees = sum(payment["fee_satoshis"] for payment in payments)
        assert_equal(actual_fees, 30_000_000)
        assert_equal(provider.getpaymastersafetystatus()["user_paid"][
            "spent_network_fee_last_day_satoshis"], 10_000_000)
        self.log.info("Limitation reproduced: actual fees 0.3 DGB, approval 0.2 DGB, restored ledger 0.1 DGB")


if __name__ == "__main__":
    PaymasterCheckpointRollbackTest().main()
