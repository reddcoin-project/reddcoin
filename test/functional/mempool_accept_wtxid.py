#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test mempool acceptance in case of an already known transaction
with identical non-witness data different witness.
"""

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    sha256,
)
from test_framework.p2p import P2PTxInvStore
from test_framework.script import (
    CScript,
    OP_0,
    OP_ELSE,
    OP_ENDIF,
    OP_EQUAL,
    OP_HASH160,
    OP_IF,
    OP_TRUE,
    hash160,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
)

class MempoolWtxidTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Whitelist test connections to:
        # 1. Bypass 5s inbound trickle relay delay for faster test execution
        # 2. Prevent timeout disconnections caused by mocktime/realtime mismatch in net.cpp
        self.extra_args = [['-whitelist=127.0.0.1']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info('Start with empty mempool and generate blocks')
        # ReddCoin: Generate PoW blocks (0-89), then advance time for PoS
        node.generate(90)  # PoW blocks 0-89

        # Advance time to age coins for PoS staking
        advance_time_for_pos(node, seconds=600)

        # Generate PoS blocks to mature coinbases (need 100 confirmations)
        # Blocks 90-101 (12 PoS blocks)
        node.generate(12)

        # ReddCoin: Use block 2 instead of block 0 (blocks 0-1 coinbases are unspendable)
        # Block 2 now has 100 confirmations (at height 101, block 2 is at height 2)
        blockhash = node.getblockhash(2)
        txid = node.getblock(blockhash=blockhash, verbosity=2)["tx"][0]["txid"]
        assert_equal(node.getmempoolinfo()['size'], 0)

        self.log.info("Submit parent with multiple script branches to mempool")
        hashlock = hash160(b'Preimage')
        witness_script = CScript([OP_IF, OP_HASH160, hashlock, OP_EQUAL, OP_ELSE, OP_TRUE, OP_ENDIF])
        witness_program = sha256(witness_script)
        script_pubkey = CScript([OP_0, witness_program])

        parent = CTransaction()
        parent.vin.append(CTxIn(COutPoint(int(txid, 16), 0), b""))
        parent.vout.append(CTxOut(int(9.9990 * COIN), script_pubkey))
        parent.rehash()

        privkeys = [node.get_deterministic_priv_key().key]
        raw_parent = node.signrawtransactionwithkey(hexstring=parent.serialize().hex(), privkeys=privkeys)['hex']
        parent_txid = node.sendrawtransaction(hexstring=raw_parent, maxfeerate=0)
        node.generate(1)

        peer_wtxid_relay = node.add_p2p_connection(P2PTxInvStore())

        # Create a new transaction with witness solving first branch
        child_witness_script = CScript([OP_TRUE])
        child_witness_program = sha256(child_witness_script)
        child_script_pubkey = CScript([OP_0, child_witness_program])

        child_one = CTransaction()
        child_one.vin.append(CTxIn(COutPoint(int(parent_txid, 16), 0), b""))
        child_one.vout.append(CTxOut(int(9.9980 * COIN), child_script_pubkey))
        child_one.wit.vtxinwit.append(CTxInWitness())
        child_one.wit.vtxinwit[0].scriptWitness.stack = [b'Preimage', b'\x01', witness_script]
        child_one_wtxid = child_one.getwtxid()
        child_one_txid = child_one.rehash()

        # Create another identical transaction with witness solving second branch
        child_two = CTransaction()
        child_two.vin.append(CTxIn(COutPoint(int(parent_txid, 16), 0), b""))
        child_two.vout.append(CTxOut(int(9.9980 * COIN), child_script_pubkey))
        child_two.wit.vtxinwit.append(CTxInWitness())
        child_two.wit.vtxinwit[0].scriptWitness.stack = [b'', witness_script]
        child_two_wtxid = child_two.getwtxid()
        child_two_txid = child_two.rehash()

        assert_equal(child_one_txid, child_two_txid)
        assert child_one_wtxid != child_two_wtxid

        self.log.info("Submit child_one to the mempool")
        txid_submitted = node.sendrawtransaction(child_one.serialize().hex())
        assert_equal(node.getrawmempool(True)[txid_submitted]['wtxid'], child_one_wtxid)

        peer_wtxid_relay.wait_for_broadcast([child_one_wtxid])
        assert_equal(node.getmempoolinfo()["unbroadcastcount"], 0)

        # testmempoolaccept reports the "already in mempool" error
        assert_equal(node.testmempoolaccept([child_one.serialize().hex()]), [{
            "txid": child_one_txid,
            "wtxid": child_one_wtxid,
            "allowed": False,
            "reject-reason": "txn-already-in-mempool"
        }])
        testres_child_two = node.testmempoolaccept([child_two.serialize().hex()])[0]
        assert_equal(testres_child_two, {
            "txid": child_two_txid,
            "wtxid": child_two_wtxid,
            "allowed": False,
            "reject-reason": "txn-same-nonwitness-data-in-mempool"
        })

        # sendrawtransaction will not throw but quits early when the exact same transaction is already in mempool
        node.sendrawtransaction(child_one.serialize().hex())

        self.log.info("Connect another peer that hasn't seen child_one before")
        peer_wtxid_relay_2 = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Submit child_two to the mempool")
        # sendrawtransaction will not throw but quits early when a transaction with the same non-witness data is already in mempool
        node.sendrawtransaction(child_two.serialize().hex())

        # The node should rebroadcast the transaction using the wtxid of the correct transaction
        # (child_one, which is in its mempool).
        peer_wtxid_relay_2.wait_for_broadcast([child_one_wtxid])
        assert_equal(node.getmempoolinfo()["unbroadcastcount"], 0)

if __name__ == '__main__':
    MempoolWtxidTest().main()
