#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a node with the -disablewallet option.

- Test that validateaddress RPC works when running with -disablewallet
- Test that generatetoaddress fails without a wallet (ReddCoin PoS requires wallet)
- Test that it is not possible to mine to an invalid address.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error

class DisableWalletTest (BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-disablewallet"]]
        self.wallet_names = []

    def run_test (self):
        # Make sure wallet is really disabled
        assert_raises_rpc_error(-32601, 'Method not found', self.nodes[0].getwalletinfo)

        # ReddCoin regtest uses PUBKEY_ADDRESS=122 (prefix 'r') and SCRIPT_ADDRESS=5 (prefix '3')
        # Bitcoin testnet address (prefix 'm', version 111) is invalid on ReddCoin regtest
        x = self.nodes[0].validateaddress('mneYUmWYsuk7kySiURxCi3AGxrAqZxLgPZ')
        assert x['isvalid'] == False
        # ReddCoin regtest P2SH address (prefix '3', version 5) is valid
        x = self.nodes[0].validateaddress('3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy')
        assert x['isvalid'] == True

        # ReddCoin's generatetoaddress requires a wallet for PoS staking,
        # so both valid and invalid addresses fail when wallet is disabled.
        assert_raises_rpc_error(-18, "No wallet is loaded", self.nodes[0].generatetoaddress, 1, '3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy')
        assert_raises_rpc_error(-5, "Invalid address", self.nodes[0].generatetoaddress, 1, 'mneYUmWYsuk7kySiURxCi3AGxrAqZxLgPZ')

if __name__ == '__main__':
    DisableWalletTest ().main ()
