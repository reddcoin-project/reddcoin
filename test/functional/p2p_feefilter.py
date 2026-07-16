#!/usr/bin/env python3
# Copyright (c) 2016-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test processing of feefilter messages."""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import MSG_TX, MSG_WTX, msg_feefilter
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, advance_time_for_pos
from test_framework.wallet import MiniWallet


class FeefilterConn(P2PInterface):
    feefilter_received = False

    def on_feefilter(self, message):
        self.feefilter_received = True

    def assert_feefilter_received(self, recv: bool):
        with p2p_lock:
            assert_equal(self.feefilter_received, recv)


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.txinvs = []

    def on_inv(self, message):
        for i in message.inv:
            if (i.type == MSG_TX) or (i.type == MSG_WTX):
                self.txinvs.append('{:064x}'.format(i.hash))

    def wait_for_invs_to_match(self, invs_expected):
        invs_expected.sort()
        self.wait_until(lambda: invs_expected == sorted(self.txinvs))

    def clear_invs(self):
        with p2p_lock:
            self.txinvs = []


class FeeFilterTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # We lower the various required feerates for this test
        # to catch a corner-case where feefilter used to slightly undercut
        # mempool and wallet feerate calculation based on GetFee
        # rounding down 3 places, leading to stranded transactions.
        # See issue #16499
        # grant noban permission to all peers to speed up tx relay / mempool sync
        # ReddCoin: DEFAULT_MIN_RELAY_TX_FEE = 100000 sat/kB (0.001 RDD/kB)
        # Using minimum relay fee for this test
        # ReddCoin: whitelist is critical to prevent mocktime-related disconnections
        # and to bypass trickle relay delays for faster test execution
        self.extra_args = [[
            "-minrelaytxfee=0.001",
            "-mintxfee=0.001",
            "-whitelist=noban,relay@127.0.0.1",
        ]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.test_feefilter_forcerelay()
        self.test_feefilter()
        self.test_feefilter_blocksonly()

    def test_feefilter_forcerelay(self):
        # Initialize mocktime from cache block time BEFORE any restarts
        # This ensures self.nodes[i].mocktime is set for use in restart arguments
        cache_time = self.nodes[0].getblockheader(self.nodes[0].getbestblockhash())['time']
        for node in self.nodes:
            node.setmocktime(cache_time)
            node.mocktime = cache_time

        self.log.info('Check that peers without forcerelay permission (default) get a feefilter message')
        self.nodes[0].add_p2p_connection(FeefilterConn()).assert_feefilter_received(True)

        self.log.info('Check that peers with forcerelay permission do not get a feefilter message')
        # Disconnect inter-node connection before restart to prevent feefilter issues
        self.disconnect_nodes(0, 1)
        # Pass mocktime as startup arg to prevent incorrect feefilter calculation
        mocktime_arg = [f'-mocktime={self.nodes[0].mocktime}']
        self.restart_node(0, extra_args=['-whitelist=forcerelay@127.0.0.1'] + mocktime_arg)
        if self.nodes[1].mocktime:
            self.nodes[0].mocktime = self.nodes[1].mocktime
        self.nodes[0].add_p2p_connection(FeefilterConn()).assert_feefilter_received(False)

        # Restart to disconnect peers and load default extra_args
        self.restart_node(0, extra_args=self.extra_args[0] + mocktime_arg)
        if self.nodes[1].mocktime:
            self.nodes[0].mocktime = self.nodes[1].mocktime
        # Connect node0 to node1 so node1 sees node0 as inbound (whitelisted)
        # This allows node1 to relay transactions to node0 without trickle delay
        self.connect_nodes(0, 1)

    def test_feefilter(self):
        node1 = self.nodes[1]
        node0 = self.nodes[0]
        advance_time_for_pos(self.nodes, seconds=600)

        miniwallet = MiniWallet(node1)
        # Add enough mature utxos to the wallet, so that all txs spend confirmed coins
        self.generate(1, node1)
        miniwallet.generate(5)
        self.generate(COINBASE_MATURITY, node1)

        conn = self.nodes[0].add_p2p_connection(TestP2PConn())

        # ReddCoin: DEFAULT_MIN_RELAY_TX_FEE = 100000 sat/kB (0.001 RDD/kB)
        # Fee rates must be above minimum relay fee to be accepted
        # ReddCoin: Advance mocktime by 10s after each tx batch to ensure trickle relay fires
        # (Poisson-distributed trickle has 2s average for outbound, but can be longer)
        self.log.info("Test txs paying 0.002 RDD/kB are received by test connection")
        txids = [miniwallet.send_self_transfer(fee_rate=Decimal('0.002'), from_node=node1)['wtxid'] for _ in range(3)]
        advance_time_for_pos(self.nodes, seconds=10)
        self.sync_mempools()  # ensure node0 has received txs from node1
        conn.wait_for_invs_to_match(txids)
        conn.clear_invs()

        # Set a fee filter of 150000 sat/kB on test connection (0.0015 RDD/kB)
        conn.send_and_ping(msg_feefilter(150000))

        self.log.info("Test txs paying 0.0015 RDD/kB are received by test connection (at filter)")
        txids = [miniwallet.send_self_transfer(fee_rate=Decimal('0.0015'), from_node=node1)['wtxid'] for _ in range(3)]
        advance_time_for_pos(self.nodes, seconds=10)
        self.sync_mempools()  # ensure node0 has received txs from node1
        conn.wait_for_invs_to_match(txids)
        conn.clear_invs()

        self.log.info("Test txs paying 0.00125 RDD/kB are no longer received by test connection (below filter)")
        low_fee_txids = [miniwallet.send_self_transfer(fee_rate=Decimal('0.00125'), from_node=node1)['wtxid'] for _ in range(3)]
        advance_time_for_pos(self.nodes, seconds=10)  # Longer time to ensure trickle relay fires
        self.sync_mempools()  # must be sure node 0 has received all txs

        # Send one transaction from node0 that should be received, so that we
        # we can sync the test on receipt (if node1's txs were relayed, they'd
        # be received by the time this node0 tx is received). This is
        # unfortunately reliant on the current relay behavior where we batch up
        # to 35 entries in an inv, which means that when this next transaction
        # is eligible for relay, the prior transactions from node1 are eligible
        # as well.
        txids = [miniwallet.send_self_transfer(fee_rate=Decimal('0.003'), from_node=node0)['wtxid'] for _ in range(1)]
        advance_time_for_pos(self.nodes, seconds=10)
        conn.wait_for_invs_to_match(txids)
        conn.clear_invs()
        self.sync_mempools()  # must be sure node 1 has received all txs

        self.log.info("Remove fee filter and check txs are received again")
        conn.send_and_ping(msg_feefilter(0))
        txids = [miniwallet.send_self_transfer(fee_rate=Decimal('0.002'), from_node=node1)['wtxid'] for _ in range(3)]
        advance_time_for_pos(self.nodes, seconds=10)
        self.sync_mempools()  # ensure node0 has received txs from node1 before checking test connection
        conn.wait_for_invs_to_match(txids)
        conn.clear_invs()

    def test_feefilter_blocksonly(self):
        """Test that we don't send fee filters to block-relay-only peers and when we're in blocksonly mode."""
        self.log.info("Check that we don't send fee filters to block-relay-only peers.")
        feefilter_peer = self.nodes[0].add_outbound_p2p_connection(FeefilterConn(), p2p_idx=0, connection_type="block-relay-only")
        feefilter_peer.sync_with_ping()
        feefilter_peer.assert_feefilter_received(False)

        self.log.info("Check that we don't send fee filters when in blocksonly mode.")
        self.restart_node(0, ["-blocksonly"])
        feefilter_peer = self.nodes[0].add_p2p_connection(FeefilterConn())
        feefilter_peer.sync_with_ping()
        feefilter_peer.assert_feefilter_received(False)


if __name__ == '__main__':
    FeeFilterTest().main()
