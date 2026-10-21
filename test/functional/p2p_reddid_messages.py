#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ReddID peer-to-peer messages against a running node.

Node 0 runs ReddID (-reddid=1), node 1 does not, which is the default.

- Node 0 advertises NODE_REDDID to its peers, node 1 does not.
- A request for the configuration of a namespace that node 0 does not know
  gets no reply.
- Every one of the 18 ReddID message types is sent to node 0 malformed and
  well formed. The node neither disconnects nor bans the peer: this is pinned
  as the current behaviour, it is not a statement that it should be so.
- A namespace configuration, a user ID auction announcement, a bid, a profile,
  a connection and a reputation that a peer sends are accepted by node 0 and
  relayed to its other peers. The messages carry no proof of anything; the
  unit tests pin each of these paths as a suspected bug.
- Node 1 ignores every ReddID message and stays up.

The message encoders in test_framework/reddid.py are written from the
serialization of the message classes in src/id/reddid_p2p.h and of the types
in src/id/reddid.h, and so are a second, independent statement of the wire
format.
"""

import os
import time

from test_framework.messages import (
    NODE_NETWORK,
    NODE_REDDID,
    NODE_WITNESS,
    ser_string,
)
from test_framework.reddid import (
    COIN,
    DAY,
    PEER_SERVICES,
    REDDID_COMMANDS,
    ReddIDPeer,
    key_id,
    msg_reddid,
    reddid_msg,
    ser_bid,
    ser_connection,
    ser_finalize,
    ser_namespace_announce,
    ser_namespace_config,
    ser_profile,
    ser_profile_request,
    ser_profile_update,
    ser_reputation,
    ser_user_announce,
    user_auction_id,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class ReddIDMessagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = False  # Use cache with pre-generated blocks
        self.extra_args = [["-reddid=1"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # The two nodes are not connected to each other
        self.setup_nodes()

    def connect(self, node):
        return node.add_p2p_connection(ReddIDPeer(), services=PEER_SERVICES)

    def assert_still_connected(self, node, peers):
        for peer in peers:
            peer.sync_with_ping()
            assert peer.is_connected
        assert_equal(len(node.getpeerinfo()), len(node.p2ps))
        assert_equal(node.listbanned(), [])

    def well_formed(self, now):
        """A payload for each message type that deserializes completely. Most name things that the node does not know."""
        auction_id = bytes([0xa1]) * 32
        bid_id = bytes([0xa2]) * 32
        owner = key_id(0xc1)
        profile = ser_profile("nobody", owner, now)
        announce = ser_user_announce(now, auction_id, "alice", "unknown", owner, now, now + 5 * DAY, 5000 * COIN)
        bid = ser_bid(now, auction_id, bid_id, owner, 6000 * COIN)
        finalize = ser_finalize(now, auction_id, bid_id, 6000 * COIN, owner)
        return {
            "nsaucann": ser_namespace_announce(now, auction_id, "other", owner, now, now + 5 * DAY, 20000 * COIN),
            "nsaucbid": bid,
            "nsaucfin": finalize,
            "nsauccan": auction_id,
            "nsconreq": ser_string(b"unknown"),
            "nsconres": ser_namespace_config(now, "Invalid", owner),
            "uidaucann": announce,
            "uidaucbid": bid,
            "uidaucfin": finalize,
            "uidauccan": auction_id,
            "ridaucann": announce,
            "ridaucbid": bid,
            "ridaucfin": finalize,
            "ridprofup": ser_profile_update(now, "ab", profile),
            "ridconn": ser_connection(now, "nobody", "noone", 0),
            "ridprofreq": ser_profile_request(now, "nobody"),
            "ridprofres": ser_profile_update(now, "nobody", profile)[:-1] + b"\x07",
            "ridrep": ser_reputation(now, "nobody", 50, now),
        }

    def run_test(self):
        node = self.nodes[0]
        plain = self.nodes[1]
        # The clock that the node uses for the times in the messages
        now = node.mocktime or int(time.time())

        self.log.info("A node with -reddid=1 advertises NODE_REDDID, a node without does not")
        sender = self.connect(node)
        listener = self.connect(node)
        assert sender.nServices & NODE_REDDID
        assert 'REDDID' in node.getnetworkinfo()['localservicesnames']
        plain_peer = self.connect(plain)
        assert not plain_peer.nServices & NODE_REDDID
        assert 'REDDID' not in plain.getnetworkinfo()['localservicesnames']
        assert not os.path.exists(os.path.join(plain.datadir, self.chain, 'reddid'))

        self.log.info("The node knows no namespace and asked a connecting peer for none")
        assert_equal(sender.take(), [])
        assert_equal(listener.take(), [])

        self.log.info("A request for an unknown namespace gets no reply")
        with node.assert_debug_log(["Don't have info for namespace unknown"]):
            sender.send_message(reddid_msg("nsconreq", ser_string(b"unknown")))
            assert_equal(sender.take(), [])
        assert_equal(listener.take(), [])

        self.log.info("Malformed messages of every type neither disconnect nor ban the peer")
        malformed = [
            b"",                       # empty
            b"\x00",                   # too short for anything
            b"\xfd\xff\xff",           # a length that promises more than there is
            b"\xff" * 300,             # nonsense
        ]
        for command in REDDID_COMMANDS:
            for payload in malformed:
                sender.send_message(reddid_msg(command, payload))
            self.assert_still_connected(node, [sender, listener])
        assert_equal(sender.take(), [])
        assert_equal(listener.take(), [])

        self.log.info("A message larger than the ReddID limit of 1 MB is ignored")
        with node.assert_debug_log(["Oversized message nsconres"]):
            sender.send_message(reddid_msg("nsconres", b"\x00" * (1024 * 1024 + 1)))
            sender.sync_with_ping()
        self.assert_still_connected(node, [sender, listener])

        self.log.info("Commands that only have a ReddID prefix are ignored")
        for command in ("nsmadeup", "uidmadeup", "ridmadeup"):
            unknown = type("msg_" + command, (msg_reddid,), {"msgtype": command.encode(), "__slots__": ()})
            with node.assert_debug_log(["Unknown message type: " + command]):
                sender.send_message(unknown(b"\x00"))
                sender.sync_with_ping()
        self.assert_still_connected(node, [sender, listener])

        self.log.info("Well-formed messages of every type that name unknown things are rejected quietly")
        # A new peer: each peer may send a few messages of a type per minute
        prober = self.connect(node)
        for command, payload in self.well_formed(now).items():
            with node.assert_debug_log(["Handler returned false for message " + command]):
                prober.send_message(reddid_msg(command, payload))
                prober.sync_with_ping()
        self.assert_still_connected(node, [sender, listener, prober])
        assert_equal(prober.take(), [])
        assert_equal(listener.take(), [])

        self.log.info("Current behaviour: a namespace configuration from a peer creates the namespace and is relayed")
        # A peer that has sent nothing yet: the malformed messages above count
        # towards the rate limit of the peer that sent them
        fuzzer = sender
        sender = self.connect(node)
        owner = key_id(0xc1)
        config = ser_namespace_config(now, "fresh", owner, tiers=[(4, 6000 * COIN)])
        with node.assert_debug_log(["Received namespace config response for fresh"], unexpected_msgs=["Handler returned false"]):
            sender.send_message(reddid_msg("nsconres", config))
            assert_equal(sender.take(), [])
        assert_equal(listener.take(), [("nsconres", config)])

        self.log.info("The namespace is now known: a request is answered, to the peer that asked")
        sender.send_message(reddid_msg("nsconreq", ser_string(b"fresh")))
        replies = sender.take()
        assert_equal([command for command, _ in replies], ["nsconres"])
        assert ser_string(b"fresh") + owner in replies[0][1]
        assert_equal(listener.take(), [])

        self.log.info("Current behaviour: a connecting peer is asked for the namespace before the version handshake")
        late = self.connect(node)
        assert_equal(late.commands(), ["nsconreq"])
        assert late.order.index("nsconreq") < late.order.index("version")
        assert late.order.index("nsconreq") < late.order.index("verack")

        self.log.info("Current behaviour: an auction announcement from a peer creates the auction")
        start = now
        announce = ser_user_announce(now, bytes(32), "alice", "fresh", key_id(0xc2), start, start + 5 * DAY, 6000 * COIN)
        with node.assert_debug_log(["Successfully created user ID auction"], unexpected_msgs=["Handler returned false"]):
            sender.send_message(reddid_msg("uidaucann", announce))
            # The auction manager announces the auction to every peer, the sender included
            assert_equal(sender.commands(), ["uidaucann"])
        # and the handler relays the message to every peer but the sender
        relayed = listener.take()
        assert_equal([command for command, _ in relayed], ["uidaucann", "uidaucann"])
        assert_equal(relayed[1][1], announce)
        assert_equal(late.commands(), ["uidaucann", "uidaucann"])

        self.log.info("Current behaviour: a bid message from a peer places a bid, with no transaction and no deposit")
        auction_id = user_auction_id("alice", "fresh", start)
        assert auction_id in relayed[0][1]
        bid = ser_bid(now, auction_id, bytes(32), key_id(0xc3), 7000 * COIN)
        with node.assert_debug_log(["Successfully processed bid"], unexpected_msgs=["Handler returned false"]):
            sender.send_message(reddid_msg("nsaucbid", bid))
            assert_equal(sender.commands(), ["uidaucbid"])
        relayed = listener.take()
        assert_equal([command for command, _ in relayed], ["uidaucbid", "nsaucbid"])
        assert_equal(relayed[1][1], bid)
        late.take()

        self.log.info("User ID bids are relayed, every copy of them, and are not placed")
        user_bid = ser_bid(now, auction_id, bytes(32), key_id(0xc4), 8000 * COIN)
        for _ in range(2):
            sender.send_message(reddid_msg("uidaucbid", user_bid))
            sender.send_message(reddid_msg("ridaucbid", user_bid))
        assert_equal(sender.take(), [])
        assert_equal(listener.take(), [("uidaucbid", user_bid)] * 4)
        late.take()

        self.log.info("Current behaviour: profiles, connections and reputations from a peer are stored and relayed")
        for name, byte in (("alice", 0xd1), ("bobby", 0xd2)):
            update = ser_profile_update(now, name, ser_profile(name, key_id(byte), now))
            with node.assert_debug_log(["Successfully processed profile update for " + name]):
                sender.send_message(reddid_msg("ridprofup", update))
                assert_equal(sender.take(), [])
            assert_equal(listener.take(), [("ridprofup", update)])

        connection = ser_connection(now, "alice", "bobby", 2)
        with node.assert_debug_log(["Successfully saved connection from alice to bobby"]):
            sender.send_message(reddid_msg("ridconn", connection))
            assert_equal(sender.take(), [])
        assert_equal(listener.take(), [("ridconn", connection)])

        # The time of the calculation must be later than that of the reputation
        # that the node gave the profile when it created it, by its own clock
        reputation = ser_reputation(now, "alice", 100, now + 10 * 365 * DAY)
        with node.assert_debug_log(["Successfully updated reputation for alice from P2P announcement"]):
            sender.send_message(reddid_msg("ridrep", reputation))
            assert_equal(sender.take(), [])
        assert_equal(listener.take(), [("ridrep", reputation)])
        late.take()

        self.log.info("Current behaviour: a profile request is answered to every peer but the one that asked")
        sender.send_message(reddid_msg("ridprofreq", ser_profile_request(now, "alice")))
        assert_equal(sender.take(), [])
        assert_equal(listener.commands(), ["ridprofres"])
        assert_equal(late.commands(), ["ridprofres"])

        self.log.info("A peer that does not advertise NODE_REDDID is ignored")
        stranger = node.add_p2p_connection(ReddIDPeer(), services=NODE_NETWORK | NODE_WITNESS)
        with node.assert_debug_log(["Ignoring ReddID message nsconres from incompatible peer"]):
            stranger.send_message(reddid_msg("nsconres", ser_namespace_config(now, "second", owner)))
            stranger.sync_with_ping()
        assert_equal(listener.take(), [])
        # It is asked for the namespace and gets what the node relays all the same
        assert_equal(stranger.commands(), ["nsconreq"])

        self.assert_still_connected(node, [fuzzer, sender, listener, prober, late, stranger])

        self.log.info("A node without ReddID ignores every ReddID message and stays up")
        with plain.assert_debug_log(["ReddID P2P manager not available for message nsconres"]):
            plain_peer.send_message(reddid_msg("nsconres", config))
            plain_peer.sync_with_ping()
        for command, payload in self.well_formed(now).items():
            plain_peer.send_message(reddid_msg(command, payload))
            for junk in malformed:
                plain_peer.send_message(reddid_msg(command, junk))
        self.assert_still_connected(plain, [plain_peer])
        assert_equal(plain_peer.take(), [])
        # A peer that connects is asked for nothing
        second = self.connect(plain)
        assert_equal(second.take(), [])
        assert "nsconreq" not in second.order
        assert not os.path.exists(os.path.join(plain.datadir, self.chain, 'reddid'))
        assert_equal(plain.getblockcount(), node.getblockcount())

        self.log.info("Both nodes restart cleanly, and the node with ReddID has kept the namespace")
        node.disconnect_p2ps()
        plain.disconnect_p2ps()
        # The node keeps its clock: on the wall clock the periodic task would
        # treat the namespace as expired and erase it a minute after the start
        restart_args = ["-reddid=1"]
        if node.mocktime:
            restart_args.append("-mocktime=%d" % node.mocktime)
        self.restart_node(0, extra_args=restart_args)
        peer = self.connect(self.nodes[0])
        assert_equal(peer.commands(), ["nsconreq"])
        peer.send_message(reddid_msg("nsconreq", ser_string(b"fresh")))
        assert_equal(peer.commands(), ["nsconres"])
        self.restart_node(1)


if __name__ == '__main__':
    ReddIDMessagesTest().main()
