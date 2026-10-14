#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that an encrypted wallet created from a mnemonic keeps no secret in clear.

Wallets of both mnemonic kinds are made, with and without a mnemonic
passphrase, encrypted when they are created and encrypted afterwards, in a
Berkeley DB file and in an SQLite file. The node is stopped, and every file
under each wallet's directory is searched for the bytes of the mnemonic
phrase, of the mnemonic passphrase and of the seed. The records are read with
the wallet tool and searched for the same, so that each finding is placed: in
the hdchain record, in another record, or in a file outside any record.

The node is run twice. The first run makes the wallets and learns their
secrets over RPC. The second run, a process that is never given and never
returns a secret, encrypts the wallets that are encrypted afterwards, and
uses all of them.

What must hold:
- no secret is in any record of an encrypted wallet;
- the seed bytes are in no file of an encrypted wallet;
- a wallet whose file was written by the second run has no secret anywhere
  in its files.

A wallet encrypted as it is created has its file written by the first run,
whose memory held the phrase in an RPC reply. A phrase found there outside
any record is reported and is not a failure of this test. With
--secret-rpcs the second run also calls the commands that return the
secrets, and nothing may be found anywhere in any file: that is a separate
matter from how the wallet stores its secrets, and the option is not part of
the test suite.

Two controls show that the search finds what is there and passes what is
not: an unencrypted mnemonic wallet, in which the secrets are expected in the
hdchain record, and an encrypted wallet with a plain HD seed, in which the
seed it had before it was encrypted must not be found.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNodeCLI
from test_framework.util import assert_equal

UNLOCK_PHRASE = "the passphrase the wallet is encrypted with"
MNEMONIC_EXTENSION = "a second factor for the mnemonic 0451"
OUTSIDE = "outside any record"


def record_type(key_hex):
    """The type of a wallet record: the string its key begins with."""
    key = bytes.fromhex(key_hex)
    return key[1:1 + key[0]].decode("ascii", "replace")


class MnemonicEncryptedStorageTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=3"]]

    def add_options(self, parser):
        parser.add_argument("--secret-rpcs", dest="secret_rpcs", default=False, action="store_true",
                            help="Also call the commands that return the secrets in the second run, and allow no finding anywhere")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()
        self.skip_if_no_bdb()
        self.skip_if_no_sqlite()

    def wallet_tool(self, *args):
        """Run the wallet tool, the command last: it must succeed and say nothing on stderr."""
        binary = self.config["environment"]["BUILDDIR"] + '/src/reddcoin-wallet' + self.config["environment"]["EXEEXT"]
        *options, command = args
        tool = TestNodeCLI(binary, self.nodes[0].datadir)('-chain=%s' % self.chain, *options)
        tool.send_cli(command)
        assert_equal(tool.stderr, "")

    def wallet_dir(self, name):
        chain_dir = os.path.join(self.nodes[0].datadir, self.chain)
        in_wallets = os.path.join(chain_dir, "wallets", name)
        return in_wallets if os.path.isdir(in_wallets) else os.path.join(chain_dir, name)

    def restart(self):
        node = self.nodes[0]
        mocktime = node.mocktime
        self.restart_node(0, self.extra_args[0])
        node._base_rpc = node.rpc
        node.rpc = node.get_wallet_rpc(self.default_wallet_name)
        if mocktime:
            node.setmocktime(mocktime)

    def make(self, name, kind, mnemonic_passphrase, when, database):
        """First run: create one wallet, and learn the secrets that must not be readable in its files."""
        node = self.nodes[0]
        options = dict(wallet_name=name, descriptors=False)
        if kind != "bip32":
            options["wallet_type"] = kind
        if mnemonic_passphrase:
            options["mnemonic_passphrase"] = mnemonic_passphrase
        if when == "creation":
            options["passphrase"] = UNLOCK_PHRASE
        created = node._base_rpc.createwallet(**options)
        wallet = node.get_wallet_rpc(name)
        if when == "creation":
            wallet.walletpassphrase(UNLOCK_PHRASE, 60)
        info = wallet.gethdwalletinfo()
        secrets = {"seed": bytes.fromhex(info["hdseed"])}
        if kind != "bip32":
            assert_equal(info["mnemonic"], created["mnemonic"])
            secrets["mnemonic"] = info["mnemonic"].encode()
        if mnemonic_passphrase:
            assert_equal(info["mnemonicpassphrase"], mnemonic_passphrase)
            secrets["mnemonic passphrase"] = mnemonic_passphrase.encode()
        wallet.getnewaddress("", "legacy")
        wallet.unloadwallet()
        if database == "sqlite":
            # A legacy wallet is made in a Berkeley DB file. Put its records in an SQLite file.
            dump = os.path.join(self.options.tmpdir, name + ".walletdump")
            self.wallet_tool('-wallet=' + name, '-dumpfile=' + dump, 'dump')
            name = name.replace("_source", "_sqlite")
            self.wallet_tool('-wallet=' + name, '-dumpfile=' + dump, '-format=sqlite', 'createfromdump')
        return name, secrets

    def use(self, name, when, database):
        """Second run: encrypt the wallet if it is to be encrypted now, and use it."""
        node = self.nodes[0]
        if name not in node.listwallets():
            node._base_rpc.loadwallet(name)
        wallet = node.get_wallet_rpc(name)
        assert_equal(wallet.getwalletinfo()["format"], database)
        if when == "afterwards":
            wallet.encryptwallet(UNLOCK_PHRASE)
        if when == "never":
            return
        assert_equal(wallet.getwalletinfo()["unlocked_until"], 0)
        wallet.getnewaddress("", "legacy")
        wallet.walletpassphrase(UNLOCK_PHRASE, 60)
        wallet.keypoolrefill()
        for _ in range(5):
            wallet.getnewaddress("", "legacy")
        if self.options.secret_rpcs:
            wallet.gethdwalletinfo()
            wallet.dumpwallet(os.path.join(self.options.tmpdir, name + ".dumpwallet.txt"))
        wallet.walletlock()

    def search(self, name, secrets):
        """Where the secrets of a wallet are found: (secret, place, file or record, times)."""
        directory = self.wallet_dir(name)
        in_files = []
        for root, _, files in os.walk(directory):
            for file in sorted(files):
                if file == ".walletlock":
                    continue  # empty, and locked while its wallet is loaded: Windows refuses to read it
                with open(os.path.join(root, file), "rb") as f:
                    content = f.read()
                for what, secret in secrets.items():
                    if content.count(secret):
                        in_files.append((what, os.path.relpath(os.path.join(root, file), directory), content.count(secret)))
        # The records, read after the files: the tool opens the database.
        dump = os.path.join(self.options.tmpdir, name + ".stopped.walletdump")
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + dump, 'dump')
        found = []
        in_records = {what: 0 for what in secrets}
        with open(dump, encoding="utf8") as f:
            for line in f:
                key, value = line.rstrip("\n").split(",")
                if key in ("BITCOIN_CORE_WALLET_DUMP", "format", "checksum"):
                    continue
                for what, secret in secrets.items():
                    times = (bytes.fromhex(key) + b"|" + bytes.fromhex(value)).count(secret)
                    if times:
                        kind = record_type(key)
                        found.append((what, "hdchain record" if kind == "hdchain" else "another record", kind, times))
                        in_records[what] += times
        for what, file, times in in_files:
            # The database file holds the records: what it has beyond them is outside any record.
            outside = times - in_records[what] if file == "wallet.dat" else times
            if outside > 0:
                found.append((what, OUTSIDE, file, outside))
        return found

    def run_test(self):
        cases = []
        self.log.info("First run: create the wallets")
        for database in ("bdb", "sqlite"):
            for kind in ("bip39", "bip44"):
                for mnemonic_passphrase in ("", MNEMONIC_EXTENSION):
                    # A legacy wallet in an SQLite file exists only through the
                    # wallet tool, so it cannot be encrypted as it is created.
                    for when in (("creation", "afterwards") if database == "bdb" else ("afterwards",)):
                        name = "%s_%s_%s_%s" % (kind, "mpass" if mnemonic_passphrase else "nompass", when, "bdb" if database == "bdb" else "source")
                        cases.append((when, database) + self.make(name, kind, mnemonic_passphrase, when, database))
        control_clear = ("never", "bdb") + self.make("control_unencrypted_bip44", "bip44", MNEMONIC_EXTENSION, "never", "bdb")
        control_plain = ("afterwards", "bdb") + self.make("control_bip32", "bip32", "", "afterwards", "bdb")

        self.log.info("Second run: a process that is given no secret%s" % (", but returns them" if self.options.secret_rpcs else " and returns none"))
        self.restart()
        for when, database, name, _ in cases + [control_clear, control_plain]:
            self.use(name, when, database)

        self.log.info("Stop the node, and search the files")
        self.stop_node(0)

        found = self.search(*control_clear[2:])
        self.log.info("unencrypted mnemonic wallet: %s" % found)
        # Older copies of the record may also lie outside any record: an
        # unencrypted wallet writes it again for every key it derives.
        assert_equal(sorted((what, place) for what, place, _, _ in found if place != OUTSIDE), [("mnemonic", "hdchain record"), ("mnemonic passphrase", "hdchain record"), ("seed", "hdchain record")])
        found = self.search(*control_plain[2:])
        self.log.info("encrypted wallet with a plain seed, the seed it had before: %s" % found)
        assert_equal(found, [])

        failures = []
        for when, database, name, secrets in cases:
            found = self.search(name, secrets)
            self.log.info("%-36s %s" % (name, found if found else "nothing in clear"))
            for what, place, where, times in found:
                # Tolerated: text that the first run returned over RPC, outside
                # any record of a file which that run wrote.
                if place == OUTSIDE and what != "seed" and when == "creation" and not self.options.secret_rpcs:
                    self.log.info("  not counted: %s %s in %s" % (what, place, where))
                    continue
                failures.append((name, what, place, where, times))
        if failures:
            in_records = [f for f in failures if f[2] != OUTSIDE]
            raise AssertionError("secrets of %d encrypted wallets are readable in their files: %d in records, %d outside any record" % (
                len({f[0] for f in failures}), len(in_records), len(failures) - len(in_records)))

        self.start_node(0)
        self.nodes[0]._base_rpc = self.nodes[0].rpc


if __name__ == '__main__':
    MnemonicEncryptedStorageTest().main()
