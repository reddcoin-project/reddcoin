#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ReddID user ID commands and the auction configuration.

One node runs ReddID and takes a namespace through its auction. The test then
follows user ID auctions made with createuseridauction, bidonuseridauction and
finalizeuseridauction, which change the state of the node with no transaction,
and checks the commands that list and show auctions, bids and user IDs, the
commands for the owner of a user ID, the periodic task and
reddidgetauctionconfig and reddidsetauctionconfig.

The auctions with deposits, which do use transactions, are in
rpc_reddid_deposit.py. What looks wrong is asserted as it is, under a comment
that starts with SUSPECTED BUG.
"""

from decimal import Decimal

from test_framework.reddid import (
    DAY,
    acquire_namespace,
    advance_clock,
    expiry_task_idle,
    keep_clock,
    run_expiry_task,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

HOUR = 60 * 60
CREATE_FAILED = "Failed to create user ID auction"
NOT_OWNER = "You are not the owner of this user ID"

USERID_DEFAULTS = {
    "config_id": "userid",
    "deposit_percentage": 15,
    "settlement_window_blocks": 4320,
    "anti_snipe_threshold_seconds": 86400,
    "anti_snipe_extension_seconds": 21600,
    "min_duration_days": 3,
    "max_duration_days": 7,
    "min_bid_increment_pct": 5,
    "community_fund_address": "",
}


class ReddIDAuctionTest(BitcoinTestFramework):
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

    def run_test(self):
        node = self.nodes[0]

        self.log.info("A user ID auction needs a namespace")
        assert_raises_rpc_error(-8, "Namespace not found", node.createuseridauction, "alice", "redd", 10000, 7)
        assert_raises_rpc_error(-8, "Namespace not found", node.getuseridlist, "redd")
        assert_raises_rpc_error(-8, "User ID not found", node.getuseridinfo, "alice", "redd")
        namespace_owner = acquire_namespace(self.nodes, node, "redd")
        now = node.mocktime

        self.log.info("createuseridauction checks the name, the reserve price and the duration")
        # The name must have the minimum length of the namespace, 3
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "ab", "redd", 50000, 7)
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "Alice", "redd", 10000, 7)
        # The lowest price for a name of four characters or more is 10000, for one of three 25000
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "alice", "redd", 9999, 7)
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "bob", "redd", 24999, 7)
        # The namespace allows auctions of 3 to 7 days
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "alice", "redd", 10000, 2)
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "alice", "redd", 10000, 8)

        self.log.info("createuseridauction creates an auction with no transaction")
        # Before the positions of the optional arguments were corrected this
        # call, like every call of the command, failed: the reserve price was
        # read as the address type and is not a string
        auction = node.createuseridauction("alice", "redd", 10000, 7)
        auction_id = auction.pop('auction_id')
        creator = auction.pop('creator_address')
        assert_equal(auction, {
            "name": "alice",
            "namespace_id": "redd",
            "start_time": now,
            "end_time": now + 7 * DAY,
            "reserve_price": Decimal("10000"),
            "auction_type": 0,
        })
        assert_equal(node.getaddressinfo(creator)['ismine'], True)
        assert_equal(node.getrawmempool(), [])
        # The same name at the same second is the same auction
        assert_raises_rpc_error(-1, CREATE_FAILED, node.createuseridauction, "alice", "redd", 10000, 7)

        self.log.info("The optional arguments are read from the positions that the help gives them")
        bob = node.createuseridauction("bob", "redd", 25000, 3, 1, "account", "legacy", "label")
        assert_equal(bob['auction_type'], 1)
        assert_equal(bob['end_time'], now + 3 * DAY)
        node.createuseridauction("carol", "redd", 10000, 5, 0, "account")
        assert_raises_rpc_error(-8, "Unknown address type", node.createuseridauction, "dave", "redd", 10000, 7, 0, "account", "nonsense")

        self.log.info("A second auction for the same name can be created a second later")
        # SUSPECTED BUG: the ID of an auction is made of the name, the
        # namespace and the start time, and nothing looks for another auction
        # for the same name
        advance_clock(self.nodes, 1)
        twin = node.createuseridauction("alice", "redd", 10000, 7)
        assert twin['auction_id'] != auction_id
        assert_equal(twin['start_time'], now + 1)

        self.log.info("getuserauctionlist and getuseridauctions are copies of each other")
        listed = node.getuserauctionlist()
        assert_equal(node.getuseridauctions(), listed)
        assert_equal(sorted(entry['name'] for entry in listed), ["alice", "alice", "bob", "carol"])
        entry = next(entry for entry in listed if entry['auction_id'] == auction_id)
        assert_equal(entry, {
            "auction_id": auction_id,
            "name": "alice",
            "namespace_id": "redd",
            "creator": creator,
            "start_time": now,
            "end_time": now + 7 * DAY,
            "reserve_price": Decimal("10000"),
            "current_bid": Decimal("0"),
            "current_bidder": "",
            "state": 0,
            "type": 0,
            "time_remaining": 0,
        })
        assert_equal(node.getuserauctionlist("redd"), listed)
        assert_equal(node.getuseridauctions("redd", False), listed)
        assert_equal(node.getuserauctionlist("other"), [])
        # Pending auctions are not active
        for command in (node.getuserauctionlist, node.getuseridauctions):
            assert_equal(command("redd", True), [])
            assert_equal(command("", True), [])

        self.log.info("getuserauctioninfo")
        info = node.getuserauctioninfo(auction_id)
        assert_equal(info['state'], 0)
        assert_equal(info['deposit_amount'], Decimal("1000"))
        assert_equal(info['namespace_info'], {
            "id": "redd",
            "owner": namespace_owner,
            "allow_numbers": True,
            "allow_hyphens": True,
            "allow_underscores": True,
            "min_length": 3,
            "max_length": 32,
            "renewal_period": 365,
            "grace_period": 30,
        })
        assert 'bids' not in info
        assert_equal(node.getuserauctioninfo(auction_id, True)['bids'], [])
        assert_raises_rpc_error(-8, "Auction not found", node.getuserauctioninfo, "00" * 32)
        # The two kinds of auction are told apart by their name
        namespace_auction = node.getnamespacesauctions()[0]['auction_id']
        assert_raises_rpc_error(-8, "This is not a user ID auction", node.getuserauctioninfo, namespace_auction)
        assert_raises_rpc_error(-8, "This is not a user ID auction", node.getuseridauctionbids, namespace_auction)
        assert_raises_rpc_error(-8, "This is not a namespace auction", node.getnamespaceauctioninfo, auction_id)
        assert_raises_rpc_error(-8, "This is not a namespace auction", node.getnamespacesauctionbids, auction_id)

        self.log.info("bidonuseridauction places a bid with no transaction and no deposit")
        # SUSPECTED BUG: nothing is paid and nothing is locked
        assert_raises_rpc_error(-8, "Auction not found", node.bidonuseridauction, "00" * 32, 10000)
        assert_raises_rpc_error(-1, "Failed to place bid", node.bidonuseridauction, auction_id, 9999)
        balance = node.getbalance()
        first_bid = node.bidonuseridauction(auction_id, 10000)
        assert_equal(first_bid['auction_id'], auction_id)
        assert_equal(first_bid['bid_amount'], Decimal("10000"))
        assert_equal(first_bid['deposit_amount'], Decimal("1000"))
        assert_equal(first_bid['bid_time'], now + 1)
        assert_equal(node.getbalance(), balance)
        assert_equal(node.getrawmempool(), [])
        # The next bid must be 5% higher. The node computes the minimum in
        # floating point, and a build with extended-precision evaluation
        # (32-bit x86) gets 10499.99999999 where the others get 10500, so the
        # bid that is refused here is two base units short.
        assert_raises_rpc_error(-1, "Failed to place bid", node.bidonuseridauction, auction_id, "10499.99999998")
        # The account is no longer read as the address type
        second_bid = node.bidonuseridauction(auction_id, 10500, "account")
        assert_raises_rpc_error(-8, "Unknown address type", node.bidonuseridauction, auction_id, 20000, "account", "nonsense")
        third_bid = node.bidonuseridauction(auction_id, 12000, "account", "legacy", "label")

        self.log.info("An auction with a bid is active")
        for command in (node.getuserauctionlist, node.getuseridauctions):
            for namespace in ("redd", ""):
                active = command(namespace, True)
                assert_equal([entry['auction_id'] for entry in active], [auction_id])
                assert_equal(active[0]['state'], 1)
                assert_equal(active[0]['current_bid'], Decimal("12000"))
                assert_equal(active[0]['current_bidder'], third_bid['bidder_address'])
                assert_equal(active[0]['time_remaining'], 7 * DAY - 1)

        self.log.info("getuseridauctionbids sorts by time or by amount")
        bids = node.getuseridauctionbids(auction_id, "amount", "asc")
        assert_equal(bids['name'], "alice")
        assert_equal(bids['namespace_id'], "redd")
        assert_equal(bids['total_bids'], 3)
        assert_equal(bids['highest_bid'], Decimal("12000"))
        assert_equal([bid['bid_id'] for bid in bids['bids']], [first_bid['bid_id'], second_bid['bid_id'], third_bid['bid_id']])
        assert_equal(bids['bids'][0], {
            "bid_id": first_bid['bid_id'],
            "bidder": first_bid['bidder_address'],
            "amount": Decimal("10000"),
            "deposit": Decimal("1000"),
            "time": now + 1,
            "is_winner": False,
            "refunded": False,
        })
        descending = node.getuseridauctionbids(auction_id, "amount", "desc")
        assert_equal([bid['amount'] for bid in descending['bids']], [Decimal("12000"), Decimal("10500"), Decimal("10000")])
        assert_equal(len(node.getuseridauctionbids(auction_id)['bids']), 3)
        assert_raises_rpc_error(-8, "sort_by must be 'time' or 'amount'", node.getuseridauctionbids, auction_id, "size")
        assert_raises_rpc_error(-8, "sort_dir must be 'asc' or 'desc'", node.getuseridauctionbids, auction_id, "time", "up")
        assert_equal(len(node.getuserauctioninfo(auction_id, True)['bids']), 3)

        self.log.info("canceluseridauction does not accept the key that created the auction")
        # SUSPECTED BUG: the command compares the creator of the auction with
        # a key that it takes from the wallet for this call, the next unused
        # one. A wallet can therefore never pass with a key that it has
        # already been given, which the creator is. Every call uses up a key.
        # (Not tried: a creator that is the next key of the wallet, which
        # another wallet with the same seed could have used.)
        # The auction for bob has no bid and its state would allow the
        # cancellation. The command has one error for every refusal;
        # ReddIDManager::CancelUserIDAuction, which compares the keys first,
        # logs nothing, so the test asserts that the call does not get as far
        # as AuctionManager::CancelAuction, which logs when it starts.
        before = self.key_index(node)
        with node.assert_debug_log([], unexpected_msgs=["Canceling auction " + bob['auction_id']]):
            assert_raises_rpc_error(-1, "Failed to cancel auction", node.canceluseridauction, bob['auction_id'])
            assert_raises_rpc_error(-1, "Failed to cancel auction", node.canceluseridauction, bob['auction_id'], "account", "legacy", "label")
        assert_equal(self.key_index(node), before + 3)
        assert_raises_rpc_error(-8, "Unknown address type", node.canceluseridauction, bob['auction_id'], "account", "nonsense")
        assert_raises_rpc_error(-8, "Auction not found", node.canceluseridauction, "00" * 32)
        assert_equal(node.getuserauctioninfo(bob['auction_id'])['state'], 0)

        self.log.info("A bid in the last day of an auction moves its end to six hours after the bid")
        assert_raises_rpc_error(-8, "Auction has not ended yet, %d seconds remaining" % (7 * DAY - 1), node.finalizeuseridauction, auction_id)
        run_expiry_task(node)
        # From here to the next run of the periodic task the test needs the
        # task not to run: it would cancel the auctions for bob and carol,
        # which end with no bid when the clock moves, and finalize the
        # auction for alice once that has ended
        with expiry_task_idle(node):
            advance_clock(self.nodes, 6 * DAY + 2 * HOUR)
            late = node.mocktime
            assert_equal(node.getuserauctioninfo(auction_id)['time_remaining'], now + 7 * DAY - late)
            # SUSPECTED BUG: the end is set to six hours after the bid although
            # the auction had almost 22 hours left: the bid shortens the auction
            fourth_bid = node.bidonuseridauction(auction_id, 13000)
            info = node.getuserauctioninfo(auction_id)
            assert_equal(info['end_time'], late + 6 * HOUR)
            assert info['end_time'] < now + 7 * DAY
            assert_equal(info['time_remaining'], 6 * HOUR)

            self.log.info("finalizeuseridauction registers the user ID to the key of the highest bid")
            assert_raises_rpc_error(-8, "Auction not found", node.finalizeuseridauction, "00" * 32)
            assert_raises_rpc_error(-8, "Auction is not active or ended", node.finalizeuseridauction, twin['auction_id'])
            advance_clock(self.nodes, 6 * HOUR)
            end = node.mocktime
            assert_raises_rpc_error(-1, "Failed to place bid", node.bidonuseridauction, auction_id, 20000)
            winner = fourth_bid['bidder_address']
            assert_equal(node.finalizeuseridauction(auction_id), {
                "auction_id": auction_id,
                "name": "alice",
                "namespace_id": "redd",
                "winner": winner,
                "final_price": Decimal("13000"),
                "state": 3,
            })
            assert_raises_rpc_error(-8, "Auction is not active or ended", node.finalizeuseridauction, auction_id)
            assert_raises_rpc_error(-8, "Auction is not active", node.bidonuseridauction, auction_id, 20000)
            info = node.getuserauctioninfo(auction_id, True)
            assert_equal(info['state'], 3)
            assert_equal([bid['bid_id'] for bid in info['bids'] if bid['is_winner']], [fourth_bid['bid_id']])

            self.log.info("getuseridinfo and getuseridlist")
            user_id = {
                "name": "alice",
                "namespace_id": "redd",
                "owner": winner,
                "registration_time": end,
                "expiration_time": end + 365 * DAY,
                "last_transaction": end,
                "transaction_count": 1,
            }
            assert_equal(node.getuseridinfo("alice", "redd"), user_id)
            assert_equal(node.getuseridlist("redd"), [user_id])
            assert_raises_rpc_error(-8, "User ID not found", node.getuseridinfo, "alice", "other")
            assert_raises_rpc_error(-8, "User ID not found", node.getuseridinfo, "bob", "redd")
            assert_raises_rpc_error(-8, "User ID already exists", node.createuseridauction, "alice", "redd", 10000, 7)

            self.log.info("The second auction for the name is still there")
            # SUSPECTED BUG: it can take bids, and when it is finalized the user
            # ID that the first auction registered goes to its winner (see below)
            twin_bid = node.bidonuseridauction(twin['auction_id'], 10000)
            assert_equal(node.getuserauctioninfo(twin['auction_id'])['state'], 1)

            self.log.info("renewuserid and transferuserid do not accept the key that owns the user ID")
            # SUSPECTED BUG: both commands compare the owner of the user ID with a
            # key that they take from the wallet for the call, the next unused
            # one. A wallet can never pass with a key that it has already been
            # given: the wallet that holds the key of the owner is told that it is
            # not the owner, and each call uses up a key. (Not tried: an owner
            # that is the next key of the wallet. Nothing that the commands do
            # after this check is tested.)
            assert_equal(node.getaddressinfo(winner)['ismine'], True)
            target = node.getnewaddress("", "legacy")
            before = self.key_index(node)
            assert_raises_rpc_error(-1, NOT_OWNER, node.renewuserid, "alice", "redd")
            assert_raises_rpc_error(-1, NOT_OWNER, node.renewuserid, "alice", "redd", "account", "legacy", "label")
            assert_raises_rpc_error(-1, NOT_OWNER, node.transferuserid, "alice", "redd", target)
            assert_raises_rpc_error(-1, NOT_OWNER, node.transferuserid, "alice", "redd", target, "account", "legacy", "label")
            assert_equal(self.key_index(node), before + 5)
            assert_raises_rpc_error(-8, "User ID not found", node.renewuserid, "bob", "redd")
            assert_raises_rpc_error(-8, "User ID not found", node.transferuserid, "bob", "redd", target)
            # The address type of renewuserid is the fourth argument; it was read from the fifth
            assert_raises_rpc_error(-8, "Unknown address type", node.renewuserid, "alice", "redd", "account", "nonsense")
            assert_raises_rpc_error(-8, "Unknown address type", node.transferuserid, "alice", "redd", target, "account", "nonsense")
            # The owner is checked before the address is, so a bad address is not reported
            assert_raises_rpc_error(-1, NOT_OWNER, node.transferuserid, "alice", "redd", "notanaddress")
            assert_equal(node.getuseridinfo("alice", "redd"), user_id)

            self.log.info("The periodic task cancels an ended auction with no bid and finalizes one with bids")
            # bob and carol have no bid and have ended; dave is new and gets a bid
            carol = next(entry['auction_id'] for entry in node.getuserauctionlist() if entry['name'] == "carol")
            assert_raises_rpc_error(-1, "Failed to place bid", node.bidonuseridauction, bob['auction_id'], 25000)
            dave = node.createuseridauction("dave", "redd", 10000, 3)
            dave_bid = node.bidonuseridauction(dave['auction_id'], 10000)
        run_expiry_task(node)
        for ended in (bob['auction_id'], carol):
            assert_equal(node.getuserauctioninfo(ended)['state'], 8)
            assert_raises_rpc_error(-8, "Auction is not active", node.bidonuseridauction, ended, 30000)
            assert_raises_rpc_error(-8, "Auction is not active or ended", node.finalizeuseridauction, ended)
        assert_equal(node.getuserauctioninfo(dave['auction_id'])['state'], 1)
        with expiry_task_idle(node):
            advance_clock(self.nodes, 3 * DAY)
            assert_equal(node.getuserauctioninfo(dave['auction_id'])['state'], 1)
        run_expiry_task(node)
        # SUSPECTED BUG: the task gives the user ID to the highest bidder as
        # finalizeuseridauction does, with no payment
        assert_equal(node.getuserauctioninfo(dave['auction_id'])['state'], 3)
        registered = node.getuseridinfo("dave", "redd")
        assert_equal(registered['owner'], dave_bid['bidder_address'])
        assert_equal(registered['registration_time'], node.mocktime)
        assert_raises_rpc_error(-8, "Auction is not active or ended", node.finalizeuseridauction, dave['auction_id'])
        assert_equal(sorted(entry['name'] for entry in node.getuseridlist("redd")), ["alice", "dave"])
        # SUSPECTED BUG: the task also finalized the second auction for alice,
        # and the user ID has passed from the winner of the first to its winner
        assert_equal(node.getuserauctioninfo(twin['auction_id'])['state'], 3)
        assert_equal(node.getuseridinfo("alice", "redd")['owner'], twin_bid['bidder_address'])
        assert twin_bid['bidder_address'] != winner

        self.log.info("reddidgetauctionconfig shows the configuration of each kind of auction")
        assert_equal(node.reddidgetauctionconfig(), USERID_DEFAULTS)
        assert_equal(node.reddidgetauctionconfig("userid"), USERID_DEFAULTS)
        namespace_config = node.reddidgetauctionconfig("namespace")
        assert_equal(namespace_config, dict(USERID_DEFAULTS, config_id="namespace", deposit_percentage=20))
        reddid_config = node.reddidgetauctionconfig("reddid")
        assert_equal(reddid_config['config_id'], "reddid")
        # An unknown ID gets the defaults for user IDs, under their name
        assert_equal(node.reddidgetauctionconfig("nothere"), USERID_DEFAULTS)

        self.log.info("reddidsetauctionconfig changes a value for anyone who can call it")
        # SUSPECTED BUG: the command asks for no wallet, no key and no
        # signature, and the change stays on this node: two nodes can apply
        # different rules to the same transaction
        expected = dict(USERID_DEFAULTS)
        for param, key, value in (
            ("depositPercentage", "deposit_percentage", 20),
            ("settlementWindowBlocks", "settlement_window_blocks", 3),
            ("antiSnipeThresholdSeconds", "anti_snipe_threshold_seconds", 3600),
            ("antiSnipeExtensionSeconds", "anti_snipe_extension_seconds", 600),
            ("minDurationDays", "min_duration_days", 1),
            ("maxDurationDays", "max_duration_days", 30),
            ("minBidIncrementPct", "min_bid_increment_pct", 10),
        ):
            assert_equal(node.reddidsetauctionconfig("userid", param, value), {
                "success": True,
                "config_id": "userid",
                "param": param,
                "value": str(value),
            })
            expected[key] = value
            assert_equal(node.reddidgetauctionconfig("userid"), expected)
        assert_equal(node.reddidgetauctionconfig("namespace"), namespace_config)
        assert_raises_rpc_error(-8, "depositPercentage must be between 0 and 100", node.reddidsetauctionconfig, "userid", "depositPercentage", 101)
        assert_raises_rpc_error(-8, "depositPercentage must be between 0 and 100", node.reddidsetauctionconfig, "userid", "depositPercentage", -1)
        assert_raises_rpc_error(-8, "Unknown config parameter: deposit_percentage", node.reddidsetauctionconfig, "userid", "deposit_percentage", 5)
        assert_raises_rpc_error(-1, "JSON value is not an integer as expected", node.reddidsetauctionconfig, "userid", "depositPercentage", "5")
        # SUSPECTED BUG: the other values are not checked, and a configuration
        # with any ID can be created
        node.reddidsetauctionconfig("userid", "settlementWindowBlocks", -5)
        expected['settlement_window_blocks'] = -5
        node.reddidsetauctionconfig("made-up", "minDurationDays", 9)
        assert_equal(node.reddidgetauctionconfig("made-up"), dict(USERID_DEFAULTS, config_id="made-up", min_duration_days=9))

        self.log.info("The configuration, the auctions and the user IDs survive a restart on the same clock")
        auctions = node.getuserauctionlist()
        user_ids = node.getuseridlist("redd")
        self.restart_node(0, extra_args=keep_clock(node, self.extra_args[0]))
        assert_equal(node.reddidgetauctionconfig("userid"), expected)
        assert_equal(node.getuserauctionlist(), auctions)
        assert_equal(node.getuseridlist("redd"), user_ids)
        assert_equal(node.getuseridauctionbids(auction_id)['total_bids'], 4)


if __name__ == '__main__':
    ReddIDAuctionTest().main()
