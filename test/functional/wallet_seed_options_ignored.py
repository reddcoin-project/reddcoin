#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that -hdseed, -mnemonic and -mnemonicpassphrase are said to be ignored.

None of them is read: walletload_tests shows that a new mnemonic wallet gets
its seed from its own phrase with them set. A node started with them starts
and warns about each one, in its log and on standard error, and a node
started without them does not.
"""

from test_framework.test_framework import BitcoinTestFramework

WARNING = "Warning: %s is no longer used and is ignored. A new wallet gets its seed from its own mnemonic phrase."


class SeedOptionsIgnoredTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        self.log.info("A node started with the options starts and warns about each")
        options = ["-hdseed=" + "aa" * 64, "-mnemonic=" + " ".join(["abandon"] * 23 + ["art"]), "-mnemonicpassphrase=a passphrase"]
        warnings = [WARNING % o for o in ("-hdseed", "-mnemonic", "-mnemonicpassphrase")]
        with node.assert_debug_log(warnings):
            self.restart_node(0, self.extra_args[0] + options)
        node.getblockcount()

        self.log.info("The warnings are also on standard error")
        self.stop_node(0, expected_stderr="\n".join(warnings))

        self.log.info("Without the options there is no warning")
        with node.assert_debug_log([], unexpected_msgs=["is no longer used and is ignored"]):
            self.start_node(0)


if __name__ == '__main__':
    SeedOptionsIgnoredTest().main()
