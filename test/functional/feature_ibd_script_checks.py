#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that blocks connected during initial block download have their
scripts verified.

Only blocks below the assumevalid block may skip script verification.
Regtest has no assumevalid block, so every block is checked, in IBD or not.

node0 mines a block F paying to a bare `OP_2 OP_EQUAL` output. It then builds
a block B whose only extra transaction spends that output with `OP_3`, so the
script fails. node0 rejects B on submission, but B stays in its block files.

  * Sync in IBD: node1 is held in initial block download (-maxtipage=1 with
    its clock ahead) and is given F and B. It must reject B.
  * Reindex: node0 restarts with -reindex, which runs in IBD. It must stop at
    F and not connect B.
"""

from test_framework.address import hash160
from test_framework.blocktools import (
    create_block,
    sign_block,
    NORMAL_GBT_REQUEST_PARAMS,
)
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
)
from test_framework.script import CScript, OP_2, OP_3, OP_EQUAL
from test_framework.script_util import keyhash_to_p2pkh_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)

POSV_TX_VERSION = 2
FUND_SAT = 10 * COIN
FEE_SAT = COIN
SCRIPT_ERROR = "Script evaluated without error but finished with a false/empty top stack element"


class IbdScriptChecksTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = False  # both nodes start from the cache tip (199)
        # One script thread, so the exact script error is logged.
        self.extra_args = [["-par=1"], ["-par=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # The nodes stay apart: node1 only receives blocks through submitblock.
        self.setup_nodes()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def tip_time(self, node):
        return node.getblockheader(node.getbestblockhash())['time']

    def sign_block_with_coinstake_key(self, node, block):
        """Sign a PoS block with the coinstake's key (coinstake vout[1] is P2PK)."""
        spk = bytes(block.vtx[1].vout[1].scriptPubKey)
        signing_key = None
        if len(spk) in (35, 67) and spk[-1] == 0xac:  # <push> <pubkey> OP_CHECKSIG
            p2pkh = keyhash_to_p2pkh_script(hash160(spk[1:-1]))
            addr = node.decodescript(p2pkh.hex()).get('address')
            if addr:
                try:
                    signing_key = node.dumpprivkey(addr)
                except Exception as e:
                    self.log.debug("coinstake key lookup failed: %s" % e)
        if not signing_key:
            signing_key = node.get_deterministic_priv_key().key
        sign_block(block, signing_key)

    def build_pos_block(self, node, txs):
        """A signed PoS block from GBT, holding only the coinstake and txs."""
        tmpl = None
        for _ in range(10):
            advance_time_for_pos(node, seconds=120)
            try:
                tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e):
                    continue
                raise
        assert tmpl is not None, "failed to get a PoS block template"
        tmpl['transactions'] = tmpl['transactions'][:1]  # keep only the coinstake
        block = create_block(tmpl=tmpl)
        for tx in txs:
            tx.rehash()
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        self.sign_block_with_coinstake_key(node, block)
        return block

    def funding_tx(self, node, script):
        """A wallet-signed, non-witness tx paying FUND_SAT to script."""
        utxo = [u for u in node.listunspent(1)
                if u["amount"] * COIN >= FUND_SAT + FEE_SAT and u["desc"].startswith(("pk(", "pkh("))][0]
        # Keep the coin out of the coinstake the block template will build.
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        tx = CTransaction()
        tx.nVersion = POSV_TX_VERSION
        tx.nTime = self.tip_time(node)
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        change = int(utxo["amount"] * COIN) - FUND_SAT - FEE_SAT
        change_spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress("", "legacy"))["scriptPubKey"])
        tx.vout = [CTxOut(FUND_SAT, script), CTxOut(change, change_spk)]
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        return tx_from_hex(signed["hex"])

    # ------------------------------------------------------------------
    # Test
    # ------------------------------------------------------------------

    def run_test(self):
        node0, node1 = self.nodes
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        advance_time_for_pos(node0, seconds=600)

        self.log.info("node0 mines F, paying to a bare OP_2 OP_EQUAL output")
        fund = self.funding_tx(node0, CScript([OP_2, OP_EQUAL]))
        block_f = self.build_pos_block(node0, [fund])
        assert_equal(node0.submitblock(block_f.serialize().hex()), None)
        assert_equal(node0.getbestblockhash(), block_f.hash)
        height_f = node0.getblockcount()

        self.log.info("node0 rejects B, which spends it with OP_3")
        spend = CTransaction()
        spend.nVersion = POSV_TX_VERSION
        spend.nTime = self.tip_time(node0)
        spend.vin = [CTxIn(COutPoint(fund.sha256, 0), CScript([OP_3]), 0xffffffff)]
        spend.vout = [CTxOut(FUND_SAT - FEE_SAT, keyhash_to_p2pkh_script(hash160(b"RED-241 sink")))]
        block_b = self.build_pos_block(node0, [spend])
        with node0.assert_debug_log([SCRIPT_ERROR]):
            assert node0.submitblock(block_b.serialize().hex()) is not None
        assert_equal(node0.getbestblockhash(), block_f.hash)

        self.log.info("Sync in IBD: node1 accepts F and rejects B")
        self.restart_node(1, extra_args=self.extra_args[1] + ["-maxtipage=1"])
        node1.setmocktime(self.tip_time(node0) + 3600)
        assert node1.getblockchaininfo()["initialblockdownload"]
        assert_equal(node1.submitblock(block_f.serialize().hex()), None)
        assert_equal(node1.getbestblockhash(), block_f.hash)
        assert node1.getblockchaininfo()["initialblockdownload"]
        with node1.assert_debug_log([SCRIPT_ERROR]):
            assert node1.submitblock(block_b.serialize().hex()) is not None
        assert_equal(node1.getbestblockhash(), block_f.hash)

        self.log.info("Reindex: node0 rebuilds its chain to F and does not connect B")
        with node0.assert_debug_log(["Reindexing finished", SCRIPT_ERROR], timeout=60):
            self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        self.wait_until(lambda: node0.getblockcount() >= height_f)
        assert_equal(node0.getblockcount(), height_f)
        assert_equal(node0.getbestblockhash(), block_f.hash)


if __name__ == '__main__':
    IbdScriptChecksTest().main()
