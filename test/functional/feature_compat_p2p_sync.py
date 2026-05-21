#!/usr/bin/env python3
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Compatibility P2P sync test between ReddCoin v4.22.9 and v4.22.10.

v4.22.10 (node1) is the lead staker — it bootstraps the chain and produces
all PoS blocks during the test phases.  v4.22.9 (node0) joins later as a
sync-only follower.  This split is required because v4.22.9 cannot reliably
generate PoS blocks on regtest (CreateCoinStake silently returns no stake;
the node falls back to a PoW template which is rejected past nLastPowHeight).

  Phase A — Bootstrap (v4.22.10 leads to height 250):
    Node1 mines 89 PoW + 161 PoS, then node0 is restarted with -mocktime and
    syncs forward.  Funds also flow to node0 so it can broadcast txns later.

  Phase B — Real-time relay (+30 blocks while connected):
    Node1 generates 30 PoS blocks.  Both nodes stay connected; node0 must
    receive every block via P2P inv/getdata in real time.

  Phase C — Bidirectional transaction relay (+1 block):
    Each node sends coins to the other.  Node1 confirms both txns in one PoS
    block; both nodes must see them confirmed.

  Phase D — Catch-up sync (+50 blocks while disconnected, twice):
    Disconnect, node1 mines 50 PoS blocks, reconnect, node0 catches up.
    Repeat once more to test repeated long-gap sync.

Requires previous releases (v4.22.9 binary), see test/README.md.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
    set_node_times,
)


PHASE_A_POW_BLOCKS = 89
PHASE_A_POS_BLOCKS = 161
PHASE_A_TARGET_HEIGHT = PHASE_A_POW_BLOCKS + PHASE_A_POS_BLOCKS  # 250

# Funding for node0 so it can broadcast txns in Phase C
NODE0_FUNDING_UTXOS = 10
NODE0_FUNDING_AMOUNT = 5000  # RDD per UTXO

PHASE_B_BLOCKS = 30
PHASE_D_BLOCKS_PER_ROUND = 50
PHASE_D_ROUNDS = 2


class CompatP2PSyncTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            # node0 — old binary (v4.22.9), follower
            ["-whitelist=127.0.0.1", "-staking=0"],
            # node1 — current binary (v4.22.10), lead staker
            ["-whitelist=127.0.0.1", "-staking=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_previous_releases()

    def setup_network(self):
        """Nodes start disconnected so node1 can build the chain without
        time-drift disconnects from v4.22.9's net.cpp realtime/mocktime bug."""
        self.add_nodes(
            self.num_nodes,
            extra_args=self.extra_args,
            versions=[4220900, None],
        )
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _sync_time_and_connect(self, leader):
        """Sync mocktime from *leader* to all nodes, then connect 0 <-> 1."""
        set_node_times(self.nodes, leader.mocktime)
        self.connect_nodes(0, 1)

    def _disconnect(self):
        self.disconnect_nodes(0, 1)

    def _restart_node0_with_mocktime(self, mocktime):
        """Restart node0 with -mocktime so v4.22.9 doesn't disconnect node1
        due to its realtime/mocktime mismatch in net.cpp."""
        self.restart_node(0, extra_args=self.extra_args[0] + [f'-mocktime={mocktime}'])
        self.nodes[0].mocktime = mocktime

    # ------------------------------------------------------------------
    # Phase A — bootstrap on node1, fund node0, sync
    # ------------------------------------------------------------------

    def _phase_a(self):
        self.log.info(
            f"Phase A: bootstrap chain on node1 to height {PHASE_A_TARGET_HEIGHT}"
        )
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        self.log.info(f"  Generating {PHASE_A_POW_BLOCKS} PoW blocks on node1...")
        node1.generate(PHASE_A_POW_BLOCKS)
        assert_equal(node1.getblockcount(), PHASE_A_POW_BLOCKS)

        self.log.info("  Advancing time for PoS coin age...")
        advance_time_for_pos(node1, seconds=600)

        self.log.info(f"  Generating {PHASE_A_POS_BLOCKS} PoS blocks on node1...")
        node1.generate(PHASE_A_POS_BLOCKS)
        assert_equal(node1.getblockcount(), PHASE_A_TARGET_HEIGHT)

        node1_hash = node1.getbestblockhash()
        self.log.info(f"  node1 tip: height={PHASE_A_TARGET_HEIGHT} hash={node1_hash}")

        self.log.info(
            f"  Funding node0 with {NODE0_FUNDING_UTXOS} x {NODE0_FUNDING_AMOUNT} RDD..."
        )
        for _ in range(NODE0_FUNDING_UTXOS):
            node1.sendtoaddress(node0.getnewaddress(), NODE0_FUNDING_AMOUNT)
        advance_time_for_pos(node1, seconds=600)
        node1.generate(10)  # confirm the funding txs

        self.log.info("  Restarting node0 with mocktime for sync...")
        self._restart_node0_with_mocktime(node1.mocktime)

        self.log.info("  Connecting and syncing...")
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=240)

        node0 = self.nodes[0]
        final_height = node1.getblockcount()
        assert_equal(node0.getblockcount(), final_height)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info(f"  Phase A passed at height {final_height}.")

    # ------------------------------------------------------------------
    # Phase B — node1 stakes 30 while connected (real-time relay)
    # ------------------------------------------------------------------

    def _phase_b(self):
        node0 = self.nodes[0]
        node1 = self.nodes[1]
        height_before = node1.getblockcount()
        self.log.info(
            f"Phase B: node1 generates {PHASE_B_BLOCKS} PoS blocks "
            f"(connected; real-time relay) from {height_before}"
        )

        advance_time_for_pos(node1, seconds=600)
        node1.generate(PHASE_B_BLOCKS)
        expected = height_before + PHASE_B_BLOCKS
        assert_equal(node1.getblockcount(), expected)

        self.sync_blocks(timeout=120)
        assert_equal(node0.getblockcount(), expected)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info(f"  Phase B passed: both nodes at height {expected}.")

    # ------------------------------------------------------------------
    # Phase C — bidirectional transaction relay; node1 confirms
    # ------------------------------------------------------------------

    def _phase_c(self):
        self.log.info("Phase C: bidirectional transaction relay")
        node0, node1 = self.nodes[0], self.nodes[1]

        addr0 = node0.getnewaddress()
        addr1 = node1.getnewaddress()

        self.log.info("  node0 sends 1000 RDD to node1...")
        txid_0to1 = node0.sendtoaddress(addr1, 1000)
        self.sync_mempools()
        assert txid_0to1 in node1.getrawmempool()

        self.log.info("  node1 sends 500 RDD to node0...")
        txid_1to0 = node1.sendtoaddress(addr0, 500)
        self.sync_mempools()
        assert txid_1to0 in node0.getrawmempool()

        self.log.info("  Confirming both txns in one PoS block on node1...")
        advance_time_for_pos(node1, seconds=600)
        node1.generate(1)
        self.sync_blocks(timeout=60)

        for tx in (txid_0to1, txid_1to0):
            assert tx not in node0.getrawmempool()
            assert tx not in node1.getrawmempool()

        assert node0.gettransaction(txid_0to1)["confirmations"] >= 1
        assert node1.gettransaction(txid_1to0)["confirmations"] >= 1
        self.log.info(f"  Phase C passed at height {node1.getblockcount()}.")

    # ------------------------------------------------------------------
    # Phase D — catch-up sync after disconnected mining
    # ------------------------------------------------------------------

    def _phase_d(self):
        self.log.info(
            f"Phase D: catch-up sync ({PHASE_D_ROUNDS} rounds of "
            f"{PHASE_D_BLOCKS_PER_ROUND} blocks while disconnected)"
        )
        node0, node1 = self.nodes[0], self.nodes[1]

        for round_num in range(1, PHASE_D_ROUNDS + 1):
            self.log.info(f"  Round {round_num}: disconnect, node1 mines, reconnect...")
            self._disconnect()

            height_before = node1.getblockcount()
            advance_time_for_pos(node1, seconds=600)
            node1.generate(PHASE_D_BLOCKS_PER_ROUND)
            expected = height_before + PHASE_D_BLOCKS_PER_ROUND
            assert_equal(node1.getblockcount(), expected)
            self.log.info(f"    node1 mined to height {expected}")

            self._sync_time_and_connect(node1)
            self.sync_blocks(timeout=180)
            assert_equal(node0.getblockcount(), expected)
            assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
            self.log.info(f"    node0 caught up to height {expected}")

        self.log.info(f"  Phase D passed at height {node1.getblockcount()}.")

    # ------------------------------------------------------------------
    # Entry point
    # ------------------------------------------------------------------

    def run_test(self):
        self.log.info(
            "CompatP2PSyncTest: validating P2P sync between "
            "v4.22.9 (node0, follower) and v4.22.10 (node1, lead staker)"
        )

        self._phase_a()
        self._phase_b()
        self._phase_c()
        self._phase_d()

        self.log.info("All phases passed — compat P2P sync test complete.")


if __name__ == "__main__":
    CompatP2PSyncTest().main()
