#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Copyright (c) 2014-2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP68 / CSV transaction version compatibility with a previous release.

Both the current build and v4.22.9 releases apply BIP68 sequence locks and
OP_CHECKSEQUENCEVERIFY from nVersion 2, as Bitcoin does, and both treat
nVersion 2 as the highest standard version. STANDARD_LOCKTIME_VERIFY_FLAGS sets
LOCKTIME_VERIFY_SEQUENCE whatever the deployment state, so the mempools apply
BIP68 even before CSV is active. The two must therefore agree on every v2
transaction, before and after CSV, and the current wallet's default
transactions must relay to the old node.

Test phases:

  Phase A: before CSV, a v2 tx with an unsatisfied relative lock is refused
    by both mempools (non-BIP68-final).

  Phase B: activate CSV via BIP9 signalling on the current build only. The
    old node follows the chain; CSV stays NEVER_ACTIVE there because the
    release carries mainnet's deployment state.

  Phase C: a v2 tx with a satisfied lock is accepted by both and mined; the
    block syncs.

  Phase D: after CSV, a v2 tx with an unsatisfied lock is still refused by
    both mempools.

  Phase E: a v3 tx is non-standard on both ("version"), and the current
    wallet's default transaction is v2 and relays to the old node.

v1 transactions are not tested: the mempool refuses them in the PoS era.

Requires previous releases (the v4.22.9.3 binary), see test/README.md.
"""

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
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
VERSION_NEW = None     # current build


def build_signed_tx(node, txid_int, vout, value_sat, nversion, nsequence, fee_sat=1_000_000):
    """Build a tx with the given nVersion and nSequence paying one of the
    node's P2PKH addresses (standard on both versions), sign it, return hex."""
    output_value = value_sat - fee_sat
    if output_value <= 0:
        raise AssertionError("Fee (%d) exceeds value (%d)" % (fee_sat, value_sat))
    spk = bytes.fromhex(node.getaddressinfo(node.getnewaddress("", "legacy"))["scriptPubKey"])
    tx = CTransaction()
    tx.nVersion = nversion
    tx.vin = [CTxIn(COutPoint(txid_int, vout), nSequence=nsequence)]
    tx.vout = [CTxOut(output_value, spk)]
    signed = node.signrawtransactionwithwallet(tx.serialize().hex())
    assert signed["complete"], signed
    return signed["hex"]


class CompatTxVersionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # node0 (v4.22.9): natural state — CSV is NEVER_ACTIVE in compiled
        # chainparams, matching the current mainnet v4.22.9 release.
        # node1 (current build): also starts with CSV disabled (regtest default
        # has nStartTime=0, so override to NEVER_ACTIVE initially).
        # -whitelist prevents mocktime-driven peer disconnections.
        # -acceptnonstdtxn=0 applies standardness on regtest (phase E).
        self.extra_args = [
            ["-whitelist=127.0.0.1", "-staking=0", "-acceptnonstdtxn=0"],                        # node0 = old v4.22.9 (natural)
            ["-whitelist=127.0.0.1", "-staking=0", "-acceptnonstdtxn=0", "-vbparams=csv:-2:0"],  # node1 = current build (CSV off initially)
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

    def _assert_both_refuse_unsatisfied_v2(self):
        """Fund a fresh UTXO with one confirmation and offer both nodes a v2
        spend with a 10-block relative lock: both must refuse it."""
        node0, node1 = self.nodes
        txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)
        tx_hex = build_signed_tx(node1, txid_int, vout, value_sat,
                                 nversion=2, nsequence=RELATIVE_LOCK_10_BLOCKS)
        assert_raises_rpc_error(-26, NOT_FINAL_ERROR, node1.sendrawtransaction, tx_hex)
        assert_raises_rpc_error(-26, NOT_FINAL_ERROR, node0.sendrawtransaction, tx_hex)

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

        self.log.info("Testing v2 unsatisfied-lock (pre-CSV)...")
        self._assert_both_refuse_unsatisfied_v2()

        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("Phase A passed: both mempools refuse v2 unsat; chain in sync at height %d"
                      % node1.getblockcount())

    # ------------------------------------------------------------------
    # Phase B — Activate CSV
    # ------------------------------------------------------------------

    def phase_b_activate_csv(self):
        """Restart node1 with CSV signaling enabled, generate blocks through
        BIP9 activation. Node0 (v4.22.9) stays in natural state — CSV remains
        NEVER_ACTIVE on it, simulating the real mainnet upgrade scenario."""
        self.log.info("=== Phase B: Activate CSV on the current build only ===")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        # Restart only node1 with CSV enabled for immediate signaling.
        # Node0 stays in its natural state (CSV = NEVER_ACTIVE).
        self.log.info("Restarting node1 with CSV signaling enabled...")
        self.disconnect_nodes(0, 1)
        mocktime = node1.mocktime

        self.restart_node(1, extra_args=[
            "-whitelist=127.0.0.1", "-staking=0", "-acceptnonstdtxn=0",
            "-vbparams=csv:0:999999999999",
        ])
        node1 = self.nodes[1]
        node1.setmocktime(mocktime)
        node1.mocktime = mocktime

        # Also restart node0 with mocktime for P2P sync
        self.restart_node(0, extra_args=[
            "-whitelist=127.0.0.1", "-staking=0", "-acceptnonstdtxn=0",
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
        # Node1 (current build): CSV is ACTIVE — BIP68 is now enforced.
        csv_on_old = self._csv_is_active(node0)
        csv_on_new = self._csv_is_active(node1)
        self.log.info("CSV on old node (v4.22.9): %s" % csv_on_old)
        self.log.info("CSV on new node (current build): %s" % csv_on_new)
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
    # Phase D: v2 tx with unsatisfied lock (post-activation)
    # ------------------------------------------------------------------

    def phase_d_v2_unsatisfied_lock(self):
        """After CSV, both mempools still refuse a v2 tx whose relative lock
        is not yet satisfied: the thresholds match, so there is no version
        whose sequence locks one node applies and the other ignores."""
        self.log.info("=== Phase D: v2 tx with unsatisfied lock (post-CSV) ===")
        self._assert_both_refuse_unsatisfied_v2()
        assert_equal(self.nodes[0].getbestblockhash(), self.nodes[1].getbestblockhash())
        self.log.info("Phase D passed: both nodes refuse v2 unsat after CSV")

    # ------------------------------------------------------------------
    # Phase E: v3 non-standard on both; wallet default relays
    # ------------------------------------------------------------------

    def phase_e_v3_and_wallet_default(self):
        """v3 is non-standard on both versions, and the current wallet's
        default transaction is v2, which the old node relays."""
        self.log.info("=== Phase E: v3 non-standard on both; wallet default is v2 ===")
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
        self.sync_blocks(timeout=60)
        v3_hex = build_signed_tx(node1, txid_int, vout, value_sat,
                                 nversion=3, nsequence=0xffffffff)
        assert_raises_rpc_error(-26, "version", node1.sendrawtransaction, v3_hex)
        assert_raises_rpc_error(-26, "version", node0.sendrawtransaction, v3_hex)
        self.log.info("v3: both nodes refuse it as non-standard")

        txid = node1.sendtoaddress(node0.getnewaddress(), 5)
        assert_equal(node1.getrawtransaction(txid, True)["version"], 2)
        self.sync_mempools(timeout=60)
        assert txid in node0.getrawmempool(), "wallet default tx not relayed to the old node"

        self._generate_pos_block(node1, count=1)
        self.sync_blocks(timeout=60)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("Phase E passed: wallet default is v2 and relays to the old node (height %d)"
                      % node1.getblockcount())

    # ------------------------------------------------------------------
    # Entry point
    # ------------------------------------------------------------------

    def run_test(self):
        self.log.info("node0: %s, node1: %s",
                      self.nodes[0].getnetworkinfo()["subversion"],
                      self.nodes[1].getnetworkinfo()["subversion"])

        self.phase_a_pre_activation()
        self.phase_b_activate_csv()
        self.phase_c_v2_satisfied_lock()
        self.phase_d_v2_unsatisfied_lock()
        self.phase_e_v3_and_wallet_default()

        self.log.info("CompatTxVersionsTest: all phases passed")


if __name__ == "__main__":
    CompatTxVersionsTest().main()
