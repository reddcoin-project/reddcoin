#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the getinterest wallet RPC.

getinterest sums what every coinstake in the wallet credited above what it
spent, optionally restricted to a [start, end] window of transaction times. It
is how a staker sees what staking has actually earned them, and it had no
coverage at all.

It also did not work. The end argument was read into nTimeStart rather than
nTimeEnd, so passing an end silently moved the *start* instead and the window
never gained an upper bound. Asking for interest earned up to some past moment
returned everything earned since it, which is close to the exact opposite.

The test pins that down with a partition: splitting the chain at an arbitrary
time T, what was earned up to T plus what was earned after T has to add up to
the total. Before the fix both halves ran to the end of time and the sum came
out at roughly twice the total.

The pre-mined cache stakes every block above nLastPowHeight with the default
wallet, so the wallet arrives holding a spread of coinstakes to measure.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# Comfortably past every block time in the test chain, and still inside the
# signed 32-bit range the RPC parses its arguments as.
FAR_FUTURE = 2 ** 31 - 1

# Before any block exists, so an end here excludes every coinstake.
LONG_PAST = 1


class WalletInterestTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # Staking would add coinstakes underneath the totals being compared.
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("The wallet holds the staked coins from the cached chain")
        total = node.getinterest()
        assert isinstance(total, Decimal)
        assert_greater_than(total, 0)

        self.log.info("An explicit start of 0 is the documented default")
        assert_equal(node.getinterest(0), total)

        self.log.info("A window covering the whole chain reports the whole total")
        assert_equal(node.getinterest(0, FAR_FUTURE), total)

        self.log.info("A start past every coinstake reports nothing")
        assert_equal(node.getinterest(FAR_FUTURE), 0)

        self.log.info("An end before every coinstake reports nothing")
        # The regression: this used to move the start instead, so an end in the
        # distant past selected everything rather than nothing.
        assert_equal(node.getinterest(0, LONG_PAST), 0)

        self.log.info("A window splits the total rather than doubling it")
        # Any split point works; the middle of the chain keeps both halves
        # non-trivial so the assertion has something to catch.
        split_height = node.getblockcount() // 2
        split_time = node.getblockheader(node.getblockhash(split_height))["time"]

        up_to_split = node.getinterest(0, split_time)
        after_split = node.getinterest(split_time + 1)
        assert_equal(up_to_split + after_split, total)

        # Both halves should hold some of it, or the split point was useless and
        # the partition above would pass even with the window ignored entirely.
        assert_greater_than(up_to_split, 0)
        assert_greater_than(after_split, 0)

        self.log.info("Widening the window can only include more")
        # A coinstake exactly at split_time belongs to the first half, so moving
        # the end one second later can only pull value in, never drop it.
        assert node.getinterest(0, split_time + 1) >= up_to_split


if __name__ == "__main__":
    WalletInterestTest().main()
