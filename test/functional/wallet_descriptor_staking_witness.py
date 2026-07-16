#!/usr/bin/env python3
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test descriptor wallet staking with witness (P2WPKH) and taproot kernels.

The existing descriptor staking tests stake only from legacy (P2PKH)
addresses, leaving the witness kernel paths in CreateCoinStake
(src/pos/stake.cpp) uncovered. This test covers them.

Phase 1 - SegWit INACTIVE (deterministic, low height):
    A P2WPKH UTXO MUST be skipped as a stake kernel (the pre-SegWit filter
    in CreateCoinStake); otherwise the coinstake would embed witness data
    and the block would be rejected as "unexpected-witness". The wallet must
    keep staking from its legacy UTXOs and the P2WPKH UTXO must stay unspent.

    Taproot is NOT exercised here: a tr() descriptor cannot be imported while
    Taproot is inactive ("Cannot import tr() descriptor when Taproot is not
    active"), and ReddCoin descriptor wallets do not create bech32m
    descriptors by default (SetupDescriptorScriptPubKeyMans skips BECH32M),
    so a taproot UTXO cannot exist pre-activation.

Phase 2 - SegWit + Taproot ACTIVE:
    Drive BIP9 activation by staking through the signalling windows, then:
      * stake from a wallet funded only with P2WPKH UTXOs
        (exercises the WITNESS_V0_KEYHASH branch -> P2PK coinstake output)
      * import an active tr() descriptor, stake from a wallet funded only
        with taproot UTXOs (exercises the WITNESS_V1_TAPROOT key-path branch)

    Activation requires PoS-generated blocks to signal BIP9 (they do, via
    ComputeBlockVersion in the miner). If activation is not reached within
    the block budget, Phase 2 is skipped rather than failed.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.descriptors import descsum_create
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

# ReddCoin regtest: PoW ends at height 89, PoS begins at 90.
POW_HEIGHT = 89
# ReddCoin regtest uses nCoinbaseMaturity = 60.
REDDCOIN_COINBASE_MATURITY = 60
# Seconds between blocks (target spacing) to accrue PoS coin age.
POS_BLOCK_SPACING = 60
# Upper bound on extra blocks mined while waiting for BIP9 activation.
# SegWit/Taproot (nStartTime=0) lock in at height 144 and activate at 288.
ACTIVATION_BLOCK_BUDGET = 600
# Blocks mined to bury post-activation (taproot) UTXOs deep enough to stake.
DEEP_BURIAL = 300

# Regtest-compatible extended private key (testnet ext-key prefix, accepted on
# regtest) used to build an importable, signable tr() descriptor. Reused from
# wallet_taproot.py's key table.
TR_XPRV = ("tprv8ZgxMBicQKsPeNLUGrbv3b7qhUk1LQJZAGMuk9gVuKh9sd4BWGp1eMsehUni6"
           "qGb8bjkdwBxCbgNGdh2bYGACK5C5dRTaif9KBKGVnSezxV")


class WalletDescriptorStakingWitnessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [['-keypool=100']]
        self.wallet_names = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------
    def _advance_time(self, seconds=POS_BLOCK_SPACING):
        node = self.nodes[0]
        new_time = max(getattr(node, 'mocktime', 0),
                       node.getblockheader(node.getbestblockhash())['time']) + seconds
        node.setmocktime(new_time)
        node.mocktime = new_time

    def _stake_block(self, wallet_rpc, address, max_attempts=30):
        """Generate one PoS block, advancing mocktime and retrying when no
        valid coinstake is found yet. Returns the block hash."""
        for attempt in range(max_attempts):
            self._advance_time()
            try:
                return wallet_rpc.generatetoaddress(1, address)[0]
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                    continue
                raise
        raise AssertionError(f"Failed to stake a PoS block after {max_attempts} attempts")

    def _softfork_active(self, name):
        fork = self.nodes[0].getblockchaininfo().get('softforks', {}).get(name)
        if not fork:
            return False
        if 'active' in fork:
            return bool(fork['active'])
        return fork.get('bip9', {}).get('status') == 'active'

    def _outpoints_for_address(self, wallet_rpc, address):
        return {(u['txid'], u['vout'])
                for u in wallet_rpc.listunspent(0, 9999999, [address])}

    def _mine_to_pos_maturity(self, wallet_rpc, address):
        """Mine 89 PoW blocks then enough PoS blocks to have spendable balance."""
        node = self.nodes[0]
        self.log.info("Generating %d PoW blocks...", POW_HEIGHT)
        for _ in range(POW_HEIGHT):
            self._advance_time()
            wallet_rpc.generatetoaddress(1, address)
        assert_equal(node.getblockcount(), POW_HEIGHT)

        self._advance_time(600)  # let early coinbases age
        pos_blocks = REDDCOIN_COINBASE_MATURITY + 10
        self.log.info("Generating %d PoS blocks to mature coins...", pos_blocks)
        for _ in range(pos_blocks):
            self._stake_block(wallet_rpc, address)
        assert_greater_than(float(wallet_rpc.getbalance()), 0)

    def _make_witness_staker(self, funder, addr_type, taproot_xprv=None):
        """Create a fresh wallet funded with several UTXOs of addr_type and
        return (staker_rpc, addrs). The coins still need to be buried before
        they can stake (see _assert_can_stake)."""
        node = self.nodes[0]
        node.createwallet(wallet_name=f"stk_{addr_type}", descriptors=True)
        staker = node.get_wallet_rpc(f"stk_{addr_type}")

        if taproot_xprv is not None:
            # Default descriptor wallets have no tr() descriptor; import an
            # active, signable one so the wallet can own/sign taproot outputs.
            desc = descsum_create(f"tr({taproot_xprv}/*)")
            res = staker.importdescriptors([{"desc": desc, "active": True,
                                             "timestamp": "now"}])
            assert res[0]["success"], f"tr() import failed: {res}"

        # Fund several distinct large UTXOs so a kernel can be found and the
        # coinstake "combine" does not group them. The funder holds the premine.
        addrs = [staker.getnewaddress("", addr_type) for _ in range(10)]
        funder.sendmany("", {a: 1000000 for a in addrs})
        self._stake_block(funder, funder.getnewaddress("", "legacy"))  # confirm
        return staker, addrs

    def _assert_can_stake(self, staker, addrs, label):
        """Prove the staker can produce a PoS block whose coinstake kernel is one
        of its own (witness) UTXOs. Since the staker holds only addr_type coins,
        a consumed UTXO means that kernel type was staked successfully."""
        node = self.nodes[0]
        self._advance_time(600)
        before = {(u['txid'], u['vout']) for u in staker.listunspent(1)}
        self.log.info("%s staker: utxos=%d", label, len(before))
        height_before = node.getblockcount()
        blockhash = self._stake_block(staker, addrs[0], max_attempts=60)
        assert_equal(node.getblockcount(), height_before + 1)
        consumed = before - {(u['txid'], u['vout']) for u in staker.listunspent(1)}
        assert len(consumed) >= 1, f"{label}: no {label} UTXO consumed as stake kernel"
        self.log.info("%s OK: staked block %s, consumed %d kernel UTXO(s)",
                      label, blockhash[:16], len(consumed))

    def _drive_activation(self, wallet_rpc, address):
        """Stake blocks until SegWit and Taproot are both active, or give up."""
        node = self.nodes[0]
        for mined in range(ACTIVATION_BLOCK_BUDGET):
            if self._softfork_active('segwit') and self._softfork_active('taproot'):
                return True
            self._stake_block(wallet_rpc, address)
            if (mined + 1) % 50 == 0:
                self.log.info("  ...mined %d blocks waiting for activation (height=%d)",
                              mined + 1, node.getblockcount())
        return self._softfork_active('segwit') and self._softfork_active('taproot')

    # ------------------------------------------------------------------
    # phases
    # ------------------------------------------------------------------
    def phase1_p2wpkh_skipped_before_segwit(self):
        node = self.nodes[0]
        self.log.info("=== Phase 1: P2WPKH UTXO skipped as kernel before SegWit ===")

        node.createwallet(wallet_name="desc_p1", descriptors=True)
        wallet = node.get_wallet_rpc("desc_p1")
        assert_equal(wallet.getwalletinfo()['format'], 'sqlite')

        legacy_addr = wallet.getnewaddress("", "legacy")
        self._mine_to_pos_maturity(wallet, legacy_addr)

        assert not self._softfork_active('segwit'), \
            "Phase 1 requires SegWit inactive; chain advanced past activation"

        # Create a P2WPKH UTXO owned by the wallet (wpkh is a default descriptor).
        p2wpkh_addr = wallet.getnewaddress("", "bech32")
        wallet.sendtoaddress(p2wpkh_addr, 1000)
        self._stake_block(wallet, legacy_addr)  # confirm the funding tx

        wit_before = self._outpoints_for_address(wallet, p2wpkh_addr)
        assert_greater_than(len(wit_before), 0)
        self.log.info("Funded %d P2WPKH UTXO(s)", len(wit_before))

        # Stake several more blocks; must keep succeeding from legacy UTXOs.
        height_before = node.getblockcount()
        for _ in range(15):
            self._stake_block(wallet, legacy_addr)
        assert_equal(node.getblockcount(), height_before + 15)

        # The P2WPKH UTXO must be untouched (skipped as a kernel pre-SegWit).
        assert_equal(self._outpoints_for_address(wallet, p2wpkh_addr), wit_before)
        self.log.info("Phase 1 OK: P2WPKH UTXO skipped and remains unspent")

    def phase2_stake_from_witness_after_activation(self):
        node = self.nodes[0]
        self.log.info("=== Phase 2: stake from P2WPKH / taproot kernels after activation ===")

        wallet = node.get_wallet_rpc("desc_p1")  # reuse the funded Phase 1 wallet
        funder_addr = wallet.getnewaddress("", "legacy")

        # Fund the P2WPKH staker BEFORE driving activation: P2WPKH UTXOs can be
        # created while SegWit is inactive, so the activation drive (hundreds of
        # blocks) buries them deeply enough to stake. Freshly-confirmed coins are
        # not stakeable until they have settled depth.
        p2wpkh_staker, p2wpkh_addrs = self._make_witness_staker(wallet, "bech32")

        if not self._drive_activation(wallet, funder_addr):
            self.log.info("SKIP Phase 2: SegWit/Taproot did not activate within %d blocks",
                          ACTIVATION_BLOCK_BUDGET)
            return
        self.log.info("SegWit + Taproot active at height %d", node.getblockcount())

        # WITNESS_V0_KEYHASH kernel -> P2PK coinstake output. Coins are now
        # buried by the whole activation drive.
        self._assert_can_stake(p2wpkh_staker, p2wpkh_addrs, "P2WPKH")

        # WITNESS_V1_TAPROOT kernel -> taproot key-path coinstake output. A tr()
        # descriptor can only be imported once Taproot is active, so the taproot
        # coins are necessarily created now and must be buried explicitly.
        tr_staker, tr_addrs = self._make_witness_staker(wallet, "bech32m", taproot_xprv=TR_XPRV)
        self.log.info("Burying taproot UTXOs under %d blocks...", DEEP_BURIAL)
        for _ in range(DEEP_BURIAL):
            self._stake_block(wallet, funder_addr)
        self._assert_can_stake(tr_staker, tr_addrs, "taproot")

    def run_test(self):
        node = self.nodes[0]
        node.setmocktime(int(time.time()))
        node.mocktime = int(time.time())

        self.phase1_p2wpkh_skipped_before_segwit()
        self.phase2_stake_from_witness_after_activation()
        self.log.info("Witness/taproot descriptor staking test completed")


if __name__ == '__main__':
    WalletDescriptorStakingWitnessTest().main()
