#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP66 (DER SIG).

Test that the DERSIG soft-fork activates at (regtest) block height 500.
ReddCoin uses BIP66Height=500 as a buried deployment.
"""

from test_framework.blocktools import (
    create_block,
    NORMAL_GBT_REQUEST_PARAMS,
    sign_block,
)
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
)
from test_framework.p2p import P2PInterface
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)

DERSIG_HEIGHT = 500


# A canonical signature consists of:
# <30> <total len> <02> <len R> <R> <02> <len S> <S> <hashtype>
def unDERify(tx):
    """
    Make the signature in vin 0 of a tx non-DER-compliant,
    by adding padding after the S-value.
    """
    scriptSig = CScript(tx.vin[0].scriptSig)
    newscript = []
    for i in scriptSig:
        if (len(newscript) == 0):
            newscript.append(i[0:-1] + b'\0' + i[-1:])
        else:
            newscript.append(i)
    tx.vin[0].scriptSig = CScript(newscript)


class BIP66Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [[
            '-whitelist=noban@127.0.0.1',
            '-par=1',  # Use only one script thread to get the exact log msg for testing
            '-staking=0',  # Prevent background staking from consuming UTXOs
        ]]
        self.setup_clean_chain = True
        self.rpc_timeout = 240

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def create_signed_spend(self, node, utxo_txid, utxo_vout, utxo_amount):
        """Create a signed P2PKH spend (with real DER signature for unDERify testing)."""
        fee = 0.01
        addr = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo_txid, "vout": utxo_vout}],
            [{addr: round(utxo_amount - fee, 8)}]
        )
        signed = node.signrawtransactionwithwallet(raw)
        assert signed['complete']
        return tx_from_hex(signed['hex'])

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

    def build_block_with_tx(self, node, tx):
        """Build a PoS block containing the given transaction via GBT."""
        advance_time_for_pos(node, seconds=120)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        block = create_block(tmpl=tmpl, txlist=[tx])
        block.hashMerkleRoot = block.calc_merkle_root()
        self.sign_block_with_coinstake_key(node, block)
        return block

    def test_dersig_info(self, *, is_active):
        assert_equal(self.nodes[0].getblockchaininfo()['softforks']['bip66'],
            {
                "active": is_active,
                "height": DERSIG_HEIGHT,
                "type": "buried",
            },
        )

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PInterface())

        self.test_dersig_info(is_active=False)

        self.log.info("Mining %d blocks", DERSIG_HEIGHT - 3)
        node.generate(DERSIG_HEIGHT - 3)

        # Create P2PKH UTXOs for DERSIG testing (need real signatures for unDERify)
        utxo_amount = 10.0
        test_utxo_txids = []
        for _ in range(4):
            txid = node.sendtoaddress(node.getnewaddress(), utxo_amount)
            test_utxo_txids.append(txid)
        node.generate(1)  # Confirm — height DERSIG_HEIGHT - 2
        assert_equal(node.getblockcount(), DERSIG_HEIGHT - 2)

        self.log.info("Test that a transaction with non-DER signature can still appear in a block")

        spendtx = self.create_signed_spend(node, test_utxo_txids[0], 0, utxo_amount)
        unDERify(spendtx)
        spendtx.rehash()

        # Build a PoS block with the non-DER tx via submitblock (bypasses mempool policy)
        # Use build_block_with_tx which creates a proper PoS block from GBT
        block = self.build_block_with_tx(node, spendtx)

        self.test_dersig_info(is_active=False)
        result = node.submitblock(block.serialize().hex())
        assert_equal(result, None)  # Accepted
        assert_equal(node.getblockcount(), DERSIG_HEIGHT - 1)  # height 499
        # At tip 499: DeploymentActiveAfter = (499+1 >= 500) = True
        self.test_dersig_info(is_active=True)

        self.log.info("Test that blocks must now be at least version 4")
        # ReddCoin: nVersion=2 is PoW (POW_BLOCK_VERSION=2), nVersion=3 is PoS but below DERSIG threshold (4).
        # Build a version 3 PoS block — should be rejected with bad-version
        advance_time_for_pos(node, seconds=120)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        block_v3 = create_block(tmpl=tmpl)
        block_v3.nVersion = 3
        block_v3.hashMerkleRoot = block_v3.calc_merkle_root()
        self.sign_block_with_coinstake_key(node, block_v3)

        with node.assert_debug_log(expected_msgs=['bad-version(0x00000003)']):
            node.submitblock(block_v3.serialize().hex())
            assert_equal(node.getblockcount(), DERSIG_HEIGHT - 1)  # Tip didn't advance

        self.log.info("Test that transactions with non-DER signatures cannot appear in a block")

        spendtx = self.create_signed_spend(node, test_utxo_txids[1], 0, utxo_amount)
        unDERify(spendtx)
        spendtx.rehash()

        # First we show that this tx is valid except for DERSIG by getting it
        # rejected from the mempool for exactly that reason.
        assert_equal(
            [{
                'txid': spendtx.hash,
                'wtxid': spendtx.getwtxid(),
                'allowed': False,
                'reject-reason': 'non-mandatory-script-verify-flag (Non-canonical DER signature)',
            }],
            node.testmempoolaccept(rawtxs=[spendtx.serialize().hex()], maxfeerate=0),
        )

        # Now we verify that a block with this transaction is also invalid.
        block = self.build_block_with_tx(node, spendtx)
        prev_best = node.getbestblockhash()

        with node.assert_debug_log(expected_msgs=['CheckInputScripts on {} failed with non-mandatory-script-verify-flag (Non-canonical DER signature)'.format(spendtx.hash)]):
            node.submitblock(block.serialize().hex())
            assert_equal(node.getbestblockhash(), prev_best)  # Tip didn't advance

        self.log.info("Test that a version 3 block with a DERSIG-compliant transaction is accepted")
        spendtx = self.create_signed_spend(node, test_utxo_txids[2], 0, utxo_amount)
        # This tx has a proper DER signature from signrawtransactionwithwallet

        block = self.build_block_with_tx(node, spendtx)
        prev_best = node.getbestblockhash()

        self.test_dersig_info(is_active=True)
        result = node.submitblock(block.serialize().hex())
        assert_equal(result, None)  # Accepted
        self.test_dersig_info(is_active=True)
        assert node.getbestblockhash() != prev_best


if __name__ == '__main__':
    BIP66Test().main()
