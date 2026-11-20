#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that memory which held secrets does not reach the files of a Berkeley DB wallet.

Berkeley DB writes whole pages. The unused part of a page used to be whatever
the heap of the process had held, so the text of an RPC reply or request
could be found in the file of a wallet it had nothing to do with.

Secrets that are made up for the test pass through the node's memory: a
wallet's seed, root key and a private key in the replies of gethdwalletinfo
and dumpprivkey, and a long passphrase in a request. Meanwhile other wallets grow. The node is
stopped and every file under every wallet directory is searched.

The secrets are searched for in the form the commands print them, which no
wallet stores: found anywhere, they are a leak.

--long runs many more rounds and is not part of the test suite.
"""

import os

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Long enough to need a buffer of several pages.
LONG_UNLOCK_PHRASE = "page-gap marker: a wrong wallet passphrase " + "0f1e2d3c4b5a6978" * 180
VICTIMS = 4


class BDBPageGapsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=5"]]

    def add_options(self, parser):
        parser.add_argument("--long", dest="long", default=False, action="store_true", help="Run 2000 rounds instead of 400")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()

    def run_test(self):
        node = self.nodes[0]
        base = node._base_rpc
        # On this line createwallet cannot make a mnemonic wallet: the source is
        # a plain HD wallet, and its seed, root key and a private key, as the
        # commands print them, are the secrets that pass through the replies.
        base.createwallet(wallet_name="source", descriptors=False)
        source = node.get_wallet_rpc("source")
        info = source.gethdwalletinfo()
        source_key = source.dumpprivkey(source.getnewaddress("", "legacy"))
        secrets = {
            "seed, as hex": info["hdseed"].encode(),
            "root private key": info["rootprivkey"].encode(),
            "private key, as WIF": source_key.encode(),
            "long passphrase of a request": LONG_UNLOCK_PHRASE[:200].encode(),
        }
        victims = []
        for i in range(VICTIMS):
            name = "victim%d" % i
            base.createwallet(wallet_name=name, descriptors=False)
            victims.append(node.get_wallet_rpc(name))
        victims[0].encryptwallet("the real passphrase of a victim")

        # On this line the secrets pass through fewer replies than a mnemonic
        # wallet's would, and 40 rounds do not show the leak of an unfixed build.
        rounds = 2000 if self.options.long else 400
        self.log.info("%d rounds: secrets in replies and in a request, while other wallets grow" % rounds)
        for round_number in range(rounds):
            assert_equal(source.gethdwalletinfo()["hdseed"], info["hdseed"])
            if round_number % 4 == 0:
                try:
                    victims[0].walletpassphrase(LONG_UNLOCK_PHRASE, 1)
                    raise AssertionError("a wrong passphrase was accepted")
                except JSONRPCException as e:
                    assert_equal(e.error["code"], -14)
            for victim in victims[1:]:
                for _ in range(3):
                    victim.getnewaddress("", "legacy")
                victim.setlabel(victim.getnewaddress("", "legacy"), "label %d %s" % (round_number, "x" * (round_number % 50)))
            source.getnewaddress("", "legacy")

        self.log.info("Stop the node, and search every file of every wallet")
        self.stop_node(0)
        chain_dir = os.path.join(node.datadir, self.chain)
        leaks = []
        searched = 0
        for root, _, files in os.walk(chain_dir):
            relative = os.path.relpath(root, chain_dir)
            top = relative.split(os.sep)[0]
            if top in ("blocks", "chainstate", "indexes") or relative == ".":
                # Not wallet files; the wallets of this node are in directories of their own.
                if relative != "." or not any(f.startswith("wallet") or f == "db.log" for f in files):
                    continue
            for file in sorted(files):
                if file == ".walletlock":
                    continue  # empty, and locked while its wallet is loaded: Windows refuses to read it
                if relative == "." and file in ("debug.log", "peers.dat", "banlist.json", "fee_estimates.dat", "mempool.dat", "settings.json", "anchors.dat", ".lock"):
                    continue
                file_path = os.path.join(root, file)
                with open(file_path, "rb") as f:
                    content = f.read()
                searched += 1
                for what, secret in secrets.items():
                    times = content.count(secret)
                    if times:
                        leaks.append((os.path.join(relative, file), what, times))
        self.log.info("searched %d files" % searched)
        for leak in leaks:
            self.log.info("LEAK: %s holds the %s, %d times" % leak)
        assert searched >= VICTIMS + 1
        assert_equal(leaks, [])
        # The log of the node is not a wallet file, and must not hold them either.
        with open(os.path.join(chain_dir, "debug.log"), "rb") as f:
            log = f.read()
        assert_equal([what for what, secret in secrets.items() if secret in log], [])

        self.start_node(0)
        node._base_rpc = node.rpc


if __name__ == '__main__':
    BDBPageGapsTest().main()
