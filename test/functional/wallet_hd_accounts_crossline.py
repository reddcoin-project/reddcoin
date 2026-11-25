#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a wallet with HD accounts moves between develop and 4.22.9 builds.

Node 0 runs the build under test and node 1 the other line's build, named by
--otherline (or OTHERLINE_BUILDDIR); without it the test is skipped. The two
nodes share one chain. The same file is on both lines, so running it on each
line against a build of the other covers both directions.

- Moving: for each HD type, plain and encrypted, a wallet with accounts made
  on one node is loaded on the other. The accounts, balances and wallet
  fields are the same, and so is the next receive and change address of each
  account, in two forms. An account is then spent from on the other node and
  the wallet is moved back, and everything is compared again.
- A descriptor wallet (SQLite) moves both ways and keeps account 0 only.
- Restoring: a phrase is restored on the other node, a Japanese one with a
  passphrase given composed on one node and decomposed on the other, and a
  bip32 seed is restored with sethdseed and rescanhdaccounts. The same accounts
  and coins are found.
- Shared vectors: data/hd_account_vectors.json, the same file on both lines,
  holds addresses derived independently of either build. Both nodes derive
  them.
"""

import json
import os
import shutil
import unicodedata
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

PASSPHRASE = "the passphrase of the wallet"
VECTORS = os.path.join(os.path.dirname(os.path.realpath(__file__)), "data", "hd_account_vectors.json")
# The fields of getwalletinfo that HD accounts add, the same on both lines.
WALLET_FIELDS = ("hd_type", "hd_accounts", "walletversion", "hd_accounts_inconsistent", "hd_restore_scan_owed")
LEGACY_ONLY = "HD accounts are available in legacy wallets only. Use a separate wallet to keep funds apart."


class WalletHDAccountsCrossLineTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [["-staking=0"], ["-staking=0"]]

    def add_options(self, parser):
        parser.add_argument("--skip-move", dest="skip_move", default=False, action="store_true",
                            help="Leave out the moving tests, for a quicker run of the restore and vector tests")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()
        self.skip_if_no_otherline()

    def setup_nodes(self):
        for args in self.extra_args:
            args.append("-txindex=1")
        self.add_nodes(self.num_nodes, self.extra_args,
                       binary=[self.options.bitcoind, self.otherline_binary("reddcoind")],
                       binary_cli=[self.options.bitcoincli, self.otherline_binary("reddcoin-cli")])
        self.start_nodes()
        for n in self.nodes:
            n.time_sync_callback = self.sync_time
        self.import_deterministic_coinbase_privkeys()

    def run_test(self):
        this, other = self.nodes
        self.log.info("node 0: %s" % this.getnetworkinfo()["subversion"])
        self.log.info("node 1, the other line: %s" % other.getnetworkinfo()["subversion"])
        self.funder = this.get_wallet_rpc(self.default_wallet_name)
        # Account keys are paid in every address type: SegWit first, at 432.
        self.mine(432 - this.getblockcount())

        self.test_vectors()
        if not self.options.skip_move:
            for wallet_type in ("bip32", "bip39", "bip44"):
                for encrypted in (False, True):
                    self.test_move(wallet_type, encrypted)
            if self.is_sqlite_compiled():
                self.test_descriptor_wallet()
        self.test_restore_phrase("bip39", "english", "")
        self.test_restore_phrase("bip44", "japanese", "Ünïcödé pässwörd")
        self.test_restore_bip32()

    # Helpers

    def mine(self, blocks=1):
        self.generate(blocks)
        self.sync_all()

    def wallet_dir(self, node, name):
        """The directory of a wallet: under wallets/ when the node has it, else in the chain's
        directory, as in a datadir that began without wallets/."""
        root = os.path.join(node.datadir, self.chain)
        if os.path.isdir(os.path.join(root, "wallets")):
            root = os.path.join(root, "wallets")
        return os.path.join(root, name)

    def move(self, name, src, dst):
        """Unload a wallet from src and load its files on dst."""
        src.get_wallet_rpc(name).unloadwallet()
        target = self.wallet_dir(dst, name)
        if os.path.exists(target):
            shutil.rmtree(target)
        shutil.copytree(self.wallet_dir(src, name), target)
        dst.loadwallet(name)
        return dst.get_wallet_rpc(name)

    def twin(self, name, src, dst, twin_name):
        """Load on dst a copy of a wallet that stays loaded on src."""
        w = src.get_wallet_rpc(name)
        target = self.wallet_dir(dst, twin_name)
        if os.path.exists(target):
            shutil.rmtree(target)
        os.makedirs(target)
        w.backupwallet(os.path.join(target, "wallet.dat"))
        dst.loadwallet(twin_name)
        return dst.get_wallet_rpc(twin_name)

    def unlock(self, w, encrypted):
        if encrypted:
            w.walletpassphrase(PASSPHRASE, 600)

    def snapshot(self, w):
        """What a wallet's accounts are, independent of the node it is on."""
        info = w.getwalletinfo()
        balances = w.getbalances()
        return {
            "accounts": w.listhdaccounts(),
            "balances": balances["mine"],
            # Absent for a wallet without accounts.
            "account_balances": balances.get("hd_accounts"),
            "unspent": sorted((c["txid"], c["vout"], c.get("hd_account"), c["amount"]) for c in w.listunspent()),
            "info": {field: info[field] for field in WALLET_FIELDS if field in info},
        }

    def assert_same(self, a, b, what):
        for key in a:
            assert_equal((what, key, a[key]), (what, key, b[key]))

    def next_addresses(self, w, accounts):
        """The next receive and change address of each active account, in two forms."""
        out = []
        for account in accounts:
            for form in ("legacy", "bech32"):
                out.append(w.getnewaddress("", form, account))
                out.append(w.getrawchangeaddress(form, account))
        return out

    def compare_next_addresses(self, name, src, dst, encrypted, tag):
        """The wallet on src and a copy of it on dst give the same next addresses."""
        w = src.get_wallet_rpc(name)
        copy = self.twin(name, src, dst, "%s_twin_%s" % (name, tag))
        self.unlock(w, encrypted)
        self.unlock(copy, encrypted)
        active = [a["hd_account"] for a in w.listhdaccounts() if a["state"] == "active"]
        assert_equal(self.next_addresses(copy, active), self.next_addresses(w, active))
        dst.get_wallet_rpc("%s_twin_%s" % (name, tag)).unloadwallet()

    # Tests

    def test_vectors(self):
        self.log.info("Shared vectors: both nodes derive the addresses of data/hd_account_vectors.json")
        vectors = json.load(open(VECTORS, encoding="utf8"))
        for i, wallet in enumerate(vectors["wallets"]):
            for node in self.nodes:
                name = "vectors_%d" % i
                if wallet["wallet_type"] == "bip32":
                    node.createwallet(wallet_name=name, descriptors=False, blank=True)
                    w = node.get_wallet_rpc(name)
                    w.sethdseed(True, wallet["hdseed"])
                else:
                    node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet["wallet_type"],
                                      mnemonic=vectors["phrase"], mnemonic_passphrase=wallet["mnemonic_passphrase"])
                    w = node.get_wallet_rpc(name)
                derived = {}
                for account in (0, 1, 2):
                    if account > 0:
                        first = w.newhdaccount("", True)
                        assert_equal(first["hd_account"], account)
                        derived[(account, False, 0)] = first["address"]
                    else:
                        derived[(0, False, 0)] = w.getnewaddress("", "legacy")
                    derived[(account, False, 1)] = w.getnewaddress("", "legacy", account)
                    derived[(account, True, 0)] = w.getrawchangeaddress("legacy", account)
                for entry in wallet["addresses"]:
                    key = (entry["account"], entry["internal"], entry["index"])
                    assert_equal((name, entry["path"], derived[key]), (name, entry["path"], entry["address"]))
                    assert_equal(w.getaddressinfo(entry["address"])["hdkeypath"], entry["path"])
                node.get_wallet_rpc(name).unloadwallet()

    def test_move(self, wallet_type, encrypted):
        this, other = self.nodes
        name = "move_%s%s" % (wallet_type, "_encrypted" if encrypted else "")
        self.log.info("%s: a wallet with accounts made on node 0" % name)
        this.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type,
                          passphrase=PASSPHRASE if encrypted else "")
        w = this.get_wallet_rpc(name)
        self.unlock(w, encrypted)
        self.funder.sendtoaddress(w.getnewaddress(), 100)
        self.mine()
        first = w.newhdaccount("savings")
        self.funder.sendtoaddress(first["address"], 40)
        self.funder.sendtoaddress(w.getnewaddress("", "bech32", 1), 30)
        self.mine()
        # Account 2, paid; account 3, forced and never paid, and so none kept
        # ahead of it; account 2 retired.
        second = w.newhdaccount("travel")
        self.funder.sendtoaddress(second["address"], 5)
        self.mine()
        w.newhdaccount("unpaid", True)
        w.sethdaccount(2, None, True)
        # Change in account 1.
        w.sendtoaddress(address=self.funder.getnewaddress(), amount=12, hd_account=1)
        self.mine()
        before = self.snapshot(w)
        assert_equal([(a["hd_account"], a["state"]) for a in before["accounts"]],
                     [(0, "active"), (1, "active"), (2, "retired"), (3, "active")])

        self.log.info("%s: the same on node 1, the other line" % name)
        moved = self.move(name, this, other)
        self.assert_same(before, self.snapshot(moved), name + " on node 1")
        self.compare_next_addresses(name, other, this, encrypted, "1")

        self.log.info("%s: spent from account 1 on node 1, then back on node 0" % name)
        self.unlock(moved, encrypted)
        moved.sendtoaddress(address=self.funder.getnewaddress(), amount=7, hd_account=1)
        moved.sethdaccount(2, "trips", False)
        self.mine()
        after = self.snapshot(moved)
        assert_equal(after["account_balances"]["1"]["trusted"] < before["account_balances"]["1"]["trusted"], True)
        back = self.move(name, other, this)
        self.assert_same(after, self.snapshot(back), name + " back on node 0")
        self.compare_next_addresses(name, this, other, encrypted, "0")
        this.get_wallet_rpc(name).unloadwallet()

    def test_descriptor_wallet(self):
        this, other = self.nodes
        name = "move_descriptors"
        self.log.info("A descriptor wallet moves both ways and keeps account 0 only")
        this.createwallet(wallet_name=name, descriptors=True)
        w = this.get_wallet_rpc(name)
        self.funder.sendtoaddress(w.getnewaddress(), 10)
        self.mine()
        before = self.snapshot(w)
        for src, dst in ((this, other), (other, this)):
            w = self.move(name, src, dst)
            self.assert_same(before, self.snapshot(w), name)
            assert_equal([a["hd_account"] for a in w.listhdaccounts()], [0])
            assert_raises_rpc_error(-4, LEGACY_ONLY, w.newhdaccount)
        this.get_wallet_rpc(name).unloadwallet()

    def test_restore_phrase(self, wallet_type, language, passphrase):
        this, other = self.nodes
        name = "phrase_%s_%s" % (wallet_type, language)
        self.log.info("%s: made on node 0, restored on node 1" % name)
        made = this.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type, language=language,
                                 mnemonic_passphrase=unicodedata.normalize("NFC", passphrase))
        phrase = made["mnemonic"]
        if language != "english":
            assert any(ord(c) > 127 for c in phrase)
        w = this.get_wallet_rpc(name)
        self.funder.sendtoaddress(w.getnewaddress(), 20)
        self.mine()
        for account in (1, 2):
            first = w.newhdaccount("")
            assert_equal(first["hd_account"], account)
            self.funder.sendtoaddress(first["address"], account)
            self.mine()
        before = self.snapshot(w)

        # The other node is given the phrase and the passphrase decomposed.
        restored = other.createwallet(wallet_name=name + "_restored", descriptors=False, wallet_type=wallet_type,
                                      mnemonic=unicodedata.normalize("NFD", phrase),
                                      mnemonic_passphrase=unicodedata.normalize("NFD", passphrase))
        assert_equal(restored["hd_restore"]["wallet_type"], wallet_type)
        assert_equal(restored["hd_restore"]["hd_accounts"], [1, 2])
        r = other.get_wallet_rpc(name + "_restored")
        after = self.snapshot(r)
        for key in ("balances", "account_balances", "unspent"):
            assert_equal((name, key, before[key]), (name, key, after[key]))
        # Labels are not in the phrase.
        assert_equal([(a["hd_account"], a["state"], a["trusted"]) for a in before["accounts"]],
                     [(a["hd_account"], a["state"], a["trusted"]) for a in after["accounts"]])
        this.get_wallet_rpc(name).unloadwallet()
        other.get_wallet_rpc(name + "_restored").unloadwallet()

    def test_restore_bip32(self):
        this, other = self.nodes
        name = "seed_bip32"
        self.log.info("%s: a bip32 seed made on node 0, restored on node 1 with sethdseed" % name)
        this.createwallet(wallet_name=name, descriptors=False)
        w = this.get_wallet_rpc(name)
        self.funder.sendtoaddress(w.getnewaddress(), 20)
        self.mine()
        first = w.newhdaccount("")
        self.funder.sendtoaddress(first["address"], 3)
        self.mine()
        before = self.snapshot(w)
        dump = os.path.join(self.options.tmpdir, name + ".dump")
        w.dumpwallet(dump)
        seed = next(line.split()[0] for line in open(dump, encoding="utf8") if "hdseed=1" in line)

        other.createwallet(wallet_name=name + "_restored", descriptors=False, blank=True)
        r = other.get_wallet_rpc(name + "_restored")
        r.sethdseed(True, seed, {"hd_accounts": True})
        r.rescanhdaccounts()
        after = self.snapshot(r)
        for key in ("balances", "account_balances", "unspent"):
            assert_equal((name, key, before[key]), (name, key, after[key]))
        assert_equal([a["hd_account"] for a in after["accounts"] if a["state"] == "active"], [0, 1])
        this.get_wallet_rpc(name).unloadwallet()
        other.get_wallet_rpc(name + "_restored").unloadwallet()
        assert_equal(Decimal(after["balances"]["trusted"]), Decimal(before["balances"]["trusted"]))


if __name__ == '__main__':
    WalletHDAccountsCrossLineTest().main()
