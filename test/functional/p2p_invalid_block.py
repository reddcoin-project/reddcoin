#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test node responses to invalid blocks.

In this test we connect to one node over p2p, and test block requests:
1) Valid blocks should be requested and become chain tip.
2) Invalid block with duplicated transaction should be re-requested.
3) Invalid block with bad coinbase value should be rejected and not
re-requested.
4) Invalid block due to future timestamp is later accepted when that timestamp
becomes valid.

ReddCoin adaptation notes:
- Uses getblocktemplate to create valid blocks (ReddCoin uses scrypt PoW)
- Tests run in PoS phase (height > 89) using proper PoS block creation
- Block signing handled via sign_block() for PoS blocks
"""
import copy

from test_framework.blocktools import (
    create_block,
    NORMAL_GBT_REQUEST_PARAMS,
    sign_block,
)
from test_framework.messages import COIN
from test_framework.p2p import P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, advance_time_for_pos, set_node_times

MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60


class InvalidBlockRequestTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with mature coins for PoS staking
        self.extra_args = [["-whitelist=noban@127.0.0.1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def build_block_on_tip(self, node, txlist=None):
        """Build a PoS block on the current tip using getblocktemplate.

        Returns a solved and signed block ready to submit or modify.
        """
        advance_time_for_pos(node, seconds=60)

        max_attempts = 10
        for attempt in range(max_attempts):
            try:
                # Check current mocktime
                best_hash = node.getbestblockhash()
                header_time = node.getblockheader(best_hash)['time']
                mocktime = getattr(node, 'mocktime', None)
                self.log.info(f"  build_block_on_tip: attempt {attempt + 1}/{max_attempts}, mocktime={mocktime}, header_time={header_time}")
                block = create_block(tmpl=node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS), txlist=txlist)
                self.log.info(f"  build_block_on_tip: success!")
                break
            except Exception as e:
                self.log.info(f"  build_block_on_tip: failed - {str(e)[:100]}")
                if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                    new_time = advance_time_for_pos(node, seconds=300)  # Advance more time
                    self.log.info(f"  build_block_on_tip: advanced time to {new_time}")
                    continue
                raise

        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()

        # Sign the PoS block
        self.sign_block_with_coinstake_key(node, block)
        return block

    def sign_block_with_coinstake_key(self, node, block):
        """Sign a PoS block using the coinstake's private key."""
        coinstake = block.vtx[1]
        coinstake_hex = coinstake.serialize().hex()
        decoded_tx = node.decoderawtransaction(coinstake_hex)

        signing_key = None
        try:
            script_pubkey = decoded_tx['vout'][1]['scriptPubKey']
            coinstake_address = script_pubkey.get('address')
            if not coinstake_address:
                addresses = script_pubkey.get('addresses', [])
                if addresses:
                    coinstake_address = addresses[0]
            if coinstake_address:
                signing_key = node.dumpprivkey(coinstake_address)
        except Exception as e:
            self.log.debug("coinstake key lookup failed: %s" % e)

        if not signing_key:
            signing_key = node.get_deterministic_priv_key().key

        sign_block(block, signing_key)

    def run_test(self):
        node = self.nodes[0]

        # ReddCoin: Using cache (setup_clean_chain = False) with 199 blocks
        # Cache has mature coins ready for PoS staking
        self.log.info("Using cache with mature coins for PoS staking")
        initial_height = node.getblockcount()
        self.log.info(f"  Initial height: {initial_height}")

        # Advance time to ensure coins have sufficient age for staking
        advance_time_for_pos(node, seconds=600)

        # Connect P2P peer after block generation
        peer = node.add_p2p_connection(P2PDataStore())

        # Use merkle-root malleability to generate an invalid block with
        # same blockheader (CVE-2012-2459).
        # For merkle root malleability, need ODD transaction count so duplicate doesn't change root.
        # PoS block: coinbase + coinstake + tx1 = 3 txs (odd)
        # Duplicating tx1 gives 4 txs but same merkle root
        # For more information on merkle-root malleability see src/consensus/merkle.cpp.
        self.log.info("Test merkle root malleability.")

        # First, build a basic PoS block to get the coinstake
        # Then we'll find a UTXO NOT used by the coinstake for tx1
        self.log.info("  Building block template to determine coinstake UTXO")
        from decimal import Decimal
        from test_framework.messages import tx_from_hex

        # Get block template first to see which UTXO the coinstake uses
        advance_time_for_pos(node, seconds=60)
        max_attempts = 10
        tmpl = None
        for attempt in range(max_attempts):
            try:
                tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                    advance_time_for_pos(node, seconds=300)
                    continue
                raise
        if tmpl is None:
            raise AssertionError("Failed to get block template")

        # Extract coinstake input to avoid using the same UTXO for tx1
        coinstake_hex = tmpl['transactions'][0]['data']
        coinstake_tx = tx_from_hex(coinstake_hex)
        coinstake_input = coinstake_tx.vin[0].prevout
        coinstake_txid = f"{coinstake_input.hash:064x}"
        coinstake_vout = coinstake_input.n
        self.log.info(f"  Coinstake uses UTXO: {coinstake_txid}:{coinstake_vout}")

        # Find a different UTXO for tx1 (not the coinstake input)
        unspent_list = node.listunspent()
        self.log.info(f"  Found {len(unspent_list)} UTXOs")
        unspent = None
        for utxo in unspent_list:
            # Skip the UTXO used by coinstake
            if utxo['txid'] == coinstake_txid and utxo['vout'] == coinstake_vout:
                continue
            if Decimal(str(utxo['amount'])) >= Decimal('1'):
                unspent = utxo
                break
        if unspent is None:
            raise AssertionError("No UTXO available for tx1 (all used by coinstake)")
        self.log.info(f"  Using UTXO for tx1: {unspent['txid']}:{unspent['vout']} amount={unspent['amount']}")
        privkey = node.dumpprivkey(unspent['address'])

        # Create tx1 that spends the selected UTXO
        utxo_amount = Decimal(str(unspent['amount']))
        tx1_output_amount = utxo_amount - Decimal('0.01')  # 0.01 RDD fee
        tx1_raw = node.createrawtransaction(
            [{"txid": unspent['txid'], "vout": unspent['vout']}],
            [{node.getnewaddress(): float(tx1_output_amount)}]
        )
        tx1_signed = node.signrawtransactionwithkey(tx1_raw, [privkey])['hex']
        tx1 = tx_from_hex(tx1_signed)
        tx1.calc_sha256()

        # Now build the block using the same template (same coinstake) + tx1
        block2 = create_block(tmpl=tmpl, txlist=[tx1])
        block2.hashMerkleRoot = block2.calc_merkle_root()
        block2.rehash()
        block2.solve()
        self.sign_block_with_coinstake_key(node, block2)
        orig_hash = block2.sha256
        block2_orig = copy.deepcopy(block2)

        # Mutate block by duplicating the last transaction
        # With odd tx count, this creates same merkle root (the CVE)
        block2.vtx.append(block2.vtx[-1])  # Duplicate tx1
        assert_equal(block2.hashMerkleRoot, block2.calc_merkle_root())
        assert_equal(orig_hash, block2.rehash())
        assert block2_orig.vtx != block2.vtx

        self.log.info(f"  block2 (mutated) hash: {block2.hash}")
        self.log.info(f"  block2_orig hash: {block2_orig.hash}")
        self.log.info(f"  Hashes equal (CVE): {block2.hash == block2_orig.hash}")

        peer.send_blocks_and_test([block2], node, success=False, reject_reason='bad-txns-duplicate')

        # Check transactions for duplicate inputs (CVE-2018-17144)
        self.log.info("Test duplicate input block.")

        block2_dup = copy.deepcopy(block2_orig)
        # In PoS block: vtx[0]=coinbase, vtx[1]=coinstake, vtx[2]=tx1, vtx[3]=tx2
        tx2_idx = len(block2_dup.vtx) - 1
        block2_dup.vtx[tx2_idx].vin.append(block2_dup.vtx[tx2_idx].vin[0])
        block2_dup.vtx[tx2_idx].rehash()
        block2_dup.hashMerkleRoot = block2_dup.calc_merkle_root()
        block2_dup.rehash()
        block2_dup.solve()
        self.sign_block_with_coinstake_key(node, block2_dup)
        peer.send_blocks_and_test([block2_dup], node, success=False, reject_reason='bad-txns-inputs-duplicate')

        self.log.info("Test very broken block (modified coinstake).")
        # ReddCoin PoS: Test that blocks with modified coinstake are rejected
        # Modifying the coinstake output invalidates the transaction signature,
        # causing the block to fail validation (CheckQueue/signature check)
        block3 = self.build_block_on_tip(node)
        block3.vtx[1].vout[1].nValue = 1000000 * COIN  # Modify coinstake output
        block3.vtx[1].sha256 = None
        block3.vtx[1].calc_sha256()
        block3.hashMerkleRoot = block3.calc_merkle_root()
        block3.rehash()
        block3.solve()
        self.sign_block_with_coinstake_key(node, block3)

        self.log.info(f"  block3 hash: {block3.hash}")

        # ReddCoin: Modified coinstake output is detected as invalid stake reward amount
        peer.send_blocks_and_test([block3], node, success=False, reject_reason='bad-posv-amount')

        # Complete testing of CVE-2012-2459 by sending the original block.
        # It should be accepted even though it has the same hash as the mutated one.
        self.log.info("Test accepting original block after rejecting its mutated version.")
        self.log.info(f"  block2_orig hash: {block2_orig.hash}")
        self.log.info(f"  block2_orig prev: {block2_orig.hashPrevBlock:064x}")
        self.log.info(f"  block2_orig merkle: {block2_orig.hashMerkleRoot:064x}")
        self.log.info(f"  block2_orig vtx count: {len(block2_orig.vtx)}")
        self.log.info(f"  current tip: {node.getbestblockhash()}")
        self.log.info(f"  current height: {node.getblockcount()}")
        peer.send_blocks_and_test([block2_orig], node, success=True, timeout=5)

        # Complete testing of CVE-2018-17144, by checking for the inflation bug.
        self.log.info("Test inflation by duplicating input")

        # Get block template first to determine coinstake UTXO
        advance_time_for_pos(node, seconds=60)
        tmpl4 = None
        for attempt in range(10):
            try:
                tmpl4 = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < 9:
                    advance_time_for_pos(node, seconds=300)
                    continue
                raise
        coinstake4_tx = tx_from_hex(tmpl4['transactions'][0]['data'])
        coinstake4_input = coinstake4_tx.vin[0].prevout
        coinstake4_txid = f"{coinstake4_input.hash:064x}"

        # Find a UTXO NOT used by coinstake for tx3
        unspent2 = None
        for utxo in node.listunspent():
            if utxo['txid'] == coinstake4_txid:
                continue
            if Decimal(str(utxo['amount'])) >= Decimal('10'):
                unspent2 = utxo
                break
        if unspent2 is None:
            raise AssertionError("No UTXO available for tx3")
        privkey2 = node.dumpprivkey(unspent2['address'])
        tx3_raw = node.createrawtransaction(
            [{"txid": unspent2['txid'], "vout": unspent2['vout']}],
            # Keep this in Decimal. The amount is a coinstake output, so it is
            # an arbitrary number of satoshis, and going through float leaves a
            # value the node will not accept.
            [{node.getnewaddress(): unspent2['amount'] - Decimal('0.01')}]
        )
        tx3_signed = node.signrawtransactionwithkey(tx3_raw, [privkey2])['hex']
        tx3 = tx_from_hex(tx3_signed)

        # Duplicate input to test inflation bug
        tx3.vin.append(tx3.vin[0])
        tx3.rehash()

        # Build block using same template + tx3
        block4 = create_block(tmpl=tmpl4, txlist=[tx3])
        block4.hashMerkleRoot = block4.calc_merkle_root()
        block4.rehash()
        block4.solve()
        self.sign_block_with_coinstake_key(node, block4)
        peer.send_blocks_and_test([block4], node, success=False, reject_reason='bad-txns-inputs-duplicate')

        self.log.info("Test accepting identical block after rejecting it due to a future timestamp.")
        best_block = node.getblock(node.getbestblockhash())
        t = best_block["time"] + 60
        set_node_times([node], t)

        # Build a valid PoS block at current time
        block = self.build_block_on_tip(node)
        self.log.info(f"  Block built at t={t}, block.nTime={block.nTime}")

        # ReddCoin PoS: Block timestamp must match coinstake timestamp.
        # To test 'time-too-new', manually set both timestamps to far future.
        # Note: Modifying coinstake.nTime changes its txid, so we must recalculate merkle root.
        future_time = t + MAX_FUTURE_BLOCK_TIME + 1
        block.nTime = future_time
        block.vtx[1].nTime = future_time  # Update coinstake timestamp to match
        block.vtx[1].sha256 = None  # Clear cached hash
        block.vtx[1].calc_sha256()  # Recalculate with new nTime
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()
        self.sign_block_with_coinstake_key(node, block)

        # Reset mocktime to t AFTER building block (build_block_on_tip advances time)
        set_node_times([node], t)
        self.log.info(f"  Modified block: block.nTime={block.nTime}, coinstake.nTime={block.vtx[1].nTime}")
        self.log.info(f"  Mocktime reset to t={t}, difference = {block.nTime - t} seconds (MAX_FUTURE={MAX_FUTURE_BLOCK_TIME})")

        # Need force_send because the block will get rejected without a getdata otherwise
        peer.send_blocks_and_test([block], node, force_send=True, success=False, reject_reason='time-too-new')

        # Advance time so the block timestamp becomes valid
        set_node_times([node], future_time + 1)
        peer.send_blocks_and_test([block], node, success=True)


if __name__ == '__main__':
    InvalidBlockRequestTest().main()
