#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that witness rules apply only once SegWit is active.

SegWit is still a BIP9 deployment here, so SCRIPT_VERIFY_WITNESS must follow
the deployment state, as in Bitcoin before SegWit was buried. Until activation
a witness program is an anyone-can-spend output, and a block that spends one
with an empty witness is valid. Once SegWit is active, the same spend fails
with "Witness program hash mismatch".

The test drives regtest's SegWit deployment through its states:

  * STARTED (the cache tip): the empty-witness spend is accepted;
  * LOCKED_IN: still accepted;
  * ACTIVE: rejected;
  * DEFINED (restart with the deployment scheduled in the future): accepted
    again, which also shows the rule is not applied to the chain retroactively.
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
from test_framework.script import CScript, OP_0
from test_framework.script_util import keyhash_to_p2pkh_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)

POSV_TX_VERSION = 2
FUND_SAT = 10 * COIN
FEE_SAT = COIN
# A deployment start far in the future keeps SegWit DEFINED on every block.
FAR_FUTURE = 4102444800  # 2100-01-01
WITNESS_ERROR = "Witness program hash mismatch"  # P2WPKH needs a two-item witness


class SegwitActivationGateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # cache tip (199): SegWit is STARTED
        self.extra_args = [[
            "-whitelist=noban@127.0.0.1",
            "-par=1",  # one script thread, so the exact script error is logged
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def segwit_status(self):
        """The deployment state that applies to the next block."""
        return self.nodes[0].getblockchaininfo()['softforks']['segwit']['bip9']['status']

    def generate_to(self, height):
        node = self.nodes[0]
        if node.getblockcount() < height:
            self.generate(height - node.getblockcount())
        assert_equal(node.getblockcount(), height)

    def tip_time(self):
        node = self.nodes[0]
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

    def build_pos_block(self, txs):
        """A signed PoS block from GBT, holding only the coinstake and txs."""
        node = self.nodes[0]
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

    def fund_witness_program(self, tag):
        """Mine a non-witness wallet tx paying FUND_SAT to a P2WPKH program
        nobody holds the key for, and return that outpoint. Before activation
        the output is anyone-can-spend; after it, only a valid witness spends
        it."""
        node = self.nodes[0]
        utxo = [u for u in node.listunspent(1)
                if u["amount"] * COIN >= FUND_SAT + FEE_SAT and u["desc"].startswith(("pk(", "pkh("))][0]
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        program = CScript([OP_0, hash160(b"RED-224 " + tag.encode())])
        tx = CTransaction()
        tx.nVersion = POSV_TX_VERSION
        tx.nTime = self.tip_time()
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        change = int(utxo["amount"] * COIN) - FUND_SAT - FEE_SAT
        change_spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress("", "legacy"))["scriptPubKey"])
        tx.vout = [CTxOut(FUND_SAT, program), CTxOut(change, change_spk)]
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        fund = tx_from_hex(signed["hex"])
        block = self.build_pos_block([fund])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)
        return COutPoint(fund.sha256, 0)

    def empty_witness_spend(self, outpoint):
        """Spend a witness program with an empty scriptSig and no witness."""
        tx = CTransaction()
        tx.nVersion = POSV_TX_VERSION
        tx.nTime = self.tip_time()
        tx.vin = [CTxIn(outpoint, b"", 0xffffffff)]
        tx.vout = [CTxOut(FUND_SAT - FEE_SAT, keyhash_to_p2pkh_script(hash160(b"RED-224 sink")))]
        tx.rehash()
        return tx

    def assert_spend_accepted(self, tag):
        node = self.nodes[0]
        spend = self.empty_witness_spend(self.fund_witness_program(tag))
        block = self.build_pos_block([spend])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)
        assert spend.hash in node.getblock(block.hash)["tx"]

    def assert_spend_rejected(self, tag):
        node = self.nodes[0]
        spend = self.empty_witness_spend(self.fund_witness_program(tag))
        block = self.build_pos_block([spend])
        tip = node.getbestblockhash()
        with node.assert_debug_log([WITNESS_ERROR]):
            res = node.submitblock(block.serialize().hex())
        assert res is not None, "block spending a witness program without a witness was accepted"
        assert_equal(node.getbestblockhash(), tip)

    # ------------------------------------------------------------------
    # Test
    # ------------------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        advance_time_for_pos(node, seconds=600)

        self.log.info("STARTED: a block spending a witness program with an empty witness is valid")
        assert_equal(self.segwit_status(), "started")
        self.assert_spend_accepted("started")

        self.log.info("LOCKED_IN: still valid")
        self.generate_to(287)  # the next block, 288, starts the LOCKED_IN period
        assert_equal(self.segwit_status(), "locked_in")
        self.assert_spend_accepted("locked_in")

        self.log.info("ACTIVE: the same spend is rejected")
        self.generate_to(431)  # the next block, 432, starts the ACTIVE period
        assert_equal(self.segwit_status(), "active")
        self.assert_spend_rejected("active")

        self.log.info("DEFINED: with SegWit scheduled in the future, the spend is valid again")
        self.restart_node(0, extra_args=self.extra_args[0] + [
            "-vbparams=segwit:{}:{}".format(FAR_FUTURE, FAR_FUTURE + 31536000)])
        advance_time_for_pos(node, seconds=600)
        assert_equal(self.segwit_status(), "defined")
        self.assert_spend_accepted("defined")


if __name__ == '__main__':
    SegwitActivationGateTest().main()
