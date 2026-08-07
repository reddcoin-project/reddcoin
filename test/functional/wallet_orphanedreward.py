#!/usr/bin/env python3
# Copyright (c) 2020-2021 The Bitcoin Core developers
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test orphaned block rewards in the wallet."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def is_abandoned(node, txid):
    """Whether every spend recorded for txid is marked abandoned.

    gettransaction reports "abandoned" on the "send" side of a transaction, which
    for a coinstake is the coin it staked.
    """
    details = [d for d in node.gettransaction(txid)["details"] if "abandoned" in d]
    assert details, f"gettransaction {txid} reported no spend details to check"
    return all(d["abandoned"] for d in details)


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

        # A coinstake orphaned by a disconnect is abandoned by the wallet as the
        # disconnect arrives, in blockDisconnected(). Until it is, IsSpent still
        # counts it as spending its input, holding that coin out of the wallet.
        #
        # This used to be done by a full mapWallet sweep run before every block
        # template, so it only took effect once this node next tried to stake,
        # and the test had to call abandontransaction by hand to get there.
        assert is_abandoned(self.nodes[1], coinstake_txid), \
            "orphaned coinstake should have been abandoned by the disconnect"

        # AbandonTransaction also abandons the in-wallet descendants of what it
        # abandons, so the spend of the coinstake output goes with it. No manual
        # abandontransaction call is needed for either; see
        # https://github.com/bitcoin/bitcoin/issues/14148 for why one used to be.
        assert is_abandoned(self.nodes[1], txid), \
            "spend of the orphaned coinstake should have been abandoned with it"

        balances_after = self.nodes[1].getbalances()["mine"]
        # With the coinstake and its spend abandoned, the staked input is back
        # in the wallet and the pre-staking balance is recovered.
        assert_equal(balances_after["trusted"], pre_stake_balance)
        assert_equal(balances_after["untrusted_pending"], 0)
        assert_equal(balances_after["immature"], 0)

        self.nodes[1].sendtoaddress(self.nodes[0].getnewaddress(), FUNDING - 1000)

if __name__ == '__main__':
    OrphanedBlockRewardTest().main()
