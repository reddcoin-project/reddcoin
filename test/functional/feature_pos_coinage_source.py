#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test coin-age source symmetry between coinstake creation and validation.

CreateCoinStake's stake-split and input-combine gates (src/pos/stake.cpp) read
the prev-coin timestamp from the same UTXO Coin source (via GetCoinAgeTimes)
that GetCoinAge / ConnectBlock use, rather than a separate disk read. This test
exercises the combine gate — which only folds in inputs older than nStakeMaxAge
(45 days) — by advancing mocktime past that age.

Stress points exercised:
  * every folded-in coinstake input is genuinely matured
    (prev.nTime + nStakeMaxAge <= coinstake.nTime), catching the historical bug
    where a young coin was combined because the timestamp source disagreed with
    the validator's,
  * a second node validates every staked block (a rejection would mean the
    staker's coin-age gates disagreed with the consensus coin-age source),
  * the staker is restarted mid-run, proving coin age is read correctly from the
    reloaded UTXO set (not cached / disk-order dependent).
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
    set_node_times,
)

STAKE_MAX_AGE = 45 * 24 * 60 * 60  # regtest consensus.nStakeMaxAge


class PosCoinageSourceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = False  # use the 199-block PoS cache
        self.extra_args = [["-staking=0"], ["-staking=0"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        self.init_wallet(0)
        self.init_wallet(1)

    def setup_network(self):
        # Start disconnected: node0 stakes in isolation, node1 validates after.
        self.setup_nodes()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _inspect_coinstake(self, node, blockhash):
        """Return (n_inputs, n_outputs, combined_ok) for a block's coinstake.

        combined_ok asserts that every folded-in input (vin[1:], i.e. every
        input other than the kernel vin[0]) is older than nStakeMaxAge relative
        to the coinstake timestamp — the exact decision the combine gate makes
        from the UTXO Coin.nTime.
        """
        block = node.getblock(blockhash, 2)
        cs = block["tx"][1]
        cs_ntime = cs.get("time", block["time"])  # coinstake nTime == block time
        for vin in cs["vin"][1:]:
            prev = node.getrawtransaction(vin["txid"], True)
            prev_ntime = prev.get("time", 0)  # 0 for legacy PoW-era coins
            assert prev_ntime + STAKE_MAX_AGE <= cs_ntime, (
                f"combine gate folded a too-young input: prev.nTime={prev_ntime} "
                f"+ {STAKE_MAX_AGE} > coinstake.nTime={cs_ntime}")
        return len(cs["vin"]), len(cs["vout"])

    def run_test(self):
        staker, validator = self.nodes[0], self.nodes[1]
        start_height = staker.getblockcount()
        assert start_height >= 199

        # Age every coin past nStakeMaxAge so the combine gate folds matured
        # same-address inputs into the coinstake.
        aged_time = max(staker.mocktime, validator.mocktime) + STAKE_MAX_AGE + 24 * 60 * 60
        set_node_times(self.nodes, aged_time)

        n_blocks = 16
        restart_at = n_blocks // 2
        combined = split = 0
        self.log.info(f"Staking {n_blocks} PoS blocks on node0 with aged coins "
                      f"(restarting the staker at block {restart_at})")
        for i in range(n_blocks):
            advance_time_for_pos(staker, seconds=60)
            blockhash = staker.generate(1)[0]
            n_in, n_out = self._inspect_coinstake(staker, blockhash)
            if n_in > 1:
                combined += 1
            if n_out >= 4:
                split += 1

            if i == restart_at:
                # Restart the staker; coin age must be read from the reloaded
                # UTXO set (mocktime is an RPC state, restore it after restart).
                saved = staker.mocktime
                self.log.info("Restarting staker mid-run to test UTXO-source persistence")
                self.restart_node(0)
                staker = self.nodes[0]
                set_node_times([staker], saved)

        assert_equal(staker.getblockcount(), start_height + n_blocks)
        self.log.info(f"combine-exercised blocks={combined} split-exercised blocks={split}")
        assert combined > 0, (
            "combine gate (stake.cpp:399) was not exercised; the aged cache did "
            "not yield multi-input coinstakes")

        # Cross-node validation: node1 must accept every block node0 staked.
        set_node_times(self.nodes, staker.mocktime)
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=120)
        assert_equal(validator.getblockcount(), start_height + n_blocks)
        assert_equal(validator.getbestblockhash(), staker.getbestblockhash())
        # The validator independently re-checks the same maturity invariant.
        self._inspect_coinstake(validator, validator.getbestblockhash())

        self.log.info("Coin-age source symmetry verified across create and validate")


if __name__ == '__main__':
    PosCoinageSourceTest().main()
