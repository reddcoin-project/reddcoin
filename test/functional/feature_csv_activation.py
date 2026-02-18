#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test CSV soft fork activation.

This soft fork will activate the following BIPS:
BIP 68  - nSequence relative lock times
BIP 112 - CHECKSEQUENCEVERIFY
BIP 113 - MedianTimePast semantics for nLockTime

ReddCoin adaptation:
- PoS blocks after height 89 require valid coinstake (use node.generate / getblocktemplate)
- BIP68 enforced for nVersion >= 3 (not >= 2); BIP112 OP_CSV still uses >= 2
- MiniWallet funded via fundrawtransaction (PoS coinbase reward is 0)
- Block time comes from coinstake nTime; BIP113 MTP read from getblockheader
- PoS blocks have coinbase + coinstake + regular txs (tx count +2 not +1)

Generate 410 blocks to node wallet, fund MiniWallet with 83 UTXOs
Generate remaining blocks to reach height 427 with 600s spacing
Seed block chain with the 83 inputs at height 428
mine 2 blocks and verify soft fork not yet activated
mine 1 block and test that soft fork is activated (rules enforced for next block)
Test BIP 113 is enforced
Mine 4 blocks and test BIP 68 is enforced for time and height
Mine 1 block and test BIP 68 now passes time but not height
Mine 1 block and test BIP 68 now passes time and height
Test that BIP 112 is enforced

Various transactions will be used to test that the BIPs rules are not enforced before the soft fork activates
And that after the soft fork activates transactions pass and fail as they should according to the rules.
For each BIP, transactions of versions 1 and 2 (3 for BIP68) will be tested.
----------------
BIP 113:
bip113tx - modify the nLocktime variable

BIP 68:
bip68txs - 16 txs with nSequence relative locktime of 10 with various bits set as per the relative_locktimes below

BIP 112:
bip112txs_vary_nSequence - 16 txs with nSequence relative_locktimes of 10 evaluated against 10 OP_CSV OP_DROP
bip112txs_vary_nSequence_9 - 16 txs with nSequence relative_locktimes of 9 evaluated against 10 OP_CSV OP_DROP
bip112txs_vary_OP_CSV - 16 txs with nSequence = 10 evaluated against varying {relative_locktimes of 10} OP_CSV OP_DROP
bip112txs_vary_OP_CSV_9 - 16 txs with nSequence = 9 evaluated against varying {relative_locktimes of 10} OP_CSV OP_DROP
bip112tx_special - test negative argument to OP_CSV
bip112tx_emptystack - test empty stack (= no argument) OP_CSV
"""
from itertools import product

from test_framework.address import key_to_p2pkh
from test_framework.blocktools import (
    create_block,
    sign_block,
    NORMAL_GBT_REQUEST_PARAMS,
)
from test_framework.messages import (
    COIN,
    CTransaction,
    CTxOut,
)
from test_framework.p2p import P2PDataStore
from test_framework.script import (
    CScript,
    OP_CHECKSEQUENCEVERIFY,
    OP_DROP,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_equal,
    softfork_active,
)
from test_framework.wallet import (
    MiniWallet,
    MiniWalletMode,
)

TESTING_TX_COUNT = 83  # Number of testing transactions: 1 BIP113 tx, 16 BIP68 txs, 66 BIP112 txs (see comments above)
BASE_RELATIVE_LOCKTIME = 10
CSV_ACTIVATION_HEIGHT = 432
SEQ_DISABLE_FLAG = 1 << 31
SEQ_RANDOM_HIGH_BIT = 1 << 25
SEQ_TYPE_FLAG = 1 << 22
SEQ_RANDOM_LOW_BIT = 1 << 18

def relative_locktime(sdf, srhb, stf, srlb):
    """Returns a locktime with certain bits set."""

    locktime = BASE_RELATIVE_LOCKTIME
    if sdf:
        locktime |= SEQ_DISABLE_FLAG
    if srhb:
        locktime |= SEQ_RANDOM_HIGH_BIT
    if stf:
        locktime |= SEQ_TYPE_FLAG
    if srlb:
        locktime |= SEQ_RANDOM_LOW_BIT
    return locktime

def all_rlt_txs(txs):
    return [tx['tx'] for tx in txs]


def _generate_to_address_with_retry(node, seconds=600):
    """Advance time and generate one PoS block with retry logic."""
    advance_time_for_pos(node, seconds=seconds)
    for attempt in range(10):
        try:
            return node.generatetoaddress(1, node.get_deterministic_priv_key().address)
        except Exception as e:
            if "no valid coinstake found" in str(e) and attempt < 9:
                advance_time_for_pos(node, seconds=60)
            else:
                raise


class BIP68_112_113Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            '-whitelist=noban@127.0.0.1',
            '-par=1',  # Use only one script thread to get the exact reject reason for testing
        ]]
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def create_self_transfer_from_utxo(self, input_tx):
        utxo = self.miniwallet.get_utxo(txid=input_tx.rehash(), mark_as_spent=False)
        tx = self.miniwallet.create_self_transfer(from_node=self.nodes[0], utxo_to_spend=utxo)['tx']
        return tx

    def create_bip112special(self, input, txversion):
        tx = self.create_self_transfer_from_utxo(input)
        tx.nVersion = txversion
        self.miniwallet.sign_tx(tx)
        tx.vin[0].scriptSig = CScript([-1, OP_CHECKSEQUENCEVERIFY, OP_DROP] + list(CScript(tx.vin[0].scriptSig)))
        return tx

    def create_bip112emptystack(self, input, txversion):
        tx = self.create_self_transfer_from_utxo(input)
        tx.nVersion = txversion
        self.miniwallet.sign_tx(tx)
        tx.vin[0].scriptSig = CScript([OP_CHECKSEQUENCEVERIFY] + list(CScript(tx.vin[0].scriptSig)))
        return tx

    def send_generic_input_tx(self):
        """Send a self-transfer from MiniWallet and return the tx object."""
        return self.miniwallet.send_self_transfer(from_node=self.nodes[0])['tx']

    def create_bip68txs(self, bip68inputs, txversion, locktime_delta=0):
        """Returns a list of bip68 transactions with different bits set."""
        txs = []
        assert len(bip68inputs) >= 16
        for i, (sdf, srhb, stf, srlb) in enumerate(product(*[[True, False]] * 4)):
            locktime = relative_locktime(sdf, srhb, stf, srlb)
            tx = self.create_self_transfer_from_utxo(bip68inputs[i])
            tx.nVersion = txversion
            tx.vin[0].nSequence = locktime + locktime_delta
            self.miniwallet.sign_tx(tx)
            tx.rehash()
            txs.append({'tx': tx, 'sdf': sdf, 'stf': stf})

        return txs

    def create_bip112txs(self, bip112inputs, varyOP_CSV, txversion, locktime_delta=0):
        """Returns a list of bip68 transactions with different bits set."""
        txs = []
        assert len(bip112inputs) >= 16
        for i, (sdf, srhb, stf, srlb) in enumerate(product(*[[True, False]] * 4)):
            locktime = relative_locktime(sdf, srhb, stf, srlb)
            tx = self.create_self_transfer_from_utxo(bip112inputs[i])
            if (varyOP_CSV):  # if varying OP_CSV, nSequence is fixed
                tx.vin[0].nSequence = BASE_RELATIVE_LOCKTIME + locktime_delta
            else:  # vary nSequence instead, OP_CSV is fixed
                tx.vin[0].nSequence = locktime + locktime_delta
            tx.nVersion = txversion
            self.miniwallet.sign_tx(tx)
            if (varyOP_CSV):
                tx.vin[0].scriptSig = CScript([locktime, OP_CHECKSEQUENCEVERIFY, OP_DROP] + list(CScript(tx.vin[0].scriptSig)))
            else:
                tx.vin[0].scriptSig = CScript([BASE_RELATIVE_LOCKTIME, OP_CHECKSEQUENCEVERIFY, OP_DROP] + list(CScript(tx.vin[0].scriptSig)))
            tx.rehash()
            txs.append({'tx': tx, 'sdf': sdf, 'stf': stf})
        return txs

    def generate_blocks(self, number):
        """Generate PoS blocks with ~600s spacing, excluding mempool transactions.

        Uses getblocktemplate + P2P submission (same as create_test_block) so that
        stale test txs in the mempool don't get mined into these blocks."""
        for _ in range(number):
            block = self.create_test_block([])
            self.send_blocks([block])
            self.tipheight += 1
        node = self.nodes[0]
        self.tip = int(node.getbestblockhash(), 16)
        self.last_block_time = node.getblockheader(node.getbestblockhash())['time']

    def create_test_block(self, txs):
        """Create a PoS test block using getblocktemplate with specified test transactions."""
        node = self.nodes[0]
        advance_time_for_pos(node, seconds=600)

        # Get template with valid coinstake (retry if needed)
        for attempt in range(10):
            try:
                tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < 9:
                    advance_time_for_pos(node, seconds=60)
                else:
                    raise

        # Strip template to coinstake only (avoid mempool conflicts after invalidateblock)
        tmpl['transactions'] = [tmpl['transactions'][0]]

        # Build block: create_block handles coinbase + coinstake from template, txlist adds test txs
        block = create_block(tmpl=tmpl, txlist=txs)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        block.solve()

        # Sign the PoS block - extract key from coinstake (following p2p_filter.py pattern)
        coinstake = block.vtx[1]
        decoded_tx = node.decoderawtransaction(coinstake.serialize().hex())
        signing_key = None
        try:
            script_pubkey = decoded_tx['vout'][1]['scriptPubKey']
            coinstake_address = script_pubkey.get('address')
            if not coinstake_address:
                addresses = script_pubkey.get('addresses', [])
                if addresses:
                    coinstake_address = addresses[0]
            if not coinstake_address and script_pubkey.get('type') == 'pubkey':
                asm = script_pubkey.get('asm', '')
                parts = asm.split()
                if len(parts) >= 1 and parts[0] != 'OP_CHECKSIG':
                    pubkey_hex = parts[0]
                    coinstake_address = key_to_p2pkh(pubkey_hex, main=False)
            if coinstake_address:
                signing_key = node.dumpprivkey(coinstake_address)
        except Exception:
            pass

        if not signing_key:
            signing_key = node.get_deterministic_priv_key().key

        sign_block(block, signing_key)
        return block

    def send_blocks(self, blocks, success=True, reject_reason=None):
        """Sends blocks to test node. Syncs and verifies that tip has advanced to most recent block.

        Call with success = False if the tip shouldn't advance to the most recent block."""
        self.helper_peer.send_blocks_and_test(blocks, self.nodes[0], success=success, reject_reason=reject_reason)

    def run_test(self):
        self.helper_peer = self.nodes[0].add_p2p_connection(P2PDataStore())
        self.miniwallet = MiniWallet(self.nodes[0], mode=MiniWalletMode.RAW_P2PK)
        node = self.nodes[0]

        self.log.info("Generate initial PoS chain and fund MiniWallet.")
        # Generate 410 blocks to node wallet (enough for mature coins + sustained PoS staking)
        # node.generate() uses 60s spacing internally - sufficient for initial chain building
        node.generate(410)
        self.tipheight = node.getblockcount()
        self.tip = int(node.getbestblockhash(), 16)
        self.last_block_time = node.getblockheader(node.getbestblockhash())['time']

        # Fund MiniWallet with TESTING_TX_COUNT P2PK UTXOs via fundrawtransaction
        # (replaces Bitcoin's miniwallet.generate(83) coinbase-based funding,
        #  which doesn't work because PoS coinbase reward is 0)
        self.log.info("Fund MiniWallet with %d UTXOs." % TESTING_TX_COUNT)
        funding_tx = CTransaction()
        funding_tx.nVersion = 2
        funding_tx.nTime = self.last_block_time
        for _ in range(TESTING_TX_COUNT):
            funding_tx.vout.append(CTxOut(1 * COIN, self.miniwallet._scriptPubKey))
        funded = node.fundrawtransaction(funding_tx.serialize().hex())
        signed = node.signrawtransactionwithwallet(funded['hex'])
        txid = node.sendrawtransaction(signed['hex'])
        self.miniwallet.scan_tx(node.getrawtransaction(txid, True))

        # Mine funding tx
        _generate_to_address_with_retry(node, seconds=600)
        self.tipheight = node.getblockcount()
        self.tip = int(node.getbestblockhash(), 16)
        self.last_block_time = node.getblockheader(node.getbestblockhash())['time']

        # Generate remaining blocks to reach CSV_ACTIVATION_HEIGHT - 5 with 600s spacing
        # (BIP68 time-based tests need ~600s block spacing for correct delta calculations)
        remaining = CSV_ACTIVATION_HEIGHT - 5 - self.tipheight
        assert remaining > 0, "Initial chain too long: height %d >= %d" % (self.tipheight, CSV_ACTIVATION_HEIGHT - 5)
        self.log.info("Generate %d blocks with 600s spacing to reach height %d." % (remaining, CSV_ACTIVATION_HEIGHT - 5))
        self.generate_blocks(remaining)
        assert not softfork_active(node, 'csv')

        # Inputs at height = CSV_ACTIVATION_HEIGHT - 4 (= 428)
        #
        # Put inputs for all tests in the chain (time increases by ~600s per block)
        # Note we reuse inputs for v1 and v2 txs so must test these separately
        # 16 normal inputs
        bip68inputs = []
        for _ in range(16):
            bip68inputs.append(self.send_generic_input_tx())

        # 2 sets of 16 inputs with 10 OP_CSV OP_DROP (actually will be prepended to spending scriptSig)
        bip112basicinputs = []
        for _ in range(2):
            inputs = []
            for _ in range(16):
                inputs.append(self.send_generic_input_tx())
            bip112basicinputs.append(inputs)

        # 2 sets of 16 varied inputs with (relative_lock_time) OP_CSV OP_DROP (actually will be prepended to spending scriptSig)
        bip112diverseinputs = []
        for _ in range(2):
            inputs = []
            for _ in range(16):
                inputs.append(self.send_generic_input_tx())
            bip112diverseinputs.append(inputs)

        # 1 special input with -1 OP_CSV OP_DROP (actually will be prepended to spending scriptSig)
        bip112specialinput = self.send_generic_input_tx()
        # 1 special input with (empty stack) OP_CSV (actually will be prepended to spending scriptSig)
        bip112emptystackinput = self.send_generic_input_tx()

        # 1 normal input
        bip113input = self.send_generic_input_tx()

        # Mine 1 block for inputs to be in chain
        inputblockhash = _generate_to_address_with_retry(node, seconds=600)[0]
        self.tip = int(inputblockhash, 16)
        self.tipheight += 1
        self.last_block_time = node.getblockheader(inputblockhash)['time']
        # PoS blocks have coinbase + coinstake + regular txs
        assert_equal(len(node.getblock(inputblockhash, True)["tx"]), TESTING_TX_COUNT + 2)

        # 2 more blocks
        self.generate_blocks(2)

        assert_equal(self.tipheight, CSV_ACTIVATION_HEIGHT - 2)
        self.log.info("Height = {}, CSV not yet active (will activate for block {}, not {})".format(self.tipheight, CSV_ACTIVATION_HEIGHT, CSV_ACTIVATION_HEIGHT - 1))
        assert not softfork_active(node, 'csv')

        # Test both version 1 and version 2 (3 for BIP68) transactions for all tests
        # BIP113 test transaction will be modified before each use to put in appropriate block time
        bip113tx_v1 = self.create_self_transfer_from_utxo(bip113input)
        bip113tx_v1.vin[0].nSequence = 0xFFFFFFFE
        bip113tx_v1.nVersion = 1
        bip113tx_v2 = self.create_self_transfer_from_utxo(bip113input)
        bip113tx_v2.vin[0].nSequence = 0xFFFFFFFE
        bip113tx_v2.nVersion = 2

        # For BIP68 test all 16 relative sequence locktimes
        bip68txs_v1 = self.create_bip68txs(bip68inputs, 1)
        # ReddCoin: BIP68 requires nVersion >= 3 (not >= 2 as in Bitcoin)
        # nVersion=2 in ReddCoin means "PoS-aware tx with nTime field" but does NOT trigger BIP68
        bip68txs_v2 = self.create_bip68txs(bip68inputs, 3)

        # For BIP112 test:
        # 16 relative sequence locktimes of 10 against 10 OP_CSV OP_DROP inputs
        bip112txs_vary_nSequence_v1 = self.create_bip112txs(bip112basicinputs[0], False, 1)
        # ReddCoin: BIP112 OP_CSV also requires nVersion >= 3 (matches BIP68)
        bip112txs_vary_nSequence_v2 = self.create_bip112txs(bip112basicinputs[0], False, 3)
        # 16 relative sequence locktimes of 9 against 10 OP_CSV OP_DROP inputs
        bip112txs_vary_nSequence_9_v1 = self.create_bip112txs(bip112basicinputs[1], False, 1, -1)
        bip112txs_vary_nSequence_9_v2 = self.create_bip112txs(bip112basicinputs[1], False, 3, -1)
        # sequence lock time of 10 against 16 (relative_lock_time) OP_CSV OP_DROP inputs
        bip112txs_vary_OP_CSV_v1 = self.create_bip112txs(bip112diverseinputs[0], True, 1)
        bip112txs_vary_OP_CSV_v2 = self.create_bip112txs(bip112diverseinputs[0], True, 3)
        # sequence lock time of 9 against 16 (relative_lock_time) OP_CSV OP_DROP inputs
        bip112txs_vary_OP_CSV_9_v1 = self.create_bip112txs(bip112diverseinputs[1], True, 1, -1)
        bip112txs_vary_OP_CSV_9_v2 = self.create_bip112txs(bip112diverseinputs[1], True, 3, -1)
        # -1 OP_CSV OP_DROP input
        bip112tx_special_v1 = self.create_bip112special(bip112specialinput, 1)
        bip112tx_special_v2 = self.create_bip112special(bip112specialinput, 3)
        # (empty stack) OP_CSV input
        bip112tx_emptystack_v1 = self.create_bip112emptystack(bip112emptystackinput, 1)
        bip112tx_emptystack_v2 = self.create_bip112emptystack(bip112emptystackinput, 3)

        self.log.info("TESTING")

        self.log.info("Pre-Soft Fork Tests. All txs should pass.")
        self.log.info("Test version 1 txs")

        success_txs = []
        # BIP113 tx, -1 CSV tx and empty stack CSV tx should succeed
        # Use actual MTP from node (PoS block times may vary slightly from 600s spacing)
        tip_mtp = node.getblockheader(node.getbestblockhash())['mediantime']
        bip113tx_v1.nLockTime = tip_mtp  # = MTP of prior block (not <) but < time put on current block
        self.miniwallet.sign_tx(bip113tx_v1)
        success_txs.append(bip113tx_v1)
        success_txs.append(bip112tx_special_v1)
        success_txs.append(bip112tx_emptystack_v1)
        # add BIP 68 txs
        success_txs.extend(all_rlt_txs(bip68txs_v1))
        # add BIP 112 with seq=10 txs
        success_txs.extend(all_rlt_txs(bip112txs_vary_nSequence_v1))
        success_txs.extend(all_rlt_txs(bip112txs_vary_OP_CSV_v1))
        # try BIP 112 with seq=9 txs
        success_txs.extend(all_rlt_txs(bip112txs_vary_nSequence_9_v1))
        success_txs.extend(all_rlt_txs(bip112txs_vary_OP_CSV_9_v1))
        self.send_blocks([self.create_test_block(success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        self.log.info("Test version 2 txs")

        success_txs = []
        # BIP113 tx, -1 CSV tx and empty stack CSV tx should succeed
        tip_mtp = node.getblockheader(node.getbestblockhash())['mediantime']
        bip113tx_v2.nLockTime = tip_mtp
        self.miniwallet.sign_tx(bip113tx_v2)
        success_txs.append(bip113tx_v2)
        success_txs.append(bip112tx_special_v2)
        success_txs.append(bip112tx_emptystack_v2)
        # add BIP 68 txs
        success_txs.extend(all_rlt_txs(bip68txs_v2))
        # add BIP 112 with seq=10 txs
        success_txs.extend(all_rlt_txs(bip112txs_vary_nSequence_v2))
        success_txs.extend(all_rlt_txs(bip112txs_vary_OP_CSV_v2))
        # try BIP 112 with seq=9 txs
        success_txs.extend(all_rlt_txs(bip112txs_vary_nSequence_9_v2))
        success_txs.extend(all_rlt_txs(bip112txs_vary_OP_CSV_9_v2))
        self.send_blocks([self.create_test_block(success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        # 1 more block so the fork should now be active for the next block
        assert not softfork_active(node, 'csv')
        self.generate_blocks(1)
        assert softfork_active(node, 'csv')

        self.log.info("Post-Soft Fork Tests.")

        self.log.info("BIP 113 tests")
        # BIP 113 tests should now fail regardless of version number if nLockTime isn't satisfied by new rules
        tip_mtp = node.getblockheader(node.getbestblockhash())['mediantime']
        bip113tx_v1.nLockTime = tip_mtp  # = MTP of prior block (not <) but < time put on current block
        self.miniwallet.sign_tx(bip113tx_v1)
        bip113tx_v1.rehash()
        bip113tx_v2.nLockTime = tip_mtp
        self.miniwallet.sign_tx(bip113tx_v2)
        bip113tx_v2.rehash()
        for bip113tx in [bip113tx_v1, bip113tx_v2]:
            self.send_blocks([self.create_test_block([bip113tx])], success=False, reject_reason='bad-txns-nonfinal')

        # BIP 113 tests should now pass if the locktime is < MTP
        tip_mtp = node.getblockheader(node.getbestblockhash())['mediantime']
        bip113tx_v1.nLockTime = tip_mtp - 1  # < MTP of prior block
        self.miniwallet.sign_tx(bip113tx_v1)
        bip113tx_v1.rehash()
        bip113tx_v2.nLockTime = tip_mtp - 1
        self.miniwallet.sign_tx(bip113tx_v2)
        bip113tx_v2.rehash()
        for bip113tx in [bip113tx_v1, bip113tx_v2]:
            self.send_blocks([self.create_test_block([bip113tx])])
            self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        # Next height after 4 blocks of random version
        self.generate_blocks(4)

        self.log.info("BIP 68 tests")
        self.log.info("Test version 1 txs - all should still pass")

        success_txs = []
        success_txs.extend(all_rlt_txs(bip68txs_v1))
        self.send_blocks([self.create_test_block(success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        self.log.info("Test version 2 txs")

        # All txs with SEQUENCE_LOCKTIME_DISABLE_FLAG set pass
        bip68success_txs = [tx['tx'] for tx in bip68txs_v2 if tx['sdf']]
        self.send_blocks([self.create_test_block(bip68success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        # All txs without flag fail as we are at delta height = 8 < 10 and delta time = 8 * 600 < 10 * 512
        bip68timetxs = [tx['tx'] for tx in bip68txs_v2 if not tx['sdf'] and tx['stf']]
        for tx in bip68timetxs:
            self.send_blocks([self.create_test_block([tx])], success=False, reject_reason='bad-txns-nonfinal')

        bip68heighttxs = [tx['tx'] for tx in bip68txs_v2 if not tx['sdf'] and not tx['stf']]
        for tx in bip68heighttxs:
            self.send_blocks([self.create_test_block([tx])], success=False, reject_reason='bad-txns-nonfinal')

        # Advance one block
        self.generate_blocks(1)

        # Height txs should fail and time txs should now pass 9 * 600 > 10 * 512
        bip68success_txs.extend(bip68timetxs)
        self.send_blocks([self.create_test_block(bip68success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
        for tx in bip68heighttxs:
            self.send_blocks([self.create_test_block([tx])], success=False, reject_reason='bad-txns-nonfinal')

        # Advance one block
        self.generate_blocks(1)

        # All BIP 68 txs should pass
        bip68success_txs.extend(bip68heighttxs)
        self.send_blocks([self.create_test_block(bip68success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        self.log.info("BIP 112 tests")
        self.log.info("Test version 1 txs")

        # -1 OP_CSV tx and (empty stack) OP_CSV tx should fail
        self.send_blocks([self.create_test_block([bip112tx_special_v1])], success=False,
                         reject_reason='non-mandatory-script-verify-flag (Negative locktime)')
        self.send_blocks([self.create_test_block([bip112tx_emptystack_v1])], success=False,
                         reject_reason='non-mandatory-script-verify-flag (Operation not valid with the current stack size)')
        # If SEQUENCE_LOCKTIME_DISABLE_FLAG is set in argument to OP_CSV, version 1 txs should still pass

        success_txs = [tx['tx'] for tx in bip112txs_vary_OP_CSV_v1 if tx['sdf']]
        success_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_9_v1 if tx['sdf']]
        self.send_blocks([self.create_test_block(success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        # If SEQUENCE_LOCKTIME_DISABLE_FLAG is unset in argument to OP_CSV, version 1 txs should now fail
        fail_txs = all_rlt_txs(bip112txs_vary_nSequence_v1)
        fail_txs += all_rlt_txs(bip112txs_vary_nSequence_9_v1)
        fail_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_v1 if not tx['sdf']]
        fail_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_9_v1 if not tx['sdf']]
        for tx in fail_txs:
            self.send_blocks([self.create_test_block([tx])], success=False,
                             reject_reason='non-mandatory-script-verify-flag (Locktime requirement not satisfied)')

        self.log.info("Test version 2 txs")

        # -1 OP_CSV tx and (empty stack) OP_CSV tx should fail
        self.send_blocks([self.create_test_block([bip112tx_special_v2])], success=False,
                         reject_reason='non-mandatory-script-verify-flag (Negative locktime)')
        self.send_blocks([self.create_test_block([bip112tx_emptystack_v2])], success=False,
                         reject_reason='non-mandatory-script-verify-flag (Operation not valid with the current stack size)')

        # If SEQUENCE_LOCKTIME_DISABLE_FLAG is set in argument to OP_CSV, version 2 txs should pass (all sequence locks are met)
        success_txs = [tx['tx'] for tx in bip112txs_vary_OP_CSV_v2 if tx['sdf']]
        success_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_9_v2 if tx['sdf']]

        self.send_blocks([self.create_test_block(success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        # SEQUENCE_LOCKTIME_DISABLE_FLAG is unset in argument to OP_CSV for all remaining txs ##

        # All txs with nSequence 9 should fail either due to earlier mismatch or failing the CSV check
        fail_txs = all_rlt_txs(bip112txs_vary_nSequence_9_v2)
        fail_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_9_v2 if not tx['sdf']]
        for tx in fail_txs:
            self.send_blocks([self.create_test_block([tx])], success=False,
                             reject_reason='non-mandatory-script-verify-flag (Locktime requirement not satisfied)')

        # If SEQUENCE_LOCKTIME_DISABLE_FLAG is set in nSequence, tx should fail
        fail_txs = [tx['tx'] for tx in bip112txs_vary_nSequence_v2 if tx['sdf']]
        for tx in fail_txs:
            self.send_blocks([self.create_test_block([tx])], success=False,
                             reject_reason='non-mandatory-script-verify-flag (Locktime requirement not satisfied)')

        # If sequencelock types mismatch, tx should fail
        fail_txs = [tx['tx'] for tx in bip112txs_vary_nSequence_v2 if not tx['sdf'] and tx['stf']]
        fail_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_v2 if not tx['sdf'] and tx['stf']]
        for tx in fail_txs:
            self.send_blocks([self.create_test_block([tx])], success=False,
                             reject_reason='non-mandatory-script-verify-flag (Locktime requirement not satisfied)')

        # Remaining txs should pass, just test masking works properly
        success_txs = [tx['tx'] for tx in bip112txs_vary_nSequence_v2 if not tx['sdf'] and not tx['stf']]
        success_txs += [tx['tx'] for tx in bip112txs_vary_OP_CSV_v2 if not tx['sdf'] and not tx['stf']]
        self.send_blocks([self.create_test_block(success_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

        # Additional test, of checking that comparison of two time types works properly
        time_txs = []
        for tx in [tx['tx'] for tx in bip112txs_vary_OP_CSV_v2 if not tx['sdf'] and tx['stf']]:
            tx.vin[0].nSequence = BASE_RELATIVE_LOCKTIME | SEQ_TYPE_FLAG
            self.miniwallet.sign_tx(tx)
            tx.rehash()
            time_txs.append(tx)

        self.send_blocks([self.create_test_block(time_txs)])
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())

if __name__ == '__main__':
    BIP68_112_113Test().main()
