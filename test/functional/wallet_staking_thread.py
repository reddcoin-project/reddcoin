#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the staking thread's candidate collection and lock-free kernel search.

The staking thread collects the wallet's stakeable coins once per block the
wallet processes, searches them for a kernel without holding the wallet lock,
and takes the lock only to build the block around a kernel it has found. That
build re-checks the kernel against the wallet and the chain, so a coin spent
after the collection is refused rather than staked, and the refusal makes the
thread collect again before its next pass.

Pinned here, through the RPC and log surface the thread has:

- the thread collects the wallet's coins when it starts and again after each
  block it processes
- it stakes blocks on its own, so the split search finds and builds kernels
  end to end
- the search publishes its interval and weight, so getstakinginfo reports
  them while the thread runs
- spending every coin after a collection makes the next kernel a refused one;
  the thread then collects again, finds nothing, and publishes a known zero
  weight instead of staking a spent coin

Mock time is held still between steps, so the thread searches only when the
test moves the clock: each step is one search interval.
"""
import os
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    set_node_times,
)

# One kernel search interval. The thread sleeps well under a second between
# passes on a wallet of this size, so a step per second of real time gives it
# fresh seconds to search on every pass.
SEARCH_STEP = 60


class StakingThreadTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # Staking is switched on deliberately once the test is watching. The
        # thread reserves a destination for its coinstake as it starts and
        # ends if the keypool cannot cover it.
        self.extra_args = [["-staking=0", "-keypool=100"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def log_size(self, node):
        with open(os.path.join(node.datadir, node.chain, "debug.log"), encoding="utf-8") as log:
            log.seek(0, 2)
            return log.tell()

    def log_since(self, node, offset):
        with open(os.path.join(node.datadir, node.chain, "debug.log"), encoding="utf-8") as log:
            log.seek(offset)
            return log.read()

    def step_until(self, predicate, steps=40):
        """Give the thread a search pass at a time, by moving the clock one
        interval, until predicate holds."""
        for _ in range(steps):
            if predicate():
                return True
            set_node_times(self.nodes, self.nodes[0].mocktime + SEARCH_STEP)
            time.sleep(1)
        return predicate()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("The thread collects the wallet's coins when it starts")
        coins = len(node.listunspent())
        assert_greater_than(coins, 0)
        # Taken before the thread exists: its first pass searches a whole
        # interval and on regtest stakes within milliseconds of starting.
        height = node.getblockcount()
        started = self.log_size(node)
        # The node-wide switch first: setstaking only starts a thread while
        # it is on, and the node came up with it off.
        node.staking(True)
        with node.assert_debug_log(["proof-of-stake timeout: ", f"for {coins} UTXOs"], timeout=10):
            node.setstaking(True)

        self.log.info("The thread stakes on its own: the split search finds and builds a kernel")
        assert self.step_until(lambda: node.getblockcount() > height), "the thread staked no block"
        assert "proof-of-stake block found" in self.log_since(node, started)
        after_block = self.log_size(node)
        self.sync_blocks()

        self.log.info("The search publishes its interval and weight while the thread runs")
        info = node.getstakinginfo()
        assert_greater_than(info["search-interval"], 0)
        assert_greater_than(info["totalweight"], 0)

        self.log.info("After the block the thread collects again")
        # It rests after a found block, then collects because the wallet's
        # view of the chain has moved. The clock stays still meanwhile, so
        # the thread searches nothing until the next step.
        self.wait_until(lambda: "stake candidates" in self.log_since(node, after_block), timeout=60)

        self.log.info("A coin spent after the collection is refused at the build, not staked")
        spent = self.log_size(node)
        node.sendtoaddress(self.nodes[1].getnewaddress(), node.getbalance(), "", "", True)
        assert self.step_until(lambda: "spent by the wallet since it was found" in self.log_since(node, spent)), \
            "no kernel on a spent coin was refused"

        self.log.info("The thread collects again, finds nothing to stake, and says so")
        assert self.step_until(lambda: node.getstakinginfo()["totalweight"] == 0), "the empty collection was never published"
        assert "collected 0 stake candidates" in self.log_since(node, spent)
        assert_equal(node.getblockcount(), height + 1)
        assert "not accepted" not in self.log_since(node, spent)

        node.setstaking(False)
        node.staking(False)


if __name__ == "__main__":
    StakingThreadTest().main()
