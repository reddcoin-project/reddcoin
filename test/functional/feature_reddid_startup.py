#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that ReddID is off by default and is switched on with -reddid=1.

A node without -reddid does not advertise the REDDID service and does not
create the reddid directory in its data directory. With -reddid=1 it does
both. With -reddid=0 the service is not advertised. The node starts and stops
cleanly in each configuration.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ReddIDStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with pre-generated blocks

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def reddid_dir(self):
        return os.path.join(self.nodes[0].datadir, self.chain, 'reddid')

    def advertises_reddid(self):
        return 'REDDID' in self.nodes[0].getnetworkinfo()['localservicesnames']

    def run_test(self):
        node = self.nodes[0]
        height = node.getblockcount()

        self.log.info("ReddID is off by default")
        assert not self.advertises_reddid()
        assert not os.path.exists(self.reddid_dir())

        self.log.info("-reddid=1 starts ReddID")
        self.restart_node(0, extra_args=['-reddid=1'])
        assert self.advertises_reddid()
        assert os.path.isdir(self.reddid_dir())
        assert_equal(node.getblockcount(), height)

        self.log.info("-reddid=0 switches it off again")
        self.restart_node(0, extra_args=['-reddid=0'])
        assert not self.advertises_reddid()
        assert_equal(node.getblockcount(), height)

        self.log.info("Without the option it stays off, with the database of the earlier run on disk")
        self.restart_node(0)
        assert not self.advertises_reddid()
        assert os.path.isdir(self.reddid_dir())
        assert_equal(node.getblockcount(), height)

        self.stop_node(0)


if __name__ == '__main__':
    ReddIDStartupTest().main()
