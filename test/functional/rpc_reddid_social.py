#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ReddID connection and reputation commands.

No command and no transaction creates a profile: a node has the profiles that
its peers send it. A test peer therefore gives the node three profiles, three
connections and a reputation with ReddID messages, and the test checks what
getuserconnections, createuserconnection, updateuserconnection,
removeuserconnection, getuserreputation, updateuserreputation,
calculateuserreputation and getuserreputationhistory do with them.

What looks wrong is asserted as it is, under a comment that starts with
SUSPECTED BUG.
"""

from decimal import Decimal

from test_framework.reddid import (
    DAY,
    PEER_SERVICES,
    ReddIDPeer,
    advance_clock,
    keep_clock,
    key_id,
    reddid_msg,
    ser_connection,
    ser_profile,
    ser_profile_update,
    ser_reputation,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

NOT_OWNER = "You are not the owner of the source ReddID"
NO_CONNECTION = "Connection does not exist between these ReddIDs"
OWNERS = {"alice": 0xd1, "bobby": 0xd2, "carol": 0xd3}
# The reputation that a node gives a profile when it stores it for the first time
INITIAL_REPUTATION = {
    "overall_score": 50,
    "longevity_score": 0,
    "reliability_score": 50,
    "engagement_score": 0,
    "verification_score": 0,
    "auction_score": 50,
}
COMPONENTS = ("longevity_score", "reliability_score", "engagement_score", "verification_score", "auction_score")


class ReddIDSocialTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with pre-generated blocks
        self.extra_args = [["-reddid=1", "-debug=reddid"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # The commands take keys from a legacy wallet
        self.skip_if_no_bdb()

    def key_index(self, node):
        """The index of the next key of the wallet, which this call uses up."""
        path = node.getaddressinfo(node.getnewaddress("", "legacy"))['hdkeypath']
        return int(path.split('/')[-1].rstrip("'"))

    def hundredths(self, reputation):
        """The reputation as the node writes it: each score in hundredths, with a half rounded up.

        The same arithmetic as the serialization of ReddIDReputation in
        src/id/reddid.h: static_cast<int32_t>(score * 100.0 + 0.5), read back
        as that number divided by 100.
        """
        return {key: Decimal(int(float(value) * 100.0 + 0.5)) / 100 if key.endswith("_score") else value for key, value in reputation.items()}

    def assert_owner(self, node, address, name):
        """The address is that of the key that the peer named as the owner of the profile."""
        assert_equal(node.validateaddress(address)['scriptPubKey'], "76a914" + key_id(OWNERS[name]).hex() + "88ac")

    def run_test(self):
        node = self.nodes[0]
        now = node.mocktime

        self.log.info("Without a profile every command reports that the ReddID is not found")
        for command, args in (
            (node.getuserconnections, ("alice",)),
            (node.getuserreputation, ("alice",)),
            (node.getuserreputationhistory, ("alice",)),
            (node.calculateuserreputation, ("alice",)),
            (node.updateuserreputation, ("alice", {"overall_score": 60})),
        ):
            assert_raises_rpc_error(-8, "ReddID not found", command, *args)
        assert_raises_rpc_error(-8, "Source ReddID not found", node.createuserconnection, "alice", "bobby", 0)
        assert_raises_rpc_error(-8, "Source ReddID not found", node.updateuserconnection, "alice", "bobby", 0)
        assert_raises_rpc_error(-8, "Source ReddID not found", node.removeuserconnection, "alice", "bobby")

        self.log.info("A peer gives the node three profiles, three connections and a reputation")
        # SUSPECTED BUG: the node stores them on the word of the peer, which
        # proves nothing and names any key as the owner
        peer = node.add_p2p_connection(ReddIDPeer(), services=PEER_SERVICES)
        for name, byte in OWNERS.items():
            update = ser_profile_update(now, name, ser_profile(name, key_id(byte), now))
            with node.assert_debug_log(["Successfully processed profile update for " + name]):
                peer.send_message(reddid_msg("ridprofup", update))
                peer.sync_with_ping()
        for source, target, kind in (("alice", "bobby", 2), ("alice", "carol", 0), ("carol", "alice", 1)):
            with node.assert_debug_log(["Successfully saved connection from %s to %s" % (source, target)]):
                peer.send_message(reddid_msg("ridconn", ser_connection(now, source, target, kind)))
                peer.sync_with_ping()
        calculated = now + 3 * DAY
        with node.assert_debug_log(["Successfully updated reputation for alice from P2P announcement"]):
            peer.send_message(reddid_msg("ridrep", ser_reputation(now, "alice", 80, calculated)))
            peer.sync_with_ping()

        self.log.info("getuserconnections shows the connections from a ReddID, to it, or both")
        to_bobby = {
            "to_reddid": "bobby",
            "connection_type": 2,
            "type_name": "endorse",
            "creation_time": now,
            "last_interaction": now,
            "visibility": 0,
            "metadata": "",
        }
        to_carol = dict(to_bobby, to_reddid="carol", connection_type=0, type_name="follow")
        from_carol = {
            "from_reddid": "carol",
            "connection_type": 1,
            "type_name": "friend",
            "creation_time": now,
            "last_interaction": now,
            "visibility": 0,
        }
        outgoing = node.getuserconnections("alice")
        assert_equal(sorted(outgoing.keys()), ["outgoing_connections", "reddid"])
        assert_equal(outgoing['reddid'], "alice")
        assert_equal(sorted(outgoing['outgoing_connections'], key=lambda c: c['to_reddid']), [to_bobby, to_carol])
        assert_equal(node.getuserconnections("alice", None, "outgoing"), outgoing)
        assert_equal(node.getuserconnections("alice", None, "incoming"), {"reddid": "alice", "incoming_connections": [from_carol]})
        both = node.getuserconnections("alice", None, "both")
        assert_equal(both['incoming_connections'], [from_carol])
        assert_equal(len(both['outgoing_connections']), 2)
        # The second argument keeps the connections of one type
        assert_equal(node.getuserconnections("alice", 2), {"reddid": "alice", "outgoing_connections": [to_bobby]})
        assert_equal(node.getuserconnections("alice", 0, "both"), {"reddid": "alice", "outgoing_connections": [to_carol], "incoming_connections": []})
        assert_equal(node.getuserconnections("alice", 3, "both"), {"reddid": "alice", "outgoing_connections": [], "incoming_connections": []})
        assert_equal(node.getuserconnections("bobby"), {"reddid": "bobby", "outgoing_connections": []})
        assert_equal(len(node.getuserconnections("bobby", None, "incoming")['incoming_connections']), 1)
        assert_raises_rpc_error(-8, "ReddID not found", node.getuserconnections, "nobody")
        assert_raises_rpc_error(-8, "Invalid connection type, must be between 0 and 3", node.getuserconnections, "alice", 4)
        assert_raises_rpc_error(-8, "Invalid connection type, must be between 0 and 3", node.getuserconnections, "alice", -1)
        assert_raises_rpc_error(-8, "direction must be 'outgoing', 'incoming', or 'both'", node.getuserconnections, "alice", None, "sideways")

        self.log.info("createuserconnection checks its arguments")
        assert_raises_rpc_error(-8, "Invalid connection type, must be between 0 and 3", node.createuserconnection, "bobby", "alice", 4)
        assert_raises_rpc_error(-8, "Invalid visibility, must be between 0 and 2", node.createuserconnection, "bobby", "alice", 0, 3)
        assert_raises_rpc_error(-8, "Source ReddID not found", node.createuserconnection, "nobody", "alice", 0)
        assert_raises_rpc_error(-8, "Target ReddID not found", node.createuserconnection, "bobby", "nobody", 0)
        assert_raises_rpc_error(-8, "Unknown address type", node.createuserconnection, "bobby", "alice", 0, 0, "metadata", "account", "nonsense")

        self.log.info("createuserconnection, updateuserconnection and removeuserconnection accept no key that the wallet has been given")
        # SUSPECTED BUG: the three commands compare the owner of the source
        # profile with a key that they take from the wallet for the call, the
        # next unused one. A wallet can never pass with a key that it has
        # already been given, and each call uses up a key. Here the wallet
        # does not hold the keys of the owners at all, which a peer named.
        # (Not tried: an owner that is the next key of the wallet, which a
        # peer that knew that key could name. Nothing that the commands do
        # after this check is tested.)
        before = self.key_index(node)
        assert_raises_rpc_error(-8, NOT_OWNER, node.createuserconnection, "bobby", "alice", 0)
        assert_raises_rpc_error(-8, NOT_OWNER, node.createuserconnection, "bobby", "alice", 1, 2, "metadata", "account", "legacy", "label")
        assert_raises_rpc_error(-8, NOT_OWNER, node.updateuserconnection, "alice", "bobby", 1)
        assert_raises_rpc_error(-8, NOT_OWNER, node.updateuserconnection, "alice", "bobby", 1, 2, "metadata", "account", "legacy", "label")
        assert_raises_rpc_error(-8, NOT_OWNER, node.removeuserconnection, "alice", "bobby")
        assert_raises_rpc_error(-8, NOT_OWNER, node.removeuserconnection, "alice", "bobby", "account", "legacy", "label")
        assert_equal(self.key_index(node), before + 7)
        # The connection is looked for before the owner is checked
        assert_raises_rpc_error(-8, NO_CONNECTION, node.updateuserconnection, "bobby", "alice", 1)
        assert_raises_rpc_error(-8, NO_CONNECTION, node.removeuserconnection, "bobby", "alice")
        assert_raises_rpc_error(-8, "Source ReddID not found", node.updateuserconnection, "nobody", "alice", 1)
        assert_raises_rpc_error(-8, "Target ReddID not found", node.updateuserconnection, "alice", "nobody", 1)
        assert_raises_rpc_error(-8, "Source ReddID not found", node.removeuserconnection, "nobody", "alice")
        assert_raises_rpc_error(-8, "Invalid connection type, must be between 0 and 3", node.updateuserconnection, "alice", "bobby", 4)
        assert_raises_rpc_error(-8, "Invalid visibility, must be between 0 and 2", node.updateuserconnection, "alice", "bobby", 1, 3)
        assert_raises_rpc_error(-8, "Unknown address type", node.updateuserconnection, "alice", "bobby", 1, 0, "metadata", "account", "nonsense")
        assert_raises_rpc_error(-8, "Unknown address type", node.removeuserconnection, "alice", "bobby", "account", "nonsense")
        # Nothing changed
        assert_equal(node.getuserconnections("alice"), outgoing)
        assert_equal(node.getuserconnections("bobby"), {"reddid": "bobby", "outgoing_connections": []})

        self.log.info("getuserreputation shows the reputation that the peer sent")
        reputation = node.getuserreputation("alice")
        self.assert_owner(node, reputation.pop('owner'), "alice")
        assert_equal(reputation, {
            "reddid": "alice",
            "overall_score": 80,
            "last_calculated": calculated,
            "profile_summary": {
                "display_name": "alice",
                "verification_status": 0,
                "creation_time": now,
                "active": True,
            },
        })
        detailed = node.getuserreputation("alice", True)
        assert_equal([detailed[component] for component in COMPONENTS], [80] * 5)
        assert_raises_rpc_error(-8, "ReddID not found", node.getuserreputation, "nobody")

        self.log.info("A profile starts with the reputation that the node gave it")
        initial = node.getuserreputation("carol", True)
        self.assert_owner(node, initial['owner'], "carol")
        assert_equal({key: initial[key] for key in INITIAL_REPUTATION}, INITIAL_REPUTATION)
        assert_equal(initial['last_calculated'], now)

        self.log.info("getuserreputationhistory")
        history = node.getuserreputationhistory("alice")
        assert_equal(history['reddid'], "alice")
        assert_equal(history['current_score'], 80)
        # SUSPECTED BUG: the history is looked for at or before the time of the
        # node, and the reputation that the peer sent is dated three days later
        assert_equal(history['history'], [])
        assert_equal(node.getuserreputationhistory("alice", 10, calculated)['history'], [{
            "timestamp": calculated,
            "overall_score": 80,
            "longevity_score": 80,
            "reliability_score": 80,
            "engagement_score": 80,
            "verification_score": 80,
            "auction_score": 80,
            "calculator_signatures": "0",
        }])
        assert_equal(node.getuserreputationhistory("carol"), {
            "reddid": "carol",
            "current_score": 50,
            "history": [dict(INITIAL_REPUTATION, timestamp=now, calculator_signatures="0")],
        })
        assert_raises_rpc_error(-8, "ReddID not found", node.getuserreputationhistory, "nobody")
        assert_raises_rpc_error(-8, "Count must be positive", node.getuserreputationhistory, "alice", 0)

        self.log.info("updateuserreputation lets any wallet set any score")
        # SUSPECTED BUG: the command checks nothing about the caller. It takes
        # a new key from the wallet, calls it the authorised calculator and
        # stores the scores.
        assert_equal(node.getaddressinfo(node.getuserreputation("bobby")['owner'])['ismine'], False)
        before = self.key_index(node)
        updated = node.updateuserreputation("bobby", {"overall_score": 99.5, "engagement_score": 12})
        assert_equal(self.key_index(node), before + 2)
        auth_address = updated.pop('auth_address')
        assert_equal(node.getaddressinfo(auth_address)['ismine'], True)
        assert_equal(updated, {
            "reddid": "bobby",
            "overall_score": 99.5,
            "previous_score": 50,
            "last_calculated": now,
            "updated": True,
        })
        stored = node.getuserreputation("bobby", True)
        assert_equal(stored['overall_score'], 99.5)
        assert_equal(stored['engagement_score'], 12)
        assert_equal(stored['auction_score'], 50)
        assert_equal(stored['last_calculated'], now)
        # SUSPECTED BUG: the address of the calculator can be given, and any
        # address will do: the wallet does not have to hold its key
        foreign = node.getuserreputation("carol")['owner']
        assert_equal(node.getaddressinfo(foreign)['ismine'], False)
        named = node.updateuserreputation("bobby", {"auction_score": 1}, foreign)
        assert_equal(named['auth_address'], foreign)
        assert_equal(named['previous_score'], 99.5)
        assert_equal(node.getuserreputation("bobby", True)['auction_score'], 1)
        # With the address given, the other optional arguments are not read
        node.updateuserreputation("bobby", {}, foreign, "account", "nonsense")
        assert_raises_rpc_error(-8, "Unknown address type", node.updateuserreputation, "bobby", {}, None, "account", "nonsense")
        assert_raises_rpc_error(-5, "Invalid auth address", node.updateuserreputation, "bobby", {}, "notanaddress")
        assert_raises_rpc_error(-8, "ReddID not found", node.updateuserreputation, "nobody", {})
        for component in ("overall_score",) + COMPONENTS:
            assert_raises_rpc_error(-8, component + " must be between 0 and 100", node.updateuserreputation, "bobby", {component: 100.5})
            assert_raises_rpc_error(-8, component + " must be between 0 and 100", node.updateuserreputation, "bobby", {component: -1})

        self.log.info("calculateuserreputation keeps a reputation that is less than an hour old")
        # The reputation of alice is dated in the future, which counts as recent
        before = self.key_index(node)
        kept = node.calculateuserreputation("alice")
        assert_equal(self.key_index(node), before + 2)
        assert_equal(node.getaddressinfo(kept.pop('calculator_address'))['ismine'], True)
        assert_equal(kept, {
            "reddid": "alice",
            "overall_score": 80,
            "previous_score": 80,
            "longevity_score": 80,
            "reliability_score": 80,
            "engagement_score": 80,
            "verification_score": 80,
            "auction_score": 80,
            "last_calculated": calculated,
            "recalculated": False,
        })
        assert_equal(node.calculateuserreputation("alice", False, "account", "legacy", "label")['recalculated'], False)
        assert_raises_rpc_error(-8, "Unknown address type", node.calculateuserreputation, "alice", False, "account", "nonsense")
        assert_raises_rpc_error(-8, "ReddID not found", node.calculateuserreputation, "nobody")

        self.log.info("calculateuserreputation calculates when it is forced to, or when the reputation is old")
        forced = node.calculateuserreputation("alice", True)
        assert_equal(forced['recalculated'], True)
        assert_equal(forced['previous_score'], 80)
        assert_equal(forced['last_calculated'], now)
        for component in ("overall_score",) + COMPONENTS:
            assert 0 <= forced[component] <= 100, forced
        # A profile that is seconds old with nothing to its name
        assert_equal(forced['longevity_score'], 0)
        assert_equal(forced['verification_score'], 0)
        # The result is stored
        stored = node.getuserreputation("alice", True)
        assert_equal(stored['last_calculated'], now)
        for component in ("overall_score",) + COMPONENTS:
            assert_equal(stored[component], forced[component])
        # An hour later the reputation of bobby, which was set by hand, is replaced
        assert_equal(node.calculateuserreputation("bobby")['recalculated'], False)
        advance_clock(self.nodes, 3600)
        replaced = node.calculateuserreputation("bobby")
        assert_equal(replaced['recalculated'], True)
        assert_equal(replaced['previous_score'], 99.5)
        assert_equal(replaced['last_calculated'], now + 3600)
        assert replaced['overall_score'] != 99.5, replaced

        self.log.info("The history is the current reputation and nothing else")
        # SUSPECTED BUG: one reputation is stored for a ReddID, and each new
        # one replaces it. The history is that one, if it is not dated after
        # the start time, so it never has more than one entry.
        assert_equal([entry['timestamp'] for entry in node.getuserreputationhistory("alice", 10, calculated)['history']], [now])
        assert_equal([entry['timestamp'] for entry in node.getuserreputationhistory("bobby")['history']], [now + 3600])
        assert_equal(node.getuserreputationhistory("bobby", 10, now)['history'], [])

        self.log.info("Profiles, connections and reputations survive a restart on the same clock")
        connections = node.getuserconnections("alice", None, "both")
        reputations = [self.hundredths(node.getuserreputation(name, True)) for name in OWNERS]
        self.restart_node(0, extra_args=keep_clock(node, self.extra_args[0]))
        assert_equal(node.getuserconnections("alice", None, "both"), connections)
        # Current behaviour: the scores are written in hundredths, with a
        # half rounded up, so a restart rounds what the node had calculated
        assert_equal([node.getuserreputation(name, True) for name in OWNERS], reputations)


if __name__ == '__main__':
    ReddIDSocialTest().main()
