#!/usr/bin/env python3
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test staking interoperability across v4.22.9 and v4.22.10 nodes with
descriptor and legacy wallets.

Three-node setup:
  node0 — v4.22.9   (old binary, legacy wallet, sync-only follower)
  node1 — v4.22.10  (current binary, legacy wallet, lead staker)
  node2 — v4.22.10  (current binary, descriptor wallet, secondary staker)

v4.22.9 cannot reliably generate PoS blocks on regtest, so it stays in a
follower role.  Both v4.22.10 wallet types (legacy and descriptor) take
turns producing blocks; node0 must accept every one.

  Phase A — Bootstrap on node1 to height 250, distribute UTXOs to all.
  Phase B — Node1 (legacy wallet) stakes 30 PoS blocks; all sync.
  Phase C — Node2 (descriptor wallet) stakes 30 PoS blocks; node0 must
            accept descriptor-wallet-staked blocks.
  Phase D — Alternating stakers (node1, node2) for 5 rounds of 5 blocks
            each; node0 follows.
  Phase E — Single active tip on every node.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
    set_node_times,
)


PHASE_A_POW_BLOCKS = 89
PHASE_A_INITIAL_POS = 11      # heights 90-100
PHASE_A_MATURITY_POS = 150    # heights 101-250
PHASE_A_TARGET_HEIGHT = (
    PHASE_A_POW_BLOCKS + PHASE_A_INITIAL_POS + PHASE_A_MATURITY_POS  # 250
)

NODE0_FUNDING_UTXOS = 20
NODE0_FUNDING_AMOUNT = 500    # RDD per UTXO

NODE2_FUNDING_UTXOS = 40
NODE2_FUNDING_AMOUNT = 5000   # larger UTXOs improve descriptor-wallet kernel hits

PHASE_B_BLOCKS = 30
PHASE_C_BLOCKS = 30

PHASE_D_ROUNDS = 5
PHASE_D_BLOCKS_PER_ROUND = 5


class CompatStakingInteropTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-whitelist=127.0.0.1", "-staking=0", "-keypool=100"],   # node0: v4.22.9
            ["-whitelist=127.0.0.1", "-staking=0", "-keypool=100"],   # node1: v4.22.10 legacy
            ["-whitelist=127.0.0.1", "-staking=0", "-keypool=100"],   # node2: v4.22.10 descriptor
        ]
        # Suppress default wallet creation on node2; created manually as descriptor.
        self.wallet_names = [self.default_wallet_name, self.default_wallet_name]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()
        self.skip_if_no_previous_releases()

    def setup_network(self):
        self.add_nodes(
            self.num_nodes,
            extra_args=self.extra_args,
            versions=[4220900, None, None],
        )
        self.start_nodes()
        self.init_wallet(0)
        self.init_wallet(1)
        # node2's descriptor wallet is created in Phase A.

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _sync_time_and_connect_all(self, leader):
        set_node_times(self.nodes, leader.mocktime)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.connect_nodes(0, 2)

    def _disconnect_all(self):
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        self.disconnect_nodes(0, 2)

    def generate_with_desc_wallet(self, node, nblocks):
        """Generate PoS blocks on a descriptor wallet with retry."""
        addr = node.get_wallet_rpc("desc_wallet").getnewaddress()
        blocks = []
        for _ in range(nblocks):
            advance_time_for_pos(node, seconds=600)
            for attempt in range(10):
                try:
                    bh = node.get_wallet_rpc("desc_wallet").generatetoaddress(1, addr)
                    blocks.extend(bh)
                    break
                except Exception as e:
                    if "no valid coinstake found" in str(e) and attempt < 9:
                        advance_time_for_pos(node, seconds=300)
                    else:
                        raise
        return blocks

    def _assert_single_active_tip(self, node):
        tips = node.getchaintips()
        active = [t for t in tips if t["status"] == "active"]
        assert_equal(len(active), 1)

    # ------------------------------------------------------------------
    # Phase A — Bootstrap, distribute UTXOs, sync
    # ------------------------------------------------------------------

    def _phase_a(self, desc_wallet):
        self.log.info(
            f"Phase A: bootstrap node1 to height {PHASE_A_TARGET_HEIGHT}, "
            "distribute UTXOs"
        )
        node0 = self.nodes[0]
        node1 = self.nodes[1]
        node2 = self.nodes[2]

        # Generate enough PoW first so wallet has spendable coinbases.
        # Distribute funds before transitioning to PoS.
        self.log.info(f"  Node1: generating 70 PoW blocks (pre-funding)...")
        node1.generate(70)

        self.log.info("  Node2: creating descriptor wallet...")
        node2.createwallet(wallet_name="desc_wallet", descriptors=True)
        wallet_info = desc_wallet.getwalletinfo()
        assert_equal(wallet_info["format"], "sqlite")

        self.log.info(
            f"  Node1: sending {NODE0_FUNDING_UTXOS} x {NODE0_FUNDING_AMOUNT} RDD to node0..."
        )
        node0_addr = node0.getnewaddress()
        for _ in range(NODE0_FUNDING_UTXOS):
            node1.sendtoaddress(node0_addr, NODE0_FUNDING_AMOUNT)

        self.log.info(
            f"  Node1: sending {NODE2_FUNDING_UTXOS} x {NODE2_FUNDING_AMOUNT} RDD to node2 descriptor..."
        )
        for _ in range(NODE2_FUNDING_UTXOS):
            node1.sendtoaddress(desc_wallet.getnewaddress(), NODE2_FUNDING_AMOUNT)

        # Finish PoW (height 89), then the initial PoS (height 100), then
        # enough additional PoS to mature the funding (height 250).
        remaining_pow = PHASE_A_POW_BLOCKS - 70
        self.log.info(f"  Node1: generating {remaining_pow} more PoW blocks (to height {PHASE_A_POW_BLOCKS})...")
        node1.generate(remaining_pow)
        assert_equal(node1.getblockcount(), PHASE_A_POW_BLOCKS)

        self.log.info("  Node1: advancing time for PoS coin age...")
        advance_time_for_pos(node1, seconds=600)
        self.log.info(
            f"  Node1: generating {PHASE_A_INITIAL_POS + PHASE_A_MATURITY_POS} PoS blocks..."
        )
        node1.generate(PHASE_A_INITIAL_POS + PHASE_A_MATURITY_POS)
        assert_equal(node1.getblockcount(), PHASE_A_TARGET_HEIGHT)

        self.log.info("  Syncing mocktime and connecting all nodes...")
        self._sync_time_and_connect_all(node1)
        self.sync_blocks(timeout=180)

        for n in (node0, node1, node2):
            assert_equal(n.getblockcount(), PHASE_A_TARGET_HEIGHT)
            assert_equal(n.getbestblockhash(), node1.getbestblockhash())

        bal0 = node0.getbalance()
        bal1 = node1.getbalance()
        bal2 = desc_wallet.getbalance()
        self.log.info(
            f"  Balances — node0: {bal0} RDD  node1: {bal1} RDD  node2(desc): {bal2} RDD"
        )
        self.log.info(f"Phase A complete at height {PHASE_A_TARGET_HEIGHT}.")

    # ------------------------------------------------------------------
    # Phase B — node1 (legacy) stakes
    # ------------------------------------------------------------------

    def _phase_b(self):
        self.log.info(f"Phase B: node1 (v4.22.10, legacy) stakes {PHASE_B_BLOCKS} PoS blocks...")
        node1 = self.nodes[1]

        self._disconnect_all()
        height_before = node1.getblockcount()
        advance_time_for_pos(node1, seconds=600)
        node1.generate(PHASE_B_BLOCKS)
        expected = height_before + PHASE_B_BLOCKS
        assert_equal(node1.getblockcount(), expected)

        self._sync_time_and_connect_all(node1)
        self.sync_blocks(timeout=180)
        for n in self.nodes:
            assert_equal(n.getblockcount(), expected)
            assert_equal(n.getbestblockhash(), node1.getbestblockhash())
        self.log.info(f"Phase B complete: all nodes at height {expected}.")

    # ------------------------------------------------------------------
    # Phase C — node2 (descriptor) stakes; v4.22.9 must accept
    # ------------------------------------------------------------------

    def _phase_c(self, desc_wallet):
        self.log.info(
            f"Phase C: node2 (v4.22.10, descriptor) stakes {PHASE_C_BLOCKS} PoS blocks..."
        )
        node2 = self.nodes[2]

        self._disconnect_all()
        height_before = node2.getblockcount()
        blocks = self.generate_with_desc_wallet(node2, nblocks=PHASE_C_BLOCKS)
        assert_equal(len(blocks), PHASE_C_BLOCKS)
        expected = height_before + PHASE_C_BLOCKS
        assert_equal(node2.getblockcount(), expected)

        self._sync_time_and_connect_all(node2)
        self.sync_blocks(timeout=180)
        for n in self.nodes:
            assert_equal(n.getblockcount(), expected)
            assert_equal(n.getbestblockhash(), node2.getbestblockhash())
        self.log.info(
            f"Phase C complete: node0 (v4.22.9) accepted {PHASE_C_BLOCKS} "
            f"descriptor-wallet PoS blocks.  All nodes at height {expected}."
        )

    # ------------------------------------------------------------------
    # Phase D — alternating stakers (node1, node2 only — v4.22.9 follows)
    # ------------------------------------------------------------------

    def _phase_d(self, desc_wallet):
        self.log.info(
            f"Phase D: alternating stakers — {PHASE_D_ROUNDS} rounds of "
            f"{PHASE_D_BLOCKS_PER_ROUND} blocks each (node1 ↔ node2)..."
        )
        node0 = self.nodes[0]
        node1 = self.nodes[1]
        node2 = self.nodes[2]

        for round_num in range(1, PHASE_D_ROUNDS + 1):
            if round_num % 2 == 1:
                staker = node1
                label = "node1 (legacy)"
                self._disconnect_all()
                advance_time_for_pos(staker, seconds=600)
                staker.generate(PHASE_D_BLOCKS_PER_ROUND)
            else:
                staker = node2
                label = "node2 (descriptor)"
                self._disconnect_all()
                blocks = self.generate_with_desc_wallet(node2, nblocks=PHASE_D_BLOCKS_PER_ROUND)
                assert_equal(len(blocks), PHASE_D_BLOCKS_PER_ROUND)

            self._sync_time_and_connect_all(staker)
            self.sync_blocks(timeout=180)
            expected_hash = staker.getbestblockhash()
            for n in self.nodes:
                assert_equal(n.getbestblockhash(), expected_hash)
            self.log.info(
                f"  Round {round_num} ({label}): height={staker.getblockcount()}"
            )

        # Final tip integrity
        self.log.info("  Verifying single active tip on all nodes...")
        self._assert_single_active_tip(node0)
        self._assert_single_active_tip(node1)
        self._assert_single_active_tip(node2)
        self.log.info(
            f"Phase D complete: continuous chain at height {node1.getblockcount()}, no forks."
        )

    # ------------------------------------------------------------------
    # Entry point
    # ------------------------------------------------------------------

    def run_test(self):
        self.log.info(
            "CompatStakingInteropTest: validating staking across "
            "v4.22.9 (node0, follower), v4.22.10 legacy (node1, lead), "
            "v4.22.10 descriptor (node2, lead)"
        )

        node2 = self.nodes[2]

        class _DescWalletProxy:
            """Lazily forwards attribute access to the real wallet RPC."""
            def __init__(self, node):
                self._node = node
                self._rpc = None

            def _ensure(self):
                if self._rpc is None:
                    self._rpc = self._node.get_wallet_rpc("desc_wallet")

            def __getattr__(self, name):
                self._ensure()
                return getattr(self._rpc, name)

        desc_wallet = _DescWalletProxy(node2)

        self._phase_a(desc_wallet)
        self._phase_b()
        self._phase_c(desc_wallet)
        self._phase_d(desc_wallet)

        self.log.info("=" * 60)
        self.log.info("CompatStakingInteropTest: all phases passed.")
        self.log.info("=" * 60)


if __name__ == "__main__":
    CompatStakingInteropTest().main()
