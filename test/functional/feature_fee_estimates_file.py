#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test fee_estimates.dat across the change of the estimator's parameters.

The file holds the estimator's buckets, decay and scale, and a node that reads
it keeps them. A file written by a previous release has the old ones, so this
version discards it; and the previous release refuses the file this version
writes, as one that needs a later version.

  - The previous release (node0) writes its file at shutdown.
  - This version (node1) starts on that file, discards it, and writes its own,
    which says it needs FEE_ESTIMATES_VERSION_REQUIRED.
  - This version reads its own file back.
  - The previous release starts on that file, refuses it and runs on.
"""

import os
import shutil
import struct

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# policy/fees.h: the version a file written now needs to be read.
FEE_ESTIMATES_VERSION_REQUIRED = 4220906
# What the previous releases write.
OLD_VERSION_REQUIRED = 42199


class FeeEstimatesFileTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-disablewallet"], ["-disablewallet"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_previous_releases()

    def setup_network(self):
        self.add_nodes(self.num_nodes, extra_args=self.extra_args, versions=[
            self.options.compat_release,  # node0: the previous release
            None,                         # node1: this version
        ])

    def fee_file(self, node):
        return os.path.join(node.datadir, self.chain, "fee_estimates.dat")

    def header(self, node):
        """The version required to read the file, and the version that wrote it."""
        with open(self.fee_file(node), "rb") as f:
            return struct.unpack("<ii", f.read(8))

    def run_test(self):
        old, new = self.nodes

        self.log.info("The previous release writes its fee estimates file")
        self.start_node(0)
        self.stop_node(0)
        required, _ = self.header(old)
        assert_equal(required, OLD_VERSION_REQUIRED)

        self.log.info("This version discards it and writes its own")
        # Started once first, for its debug.log; its file is then replaced.
        self.start_node(1)
        self.stop_node(1)
        shutil.copyfile(self.fee_file(old), self.fee_file(new))
        with new.assert_debug_log([f"incompatible old fee estimation data (non-fatal). Version: {OLD_VERSION_REQUIRED}"]):
            self.start_node(1)
        self.stop_node(1)
        required, _ = self.header(new)
        assert_equal(required, FEE_ESTIMATES_VERSION_REQUIRED)

        self.log.info("This version reads its own file back")
        with new.assert_debug_log([], unexpected_msgs=["fee estimation data", "policy estimator data"]):
            self.start_node(1)
        self.stop_node(1)

        self.log.info("The previous release refuses this version's file, and runs on")
        shutil.copyfile(self.fee_file(new), self.fee_file(old))
        with old.assert_debug_log([f"up-version ({FEE_ESTIMATES_VERSION_REQUIRED}) fee estimate file"]):
            self.start_node(0)
        assert_equal(old.getblockcount(), 0)
        self.stop_node(0)
        required, _ = self.header(old)
        assert_equal(required, OLD_VERSION_REQUIRED)


if __name__ == '__main__':
    FeeEstimatesFileTest().main()
