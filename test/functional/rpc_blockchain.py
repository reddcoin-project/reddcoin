#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test RPCs related to blockchainstate.

Test the following RPCs:
    - getblockchaininfo
    - gettxoutsetinfo
    - getdifficulty
    - getbestblockhash
    - getblockhash
    - getblockheader
    - getchaintxstats
    - getnetworkhashps
    - verifychain

Tests correspond to code in rpc/blockchain.cpp.
"""

from decimal import Decimal
import http.client
import os
import subprocess

from test_framework.blocktools import (
    create_block,
    create_coinbase,
    TIME_GENESIS_BLOCK,
)
from test_framework.messages import (
    CBlockHeader,
    from_hex,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises,
    assert_raises_rpc_error,
    assert_is_hex_string,
    assert_is_hash_string,
    get_datadir_path,
)


class BlockchainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.mine_chain()
        # ReddCoin: -prune is incompatible with -txindex=1 (required for PoS).
        # Restart without pruning. stopatheight is kept for _test_stopatheight.
        self.restart_node(0, extra_args=['-stopatheight=207'])

        self._test_getblockchaininfo()
        self._test_getchaintxstats()
        self._test_gettxoutsetinfo()
        self._test_getblockheader()
        self._test_getdifficulty()
        self._test_getnetworkhashps()
        self._test_stopatheight()
        self._test_waitforblockheight()
        self._test_getblock()
        assert self.nodes[0].verifychain(4, 0)

    def mine_chain(self):
        self.log.info('Create some old blocks')
        address = self.nodes[0].get_deterministic_priv_key().address
        for t in range(TIME_GENESIS_BLOCK, TIME_GENESIS_BLOCK + 200 * 600, 600):
            # ten-minute steps from genesis block time
            self.nodes[0].setmocktime(t)
            self.nodes[0].generatetoaddress(1, address)
        assert_equal(self.nodes[0].getblockchaininfo()['blocks'], 200)
        # Remember the last mocktime for use after restarts
        self.last_mocktime = TIME_GENESIS_BLOCK + 199 * 600

    def _test_getblockchaininfo(self):
        self.log.info("Test getblockchaininfo")

        # ReddCoin adds 'moneysupply' to the response.
        keys = [
            'bestblockhash',
            'blocks',
            'chain',
            'chainwork',
            'difficulty',
            'headers',
            'initialblockdownload',
            'mediantime',
            'moneysupply',
            'pruned',
            'size_on_disk',
            'softforks',
            'verificationprogress',
            'warnings',
        ]
        res = self.nodes[0].getblockchaininfo()

        # ReddCoin: -prune is incompatible with -txindex=1 (required for PoS).
        # Without pruning, response should have exactly these keys.
        assert_equal(sorted(res.keys()), sorted(keys))

        # size_on_disk should be > 0
        assert_greater_than(res['size_on_disk'], 0)
        assert not res['pruned']

        # ReddCoin softforks at height 200:
        # - posv: buried at height 90 (= nLastPowHeight + 1), active at height 200
        # - bip66, dev: buried at height 500, not yet active at height 200
        # - BIP9 deployments (csv, segwit, taproot) are in "started" state
        #   (signaling begins in period 1 at block 144; 200 is in period 1 with
        #   elapsed=57, count=57 blocks signaling; activation at period 3, block 288)
        # - NEVER_ACTIVE deployments are excluded from the response
        assert_equal(res['softforks'], {
            'posv': {'type': 'buried', 'active': True, 'height': 90},
            'bip66': {'type': 'buried', 'active': False, 'height': 500},
            'dev': {'type': 'buried', 'active': False, 'height': 500},
            'cltv': {
                'type': 'bip9',
                'bip9': {
                    'status': 'started',
                    'bit': 1,
                    'start_time': 0,
                    'timeout': 0x7fffffffffffffff,
                    'since': 144,
                    'statistics': {
                        'period': 144,
                        'threshold': 108,
                        'elapsed': 57,
                        'count': 57,
                        'possible': True,
                    },
                    'min_activation_height': 0,
                },
                'active': False,
            },
            'csv': {
                'type': 'bip9',
                'bip9': {
                    'status': 'started',
                    'bit': 2,
                    'start_time': 0,
                    'timeout': 0x7fffffffffffffff,
                    'since': 144,
                    'statistics': {
                        'period': 144,
                        'threshold': 108,
                        'elapsed': 57,
                        'count': 57,
                        'possible': True,
                    },
                    'min_activation_height': 0,
                },
                'active': False,
            },
            'segwit': {
                'type': 'bip9',
                'bip9': {
                    'status': 'started',
                    'bit': 3,
                    'start_time': 0,
                    'timeout': 0x7fffffffffffffff,
                    'since': 144,
                    'statistics': {
                        'period': 144,
                        'threshold': 108,
                        'elapsed': 57,
                        'count': 57,
                        'possible': True,
                    },
                    'min_activation_height': 0,
                },
                'active': False,
            },
            'taproot': {
                'type': 'bip9',
                'bip9': {
                    'status': 'started',
                    'bit': 4,
                    'start_time': 0,
                    'timeout': 0x7fffffffffffffff,
                    'since': 144,
                    'statistics': {
                        'period': 144,
                        'threshold': 108,
                        'elapsed': 57,
                        'count': 57,
                        'possible': True,
                    },
                    'min_activation_height': 0,
                },
                'active': False,
            },
        })

    def _test_getchaintxstats(self):
        self.log.info("Test getchaintxstats")

        # Test `getchaintxstats` invalid extra parameters
        assert_raises_rpc_error(-1, 'getchaintxstats', self.nodes[0].getchaintxstats, 0, '', 0)

        # Test `getchaintxstats` invalid `nblocks`
        assert_raises_rpc_error(-1, "JSON value is not an integer as expected", self.nodes[0].getchaintxstats, '')
        assert_raises_rpc_error(-8, "Invalid block count: should be between 0 and the block's height - 1", self.nodes[0].getchaintxstats, -1)
        assert_raises_rpc_error(-8, "Invalid block count: should be between 0 and the block's height - 1", self.nodes[0].getchaintxstats, self.nodes[0].getblockcount())

        # Test `getchaintxstats` invalid `blockhash`
        assert_raises_rpc_error(-1, "JSON value is not a string as expected", self.nodes[0].getchaintxstats, blockhash=0)
        assert_raises_rpc_error(-8, "blockhash must be of length 64 (not 1, for '0')", self.nodes[0].getchaintxstats, blockhash='0')
        assert_raises_rpc_error(-8, "blockhash must be hexadecimal string (not 'ZZZ0000000000000000000000000000000000000000000000000000000000000')", self.nodes[0].getchaintxstats, blockhash='ZZZ0000000000000000000000000000000000000000000000000000000000000')
        assert_raises_rpc_error(-5, "Block not found", self.nodes[0].getchaintxstats, blockhash='0000000000000000000000000000000000000000000000000000000000000000')
        blockhash = self.nodes[0].getblockhash(200)
        self.nodes[0].invalidateblock(blockhash)
        assert_raises_rpc_error(-8, "Block is not in main chain", self.nodes[0].getchaintxstats, blockhash=blockhash)
        self.nodes[0].reconsiderblock(blockhash)

        chaintxstats = self.nodes[0].getchaintxstats(nblocks=1)
        # ReddCoin: PoS blocks have 2 txs (coinbase + coinstake), PoW blocks have 1.
        # Total: genesis(1) + 89 PoW(89) + 111 PoS(222) = 312
        assert_equal(chaintxstats['txcount'], 312)
        # tx rate for last 1 block: 2 txs (PoS) in 600s window
        assert_equal(round(chaintxstats['txrate'] * 600, 10), Decimal(2))

        b1_hash = self.nodes[0].getblockhash(1)
        b1 = self.nodes[0].getblock(b1_hash)
        b200_hash = self.nodes[0].getblockhash(200)
        b200 = self.nodes[0].getblock(b200_hash)
        time_diff = b200['mediantime'] - b1['mediantime']

        chaintxstats = self.nodes[0].getchaintxstats()
        assert_equal(chaintxstats['time'], b200['time'])
        assert_equal(chaintxstats['txcount'], 312)
        assert_equal(chaintxstats['window_final_block_hash'], b200_hash)
        assert_equal(chaintxstats['window_final_block_height'], 200)
        assert_equal(chaintxstats['window_block_count'], 199)
        # ReddCoin: window_tx_count = 199 blocks worth of txs.
        # Blocks 2-89 (88 PoW blocks × 1 tx) + blocks 90-200 (111 PoS blocks × 2 txs) = 88 + 222 = 310
        assert_equal(chaintxstats['window_tx_count'], 310)
        assert_equal(chaintxstats['window_interval'], time_diff)
        assert_equal(round(chaintxstats['txrate'] * time_diff, 10), Decimal(310))

        chaintxstats = self.nodes[0].getchaintxstats(blockhash=b1_hash)
        assert_equal(chaintxstats['time'], b1['time'])
        assert_equal(chaintxstats['txcount'], 2)
        assert_equal(chaintxstats['window_final_block_hash'], b1_hash)
        assert_equal(chaintxstats['window_final_block_height'], 1)
        assert_equal(chaintxstats['window_block_count'], 0)
        assert 'window_tx_count' not in chaintxstats
        assert 'window_interval' not in chaintxstats
        assert 'txrate' not in chaintxstats

    def _test_gettxoutsetinfo(self):
        node = self.nodes[0]
        res = node.gettxoutsetinfo()

        # ReddCoin: With deterministic keys and fixed 600s block spacing, the
        # chain is fully deterministic. PoS blocks have coinbase + coinstake txs.
        # 200 blocks produce 241 unique transactions with 435 unspent outputs.
        assert_equal(res['total_amount'], Decimal('5476461511.74091084'))
        assert_equal(res['transactions'], 241)
        assert_equal(res['height'], 200)
        assert_equal(res['txouts'], 435)
        assert_equal(res['bogosize'], 29015)
        assert_equal(res['bestblock'], node.getblockhash(200))
        size = res['disk_size']
        assert size > 6400
        assert_equal(len(res['bestblock']), 64)
        assert_equal(len(res['hash_serialized_2']), 64)

        self.log.info("Test that gettxoutsetinfo() works for blockchain with just the genesis block")
        b1hash = node.getblockhash(1)
        node.invalidateblock(b1hash)

        res2 = node.gettxoutsetinfo()
        # ReddCoin: After invalidation, 111 zero-value PoS coinbase outputs
        # persist in the UTXO set (one per PoS block, heights 90-200).
        assert_equal(res2['transactions'], 111)
        assert_equal(res2['total_amount'], Decimal('0'))
        assert_equal(res2['height'], 0)
        assert_equal(res2['txouts'], 111)
        assert_equal(res2['bogosize'], 5550)
        assert_equal(res2['bestblock'], node.getblockhash(0))
        assert_equal(len(res2['hash_serialized_2']), 64)

        self.log.info("Test that gettxoutsetinfo() returns the same result after invalidate/reconsider block")
        node.reconsiderblock(b1hash)

        res3 = node.gettxoutsetinfo()
        # The field 'disk_size' is non-deterministic and can thus not be
        # compared between res and res3.  Everything else should be the same.
        del res['disk_size'], res3['disk_size']
        assert_equal(res, res3)

        self.log.info("Test hash_type option for gettxoutsetinfo()")
        # Adding hash_type 'hash_serialized_2', which is the default, should
        # not change the result.
        res4 = node.gettxoutsetinfo(hash_type='hash_serialized_2')
        del res4['disk_size']
        assert_equal(res, res4)

        # hash_type none should not return a UTXO set hash.
        res5 = node.gettxoutsetinfo(hash_type='none')
        assert 'hash_serialized_2' not in res5

        # hash_type muhash should return a different UTXO set hash.
        res6 = node.gettxoutsetinfo(hash_type='muhash')
        assert 'muhash' in res6
        assert(res['hash_serialized_2'] != res6['muhash'])

        # muhash should not be returned unless requested.
        for r in [res, res2, res3, res4, res5]:
            assert 'muhash' not in r

        # Unknown hash_type raises an error
        assert_raises_rpc_error(-8, "foohash is not a valid hash_type", node.gettxoutsetinfo, "foohash")

    def _test_getblockheader(self):
        node = self.nodes[0]

        assert_raises_rpc_error(-8, "hash must be of length 64 (not 8, for 'nonsense')", node.getblockheader, "nonsense")
        assert_raises_rpc_error(-8, "hash must be hexadecimal string (not 'ZZZ7bb8b1697ea987f3b223ba7819250cae33efacb068d23dc24859824a77844')", node.getblockheader, "ZZZ7bb8b1697ea987f3b223ba7819250cae33efacb068d23dc24859824a77844")
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, "0cf7bb8b1697ea987f3b223ba7819250cae33efacb068d23dc24859824a77844")

        besthash = node.getbestblockhash()
        secondbesthash = node.getblockhash(199)
        header = node.getblockheader(blockhash=besthash)

        assert_equal(header['hash'], besthash)
        assert_equal(header['height'], 200)
        assert_equal(header['confirmations'], 1)
        assert_equal(header['previousblockhash'], secondbesthash)
        assert_is_hex_string(header['chainwork'])
        # ReddCoin: PoS blocks have 2 txs (coinbase + coinstake)
        assert_equal(header['nTx'], 2)
        assert_is_hash_string(header['hash'])
        assert_is_hash_string(header['previousblockhash'])
        assert_is_hash_string(header['merkleroot'])
        assert_is_hash_string(header['bits'], length=None)
        assert isinstance(header['time'], int)
        assert isinstance(header['mediantime'], int)
        assert isinstance(header['nonce'], int)
        assert isinstance(header['version'], int)
        assert isinstance(int(header['versionHex'], 16), int)
        assert isinstance(header['difficulty'], Decimal)

        # Test with verbose=False, which should return the header as hex.
        header_hex = node.getblockheader(blockhash=besthash, verbose=False)
        assert_is_hex_string(header_hex)

        header = from_hex(CBlockHeader(), header_hex)
        header.calc_sha256()
        assert_equal(header.hash, besthash)

        assert 'previousblockhash' not in node.getblockheader(node.getblockhash(0))
        assert 'nextblockhash' not in node.getblockheader(node.getbestblockhash())

    def _test_getdifficulty(self):
        difficulty = self.nodes[0].getdifficulty()
        # ReddCoin: regtest pins difficulty at the powLimit/posLimit minimum
        # (nBits 0x207fffff) because fPowNoRetargeting is set, so getdifficulty
        # always returns the regtest minimum regardless of block spacing.
        assert_equal(difficulty, Decimal('4.656542373906925E-10'))

    def _test_getnetworkhashps(self):
        hashes_per_second = self.nodes[0].getnetworkhashps()
        # ReddCoin: getnetworkhashps computes hash rate from block timestamps and
        # difficulty over the default 120-block window. With difficulty pinned at
        # the regtest minimum (fPowNoRetargeting) and fixed 600s spacing, this
        # value is deterministic.
        assert_equal(hashes_per_second, Decimal('0.003333333333333334'))

    def _test_stopatheight(self):
        assert_equal(self.nodes[0].getblockcount(), 200)
        # ReddCoin: Use generate() which handles mocktime advancement and retry
        # logic for PoS staking after daemon restart (nLastCoinStakeSearchTime
        # resets to system time on restart, requiring time advancement for a
        # sufficient search interval).
        self.nodes[0].generate(6)
        assert_equal(self.nodes[0].getblockcount(), 206)
        self.log.debug('Node should not stop at this height')
        assert_raises(subprocess.TimeoutExpired, lambda: self.nodes[0].process.wait(timeout=3))
        try:
            self.nodes[0].generate(1)
        except (ConnectionError, http.client.BadStatusLine):
            pass  # The node already shut down before response
        self.log.debug('Node should stop at this height...')
        self.nodes[0].wait_until_stopped()
        self.start_node(0)
        assert_equal(self.nodes[0].getblockcount(), 207)

    def _test_waitforblockheight(self):
        self.log.info("Test waitforblockheight")
        node = self.nodes[0]

        current_height = node.getblock(node.getbestblockhash())['height']

        # Create a fork somewhere below our current height, invalidate the tip
        # of that fork, and then ensure that waitforblockheight still
        # works as expected.
        #
        # (Previously this was broken based on setting
        # `rpc/blockchain.cpp:latestblock` incorrectly.)
        #
        b20hash = node.getblockhash(20)
        b20 = node.getblock(b20hash)

        def solve_and_submit_block(prevhash, height, time):
            b = create_block(prevhash, create_coinbase(height), time)
            b.solve()
            # ReddCoin: Use submitblock instead of P2P to force processing.
            # Unrequested P2P blocks with less chainwork than the tip are
            # silently skipped by AcceptBlock.
            node.submitblock(b.serialize().hex())
            return b

        b21f = solve_and_submit_block(int(b20hash, 16), 21, b20['time'] + 1)
        b22f = solve_and_submit_block(b21f.sha256, 22, b21f.nTime + 1)

        node.invalidateblock(b22f.hash)

        def assert_waitforheight(height, timeout=2):
            assert_equal(
                node.waitforblockheight(height=height, timeout=timeout)['height'],
                current_height)

        assert_waitforheight(0)
        assert_waitforheight(current_height - 1)
        assert_waitforheight(current_height)
        assert_waitforheight(current_height + 1)

    def _test_getblock(self):
        node = self.nodes[0]

        # ReddCoin: MiniWallet uses witness (P2WSH) transactions, but SegWit
        # is not yet active at this height (~209). Use the node wallet directly
        # with a raw transaction for exact fee control.
        utxos = node.listunspent()
        utxo = utxos[0]
        addr = node.getnewaddress()
        fee = Decimal('0.001')
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: str(utxo["amount"] - fee)}
        )
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"]
        txid = node.sendrawtransaction(signed["hex"])
        blockhash = node.generate(1)[0]

        self.log.info("Test that getblock with verbosity 1 doesn't include fee")
        block = node.getblock(blockhash, 1)
        # Find the wallet tx in the block by txid
        tx_idx = block['tx'].index(txid)
        assert 'fee' not in block['tx'][tx_idx]

        self.log.info('Test that getblock with verbosity 2 includes expected fee')
        block = node.getblock(blockhash, 2)
        tx = block['tx'][tx_idx]
        assert 'fee' in tx
        assert_equal(tx['fee'], fee)

        self.log.info("Test that getblock with verbosity 2 still works with pruned Undo data")
        datadir = get_datadir_path(self.options.tmpdir, 0)

        self.log.info("Test that getblock with invalid verbosity type returns proper error message")
        assert_raises_rpc_error(-1, "JSON value is not an integer as expected", node.getblock, blockhash, "2")

        def move_block_file(old, new):
            old_path = os.path.join(datadir, self.chain, 'blocks', old)
            new_path = os.path.join(datadir, self.chain, 'blocks', new)
            os.rename(old_path, new_path)

        # Move instead of deleting so we can restore chain state afterwards
        move_block_file('rev00000.dat', 'rev_wrong')

        block = node.getblock(blockhash, 2)
        assert 'fee' not in block['tx'][tx_idx]

        # Restore chain state
        move_block_file('rev_wrong', 'rev00000.dat')

        assert 'previousblockhash' not in node.getblock(node.getblockhash(0))
        assert 'nextblockhash' not in node.getblock(node.getbestblockhash())


if __name__ == '__main__':
    BlockchainTest().main()
