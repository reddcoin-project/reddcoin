#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test HD accounts in legacy wallets.

For each HD type (bip32, bip39, bip44): an account is created only once the
account before it has a confirmed payment, as BIP44's account rule has it,
unless forced; the first one marks the wallet. A payment spends one account
and returns its change to it; inputs picked from two accounts are refused,
and inputs of one account pick it. A fee bump stays in its account. An
account hands out addresses within a gap of 20 unpaid. An account can be
renamed, retired and brought back. A wallet with accounts keeps its seed.
Everything is as it was after a restart.

An encrypted wallet hands out the addresses derived ahead while it is locked,
and derives more once it is unlocked. A descriptor wallet has account 0 only.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

FEATURE_HD_ACCOUNTS = 4220000
PASSPHRASE = "the passphrase of the wallet"
RULE = "has not received a confirmed payment. A new account is created only after the one before it has been paid"
GAP = "The last 20 addresses of this account have not been paid"
UNLOCK = "Unlock the wallet to derive further addresses for this account."
LEGACY_ONLY = "HD accounts are available in legacy wallets only. Use a separate wallet to keep funds apart."


class WalletHDAccountsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def run_test(self):
        self.node = self.nodes[0]
        self.funder = self.node.get_wallet_rpc(self.default_wallet_name)
        # Account keys are paid and spent in every address type: SegWit is
        # activated first, by BIP9, at height 432 (the cache is at 199).
        self.generate(432 - self.node.getblockcount())
        for wallet_type in ["bip32", "bip39", "bip44"]:
            self.test_accounts(wallet_type)
        self.test_restart()
        self.test_locked()
        if self.is_sqlite_compiled():
            self.test_descriptor_wallet()

    def mine(self):
        self.generate(1)

    def account_of(self, wallet, address):
        return wallet.getaddressinfo(address)["hd_account"]

    def input_accounts(self, wallet, txid):
        tx = wallet.decoderawtransaction(wallet.gettransaction(txid)["hex"])
        accounts = set()
        for vin in tx["vin"]:
            prev = wallet.decoderawtransaction(wallet.gettransaction(vin["txid"])["hex"])
            accounts.add(self.account_of(wallet, prev["vout"][vin["vout"]]["scriptPubKey"]["address"]))
        return accounts

    def change_accounts(self, wallet, txid, paid):
        tx = wallet.decoderawtransaction(wallet.gettransaction(txid)["hex"])
        return {self.account_of(wallet, out["scriptPubKey"]["address"]) for out in tx["vout"]
                if out["scriptPubKey"]["address"] != paid and wallet.getaddressinfo(out["scriptPubKey"]["address"])["ismine"]}

    def test_accounts(self, wallet_type):
        name = "accounts_" + wallet_type
        self.log.info("%s: no account before account 0 has a confirmed payment" % wallet_type)
        self.node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type)
        w = self.node.get_wallet_rpc(name)
        assert_raises_rpc_error(-4, "Account 0 " + RULE, w.newhdaccount)
        assert_equal(w.getwalletinfo()["hd_accounts"], 0)
        self.funder.sendtoaddress(w.getnewaddress(), 100)
        self.mine()

        self.log.info("%s: the first account marks the wallet" % wallet_type)
        first = w.newhdaccount("savings")
        assert_equal(first["hd_account"], 1)
        assert_equal(first["label"], "savings")
        assert any("Versions without HD accounts refuse to open it" in warning for warning in first["warnings"])
        assert_equal(any("Encrypt this wallet before creating accounts" in warning for warning in first["warnings"]), wallet_type == "bip32")
        expected_path = "m/44'/1'/1'/0/0" if wallet_type == "bip44" else "m/1'/0'/0'"
        assert_equal(first["hdkeypath"], expected_path)
        info = w.getwalletinfo()
        assert_equal(info["hd_accounts"], 1)
        assert_equal(info["walletversion"], FEATURE_HD_ACCOUNTS)
        assert_equal(info["hd_accounts_inconsistent"], False)
        assert_equal(self.account_of(w, first["address"]), 1)
        assert_equal(w.getaddressinfo(first["address"])["labels"], ["savings"])
        assert_raises_rpc_error(-4, "Account 1 " + RULE, w.newhdaccount)

        self.log.info("%s: payments to the account, in every address type" % wallet_type)
        bech32 = w.getnewaddress("", "bech32", 1)
        p2sh = w.getnewaddress("", "p2sh-segwit", 1)
        for address in (bech32, p2sh):
            assert_equal(self.account_of(w, address), 1)
        for address, amount in ((first["address"], 50), (bech32, 30), (p2sh, 20)):
            self.funder.sendtoaddress(address, amount)
        self.mine()
        assert_equal(w.getbalance(hd_account=1), 100)
        assert_equal(w.getbalance(hd_account=0), 100)
        assert_equal(w.getbalance(), 200)
        balances = w.getbalances()
        assert_equal(balances["hd_accounts"]["1"]["trusted"], 100)
        assert_equal(balances["hd_accounts"]["0"]["trusted"], 100)
        assert_equal(sorted(coin["hd_account"] for coin in w.listunspent()), [0, 1, 1, 1])
        listed = w.listhdaccounts()
        assert_equal([(a["hd_account"], a["state"]) for a in listed], [(0, "active"), (1, "active"), (2, "pool")])
        assert_equal(listed[1]["used"], True)
        assert_equal(listed[1]["trusted"], 100)

        self.log.info("%s: a payment spends one account, and its change goes back to it" % wallet_type)
        to = self.funder.getnewaddress()
        assert_raises_rpc_error(-6, "Insufficient funds in HD account 0. This wallet holds", w.sendtoaddress, to, 150)
        txid = w.sendtoaddress(to, 60)
        assert_equal(self.input_accounts(w, txid), {0})
        assert_equal(self.change_accounts(w, txid, to), {0})
        assert_equal(w.getbalance(minconf=0, hd_account=1), 100)
        txid = w.sendtoaddress(address=to, amount=70, hd_account=1)
        assert_equal(self.input_accounts(w, txid), {1})
        assert_equal(self.change_accounts(w, txid, to), {1})
        txid = w.sendmany(dummy="", amounts={to: 5}, hd_account=1)
        assert_equal(self.input_accounts(w, txid), {1})
        self.mine()

        self.log.info("%s: inputs picked from two accounts are refused; inputs of one pick it" % wallet_type)
        coins = w.listunspent()
        zero = [c for c in coins if c["hd_account"] == 0][0]
        one = [c for c in coins if c["hd_account"] == 1][0]
        raw = w.createrawtransaction([{"txid": zero["txid"], "vout": zero["vout"]}, {"txid": one["txid"], "vout": one["vout"]}], {to: 1})
        assert_raises_rpc_error(-4, "The selected inputs are of HD accounts 0, 1", w.fundrawtransaction, raw)
        raw = w.createrawtransaction([{"txid": one["txid"], "vout": one["vout"]}], {to: 1})
        assert_raises_rpc_error(-4, "A selected input is of HD account 1", w.fundrawtransaction, raw, {"hd_account": 0})
        funded = w.fundrawtransaction(raw)
        decoded = w.decoderawtransaction(funded["hex"])
        change = decoded["vout"][funded["changepos"]]["scriptPubKey"]["address"]
        assert_equal(self.account_of(w, change), 1)
        assert_raises_rpc_error(-4, "The change address is not of HD account 1", w.fundrawtransaction, raw, {"change_address": w.getrawchangeaddress()})

        self.log.info("%s: a fee bump stays in its account" % wallet_type)
        txid = w.sendtoaddress(address=to, amount=3, replaceable=True, hd_account=1)
        bumped = w.bumpfee(txid)["txid"]
        assert_equal(self.input_accounts(w, bumped), {1})
        assert_equal(self.change_accounts(w, bumped, to), {1})
        self.mine()

        self.log.info("%s: the gap of 20 unpaid addresses" % wallet_type)
        # Indexes 0 to 2 are paid; 3 to 22 can be handed out.
        handed_out = 0
        while True:
            try:
                w.getnewaddress("", "bech32", 1)
                handed_out += 1
            except Exception as e:
                assert GAP in str(e), str(e)
                break
        assert_equal(handed_out, 20)
        assert_equal(len(w.listhdaccounts(1)["unpaid_addresses"]), 20)
        forced = w.getnewaddress("", "bech32", 1, True)
        assert_equal(self.account_of(w, forced), 1)
        assert_equal(self.account_of(w, w.getrawchangeaddress("bech32", 1)), 1)

        self.log.info("%s: rename, retire, bring back" % wallet_type)
        assert_equal(w.sethdaccount(1, "renamed")["label"], "renamed")
        assert_equal(w.sethdaccount(1, None, True)["state"], "retired")
        assert_raises_rpc_error(-12, "HD account 1 is retired", w.getnewaddress, "", "bech32", 1)
        assert_equal(w.getbalance(hd_account=1) > 0, True)
        assert_equal(w.sethdaccount(1, None, False)["state"], "active")
        assert_raises_rpc_error(-8, "Account 0 is the wallet's own", w.sethdaccount, 0, "zero")
        assert_raises_rpc_error(-4, "This wallet has no HD account 2", w.sethdaccount, 2, "pool")
        assert_raises_rpc_error(-4, "This wallet has no HD account 7", w.getnewaddress, "", "bech32", 7)

        self.log.info("%s: the next account once account 1 is paid; past the rule only with force" % wallet_type)
        second = w.newhdaccount("second")
        assert_equal(second["hd_account"], 2)
        assert_raises_rpc_error(-4, "Account 2 " + RULE, w.newhdaccount, "third")
        third = w.newhdaccount("third", True)
        assert_equal(third["hd_account"], 3)
        assert any(RULE in warning for warning in third["warnings"])
        assert_equal(self.account_of(w, third["address"]), 3)

        self.log.info("%s: a wallet with accounts keeps its seed" % wallet_type)
        assert_raises_rpc_error(-4, "Cannot set a new HD seed on a wallet", w.sethdseed)
        self.accounts = getattr(self, "accounts", {})
        self.accounts[name] = (w.listhdaccounts(), w.getbalances())

    def test_restart(self):
        self.log.info("After a restart: the same accounts and balances")
        names = list(self.accounts)
        self.restart_node(0, self.extra_args[0] + ["-wallet=" + n for n in names])
        if self.default_wallet_name not in self.node.listwallets():
            self.node.loadwallet(self.default_wallet_name)
        for name, (accounts, balances) in self.accounts.items():
            w = self.node.get_wallet_rpc(name)
            assert_equal(w.listhdaccounts(), accounts)
            assert_equal(w.getbalances(), balances)
            assert_equal(w.getwalletinfo()["walletversion"], FEATURE_HD_ACCOUNTS)
        self.funder = self.node.get_wallet_rpc(self.default_wallet_name)
        # Blocks are staked by the default wallet, which a node call without a
        # wallet cannot name once several are loaded.
        self.node._base_rpc = self.node.rpc
        self.node.rpc = self.funder

    def test_locked(self):
        self.log.info("Locked: the addresses derived ahead are handed out, then the wallet asks to be unlocked")
        self.node.createwallet(wallet_name="locked", descriptors=False, passphrase=PASSPHRASE, wallet_type="bip39")
        w = self.node.get_wallet_rpc("locked")
        self.funder.sendtoaddress(w.getnewaddress(), 10)
        self.mine()
        assert_raises_rpc_error(-13, "walletpassphrase", w.newhdaccount)
        w.walletpassphrase(PASSPHRASE, 600)
        w.newhdaccount()
        w.walletlock()
        # Handed out while locked: indexes 1 to 10, from the keys derived at
        # the last unlock, which reach index 20. Index 10 is paid while locked,
        # and the gap moves to 30, past those keys.
        addresses = [w.getnewaddress("", "bech32", 1) for _ in range(10)]
        self.funder.sendtoaddress(addresses[-1], 5)
        self.mine()
        assert_equal(w.getbalance(hd_account=1), 5)
        for _ in range(10):
            w.getnewaddress("", "bech32", 1)
        assert_raises_rpc_error(-12, UNLOCK, w.getnewaddress, "", "bech32", 1)
        w.walletpassphrase(PASSPHRASE, 600)
        self.wait_until(lambda: self.try_address(w))
        w.walletlock()

    def try_address(self, w):
        try:
            w.getnewaddress("", "bech32", 1)
            return True
        except Exception as e:
            assert UNLOCK in str(e), str(e)
            return False

    def test_descriptor_wallet(self):
        self.log.info("A descriptor wallet has account 0 only")
        self.node.createwallet(wallet_name="descriptors", descriptors=True)
        w = self.node.get_wallet_rpc("descriptors")
        assert_raises_rpc_error(-4, LEGACY_ONLY, w.newhdaccount)
        assert_raises_rpc_error(-4, LEGACY_ONLY, w.sethdaccount, 1, "x")
        assert_raises_rpc_error(-4, LEGACY_ONLY, w.getnewaddress, "", "bech32", 1)
        address = w.getnewaddress("", "bech32", 0)
        assert "hd_account" not in w.getaddressinfo(address)
        listed = w.listhdaccounts()
        assert_equal([a["hd_account"] for a in listed], [0])
        self.funder.sendtoaddress(address, 1)
        self.mine()
        assert "hd_account" not in w.listunspent()[0]
        assert_equal(w.getbalance(hd_account=0), Decimal("1"))
        assert_equal(w.getwalletinfo()["hd_accounts"], 0)


if __name__ == '__main__':
    WalletHDAccountsTest().main()
