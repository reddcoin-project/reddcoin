#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the PoS-era transaction rules against a previous-release peer.

The current build rejects v1 transactions (bad-txns-version-pos) and v2+
transactions with nTime == 0 (bad-txns-time-zero) from
consensus.nPosTxVersionFloorHeight on (regtest 90). v4.22.9 releases have
neither rule: they accept such transactions into their mempool and relay them.
The current build must reject them without punishing the relaying peer, and
everything else must keep flowing between the two.

  node0 — previous release (--compat-release), wallet, sync-only follower
  node1 — current build, lead staker

  Phase A — node1 bootstraps past the floor and funds node0; node0 syncs.
  Phase B — node0 signs, accepts and relays a v1 tx and a v2 tx with
            nTime == 0. node1 rejects each with its reason, logs no
            misbehaviour, and both nodes stay connected.
  Phase C — normal wallet txs relay both ways, node1 mines them, node0
            follows; the rejected txs are not in the block and both nodes
            have a single chain tip.

Requires previous releases, see test/README.md.
"""
import time

from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, tx_from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    advance_time_for_pos,
    set_node_times,
)

FLOOR_HEIGHT = 90  # regtest consensus.nPosTxVersionFloorHeight
POW_BLOCKS = 89
POS_BLOCKS = 40
NODE0_FUNDING_UTXOS = 4
NODE0_FUNDING_AMOUNT = 5000  # RDD per UTXO


class CompatPosEraTxRulesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-whitelist=127.0.0.1", "-staking=0"],  # node0: previous release
            ["-whitelist=127.0.0.1", "-staking=0"],  # node1: current build
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_previous_releases()

    def setup_network(self):
        """Nodes start disconnected so node1 can build the chain first."""
        self.add_nodes(self.num_nodes, extra_args=self.extra_args,
                       versions=[self.options.compat_release, None])
        self.start_nodes()
        self.import_deterministic_coinbase_privkeys()

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _step_clocks(self, steps=10, seconds=3):
        """Frozen mocktime stalls the inv trickle, so a relayed tx is never
        announced. Step both nodes' clocks forward together."""
        for _ in range(steps):
            set_node_times(self.nodes, self.nodes[1].mocktime + seconds)
            time.sleep(0.3)

    def _signed_by_node0(self, version, ntime):
        """A tx spending one of node0's coins, signed by node0's wallet. nTime
        is not covered by the signature hash, so it is set freely."""
        node0 = self.nodes[0]
        utxo = [u for u in node0.listunspent(1) if u["amount"] >= 100][0]
        node0.lockunspent(False, [{"txid": utxo["txid"], "vout": utxo["vout"]}])
        tx = CTransaction()
        tx.nVersion = version
        tx.nTime = ntime
        tx.vin = [CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"]))]
        spk = bytes.fromhex(node0.getaddressinfo(node0.getnewaddress())["scriptPubKey"])
        tx.vout = [CTxOut(int(utxo["amount"] * COIN) - COIN, spk)]  # 1 RDD fee
        signed = node0.signrawtransactionwithwallet(tx.serialize().hex())
        assert signed["complete"], signed
        return signed["hex"]

    # ------------------------------------------------------------------
    # Phases
    # ------------------------------------------------------------------

    def _phase_a(self):
        node0, node1 = self.nodes
        self.log.info("Phase A: node1 bootstraps past the floor and funds node0")
        node1.generate(POW_BLOCKS)
        advance_time_for_pos(node1, seconds=600)
        node1.generate(POS_BLOCKS)
        for _ in range(NODE0_FUNDING_UTXOS):
            node1.sendtoaddress(node0.getnewaddress(), NODE0_FUNDING_AMOUNT)
        advance_time_for_pos(node1, seconds=600)
        node1.generate(10)
        assert node1.getblockcount() > FLOOR_HEIGHT

        # v4.22.9 compares real time with mocktime in net.cpp; start it at
        # node1's mocktime so it does not drop the connection while syncing.
        self.restart_node(0, extra_args=self.extra_args[0] + [f"-mocktime={node1.mocktime}"])
        self.nodes[0].mocktime = node1.mocktime
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=240)
        assert_equal(len(self.nodes[0].listunspent(1)), NODE0_FUNDING_UTXOS)

    def _phase_b(self):
        node0, node1 = self.nodes
        self.log.info("Phase B: node0 relays a v1 tx and a v2 tx with nTime == 0")
        # v4.22.9's InactivityCheck also drops a peer about 60 s after it
        # connects under mocktime; reconnect so phases B and C fit in that window.
        self.disconnect_nodes(0, 1)
        self.connect_nodes(0, 1)

        self.bad_txs = []
        for label, version, reason in (("v1", 1, "bad-txns-version-pos"),
                                       ("v2 nTime=0", 2, "bad-txns-time-zero")):
            hexstr = self._signed_by_node0(version, 0)
            txid = tx_from_hex(hexstr).rehash()
            with node1.assert_debug_log([reason],
                                        unexpected_msgs=["Misbehaving", "disconnecting peer"],
                                        timeout=20):
                assert_equal(node0.sendrawtransaction(hexstr, 0), txid)
                self._step_clocks()
            assert txid in node0.getrawmempool(), f"{label}: previous release did not accept it"
            assert txid not in node1.getrawmempool(), f"{label}: current build accepted it"
            res = node1.testmempoolaccept([hexstr], 0)[0]
            assert_equal(res["allowed"], False)
            assert_equal(res["reject-reason"], reason)
            self.bad_txs.append(txid)
            self.log.info("  %s: previous release relayed it, current build rejected it with %s", label, reason)

        assert_equal(len(node0.getpeerinfo()), 1)
        assert_equal(len(node1.getpeerinfo()), 1)

    def _phase_c(self):
        node0, node1 = self.nodes
        self.log.info("Phase C: normal relay both ways, block sync, single tip")
        t01 = node0.sendtoaddress(node1.getnewaddress(), 1000)
        t10 = node1.sendtoaddress(node0.getnewaddress(), 500)
        self._step_clocks()
        self.wait_until(lambda: t01 in node1.getrawmempool(), timeout=20)
        self.wait_until(lambda: t10 in node0.getrawmempool(), timeout=20)

        advance_time_for_pos(node1, seconds=600)
        set_node_times([node0], node1.mocktime)
        node1.generate(1)
        self.sync_blocks(timeout=60)

        block_txs = node1.getblock(node1.getbestblockhash())["tx"]
        assert t01 in block_txs and t10 in block_txs
        for txid in self.bad_txs:
            assert txid not in block_txs
        assert_equal(len(node0.getchaintips()), 1)
        assert_equal(len(node1.getchaintips()), 1)

    def run_test(self):
        self.log.info("node0: %s, node1: %s",
                      self.nodes[0].getnetworkinfo()["subversion"],
                      self.nodes[1].getnetworkinfo()["subversion"])
        self._phase_a()
        self._phase_b()
        self._phase_c()


if __name__ == '__main__':
    CompatPosEraTxRulesTest().main()
