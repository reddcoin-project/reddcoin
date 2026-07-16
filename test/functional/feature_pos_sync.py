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

    def setup_network(self):
        """Override to prevent auto-connection during initial generation.
        Nodes must be disconnected to avoid mocktime drift issues."""
        self.setup_nodes()
        # Don't connect — will connect after initial generation

    def run_test(self):
        self.log.info("Starting PoS sync test")

        # Generate 70 PoW blocks — first mature coins at height 61
        self.log.info("Node0: Generating 70 PoW blocks...")
        self.nodes[0].generate(70)
        self.log.info(f"Node0 balance at height 70: {self.nodes[0].getbalance()}")

        # Send UTXOs to node1 during PoW phase so they accumulate maximum
        # coin age and diverse stake modifiers by the time node1 generates.
        self.log.info("Node0: Sending coins to node1 during PoW phase...")
        node1_staking_address = self.nodes[1].getnewaddress()
        for i in range(20):
            self.nodes[0].sendtoaddress(node1_staking_address, 500)

        # Generate remaining PoW blocks (confirms the sends at height 71,
        # giving them 18 blocks of PoW confirmations before PoS transition)
        self.log.info("Node0: Generating remaining PoW blocks...")
        self.nodes[0].generate(19)  # up to height 89
        assert_equal(self.nodes[0].getblockcount(), 89)

        # Advance time to age coins for PoS
        self.log.info("Node0: Advancing time for coin age...")
        advance_time_for_pos(self.nodes[0], seconds=600)

        # Generate PoS blocks — coins sent at height 50 now have 40+ blocks of age
        self.log.info("Node0: Generating 11 PoS blocks...")
        self.nodes[0].generate(11)

        node0_height = self.nodes[0].getblockcount()
        node0_hash = self.nodes[0].getbestblockhash()
        self.log.info(f"Node0: Generated {node0_height} blocks, best hash: {node0_hash}")
        assert_equal(node0_height, 100)

        # Sync mocktime before connecting to prevent header rejection
        from test_framework.util import set_node_times
        set_node_times(self.nodes, self.nodes[0].mocktime)

        # Connect node1 and sync
        self.log.info("Connecting node1 to node0...")
        self.connect_nodes(0, 1)
        self.sync_blocks()

        node1_height = self.nodes[1].getblockcount()
        node1_hash = self.nodes[1].getbestblockhash()
        self.log.info(f"Node1: Synced to height {node1_height}, best hash: {node1_hash}")

        # Verify node1 synced all blocks
        assert_equal(node1_height, 100)
        assert_equal(node1_hash, node0_hash)
        self.log.info(f"Node1 balance: {self.nodes[1].getbalance()}")

        # Disconnect before node1 generates to prevent mocktime drift
        self.log.info("Disconnecting nodes for node1 generation...")
        self.disconnect_nodes(0, 1)
        self.nodes[1].time_sync_callback = None

        # Node1 generates 10 PoS blocks using the early-sent UTXOs
        # which now have ~50 blocks of age and diverse stake modifiers
        self.log.info("Node1: Advancing time for coin age...")
        advance_time_for_pos(self.nodes[1], seconds=600)
        self.log.info("Node1: Generating 10 additional PoS blocks...")
        self.nodes[1].generate(10)

        node1_height_after = self.nodes[1].getblockcount()
        expected_height = 110
        node1_hash_after = self.nodes[1].getbestblockhash()
        self.log.info(f"Node1: Generated 10 blocks, now at height {node1_height_after}")
        assert_equal(node1_height_after, expected_height)

        # Reconnect: sync mocktime first, then connect and sync blocks
        self.log.info("Reconnecting nodes and syncing...")
        set_node_times(self.nodes, self.nodes[1].mocktime)
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=120)

        node0_height_after = self.nodes[0].getblockcount()
        node0_hash_after = self.nodes[0].getbestblockhash()
        self.log.info(f"Node0: Synced to height {node0_height_after}, best hash: {node0_hash_after}")

        # Verify both nodes are in sync
        assert_equal(node0_height_after, expected_height)
        assert_equal(node0_hash_after, node1_hash_after)

        self.log.info("PoS sync test completed successfully!")


if __name__ == '__main__':
    PosSyncTest().main()
