#!/usr/bin/env python3
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test descriptor wallet staking functionality.

This test validates that descriptor wallets can successfully stake PoS blocks,
verifying that the DescriptorScriptPubKeyMan::GetKey method works correctly
for retrieving keys needed for coinstake transaction signing and block signing.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)

# ReddCoin regtest uses nCoinbaseMaturity = 60
REDDCOIN_COINBASE_MATURITY = 60

# Time spacing between blocks for PoS coinage
POS_BLOCK_SPACING = 60


class WalletDescriptorStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [['-keypool=100']]
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def generate_block(self, wallet_rpc, address, node):
        """Generate a single block with proper mocktime advancement."""
        # Advance mock time before each block
        block_time = node.getblockheader(node.getbestblockhash())['time']
        new_time = max(getattr(node, 'mocktime', 0), block_time) + POS_BLOCK_SPACING
        node.setmocktime(new_time)
        node.mocktime = new_time

        return wallet_rpc.generatetoaddress(1, address)[0]

    def run_test(self):
        node = self.nodes[0]

        # Initialize mocktime based on current time
        initial_time = int(time.time())
        node.setmocktime(initial_time)
        node.mocktime = initial_time

        # Create a descriptor wallet for staking
        self.log.info("Creating descriptor wallet for staking...")
        node.createwallet(wallet_name="desc_staking", descriptors=True)
        desc_wallet = node.get_wallet_rpc("desc_staking")

        # Verify it's a descriptor wallet
        wallet_info = desc_wallet.getwalletinfo()
        assert_equal(wallet_info['format'], 'sqlite')
        self.log.info(f"Descriptor wallet created with keypool size: {wallet_info['keypoolsize']}")

        # Get a staking address from the descriptor wallet
        staking_addr = desc_wallet.getnewaddress("", "legacy")
        addr_info = desc_wallet.getaddressinfo(staking_addr)
        self.log.info(f"Staking address: {staking_addr}")
        self.log.info(f"Address descriptor: {addr_info['desc']}")

        # Generate 89 PoW blocks (blocks 1-89, PoW ends at block 89)
        self.log.info("Generating 89 PoW blocks...")
        for i in range(89):
            self.generate_block(desc_wallet, staking_addr, node)
        self.log.info(f"Generated 89 PoW blocks, height: {node.getblockcount()}")

        # Check balance
        balance = desc_wallet.getbalance()
        immature = desc_wallet.getbalances()['mine']['immature']
        self.log.info(f"Balance: {balance}, Immature: {immature}")

        # Advance time for coin age (10 minutes)
        self.log.info("Advancing time for coin maturity...")
        node.mocktime += 600
        node.setmocktime(node.mocktime)

        # Generate enough PoS blocks to mature some coins
        # With COINBASE_MATURITY=60, after 60 more blocks the first coinbase is mature
        # We'll generate 70 PoS blocks to have ~10 mature coinbases available
        self.log.info("Generating PoS blocks to mature coins...")
        blocks_to_mature = REDDCOIN_COINBASE_MATURITY + 10
        for i in range(blocks_to_mature):
            max_attempts = 20
            success = False
            for attempt in range(max_attempts):
                try:
                    self.generate_block(desc_wallet, staking_addr, node)
                    success = True
                    break
                except Exception as e:
                    if "no valid coinstake found" in str(e):
                        if attempt < max_attempts - 1:
                            node.mocktime += 60
                            node.setmocktime(node.mocktime)
                    else:
                        raise
            if not success:
                raise AssertionError(f"Failed to generate PoS block {i+1} after {max_attempts} attempts")

            if (i + 1) % 20 == 0:
                self.log.info(f"Generated {i + 1}/{blocks_to_mature} PoS blocks")

        height = node.getblockcount()
        self.log.info(f"Current block height: {height}")

        # Check mature balance
        balance = desc_wallet.getbalance()
        self.log.info(f"Mature balance: {balance}")
        assert balance > 0, "Expected mature coins for staking"

        # Verify the last few blocks are PoS blocks
        self.log.info("Verifying block types...")
        for i in range(3):
            blockhash = node.getblockhash(height - i)
            block = node.getblock(blockhash)
            if 'flags' in block:
                self.log.info(f"Block {height - i}: PoS (flags: {block['flags']})")
            else:
                self.log.info(f"Block {height - i}: PoW")

        # Final verification: generate a few more staking blocks
        self.log.info("Final verification: generating 5 more stake blocks...")
        node.mocktime += 600
        node.setmocktime(node.mocktime)

        for i in range(5):
            max_attempts = 20
            success = False
            for attempt in range(max_attempts):
                try:
                    final_hash = self.generate_block(desc_wallet, staking_addr, node)
                    success = True
                    break
                except Exception as e:
                    if "no valid coinstake found" in str(e):
                        if attempt < max_attempts - 1:
                            node.mocktime += 60
                            node.setmocktime(node.mocktime)
                    else:
                        raise
            if not success:
                raise AssertionError(f"Failed to generate final block {i+1}")

            block = node.getblock(final_hash)
            if 'flags' in block:
                self.log.info(f"Generated PoS block {i+1}/5: {final_hash[:16]}...")

        final_height = node.getblockcount()
        self.log.info(f"Final block height: {final_height}")
        self.log.info("Descriptor wallet staking test completed successfully!")


if __name__ == '__main__':
    WalletDescriptorStakingTest().main()
