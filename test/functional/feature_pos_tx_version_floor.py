#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the PoS-era transaction rules (bad-txns-version-pos, bad-txns-time-zero).

nTime is serialized only for nVersion > POW_TX_VERSION, so a v1 transaction
has no timestamp and its outputs enter the UTXO set with Coin.nTime == 0. From
consensus.nPosTxVersionFloorHeight on (regtest 90, the whole PoS era; mainnet
and testnet not active until a deployment sets a height), every transaction
must be v2+ (bad-txns-version-pos) with a non-zero nTime (bad-txns-time-zero).
Both rules are enforced in two places:

  * the mempool, for the next block (MemPoolAccept::PreChecks), as
    TX_RECENT_CONSENSUS_CHANGE so a peer relaying such a tx is not punished;
  * block validation (ContextualCheckBlock).

Before that height, but once the next block is a PoS block (mainnet and
testnet today), the mempool still refuses both, as policy (TX_NOT_STANDARD),
while blocks containing them stay valid. The 4.22.9 line has the same policy.

This test drives:

  * v1                      -> bad-txns-version-pos (mempool and block),
  * v2 with nTime == 0      -> bad-txns-time-zero (mempool and block),
  * both relayed by a peer  -> rejected, and the peer stays connected,
  * v2 with a valid nTime   -> accepted and mined,
  * a normal wallet tx      -> accepted and mined,
  * an empty PoS block      -> accepted,
  * with the rules not active (-postxfloorheight=-1): both are still refused
    by the mempool and the peer stays connected, but a block containing both
    is accepted.
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

POW_TX_VERSION = 1                 # below the PoS-era floor
POSV_TX_VERSION = 2                # first version above the floor (carries nTime)
FLOOR_HEIGHT = 90                  # regtest consensus.nPosTxVersionFloorHeight


class PosTxVersionFloorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # cache tip (199) is above the regtest floor

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _v1_tx(self, outpoint, value_sat, spk):
        """A structurally valid but unsigned v1 transaction. The version floor
        rejects it before any script/input checks, so no signature is needed."""
        tx = CTransaction()
        tx.nVersion = POW_TX_VERSION  # v1: no nTime is serialized for nVersion <= 1
        tx.nTime = 0
        tx.vin = [CTxIn(outpoint)]
        tx.vout = [CTxOut(value_sat, spk)]
        tx.rehash()
        return tx

    def _v2_zero_time_tx(self, outpoint, value_sat, spk):
        """A v2 transaction with nTime == 0, unsigned: the timestamp rule rejects
        it before any input or script check."""
        tx = CTransaction()
        tx.nVersion = POSV_TX_VERSION
        tx.nTime = 0
        tx.vin = [CTxIn(outpoint)]
        tx.vout = [CTxOut(value_sat, spk)]
        tx.rehash()
        return tx

    def _signed_tx(self, node, version, ntime):
        """A tx spending one of the wallet's coins, signed by the wallet, so a
        block can contain it when the rules are not active. nTime is not
        covered by the signature hash."""
        utxo = [u for u in node.listunspent(1) if u["amount"] >= 100][0]
        node.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        tx = CTransaction()
        tx.nVersion = version
        tx.nTime = ntime
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        tx.vout = [CTxOut(int(utxo["amount"] * COIN) - COIN, self._new_spk(node))]
        signed = node.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        return tx_from_hex(signed["hex"])

    def _new_spk(self, node):
        return bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"])

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
        # Keep only the coinstake; append our own txs deterministically.
        tmpl['transactions'] = tmpl['transactions'][:1]
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
        assert node.getblockcount() >= FLOOR_HEIGHT
        advance_time_for_pos(node, seconds=600)

        # --- Mempool enforcement --------------------------------------------
        # v1 is rejected by the version floor (MemPoolAccept::PreChecks).
        self.log.info("Mempool: v1 tx -> bad-txns-version-pos")
        utxo = node.listunspent(1)[0]
        v1 = self._v1_tx(
            COutPoint(int(utxo["txid"], 16), utxo["vout"]),
            int(utxo["amount"] * COIN) - COIN,  # leave a 1-RDD fee
            self._new_spk(node),
        )
        # maxfeerate=0 disables the fee guard so the version rule is what fires.
        assert_raises_rpc_error(-26, "bad-txns-version-pos",
                                node.sendrawtransaction, v1.serialize().hex(), 0)

        # A v2 tx with nTime == 0 is rejected by the timestamp rule before any
        # input or script check (the prevout doesn't exist).
        self.log.info("Mempool: v2 tx with nTime==0 -> bad-txns-time-zero")
        v2_zero = self._v2_zero_time_tx(COutPoint(0xdeadbeef, 1), COIN, self._new_spk(node))
        assert_raises_rpc_error(-26, "bad-txns-time-zero",
                                node.sendrawtransaction, v2_zero.serialize().hex(), 0)

        # A peer on a release without these rules relays such txs in good faith.
        # The node rejects them but must not punish the peer: a TX_CONSENSUS
        # rejection would discourage and disconnect it.
        self.log.info("P2P: a peer relaying either tx is not punished")
        peer = node.add_p2p_connection(P2PInterface())
        for tx, reason in ((v1, "bad-txns-version-pos"), (v2_zero, "bad-txns-time-zero")):
            with node.assert_debug_log([reason]):
                peer.send_and_ping(msg_tx(tx))
            assert tx.hash not in node.getrawmempool()
        assert peer.is_connected
        assert_equal(len(node.getpeerinfo()), 1)
        node.disconnect_p2ps()

        # A signed v2 tx with a valid nTime is accepted: v2 is above the floor.
        self.log.info("Mempool: signed v2 tx with a valid nTime -> accepted")
        u2 = node.listunspent(1)[1]
        v2 = CTransaction()
        v2.nVersion = POSV_TX_VERSION
        v2.nTime = node.mocktime  # aligned with the chain, non-zero
        v2.vin = [CTxIn(COutPoint(int(u2["txid"], 16), u2["vout"]))]
        v2.vout = [CTxOut(int(u2["amount"] * COIN) - COIN, self._new_spk(node))]
        v2.rehash()
        signed = node.signrawtransactionwithwallet(v2.serialize().hex())
        assert signed["complete"], "failed to sign v2 tx"
        v2_txid = node.sendrawtransaction(signed["hex"], 0)
        assert v2_txid in node.getrawmempool()

        # A normal v3 wallet tx is also accepted; mine a block and confirm both
        # the v2 and v3 txs are included (block-path acceptance of v2/v3).
        self.log.info("Mempool: v3 wallet tx accepted; v2 and v3 mine into a PoS block")
        v3_txid = node.sendtoaddress(node.getnewaddress(), 1)
        assert v3_txid in node.getrawmempool()
        advance_time_for_pos(node, seconds=60)
        blk_txs = node.getblock(node.generate(1)[0])["tx"]
        assert v2_txid in blk_txs and v3_txid in blk_txs, \
            "v2 and v3 txs should mine into a PoS block"

        # --- Block enforcement (ContextualCheckBlock) ------------------------
        self.log.info("Block: a PoS block containing a v1 transaction is rejected")
        tip = node.getbestblockhash()
        # Use a dummy prevout so the v1 tx can never collide with a coinstake
        # input; ContextualCheckBlock rejects on version before inputs are checked.
        bad_block = self._build_pos_block(
            node, [self._v1_tx(COutPoint(0xdeadbeef, 0), COIN, self._new_spk(node))])
        resp = node.submitblock(bad_block.serialize().hex())
        assert resp is not None and "bad-txns-version-pos" in resp, \
            f"expected bad-txns-version-pos rejection, got: {resp!r}"
        assert_equal(node.getbestblockhash(), tip)  # tip must not advance

        self.log.info("Block: a PoS block containing a v2 tx with nTime==0 is rejected")
        bad_block = self._build_pos_block(
            node, [self._v2_zero_time_tx(COutPoint(0xdeadbeef, 2), COIN, self._new_spk(node))])
        resp = node.submitblock(bad_block.serialize().hex())
        assert resp is not None and "bad-txns-time-zero" in resp, \
            f"expected bad-txns-time-zero rejection, got: {resp!r}"
        assert_equal(node.getbestblockhash(), tip)

        # Positive control: the same block shape without the bad tx is accepted.
        self.log.info("Block: an empty PoS block (no v1 tx) is accepted")
        good_block = self._build_pos_block(node, [])
        resp = node.submitblock(good_block.serialize().hex())
        assert resp is None, f"a valid empty PoS block was rejected: {resp!r}"
        assert_equal(node.getbestblockhash(), good_block.hash)

        # --- Policy before the rules activate --------------------------------
        # Mainnet and testnet are here today: PoS era, rules not active. The
        # mempool still refuses both shapes, but blocks containing them are valid.
        self.log.info("Rules not active (-postxfloorheight=-1): mempool still refuses both")
        self.restart_node(0, extra_args=["-postxfloorheight=-1", f"-mocktime={node.mocktime}"])
        node = self.nodes[0]
        cases = ((self._signed_tx(node, POW_TX_VERSION, 0), "bad-txns-version-pos"),
                 (self._signed_tx(node, POSV_TX_VERSION, 0), "bad-txns-time-zero"))
        for tx, reason in cases:
            assert_raises_rpc_error(-26, reason, node.sendrawtransaction, tx.serialize().hex(), 0)

        self.log.info("Rules not active: a peer relaying either is not punished")
        peer = node.add_p2p_connection(P2PInterface())
        for tx, reason in cases:
            with node.assert_debug_log([reason], unexpected_msgs=["Misbehaving"]):
                peer.send_and_ping(msg_tx(tx))
            assert tx.hash not in node.getrawmempool()
        assert peer.is_connected
        node.disconnect_p2ps()

        self.log.info("Rules not active: a PoS block containing both is accepted")
        block = self._build_pos_block(node, [tx for tx, _ in cases])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)
        block_txs = node.getblock(block.hash)["tx"]
        for tx, _ in cases:
            assert tx.hash in block_txs

        self.log.info("PoS-era transaction rules verified (mempool, P2P and block enforcement, and policy before activation)")


if __name__ == '__main__':
    PosTxVersionFloorTest().main()
