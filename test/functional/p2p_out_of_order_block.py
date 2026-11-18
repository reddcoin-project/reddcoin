#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a requested block that arrives before its parent.

The node keeps such a block aside and connects it once the parent has
arrived. After that, nothing may still be marked as in flight from the peer
that sent it. Otherwise the block download timeout fires later and the node
disconnects the peer, though it delivered every block it was asked for.

node0 stakes two blocks while it is not connected to node1. A P2P connection
then announces both headers to node1, waits until node1 requests both blocks,
and sends the child before the parent.
"""

from test_framework.messages import (
    CBlock,
    CBlockHeader,
    from_hex,
    msg_block,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Regtest nPowTargetSpacing. With a single downloading peer, the block
# download timeout is exactly this long.
BLOCK_DOWNLOAD_TIMEOUT = 600


class RequestRecorder(P2PInterface):
    def __init__(self):
        super().__init__()
        self.requested = set()

    def on_getdata(self, message):
        for inv in message.inv:
            self.requested.add(inv.hash)


class OutOfOrderBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # Keep the nodes apart: node1 must get the blocks only from the P2P
        # connection below, in the order the test chooses.
        self.setup_nodes()

    def run_test(self):
        node0, node1 = self.nodes
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

        self.log.info("Stake two blocks on node0")
        hashes = self.generate(2, node=node0)
        parent, child = [from_hex(CBlock(), node0.getblock(h, 0)) for h in hashes]
        parent.rehash()
        child.rehash()
        assert_equal(child.hashPrevBlock, parent.sha256)

        peer = node1.add_p2p_connection(RequestRecorder())

        self.log.info("Announce both headers and wait for node1 to request both blocks")
        peer.send_message(msg_headers([CBlockHeader(parent), CBlockHeader(child)]))
        peer.wait_until(lambda: {parent.sha256, child.sha256} <= peer.requested)

        self.log.info("Send the child first, then the parent")
        with node1.assert_debug_log(expected_msgs=[f"Received block out of order: {child.hash}"]):
            peer.send_and_ping(msg_block(child))
        with node1.assert_debug_log(expected_msgs=[f"Processing out of order child {child.hash}"]):
            peer.send_and_ping(msg_block(parent))
        assert_equal(node1.getbestblockhash(), child.hash)

        self.log.info("Check that nothing is left in flight from the peer")
        peer_info = node1.getpeerinfo()
        assert_equal(len(peer_info), 1)
        assert_equal(peer_info[0]['inflight'], [])

        self.log.info("Check that the peer survives a clock step past the download timeout")
        node1.mocktime += BLOCK_DOWNLOAD_TIMEOUT + 60
        node1.setmocktime(node1.mocktime)
        # The timeout is checked in SendMessages, which runs after each
        # message is processed. The second ping runs after the SendMessages
        # pass that follows the first, so a disconnect cannot slip past.
        peer.sync_with_ping()
        peer.sync_with_ping()
        assert peer.is_connected
        assert_equal(len(node1.getpeerinfo()), 1)


if __name__ == '__main__':
    OutOfOrderBlockTest().main()
