#!/usr/bin/env python3
# Copyright (c) 2016-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test NULLDUMMY softfork.

Connect to a single node.
Generate 2 blocks (save the coinbases for later).
Generate COINBASE_MATURITY (CB) more blocks to ensure the coinbases are mature.
[Policy/Consensus] Check that NULLDUMMY compliant transactions are accepted in block CB + 3.
[Policy] Check that non-NULLDUMMY transactions are rejected before activation.
[Consensus] Check that the new NULLDUMMY rules are not enforced on block CB + 4.
[Policy/Consensus] Check that the new NULLDUMMY rules are enforced on block CB + 5.
"""
import time

from test_framework.blocktools import (
    COINBASE_MATURITY,
    NORMAL_GBT_REQUEST_PARAMS,
    add_witness_commitment,
    create_block,
    create_transaction,
    sign_block,
)
from test_framework.messages import CTransaction
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_equal,
    assert_raises_rpc_error,
)

NULLDUMMY_ERROR = "non-mandatory-script-verify-flag (Dummy CHECKMULTISIG argument must be zero)"


def trueDummy(tx):
    scriptSig = CScript(tx.vin[0].scriptSig)
    newscript = []
    for i in scriptSig:
        if len(newscript) == 0:
            assert len(i) == 0
            newscript.append(b'\x51')
        else:
            newscript.append(i)
    tx.vin[0].scriptSig = CScript(newscript)
    tx.rehash()


class NULLDUMMYTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # This script tests NULLDUMMY activation, which is part of the 'segwit' deployment, so we go through
        # normal segwit activation here (and don't use the default always-on behaviour).
        # ReddCoin: SegWit activates at height 432 via BIP9 (no -segwitheight param).
        # NULLDUMMY is enforced as part of SegWit activation.
        self.extra_args = [[
            '-addresstype=legacy',
            '-staking=0',
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.nodes[0].createwallet(wallet_name='wmulti', disable_private_keys=True)
        wmulti = self.nodes[0].get_wallet_rpc('wmulti')
        w0 = self.nodes[0].get_wallet_rpc(self.default_wallet_name)
        self.address = w0.getnewaddress()
        self.pubkey = w0.getaddressinfo(self.address)['pubkey']
        self.ms_address = wmulti.addmultisigaddress(1, [self.pubkey])['address']
        self.wit_address = w0.getnewaddress(address_type='p2sh-segwit')
        self.wit_ms_address = wmulti.addmultisigaddress(1, [self.pubkey], '', 'p2sh-segwit')['address']
        if not self.options.descriptors:
            # Legacy wallets need to import these so that they are watched by the wallet. This is unnecessary (and does not need to be tested) for descriptor wallets
            wmulti.importaddress(self.ms_address)
            wmulti.importaddress(self.wit_ms_address)

        # ReddCoin: SegWit (and NULLDUMMY) activates at height 432 via BIP9.
        # Create test UTXOs early, mature them, then reach activation height.
        SEGWIT_HEIGHT = 432
        self.log.info("Generating blocks and creating test UTXOs")
        # Generate to height where UTXOs will mature BEFORE activation
        self.nodes[0].generate(SEGWIT_HEIGHT - COINBASE_MATURITY - 5)  # height = 367

        # Create test UTXOs via createrawtransaction (ensure output at vout 0)
        coinbase_txid = []
        for _ in range(2):
            raw = w0.createrawtransaction([], [{self.address: 50}])
            funded = w0.fundrawtransaction(raw, {"changePosition": 1})
            signed = w0.signrawtransactionwithwallet(funded['hex'])
            coinbase_txid.append(w0.sendrawtransaction(signed['hex']))
        self.nodes[0].generate(1)  # height = 368, confirm the UTXOs
        # ReddCoin: the height-advancing generate() below produces PoS blocks
        # whose coinstakes select wallet coins via AvailableCoins. Lock the test
        # UTXOs (vout 0) so they aren't staked/spent before the tests use them;
        # AvailableCoins skips locked coins, and a raw tx can still spend them.
        for txid in coinbase_txid:
            w0.lockunspent(False, [{"txid": txid, "vout": 0}])
        self.nodes[0].generate(COINBASE_MATURITY)  # height = 428, UTXOs now mature
        # Now at height 428 — 4 blocks before SegWit activation at 432
        self.lastblockhash = self.nodes[0].getbestblockhash()
        self.lastblockheight = self.nodes[0].getblockcount()
        self.lastblocktime = self.nodes[0].getblockheader(self.lastblockhash)['time'] + 1

        self.log.info(f"Test 1: NULLDUMMY compliant base transactions should be accepted to mempool and mined before activation [{self.lastblockheight + 1}]")
        test1txs = [create_transaction(self.nodes[0], coinbase_txid[0], self.ms_address, amount=49)]
        txid1 = self.nodes[0].sendrawtransaction(test1txs[0].serialize_with_witness().hex(), 0)
        test1txs.append(create_transaction(self.nodes[0], txid1, self.ms_address, amount=48))
        txid2 = self.nodes[0].sendrawtransaction(test1txs[1].serialize_with_witness().hex(), 0)
        test1txs.append(create_transaction(self.nodes[0], coinbase_txid[1], self.wit_ms_address, amount=49))
        txid3 = self.nodes[0].sendrawtransaction(test1txs[2].serialize_with_witness().hex(), 0)
        self.block_submit(self.nodes[0], test1txs, False, True)

        self.log.info("Test 2: Non-NULLDUMMY base multisig transaction should not be accepted to mempool before activation")
        test2tx = create_transaction(self.nodes[0], txid2, self.ms_address, amount=47)
        trueDummy(test2tx)
        assert_raises_rpc_error(-26, NULLDUMMY_ERROR, self.nodes[0].sendrawtransaction, test2tx.serialize_with_witness().hex(), 0)

        self.log.info(f"Test 3: Non-NULLDUMMY base transactions should be accepted in a block before activation [{COINBASE_MATURITY + 4}]")
        self.block_submit(self.nodes[0], [test2tx], False, True)

        # ReddCoin: Advance past SegWit activation (height 432) for post-activation tests
        while self.lastblockheight < SEGWIT_HEIGHT:
            self.nodes[0].generate(1)
            self.lastblockheight = self.nodes[0].getblockcount()
            self.lastblockhash = self.nodes[0].getbestblockhash()
            self.lastblocktime = self.nodes[0].getblockheader(self.lastblockhash)['time'] + 1

        self.log.info("Test 4: Non-NULLDUMMY base multisig transaction is invalid after activation")
        test4tx = create_transaction(self.nodes[0], test2tx.hash, self.address, amount=46)
        test6txs = [CTransaction(test4tx)]
        trueDummy(test4tx)
        assert_raises_rpc_error(-26, NULLDUMMY_ERROR, self.nodes[0].sendrawtransaction, test4tx.serialize_with_witness().hex(), 0)
        self.block_submit(self.nodes[0], [test4tx])

        self.log.info("Test 5: Non-NULLDUMMY P2WSH multisig transaction invalid after activation")
        test5tx = create_transaction(self.nodes[0], txid3, self.wit_address, amount=48)
        test6txs.append(CTransaction(test5tx))
        test5tx.wit.vtxinwit[0].scriptWitness.stack[0] = b'\x01'
        assert_raises_rpc_error(-26, NULLDUMMY_ERROR, self.nodes[0].sendrawtransaction, test5tx.serialize_with_witness().hex(), 0)
        self.block_submit(self.nodes[0], [test5tx], True)

        self.log.info(f"Test 6: NULLDUMMY compliant base/witness transactions should be accepted to mempool and in block after activation [{COINBASE_MATURITY + 5}]")
        for i in test6txs:
            self.nodes[0].sendrawtransaction(i.serialize_with_witness().hex(), 0)
        self.block_submit(self.nodes[0], test6txs, True, True)

    def sign_block_with_coinstake_key(self, node, block):
        """Sign a PoS block using the coinstake's private key."""
        coinstake = block.vtx[1]
        decoded = node.decoderawtransaction(coinstake.serialize().hex())
        signing_key = None
        try:
            script_pubkey = decoded['vout'][1]['scriptPubKey']
            addr = script_pubkey.get('address')
            if not addr:
                addresses = script_pubkey.get('addresses', [])
                if addresses:
                    addr = addresses[0]
            if addr:
                signing_key = node.dumpprivkey(addr)
        except Exception:
            pass
        if not signing_key:
            signing_key = node.get_deterministic_priv_key().key
        sign_block(block, signing_key)

    def block_submit(self, node, txs, witness=False, accept=False):
        advance_time_for_pos(node, seconds=120)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal(tmpl['previousblockhash'], self.lastblockhash)
        assert_equal(tmpl['height'], self.lastblockheight + 1)
        # Create block with only coinstake from GBT (exclude mempool txs to avoid duplicates
        # since we manually append the test txs below)
        # Filter template to only keep coinstake (first tx)
        if len(tmpl['transactions']) > 1:
            tmpl['transactions'] = [tmpl['transactions'][0]]  # keep only coinstake
        block = create_block(tmpl=tmpl)
        for tx in txs:
            tx.rehash()
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        witness and add_witness_commitment(block)
        block.rehash()
        self.sign_block_with_coinstake_key(node, block)
        assert_equal(None if accept else 'block-validation-failed', node.submitblock(block.serialize().hex()))
        if (accept):
            assert_equal(node.getbestblockhash(), block.hash)
            self.lastblockhash = block.hash
            self.lastblockheight += 1
            self.lastblocktime = node.getblockheader(self.lastblockhash)['time'] + 1
        else:
            assert_equal(node.getbestblockhash(), self.lastblockhash)


if __name__ == '__main__':
    NULLDUMMYTest().main()
