#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a wallet whose seed cannot be read, and wallets the change leaves alone.

A wallet whose chain holds its seed itself, and whose seed is neither in the
chain record nor in a readable record of its own, cannot derive a key. It
must still load, unlock, give the addresses in its key pool, recognise a
payment to one of them and spend; asking it for a key it cannot make must be
an ordinary error, and a payment to a pool key, which makes a wallet that can
derive top up its pool, must not end the node.

Two such wallets are made with the wallet tool: an encrypted wallet with its
record of encrypted secrets taken out, flagged and not.

Then the wallets the change does not touch: an unencrypted mnemonic wallet
keeps its secrets in its chain record and is not flagged; an encrypted plain
HD wallet gets a new seed at encryption as before and is not flagged; a
descriptor wallet is not flagged. --unfixed-bindir=<dir> also loads the
unencrypted mnemonic wallet with the binaries of an earlier version.
"""

import os

from test_framework.messages import hash256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

UNLOCK_PHRASE = "the passphrase the wallet is encrypted with"
FLAG_HD_SECRETS_ENCRYPTED = 1 << 49
MAGIC = "BITCOIN_CORE_WALLET_DUMP"
NOTICE_UNREADABLE = "The seed of this wallet cannot be read."


def record_type(key_hex):
    key = bytes.fromhex(key_hex)
    return key[1:1 + key[0]].decode("ascii", "replace")


class HDSecretsUnreadableTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-staking=0", "-keypool=4"]]

    def add_options(self, parser):
        parser.add_argument("--unfixed-bindir", dest="unfixed_bindir", default=None,
                            help="Directory with the reddcoind and reddcoin-cli of a version without this change")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()
        self.skip_if_no_bdb()

    def wallet_tool(self, *args):
        """Run the wallet tool, the command last: it must succeed and say nothing on stderr."""
        _, stderr = self.run_wallet_tool(*args)
        assert_equal(stderr, "")

    def dump(self, name):
        path = os.path.join(self.options.tmpdir, name + ".walletdump")
        if os.path.exists(path):
            os.unlink(path)
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, 'dump')
        with open(path, encoding="utf8") as f:
            records = [tuple(line.rstrip("\n").split(",")) for line in f]
        return records[:2], records[2:-1]

    def create_from(self, name, header, records):
        path = os.path.join(self.options.tmpdir, name + ".built.walletdump")
        lines = "".join("%s,%s\n" % record for record in header + records)
        with open(path, "w", encoding="utf8") as f:
            f.write(lines)
            f.write("checksum,%s\n" % hash256(lines.encode()).hex())
        self.wallet_tool('-wallet=' + name, '-dumpfile=' + path, 'createfromdump')

    def unreadable(self, name, flagged):
        """An encrypted bip44 wallet without the record of its secrets."""
        node = self.nodes[0]
        node._base_rpc.createwallet(wallet_name=name + "_source", descriptors=False, wallet_type="bip44", passphrase=UNLOCK_PHRASE)
        source = node.get_wallet_rpc(name + "_source")
        address = source.getnewaddress("", "legacy")
        source.unloadwallet()
        header, records = self.dump(name + "_source")
        built = []
        for key, value in records:
            if record_type(key) == "hdsecrets":
                continue
            if record_type(key) == "flags" and not flagged:
                value = (int.from_bytes(bytes.fromhex(value), "little") & ~FLAG_HD_SECRETS_ENCRYPTED).to_bytes(8, "little").hex()
            built.append((key, value))
        self.create_from(name, header, built)
        return address

    def run_test(self):
        node = self.nodes[0]
        for name, flagged in (("unreadable_flagged", True), ("unreadable_unflagged", False)):
            self.log.info("%s: it loads, and says what it cannot do" % name)
            given = self.unreadable(name, flagged)
            loaded = node._base_rpc.loadwallet(name)
            assert NOTICE_UNREADABLE in loaded["warning"], loaded["warning"]
            wallet = node.get_wallet_rpc(name)
            assert NOTICE_UNREADABLE in wallet.getwalletinfo()["hd_warnings"]
            wallet.walletpassphrase(UNLOCK_PHRASE, 600)
            assert_equal(wallet.getaddressinfo(given)["ismine"], True)

            self.log.info("%s: the key pool is given out, and then an ordinary error" % name)
            assert_raises_rpc_error(-4, "Error refreshing keypool.", wallet.keypoolrefill, 10)
            pool = []
            for _ in range(8):
                try:
                    pool.append(wallet.getnewaddress("", "legacy"))
                except Exception as e:
                    assert "Keypool ran out" in str(e), str(e)
                    break
            assert 0 < len(pool) < 8, pool

            self.log.info("%s: a payment to a key of its pool is credited, and the node stays up" % name)
            node.sendtoaddress(pool[-1], 10)
            node.generate(1)
            node.syncwithvalidationinterfacequeue()
            # Said once, not at every attempt.
            with open(os.path.join(node.datadir, self.chain, "debug.log"), encoding="utf8") as f:
                assert_equal(sum(1 for line in f if "[%s] cannot derive a key" % name in line), 1)
            assert_equal(wallet.getbalance(), 10)
            assert_equal(node.getblockcount(), node.getblockchaininfo()["blocks"])

            self.log.info("%s: and it can spend" % name)
            wallet.sendtoaddress(node.getnewaddress(), 4)
            node.generate(1)
            assert wallet.getbalance() < 6
            wallet.walletlock()
            wallet.unloadwallet()

        self.log.info("An unencrypted mnemonic wallet: secrets in the chain record, no flag, no record of its own")
        node._base_rpc.createwallet(wallet_name="clear", descriptors=False, wallet_type="bip39")
        wallet = node.get_wallet_rpc("clear")
        info = wallet.getwalletinfo()
        assert_equal((info["hd_type"], info["hd_secrets_encrypted"], info.get("hd_warnings")), ("bip39", False, None))
        phrase = wallet.gethdwalletinfo()["mnemonic"]
        for _ in range(5):
            wallet.getnewaddress()
        wallet.unloadwallet()
        _, records = self.dump("clear")
        types = [record_type(k) for k, _ in records]
        assert "hdsecrets" not in types and "hdsecretspending" not in types
        flags = [int.from_bytes(bytes.fromhex(v), "little") for k, v in records if record_type(k) == "flags"]
        assert not any(f & FLAG_HD_SECRETS_ENCRYPTED for f in flags)
        assert [v for k, v in records if record_type(k) == "hdchain" and phrase.encode() in bytes.fromhex(v)]

        self.log.info("An encrypted plain HD wallet: a new seed at encryption, as before, and no flag")
        node._base_rpc.createwallet(wallet_name="plain", descriptors=False)
        wallet = node.get_wallet_rpc("plain")
        seed = wallet.getwalletinfo()["hdseedid"]
        wallet.encryptwallet(UNLOCK_PHRASE)
        info = wallet.getwalletinfo()
        assert info["hdseedid"] != seed
        assert_equal((info["hd_type"], info["hd_secrets_encrypted"]), ("bip32", False))
        wallet.unloadwallet()
        _, records = self.dump("plain")
        assert "hdsecrets" not in [record_type(k) for k, _ in records]

        if self.is_sqlite_compiled():
            self.log.info("A descriptor wallet, encrypted: no flag")
            node._base_rpc.createwallet(wallet_name="desc", descriptors=True, passphrase=UNLOCK_PHRASE)
            assert_equal(node.get_wallet_rpc("desc").getwalletinfo()["hd_secrets_encrypted"], False)
            node.get_wallet_rpc("desc").unloadwallet()

        if self.options.unfixed_bindir:
            self.log.info("The unencrypted mnemonic wallet loads on a version without the change")
            self.stop_node(0)
            self.nodes[0].args[0] = os.path.join(self.options.unfixed_bindir, "reddcoind")
            self.start_node(0, self.extra_args[0])
            assert_equal(self.nodes[0].process.args[0], os.path.join(self.options.unfixed_bindir, "reddcoind"))
            assert_equal(node.loadwallet("clear")["warning"], "")
            assert_equal(node.get_wallet_rpc("clear").gethdwalletinfo()["mnemonic"], phrase)


if __name__ == '__main__':
    HDSecretsUnreadableTest().main()
