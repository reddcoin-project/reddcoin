#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a PoS coinstake collects the block's transaction fees.

Reddcoin's ConnectBlock computes the allowed stake reward as
GetProofOfStakeReward(nCoinAge, nFees, ...) — subsidy + block fees. The block
assembler builds the coinstake before the mempool fees are known, so
FinalizeCoinStakeReward (src/pos/stake.cpp), called after addPackageTxs, folds
the fees back into the coinstake so the staker collects them (split 92/8 with
the dev fund) and the money supply is conserved.

The per-block subsidy drifts as coins age/combine, so each fee block is paired
with a fresh fee-free *reference* block staked immediately before it; the fee is
sized off that reference (~3x the current subsidy). Each fee block then asserts:
  a) money supply grew by ~the subsidy — NOT by subsidy - fee (which would be
     negative here since fee > subsidy): fees were not destroyed,
  b) the exact accounting identity coinstake_mint == supply_delta + fees,
     computed directly from the transaction (independent of the moneysupply RPC),
  c) the dev fund received ~8% of the fees (dev output rose by ~0.08 * fee).
Single- and multi-transaction fee blocks exercise nFees aggregation.

With --previous_release a v4.22.9 node joins as a sync-only follower and must
accept the current build's fee-collecting coinstakes; because Fix 2 changes the
dev output, and v4.22.9's dev-amount check only runs past DonationHeight
(regtest 500), that mode bootstraps past height 502 first.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
    set_node_times,
)

POW_BLOCKS = 89
COMPAT_TARGET_HEIGHT = 504
COMPAT_POS_BLOCKS = COMPAT_TARGET_HEIGHT - POW_BLOCKS
COMPAT_FEE_BLOCKS = 4

DEV_SHARE = Decimal("0.08")      # dev fund gets 8% of the reward (staker 92%)
SATOSHI = Decimal("0.00000001")


class PosCoinstakeFeesTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument(
            "--previous_release", dest="previous_release", default=False,
            action="store_true",
            help="also verify a v4.22.9 follower accepts the fee-collecting coinstakes")

    def set_test_params(self):
        self.compat = self.options.previous_release
        if self.compat:
            self.num_nodes = 2
            self.setup_clean_chain = True
            self.extra_args = [
                ["-whitelist=127.0.0.1", "-staking=0", "-printpriority"],
                ["-whitelist=127.0.0.1", "-staking=0", "-printpriority"],
            ]
        else:
            self.num_nodes = 1
            self.setup_clean_chain = False  # use the 199-block PoS cache

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if self.options.previous_release:
            self.skip_if_no_previous_releases()

    def setup_network(self):
        if self.compat:
            self.add_nodes(self.num_nodes, extra_args=self.extra_args, versions=[
                4220903,  # node0 - v4.22.9.3 follower
                None,     # node1 — current build, lead staker
            ])
            self.start_nodes()
            self.import_deterministic_coinbase_privkeys()
        else:
            super().setup_network()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _money_supply(self, node):
        return node.getblockchaininfo()["moneysupply"]

    def _stake_one(self, node):
        advance_time_for_pos(node, seconds=60)
        return node.generate(1)[0]

    def _coinstake(self, node, blockhash):
        block = node.getblock(blockhash, 2)
        assert len(block["tx"]) >= 2, "PoS block must have a coinstake at tx[1]"
        return block["tx"][1]

    def _dev_output(self, node, blockhash):
        """Dev-fund output value of a coinstake (always the last output)."""
        return self._coinstake(node, blockhash)["vout"][-1]["value"]

    def _coinstake_mint(self, node, blockhash):
        """Coinstake reward computed directly from the tx: Sum(out) - Sum(in).
        Input values are fetched via getrawtransaction (txindex is always on)."""
        cs = self._coinstake(node, blockhash)
        out_total = sum(o["value"] for o in cs["vout"])
        in_total = Decimal(0)
        for vin in cs["vin"]:
            prev = node.getrawtransaction(vin["txid"], True)
            in_total += prev["vout"][vin["vout"]]["value"]
        return out_total - in_total

    def _fee_free_reference(self, node):
        """Stake one fee-free block; return (subsidy_delta, dev_output).

        This is the adaptive per-block subsidy estimate: the coinstake reward of
        an empty block is exactly the subsidy, and the block right before the fee
        block is the closest available reference."""
        assert_equal(node.getrawmempool(), [])
        before = self._money_supply(node)
        h = self._stake_one(node)
        return self._money_supply(node) - before, self._dev_output(node, h)

    def _build_fee_tx(self, node, fee):
        """Broadcast a wallet tx paying exactly `fee`, combining as many
        confirmed UTXOs as needed. Spent inputs are mempool-spent and thus
        excluded from the coinstake's coin selection, so there is no conflict."""
        utxos = sorted(node.listunspent(1), key=lambda u: u["amount"], reverse=True)
        chosen, total = [], Decimal(0)
        for u in utxos:
            chosen.append(u)
            total += u["amount"]
            if total > fee + Decimal("1"):
                break
        assert total > fee + Decimal("1"), "insufficient confirmed funds for the fee test"
        inputs = [{"txid": u["txid"], "vout": u["vout"]} for u in chosen]
        change = (total - fee).quantize(SATOSHI)
        raw = node.createrawtransaction(inputs, {node.getnewaddress(): change})
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"], "failed to sign fee transaction"
        # maxfeerate=0 disables the node's high-fee guard: this fee is
        # intentionally larger than the per-block subsidy.
        txid = node.sendrawtransaction(signed["hex"], 0)
        assert txid in node.getrawmempool()
        return txid

    def _assert_fee_block(self, node, n_txs, label):
        """Stake a fee-free reference block, then a fee block with `n_txs` fee
        txs summing to ~3x the current subsidy, and assert fees are collected."""
        ref_delta, ref_dev = self._fee_free_reference(node)
        assert ref_delta > 0, "subsidy reference must be positive"

        total_fee = (ref_delta * 3 + Decimal("1")).quantize(SATOSHI)
        per = (total_fee / n_txs).quantize(SATOSHI)
        fees = [per] * n_txs
        total_fee = per * n_txs  # exact after quantization

        feetxids = [self._build_fee_tx(node, f) for f in fees]
        before = self._money_supply(node)
        blockhash = self._stake_one(node)
        block = node.getblock(blockhash, 1)
        for txid in feetxids:
            assert txid in block["tx"], "fee tx must be mined into the PoS block"
        assert len(block["tx"]) >= 2 + n_txs, "expected coinbase + coinstake + fee txs"

        delta = self._money_supply(node) - before
        dev = self._dev_output(node, blockhash)
        mint = self._coinstake_mint(node, blockhash)
        dev_bump = dev - ref_dev
        self.log.info(f"[{label}] n_txs={n_txs} fee={total_fee} ref_subsidy={ref_delta} "
                      f"supply_delta={delta} dev_bump={dev_bump} mint={mint}")

        # a) Supply GREW even though the fee exceeds the subsidy. Without the fix
        #    the coinstake ignores the fee, so delta = subsidy - fee < 0 (supply
        #    would shrink on a minting block). This is the spike-proof sign-flip.
        assert total_fee > ref_delta, "fee must exceed the subsidy for the sign-flip check"
        assert delta > 0, (
            f"[{label}] supply shrank on a minting block (delta={delta}); the fee "
            f"of {total_fee} was destroyed instead of collected into the coinstake")
        # b) Exact accounting identity: coinstake mint == supply_delta + fees.
        assert abs(mint - (delta + total_fee)) <= Decimal("0.001"), (
            f"[{label}] coinstake mint {mint} != supply_delta+fee {delta + total_fee}")
        # c) Dev fund received ~8% of the fees (bracket absorbs subsidy drift
        #    between the reference and fee block).
        expected = total_fee * DEV_SHARE
        assert expected / 2 <= dev_bump <= expected * Decimal("1.5"), (
            f"[{label}] dev fund share of fees {dev_bump} outside "
            f"[{expected / 2}, {expected * Decimal('1.5')}] (fee={total_fee})")

    # ------------------------------------------------------------------
    # Test bodies
    # ------------------------------------------------------------------

    def run_test(self):
        if self.compat:
            self._run_compat()
        else:
            self._run_standalone()

    def _run_standalone(self):
        node = self.nodes[0]
        assert node.getblockcount() >= 199
        advance_time_for_pos(node, seconds=600)
        for _ in range(2):  # warm up staking
            self._stake_one(node)

        self._assert_fee_block(node, 1, "single-fee")
        self._assert_fee_block(node, 5, "multi-fee")
        self._assert_fee_block(node, 1, "consecutive-0")
        self._assert_fee_block(node, 1, "consecutive-1")
        self.log.info("Coinstake fee reconciliation verified (all stress points)")

    def _run_compat(self):
        node0 = self.nodes[0]  # v4.22.9 follower
        node1 = self.nodes[1]  # current build, lead staker

        self.log.info(f"Phase A: bootstrap node1 past DonationHeight to {COMPAT_TARGET_HEIGHT}")
        node1.generate(POW_BLOCKS)
        advance_time_for_pos(node1, seconds=600)
        node1.generate(COMPAT_POS_BLOCKS)
        assert_equal(node1.getblockcount(), COMPAT_TARGET_HEIGHT)

        mocktime = node1.mocktime
        self.restart_node(0, extra_args=self.extra_args[0] + [f'-mocktime={mocktime}'])
        node0 = self.nodes[0]
        node0.mocktime = mocktime
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=180)
        assert_equal(node0.getblockcount(), COMPAT_TARGET_HEIGHT)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info("Phase A complete: v4.22.9 synced past DonationHeight")

        self.disconnect_nodes(0, 1)
        self.log.info(f"Phase B: node1 mines {COMPAT_FEE_BLOCKS} fee-paying PoS blocks")
        for i in range(COMPAT_FEE_BLOCKS):
            # Alternate single- and multi-tx fee blocks to vary nFees aggregation.
            self._assert_fee_block(node1, 1 if i % 2 == 0 else 3, f"compat-{i}")
        expected_height = node1.getblockcount()

        # v4.22.9 must accept every fee-collecting block, including the changed
        # dev output validated past DonationHeight.
        set_node_times(self.nodes, node1.mocktime)
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=180)
        assert_equal(node0.getblockcount(), expected_height)
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        active = [t for t in node0.getchaintips() if t['status'] == 'active']
        assert_equal(len(active), 1)
        self.log.info(
            "v4.22.9 accepted the current build's fee-collecting coinstakes "
            f"past DonationHeight (height {expected_height})")


if __name__ == '__main__':
    PosCoinstakeFeesTest().main()
