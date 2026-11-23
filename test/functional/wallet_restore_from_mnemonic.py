#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a wallet restored from its mnemonic phrase finds its history.

A restore from a phrase scans the chain from its start for the keys of the
phrase, once, after the seed is set up: with a wallet passphrase too, where
the wallet is created blank, encrypted and only then given its seed. The
restored wallet has the transactions and the balance of the original without
a manual rescan, keeps them after a restart, and owes no scan afterwards.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

PASSPHRASE = "the passphrase of the restored wallet"


class WalletRestoreFromMnemonicTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # A restore finds keys up to the key pool beyond the last one used; the
        # original hands out address 29.
        self.extra_args = [["-staking=0", "-keypool=100"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def history(self, wallet):
        info = wallet.getwalletinfo()
        return info["txcount"], wallet.getbalances()["mine"]

    def run_test(self):
        node = self.nodes[0]
        funder = node.get_wallet_rpc(self.default_wallet_name)

        originals = {}
        for wallet_type in ["bip39", "bip44"]:
            name = "original_" + wallet_type
            self.log.info("A %s wallet with payments to several receive addresses and change" % wallet_type)
            phrase = node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type)["mnemonic"]
            wallet = node.get_wallet_rpc(name)
            addresses = [wallet.getnewaddress() for _ in range(30)]
            for address in (addresses[0], addresses[1], addresses[29]):
                funder.sendtoaddress(address, 1000)
            self.generate(1)
            wallet.sendtoaddress(funder.getnewaddress(), 500)
            self.generate(1)
            txcount, balances = self.history(wallet)
            assert_equal(txcount, 4)
            originals[wallet_type] = (phrase, txcount, balances)

        restored = []
        for wallet_type, (phrase, txcount, balances) in originals.items():
            name = "restored_" + wallet_type
            self.log.info("Restored as %s without a wallet passphrase: one scan, everything found" % wallet_type)
            with node.assert_debug_log(expected_msgs=["[%s] A restore scan is owed from block 0" % name]):
                node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type, mnemonic=phrase)
            assert_equal(self.history(node.get_wallet_rpc(name)), (txcount, balances))
            restored.append((name, txcount, balances))

            name = "restored_encrypted_" + wallet_type
            self.log.info("Restored as %s with a wallet passphrase: no scan before the seed exists, one after it" % wallet_type)
            with node.assert_debug_log(expected_msgs=["[%s] Scanning for the transactions of the restored wallet from block 0" % name],
                                       unexpected_msgs=["[%s] Rescanning last" % name]):
                result = node.createwallet(wallet_name=name, descriptors=False, wallet_type=wallet_type, mnemonic=phrase, passphrase=PASSPHRASE)
            assert_equal(result["warning"], "")
            wallet = node.get_wallet_rpc(name)
            assert_equal(self.history(wallet), (txcount, balances))
            assert_equal(wallet.getwalletinfo()["unlocked_until"], 0)
            restored.append((name, txcount, balances))

        self.log.info("After a restart: the same history, and no scan is owed")
        with node.assert_debug_log(expected_msgs=[], unexpected_msgs=["restore scan is owed"]):
            self.restart_node(0, self.extra_args[0] + ["-wallet=" + name for name, _, _ in restored])
        for name, txcount, balances in restored:
            assert_equal(self.history(node.get_wallet_rpc(name)), (txcount, balances))


if __name__ == '__main__':
    WalletRestoreFromMnemonicTest().main()
