#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ReddID namespace commands.

Two connected nodes run ReddID. Node 0 takes a namespace through its auction
with createnamespacesauction, bidonnamespacesauction and
finalizenamespacesauction, and the test checks every command that reads or
changes a namespace on the way.

None of these commands builds a transaction: each changes the state of the
node that it is called on, and nothing of it reaches node 1. That and the other
things that look wrong are asserted as they are, each under a comment that
starts with SUSPECTED BUG, so that the commit that fixes one finds its test.

The commands that only the creator of an auction or the owner of a namespace
may use ask the wallet for the key in the record: a wallet that holds it
passes, and one that does not hold it, or only watches its address, does not.
"""

from decimal import Decimal

from test_framework.descriptors import descsum_create
from test_framework.reddid import (
    DAY,
    advance_clock,
    keep_clock,
    run_expiry_task,
    second_wallet,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

AUCTION_SECONDS = 14 * DAY
NOT_OWNER = "You are not the owner of this namespace"
NOT_CREATOR = "You are not the creator of this auction"


class ReddIDNamespaceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = False  # Use cache with pre-generated blocks
        self.extra_args = [["-reddid=1", "-debug=reddid"], ["-reddid=1", "-debug=reddid"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # The commands take keys from a legacy wallet
        self.skip_if_no_bdb()

    def key_index(self, node):
        """The index of the next key of the wallet, which this call uses up."""
        path = node.getaddressinfo(node.getnewaddress("", "legacy"))['hdkeypath']
        return int(path.split('/')[-1].rstrip("'"))

    def run_test(self):
        node, other = self.nodes
        now = node.mocktime

        self.log.info("getreddidnetworkinfo reports the service and the peers that advertise it")
        info = node.getreddidnetworkinfo()
        assert_equal(info['enabled'], True)
        assert_equal(info['connections'], 1)
        assert_equal(info['peers'], [node.getpeerinfo()[0]['id']])

        self.log.info("There is no namespace to begin with")
        assert_equal(node.getnamespacelist(), [])
        assert_raises_rpc_error(-8, "Namespace not found", node.getnamespaceinfo, "redd")
        assert_equal(node.getnamespacesauctions(), [])
        assert_equal(node.getnamespaceauctionlist(), [])

        self.log.info("createnamespacesauction checks the name, the reserve price and the duration")
        assert_raises_rpc_error(-8, "Invalid namespace ID format", node.createnamespacesauction, "REDD", 50000, 14)
        assert_raises_rpc_error(-8, "Reserve price too low, minimum is 10000.00 RDD", node.createnamespacesauction, "redd", 9999, 14)
        assert_raises_rpc_error(-8, "Duration must be between 14 and 30 days", node.createnamespacesauction, "redd", 50000, 13)
        assert_raises_rpc_error(-8, "Duration must be between 14 and 30 days", node.createnamespacesauction, "redd", 50000, 31)
        assert_raises_rpc_error(-3, "Amount out of range", node.createnamespacesauction, "redd", -1, 14)

        self.log.info("createnamespacesauction creates an auction on this node, and announces it to no effect")
        # SUSPECTED BUG: the auction exists on the node that the command was
        # called on and nowhere else. The announcement reaches the peer, whose
        # handler can never accept one: it wants the namespace not to exist,
        # and the auction manager that it calls wants it to exist.
        with other.assert_debug_log(["Received namespace auction announcement for redd", "Namespace not found: redd"], timeout=30):
            auction = node.createnamespacesauction("redd", 50000, 14)
        auction_id = auction['auction_id']
        assert_equal(auction['namespace_id'], "redd")
        assert_equal(auction['start_time'], now)
        assert_equal(auction['end_time'], now + AUCTION_SECONDS)
        assert_equal(auction['reserve_price'], Decimal("50000"))
        assert_equal(auction['auction_type'], 0)
        creator = auction['creator_address']
        assert_equal(node.getaddressinfo(creator)['ismine'], True)
        assert_equal(other.getnamespacesauctions(), [])
        assert_raises_rpc_error(-8, "Auction not found", other.getnamespaceauctioninfo, auction_id)
        assert_raises_rpc_error(-8, "Auction not found", other.bidonnamespacesauction, auction_id, 60000)

        self.log.info("The optional arguments are read from the positions that the help gives them")
        # These three calls failed before the positions were corrected: the
        # account was read as the address type as well
        second = node.createnamespacesauction("second", 50000, 30, 1, "account")
        assert_equal(second['auction_type'], 1)
        assert_equal(second['end_time'], now + 30 * DAY)
        third = node.createnamespacesauction("third", 50000, 14, 0, "account", "legacy", "label")
        bech32 = node.createnamespacesauction("fourth", 50000, 14, 0, "account", "bech32")
        assert bech32['creator_address'].startswith('r'), bech32
        assert_raises_rpc_error(-8, "Unknown address type", node.createnamespacesauction, "fifth", 50000, 14, 0, "account", "nonsense")
        assert_raises_rpc_error(-1, "JSON value is not a string as expected", node.createnamespacesauction, "fifth", 50000, 14, 0, "account", "legacy", 7)

        self.log.info("Within the same second, a second call for a namespace overwrites its auction")
        # SUSPECTED BUG: the command does not look for an auction that the
        # namespace already has. The ID of an auction is the hash of the
        # namespace and the start time, so a call in the second in which the
        # auction started makes the same ID and writes a new auction over the
        # old one: with the new key as its creator, pending, and with no
        # current bid. A bid that the old one had stays in the database and
        # is still listed. A call in a later second creates a second auction
        # (see below).
        again = node.createnamespacesauction("redd", 50000, 14)
        assert_equal(again['auction_id'], auction_id)
        assert_equal(again['start_time'], now)
        assert again['creator_address'] != creator
        creator = again['creator_address']
        fourth_id = bech32['auction_id']
        lost_bid = node.bidonnamespacesauction(fourth_id, 50000)
        info = node.getnamespaceauctioninfo(fourth_id, True)
        assert_equal((info['state'], info['current_bid'], info['current_bidder']), (1, Decimal("50000"), lost_bid['bidder_address']))
        assert_equal([bid['bid_id'] for bid in info['bids']], [lost_bid['bid_id']])
        assert_equal(node.createnamespacesauction("fourth", 50000, 14)['auction_id'], fourth_id)
        info = node.getnamespaceauctioninfo(fourth_id, True)
        assert_equal((info['state'], info['current_bid'], info['current_bidder']), (0, Decimal("0"), ""))
        assert_equal([bid['bid_id'] for bid in info['bids']], [lost_bid['bid_id']])
        assert_equal(node.getnamespacesauctionbids(fourth_id)['total_bids'], 1)

        self.log.info("The two listing commands are copies of each other")
        listed = node.getnamespacesauctions()
        assert_equal(node.getnamespaceauctionlist(), listed)
        assert_equal(sorted(entry['namespace_id'] for entry in listed), ["fourth", "redd", "second", "third"])
        entry = next(entry for entry in listed if entry['namespace_id'] == "redd")
        assert_equal(entry, {
            "auction_id": auction_id,
            "namespace_id": "redd",
            "creator": creator,
            "start_time": now,
            "end_time": now + AUCTION_SECONDS,
            "reserve_price": Decimal("50000"),
            "current_bid": Decimal("0"),
            "current_bidder": "",
            "state": 0,
            "type": 0,
        })
        # An auction is pending until it has a bid, and only then active
        assert_equal(node.getnamespacesauctions(True), [])
        assert_equal(node.getnamespaceauctionlist(True), [])

        self.log.info("getnamespaceauctioninfo")
        info = node.getnamespaceauctioninfo(auction_id)
        assert_equal(info['state'], 0)
        assert_equal(info['deposit_amount'], Decimal("10000"))
        assert_equal(info['time_remaining'], 0)
        assert 'bids' not in info
        assert_equal(node.getnamespaceauctioninfo(auction_id, True)['bids'], [])
        assert_raises_rpc_error(-8, "Auction not found", node.getnamespaceauctioninfo, "00" * 32)
        assert_raises_rpc_error(-8, "auction_id must be of length 64", node.getnamespaceauctioninfo, "abcd")

        self.log.info("bidonnamespacesauction places a bid with no transaction and no deposit")
        # SUSPECTED BUG: nothing is paid and nothing is locked, and the bid
        # exists on this node only
        assert_raises_rpc_error(-8, "Auction not found", node.bidonnamespacesauction, "00" * 32, 60000)
        assert_raises_rpc_error(-1, "Failed to place bid", node.bidonnamespacesauction, auction_id, 49999)
        balance = node.getbalance()
        first_bid = node.bidonnamespacesauction(auction_id, 60000)
        assert_equal(first_bid['auction_id'], auction_id)
        assert_equal(first_bid['bid_amount'], Decimal("60000"))
        assert_equal(first_bid['deposit_amount'], Decimal("10000"))
        assert_equal(first_bid['bid_time'], now)
        assert_equal(node.getbalance(), balance)
        assert_equal(node.getrawmempool(), [])
        # A bid must be higher than the last by the minimum increment of 5%
        assert_raises_rpc_error(-1, "Failed to place bid", node.bidonnamespacesauction, auction_id, 62999)
        # The account is no longer read as the address type
        second_bid = node.bidonnamespacesauction(auction_id, 63000, "account")
        third_bid = node.bidonnamespacesauction(auction_id, 70000, "account", "legacy", "label")
        assert_raises_rpc_error(-8, "Unknown address type", node.bidonnamespacesauction, auction_id, 80000, "account", "nonsense")
        winner = third_bid['bidder_address']

        self.log.info("The auction is active once it has a bid")
        active = node.getnamespacesauctions(True)
        assert_equal(node.getnamespaceauctionlist(True), active)
        assert_equal([entry['namespace_id'] for entry in active], ["redd"])
        assert_equal(active[0]['state'], 1)
        assert_equal(active[0]['current_bid'], Decimal("70000"))
        assert_equal(active[0]['current_bidder'], winner)
        assert_equal(node.getnamespaceauctioninfo(auction_id)['time_remaining'], AUCTION_SECONDS)

        self.log.info("getnamespacesauctionbids sorts by time or by amount")
        bids = node.getnamespacesauctionbids(auction_id, "amount", "asc")
        assert_equal(bids['auction_id'], auction_id)
        assert_equal(bids['namespace_id'], "redd")
        assert_equal(bids['total_bids'], 3)
        assert_equal(bids['highest_bid'], Decimal("70000"))
        assert_equal([bid['bid_id'] for bid in bids['bids']], [first_bid['bid_id'], second_bid['bid_id'], third_bid['bid_id']])
        assert_equal(bids['bids'][0], {
            "bid_id": first_bid['bid_id'],
            "bidder": first_bid['bidder_address'],
            "amount": Decimal("60000"),
            "deposit": Decimal("10000"),
            "time": now,
            "is_winner": False,
            "refunded": False,
        })
        descending = node.getnamespacesauctionbids(auction_id, "amount", "desc")
        assert_equal([bid['amount'] for bid in descending['bids']], [Decimal("70000"), Decimal("63000"), Decimal("60000")])
        assert_equal(len(node.getnamespacesauctionbids(auction_id)['bids']), 3)
        assert_equal(len(node.getnamespacesauctionbids(auction_id, "time", "asc")['bids']), 3)
        assert_raises_rpc_error(-8, "sort_by must be 'time' or 'amount'", node.getnamespacesauctionbids, auction_id, "size")
        assert_raises_rpc_error(-8, "sort_dir must be 'asc' or 'desc'", node.getnamespacesauctionbids, auction_id, "time", "up")
        assert_raises_rpc_error(-8, "Auction not found", node.getnamespacesauctionbids, "00" * 32)
        verbose = node.getnamespaceauctioninfo(auction_id, True)['bids']
        assert_equal(sorted(bid['amount'] for bid in verbose), [Decimal("60000"), Decimal("63000"), Decimal("70000")])

        self.log.info("cancelnamespacesauction is for the wallet that holds the key of the creator")
        assert_raises_rpc_error(-8, "Auction not found", node.cancelnamespacesauction, "00" * 32)
        fifth = node.createnamespacesauction("fifth", 50000, 14)
        # A wallet with no key of the auction is refused, and so is one that
        # only watches the address of the creator
        with second_wallet(node, "stranger") as stranger:
            assert_raises_rpc_error(-1, NOT_CREATOR, stranger.cancelnamespacesauction, fifth['auction_id'])
            stranger.importaddress(fifth['creator_address'], "", False)
            assert_equal(stranger.getaddressinfo(fifth['creator_address'])['iswatchonly'], True)
            assert_raises_rpc_error(-1, NOT_CREATOR, stranger.cancelnamespacesauction, fifth['auction_id'])
            assert_equal(node.getnamespaceauctioninfo(fifth['auction_id'])['state'], 0)
        # The wallet of the creator passes with the key that it holds, and no
        # call takes a key from the wallet. An auction with bids cannot be
        # cancelled: the command has one error for that and for every other
        # refusal of the namespace manager.
        before = self.key_index(node)
        assert_raises_rpc_error(-1, "Failed to cancel auction", node.cancelnamespacesauction, auction_id)
        assert_equal(node.getnamespaceauctioninfo(auction_id)['state'], 1)
        # SUSPECTED BUG: no transaction cancels the auction. It is cancelled
        # on this node, and a node that had it would be told by a message
        # that it accepts from any peer.
        assert_equal(node.cancelnamespacesauction(fifth['auction_id'], "account"), {
            "auction_id": fifth['auction_id'],
            "namespace_id": "fifth",
            "state": 8,
        })
        assert_equal(node.getnamespaceauctioninfo(fifth['auction_id'])['state'], 8)
        assert_equal(node.getrawmempool(), [])
        assert_equal(self.key_index(node), before + 1)
        assert_raises_rpc_error(-8, "Auction is not active", node.bidonnamespacesauction, fifth['auction_id'], 60000)
        # The command has two arguments
        assert_raises_rpc_error(-1, "cancelnamespacesauction \"auction_id\" ( \"account\" )", node.cancelnamespacesauction, second['auction_id'], "account", "legacy")
        assert_equal(node.getnamespaceauctioninfo(second['auction_id'])['state'], 0)

        self.log.info("finalizenamespacesauction waits for the end of the auction")
        assert_raises_rpc_error(-8, "Auction not found", node.finalizenamespacesauction, "00" * 32)
        assert_raises_rpc_error(-8, "Auction has not ended yet, %d seconds remaining" % AUCTION_SECONDS, node.finalizenamespacesauction, auction_id)
        # An auction with no bid is pending, which the command does not accept
        assert_raises_rpc_error(-8, "Auction is not active or ended", node.finalizenamespacesauction, second['auction_id'])

        self.log.info("A second later, a second call for a namespace creates a second auction for it")
        # SUSPECTED BUG: the start time is another one, so the ID is, and the
        # namespace now has two auctions. The first keeps its bids.
        advance_clock(self.nodes, 1)
        rival = node.createnamespacesauction("redd", 50000, 14)
        assert rival['auction_id'] != auction_id
        assert_equal(rival['start_time'], now + 1)
        for_redd = {entry['auction_id']: entry for entry in node.getnamespacesauctions() if entry['namespace_id'] == "redd"}
        assert_equal(sorted(for_redd), sorted([auction_id, rival['auction_id']]))
        assert_equal((for_redd[auction_id]['state'], for_redd[auction_id]['current_bid'], for_redd[auction_id]['creator']), (1, Decimal("70000"), creator))
        assert_equal((for_redd[rival['auction_id']]['state'], for_redd[rival['auction_id']]['current_bid'], for_redd[rival['auction_id']]['creator']), (0, Decimal("0"), rival['creator_address']))
        assert_equal(node.getnamespacesauctionbids(auction_id)['total_bids'], 3)
        assert_equal(node.getnamespacesauctionbids(rival['auction_id'])['total_bids'], 0)

        self.log.info("After its end the auction is finalized and the namespace belongs to the key of the highest bid")
        # The periodic task leaves namespace auctions alone (see below), so
        # these steps do not depend on when it runs
        run_expiry_task(node)
        advance_clock(self.nodes, AUCTION_SECONDS - 1)
        end = node.mocktime
        assert_equal(end, now + AUCTION_SECONDS)
        result = node.finalizenamespacesauction(auction_id)
        assert_equal(result, {
            "auction_id": auction_id,
            "namespace_id": "redd",
            "winner": winner,
            "final_price": Decimal("70000"),
            "state": 3,
        })
        assert_raises_rpc_error(-8, "Auction is not active or ended", node.finalizenamespacesauction, auction_id)
        assert_raises_rpc_error(-8, "Auction is not active", node.bidonnamespacesauction, auction_id, 90000)
        info = node.getnamespaceauctioninfo(auction_id, True)
        assert_equal(info['state'], 3)
        assert_equal(info['time_remaining'], 0)
        assert_equal([bid['is_winner'] for bid in info['bids'] if bid['bid_id'] == third_bid['bid_id']], [True])
        assert_equal(sum(bid['is_winner'] for bid in info['bids']), 1)
        assert_equal(node.getnamespacesauctions(True), [])
        assert_raises_rpc_error(-8, "Namespace already exists", node.createnamespacesauction, "redd", 50000, 14)

        self.log.info("getnamespaceinfo and getnamespacelist")
        namespace = node.getnamespaceinfo("redd")
        tiers = namespace.pop('pricing_tiers')
        assert_equal(namespace, {
            "id": "redd",
            "owner": winner,
            "allow_numbers": True,
            "allow_hyphens": True,
            "allow_underscores": True,
            "min_length": 3,
            "max_length": 32,
            "renewal_period": 365,
            "grace_period": 30,
            "namespace_revenue_pct": 10,
            "burn_pct": 70,
            "node_pct": 15,
            "dev_pct": 5,
            "min_auction_duration": 3,
            "max_auction_duration": 7,
            "min_bid_increment": 5,
            "last_updated": end,
            "expiration": end + 365 * DAY,
        })
        assert_equal(tiers, [
            {"min_length": 1, "min_price": Decimal("100000")},
            {"min_length": 2, "min_price": Decimal("50000")},
            {"min_length": 3, "min_price": Decimal("25000")},
            {"min_length": 4, "min_price": Decimal("10000")},
        ])
        assert_equal(node.getnamespacelist(), [namespace])
        assert_equal(node.getuseridlist("redd"), [])

        self.log.info("The namespace exists on node 0 only")
        # SUSPECTED BUG: no transaction created it and no message announces
        # it, so no other node ever learns of it
        other.syncwithvalidationinterfacequeue()
        assert_equal(other.getnamespacelist(), [])
        assert_raises_rpc_error(-8, "Namespace not found", other.getnamespaceinfo, "redd")

        self.log.info("configurenamespacess, renewnamespace and transfernamespace are for the wallet that holds the key of the owner")
        assert_equal(node.getaddressinfo(winner)['ismine'], True)
        target = other.getnewaddress("", "legacy")
        with second_wallet(node, "watcher") as stranger:
            for watching in (False, True):
                if watching:
                    stranger.importaddress(winner, "", False)
                assert_raises_rpc_error(-1, NOT_OWNER, stranger.configurenamespacess, "redd", {"min_length": 4})
                assert_raises_rpc_error(-1, NOT_OWNER, stranger.renewnamespace, "redd")
                assert_raises_rpc_error(-1, NOT_OWNER, stranger.transfernamespace, "redd", target)
        # A wallet without private keys that knows the address of the owner
        # calls it its own, and holds no key
        if self.is_sqlite_compiled():
            node.createwallet(wallet_name="keyless", descriptors=True, disable_private_keys=True)
            keyless = node.get_wallet_rpc("keyless")
            assert_equal(keyless.importdescriptors([{"desc": descsum_create("addr(%s)" % winner), "timestamp": "now"}])[0]['success'], True)
            assert_equal(keyless.getaddressinfo(winner)['ismine'], True)
            assert_raises_rpc_error(-1, NOT_OWNER, keyless.configurenamespacess, "redd", {"min_length": 4})
            assert_raises_rpc_error(-1, NOT_OWNER, keyless.renewnamespace, "redd")
            # transfernamespace is for legacy wallets, which it says first
            assert_raises_rpc_error(-4, "This type of wallet does not support this command", keyless.transfernamespace, "redd", target)
            keyless.unloadwallet()
        assert_raises_rpc_error(-8, "Namespace not found", node.configurenamespacess, "nothere", {})
        assert_raises_rpc_error(-8, "Namespace not found", node.renewnamespace, "nothere")
        assert_raises_rpc_error(-8, "Namespace not found", node.transfernamespace, "nothere", target)
        assert_raises_rpc_error(-5, "Invalid destination address", node.transfernamespace, "redd", "notanaddress")
        # The address type and the label were for the key that the commands
        # used to take from the wallet. The address type is still checked.
        # That of configurenamespacess is the fifth argument.
        assert_raises_rpc_error(-8, "Unknown address type", node.configurenamespacess, "redd", {}, None, "account", "nonsense")
        assert_raises_rpc_error(-8, "Unknown address type", node.renewnamespace, "redd", "account", "nonsense")
        assert_raises_rpc_error(-8, "Unknown address type", node.transfernamespace, "redd", target, "account", "nonsense")
        # Nothing changed
        namespace['pricing_tiers'] = tiers
        assert_equal(node.getnamespaceinfo("redd"), namespace)

        self.log.info("configurenamespacess changes the configuration and replaces the pricing tiers")
        before = self.key_index(node)
        balance = node.getbalance()
        # What the command refuses changes nothing
        assert_raises_rpc_error(-8, "min_length must be between 1 and 10", node.configurenamespacess, "redd", {"min_length": 11})
        assert_raises_rpc_error(-8, "Revenue percentages must sum to 100% (current: 105%)", node.configurenamespacess, "redd", {"burn_pct": 75})
        assert_raises_rpc_error(-8, "min_bid_increment must be between 1.0 and 10.0", node.configurenamespacess, "redd", {"min_bid_increment": 10.5})
        assert_raises_rpc_error(-8, "min_price must be positive", node.configurenamespacess, "redd", {"min_length": 4}, [{"min_length": 2, "min_price": 30000}, {"min_length": 1, "min_price": 0}])
        assert_raises_rpc_error(-8, "Pricing tier must have min_length and min_price", node.configurenamespacess, "redd", {"min_length": 4}, [{"min_length": 2}])
        assert_raises_rpc_error(-8, "More than one pricing tier with min_length 2", node.configurenamespacess, "redd", {"min_length": 4}, [{"min_length": 2, "min_price": 30000}, {"min_length": 3, "min_price": 20000}, {"min_length": 2, "min_price": 40000}])
        assert_equal(node.getnamespaceinfo("redd"), namespace)
        # A call that changes nothing returns the configuration
        assert_equal(node.configurenamespacess("redd", {}), namespace)
        advance_clock(self.nodes, 10)
        # SUSPECTED BUG: no transaction carries the change and nothing is
        # paid for it. The configuration changes on this node alone: the
        # other node does not have the namespace, and is not given it.
        configured = node.configurenamespacess("redd", {"allow_hyphens": False, "min_length": 4, "burn_pct": 65, "node_pct": 20, "min_bid_increment": 7.55}, [{"min_length": 2, "min_price": 30000}, {"min_length": 6, "min_price": 2000}], "account", "legacy", "label")
        namespace.update({
            "allow_hyphens": False,
            "min_length": 4,
            "burn_pct": 65,
            "node_pct": 20,
            "min_bid_increment": Decimal("7.55"),
            "last_updated": node.mocktime,
            "pricing_tiers": [
                {"min_length": 2, "min_price": Decimal("30000")},
                {"min_length": 6, "min_price": Decimal("2000")},
            ],
        })
        assert_equal(configured, namespace)
        assert_equal(node.getnamespaceinfo("redd"), namespace)
        # The tiers alone
        tiers = [{"min_length": 1, "min_price": Decimal("90000")}, {"min_length": 3, "min_price": Decimal("20000")}, {"min_length": 6, "min_price": Decimal("3000")}]
        namespace['pricing_tiers'] = tiers
        assert_equal(node.configurenamespacess("redd", {}, tiers), namespace)
        assert_equal(node.getnamespaceinfo("redd"), namespace)
        assert_equal(node.getbalance(), balance)
        assert_equal(node.getrawmempool(), [])
        other.syncwithvalidationinterfacequeue()
        assert_equal(other.getnamespacelist(), [])

        self.log.info("renewnamespace adds the renewal period to the expiration")
        # SUSPECTED BUG: the command reports a renewal fee, and nothing is
        # paid: there is no transaction. The namespace is renewed on this
        # node alone, as often as the owner likes.
        advance_clock(self.nodes, 10)
        fee = Decimal("2250")  # A quarter of the price of a namespace of four characters, less 10%
        assert_equal(node.renewnamespace("redd"), {
            "id": "redd",
            "owner": winner,
            "previous_expiration": end + 365 * DAY,
            "new_expiration": end + 2 * 365 * DAY,
            "renewal_fee": fee,
        })
        assert_equal(node.renewnamespace("redd", "account", "legacy", "label"), {
            "id": "redd",
            "owner": winner,
            "previous_expiration": end + 2 * 365 * DAY,
            "new_expiration": end + 3 * 365 * DAY,
            "renewal_fee": fee,
        })
        namespace.update({"last_updated": node.mocktime, "expiration": end + 3 * 365 * DAY})
        assert_equal(node.getnamespaceinfo("redd"), namespace)
        assert_equal(node.getbalance(), balance)
        assert_equal(node.getrawmempool(), [])

        self.log.info("transfernamespace gives the namespace to another key")
        advance_clock(self.nodes, 10)
        assert_raises_rpc_error(-1, "Failed to transfer namespace", node.transfernamespace, "redd", winner)
        # SUSPECTED BUG: again with no transaction, on this node alone. The
        # wallet of the other node holds the key of the new owner, and that
        # node does not have the namespace.
        assert_equal(node.transfernamespace("redd", target, "account", "legacy", "label"), {
            "id": "redd",
            "previous_owner": winner,
            "new_owner": target,
            "transfer_time": node.mocktime,
        })
        namespace.update({"owner": target, "last_updated": node.mocktime})
        assert_equal(node.getnamespaceinfo("redd"), namespace)
        assert_equal(self.key_index(node), before + 1)
        assert_equal(other.getaddressinfo(target)['ismine'], True)
        assert_raises_rpc_error(-8, "Namespace not found", other.renewnamespace, "redd")
        # The wallet that gave the namespace away is no longer its owner
        assert_raises_rpc_error(-1, NOT_OWNER, node.configurenamespacess, "redd", {"min_length": 3})
        assert_raises_rpc_error(-1, NOT_OWNER, node.renewnamespace, "redd")
        assert_raises_rpc_error(-1, NOT_OWNER, node.transfernamespace, "redd", winner)
        assert_equal(node.getnamespaceinfo("redd"), namespace)

        self.log.info("The periodic task leaves namespace auctions alone")
        # The auction for "third" has ended with no bid. The task cancels a
        # user ID auction in that state (see rpc_reddid_auction.py); a
        # namespace auction stays pending for good.
        assert_equal(node.getnamespaceauctioninfo(third['auction_id'])['end_time'], end)
        assert node.mocktime > end
        run_expiry_task(node)
        assert_equal(node.getnamespaceauctioninfo(third['auction_id'])['state'], 0)
        assert_equal(node.getnamespaceauctioninfo(second['auction_id'])['state'], 0)

        self.log.info("The namespace and the auctions survive a restart on the same clock")
        auctions = node.getnamespacesauctions()
        self.restart_node(0, extra_args=keep_clock(node, self.extra_args[0]))
        assert_equal(node.getnamespaceinfo("redd"), namespace)
        assert_equal(node.getnamespacesauctions(), auctions)
        assert_equal(node.getnamespacesauctionbids(auction_id)['total_bids'], 3)


if __name__ == '__main__':
    ReddIDNamespaceTest().main()
