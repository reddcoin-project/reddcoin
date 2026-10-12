#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test what of the ReddID state of one node reaches another.

Three nodes run ReddID. Nodes 0 and 1 are connected, node 2 joins at the end.

State reaches another node in two ways, and neither is complete:

- A few messages make the peer do what the sender did: a namespace
  configuration and the announcement of a user ID auction. A bid that a
  command placed is announced and not placed by the peer, and a finalization
  by a command is not announced at all.
- A record in a transaction takes effect on every node that connects the
  block, if the node already knows the auction that the record is about. Bids
  with deposits, the start of the settlement and the settlement do reach node
  1 this way. Nothing creates a namespace or an auction from a block, so node
  2, which was not connected when they were announced, learns nothing from
  the same chain.

Every difference between the nodes is asserted on both, under a comment that
starts with SUSPECTED BUG.
"""

from decimal import Decimal

from test_framework.reddid import (
    COIN,
    DAY,
    PEER_SERVICES,
    ReddIDPeer,
    advance_clock,
    expiry_task_idle,
    keep_clock,
    key_id,
    mine_block,
    reddid_msg,
    run_expiry_task,
    ser_namespace_config,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

SHARED = ("auction_id", "name", "namespace_id", "creator", "start_time", "end_time", "reserve_price", "type")


class ReddIDSyncTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = False  # Use cache with pre-generated blocks
        self.extra_args = [["-reddid=1", "-debug=reddid"]] * 3

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # The commands take keys from a legacy wallet
        self.skip_if_no_bdb()

    def setup_network(self):
        # Node 2 stays on its own until the end
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.sync_all(self.nodes[:2])

    def mine(self):
        """A block of node 0, on nodes 0 and 1."""
        block = mine_block(self.nodes, self.nodes[0], self.stake_address)
        self.sync_blocks(self.nodes[:2])
        return block

    def relay(self, source, txid):
        """Give the transaction of one of nodes 0 and 1 to the other.

        The nodes announce transactions to each other on a timer. Handing the
        transaction over does not wait for it, which keeps the steps after a
        change of the clock well inside the minute between two runs of the
        periodic task.
        """
        raw = source.getrawtransaction(txid)
        for node in self.nodes[:2]:
            assert_equal(node.sendrawtransaction(raw), txid)
            assert txid in node.getrawmempool()

    def shared(self, node, auction_id):
        info = node.getuserauctioninfo(auction_id)
        return {key: info[key] for key in SHARED}

    def run_test(self):
        node, other, late = self.nodes
        self.stake_address = node.getnewaddress("", "legacy")
        now = node.mocktime

        self.log.info("A namespace configuration from a peer spreads from node to node")
        # SUSPECTED BUG: this is the only way in which a node learns of a
        # namespace that it did not create itself, and the message proves
        # nothing: the peer names the owner
        peer = node.add_p2p_connection(ReddIDPeer(), services=PEER_SERVICES)
        owner = key_id(0xc1)
        config = ser_namespace_config(now, "shared", owner, tiers=[(4, 6000 * COIN)])
        with other.assert_debug_log(["Received namespace config response for shared"], timeout=30):
            peer.send_message(reddid_msg("nsconres", config))
            peer.sync_with_ping()
        self.wait_until(lambda: len(other.getnamespacelist()) == 1)
        namespace = node.getnamespaceinfo("shared")
        assert_equal(node.validateaddress(namespace['owner'])['scriptPubKey'], "76a914" + owner.hex() + "88ac")
        assert_equal(other.getnamespaceinfo("shared"), namespace)
        assert_equal(late.getnamespacelist(), [])

        self.log.info("A user ID auction that a command creates is announced, and the peer creates it too")
        with other.assert_debug_log(["Successfully created user ID auction"], timeout=30):
            alice = node.createuseridauction("alice", "shared", 10000, 3)
        with other.assert_debug_log(["Successfully created user ID auction"], timeout=30):
            bobby = node.createuseridauction("bobby", "shared", 10000, 3)
        for auction in (alice, bobby):
            assert_equal(self.shared(other, auction['auction_id']), self.shared(node, auction['auction_id']))
            assert_equal(other.getuserauctioninfo(auction['auction_id'])['state'], 0)
        alice_id = alice['auction_id']
        bobby_id = bobby['auction_id']

        self.log.info("A bid that a command places is announced, and the peer does not place it")
        # SUSPECTED BUG: the handler of the peer passes the message on and
        # changes nothing. The two nodes now disagree about both auctions.
        for auction_id in (alice_id, bobby_id):
            with other.assert_debug_log(["Processing message uidaucbid from peer"], timeout=30):
                node.bidonuseridauction(auction_id, 10000)
            other.syncwithvalidationinterfacequeue()
            here = node.getuserauctioninfo(auction_id, True)
            there = other.getuserauctioninfo(auction_id, True)
            assert_equal((here['state'], here['current_bid'], len(here['bids'])), (1, Decimal("10000"), 1))
            assert_equal((there['state'], there['current_bid'], len(there['bids'])), (0, Decimal("0"), 0))

        self.log.info("A bid with a deposit is placed on both nodes when the block connects")
        # The key of the creator needs a coin for the finalization later on
        node.sendtoaddress(alice['creator_address'], 10)
        self.mine()
        bid = other.reddidplacebid(alice_id, 12000)
        other.lockunspent(False, [{"txid": bid['txid'], "vout": 1}])
        self.relay(other, bid['txid'])
        for each in (node, other):
            assert_raises_rpc_error(-8, "Deposit lock not found", each.reddidgetdepositlock, bid['txid'], 1)
        self.mine()
        lock = node.reddidgetdepositlock(bid['txid'], 1)
        assert_equal(lock['state'], "active")
        assert_equal(lock['bidder'], bid['bidder_address'])
        assert_equal(other.reddidgetdepositlock(bid['txid'], 1), lock)
        assert_equal(other.reddidlistmydeposits(), [lock])
        assert_equal(node.reddidlistmydeposits(), [])
        # Both nodes have the bid with the deposit, node 0 also has its own
        here = node.getuserauctioninfo(alice_id, True)
        there = other.getuserauctioninfo(alice_id, True)
        for info in (here, there):
            assert_equal(info['state'], 1)
            assert_equal(info['current_bid'], Decimal("12000"))
            assert_equal(info['current_bidder'], bid['bidder_address'])
        assert_equal((len(here['bids']), len(there['bids'])), (2, 1))

        self.log.info("A finalization by a command stays on the node")
        for each in (node, other):
            run_expiry_task(each)
        # Both auctions end when the clock moves. Until the block with the
        # finalization for alice has connected, the periodic task of node 0
        # would finalize both auctions and that of node 1 would finalize the
        # one for alice and cancel the one for bobby, which has no bid there.
        with expiry_task_idle(node, other):
            advance_clock(self.nodes, 3 * DAY)
            # SUSPECTED BUG: node 0 registers the user ID, with no transaction and
            # no message, and no other node will ever have it
            finalized = node.finalizeuseridauction(bobby_id)
            assert_equal(finalized['state'], 3)
            assert_equal(node.getuseridinfo("bobby", "shared")['owner'], finalized['winner'])
            assert_equal(other.getuserauctioninfo(bobby_id)['state'], 0)
            assert_raises_rpc_error(-8, "User ID not found", other.getuseridinfo, "bobby", "shared")

            self.log.info("A finalization in a transaction starts the settlement on both nodes")
            # The first input must be the coin of the creator key: see rpc_reddid_deposit.py
            others = [{"txid": coin['txid'], "vout": coin['vout']} for coin in node.listunspent() if coin['address'] != alice['creator_address']]
            node.lockunspent(False, others)
            start = node.reddidfinalizeauction(alice_id)
            node.lockunspent(True, others)
            self.relay(node, start['txid'])
            self.mine()
            for each in (node, other):
                info = each.getuserauctioninfo(alice_id, True)
                assert_equal(info['state'], 4)
                assert_equal([entry['bidder'] for entry in info['bids'] if entry['is_winner']], [bid['bidder_address']])
                assert_equal(each.reddidgetdepositlock(bid['txid'], 1), lock)
            assert_equal(other.getuserauctioninfo(bobby_id)['state'], 0)

        self.log.info("The settlement registers the user ID on both nodes")
        # The wallet of node 0 holds no key of the winning bid
        assert_raises_rpc_error(-4, "Winning bid deposit key does not belong to this wallet", node.reddidsettleauction, alice_id)
        settle = other.reddidsettleauction(alice_id)
        assert_equal(settle['bid_id'], lock['bid_id'])
        self.relay(other, settle['txid'])
        self.mine()
        user_id = node.getuseridinfo("alice", "shared")
        assert_equal(user_id['owner'], bid['bidder_address'])
        assert_equal(other.getuseridinfo("alice", "shared"), user_id)
        for each in (node, other):
            assert_equal(each.getuserauctioninfo(alice_id)['state'], 5)
            assert_equal(each.reddidgetdepositlock(bid['txid'], 1)['state'], "consumed")
        assert_equal([entry['name'] for entry in other.getuseridlist("shared")], ["alice"])
        assert_equal(sorted(entry['name'] for entry in node.getuseridlist("shared")), ["alice", "bobby"])

        self.log.info("A node that joins later has the same chain and none of the state")
        # SUSPECTED BUG: no block creates a namespace or an auction, so every
        # record in the chain is about something that node 2 does not know.
        # When it connects, node 1 asks it for the namespace that node 1
        # already has, before the handshake, so that node 2 drops the
        # request; nobody tells node 2 about the namespace.
        expected = [
            'Unsupported message "nsconreq" prior to verack',
            "Auction not found for deposit: " + alice_id,
            "CompleteSettlement: Auction not found",
        ]
        with late.assert_debug_log(expected, timeout=60):
            self.connect_nodes(1, 2)
            self.sync_blocks()
        assert_equal(late.getbestblockhash(), node.getbestblockhash())
        late.syncwithvalidationinterfacequeue()
        assert_equal(late.getnamespacelist(), [])
        assert_equal(late.getuserauctionlist(), [])
        assert_raises_rpc_error(-8, "Namespace not found", late.getuseridlist, "shared")
        assert_raises_rpc_error(-8, "User ID not found", late.getuseridinfo, "alice", "shared")
        # Node 2 never had the lock on the deposit that nodes 0 and 1 had
        assert_raises_rpc_error(-8, "Deposit lock not found", late.reddidgetdepositlock, bid['txid'], 1)
        assert_equal(late.reddidgetauctionconfig("userid"), node.reddidgetauctionconfig("userid"))

        self.log.info("The periodic task of node 1 cancels the auction that node 0 finalized")
        # SUSPECTED BUG: on node 1 the auction for bobby has ended with no
        # bid. Node 0 has given the name to the bidder that only it knew.
        run_expiry_task(other)
        assert_equal(other.getuserauctioninfo(bobby_id)['state'], 8)
        assert_equal(node.getuserauctioninfo(bobby_id)['state'], 3)

        self.log.info("The state of node 1 survives a restart on the same clock")
        self.restart_node(1, extra_args=keep_clock(other, self.extra_args[1]))
        assert_equal(other.getuseridinfo("alice", "shared"), user_id)
        assert_equal(other.getnamespaceinfo("shared"), namespace)
        assert_equal(other.reddidgetdepositlock(bid['txid'], 1)['state'], "consumed")
        assert_equal(other.getuserauctioninfo(bobby_id)['state'], 8)


if __name__ == '__main__':
    ReddIDSyncTest().main()
