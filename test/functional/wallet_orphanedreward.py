#!/usr/bin/env python3
# Copyright (c) 2020-2021 The Bitcoin Core developers
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test orphaned block rewards in the wallet."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

class OrphanedBlockRewardTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        # Both nodes need cache staking keys for block generation
        self.init_wallet(0)
        self.init_wallet(1)

    def run_test(self):
        # Node 1 has cache balance for staking.
        # Send additional FUNDING that we'll track through the orphan.
        FUNDING = 5000000
        self.nodes[0].sendtoaddress(self.nodes[1].getnewaddress(), FUNDING)
        self.nodes[0].generate(1)
        self.sync_all()
        pre_stake_balance = self.nodes[1].getbalance()

        # Get a block reward with node 1 and remember the block so we can orphan
        # it later.  Also remember the coinstake txid for later abandonment.
        blk = self.nodes[1].generate(1)[0]
        blk_data = self.nodes[1].getblock(blk)
        coinstake_txid = blk_data["tx"][1]  # tx[1] is coinstake in PoS blocks
        self.sync_all()

        # Let the block reward mature and send coins including both
        # the existing balance and the block reward.
        self.nodes[0].generate(61)
        self.sync_all()
        post_mature_balance = self.nodes[1].getbalance()
        block_reward = post_mature_balance - pre_stake_balance
        self.log.info(f"pre_stake={pre_stake_balance} post_mature={post_mature_balance} reward={block_reward}")
        assert block_reward > 0, "Node 1 should have earned a staking reward"

        # Send more than the pre-staking balance, proving the block reward is counted.
        txid = self.nodes[1].sendtoaddress(self.nodes[0].getnewaddress(), pre_stake_balance + 1)

        # Orphan the block reward and make sure that the original coins
        # from the wallet can still be spent.
        self.nodes[0].invalidateblock(blk)
        self.nodes[1].invalidateblock(blk)
        self.nodes[0].generate(63)
        self.nodes[1].setmocktime(self.nodes[0].mocktime)
        self.sync_all()

        # Without the following abandontransaction call, the coins are
        # not considered available yet.
        balances_before = self.nodes[1].getbalances()["mine"]
        assert_equal(balances_before["trusted"], 0)
        assert_equal(balances_before["untrusted_pending"], 0)
        assert_equal(balances_before["immature"], 0)

        # The following abandontransaction calls are necessary to make the
        # later lines succeed, and probably should not be needed; see
        # https://github.com/bitcoin/bitcoin/issues/14148.
        # In PoS, we must also abandon the coinstake from the orphaned block
        # to free the UTXO it consumed.
        self.nodes[1].abandontransaction(coinstake_txid)
        self.nodes[1].abandontransaction(txid)
        balances_after = self.nodes[1].getbalances()["mine"]
        # After orphaning the staked block and abandoning the send tx,
        # the wallet should recover the pre-staking balance.
        assert_equal(balances_after["trusted"], pre_stake_balance)
        assert_equal(balances_after["untrusted_pending"], 0)
        assert_equal(balances_after["immature"], 0)

        self.nodes[1].sendtoaddress(self.nodes[0].getnewaddress(), FUNDING - 1000)

if __name__ == '__main__':
    OrphanedBlockRewardTest().main()
