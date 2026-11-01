#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test spending coinbase and coinstake transactions.

The coinbase transaction in block N can appear in block
N+COINBASE_MATURITY... so is valid in the mempool when the best block
height is N+COINBASE_MATURITY-1.
This test makes sure coinbase/coinstake spends that will be mature
in the next block are accepted into the memory pool,
but less mature spends are NOT.

Tests both PoW coinbase maturity and PoS coinstake maturity.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class MempoolSpendCoinbaseTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        # Helper: create a raw spend of a UTXO, return {hex, txid}
        fee = Decimal("0.01")

        def create_spend(utxo):
            addr = node.getnewaddress()
            raw = node.createrawtransaction(
                [{"txid": utxo['txid'], "vout": utxo['vout']}],
                {addr: utxo['value'] - fee}
            )
            # Provide prevtx scriptPubKey so the wallet signs P2PK outputs correctly
            prevtxs = []
            if 'scriptPubKey' in utxo:
                prevtxs = [{"txid": utxo['txid'], "vout": utxo['vout'],
                            "scriptPubKey": utxo['scriptPubKey'], "amount": utxo['value']}]
            signed = node.signrawtransactionwithwallet(raw, prevtxs)
            txid = node.decoderawtransaction(signed['hex'])['txid']
            return {'hex': signed['hex'], 'txid': txid}

        # Helper: get coinbase UTXO from a PoW block
        def get_coinbase_utxo(height):
            bh = node.getblockhash(height)
            block = node.getblock(bh, 2)
            cb_tx = block['tx'][0]
            for vout in cb_tx['vout']:
                if vout['value'] > 0 and node.gettxout(cb_tx['txid'], vout['n']):
                    return {'txid': cb_tx['txid'], 'vout': vout['n'], 'value': vout['value']}
            return None

        # Helper: get coinstake UTXO from a PoS block (tx[1], non-zero output)
        def get_coinstake_utxo(height):
            bh = node.getblockhash(height)
            block = node.getblock(bh, 2)
            if len(block['tx']) < 2:
                return None
            cs_tx = block['tx'][1]
            for vout in cs_tx['vout']:
                if vout['value'] > 0 and node.gettxout(cs_tx['txid'], vout['n']):
                    return {'txid': cs_tx['txid'], 'vout': vout['n'], 'value': vout['value'],
                            'scriptPubKey': vout['scriptPubKey']['hex']}
            return None

        self.test_pow_coinbase_maturity(node, create_spend, get_coinbase_utxo)
        self.test_pos_coinstake_maturity(node, create_spend, get_coinstake_utxo)

    def test_pow_coinbase_maturity(self, node, create_spend, get_coinbase_utxo):
        """Test that PoW coinbase maturity is enforced at the mempool level."""
        self.log.info("=== PoW coinbase maturity test ===")

        # Find two consecutive unspent PoW coinbase UTXOs.
        # PoS coinstake consumes some coinbase UTXOs during cache generation.
        mature_block = None
        for h in range(70, 89):
            u1 = get_coinbase_utxo(h)
            u2 = get_coinbase_utxo(h + 1)
            if u1 and u2:
                mature_block = h
                immature_block = h + 1
                utxo_mature = u1
                utxo_immature = u2
        assert mature_block is not None, "Need two consecutive unspent PoW coinbase UTXOs"
        self.log.info("Using PoW coinbase UTXOs from blocks %d (mature) and %d (immature)" %
                      (mature_block, immature_block))

        # Invalidate blocks so that the mature coinbase is just barely
        # spendable in the mempool (will mature in the next block) and the
        # immature one is not yet spendable.
        # Mempool accepts coinbase spend when tip >= coinbase_height + COINBASE_MATURITY - 1
        chain_height = mature_block + COINBASE_MATURITY - 1
        node.invalidateblock(node.getblockhash(chain_height + 1))
        assert_equal(chain_height, node.getblockcount())

        # Coinbase at mature_block — ok in mempool
        spend_mature = create_spend(utxo_mature)
        spend_mature_id = node.sendrawtransaction(spend_mature['hex'])

        # Coinbase at immature_block — too immature
        immature_tx = create_spend(utxo_immature)
        assert_raises_rpc_error(-26,
                                "bad-txns-premature-spend-of-coinbase",
                                lambda: node.sendrawtransaction(immature_tx['hex']))

        # mempool should have just the mature one
        assert_equal(node.getrawmempool(), [spend_mature_id])

        # mine a block, mature one should get confirmed
        node.generate(1)
        assert_equal(set(node.getrawmempool()), set())

        # ... and now previously immature can be spent:
        spend_new_id = node.sendrawtransaction(immature_tx['hex'])
        assert_equal(node.getrawmempool(), [spend_new_id])

        self.log.info("PoW coinbase maturity test PASSED")

    def test_pos_coinstake_maturity(self, node, create_spend, get_coinstake_utxo):
        """Test that PoS coinstake maturity is enforced at the mempool level."""
        self.log.info("=== PoS coinstake maturity test ===")

        # Generate enough PoS blocks so some coinstake outputs reach maturity.
        # We need COINBASE_MATURITY + a few extra blocks.
        num_blocks = COINBASE_MATURITY + 5
        pos_start = node.getblockcount() + 1
        self.log.info("Generating %d PoS blocks (from height %d)..." % (num_blocks, pos_start))
        node.generate(num_blocks)
        current_height = node.getblockcount()
        self.log.info("Chain at height %d" % current_height)

        # Pick two consecutive coinstake blocks near the maturity boundary.
        # We'll invalidate to the boundary FIRST, then find unspent vout 1
        # (the staker's output). Before invalidation, vout 1 is consumed by
        # later staking; after invalidation those consuming blocks are removed,
        # making vout 1 unspent again.
        cs_mature_block = pos_start
        cs_immature_block = pos_start + 1

        # Invalidate to coinstake maturity boundary
        cs_chain_height = cs_mature_block + COINBASE_MATURITY - 1
        self.log.info("Invalidating to height %d (maturity boundary for block %d)" %
                      (cs_chain_height, cs_mature_block))
        node.invalidateblock(node.getblockhash(cs_chain_height + 1))
        assert_equal(cs_chain_height, node.getblockcount())

        # Now find unspent coinstake UTXOs (vout 1 should be unspent after invalidation)
        cs_utxo_mature = get_coinstake_utxo(cs_mature_block)
        cs_utxo_immature = get_coinstake_utxo(cs_immature_block)
        assert cs_utxo_mature is not None, "Need unspent coinstake UTXO at block %d" % cs_mature_block
        assert cs_utxo_immature is not None, "Need unspent coinstake UTXO at block %d" % cs_immature_block
        self.log.info("Using PoS coinstake UTXOs from blocks %d (mature, vout %d) and %d (immature, vout %d)" %
                      (cs_mature_block, cs_utxo_mature['vout'],
                       cs_immature_block, cs_utxo_immature['vout']))

        # Coinstake at cs_mature_block — ok in mempool
        cs_spend_mature = create_spend(cs_utxo_mature)
        cs_spend_mature_id = node.sendrawtransaction(cs_spend_mature['hex'])

        # Coinstake at cs_immature_block — too immature
        cs_immature_tx = create_spend(cs_utxo_immature)
        assert_raises_rpc_error(-26,
                                "bad-txns-premature-spend-of-coinbase",
                                lambda: node.sendrawtransaction(cs_immature_tx['hex']))

        # mempool should have just the mature coinstake spend
        assert_equal(node.getrawmempool(), [cs_spend_mature_id])

        # mine a block, mature one should get confirmed
        node.generate(1)
        assert_equal(set(node.getrawmempool()), set())

        # ... and now previously immature coinstake can be spent:
        cs_spend_new_id = node.sendrawtransaction(cs_immature_tx['hex'])
        assert_equal(node.getrawmempool(), [cs_spend_new_id])

        self.log.info("PoS coinstake maturity test PASSED")


if __name__ == '__main__':
    MempoolSpendCoinbaseTest().main()
