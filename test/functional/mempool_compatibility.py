#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that mempool.dat is both backward and forward compatible between versions

NOTE: The test is designed to prevent cases when compatibility is broken accidentally.
In case we need to break mempool compatibility we can continue to use the test by just bumping the version number.

The previous release v0.15.2 is required by this test, see test/README.md.
"""

import os

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.wallet import MiniWallet


class MempoolCompatibilityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # ReddCoin: at the cached chain height (200) generate() takes the PoS path,
        # so MiniWallet.generate() drives node.generate()+sendtoaddress() and the
        # old node uses getnewaddress()/sendtoaddress() — both need a funded default
        # wallet. Upstream left them walletless (PoW MiniWallet needs no node wallet).
        self.wallet_names = [self.default_wallet_name, self.default_wallet_name]
        # ReddCoin: the PoS test chain runs on regtest mocktime (~2011), but a node
        # restarted without -mocktime falls back to real time (~2026). A tx carried
        # via mempool.dat then looks ~15 years old and is dropped as expired on load
        # ("N expired"). Disable mempool expiry so the moved tx survives the reload.
        self.extra_args = [['-mempoolexpiry=999999999'], ['-mempoolexpiry=999999999']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_previous_releases()

    def setup_network(self):
        self.add_nodes(self.num_nodes, extra_args=self.extra_args, versions=[
            4220903,  # oldest version with getmempoolinfo.loaded (used to avoid intermittent issues)
            None,
        ])
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()

    def run_test(self):
        self.log.info("Test that mempool.dat is compatible between versions")

        old_node, new_node = self.nodes
        new_wallet = MiniWallet(new_node)
        new_wallet.generate(1)
        new_node.generate(COINBASE_MATURITY)
        # Sync the nodes to ensure old_node has the block that contains the coinbase that new_wallet will spend.
        # Otherwise, because coinbases are only valid in a block and not as loose txns, if the nodes aren't synced
        # unbroadcasted_tx won't pass old_node's `MemPoolAccept::PreChecks`.
        self.connect_nodes(0, 1)
        self.sync_blocks()
        recipient = old_node.getnewaddress()
        self.stop_node(1)

        self.log.info("Add a transaction to mempool on old node and shutdown")
        old_tx_hash = old_node.sendtoaddress(recipient, 0.0001)
        assert old_tx_hash in old_node.getrawmempool()
        self.stop_node(0)

        self.log.info("Move mempool.dat from old to new node")
        old_node_mempool = os.path.join(old_node.datadir, self.chain, 'mempool.dat')
        new_node_mempool = os.path.join(new_node.datadir, self.chain, 'mempool.dat')
        os.rename(old_node_mempool, new_node_mempool)

        self.log.info("Start new node and verify mempool contains the tx")
        self.start_node(1)
        assert old_tx_hash in new_node.getrawmempool()

        self.log.info("Add unbroadcasted tx to mempool on new node and shutdown")
        unbroadcasted_tx_hash = new_wallet.send_self_transfer(from_node=new_node)['txid']
        assert unbroadcasted_tx_hash in new_node.getrawmempool()
        mempool = new_node.getrawmempool(True)
        assert mempool[unbroadcasted_tx_hash]['unbroadcast']
        self.stop_node(1)

        self.log.info("Move mempool.dat from new to old node")
        os.rename(new_node_mempool, old_node_mempool)

        self.log.info("Start old node again and verify mempool contains both txs")
        # Keep -mempoolexpiry on this restart too: passing extra_args to start_node
        # replaces the node's configured args, so it must be repeated here or the
        # old-mocktime txs expire on load again.
        self.start_node(0, ['-nowallet', '-mempoolexpiry=999999999'])
        assert old_tx_hash in old_node.getrawmempool()
        assert unbroadcasted_tx_hash in old_node.getrawmempool()


if __name__ == "__main__":
    MempoolCompatibilityTest().main()
