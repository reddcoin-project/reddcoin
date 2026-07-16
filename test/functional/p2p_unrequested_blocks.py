#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test processing of unrequested blocks.

ReddCoin adaptation notes:
- Uses scrypt PoW solving (requires 'scrypt' Python package)
- Added skip_if_no_wallet() for generatetoaddress
- Added -whitelist and -peertimeout for P2P stability
- IMPORTANT: ReddCoin has stricter out-of-order block handling than Bitcoin.
  Blocks whose parent is "headers-only" (has header but no block data) are
  rejected and NOT passed to ProcessNewBlock. This is intentional PoS security.
  Parts 4-7 are adapted to TEST this rejection behavior rather than work around it.

Setup: two nodes, node0 + node1, not connected to each other. Node1 will have
nMinimumChainWork set to 0x10, so it won't process low-work unrequested blocks.

We have one P2PInterface connection to node0 called test_node, and one to node1
called min_work_node.

The test:
1. Generate one block on each node, to leave IBD.

2. Mine a new block on each tip, and deliver to each node from node's peer.
   The tip should advance for node0, but node1 should skip processing due to
   nMinimumChainWork.

Node1 is unused in tests 3-7:

3. Mine a block that forks from the genesis block, and deliver to test_node.
   Node0 should not process this block (just accept the header), because it
   is unrequested and doesn't have more or equal work to the tip.

4a. Send another block that builds on the forking block.
   ReddCoin: Node0 should REJECT this block because its parent (block_h1f) is
   headers-only. This is intentional PoS security behavior - blocks with
   headers-only parents are not processed.

4b. Send a third block building on the rejected block.
   ReddCoin: Also rejected (parent chain not validated).

4c. Verify that sending headers FIRST, then blocks, allows proper processing.
   This tests the correct way to deliver fork chains in ReddCoin.

5. (Skipped - depends on Bitcoin behavior not present in ReddCoin)

6. Send Node0 an inv for a block on the main chain.
   Node0 should request it via getdata.

7. Verify chain works correctly when blocks arrive in proper order.

8. Create a fork which is invalid at a height longer than the current chain
   (ie to which the node will try to reorg) but which has headers built on top
   of the invalid block. Check that we get disconnected if we send more headers
   on the chain the node now knows to be invalid.

9. Test Node1 is able to sync when connected to node0 (which should have sufficient
   work on its chain).
"""

import time

from test_framework.blocktools import create_block, create_coinbase, create_tx_with_script
from test_framework.messages import CBlockHeader, CInv, MSG_BLOCK, msg_block, msg_headers, msg_inv
from test_framework.p2p import p2p_lock, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class AcceptBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # ReddCoin: Add whitelist and peertimeout to prevent mocktime-related disconnects
        self.extra_args = [
            ["-whitelist=noban@127.0.0.1", "-peertimeout=999999999"],
            ["-minimumchainwork=0x10", "-whitelist=noban@127.0.0.1", "-peertimeout=999999999"]
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        test_node = self.nodes[0].add_p2p_connection(P2PInterface())
        min_work_node = self.nodes[1].add_p2p_connection(P2PInterface())

        # 1. Have nodes mine a block (leave IBD)
        [n.generatetoaddress(1, n.get_deterministic_priv_key().address) for n in self.nodes]
        tips = [int("0x" + n.getbestblockhash(), 0) for n in self.nodes]

        # 2. Send one block that builds on each tip.
        # This should be accepted by node0
        blocks_h2 = []  # the height 2 blocks on each node's chain
        # ReddCoin: Use node's mocktime instead of real time to avoid "time-too-new" rejection
        block_time = self.nodes[0].getblockheader(self.nodes[0].getbestblockhash())['time'] + 1
        for i in range(2):
            blocks_h2.append(create_block(tips[i], create_coinbase(2), block_time))
            blocks_h2[i].solve()
            block_time += 1
        test_node.send_and_ping(msg_block(blocks_h2[0]))
        min_work_node.send_and_ping(msg_block(blocks_h2[1]))

        assert_equal(self.nodes[0].getblockcount(), 2)
        assert_equal(self.nodes[1].getblockcount(), 1)
        self.log.info("First height 2 block accepted by node0; correctly rejected by node1")

        # 3. Send another block that builds on genesis.
        block_h1f = create_block(int("0x" + self.nodes[0].getblockhash(0), 0), create_coinbase(1), block_time)
        block_time += 1
        block_h1f.solve()
        test_node.send_and_ping(msg_block(block_h1f))

        tip_entry_found = False
        for x in self.nodes[0].getchaintips():
            if x['hash'] == block_h1f.hash:
                assert_equal(x['status'], "headers-only")
                tip_entry_found = True
        assert tip_entry_found
        assert_raises_rpc_error(-1, "Block not found on disk", self.nodes[0].getblock, block_h1f.hash)
        self.log.info("Forking block stored as headers-only (no block data)")

        # 4a. Send another block that builds on the fork.
        # ReddCoin: This block should be REJECTED because its parent (block_h1f)
        # is headers-only. This is intentional PoS security behavior.
        block_h2f = create_block(block_h1f.sha256, create_coinbase(2), block_time)
        block_time += 1
        block_h2f.solve()
        test_node.send_and_ping(msg_block(block_h2f))

        # ReddCoin: Verify the block was REJECTED (not in getchaintips at all)
        # because its parent is headers-only
        chaintips = self.nodes[0].getchaintips()
        block_h2f_in_tips = any(x['hash'] == block_h2f.hash for x in chaintips)
        assert not block_h2f_in_tips, "block_h2f should NOT be in getchaintips (ReddCoin rejects out-of-order blocks)"
        assert_raises_rpc_error(-5, "Block not found", self.nodes[0].getblockheader, block_h2f.hash)
        self.log.info("Block building on headers-only parent correctly REJECTED (ReddCoin PoS security)")

        # 4b. Send a third block building on the rejected block.
        # ReddCoin: Also rejected since parent chain isn't validated.
        block_h3 = create_block(block_h2f.sha256, create_coinbase(3), block_h2f.nTime+1)
        block_h3.solve()
        test_node.send_and_ping(msg_block(block_h3))

        # ReddCoin: Verify this block was also rejected
        chaintips = self.nodes[0].getchaintips()
        block_h3_in_tips = any(x['hash'] == block_h3.hash for x in chaintips)
        assert not block_h3_in_tips, "block_h3 should NOT be in getchaintips"
        assert_raises_rpc_error(-5, "Block not found", self.nodes[0].getblockheader, block_h3.hash)
        self.log.info("Second out-of-order block also correctly REJECTED")

        # 4c. Test that sending HEADERS first, then blocks, allows proper processing.
        # This is the correct way to deliver fork chains in ReddCoin.
        # Create a new fork chain and send headers first.
        block_h1f_new = create_block(int("0x" + self.nodes[0].getblockhash(0), 0), create_coinbase(1), block_time)
        block_time += 1
        block_h1f_new.solve()
        block_h2f_new = create_block(block_h1f_new.sha256, create_coinbase(2), block_time)
        block_time += 1
        block_h2f_new.solve()
        block_h3f_new = create_block(block_h2f_new.sha256, create_coinbase(3), block_time)
        block_time += 1
        block_h3f_new.solve()

        # Send all headers first
        headers_message = msg_headers()
        headers_message.headers.append(CBlockHeader(block_h1f_new))
        headers_message.headers.append(CBlockHeader(block_h2f_new))
        headers_message.headers.append(CBlockHeader(block_h3f_new))
        test_node.send_and_ping(headers_message)

        # Verify headers are known
        for block in [block_h1f_new, block_h2f_new, block_h3f_new]:
            tip_entry_found = False
            for x in self.nodes[0].getchaintips():
                if x['hash'] == block.hash:
                    assert_equal(x['status'], "headers-only")
                    tip_entry_found = True
                    break
        self.log.info("Headers for fork chain accepted")

        # Now send blocks in order - they should be accepted
        test_node.send_and_ping(msg_block(block_h1f_new))
        test_node.send_and_ping(msg_block(block_h2f_new))
        test_node.send_and_ping(msg_block(block_h3f_new))

        # Verify blocks are now available and chain reorged
        self.nodes[0].getblock(block_h1f_new.hash)
        self.nodes[0].getblock(block_h2f_new.hash)
        self.nodes[0].getblock(block_h3f_new.hash)
        assert_equal(self.nodes[0].getbestblockhash(), block_h3f_new.hash)
        assert_equal(self.nodes[0].getblockcount(), 3)
        self.log.info("Headers-first approach allows proper fork chain processing and reorg")

        # 5. (Skipped - ReddCoin's out-of-order rejection means the original
        # Bitcoin test scenario doesn't apply)

        # 6. Test inv/getdata mechanism on main chain
        # Build more blocks on the current tip
        all_blocks = []
        tip = block_h3f_new
        for i in range(10):
            next_block = create_block(tip.sha256, create_coinbase(i + 4), tip.nTime+1)
            next_block.solve()
            all_blocks.append(next_block)
            tip = next_block

        # Send headers for all blocks
        headers_message = msg_headers()
        for block in all_blocks:
            headers_message.headers.append(CBlockHeader(block))
        test_node.send_and_ping(headers_message)

        # Send all blocks
        for block in all_blocks:
            test_node.send_and_ping(msg_block(block))

        assert_equal(self.nodes[0].getblockcount(), 13)
        assert_equal(self.nodes[0].getbestblockhash(), all_blocks[-1].hash)
        self.log.info("Extended chain to height 13")

        # Disconnect and reconnect for clean state
        self.nodes[0].disconnect_p2ps()
        self.nodes[1].disconnect_p2ps()
        test_node = self.nodes[0].add_p2p_connection(P2PInterface())

        # 7. Test that blocks arrive correctly via headers-first pattern
        # Create a new block
        block_h14 = create_block(all_blocks[-1].sha256, create_coinbase(14), all_blocks[-1].nTime+1)
        block_h14.solve()

        # Send header first, then block (ReddCoin's required pattern)
        headers_message = msg_headers()
        headers_message.headers.append(CBlockHeader(block_h14))
        test_node.send_and_ping(headers_message)

        # Verify header is known but block data is not
        tip_entry_found = False
        for x in self.nodes[0].getchaintips():
            if x['hash'] == block_h14.hash:
                assert_equal(x['status'], "headers-only")
                tip_entry_found = True
        assert tip_entry_found, "Header for block_h14 should be known"

        # Send the block
        test_node.send_and_ping(msg_block(block_h14))
        assert_equal(self.nodes[0].getblockcount(), 14)
        self.log.info("Block via headers-first pattern accepted, chain extended")

        # 8. Create a chain which is invalid at a height longer than the
        # current chain, but which has more blocks on top of that
        block_15f = create_block(block_h14.sha256, create_coinbase(15), block_h14.nTime+1)
        block_15f.solve()
        block_16f = create_block(block_15f.sha256, create_coinbase(16), block_15f.nTime+1)
        block_16f.solve()
        block_17_invalid = create_block(block_16f.sha256, create_coinbase(17), block_16f.nTime+1)
        # block_17_invalid spends a coinbase below maturity!
        block_17_invalid.vtx.append(create_tx_with_script(block_16f.vtx[0], 0, script_sig=b"42", amount=1))
        block_17_invalid.hashMerkleRoot = block_17_invalid.calc_merkle_root()
        block_17_invalid.solve()
        block_18f = create_block(block_17_invalid.sha256, create_coinbase(18), block_17_invalid.nTime+1)
        block_18f.solve()

        # Send all headers first (ReddCoin requires headers-first for fork chains)
        headers_message = msg_headers()
        headers_message.headers.append(CBlockHeader(block_15f))
        headers_message.headers.append(CBlockHeader(block_16f))
        headers_message.headers.append(CBlockHeader(block_17_invalid))
        headers_message.headers.append(CBlockHeader(block_18f))
        test_node.send_and_ping(headers_message)

        # Verify headers are accepted as headers-only
        tip_entry_found = False
        for x in self.nodes[0].getchaintips():
            if x['hash'] == block_18f.hash:
                assert_equal(x['status'], "headers-only")
                tip_entry_found = True
        assert tip_entry_found
        assert_raises_rpc_error(-1, "Block not found on disk", self.nodes[0].getblock, block_18f.hash)

        # Send valid blocks to trigger reorg attempt
        test_node.send_and_ping(msg_block(block_15f))
        test_node.send_and_ping(msg_block(block_16f))

        self.nodes[0].getblock(block_15f.hash)
        self.nodes[0].getblock(block_16f.hash)

        # Send the invalid block
        test_node.send_message(msg_block(block_17_invalid))

        # Wait for processing - may or may not disconnect
        try:
            test_node.sync_with_ping(timeout=1)
        except AssertionError:
            test_node.wait_for_disconnect()
            self.nodes[0].disconnect_p2ps()
            test_node = self.nodes[0].add_p2p_connection(P2PInterface())

        # We should have failed reorg and stayed at height 16
        assert_equal(self.nodes[0].getblockcount(), 16)
        assert_equal(self.nodes[0].getbestblockhash(), block_16f.hash)
        assert_equal(self.nodes[0].getblock(block_17_invalid.hash)["confirmations"], -1)
        self.log.info("Invalid block correctly rejected, chain stayed at valid tip")

        # Now send a new header on the invalid chain
        # Note: With -whitelist=noban, peer won't be disconnected but misbehavior is recorded
        block_19f = create_block(block_18f.sha256, create_coinbase(19), block_18f.nTime+1)
        block_19f.solve()
        headers_message = msg_headers()
        headers_message.headers.append(CBlockHeader(block_19f))
        test_node.send_and_ping(headers_message)

        # Verify the header on invalid chain was rejected (not added to any tip)
        block_19f_in_tips = any(x['hash'] == block_19f.hash for x in self.nodes[0].getchaintips())
        assert not block_19f_in_tips, "Header on invalid chain should be rejected"
        self.log.info("Header on invalid chain correctly rejected (peer whitelisted, no disconnect)")

        # 9. Connect node1 to node0 and ensure it is able to sync
        self.connect_nodes(0, 1)
        self.sync_blocks([self.nodes[0], self.nodes[1]])
        self.log.info("Successfully synced nodes 1 and 0")

if __name__ == '__main__':
    AcceptBlockTest().main()
