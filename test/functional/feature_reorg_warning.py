#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the deep reorg warning.

Node 1 mines a longer branch while disconnected from node 0, then the nodes
reconnect and node 0 reorgs onto it. A reorg that replaces fewer than
-reorgwarndepth blocks (default 10) raises nothing. One that replaces at least
that many raises a warning in RPC, runs -alertnotify once and logs both tips.
The warning goes away after 24 hours or a restart, and -reorgwarndepth=0
turns it off.
"""
import os
import re

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

DEPTH = 10  # DEFAULT_REORG_WARN_DEPTH
DAY = 24 * 60 * 60
REORG_TEXT = "switched to another chain and replaced its last"


def reorg_text(depth, fork, old, new):
    return ("this node switched to another chain and replaced its last {} blocks: the chains branch off at block {}, and the tip moved from block {} to block {}. "
            "Payments in the replaced blocks may have been reversed or may confirm again later, so check recent payments before relying on them.").format(depth, fork, old, new)


class ReorgWarningTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # Neither node stakes on its own, so every block comes from generate.
        self.extra_args = [["-staking=0"], ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.alert_filename = os.path.join(self.options.tmpdir, "alert.txt")
        with open(self.alert_filename, 'w', encoding='utf8'):
            pass
        self.alert_arg = "-alertnotify=echo %s >> \"" + self.alert_filename + "\""
        self.extra_args[0] = self.extra_args[0] + [self.alert_arg]
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.sync_all()

    def reorg_warnings(self):
        """The deep reorg warnings node 0 reports, from getblockchaininfo and getnetworkinfo."""
        node0 = self.nodes[0]
        warnings = node0.getblockchaininfo()["warnings"]
        assert_equal(node0.getnetworkinfo()["warnings"], warnings)
        return [w for w in warnings.split("<hr />") if REORG_TEXT in w] if warnings else []

    def check_warning(self, depth, fork, old, last):
        """One deep reorg warning is up, with these heights, stamped with the time of the reorg.

        The node switches as soon as the new branch has more work than its old
        tip, before the rest of the branch arrives, so the new tip in the text
        is above the old one and at most the branch's last block. Returns it."""
        warnings = self.reorg_warnings()
        assert_equal(len(warnings), 1)
        match = re.fullmatch(r"At (\S+) .*to block (\d+)\. .*", warnings[0])
        assert match, warnings[0]
        new = int(match.group(2))
        assert old < new <= last, (old, new, last)
        assert_equal(warnings[0], "At {} {}".format(match.group(1), reorg_text(depth, fork, old, new)))
        return new

    def alerts(self):
        """How many -alertnotify runs carried the deep reorg warning."""
        with open(self.alert_filename, 'r', encoding='utf8') as f:
            return f.read().count(REORG_TEXT)

    def wait_for_alerts(self, count):
        """-alertnotify runs on its own thread: wait for it, then check it ran no more than count times."""
        self.wait_until(lambda: self.alerts() >= count)
        assert_equal(self.alerts(), count)

    def settle(self):
        # A block download in flight when generate() moves the clock trips the
        # stall timeout, so only mine once no node is waiting for a block.
        self.sync_blocks()
        self.wait_until(lambda: all(not p["inflight"] for n in self.nodes for p in n.getpeerinfo()))

    def reorg(self, depth):
        """Make node 0 reorg away its last depth blocks onto a longer branch from node 1."""
        node0, node1 = self.nodes
        self.settle()
        self.disconnect_nodes(0, 1)
        fork = node0.getblockcount()
        self.generate(depth, node=node0, sync_every=1000)
        old_tip = node0.getbestblockhash()
        self.generate(depth + 5, node=node1, sync_every=1000)
        new_tip = node1.getbestblockhash()
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), new_tip)
        return fork, fork + depth, fork + depth + 5, old_tip, new_tip

    def restart(self, *args):
        """Restart node 0 with extra arguments, back on the shared clock and connected to node 1."""
        node0 = self.nodes[0]
        self.restart_node(0, extra_args=self.extra_args[0] + list(args))
        node0.setmocktime(node0.mocktime)
        self.connect_nodes(0, 1)
        self.generate(1, node=node0)
        self.settle()

    def advance_time(self, seconds):
        self.settle()
        for node in self.nodes:
            node.mocktime += seconds
            node.setmocktime(node.mocktime)

    def run_test(self):
        node0 = self.nodes[0]
        # Leave initial block download: the warning is off during it.
        self.generate(1, node=node0)
        self.settle()
        assert_equal(self.reorg_warnings(), [])

        self.log.info("A reorg that replaces fewer than 10 blocks raises nothing")
        self.reorg(DEPTH - 1)
        assert_equal(self.reorg_warnings(), [])
        assert_equal(self.alerts(), 0)

        self.log.info("A reorg that replaces 10 blocks raises the warning, runs -alertnotify and logs both tips")
        self.settle()
        fork = node0.getblockcount()
        with node0.assert_debug_log(["Warning: Reorg replaced {} blocks: fork at {}".format(DEPTH, fork)]):
            fork, old, last, old_tip, _ = self.reorg(DEPTH)
        new = self.check_warning(DEPTH, fork, old, last)
        self.wait_for_alerts(1)
        expected_log = "old tip {} at {}, new tip {} at {}".format(old_tip, old, node0.getblockhash(new), new)
        with open(os.path.join(node0.datadir, self.chain, "debug.log"), encoding="utf-8") as f:
            assert expected_log in f.read()

        self.log.info("Later blocks don't run -alertnotify again")
        self.generate(1, node=node0)
        self.settle()
        assert_equal(self.check_warning(DEPTH, fork, old, last), new)
        assert_equal(self.alerts(), 1)

        self.log.info("A restart clears it")
        self.restart()
        assert_equal(self.reorg_warnings(), [])

        self.log.info("It goes away 24 hours after the reorg")
        fork, old, last, _, _ = self.reorg(DEPTH + 2)
        new = self.check_warning(DEPTH + 2, fork, old, last)
        self.wait_for_alerts(2)
        self.advance_time(DAY - 120)
        self.generate(1, node=node0)
        self.settle()
        assert_equal(self.check_warning(DEPTH + 2, fork, old, last), new)
        self.advance_time(120)
        self.generate(1, node=node0)
        self.settle()
        assert_equal(self.reorg_warnings(), [])

        self.log.info("-reorgwarndepth=0 turns it off")
        self.restart("-reorgwarndepth=0")
        self.reorg(DEPTH)
        assert_equal(self.reorg_warnings(), [])
        assert_equal(self.alerts(), 2)

        self.log.info("-reorgwarndepth sets the depth")
        self.restart("-reorgwarndepth=3")
        fork, old, last, _, _ = self.reorg(3)
        self.check_warning(3, fork, old, last)
        self.wait_for_alerts(3)


if __name__ == '__main__':
    ReorgWarningTest().main()
