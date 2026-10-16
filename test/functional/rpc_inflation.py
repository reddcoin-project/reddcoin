#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the PoSV inflation RPCs.

- getinflation
- getinflationmultiplier

Both report the inflation measured over the month of blocks ending at a given
height, and getinflationmultiplier additionally reports the reward multiplier
derived from it.

Two things had to be true for the measurement to be possible, and neither was
checked. The measured block needs a predecessor to read a money supply from,
which genesis does not have, and the block a month back has to exist, which it
does not on a chain shorter than the interval. Either one dereferenced a null
CBlockIndex and killed the node, so an ordinary unprivileged `getinflation`
segfaulted a fresh regtest node, or a mainnet node still in early IBD below
height 44541. Both now answer with a neutral measurement instead: no inflation,
and a multiplier that scales the stake reward by one.

The `height` argument was separately dead. Both RPCs tested request.params[1]
while declaring a single parameter, so it read as null on every call and the tip
was measured whatever the caller asked for, with the local height variable
computed and then never used.

Reddcoin regtest caps proof of work at nLastPowHeight (89) and every block above
it must be staked, so this test stays at or below 89, where blocks are cheap to
mine and, more to the point, where the short-chain paths are reachable at all.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

# Reddcoin regtest consensus.nLastPowHeight: the last height that may be mined.
LAST_POW_HEIGHT = 89

# GetInflationAdjustment clamps the multiplier to this range.
MIN_MULTIPLIER = Decimal("0.5")
MAX_MULTIPLIER = Decimal("5")


class InflationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Staking would extend the chain underneath the height assertions.
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_neutral(self, height):
        """A chain too short to measure reports no inflation, and no adjustment.

        Reaching either RPC at these heights used to be fatal, so the assertion
        that matters most here is simply that the node is still answering.
        """
        node = self.nodes[0]

        inflation = node.getinflation(height)
        assert_equal(inflation["height"], height)
        assert_equal(inflation["inflation"], 0)

        multiplier = node.getinflationmultiplier(height)
        assert_equal(multiplier["height"], height)
        assert_equal(multiplier["inflation"], 0)
        assert_equal(multiplier["multiplier"], 1)

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Genesis has no predecessor to measure against")
        assert_equal(node.getblockcount(), 0)
        self.assert_neutral(0)

        self.log.info("Neither does a chain shorter than the measurement interval")
        self.generate(1)
        self.assert_neutral(1)

        self.generate(49)
        assert_equal(node.getblockcount(), 50)
        self.assert_neutral(50)

        self.log.info("At nLastPowHeight the interval closes and the measurement runs")
        self.generate(LAST_POW_HEIGHT - 50)
        assert_equal(node.getblockcount(), LAST_POW_HEIGHT)

        inflation = node.getinflation()
        assert_equal(inflation["height"], LAST_POW_HEIGHT)
        assert isinstance(inflation["inflation"], Decimal)

        multiplier = node.getinflationmultiplier()
        assert_equal(multiplier["height"], LAST_POW_HEIGHT)
        assert_equal(multiplier["inflation"], inflation["inflation"])
        # The multiplier feeds GetProofOfStakeReward, which is consensus, so the
        # documented clamp is the part worth pinning down.
        assert MIN_MULTIPLIER <= multiplier["multiplier"] <= MAX_MULTIPLIER, \
            "multiplier {} outside the documented clamp".format(multiplier["multiplier"])

        self.log.info("An explicit height is measured, not silently replaced by the tip")
        # Before the params[1] fix every one of these came back as the tip, so
        # the echoed height is the regression: asking for 50 answered for 89.
        for height in [0, 1, 50, LAST_POW_HEIGHT]:
            requested = node.getinflation(height)
            expected = LAST_POW_HEIGHT if height == 0 else height
            assert_equal(requested["height"], expected)
            assert_equal(node.getinflationmultiplier(height)["height"], expected)

        # A height of 0 is the documented default and means the tip, so the two
        # spellings have to agree.
        assert_equal(node.getinflation(0), node.getinflation())
        assert_equal(node.getinflationmultiplier(0), node.getinflationmultiplier())

        # ...and asking for a short chain still answers neutrally now that the
        # height is honoured, which is the same path the crash used to take.
        assert_equal(node.getinflation(1)["inflation"], 0)
        assert_equal(node.getinflationmultiplier(1)["multiplier"], 1)

        self.log.info("A height past the tip is rejected")
        tip = node.getblockcount()
        for height in [tip + 1, tip + 1000]:
            assert_raises_rpc_error(
                -8,
                "Target block height {} after current tip {}".format(height, tip),
                node.getinflation,
                height,
            )
            assert_raises_rpc_error(
                -8,
                "Target block height {} after current tip {}".format(height, tip),
                node.getinflationmultiplier,
                height,
            )

        self.log.info("The node survived every one of those calls")
        assert_equal(node.getblockcount(), LAST_POW_HEIGHT)


if __name__ == "__main__":
    InflationTest().main()
