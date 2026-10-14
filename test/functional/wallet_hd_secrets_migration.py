#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the move of a wallet's seed into its encrypted record at the first unlock.

Earlier versions left the seed of an encrypted wallet made from a mnemonic,
and the mnemonic itself, in clear in the chain record. Such a wallet is
recognised when it is loaded, and the first time it is unlocked the secrets
are encrypted into a record of their own, the chain record is written without
them, the wallet is flagged, and the file is rewritten.

No version that has this change can make such a wallet, so the test builds
one with the wallet tool: the records of a wallet encrypted by this version,
with the flag taken off, the encrypted secrets taken out, and the chain
record put back as it was before encryption. --fixtures=<directory> runs the
same checks on wallet files that earlier versions really made; those are
kept outside the source tree and that run is not part of the test suite.

The node is run twice. The first run makes the wallets and learns their
secrets. The second, which is given no secret and returns none until the
very end, loads and unlocks them. Every file under each wallet directory is
then searched for the mnemonic, the mnemonic passphrase and the seed: while
the wallet is still loaded, after it is unloaded, and after the node has
stopped.
"""

import json
import os
import shutil

from test_framework.messages import hash256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNodeCLI
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

UNLOCK_PHRASE = "the passphrase the wallet is encrypted with"
NEW_UNLOCK_PHRASE = "another passphrase for the wallet"
MNEMONIC_EXTENSION = "a second factor for the mnemonic 0451"
FLAG_HD_SECRETS_ENCRYPTED = 1 << 49
MAGIC = "BITCOIN_CORE_WALLET_DUMP"
# How the wallet is unlocked for the first time.
ROUTES = ("walletpassphrase", "staking only", "walletpassphrasechange")


def record_type(key_hex):
    """The type of a wallet record: the string its key begins with."""
    key = bytes.fromhex(key_hex)
    return key[1:1 + key[0]].decode("ascii", "replace")


class HDSecretsMigrationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=3"]]

    def add_options(self, parser):
        parser.add_argument("--fixtures", dest="fixtures", default=None,
                            help="Directory of wallet files made by earlier versions, to migrate in place of wallets built by the test")
        parser.add_argument("--fixtureline", dest="fixtureline", default="develop", help="Which set of the fixtures: develop or 4.22.9")

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

    def wallets_root(self):
        chain_dir = os.path.join(self.nodes[0].datadir, self.chain)
        return os.path.join(chain_dir, "wallets") if os.path.isdir(os.path.join(chain_dir, "wallets")) else chain_dir

    def dump(self, name):
        """The records of a wallet that is not loaded: the two header lines, and the rest."""
        path = os.path.join(self.options.tmpdir, name + ".walletdump")
        if os.path.exists(path):
            os.unlink(path)
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, 'dump')
        with open(path, encoding="utf8") as f:
            records = [tuple(line.rstrip("\n").split(",")) for line in f]
        assert_equal(records[0][0], MAGIC)
        assert_equal(records[-1][0], "checksum")
        return records[:2], records[2:-1]

    def create_from(self, name, header, records, database):
        path = os.path.join(self.options.tmpdir, name + ".built.walletdump")
        lines = "".join("%s,%s\n" % record for record in header + records)
        with open(path, "w", encoding="utf8") as f:
            f.write(lines)
            f.write("checksum,%s\n" % hash256(lines.encode()).hex())
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, '-format=' + database, 'createfromdump')

    def restart(self):
        node = self.nodes[0]
        mocktime = node.mocktime
        self.restart_node(0, self.extra_args[0])
        node._base_rpc = node.rpc
        node.rpc = node.get_wallet_rpc(self.default_wallet_name)
        if mocktime:
            node.setmocktime(mocktime)

    def build(self, kind, mnemonic_passphrase, database):
        """First run: an encrypted wallet with its secrets in the chain record, as earlier versions left it."""
        node = self.nodes[0]
        name = "%s_%s_%s" % (kind, "mpass" if mnemonic_passphrase else "nompass", database)
        options = dict(wallet_name="source", descriptors=False, wallet_type=kind)
        if mnemonic_passphrase:
            options["mnemonic_passphrase"] = mnemonic_passphrase
        mnemonic = node._base_rpc.createwallet(**options)["mnemonic"]
        source = node.get_wallet_rpc("source")
        info = source.gethdwalletinfo()
        assert_equal(info["mnemonic"], mnemonic)
        given = [source.getnewaddress("", "legacy") for _ in range(2)]
        source.unloadwallet()
        _, clear_records = self.dump("source")
        clear_chain = [r for r in clear_records if record_type(r[0]) == "hdchain"]
        assert_equal(len(clear_chain), 1)
        assert mnemonic.encode() in bytes.fromhex(clear_chain[0][1])

        node._base_rpc.loadwallet("source")
        source.encryptwallet(UNLOCK_PHRASE)
        source.unloadwallet()
        header, records = self.dump("source")
        types = [record_type(r[0]) for r in records]
        assert_equal((types.count("hdsecrets"), types.count("hdchain"), types.count("flags"), types.count("mkey")), (1, 1, 1, 1))
        unmigrated = []
        for key, value in records:
            kind_of_record = record_type(key)
            if kind_of_record == "hdsecrets":
                continue
            if kind_of_record == "hdchain":
                value = clear_chain[0][1]
            if kind_of_record == "flags":
                flags = int.from_bytes(bytes.fromhex(value), "little")
                assert flags & FLAG_HD_SECRETS_ENCRYPTED
                value = (flags & ~FLAG_HD_SECRETS_ENCRYPTED).to_bytes(8, "little").hex()
            unmigrated.append((key, value))
        header[1] = ("format", database)
        self.create_from(name, header, unmigrated, database)

        # A second wallet of the same phrase tells the addresses that come next.
        options.update(wallet_name="twin", mnemonic=mnemonic)
        node._base_rpc.createwallet(**options)
        twin = node.get_wallet_rpc("twin")
        addresses = [twin.getnewaddress("", "legacy") for _ in range(12)]
        assert_equal(addresses[:2], given)
        twin.unloadwallet()
        for scratch in ("source", "twin"):
            shutil.rmtree(os.path.join(self.wallets_root(), scratch))
        return name, dict(kind=kind, database=database, given=2, addresses=addresses,
                          secrets={"mnemonic": mnemonic.encode(), "mnemonic passphrase": mnemonic_passphrase.encode(), "seed": bytes.fromhex(info["hdseed"])})

    def fixtures(self):
        """Wallet files that earlier versions made, copied into the node's wallet directory."""
        root = self.options.fixtures
        line = self.options.fixtureline
        with open(os.path.join(root, "wallets.json"), encoding="utf8") as f:
            known = json.load(f)["wallets"]
        created = {}
        if os.path.exists(os.path.join(root, line, "created.json")):
            with open(os.path.join(root, line, "created.json"), encoding="utf8") as f:
                created = json.load(f)
        wallets = {}
        for name in sorted(os.listdir(os.path.join(root, line))):
            if not os.path.isdir(os.path.join(root, line, name)):
                continue
            facts = created[name] if name in created else known["_".join(name.split("_")[:2])]
            shutil.copytree(os.path.join(root, line, name), os.path.join(self.wallets_root(), name))
            wallets[name] = dict(kind=facts["kind"], database="sqlite" if name.endswith("sqlite") else "bdb", given=facts["given"], addresses=facts["external_addresses"],
                                 secrets={"mnemonic": facts["mnemonic"].encode(), "mnemonic passphrase": facts["mnemonic_passphrase"].encode(), "seed": bytes.fromhex(facts["seed"])})
        return wallets

    def find(self, name, secrets):
        """The files of the wallet's directory that hold one of its secrets."""
        found = []
        directory = os.path.join(self.wallets_root(), name)
        for root, _, files in os.walk(directory):
            for file in sorted(files):
                if file == ".walletlock":
                    continue  # empty, and locked while its wallet is loaded: Windows refuses to read it
                with open(os.path.join(root, file), "rb") as f:
                    content = f.read()
                found += [(what, os.path.relpath(os.path.join(root, file), directory)) for what, secret in secrets.items() if secret and secret in content]
        return found

    def flags(self, wallet):
        return ["hd_secrets_encrypted"] if wallet.getwalletinfo()["hd_secrets_encrypted"] else []

    def run_test(self):
        node = self.nodes[0]
        if self.options.fixtures:
            self.log.info("Wallets made by earlier versions, from %s (%s)" % (self.options.fixtures, self.options.fixtureline))
            wallets = self.fixtures()
        else:
            self.log.info("First run: build encrypted wallets with their secrets in the chain record")
            wallets = dict(self.build(kind, mnemonic_passphrase, database)
                           for kind, mnemonic_passphrase, database in (("bip39", "", "bdb"), ("bip44", MNEMONIC_EXTENSION, "bdb"), ("bip44", "", "sqlite"), ("bip39", MNEMONIC_EXTENSION, "sqlite")))
        for name, facts in wallets.items():
            assert ("mnemonic", "wallet.dat") in self.find(name, facts["secrets"]), name

        self.log.info("Second run: a process that is given no secret")
        self.restart()
        for number, (name, facts) in enumerate(sorted(wallets.items())):
            route = ROUTES[number % len(ROUTES)]
            self.log.info("%s, first unlocked by: %s" % (name, route))
            node._base_rpc.loadwallet(name)
            wallet = node.get_wallet_rpc(name)
            info = wallet.getwalletinfo()
            assert_equal((info["format"], info["hd_type"], info["unlocked_until"]), (facts["database"], facts["kind"], 0))
            assert "hd_secrets_encrypted" not in self.flags(wallet)
            seed_id = info["hdseedid"]

            # A wrong passphrase changes nothing.
            assert_raises_rpc_error(-14, "wallet passphrase entered was incorrect", wallet.walletpassphrase, "not the passphrase", 10)
            assert "hd_secrets_encrypted" not in self.flags(wallet)
            assert ("mnemonic", "wallet.dat") in self.find(name, facts["secrets"])

            passphrase = UNLOCK_PHRASE
            if route == "walletpassphrase":
                wallet.walletpassphrase(passphrase, 60)
            elif route == "staking only":
                wallet.walletpassphrase(passphrase, 60, True)
            else:
                wallet.walletpassphrasechange(passphrase, NEW_UNLOCK_PHRASE)
                passphrase = NEW_UNLOCK_PHRASE
                wallet.walletpassphrase(passphrase, 60)

            # Moved, flagged, and still the wallet it was.
            info = wallet.getwalletinfo()
            assert "hd_secrets_encrypted" in self.flags(wallet)
            assert_equal((info["hd_type"], info["hdseedid"]), (facts["kind"], seed_id))
            if route == "staking only":
                # The seed is not given out in this state, and the wallet was moved all the same.
                assert_raises_rpc_error(-13, "Wallet is unlocked for staking only", wallet.keypoolrefill)
                assert_equal(self.find(name, facts["secrets"]), [])
                wallet.walletlock()
                wallet.walletpassphrase(passphrase, 60)
            wallet.keypoolrefill()
            next_addresses = [wallet.getnewaddress("", "legacy") for _ in range(4)]
            assert_equal(next_addresses, facts["addresses"][facts["given"]:facts["given"] + 4])
            wallet.walletlock()
            assert_raises_rpc_error(-4, "Cannot set a new HD seed on a wallet created from a mnemonic", wallet.sethdseed)
            assert_equal(wallet.getwalletinfo()["hd_type"], facts["kind"])

            # Nothing in any file, with the wallet still loaded.
            assert_equal(self.find(name, facts["secrets"]), [])
            wallet.unloadwallet()
            assert_equal(self.find(name, facts["secrets"]), [])
            facts["passphrase"] = passphrase

        self.log.info("Stop the node, and search again; then read the records")
        self.stop_node(0)
        for name, facts in sorted(wallets.items()):
            assert_equal(self.find(name, facts["secrets"]), [])
            _, records = self.dump(name)
            types = [record_type(r[0]) for r in records]
            assert_equal((types.count("hdsecrets"), types.count("hdsecretspending"), types.count("hdchain")), (1, 0, 1))
            for key, value in records:
                assert_equal([what for what, secret in facts["secrets"].items() if secret and secret in bytes.fromhex(value)], [])
                if record_type(key) == "hdchain":
                    chain = bytes.fromhex(value)
                    # version 3, both counters, the seed id, three empty vectors, the BIP44 switch
                    assert_equal(len(chain), 4 + 4 + 20 + 4 + 3 + 1)
                    assert_equal((chain[:4], chain[32:35], chain[35]), (b"\x03\x00\x00\x00", b"\x00\x00\x00", 1 if facts["kind"] == "bip44" else 0))
                if record_type(key) == "flags":
                    assert int.from_bytes(bytes.fromhex(value), "little") & FLAG_HD_SECRETS_ENCRYPTED

        self.log.info("Third run: the wallets load as they are, and give their secrets when unlocked")
        self.start_node(0)
        node._base_rpc = node.rpc
        node.rpc = node.get_wallet_rpc(self.default_wallet_name)
        for name, facts in sorted(wallets.items()):
            node._base_rpc.loadwallet(name)
            wallet = node.get_wallet_rpc(name)
            assert "hd_secrets_encrypted" in self.flags(wallet)
            assert_equal(wallet.getwalletinfo()["hd_type"], facts["kind"])
            # Locked: an address from the key pool, and nothing that needs the seed.
            wallet.getnewaddress("", "legacy")
            assert_raises_rpc_error(-13, "Please enter the wallet passphrase with walletpassphrase first", wallet.gethdwalletinfo)
            wallet.walletpassphrase(facts["passphrase"], 60, True)
            assert_raises_rpc_error(-13, "Wallet is unlocked for staking only", wallet.gethdwalletinfo)
            wallet.walletlock()
            wallet.walletpassphrase(facts["passphrase"], 60)
            info = wallet.gethdwalletinfo()
            assert_equal(info["mnemonic"].encode(), facts["secrets"]["mnemonic"])
            assert_equal(info["mnemonicpassphrase"].encode(), facts["secrets"]["mnemonic passphrase"])
            assert_equal(bytes.fromhex(info["hdseed"]), facts["secrets"]["seed"])
            wallet.walletlock()
            wallet.unloadwallet()


if __name__ == '__main__':
    HDSecretsMigrationTest().main()
