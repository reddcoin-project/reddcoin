#!/usr/bin/env python3
# Copyright (c) 2018-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test createwallet watchonly arguments.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    advance_time_for_pos,
    assert_equal,
    assert_raises_rpc_error
)


class CreateWalletWatchonlyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # importpubkey is legacy-wallet only; descriptor wallets use importdescriptors
        self.skip_if_no_bdb()

    def gen_block(self):
        """Generate one PoS block via the framework wallet.

        Routes generatetoaddress through the wallet RPC so --usecli works
        when multiple wallets are loaded (bare-node CLI can't pick a wallet).
        Handles PoS time advancement and retry logic.
        """
        node = self.nodes[0]
        fw = node.get_wallet_rpc(self.default_wallet_name)
        addr = node.get_deterministic_priv_key().address
        advance_time_for_pos(node, seconds=60)
        for attempt in range(10):
            try:
                return fw.generatetoaddress(1, addr)
            except Exception as e:
                if "no valid coinstake found" in str(e) and attempt < 9:
                    advance_time_for_pos(node, seconds=300)
                else:
                    raise

    def run_test(self):
        node = self.nodes[0]

        # The framework's default wallet has cache coins for staking/funding.
        fw_wallet = node.get_wallet_rpc(self.default_wallet_name)

        self.nodes[0].createwallet(wallet_name='default', descriptors=False)
        def_wallet = node.get_wallet_rpc('default')

        a1 = def_wallet.getnewaddress()
        wo_change = def_wallet.getnewaddress()
        wo_addr = def_wallet.getnewaddress()

        self.nodes[0].createwallet(wallet_name='wo', disable_private_keys=True, descriptors=False)
        wo_wallet = node.get_wallet_rpc('wo')

        wo_wallet.importpubkey(pubkey=def_wallet.getaddressinfo(wo_addr)['pubkey'])
        wo_wallet.importpubkey(pubkey=def_wallet.getaddressinfo(wo_change)['pubkey'])

        # Fund the default wallet from the framework wallet (which has cache coins).
        # In PoS, generatetoaddress doesn't pay to the target address — rewards go
        # to the staking UTXO owner. So we fund explicitly via sendtoaddress.
        fw_wallet.sendtoaddress(a1, 100)
        self.gen_block()

        # send 1 rdd to our watch-only address
        txid = def_wallet.sendtoaddress(wo_addr, 1)
        self.gen_block()

        # getbalance
        self.log.info('include_watchonly should default to true for watch-only wallets')
        self.log.info('Testing getbalance watch-only defaults')
        assert_equal(wo_wallet.getbalance(), 1)
        assert_equal(len(wo_wallet.listtransactions()), 1)
        assert_equal(wo_wallet.getbalance(include_watchonly=False), 0)

        self.log.info('Test sending from a watch-only wallet raises RPC error')
        msg = "Error: Private keys are disabled for this wallet"
        assert_raises_rpc_error(-4, msg, wo_wallet.sendtoaddress, a1, 0.1)
        assert_raises_rpc_error(-4, msg, wo_wallet.sendmany, amounts={a1: 0.1})

        self.log.info('Testing listreceivedbyaddress watch-only defaults')
        result = wo_wallet.listreceivedbyaddress()
        assert_equal(len(result), 1)
        assert_equal(result[0]["involvesWatchonly"], True)
        result = wo_wallet.listreceivedbyaddress(include_watchonly=False)
        assert_equal(len(result), 0)

        self.log.info('Testing listreceivedbylabel watch-only defaults')
        result = wo_wallet.listreceivedbylabel()
        assert_equal(len(result), 1)
        assert_equal(result[0]["involvesWatchonly"], True)
        result = wo_wallet.listreceivedbylabel(include_watchonly=False)
        assert_equal(len(result), 0)

        self.log.info('Testing listtransactions watch-only defaults')
        result = wo_wallet.listtransactions()
        assert_equal(len(result), 1)
        assert_equal(result[0]["involvesWatchonly"], True)
        result = wo_wallet.listtransactions(include_watchonly=False)
        assert_equal(len(result), 0)

        self.log.info('Testing listsinceblock watch-only defaults')
        result = wo_wallet.listsinceblock()
        assert_equal(len(result["transactions"]), 1)
        assert_equal(result["transactions"][0]["involvesWatchonly"], True)
        result = wo_wallet.listsinceblock(include_watchonly=False)
        assert_equal(len(result["transactions"]), 0)

        self.log.info('Testing gettransaction watch-only defaults')
        result = wo_wallet.gettransaction(txid)
        assert_equal(result["details"][0]["involvesWatchonly"], True)
        result = wo_wallet.gettransaction(txid=txid, include_watchonly=False)
        assert_equal(len(result["details"]), 0)

        self.log.info('Testing walletcreatefundedpsbt watch-only defaults')
        inputs = []
        outputs = [{a1: 0.5}]
        options = {'changeAddress': wo_change}
        no_wo_options = {'changeAddress': wo_change, 'includeWatching': False}

        result = wo_wallet.walletcreatefundedpsbt(inputs=inputs, outputs=outputs, options=options)
        assert_equal("psbt" in result, True)
        assert_raises_rpc_error(-4, "Insufficient funds", wo_wallet.walletcreatefundedpsbt, inputs, outputs, 0, no_wo_options)

        self.log.info('Testing fundrawtransaction watch-only defaults')
        rawtx = wo_wallet.createrawtransaction(inputs=inputs, outputs=outputs)
        result = wo_wallet.fundrawtransaction(hexstring=rawtx, options=options)
        assert_equal("hex" in result, True)
        assert_raises_rpc_error(-4, "Insufficient funds", wo_wallet.fundrawtransaction, rawtx, no_wo_options)



if __name__ == '__main__':
    CreateWalletWatchonlyTest().main()
