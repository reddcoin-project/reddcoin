#!/usr/bin/env python3
# Copyright (c) 2014-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test basic PoS block generation functionality.

This test validates that the test framework properly supports Reddcoin's
Proof-of-Stake consensus mechanism, including:
- Cache creation with aged coins
- Mock time advancement for coinage
- PoS block generation with generatetoaddress
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)


class PosBasicTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with pre-generated blocks

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.log.info("Starting PoS basic test")
        node = self.nodes[0]

        # Verify we started with the cache
        initial_height = node.getblockcount()
        self.log.info(f"Initial block height: {initial_height}")
        assert initial_height >= 199, f"Expected at least 199 blocks, got {initial_height}"

        # Check wallet has mature coins from cache
        balance = node.getbalance()
        self.log.info(f"Wallet balance: {balance}")
        assert balance > 0, "Wallet should have coins from cache"

        # Advance time to ensure coins have sufficient age for staking
        advance_time_for_pos(node, seconds=600)

        # Try to generate a PoS block with retry logic
        # PoS is probabilistic - we may need to advance time and retry
        self.log.info("Generating PoS block...")
        from test_framework.test_node import TestNode as TN
        test_address = TN.PRIV_KEYS[0].address

        max_attempts = 10
        success = False
        for attempt in range(max_attempts):
            try:
                blockhash = node.generatetoaddress(1, test_address)[0]
                new_height = node.getblockcount()
                self.log.info(f"Generated PoS block at height {new_height} (took {attempt + 1} attempts)")
                assert_equal(new_height, initial_height + 1)
                success = True
                break
            except Exception as e:
                if "no valid coinstake found" in str(e):
                    if attempt < max_attempts - 1:
                        advance_time_for_pos(node, seconds=60)
                else:
                    raise

        if not success:
            raise AssertionError(f"Failed to generate PoS block after {max_attempts} attempts")

        # Try to generate a few more blocks with time advancement
        self.log.info("Generating additional PoS blocks...")
        for i in range(3):
            advance_time_for_pos(node, seconds=60)
            blockhash = node.generate(1)[0]
            self.log.info(f"Generated block {i+1}: {blockhash}")

        final_height = node.getblockcount()
        self.log.info(f"Final block height: {final_height}")
        assert_equal(final_height, initial_height + 4)

        self.log.info("PoS basic test completed successfully!")


if __name__ == '__main__':
    PosBasicTest().main()
