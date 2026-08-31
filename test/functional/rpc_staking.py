#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the staking control and status RPCs.

- staking          (node-wide switch, mirrors the -staking argument)
- setstaking       (per-wallet switch)
- getstakinginfo   (staking status for the selected wallet)

These are how an operator turns staking on and off and inspects what it is
doing, and none of them had any functional coverage: grepping the suite for
"staking" finds -staking=0 on the command line in dozens of tests, but nothing
that calls the RPCs.

The two switches are deliberately separate. `staking` sets the node-wide
gArgs -staking flag and starts or stops the staking threads; `setstaking`
records the intent on one wallet and adds or removes it from the staking set.
A wallet only actually stakes when both are on, which is what the interaction
at the end of this test pins down.

The last case covers the thread behind the switches rather than the flag in
front of it. Reporting the flag is not enough: a wallet whose staking thread
has ended reports `setstaking` true and stakes nothing.

Staking is left off at every point where the test asserts on chain height,
since a staking thread that finds a kernel would move the tip underneath it.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Keys getstakinginfo always reports, per its RPCResult. currentblockweight and
# currentblocktx are omitted until a block has been assembled, so they are not
# required here.
STAKINGINFO_KEYS = {
    "enabled",
    "staking",
    "chain",
    "blocks",
    "difficulty",
    "networkhashps",
    "pooledtx",
    "search-interval",
    "averageweight",
    "totalweight",
    "netstakeweight",
    "expectedtime",
    "warnings",
}


class StakingRpcTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # Start with staking off so the tip stays put; the switches are then
        # exercised explicitly. A staking thread reserves a destination for its
        # coinstake rewards as it starts and throws if the keypool cannot cover
        # it, which would end the thread the lifecycle case is watching.
        self.extra_args = [["-staking=0", "-keypool=100"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def staking_by_wallet(self, entries):
        """setstaking and staking report a list of one-key {name: enabled} objects.

        Not named wallet_names: the framework already owns that attribute, and
        assigning the list of wallets to create silently replaces the method.
        """
        names = {}
        for entry in entries:
            assert_equal(len(entry), 1)
            names.update(entry)
        return names

    def test_staking_switch(self):
        node = self.nodes[0]
        wallet = self.default_wallet_name

        self.log.info("staking reports the node-wide switch")
        status = node.staking()
        assert_equal(status["enabled"], False)  # -staking=0
        assert_equal(status["running"], False)
        assert_equal(status["thread_count"], 0)
        assert wallet in self.staking_by_wallet(status["enabled_wallet"])

        self.log.info("staking with no argument does not change the switch")
        assert_equal(node.staking()["enabled"], False)

        self.log.info("staking(True) turns it on and starts the threads")
        started = node.staking(True)
        assert_equal(started["enabled"], True)
        assert_equal(node.staking()["enabled"], True)

        self.log.info("staking(False) turns it off again")
        stopped = node.staking(False)
        assert_equal(stopped["enabled"], False)
        assert_equal(stopped["running"], False)
        assert_equal(stopped["thread_count"], 0)
        assert_equal(node.staking()["enabled"], False)

    def test_setstaking_switch(self):
        node = self.nodes[0]
        wallet = self.default_wallet_name

        self.log.info("setstaking with no argument lists every loaded wallet")
        wallets = self.staking_by_wallet(node.setstaking())
        assert wallet in wallets

        self.log.info("setstaking(True) enables staking for the selected wallet")
        enabled = node.setstaking(True)
        assert_equal(enabled["enabled"], True)
        assert_equal(enabled["warning"], "")
        assert_equal(self.staking_by_wallet(node.setstaking())[wallet], True)

        self.log.info("setstaking(False) disables it again")
        disabled = node.setstaking(False)
        assert_equal(disabled["enabled"], False)
        assert_equal(self.staking_by_wallet(node.setstaking())[wallet], False)

        self.log.info("The node-wide switch is left alone by the per-wallet one")
        assert_equal(node.staking()["enabled"], False)

    def test_getstakinginfo(self):
        node = self.nodes[0]

        self.log.info("getstakinginfo reports every documented field")
        info = node.getstakinginfo()
        missing = STAKINGINFO_KEYS - set(info)
        assert_equal(missing, set())

        assert_equal(info["chain"], self.chain)
        assert_equal(info["blocks"], node.getblockcount())
        assert_equal(info["pooledtx"], len(node.getrawmempool()))
        assert_equal(info["enabled"], False)  # -staking=0
        assert_equal(info["staking"], False)  # not searching, so not staking

        # The wallet holds the cache's staked coins, so it has weight to report
        # even while staking is switched off.
        assert info["totalweight"] >= 0
        assert info["netstakeweight"] >= 0
        assert info["expectedtime"] >= 0

        self.log.info("getstakinginfo follows the enabled flag")
        node.staking(True)
        assert_equal(node.getstakinginfo()["enabled"], True)
        node.staking(False)
        assert_equal(node.getstakinginfo()["enabled"], False)

    def test_switches_are_independent(self):
        node = self.nodes[0]
        wallet = self.default_wallet_name

        self.log.info("The node-wide and per-wallet switches do not shadow each other")
        node.staking(False)
        node.setstaking(True)
        assert_equal(node.staking()["enabled"], False)
        assert_equal(self.staking_by_wallet(node.setstaking())[wallet], True)
        assert_equal(node.getstakinginfo()["enabled"], False)

        node.staking(True)
        node.setstaking(False)
        assert_equal(node.staking()["enabled"], True)
        assert_equal(self.staking_by_wallet(node.setstaking())[wallet], False)
        assert_equal(node.getstakinginfo()["enabled"], True)

        # Leave the node as the test found it.
        node.staking(False)
        assert_equal(node.staking()["enabled"], False)

    def test_staking_thread_lifecycle(self):
        """setstaking has to be able to start a thread it stopped.

        RED-102: turning staking off for a wallet ended its thread, and turning
        it back on set the flag without starting a new one, so the node stopped
        staking with nothing reporting an error. thread_count claimed a thread
        was running the whole time, because a thread that had returned was
        still counted.

        This runs last: it is the only case that leaves both switches on, so
        from here the node is free to move its own tip.
        """
        node = self.nodes[0]

        node.setstaking(False)
        node.staking(True)
        assert_equal(node.staking()["thread_count"], 0)

        self.log.info("setstaking(True) starts a thread for the wallet")
        node.setstaking(True)
        status = node.staking()
        assert_equal(status["thread_count"], 1)
        assert_equal(status["running"], True)

        self.log.info("setstaking(False) stops it and stops counting it")
        node.setstaking(False)
        status = node.staking()
        assert_equal(status["thread_count"], 0)
        assert_equal(status["running"], False)

        self.log.info("setstaking(True) starts a replacement thread")
        node.setstaking(True)
        assert_equal(node.staking()["thread_count"], 1)

        self.log.info("Enabling an already staking wallet does not add a second thread")
        with node.assert_debug_log(expected_msgs=["is already staking"]):
            node.setstaking(True)
        assert_equal(node.staking()["thread_count"], 1)

        self.log.info("The node-wide switch stops the thread as well")
        node.staking(False)
        assert_equal(node.staking()["thread_count"], 0)

        self.log.info("With the node-wide switch off, setstaking starts nothing")
        node.setstaking(True)
        assert_equal(node.staking()["thread_count"], 0)

        node.setstaking(False)

    def test_locked_wallet_thread_can_be_stopped(self):
        """A thread waiting for its wallet to be unlocked still has to stop.

        The wait loops checked only the shutdown flag, so a thread parked on a
        locked wallet ignored both switches and the join that stops it never
        returned. That hung the RPC that asked for the stop, and with it the
        `staking` RPC behind the same lock, including the node-wide
        `staking false` that RED-102 documents as the way to recover.

        Without a fix this case does not fail, it hangs, and the framework's
        RPC timeout is what ends it.
        """
        node = self.nodes[0]

        # A wallet of its own, encrypted before it ever stakes: encryptwallet
        # reloads the wallet, and a running thread holds the one it started on.
        node.createwallet(wallet_name="locked_staker")
        wallet = node.get_wallet_rpc("locked_staker")
        wallet.encryptwallet("passphrase")

        wallet.walletpassphrase("passphrase", 60)
        # A starting thread reserves a destination, which the keypool of a
        # freshly encrypted wallet cannot cover.
        wallet.keypoolrefill(100)

        node.staking(True)
        wallet.setstaking(True)
        assert_equal(node.staking()["thread_count"], 1)

        self.log.info("A staking thread parks when its wallet is locked")
        wallet.walletlock()
        # The warning is only reachable from inside the wait loop, so seeing it
        # is what proves the thread is parked rather than between passes.
        self.wait_until(lambda: "locked wallet" in wallet.getstakinginfo()["warnings"])

        self.log.info("setstaking(False) stops a thread parked on a locked wallet")
        wallet.setstaking(False)
        assert_equal(node.staking()["thread_count"], 0)

        node.staking(False)

    def run_test(self):
        self.test_staking_switch()
        self.test_setstaking_switch()
        self.test_getstakinginfo()
        self.test_switches_are_independent()
        self.test_staking_thread_lifecycle()
        self.test_locked_wallet_thread_can_be_stopped()


if __name__ == "__main__":
    StakingRpcTest().main()
