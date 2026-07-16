#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test node responses to invalid transactions.

In this test we connect to one node over p2p, and test tx requests."""
from io import BytesIO

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
)
from test_framework.p2p import P2PDataStore
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_equal,
)
from data import invalid_txs


class InvalidTxRequestTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [[
            "-acceptnonstdtxn=1",
            # Note: No -whitelist to allow expected peer disconnections
            # This test validates that invalid tx types cause disconnects
        ]]
        # Use cache with 199 blocks of mature coins for PoS
        self.setup_clean_chain = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def bootstrap_p2p(self, *, num_connections=1):
        """Add a P2P connection to the node.

        Helper to connect and wait for version handshake."""
        for _ in range(num_connections):
            self.nodes[0].add_p2p_connection(P2PDataStore())

    def reconnect_p2p(self, **kwargs):
        """Tear down and bootstrap the P2P connection to the node.

        The node gets disconnected several times in this test. This helper
        method reconnects the p2p and restarts the network thread."""
        self.nodes[0].disconnect_p2ps()
        self.bootstrap_p2p(**kwargs)

    def run_test(self):
        node = self.nodes[0]  # convenience reference to the node

        self.bootstrap_p2p()  # Add one p2p connection to the node

        # ReddCoin: Use cache with mature coins instead of manual block creation
        # Create a funding transaction with an anyone-can-spend output
        self.log.info("Create a funding transaction with anyone-can-spend output.")

        # Anyone-can-spend script (OP_TRUE = 0x51)
        # Templates will spend this with empty scriptSig
        SCRIPT_PUB_KEY_OP_TRUE = CScript([OP_TRUE])

        # Get a UTXO from the wallet - find one close to 50 COIN if possible
        utxos = node.listunspent()
        funding_value = int(50 * COIN)  # 50 RDD to match original test
        fee = int(0.001 * COIN)  # ReddCoin: Minimum relay fee (100000 satoshis)

        # Find a UTXO that's >= funding_value + fee
        utxo = None
        for u in utxos:
            if int(u["amount"] * COIN) >= funding_value + fee:
                utxo = u
                break
        assert utxo is not None, "No suitable UTXO found"

        # First, send the exact amount we need to a wallet address
        # This creates a UTXO of exactly 50.01 COIN that we can use
        intermediate_addr = node.getnewaddress()
        intermediate_amount = (funding_value + fee) / COIN
        intermediate_txid = node.sendtoaddress(intermediate_addr, intermediate_amount)

        # Mine to confirm
        advance_time_for_pos(node, seconds=600)
        for attempt in range(10):
            try:
                node.generate(1)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e):
                    advance_time_for_pos(node, seconds=120)
                else:
                    raise

        # Find the intermediate UTXO (match txid AND amount to avoid grabbing change output)
        utxos = node.listunspent()
        utxo = None
        for u in utxos:
            if u["txid"] == intermediate_txid and u["address"] == intermediate_addr:
                utxo = u
                break
        assert utxo is not None, "Intermediate UTXO not found"

        # Create a raw transaction with ONLY the OP_TRUE output (no change)
        # Use createrawtransaction then modify the output scriptPubKey
        # signrawtransactionwithwallet will sign the input (which is standard)
        # -acceptnonstdtxn=1 allows the non-standard output
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        # Use a dummy address, we'll replace the output
        dummy_addr = node.getnewaddress()
        outputs = {dummy_addr: funding_value / COIN}  # Only one output, fee is implicit
        raw_tx = node.createrawtransaction(inputs, outputs)

        # Parse and modify to use OP_TRUE output
        funding_tx = CTransaction()
        funding_tx.deserialize(BytesIO(bytes.fromhex(raw_tx)))
        funding_tx.vout[0].scriptPubKey = SCRIPT_PUB_KEY_OP_TRUE
        funding_tx.vout[0].nValue = funding_value

        # Sign the input (wallet doesn't care about output script)
        signed = node.signrawtransactionwithwallet(funding_tx.serialize().hex())
        assert signed["complete"], f"Failed to sign: {signed.get('errors', [])}"

        # Parse the signed transaction
        funding_tx = CTransaction()
        funding_tx.deserialize(BytesIO(bytes.fromhex(signed["hex"])))
        funding_tx.calc_sha256()

        # Broadcast and confirm
        node.sendrawtransaction(signed["hex"])

        # ReddCoin: PoS block generation with retry logic
        advance_time_for_pos(node, seconds=600)  # Age coins
        for attempt in range(10):
            try:
                node.generate(1)
                break
            except Exception as e:
                if "no valid coinstake found" in str(e):
                    advance_time_for_pos(node, seconds=120)
                else:
                    raise
        else:
            raise AssertionError("Could not generate PoS block after 10 attempts")

        # Create a mock block object for invalid_txs templates compatibility
        # The templates expect block.vtx[0] to be the spendable transaction
        class MockBlock:
            def __init__(self, tx):
                self.vtx = [tx]
        block1 = MockBlock(funding_tx)

        # Iterate through a list of known invalid transaction types, ensuring each is
        # rejected. Some are consensus invalid and some just violate policy.
        for BadTxTemplate in invalid_txs.iter_all_templates():
            self.log.info("Testing invalid transaction: %s", BadTxTemplate.__name__)
            template = BadTxTemplate(spend_block=block1)
            tx = template.get_tx()
            node.p2ps[0].send_txs_and_test(
                [tx], node, success=False,
                expect_disconnect=template.expect_disconnect,
                reject_reason=template.reject_reason,
            )

            if template.expect_disconnect:
                self.log.info("Reconnecting to peer")
                self.reconnect_p2p()

        # Make two p2p connections to provide the node with orphans
        # * p2ps[0] will send valid orphan txs (one with low fee)
        # * p2ps[1] will send an invalid orphan tx (and is later disconnected for that)
        self.reconnect_p2p(num_connections=2)

        self.log.info('Test orphan transaction handling ... ')
        # Create a root transaction that we withhold until all dependent transactions
        # are sent out and in the orphan cache
        # ReddCoin: Use version 2 transactions with nTime for PoS
        # ReddCoin: Fees increased 100x (12000 → 1200000 satoshis)
        SCRIPT_PUB_KEY_OP_TRUE_LONG = b'\x51\x75' * 15 + b'\x51'  # OP_TRUE OP_DROP × 15 + OP_TRUE
        mocktime = node.getblockheader(node.getbestblockhash())['time']
        tx_withhold = CTransaction()
        tx_withhold.nVersion = 2  # ReddCoin: PoS uses v2
        tx_withhold.nTime = mocktime  # ReddCoin: Required for v2 transactions
        tx_withhold.vin.append(CTxIn(outpoint=COutPoint(block1.vtx[0].sha256, 0)))
        tx_withhold.vout.append(CTxOut(nValue=50 * COIN - 1200000, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG))  # ReddCoin: 100x fee
        tx_withhold.calc_sha256()

        # Our first orphan tx with some outputs to create further orphan txs
        tx_orphan_1 = CTransaction()
        tx_orphan_1.nVersion = 2  # ReddCoin: PoS uses v2
        tx_orphan_1.nTime = mocktime  # ReddCoin: Required for v2 transactions
        tx_orphan_1.vin.append(CTxIn(outpoint=COutPoint(tx_withhold.sha256, 0)))
        tx_orphan_1.vout = [CTxOut(nValue=10 * COIN, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG)] * 3
        tx_orphan_1.calc_sha256()

        # A valid transaction with low fee
        tx_orphan_2_no_fee = CTransaction()
        tx_orphan_2_no_fee.nVersion = 2  # ReddCoin: PoS uses v2
        tx_orphan_2_no_fee.nTime = mocktime  # ReddCoin: Required for v2 transactions
        tx_orphan_2_no_fee.vin.append(CTxIn(outpoint=COutPoint(tx_orphan_1.sha256, 0)))
        tx_orphan_2_no_fee.vout.append(CTxOut(nValue=10 * COIN, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG))

        # A valid transaction with sufficient fee
        tx_orphan_2_valid = CTransaction()
        tx_orphan_2_valid.nVersion = 2  # ReddCoin: PoS uses v2
        tx_orphan_2_valid.nTime = mocktime  # ReddCoin: Required for v2 transactions
        tx_orphan_2_valid.vin.append(CTxIn(outpoint=COutPoint(tx_orphan_1.sha256, 1)))
        tx_orphan_2_valid.vout.append(CTxOut(nValue=10 * COIN - 1200000, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG))  # ReddCoin: 100x fee
        tx_orphan_2_valid.calc_sha256()

        # An invalid transaction with negative fee
        tx_orphan_2_invalid = CTransaction()
        tx_orphan_2_invalid.nVersion = 2  # ReddCoin: PoS uses v2
        tx_orphan_2_invalid.nTime = mocktime  # ReddCoin: Required for v2 transactions
        tx_orphan_2_invalid.vin.append(CTxIn(outpoint=COutPoint(tx_orphan_1.sha256, 2)))
        tx_orphan_2_invalid.vout.append(CTxOut(nValue=11 * COIN, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG))
        tx_orphan_2_invalid.calc_sha256()

        self.log.info('Send the orphans ... ')
        # Send valid orphan txs from p2ps[0]
        node.p2ps[0].send_txs_and_test([tx_orphan_1, tx_orphan_2_no_fee, tx_orphan_2_valid], node, success=False)
        # Send invalid tx from p2ps[1]
        node.p2ps[1].send_txs_and_test([tx_orphan_2_invalid], node, success=False)

        assert_equal(0, node.getmempoolinfo()['size'])  # Mempool should be empty
        assert_equal(2, len(node.getpeerinfo()))  # p2ps[1] is still connected

        self.log.info('Send the withhold tx ... ')
        with node.assert_debug_log(expected_msgs=["bad-txns-in-belowout"]):
            node.p2ps[0].send_txs_and_test([tx_withhold], node, success=True)

        # Transactions that should end up in the mempool
        expected_mempool = {
            t.hash
            for t in [
                tx_withhold,  # The transaction that is the root for all orphans
                tx_orphan_1,  # The orphan transaction that splits the coins
                tx_orphan_2_valid,  # The valid transaction (with sufficient fee)
            ]
        }
        # Transactions that do not end up in the mempool
        # tx_orphan_no_fee, because it has too low fee (p2ps[0] is not disconnected for relaying that tx)
        # tx_orphan_invalid, because it has negative fee (p2ps[1] is disconnected for relaying that tx)

        self.wait_until(lambda: 1 == len(node.getpeerinfo()), timeout=12)  # p2ps[1] is no longer connected
        assert_equal(expected_mempool, set(node.getrawmempool()))

        self.log.info('Test orphan pool overflow')
        orphan_tx_pool = [CTransaction() for _ in range(101)]
        for i in range(len(orphan_tx_pool)):
            orphan_tx_pool[i].nVersion = 2  # ReddCoin: PoS uses v2
            orphan_tx_pool[i].nTime = mocktime  # ReddCoin: Required for v2 transactions
            orphan_tx_pool[i].vin.append(CTxIn(outpoint=COutPoint(i, 333)))
            orphan_tx_pool[i].vout.append(CTxOut(nValue=11 * COIN, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG))

        with node.assert_debug_log(['orphanage overflow, removed 1 tx']):
            node.p2ps[0].send_txs_and_test(orphan_tx_pool, node, success=False)

        rejected_parent = CTransaction()
        rejected_parent.nVersion = 2  # ReddCoin: PoS uses v2
        rejected_parent.nTime = mocktime  # ReddCoin: Required for v2 transactions
        rejected_parent.vin.append(CTxIn(outpoint=COutPoint(tx_orphan_2_invalid.sha256, 0)))
        rejected_parent.vout.append(CTxOut(nValue=11 * COIN, scriptPubKey=SCRIPT_PUB_KEY_OP_TRUE_LONG))
        rejected_parent.rehash()
        with node.assert_debug_log(['not keeping orphan with rejected parents {}'.format(rejected_parent.hash)]):
            node.p2ps[0].send_txs_and_test([rejected_parent], node, success=False)


if __name__ == '__main__':
    InvalidTxRequestTest().main()
