#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test mempool re-org scenarios.

Test re-org scenarios with a mempool that contains transactions
that spend (directly or indirectly) coinbase transactions.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

class MempoolCoinbaseTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            [
                '-whitelist=noban@127.0.0.1',  # immediate tx relay
            ],
            []
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        # Start with a 199 block chain (ReddCoin cache)
        assert_equal(node.getblockcount(), 199)

        # Helper: create a spend of a specific UTXO, return {hex, txid, vout, value}
        fee = Decimal("0.01")

        def create_spend(utxo, locktime=0):
            addr = node.getnewaddress()
            out_value = utxo['value'] - fee
            raw = node.createrawtransaction(
                [{"txid": utxo['txid'], "vout": utxo['vout']}],
                {addr: out_value},
                locktime
            )
            signed = node.signrawtransactionwithwallet(raw)
            txid = node.decoderawtransaction(signed['hex'])['txid']
            return {'hex': signed['hex'], 'txid': txid, 'vout': 0, 'value': out_value}

        self.log.info("Find 4 unspent coinbase utxos from PoW blocks")
        # PoS coinstake consumes some coinbase UTXOs during cache generation,
        # so scan PoW blocks (1-89) for ones that are still unspent.
        unspent_coinbase = []
        for h in range(70, 89):
            bh = node.getblockhash(h)
            block = node.getblock(bh, 2)
            cb_tx = block['tx'][0]
            for vout in cb_tx['vout']:
                if vout['value'] > 0 and node.gettxout(cb_tx['txid'], vout['n']):
                    unspent_coinbase.append({
                        'txid': cb_tx['txid'],
                        'vout': vout['n'],
                        'value': vout['value'],
                        'height': h,
                    })
            if len(unspent_coinbase) >= 4:
                break
        assert len(unspent_coinbase) >= 4, "Need at least 4 unspent coinbase UTXOs from PoW blocks"

        first_block = unspent_coinbase[0]['height']
        utxo_0 = unspent_coinbase[0]
        utxo_1 = unspent_coinbase[1]
        utxo_2 = unspent_coinbase[2]
        utxo_3 = unspent_coinbase[3]
        self.log.info("Using coinbase UTXOs from blocks: %s" %
                      [u['height'] for u in unspent_coinbase[:4]])

        # Three scenarios for re-orging coinbase spends in the memory pool:
        # 1. Direct coinbase spend  :  spend_1
        # 2. Indirect (coinbase spend in chain, child in mempool) : spend_2 and spend_2_1
        # 3. Indirect (coinbase and child both in chain) : spend_3 and spend_3_1
        # Use invalidateblock to make all of the above coinbase spends invalid (immature coinbase),
        # and make sure the mempool code behaves correctly.
        self.log.info("Create three transactions spending from coinbase utxos: spend_1, spend_2, spend_3")
        spend_1 = create_spend(utxo_1)
        spend_2 = create_spend(utxo_2)
        spend_3 = create_spend(utxo_3)

        self.log.info("Create another transaction which is time-locked to two blocks in the future")
        timelock_tx = create_spend(utxo_0, locktime=node.getblockcount() + 2)['hex']

        self.log.info("Check that the time-locked transaction is too immature to spend")
        assert_raises_rpc_error(-26, "non-final", node.sendrawtransaction, timelock_tx)

        self.log.info("Broadcast and mine spend_2 and spend_3")
        node.sendrawtransaction(spend_2['hex'])
        node.sendrawtransaction(spend_3['hex'])
        self.log.info("Generate a block")
        node.generate(1)
        self.log.info("Check that time-locked transaction is still too immature to spend")
        assert_raises_rpc_error(-26, 'non-final', node.sendrawtransaction, timelock_tx)

        self.log.info("Create spend_2_1 and spend_3_1")
        spend_2_1 = create_spend({'txid': spend_2['txid'], 'vout': spend_2['vout'], 'value': spend_2['value']})
        spend_3_1 = create_spend({'txid': spend_3['txid'], 'vout': spend_3['vout'], 'value': spend_3['value']})

        self.log.info("Broadcast and mine spend_3_1")
        spend_3_1_id = node.sendrawtransaction(spend_3_1['hex'])
        self.log.info("Generate a block")
        last_block = node.generate(1)
        # Sync blocks, so that peer 1 gets the block before timelock_tx
        # Otherwise, peer 1 would put the timelock_tx in recentRejects
        self.sync_all()

        self.log.info("The time-locked transaction can now be spent")
        timelock_tx_id = node.sendrawtransaction(timelock_tx)

        self.log.info("Add spend_1 and spend_2_1 to the mempool")
        spend_1_id = node.sendrawtransaction(spend_1['hex'])
        spend_2_1_id = node.sendrawtransaction(spend_2_1['hex'])

        assert_equal(set(node.getrawmempool()), {spend_1_id, spend_2_1_id, timelock_tx_id})
        self.sync_all()

        self.log.info("invalidate the last block")
        for n in self.nodes:
            n.invalidateblock(last_block[0])
        self.log.info("The time-locked transaction is now too immature and has been removed from the mempool")
        self.log.info("spend_3_1 has been re-orged out of the chain and is back in the mempool")
        assert_equal(set(node.getrawmempool()), {spend_1_id, spend_2_1_id, spend_3_1_id})

        self.log.info("Use invalidateblock to re-org back and make all those coinbase spends immature/invalid")
        b = node.getblockhash(first_block + COINBASE_MATURITY)
        for n in self.nodes:
            n.invalidateblock(b)

        self.log.info("Check that the mempool is empty")
        assert_equal(set(node.getrawmempool()), set())
        self.sync_all()


if __name__ == '__main__':
    MempoolCoinbaseTest().main()
