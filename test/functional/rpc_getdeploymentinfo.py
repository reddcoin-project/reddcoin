#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test getdeploymentinfo: versionbits signalling counts for every deployment,
deployments that aren't scheduled, and bits no deployment uses.

node0 mines blocks with its normal version, which signals the deployments
that are STARTED. node1 mines with -blockversion set to a bit no deployment
uses, so its blocks signal nothing else.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

VB_PERIOD = 144
VB_THRESHOLD = 108
VB_TOP_BITS = 0x20000000
UNKNOWN_BIT = 27
UNKNOWN_VERSION = VB_TOP_BITS | (1 << UNKNOWN_BIT)


class GetDeploymentInfoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # Neither node stakes on its own, so every block comes from generate.
        self.extra_args = [["-staking=0"], ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def info(self):
        return self.nodes[0].getdeploymentinfo()

    def mine(self, node_index, n):
        self.generate(n, node=self.nodes[node_index])
        self.sync_blocks()

    def unknown(self, info):
        return {u["bit"]: (u["count"], u["previous_count"]) for u in info["unknown_bits"]}

    def run_test(self):
        node0 = self.nodes[0]

        self.log.info("node1 mines blocks that set only an unknown bit")
        self.restart_node(1, extra_args=["-staking=0", "-blockversion={}".format(UNKNOWN_VERSION)])
        self.connect_nodes(0, 1)

        self.log.info("Every deployment is listed, including those not scheduled")
        base = self.info()
        height = node0.getblockcount()
        assert_equal(base["height"], height)
        assert_equal(base["hash"], node0.getbestblockhash())
        period = base["period"]
        assert_equal(period["length"], VB_PERIOD)
        assert_equal(period["threshold"], VB_THRESHOLD)
        # The period holding the next block started at the last multiple of VB_PERIOD.
        assert_equal(period["elapsed"], height + 1 - (height + 1) // VB_PERIOD * VB_PERIOD)
        assert_equal(period["end_height"], ((height + 1) // VB_PERIOD + 1) * VB_PERIOD - 1)
        deps = base["deployments"]
        for name in ("testdummy", "heightincb"):
            assert_equal(deps[name]["status"], "not_scheduled")
            assert "start_time" not in deps[name] and "since" not in deps[name]
        assert_equal(deps["testdummy"]["bit"], 28)
        csv = deps["csv"]
        assert_equal(csv["status"], "started")
        assert_equal(csv["bit"], 2)
        # Every block of this period so far signalled csv, which is STARTED.
        assert_equal(csv["count"], period["elapsed"])
        assert csv["possible"]
        assert_equal(base["unknown_bits"], [])

        self.log.info("Blocks setting an unknown bit are counted for that bit only")
        self.mine(1, 10)
        info = self.info()
        assert_equal(info["period"]["elapsed"], period["elapsed"] + 10)
        assert_equal(info["deployments"]["csv"]["count"], csv["count"])
        assert_equal(self.unknown(info), {UNKNOWN_BIT: (10, 0)})

        self.log.info("Blocks with the normal version count for the started deployments")
        self.mine(0, 5)
        info = self.info()
        assert_equal(info["deployments"]["csv"]["count"], csv["count"] + 5)
        assert_equal(self.unknown(info), {UNKNOWN_BIT: (10, 0)})
        before_reorg = info

        self.log.info("A reorg updates the counts")
        first_new_block = node0.getblockhash(height + 1)
        node0.invalidateblock(first_new_block)
        info = self.info()
        assert_equal(info["height"], height)
        assert_equal(info["period"]["elapsed"], period["elapsed"])
        assert_equal(info["deployments"]["csv"]["count"], csv["count"])
        assert_equal(info["unknown_bits"], [])
        node0.reconsiderblock(first_new_block)
        self.wait_until(lambda: node0.getbestblockhash() == before_reorg["hash"])
        assert_equal(self.info(), before_reorg)

        self.log.info("Once too many blocks miss the bit, the threshold can't be reached this period")
        info = self.info()
        missed = info["period"]["elapsed"] - info["deployments"]["csv"]["count"]
        self.mine(1, VB_PERIOD - VB_THRESHOLD - missed)
        assert self.info()["deployments"]["csv"]["possible"]
        self.mine(1, 1)
        info = self.info()
        assert not info["deployments"]["csv"]["possible"]
        signalled_unknown = info["unknown_bits"][0]["count"]

        self.log.info("At a new period the last one's counts move to previous_count")
        self.mine(0, info["period"]["end_height"] - info["height"])
        info = self.info()
        assert_equal(info["period"]["elapsed"], 0)
        assert_equal(info["period"]["previous_elapsed"], VB_PERIOD)
        assert_equal(self.unknown(info), {UNKNOWN_BIT: (0, signalled_unknown)})
        assert_equal(info["deployments"]["csv"]["status"], "started")
        assert_equal(info["deployments"]["csv"]["count"], 0)
        assert_equal(info["deployments"]["csv"]["previous_count"], VB_PERIOD - signalled_unknown)

        self.log.info("A full period of signalling locks the deployment in, with its activation height")
        self.mine(0, VB_PERIOD)
        info = self.info()
        csv = info["deployments"]["csv"]
        assert_equal(csv["status"], "locked_in")
        assert_equal(csv["since"], info["height"] + 1)
        assert_equal(csv["activation_height"], info["height"] + 1 + VB_PERIOD)
        assert_equal(csv["previous_count"], VB_PERIOD)
        assert "activation_time" in csv and "possible" not in csv

        self.log.info("Once active, the deployment reports since when")
        activation_height = csv["activation_height"]
        self.mine(0, VB_PERIOD)
        info = self.info()
        csv = info["deployments"]["csv"]
        assert_equal(csv["status"], "active")
        assert_equal(csv["since"], activation_height)
        assert_equal(csv["activation_height"], activation_height)
        assert "activation_time" not in csv
        assert_equal(info["unknown_bits"], [])


if __name__ == '__main__':
    GetDeploymentInfoTest().main()
