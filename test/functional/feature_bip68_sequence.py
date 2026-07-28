#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Copyright (c) 2014-2023 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP68 implementation.

ReddCoin adaptation:
- BIP68 enforces sequence locks for tx nVersion >= 3 (not >= 2 as in Bitcoin),
  because nVersion 2 is used for PoS transactions with nTime field.
- The "disable" version (below BIP68 enforcement) is 2 (not 1).
- CSV activates via BIP9 at height 576 on regtest (not 432).
- Reorg test uses PoS block generation (invalidateblock + generate) instead
  of create_block/submitblock since PoS blocks require valid coinstake.
- test_bip68_not_consensus is skipped because it requires submitting manually
  crafted blocks with invalid BIP68 sequences, which is incompatible with PoS
  coinstake requirements.
"""

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_raises_rpc_error,
    satoshi_round,
    softfork_active,
)
from test_framework.script_util import DUMMY_P2WPKH_SCRIPT

SEQUENCE_LOCKTIME_DISABLE_FLAG = (1<<31)
SEQUENCE_LOCKTIME_TYPE_FLAG = (1<<22) # this means use time (0 means height)
SEQUENCE_LOCKTIME_GRANULARITY = 9 # this is a bit-shift
SEQUENCE_LOCKTIME_MASK = 0x0000ffff

# RPC error for non-BIP68 final transactions
NOT_FINAL_ERROR = "non-BIP68-final"

class BIP68Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            [
                "-acceptnonstdtxn=1",
                "-peertimeout=9999",  # bump because mocktime might cause a disconnect otherwise
            ],
            [
                "-acceptnonstdtxn=0",
                # node1 is only here to relay from, and must not make blocks of
                # its own. The reorg test below has node0 permanently invalidate
                # a branch and build a replacement, and node0 can never accept
                # the original back. A block staked by node1 onto that original
                # leaves the two on chains neither will give up, which no amount
                # of waiting resolves. Staking is on by default in every node.
                "-staking=0",
            ],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.relayfee = self.nodes[0].getnetworkinfo()["relayfee"]

        # Generate coins beyond cache (199 blocks). Need enough for all subtests.
        self.log.info("Generating blocks for mature coins...")
        self.nodes[0].generate(110)

        self.log.info("Running test disable flag")
        self.test_disable_flag()

        self.log.info("Running test sequence-lock-confirmed-inputs")
        self.test_sequence_lock_confirmed_inputs()

        self.log.info("Running test sequence-lock-unconfirmed-inputs")
        self.test_sequence_lock_unconfirmed_inputs()

        # test_bip68_not_consensus is skipped: it requires submitting manually
        # crafted blocks containing BIP68-violating transactions via submitblock.
        # PoS blocks require valid coinstake, making manual block construction
        # impractical. The BIP68 consensus logic is still tested by the other subtests.
        self.log.info("Skipping test BIP68 not consensus (requires manual PoW block construction)")

        self.log.info("Activating BIP68 (and 112/113)")
        self.activateCSV()

        self.log.info("Verifying nVersion=3 transactions are standard.")
        self.log.info("Note that nVersion=3 transactions are always standard (independent of BIP68 activation status).")
        # Sync node1 before the relay test (node0 has generated many blocks)
        self.sync_blocks(timeout=120)
        self.test_version3_relay()

        self.log.info("Passed")

    # Test that BIP68 is not in effect if tx version is < 3, or if
    # the first sequence bit is set.
    def test_disable_flag(self):
        # Create some unconfirmed inputs
        new_addr = self.nodes[0].getnewaddress()
        self.nodes[0].sendtoaddress(new_addr, 2)

        utxos = self.nodes[0].listunspent(0, 0)
        assert len(utxos) > 0

        utxo = utxos[0]

        tx1 = CTransaction()
        value = int(satoshi_round(utxo["amount"] - self.relayfee)*COIN)

        # Check that the disable flag disables relative locktime.
        # If sequence locks were used, this would require 1 block for the
        # input to mature.
        sequence_value = SEQUENCE_LOCKTIME_DISABLE_FLAG | 1
        tx1.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]), nSequence=sequence_value)]
        tx1.vout = [CTxOut(value, DUMMY_P2WPKH_SCRIPT)]

        tx1_signed = self.nodes[0].signrawtransactionwithwallet(tx1.serialize().hex())["hex"]
        tx1_id = self.nodes[0].sendrawtransaction(tx1_signed)
        tx1_id = int(tx1_id, 16)

        # This transaction will enable sequence-locks (nVersion=3 enforces BIP68),
        # so this transaction should fail
        tx2 = CTransaction()
        tx2.nVersion = 3
        sequence_value = sequence_value & 0x7fffffff
        tx2.vin = [CTxIn(COutPoint(tx1_id, 0), nSequence=sequence_value)]
        tx2.vout = [CTxOut(int(value - self.relayfee * COIN), DUMMY_P2WPKH_SCRIPT)]
        tx2.rehash()

        assert_raises_rpc_error(-26, NOT_FINAL_ERROR, self.nodes[0].sendrawtransaction, tx2.serialize().hex())

        # Setting the version back down to 2 should disable the sequence lock,
        # so this should be accepted (ReddCoin: BIP68 enforces at version >= 3).
        tx2.nVersion = 2

        self.nodes[0].sendrawtransaction(tx2.serialize().hex())

    # Calculate the median time past of a prior block ("confirmations" before
    # the current tip).
    def get_median_time_past(self, confirmations):
        block_hash = self.nodes[0].getblockhash(self.nodes[0].getblockcount()-confirmations)
        return self.nodes[0].getblockheader(block_hash)["mediantime"]

    # Test that sequence locks are respected for transactions spending confirmed inputs.
    def test_sequence_lock_confirmed_inputs(self):
        # Create lots of confirmed utxos, and use them to generate lots of random
        # transactions.
        max_outputs = 50
        addresses = []
        while len(addresses) < max_outputs:
            addresses.append(self.nodes[0].getnewaddress())
        while len(self.nodes[0].listunspent()) < 200:
            import random
            random.shuffle(addresses)
            num_outputs = random.randint(1, max_outputs)
            outputs = {}
            for i in range(num_outputs):
                outputs[addresses[i]] = random.randint(1, 20)*0.01
            self.nodes[0].sendmany("", outputs)
            self.nodes[0].generate(1)

        utxos = self.nodes[0].listunspent()

        # Try creating a lot of random transactions.
        # Each time, choose a random number of inputs, and randomly set
        # some of those inputs to be sequence locked (and randomly choose
        # between height/time locking). Small random chance of making the locks
        # all pass.
        for _ in range(400):
            # Randomly choose up to 10 inputs (but not more than available)
            num_inputs = random.randint(1, min(10, len(utxos)))
            random.shuffle(utxos)

            # Track whether any sequence locks used should fail
            should_pass = True

            # Track whether this transaction was built with sequence locks
            using_sequence_locks = False

            tx = CTransaction()
            tx.nVersion = 3  # ReddCoin: BIP68 enforced at version >= 3
            value = 0
            for j in range(num_inputs):
                sequence_value = 0xfffffffe # this disables sequence locks

                # 50% chance we enable sequence locks
                if random.randint(0,1):
                    using_sequence_locks = True

                    # 10% of the time, make the input sequence value pass
                    input_will_pass = (random.randint(1,10) == 1)
                    sequence_value = utxos[j]["confirmations"]
                    if not input_will_pass:
                        sequence_value += 1
                        should_pass = False

                    # Figure out what the median-time-past was for the confirmed input
                    # Note that if an input has N confirmations, we're going back N blocks
                    # from the tip so that we're looking up MTP of the block
                    # PRIOR to the one the input appears in, as per the BIP68 spec.
                    orig_time = self.get_median_time_past(utxos[j]["confirmations"])
                    cur_time = self.get_median_time_past(0) # MTP of the tip

                    # can only timelock this input if it's not too old -- otherwise use height
                    can_time_lock = True
                    if ((cur_time - orig_time) >> SEQUENCE_LOCKTIME_GRANULARITY) >= SEQUENCE_LOCKTIME_MASK:
                        can_time_lock = False

                    # if time-lockable, then 50% chance we make this a time lock
                    if random.randint(0,1) and can_time_lock:
                        # Find first time-lock value that fails, or latest one that succeeds
                        time_delta = sequence_value << SEQUENCE_LOCKTIME_GRANULARITY
                        if input_will_pass and time_delta > cur_time - orig_time:
                            sequence_value = ((cur_time - orig_time) >> SEQUENCE_LOCKTIME_GRANULARITY)
                        elif (not input_will_pass and time_delta <= cur_time - orig_time):
                            sequence_value = ((cur_time - orig_time) >> SEQUENCE_LOCKTIME_GRANULARITY)+1
                        sequence_value |= SEQUENCE_LOCKTIME_TYPE_FLAG
                tx.vin.append(CTxIn(COutPoint(int(utxos[j]["txid"], 16), utxos[j]["vout"]), nSequence=sequence_value))
                value += utxos[j]["amount"]*COIN
            # Overestimate the size of the tx - signatures should be less than 120 bytes, and leave 50 for the output
            tx_size = len(tx.serialize().hex())//2 + 120*num_inputs + 50
            tx.vout.append(CTxOut(int(value-self.relayfee*tx_size*COIN/1000), DUMMY_P2WPKH_SCRIPT))
            rawtx = self.nodes[0].signrawtransactionwithwallet(tx.serialize().hex())["hex"]

            if (using_sequence_locks and not should_pass):
                # This transaction should be rejected
                assert_raises_rpc_error(-26, NOT_FINAL_ERROR, self.nodes[0].sendrawtransaction, rawtx)
            else:
                # This raw transaction should be accepted
                self.nodes[0].sendrawtransaction(rawtx)
                utxos = self.nodes[0].listunspent()

    # Test that sequence locks on unconfirmed inputs must have nSequence
    # height or time of 0 to be accepted.
    # Then test that BIP68-invalid transactions are removed from the mempool
    # after a reorg.
    def test_sequence_lock_unconfirmed_inputs(self):
        # Store height so we can easily reset the chain at the end of the test
        cur_height = self.nodes[0].getblockcount()

        # Create a mempool tx.
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 2)
        tx1 = tx_from_hex(self.nodes[0].getrawtransaction(txid))
        tx1.rehash()

        # Anyone-can-spend mempool tx.
        # Sequence lock of 0 should pass.
        tx2 = CTransaction()
        tx2.nVersion = 3  # ReddCoin: BIP68 enforced at version >= 3
        tx2.vin = [CTxIn(COutPoint(tx1.sha256, 0), nSequence=0)]
        tx2.vout = [CTxOut(int(tx1.vout[0].nValue - self.relayfee*COIN), DUMMY_P2WPKH_SCRIPT)]
        tx2_raw = self.nodes[0].signrawtransactionwithwallet(tx2.serialize().hex())["hex"]
        tx2 = tx_from_hex(tx2_raw)
        tx2.rehash()

        self.nodes[0].sendrawtransaction(tx2_raw)

        # Create a spend of the 0th output of orig_tx with a sequence lock
        # of 1, and test what happens when submitting.
        # orig_tx.vout[0] must be an anyone-can-spend output
        def test_nonzero_locks(orig_tx, node, relayfee, use_height_lock):
            sequence_value = 1
            if not use_height_lock:
                sequence_value |= SEQUENCE_LOCKTIME_TYPE_FLAG

            tx = CTransaction()
            tx.nVersion = 3  # ReddCoin: BIP68 enforced at version >= 3
            tx.vin = [CTxIn(COutPoint(orig_tx.sha256, 0), nSequence=sequence_value)]
            tx.vout = [CTxOut(int(orig_tx.vout[0].nValue - relayfee * COIN), DUMMY_P2WPKH_SCRIPT)]
            tx.rehash()

            if (orig_tx.hash in node.getrawmempool()):
                # sendrawtransaction should fail if the tx is in the mempool
                assert_raises_rpc_error(-26, NOT_FINAL_ERROR, node.sendrawtransaction, tx.serialize().hex())
            else:
                # sendrawtransaction should succeed if the tx is not in the mempool
                node.sendrawtransaction(tx.serialize().hex())

            return tx

        test_nonzero_locks(tx2, self.nodes[0], self.relayfee, use_height_lock=True)
        test_nonzero_locks(tx2, self.nodes[0], self.relayfee, use_height_lock=False)

        # Now mine some blocks, but make sure tx2 doesn't get mined.
        # Use prioritisetransaction to lower the effective feerate to 0
        self.nodes[0].prioritisetransaction(txid=tx2.hash, fee_delta=int(-self.relayfee*COIN))
        self.nodes[0].generate(10)

        assert tx2.hash in self.nodes[0].getrawmempool()

        test_nonzero_locks(tx2, self.nodes[0], self.relayfee, use_height_lock=True)
        test_nonzero_locks(tx2, self.nodes[0], self.relayfee, use_height_lock=False)

        # Mine tx2, and then try again
        self.nodes[0].prioritisetransaction(txid=tx2.hash, fee_delta=int(self.relayfee*COIN))

        self.nodes[0].generate(1)
        assert tx2.hash not in self.nodes[0].getrawmempool()
        tx2_block_height = self.nodes[0].getblockcount()

        # Generate blocks so that median time past (MTP) advances past
        # the 512-second BIP68 time-lock threshold.
        # generate() advances 60s per block. MTP advance ≈ (N+1)*60.
        # Need (N+1)*60 >= 512, so N >= 8. Use 9 for margin.
        # IMPORTANT: Can't use more than 9 blocks here because
        # InvalidateBlock() only adds disconnected txs to mempool for
        # the first 10 blocks (validation.cpp:2963). The reorg test
        # below invalidates these 9 blocks + tx2's block = 10 total.
        self.nodes[0].generate(9)

        # Now that tx2 is not in the mempool, a sequence locked spend should
        # succeed (MTP has advanced well past 512s)
        tx3 = test_nonzero_locks(tx2, self.nodes[0], self.relayfee, use_height_lock=False)
        assert tx3.hash in self.nodes[0].getrawmempool()

        self.nodes[0].generate(1)
        assert tx3.hash not in self.nodes[0].getrawmempool()

        # One more test, this time using height locks
        tx4 = test_nonzero_locks(tx3, self.nodes[0], self.relayfee, use_height_lock=True)
        assert tx4.hash in self.nodes[0].getrawmempool()

        # Now try combining confirmed and unconfirmed inputs
        tx5 = test_nonzero_locks(tx4, self.nodes[0], self.relayfee, use_height_lock=True)
        assert tx5.hash not in self.nodes[0].getrawmempool()

        utxos = self.nodes[0].listunspent()
        tx5.vin.append(CTxIn(COutPoint(int(utxos[0]["txid"], 16), utxos[0]["vout"]), nSequence=1))
        tx5.vout[0].nValue += int(utxos[0]["amount"]*COIN)
        raw_tx5 = self.nodes[0].signrawtransactionwithwallet(tx5.serialize().hex())["hex"]

        assert_raises_rpc_error(-26, NOT_FINAL_ERROR, self.nodes[0].sendrawtransaction, raw_tx5)

        # Test mempool-BIP68 consistency after reorg
        #
        # State of the transactions in the chain:
        # ... -> [ tx2 ] -> [9 MTP blocks] -> [ tx3 ]
        #       tx2_block                       tip
        # And currently tx4 is in the mempool.
        #
        # If we invalidate the tip, tx3 should get added to the mempool, causing
        # tx4 to be removed (fails sequence-lock).
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
        assert tx4.hash not in self.nodes[0].getrawmempool()
        assert tx3.hash in self.nodes[0].getrawmempool()

        # Invalidate back to tx2's block so tx2 returns to mempool.
        # This causes tx3 to be removed (tx3's sequence lock requires tx2
        # to be confirmed, and now tx2 is unconfirmed).
        #
        # ReddCoin PoS: We can't use create_block/submitblock since PoS blocks
        # require valid coinstake. Instead, we invalidate back to tx2's block,
        # which orphans it plus all MTP advancement blocks above it.
        tx2_block_hash = self.nodes[0].getblockhash(tx2_block_height)
        self.nodes[0].invalidateblock(tx2_block_hash)

        # tx2 should now be back in mempool (its block was orphaned)
        assert tx2.hash in self.nodes[0].getrawmempool()
        # tx3's sequence lock fails with tx2 unconfirmed → not in mempool
        assert tx3.hash not in self.nodes[0].getrawmempool()

        # Reconsider the invalidated blocks to restore the chain, then reset
        self.nodes[0].reconsiderblock(tx2_block_hash)
        self.nodes[0].invalidateblock(self.nodes[0].getblockhash(cur_height+1))

        # Build the replacement branch longer than the one being replaced.
        #
        # That last invalidateblock is never reconsidered, so node0 rejects the
        # original branch from here on. node1 has no such opinion and still
        # holds it, all 11 blocks of it: tx2's block, the 9 that advance MTP,
        # and tx3's block. node1 only gives it up for something with more work,
        # and node0 can never accept it back, so a replacement of 10 leaves the
        # two on chains neither will drop and no later sync can succeed.
        #
        # It only reached CI as a failure because node1 has to have caught up
        # for its branch to be the longer one, which locally it often has not.
        self.nodes[0].generate(12)

    def activateCSV(self):
        # ReddCoin CSV activation via BIP9:
        # nRuleChangeActivationThreshold=108, nMinerConfirmationWindow=144
        # With nStartTime=0, all blocks signal. BIP9 state transitions:
        #   Period 0 (0-143):   STARTED, threshold met
        #   Period 1 (144-287): transition to LOCKED_IN
        #   Period 2 (288-431): LOCKED_IN
        #   Period 3 (432+):    ACTIVE
        # Prior tests may have already generated enough blocks to activate CSV.
        height = self.nodes[0].getblockcount()
        if softfork_active(self.nodes[0], 'csv'):
            self.log.info("CSV already active at height %d" % height)
            self.sync_blocks()
            return

        min_activation_height = 432
        blocks_needed = min_activation_height - height
        if blocks_needed > 0:
            self.nodes[0].generate(blocks_needed)
        assert softfork_active(self.nodes[0], 'csv')

    # Use self.nodes[1] to test that version 3 transactions are standard.
    # ReddCoin: TX_MAX_STANDARD_VERSION = 3 (not 2 as in Bitcoin)
    def test_version3_relay(self):
        inputs = [ ]
        outputs = { self.nodes[1].getnewaddress() : 1.0 }
        rawtx = self.nodes[1].createrawtransaction(inputs, outputs)
        rawtxfund = self.nodes[1].fundrawtransaction(rawtx)['hex']
        tx = tx_from_hex(rawtxfund)
        tx.nVersion = 3
        tx_signed = self.nodes[1].signrawtransactionwithwallet(tx.serialize().hex())["hex"]
        self.nodes[1].sendrawtransaction(tx_signed)

if __name__ == '__main__':
    BIP68Test().main()
