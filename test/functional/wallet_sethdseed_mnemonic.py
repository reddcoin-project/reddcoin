#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test sethdseed on wallets created from a mnemonic.

A wallet created from a mnemonic (hd_type bip39 or bip44) derives every key
from its phrase. sethdseed is refused on it and leaves it as it was: the same
type, the same seed, the phrase still there, the next address the next one of
the same chain. On a wallet with a plain HD seed, and on a blank wallet, it
works as before.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

REFUSAL = "Cannot set a new HD seed on a wallet created from a mnemonic"


class SetHDSeedMnemonicTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=3"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # Only legacy wallets have an HD seed to set.
        self.skip_if_no_bdb()

    def create(self, name, **kwargs):
        node = self.nodes[0]
        node._base_rpc.createwallet(wallet_name=name, descriptors=False, **kwargs)
        return node.get_wallet_rpc(name)

    def run_test(self):
        node = self.nodes[0]
        # A key no wallet below has, to offer as a seed.
        donor = self.create("donor")
        seed_key = donor.dumpprivkey(donor.getnewaddress("", "legacy"))

        for kind, paths in (("bip39", ["m/0'/0'/%d'" % i for i in range(3)]), ("bip44", ["m/44'/1'/0'/0/%d" % i for i in range(3)])):
            self.log.info("sethdseed is refused on a %s wallet" % kind)
            wallet = self.create(kind, wallet_type=kind)
            first = wallet.getnewaddress("", "legacy")
            assert_equal(wallet.getaddressinfo(first)["hdkeypath"], paths[0])
            node.sendtoaddress(first, 10)
            node.generate(1)
            info = wallet.getwalletinfo()
            assert_equal(info["hd_type"], kind)
            hd = wallet.gethdwalletinfo()
            assert_equal(len(hd["mnemonic"].split()), 24)
            for args in ([], [True], [False], [True, seed_key], [False, seed_key]):
                assert_raises_rpc_error(-4, REFUSAL, wallet.sethdseed, *args)

            self.log.info("The wallet is as it was")
            after = wallet.getwalletinfo()
            assert_equal((after["hd_type"], after["hdseedid"], after["keypoolsize"], after["balance"]), (kind, info["hdseedid"], info["keypoolsize"], 10))
            assert_equal(wallet.gethdwalletinfo(), hd)
            # The next address is the next one of the same chain.
            second = wallet.getnewaddress("", "legacy")
            assert_equal(wallet.getaddressinfo(second)["hdkeypath"], paths[1])
            assert_equal(wallet.getaddressinfo(second)["hdseedid"], info["hdseedid"])
            dump = os.path.join(self.options.tmpdir, kind + ".dump")
            wallet.dumpwallet(dump)
            with open(dump, encoding="utf8") as f:
                assert "# mnemonic: %s\n" % hd["mnemonic"] in f.read()
            # It loads again, and goes on from where it was.
            wallet.unloadwallet()
            node._base_rpc.loadwallet(kind)
            assert_equal(wallet.gethdwalletinfo(), hd)
            assert_equal(wallet.getaddressinfo(wallet.getnewaddress("", "legacy"))["hdkeypath"], paths[2])
            assert_equal(wallet.getbalance(), 10)

        self.log.info("sethdseed still works on a wallet with a plain HD seed")
        wallet = self.create("bip32")
        info = wallet.getwalletinfo()
        assert_equal(info["hd_type"], "bip32")
        old = wallet.getnewaddress("", "legacy")
        assert_equal(wallet.getaddressinfo(old)["hdkeypath"], "m/0'/0'/0'")
        wallet.sethdseed()
        after = wallet.getwalletinfo()
        assert_equal(after["hd_type"], "bip32")
        assert after["hdseedid"] != info["hdseedid"]
        new = wallet.getnewaddress("", "legacy")
        assert_equal((wallet.getaddressinfo(new)["hdkeypath"], wallet.getaddressinfo(new)["hdseedid"]), ("m/0'/0'/0'", after["hdseedid"]))
        wallet.sethdseed(True, seed_key)
        assert_equal(wallet.getwalletinfo()["hdseedid"], wallet.getaddressinfo(wallet.getnewaddress("", "legacy"))["hdseedid"])
        assert wallet.getwalletinfo()["hdseedid"] not in (info["hdseedid"], after["hdseedid"])
        # Its old addresses go on working: the old seed is a key of the wallet.
        node.sendtoaddress(old, 5)
        node.generate(1)
        assert_equal(wallet.getbalance(), 5)
        wallet.unloadwallet()
        node._base_rpc.loadwallet("bip32")
        assert_equal(wallet.getbalance(), 5)

        self.log.info("And on a blank wallet")
        wallet = self.create("blank", blank=True)
        assert_equal(wallet.getwalletinfo()["hd_type"], "none")
        wallet.sethdseed()
        assert_equal(wallet.getwalletinfo()["hd_type"], "bip32")
        assert_equal(wallet.getaddressinfo(wallet.getnewaddress("", "legacy"))["hdkeypath"], "m/0'/0'/0'")


if __name__ == '__main__':
    SetHDSeedMnemonicTest().main()
