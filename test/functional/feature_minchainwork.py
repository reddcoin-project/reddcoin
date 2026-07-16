#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Copyright (c) 2017-2022 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test logic for setting nMinimumChainWork on command line.

Nodes don't consider themselves out of "initial block download" until
their active chain has more work than nMinimumChainWork.

Nodes don't download blocks from a peer unless the peer's best known block
has more work than nMinimumChainWork.

While in initial block download, nodes won't relay blocks to their peers, so
test that this parameter functions as intended by verifying that block relay
only succeeds past a given node once its nMinimumChainWork has been exceeded.
"""

import time

from test_framework.p2p import P2PInterface, msg_getheaders
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# 2 hashes required per regtest block (with no difficulty adjustment)
REGTEST_WORK_PER_BLOCK = 2

class MinimumChainWorkTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3

        self.extra_args = [["-whitelist=127.0.0.1"], ["-minimumchainwork=0x65", "-whitelist=127.0.0.1"], ["-minimumchainwork=0x65", "-whitelist=127.0.0.1"]]
        self.node_min_work = [0, 101, 101]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # This test relies on the chain setup being:
        # node0 <- node1 <- node2
        # Before leaving IBD, nodes prefer to download blocks from outbound
        # peers, so ensure that we're mining on an outbound peer and testing
        # block relay to inbound peers.
        self.setup_nodes()

        # Set mocktime BEFORE connecting to avoid mocktime-jump disconnections.
        # Nodes 0-1 get genesis+48h for block generation. Node2 gets genesis+72h
        # so the tip always appears old from node2's perspective (>8h = DEFAULT_MAX_TIP_AGE),
        # keeping node2 in IBD throughout the test.
        genesis_time = self.nodes[0].getblockheader(self.nodes[0].getblockhash(0))['time']
        self.mocktime = genesis_time + 48 * 60 * 60
        self.nodes[0].setmocktime(self.mocktime)
        self.nodes[1].setmocktime(self.mocktime)
        self.nodes[2].setmocktime(genesis_time + 72 * 60 * 60)

        for i in range(self.num_nodes-1):
            self.connect_nodes(i+1, i)

    def run_test(self):
        # Start building a chain on node0.  node2 shouldn't be able to sync until node1's
        # minchainwork is exceeded
        starting_chain_work = REGTEST_WORK_PER_BLOCK # Genesis block's work
        self.log.info("Testing relay across node %d (minChainWork = %d)", 1, self.node_min_work[1])

        starting_blockcount = self.nodes[2].getblockcount()

        num_blocks_to_generate = int((self.node_min_work[1] - starting_chain_work) / REGTEST_WORK_PER_BLOCK)
        self.log.info("Generating %d blocks on node0", num_blocks_to_generate)
        hashes = self.nodes[0].generate(num_blocks_to_generate)

        self.log.info("Node0 current chain work: %s", self.nodes[0].getblockheader(hashes[-1])['chainwork'])

        # Sleep a few seconds and verify that node2 didn't get any new blocks
        # or headers.  We sleep, rather than sync_blocks(node0, node1) because
        # it's reasonable either way for node1 to get the blocks, or not get
        # them (since they're below node1's minchainwork).
        time.sleep(3)

        self.log.info("Verifying node 2 has no more blocks than before")
        self.log.info("Blockcounts: %s", [n.getblockcount() for n in self.nodes])
        # Node2 shouldn't have any new headers yet, because node1 should not
        # have relayed anything.
        assert_equal(len(self.nodes[2].getchaintips()), 1)
        assert_equal(self.nodes[2].getchaintips()[0]['height'], 0)

        # Note: node1 may or may not have synced with node0 at this point.
        # In ReddCoin, CanDirectFetch() returns true (genesis is recent relative
        # to mocktime), so HeadersDirectFetchBlocks bypasses nMinimumChainWork.
        # The important check is that node2 hasn't received any blocks.
        assert_equal(self.nodes[2].getblockcount(), starting_blockcount)

        self.log.info("Check that getheaders requests to node2 are ignored")
        peer = self.nodes[2].add_p2p_connection(P2PInterface())
        msg = msg_getheaders()
        msg.locator.vHave = [int(self.nodes[2].getbestblockhash(), 16)]
        msg.hashstop = 0
        peer.send_and_ping(msg)
        time.sleep(5)
        assert ("headers" not in peer.last_message or len(peer.last_message["headers"].headers) == 0)

        self.log.info("Generating one more block")
        self.nodes[0].generate(1)

        self.log.info("Verifying nodes are all synced")

        # Sync mocktime from node0 to nodes 1/2 so P2P relay works
        if self.nodes[0].mocktime:
            # Keep node2's time offset (it's deliberately ahead for IBD testing)
            node2_offset = 24 * 60 * 60  # 24h ahead of nodes 0/1
            self.nodes[1].setmocktime(self.nodes[0].mocktime)
            self.nodes[1].mocktime = self.nodes[0].mocktime
            self.nodes[2].setmocktime(self.nodes[0].mocktime + node2_offset)
            self.nodes[2].mocktime = self.nodes[0].mocktime + node2_offset

        self.sync_blocks(timeout=120)
        self.log.info("Blockcounts: %s", [n.getblockcount() for n in self.nodes])

        self.log.info("Test that getheaders requests to node2 are not ignored")
        peer.send_and_ping(msg)
        assert "headers" in peer.last_message

        # Verify that node2 is in fact still in IBD (otherwise this test may
        # not be exercising the logic we want!)
        assert_equal(self.nodes[2].getblockchaininfo()['initialblockdownload'], True)

if __name__ == '__main__':
    MinimumChainWorkTest().main()
