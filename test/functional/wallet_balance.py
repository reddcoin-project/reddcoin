#!/usr/bin/env python3
# Copyright (c) 2018-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the wallet balance RPC methods."""
from decimal import Decimal
import struct

from test_framework.address import ADDRESS_BCRT1_UNSPENDABLE as ADDRESS_WATCHONLY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_equal,
    assert_raises_rpc_error,
)


def create_transactions(node, address, amt, fees):
    # Create and sign raw transactions from node to address for amt.
    # Creates a transaction for each fee and returns an array
    # of the raw transactions.
    utxos = [u for u in node.listunspent(0) if u['spendable']]

    # Create transactions
    inputs = []
    ins_total = 0
    for utxo in utxos:
        inputs.append({"txid": utxo["txid"], "vout": utxo["vout"]})
        ins_total += utxo['amount']
        if ins_total >= amt + max(fees):
            break
    # make sure there was enough utxos
    assert ins_total >= amt + max(fees)

    txs = []
    for fee in fees:
        outputs = {address: amt}
        # prevent 0 change output
        if ins_total > amt + fee:
            outputs[node.getrawchangeaddress()] = ins_total - amt - fee
        raw_tx = node.createrawtransaction(inputs, outputs, 0, True)
        raw_tx = node.signrawtransactionwithwallet(raw_tx)
        assert_equal(raw_tx['complete'], True)
        txs.append(raw_tx)

    return txs

class WalletTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3  # Node 2 = miner/bank with cache keys
        self.setup_clean_chain = False  # PoS territory (cache at height 199)
        self.extra_args = [
            ['-limitdescendantcount=3'],
            [],
            [],  # miner
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        # Only miner (node 2) gets cache keys for staking
        self.init_wallet(2)
        # Nodes 0 and 1 get fresh isolated wallets
        for i in [0, 1]:
            self.nodes[i].createwallet(
                wallet_name=self.default_wallet_name,
                descriptors=self.options.descriptors,
                load_on_startup=True,
            )

    def run_test(self):
        FUND = Decimal('1000')
        SEND_0_TO_1 = FUND - Decimal('10')     # 990
        SEND_1_TO_0 = FUND + Decimal('20')     # 1020 (forces unsafe input)
        NODE1_CHANGE = FUND + SEND_0_TO_1 - SEND_1_TO_0  # 970
        WATCHONLY_FUND = FUND  # Match FUND for symmetry
        miner = self.nodes[2]

        if not self.options.descriptors:
            # Tests legacy watchonly behavior which is not present (and does not need to be tested) in descriptor wallets
            self.nodes[0].importaddress(ADDRESS_WATCHONLY)
            assert 'watchonly' in self.nodes[0].getbalances()
            assert 'watchonly' not in self.nodes[1].getbalances()

        self.log.info("Funding test wallets from miner...")
        addr0 = self.nodes[0].getnewaddress()
        addr1 = self.nodes[1].getnewaddress()
        miner.sendtoaddress(addr0, FUND)
        miner.sendtoaddress(addr1, FUND)
        if not self.options.descriptors:
            miner.sendtoaddress(ADDRESS_WATCHONLY, WATCHONLY_FUND)

        advance_time_for_pos(self.nodes, seconds=60)
        miner.generate(1)
        self.sync_all()

        if not self.options.descriptors:
            assert_equal(self.nodes[0].getbalances()['mine']['trusted'], FUND)
            assert_equal(self.nodes[0].getwalletinfo()['balance'], FUND)
            assert_equal(self.nodes[1].getbalances()['mine']['trusted'], FUND)

            # Watchonly funded via send -> trusted (not immature like Bitcoin's coinbase)
            assert_equal(self.nodes[0].getbalances()['watchonly']['trusted'], WATCHONLY_FUND)
            assert 'watchonly' not in self.nodes[1].getbalances()

            assert_equal(self.nodes[0].getbalance(), FUND)
            assert_equal(self.nodes[1].getbalance(), FUND)

        self.log.info("Test getbalance with different arguments")
        assert_equal(self.nodes[0].getbalance("*"), FUND)
        assert_equal(self.nodes[0].getbalance("*", 1), FUND)
        assert_equal(self.nodes[0].getbalance(minconf=1), FUND)
        if not self.options.descriptors:
            assert_equal(self.nodes[0].getbalance(minconf=0, include_watchonly=True), FUND + WATCHONLY_FUND)
            assert_equal(self.nodes[0].getbalance("*", 1, True), FUND + WATCHONLY_FUND)
        else:
            assert_equal(self.nodes[0].getbalance(minconf=0, include_watchonly=True), FUND)
            assert_equal(self.nodes[0].getbalance("*", 1, True), FUND)
        assert_equal(self.nodes[1].getbalance(minconf=0, include_watchonly=True), FUND)

        # Send 990 from node 0 to node 1 and 1020 from node 1 to node 0.
        txs = create_transactions(self.nodes[0], self.nodes[1].getnewaddress(), SEND_0_TO_1, [Decimal('0.01')])
        self.nodes[0].sendrawtransaction(txs[0]['hex'])
        self.nodes[1].sendrawtransaction(txs[0]['hex'])  # sending on all nodes is faster than waiting for propagation
        miner.sendrawtransaction(txs[0]['hex'])
        self.sync_all()

        txs = create_transactions(self.nodes[1], self.nodes[0].getnewaddress(), SEND_1_TO_0, [Decimal('0.01'), Decimal('0.02')])
        self.nodes[1].sendrawtransaction(txs[0]['hex'])
        self.nodes[0].sendrawtransaction(txs[0]['hex'])  # sending on all nodes is faster than waiting for propagation
        miner.sendrawtransaction(txs[0]['hex'])
        self.sync_all()

        # First argument of getbalance must be set to "*"
        assert_raises_rpc_error(-32, "dummy first argument must be excluded or set to \"*\"", self.nodes[1].getbalance, "")

        self.log.info("Test balances with unconfirmed inputs")

        # Before `test_balance()`, we have had two nodes with a balance of
        # FUND (1000) each and then we:
        #
        # 1) Sent 990 from node A to node B with fee 0.01
        # 2) Sent 1020 from node B to node A with fee 0.01
        #
        # Then we check the balances:
        #
        # 1) As is
        # 2) With transaction 2 from above with 2x the fee
        #
        # Prior to #16766, in this situation, the node would immediately report
        # a balance of 970 on node B as unconfirmed and trusted.
        #
        # After #16766, we show that balance as unconfirmed.
        #
        # The balance is indeed "trusted" and "confirmed" insofar as removing
        # the mempool transactions would return at least that much money. But
        # the algorithm after #16766 marks it as unconfirmed because the 'taint'
        # tracking of transaction trust for summing balances doesn't consider
        # which inputs belong to a user. In this case, the change output in
        # question could be "destroyed" by replace the 1st transaction above.
        #
        # The post #16766 behavior is correct; we shouldn't be treating those
        # funds as confirmed. If you want to rely on that specific UTXO existing
        # which has given you that balance, you cannot, as a third party
        # spending the other input would destroy that unconfirmed.
        #
        # For example, if the test transactions were:
        #
        # 1) Sent 990 from node A to node B with fee 0.01
        # 2) Sent 10 from node B to node A with fee 0.01
        #
        # Then our node would report a confirmed balance of 990 + 1000 - 10 = 1980
        # RDD, which is more than would be available if transaction 1 were
        # replaced.


        def test_balances(*, fee_node_1=0):
            # getbalances
            expected_balances_0 = {
                'mine': {
                    'immature': Decimal('0E-8'),
                    'trusted': Decimal('9.99'),  # change from node 0's send
                    'untrusted_pending': SEND_1_TO_0,
                },
                'watchonly': {
                    'immature': Decimal('0E-8'),
                    'trusted': WATCHONLY_FUND,
                    'untrusted_pending': Decimal('0E-8'),
                },
            }
            expected_balances_1 = {
                'mine': {
                    'immature': Decimal('0E-8'),
                    'trusted': Decimal('0E-8'),  # node 1's send had an unsafe input
                    'untrusted_pending': NODE1_CHANGE - fee_node_1,
                },
            }
            if self.options.descriptors:
                del expected_balances_0["watchonly"]
            assert_equal(self.nodes[0].getbalances(), expected_balances_0)
            assert_equal(self.nodes[1].getbalances(), expected_balances_1)
            # getbalance without any arguments includes unconfirmed transactions, but not untrusted transactions
            assert_equal(self.nodes[0].getbalance(), Decimal('9.99'))  # change from node 0's send
            assert_equal(self.nodes[1].getbalance(), Decimal('0'))  # node 1's send had an unsafe input
            # Same with minconf=0
            assert_equal(self.nodes[0].getbalance(minconf=0), Decimal('9.99'))
            assert_equal(self.nodes[1].getbalance(minconf=0), Decimal('0'))
            # getbalance with a minconf incorrectly excludes coins that have been spent more recently than the minconf blocks ago
            # TODO: fix getbalance tracking of coin spentness depth
            assert_equal(self.nodes[0].getbalance(minconf=1), Decimal('0'))
            assert_equal(self.nodes[1].getbalance(minconf=1), Decimal('0'))
            # getunconfirmedbalance
            assert_equal(self.nodes[0].getunconfirmedbalance(), SEND_1_TO_0)
            assert_equal(self.nodes[1].getunconfirmedbalance(), NODE1_CHANGE - fee_node_1)
            # getwalletinfo.unconfirmed_balance
            assert_equal(self.nodes[0].getwalletinfo()["unconfirmed_balance"], SEND_1_TO_0)
            assert_equal(self.nodes[1].getwalletinfo()["unconfirmed_balance"], NODE1_CHANGE - fee_node_1)

        test_balances(fee_node_1=Decimal('0.01'))

        # Node 1 bumps the transaction fee and resends
        self.nodes[1].sendrawtransaction(txs[1]['hex'])
        self.nodes[0].sendrawtransaction(txs[1]['hex'])  # sending on all nodes is faster than waiting for propagation
        miner.sendrawtransaction(txs[1]['hex'])
        self.sync_all()

        self.log.info("Test getbalance and getbalances.mine.untrusted_pending with conflicted unconfirmed inputs")
        test_balances(fee_node_1=Decimal('0.02'))

        advance_time_for_pos(self.nodes, seconds=60)
        miner.generate(1)
        self.sync_all()

        # balances are correct after the transactions are confirmed
        balance_node0 = Decimal('9.99') + SEND_1_TO_0   # 1029.99
        balance_node1 = NODE1_CHANGE - Decimal('0.02')   # 969.98
        assert_equal(self.nodes[0].getbalances()['mine']['trusted'], balance_node0)
        assert_equal(self.nodes[1].getbalances()['mine']['trusted'], balance_node1)
        assert_equal(self.nodes[0].getbalance(), balance_node0)
        assert_equal(self.nodes[1].getbalance(), balance_node1)

        # Send total balance away from node 1
        txs = create_transactions(self.nodes[1], self.nodes[0].getnewaddress(), balance_node1 - Decimal('0.01'), [Decimal('0.01')])
        self.nodes[1].sendrawtransaction(txs[0]['hex'])
        miner.sendrawtransaction(txs[0]['hex'])
        advance_time_for_pos(self.nodes, seconds=60)
        miner.generate(2)
        self.sync_all()

        # getbalance with a minconf incorrectly excludes coins that have been spent more recently than the minconf blocks ago
        # TODO: fix getbalance tracking of coin spentness depth
        # getbalance with minconf=3 should still show the old balance
        assert_equal(self.nodes[1].getbalance(minconf=3), Decimal('0'))

        # getbalance with minconf=2 will show the new balance.
        assert_equal(self.nodes[1].getbalance(minconf=2), Decimal('0'))

        # check mempool transactions count for wallet unconfirmed balance after
        # dynamically loading the wallet.
        before = self.nodes[1].getbalances()['mine']['untrusted_pending']
        dst = self.nodes[1].getnewaddress()
        self.nodes[1].unloadwallet(self.default_wallet_name)
        txid = self.nodes[0].sendtoaddress(dst, 0.1)
        rawtx = self.nodes[0].getrawtransaction(txid)
        self.nodes[1].sendrawtransaction(rawtx)
        self.nodes[1].loadwallet(self.default_wallet_name)
        after = self.nodes[1].getbalances()['mine']['untrusted_pending']
        assert_equal(before + Decimal('0.1'), after)

        # Create 3 more wallet txs, where the last is not accepted to the
        # mempool because it is the third descendant of the tx above
        for _ in range(3):
            # Set amount high enough such that all coins are spent by each tx
            node0_bal = self.nodes[0].getbalance()
            large_send = node0_bal - Decimal('1')
            txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), large_send)

        self.log.info('Check that wallet txs not in the mempool are untrusted')
        assert txid not in self.nodes[0].getrawmempool()
        assert_equal(self.nodes[0].gettransaction(txid)['trusted'], False)
        assert_equal(self.nodes[0].getbalance(minconf=0), 0)

        self.log.info("Test replacement and reorg of non-mempool tx")
        tx_orig = self.nodes[0].gettransaction(txid)['hex']
        # Increase fee by 1 coin
        tx_replace = tx_orig.replace(
            struct.pack("<q", int(large_send * 10**8)).hex(),
            struct.pack("<q", int((large_send - 1) * 10**8)).hex(),
        )
        tx_replace = self.nodes[0].signrawtransactionwithwallet(tx_replace)['hex']
        # Total balance is given by the sum of outputs of the tx
        total_amount = sum([o['value'] for o in self.nodes[0].decoderawtransaction(tx_replace)['vout']])
        # Relay mempool parent txs to nodes 1 and miner so they can accept tx_replace
        # Sort by ancestor count to ensure parents are sent before children
        mempool_info = self.nodes[0].getrawmempool(True)
        for txid_relay in sorted(mempool_info, key=lambda t: mempool_info[t]['ancestorcount']):
            rawtx = self.nodes[0].getrawtransaction(txid_relay)
            self.nodes[1].sendrawtransaction(rawtx)
            miner.sendrawtransaction(rawtx)
        self.nodes[1].sendrawtransaction(hexstring=tx_replace, maxfeerate=0)
        miner.sendrawtransaction(hexstring=tx_replace, maxfeerate=0)

        # Now confirm tx_replace
        advance_time_for_pos(self.nodes, seconds=60)
        block_reorg = miner.generate(1)[0]
        self.sync_all()
        assert_equal(self.nodes[0].getbalance(minconf=0), total_amount)

        self.log.info('Put txs back into mempool of node 1 (not node 0)')
        self.nodes[0].invalidateblock(block_reorg)
        self.nodes[1].invalidateblock(block_reorg)
        # Invalidate on miner, then restart to clear mempool (otherwise the
        # new block would include tx_replace, defeating the test purpose)
        miner.invalidateblock(block_reorg)
        self.restart_node(2, ['-persistmempool=0'])
        self.connect_nodes(1, 2)
        assert_equal(self.nodes[0].getbalance(minconf=0), 0)  # wallet txs not in the mempool are untrusted

        # Relay parent txs (NOT tx_replace) from node 0 to miner for inclusion
        mempool_info = self.nodes[0].getrawmempool(True)
        for txid_relay in sorted(mempool_info, key=lambda t: mempool_info[t]['ancestorcount']):
            miner.sendrawtransaction(self.nodes[0].getrawtransaction(txid_relay))

        advance_time_for_pos(self.nodes, seconds=60)
        miner.generate(1)
        self.sync_blocks()
        assert_equal(self.nodes[0].getbalance(minconf=0), 0)  # wallet txs not in the mempool are untrusted

        # Now confirm tx_orig
        self.restart_node(1, ['-persistmempool=0'])
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.sync_blocks()
        self.nodes[1].sendrawtransaction(tx_orig)
        miner.sendrawtransaction(tx_orig)
        advance_time_for_pos(self.nodes, seconds=60)
        miner.generate(1)
        self.sync_all()
        assert_equal(self.nodes[0].getbalance(minconf=0), total_amount + 1)  # The reorg recovered our fee of 1 coin


if __name__ == '__main__':
    WalletTest().main()
