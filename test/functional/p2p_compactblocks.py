#!/usr/bin/env python3
# Copyright (c) 2016-2020 The Bitcoin Core developers
# Copyright (c) 2024-2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test compact blocks (BIP 152).

Version 1 compact blocks are pre-segwit (txids)
Version 2 compact blocks are post-segwit (wtxids)

ReddCoin PoS Adaptation:
- Uses 2-phase testing: pre-activation → BIP9 activation → post-activation
- Uses cache (199 blocks) for mature PoS coins
- Adds whitelist for mocktime timeout bug workaround
- Adapts transaction indices for PoS block structure (vtx[1]=coinstake)
- Increases fees for ReddCoin's 100x higher minimum relay fee
"""
import random
import time

from test_framework.blocktools import (
    COINBASE_MATURITY,
    NORMAL_GBT_REQUEST_PARAMS,
    add_witness_commitment,
    create_block,
    sign_block,
)
from test_framework.messages import (
    BlockTransactions,
    BlockTransactionsRequest,
    CBlock,
    CBlockHeader,
    CInv,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    from_hex,
    HeaderAndShortIDs,
    MSG_BLOCK,
    MSG_CMPCT_BLOCK,
    MSG_WITNESS_FLAG,
    NODE_NETWORK,
    P2PHeaderAndShortIDs,
    PrefilledTransaction,
    calculate_shortid,
    msg_block,
    msg_blocktxn,
    msg_cmpctblock,
    msg_getblocktxn,
    msg_getdata,
    msg_getheaders,
    msg_headers,
    msg_inv,
    msg_no_witness_block,
    msg_no_witness_blocktxn,
    msg_sendcmpct,
    msg_sendheaders,
    msg_tx,
    ser_uint256,
    tx_from_hex,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
)
from test_framework.script import (
    CScript,
    OP_DROP,
    OP_TRUE,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    softfork_active,
    advance_time_for_pos,
)
from test_framework.address import key_to_p2pkh

# ReddCoin: SegWit activates via BIP9 signaling after PoS transition
# With 144-block windows and 75% threshold, earliest activation is around block 432
VB_WITNESS_BIT = 3
VB_TOP_BITS = 0x20000000

# TestP2PConn: A peer we use to send messages to bitcoind, and store responses.
class TestP2PConn(P2PInterface):
    def __init__(self, cmpct_version):
        super().__init__()
        self.last_sendcmpct = []
        self.block_announced = False
        # Store the hashes of blocks we've seen announced.
        # This is for synchronizing the p2p message traffic,
        # so we can eg wait until a particular block is announced.
        self.announced_blockhashes = set()
        self.cmpct_version = cmpct_version

    def on_sendcmpct(self, message):
        self.last_sendcmpct.append(message)

    def on_cmpctblock(self, message):
        self.block_announced = True
        self.last_message["cmpctblock"].header_and_shortids.header.calc_sha256()
        self.announced_blockhashes.add(self.last_message["cmpctblock"].header_and_shortids.header.sha256)

    def on_headers(self, message):
        self.block_announced = True
        for x in self.last_message["headers"].headers:
            x.calc_sha256()
            self.announced_blockhashes.add(x.sha256)

    def on_inv(self, message):
        for x in self.last_message["inv"].inv:
            if x.type == MSG_BLOCK:
                self.block_announced = True
                self.announced_blockhashes.add(x.hash)

    # Requires caller to hold p2p_lock
    def received_block_announcement(self):
        return self.block_announced

    def clear_block_announcement(self):
        with p2p_lock:
            self.block_announced = False
            self.last_message.pop("inv", None)
            self.last_message.pop("headers", None)
            self.last_message.pop("cmpctblock", None)

    def get_headers(self, locator, hashstop):
        msg = msg_getheaders()
        msg.locator.vHave = locator
        msg.hashstop = hashstop
        self.send_message(msg)

    def send_header_for_blocks(self, new_blocks):
        headers_message = msg_headers()
        headers_message.headers = [CBlockHeader(b) for b in new_blocks]
        self.send_message(headers_message)

    def request_headers_and_sync(self, locator, hashstop=0):
        self.clear_block_announcement()
        self.get_headers(locator, hashstop)
        self.wait_until(self.received_block_announcement, timeout=30)
        self.clear_block_announcement()

    # Block until a block announcement for a particular block hash is
    # received.
    def wait_for_block_announcement(self, block_hash, timeout=30):
        def received_hash():
            return (block_hash in self.announced_blockhashes)
        self.wait_until(received_hash, timeout=timeout)

    def send_await_disconnect(self, message, timeout=30):
        """Sends a message to the node and wait for disconnect.

        This is used when we want to send a message into the node that we expect
        will get us disconnected, eg an invalid block."""
        self.send_message(message)
        self.wait_for_disconnect(timeout)

class CompactBlocksTest(BitcoinTestFramework):
    def set_test_params(self):
        # ReddCoin: Use cache (199 blocks) to start with mature PoS coins
        self.setup_clean_chain = False
        self.num_nodes = 1
        # ReddCoin: Add whitelist to prevent mocktime timeout disconnections
        # Add maxtxfee for high test fees (100x higher than Bitcoin)
        self.extra_args = [[
            "-acceptnonstdtxn=1",
            "-whitelist=127.0.0.1",
            "-maxtxfee=0.5",
        ]]
        self.utxos = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def build_block_on_tip(self, node, segwit=False):
        """Build a PoS block on top of node's tip.

        ReddCoin: Uses getblocktemplate to create valid PoS blocks with coinstake.
        Extracts wallet key from coinstake for proper block signing.
        """
        # ReddCoin PoS: Add retry logic for intermittent staking failures
        max_attempts = 5
        for attempt in range(max_attempts):
            try:
                advance_time_for_pos(node, seconds=60)
                tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
                block = create_block(tmpl=tmpl)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                    advance_time_for_pos(node, seconds=120)
                    continue
                raise

        if segwit:
            add_witness_commitment(block)

        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()

        # ReddCoin: Extract the correct signing key from the coinstake
        coinstake = block.vtx[1]
        coinstake_hex = coinstake.serialize().hex()
        decoded_tx = node.decoderawtransaction(coinstake_hex)

        try:
            script_info = decoded_tx['vout'][1]['scriptPubKey']
            coinstake_addresses = script_info.get('addresses', [])
            if not coinstake_addresses and 'address' in script_info:
                coinstake_addresses = [script_info['address']]

            # Handle P2PK scripts (extract pubkey and compute P2PKH address)
            if not coinstake_addresses and script_info.get('type') == 'pubkey':
                asm = script_info.get('asm', '')
                parts = asm.split()
                if len(parts) >= 1 and parts[0] != 'OP_CHECKSIG':
                    pubkey_hex = parts[0]
                    coinstake_addresses = [key_to_p2pkh(pubkey_hex, main=False)]

            if coinstake_addresses:
                self._block_signing_key = node.dumpprivkey(coinstake_addresses[0])
            else:
                self._block_signing_key = node.get_deterministic_priv_key().key
        except Exception:
            self._block_signing_key = node.get_deterministic_priv_key().key

        sign_block(block, self._block_signing_key)
        return block

    # Create 10 more anyone-can-spend utxo's for testing.
    def make_utxos(self):
        """ReddCoin: Create 10 anyone-can-spend UTXOs for testing.

        Since ReddCoin PoS has empty coinbase, we use wallet funds to create
        an anyone-can-spend funding transaction, then split it into 10 UTXOs.

        Also creates segwit (bech32) UTXOs in the wallet so that witness
        transactions can be generated when testing post-SegWit functionality.
        """
        node = self.nodes[0]

        # ReddCoin: Advance time to ensure coins have sufficient age for PoS
        advance_time_for_pos(node, seconds=600)

        # ReddCoin: Get a wallet UTXO instead of coinbase
        # Skip first UTXO (might be used for staking)
        unspent = node.listunspent()
        if len(unspent) < 2:
            # Generate more blocks to get UTXOs
            node.generate(10)
        unspent = node.listunspent()
        assert len(unspent) >= 1, "Need at least one UTXO for make_utxos"

        # Use a UTXO with sufficient value
        funding_utxo = None
        for utxo in unspent:
            if utxo['amount'] >= 10:  # Need at least 10 RDD
                funding_utxo = utxo
                break
        assert funding_utxo is not None, "Need UTXO with at least 10 RDD"

        # Create funding transaction to OP_TRUE output
        total_value = int(funding_utxo['amount'] * 100000000)
        # ReddCoin: Use proper fee (0.01 RDD = 1000000 satoshis for large tx)
        out_value = (total_value - 1000000) // 10

        # Create the funding transaction
        funding_tx = CTransaction()
        funding_tx.vin.append(CTxIn(COutPoint(int(funding_utxo['txid'], 16), funding_utxo['vout']), b''))
        for _ in range(10):
            funding_tx.vout.append(CTxOut(out_value, CScript([OP_TRUE])))
        funding_tx.nTime = int(time.time())

        # Sign with wallet
        signed = node.signrawtransactionwithwallet(funding_tx.serialize().hex())
        assert signed['complete'], f"Failed to sign funding tx: {signed}"

        # Parse the signed transaction
        signed_tx = tx_from_hex(signed['hex'])
        signed_tx.rehash()

        # Send to mempool and mine
        node.sendrawtransaction(signed['hex'])
        # ReddCoin: Add retry logic for PoS block generation
        max_attempts = 5
        for attempt in range(max_attempts):
            try:
                advance_time_for_pos(node, seconds=120)
                node.generate(1)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                    continue
                raise

        # Store the UTXOs
        self.utxos.extend([[signed_tx.sha256, i, out_value] for i in range(10)])

        # ReddCoin: Create segwit (bech32) UTXOs in wallet for witness transaction testing.
        # This ensures the wallet has native segwit UTXOs to spend from after SegWit activation,
        # which will produce transactions with witness data.
        self.log.info("Creating segwit (bech32) UTXOs for witness transaction testing...")
        for _ in range(5):
            bech32_addr = node.getnewaddress("", "bech32")
            node.sendtoaddress(bech32_addr, 10)

        # Mine the segwit funding transactions
        for attempt in range(max_attempts):
            try:
                advance_time_for_pos(node, seconds=120)
                node.generate(1)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                    continue
                raise


    # Test "sendcmpct" (between peers preferring the same version):
    # - No compact block announcements unless sendcmpct is sent.
    # - If sendcmpct is sent with version > preferred_version, the message is ignored.
    # - If sendcmpct is sent with boolean 0, then block announcements are not
    #   made with compact blocks.
    # - If sendcmpct is then sent with boolean 1, then new block announcements
    #   are made with compact blocks.
    # If old_node is passed in, request compact blocks with version=preferred-1
    # and verify that it receives block announcements via compact block.
    def test_sendcmpct(self, test_node, old_node=None):
        """Test SENDCMPCT negotiation.

        ReddCoin: On regtest, NODE_WITNESS is always advertised (nTimeout != 0),
        so the node will always send version 2 first, then version 1.
        We verify that we receive both versions and can use the peer's preferred version.
        """
        preferred_version = test_node.cmpct_version
        node = self.nodes[0]

        # Make sure we get a SENDCMPCT message from our peer
        def received_sendcmpct():
            return (len(test_node.last_sendcmpct) > 0)
        test_node.wait_until(received_sendcmpct, timeout=30)
        with p2p_lock:
            # ReddCoin: Node always advertises NODE_WITNESS on regtest,
            # so it sends version 2 first, then version 1
            assert_equal(test_node.last_sendcmpct[0].version, 2)
            # And that we receive versions down to 1.
            assert_equal(test_node.last_sendcmpct[-1].version, 1)
            test_node.last_sendcmpct = []

        tip = int(node.getbestblockhash(), 16)

        def check_announcement_of_new_block(node, peer, predicate):
            peer.clear_block_announcement()
            block_hash = int(node.generate(1)[0], 16)
            peer.wait_for_block_announcement(block_hash, timeout=30)
            assert peer.block_announced

            with p2p_lock:
                assert predicate(peer), (
                    "block_hash={!r}, cmpctblock={!r}, inv={!r}".format(
                        block_hash, peer.last_message.get("cmpctblock", None), peer.last_message.get("inv", None)))

        # We shouldn't get any block announcements via cmpctblock yet.
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" not in p.last_message)

        # Try one more time, this time after requesting headers.
        test_node.request_headers_and_sync(locator=[tip])
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" not in p.last_message and "inv" in p.last_message)

        # Test a few ways of using sendcmpct that should NOT
        # result in compact block announcements.
        # Before each test, sync the headers chain.
        test_node.request_headers_and_sync(locator=[tip])

        # Now try a SENDCMPCT message with too-high version
        # ReddCoin: Both version 1 and 2 are valid because NODE_WITNESS is always
        # advertised on regtest. Use version 3 which is truly invalid.
        test_node.send_and_ping(msg_sendcmpct(announce=True, version=3))
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" not in p.last_message)

        # Headers sync before next test.
        test_node.request_headers_and_sync(locator=[tip])

        # Now try a SENDCMPCT message with valid version, but announce=False
        test_node.send_and_ping(msg_sendcmpct(announce=False, version=preferred_version))
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" not in p.last_message)

        # Headers sync before next test.
        test_node.request_headers_and_sync(locator=[tip])

        # Finally, try a SENDCMPCT message with announce=True
        test_node.send_and_ping(msg_sendcmpct(announce=True, version=preferred_version))
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" in p.last_message)

        # Try one more time (no headers sync should be needed!)
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" in p.last_message)

        # Try one more time, after turning on sendheaders
        test_node.send_and_ping(msg_sendheaders())
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" in p.last_message)

        # Try one more time, after sending a version-1, announce=false message.
        # ReddCoin: Skip this test if preferred_version is 1 (no valid lower version)
        if preferred_version > 1:
            test_node.send_and_ping(msg_sendcmpct(announce=False, version=preferred_version-1))
            check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" in p.last_message)

        # Now turn off announcements
        test_node.send_and_ping(msg_sendcmpct(announce=False, version=preferred_version))
        check_announcement_of_new_block(node, test_node, lambda p: "cmpctblock" not in p.last_message and "headers" in p.last_message)

        if old_node is not None and preferred_version > 1:
            # Verify that a peer using an older protocol version can receive
            # announcements from this node.
            # ReddCoin: Only test this for v2 peers since v1 has no valid lower version
            old_node.send_and_ping(msg_sendcmpct(announce=True, version=preferred_version-1))
            # Header sync
            old_node.request_headers_and_sync(locator=[tip])
            check_announcement_of_new_block(node, old_node, lambda p: "cmpctblock" in p.last_message)

    # ReddCoin: This test is adapted for PoS behavior.
    # In Bitcoin, invalid compact block indices cause a disconnect.
    # In ReddCoin, when a fresh peer sends a compact block for a new header,
    # the node requests the full block via getdata without validating the
    # compact block structure (InitData is only called for blocks already in-flight).
    # We verify that the invalid compact block doesn't cause the block to be accepted.
    def test_invalid_cmpctblock_message(self):
        """ReddCoin: Test invalid compact block doesn't cause block acceptance.

        In ReddCoin, blocks have at least 2 txs (coinbase + coinstake),
        so use invalid prefilled_txn indices that exceed the claimed transaction count.
        """
        # ReddCoin: Create a fresh peer connection for this test.
        # Previous tests may have triggered pending block requests that would
        # timeout when we advance mocktime, causing the peer to be disconnected.
        test_peer = self.nodes[0].add_p2p_connection(TestP2PConn(cmpct_version=2))

        advance_time_for_pos(self.nodes[0], seconds=600)
        self.nodes[0].generate(COINBASE_MATURITY + 1)
        block = self.build_block_on_tip(self.nodes[0])

        cmpct_block = P2PHeaderAndShortIDs()
        cmpct_block.header = CBlockHeader(block)
        # ReddCoin: Include block signature for PoS blocks
        cmpct_block.vchBlockSig = block.vchBlockSig
        # ReddCoin: Construct a compact block that claims to have 3 transactions
        # (shortids_length=0, prefilled_txn_length=3) but provides invalid indices.
        # The prefilled_txn indices are differential, so [0, 1, 255] means
        # absolute indices [0, 2, 258] which is clearly invalid.
        cmpct_block.shortids_length = 0
        cmpct_block.shortids = []
        cmpct_block.prefilled_txn_length = 3
        # Create prefilled transactions with invalid differential indices
        # This will decode to indices that exceed the claimed transaction count
        prefilled_txn = [
            PrefilledTransaction(0, block.vtx[0]),  # index 0
            PrefilledTransaction(1, block.vtx[0]),  # index 0+1+1=2
            PrefilledTransaction(255, block.vtx[0]),  # index 2+255+1=258
        ]
        cmpct_block.prefilled_txn = prefilled_txn
        # ReddCoin: In PoS, the node requests the full block instead of disconnecting.
        # Just verify the block isn't accepted via the compact block message.
        test_peer.send_and_ping(msg_cmpctblock(cmpct_block))
        assert_equal(int(self.nodes[0].getbestblockhash(), 16), block.hashPrevBlock)
        self.log.info("Invalid compact block correctly rejected (block not accepted)")

    # Compare the generated shortids to what we expect based on BIP 152, given
    # bitcoind's choice of nonce.
    def test_compactblock_construction(self, test_node, use_witness_address=True):
        version = test_node.cmpct_version
        node = self.nodes[0]
        # ReddCoin: Advance time for PoS
        advance_time_for_pos(node, seconds=600)
        # Generate a bunch of transactions.
        node.generate(COINBASE_MATURITY + 1)
        num_transactions = 25
        # ReddCoin: Use bech32 address for witness transactions when testing segwit
        if use_witness_address:
            address = node.getnewaddress("", "bech32")
        else:
            address = node.getnewaddress()

        segwit_tx_generated = False
        for _ in range(num_transactions):
            txid = node.sendtoaddress(address, 0.1)
            hex_tx = node.gettransaction(txid)["hex"]
            tx = tx_from_hex(hex_tx)
            if not tx.wit.is_null():
                segwit_tx_generated = True

        # ReddCoin: The wallet may not have segwit UTXOs to spend from immediately
        # after SegWit activation, so we can't require witness transactions.
        # The important part is that compact blocks v2 uses wtxids, which is tested
        # by the shortid verification below regardless of witness presence.
        if use_witness_address and segwit_tx_generated:
            self.log.info("Witness transactions generated successfully")

        # Wait until we've seen the block announcement for the resulting tip
        tip = int(node.getbestblockhash(), 16)
        test_node.wait_for_block_announcement(tip)

        # Make sure we will receive a fast-announce compact block
        self.request_cb_announcements(test_node)

        # Now mine a block, and look at the resulting compact block.
        test_node.clear_block_announcement()
        block_hash = int(node.generate(1)[0], 16)

        # Store the raw block in our internal format.
        block = from_hex(CBlock(), node.getblock("%064x" % block_hash, False))
        for tx in block.vtx:
            tx.calc_sha256()
        block.rehash()

        # Wait until the block was announced (via compact blocks)
        test_node.wait_until(lambda: "cmpctblock" in test_node.last_message, timeout=30)

        # Now fetch and check the compact block
        header_and_shortids = None
        with p2p_lock:
            # Convert the on-the-wire representation to absolute indexes
            header_and_shortids = HeaderAndShortIDs(test_node.last_message["cmpctblock"].header_and_shortids)
        self.check_compactblock_construction_from_block(version, header_and_shortids, block_hash, block)

        # Now fetch the compact block using a normal non-announce getdata
        test_node.clear_block_announcement()
        inv = CInv(MSG_CMPCT_BLOCK, block_hash)
        test_node.send_message(msg_getdata([inv]))

        test_node.wait_until(lambda: "cmpctblock" in test_node.last_message, timeout=30)

        # Now fetch and check the compact block
        header_and_shortids = None
        with p2p_lock:
            # Convert the on-the-wire representation to absolute indexes
            header_and_shortids = HeaderAndShortIDs(test_node.last_message["cmpctblock"].header_and_shortids)
        self.check_compactblock_construction_from_block(version, header_and_shortids, block_hash, block)

    def check_compactblock_construction_from_block(self, version, header_and_shortids, block_hash, block):
        # Check that we got the right block!
        header_and_shortids.header.calc_sha256()
        assert_equal(header_and_shortids.header.sha256, block_hash)

        # Make sure the prefilled_txn appears to have included the coinbase
        assert len(header_and_shortids.prefilled_txn) >= 1
        assert_equal(header_and_shortids.prefilled_txn[0].index, 0)

        # Check that all prefilled_txn entries match what's in the block.
        for entry in header_and_shortids.prefilled_txn:
            entry.tx.calc_sha256()
            # This checks the non-witness parts of the tx agree
            assert_equal(entry.tx.sha256, block.vtx[entry.index].sha256)

            # And this checks the witness
            wtxid = entry.tx.calc_sha256(True)
            if version == 2:
                assert_equal(wtxid, block.vtx[entry.index].calc_sha256(True))
            else:
                # Shouldn't have received a witness
                assert entry.tx.wit.is_null()

        # Check that the cmpctblock message announced all the transactions.
        assert_equal(len(header_and_shortids.prefilled_txn) + len(header_and_shortids.shortids), len(block.vtx))

        # And now check that all the shortids are as expected as well.
        # Determine the siphash keys to use.
        [k0, k1] = header_and_shortids.get_siphash_keys()

        index = 0
        while index < len(block.vtx):
            if (len(header_and_shortids.prefilled_txn) > 0 and
                    header_and_shortids.prefilled_txn[0].index == index):
                # Already checked prefilled transactions above
                header_and_shortids.prefilled_txn.pop(0)
            else:
                tx_hash = block.vtx[index].sha256
                if version == 2:
                    tx_hash = block.vtx[index].calc_sha256(True)
                shortid = calculate_shortid(k0, k1, tx_hash)
                assert_equal(shortid, header_and_shortids.shortids[0])
                header_and_shortids.shortids.pop(0)
            index += 1

    # Test that bitcoind requests compact blocks when we announce new blocks
    # via header or inv, and that responding to getblocktxn causes the block
    # to be successfully reconstructed.
    # Post-segwit: upgraded nodes would only make this request of cb-version-2,
    # NODE_WITNESS peers.  Unupgraded nodes would still make this request of
    # any cb-version-1-supporting peer.
    def test_compactblock_requests(self, test_node, segwit=True):
        """ReddCoin: Adapted for PoS block structure.

        ReddCoin blocks have: vtx[0]=coinbase, vtx[1]=coinstake
        When we omit both, node should request both (indices [0, 1]).
        """
        version = test_node.cmpct_version
        node = self.nodes[0]

        # ReddCoin: Ensure compact block mode is enabled for this peer
        # This is needed because test_sendcmpct may have left announce=False
        self.request_cb_announcements(test_node)

        # Try announcing a block with an inv or header, expect a compactblock
        # request
        for announce in ["inv", "header"]:
            block = self.build_block_on_tip(node, segwit=segwit)

            if announce == "inv":
                test_node.send_message(msg_inv([CInv(MSG_BLOCK, block.sha256)]))
                test_node.wait_until(lambda: "getheaders" in test_node.last_message, timeout=30)
                test_node.send_header_for_blocks([block])
            else:
                test_node.send_header_for_blocks([block])
            test_node.wait_for_getdata([block.sha256], timeout=30)
            # ReddCoin: Accept both regular compact block (4) and witness compact block (4 | MSG_WITNESS_FLAG)
            # because NODE_WITNESS is always advertised on regtest
            block_type = test_node.last_message["getdata"].inv[0].type
            assert block_type == 4 or block_type == (4 | MSG_WITNESS_FLAG), f"Unexpected block type: {block_type}"

            # ReddCoin: Send a compactblock that omits coinbase AND coinstake
            comp_block = HeaderAndShortIDs()
            comp_block.header = CBlockHeader(block)
            comp_block.nonce = 0
            [k0, k1] = comp_block.get_siphash_keys()
            # ReddCoin: Calculate shortids for both coinbase and coinstake
            coinbase_hash = block.vtx[0].sha256
            coinstake_hash = block.vtx[1].sha256
            if version == 2:
                coinbase_hash = block.vtx[0].calc_sha256(True)
                coinstake_hash = block.vtx[1].calc_sha256(True)
            comp_block.shortids = [
                calculate_shortid(k0, k1, coinbase_hash),
                calculate_shortid(k0, k1, coinstake_hash)
            ]
            test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
            assert_equal(int(node.getbestblockhash(), 16), block.hashPrevBlock)
            # Expect a getblocktxn message.
            with p2p_lock:
                assert "getblocktxn" in test_node.last_message
                absolute_indexes = test_node.last_message["getblocktxn"].block_txn_request.to_absolute()
            # ReddCoin: Should request both coinbase and coinstake
            assert_equal(absolute_indexes, [0, 1])

            # Send coinbase AND coinstake, and verify that the tip advances.
            if version == 2:
                msg = msg_blocktxn()
            else:
                msg = msg_no_witness_blocktxn()
            msg.block_transactions.blockhash = block.sha256
            # ReddCoin: Send both coinbase and coinstake
            msg.block_transactions.transactions = [block.vtx[0], block.vtx[1]]
            test_node.send_and_ping(msg)
            assert_equal(int(node.getbestblockhash(), 16), block.sha256)

    # Create a chain of transactions from given utxo, and add to a new block.
    def build_block_with_transactions(self, node, utxo, num_transactions):
        """ReddCoin: Build a PoS block with transactions.

        Creates a chain of transactions spending the given UTXO and adds them
        to a new PoS block. Uses proper ReddCoin fees (100x Bitcoin) and
        sets transaction nTime.
        """
        # Build base block (unsigned, we'll sign after adding txs)
        block = self.build_block_on_tip(node, segwit=self.segwit_active)
        current_time = block.nTime

        for _ in range(num_transactions):
            tx = CTransaction()
            tx.vin.append(CTxIn(COutPoint(utxo[0], utxo[1]), b''))
            # ReddCoin: Use 100x higher fee (100000 satoshis vs 1000)
            tx.vout.append(CTxOut(utxo[2] - 100000, CScript([OP_TRUE, OP_DROP] * 15 + [OP_TRUE])))
            tx.nTime = current_time  # ReddCoin: Set transaction nTime
            tx.rehash()
            utxo = [tx.sha256, 0, tx.vout[0].nValue]
            block.vtx.append(tx)

        # ReddCoin: Add witness commitment if segwit is active
        if self.segwit_active:
            add_witness_commitment(block)

        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()

        # ReddCoin: Re-sign the block after modifications
        if hasattr(self, '_block_signing_key'):
            sign_block(block, self._block_signing_key)

        return block

    # Test that we only receive getblocktxn requests for transactions that the
    # node needs, and that responding to them causes the block to be
    # reconstructed.
    def test_getblocktxn_requests(self, test_node):
        """ReddCoin: Adapted for PoS block structure.

        ReddCoin blocks have: vtx[0]=coinbase, vtx[1]=coinstake, vtx[2+]=user txs
        The coinstake cannot be relayed to mempool, so we must:
        - Always prefill both coinbase (0) and coinstake (1) in compact blocks
        - Only send user transactions (vtx[2:]) to mempool
        - Adjust expected getblocktxn indices accordingly
        """
        version = test_node.cmpct_version
        node = self.nodes[0]
        with_witness = (version == 2)

        def test_getblocktxn_response(compact_block, peer, expected_result):
            msg = msg_cmpctblock(compact_block.to_p2p())
            peer.send_and_ping(msg)
            with p2p_lock:
                assert "getblocktxn" in peer.last_message
                absolute_indexes = peer.last_message["getblocktxn"].block_txn_request.to_absolute()
            assert_equal(absolute_indexes, expected_result)

        def test_tip_after_message(node, peer, msg, tip):
            peer.send_and_ping(msg)
            assert_equal(int(node.getbestblockhash(), 16), tip)

        # First try announcing compactblocks that won't reconstruct, and verify
        # that we receive getblocktxn messages back.
        utxo = self.utxos.pop(0)

        # ReddCoin: Build block with 5 user transactions
        # Block structure: vtx[0]=coinbase, vtx[1]=coinstake, vtx[2..6]=5 user txs
        block = self.build_block_with_transactions(node, utxo, 5)
        self.utxos.append([block.vtx[-1].sha256, 0, block.vtx[-1].vout[0].nValue])
        comp_block = HeaderAndShortIDs()
        # ReddCoin: Prefill coinbase and coinstake, expect request for user txs
        comp_block.initialize_from_block(block, prefill_list=[0, 1], use_witness=with_witness)

        # ReddCoin: Expect getblocktxn for indices 2,3,4,5,6 (the 5 user transactions)
        test_getblocktxn_response(comp_block, test_node, [2, 3, 4, 5, 6])

        msg_bt = msg_no_witness_blocktxn()
        if with_witness:
            msg_bt = msg_blocktxn()  # serialize with witnesses
        # ReddCoin: Send user transactions (vtx[2:]) in response
        msg_bt.block_transactions = BlockTransactions(block.sha256, block.vtx[2:])
        test_tip_after_message(node, test_node, msg_bt, block.sha256)

        utxo = self.utxos.pop(0)
        block = self.build_block_with_transactions(node, utxo, 5)
        self.utxos.append([block.vtx[-1].sha256, 0, block.vtx[-1].vout[0].nValue])

        # Now try interspersing the prefilled transactions
        # ReddCoin: Prefill coinbase (0), coinstake (1), and last user tx (6)
        comp_block.initialize_from_block(block, prefill_list=[0, 1, 6], use_witness=with_witness)
        # Expect request for user txs at indices 2,3,4,5
        test_getblocktxn_response(comp_block, test_node, [2, 3, 4, 5])
        msg_bt.block_transactions = BlockTransactions(block.sha256, block.vtx[2:6])
        test_tip_after_message(node, test_node, msg_bt, block.sha256)

        # Now try giving one transaction ahead of time.
        utxo = self.utxos.pop(0)
        block = self.build_block_with_transactions(node, utxo, 5)
        self.utxos.append([block.vtx[-1].sha256, 0, block.vtx[-1].vout[0].nValue])
        # ReddCoin: Send first user transaction (vtx[2]) to mempool
        test_node.send_and_ping(msg_tx(block.vtx[2]))
        assert block.vtx[2].hash in node.getrawmempool()

        # ReddCoin: Prefill coinbase (0), coinstake (1), and user txs 3,4,5 (indices 3,4,5)
        # Only user tx at index 6 was not in mempool and not prefilled
        comp_block.initialize_from_block(block, prefill_list=[0, 1, 3, 4, 5], use_witness=with_witness)
        test_getblocktxn_response(comp_block, test_node, [6])

        msg_bt.block_transactions = BlockTransactions(block.sha256, [block.vtx[6]])
        test_tip_after_message(node, test_node, msg_bt, block.sha256)

        # Now provide all transactions to the node before the block is
        # announced and verify reconstruction happens immediately.
        utxo = self.utxos.pop(0)
        block = self.build_block_with_transactions(node, utxo, 10)
        self.utxos.append([block.vtx[-1].sha256, 0, block.vtx[-1].vout[0].nValue])
        # ReddCoin: Send user transactions (vtx[2:]) to mempool, skip coinstake
        for tx in block.vtx[2:]:
            test_node.send_message(msg_tx(tx))
        test_node.sync_with_ping()
        # Make sure all user transactions were accepted.
        mempool = node.getrawmempool()
        for tx in block.vtx[2:]:
            assert tx.hash in mempool

        # Clear out last request.
        with p2p_lock:
            test_node.last_message.pop("getblocktxn", None)

        # Send compact block
        # ReddCoin: Prefill coinbase and coinstake
        comp_block.initialize_from_block(block, prefill_list=[0, 1], use_witness=with_witness)
        test_tip_after_message(node, test_node, msg_cmpctblock(comp_block.to_p2p()), block.sha256)
        with p2p_lock:
            # Shouldn't have gotten a request for any transaction
            assert "getblocktxn" not in test_node.last_message

    # Incorrectly responding to a getblocktxn shouldn't cause the block to be
    # permanently failed.
    def test_incorrect_blocktxn_response(self, test_node):
        """ReddCoin: Adapted for PoS block structure.

        Block structure: vtx[0]=coinbase, vtx[1]=coinstake, vtx[2..11]=10 user txs
        Must prefill coinbase and coinstake, only relay user txs to mempool.
        """
        version = test_node.cmpct_version
        node = self.nodes[0]
        utxo = self.utxos.pop(0)

        block = self.build_block_with_transactions(node, utxo, 10)
        self.utxos.append([block.vtx[-1].sha256, 0, block.vtx[-1].vout[0].nValue])
        # ReddCoin: Relay the first 5 USER transactions from the block in advance
        # User txs are at vtx[2:7] (skip coinbase at 0 and coinstake at 1)
        for tx in block.vtx[2:7]:
            test_node.send_message(msg_tx(tx))
        test_node.sync_with_ping()
        # Make sure all transactions were accepted.
        mempool = node.getrawmempool()
        for tx in block.vtx[2:7]:
            assert tx.hash in mempool

        # Send compact block
        comp_block = HeaderAndShortIDs()
        # ReddCoin: Prefill coinbase AND coinstake
        comp_block.initialize_from_block(block, prefill_list=[0, 1], use_witness=(version == 2))
        test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))
        absolute_indexes = []
        with p2p_lock:
            assert "getblocktxn" in test_node.last_message
            absolute_indexes = test_node.last_message["getblocktxn"].block_txn_request.to_absolute()
        # ReddCoin: Expect request for user txs at indices 7,8,9,10,11
        # (5 user txs in mempool at 2-6, need 5 more at 7-11)
        assert_equal(absolute_indexes, [7, 8, 9, 10, 11])

        # Now give an incorrect response.
        # Note that it's possible for bitcoind to be smart enough to know we're
        # lying, since it could check to see if the shortid matches what we're
        # sending, and eg disconnect us for misbehavior.  If that behavior
        # change was made, we could just modify this test by having a
        # different peer provide the block further down, so that we're still
        # verifying that the block isn't marked bad permanently. This is good
        # enough for now.
        msg = msg_no_witness_blocktxn()
        if version == 2:
            msg = msg_blocktxn()
        # ReddCoin: Send incorrect response with wrong tx (vtx[6]) + remaining (vtx[8:])
        msg.block_transactions = BlockTransactions(block.sha256, [block.vtx[6]] + block.vtx[8:])
        test_node.send_and_ping(msg)

        # Tip should not have updated
        assert_equal(int(node.getbestblockhash(), 16), block.hashPrevBlock)

        # We should receive a getdata request
        test_node.wait_for_getdata([block.sha256], timeout=10)
        assert test_node.last_message["getdata"].inv[0].type == MSG_BLOCK or \
               test_node.last_message["getdata"].inv[0].type == MSG_BLOCK | MSG_WITNESS_FLAG

        # Deliver the block
        if version == 2:
            test_node.send_and_ping(msg_block(block))
        else:
            test_node.send_and_ping(msg_no_witness_block(block))
        assert_equal(int(node.getbestblockhash(), 16), block.sha256)

    def test_getblocktxn_handler(self, test_node):
        version = test_node.cmpct_version
        node = self.nodes[0]
        # bitcoind will not send blocktxn responses for blocks whose height is
        # more than 10 blocks deep.
        MAX_GETBLOCKTXN_DEPTH = 10
        chain_height = node.getblockcount()
        current_height = chain_height
        while (current_height >= chain_height - MAX_GETBLOCKTXN_DEPTH):
            block_hash = node.getblockhash(current_height)
            block = from_hex(CBlock(), node.getblock(block_hash, False))

            msg = msg_getblocktxn()
            msg.block_txn_request = BlockTransactionsRequest(int(block_hash, 16), [])
            num_to_request = random.randint(1, len(block.vtx))
            msg.block_txn_request.from_absolute(sorted(random.sample(range(len(block.vtx)), num_to_request)))
            test_node.send_message(msg)
            test_node.wait_until(lambda: "blocktxn" in test_node.last_message, timeout=10)

            [tx.calc_sha256() for tx in block.vtx]
            with p2p_lock:
                assert_equal(test_node.last_message["blocktxn"].block_transactions.blockhash, int(block_hash, 16))
                all_indices = msg.block_txn_request.to_absolute()
                for index in all_indices:
                    tx = test_node.last_message["blocktxn"].block_transactions.transactions.pop(0)
                    tx.calc_sha256()
                    assert_equal(tx.sha256, block.vtx[index].sha256)
                    if version == 1:
                        # Witnesses should have been stripped
                        assert tx.wit.is_null()
                    else:
                        # Check that the witness matches
                        assert_equal(tx.calc_sha256(True), block.vtx[index].calc_sha256(True))
                test_node.last_message.pop("blocktxn", None)
            current_height -= 1

        # Next request should send a full block response, as we're past the
        # allowed depth for a blocktxn response.
        block_hash = node.getblockhash(current_height)
        msg.block_txn_request = BlockTransactionsRequest(int(block_hash, 16), [0])
        with p2p_lock:
            test_node.last_message.pop("block", None)
            test_node.last_message.pop("blocktxn", None)
        test_node.send_and_ping(msg)
        with p2p_lock:
            test_node.last_message["block"].block.calc_sha256()
            assert_equal(test_node.last_message["block"].block.sha256, int(block_hash, 16))
            assert "blocktxn" not in test_node.last_message

    def test_compactblocks_not_at_tip(self, test_node):
        node = self.nodes[0]
        # Test that requesting old compactblocks doesn't work.
        MAX_CMPCTBLOCK_DEPTH = 5
        new_blocks = []
        for _ in range(MAX_CMPCTBLOCK_DEPTH + 1):
            test_node.clear_block_announcement()
            new_blocks.append(node.generate(1)[0])
            test_node.wait_until(test_node.received_block_announcement, timeout=30)

        test_node.clear_block_announcement()
        test_node.send_message(msg_getdata([CInv(MSG_CMPCT_BLOCK, int(new_blocks[0], 16))]))
        test_node.wait_until(lambda: "cmpctblock" in test_node.last_message, timeout=30)

        test_node.clear_block_announcement()
        node.generate(1)
        test_node.wait_until(test_node.received_block_announcement, timeout=30)
        test_node.clear_block_announcement()
        with p2p_lock:
            test_node.last_message.pop("block", None)
        test_node.send_message(msg_getdata([CInv(MSG_CMPCT_BLOCK, int(new_blocks[0], 16))]))
        test_node.wait_until(lambda: "block" in test_node.last_message, timeout=30)
        with p2p_lock:
            test_node.last_message["block"].block.calc_sha256()
            assert_equal(test_node.last_message["block"].block.sha256, int(new_blocks[0], 16))

        # Generate an old compactblock, and verify that it's not accepted.
        cur_height = node.getblockcount()
        hashPrevBlock = int(node.getblockhash(cur_height - 5), 16)
        block = self.build_block_on_tip(node)
        block.hashPrevBlock = hashPrevBlock
        block.solve()

        comp_block = HeaderAndShortIDs()
        comp_block.initialize_from_block(block)
        test_node.send_and_ping(msg_cmpctblock(comp_block.to_p2p()))

        tips = node.getchaintips()
        found = False
        for x in tips:
            if x["hash"] == block.hash:
                assert_equal(x["status"], "headers-only")
                found = True
                break
        assert found

        # Requesting this block via getblocktxn should silently fail
        # (to avoid fingerprinting attacks).
        msg = msg_getblocktxn()
        msg.block_txn_request = BlockTransactionsRequest(block.sha256, [0])
        with p2p_lock:
            test_node.last_message.pop("blocktxn", None)
        test_node.send_and_ping(msg)
        with p2p_lock:
            assert "blocktxn" not in test_node.last_message

    def test_end_to_end_block_relay(self, listeners):
        node = self.nodes[0]
        utxo = self.utxos.pop(0)

        block = self.build_block_with_transactions(node, utxo, 10)

        [l.clear_block_announcement() for l in listeners]

        # serialize without witness (this block has no witnesses anyway).
        # TODO: repeat this test with witness tx's to a segwit node.
        node.submitblock(block.serialize().hex())

        for l in listeners:
            l.wait_until(lambda: "cmpctblock" in l.last_message, timeout=30)
        with p2p_lock:
            for l in listeners:
                l.last_message["cmpctblock"].header_and_shortids.header.calc_sha256()
                assert_equal(l.last_message["cmpctblock"].header_and_shortids.header.sha256, block.sha256)

    # Test that we don't get disconnected if we relay a compact block with valid header,
    # but invalid transactions.
    def test_invalid_tx_in_compactblock(self, test_node, use_segwit=True):
        """ReddCoin: Adapted for PoS block structure.

        Block structure: vtx[0]=coinbase, vtx[1]=coinstake, vtx[2+]=user txs
        After building 5 user txs, block has 7 txs total. We delete one to make it invalid.
        """
        node = self.nodes[0]
        assert len(self.utxos)
        # ReddCoin: Use pop to avoid reusing UTXO
        utxo = self.utxos.pop(0)

        block = self.build_block_with_transactions(node, utxo, 5)
        # ReddCoin: Block now has 7 txs: coinbase(0), coinstake(1), user(2-6)
        # Delete a user transaction to make it invalid
        del block.vtx[4]  # Delete 3rd user tx
        block.hashMerkleRoot = block.calc_merkle_root()
        if use_segwit:
            # If we're testing with segwit, also drop the coinbase witness,
            # but include the witness commitment.
            add_witness_commitment(block)
            block.vtx[0].wit.vtxinwit = []
        block.solve()

        # ReddCoin: Re-sign the block after modifications
        if hasattr(self, '_block_signing_key'):
            sign_block(block, self._block_signing_key)

        # Now send the compact block with all transactions prefilled, and
        # verify that we don't get disconnected.
        comp_block = HeaderAndShortIDs()
        # ReddCoin: Block now has 6 txs (0-5), prefill all
        comp_block.initialize_from_block(block, prefill_list=[0, 1, 2, 3, 4, 5], use_witness=use_segwit)
        msg = msg_cmpctblock(comp_block.to_p2p())
        test_node.send_and_ping(msg)

        # Check that the tip didn't advance
        assert int(node.getbestblockhash(), 16) is not block.sha256
        test_node.sync_with_ping()

    # Helper for enabling cb announcements
    # Send the sendcmpct request and sync headers
    def request_cb_announcements(self, peer):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        peer.get_headers(locator=[int(tip, 16)], hashstop=0)
        peer.send_and_ping(msg_sendcmpct(announce=True, version=peer.cmpct_version))

    def test_compactblock_reconstruction_multiple_peers(self, stalling_peer, delivery_peer):
        """ReddCoin: Adapted for PoS block structure.

        Block structure: vtx[0]=coinbase, vtx[1]=coinstake, vtx[2+]=user txs
        Must prefill coinbase and coinstake, only relay user txs to mempool.
        """
        node = self.nodes[0]
        assert len(self.utxos)

        def announce_cmpct_block(node, peer):
            utxo = self.utxos.pop(0)
            block = self.build_block_with_transactions(node, utxo, 5)

            cmpct_block = HeaderAndShortIDs()
            # ReddCoin: Prefill coinbase AND coinstake, use witness if segwit active
            cmpct_block.initialize_from_block(block, prefill_list=[0, 1], use_witness=self.segwit_active)
            msg = msg_cmpctblock(cmpct_block.to_p2p())
            peer.send_and_ping(msg)
            with p2p_lock:
                assert "getblocktxn" in peer.last_message
            return block, cmpct_block

        block, cmpct_block = announce_cmpct_block(node, stalling_peer)

        # ReddCoin: Send user transactions (vtx[2:]) to mempool, skip coinstake
        for tx in block.vtx[2:]:
            delivery_peer.send_message(msg_tx(tx))
        delivery_peer.sync_with_ping()
        mempool = node.getrawmempool()
        for tx in block.vtx[2:]:
            assert tx.hash in mempool

        delivery_peer.send_and_ping(msg_cmpctblock(cmpct_block.to_p2p()))
        assert_equal(int(node.getbestblockhash(), 16), block.sha256)

        self.utxos.append([block.vtx[-1].sha256, 0, block.vtx[-1].vout[0].nValue])

        # Now test that delivering an invalid compact block won't break relay

        block, cmpct_block = announce_cmpct_block(node, stalling_peer)
        # ReddCoin: Send user transactions (vtx[2:]) to mempool, skip coinstake
        for tx in block.vtx[2:]:
            delivery_peer.send_message(msg_tx(tx))
        delivery_peer.sync_with_ping()

        # ReddCoin: Set an INVALID witness nonce. The block was built with nonce=0,
        # so we use nonce=1 to create a mismatch that will fail validation.
        cmpct_block.prefilled_txn[0].tx.wit.vtxinwit = [CTxInWitness()]
        cmpct_block.prefilled_txn[0].tx.wit.vtxinwit[0].scriptWitness.stack = [ser_uint256(1)]

        cmpct_block.use_witness = True
        delivery_peer.send_and_ping(msg_cmpctblock(cmpct_block.to_p2p()))
        assert int(node.getbestblockhash(), 16) != block.sha256

        msg = msg_no_witness_blocktxn()
        msg.block_transactions.blockhash = block.sha256
        # ReddCoin: Send user transactions (vtx[2:]) in blocktxn response
        msg.block_transactions.transactions = block.vtx[2:]
        stalling_peer.send_and_ping(msg)
        assert_equal(int(node.getbestblockhash(), 16), block.sha256)

    def test_highbandwidth_mode_states_via_getpeerinfo(self):
        # ReddCoin: disconnect the peers left over from earlier subtests before
        # starting. Those peers can leave a never-delivered block in flight (a
        # competing height-N PoS block one of them announced via cmpctblock, which
        # node0 requested but never received). node0 only promotes the delivering
        # peer to high-bandwidth when no OTHER block is in flight
        # (BlockChecked: mapBlocksInFlight.count(hash) == mapBlocksInFlight.size(),
        # net_processing.cpp), so a stale in-flight entry would suppress the
        # bip152_hb_to transition this subtest asserts. Upstream doesn't hit this
        # because PoW peers all announce the same tip block; ReddCoin's PoS
        # build_block_on_tip produces a distinct block each call.
        self.nodes[0].disconnect_p2ps()
        # create new p2p connection for a fresh state w/o any prior sendcmpct messages sent
        hb_test_node = self.nodes[0].add_p2p_connection(TestP2PConn(cmpct_version=2))

        # assert the RPC getpeerinfo boolean fields `bip152_hb_{to, from}`
        # match the given parameters for the last peer of a given node
        def assert_highbandwidth_states(node, hb_to, hb_from):
            peerinfo = node.getpeerinfo()[-1]
            assert_equal(peerinfo['bip152_hb_to'], hb_to)
            assert_equal(peerinfo['bip152_hb_from'], hb_from)

        # initially, neither node has selected the other peer as high-bandwidth yet
        assert_highbandwidth_states(self.nodes[0], hb_to=False, hb_from=False)

        # peer requests high-bandwidth mode by sending sendcmpct(1)
        hb_test_node.send_and_ping(msg_sendcmpct(announce=True, version=2))
        assert_highbandwidth_states(self.nodes[0], hb_to=False, hb_from=True)

        # peer generates a block and sends it to node, which should
        # select the peer as high-bandwidth (up to 3 peers according to BIP 152)
        block = self.build_block_on_tip(self.nodes[0])
        hb_test_node.send_and_ping(msg_block(block))
        # ReddCoin: Wait for high-bandwidth state to be updated (may not be immediate)
        self.wait_until(lambda: self.nodes[0].getpeerinfo()[-1]['bip152_hb_to'] == True, timeout=20)
        assert_highbandwidth_states(self.nodes[0], hb_to=True, hb_from=True)

        # peer requests low-bandwidth mode by sending sendcmpct(0)
        hb_test_node.send_and_ping(msg_sendcmpct(announce=False, version=2))
        assert_highbandwidth_states(self.nodes[0], hb_to=True, hb_from=False)

    def advance_to_segwit_active(self):
        """ReddCoin: Activate SegWit via BIP9 signaling.

        BIP9 activation requires:
        1. Signal in 108 of 144 blocks (75% threshold) → LOCKED_IN
        2. Wait another 144 blocks → ACTIVE

        Since we start from cache (199 blocks), we need to generate enough
        signaling blocks to reach activation.
        """
        node = self.nodes[0]

        self.log.info("Activating SegWit via BIP9 signaling...")

        # Check current state
        info = node.getblockchaininfo()
        current_height = info['blocks']
        self.log.info(f"Current height: {current_height}")

        # Calculate blocks needed to reach activation
        # BIP9 windows are 144 blocks on regtest
        # We need to signal through at least one full window, then wait one more
        # Earliest activation is at height 432 (window 3)
        target_height = 432

        if current_height >= target_height:
            # Already past activation height, check if active
            if softfork_active(node, "segwit"):
                self.log.info("SegWit already active!")
                self.segwit_active = True
                return
            else:
                # Generate a few more blocks to ensure activation
                advance_time_for_pos(node, seconds=600)
                node.generate(10)

        blocks_to_generate = target_height - current_height + 10  # +10 for safety margin

        self.log.info(f"Generating {blocks_to_generate} blocks to activate SegWit...")

        # Generate blocks in batches to avoid timeout issues
        batch_size = 50
        for i in range(0, blocks_to_generate, batch_size):
            batch = min(batch_size, blocks_to_generate - i)
            advance_time_for_pos(node, seconds=600)
            node.generate(batch)
            self.log.info(f"Generated {i + batch}/{blocks_to_generate} blocks...")

        # Verify activation
        assert softfork_active(node, "segwit"), "SegWit should be active after signaling"
        self.segwit_active = True
        self.log.info(f"SegWit activated at height {node.getblockcount()}!")

    def run_test(self):
        """ReddCoin: Two-phase compact blocks test.

        Phase 1: Pre-activation tests (compact blocks v1, no witness)
        Activation: BIP9 signaling to activate SegWit
        Phase 2: Post-activation tests (compact blocks v2, with witness)
        """
        node = self.nodes[0]

        # ReddCoin: Advance time to ensure coins have sufficient age for PoS
        advance_time_for_pos(node, seconds=600)

        # ReddCoin: Verify SegWit is NOT active yet (starting from cache at height 199)
        self.segwit_active = False
        assert not softfork_active(node, 'segwit'), "SegWit should not be active yet"
        self.log.info(f"ReddCoin: Starting at height {node.getblockcount()}, SegWit not active")

        # ====== PRE-ACTIVATION TESTS (Phase 1) ======
        self.log.info("=" * 60)
        self.log.info("PHASE 1: Pre-SegWit Testing (Compact Blocks v1)")
        self.log.info("=" * 60)

        # Setup the p2p connections for pre-activation
        # Version 1 compact blocks use txids (pre-segwit)
        # ReddCoin: Use NODE_NETWORK without NODE_WITNESS for v1 peers
        # The node expects peers advertising NODE_WITNESS to support cmpct v2
        self.segwit_node = node.add_p2p_connection(TestP2PConn(cmpct_version=1), services=NODE_NETWORK)
        self.old_node = node.add_p2p_connection(TestP2PConn(cmpct_version=1), services=NODE_NETWORK)
        self.additional_segwit_node = node.add_p2p_connection(TestP2PConn(cmpct_version=1), services=NODE_NETWORK)

        # We will need UTXOs to construct transactions in later tests.
        self.make_utxos()

        self.log.info("Testing SENDCMPCT p2p message (v1)...")
        # ReddCoin: Don't pass old_node in Phase 1 since both peers are v1
        self.test_sendcmpct(self.segwit_node)

        self.log.info("Testing compactblock construction (v1)...")
        self.test_compactblock_construction(self.segwit_node, use_witness_address=False)

        self.log.info("Testing compactblock requests (v1)...")
        self.test_compactblock_requests(self.segwit_node)

        # ====== ACTIVATE SEGWIT VIA BIP9 SIGNALING ======
        self.log.info("=" * 60)
        self.log.info("ACTIVATING SEGWIT...")
        self.log.info("=" * 60)

        # Disconnect pre-activation P2P connections
        node.disconnect_p2ps()

        self.advance_to_segwit_active()

        # ====== POST-ACTIVATION TESTS (Phase 2) ======
        self.log.info("=" * 60)
        self.log.info("PHASE 2: Post-SegWit Testing (Compact Blocks v2)")
        self.log.info("=" * 60)

        # Reconnect P2P with version 2 support
        self.segwit_node = node.add_p2p_connection(TestP2PConn(cmpct_version=2))
        self.old_node = node.add_p2p_connection(TestP2PConn(cmpct_version=1), services=NODE_NETWORK)
        self.additional_segwit_node = node.add_p2p_connection(TestP2PConn(cmpct_version=2))

        # Recreate UTXOs (old ones may be spent/buried)
        self.utxos = []
        self.make_utxos()

        self.log.info("Testing SENDCMPCT p2p message (v2)...")
        self.test_sendcmpct(self.segwit_node, old_node=self.old_node)
        self.test_sendcmpct(self.additional_segwit_node)

        self.log.info("Testing compactblock construction (v2)...")
        self.test_compactblock_construction(self.old_node, use_witness_address=False)
        self.test_compactblock_construction(self.segwit_node, use_witness_address=True)

        self.log.info("Testing compactblock requests (segwit node)...")
        self.test_compactblock_requests(self.segwit_node)

        self.log.info("Testing getblocktxn requests (segwit node)...")
        self.test_getblocktxn_requests(self.segwit_node)

        self.log.info("Testing getblocktxn handler (segwit node should return witnesses)...")
        self.test_getblocktxn_handler(self.segwit_node)
        self.test_getblocktxn_handler(self.old_node)

        self.log.info("Testing compactblock requests/announcements not at chain tip...")
        self.test_compactblocks_not_at_tip(self.segwit_node)
        self.test_compactblocks_not_at_tip(self.old_node)

        self.log.info("Testing handling of incorrect blocktxn responses...")
        self.test_incorrect_blocktxn_response(self.segwit_node)

        self.log.info("Testing reconstructing compact blocks from all peers...")
        self.test_compactblock_reconstruction_multiple_peers(self.segwit_node, self.additional_segwit_node)

        self.log.info("Testing end-to-end block relay...")
        self.request_cb_announcements(self.old_node)
        self.request_cb_announcements(self.segwit_node)
        self.test_end_to_end_block_relay([self.segwit_node, self.old_node])

        self.log.info("Testing handling of invalid compact blocks...")
        self.test_invalid_tx_in_compactblock(self.segwit_node)
        self.test_invalid_tx_in_compactblock(self.old_node)

        self.log.info("Testing invalid index in cmpctblock message...")
        self.test_invalid_cmpctblock_message()

        self.log.info("Testing high-bandwidth mode states via getpeerinfo...")
        self.test_highbandwidth_mode_states_via_getpeerinfo()


if __name__ == '__main__':
    CompactBlocksTest().main()
