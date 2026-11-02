#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test running reddcoind with -reindex and -reindex-chainstate options.

- Start a single node and generate 150 blocks (90 PoW + 60 PoS).
- Stop the node and restart it with -reindex. Verify that the node has reindexed up to block 150.
- Stop the node and restart it with -reindex-chainstate. Verify that the node has reindexed up to block 150.
- After each restart, every block has the stake modifier it had before. A
  reindex accepts the genesis block like any other block, and once counted it
  as having generated a modifier, which a node that started from scratch does
  not: every later generated modifier then moved one block on, and a kernel
  whose modifier was in its parent block failed with "unable to determine
  stakemodifier".
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, advance_time_for_pos


class ReindexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def reindex(self, justchainstate=False):
        self.nodes[0].generate(3)
        # The tip, the UTXO set and the stake modifier of every block, so that
        # reindex is shown to rebuild the same chain, not one of the same height.
        blockcount = self.nodes[0].getblockcount()
        besthash = self.nodes[0].getbestblockhash()
        utxo_hash = self.nodes[0].gettxoutsetinfo()['hash_serialized_2']
        modifiers = self.modifiers()
        self.stop_nodes()
        extra_args = [["-reindex-chainstate" if justchainstate else "-reindex"]]
        self.start_nodes(extra_args)
        assert_equal(self.nodes[0].getblockcount(), blockcount)
        assert_equal(self.nodes[0].getbestblockhash(), besthash)
        assert_equal(self.nodes[0].gettxoutsetinfo()['hash_serialized_2'], utxo_hash)
        assert_equal(self.modifiers(), modifiers)
        self.log.info("Success (height=%d, tip, UTXO set and stake modifiers match)", blockcount)

    def modifiers(self):
        node = self.nodes[0]
        return [node.getblock(node.getblockhash(height))["modifier"] for height in range(node.getblockcount() + 1)]

    def run_test(self):
        # Generate initial 150 blocks spanning PoW and PoS phases
        self.log.info("Generating 150 blocks (90 PoW + 60 PoS)...")
        self.nodes[0].generate(90)
        advance_time_for_pos(self.nodes[0], seconds=600)
        self.nodes[0].generate(60)

        self.reindex(False)
        self.reindex(True)
        self.reindex(False)
        self.reindex(True)

if __name__ == '__main__':
    ReindexTest().main()
