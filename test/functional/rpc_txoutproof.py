#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test gettxoutproof and verifytxoutproof RPCs."""

from decimal import Decimal
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    CMerkleBlock,
    from_hex,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class MerkleBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            [],
            ["-txindex"],
        ]
        # ReddCoin: framework adds -txindex=1 to all nodes for PoS staking.
        # Both nodes effectively have txindex, so the "spent tx without txindex"
        # test case is skipped (gettxoutproof always finds confirmed txs via txindex).

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        # Use cache (199 blocks) + generate enough for mature coins
        node.generate(COINBASE_MATURITY)
        self.sync_all()

        chain_height = self.nodes[1].getblockcount()
        assert_equal(chain_height, 199 + COINBASE_MATURITY)

        # Create single-output raw transactions for full control
        addr1 = node.getnewaddress()
        addr2 = node.getnewaddress()

        # Pick two unspent outputs for our two txs
        unspents = node.listunspent()
        utxo1 = unspents[0]
        utxo2 = unspents[1]

        fee = Decimal("0.01")
        raw1 = node.createrawtransaction(
            [{"txid": utxo1["txid"], "vout": utxo1["vout"]}],
            {addr1: float(utxo1["amount"] - fee)})
        signed1 = node.signrawtransactionwithwallet(raw1)
        txid1 = node.sendrawtransaction(signed1["hex"])

        raw2 = node.createrawtransaction(
            [{"txid": utxo2["txid"], "vout": utxo2["vout"]}],
            {addr2: float(utxo2["amount"] - fee)})
        signed2 = node.signrawtransactionwithwallet(raw2)
        txid2 = node.sendrawtransaction(signed2["hex"])

        # This will raise an exception because the transaction is not yet in a block
        assert_raises_rpc_error(-5, "Transaction not yet in block", node.gettxoutproof, [txid1])

        node.generate(1)
        blockhash = node.getblockhash(chain_height + 1)
        self.sync_all()

        txlist = []
        blocktxn = node.getblock(blockhash, True)["tx"]
        # PoS blocks have [coinbase, coinstake, tx1, tx2, ...]; find our txs by txid
        assert txid1 in blocktxn and txid2 in blocktxn
        txlist.append(txid1)
        txlist.append(txid2)

        assert_equal(node.verifytxoutproof(node.gettxoutproof([txid1])), [txid1])
        # PoS block tx ordering may differ from send order; use sorted comparison
        assert_equal(sorted(node.verifytxoutproof(node.gettxoutproof([txid1, txid2]))), sorted(txlist))
        assert_equal(sorted(node.verifytxoutproof(node.gettxoutproof([txid1, txid2], blockhash))), sorted(txlist))

        # Lock txid1's output so coinstake doesn't consume it before we spend it
        node.lockunspent(False, [{"txid": txid1, "vout": 0}])

        # Create txid3 in a separate block
        utxo3 = node.listunspent()[0]
        addr3 = node.getnewaddress()
        raw3 = node.createrawtransaction(
            [{"txid": utxo3["txid"], "vout": utxo3["vout"]}],
            {addr3: float(utxo3["amount"] - fee)})
        signed3 = node.signrawtransactionwithwallet(raw3)
        txid3 = node.sendrawtransaction(signed3["hex"])
        node.generate(1)
        self.sync_all()

        # Unlock and spend txid1's only output to make it fully spent
        node.lockunspent(True, [{"txid": txid1, "vout": 0}])
        tx1_detail = node.getrawtransaction(txid1, True)
        spend_addr = node.getnewaddress()
        raw_spend = node.createrawtransaction(
            [{"txid": txid1, "vout": 0}],
            {spend_addr: float(Decimal(str(tx1_detail["vout"][0]["value"])) - fee)})
        signed_spend = node.signrawtransactionwithwallet(raw_spend)
        assert signed_spend["complete"]
        node.sendrawtransaction(signed_spend["hex"])
        node.generate(1)
        self.sync_all()

        txid_spent = txid1   # Single output now spent
        txid_unspent = txid2  # Single output still unspent

        # Invalid txids
        assert_raises_rpc_error(-8, "txid must be of length 64 (not 32, for '00000000000000000000000000000000')", node.gettxoutproof, ["00000000000000000000000000000000"], blockhash)
        assert_raises_rpc_error(-8, "txid must be hexadecimal string (not 'ZZZ0000000000000000000000000000000000000000000000000000000000000')", node.gettxoutproof, ["ZZZ0000000000000000000000000000000000000000000000000000000000000"], blockhash)
        # Invalid blockhashes
        assert_raises_rpc_error(-8, "blockhash must be of length 64 (not 32, for '00000000000000000000000000000000')", node.gettxoutproof, [txid_spent], "00000000000000000000000000000000")
        assert_raises_rpc_error(-8, "blockhash must be hexadecimal string (not 'ZZZ0000000000000000000000000000000000000000000000000000000000000')", node.gettxoutproof, [txid_spent], "ZZZ0000000000000000000000000000000000000000000000000000000000000")
        # ReddCoin: txindex is always enabled (required for PoS staking), so
        # gettxoutproof can always find confirmed txs even when fully spent.
        # The "Transaction not yet in block" error only occurs for unconfirmed txs.
        # We can still get a proof for a spent tx by specifying the block explicitly:
        assert_equal(node.verifytxoutproof(node.gettxoutproof([txid_spent], blockhash)), [txid_spent])
        # And via txindex (without specifying blockhash) since all nodes have txindex:
        assert_equal(node.verifytxoutproof(node.gettxoutproof([txid_spent])), [txid_spent])
        # We can't get the proof if we specify a non-existent block
        assert_raises_rpc_error(-5, "Block not found", node.gettxoutproof, [txid_spent], "0000000000000000000000000000000000000000000000000000000000000000")
        # We can get the proof if the transaction is unspent
        assert_equal(node.verifytxoutproof(node.gettxoutproof([txid_unspent])), [txid_unspent])
        # We can get the proof if we provide a list of transactions and one of them is unspent. The ordering of the list should not matter.
        assert_equal(sorted(node.verifytxoutproof(node.gettxoutproof([txid1, txid2]))), sorted(txlist))
        assert_equal(sorted(node.verifytxoutproof(node.gettxoutproof([txid2, txid1]))), sorted(txlist))
        # We can always get a proof if we have a -txindex
        assert_equal(node.verifytxoutproof(self.nodes[1].gettxoutproof([txid_spent])), [txid_spent])
        # We can't get a proof if we specify transactions from different blocks
        assert_raises_rpc_error(-5, "Not all transactions found in specified or retrieved block", node.gettxoutproof, [txid1, txid3])
        # Test empty list
        assert_raises_rpc_error(-8, "Parameter 'txids' cannot be empty", node.gettxoutproof, [])
        # Test duplicate txid
        assert_raises_rpc_error(-8, 'Invalid parameter, duplicated txid', node.gettxoutproof, [txid1, txid1])

        # Now we'll try tweaking a proof.
        proof = self.nodes[1].gettxoutproof([txid1, txid2])
        assert txid1 in node.verifytxoutproof(proof)
        assert txid2 in self.nodes[1].verifytxoutproof(proof)

        tweaked_proof = from_hex(CMerkleBlock(), proof)

        # Make sure that our serialization/deserialization is working
        assert txid1 in node.verifytxoutproof(tweaked_proof.serialize().hex())

        # Check to see if we can go up the merkle tree and pass this off as a
        # single-transaction block
        tweaked_proof.txn.nTransactions = 1
        tweaked_proof.txn.vHash = [tweaked_proof.header.hashMerkleRoot]
        tweaked_proof.txn.vBits = [True] + [False]*7

        for n in self.nodes:
            assert not n.verifytxoutproof(tweaked_proof.serialize().hex())

        # TODO: try more variants, eg transactions at different depths, and
        # verify that the proofs are invalid

if __name__ == '__main__':
    MerkleBlockTest().main()
