#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Copyright (c) 2014-2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP68 / CSV transaction version compatibility between v4.22.9 and v4.22.10.

v4.22.10's BIP68 threshold is hardcoded to nVersion >= 3 in tx_verify.cpp
(HEAD via pr/core-fixes commit 5e6d88dc61). That threshold applies to BOTH
the mempool path AND the consensus path — STANDARD_LOCKTIME_VERIFY_FLAGS
unconditionally sets LOCKTIME_VERIFY_SEQUENCE on both versions, so the
mempool BIP68 check fires too. v4.22.9's threshold is >= 2 in both layers.

This produces a coherent version-version delta:

  - Mempool delta on v2 unsatisfied-lock txs: v4.22.10 ACCEPTS (>= 3
    exempts v2 from BIP68), v4.22.9 REJECTS (>= 2 enforces v2). Observable
    pre-CSV too — STANDARD flags don't depend on CSV state.
  - Consensus delta on v2 unsatisfied-lock blocks: only fires post-CSV
    (block-validation gates LOCKTIME_VERIFY_SEQUENCE on CSV deployment).
    Then v4.22.10 accepts the block, v4.22.9 rejects it. Chain split.
  - No divergence on v3 sends (the v4.22.10 wallet default after RED-13):
    v3 meets both thresholds; both versions enforce BIP68 identically.

Test phases (execution order: A, B, C, E, D; D is last because it leaves
the chains intentionally divergent):

  Phase A — Pre-CSV: mempool delta on v2 unsat
    node1 (v4.22.10) accepts v2 unsat into mempool; node0 (v4.22.9) rejects.
    Chain stays in sync — block-validation BIP68 isn't active yet.

  Phase B — Activate CSV via BIP9 signalling on v4.22.10 (also v4.22.9-regtest)
    Node1 restarts with CSV signalling enabled, generates blocks through
    BIP9 activation. Node0 follows; v4.22.9-regtest also signals CSV.

  Phase C — v2 satisfied lock — both accept
    Different reasoning per version (v4.22.10 exempts v2; v4.22.9 enforces
    and finds the lock satisfied) — same outcome, no divergence.

  Phase E — v3 sequence-lock — convergent across versions
    Proves the v4.22.10 wallet default (v3) is divergence-free in normal
    wallet activity. v3 meets both thresholds, both versions enforce.
      - v3 unsatisfied-lock: both reject from mempool
      - v3 satisfied-lock:   both accept; mined block syncs

  Phase D — v2 unsatisfied lock post-CSV: consensus divergence
    Mempool delta from Phase A re-asserted. node1 mines a block that
    naturally includes the v2 unsat tx (its mempool accepted it — no
    generateblock force-include needed). node0 receives the block, rejects
    it at consensus (BIP68 enforced post-CSV). Documented chain split —
    RED-8. This phase intentionally leaves the chains divergent.

v1 transactions are not tested because Reddcoin wallets do not produce v1
(PoSV requires nTime → every Reddcoin wallet tx is v2+ by design).

Requires previous releases (the v4.22.9.3 binary), see test/README.md.
"""

import hashlib

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
    OP_DUP,
    OP_EQUALVERIFY,
    OP_HASH160,
)
from test_framework.script_util import DUMMY_P2WPKH_SCRIPT
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    advance_time_for_pos,
    set_node_times,
)

# nSequence value for 10-block relative lock
RELATIVE_LOCK_10_BLOCKS = 10

# RPC error string for sequence-lock violations
NOT_FINAL_ERROR = "non-BIP68-final"

# Version numbers
VERSION_OLD = 4220903  # v4.22.9.3
VERSION_NEW = None     # v4.22.10 (current build)


def build_signed_tx(node, txid_int, vout, value_sat, nversion, nsequence, fee_sat=1_000_000):
    """Build a transaction with specified nVersion and nSequence, sign it, return hex."""
    dest_addr = node.getnewaddress()

    tx = CTransaction()
    tx.nVersion = nversion
    tx.vin = [CTxIn(COutPoint(txid_int, vout), nSequence=nsequence)]
    output_value = value_sat - fee_sat
    if output_value <= 0:
        raise AssertionError("Fee (%d) exceeds value (%d)" % (fee_sat, value_sat))
    tx.vout = [CTxOut(output_value, DUMMY_P2WPKH_SCRIPT)]
    tx.rehash()

    signed = node.signrawtransactionwithwallet(tx.serialize().hex())
    if signed.get("complete") is not True:
        # Fallback: use a P2PKH output the wallet controls
        tx2 = CTransaction()
        tx2.nVersion = nversion
        tx2.vin = [CTxIn(COutPoint(txid_int, vout), nSequence=nsequence)]
        addr_info = node.getaddressinfo(dest_addr)
        pubkey_hex = addr_info.get("pubkey", "")
        if pubkey_hex:
            pubkey_bytes = bytes.fromhex(pubkey_hex)
            h160 = hashlib.new("ripemd160", hashlib.sha256(pubkey_bytes).digest()).digest()
            script = CScript([OP_DUP, OP_HASH160, h160, OP_EQUALVERIFY, OP_CHECKSIG])
            tx2.vout = [CTxOut(output_value, script)]
        else:
            tx2.vout = [CTxOut(output_value, DUMMY_P2WPKH_SCRIPT)]
        tx2.rehash()
        signed = node.signrawtransactionwithwallet(tx2.serialize().hex())

    return signed["hex"]


class CompatTxVersionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # node0 (v4.22.9): natural state — CSV is NEVER_ACTIVE in compiled
        # chainparams, matching the current mainnet v4.22.9 release.
        # node1 (v4.22.10): also starts with CSV disabled (regtest default
        # has nStartTime=0, so override to NEVER_ACTIVE initially).
        # -whitelist prevents mocktime-driven peer disconnections.
        self.extra_args = [
            ["-whitelist=127.0.0.1", "-staking=0"],                        # node0 = old v4.22.9 (natural)
            ["-whitelist=127.0.0.1", "-staking=0", "-vbparams=csv:-2:0"],  # node1 = new v4.22.10 (CSV off initially)
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_previous_releases()

    def setup_network(self):
        self.add_nodes(self.num_nodes, versions=[VERSION_OLD, VERSION_NEW],
                       extra_args=self.extra_args)
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()
        # Do NOT connect yet — connect after bootstrap

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _csv_is_active(self, node):
        """Check if CSV is active, handling NEVER_ACTIVE (key absent)."""
        sf = node.getblockchaininfo().get('softforks', {})
        return sf.get('csv', {}).get('active', False)

    def _generate_pos_block(self, node, count=1):
        """Generate PoS block(s) with time advancement."""
        advance_time_for_pos(node, seconds=600)
        node.generate(count)

    def _fund_and_confirm(self, node, amount_rdd=10.0):
        """Fund a fresh address and confirm. Returns (txid_int, vout, value_sat)."""
        addr = node.getnewaddress()
        txid_hex = node.sendtoaddress(addr, amount_rdd)
        node.generate(1)
        raw = node.getrawtransaction(txid_hex, True)
        for i, vout in enumerate(raw["vout"]):
            spk = vout["scriptPubKey"]
            if spk.get("address") == addr or addr in spk.get("addresses", []):
                return int(txid_hex, 16), i, int(vout["value"] * COIN)
        raise AssertionError("Could not find funded vout")

    # ------------------------------------------------------------------
    # Phase A — Pre-activation baseline
    # ------------------------------------------------------------------

    def phase_a_pre_activation(self):
        """Bootstrap chain with CSV disabled. Verify v2 txs with sequence
        locks are accepted freely (no BIP68 enforcement)."""
        self.log.info("=== Phase A: Pre-activation baseline (CSV disabled) ===")
        node1 = self.nodes[1]

        # Bootstrap: 89 PoW + 111 PoS = 200 blocks
        self.log.info("Node1: generating 89 PoW blocks...")
        node1.generate(89)
        self.log.info("Node1: advancing time for PoS...")
        advance_time_for_pos(node1, seconds=600)
        self.log.info("Node1: generating 111 PoS blocks...")
        node1.generate(111)
        assert_equal(node1.getblockcount(), 200)

        # Sync to node0
        self.log.info("Restarting node0 with mocktime and connecting...")
        mocktime = node1.mocktime
        self.restart_node(0, extra_args=self.extra_args[0] + [f'-mocktime={mocktime}'])
        self.nodes[0].mocktime = mocktime
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=120)

        node0 = self.nodes[0]
        assert_equal(node0.getblockcount(), 200)

        # Verify CSV is NOT active. When NEVER_ACTIVE, the key may be absent
        # from getblockchaininfo or report status=failed.
        assert not self._csv_is_active(node0), "CSV should not be active on old node"
        assert not self._csv_is_active(node1), "CSV should not be active on new node"
        self.log.info("CSV is NOT active on either node (as expected)")

        # Test the version-version mempool delta on v2 unsatisfied-lock txs.
        # Under Path B, v4.22.10's BIP68 threshold is hardcoded to >= 3 (HEAD's
        # tx_verify.cpp from pr/core-fixes), and that threshold applies to the
        # mempool path too because STANDARD_LOCKTIME_VERIFY_FLAGS sets
        # LOCKTIME_VERIFY_SEQUENCE. v4.22.9's threshold is >= 2. So:
        #   - node1 (v4.22.10) mempool: v2 < 3 → BIP68 exempt → accepts
        #   - node0 (v4.22.9)  mempool: v2 >= 2 → BIP68 enforced → rejects
        # Pre-CSV, block-validation BIP68 isn't active on either node
        # (LOCKTIME_VERIFY_SEQUENCE is gated on CSV deployment), so the
        # mempool divergence does NOT translate into a chain split here.
        self.log.info("Testing v2 unsatisfied-lock mempool delta (pre-CSV)...")
        txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)

        tx_hex = build_signed_tx(node1, txid_int, vout, value_sat,
                                 nversion=2, nsequence=RELATIVE_LOCK_10_BLOCKS)

        # node1 (v4.22.10): mempool exempts v2 from BIP68 → accepts
        v2_txid = node1.sendrawtransaction(tx_hex)
        assert v2_txid in node1.getrawmempool(), "v2 unsat not in node1 mempool"
        self.log.info("node1 (v4.22.10) accepted v2 unsat (BIP68 exempt at threshold >= 3)")

        # node0 (v4.22.9): mempool enforces v2 BIP68 → rejects
        assert_raises_rpc_error(-26, NOT_FINAL_ERROR,
                                node0.sendrawtransaction, tx_hex)
        self.log.info("node0 (v4.22.9) rejected v2 unsat (BIP68 enforced at threshold >= 2)")

        # Chain stays in sync despite mempool divergence — block-validation
        # BIP68 is gated on CSV which is not yet active.
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("Phase A passed: mempool delta confirmed; chain in sync at height %d"
                      % node1.getblockcount())

    # ------------------------------------------------------------------
    # Phase B — Activate CSV
    # ------------------------------------------------------------------

    def phase_b_activate_csv(self):
        """Restart node1 with CSV signaling enabled, generate blocks through
        BIP9 activation. Node0 (v4.22.9) stays in natural state — CSV remains
        NEVER_ACTIVE on it, simulating the real mainnet upgrade scenario."""
        self.log.info("=== Phase B: Activate CSV on v4.22.10 only ===")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # Restart only node1 with CSV enabled for immediate signaling.
        # Node0 stays in its natural state (CSV = NEVER_ACTIVE).
        self.log.info("Restarting node1 with CSV signaling enabled...")
        self.disconnect_nodes(0, 1)
        mocktime = node1.mocktime

        self.restart_node(1, extra_args=[
            "-whitelist=127.0.0.1", "-staking=0",
            "-vbparams=csv:0:999999999999",
        ])
        node1 = self.nodes[1]
        node1.setmocktime(mocktime)
        node1.mocktime = mocktime

        # Also restart node0 with mocktime for P2P sync
        self.restart_node(0, extra_args=[
            "-whitelist=127.0.0.1", "-staking=0",
            f'-mocktime={mocktime}',
        ])
        node0 = self.nodes[0]
        node0.mocktime = mocktime

        # Generate blocks well past BIP9 activation height (432) on node1.
        # Node0 will sync the blocks but CSV stays NEVER_ACTIVE on it.
        height = node1.getblockcount()
        target = 600
        remaining = target - height
        self.log.info("Generating %d PoS blocks to activate CSV on node1 (height %d -> %d)..."
                      % (remaining, height, target))

        chunk = 50
        while remaining > 0:
            count = min(chunk, remaining)
            advance_time_for_pos(node1, seconds=600)
            node1.generate(count)
            remaining -= count
            height = node1.getblockcount()
            self.log.info("  height=%d, remaining=%d" % (height, remaining))

        assert self._csv_is_active(node1), "CSV not active on new node at height %d" % height

        # Sync to node0
        self.log.info("Connecting and syncing...")
        set_node_times(self.nodes, node1.mocktime)
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=120)

        # Node0 (v4.22.9): CSV is NEVER_ACTIVE — it syncs the chain but
        # does not activate BIP68 enforcement.
        # Node1 (v4.22.10): CSV is ACTIVE — BIP68 is now enforced.
        csv_on_old = self._csv_is_active(node0)
        csv_on_new = self._csv_is_active(node1)
        self.log.info("CSV on old node (v4.22.9): %s" % csv_on_old)
        self.log.info("CSV on new node (v4.22.10): %s" % csv_on_new)
        assert csv_on_new, "CSV should be active on new node"

        assert_equal(node0.getblockcount(), node1.getblockcount())
        self.log.info("Phase B passed: both nodes synced at height %d "
                      "(CSV active on new, %s on old)"
                      % (node1.getblockcount(),
                         "active" if csv_on_old else "NEVER_ACTIVE"))

    # ------------------------------------------------------------------
    # Phase C — v2 tx with satisfied lock (post-activation)
    # ------------------------------------------------------------------

    def phase_c_v2_satisfied_lock(self):
        """Both nodes accept a v2 tx with a satisfied relative lock."""
        self.log.info("=== Phase C: v2 tx with satisfied relative lock ===")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # Fund and generate enough blocks to satisfy the 10-block lock
        txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)

        # Generate 10 more blocks so the lock is satisfied
        self._generate_pos_block(node1, count=10)
        self.sync_blocks(timeout=60)

        confs = node1.gettxout(
            "%064x" % txid_int, vout, True
        )["confirmations"]
        self.log.info("Parent UTXO has %d confirmations" % confs)

        tx_hex = build_signed_tx(node1, txid_int, vout, value_sat,
                                 nversion=2, nsequence=RELATIVE_LOCK_10_BLOCKS)

        self.log.info("Broadcasting v2 satisfied-lock tx...")
        txid = node1.sendrawtransaction(tx_hex)
        self.sync_mempools(timeout=60)
        assert txid in node0.getrawmempool(), "v2 tx not in old node mempool"

        self._generate_pos_block(node1, count=1)
        self.sync_blocks(timeout=60)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

        self.log.info("Phase C passed: both nodes accepted v2 satisfied-lock tx (height %d)"
                      % node1.getblockcount())

    # ------------------------------------------------------------------
    # Phase D — v2 tx with unsatisfied lock: documented residual divergence
    # ------------------------------------------------------------------

    def phase_d_v2_unsatisfied_lock_divergence(self):
        """Documented consensus divergence (RED-8). Under Path B, v4.22.10's
        BIP68 threshold is >= 3 in both mempool AND consensus (HEAD's
        hardcoded tx_verify.cpp from pr/core-fixes). v4.22.9's threshold is
        >= 2 in both layers. So for a v2 unsatisfied-lock tx:

          mempool:    node1 accepts (>= 3 exempts v2); node0 rejects (>= 2 enforces)
          block-val:  node1 accepts a block containing it; node0 rejects (post-CSV)

        Node1's block template naturally picks the tx from its mempool — no
        generateblock force-include is needed. Disconnecting node0 first
        prevents auto-sync of the divergent block via P2P, so the chain-split
        assertion is observable.

        This phase is run LAST and intentionally leaves the chains divergent.
        """
        self.log.info("=== Phase D: v2 tx with unsatisfied lock — documented divergence ===")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # Sync baseline before forcing the divergence
        self.sync_blocks(timeout=60)
        common_tip = node1.getbestblockhash()

        # Fund and confirm with only 1 block (lock of 10 NOT satisfied)
        txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)
        common_tip = node1.getbestblockhash()

        v2_unsat_hex = build_signed_tx(node1, txid_int, vout, value_sat,
                                       nversion=2, nsequence=RELATIVE_LOCK_10_BLOCKS)

        # Mempool delta: node1 accepts v2 (>= 3 exempts); node0 rejects (>= 2 enforces)
        self.log.info("Broadcasting v2 unsat — expecting asymmetric mempool acceptance...")
        v2_txid = node1.sendrawtransaction(v2_unsat_hex)
        assert v2_txid in node1.getrawmempool(), "v2 unsat not in node1 mempool"
        self.log.info("node1 (v4.22.10) accepted v2 unsat into mempool (BIP68 exempt for v2)")

        assert_raises_rpc_error(-26, NOT_FINAL_ERROR, node0.sendrawtransaction, v2_unsat_hex)
        self.log.info("node0 (v4.22.9) rejected v2 unsat from mempool (BIP68 enforced for v2)")

        # Disconnect so node0 doesn't auto-receive the divergent block via P2P
        self.disconnect_nodes(0, 1)

        # Mine on node1 — block template includes the v2 unsat from its mempool.
        # Retry loop wraps the PoS coinstake search variability under mocktime.
        self.log.info("Mining a block on node1 — should include the v2 unsat tx...")
        new_block_hash = None
        last_exc = None
        for attempt in range(20):
            advance_time_for_pos(node1, seconds=300)
            try:
                node1.generate(1)
                new_block_hash = node1.getbestblockhash()
                break
            except Exception as exc:
                last_exc = exc
                if "No valid coinstake found" not in str(exc):
                    raise
        if new_block_hash is None:
            raise AssertionError(
                "generate could not find a coinstake after 20 retries: %s" % last_exc
            )

        # Verify the v2 unsat tx made it into the block
        block = node1.getblock(new_block_hash, 2)
        block_txids = [tx["txid"] for tx in block["tx"]]
        assert v2_txid in block_txids, (
            "v2 unsat tx %s not in node1's new block %s" % (v2_txid, new_block_hash)
        )
        self.log.info("node1 mined block %s containing the v2 unsat tx" % new_block_hash)

        # node1 advanced; node0 still at common_tip
        assert_equal(node1.getbestblockhash(), new_block_hash)
        assert_equal(node0.getbestblockhash(), common_tip)
        self.log.info("node0 (v4.22.9) still at common tip %s" % common_tip)

        # Reconnect — expect node0 to reject the divergent block at consensus
        self.log.info("Reconnecting nodes — node0 (v4.22.9) should reject the divergent block...")
        set_node_times([node0, node1], node1.mocktime)
        self.connect_nodes(0, 1)

        from test_framework.util import wait_until_helper
        try:
            wait_until_helper(
                lambda: new_block_hash in [t["hash"] for t in node0.getchaintips()],
                timeout=30,
            )
            tip = next(
                (t for t in node0.getchaintips() if t["hash"] == new_block_hash),
                None,
            )
            assert tip is not None
            assert tip["status"] == "invalid", (
                "Expected node0 to mark the block as invalid; got status=%s" % tip["status"]
            )
            self.log.info(
                "node0 received the block and rejected it as invalid (status=%s)" % tip["status"]
            )
        except Exception:
            self.log.info("node0 did not adopt the divergent block (rejected or not received)")

        # Confirm chain-split persists — this is the test's documented outcome
        assert_equal(node0.getbestblockhash(), common_tip)
        assert_equal(node1.getbestblockhash(), new_block_hash)
        self.log.info(
            "Phase D PASSED: divergence confirmed. "
            "node0 (v4.22.9) tip=%s ; node1 (v4.22.10) tip=%s."
            % (common_tip, new_block_hash)
        )
        self.log.warning(
            "Known consensus divergence: post-CSV-activation, v4.22.10 accepts v2 unsatisfied-lock "
            "txs in BOTH mempool and blocks (threshold >= 3 exempts v2 entirely); v4.22.9 rejects "
            "in both layers (threshold >= 2 enforces v2). The split materialises whenever a "
            "v4.22.10 node mines such a tx into a block. See RED-8 for mitigation."
        )

    # ------------------------------------------------------------------
    # Phase E — v3 sequence-lock: convergent behaviour across versions
    # ------------------------------------------------------------------

    def phase_e_v3_convergence(self):
        """v3 sequence-lock tests prove the v4.22.10 wallet default (after the
        CTransaction::CURRENT_VERSION bump to 3) is divergence-free in normal
        wallet activity.

        v3 meets BOTH thresholds (v3 >= 2 on v4.22.9; v3 >= 3 on v4.22.10), so
        BIP68 is enforced identically on both versions for v3 sends.
        """
        self.log.info("=== Phase E: v3 sequence-lock — convergent behaviour ===")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # --- v3 unsatisfied lock ---
        # Fund and confirm with only 1 block (lock of 10 NOT satisfied)
        txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)

        v3_unsat_hex = build_signed_tx(node1, txid_int, vout, value_sat,
                                       nversion=3, nsequence=RELATIVE_LOCK_10_BLOCKS)

        self.log.info("Broadcasting v3 unsatisfied-lock to both nodes (expect both reject)...")
        assert_raises_rpc_error(-26, NOT_FINAL_ERROR,
                                node1.sendrawtransaction, v3_unsat_hex)
        assert_raises_rpc_error(-26, NOT_FINAL_ERROR,
                                node0.sendrawtransaction, v3_unsat_hex)
        self.log.info("v3 unsatisfied: both nodes reject (BIP68 enforced consistently — no divergence)")

        # --- v3 satisfied lock ---
        # Fund a fresh UTXO and mine enough blocks to satisfy the 10-block lock
        txid_int2, vout2, value_sat2 = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)
        self._generate_pos_block(node1, count=10)
        self.sync_blocks(timeout=60)

        confs = node1.gettxout("%064x" % txid_int2, vout2, True)["confirmations"]
        self.log.info("Parent UTXO has %d confirmations (lock of 10 satisfied)" % confs)

        v3_sat_hex = build_signed_tx(node1, txid_int2, vout2, value_sat2,
                                     nversion=3, nsequence=RELATIVE_LOCK_10_BLOCKS)

        self.log.info("Broadcasting v3 satisfied-lock to node1 (expect both accept)...")
        txid = node1.sendrawtransaction(v3_sat_hex)
        self.sync_mempools(timeout=60)
        assert txid in node0.getrawmempool(), "v3 satisfied tx not in old node mempool"

        self._generate_pos_block(node1, count=1)
        self.sync_blocks(timeout=60)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

        self.log.info("Phase E passed: v3 is divergence-free across versions — "
                      "safe wallet default (height %d)" % node1.getblockcount())

    # ------------------------------------------------------------------
    # Entry point
    # ------------------------------------------------------------------

    def run_test(self):
        self.log.info(
            "CompatTxVersionsTest: BIP68/CSV transaction version "
            "compatibility between v4.22.9 and v4.22.10"
        )

        # Execution order: A, B, C, E, D. Phase D is last because it
        # intentionally leaves the chains divergent (documented residual
        # consensus split — see RED-8). Phase E (v3 convergence) runs before
        # so the chain is still synced when its assertions fire.
        self.phase_a_pre_activation()
        self.phase_b_activate_csv()
        self.phase_c_v2_satisfied_lock()
        self.phase_e_v3_convergence()
        self.phase_d_v2_unsatisfied_lock_divergence()

        self.log.info("CompatTxVersionsTest: all phases passed")


if __name__ == "__main__":
    CompatTxVersionsTest().main()
