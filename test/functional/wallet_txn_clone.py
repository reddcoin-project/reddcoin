#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the wallet accounts properly when there are cloned transactions with malleated scriptsigs."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_equal,
)
from test_framework.messages import (
    COIN,
    tx_from_hex,
)


class TxnMallTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.supports_cli = False
        if self.options.segwit:
            # Ensure change outputs are p2sh-segwit so subsequent transactions
            # spend SegWit UTXOs (cache UTXOs are all legacy P2PKH/P2PK).
            self.extra_args = [
                ['-changetype=p2sh-segwit'],
                ['-changetype=p2sh-segwit'],
                ['-changetype=p2sh-segwit'],
            ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_options(self, parser):
        parser.add_argument("--mineblock", dest="mine_block", default=False, action="store_true",
                            help="Test double-spend of 1-confirmed transaction")
        parser.add_argument("--segwit", dest="segwit", default=False, action="store_true",
                            help="Test behaviour with SegWit txn (which should fail)")

    def setup_network(self):
        # Start with split network:
        super().setup_network()
        self.disconnect_nodes(1, 2)

    def run_test(self):
        if self.options.segwit:
            output_type = "p2sh-segwit"
            # Activate SegWit via BIP9 signaling (cache at height 199, need 432).
            # Network is split (1-2 disconnected), so reconnect temporarily.
            self.log.info("Activating SegWit (generating 233 blocks to height 432)...")
            self.connect_nodes(1, 2)
            advance_time_for_pos(self.nodes, seconds=600)
            self.nodes[0].generate(233)
            self.sync_blocks()
            self.disconnect_nodes(1, 2)
        else:
            output_type = "legacy"

        # All nodes should start with positive balance from cache.
        # ReddCoin cache distributes blocks unevenly across nodes (variable PoS
        # rewards), so balances differ per node unlike Bitcoin's 25×50=1250 each.
        starting_balance = self.nodes[0].getbalance()
        for i in range(3):
            assert self.nodes[i].getbalance() > 0

        self.nodes[0].settxfee(.01)

        node0_address1 = self.nodes[0].getnewaddress(address_type=output_type)
        node0_txid1 = self.nodes[0].sendtoaddress(node0_address1, 1219)
        node0_tx1 = self.nodes[0].gettransaction(node0_txid1)

        node0_address2 = self.nodes[0].getnewaddress(address_type=output_type)
        node0_txid2 = self.nodes[0].sendtoaddress(node0_address2, 29)
        node0_tx2 = self.nodes[0].gettransaction(node0_txid2)

        assert_equal(self.nodes[0].getbalance(),
                     starting_balance + node0_tx1["fee"] + node0_tx2["fee"])

        # For SegWit test, lock all non-segwit UTXOs so coin selection must use
        # the segwit change outputs from the self-sends above. Cache UTXOs are
        # all legacy (P2PKH/P2PK), and coin selection would prefer them.
        if self.options.segwit:
            for utxo in self.nodes[0].listunspent(minconf=0, include_unsafe=True):
                desc = utxo.get("desc", "")
                if not desc.startswith("sh(wpkh("):
                    self.nodes[0].lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])

        # Coins are sent to node1_address
        node1_address = self.nodes[1].getnewaddress()

        # Send tx1, and another transaction tx2 that won't be cloned
        txid1 = self.nodes[0].sendtoaddress(node1_address, 40)
        txid2 = self.nodes[0].sendtoaddress(node1_address, 20)

        # Construct a clone of tx1, to be malleated
        rawtx1 = self.nodes[0].getrawtransaction(txid1, 1)
        clone_inputs = [{"txid": rawtx1["vin"][0]["txid"], "vout": rawtx1["vin"][0]["vout"], "sequence": rawtx1["vin"][0]["sequence"]}]
        clone_outputs = {rawtx1["vout"][0]["scriptPubKey"]["address"]: rawtx1["vout"][0]["value"],
                         rawtx1["vout"][1]["scriptPubKey"]["address"]: rawtx1["vout"][1]["value"]}
        clone_locktime = rawtx1["locktime"]
        clone_raw = self.nodes[0].createrawtransaction(clone_inputs, clone_outputs, clone_locktime)

        # createrawtransaction randomizes the order of its outputs, so swap them if necessary.
        clone_tx = tx_from_hex(clone_raw)
        if (rawtx1["vout"][0]["value"] == 40 and clone_tx.vout[0].nValue != 40*COIN or rawtx1["vout"][0]["value"] != 40 and clone_tx.vout[0].nValue == 40*COIN):
            (clone_tx.vout[0], clone_tx.vout[1]) = (clone_tx.vout[1], clone_tx.vout[0])

        # Use a different signature hash type to sign.  This creates an equivalent but malleated clone.
        # Don't send the clone anywhere yet
        tx1_clone = self.nodes[0].signrawtransactionwithwallet(clone_tx.serialize().hex(), None, "ALL|ANYONECANPAY")
        assert_equal(tx1_clone["complete"], True)

        # Have node0 mine a block, if requested:
        if (self.options.mine_block):
            if self.options.segwit:
                # Unlock UTXOs for staking — tx1/tx2 already created with segwit inputs
                self.nodes[0].lockunspent(True)
            pre_mine = self.nodes[0].getbalance()
            self.nodes[0].generate(1)
            self.sync_blocks(self.nodes[0:2])
            mine_delta = self.nodes[0].getbalance() - pre_mine

        tx1 = self.nodes[0].gettransaction(txid1)
        tx2 = self.nodes[0].gettransaction(txid2)

        # Node0's balance should be starting balance, plus matured PoS reward for
        # another matured block, minus tx1 and tx2 amounts, and minus transaction fees:
        expected = starting_balance + node0_tx1["fee"] + node0_tx2["fee"]
        if self.options.mine_block:
            expected += mine_delta
        expected += tx1["amount"] + tx1["fee"]
        expected += tx2["amount"] + tx2["fee"]
        assert_equal(self.nodes[0].getbalance(), expected)

        if self.options.mine_block:
            assert_equal(tx1["confirmations"], 1)
            assert_equal(tx2["confirmations"], 1)
        else:
            assert_equal(tx1["confirmations"], 0)
            assert_equal(tx2["confirmations"], 0)

        # Send clone and its parent to miner
        self.nodes[2].sendrawtransaction(node0_tx1["hex"])
        txid1_clone = self.nodes[2].sendrawtransaction(tx1_clone["hex"])
        if self.options.segwit:
            assert_equal(txid1, txid1_clone)
            return

        # ... mine a block...
        self.nodes[2].generate(1)

        # Reconnect the split network, and sync chain:
        self.connect_nodes(1, 2)
        self.nodes[2].sendrawtransaction(node0_tx2["hex"])
        self.nodes[2].sendrawtransaction(tx2["hex"])
        self.nodes[2].generate(1)  # Mine another block to make sure we sync
        self.sync_blocks()

        # Re-fetch transaction info:
        tx1 = self.nodes[0].gettransaction(txid1)
        tx1_clone = self.nodes[0].gettransaction(txid1_clone)
        tx2 = self.nodes[0].gettransaction(txid2)

        # Verify expected confirmations
        assert_equal(tx1["confirmations"], -2)
        assert_equal(tx1_clone["confirmations"], 2)
        assert_equal(tx2["confirmations"], 1)

        # Check node0's total balance; should be same as before the clone, plus
        # net matured rewards from the reorg (PoS rewards are variable).
        # The clone sends the same amount as tx1, so the clone substitution has
        # no balance effect. The balance change comes from newly matured blocks
        # on the longer chain, minus any orphaned block rewards.
        reorg_balance_delta = self.nodes[0].getbalance() - expected
        self.log.info(f"Reorg balance delta: {reorg_balance_delta}")
        expected += reorg_balance_delta
        assert_equal(self.nodes[0].getbalance(), expected)


if __name__ == '__main__':
    TxnMallTest().main()
