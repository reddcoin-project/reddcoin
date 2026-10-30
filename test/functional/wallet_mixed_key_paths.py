#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a plain HD wallet with keys of its seed on two kinds of path.

Versions 4.22.7 to 4.22.9.3 could give a wallet created with a passphrase all
its first keys on BIP44 paths (m/44'/coin'/0'/chain/index), from its plain
seed. Later versions derive its new keys on the ordinary paths. The wallet
keeps all its keys, but the seed alone restores only those on the ordinary
paths. Such a wallet is said to have mixed key paths: getwalletinfo reports
it, and the owner is told at load and by getwalletinfo. Nothing about
derivation changes.

The test makes such a wallet from an ordinary one with the wallet tool, by
giving some keys of its seed the BIP44 paths in their metadata: the wallet
created with a passphrase, so that it is found without the passphrase.
Wallets that are not like it are not reported: an ordinary plain wallet, and
a wallet made from a mnemonic, whose keys are all on BIP44 paths.

--fixtures=<dir> runs the same checks on wallets those versions made, in
<dir>/<version>/wallet-created-with-passphrase/ with the passphrase in
<dir>/<version>/facts.json; that run is not part of the test suite.
"""

import json
import os
import shutil
import struct

from test_framework.authproxy import JSONRPCException
from test_framework.messages import hash256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNodeCLI
from test_framework.util import (
    assert_equal,
)

UNLOCK_PHRASE = "the passphrase the wallet is encrypted with"
NOTICE = ("some of this wallet's keys were derived on BIP44 paths by an earlier version, and its new keys are derived on the "
          "wallet's ordinary paths. The HD seed alone does not restore the earlier keys. Only the wallet file holds all of its keys: "
          "keep a backup of the file, or create a new wallet and move the funds to it.")
HARDENED = 0x80000000
RELEASES = ("4.22.7", "4.22.8", "4.22.9", "4.22.9.3")


def record_type(key_hex):
    key = bytes.fromhex(key_hex)
    return key[1:1 + key[0]].decode("ascii", "replace")


def read_compact(b, off):
    n = b[off]
    if n < 253:
        return n, off + 1
    if n == 253:
        return struct.unpack_from("<H", b, off + 1)[0], off + 3
    return struct.unpack_from("<I", b, off + 1)[0], off + 5


def parse_meta(b):
    """CKeyMetadata with key origin: version, time, path text, seed id, fingerprint, path, has origin."""
    version, time = struct.unpack_from("<iq", b, 0)
    off = 12
    n, off = read_compact(b, off)
    text = b[off:off + n].decode()
    off += n
    seed_id = b[off:off + 20]
    off += 20
    fingerprint = b[off:off + 4]
    off += 4
    n, off = read_compact(b, off)
    path = list(struct.unpack_from("<%dI" % n, b, off))
    off += 4 * n
    return dict(version=version, time=time, text=text, seed_id=seed_id, fingerprint=fingerprint, path=path, has_origin=b[off], rest=b[off + 1:])


def build_meta(m):
    text = m["text"].encode()
    return (struct.pack("<iq", m["version"], m["time"]) + bytes([len(text)]) + text + m["seed_id"] + m["fingerprint"] +
            bytes([len(m["path"])]) + struct.pack("<%dI" % len(m["path"]), *m["path"]) + bytes([m["has_origin"]]) + m["rest"])


class WalletMixedKeyPathsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=6"]]

    def add_options(self, parser):
        parser.add_argument("--fixtures", dest="fixtures", default=None,
                            help="Directory of wallets made by versions 4.22.7 to 4.22.9.3, to check in place of the one the test makes")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()
        self.skip_if_no_bdb()

    def wallet_tool(self, *args):
        """Run reddcoin-wallet on node 0's data directory, the command last; it raises if the tool fails."""
        binary = self.config["environment"]["BUILDDIR"] + '/src/reddcoin-wallet' + self.config["environment"]["EXEEXT"]
        *options, command = args
        TestNodeCLI(binary, self.nodes[0].datadir)('-chain=%s' % self.chain, *options).send_cli(command)

    def wallets_root(self):
        chain_dir = os.path.join(self.nodes[0].datadir, self.chain)
        return os.path.join(chain_dir, "wallets") if os.path.isdir(os.path.join(chain_dir, "wallets")) else chain_dir

    def make_mixed(self, name):
        """A wallet created with a passphrase, whose first four receiving keys get BIP44 paths."""
        node = self.nodes[0]
        node.createwallet(wallet_name=name + "_source", descriptors=False, passphrase=UNLOCK_PHRASE)
        source = node.get_wallet_rpc(name + "_source")
        addresses = [source.getnewaddress("", "legacy") for _ in range(4)]
        paths = {a: source.getaddressinfo(a)["hdkeypath"] for a in addresses}
        assert all(p.count("/") == 3 for p in paths.values()), paths
        source.unloadwallet()

        dump = os.path.join(self.options.tmpdir, name + ".walletdump")
        self.wallet_tool('-wallet=' + name + "_source", '-dumpfile=' + dump, 'dump')
        with open(dump, encoding="utf8") as f:
            records = [tuple(line.rstrip("\n").split(",")) for line in f][:-1]
        built = []
        moved = 0
        for key, value in records:
            if key not in ("BITCOIN_CORE_WALLET_DUMP", "format") and record_type(key) == "keymeta":
                meta = parse_meta(bytes.fromhex(value))
                if len(meta["path"]) == 3 and meta["path"][1] == HARDENED and meta["path"][2] - HARDENED < 4:
                    index = meta["path"][2] - HARDENED
                    meta["path"] = [44 | HARDENED, 1 | HARDENED, HARDENED, 0, index]
                    meta["text"] = "m/44'/1'/0'/0/%d" % index
                    value = build_meta(meta).hex()
                    moved += 1
            built.append((key, value))
        assert_equal(moved, 4)
        lines = "".join("%s,%s\n" % record for record in built)
        built_dump = os.path.join(self.options.tmpdir, name + ".built.walletdump")
        with open(built_dump, "w", encoding="utf8") as f:
            f.write(lines)
            f.write("checksum,%s\n" % hash256(lines.encode()).hex())
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + built_dump, 'createfromdump')
        return addresses

    def check_mixed(self, name, passphrase, held=None):
        node = self.nodes[0]
        self.log.info("%s: found and said without the passphrase" % name)
        loaded = node.loadwallet(name)
        assert_equal(loaded["warning"], "Wallet %s: %s" % (name, NOTICE))
        wallet = node.get_wallet_rpc(name)
        info = wallet.getwalletinfo()
        assert_equal((info["mixed_key_paths"], info.get("hd_type", "bip32"), info["unlocked_until"]), (True, "bip32", 0))
        assert_equal(info["hd_warnings"], NOTICE[0].upper() + NOTICE[1:])
        with open(os.path.join(node.datadir, self.chain, "debug.log"), encoding="utf8") as f:
            assert any(("[%s] %s" % (name, NOTICE)) in line for line in f)

        self.log.info("%s: derivation is as before, on the ordinary paths; the earlier keys stay the wallet's" % name)
        wallet.walletpassphrase(passphrase, 60)
        # New keys are derived on the ordinary paths; the keys already in the
        # key pool are given out first, whatever their paths.
        before = self.paths(wallet, name)
        wallet.keypoolrefill(wallet.getwalletinfo()["keypoolsize"] + 3)
        after = self.paths(wallet, name)
        assert_equal(after["bip44"], before["bip44"])
        assert after["ordinary"] >= before["ordinary"] + 3, (before, after)
        given = {wallet.getnewaddress("", "legacy") for _ in range(3)}
        if held:
            assert not given & set(held)
            for address in held:
                info = wallet.getaddressinfo(address)
                assert_equal(info["ismine"], True)
                assert info["hdkeypath"].startswith("m/44'"), info["hdkeypath"]
        wallet.walletlock()
        return wallet

    def paths(self, wallet, name):
        """How many keys of the wallet are on BIP44 paths, and how many on the ordinary ones."""
        dump = os.path.join(self.options.tmpdir, "%s-%d.txt" % (name, len(os.listdir(self.options.tmpdir))))
        wallet.dumpwallet(dump)
        with open(dump, encoding="utf8") as f:
            paths = [line.split("hdkeypath=")[1].split()[0] for line in f if "hdkeypath=" in line]
        return {"bip44": sum(1 for p in paths if p.startswith("m/44'")), "ordinary": sum(1 for p in paths if p.count("/") == 3 and p.startswith("m/0'"))}

    def run_test(self):
        node = self.nodes[0]
        if self.options.fixtures:
            for version in RELEASES:
                directory = os.path.join(self.options.fixtures, version)
                with open(os.path.join(directory, "facts.json"), encoding="utf8") as f:
                    facts = json.load(f)["wallets"]["wallet-created-with-passphrase"]
                name = "release_" + version.replace(".", "_")
                shutil.copytree(os.path.join(directory, "wallet-created-with-passphrase"), os.path.join(self.wallets_root(), name))
                wallet = self.check_mixed(name, facts["wallet_passphrase"])
                wallet.walletpassphrase(facts["wallet_passphrase"], 60)
                dump = os.path.join(self.options.tmpdir, name + ".txt")
                wallet.dumpwallet(dump)
                with open(dump, encoding="utf8") as f:
                    paths = [line.split("hdkeypath=")[1].split()[0] for line in f if "hdkeypath=" in line]
                assert_equal(sum(1 for p in paths if p.startswith("m/44'")), sum(n["n"] for n in facts["dumpwallet_key_paths"].values()))
                wallet.walletlock()
                wallet.unloadwallet()
            return

        held = self.make_mixed("mixed")
        wallet = self.check_mixed("mixed", UNLOCK_PHRASE, held)

        self.log.info("A payment to one of the earlier keys is the wallet's, and the wallet signs for it")
        txid = node.sendtoaddress(held[0], 10)
        node.syncwithvalidationinterfacequeue()
        coin = [u for u in wallet.listunspent(0) if u["txid"] == txid]
        assert_equal([(c["address"], c["amount"]) for c in coin], [(held[0], 10)])
        raw = wallet.createrawtransaction([{"txid": txid, "vout": coin[0]["vout"]}], {node.getnewaddress(): 9.99})
        wallet.walletpassphrase(UNLOCK_PHRASE, 60)
        assert_equal(wallet.signrawtransactionwithwallet(raw)["complete"], True)
        wallet.walletlock()

        self.log.info("Stays so across a reload")
        wallet.unloadwallet()
        assert_equal(node.loadwallet("mixed")["warning"], "Wallet mixed: %s" % NOTICE)
        assert_equal(node.get_wallet_rpc("mixed").getwalletinfo()["mixed_key_paths"], True)

        self.log.info("Other wallets are not reported")
        node.createwallet(wallet_name="plain", descriptors=False, passphrase=UNLOCK_PHRASE)
        others = ["plain"]
        # createwallet makes a mnemonic wallet only where it has the parameter for it.
        try:
            node.createwallet(wallet_name="mnemonic", descriptors=False, wallet_type="bip44")
            others.append("mnemonic")
        except JSONRPCException as e:
            assert_equal(e.error["code"], -8)
        for name in others:
            other = node.get_wallet_rpc(name)
            other.getnewaddress()
            other.unloadwallet()
            assert_equal(node.loadwallet(name)["warning"], "")
            info = node.get_wallet_rpc(name).getwalletinfo()
            assert_equal(info["mixed_key_paths"], False)
            assert "hd_warnings" not in info


if __name__ == '__main__':
    WalletMixedKeyPathsTest().main()
