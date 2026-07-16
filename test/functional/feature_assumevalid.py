#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test logic for skipping signature validation on old blocks.

Test logic for skipping signature validation on blocks which we've assumed
valid (https://github.com/bitcoin/bitcoin/pull/9484)

ReddCoin adaptation: PoW ends at block 89 on regtest (nLastPowHeight=89).
We build PoW blocks 1-89 (with a bad block at height COINBASE_MATURITY+2),
then extend the chain with PoS-versioned headers (no actual block bodies)
to provide sufficient burial depth for the assumevalid time-equivalent check.

Chain structure:
    0:        genesis block
    1:        block 1 with coinbase transaction output.
    2-61:     bury that block with COINBASE_MATURITY (60) blocks so the
              coinbase transaction output can be spent
    62:       a block containing a transaction spending the coinbase
              transaction output. The transaction has an invalid signature.
    63-89:    remaining PoW blocks for additional burial
    90-2289:  PoS-versioned headers only (no block bodies) to provide
              burial depth exceeding two weeks' equivalent work

Start three nodes:

    - node0 has no -assumevalid parameter. Try to sync blocks 1-89.
      It will reject block 62 and only sync as far as block 61.
    - node1 has -assumevalid set to the hash of block 62. Receives all
      2289 headers plus blocks 1-89. The burial depth (2227 blocks at
      600s target = ~15.5 days) exceeds two weeks, so script validation
      is skipped. node1 will sync all the way to block 89.
    - node2 has -assumevalid set to the hash of block 62. Only receives
      200 headers. The burial depth (138 blocks at 600s = ~23 hours) is
      less than two weeks, so script validation still runs. node2 will
      reject block 62 and only sync as far as block 61.
"""

from test_framework.blocktools import (
    COINBASE_MATURITY,
    create_coinbase,
)
from test_framework.key import ECKey
from test_framework.messages import (
    CBlock,
    CBlockHeader,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    msg_block,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (CScript, OP_TRUE)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
import time

# ReddCoin regtest: PoW ends at block 89, PoS from block 90+
POW_HEIGHT = 89
# Number of PoS-versioned headers for burial (>2100 needed for 2-week equivalent)
POS_HEADERS = 2200
# Bad block is at COINBASE_MATURITY + 2 (= 62)
BAD_BLOCK_HEIGHT = COINBASE_MATURITY + 2
# PoW block version (version <= 2 = POW_BLOCK_VERSION)
POW_VERSION = 2
# PoS header version (must satisfy all buried deployment version requirements)
POS_VERSION = 5


class BaseNode(P2PInterface):
    def send_header_for_blocks(self, new_blocks):
        """Send headers extracted from CBlock objects."""
        headers_message = msg_headers()
        headers_message.headers = [CBlockHeader(b) for b in new_blocks]
        self.send_message(headers_message)

    def send_headers(self, headers):
        """Send pre-constructed CBlockHeader objects."""
        headers_message = msg_headers()
        headers_message.headers = headers
        self.send_message(headers_message)


class AssumeValidTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.rpc_timeout = 120

    def setup_network(self):
        self.add_nodes(3)
        # Start node0. We don't start the other nodes yet since
        # we need to pre-mine a block with an invalid transaction
        # signature so we can pass in the block hash as assumevalid.
        self.start_node(0)

    def send_blocks_until_disconnected(self, p2p_conn):
        """Keep sending blocks to the node until we're disconnected."""
        for i in range(len(self.blocks)):
            if not p2p_conn.is_connected:
                break
            try:
                p2p_conn.send_message(msg_block(self.blocks[i]))
            except IOError:
                assert not p2p_conn.is_connected
                break

    def run_test(self):
        p2p0 = self.nodes[0].add_p2p_connection(BaseNode())

        # Build the blockchain
        self.tip = int(self.nodes[0].getbestblockhash(), 16)
        # Use current real time for block timestamps so IsInitialBlockDownload()
        # returns false (blocks must be within DEFAULT_MAX_TIP_AGE = 8 hours of real time).
        # Without this, fScriptChecks would be false during IBD, defeating the test.
        self.block_time = int(time.time()) - 600

        self.blocks = []

        # Get a pubkey for the coinbase TXO
        coinbase_key = ECKey()
        coinbase_key.generate()
        coinbase_pubkey = coinbase_key.get_pubkey().get_bytes()

        # Create the first block with a coinbase output to our key
        height = 1
        block = CBlock()
        block.nVersion = POW_VERSION
        block.hashPrevBlock = self.tip
        block.vtx = [create_coinbase(height, coinbase_pubkey)]
        block.nTime = self.block_time
        block.nBits = 0x207fffff
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()
        self.blocks.append(block)
        self.block_time += 1
        # Save the coinbase for later
        self.block1 = block
        self.tip = block.sha256
        height += 1

        # Bury the block COINBASE_MATURITY deep so the coinbase output is spendable
        for _ in range(COINBASE_MATURITY):
            block = CBlock()
            block.nVersion = POW_VERSION
            block.hashPrevBlock = self.tip
            block.vtx = [create_coinbase(height)]
            block.nTime = self.block_time
            block.nBits = 0x207fffff
            block.hashMerkleRoot = block.calc_merkle_root()
            block.rehash()
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        # Create a transaction spending the coinbase output with an invalid (null) signature
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(self.block1.vtx[0].sha256, 0), scriptSig=b""))
        tx.vout.append(CTxOut(49 * 100000000, CScript([OP_TRUE])))
        tx.calc_sha256()

        bad_block = CBlock()
        bad_block.nVersion = POW_VERSION
        bad_block.hashPrevBlock = self.tip
        bad_block.vtx = [create_coinbase(height), tx]
        bad_block.nTime = self.block_time
        bad_block.nBits = 0x207fffff
        bad_block.hashMerkleRoot = bad_block.calc_merkle_root()
        bad_block.rehash()
        bad_block.solve()
        self.blocks.append(bad_block)
        self.tip = bad_block.sha256
        self.block_time += 1
        height += 1

        # Fill remaining PoW blocks up to POW_HEIGHT
        remaining_pow = POW_HEIGHT - height + 1
        for _ in range(remaining_pow):
            block = CBlock()
            block.nVersion = POW_VERSION
            block.hashPrevBlock = self.tip
            block.vtx = [create_coinbase(height)]
            block.nTime = self.block_time
            block.nBits = 0x207fffff
            block.hashMerkleRoot = block.calc_merkle_root()
            block.rehash()
            block.solve()
            self.blocks.append(block)
            self.tip = block.sha256
            self.block_time += 1
            height += 1

        assert_equal(len(self.blocks), POW_HEIGHT)
        assert_equal(height, POW_HEIGHT + 1)

        # Create fake PoS-versioned headers for burial depth.
        # These are header-only entries (no block bodies) that extend the chain
        # to provide enough equivalent work time for assumevalid to skip script checks.
        # PoS headers (nVersion > POW_BLOCK_VERSION) skip the PoW hash check in
        # CheckBlockHeader, so they don't need valid scrypt solutions.
        self.pos_headers = []
        for _ in range(POS_HEADERS):
            header = CBlockHeader()
            header.nVersion = POS_VERSION
            header.hashPrevBlock = self.tip
            header.hashMerkleRoot = 0  # Doesn't matter for header-only acceptance
            header.nTime = self.block_time
            header.nBits = 0x207fffff  # Same difficulty as PoW for equivalent chainwork
            header.nNonce = 0
            header.rehash()
            self.pos_headers.append(header)
            self.tip = header.sha256
            self.block_time += 1
            height += 1

        # Combine all headers for sending (PoW block headers + PoS fake headers)
        all_headers = [CBlockHeader(b) for b in self.blocks] + self.pos_headers
        total_height = POW_HEIGHT + POS_HEADERS  # = 2289

        self.nodes[0].disconnect_p2ps()

        # Start node1 and node2 with assumevalid so they accept a block with a bad signature.
        self.start_node(1, extra_args=["-assumevalid=" + hex(bad_block.sha256)])
        self.start_node(2, extra_args=["-assumevalid=" + hex(bad_block.sha256)])

        p2p0 = self.nodes[0].add_p2p_connection(BaseNode())
        p2p1 = self.nodes[1].add_p2p_connection(BaseNode())
        p2p2 = self.nodes[2].add_p2p_connection(BaseNode())

        # send header lists to all three nodes
        # Node0 and node1 get all headers (PoW + PoS)
        p2p0.send_headers(all_headers[0:2000])
        p2p0.send_headers(all_headers[2000:])
        p2p1.send_headers(all_headers[0:2000])
        p2p1.send_headers(all_headers[2000:])
        # Node2 only gets 200 headers (not enough burial for assumevalid)
        p2p2.send_headers(all_headers[0:200])

        # Send PoW blocks to node0. Bad block will be rejected.
        self.send_blocks_until_disconnected(p2p0)
        self.wait_until(lambda: self.nodes[0].getblockcount() >= COINBASE_MATURITY + 1)
        assert_equal(self.nodes[0].getblockcount(), COINBASE_MATURITY + 1)

        # Send all PoW blocks to node1. All blocks will be accepted because
        # assumevalid skips script checks (burial depth > 2 weeks equivalent).
        for i in range(len(self.blocks)):
            p2p1.send_message(msg_block(self.blocks[i]))
        p2p1.sync_with_ping(960)
        assert_equal(self.nodes[1].getblock(self.nodes[1].getbestblockhash())['height'], POW_HEIGHT)

        # Send blocks to node2. Bad block will be rejected because burial
        # depth (200 headers) is not enough for 2-week equivalent time.
        self.send_blocks_until_disconnected(p2p2)
        self.wait_until(lambda: self.nodes[2].getblockcount() >= COINBASE_MATURITY + 1)
        assert_equal(self.nodes[2].getblockcount(), COINBASE_MATURITY + 1)


if __name__ == '__main__':
    AssumeValidTest().main()
