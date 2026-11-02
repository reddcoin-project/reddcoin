#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a change of passphrase leaves no master key the old passphrase opens.

walletpassphrasechange writes the master key, encrypted with the new
passphrase, over the record that held it encrypted with the old one. A
record that is overwritten is not erased: a SQLite file kept the old one in
the unused part of a page, and a Berkeley DB wallet in its log files or in the
unused part of a page, where the old passphrase still opened it, and with it
every private key of the wallet. The file is now rewritten after the change.

The passphrase of a wallet is changed three times, in a wallet whose pages
have split since it was encrypted. After each change every file of its
directory is searched for the encrypted master keys it held before, which the
wallet tool reads from the wallet after each step.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

UNLOCK_PHRASES = ["the first passphrase", "the second passphrase", "the third passphrase", "the fourth passphrase"]
# The key of the record of master key 1: the type, then the id.
MASTER_KEY_RECORD = "046d6b657901000000"


class PassphraseChangeResidueTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()

    def master_key(self, name):
        """The encrypted master key of the wallet, as the wallet tool reads it from the file."""
        path = os.path.join(self.options.tmpdir, name + ".dump")
        if os.path.exists(path):
            os.unlink(path)
        self.run_wallet_tool("-wallet=" + name, "-dumpfile=" + path, "dump")
        with open(path, encoding="utf8") as f:
            values = [line.strip().split(",")[1] for line in f if line.startswith(MASTER_KEY_RECORD + ",")]
        assert_equal(len(values), 1)
        value = bytes.fromhex(values[0])
        # The encrypted key comes first: its length (48), then the key.
        assert_equal(value[0], 48)
        return value[1:49]

    def files_holding(self, directory, keys):
        """For each file of the directory that holds any of the keys, how many it holds."""
        found = {}
        for root, _, files in os.walk(directory):
            for file in files:
                if file == ".walletlock":
                    continue  # empty, and locked while its wallet is loaded: Windows refuses to read it
                path = os.path.join(root, file)
                with open(path, "rb") as f:
                    content = f.read()
                hits = sum(1 for key in keys if key in content)
                if hits:
                    found[os.path.relpath(path, directory)] = hits
        return found

    def run_test(self):
        node = self.nodes[0]
        name = "changed"
        directory = os.path.join(node.datadir, self.chain, name)
        node.createwallet(wallet_name=name, passphrase=UNLOCK_PHRASES[0])
        wallet = node.get_wallet_rpc(name)
        # Keys written after the encryption split the pages: a Berkeley DB
        # file keeps what was moved off a page where it was, the master key too.
        wallet.walletpassphrase(UNLOCK_PHRASES[0], 600)
        wallet.keypoolrefill(1000)
        wallet.walletlock()
        wallet.unloadwallet()
        earlier = [self.master_key(name)]

        for old, new in zip(UNLOCK_PHRASES, UNLOCK_PHRASES[1:]):
            node.loadwallet(name)
            wallet.walletpassphrasechange(old, new)
            assert_raises_rpc_error(-14, "The wallet passphrase entered was incorrect", wallet.walletpassphrase, old, 60)
            wallet.walletpassphrase(new, 600)
            wallet.keypoolrefill(1000 + 500 * UNLOCK_PHRASES.index(new))
            wallet.walletlock()
            wallet.unloadwallet()

            left = self.files_holding(directory, earlier)
            current = self.master_key(name)
            assert current not in earlier
            self.log.info("After the change to '%s': the current master key in %s; one of an earlier passphrase in %s" % (
                new, self.files_holding(directory, [current]), left or "no file"))
            assert_equal(left, {})
            assert self.files_holding(directory, [current])
            earlier.append(current)


if __name__ == '__main__':
    PassphraseChangeResidueTest().main()
