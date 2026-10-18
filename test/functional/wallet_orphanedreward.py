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
        self.num_nodes = 3

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        # Every node needs cache staking keys for block generation
        for i in range(self.num_nodes):
            self.init_wallet(i)

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
        #
        # Node 2 stakes these blocks, so that none of node 0's coins is spent
        # in them: node 0 stakes the longer chain below from the fork point,
        # and node 1 cannot check a kernel on a coin its own chain has spent.
        self.nodes[2].generate(61)
        self.sync_all()
        post_mature_balance = self.nodes[1].getbalance()
        block_reward = post_mature_balance - pre_stake_balance
        self.log.info(f"pre_stake={pre_stake_balance} post_mature={post_mature_balance} reward={block_reward}")
        assert block_reward > 0, "Node 1 should have earned a staking reward"

        # Send more than the pre-staking balance, proving the block reward is counted.
        txid = self.nodes[1].send(outputs=[{self.nodes[0].getnewaddress(): pre_stake_balance + 1}])["txid"]

        # Orphan the block reward and make sure that the original coins
        # from the wallet can still be spent.
        #
        # Node 1 leaves blk through an ordinary reorg onto a longer chain, the
        # way a node does on the network. That disconnects every block back to
        # the fork before the mempool is updated once, so the spend above is
        # still in the mempool when the coinstake's block goes, and leaves only
        # after that disconnect has reached the wallet. The wallet must not
        # abandon the coinstake, and this spend with it, until it does.
        #
        # Node 0 builds the longer chain while disconnected, and node 1 is given
        # it by submitblock rather than P2P: node 0 has invalidated blk, and
        # would punish node 1 for headers building on it.
        # Node 2 stays on the old chain, out of the way.
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        self.nodes[0].invalidateblock(blk)
        # Node 1 checks the fork's kernels while the fork is still a side chain,
        # against coins it already knows, so node 0 must not stake a coin the
        # fork itself created. Lock each new coinstake's outputs; the staker
        # leaves locked coins alone.
        fork = []
        for _ in range(63):
            fork += self.nodes[0].generate(1)
            stake_txid = self.nodes[0].getblock(fork[-1])["tx"][1]
            coinstake = self.nodes[0].decoderawtransaction(self.nodes[0].gettransaction(stake_txid)["hex"])
            self.nodes[0].lockunspent(False, [{"txid": stake_txid, "vout": out["n"]}
                                              for out in coinstake["vout"] if out["value"] > 0])
        self.nodes[1].setmocktime(self.nodes[0].mocktime)
        for block_hash in fork:
            self.nodes[1].submitblock(self.nodes[0].getblock(block_hash, 0))
        assert_equal(self.nodes[1].getbestblockhash(), fork[-1])
        self.connect_nodes(0, 1)
        self.sync_all(self.nodes[0:2])

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
