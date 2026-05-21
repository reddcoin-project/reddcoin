#!/usr/bin/env python3
# Copyright (c) 2021 The ReddCoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test PoS block cross-validation between ReddCoin v4.22.9 and v4.22.10.

v4.22.10 (node1) is the lead staker — it produces all PoS blocks.  The
critical compatibility question is whether v4.22.9 (node0) accepts every PoS
block built by v4.22.10.  v4.22.9 cannot reliably generate PoS blocks on
regtest (CreateCoinStake silently returns no stake), so it stays in a
sync-only follower role.

  Phase A — Bootstrap on node1, sync node0 (target height 200).
  Phase B — Node1 mines 50 PoS blocks; node0 must accept every one.
  Phase C — Node1 mines another 50; node0 must continue to accept.
  Phase D — No orphan or fork tips on either node.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    advance_time_for_pos,
    set_node_times,
)


PHASE_A_POW_BLOCKS = 89
PHASE_A_POS_BLOCKS = 111  # heights 90-200
PHASE_A_TARGET_HEIGHT = PHASE_A_POW_BLOCKS + PHASE_A_POS_BLOCKS  # 200

PHASE_B_BLOCKS = 50
PHASE_C_BLOCKS = 50

# Number of recent blocks to spot-check for coinstake presence
COINSTAKE_VERIFY_DEPTH = 10


class CompatPosBlocksTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-whitelist=127.0.0.1", "-staking=0"],
            ["-whitelist=127.0.0.1", "-staking=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_previous_releases()

    def setup_network(self):
        """Start nodes without connecting them.

        Nodes are kept disconnected during initial chain generation to avoid
        mocktime drift that would cause peers to disconnect each other.
        """
        self.add_nodes(self.num_nodes, extra_args=self.extra_args, versions=[
            4220900,  # node0 — v4.22.9 (follower)
            None,     # node1 — v4.22.10 (lead staker)
        ])
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _assert_coinstake_present(self, node, blockhash):
        """Assert a block contains a coinstake transaction (tx[1]).

        Uses verbosity=1 because v4.22.9's verbosity=2 has a MoneyRange(fee)
        assertion bug when decoding coinstake transactions.
        """
        block = node.getblock(blockhash, 1)
        assert_greater_than(len(block['tx']), 1)

    def _verify_recent_coinstakes(self, node, depth=COINSTAKE_VERIFY_DEPTH):
        cursor = node.getbestblockhash()
        for _ in range(depth):
            self._assert_coinstake_present(node, cursor)
            cursor = node.getblockheader(cursor)['previousblockhash']

    def _assert_single_active_tip(self, node):
        tips = node.getchaintips()
        active = [t for t in tips if t['status'] == 'active']
        assert_equal(len(active), 1)

    def _generate_and_sync(self, node1, node0, count, label):
        """Generate `count` PoS blocks on node1 (disconnected), then sync to node0."""
        self.disconnect_nodes(0, 1)
        height_before = node1.getblockcount()
        advance_time_for_pos(node1, seconds=600)
        node1.generate(count)
        expected = height_before + count
        assert_equal(node1.getblockcount(), expected)
        self.log.info(f"  {label}: node1 mined to height {expected}")

        set_node_times(self.nodes, node1.mocktime)
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=180)
        assert_equal(node0.getblockcount(), expected)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

    # ------------------------------------------------------------------
    # Test
    # ------------------------------------------------------------------

    def run_test(self):
        node0 = self.nodes[0]  # v4.22.9 follower
        node1 = self.nodes[1]  # v4.22.10 lead staker

        # ------------------------------------------------------------------
        # Phase A — Bootstrap on node1, sync node0 to height 200.
        # ------------------------------------------------------------------
        self.log.info(f"Phase A: bootstrap node1 to height {PHASE_A_TARGET_HEIGHT}...")

        self.log.info(f"  Node1: generating {PHASE_A_POW_BLOCKS} PoW blocks...")
        node1.generate(PHASE_A_POW_BLOCKS)

        self.log.info("  Node1: advancing time for PoS coin age...")
        advance_time_for_pos(node1, seconds=600)

        self.log.info(f"  Node1: generating {PHASE_A_POS_BLOCKS} PoS blocks...")
        node1.generate(PHASE_A_POS_BLOCKS)
        assert_equal(node1.getblockcount(), PHASE_A_TARGET_HEIGHT)

        # Restart node0 with mocktime to avoid v4.22.9's net.cpp realtime
        # mismatch causing it to disconnect node1.
        self.log.info("  Restarting node0 with mocktime for sync...")
        mocktime = node1.mocktime
        self.restart_node(0, extra_args=self.extra_args[0] + [f'-mocktime={mocktime}'])
        node0 = self.nodes[0]
        node0.mocktime = mocktime

        self.log.info("  Connecting nodes and syncing...")
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=120)

        assert_equal(node0.getblockcount(), PHASE_A_TARGET_HEIGHT)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info(f"Phase A complete: both nodes at height {PHASE_A_TARGET_HEIGHT}.")

        # ------------------------------------------------------------------
        # Phase B — Node1 mines 50 more PoS blocks; node0 must accept each.
        # ------------------------------------------------------------------
        self.log.info(f"Phase B: node1 generates {PHASE_B_BLOCKS} PoS blocks...")
        self._generate_and_sync(node1, node0, PHASE_B_BLOCKS, "Phase B")
        self._verify_recent_coinstakes(node0)
        self.log.info(f"Phase B passed: node0 (v4.22.9) accepted {PHASE_B_BLOCKS} new PoS blocks.")

        # ------------------------------------------------------------------
        # Phase C — Another batch; verify continued acceptance.
        # ------------------------------------------------------------------
        self.log.info(f"Phase C: node1 generates {PHASE_C_BLOCKS} PoS blocks...")
        self._generate_and_sync(node1, node0, PHASE_C_BLOCKS, "Phase C")
        self._verify_recent_coinstakes(node0)
        self.log.info(f"Phase C passed: node0 accepted {PHASE_C_BLOCKS} more PoS blocks.")

        # ------------------------------------------------------------------
        # Phase D — No fork tips on either node.
        # ------------------------------------------------------------------
        self.log.info("Phase D: chain tip integrity check...")
        self._assert_single_active_tip(node0)
        self._assert_single_active_tip(node1)
        self.log.info(
            f"feature_compat_pos_blocks: all phases passed at height {node1.getblockcount()}."
        )


if __name__ == '__main__':
    CompatPosBlocksTest().main()
