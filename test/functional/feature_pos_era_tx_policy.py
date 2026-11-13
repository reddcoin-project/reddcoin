#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the PoS-era transaction rules as relay policy on the 4.22.9 line.

Develop enforces two rules as consensus from nPosTxVersionFloorHeight: a
PoS-era transaction must be v2+ (bad-txns-version-pos) and carry a non-zero
nTime (bad-txns-time-zero). This line keeps them as policy only: once the next
block is a PoS block, the mempool refuses such transactions on every network,
so these nodes neither relay nor mine them, but blocks containing them stay
valid and nothing can split.

This test drives:

  * mempool: a signed v1 tx and a signed v2 tx with nTime == 0 are refused,
    even with -acceptnonstdtxn=1;
  * P2P: a peer relaying either is refused and stays connected;
  * block: a PoS block containing both is still accepted and connects;
  * a normal wallet tx is accepted and mined.
"""

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
    msg_tx,
    tx_from_hex,
)
from test_framework.p2p import P2PInterface
from test_framework.script_util import keyhash_to_p2pkh_script
from test_framework.address import hash160
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    advance_time_for_pos,
)

POW_TX_VERSION = 1
POSV_TX_VERSION = 2
LAST_POW_HEIGHT = 89  # regtest consensus.nLastPowHeight


class PosEraTxPolicyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # cache tip (199) is in the PoS era
        # Non-standard txs are accepted, but this policy applies regardless.
        self.extra_args = [["-acceptnonstdtxn=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _signed_tx(self, version, ntime):
        """A tx spending one of the wallet's coins, signed by the wallet. nTime
        is not serialized for v1 and is set freely for v2."""
        node = self.nodes[0]
        utxo = [u for u in node.listunspent(1) if u["amount"] >= 100][0]
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        tx = CTransaction()
        tx.nVersion = version
        tx.nTime = ntime
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])
        tx.vout = [CTxOut(int(utxo["amount"] * COIN) - COIN, spk)]  # 1 RDD fee
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        return tx_from_hex(signed["hex"])

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

    def _build_pos_block(self, node, extra_txs):
        """Build (but do not submit) a signed PoS block via GBT, appending
        extra_txs after the coinstake."""
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
        for tx in extra_txs:
            tx.rehash()
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        self.sign_block_with_coinstake_key(node, block)
        return block

    # ------------------------------------------------------------------
    # Test
    # ------------------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        assert node.getblockcount() + 1 > LAST_POW_HEIGHT
        advance_time_for_pos(node, seconds=600)

        v1 = self._signed_tx(POW_TX_VERSION, 0)
        v2_zero = self._signed_tx(POSV_TX_VERSION, 0)
        cases = ((v1, "bad-txns-version-pos"), (v2_zero, "bad-txns-time-zero"))

        self.log.info("Mempool: a v1 tx and a v2 tx with nTime == 0 are refused")
        for tx, reason in cases:
            assert_raises_rpc_error(-26, reason, node.sendrawtransaction, tx.serialize().hex(), 0)
            res = node.testmempoolaccept([tx.serialize().hex()], 0)[0]
            assert_equal(res["allowed"], False)
            assert_equal(res["reject-reason"], reason)

        self.log.info("P2P: a peer relaying either is refused and stays connected")
        peer = node.add_p2p_connection(P2PInterface())
        for tx, reason in cases:
            with node.assert_debug_log([reason], unexpected_msgs=["Misbehaving"]):
                peer.send_and_ping(msg_tx(tx))
            assert tx.hash not in node.getrawmempool()
        assert peer.is_connected
        assert_equal(len(node.getpeerinfo()), 1)
        node.disconnect_p2ps()

        self.log.info("Block: a PoS block containing both is still valid")
        block = self._build_pos_block(node, [v1, v2_zero])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)
        block_txs = node.getblock(block.hash)["tx"]
        assert v1.hash in block_txs and v2_zero.hash in block_txs

        self.log.info("A normal wallet tx is accepted and mined")
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        assert txid in node.getrawmempool()
        advance_time_for_pos(node, seconds=60)
        assert txid in node.getblock(node.generate(1)[0])["tx"]


if __name__ == '__main__':
    PosEraTxPolicyTest().main()
