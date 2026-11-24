#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a restore from a mnemonic phrase finds the HD accounts of the phrase.

For bip39 and bip44, a wallet with accounts 1 and 2 used, paid in every
address type and with change, is restored from its phrase, without and with
a wallet passphrase. The restore finds both accounts, as BIP44's account
discovery does, one account after the other; it has the same accounts, the
same balances and the same version as the original, and says what it found.

A phrase whose accounts were never used restores a wallet that is not marked
as holding accounts. A restore as the wrong type finds nothing and says
what to check. With hd_accounts false, the accounts are not looked for.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

FEATURE_HD_ACCOUNTS = 4220000
FEATURE_LATEST = 169900
PASSPHRASE = "the passphrase of the restored wallet"
NO_HISTORY = "No history was found. Check the wallet type (bip39 or bip44) and the mnemonic passphrase"


class WalletHDAccountsRestoreTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=100"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def mine(self):
        self.generate(1)

    def accounts(self, wallet):
        return [(a["hd_account"], a["state"], a["trusted"], a["used"]) for a in wallet.listhdaccounts()]

    def run_test(self):
        self.node = self.nodes[0]
        self.funder = self.node.get_wallet_rpc(self.default_wallet_name)
        # Payments to segwit addresses of the accounts are spent too.
        self.generate(432 - self.node.getblockcount())

        for wallet_type in ["bip39", "bip44"]:
            phrase, original = self.make_original(wallet_type)
            for passphrase in ["", PASSPHRASE]:
                self.restore_finds_accounts(wallet_type, phrase, original, passphrase)
            if wallet_type == "bip39":
                self.wrong_type(phrase)
                self.without_looking(phrase, original)
        self.no_accounts()

    def make_original(self, wallet_type):
        self.log.info("%s: a wallet with accounts 1 and 2 used" % wallet_type)
        name = "original_" + wallet_type
        phrase = self.node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type)["mnemonic"]
        w = self.node.get_wallet_rpc(name)
        self.funder.sendtoaddress(w.getnewaddress(), 100)
        self.mine()
        first = w.newhdaccount("one")["address"]
        for address, amount in ((first, 10), (w.getnewaddress("", "bech32", 1), 20), (w.getnewaddress("", "p2sh-segwit", 1), 30)):
            self.funder.sendtoaddress(address, amount)
        self.mine()
        # Change on account 1's change chain.
        w.sendtoaddress(address=self.funder.getnewaddress(), amount=15, hd_account=1)
        self.mine()
        second = w.newhdaccount("two")["address"]
        self.funder.sendtoaddress(second, 7)
        self.mine()
        assert_equal([a for a, _, _, _ in self.accounts(w)], [0, 1, 2, 3])
        return phrase, w

    def restore_finds_accounts(self, wallet_type, phrase, original, passphrase):
        self.log.info("%s: restored%s, both accounts are found" % (wallet_type, " with a wallet passphrase" if passphrase else ""))
        name = "restored_%s_%s" % (wallet_type, "encrypted" if passphrase else "plain")
        result = self.node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type, mnemonic=phrase, passphrase=passphrase)
        report = result["hd_restore"]
        assert_equal(report["wallet_type"], wallet_type)
        assert_equal(report["first_path"], "m/44'/1'/0'/0/0" if wallet_type == "bip44" else "m/0'/0'/0'")
        assert_equal(report["hd_accounts"], [1, 2])
        assert report["used_addresses"] > 0
        assert "warning" not in report
        w = self.node.get_wallet_rpc(name)
        assert_equal(self.accounts(w), self.accounts(original))
        assert_equal(w.getbalances()["hd_accounts"], original.getbalances()["hd_accounts"])
        assert_equal(w.getbalance(), original.getbalance())
        info = w.getwalletinfo()
        assert_equal(info["walletversion"], FEATURE_HD_ACCOUNTS)
        assert_equal(info["hd_accounts"], 2)
        assert_equal(info["hd_restore_scan_owed"], False)
        if passphrase:
            assert_equal(info["unlocked_until"], 0)

    def wrong_type(self, phrase):
        self.log.info("bip39: restored as bip44, nothing is found, and the result says what to check")
        report = self.node.createwallet(wallet_name="wrong_type", descriptors=False, wallet_type="bip44", mnemonic=phrase)["hd_restore"]
        assert report["warning"].startswith(NO_HISTORY)
        assert_equal(report["hd_accounts"], [])
        w = self.node.get_wallet_rpc("wrong_type")
        assert_equal(w.getwalletinfo()["walletversion"], FEATURE_LATEST)

    def without_looking(self, phrase, original):
        self.log.info("bip39: with hd_accounts false, only account 0 is restored")
        report = self.node.createwallet(wallet_name="not_looking", descriptors=False, wallet_type="bip39", mnemonic=phrase, hd_accounts=False)["hd_restore"]
        assert_equal(report["hd_accounts"], [])
        w = self.node.get_wallet_rpc("not_looking")
        info = w.getwalletinfo()
        assert_equal(info["walletversion"], FEATURE_LATEST)
        assert_equal(info["hd_accounts"], 0)
        assert_equal(w.getbalance(), original.getbalance(hd_account=0))

    def no_accounts(self):
        self.log.info("A phrase whose accounts were never used: not marked as holding accounts")
        phrase = self.node.createwallet(wallet_name="plain_original", descriptors=False, wallet_type="bip39")["mnemonic"]
        original = self.node.get_wallet_rpc("plain_original")
        self.funder.sendtoaddress(original.getnewaddress("", "bech32"), 5)
        self.mine()
        report = self.node.createwallet(wallet_name="plain_restored", descriptors=False, wallet_type="bip39", mnemonic=phrase)["hd_restore"]
        assert_equal(report["hd_accounts"], [])
        assert_equal(report["used_addresses"], 1)
        w = self.node.get_wallet_rpc("plain_restored")
        info = w.getwalletinfo()
        assert_equal(info["walletversion"], FEATURE_LATEST)
        assert_equal(info["hd_accounts"], 0)
        assert_equal([a["hd_account"] for a in w.listhdaccounts()], [0])
        assert_equal(w.getbalance(), original.getbalance())
        self.log.info("After a restart, the wallet that was not marked holds no keys of an account")
        self.restart_node(0, self.extra_args[0] + ["-wallet=plain_restored"])
        w = self.node.get_wallet_rpc("plain_restored")
        assert_equal(w.getwalletinfo()["hd_accounts_inconsistent"], False)
        assert_equal(w.getwalletinfo()["walletversion"], FEATURE_LATEST)


if __name__ == '__main__':
    WalletHDAccountsRestoreTest().main()
