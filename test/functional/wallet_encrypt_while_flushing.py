#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test encryptwallet on SQLite wallets while the node writes its chain state.

Every write of the chain state tells each wallet the best block, which the
wallet writes with a batch of its own. An SQLite wallet has one connection:
a batch that closed while encryptwallet held a transaction on it ended that
transaction, the encryption could not commit, and the program stopped.

Two clients make the node write its chain state again and again (each
gettxoutsetinfo does) while wallets are encrypted one after another.
"""

import threading

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy

WALLETS = 40


class WalletEncryptWhileFlushingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        node = self.nodes[0]
        names = ["flushing_%d" % i for i in range(WALLETS)]
        for name in names:
            node.createwallet(wallet_name=name, descriptors=True)

        stop = threading.Event()
        flushes = []
        failures = []

        def flush():
            proxy = get_rpc_proxy(node.url, 0, timeout=600)
            count = 0
            while not stop.is_set():
                try:
                    proxy.gettxoutsetinfo("none")
                    count += 1
                except Exception as e:
                    failures.append(repr(e))
                    break
            flushes.append(count)

        threads = [threading.Thread(target=flush) for _ in range(2)]
        for t in threads:
            t.start()
        try:
            for name in names:
                node.get_wallet_rpc(name).encryptwallet("a passphrase")
        finally:
            stop.set()
            for t in threads:
                t.join()
        self.log.info("%d wallets encrypted while the chain state was written %d times" % (WALLETS, sum(flushes)))
        assert_equal(failures, [])
        assert sum(flushes) > WALLETS
        for name in names:
            assert "unlocked_until" in node.get_wallet_rpc(name).getwalletinfo()


if __name__ == '__main__':
    WalletEncryptWhileFlushingTest().main()
