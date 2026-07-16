#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test node responses to invalid locators.

ReddCoin adaptation notes:
- Uses cache with 199 blocks (setup_clean_chain=False) for PoS staking
- Advances mocktime before generating PoS blocks
- Uses noban whitelist to prevent mocktime timeout while still allowing disconnect
"""

from test_framework.messages import msg_getheaders, msg_getblocks, MAX_LOCATOR_SZ
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import advance_time_for_pos


class InvalidLocatorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with 199 blocks for PoS staking
        # noban whitelist: prevents mocktime timeout bug while still allowing
        # disconnect for invalid protocol messages (like oversized locators)
        self.extra_args = [["-whitelist=noban@127.0.0.1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]  # convenience reference to the node

        # ReddCoin: Advance time for PoS staking before generating blocks
        # Use retry logic as PoS block generation is probabilistic
        advance_time_for_pos(node, seconds=600)
        for attempt in range(10):
            try:
                node.generatetoaddress(1, node.get_deterministic_priv_key().address)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < 9:
                    advance_time_for_pos(node, seconds=300)
                    continue
                raise

        self.log.info('Test max locator size')
        block_count = node.getblockcount()
        for msg in [msg_getheaders(), msg_getblocks()]:
            self.log.info('Wait for disconnect when sending {} hashes in locator'.format(MAX_LOCATOR_SZ + 1))
            exceed_max_peer = node.add_p2p_connection(P2PInterface())
            msg.locator.vHave = [int(node.getblockhash(i - 1), 16) for i in range(block_count, block_count - (MAX_LOCATOR_SZ + 1), -1)]
            exceed_max_peer.send_message(msg)
            exceed_max_peer.wait_for_disconnect()
            node.disconnect_p2ps()

            self.log.info('Wait for response when sending {} hashes in locator'.format(MAX_LOCATOR_SZ))
            within_max_peer = node.add_p2p_connection(P2PInterface())
            msg.locator.vHave = [int(node.getblockhash(i - 1), 16) for i in range(block_count, block_count - (MAX_LOCATOR_SZ), -1)]
            within_max_peer.send_message(msg)
            if type(msg) == msg_getheaders:
                within_max_peer.wait_for_header(node.getbestblockhash())
            else:
                within_max_peer.wait_for_block(int(node.getbestblockhash(), 16))


if __name__ == '__main__':
    InvalidLocatorTest().main()
