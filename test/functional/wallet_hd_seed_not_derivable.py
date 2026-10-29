#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a wallet that holds keys of an HD seed it cannot derive from.

sethdseed used to be accepted on a wallet created from a mnemonic. It put a
plain HD seed in the place of the phrase, and what was left was a wallet with
the keys it had already derived from the phrase, and neither the phrase nor a
seed to derive more of them from. Such a wallet failed to load once one of
those keys had been paid, a node started with it did not start, and a payment
to one of them while it was loaded stopped the node.

sethdseed is refused on such wallets now, so the test builds one the way the
old command left it: the records of a wallet with a plain HD seed, with the
keys of a mnemonic wallet added, put together with the wallet tool.

Checked: it loads although its old keys have been paid, a node starts with
it, a payment to an old key while it is loaded is credited and the node stays
up, and the coins on its old keys can be spent. And what it cannot do: it has
no phrase, and it does not see a payment to an address of the old phrase
beyond the keys it holds.
"""

import os

from test_framework.messages import hash256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNodeCLI
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

MAGIC = "BITCOIN_CORE_WALLET_DUMP"
# The records a wallet keeps for a key it holds, and for the label of its address.
KEY_RECORDS = ("key", "keymeta", "name", "purpose")
LOG_LINE = "cannot derive keys of HD seed"


def record_type(key_hex):
    """The type of a wallet record: the string its key begins with."""
    key = bytes.fromhex(key_hex)
    return key[1:1 + key[0]].decode("ascii", "replace")


class HDSeedNotDerivableTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=3"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()
        # Only legacy wallets have an HD seed that can be set.
        self.skip_if_no_bdb()

    def wallet_tool(self, *args):
        """Run reddcoin-wallet on node 0's data directory, the command last; it raises if the tool fails."""
        binary = self.config["environment"]["BUILDDIR"] + '/src/reddcoin-wallet' + self.config["environment"]["EXEEXT"]
        *options, command = args
        TestNodeCLI(binary, self.nodes[0].datadir)('-chain=%s' % self.chain, *options).send_cli(command)

    def dump(self, name):
        """The records of a wallet that is not loaded, as the wallet tool dumps them."""
        path = os.path.join(self.options.tmpdir, name + ".walletdump")
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, 'dump')
        records = []
        with open(path, encoding="utf8") as f:
            for line in f:
                key, value = line.rstrip("\n").split(",")
                records.append((key, value))
        assert_equal(records[0][0], MAGIC)
        assert_equal(records[1], ("format", "bdb"))
        assert_equal(records[-1][0], "checksum")
        return records[:2], records[2:-1]

    def create_from(self, name, header, records):
        path = os.path.join(self.options.tmpdir, name + ".walletdump")
        lines = "".join("%s,%s\n" % record for record in header + records)
        with open(path, "w", encoding="utf8") as f:
            f.write(lines)
            f.write("checksum,%s\n" % hash256(lines.encode()).hex())
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, 'createfromdump')

    def restart(self, extra=None):
        node = self.nodes[0]
        mocktime = node.mocktime
        self.restart_node(0, self.extra_args[0] + (extra or []))
        node._base_rpc = node.rpc
        node.rpc = node.get_wallet_rpc(self.default_wallet_name)
        if mocktime:
            node.setmocktime(mocktime)

    def run_test(self):
        for kind in ("bip44", "bip39"):
            self.test_kind(kind)

    def test_kind(self, kind):
        node = self.nodes[0]
        name = "replaced_" + kind
        self.log.info("A %s wallet and a wallet with a plain HD seed, and the wallet sethdseed would have made of the two" % kind)
        node._base_rpc.createwallet(wallet_name="phrase_" + kind, descriptors=False, wallet_type=kind)
        phrase_wallet = node.get_wallet_rpc("phrase_" + kind)
        phrase = phrase_wallet.gethdwalletinfo()["mnemonic"]
        old = [phrase_wallet.getnewaddress("", "legacy") for _ in range(3)]
        old_seed = phrase_wallet.getwalletinfo()["hdseedid"]
        old_paths = [phrase_wallet.getaddressinfo(a)["hdkeypath"] for a in old]
        assert_equal(old_paths[0], "m/44'/1'/0'/0/0" if kind == "bip44" else "m/0'/0'/0'")
        node._base_rpc.createwallet(wallet_name="plain_" + kind, descriptors=False)
        plain_wallet = node.get_wallet_rpc("plain_" + kind)
        new_seed = plain_wallet.getwalletinfo()["hdseedid"]
        phrase_wallet.unloadwallet()
        plain_wallet.unloadwallet()
        header, plain_records = self.dump("plain_" + kind)
        _, phrase_records = self.dump("phrase_" + kind)
        old_keys = [record for record in phrase_records if record_type(record[0]) in KEY_RECORDS]
        # Three addresses given out and the keys held ready behind them, on both chains.
        assert_greater_than(len([r for r in old_keys if record_type(r[0]) == "key"]), 5)
        assert not [r for r in plain_records if record_type(r[0]) == "hdchain" and len(r[1]) > 200]
        self.create_from(name, header, plain_records + old_keys)

        self.log.info("Its old keys are paid while it is not loaded: it loads all the same")
        # The last address given out: with -keypool=3 fewer than three keys
        # were held ready behind it, so the wallet tries to derive more.
        node.sendtoaddress(old[2], 10)
        node.generate(1)
        with node.assert_debug_log([LOG_LINE, old_seed]):
            node._base_rpc.loadwallet(name)
        wallet = node.get_wallet_rpc(name)
        assert_equal(wallet.getbalance(), 10)
        info = wallet.getwalletinfo()
        # It is a wallet with a plain HD seed now, the new one.
        assert_equal((info["hd_type"], info["hdseedid"]), ("bip32", new_seed))
        for address, path in zip(old, old_paths):
            address_info = wallet.getaddressinfo(address)
            assert_equal((address_info["ismine"], address_info["hdkeypath"], address_info["hdseedid"]), (True, path, old_seed))

        self.log.info("A node starts with it")
        wallet.unloadwallet()
        node.sendtoaddress(old[1], 10)
        node.generate(1)
        self.restart(["-wallet=" + name])
        assert name in node.listwallets()
        wallet = node.get_wallet_rpc(name)
        assert_equal(wallet.getbalance(), 20)

        self.log.info("A payment to an old key while it is loaded is credited, and the node stays up")
        txid = node.sendtoaddress(old[0], 10)
        node.syncwithvalidationinterfacequeue()
        assert_equal(wallet.gettransaction(txid)["details"][0]["address"], old[0])
        node.generate(1)
        node.syncwithvalidationinterfacequeue()
        assert_equal(wallet.getbalance(), 30)
        assert_equal(node.getblockcount(), node.getblockchaininfo()["blocks"])

        self.log.info("The coins on its old keys can be spent")
        wallet.sendtoaddress(node.getnewaddress(), 25)
        node.generate(1)
        assert_greater_than(5, wallet.getbalance())
        assert_greater_than(wallet.getbalance(), 4)
        wallet.unloadwallet()
        node._base_rpc.loadwallet(name)
        assert_greater_than(wallet.getbalance(), 4)

        self.log.info("What it cannot do: it has no phrase, and derives no further key of the old one")
        assert "mnemonic" not in wallet.gethdwalletinfo()
        # New addresses, and the change of the payment above, come from the new seed.
        fresh = wallet.getaddressinfo(wallet.getnewaddress("", "legacy"))
        assert_equal(fresh["hdseedid"], new_seed)
        # A wallet restored from the phrase goes on along the old chain, past
        # the keys the other one holds. A payment there is the phrase's, and
        # the wallet that lost the phrase does not see it.
        node._base_rpc.createwallet(wallet_name="restored_" + kind, descriptors=False, wallet_type=kind, mnemonic=phrase)
        restored = node.get_wallet_rpc("restored_" + kind)
        further = None
        for _ in range(12):
            further = restored.getnewaddress("", "legacy")
        assert_equal(restored.getaddressinfo(old[2])["ismine"], True)
        assert_equal(restored.getaddressinfo(further)["hdseedid"], old_seed)
        assert_equal(wallet.getaddressinfo(further)["ismine"], False)
        before = wallet.getbalance()
        node.sendtoaddress(further, 7)
        node.generate(1)
        node.syncwithvalidationinterfacequeue()
        assert_equal(wallet.getbalance(), before)
        assert_greater_than(restored.getbalance(), 6)
        for loaded in (wallet, restored):
            loaded.unloadwallet()


if __name__ == '__main__':
    HDSeedNotDerivableTest().main()
