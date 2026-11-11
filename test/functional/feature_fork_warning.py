#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the fork warnings for a chain with more work than ours.

Node 1 mines a longer branch while disconnected from node 0. Node 0 learns of
it in three ways and is checked each time:

- From its peer, as an ordinary reorg: no warning, as the reorg finishes well
  within the 10 minute grace.
- As headers only (submitheader), the shape of the August 2024 fork where side
  chain blocks failed without being marked invalid: the warning comes up once
  the branch has gone unfollowed for 10 minutes, runs -alertnotify once, and
  clears when node 0 follows the branch.
- As headers with an invalidated block: the invalid chain warning comes up at
  once and clears after reconsiderblock.
"""
import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

GRACE = 10 * 60  # HEAVIER_CHAIN_GRACE
UPGRADE_URL = "https://download.reddcoin.com/"
# -alertnotify strips the apostrophe in "node's", so match text without one.
FORK_TEXT = "branches off at block"
NOT_ARRIVED = "its blocks have not arrived or could not be verified"
INVALID = "it contains a block that this node considers invalid"


def heavier_text(fork, theirs, ours):
    return ("A chain with more work than this node's chain branches off at block {} and reaches block {}, but its blocks have not arrived or could not be verified, "
            "so this node stays on its own chain at block {}. This node may be on a minority fork, or the other chain may break rules that this version enforces. "
            "Make sure Reddcoin Core is up to date ({}) and compare your best block with other nodes before relying on recent payments.").format(fork, theirs, ours, UPGRADE_URL)


def invalid_text(fork, theirs, ours):
    return ("A chain with more work than this node's chain branches off at block {} and reaches block {}, but it contains a block that this node considers invalid, "
            "so this node stays on its own chain at block {}. Unless you marked that block invalid yourself, either this version is missing a rule change "
            "(upgrade Reddcoin Core: {}) or the other chain breaks the rules. Compare your best block with other nodes before relying on recent payments.").format(fork, theirs, ours, UPGRADE_URL)


class ForkWarningTest(BitcoinTestFramework):
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
        self.extra_args[0] = self.extra_args[0] + ["-alertnotify=echo %s >> \"" + self.alert_filename + "\""]
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.sync_all()

    def fork_warnings(self):
        """The fork warnings node 0 reports, from getblockchaininfo and getnetworkinfo."""
        node0 = self.nodes[0]
        warnings = node0.getblockchaininfo()["warnings"]
        assert_equal(node0.getnetworkinfo()["warnings"], warnings)
        return [w for w in warnings.split("<hr />") if FORK_TEXT in w] if warnings else []

    def alerts(self, text):
        """How many -alertnotify runs carried text."""
        with open(self.alert_filename, 'r', encoding='utf8') as f:
            return f.read().count(text)

    def wait_for_alerts(self, text, count):
        """-alertnotify runs on its own thread: wait for it, then check it ran no more than count times."""
        self.wait_until(lambda: self.alerts(text) >= count)
        assert_equal(self.alerts(text), count)

    def settle(self):
        # A block download in flight when generate() moves the clock trips the
        # stall timeout, so only mine once no node is waiting for a block.
        self.sync_blocks()
        self.wait_until(lambda: all(not p["inflight"] for n in self.nodes for p in n.getpeerinfo()))

    def fork(self, ours, theirs):
        """Disconnect, mine a short branch on node 0 and a longer one on node 1."""
        self.settle()
        self.disconnect_nodes(0, 1)
        fork_height = self.nodes[0].getblockcount()
        self.generate(ours, node=self.nodes[0], sync_every=1000)
        hashes = self.generate(theirs, node=self.nodes[1], sync_every=1000)
        return fork_height, hashes

    def submit_headers(self, hashes):
        for h in hashes:
            self.nodes[0].submitheader(self.nodes[1].getblockheader(h, False))

    def advance_time(self, seconds):
        for node in self.nodes:
            node.mocktime += seconds
            node.setmocktime(node.mocktime)

    def reconnect(self):
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(self.nodes[0].getbestblockhash(), self.nodes[1].getbestblockhash())

    def run_test(self):
        node0, node1 = self.nodes
        # Leave initial block download: the fork warnings are off during it.
        self.generate(1, node=node0)
        self.settle()
        assert_equal(self.fork_warnings(), [])

        self.log.info("A reorg onto a valid chain with more work raises no warning")
        self.fork(2, 15)
        self.reconnect()
        assert_equal(self.fork_warnings(), [])
        assert_equal(self.alerts(FORK_TEXT), 0)

        self.log.info("Headers of a chain with more work: no warning within the grace period")
        fork_height, hashes = self.fork(2, 15)
        ours = node0.getblockcount()
        self.submit_headers(hashes[:-1])
        tips = {t["hash"]: t["status"] for t in node0.getchaintips()}
        assert_equal(tips[hashes[-2]], "headers-only")
        self.advance_time(GRACE - 60)
        self.submit_headers(hashes[-1:])
        assert_equal(self.fork_warnings(), [])

        self.log.info("The warning comes up once the chain has gone unfollowed for 10 minutes")
        self.advance_time(60)
        hashes += self.generate(1, node=node1, sync_every=1000)
        self.submit_headers(hashes[-1:])
        theirs = node1.getblockcount()
        assert_equal(self.fork_warnings(), [heavier_text(fork_height, theirs, ours)])
        self.wait_for_alerts(NOT_ARRIVED, 1)
        self.log.info("More headers on the same chain update the text without running -alertnotify again")
        hashes += self.generate(1, node=node1, sync_every=1000)
        self.submit_headers(hashes[-1:])
        assert_equal(self.fork_warnings(), [heavier_text(fork_height, theirs + 1, ours)])
        assert_equal(self.alerts(NOT_ARRIVED), 1)

        self.log.info("The warning clears once node 0 follows the chain")
        self.reconnect()
        assert_equal(self.fork_warnings(), [])

        self.log.info("Headers behind an invalidated block raise the invalid chain warning at once")
        fork_height, hashes = self.fork(2, 15)
        ours = node0.getblockcount()
        theirs = node1.getblockcount()
        self.submit_headers(hashes)
        node0.invalidateblock(hashes[0])
        assert_equal(self.fork_warnings(), [invalid_text(fork_height, theirs, ours)])
        self.wait_for_alerts(INVALID, 1)

        self.log.info("reconsiderblock clears it, and the heavier chain warning follows after the grace period")
        node0.reconsiderblock(hashes[0])
        assert_equal(self.fork_warnings(), [])
        self.advance_time(GRACE)
        hashes += self.generate(1, node=node1, sync_every=1000)
        self.submit_headers(hashes[-1:])
        assert_equal(self.fork_warnings(), [heavier_text(fork_height, theirs + 1, ours)])
        # A new fork runs -alertnotify again.
        self.wait_for_alerts(NOT_ARRIVED, 2)

        self.reconnect()
        assert_equal(self.fork_warnings(), [])


if __name__ == '__main__':
    ForkWarningTest().main()
