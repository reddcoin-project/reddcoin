#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ReddID auctions with deposits, from the bid to the settlement.

reddidplacebid, reddidfinalizeauction and reddidsettleauction are the commands
that build transactions. The records in those transactions take effect when a
block connects them, on every node that knows the auction. The test checks
what each command puts into its transaction, since the node reads the records
by position: the deposit of a bid by its index among the outputs, and the
sender of a transaction from its first input.

One node runs ReddID with three wallets: the default wallet, which also stakes
the blocks, and a second wallet bid, and a third creates the auctions. The test
follows these auctions:

- dave: two bids with deposits, the locks on the deposits, the finalization
  that starts the settlement, the settlement by reddidsettleauction within
  its window and the registration of the user ID, and a reorganization across
  the settlement.
- erin: one bid, a reorganization across the bid, a winner that does not
  settle, and the default record after the window.
- frank: one bid, a creator key with no coin, and a finalization that the
  creator key did not pay for.
- grace, heidi, ivan and judy: many bids in a row, none of which is lost.

What still looks wrong is asserted as it is, under a comment that starts with
SUSPECTED BUG. Where a command now refuses what the node would ignore, the
test sends the record in a transaction that it builds by hand, to show that
the node does ignore it.
"""

from decimal import ROUND_DOWN, ROUND_UP, Decimal
import struct

from test_framework.reddid import (
    COIN,
    DAY,
    acquire_namespace,
    advance_clock,
    expiry_task_idle,
    mine_block,
    run_expiry_task,
    second_wallet,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

NO_LOCK = "Deposit lock not found for specified UTXO"
NOT_CREATOR = "The key of the creator of this auction does not belong to this wallet"
NO_SENDER = "The wallet has no confirmed coin at a legacy address, which a bid needs as its first input"
WINDOW = 3


def record(opcode, auction_id, bid_id):
    """The payload of a ReddID record that names an auction and a bid."""
    return (b'R' + opcode + bytes.fromhex(auction_id)[::-1] + bytes.fromhex(bid_id)[::-1]).hex()


class ReddIDDepositTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = False  # Use cache with pre-generated blocks
        self.extra_args = [["-reddid=1", "-debug=reddid"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        # The commands take keys from a legacy wallet
        self.skip_if_no_bdb()

    def mine(self):
        return mine_block(self.nodes, self.staker, self.stake_address)

    def next_keys(self, wallet):
        """The indexes of the next receiving key and the next change key of the wallet, which this call uses up."""
        paths = [wallet.getaddressinfo(address)['hdkeypath'] for address in (wallet.getnewaddress("", "legacy"), wallet.getrawchangeaddress("legacy"))]
        return [int(path.split('/')[-1].rstrip("'")) for path in paths]

    def assert_no_key_used(self, wallet, keys):
        """No key has left the key pool of the wallet since next_keys returned the indexes."""
        assert_equal(self.next_keys(wallet), [index + 1 for index in keys])

    def bid_count(self, auction_id):
        return self.nodes[0].getuseridauctionbids(auction_id)['total_bids']

    def send_bid(self, wallet, auction_id, amount):
        """Bid with reddidplacebid and check the transaction. Returns its ID and the address of the bidder."""
        node = self.nodes[0]
        deposit = amount * Decimal("0.15")
        bid = wallet.reddidplacebid(auction_id, amount)
        txid = bid.pop('txid')
        bidder = bid.pop('bidder_address')
        assert_equal(bid, {
            "auction_id": auction_id,
            "bid_amount": amount,
            "deposit_amount": deposit,
            "deposit_vout": 1,
        })
        assert_equal(wallet.getaddressinfo(bidder)['ismine'], True)
        assert txid in node.getrawmempool()
        tx = node.getrawtransaction(txid, True)
        # The record, the deposit, which the record names by its index, and
        # then the change
        assert_equal(len(tx['vout']), 3)
        assert_equal(tx['vout'][0]['scriptPubKey']['type'], 'nulldata')
        assert_equal(tx['vout'][1]['scriptPubKey']['address'], bidder)
        assert_equal(tx['vout'][1]['value'], deposit)
        # The first input is a confirmed coin of the wallet at a legacy
        # address: the node reads the sender of the transaction from it, and
        # ignores a bid with no sender
        spent = wallet.gettransaction(tx['vin'][0]['txid'], True, True)
        assert spent['confirmations'] > 0
        assert spent['decoded']['vout'][tx['vin'][0]['vout']]['scriptPubKey']['type'] in ('pubkey', 'pubkeyhash')
        # SUSPECTED BUG: the wallet does not know that the deposit is about
        # to be locked. Keep its coin selection and its staking away from it.
        wallet.lockunspent(False, [{"txid": txid, "vout": 1}])
        return txid, bidder

    def place_bid(self, wallet, auction_id, amount):
        """Bid with reddidplacebid, mine the bid and return the lock on its deposit."""
        node = self.nodes[0]
        bids = self.bid_count(auction_id)
        locks = wallet.reddidlistmydeposits()
        keys = self.next_keys(wallet)
        txid, bidder = self.send_bid(wallet, auction_id, amount)
        # The bid took the next receiving key for the bidder and the next
        # change key for the change
        assert_equal(wallet.getaddressinfo(bidder)['hdkeypath'].split('/')[-1], "%d'" % (keys[0] + 1))
        assert_equal(self.next_keys(wallet), [index + 2 for index in keys])

        # In the mempool the bid is not placed and nothing is locked
        assert_equal(self.bid_count(auction_id), bids)
        assert_raises_rpc_error(-8, NO_LOCK, node.reddidgetdepositlock, txid, 1)
        assert_equal(wallet.reddidlistmydeposits(), locks)

        self.mine()
        assert_equal(node.getrawtransaction(txid, True)['confirmations'], 1)
        assert_equal(self.bid_count(auction_id), bids + 1)
        lock = node.reddidgetdepositlock(txid, 1)
        assert_equal(lock['lock_height'], node.getblockcount())
        return dict(lock, bidder=bidder)

    def raw_bid(self, wallet, auction_id, amount):
        """Send a bid with a deposit of 15% in a transaction that is made by hand, and return its ID."""
        bidder = wallet.getnewaddress("", "legacy")
        key_hash = bytes.fromhex(wallet.getaddressinfo(bidder)['scriptPubKey'])[3:23]
        deposit = (amount * Decimal("0.15")).quantize(Decimal("0.00000001"), rounding=ROUND_DOWN)
        # The record: the auction, the bid in base units, the index of the
        # deposit among the outputs and the key that the deposit is paid to
        payload = b'R' + b'D' + bytes.fromhex(auction_id)[::-1] + struct.pack("<q", int(amount * COIN)) + bytes([1]) + key_hash
        raw = wallet.createrawtransaction([], [{"data": payload.hex()}, {bidder: deposit}])
        funded = wallet.fundrawtransaction(raw, {"changePosition": 2})
        return self.nodes[0].sendrawtransaction(wallet.signrawtransactionwithwallet(funded['hex'])['hex'])

    def spend(self, wallet, txid, vout, amount, payload=None):
        """A signed transaction of the wallet that spends one output, with a ReddID record if a payload is given."""
        outputs = [{"data": payload}] if payload else []
        outputs.append({self.stake_address: amount - Decimal("0.01")})
        raw = wallet.createrawtransaction([{"txid": txid, "vout": vout}], outputs)
        signed = wallet.signrawtransactionwithwallet(raw)
        assert_equal(signed['complete'], True)
        return signed['hex']

    def raw_record(self, wallet, payload):
        """Send a ReddID record in a transaction that the wallet funds from any of its coins, and return its ID."""
        raw = wallet.createrawtransaction([], [{"data": payload}])
        return self.nodes[0].sendrawtransaction(wallet.signrawtransactionwithwallet(wallet.fundrawtransaction(raw)['hex'])['hex'])

    def create_auction(self, name, fund=True):
        """A three day auction that the third wallet creates, with a coin for its creator key."""
        auction = self.creator.createuseridauction(name, "redd", 10000, 3)
        self.creator_address[auction['auction_id']] = auction['creator_address']
        # SUSPECTED BUG: the finalization of an auction is accepted from its
        # creator only, and the node takes the creator to be the key of the
        # first input of the transaction. The creator is a key that
        # createuseridauction took from the wallet: it has to be sent a coin,
        # and the coin has to confirm, before the auction can be finalized.
        if fund:
            self.staker.sendtoaddress(auction['creator_address'], 10)
            self.mine()
        return auction['auction_id']

    def finalize(self, auction_id):
        """reddidfinalizeauction from the wallet of the creator. Checks the transaction that it built."""
        node = self.nodes[0]
        address = self.creator_address[auction_id]
        result = self.creator.reddidfinalizeauction(auction_id)
        txid = result.pop('txid')
        # The command reports the state that the auction has: nothing
        # happens to it before a block connects the transaction
        assert_equal(result, {"auction_id": auction_id, "state": "active"})
        assert_equal(node.getuserauctioninfo(auction_id)['state'], 1)
        assert txid in node.getrawmempool()
        tx = node.getrawtransaction(txid, True)
        # The first input is a confirmed coin of the creator key, though the
        # wallet has larger coins, and the change returns to its address
        spent = self.creator.gettransaction(tx['vin'][0]['txid'], True, True)
        assert spent['confirmations'] > 0
        assert_equal(spent['decoded']['vout'][tx['vin'][0]['vout']]['scriptPubKey']['address'], address)
        assert_equal(tx['vout'][0]['scriptPubKey']['hex'], "6a22" + (b'R"' + bytes.fromhex(auction_id)[::-1]).hex())
        assert_equal([out['scriptPubKey']['address'] for out in tx['vout'][1:]], [address])
        return txid

    def run_test(self):
        node = self.nodes[0]
        self.creator_address = {}
        self.staker = first = node.get_wallet_rpc(self.default_wallet_name)
        self.stake_address = first.getnewaddress("", "legacy")
        node.createwallet(wallet_name="second", descriptors=False)
        node.createwallet(wallet_name="creator", descriptors=False)
        second = node.get_wallet_rpc("second")
        self.creator = creator = node.get_wallet_rpc("creator")
        for _ in range(4):
            first.sendtoaddress(second.getnewaddress("", "legacy"), 500000)
        # A coin for the third wallet that is not at the address of a creator
        first.sendtoaddress(creator.getnewaddress("", "legacy"), 1000)
        self.mine()

        self.log.info("No wallet has a deposit to begin with")
        for wallet in (first, second, creator):
            assert_equal(wallet.reddidlistmydeposits(), [])

        acquire_namespace(self.nodes, first, "redd")

        self.log.info("The settlement window is shortened to %d blocks, for this node" % WINDOW)
        assert_equal(node.reddidgetauctionconfig("userid")['settlement_window_blocks'], 4320)
        assert_equal(node.reddidgetauctionconfig("userid")['deposit_percentage'], 15)
        node.reddidsetauctionconfig("userid", "settlementWindowBlocks", WINDOW)

        self.log.info("reddidplacebid checks the auction and the amount")
        dave = self.create_auction("dave")
        assert_raises_rpc_error(-8, "Auction not found", first.reddidplacebid, "00" * 32, 10000)
        assert_raises_rpc_error(-8, "Bid amount 9999.00 is below minimum 10000.00", first.reddidplacebid, dave, 9999)
        assert_raises_rpc_error(-3, "Amount out of range", first.reddidplacebid, dave, -1)
        # A bid that the command refuses takes no key from the wallet: neither
        # for the bidder nor for the change
        keys = self.next_keys(creator)
        assert_raises_rpc_error(-4, "Insufficient funds", creator.reddidplacebid, dave, 10000)
        assert_raises_rpc_error(-8, "Bid amount 9999.00 is below minimum 10000.00", creator.reddidplacebid, dave, 9999)
        self.assert_no_key_used(creator, keys)
        # The node reads the sender of a bid from a confirmed coin at a legacy
        # address, and ignores a bid without one: the command asks for the coin
        with second_wallet(node, "segwit") as segwit:
            assert_raises_rpc_error(-4, NO_SENDER, segwit.reddidplacebid, dave, 10000)
            legacy = segwit.getnewaddress("", "legacy")
            first.sendtoaddress(segwit.getnewaddress("", "bech32"), 20000)
            first.sendtoaddress(legacy, 20000)
            assert_raises_rpc_error(-4, NO_SENDER, segwit.reddidplacebid, dave, 10000)
            self.mine()
            assert_equal(len(segwit.listunspent(1)), 2)
            keys = self.next_keys(segwit)
            # With the legacy coin held back, the confirmed coin that is left
            # is not one that the node reads a sender from
            segwit.lockunspent(False, [{"txid": coin['txid'], "vout": coin['vout']} for coin in segwit.listunspent() if coin['address'] == legacy])
            assert_equal(len(segwit.listunspent(1)), 1)
            assert_raises_rpc_error(-4, NO_SENDER, segwit.reddidplacebid, dave, 10000)
            self.assert_no_key_used(segwit, keys)
            # A wallet that cannot sign is told so before anything is built
            segwit.encryptwallet("passphrase")
            assert_raises_rpc_error(-13, "Please enter the wallet passphrase with walletpassphrase first", segwit.reddidplacebid, dave, 10000)

        self.log.info("A bid is placed, and its deposit locked, when a block connects the transaction")
        low = self.place_bid(first, dave, Decimal("10000"))
        info = node.getuserauctioninfo(dave)
        assert_equal(info['state'], 1)
        assert_equal(info['current_bid'], Decimal("10000"))
        assert_equal(info['current_bidder'], low['bidder'])

        self.log.info("The command refuses a bid below the minimum of the node")
        # The minimum over a bid is that bid and 5% more, in this namespace:
        # the command and the node take it from the same place
        mempool = node.getrawmempool()
        assert_raises_rpc_error(-8, "Bid amount 10000.00 is below minimum 10500.00", second.reddidplacebid, dave, 10000)
        assert_raises_rpc_error(-8, "Bid amount 10499.99999999 is below minimum 10500.00", second.reddidplacebid, dave, Decimal("10499.99999999"))
        assert_equal(node.getrawmempool(), mempool)

        self.log.info("The node ignores such a bid in a transaction that the command did not build")
        short = self.raw_bid(second, dave, Decimal("10499.99999999"))
        with node.assert_debug_log(["AuctionManager::ValidateBid: Bid increment too low: 10499.99999999 (minimum: 10500.00)", "AuctionManager::PlaceBid: Invalid bid"]):
            self.mine()
        assert_equal(node.getrawtransaction(short, True)['confirmations'], 1)
        assert_equal(self.bid_count(dave), 1)
        assert_equal(second.reddidlistmydeposits(), [])

        high = self.place_bid(second, dave, Decimal("12000"))
        info = node.getuserauctioninfo(dave, True)
        assert_equal(info['state'], 1)
        assert_equal(info['current_bid'], Decimal("12000"))
        assert_equal(info['current_bidder'], high['bidder'])
        bids = node.getuseridauctionbids(dave, "amount", "asc")['bids']
        assert_equal([bid['bid_id'] for bid in bids], [low['bid_id'], high['bid_id']])
        assert_equal([bid['deposit'] for bid in bids], [Decimal("1500"), Decimal("1800")])
        assert_equal([bid['bidder'] for bid in bids], [low['bidder'], high['bidder']])

        self.log.info("reddidgetdepositlock and reddidlistmydeposits")
        assert_equal(node.reddidgetdepositlock(low['txid'], 1), {
            "txid": low['txid'],
            "vout": 1,
            "auction_id": dave,
            "bid_id": low['bid_id'],
            "bidder": low['bidder'],
            "amount": Decimal("1500"),
            "lock_height": low['lock_height'],
            "release_height": 0,
            "state": "active",
        })
        assert_equal(first.reddidlistmydeposits(), [node.reddidgetdepositlock(low['txid'], 1)])
        assert_equal(second.reddidlistmydeposits(), [node.reddidgetdepositlock(high['txid'], 1)])
        assert_equal(creator.reddidlistmydeposits(), [])
        assert_raises_rpc_error(-8, NO_LOCK, node.reddidgetdepositlock, low['txid'], 0)
        assert_raises_rpc_error(-8, NO_LOCK, node.reddidgetdepositlock, "00" * 32, 1)

        self.log.info("A locked deposit cannot be spent through the mempool")
        # SUSPECTED BUG: the wallet lists the deposit as an ordinary coin, and
        # would choose it for a payment or for a stake had the test not
        # locked it in the wallet. What is asserted here is that the mempool
        # refuses the spend. That a block may contain it all the same is
        # asserted by the unit test block_may_spend_a_locked_deposit in
        # src/test/reddid_chain_tests.cpp.
        first.lockunspent(True, [{"txid": low['txid'], "vout": 1}])
        assert_equal([coin['spendable'] for coin in first.listunspent() if coin['txid'] == low['txid'] and coin['vout'] == 1], [True])
        first.lockunspent(False, [{"txid": low['txid'], "vout": 1}])
        spend_low = self.spend(first, low['txid'], 1, Decimal("1500"))
        assert_raises_rpc_error(-26, "reddid-deposit-locked", node.sendrawtransaction, spend_low)
        assert_equal(node.testmempoolaccept([spend_low])[0]['reject-reason'], "reddid-deposit-locked")
        # A record of another kind does not make the spend a settlement
        assert_raises_rpc_error(-26, "reddid-deposit-locked", node.sendrawtransaction, self.spend(first, low['txid'], 1, Decimal("1500"), record(b'X', dave, low['bid_id'])))

        self.log.info("A bid that the mempool refuses is an error of the command")
        # SUSPECTED BUG: the wallet lists a locked deposit as an ordinary
        # coin (see above), and chooses it here, where its other coins do not
        # cover the deposit of a new bid. The mempool refuses the
        # transaction. The command says so, and keeps nothing: no
        # transaction in the wallet and no coin held back.
        second.sendtoaddress(second.getnewaddress("", "legacy"), 100)
        self.mine()
        small = [coin for coin in second.listunspent(1) if coin['amount'] == Decimal("100")]
        assert_equal(len(small), 1)
        others = [{"txid": coin['txid'], "vout": coin['vout']} for coin in second.listunspent() if coin != small[0]]
        second.lockunspent(False, others)
        second.lockunspent(True, [{"txid": high['txid'], "vout": 1}])
        held = second.listlockunspent()
        transactions = len(second.listtransactions("*", 1000))
        mempool = node.getrawmempool()
        keys = self.next_keys(second)
        assert_raises_rpc_error(-26, "Transaction was not accepted: reddid-deposit-locked", second.reddidplacebid, dave, Decimal("12600"))
        self.assert_no_key_used(second, keys)
        assert_equal(node.getrawmempool(), mempool)
        assert_equal(len(second.listtransactions("*", 1000)), transactions)
        assert_equal(second.listlockunspent(), held)
        second.lockunspent(False, [{"txid": high['txid'], "vout": 1}])
        second.lockunspent(True, others)

        self.log.info("reddidfinalizeauction waits for the end of the auction")
        assert_raises_rpc_error(-8, "Auction not found", first.reddidfinalizeauction, "00" * 32)
        remaining = node.getuserauctioninfo(dave)['time_remaining']
        assert_raises_rpc_error(-8, "Auction has not ended yet, %d seconds remaining" % remaining, first.reddidfinalizeauction, dave)
        assert_raises_rpc_error(-8, "Auction is not in settlement state (current state: 1)", second.reddidsettleauction, dave)
        assert_raises_rpc_error(-8, "Auction not found", second.reddidsettleauction, "00" * 32)

        self.log.info("The finalization from the creator key starts the settlement")
        # The auction has ended and is active until the block with the
        # finalization connects. In between, the periodic task would finalize
        # it as an auction with no deposits (see rpc_reddid_auction.py).
        run_expiry_task(node)
        advance_clock(self.nodes, 3 * DAY)
        with expiry_task_idle(node):
            # The node accepts the finalization from the creator only, and
            # the command is for the wallet that holds the key of the creator
            for wallet in (first, second):
                assert_raises_rpc_error(-4, NOT_CREATOR, wallet.reddidfinalizeauction, dave)
            with node.assert_debug_log(["Settlement begun for auction " + dave]):
                self.finalize(dave)
                self.mine()
            finalized = node.getblockcount()
            info = node.getuserauctioninfo(dave, True)
            assert_equal(info['state'], 4)
        assert_equal([bid['bid_id'] for bid in info['bids'] if bid['is_winner']], [high['bid_id']])
        assert_raises_rpc_error(-8, "User ID not found", node.getuseridinfo, "dave", "redd")
        assert_raises_rpc_error(-8, "Auction is not active or ended", creator.reddidfinalizeauction, dave)
        assert_raises_rpc_error(-8, "Auction is not active or pending", first.reddidplacebid, dave, 20000)

        self.log.info("The deposit of the bid that lost is released, that of the winner stays locked")
        released = node.reddidgetdepositlock(low['txid'], 1)
        assert_equal(released['state'], "released")
        # Current behaviour: the height of the block before the one that
        # released the deposit
        assert_equal(released['release_height'], finalized - 1)
        assert_equal(first.reddidlistmydeposits(), [released])
        assert_equal(node.testmempoolaccept([spend_low])[0]['allowed'], True)
        first.lockunspent(True, [{"txid": low['txid'], "vout": 1}])
        assert_equal(node.reddidgetdepositlock(high['txid'], 1)['state'], "active")
        spend_high = self.spend(second, high['txid'], 1, Decimal("1800"))
        assert_raises_rpc_error(-26, "reddid-deposit-locked", node.sendrawtransaction, spend_high)

        self.log.info("reddidsettleauction is for the wallet of the winner")
        for wallet in (first, creator):
            assert_raises_rpc_error(-4, "Winning bid deposit key does not belong to this wallet", wallet.reddidsettleauction, dave)
        # The wallet has to sign for the deposit: watching its address is not enough
        with second_wallet(node, "watcher") as watcher:
            watcher.importaddress(high['bidder'], "", False)
            assert_equal(watcher.getaddressinfo(high['bidder'])['iswatchonly'], True)
            assert_raises_rpc_error(-4, "Winning bid deposit key does not belong to this wallet", watcher.reddidsettleauction, dave)

        self.log.info("reddidsettleauction spends the deposit of the winner, and the block completes the auction")
        # The next block, finalized + 1, is within the window, which ends
        # with block finalized + WINDOW
        mempool = node.getrawmempool()
        settled = second.reddidsettleauction(dave)
        settle_txid = settled.pop('txid')
        assert_equal(settled, {
            "auction_id": dave,
            "bid_id": high['bid_id'],
            "settlement_amount": Decimal("1800"),
        })
        # The mempool lets a settlement of the auction spend the locked deposit
        assert_equal(sorted(node.getrawmempool()), sorted(mempool + [settle_txid]))
        tx = node.getrawtransaction(settle_txid, True)
        # The deposit is the first input, and the only one: it is more than the fee
        assert_equal([(vin['txid'], vin['vout']) for vin in tx['vin']], [(high['txid'], 1)])
        assert_equal(tx['vout'][0]['scriptPubKey']['hex'], "6a42" + record(b'S', dave, high['bid_id']))
        # SUSPECTED BUG: the winner pays the fee of the transaction and
        # nothing else. The bid of 12000 is paid to nobody, and the deposit
        # returns to the wallet of the winner as change.
        assert_equal(len(tx['vout']), 2)
        assert_equal(second.getaddressinfo(tx['vout'][1]['scriptPubKey']['address'])['ismine'], True)
        assert tx['vout'][1]['value'] > Decimal("1799")
        # Nothing has changed before the block
        assert_equal(node.getuserauctioninfo(dave)['state'], 4)
        assert_equal(node.reddidgetdepositlock(high['txid'], 1)['state'], "active")
        with node.assert_debug_log(["Settlement completed for auction " + dave]):
            settle_block = self.mine()
        assert_equal(node.getblockcount(), finalized + 1)
        assert_equal(node.getrawtransaction(settle_txid, True)['confirmations'], 1)
        assert_equal(node.getuserauctioninfo(dave)['state'], 5)
        user_id = node.getuseridinfo("dave", "redd")
        assert_equal(user_id['owner'], high['bidder'])
        assert_equal(second.getaddressinfo(user_id['owner'])['ismine'], True)
        assert_equal(user_id['registration_time'], node.mocktime)
        consumed = node.reddidgetdepositlock(high['txid'], 1)
        assert_equal(consumed['state'], "consumed")
        assert_equal(second.reddidlistmydeposits(), [consumed])
        assert_raises_rpc_error(-8, "Auction is not in settlement state (current state: 5)", second.reddidsettleauction, dave)

        self.log.info("A reorganization across the settlement")
        node.invalidateblock(settle_block)
        assert_equal(node.getblockcount(), finalized)
        assert_equal(node.getuserauctioninfo(dave)['state'], 4)
        assert_raises_rpc_error(-8, "User ID not found", node.getuseridinfo, "dave", "redd")
        assert_equal(node.reddidgetdepositlock(high['txid'], 1)['state'], "active")
        assert settle_txid in node.getrawmempool()
        node.reconsiderblock(settle_block)
        assert_equal(node.getbestblockhash(), settle_block)
        assert_equal(node.getuserauctioninfo(dave)['state'], 5)
        assert_equal(node.getuseridinfo("dave", "redd"), user_id)
        assert_equal(node.reddidgetdepositlock(high['txid'], 1), consumed)

        self.log.info("A reorganization across a bid")
        erin = self.create_auction("erin")
        only = self.place_bid(first, erin, Decimal("10000"))
        bid_block = node.getbestblockhash()
        active = node.getuserauctioninfo(erin, True)
        assert_equal(active['state'], 1)
        node.invalidateblock(bid_block)
        undone = node.getuserauctioninfo(erin, True)
        assert_equal(undone['state'], 0)
        assert_equal(undone['current_bid'], Decimal("0"))
        assert_equal(undone['bids'], [])
        assert_raises_rpc_error(-8, NO_LOCK, node.reddidgetdepositlock, only['txid'], 1)
        assert_equal([lock['auction_id'] for lock in first.reddidlistmydeposits()], [dave])
        assert only['txid'] in node.getrawmempool()
        node.reconsiderblock(bid_block)
        assert_equal(node.getbestblockhash(), bid_block)
        assert_equal(node.getuserauctioninfo(erin, True), active)
        assert_equal(node.reddidgetdepositlock(only['txid'], 1)['state'], "active")

        self.log.info("A winner that does not settle defaults after the window, by a record that no command builds")
        run_expiry_task(node)
        advance_clock(self.nodes, 3 * DAY)
        with expiry_task_idle(node):
            self.finalize(erin)
            self.mine()
            finalized = node.getblockcount()
            assert_equal(node.getuserauctioninfo(erin)['state'], 4)
        default = record(b'X', erin, only['bid_id'])
        # The second wallet has no part in this auction: it holds neither the
        # key of the creator nor the key of the only bid
        assert_equal(second.getaddressinfo(self.creator_address[erin])['ismine'], False)
        assert_equal(second.getaddressinfo(only['bidder'])['ismine'], False)
        assert_equal(first.getaddressinfo(only['bidder'])['ismine'], True)

        # Blocks finalized + 1 to finalized + WINDOW are the window: a default
        # record in them is mined and ignored
        for height in range(finalized + 1, finalized + WINDOW + 1):
            with node.assert_debug_log(["Not past settlement deadline yet"]):
                early = self.raw_record(second, default)
                self.mine()
            assert_equal(node.getblockcount(), height)
            assert_equal(node.getrawtransaction(early, True)['confirmations'], 1)
            assert_equal(node.getuserauctioninfo(erin)['state'], 4)
            assert_equal(node.reddidgetdepositlock(only['txid'], 1)['state'], "active")
        # The command does not build a settlement that the node would ignore
        assert_raises_rpc_error(-8, "The settlement window of this auction has ended: the settlement had to be in block %d or an earlier one" % (finalized + WINDOW), first.reddidsettleauction, erin)
        # SUSPECTED BUG: anyone may send the record; nothing ties it to a
        # party of the auction. The second wallet pays for this one and
        # signs it.
        with node.assert_debug_log(["Default processed for auction " + erin]):
            self.raw_record(second, default)
            self.mine()
        assert_equal(node.getblockcount(), finalized + WINDOW + 1)
        assert_equal(node.getuserauctioninfo(erin)['state'], 6)
        assert_raises_rpc_error(-8, "User ID not found", node.getuseridinfo, "erin", "redd")
        forfeited = node.reddidgetdepositlock(only['txid'], 1)
        assert_equal(forfeited['state'], "forfeited")
        assert_raises_rpc_error(-8, "Auction is not in settlement state (current state: 6)", first.reddidsettleauction, erin)

        self.log.info("A forfeited deposit can be taken back, and the name is auctioned again")
        # SUSPECTED BUG: a forfeited deposit is meant for the community fund.
        # The address of the fund is empty in every configuration and no
        # command can set it, and with no address the bidder that defaulted
        # may spend the deposit as it likes.
        assert_equal(node.reddidgetauctionconfig("userid")['community_fund_address'], "")
        assert_equal(node.testmempoolaccept([self.spend(first, only['txid'], 1, Decimal("1500"))])[0]['allowed'], True)
        again = [entry for entry in node.getuserauctionlist("redd") if entry['name'] == "erin" and entry['auction_id'] != erin]
        assert_equal(len(again), 1)
        assert_equal(again[0]['state'], 0)

        self.log.info("reddidfinalizeauction needs a confirmed coin of the creator key")
        # On an auction of its own, whose creator key has no coin yet
        frank = self.create_auction("frank", fund=False)
        address = self.creator_address[frank]
        no_coin = "The wallet has no confirmed coin at the address of the creator of this auction, %s, which the finalization needs as its first input" % address
        self.place_bid(second, frank, Decimal("10000"))
        run_expiry_task(node)
        advance_clock(self.nodes, 3 * DAY)
        with expiry_task_idle(node):
            # The wallet of the creator has coins, and none of the creator key
            assert creator.getbalance() > 900
            mempool = node.getrawmempool()
            assert_raises_rpc_error(-4, no_coin, creator.reddidfinalizeauction, frank)
            # A coin that is not in a block yet is not one that the node
            # reads the sender from
            funding = first.sendtoaddress(address, 10)
            assert_raises_rpc_error(-4, no_coin, creator.reddidfinalizeauction, frank)
            assert_equal(sorted(node.getrawmempool()), sorted(mempool + [funding]))

            self.log.info("A finalization that the creator key did not pay for is mined and ignored")
            # The command does not build one. The record in a transaction of
            # another wallet is rejected by the node when the block connects.
            with node.assert_debug_log(["Unauthorized finalization attempt"]):
                ignored = self.raw_record(first, (b'R"' + bytes.fromhex(frank)[::-1]).hex())
                self.mine()
            assert_equal(node.getrawtransaction(ignored, True)['confirmations'], 1)
            assert_equal(node.getuserauctioninfo(frank)['state'], 1)

            # That block confirmed the coin of the creator key
            with node.assert_debug_log(["Settlement begun for auction " + frank]):
                self.finalize(frank)
                self.mine()
            assert_equal(node.getuserauctioninfo(frank)['state'], 4)

        self.log.info("Every bid that the command sends is placed")
        # Twelve bids on one auction, from two wallets in turn, each 5% above
        # the one before
        grace = creator.createuseridauction("grace", "redd", 10000, 3)['auction_id']
        amount = Decimal("10000")
        placed = []
        for turn in range(12):
            placed.append(self.place_bid((first, second)[turn % 2], grace, amount))
            highest = amount
            amount = (amount * Decimal("1.05")).quantize(Decimal("1"), rounding=ROUND_UP)
        info = node.getuserauctioninfo(grace, True)
        assert_equal(info['current_bid'], highest)
        assert_equal(info['current_bidder'], placed[-1]['bidder'])
        assert_equal(sorted(bid['bid_id'] for bid in info['bids']), sorted(lock['bid_id'] for lock in placed))
        assert_equal([node.reddidgetdepositlock(lock['txid'], 1)['state'] for lock in placed], ["active"] * 12)
        # Three bids of one wallet in one block, on three auctions
        three = [creator.createuseridauction(name, "redd", 10000, 3)['auction_id'] for name in ("heidi", "ivan", "judy")]
        sent = [self.send_bid(second, auction_id, Decimal("10000")) for auction_id in three]
        assert_equal(len(set(txid for txid, _ in sent)), 3)
        self.mine()
        for auction_id, (txid, bidder) in zip(three, sent):
            assert_equal(node.getuserauctioninfo(auction_id)['current_bidder'], bidder)
            assert_equal(node.reddidgetdepositlock(txid, 1)['auction_id'], auction_id)

        self.log.info("A deposit bid on a namespace auction is never accepted")
        # SUSPECTED BUG: the command finds the auction, which the namespace
        # manager created, in the database and makes a transaction for it.
        # The auction manager, which reads the record when the block
        # connects, does not have the auction. The deposit is the 20% of
        # namespace auctions.
        namespace_auction = first.createnamespacesauction("other", 50000, 14)['auction_id']
        assert_equal(node.reddidgetauctionconfig("namespace")['deposit_percentage'], 20)
        with node.assert_debug_log(["Auction not found for deposit: " + namespace_auction]):
            bid = first.reddidplacebid(namespace_auction, 50000)
            assert_equal(bid['deposit_amount'], Decimal("10000"))
            self.mine()
        assert_equal(node.getrawtransaction(bid['txid'], True)['confirmations'], 1)
        for vout in range(3):
            assert_raises_rpc_error(-8, NO_LOCK, node.reddidgetdepositlock, bid['txid'], vout)
        assert_equal(node.getnamespaceauctioninfo(namespace_auction)['state'], 0)


if __name__ == '__main__':
    ReddIDDepositTest().main()
