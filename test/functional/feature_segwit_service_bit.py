#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that NODE_WITNESS is advertised only when SegWit is scheduled.

A node that does not enforce SegWit must not claim to serve witness data. If it
advertises NODE_WITNESS it also requests witness blocks (GetFetchFlags), so an
upgraded peer sends it post-activation blocks with the coinbase witness attached
and ContextualCheckBlock rejects every one of them as unexpected-witness. On a
partially upgraded network that is a chain split the moment SegWit activates.

RED-48: the gate in init.cpp read

    vDeployments[DEPLOYMENT_SEGWIT].nTimeout != 0

but no network sets nTimeout to 0, so it was always true and mainnet advertised
NODE_WITNESS with SegWit NEVER_ACTIVE. The gate is now DeploymentEnabled(), i.e.
nStartTime != NEVER_ACTIVE, which is what "scheduled" actually means.
"""

from test_framework.test_framework import BitcoinTestFramework

# Consensus::BIP9Deployment sentinels, src/consensus/params.h
NEVER_ACTIVE = -2
NO_TIMEOUT = 9223372036854775807  # std::numeric_limits<int64_t>::max()


class SegwitServiceBitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def advertises_witness(self):
        return "WITNESS" in self.nodes[0].getnetworkinfo()["localservicesnames"]

    def segwit_scheduled(self):
        # getblockchaininfo omits deployments for which DeploymentEnabled() is
        # false, so this is an independent read of the same predicate the
        # NODE_WITNESS gate uses. It confirms -vbparams was actually applied,
        # rather than the service bit being absent for some unrelated reason.
        return "segwit" in self.nodes[0].getblockchaininfo()["softforks"]

    def run_test(self):
        self.log.info("Default regtest schedules SegWit (nStartTime=0): WITNESS advertised")
        assert self.segwit_scheduled()
        assert self.advertises_witness()

        # NOTE: nTimeout MUST be NO_TIMEOUT here, not 0. The regressed gate
        # tested "nTimeout != 0", so -vbparams=segwit:-2:0 makes the buggy build
        # withhold the bit and pass this test for the wrong reason. NEVER_ACTIVE
        # with NO_TIMEOUT is mainnet's exact configuration and is the only vector
        # that reproduces RED-48. Do not "simplify" the timeout away.
        self.log.info("SegWit NEVER_ACTIVE with NO_TIMEOUT: WITNESS withheld")
        self.restart_node(0, [f"-vbparams=segwit:{NEVER_ACTIVE}:{NO_TIMEOUT}"])
        assert not self.segwit_scheduled()
        assert not self.advertises_witness()

        self.log.info("SegWit scheduled with NO_TIMEOUT: WITNESS advertised")
        self.restart_node(0, [f"-vbparams=segwit:0:{NO_TIMEOUT}"])
        assert self.segwit_scheduled()
        assert self.advertises_witness()


if __name__ == '__main__':
    SegwitServiceBitTest().main()
