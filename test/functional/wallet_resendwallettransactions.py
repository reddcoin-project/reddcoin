#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the wallet resends transactions periodically."""
import time

from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ResendWalletTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # -whitelist: bypass trickle relay delay for P2P broadcast checks
        # -blockmaxweight=4000: force empty blocks so the mempool tx stays unconfirmed
        # -peertimeout=9999999: prevent P2P disconnect from 36-hour mocktime jumps
        self.extra_args = [['-whitelist=127.0.0.1', '-blockmaxweight=4000', '-peertimeout=9999999']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]  # alias

        # Sync mocktime with real clock. The cache uses ~2022 timestamps, but
        # this test uses time.time() (~2026) for mocktime jumps. Without this,
        # nNextResend is initialized at 2022 and the 12h/36h checks break.
        real_now = int(time.time())
        node.setmocktime(real_now)
        node.mocktime = real_now  # Update framework tracker so generate() respects it

        peer_first = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a new transaction and wait until it's broadcast")
        txid = node.sendtoaddress(node.getnewaddress(), 1)

        # Wallet rebroadcast is first scheduled 1 sec after startup (see
        # nNextResend in ResendWalletTransactions()). Tell scheduler to call
        # MaybeResendWalletTxn now to initialize nNextResend before the first
        # setmocktime call below.
        node.mockscheduler(1)

        # Can take a few seconds due to transaction trickling
        peer_first.wait_for_broadcast([txid])

        # Add a second peer since txs aren't rebroadcast to the same peer (see filterInventoryKnown)
        peer_second = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a block")
        # Generate a block without the transaction.
        # -blockmaxweight=4000 ensures the mempool tx is not included.
        # Transactions are only rebroadcast if there has been a block at least five minutes
        # after the last time we tried to broadcast. Use mocktime and give an extra minute to be sure.
        block_time = int(time.time()) + 6 * 60
        node.setmocktime(block_time)
        node.mocktime = block_time  # Update framework tracker so generate() respects it
        node.generate(1)

        # Set correct m_best_block_time, which is used in ResendWalletTransactions
        node.syncwithvalidationinterfacequeue()
        now = int(time.time())

        # Transaction should not be rebroadcast within first 12 hours
        # Leave 2 mins for buffer
        twelve_hrs = 12 * 60 * 60
        two_min = 2 * 60
        node.setmocktime(now + twelve_hrs - two_min)
        node.mocktime = now + twelve_hrs - two_min
        node.mockscheduler(1)  # Tell scheduler to call MaybeResendWalletTxn now
        assert_equal(int(txid, 16) in peer_second.get_invs(), False)

        self.log.info("Bump time & check that transaction is rebroadcast")
        # Transaction should be rebroadcast approximately 24 hours in the future,
        # but can range from 12-36. So bump 36 hours to be sure.
        with node.assert_debug_log(['ResendWalletTransactions: resubmit 1 unconfirmed transactions']):
            node.setmocktime(now + 36 * 60 * 60)
            node.mocktime = now + 36 * 60 * 60
            # Tell scheduler to call MaybeResendWalletTxn now.
            node.mockscheduler(1)
        # Give some time for trickle to occur
        node.setmocktime(now + 36 * 60 * 60 + 600)
        node.mocktime = now + 36 * 60 * 60 + 600
        peer_second.wait_for_broadcast([txid])


if __name__ == '__main__':
    ResendWalletTransactionsTest().main()
