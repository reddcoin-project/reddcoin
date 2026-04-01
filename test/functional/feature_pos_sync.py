#!/usr/bin/env python3
# Copyright (c) 2014-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test PoS block synchronization between nodes.

This test validates that:
- Node0 can generate 110 blocks (PoW and PoS) from genesis
- Node1 can connect and sync all blocks from node0
- Node1 can generate 10 additional PoS blocks
- Node0 can sync the new blocks from node1
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)


class PosSyncTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True  # Start from genesis
        self.extra_args = [["-staking=0"], ["-staking=0"]]  # Prevent background staking

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Starting PoS sync test")

        # Node0 generates 110 blocks (89 PoW + 11 PoS)
        self.log.info("Node0: Generating 110 blocks from genesis...")

        # Generate PoW blocks (0-89)
        self.log.info("Node0: Generating 89 PoW blocks...")
        self.nodes[0].generate(89)
        assert_equal(self.nodes[0].getblockcount(), 89)

        # Advance time to age coins for PoS
        self.log.info("Node0: Advancing time for coin age...")
        advance_time_for_pos(self.nodes[0], seconds=600)

        # Generate PoS blocks (90-100) — generate() handles PoS retry automatically
        self.log.info("Node0: Generating 11 PoS blocks...")
        self.nodes[0].generate(11)

        node0_height = self.nodes[0].getblockcount()
        node0_hash = self.nodes[0].getbestblockhash()
        self.log.info(f"Node0: Generated {node0_height} blocks, best hash: {node0_hash}")
        assert_equal(node0_height, 100)

        # Connect node1 and sync
        self.log.info("Connecting node1 to node0...")
        self.connect_nodes(0, 1)

        # Sync time before syncing blocks
        # self.sync_all()

        self.log.info("Node1: Syncing blocks from node0...")
        node1_height = self.nodes[1].getblockcount()
        node1_hash = self.nodes[1].getbestblockhash()
        self.log.info(f"Node1: Synced to height {node1_height}, best hash: {node1_hash}")

        # Verify node1 synced all blocks
        assert_equal(node1_height, 100)
        assert_equal(node1_hash, node0_hash)

        # Give node1 coins so it can stake (15 UTXOs for 10 PoS blocks + margin)
        self.log.info("Node0: Sending coins to node1...")
        node1_staking_address = self.nodes[1].getnewaddress()
        for i in range(15):
            self.nodes[0].sendtoaddress(node1_staking_address, 500)

        # Confirm and mature the coins (COINBASE_MATURITY = 60)
        self.log.info("Node0: Confirming and maturing node1 coins...")
        self.nodes[0].generate(61)  # 1 confirmation + 60 maturity
        self.sync_all()
        self.log.info(f"Node1 balance: {self.nodes[1].getbalance()}")

        # Node1 generates 10 more PoS blocks
        self.log.info("Node1: Generating 10 additional PoS blocks...")
        self.nodes[1].generate(10)

        node1_height_after = self.nodes[1].getblockcount()
        expected_height = 100 + 61 + 10  # initial + maturity + new
        node1_hash_after = self.nodes[1].getbestblockhash()
        self.log.info(f"Node1: Generated 10 blocks, now at height {node1_height_after}, best hash: {node1_hash_after}")
        assert_equal(node1_height_after, expected_height)

        # Sync node0 with node1's new blocks
        self.log.info("Node0: Syncing new blocks from node1...")
        self.sync_all()

        node0_height_after = self.nodes[0].getblockcount()
        node0_hash_after = self.nodes[0].getbestblockhash()
        self.log.info(f"Node0: Synced to height {node0_height_after}, best hash: {node0_hash_after}")

        # Verify both nodes are in sync
        assert_equal(node0_height_after, expected_height)
        assert_equal(node0_hash_after, node1_hash_after)

        self.log.info("PoS sync test completed successfully!")


if __name__ == '__main__':
    PosSyncTest().main()
