#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a coinstake orphaned by a reorg that happened while the wallet was unloaded.

The wallet only learns that a block left the chain through blockDisconnected, so
a reorg that happens while it is not loaded never reaches it that way, and the
rescan at load only moves forward from the fork point. A coinstake is bound to
its block and can never be re-mined, so the rescan cannot rescue it either.

Two separate mechanisms have to cooperate to get the staked coin back, and
nothing else exercises the pair together:

  - CWallet::LoadToWallet resolves the recorded block as the wallet is read off
    disk. m_confirm.block_height is not serialized, so it has to be looked up,
    and a transaction whose block is not in the active chain is reset to
    UNCONFIRMED there rather than keeping a confirmation against a block in no
    chain. Without this the coinstake would report a positive depth, and both
    IsSpent and AbandonTransaction key off depth: the coin would stay held back
    with not even abandontransaction able to release it.

  - CWallet::AbandonOrphanedCoinstakes, run once from postInitProcess, then
    abandons the now depth zero coinstake, which is what actually returns the
    staked input to the wallet.

The node the reorg is replayed onto ends up longer than the branch it replaces,
so the wallet cannot fall back on depth arithmetic to notice: on an equal or
longer chain the recorded height is still a real height, and only a chain
membership check distinguishes it.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class WalletReorgStaleCoinstakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def import_deterministic_coinbase_privkeys(self):
        # Both nodes stake here, so both need their cache staking keys.
        self.init_wallet(0)
        self.init_wallet(1)

    def run_test(self):
        node0, node1 = self.nodes

        self.log.info("Split the network so the nodes build competing chains")
        self.disconnect_nodes(0, 1)

        # node0 stakes one block. That coinstake, and the coin it spends, are
        # what has to come back once the block is reorged away.
        [orphan_hash] = self.generate(1, node0)
        height = node0.getblockcount()
        coinstake_txid = node0.getblock(orphan_hash)["tx"][1]  # tx[1] is the coinstake
        staked_in = node0.getrawtransaction(coinstake_txid, True)["vin"][0]
        staked_outpoint = (staked_in["txid"], staked_in["vout"])
        self.log.info("node0 staked %s at height %d, consuming %s:%d"
                      % (coinstake_txid, height, staked_outpoint[0], staked_outpoint[1]))
        assert_equal(node0.gettransaction(coinstake_txid)["confirmations"], 1)

        # node1 builds a longer chain, so a different block occupies `height`.
        self.generate(3, node1)
        assert node1.getblockcount() > height
        assert node1.getblockhash(height) != orphan_hash

        self.log.info("Unload node0's wallet, then let node0 reorg without one")
        node0.unloadwallet(self.default_wallet_name)

        # With no wallet attached, the disconnect of the staked block is never
        # delivered to it. This is the case blockDisconnected cannot cover.
        node0.setmocktime(node1.mocktime)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node0.getblockhash(height), node1.getblockhash(height))

        self.log.info("Load the wallet again and check it does not still claim the confirmation")
        node0.loadwallet(self.default_wallet_name)
        wallet = node0.get_wallet_rpc(self.default_wallet_name)

        # LoadToWallet drops confirmations recorded against blocks no longer in
        # the chain. Without that the wallet reports this coinstake confirmed,
        # against a block the node no longer has on its chain.
        assert wallet.gettransaction(coinstake_txid)["confirmations"] <= 0, \
            "coinstake still claims a confirmation from the abandoned branch"

        # Once the confirmation is gone the coinstake is a depth zero orphan, so
        # the sweep postInitProcess runs at load abandons it.
        details = [d for d in wallet.gettransaction(coinstake_txid)["details"] if "abandoned" in d]
        assert details, "gettransaction reported no spend details to check"
        assert all(d["abandoned"] for d in details), \
            "orphaned coinstake was not abandoned after its confirmation was dropped"

        # Which is the point of the whole exercise: the staked coin is spendable
        # again. node1 holds a different deterministic key, so nothing else can
        # have spent it on the chain node0 just adopted.
        unspent = {(u["txid"], u["vout"]) for u in wallet.listunspent(0)}
        assert staked_outpoint in unspent, "staked input was not returned to the wallet"


if __name__ == '__main__':
    WalletReorgStaleCoinstakeTest().main()
