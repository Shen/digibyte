#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Adopt stronger forks using only peer messages.

This is a fork-adoption test, not proof of PRESYNC/REDOWNLOAD coverage:
DigiByte permits 20000 headers per message, above either fork used here.
"""

from test_framework.test_framework import DigiByteTestFramework
from test_framework.util import assert_equal, assert_greater_than


class HeadersChainworkTest(DigiByteTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-digidollaractivationheight=2147483646"]] * 2
        self.wallet_names = []

    def mine_batches(self, node, count, *, sync=True):
        # Keep each mining RPC below the normal RPC timeout on modest hosts.
        # In particular, do not reduce the 2600-block fork under test.
        blocks = []
        while len(blocks) < count:
            blocks.extend(self.generate(node, min(100, count - len(blocks)), sync_fun=self.no_op))
        assert_equal(len(blocks), count)
        if sync:
            self.sync_blocks(timeout=120)
        return blocks

    def replace_chain(self, fork_height, count):
        miner, peer = self.nodes
        old_work = int(peer.getblockchaininfo()["chainwork"], 16)
        fork_time = miner.getblock(miner.getbestblockhash())["time"] + 1000
        self.disconnect_nodes(0, 1)
        miner.invalidateblock(miner.getblockhash(fork_height + 1))
        for node in self.nodes:
            node.setmocktime(fork_time)
        fork = self.mine_batches(miner, count, sync=False)
        assert_greater_than(int(miner.getblockchaininfo()["chainwork"], 16), old_work)
        self.connect_nodes(0, 1)
        # On the recorded regression run the peer was still actively applying
        # the new fork at height 3312/3350 when the 120-second limit expired.
        # Allow bounded time for header checks, disconnecting the old chain,
        # downloading blocks and connecting all 2600 replacements. Keep the
        # exact-tip/height/work assertions: timeout is never treated as success.
        self.sync_blocks(timeout=600)
        assert_equal(peer.getbestblockhash(), fork[-1])
        assert_equal(peer.getblockcount(), fork_height + count)
        assert_greater_than(int(peer.getblockchaininfo()["chainwork"], 16), old_work)

    def run_test(self):
        self.mine_batches(self.nodes[0], 1000)
        self.log.info("Accept a stronger fork in one headers message")
        self.replace_chain(750, 301)

        self.mine_batches(self.nodes[0], 2249)
        self.log.info("Accept a stronger 2600-block fork through peer relay")
        self.replace_chain(750, 2600)


if __name__ == "__main__":
    HeadersChainworkTest().main()
