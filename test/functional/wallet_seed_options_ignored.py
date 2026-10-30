#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that -hdseed, -mnemonic and -mnemonicpassphrase are ignored, and said to be.

None of them is read. A node started with them starts, warns about each one,
and a mnemonic wallet it creates has a phrase of its own, no mnemonic
passphrase, and a seed that is not the one given.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

SEED_HEX = "aa" * 64
PHRASE = " ".join(["abandon"] * 23 + ["art"])
WARNING = "Warning: %s is no longer used and is ignored. A new wallet gets its seed from its own mnemonic phrase."


class SeedOptionsIgnoredTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=3"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # Only legacy wallets are created from a mnemonic.
        self.skip_if_no_bdb()

    def run_test(self):
        node = self.nodes[0]
        self.log.info("A node started with the options starts and warns about each")
        options = ["-hdseed=" + SEED_HEX, "-mnemonic=" + PHRASE, "-mnemonicpassphrase=a passphrase"]
        warnings = [WARNING % o for o in ("-hdseed", "-mnemonic", "-mnemonicpassphrase")]
        with node.assert_debug_log(warnings):
            self.restart_node(0, self.extra_args[0] + options)

        for kind in ("bip39", "bip44"):
            self.log.info("A new %s wallet has a phrase and a seed of its own" % kind)
            node.createwallet(wallet_name=kind, descriptors=False, wallet_type=kind)
            hd = node.get_wallet_rpc(kind).gethdwalletinfo()
            assert_equal(len(hd["mnemonic"].split()), 24)
            assert hd["mnemonic"] != PHRASE
            assert_equal(hd["mnemonicpassphrase"], "")
            assert hd["hdseed"] != SEED_HEX

        self.log.info("The warnings are also on standard error")
        self.stop_node(0, expected_stderr="\n".join(warnings))

        self.log.info("Without the options there is no warning")
        with node.assert_debug_log([], unexpected_msgs=["is no longer used and is ignored"]):
            self.start_node(0)


if __name__ == '__main__':
    SeedOptionsIgnoredTest().main()
