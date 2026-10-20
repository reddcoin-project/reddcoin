#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a side-chain block's stake kernel is hashed with its own chain's modifier.

A kernel is hashed with the stake modifier generated about a selection interval
after the block holding the staked coin. For a block on a side chain, the blocks
after the coin's block are the side chain's, not the active chain's.

Node 0 and node 2 each extend the same tip on their own. Node 1 follows node 2,
then is given node 0's longer chain while it is still a side chain, and
switches to it. Node 3, which has seen neither branch, then syncs node 0's chain
as its active chain. Every block of that chain must have the same proof-of-stake
hash on all of them.

On regtest the proof-of-stake target lets any hash pass, so a wrong modifier
does not get a block rejected here; on mainnet it would. The proof hash each
node records for a block shows which modifier it used.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class PosSideChainModifierTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 4
        self.extra_args = [["-staking=0"]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        # Node 0 and node 2 stake from the cache's coins
        for i in range(self.num_nodes):
            self.init_wallet(i)

    def setup_network(self):
        # Node 0 builds its chain alone; node 3 joins only at the end.
        self.setup_nodes()
        self.connect_nodes(1, 2)

    def run_test(self):
        fork_point = self.nodes[1].getbestblockhash()
        fork_height = self.nodes[1].getblockcount()

        self.log.info("Node 2 extends the tip, node 1 follows")
        self.nodes[2].generate(62)
        self.sync_all(self.nodes[1:3])

        self.log.info("Node 0 extends the same tip on its own, one block longer")
        # Node 1 cannot yet check a kernel on a coin created on a side chain
        # (RED-174), so node 0 must not stake one: lock each new coinstake's
        # outputs, which the staker leaves alone.
        node0 = self.nodes[0]
        fork = []
        for _ in range(63):
            fork += node0.generate(1)
            stake_txid = node0.getblock(fork[-1])["tx"][1]
            coinstake = node0.decoderawtransaction(node0.gettransaction(stake_txid)["hex"])
            node0.lockunspent(False, [{"txid": stake_txid, "vout": out["n"]}
                                      for out in coinstake["vout"] if out["value"] > 0])
        assert_equal(node0.getblock(fork[0])["previousblockhash"], fork_point)

        self.log.info("Node 1 is given node 0's chain while it is a side chain, and switches to it")
        node1 = self.nodes[1]
        node1.setmocktime(node0.mocktime)
        for block_hash in fork:
            node1.submitblock(node0.getblock(block_hash, 0))
        assert_equal(node1.getbestblockhash(), fork[-1])

        self.log.info("Node 3 syncs node 0's chain as its active chain")
        node3 = self.nodes[3]
        assert_equal(node3.getblockcount(), fork_height)
        node3.setmocktime(node0.mocktime)
        self.connect_nodes(3, 0)
        self.sync_blocks([node0, node3])

        self.log.info("Every node hashed every kernel with the same modifier")
        mismatched = []
        for block_hash in fork:
            expected = node3.getblock(block_hash)["hashproof"]
            for node in (node0, node1):
                if node.getblock(block_hash)["hashproof"] != expected:
                    mismatched.append((node.index, node.getblock(block_hash)["height"]))
        assert_equal(mismatched, [])


if __name__ == '__main__':
    PosSideChainModifierTest().main()
