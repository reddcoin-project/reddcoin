#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that we don't leak txs to inbound peers that we haven't yet announced to

ReddCoin adaptation notes:
- Uses cache with 199 blocks for PoS staking
- Advances mocktime before generating PoS blocks
- Uses whitelist to prevent mocktime timeout bug
"""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import msg_getdata, CInv, MSG_TX
from test_framework.p2p import p2p_lock, P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)
from test_framework.wallet import MiniWallet


class P2PNode(P2PDataStore):
    def on_inv(self, msg):
        pass


class P2PLeakTxTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with 199 blocks for PoS
        self.extra_args = [["-whitelist=127.0.0.1"]]  # Prevent mocktime timeout bug

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def generate_pos_block(self, node):
        """Generate a single PoS block with retry logic."""
        for attempt in range(10):
            try:
                return node.generatetoaddress(1, node.get_deterministic_priv_key().address)[0]
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < 9:
                    advance_time_for_pos(node, seconds=300)
                    continue
                raise

    def run_test(self):
        gen_node = self.nodes[0]  # The block and tx generating node

        # ReddCoin: Advance time significantly for PoS staking
        advance_time_for_pos(gen_node, seconds=600)

        miniwallet = MiniWallet(gen_node)

        # ReddCoin: Generate blocks with retry logic and fund MiniWallet manually
        # Generate 1 block and send coins to MiniWallet's address
        self.generate_pos_block(gen_node)
        if miniwallet._address:
            txid = gen_node.sendtoaddress(miniwallet._address, 10)
            tx = gen_node.getrawtransaction(txid, True)
            miniwallet.scan_tx(tx)

        # Generate COINBASE_MATURITY blocks to mature coins
        for _ in range(COINBASE_MATURITY):
            self.generate_pos_block(gen_node)

        inbound_peer = self.nodes[0].add_p2p_connection(P2PNode())  # An "attacking" inbound peer

        MAX_REPEATS = 100
        self.log.info("Running test up to {} times.".format(MAX_REPEATS))
        for i in range(MAX_REPEATS):
            self.log.info('Run repeat {}'.format(i + 1))
            txid = miniwallet.send_self_transfer(from_node=gen_node)['wtxid']

            want_tx = msg_getdata()
            want_tx.inv.append(CInv(t=MSG_TX, h=int(txid, 16)))
            with p2p_lock:
                inbound_peer.last_message.pop('notfound', None)
            inbound_peer.send_and_ping(want_tx)

            if inbound_peer.last_message.get('notfound'):
                self.log.debug('tx {} was not yet announced to us.'.format(txid))
                self.log.debug("node has responded with a notfound message. End test.")
                assert_equal(inbound_peer.last_message['notfound'].vec[0].hash, int(txid, 16))
                with p2p_lock:
                    inbound_peer.last_message.pop('notfound')
                break
            else:
                self.log.debug('tx {} was already announced to us. Try test again.'.format(txid))
                assert int(txid, 16) in [inv.hash for inv in inbound_peer.last_message['inv'].inv]


if __name__ == '__main__':
    P2PLeakTxTest().main()
