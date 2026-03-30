#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP65 (CHECKLOCKTIMEVERIFY).

Test that the CHECKLOCKTIMEVERIFY soft-fork activates via BIP9 signaling.
ReddCoin uses BIP9 for CLTV (bit 1), activating at height 433 on regtest
(window=144, 75% threshold, 3 periods from genesis).
"""

from test_framework.blocktools import (
    create_block,
    NORMAL_GBT_REQUEST_PARAMS,
    sign_block,
)
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.p2p import P2PInterface
from test_framework.script import (
    CScript,
    CScriptNum,
    OP_1NEGATE,
    OP_CHECKLOCKTIMEVERIFY,
    OP_DROP,
    OP_NOP,
    OP_TRUE,
)

# OP_TRUE with NOP padding to ensure spends meet minimum tx size (82 bytes)
OP_TRUE_SCRIPT = CScript([OP_TRUE] + [OP_NOP] * 50)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)

# BIP9 activation: window=144, threshold=108 (75%)
# Period 0 (0-143): DEFINED -> STARTED (block time > nStartTime=0)
# Period 1 (144-287): STARTED, blocks signal bit 1 -> LOCKED_IN at end
# Period 2 (288-431): LOCKED_IN -> State(tip=431) returns ACTIVE
# DeploymentActiveAt(432) = True (first block where CLTV is enforced)
# RPC reports active=True starting at height 431 (looks ahead to next period)
CLTV_HEIGHT = 432


# Helper function to modify a transaction by
# 1) prepending a given script to the scriptSig of vin 0 and
# 2) (optionally) modify the nSequence of vin 0 and the tx's nLockTime
def cltv_modify_tx(tx, prepend_scriptsig, nsequence=None, nlocktime=None):
    assert_equal(len(tx.vin), 1)
    if nsequence is not None:
        tx.vin[0].nSequence = nsequence
        tx.nLockTime = nlocktime

    tx.vin[0].scriptSig = CScript(prepend_scriptsig + list(CScript(tx.vin[0].scriptSig)))
    tx.rehash()


def cltv_invalidate(tx, failure_reason):
    # Modify the signature in vin 0 and nSequence/nLockTime of the tx to fail CLTV
    #
    # According to BIP65, OP_CHECKLOCKTIMEVERIFY can fail due the following reasons:
    # 1) the stack is empty
    # 2) the top item on the stack is less than 0
    # 3) the lock-time type (height vs. timestamp) of the top stack item and the
    #    nLockTime field are not the same
    # 4) the top stack item is greater than the transaction's nLockTime field
    # 5) the nSequence field of the txin is 0xffffffff
    assert failure_reason in range(5)
    # nLockTime values must be below the current block height for finality.
    # Use 400 (below CLTV_HEIGHT=433) instead of 500 to avoid bad-txns-nonfinal.
    scheme = [
        # | Script to prepend to scriptSig                  | nSequence  | nLockTime    |
        # +-------------------------------------------------+------------+--------------+
        [[OP_CHECKLOCKTIMEVERIFY],                            None,       None],
        [[OP_1NEGATE, OP_CHECKLOCKTIMEVERIFY, OP_DROP],       None,       None],
        [[CScriptNum(1000), OP_CHECKLOCKTIMEVERIFY, OP_DROP], 0,          1296688602],  # timestamp (type mismatch with height-based stack value)
        [[CScriptNum(1000), OP_CHECKLOCKTIMEVERIFY, OP_DROP], 0,          400],         # stack(1000) > nLockTime(400)
        [[CScriptNum(400),  OP_CHECKLOCKTIMEVERIFY, OP_DROP], 0xffffffff, 400],         # nSequence=max disables CLTV
    ][failure_reason]

    cltv_modify_tx(tx, prepend_scriptsig=scheme[0], nsequence=scheme[1], nlocktime=scheme[2])


def cltv_validate(tx, height):
    # Modify the signature in vin 0 and nSequence/nLockTime of the tx to pass CLTV
    scheme = [[CScriptNum(height), OP_CHECKLOCKTIMEVERIFY, OP_DROP], 0, height]

    cltv_modify_tx(tx, prepend_scriptsig=scheme[0], nsequence=scheme[1], nlocktime=scheme[2])


class BIP65Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [[
            '-whitelist=noban@127.0.0.1',
            '-par=1',  # Use only one script thread to get the exact reject reason for testing
            '-acceptnonstdtxn=1',  # cltv_invalidate is nonstandard
            '-staking=0',  # Prevent background staking from consuming UTXOs
        ]]
        self.rpc_timeout = 480

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def test_cltv_info(self, *, is_active):
        """Check CLTV deployment status via getblockchaininfo.
        ReddCoin uses BIP9 deployment (not buried), so the key is 'cltv' and format differs."""
        info = self.nodes[0].getblockchaininfo()['softforks']['cltv']
        assert_equal(info['active'], is_active)
        assert_equal(info['type'], 'bip9')

    def create_op_true_utxo(self, node, amount):
        """Create a UTXO with OP_TRUE scriptPubKey by spending from the node wallet."""
        utxo = node.listunspent()[0]
        fee = 0.01
        change_amount = float(utxo['amount']) - amount - fee
        change_addr = node.getnewaddress()

        funding_tx = CTransaction()
        funding_tx.nVersion = 1  # Use v1 to avoid nTime complications
        funding_tx.vin = [CTxIn(COutPoint(int(utxo['txid'], 16), utxo['vout']))]
        funding_tx.vout = [
            CTxOut(int(amount * COIN), OP_TRUE_SCRIPT),
        ]
        if change_amount > 0.01:
            change_script = bytes.fromhex(node.getaddressinfo(change_addr)['scriptPubKey'])
            funding_tx.vout.append(CTxOut(int(change_amount * COIN), CScript(change_script)))
        funding_tx.rehash()

        signed = node.signrawtransactionwithwallet(funding_tx.serialize().hex())
        assert signed['complete']
        txid = node.sendrawtransaction(signed['hex'], 0)
        return txid

    def create_op_true_spend(self, node, utxo_txid, utxo_vout, utxo_amount):
        """Create a transaction spending an OP_TRUE output (anyone-can-spend)."""
        fee = 0.01
        spend_tx = CTransaction()
        spend_tx.nVersion = 1  # Use v1 to avoid nTime
        spend_tx.vin = [CTxIn(COutPoint(int(utxo_txid, 16), utxo_vout), CScript([OP_TRUE]))]
        spend_tx.vout = [CTxOut(int((utxo_amount - fee) * COIN), OP_TRUE_SCRIPT)]
        spend_tx.rehash()
        return spend_tx

    def sign_block_with_coinstake_key(self, node, block):
        """Sign a PoS block using the coinstake's private key."""
        coinstake = block.vtx[1]
        decoded = node.decoderawtransaction(coinstake.serialize().hex())
        signing_key = None
        try:
            script_pubkey = decoded['vout'][1]['scriptPubKey']
            addr = script_pubkey.get('address')
            if not addr:
                addresses = script_pubkey.get('addresses', [])
                if addresses:
                    addr = addresses[0]
            if addr:
                signing_key = node.dumpprivkey(addr)
        except Exception:
            pass
        if not signing_key:
            signing_key = node.get_deterministic_priv_key().key
        sign_block(block, signing_key)

    def build_block_with_tx(self, node, tx):
        """Build a PoS block containing the given transaction via GBT."""
        advance_time_for_pos(node, seconds=120)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        block = create_block(tmpl=tmpl, txlist=[tx])
        block.hashMerkleRoot = block.calc_merkle_root()
        self.sign_block_with_coinstake_key(node, block)
        return block

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PInterface())

        self.test_cltv_info(is_active=False)

        self.log.info("Mining %d blocks to reach CLTV activation", CLTV_HEIGHT - 3 - node.getblockcount())
        blocks_needed = CLTV_HEIGHT - 3 - node.getblockcount()
        node.generate(blocks_needed)
        assert_equal(node.getblockcount(), CLTV_HEIGHT - 3)  # height 429

        # Create OP_TRUE UTXOs for testing (one per CLTV test case + extras for post-activation)
        self.log.info("Creating OP_TRUE UTXOs for CLTV testing")
        num_utxos = 12
        utxo_amount = 10.0
        op_true_txids = []
        for _ in range(num_utxos):
            txid = self.create_op_true_utxo(node, utxo_amount)
            op_true_txids.append(txid)
        node.generate(1)  # Confirm funding txs → height 430
        assert_equal(node.getblockcount(), CLTV_HEIGHT - 2)  # height 430

        # BIP9 state at height 430: LOCKED_IN (period 2: 288-431)
        # RPC reports active=False, consensus does not enforce CLTV
        self.test_cltv_info(is_active=False)

        self.log.info("Test that invalid-according-to-CLTV transactions can still appear in a block")
        utxo_idx = 0
        invalid_cltv_txs = []
        for i in range(5):
            spendtx = self.create_op_true_spend(node, op_true_txids[utxo_idx], 0, utxo_amount)
            cltv_invalidate(spendtx, i)
            invalid_cltv_txs.append(spendtx)
            utxo_idx += 1

        # Build a PoS block containing all 5 invalid CLTV txs
        advance_time_for_pos(node, seconds=120)
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        block = create_block(tmpl=tmpl, txlist=invalid_cltv_txs)
        block.hashMerkleRoot = block.calc_merkle_root()
        self.sign_block_with_coinstake_key(node, block)

        result = node.submitblock(block.serialize().hex())
        assert_equal(result, None)  # None means accepted
        assert_equal(node.getblockcount(), CLTV_HEIGHT - 1)  # height 431

        self.log.info("Block accepted with invalid-according-to-CLTV transactions (pre-enforcement)")

        # BIP9 state at height 431: ACTIVE (State looks ahead — period 3 starts at 432)
        # RPC reports active=True. Block 431 itself was NOT enforced, but next block (432) WILL be.
        self.test_cltv_info(is_active=True)

        self.log.info("Test that invalid-according-to-CLTV transactions cannot appear in a block")

        # create and test one invalid tx per CLTV failure reason (5 in total)
        for i in range(5):
            spendtx = self.create_op_true_spend(node, op_true_txids[utxo_idx], 0, utxo_amount)
            cltv_invalidate(spendtx, i)
            utxo_idx += 1

            expected_cltv_reject_reason = [
                "non-mandatory-script-verify-flag (Operation not valid with the current stack size)",
                "non-mandatory-script-verify-flag (Negative locktime)",
                "non-mandatory-script-verify-flag (Locktime requirement not satisfied)",
                "non-mandatory-script-verify-flag (Locktime requirement not satisfied)",
                "non-mandatory-script-verify-flag (Locktime requirement not satisfied)",
            ][i]
            # First we show that this tx is valid except for CLTV by getting it
            # rejected from the mempool for exactly that reason.
            assert_equal(
                [{
                    'txid': spendtx.hash,
                    'wtxid': spendtx.getwtxid(),
                    'allowed': False,
                    'reject-reason': expected_cltv_reject_reason,
                }],
                node.testmempoolaccept(rawtxs=[spendtx.serialize().hex()], maxfeerate=0),
            )

            # Now we verify that a block with this transaction is also invalid.
            block = self.build_block_with_tx(node, spendtx)
            prev_best = node.getbestblockhash()

            with node.assert_debug_log(expected_msgs=['CheckInputScripts on {} failed with {}'.format(
                                                      spendtx.hash, expected_cltv_reject_reason)]):
                node.submitblock(block.serialize().hex())
                assert_equal(node.getbestblockhash(), prev_best)

        self.log.info("Test that a block with a valid-according-to-CLTV transaction is accepted")
        spendtx = self.create_op_true_spend(node, op_true_txids[utxo_idx], 0, utxo_amount)
        cltv_validate(spendtx, CLTV_HEIGHT - 1)
        utxo_idx += 1

        # Use submitblock since OP_TRUE spends fail CLEANSTACK policy in mempool
        block = self.build_block_with_tx(node, spendtx)
        prev_best = node.getbestblockhash()
        result = node.submitblock(block.serialize().hex())
        assert_equal(result, None)  # None means accepted
        assert node.getbestblockhash() != prev_best  # Tip advanced

        self.test_cltv_info(is_active=True)
        self.log.info("Valid CLTV transaction accepted at height %d", node.getblockcount())


if __name__ == '__main__':
    BIP65Test().main()
