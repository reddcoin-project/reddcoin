#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a node without SegWit stays in sync across SegWit activation.

This is the end-to-end form of RED-48. A node that does not enforce SegWit but
still advertises NODE_WITNESS asks its peers for witness-serialized blocks
(GetFetchFlags, and compact block version 2). Once an upgraded peer activates
SegWit its coinbase carries the 32-byte reserved witness value, so the
non-upgraded node receives witness data for a block it believes commits to
none, and ContextualCheckBlock rejects it as unexpected-witness. Every
post-activation block fails the same way, which is a chain split on any
partially upgraded network.

Both nodes are current builds. A v4.22.9 node would not reproduce this: it
already gates NODE_WITNESS on DeploymentEnabled and therefore asks for stripped
blocks.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Consensus::BIP9Deployment sentinels, src/consensus/params.h
NEVER_ACTIVE = -2
NO_TIMEOUT = 9223372036854775807  # std::numeric_limits<int64_t>::max()

# Regtest SegWit locks in over three 144-block BIP9 windows and activates here.
SEGWIT_ACTIVATION_HEIGHT = 432

# WarningBitsConditionChecker fires for a bit the node itself does not signal.
SEGWIT_BIT = 3
WARN_UNKNOWN_RULES = "Unknown new rules activated (versionbit {})".format(SEGWIT_BIT)


class SegwitNonUpgradedSyncTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            [
                # node0 is the non-upgraded node: SegWit is unscheduled, exactly
                # as on mainnet. nTimeout MUST be NO_TIMEOUT rather than 0. The
                # gate this test guards read "nTimeout != 0", so segwit:-2:0
                # would make a regressed build withhold NODE_WITNESS and pass
                # for the wrong reason. Do not "simplify" the timeout away.
                f"-vbparams=segwit:{NEVER_ACTIVE}:{NO_TIMEOUT}",
                # Neither node may stake on its own account. node1 is the sole
                # block producer so the heights below stay deterministic, and a
                # block staked by node0 would fork the pair for unrelated
                # reasons. Staking is on by default in every node.
                "-staking=0",
                "-peertimeout=9999",  # mocktime jumps over ~230 blocks would otherwise disconnect
            ],
            [
                # node1 is the upgraded node: default regtest, SegWit activates.
                "-staking=0",
                "-peertimeout=9999",
            ],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def segwit_scheduled(self, node):
        # getblockchaininfo omits deployments for which DeploymentEnabled() is
        # false, which is the same predicate the NODE_WITNESS gate uses.
        return "segwit" in node.getblockchaininfo()["softforks"]

    def segwit_active(self, node):
        # Note this is DeploymentActiveAfter: it reports whether SegWit applies
        # to the block after the tip, not to the tip itself.
        softforks = node.getblockchaininfo()["softforks"]
        return "segwit" in softforks and softforks["segwit"]["active"]

    def block_has_witness(self, node, blockhash):
        """Whether the block's coinbase carries witness data.

        A witness-bearing transaction has a wtxid distinct from its txid. Post
        activation the coinbase holds the 32-byte reserved value added by
        UpdateUncommittedBlockStructures, which is the data a node with SegWit
        inactive rejects as unexpected-witness.
        """
        coinbase = node.getblock(blockhash, 2)["tx"][0]
        return coinbase["hash"] != coinbase["txid"]

    def run_test(self):
        node0, node1 = self.nodes

        self.log.info("node0 has SegWit unscheduled, node1 has it scheduled")
        assert not self.segwit_scheduled(node0)
        assert self.segwit_scheduled(node1)
        # node0's service bits are asserted at the end rather than here, so that
        # a regressed build fails on the chain split itself instead of on the
        # precondition. feature_segwit_service_bit.py is the direct guard on the
        # advertisement; this test exists to show what the advertisement costs.
        self.log.info("node0 advertises %s", node0.getnetworkinfo()["localservicesnames"])

        self.log.info("Sync the pair up to the last pre-activation block")
        blocks_to_lock_in = SEGWIT_ACTIVATION_HEIGHT - 1 - node1.getblockcount()
        assert blocks_to_lock_in > 0
        self.generate(blocks_to_lock_in, node=node1)
        self.sync_blocks()
        assert_equal(node1.getblockcount(), SEGWIT_ACTIVATION_HEIGHT - 1)
        # Blocks up to here carry no witness, so both nodes see identical bytes
        # however they ask for them and the pair cannot have split yet.
        assert not self.block_has_witness(node1, node1.getbestblockhash())
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

        self.log.info("Cross activation: node1's blocks now carry a coinbase witness")
        assert self.segwit_active(node1)  # active for the next block, 432
        self.generate(4, node=node1)
        assert self.block_has_witness(node1, node1.getblockhash(SEGWIT_ACTIVATION_HEIGHT))

        self.log.info("node0 accepts the post-activation blocks")
        # A regressed build fails here. node0 asked for witness serialization,
        # so it rejects block SEGWIT_ACTIVATION_HEIGHT with unexpected-witness /
        # BLOCK_MUTATED, scores node1 as Misbehaving 100 and drops the
        # connection, which surfaces as sync_blocks finding no peers at all.
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())
        assert_equal(node0.getblockcount(), SEGWIT_ACTIVATION_HEIGHT + 3)

        self.log.info("node0 followed the chain without adopting the new rules")
        assert not self.segwit_scheduled(node0)
        assert "WITNESS" not in node0.getnetworkinfo()["localservicesnames"]
        assert "WITNESS" in node1.getnetworkinfo()["localservicesnames"]
        assert WARN_UNKNOWN_RULES in node0.getblockchaininfo()["warnings"]


if __name__ == '__main__':
    SegwitNonUpgradedSyncTest().main()
