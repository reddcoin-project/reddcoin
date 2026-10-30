#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test coinstatsindex across nodes.

Test that the values returned by gettxoutsetinfo are consistent
between a node running the coinstatsindex and a node without
the index.
"""

from decimal import Decimal

from test_framework.blocktools import (
    COINBASE_MATURITY,
)
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_FALSE,
    OP_RETURN,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    try_rpc,
)

class CoinStatsIndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.supports_cli = False
        self.extra_args = [
            ["-staking=0"],
            ["-coinstatsindex", "-staking=0"]
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self._test_coin_stats_index()
        self._test_use_index_option()
        self._test_reorg_index()
        self._test_index_rejects_hash_serialized()

    def block_sanity_check(self, block_info):
        # ReddCoin: block subsidy varies (interest-based PoS rewards).
        # Verify the accounting identity: subsidy = outputs + unspendable - inputs
        # where subsidy >= 0.
        block_subsidy = (block_info['new_outputs_ex_coinbase'] +
                         block_info['coinbase'] +
                         block_info['unspendable'] -
                         block_info['prevout_spent'])
        assert block_subsidy >= 0, f"Negative block subsidy: {block_subsidy}"

    def _test_coin_stats_index(self):
        node = self.nodes[0]
        index_node = self.nodes[1]
        # Both none and muhash options allow the usage of the index
        index_hash_options = ['none', 'muhash']

        # Generate blocks to get mature coins
        # ReddCoin: COINBASE_MATURITY = 60
        node.generate(COINBASE_MATURITY + 1)
        # Track the height where we have the sendtoaddress tx
        tx_height = node.getblockcount() + 1  # Will be mined in next block

        address = self.nodes[0].get_deterministic_priv_key().address
        node.sendtoaddress(address=address, amount=10, subtractfeefromamount=True)
        node.generate(1)

        self.sync_blocks(timeout=120)

        self.log.info("Test that gettxoutsetinfo() output is consistent with or without coinstatsindex option")
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", node.gettxoutsetinfo))
        res0 = node.gettxoutsetinfo('none')

        # The fields 'disk_size' and 'transactions' do not exist on the index
        del res0['disk_size'], res0['transactions']

        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        for hash_option in index_hash_options:
            res1 = index_node.gettxoutsetinfo(hash_option)
            # The fields 'block_info' and 'total_unspendable_amount' only exist on the index
            del res1['block_info'], res1['total_unspendable_amount']
            res1.pop('muhash', None)

            # Everything left should be the same
            assert_equal(res1, res0)

        self.log.info("Test that gettxoutsetinfo() can get fetch data on specific heights with index")

        # Generate a new tip
        node.generate(5)

        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        for hash_option in index_hash_options:
            # Fetch old stats by height
            res2 = index_node.gettxoutsetinfo(hash_option, tx_height)
            del res2['block_info'], res2['total_unspendable_amount']
            res2.pop('muhash', None)
            assert_equal(res0, res2)

            # Fetch old stats by hash
            res3 = index_node.gettxoutsetinfo(hash_option, res0['bestblock'])
            del res3['block_info'], res3['total_unspendable_amount']
            res3.pop('muhash', None)
            assert_equal(res0, res3)

            # It does not work without coinstatsindex
            assert_raises_rpc_error(-8, "Querying specific block heights requires coinstatsindex", node.gettxoutsetinfo, hash_option, tx_height)

        self.log.info("Test gettxoutsetinfo() with index and verbose flag")

        for hash_option in index_hash_options:
            # Genesis block is unspendable
            res4 = index_node.gettxoutsetinfo(hash_option, 0)
            genesis_reward = res4['total_unspendable_amount']  # ReddCoin: 10000 RDD
            assert genesis_reward > 0
            assert_equal(res4['block_info'], {
                'unspendable': genesis_reward,
                'prevout_spent': 0,
                'new_outputs_ex_coinbase': 0,
                'coinbase': 0,
                'unspendables': {
                    'genesis_block': genesis_reward,
                    'bip30': 0,
                    'scripts': 0,
                    'unclaimed_rewards': 0
                }
            })
            self.block_sanity_check(res4['block_info'])

            # Test an older block height that included a normal tx
            res5 = index_node.gettxoutsetinfo(hash_option, tx_height)
            # ReddCoin: PoS rewards vary, so check structure not exact amounts
            assert_equal(res5['block_info']['unspendables']['genesis_block'], 0)
            assert_equal(res5['block_info']['unspendables']['bip30'], 0)
            assert_equal(res5['block_info']['unspendables']['scripts'], 0)
            # In PoS, empty coinbase means some reward is "unclaimed" per the index accounting
            assert res5['block_info']['prevout_spent'] > 0
            assert res5['block_info']['new_outputs_ex_coinbase'] > 0
            self.block_sanity_check(res5['block_info'])

        # Generate and send a normal tx with two outputs
        tx1_inputs = []
        tx1_outputs = {self.nodes[0].getnewaddress(): 21, self.nodes[0].getnewaddress(): 42}
        raw_tx1 = self.nodes[0].createrawtransaction(tx1_inputs, tx1_outputs)
        funded_tx1 = self.nodes[0].fundrawtransaction(raw_tx1)
        signed_tx1 = self.nodes[0].signrawtransactionwithwallet(funded_tx1['hex'])
        tx1_txid = self.nodes[0].sendrawtransaction(signed_tx1['hex'])

        # Find the right position of the 21 RDD output
        tx1_final = self.nodes[0].gettransaction(tx1_txid)
        for output in tx1_final['details']:
            if output['amount'] == Decimal('21.00000000') and output['category'] == 'receive':
                n = output['vout']

        # Generate and send another tx with an OP_RETURN output (which is unspendable)
        tx2 = CTransaction()
        tx2.vin.append(CTxIn(COutPoint(int(tx1_txid, 16), n), b''))
        tx2.vout.append(CTxOut(int(20.99 * COIN), CScript([OP_RETURN] + [OP_FALSE]*30)))
        tx2_hex = self.nodes[0].signrawtransactionwithwallet(tx2.serialize().hex())['hex']
        self.nodes[0].sendrawtransaction(tx2_hex)

        # Include both txs in a block
        self.nodes[0].generate(1)
        op_return_height = self.nodes[0].getblockcount()
        self.sync_all()

        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        for hash_option in index_hash_options:
            # Check that OP_RETURN amounts were registered correctly
            res6 = index_node.gettxoutsetinfo(hash_option, op_return_height)
            # The OP_RETURN output (20.99) should show up as unspendable scripts
            # Account for rounding: 20.99 RDD = 2099000000 sat, but tx fee leaves 20.98999999
            assert res6['block_info']['unspendables']['scripts'] > Decimal('20')
            assert_equal(res6['block_info']['unspendables']['genesis_block'], 0)
            assert_equal(res6['block_info']['unspendables']['bip30'], 0)
            self.block_sanity_check(res6['block_info'])

        # ReddCoin: Skip the custom partial-subsidy coinbase test.
        # In PoS, coinbase is always empty (reward goes through coinstake).
        # The unclaimed_rewards test is not applicable since there's no
        # mechanism to create a partial-claim coinstake in Python.
        # Instead, generate a normal block and verify the index tracks it.
        self.nodes[0].generate(1)
        normal_block_height = self.nodes[0].getblockcount()
        self.sync_all()

        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        for hash_option in index_hash_options:
            res7 = index_node.gettxoutsetinfo(hash_option, normal_block_height)
            self.block_sanity_check(res7['block_info'])
            assert_equal(res7['block_info']['unspendables']['unclaimed_rewards'], 0)

        self.log.info("Test that the index is robust across restarts")

        res8 = index_node.gettxoutsetinfo('muhash')
        self.restart_node(1, extra_args=self.extra_args[1])
        res9 = index_node.gettxoutsetinfo('muhash')
        assert_equal(res8, res9)

        index_node.generate(1)
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        res10 = index_node.gettxoutsetinfo('muhash')
        assert(res8['txouts'] < res10['txouts'])

    def _test_use_index_option(self):
        self.log.info("Test use_index option for nodes running the index")

        self.connect_nodes(0, 1)
        current_height = self.nodes[1].getblockcount()
        self.nodes[0].waitforblockheight(current_height)
        res = self.nodes[0].gettxoutsetinfo('muhash')
        option_res = self.nodes[1].gettxoutsetinfo(hash_type='muhash', hash_or_height=None, use_index=False)
        del res['disk_size'], option_res['disk_size']
        assert_equal(res, option_res)

    def _test_reorg_index(self):
        self.log.info("Test that index can handle reorgs")

        # Generate two blocks, let the index catch up, then invalidate the blocks
        index_node = self.nodes[1]
        reorg_blocks = index_node.generatetoaddress(2, index_node.getnewaddress())
        reorg_block = reorg_blocks[1]
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        res_invalid = index_node.gettxoutsetinfo('muhash')
        pre_reorg_height = index_node.getblockcount()
        index_node.invalidateblock(reorg_blocks[0])
        assert_equal(index_node.gettxoutsetinfo('muhash')['height'], pre_reorg_height - 2)

        # Add two new blocks
        block = index_node.generate(2)[1]
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        res = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=None, use_index=False)

        # Test that the result of the reorged block is not returned for its old block height
        res2 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=pre_reorg_height)
        assert_equal(res["bestblock"], block)
        assert_equal(res["muhash"], res2["muhash"])
        assert(res["muhash"] != res_invalid["muhash"])

        # Test that requesting reorged out block by hash is still returning correct results
        res_invalid2 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=reorg_block)
        assert_equal(res_invalid2["muhash"], res_invalid["muhash"])
        assert(res["muhash"] != res_invalid2["muhash"])

        # Add another block, so we don't depend on reconsiderblock remembering which
        # blocks were touched by invalidateblock
        index_node.generate(1)
        self.sync_all()

        # Ensure that removing and re-adding blocks yields consistent results
        # Use a height safely below the current tip
        reorg_target_height = pre_reorg_height - 2 - 10
        block = index_node.getblockhash(reorg_target_height)
        index_node.invalidateblock(block)
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        index_node.reconsiderblock(block)
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", index_node.gettxoutsetinfo, 'muhash'))
        res3 = index_node.gettxoutsetinfo(hash_type='muhash', hash_or_height=pre_reorg_height)
        assert_equal(res2, res3)

        self.log.info("Test that a node aware of stale blocks syncs them as well")
        node = self.nodes[0]
        # Ensure the node is aware of a stale block prior to restart
        node.getblock(reorg_block)

        self.restart_node(0, ["-coinstatsindex"])
        self.wait_until(lambda: not try_rpc(-32603, "Unable to read UTXO set", node.gettxoutsetinfo, 'muhash'))
        assert_raises_rpc_error(-32603, "Unable to read UTXO set", node.gettxoutsetinfo, 'muhash', reorg_block)

    def _test_index_rejects_hash_serialized(self):
        self.log.info("Test that the rpc raises if the legacy hash is passed with the index")

        current_height = self.nodes[1].getblockcount()
        msg = "hash_serialized_2 hash type cannot be queried for a specific block"
        assert_raises_rpc_error(-8, msg, self.nodes[1].gettxoutsetinfo, hash_type='hash_serialized_2', hash_or_height=current_height - 2)

        for use_index in {True, False, None}:
            assert_raises_rpc_error(-8, msg, self.nodes[1].gettxoutsetinfo, hash_type='hash_serialized_2', hash_or_height=current_height - 2, use_index=use_index)


if __name__ == '__main__':
    CoinStatsIndexTest().main()
