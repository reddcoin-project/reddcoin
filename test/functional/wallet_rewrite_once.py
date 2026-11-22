#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the rewrite of every Berkeley DB wallet once, at its first load.

A Berkeley DB wallet that an earlier version wrote is rewritten when this
version first loads it, and gets a record that says so; it is not rewritten
at the next load. The owner is told while it happens and once afterwards, at
startup in one notice that names all the wallets rewritten. A
file this version makes is marked from the start. SQLite wallets are left
alone. rewritewallet does it again on request.

In the suite the unmarked file comes from the wallet tool's salvage, which
writes a new file without the record. With --unfixed-bindir=<dir> the wallets
are made by an earlier version, encrypted and not, and that version is shown
to read them still after the rewrite.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

UNLOCK_PHRASE = "the passphrase the wallet is encrypted with"
DOING = "Rewriting wallet file %s to remove data left by earlier versions. This is done once. Do not close the program."
DONE = ("The wallet file has been rewritten to remove data that earlier versions left in its unused space, which could "
        "include private keys without encryption, of this wallet or of other wallets the program had loaded. This is "
        "done once. Backups and copies made with earlier versions are not changed and may still contain such data. Make "
        "a new backup and destroy old ones.")


def record_type(key_hex):
    key = bytes.fromhex(key_hex)
    return key[1:1 + key[0]].decode("ascii", "replace")


class WalletRewriteOnceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=10"]]

    def add_options(self, parser):
        parser.add_argument("--unfixed-bindir", dest="unfixed_bindir", default=None,
                            help="Directory with the reddcoind of a version without this change, to make the wallets and read them afterwards")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()
        self.skip_if_no_bdb()

    def wallet_tool(self, *args):
        """Run the wallet tool, the command last; it raises if the tool fails."""
        return self.run_wallet_tool(*args)

    def marked(self, name):
        path = os.path.join(self.options.tmpdir, name + ".dump")
        if os.path.exists(path):
            os.unlink(path)
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, 'dump')
        with open(path, encoding="utf8") as f:
            return any(record_type(line.split(",")[0]) == "rewritten" for line in f if "," in line and not line.startswith(("BITCOIN_CORE", "format", "checksum")))

    def log_lines(self, text):
        with open(os.path.join(self.nodes[0].datadir, self.chain, "debug.log"), encoding="utf8") as f:
            return sum(1 for line in f if text in line)

    def use_binary(self, binary):
        self.stop_node(0)
        self.nodes[0].args[0] = binary
        self.start_node(0, self.extra_args[0])
        assert_equal(self.nodes[0].process.args[0], binary)

    def run_test(self):
        node = self.nodes[0]
        bdb = ["plain", "encrypted"]
        if self.options.unfixed_bindir:
            self.log.info("Wallets made by an earlier version")
            self.use_binary(os.path.join(self.options.unfixed_bindir, "reddcoind"))
        node.createwallet(wallet_name="plain", descriptors=False)
        node.createwallet(wallet_name="encrypted", descriptors=False, passphrase=UNLOCK_PHRASE)
        sqlite = self.is_sqlite_compiled()
        if sqlite:
            node.createwallet(wallet_name="sqlite", descriptors=True)
        given = {}
        for name in bdb + (["sqlite"] if sqlite else []):
            wallet = node.get_wallet_rpc(name)
            given[name] = [wallet.getnewaddress() for _ in range(3)]
            wallet.unloadwallet()
        if self.options.unfixed_bindir:
            self.use_binary(self.options.bitcoind)
        else:
            self.log.info("Files this version makes are marked; salvage writes them anew without the mark")
            for name in bdb:
                assert self.marked(name)
                self.wallet_tool('-wallet=' + name, 'salvage')
        for name in bdb:
            assert not self.marked(name)

        self.log.info("At the first load: rewritten, said while it happens and once afterwards, and marked")
        for name in bdb:
            loaded = node.loadwallet(name)
            assert ("Wallet %s:\n%s" % (name, DONE)) in loaded["warning"], loaded["warning"]
            assert_equal(self.log_lines("[%s] " % name + DOING % name), 1)
            wallet = node.get_wallet_rpc(name)
            for address in given[name]:
                assert_equal(wallet.getaddressinfo(address)["ismine"], True)
            wallet.unloadwallet()
            assert self.marked(name)
        if sqlite:
            assert_equal(node.loadwallet("sqlite")["warning"], "")
            assert_equal(self.log_lines(DOING % "sqlite"), 0)
            node.get_wallet_rpc("sqlite").unloadwallet()

        self.log.info("At the next load: nothing")
        for name in bdb:
            assert_equal(node.loadwallet(name)["warning"], "")
            assert_equal(self.log_lines("[%s] " % name + DOING % name), 1)

        self.log.info("rewritewallet does it again on request, without the passphrase; not for SQLite")
        assert_equal(node.get_wallet_rpc("encrypted").rewritewallet(), {"rewritten": True})
        assert_equal(self.log_lines("[encrypted] Wallet file rewritten on request"), 1)
        if sqlite:
            node.loadwallet("sqlite")
            assert_raises_rpc_error(-4, "only a wallet in the Berkeley DB format needs this", node.get_wallet_rpc("sqlite").rewritewallet)
            node.get_wallet_rpc("sqlite").unloadwallet()
        for name in bdb:
            node.get_wallet_rpc(name).unloadwallet()
            assert self.marked(name)

        if not self.options.unfixed_bindir:
            # (Wallets of an earlier version can give other notices at
            # startup as well; these give only this one.)
            self.log.info("At startup: one notice for all the wallets rewritten, naming them")
            self.stop_node(0)
            for name in bdb:
                self.wallet_tool('-wallet=' + name, 'salvage')
                assert not self.marked(name)
            self.start_node(0, self.extra_args[0] + ["-wallet=" + name for name in bdb])
            for name in bdb:
                assert_equal(self.log_lines("[%s] " % name + DOING % name), 2)
            self.stop_node(0, expected_stderr=os.linesep.join(["Warning: Wallets %s:" % ", ".join(bdb), DONE]))
            for name in bdb:
                assert self.marked(name)
            self.start_node(0, self.extra_args[0])

        if self.options.unfixed_bindir:
            self.log.info("The earlier version still reads them")
            self.use_binary(os.path.join(self.options.unfixed_bindir, "reddcoind"))
            for name in bdb:
                assert_equal(node.loadwallet(name)["warning"], "")
                wallet = node.get_wallet_rpc(name)
                for address in given[name]:
                    assert_equal(wallet.getaddressinfo(address)["ismine"], True)
                if name == "encrypted":
                    wallet.walletpassphrase(UNLOCK_PHRASE, 10)
                wallet.getnewaddress()
                wallet.unloadwallet()


if __name__ == '__main__':
    WalletRewriteOnceTest().main()
