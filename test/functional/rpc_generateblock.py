#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
'''Test generateblock rpc.

ReddCoin PoS note: After nLastPowHeight (block 89 on regtest), generateblock
creates PoS blocks. In PoS mode the coinbase output is empty (no block reward),
so the 'output' address/descriptor parameter does not control the coinbase
destination — the coinstake determines the staking reward instead. The
coinbase still has outputs but they are not the block reward. The output
parameter is still validated (address/descriptor parsing) but has no effect
on block contents in PoS mode.
'''

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    advance_time_for_pos,
)


def generateblock_pos(node, output, transactions=[]):
    """Wrapper with retry logic for PoS generateblock (coinstake is probabilistic)."""
    max_attempts = 5
    for attempt in range(max_attempts):
        try:
            return node.generateblock(output=output, transactions=transactions)
        except Exception as e:
            if "No valid coinstake found" in str(e) and attempt < max_attempts - 1:
                advance_time_for_pos(node, seconds=60)
            else:
                raise


class GenerateBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with 199 blocks (PoS range)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        # Verify we're in PoS range
        initial_height = node.getblockcount()
        self.log.info(f'Initial block height: {initial_height} (PoS range)')
        assert initial_height >= 199

        # Advance time to ensure coins have sufficient age for staking
        advance_time_for_pos(node, seconds=600)

        self.log.info('Generate an empty block to address')
        address = node.getnewaddress()
        hash = generateblock_pos(node, address)['hash']
        block = node.getblock(blockhash=hash, verbose=2)
        # PoS blocks have coinbase + coinstake = 2 txs
        assert_equal(len(block['tx']), 2)

        self.log.info('Generate an empty block to a descriptor')
        hash = generateblock_pos(node, 'addr(' + address + ')')['hash']
        block = node.getblock(blockhash=hash, verbosity=2)
        assert_equal(len(block['tx']), 2)

        self.log.info('Generate an empty block to a combo descriptor with compressed pubkey')
        combo_key = '0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
        hash = generateblock_pos(node, 'combo(' + combo_key + ')')['hash']
        block = node.getblock(hash, 2)
        assert_equal(len(block['tx']), 2)

        self.log.info('Generate an empty block to a combo descriptor with uncompressed pubkey')
        combo_key = '0408ef68c46d20596cc3f6ddf7c8794f71913add807f1dc55949fa805d764d191c0b7ce6894c126fce0babc6663042f3dde9b0cf76467ea315514e5a6731149c67'
        hash = generateblock_pos(node, 'combo(' + combo_key + ')')['hash']
        block = node.getblock(hash, 2)
        assert_equal(len(block['tx']), 2)

        # Generate some extra mempool transactions to verify they don't get mined
        for _ in range(10):
            node.sendtoaddress(address, 0.001)

        self.log.info('Generate block with txid')
        txid = node.sendtoaddress(address, 1)
        hash = generateblock_pos(node, address, [txid])['hash']
        block = node.getblock(hash, 1)
        # PoS blocks: coinbase + coinstake + user tx = 3 txs
        assert_equal(len(block['tx']), 3)
        assert_equal(block['tx'][2], txid)

        self.log.info('Generate block with raw tx')
        utxos = node.listunspent(addresses=[address])
        raw = node.createrawtransaction([{'txid':utxos[0]['txid'], 'vout':utxos[0]['vout']}],[{address:1}])
        signed_raw = node.signrawtransactionwithwallet(raw)['hex']
        hash = generateblock_pos(node, address, [signed_raw])['hash']
        block = node.getblock(hash, 1)
        assert_equal(len(block['tx']), 3)
        txid = block['tx'][2]
        assert_equal(node.gettransaction(txid)['hex'], signed_raw)

        self.log.info('Fail to generate block with out of order txs')
        # Advance time so coinstake succeeds and the test reaches TestBlockValidity
        advance_time_for_pos(node, seconds=600)
        raw1 = node.createrawtransaction([{'txid':txid, 'vout':0}],[{address:0.998}])
        signed_raw1 = node.signrawtransactionwithwallet(raw1)['hex']
        txid1 = node.sendrawtransaction(signed_raw1)
        raw2 = node.createrawtransaction([{'txid':txid1, 'vout':0}],[{address:0.996}])
        signed_raw2 = node.signrawtransactionwithwallet(raw2)['hex']
        assert_raises_rpc_error(-25, 'TestBlockValidity failed: bad-txns-inputs-missingorspent', node.generateblock, address, [signed_raw2, txid1])

        self.log.info('Fail to generate block with txid not in mempool')
        missing_txid = '0000000000000000000000000000000000000000000000000000000000000000'
        assert_raises_rpc_error(-5, 'Transaction ' + missing_txid + ' not in mempool.', node.generateblock, address, [missing_txid])

        self.log.info('Fail to generate block with invalid raw tx')
        invalid_raw_tx = '0000'
        assert_raises_rpc_error(-22, 'Transaction decode failed for ' + invalid_raw_tx, node.generateblock, address, [invalid_raw_tx])

        self.log.info('Fail to generate block with invalid address/descriptor')
        assert_raises_rpc_error(-5, 'Invalid address or descriptor', node.generateblock, '1234', [])

        self.log.info('Fail to generate block with a ranged descriptor')
        ranged_descriptor = 'pkh(tpubD6NzVbkrYhZ4XgiXtGrdW5XDAPFCL9h7we1vwNCpn8tGbBcgfVYjXyhWo4E1xkh56hjod1RhGjxbaTLV3X4FyWuejifB9jusQ46QzG87VKp/0/*)'
        assert_raises_rpc_error(-8, 'Ranged descriptor not accepted. Maybe pass through deriveaddresses first?', node.generateblock, ranged_descriptor, [])

        self.log.info('Fail to generate block with a descriptor missing a private key')
        child_descriptor = 'pkh(tpubD6NzVbkrYhZ4XgiXtGrdW5XDAPFCL9h7we1vwNCpn8tGbBcgfVYjXyhWo4E1xkh56hjod1RhGjxbaTLV3X4FyWuejifB9jusQ46QzG87VKp/0\'/0)'
        assert_raises_rpc_error(-5, 'Cannot derive script without private keys', node.generateblock, child_descriptor, [])

if __name__ == '__main__':
    GenerateBlockTest().main()
