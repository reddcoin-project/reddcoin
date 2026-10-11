#!/usr/bin/env python3
# Copyright (c) 2025 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Helpers for the ReddID functional tests.

The message classes, the peer and the encoders for the 18 ReddID peer-to-peer
messages, and the steps that several tests share: running the periodic task of
a node and checking that it does not run again by itself, restarting a node on
the clock it had, mining with a chosen wallet and acquiring a namespace through
its auction.

The encoders are written from the serialization of the message classes in
src/id/reddid_p2p.h and of the types in src/id/reddid.h.
"""

from contextlib import ExitStack, contextmanager
import os
import struct

from test_framework.authproxy import JSONRPCException
from test_framework.messages import (
    NODE_NETWORK,
    NODE_REDDID,
    NODE_WITNESS,
    hash256,
    ser_compact_size,
    ser_string,
)
from test_framework import p2p
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.util import set_node_times

COIN = 100000000
DAY = 24 * 60 * 60
# The first line that the periodic task logs, with -debug=reddid, each time it runs
EXPIRY_TASK_LOG = "Processing expired auctions at time"

REDDID_COMMANDS = [
    "nsaucann", "nsaucbid", "nsaucfin", "nsauccan", "nsconreq", "nsconres",
    "uidaucann", "uidaucbid", "uidaucfin", "uidauccan",
    "ridaucann", "ridaucbid", "ridaucfin", "ridprofup", "ridconn", "ridprofreq", "ridprofres", "ridrep",
]

PEER_SERVICES = NODE_NETWORK | NODE_WITNESS | NODE_REDDID


class msg_reddid:
    """A ReddID message with its payload as raw bytes."""
    __slots__ = ("payload",)
    msgtype = b""

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def deserialize(self, f):
        self.payload = f.read()

    def __repr__(self):
        return "msg_reddid(%s, %d bytes)" % (self.msgtype.decode(), len(self.payload))


MESSAGE_CLASSES = {}
for _command in REDDID_COMMANDS:
    _cls = type("msg_" + _command, (msg_reddid,), {"msgtype": _command.encode(), "__slots__": ()})
    MESSAGE_CLASSES[_command] = _cls
    # The framework refuses to read a message type that it does not know
    p2p.MESSAGEMAP[_command.encode()] = _cls


def reddid_msg(command, payload):
    return MESSAGE_CLASSES[command](payload)


class ReddIDPeer(P2PInterface):
    """A peer that records the ReddID messages it receives and the order of all messages."""

    def __init__(self):
        super().__init__()
        self.reddid_messages = []
        self.order = []
        for command in REDDID_COMMANDS:
            setattr(self, "on_" + command, self.on_reddid)

    def on_message(self, message):
        self.order.append(message.msgtype.decode('ascii'))
        super().on_message(message)

    def on_reddid(self, message):
        self.reddid_messages.append((message.msgtype.decode('ascii'), message.payload))

    def take(self):
        """The ReddID messages received since the last call, after a ping round trip."""
        self.sync_with_ping()
        with p2p_lock:
            messages = self.reddid_messages
            self.reddid_messages = []
        return messages

    def commands(self):
        return [command for command, _ in self.take()]


# --- Encoders ---

def i32(v):
    return struct.pack("<i", v)


def i64(v):
    return struct.pack("<q", v)


def u8(v):
    return struct.pack("<B", v)


def key_id(byte):
    return bytes([byte]) * 20


def ser_namespace_info(ns_id, owner, expiration=0, config_hash=bytes(32), last_updated=0):
    r = ser_string(ns_id.encode()) + owner
    r += u8(1) + u8(1) + u8(1)  # allowNumbers, allowHyphens, allowUnderscores
    for v in (3, 32, 365, 30, 10, 70, 15, 5, 3, 7):
        # minLength, maxLength, renewalPeriod, gracePeriod, namespaceRevenuePct,
        # burnPct, nodePct, devPct, minAuctionDuration, maxAuctionDuration
        r += i32(v)
    r += i32(500)  # minBidIncrement, in hundredths
    r += config_hash + i64(last_updated) + i64(expiration)
    return r


def ser_namespace_config(now, ns_id, owner, tiers=()):
    # The hash covers the configuration with the hash itself and the time of
    # the last update set to zero
    config_hash = hash256(ser_namespace_info(ns_id, owner))
    r = i64(now) + ser_namespace_info(ns_id, owner, config_hash=config_hash)
    r += ser_compact_size(len(tiers))
    for min_length, min_price in tiers:
        r += ser_string(ns_id.encode()) + i32(min_length) + i64(min_price)
    return r


def ser_namespace_announce(now, auction_id, ns_id, creator, start, end, reserve):
    return i64(now) + auction_id + ser_string(ns_id.encode()) + creator + i64(start) + i64(end) + i64(reserve) + u8(0) + bytes(32)


def ser_user_announce(now, auction_id, name, ns_id, creator, start, end, reserve):
    return i64(now) + auction_id + ser_string(name.encode()) + ser_string(ns_id.encode()) + creator + i64(start) + i64(end) + i64(reserve) + u8(0)


def ser_bid(timestamp, auction_id, bid_id, bidder, amount, deposit=0):
    return i64(timestamp) + auction_id + bid_id + bidder + i64(amount) + i64(deposit)


def ser_finalize(now, auction_id, bid_id, price, creator):
    return i64(now) + auction_id + bid_id + i64(price) + creator


def ser_profile(redd_id, owner, now, bio=b"bio"):
    r = ser_string(redd_id.encode()) + owner + ser_string(redd_id.encode()) + bytes(32) + ser_string(bio) + bytes(32)
    r += ser_string(b"") + ser_string(b"")  # socialData, messagingPubkey
    r += u8(0)  # verificationStatus
    r += i64(now) + i64(now) + i64(now + 365 * DAY) + i64(now)  # creation, lastUpdated, expiration, lastActivity
    r += u8(1) + u8(0)  # active, flags
    return r


def ser_profile_update(now, redd_id, profile):
    return i64(now) + ser_string(redd_id.encode()) + hash256(profile) + profile


def ser_profile_request(now, redd_id):
    return i64(now) + ser_string(redd_id.encode())


def ser_connection(now, from_id, to_id, connection_type):
    r = i64(now) + ser_string(from_id.encode()) + ser_string(to_id.encode()) + u8(connection_type)
    r += i64(now) + i64(now) + i32(0) + ser_string(b"") + bytes(32)
    return r


def ser_reputation(now, redd_id, score, last_calculated):
    r = i64(now) + ser_string(redd_id.encode())
    for _ in range(6):
        r += i32(score * 100)
    r += i64(last_calculated) + bytes(32) + ser_string(b"")
    return r


def user_auction_id(name, ns_id, start):
    return hash256(ser_string(name.encode()) + ser_string(ns_id.encode()) + i64(start))


# --- Steps that several tests share ---

def run_expiry_task(rpc):
    """Run the periodic ReddID task of the node now and return when it has run.

    The task runs once a minute of real time and acts on the clock of the node:
    it finalizes the auctions that have ended and erases what has expired.
    mockscheduler makes it due at once, and the scheduler runs it before the
    validation queue that the second call waits for.

    A test that moves the clock past the end of an auction calls this first.
    The task then does not run by itself for another minute of real time. The
    steps that need it not to have run, from the change of the clock on, go
    inside expiry_task_idle, which fails if the minute was not enough.
    """
    rpc.mockscheduler(60)
    rpc.syncwithvalidationinterfacequeue()


@contextmanager
def expiry_task_idle(*nodes):
    """Fail if the periodic task of one of the nodes runs inside the with statement.

    For the steps after run_expiry_task whose result is another one once the
    task has run again: it finalizes a user ID auction that has ended with a
    bid and cancels one that has ended with none. The nodes need -debug=reddid.

    The log of each node is checked when the statement ends, by
    assert_debug_log, and also when a step inside it fails, so that a failure
    that the task caused is reported as that.
    """
    logs = [os.path.join(node.datadir, node.chain, 'debug.log') for node in nodes]
    sizes = [os.path.getsize(log) for log in logs]
    with ExitStack() as stack:
        for node in nodes:
            stack.enter_context(node.assert_debug_log([], unexpected_msgs=[EXPIRY_TASK_LOG]))
        try:
            yield
        except Exception as e:
            for node, path, size in zip(nodes, logs, sizes):
                with open(path, encoding='utf-8') as log:
                    log.seek(size)
                    if EXPIRY_TASK_LOG in log.read():
                        raise AssertionError("The periodic ReddID task of node %d ran during steps that need it not to (\"%s\" in its log); the failure above follows from that" % (node.index, EXPIRY_TASK_LOG)) from e
            raise


def advance_clock(nodes, seconds):
    """Move the clock of every node on by the given number of seconds."""
    set_node_times(nodes, max(node.mocktime for node in nodes) + seconds)


def keep_clock(node, extra_args):
    """Arguments to restart a node on the clock that it has.

    A node that is restarted without them returns on the wall clock, years
    after the mock time of the cached chain, and its periodic task erases
    every namespace, user ID and profile as expired.
    """
    assert node.mocktime
    return list(extra_args) + ["-mocktime=%d" % node.mocktime]


def mine_block(nodes, rpc, address):
    """Stake one block with the wallet behind rpc, a minute after the later of the tip and the clock."""
    tip_time = rpc.getblockheader(rpc.getbestblockhash())['time']
    set_node_times(nodes, max(max(node.mocktime for node in nodes), tip_time) + 60)
    for _ in range(10):
        try:
            return rpc.generatetoaddress(1, address)[0]
        except JSONRPCException as e:
            if "no valid coinstake found" not in e.error['message']:
                raise
            # Widen the window in which a kernel is searched
            advance_clock(nodes, 300)
    raise AssertionError("no block was staked")


def acquire_namespace(nodes, rpc, ns_id, reserve=50000, bid=60000):
    """Take a namespace through its auction with the commands of one node.

    Creates the auction, bids once and finalizes the auction after its 14 days.
    Returns the address that owns the namespace: the key of the bid.
    """
    auction = rpc.createnamespacesauction(ns_id, reserve, 14)
    winner = rpc.bidonnamespacesauction(auction['auction_id'], bid)['bidder_address']
    run_expiry_task(rpc)
    advance_clock(nodes, 14 * DAY + 3600)
    result = rpc.finalizenamespacesauction(auction['auction_id'])
    assert result['winner'] == winner
    return winner
