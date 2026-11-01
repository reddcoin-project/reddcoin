#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the checkupdates RPC.

checkupdates asks api.github.com for the latest release and compares it against
the running version, so unlike every other RPC in the suite its result depends
on whether the machine running the test can reach the internet.

That is exactly why it is worth a test that does not care. node::CheckForUpdates
wraps the whole exchange in a try block with a bounded deadline and reports what
went wrong in the errors field, so the RPC is contractually obliged to return
the same fully-populated object either way. The assertions below hold offline,
and tighten to check the version comparison only when a response actually
arrived. A network failure must never propagate out as an RPC error, and must
never take longer than the fetch deadline.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# node::CheckForUpdates gives the whole exchange UPDATE_CHECK_TIMEOUT (10s),
# with generous headroom here for a loaded CI machine.
FETCH_DEADLINE = 60

STR_FIELDS = [
    "localversion",
    "remoteversion",
    "message",
    "warning",
    "officialDownloadLink",
    "errors",
]


class CheckUpdatesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]

        self.log.info("checkupdates answers whether or not the fetch succeeds")
        start = time.time()
        info = node.checkupdates()
        elapsed = time.time() - start

        assert elapsed < FETCH_DEADLINE, \
            "checkupdates took {:.1f}s, past its own fetch deadline".format(elapsed)

        self.log.info("Every documented field is present and correctly typed")
        for field in STR_FIELDS:
            assert field in info, "missing field {}".format(field)
            assert isinstance(info[field], str), \
                "{} is {}, expected str".format(field, type(info[field]).__name__)
        assert isinstance(info["updateavailable"], bool)

        if info["errors"]:
            # No route to api.github.com, which is the normal case on an
            # isolated CI runner. The contract is that it degrades quietly.
            self.log.info("Fetch failed as %s; checking it degraded quietly", info["errors"])
            assert_equal(info["localversion"], "")
            assert_equal(info["remoteversion"], "")
            assert_equal(info["updateavailable"], False)
            assert_equal(info["officialDownloadLink"], "")
        else:
            self.log.info("Fetch succeeded; checking the version comparison")
            assert info["localversion"], "no local version parsed from a successful fetch"
            assert info["remoteversion"], "no remote version parsed from a successful fetch"
            # updateavailable is set only when the remote version is strictly
            # ahead, so it and an equal-version message cannot both hold.
            if info["updateavailable"]:
                assert info["localversion"] != info["remoteversion"]
                assert info["officialDownloadLink"].startswith("https://download.reddcoin.com/")
            if info["localversion"] == info["remoteversion"]:
                assert_equal(info["updateavailable"], False)
                assert info["message"], "no message for an up-to-date node"

        self.log.info("A second call is consistent with the first")
        # Nothing is cached between calls, so this also covers the fetch path
        # running twice in one process.
        again = node.checkupdates()
        assert_equal(set(again), set(info))

        self.check_downloadupdate(node)

    def check_downloadupdate(self, node):
        """downloadupdate, without ever actually downloading.

        The happy path fetches a 29 MB artifact from download.reddcoin.com, so
        it is deliberately not exercised here: a test suite that pulls a release
        on every run is a bad neighbour to CI and to anyone running the suite on
        a metered connection. It is covered by hand against the live server
        instead.

        What is worth asserting is that the argument is validated before
        anything reaches the network, so a caller who mistypes it gets an error
        rather than a download.
        """
        self.log.info("downloadupdate rejects an unknown artifact, without fetching")
        start = time.time()
        assert_raises_rpc_error(-8, 'artifact must be "daemon" or "gui"',
                                node.downloadupdate, "nonsense")
        elapsed = time.time() - start

        # A network round trip could not fit in this, so the rejection provably
        # happened before one was attempted.
        assert elapsed < 2, \
            "downloadupdate took {:.1f}s to reject an argument, so it reached the network first".format(elapsed)

        self.log.info("downloadupdate is documented")
        assert "daemon" in node.help("downloadupdate")


if __name__ == "__main__":
    CheckUpdatesTest().main()
