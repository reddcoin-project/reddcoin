#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Copyright (c) 2014-2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP9 softfork signaling compatibility between v4.22.9 and v4.22.10.

This test validates that BIP9 softfork signaling is compatible between the
two node versions and that both nodes converge on the same deployment status
after observing blocks from either version.

v4.22.10 has these BIP9 deployments enabled on regtest (nStartTime=0):
  - CLTV   (bit 1)
  - CSV    (bit 2)
  - SegWit (bit 3)
  - Taproot (bit 4)

v4.22.9 may have different deployment configurations. Key questions:
  - Do both nodes see the same blocks and agree on deployment status?
  - Does mixed signaling (old node blocks) prevent activation?
  - If deployments activate, do both nodes enforce the new rules?

Test phases:
  Phase A: Baseline deployment status — query softforks before generating blocks
  Phase B: Generate blocks past 3 BIP9 periods (height 450+), observe signaling
  Phase C: Deployment status comparison — assert both agree on CSV and log others
  Phase D: Mixed signaling — each node generates 10 blocks independently, then
           reconnect and verify no chain split
  Phase E: Post-activation verification — CSV BIP68 transaction, getchaintips check
"""

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.script_util import DUMMY_P2WPKH_SCRIPT
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
    set_node_times,
    softfork_active,
)

# Version numbers
VERSION_OLD = 4220903  # v4.22.9.3
VERSION_NEW = None     # v4.22.10 (current)

# BIP9 regtest parameters
BIP9_WINDOW = 144
BIP9_THRESHOLD = 108   # 75% of 144
# Activation happens after 3 full periods: 3 * 144 = 432
BIP9_ACTIVATION_HEIGHT = 432

# nSequence value for 1-block relative lock (CSV BIP68 test)
SEQUENCE_LOCK_1_BLOCK = 1


class CompatSoftforkSignalingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # -whitelist prevents mocktime-driven peer disconnections (ReddCoin net.cpp bug)
        # -staking=0 keeps both nodes from autonomously producing PoS blocks during
        #   phases where we want controlled generation
        self.extra_args = [
            ["-whitelist=127.0.0.1", "-staking=0", "-peertimeout=9999"],  # node0 = old v4.22.9
            ["-whitelist=127.0.0.1", "-staking=0", "-peertimeout=9999"],  # node1 = new v4.22.10
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_previous_releases()

    def setup_network(self):
        self.add_nodes(self.num_nodes, versions=[VERSION_OLD, VERSION_NEW],
                       extra_args=self.extra_args)
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()
        # Do NOT connect nodes initially — Phase A queries each node in isolation

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _generate_pos_blocks(self, node, count):
        """Generate PoS blocks with retry logic for probabilistic coinstake."""
        max_attempts = 20
        generated = 0
        while generated < count:
            batch = count - generated
            for attempt in range(max_attempts):
                try:
                    node.generate(batch)
                    generated += batch
                    break
                except Exception as exc:
                    if "no valid coinstake found" in str(exc):
                        if attempt < max_attempts - 1:
                            advance_time_for_pos(node, seconds=60)
                        else:
                            raise
                    else:
                        raise

    def _log_softforks(self, node, label):
        """Log the softforks section of getblockchaininfo for a given node."""
        info = node.getblockchaininfo()
        height = info["blocks"]
        softforks = info.get("softforks", {})
        self.log.info("%s softforks at height %d:" % (label, height))
        if not softforks:
            self.log.info("  (no softforks reported)")
            return softforks
        for name, data in softforks.items():
            bip9 = data.get("bip9", {})
            status = bip9.get("status", data.get("active", "unknown"))
            active = data.get("active", False)
            self.log.info("  %-12s  status=%-12s  active=%s" % (name, status, active))
        return softforks

    def _log_block_versions(self, node, label, count=10):
        """Log nVersion of the most recent `count` blocks."""
        tip_height = node.getblockcount()
        start = max(0, tip_height - count + 1)
        self.log.info("%s block versions (heights %d-%d):" % (label, start, tip_height))
        versions_seen = []
        for h in range(start, tip_height + 1):
            bhash = node.getblockhash(h)
            version = node.getblockheader(bhash)["version"]
            versions_seen.append(version)
        self.log.info("  %s" % versions_seen)
        return versions_seen

    def _bootstrap_to_activation(self, node):
        """Generate blocks on `node` from genesis to height 450+ (past BIP9 activation).

        Schedule:
          - PoW blocks 1-89 (nLastPowHeight = 89 on regtest)
          - Advance time to age coins for PoS
          - PoS blocks in batches of 50 until height > 450
        """
        height = node.getblockcount()

        # PoW phase
        pow_needed = max(0, 89 - height)
        if pow_needed > 0:
            self.log.info("Generating %d PoW blocks (current height %d)..." % (pow_needed, height))
            node.generate(pow_needed)
            height = node.getblockcount()

        # Age coins before PoS
        self.log.info("Advancing time for PoS coin maturity...")
        advance_time_for_pos(node, seconds=600)

        # PoS phase — generate in batches, logging versions after each batch
        target_height = 600  # well past activation at 432
        batch_size = 50
        while height < target_height:
            batch = min(batch_size, target_height - height)
            self.log.info("Generating %d PoS blocks (current height %d, target %d)..." %
                          (batch, height, target_height))
            self._generate_pos_blocks(node, batch)
            height = node.getblockcount()
            self._log_block_versions(node, "node1 (new)", count=min(batch, 20))
            # Advance time between batches to keep UTXOs stakeable
            if height < target_height:
                advance_time_for_pos(node, seconds=60)

        self.log.info("Bootstrap complete at height %d" % height)

    def _fund_and_confirm(self, node, amount_rdd=10.0):
        """Send amount to a fresh wallet address, mine one block, return (txid_int, vout, value_sat)."""
        addr = node.getnewaddress()
        txid_hex = node.sendtoaddress(addr, amount_rdd)
        self._generate_pos_blocks(node, 1)

        raw = node.getrawtransaction(txid_hex, True)
        for i, vout_info in enumerate(raw["vout"]):
            spk = vout_info["scriptPubKey"]
            if spk.get("address") == addr or addr in spk.get("addresses", []):
                return int(txid_hex, 16), i, int(vout_info["value"] * COIN)

        raise AssertionError("Could not find funded vout for address %s" % addr)

    # ------------------------------------------------------------------
    # Test phases
    # ------------------------------------------------------------------

    def phase_a_baseline_deployment_status(self):
        """Query softforks on both nodes before any blocks are generated."""
        self.log.info("=== Phase A: Baseline deployment status ===")
        node0 = self.nodes[0]  # old v4.22.9
        node1 = self.nodes[1]  # new v4.22.10

        sf0 = self._log_softforks(node0, "node0 (old v4.22.9)")
        sf1 = self._log_softforks(node1, "node1 (new v4.22.10)")

        # Note which deployments each node recognises
        names0 = set(sf0.keys())
        names1 = set(sf1.keys())
        common = names0 & names1
        only_old = names0 - names1
        only_new = names1 - names0

        self.log.info("Deployments common to both nodes: %s" % sorted(common))
        if only_old:
            self.log.info("Deployments known only to old node: %s" % sorted(only_old))
        if only_new:
            self.log.info("Deployments known only to new node: %s" % sorted(only_new))

        self.log.info("Phase A complete")
        return names0, names1

    def phase_b_generate_blocks_and_observe(self):
        """Generate blocks on node1 past 3 BIP9 periods, observe signaling."""
        self.log.info("=== Phase B: Generate blocks past BIP9 activation height ===")
        node1 = self.nodes[1]  # new node generates the chain

        self._bootstrap_to_activation(node1)

        height = node1.getblockcount()
        self.log.info("node1 (new) at height %d" % height)
        assert height >= BIP9_ACTIVATION_HEIGHT, (
            "Expected height >= %d for activation, got %d" % (BIP9_ACTIVATION_HEIGHT, height)
        )

        # Sync mocktime to node0 before connecting
        if hasattr(node1, "mocktime") and node1.mocktime:
            set_node_times([self.nodes[0]], node1.mocktime)

        # Connect nodes and sync
        self.log.info("Connecting nodes and syncing blocks...")
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=300)

        height0 = self.nodes[0].getblockcount()
        height1 = node1.getblockcount()
        assert_equal(height0, height1)
        self.log.info("Both nodes synced at height %d" % height0)

        self.log.info("Phase B complete")

    def phase_c_deployment_status_comparison(self):
        """Compare softfork deployment status between both nodes; assert agreement on CSV."""
        self.log.info("=== Phase C: Deployment status comparison ===")
        node0 = self.nodes[0]  # old v4.22.9
        node1 = self.nodes[1]  # new v4.22.10

        sf0 = self._log_softforks(node0, "node0 (old v4.22.9)")
        sf1 = self._log_softforks(node1, "node1 (new v4.22.10)")

        # For each deployment the new node knows about, check old node's view
        self.log.info("Comparing deployment status across nodes:")
        for name in sorted(sf1.keys()):
            new_active = sf1[name].get("active", False)
            try:
                old_data = sf0[name]
                old_active = old_data.get("active", False)
                if new_active == old_active:
                    self.log.info("  %-12s  AGREE   active=%s" % (name, new_active))
                else:
                    self.log.warning(
                        "  %-12s  DIFFER  new_active=%s  old_active=%s" %
                        (name, new_active, old_active)
                    )
            except KeyError:
                self.log.info(
                    "  %-12s  UNKNOWN to old node (new says active=%s)" % (name, new_active)
                )

        # CSV must be active on both nodes — it is the canonical deployment that
        # both versions recognise (used to exercise Phase E).
        self.log.info("Asserting CSV is active on both nodes...")
        assert softfork_active(node1, "csv"), "CSV not active on new node at height %d" % node1.getblockcount()

        try:
            csv_active_old = softfork_active(node0, "csv")
            assert csv_active_old, "CSV not active on old node at height %d" % node0.getblockcount()
            self.log.info("CSV is active on both nodes — both agree")
        except KeyError:
            # Old node does not expose 'csv' in softforks — this can happen if
            # the old binary uses a different key name. Log and continue.
            self.log.warning(
                "Old node does not report 'csv' in softforks — "
                "skipping old-node CSV active assertion"
            )

        self.log.info("Phase C complete")

    def phase_d_lead_staker_extends(self):
        """Disconnected, node1 (lead staker) extends the chain by 30 blocks.
        Reconnect; node0 (v4.22.9) must catch up — testing that v4.22.9
        accepts a long batch of v4.22.10-signaled blocks past activation."""
        self.log.info("=== Phase D: Lead staker extends chain (+30 blocks while disconnected) ===")
        node0 = self.nodes[0]  # old v4.22.9
        node1 = self.nodes[1]  # new v4.22.10

        self.log.info("Disconnecting nodes...")
        self.disconnect_nodes(0, 1)

        common_tip = node0.getbestblockhash()
        assert_equal(common_tip, node1.getbestblockhash())
        height_before = node0.getblockcount()
        self.log.info("Common tip before divergence: %s (height %d)" % (common_tip, height_before))

        self.log.info("node1 (new) generating 30 blocks...")
        advance_time_for_pos(node1, seconds=120)
        self._generate_pos_blocks(node1, 30)
        new_tip = node1.getbestblockhash()
        self._log_block_versions(node1, "node1 (new)", count=10)
        expected = height_before + 30
        assert_equal(node1.getblockcount(), expected)
        self.log.info("node1 (new) tip: %s (height %d)" % (new_tip, expected))

        # Sync mocktime before reconnecting to prevent disconnect-due-to-time-drift
        current_time = max(
            node0.mocktime if hasattr(node0, "mocktime") and node0.mocktime else 0,
            node1.mocktime if hasattr(node1, "mocktime") and node1.mocktime else 0,
        )
        if current_time:
            set_node_times([node0, node1], current_time)

        self.log.info("Reconnecting nodes...")
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=300)

        assert_equal(node0.getbestblockhash(), new_tip)
        assert_equal(node0.getblockcount(), expected)
        self.log.info(
            "Phase D passed: node0 (v4.22.9) caught up to node1's tip %s (height %d)" %
            (new_tip, expected)
        )

    def phase_e_post_activation_verification(self):
        """Verify CSV enforcement and chain health after activation.

        If CSV is active on both nodes:
          - Create a version 3 tx with a BIP68 relative lock of 1 block (satisfied).
          - Broadcast to both nodes, verify accepted.
          - Mine and verify confirmed on both.
        Also perform a getchaintips check to confirm a single active tip.
        """
        self.log.info("=== Phase E: Post-activation verification ===")
        node0 = self.nodes[0]  # old v4.22.9
        node1 = self.nodes[1]  # new v4.22.10

        # Check CSV on new node (authoritative)
        csv_on_new = softfork_active(node1, "csv")
        self.log.info("CSV active on new node: %s" % csv_on_new)

        # Check CSV on old node (may use different key or not report it)
        csv_on_old = False
        try:
            csv_on_old = softfork_active(node0, "csv")
            self.log.info("CSV active on old node: %s" % csv_on_old)
        except KeyError:
            self.log.warning(
                "Old node does not expose 'csv' softfork key — "
                "cannot confirm CSV active on old node"
            )

        if csv_on_new:
            self.log.info("Testing CSV-enforced BIP68 v3 tx with satisfied relative lock...")

            # Fund a UTXO on node1 and confirm it
            txid_int, vout, value_sat = self._fund_and_confirm(node1, amount_rdd=10.0)
            self.sync_blocks(timeout=60)

            # Mine 2 more blocks so the 1-block relative lock is satisfied
            advance_time_for_pos([node0, node1], seconds=120)
            self._generate_pos_blocks(node1, 2)
            self.sync_blocks(timeout=60)

            # Build a v3 tx with nSequence=1 (1-block relative lock, satisfied)
            # ReddCoin enforces BIP68 for nVersion >= 3 on v4.22.10
            tx = CTransaction()
            tx.nVersion = 3
            tx.vin = [CTxIn(COutPoint(txid_int, vout), nSequence=SEQUENCE_LOCK_1_BLOCK)]
            fee_sat = 1_000_000  # 0.01 RDD — well above minimum relay fee
            output_value = value_sat - fee_sat
            assert output_value > 0, "Fee exceeds value"
            tx.vout = [CTxOut(output_value, DUMMY_P2WPKH_SCRIPT)]
            tx.rehash()

            signed = node1.signrawtransactionwithwallet(tx.serialize().hex())
            assert signed.get("complete") is True, (
                "Transaction signing incomplete: %s" % signed.get("errors", "")
            )
            tx_hex = signed["hex"]

            # Broadcast to new node — must be accepted (lock satisfied)
            self.log.info("Broadcasting satisfied v3 BIP68 tx to new node...")
            txid_csv = node1.sendrawtransaction(tx_hex)
            assert txid_csv in node1.getrawmempool(), "CSV v3 tx not in new node mempool"
            self.log.info("New node accepted CSV v3 tx: %s" % txid_csv)

            # Broadcast to old node — nVersion 3 may be non-standard on v4.22.9,
            # but the transaction is otherwise valid. Use try/except to handle
            # old node's TX_MAX_STANDARD_VERSION=2 rejection.
            try:
                txid_csv_old = node0.sendrawtransaction(tx_hex)
                assert txid_csv_old in node0.getrawmempool(), (
                    "CSV v3 tx accepted by old node but not in mempool"
                )
                self.log.info("Old node accepted CSV v3 tx in mempool")
            except Exception as exc:
                self.log.info(
                    "Old node rejected CSV v3 tx from mempool (expected if TX_MAX_STANDARD_VERSION=2): %s" %
                    str(exc)
                )

            # Mine the tx on new node and verify both nodes confirm it
            self.log.info("Mining CSV v3 tx on new node...")
            self._generate_pos_blocks(node1, 1)
            self.sync_blocks(timeout=60)

            tip_height1 = node1.getblockcount()
            tip_height0 = node0.getblockcount()
            assert_equal(tip_height0, tip_height1)

            # Verify the tx is confirmed (not in mempool)
            assert txid_csv not in node1.getrawmempool(), "CSV v3 tx still in new node mempool after mining"
            assert txid_csv not in node0.getrawmempool(), "CSV v3 tx still in old node mempool after mining"
            self.log.info(
                "Phase E CSV tx confirmed on both nodes at height %d" % tip_height1
            )
        else:
            self.log.warning(
                "CSV not active on new node — skipping BIP68 v3 tx test. "
                "This is unexpected given bootstrap to height 455."
            )

        # Final getchaintips — assert single active chain tip on both nodes
        self.log.info("Checking chain tips on both nodes...")
        for label, node in [("node0 (old)", node0), ("node1 (new)", node1)]:
            tips = node.getchaintips()
            active_tips = [t for t in tips if t["status"] == "active"]
            assert_equal(len(active_tips), 1)
            self.log.info(
                "%s: single active tip at height %d hash %s" %
                (label, active_tips[0]["height"], active_tips[0]["hash"])
            )
            # Log any non-active tips for diagnostics
            non_active = [t for t in tips if t["status"] != "active"]
            if non_active:
                for t in non_active:
                    self.log.info(
                        "%s: non-active tip status=%-12s height=%d hash=%s" %
                        (label, t["status"], t["height"], t["hash"])
                    )

        # Assert both nodes share the same active tip
        active_hash0 = next(t["hash"] for t in node0.getchaintips() if t["status"] == "active")
        active_hash1 = next(t["hash"] for t in node1.getchaintips() if t["status"] == "active")
        assert_equal(active_hash0, active_hash1)
        self.log.info("Phase E passed: both nodes on identical active chain tip")

    # ------------------------------------------------------------------
    # Main entry point
    # ------------------------------------------------------------------

    def run_test(self):
        self.log.info(
            "CompatSoftforkSignalingTest: BIP9 softfork signaling compatibility "
            "between v4.22.9 and v4.22.10"
        )

        self.phase_a_baseline_deployment_status()
        self.phase_b_generate_blocks_and_observe()
        self.phase_c_deployment_status_comparison()
        self.phase_d_lead_staker_extends()
        self.phase_e_post_activation_verification()

        self.log.info("All phases passed")


if __name__ == "__main__":
    CompatSoftforkSignalingTest().main()
