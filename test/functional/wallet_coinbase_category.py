#!/usr/bin/env python3
# Copyright (c) 2014-2018 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test coinbase/coinstake transactions return the correct categories.

Tests listtransactions, listsinceblock, and gettransaction.

ReddCoin PoS adaptation: In PoS blocks (height 90+ on regtest), coinbase
outputs are empty (0 value). Block rewards are distributed through the
coinstake transaction (tx[1]). ReddCoin uses "stake" category for active
coinstake transactions and "stake-orphan" for orphaned ones (unlike
Bitcoin's immature/generate/orphan coinbase categories).
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_array_result,
)

class CoinbaseCategoryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_category(self, category, address, txid, skip):
        # Use large count — PoS coinstake entries make fixed skip unreliable
        assert_array_result(self.nodes[0].listtransactions("*", 9999),
                            {"txid": txid, "address": address},
                            {"category": category})
        assert_array_result(self.nodes[0].listsinceblock()["transactions"],
                            {"txid": txid, "address": address},
                            {"category": category})
        assert_array_result(self.nodes[0].gettransaction(txid)["details"],
                            {"address": address},
                            {"category": category})

    def run_test(self):
        node = self.nodes[0]

        # Advance time to ensure coins have sufficient age for staking
        advance_time_for_pos(node, seconds=600)

        # Generate one PoS block
        blockhash = node.generate(1)[0]
        block = node.getblock(blockhash)

        # In PoS, reward is in coinstake (tx[1]), not coinbase (tx[0])
        assert len(block["tx"]) > 1, "Expected PoS block with coinstake"
        coinstake_txid = block["tx"][1]

        # Find the address that received the coinstake reward (positive amount)
        cs_details = node.gettransaction(coinstake_txid)["details"]
        address = None
        for detail in cs_details:
            if detail["category"] == "stake" and detail["amount"] > 0:
                address = detail["address"]
                break
        assert address is not None, "Could not find coinstake receive address"

        # Coinstake transaction has "stake" category after 1 confirmation
        self.assert_category("stake", address, coinstake_txid, 0)

        # Mine another 59 blocks on top (60 total confirmations)
        # ReddCoin regtest nCoinbaseMaturity = 60
        node.generate(59)
        # Coinstake still shows "stake" category — no immature/generate
        # transition like Bitcoin coinbase; maturity only affects spendability
        self.assert_category("stake", address, coinstake_txid, 59)

        # Mine one more block (61 total confirmations — now mature)
        node.generate(1)
        # Category remains "stake" even after maturity
        self.assert_category("stake", address, coinstake_txid, 60)

        # Orphan block that contained the coinstake
        node.invalidateblock(blockhash)
        # Coinstake transaction is now orphaned — category changes to "stake-orphan"
        self.assert_category("stake-orphan", address, coinstake_txid, 60)

if __name__ == '__main__':
    CoinbaseCategoryTest().main()
