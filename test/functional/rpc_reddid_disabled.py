#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ReddID commands on a node that does not run ReddID.

ReddID is off by default. The 41 commands are registered all the same, so
that help describes them, and each of them reports that ReddID is not running
instead of doing anything. getreddidnetworkinfo is the exception: it reports
that the service is not enabled.

The 20 commands that use a wallet are registered with the wallet: a node that
runs with -disablewallet does not have them, and has the other 21.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

AUCTION = "ab" * 32
NOT_RUNNING = "ReddID manager not initialized"

# Each command with arguments that it would accept
QUERY_COMMANDS = {
    "getnamespacelist": [],
    "getnamespaceinfo": ["redd"],
    "getnamespaceauctionlist": [],
    "getnamespaceauctioninfo": [AUCTION],
    "finalizenamespacesauction": [AUCTION],
    "getnamespacesauctions": [],
    "getnamespacesauctionbids": [AUCTION],
    "getuseridlist": ["redd"],
    "getuseridinfo": ["alice", "redd"],
    "getuserauctionlist": [],
    "getuserauctioninfo": [AUCTION],
    "finalizeuseridauction": [AUCTION],
    "getuseridauctions": [],
    "getuseridauctionbids": [AUCTION],
    "getuserconnections": ["alice"],
    "getuserreputation": ["alice"],
    "getuserreputationhistory": ["alice"],
    "reddidgetdepositlock": [AUCTION, 1],
    "reddidgetauctionconfig": [],
    "reddidsetauctionconfig": ["userid", "depositPercentage", 20],
}
WALLET_COMMANDS = {
    "createnamespacesauction": ["redd", 50000, 14],
    "bidonnamespacesauction": [AUCTION, 60000],
    "cancelnamespacesauction": [AUCTION],
    "configurenamespacess": ["redd", {}],
    "renewnamespace": ["redd"],
    "transfernamespace": ["redd", None],
    "createuseridauction": ["alice", "redd", 10000, 7],
    "bidonuseridauction": [AUCTION, 10000],
    "canceluseridauction": [AUCTION],
    "renewuserid": ["alice", "redd"],
    "transferuserid": ["alice", "redd", None],
    "createuserconnection": ["alice", "bobby", 0],
    "updateuserconnection": ["alice", "bobby", 0],
    "removeuserconnection": ["alice", "bobby"],
    "updateuserreputation": ["alice", {}],
    "calculateuserreputation": ["alice"],
    "reddidplacebid": [AUCTION, 10000],
    "reddidsettleauction": [AUCTION],
    "reddidfinalizeauction": [AUCTION],
    "reddidlistmydeposits": [],
}


class ReddIDDisabledTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with pre-generated blocks

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # The commands take keys from a legacy wallet
        self.skip_if_no_bdb()

    def listed(self, node):
        """The commands that help lists in the Reddid category."""
        lines = node.help().splitlines()
        start = lines.index("== Reddid ==") + 1
        end = next(i for i in range(start, len(lines)) if lines[i].startswith("=="))
        return sorted(line.split(' ', 1)[0] for line in lines[start:end] if line)

    def key_index(self, node):
        """The index of the next key of the wallet, which this call uses up."""
        path = node.getaddressinfo(node.getnewaddress("", "legacy"))['hdkeypath']
        return int(path.split('/')[-1].rstrip("'"))

    def assert_not_running(self, node, commands):
        for name, args in commands.items():
            assert_raises_rpc_error(-32603, NOT_RUNNING, getattr(node, name), *args)

    def run_test(self):
        node = self.nodes[0]
        address = node.getnewaddress("", "legacy")
        for name in ("transfernamespace", "transferuserid"):
            WALLET_COMMANDS[name][-1] = address
        assert_equal(len(QUERY_COMMANDS) + 1 + len(WALLET_COMMANDS), 41)

        self.log.info("help lists the 41 commands on a node that does not run ReddID")
        assert_equal(self.listed(node), sorted(["getreddidnetworkinfo"] + list(QUERY_COMMANDS) + list(WALLET_COMMANDS)))
        for name in self.listed(node):
            assert node.help(name).startswith(name), name

        self.log.info("getreddidnetworkinfo reports that ReddID is not enabled")
        assert_equal(node.getreddidnetworkinfo(), {"enabled": False, "connections": 0, "peers": []})

        self.log.info("Every other command reports that ReddID is not running")
        before = self.key_index(node)
        self.assert_not_running(node, QUERY_COMMANDS)
        self.assert_not_running(node, WALLET_COMMANDS)
        # No command took a key from the wallet or created the database
        assert_equal(self.key_index(node), before + 1)
        assert not os.path.exists(os.path.join(node.datadir, self.chain, 'reddid'))
        assert_equal(node.getrawmempool(), [])

        self.log.info("The arguments are still checked first")
        assert_raises_rpc_error(-1, "getnamespaceinfo \"namespaceid\"", node.getnamespaceinfo)
        assert_raises_rpc_error(-1, "reddidplacebid \"auction_id\" bid_amount", node.reddidplacebid, AUCTION)

        self.log.info("Without a wallet the node has the 21 commands that need none")
        self.restart_node(0, extra_args=["-disablewallet"])
        assert_equal(self.listed(node), sorted(["getreddidnetworkinfo"] + list(QUERY_COMMANDS)))
        assert_equal(node.getreddidnetworkinfo(), {"enabled": False, "connections": 0, "peers": []})
        self.assert_not_running(node, QUERY_COMMANDS)
        for name, args in WALLET_COMMANDS.items():
            assert_raises_rpc_error(-32601, "Method not found", getattr(node, name), *args)

        self.log.info("With ReddID and without a wallet those 21 commands work")
        self.restart_node(0, extra_args=["-disablewallet", "-reddid=1"])
        assert_equal(self.listed(node), sorted(["getreddidnetworkinfo"] + list(QUERY_COMMANDS)))
        assert_equal(node.getreddidnetworkinfo(), {"enabled": True, "connections": 0, "peers": []})
        assert_equal(node.getnamespacelist(), [])
        assert_equal(node.getnamespacesauctions(), [])
        assert_equal(node.getuserauctionlist(), [])
        assert_equal(node.reddidgetauctionconfig()['config_id'], "userid")
        assert_raises_rpc_error(-8, "Namespace not found", node.getnamespaceinfo, "redd")
        assert_raises_rpc_error(-8, "Auction not found", node.finalizeuseridauction, AUCTION)
        assert_raises_rpc_error(-8, "Deposit lock not found", node.reddidgetdepositlock, AUCTION, 1)
        for name, args in WALLET_COMMANDS.items():
            assert_raises_rpc_error(-32601, "Method not found", getattr(node, name), *args)


if __name__ == '__main__':
    ReddIDDisabledTest().main()
