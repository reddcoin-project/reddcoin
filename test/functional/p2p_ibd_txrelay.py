#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test fee filters during and after IBD."""

from decimal import Decimal

from test_framework.messages import COIN
from test_framework.test_framework import BitcoinTestFramework

# ReddCoin: Fee filter values differ from Bitcoin due to higher DEFAULT_MIN_RELAY_TX_FEE (100000 vs 1000)
# The MAX_FEE_FILTER is the rounded MAX_MONEY value based on bucket boundaries
MAX_FEE_FILTER = Decimal(9452957) / COIN
# ReddCoin: NORMAL_FEE_FILTER is 100x Bitcoin's value (0.001 RDD/kvB vs 0.00001 BTC/kvB)
NORMAL_FEE_FILTER = Decimal(100000) / COIN


class P2PIBDTxRelayTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # ReddCoin: Use -maxtipage=0 to force IBD state at startup since regtest
        # genesis block has a recent timestamp (2022) that would immediately exit IBD
        self.extra_args = [
            ["-minrelaytxfee={}".format(NORMAL_FEE_FILTER), "-maxtipage=0"],
            ["-minrelaytxfee={}".format(NORMAL_FEE_FILTER), "-maxtipage=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Check that nodes set minfilter to MAX_MONEY while still in IBD")
        for node in self.nodes:
            assert node.getblockchaininfo()['initialblockdownload']
            self.wait_until(lambda: all(peer['minfeefilter'] == MAX_FEE_FILTER for peer in node.getpeerinfo()))

        # Come out of IBD by generating a block
        self.nodes[0].generate(1)
        self.sync_all()

        self.log.info("Check that nodes reset minfilter after coming out of IBD")
        for node in self.nodes:
            assert not node.getblockchaininfo()['initialblockdownload']
            self.wait_until(lambda: all(peer['minfeefilter'] == NORMAL_FEE_FILTER for peer in node.getpeerinfo()))


if __name__ == '__main__':
    P2PIBDTxRelayTest().main()
