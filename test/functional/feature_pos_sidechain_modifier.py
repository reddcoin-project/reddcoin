#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a side-chain block's stake kernel is checked against its own chain.

A kernel is hashed with the stake modifier generated about a selection interval
after the block holding the staked coin, and that coin is looked up on the chain
the kernel's block builds on. For a block on a side chain, both are the side
chain's, not the active chain's: the blocks after the coin's block are the side
chain's, the coin may have been created in one of them, and a coin created
before the fork point may since have been spent on the active chain.

Node 0 and node 2 each extend the same tip on their own, staking from the same
coins. Node 1, and node 4 without a transaction index, follow node 2, then are
given node 0's longer chain while it is still a side chain, and switch to it.
Node 3, which has seen neither branch, then syncs node 0's chain as its active
chain. Every block of that chain must have the same proof-of-stake hash on all
of them.

On regtest the proof-of-stake target lets any hash pass, so a wrong modifier
does not get a block rejected here; on mainnet it would. The proof hash each
node records for a block shows which modifier it used.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than


class PosSideChainModifierTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 5
        self.extra_args = [["-staking=0"]] * 4 + [["-staking=0", "-txindex=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        for i in range(self.num_nodes):
            self.init_wallet(i)
        # Node 2 also stakes node 0's coins, so the two branches spend some of
        # the same coins created before the fork point.
        self.nodes[2].importprivkey(self.nodes[0].get_deterministic_priv_key().key)

    def setup_network(self):
        # Node 0 builds its chain alone; node 3 joins only at the end.
        self.setup_nodes()
        self.connect_nodes(1, 2)
        self.connect_nodes(4, 2)

    def run_test(self):
        node0, node1, node2, node3, node4 = self.nodes
        fork_point = node1.getbestblockhash()
        fork_height = node1.getblockcount()

        self.log.info("Node 2 extends the tip, nodes 1 and 4 follow")
        node2.generate(62)
        self.sync_all([node1, node2, node4])

        self.log.info("Node 0 extends the same tip on its own, one block longer")
        fork = []
        for _ in range(63):
            if len(fork) == 61:
                # The fork's first coins are now mature: lock every older coin
                # so that the last blocks stake coins created on the fork.
                tip_height = node0.getblockcount()
                node0.lockunspent(False, [{"txid": u["txid"], "vout": u["vout"]} for u in node0.listunspent()
                                          if tip_height - u["confirmations"] + 1 <= fork_height])
            fork += node0.generate(1)
        assert_equal(node0.getblock(fork[0])["previousblockhash"], fork_point)

        # The fork must stake both kinds of coin the active chain cannot show a
        # side-chain validator: one created on the fork, and one created before
        # the fork point and spent on node 2's branch.
        created_on_fork = spent_on_other_branch = 0
        for block_hash in fork:
            kernel = node0.getrawtransaction(node0.getblock(block_hash)["tx"][1], True)["vin"][0]
            source = node0.getrawtransaction(kernel["txid"], True)
            if node0.getblock(source["blockhash"])["height"] > fork_height:
                created_on_fork += 1
            elif node2.gettxout(kernel["txid"], kernel["vout"]) is None:
                spent_on_other_branch += 1
        self.log.info("Fork kernels: %d created on the fork, %d spent on node 2's branch",
                      created_on_fork, spent_on_other_branch)
        assert_greater_than(created_on_fork, 0)
        assert_greater_than(spent_on_other_branch, 0)

        self.log.info("Nodes 1 and 4 are given node 0's chain while it is a side chain, and switch to it")
        for node in (node1, node4):
            node.setmocktime(node0.mocktime)
            for block_hash in fork:
                node.submitblock(node0.getblock(block_hash, 0))
            assert_equal(node.getbestblockhash(), fork[-1])

        self.log.info("Node 3 syncs node 0's chain as its active chain")
        assert_equal(node3.getblockcount(), fork_height)
        node3.setmocktime(node0.mocktime)
        self.connect_nodes(3, 0)
        self.sync_blocks([node0, node3])

        self.log.info("Every node hashed every kernel with the same modifier")
        mismatched = []
        for block_hash in fork:
            expected = node3.getblock(block_hash)["hashproof"]
            for node in (node0, node1, node4):
                if node.getblock(block_hash)["hashproof"] != expected:
                    mismatched.append((node.index, node.getblock(block_hash)["height"]))
        assert_equal(mismatched, [])


if __name__ == '__main__':
    PosSideChainModifierTest().main()
