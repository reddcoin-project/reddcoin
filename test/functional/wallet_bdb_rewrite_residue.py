#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that encrypting a Berkeley DB wallet leaves none of its old keys in clear.

encryptwallet encrypts the keys and rewrites the file, so that the keys as
they were stored before are gone from it. The rewrite used to build the new
file in the cache that had just read the old one; Berkeley DB clears only the
head of a new page, so pages of the new file kept parts of old pages, and a
few of the wallet's keys, in clear, in the file of the encrypted wallet.

Wallets with some thousands of keys are encrypted, and every file of their
directories is searched for the 32 bytes of each private key they held
before.

--long uses more and larger wallets and is not part of the test suite.
"""

import os

from test_framework.address import base58_to_byte
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class BDBRewriteResidueTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=10"]]

    def add_options(self, parser):
        parser.add_argument("--long", dest="long", default=False, action="store_true", help="Six wallets of 8,000 keys instead of three of 3,000")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def private_keys(self, wallet, name):
        """The raw private keys of the wallet, from a dump of it."""
        path = os.path.join(self.options.tmpdir, name + ".keys.txt")
        wallet.dumpwallet(path)
        keys = set()
        with open(path, encoding="utf8") as f:
            for line in f:
                # Key lines begin with the key in WIF, 51 or 52 characters;
                # script lines begin with the script in hex.
                first = line.split()[0] if line.strip() else ""
                if line.startswith("#") or len(first) not in (51, 52) or "script=1" in line:
                    continue
                payload, _ = base58_to_byte(first)
                keys.add(payload[:32])
        return keys

    def run_test(self):
        node = self.nodes[0]
        wallets, pool = (6, 4000) if self.options.long else (3, 1500)
        chain_dir = os.path.join(node.datadir, self.chain)
        total = found = 0
        for i in range(wallets):
            name = "w%d" % i
            node._base_rpc.createwallet(wallet_name=name, descriptors=False)
            wallet = node.get_wallet_rpc(name)
            wallet.keypoolrefill(pool)
            for _ in range(20):
                wallet.getnewaddress()
            keys = self.private_keys(wallet, name)
            assert len(keys) > 2 * pool
            wallet.encryptwallet("a passphrase")
            wallet.unloadwallet()
            directory = os.path.join(chain_dir, name)
            # The rewrite leaves the database and the environment's own files, nothing more.
            left = sorted(os.path.relpath(os.path.join(root, file), directory) for root, _, files in os.walk(directory) for file in files)
            assert not [f for f in left if f.startswith("__db") or f.endswith(".rewrite")], left
            for root, _, files in os.walk(directory):
                for file in files:
                    if file == ".walletlock":
                        continue  # empty, and locked while its wallet is loaded: Windows refuses to read it
                    with open(os.path.join(root, file), "rb") as f:
                        content = f.read()
                    hits = sum(1 for key in keys if key in content)
                    if hits:
                        self.log.info("%s: %d of its %d keys in clear in %s" % (name, hits, len(keys), os.path.relpath(os.path.join(root, file), directory)))
                    found += hits
            total += len(keys)
        self.log.info("%d keys in %d wallets; in clear after encryption: %d" % (total, wallets, found))
        assert_equal(found, 0)


if __name__ == '__main__':
    BDBRewriteResidueTest().main()
