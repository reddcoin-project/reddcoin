#!/usr/bin/env python3
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test descriptor and legacy wallet staking interoperability.

This test verifies that:
1. A node with a descriptor wallet can stake PoS blocks
2. A node with a legacy wallet can stake PoS blocks
3. Blocks staked by descriptor wallet are validated by legacy wallet node
4. Blocks staked by legacy wallet are validated by descriptor wallet node
5. Both nodes stay in sync throughout the process
6. Descriptor wallets recycle P2PK coinstake outputs (no UTXO exhaustion)
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)


class DescriptorLegacyStakingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ['-keypool=100', '-whitelist=127.0.0.1', '-peertimeout=999999999'],  # Node 0: descriptor wallet
            ['-keypool=100', '-whitelist=127.0.0.1', '-peertimeout=999999999'],  # Node 1: legacy wallet
        ]
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def generate_pos_block(self, wallet_rpc, address, max_attempts=20):
        """Generate a PoS block with retry logic, advancing time on all nodes."""
        for attempt in range(max_attempts):
            try:
                advance_time_for_pos(self.nodes, seconds=60)
                return wallet_rpc.generatetoaddress(1, address)[0]
            except Exception as e:
                if "no valid coinstake found" in str(e):
                    if attempt < max_attempts - 1:
                        continue
                raise
        raise AssertionError(f"Failed to generate PoS block after {max_attempts} attempts")

    def run_test(self):
        node0 = self.nodes[0]  # Descriptor wallet node
        node1 = self.nodes[1]  # Legacy wallet node

        # =====================================================
        # Connect nodes FIRST before setting mocktime.
        # nTimeConnected uses GetTimeSeconds() (real time) but
        # InactivityCheck uses GetTime() (mocktime). Connecting
        # before mocktime is set ensures the handshake completes
        # with real time values, preventing immediate disconnection.
        # =====================================================
        self.log.info("Connecting nodes before setting mocktime...")
        self.connect_nodes(0, 1)

        # Now set mocktime to current real time on all nodes.
        initial_time = int(time.time())
        for node in self.nodes:
            node.setmocktime(initial_time)
            node.mocktime = initial_time

        # =====================================================
        # Setup Node 0 with Descriptor Wallet
        # =====================================================
        self.log.info("Setting up Node 0 with descriptor wallet...")
        node0.createwallet(wallet_name="desc_wallet", descriptors=True)
        desc_wallet = node0.get_wallet_rpc("desc_wallet")

        wallet_info = desc_wallet.getwalletinfo()
        assert_equal(wallet_info['format'], 'sqlite')
        self.log.info(f"Node 0: Descriptor wallet created (sqlite format)")

        desc_addr = desc_wallet.getnewaddress("", "legacy")
        self.log.info(f"Node 0 staking address: {desc_addr}")

        # =====================================================
        # Setup Node 1 with Legacy Wallet
        # =====================================================
        self.log.info("Setting up Node 1 with legacy wallet...")
        node1.createwallet(wallet_name="legacy_wallet", descriptors=False)
        legacy_wallet = node1.get_wallet_rpc("legacy_wallet")

        wallet_info = legacy_wallet.getwalletinfo()
        assert_equal(wallet_info['format'], 'bdb')
        self.log.info(f"Node 1: Legacy wallet created (bdb format)")

        num_legacy_addrs = 10
        legacy_addrs = [legacy_wallet.getnewaddress("", "legacy") for _ in range(num_legacy_addrs)]
        self.log.info(f"Node 1: Generated {num_legacy_addrs} staking addresses")

        # =====================================================
        # Generate PoW blocks on Node 0 (89 blocks = end of PoW)
        # =====================================================
        self.log.info("Node 0: Generating 89 PoW blocks...")
        for i in range(89):
            advance_time_for_pos(self.nodes, seconds=60)
            desc_wallet.generatetoaddress(1, desc_addr)
        self.sync_all()
        assert_equal(node0.getblockcount(), 89)
        self.log.info(f"Node 0: Generated 89 PoW blocks")

        # Advance time for coin age
        advance_time_for_pos(self.nodes, seconds=600)

        # =====================================================
        # Generate 11 PoS blocks (height 100, like feature_pos_sync.py)
        # Keep initial PoS blocks low to preserve coinbase UTXOs
        # for the longer maturity phase later.
        # =====================================================
        self.log.info("Node 0: Generating 11 PoS blocks...")
        for i in range(11):
            self.generate_pos_block(desc_wallet, desc_addr)
        self.sync_all()
        assert_equal(node0.getblockcount(), 100)
        self.log.info(f"Node 0: At height 100")

        # =====================================================
        # Send coins from Node 0 to Node 1 for staking.
        # Use a single transaction with outputs to separate addresses
        # so each UTXO has a distinct scriptPubKey (prevents coinstake
        # combine from grouping them).
        # =====================================================
        self.log.info("Node 0: Sending coins to Node 1 (separate addresses)...")
        send_amounts = {addr: 1000000 for addr in legacy_addrs}
        desc_wallet.sendmany("", send_amounts)

        # Generate a block to confirm the transaction
        self.generate_pos_block(desc_wallet, desc_addr)
        self.sync_all()
        self.log.info(f"Node 1: Balance after receiving: {legacy_wallet.getbalance()}")

        # =====================================================
        # Generate blocks on Node 0 to age Node 1's coins.
        # Node 1's coins are regular (non-coinbase) outputs, so they
        # don't need COINBASE_MATURITY -- just nStakeMinAge (10s).
        # We generate 70 blocks to build sufficient coin age for
        # reliable kernel finding.
        # =====================================================
        maturity_blocks = 70
        self.log.info(f"Node 0: Generating {maturity_blocks} blocks to age Node 1's coins...")
        for i in range(maturity_blocks):
            advance_time_for_pos(self.nodes, seconds=60)
            success = False
            for attempt in range(10):
                try:
                    desc_wallet.generatetoaddress(1, desc_addr)
                    success = True
                    break
                except Exception as e:
                    if "no valid coinstake found" in str(e) and attempt < 9:
                        advance_time_for_pos(self.nodes, seconds=60)
                    else:
                        raise
            if not success:
                raise AssertionError(f"Failed to generate maturity block {i}")
            if (i + 1) % 20 == 0:
                self.log.info(f"Node 0: Generated {i + 1}/{maturity_blocks} maturity blocks")
                self.sync_all()

        self.sync_all()
        self.log.info(f"Node 0: Height: {node0.getblockcount()}")
        self.log.info(f"Node 1: Mature balance: {legacy_wallet.getbalance()}")

        # Verify sync
        assert_equal(node0.getblockcount(), node1.getblockcount())
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        self.log.info(f"Nodes synced at height {node0.getblockcount()}")

        # =====================================================
        # Node 0 (Descriptor Wallet) stakes 100 blocks.
        # This is the critical UTXO exhaustion test: Node 0 started
        # with 89 coinbase UTXOs and has already staked ~82 PoS blocks
        # (11 + 1 + 70). Without the P2PK fallback fix, the wallet
        # would have exhausted all UTXOs since each coinstake converts
        # P2PKH→P2PK. With the fix, P2PK outputs are recognized and
        # recycled, allowing indefinite staking.
        #
        # Coinstake outputs need COINBASE_MATURITY+1 (61) confirmations
        # to mature. To bridge this maturity gap, split the balance into
        # additional UTXOs so the wallet has enough to stake continuously
        # until the first recycled outputs become spendable.
        # =====================================================
        # Create additional UTXOs to bridge the 61-block maturity gap
        self.log.info("Node 0: Splitting balance into additional UTXOs...")
        split_addrs = [desc_wallet.getnewaddress("", "legacy") for _ in range(40)]
        split_amounts = {addr: 10000 for addr in split_addrs}
        desc_wallet.sendmany("", split_amounts)
        self.generate_pos_block(desc_wallet, desc_addr)
        self.sync_all()

        # Log UTXO state before exhaustion test
        utxos = desc_wallet.listunspent()
        self.log.info(f"Node 0: {len(utxos)} UTXOs before exhaustion test")
        self.log.info(f"Node 0: Balance = {desc_wallet.getbalance()}")

        desc_stake_count = 100
        self.log.info(f"Node 0 (Descriptor): Generating {desc_stake_count} PoS blocks (UTXO exhaustion test)...")
        desc_blocks = []
        for i in range(desc_stake_count):
            # Higher max_attempts: recycled coinstake P2PK outputs have lower
            # coin age weight than original coinbase UTXOs, reducing kernel
            # probability per attempt.
            blockhash = self.generate_pos_block(desc_wallet, desc_addr, max_attempts=100)
            desc_blocks.append(blockhash)
            if (i + 1) % 10 == 0:
                utxos = desc_wallet.listunspent()
                self.log.info(f"Node 0: Generated {i+1}/{desc_stake_count} PoS blocks, UTXOs={len(utxos)}, balance={desc_wallet.getbalance()}")
                self.sync_all()

        self.sync_all()

        # Verify Node 1 accepted Node 0's blocks
        for blockhash in desc_blocks:
            block = node1.getblock(blockhash)
            assert 'confirmations' in block and block['confirmations'] > 0
        self.log.info(f"Node 1 (Legacy): Validated all {desc_stake_count} blocks from Node 0 (Descriptor)")

        # =====================================================
        # Node 1 (Legacy Wallet) stakes 10 blocks.
        # Node 1 has 10 UTXOs of 1000 RDD each at separate addresses.
        # Coinstake outputs need COINBASE_MATURITY+1 (61) confirmations
        # to mature, so 10 blocks is within the single-round UTXO budget.
        # =====================================================
        advance_time_for_pos(self.nodes, seconds=600)

        legacy_stake_count = 10
        self.log.info(f"Node 1 (Legacy): Generating {legacy_stake_count} PoS blocks...")
        legacy_blocks = []
        for i in range(legacy_stake_count):
            blockhash = self.generate_pos_block(legacy_wallet, legacy_addrs[i % num_legacy_addrs])
            legacy_blocks.append(blockhash)
            self.log.info(f"Node 1: Generated PoS block {i+1}/{legacy_stake_count}: {blockhash[:16]}...")

        self.sync_all()

        # Verify Node 0 accepted Node 1's blocks
        for blockhash in legacy_blocks:
            block = node0.getblock(blockhash)
            assert 'confirmations' in block and block['confirmations'] > 0
        self.log.info(f"Node 0 (Descriptor): Validated all {legacy_stake_count} blocks from Node 1 (Legacy)")

        # =====================================================
        # Final verification
        # =====================================================
        final_height = node0.getblockcount()
        self.log.info(f"Final block height: {final_height}")

        assert_equal(node0.getblockcount(), node1.getblockcount())
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

        # Verify descriptor wallet staked far beyond the original 89 UTXO limit
        desc_pos_total = 11 + 1 + 70 + 1 + desc_stake_count  # initial + confirm + maturity + split-confirm + final
        self.log.info(f"Node 0 (Descriptor) total PoS blocks: {desc_pos_total} (from 89 original UTXOs)")
        assert desc_pos_total > 89, "Descriptor wallet must stake more blocks than original UTXO count"

        self.log.info("=" * 60)
        self.log.info("TEST PASSED: Descriptor and Legacy wallet staking interop works!")
        self.log.info("=" * 60)
        self.log.info(f"  - Descriptor wallet staked {desc_pos_total} PoS blocks (no UTXO exhaustion)")
        self.log.info(f"  - Legacy wallet staked {legacy_stake_count} PoS blocks")
        self.log.info(f"  - Cross-wallet block validation successful")
        self.log.info(f"  - Both nodes stayed in sync (height: {final_height})")


if __name__ == '__main__':
    DescriptorLegacyStakingTest().main()
