#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the abortrescan wallet RPC.

abortrescan stops a wallet rescan started by another RPC, such as
importprivkey with rescan set. It is inherited from Bitcoin rather than
Reddcoin's own, but nothing in the suite called it, so the coverage gate
reported it alongside the Reddcoin RPCs that genuinely had no test.

What is asserted here is the "nothing to abort" contract, which is the
deterministic half: with no rescan in progress the RPC reports false rather
than raising, and it does so whether or not a rescan has ever run.

The true branch is not asserted. Reaching it needs a rescan still running when
the call arrives, and a regtest chain is scanned in milliseconds, so there is
no window to hit. Widening it would mean either an artificially large chain or
racing a background thread against the scan, and a test that usually wins a
race is worse than one that does not try: it passes for the wrong reason and
fails for no reason. Upstream has no dedicated test for this RPC either.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class AbortRescanTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("With no rescan running, abortrescan reports false")
        assert_equal(node.abortrescan(), False)

        self.log.info("The wallet is not left in a scanning state by the call")
        assert_equal(node.getwalletinfo()["scanning"], False)

        self.log.info("It still reports false after a rescan has run to completion")
        # rescanblockchain returns once the scan is done, so by the time the
        # next call is made there is again nothing in progress. This covers the
        # IsScanning() check against a wallet that has scanned, not just a fresh
        # one that never has.
        node.rescanblockchain()
        assert_equal(node.getwalletinfo()["scanning"], False)
        assert_equal(node.abortrescan(), False)

        self.log.info("Repeated calls stay false rather than becoming an error")
        assert_equal(node.abortrescan(), False)
        assert_equal(node.abortrescan(), False)

        self.log.info("Without a loaded wallet it is refused, not answered")
        # The test framework addresses the node through the wallet endpoint, so
        # unloading makes the endpoint itself unresolvable rather than reaching
        # the RPC and finding no wallet. Either way it is refused, which is the
        # contract worth holding: this never answers false for a missing wallet.
        wallet = self.default_wallet_name
        node.unloadwallet(wallet)
        assert_raises_rpc_error(-18, "Requested wallet does not exist or is not loaded", node.abortrescan)
        node.loadwallet(wallet)
        assert_equal(node.abortrescan(), False)


if __name__ == "__main__":
    AbortRescanTest().main()
