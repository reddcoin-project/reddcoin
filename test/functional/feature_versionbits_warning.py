#!/usr/bin/env python3
# Copyright (c) 2016-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the upgrade warnings for unknown rule changes and block versions.

Generate chains with block versions that signal an unknown soft fork, or use a
block version format the node doesn't recognise, and check the warnings in RPC
and -alertnotify at each stage: signalling, locked in and active.
"""
import os

from test_framework.address import key_to_p2pkh
from test_framework.blocktools import create_block, create_coinbase, sign_block, NORMAL_GBT_REQUEST_PARAMS
from test_framework.messages import msg_block
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import advance_time_for_pos, assert_equal

VB_PERIOD = 144           # versionbits period length for regtest
VB_THRESHOLD = 108        # versionbits activation threshold for regtest
VB_RECENT = VB_PERIOD // 10  # blocks examined for the signalling warning
VB_TOP_BITS = 0x20000000
VB_UNKNOWN_BIT = 27       # Choose a bit unassigned to any deployment
VB_UNKNOWN_VERSION = VB_TOP_BITS | (1 << VB_UNKNOWN_BIT)
UNKNOWN_FORMAT_VERSION = 0x40000000  # not the versionbits format (top bits 001)
UNKNOWN_VERSION_WINDOW = 100

UPGRADE_URL = "https://download.reddcoin.com/"
WARN_SIGNALLING = "A network rule change that this version does not know about is being signalled (versionbit {}, 100% of the last {} blocks). Upgrade Reddcoin Core: {}".format(VB_UNKNOWN_BIT, VB_RECENT, UPGRADE_URL)
WARN_ANY = "network rule change that this version does not know about"


def warn_locked_in(height):
    return "(versionbit {}) is locked in and takes effect at block {} (about ".format(VB_UNKNOWN_BIT, height)


def warn_active(height):
    return "A network rule change that this version does not know about (versionbit {}) is in effect since block {}. This version may not follow the network. Upgrade Reddcoin Core now: {}".format(VB_UNKNOWN_BIT, height, UPGRADE_URL)


def warn_block_version(count):
    return "{} of the last {} blocks use a block version that this version does not recognise. The network may be changing its rules. Upgrade Reddcoin Core: {}".format(count, UNKNOWN_VERSION_WINDOW, UPGRADE_URL)


class VersionBitsWarningTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.alert_filename = os.path.join(self.options.tmpdir, "alert.txt")
        # Open and close to create zero-length file
        with open(self.alert_filename, 'w', encoding='utf8'):
            pass
        self.extra_args = [["-alertnotify=echo %s >> \"" + self.alert_filename + "\"", "-whitelist=127.0.0.1"]]
        self.setup_nodes()

    def get_signing_key(self, node, block):
        """Extract the signing key for a PoS block from the coinstake transaction."""
        coinstake = block.vtx[1]
        coinstake_hex = coinstake.serialize().hex()
        decoded_tx = node.decoderawtransaction(coinstake_hex)

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
        except Exception as e:
            self.log.debug(f"Could not get signing key: {e}")

        if not signing_key:
            self.log.debug("No signing key found, using deterministic key")
            signing_key = node.get_deterministic_priv_key().key

        return signing_key

    def send_blocks_with_version(self, peer, numblocks, version):
        """Send numblocks blocks to peer with version set"""
        node = self.nodes[0]
        height = node.getblockcount()

        # PoW path for heights <= 89 (nLastPowHeight)
        if height < 89:
            tip = node.getbestblockhash()
            block_time = node.getblockheader(tip)["time"] + 1
            tip = int(tip, 16)

            pow_blocks = min(numblocks, 89 - height)
            for _ in range(pow_blocks):
                block = create_block(tip, create_coinbase(height + 1), block_time)
                block.nVersion = version
                block.solve()
                peer.send_message(msg_block(block))
                block_time += 1
                height += 1
                tip = block.sha256
            peer.sync_with_ping()
            numblocks -= pow_blocks

        # PoS path for heights >= 90
        for _ in range(numblocks):
            max_attempts = 10
            for attempt in range(max_attempts):
                try:
                    advance_time_for_pos(node, seconds=60)
                    tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
                    block = create_block(tmpl=tmpl, version=version)
                    block.solve()

                    signing_key = self.get_signing_key(node, block)
                    sign_block(block, signing_key)

                    peer.send_message(msg_block(block))
                    peer.sync_with_ping()
                    break
                except Exception as e:
                    if "no valid coinstake found" in str(e) and attempt < max_attempts - 1:
                        advance_time_for_pos(node, seconds=120)
                        continue
                    raise

    def alerts(self, text):
        """How many -alertnotify runs carried text. -alertnotify strips '%'."""
        with open(self.alert_filename, 'r', encoding='utf8') as f:
            return f.read().count(text)

    def warnings(self):
        node = self.nodes[0]
        # All three RPCs return the same concise warning.
        warning = node.getnetworkinfo()["warnings"]
        assert warning == node.getmininginfo()["warnings"] == node.getblockchaininfo()["warnings"]
        return warning

    def run_test(self):
        node = self.nodes[0]
        peer = node.add_p2p_connection(P2PInterface())

        # Mine one period worth of blocks
        node.generate(VB_PERIOD)
        self.wait_until(lambda: not node.getblockchaininfo()['initialblockdownload'])
        assert WARN_ANY not in self.warnings()

        self.log.info("A majority of recent blocks signalling an unknown bit raises the signalling warning")
        self.send_blocks_with_version(peer, VB_THRESHOLD - 1, VB_UNKNOWN_VERSION)
        assert_equal(self.warnings(), WARN_SIGNALLING)
        self.wait_until(lambda: self.alerts("is being signalled (versionbit {}".format(VB_UNKNOWN_BIT)) == 1)

        self.log.info("The warning goes away once recent blocks stop signalling, and the bit fails to lock in")
        node.generate(VB_PERIOD - VB_THRESHOLD + 1)
        assert WARN_ANY not in self.warnings()

        self.log.info("Signalling again raises the warning without running -alertnotify a second time")
        self.send_blocks_with_version(peer, VB_THRESHOLD, VB_UNKNOWN_VERSION)
        assert_equal(self.warnings(), WARN_SIGNALLING)

        self.log.info("Once the bit locks in, the warning gives the activation height")
        node.generate(VB_PERIOD - VB_THRESHOLD)
        # Periods start at multiples of VB_PERIOD. The period holding the next
        # block is locked in, and the rule takes effect one period later.
        activation_height = ((node.getblockcount() + 1) // VB_PERIOD + 1) * VB_PERIOD
        assert warn_locked_in(activation_height) in self.warnings()
        assert self.warnings().endswith("Upgrade Reddcoin Core before then: " + UPGRADE_URL)
        self.wait_until(lambda: self.alerts(warn_locked_in(activation_height)) == 1)
        assert_equal(self.alerts("is being signalled (versionbit {}".format(VB_UNKNOWN_BIT)), 1)

        self.log.info("Once the bit is active, the warning says since when and to upgrade now")
        node.generate(VB_PERIOD)
        assert_equal(self.warnings(), warn_active(activation_height))
        self.wait_until(lambda: self.alerts(warn_active(activation_height)) == 1)

        self.log.info("After a restart the active warning comes back and runs -alertnotify again")
        self.restart_node(0)
        # Generating one block guarantees that we'll get out of IBD
        node.generate(1)
        self.wait_until(lambda: not node.getblockchaininfo()['initialblockdownload'])
        node.generate(1)
        assert_equal(self.warnings(), warn_active(activation_height))
        self.wait_until(lambda: self.alerts(warn_active(activation_height)) == 2)

        self.log.info("Half of recent blocks with an unknown block version format raises no warning")
        peer = node.add_p2p_connection(P2PInterface())
        half = UNKNOWN_VERSION_WINDOW // 2
        self.send_blocks_with_version(peer, half, UNKNOWN_FORMAT_VERSION)
        assert_equal(self.warnings(), warn_active(activation_height))

        self.log.info("More than half raises the unknown block version warning")
        self.send_blocks_with_version(peer, 1, UNKNOWN_FORMAT_VERSION)
        assert_equal(self.warnings(), warn_block_version(half + 1))
        self.wait_until(lambda: self.alerts("of the last {} blocks use a block version".format(UNKNOWN_VERSION_WINDOW)) == 1)

        self.log.info("The warning goes away as those blocks leave the window")
        node.generate(half)
        assert_equal(self.warnings(), warn_active(activation_height))


if __name__ == '__main__':
    VersionBitsWarningTest().main()
