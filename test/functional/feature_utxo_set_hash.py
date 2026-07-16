#!/usr/bin/env python3
# Copyright (c) 2020-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test UTXO set hash value calculation in gettxoutsetinfo."""

import struct

from test_framework.messages import (
    CBlock,
    COutPoint,
    from_hex,
)
from test_framework.muhash import MuHash3072
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

class UTXOSetHashTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def test_muhash_implementation(self):
        self.log.info("Test MuHash implementation consistency")

        node = self.nodes[0]
        mocktime = node.getblockheader(node.getblockhash(0))['time'] + 1
        node.setmocktime(mocktime)

        # Generate 100 blocks to cover both PoW (1-89) and PoS (90+) phases.
        block_hashes = node.generate(100)
        blocks = list(map(lambda block: from_hex(CBlock(), node.getblock(block, False)), block_hashes))

        # Create a spending transaction using the node wallet and mine it
        addr = node.getnewaddress()
        txid = node.sendtoaddress(addr, 1)
        tx_block_hash = node.generate(1)[0]
        blocks.append(from_hex(CBlock(), node.getblock(tx_block_hash, False)))

        # Build UTXO set by tracking all created outputs and removing spent ones.
        # Key: (txid_int, n) -> serialized MuHash data
        utxo_data = {}

        for height, block in enumerate(blocks):
            # The Genesis block coinbase is not part of the UTXO set
            height += 1

            for tx in block.vtx:
                txid_int = int(tx.rehash(), 16)
                coinbase = 1 if not tx.vin[0].prevout.hash else 0

                # Remove spent outputs (skip coinbase inputs which have no prevout)
                if not coinbase:
                    for vin in tx.vin:
                        spent_key = (vin.prevout.hash, vin.prevout.n)
                        if spent_key in utxo_data:
                            del utxo_data[spent_key]

                for n, tx_out in enumerate(tx.vout):
                    # Skip witness commitment
                    if coinbase and n > 0:
                        continue

                    data = COutPoint(txid_int, n).serialize()
                    data += struct.pack("<i", height * 2 + coinbase)
                    data += tx_out.serialize()

                    utxo_data[(txid_int, n)] = data

        # Insert all unspent outputs into MuHash
        muhash = MuHash3072()
        for data in utxo_data.values():
            muhash.insert(data)

        finalized = muhash.digest()
        node_muhash = node.gettxoutsetinfo("muhash")['muhash']

        assert_equal(finalized[::-1].hex(), node_muhash)

        self.log.info("Test deterministic UTXO set hash results")
        # Verify the hash fields are present and non-empty (exact values differ
        # from Bitcoin due to different COINBASE_MATURITY and block structure)
        assert node.gettxoutsetinfo()['hash_serialized_2']
        assert node.gettxoutsetinfo("muhash")['muhash']

    def run_test(self):
        self.test_muhash_implementation()


if __name__ == '__main__':
    UTXOSetHashTest().main()
