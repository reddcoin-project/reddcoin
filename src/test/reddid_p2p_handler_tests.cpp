// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/reddid_p2p.h>
#include <net.h>
#include <streams.h>
#include <test/reddid_p2p_test_util.h>
#include <uint256.h>
#include <util/string.h>
#include <util/time.h>
#include <version.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace reddid_test;

// What ReddIDP2PManager does with each of the 18 message types: what it
// rejects, what it changes in the component managers and what it sends on.
//
// The handlers are those of reddid_dev. Several of them change the state of
// this node on the word of a peer, with no transaction, signature or deposit
// behind the message; they are ported as they are, because they are how state
// reaches other nodes today, and each such path is pinned here by a case
// marked SUSPECTED BUG, so that a later fix knows which tests must change.

namespace {
using Commands = std::vector<std::string>;

const int64_t FAR_FUTURE = std::numeric_limits<int64_t>::max();

/**
 * P2PTestingSetup with two subscribed peers: `from`, which sends the messages
 * under test, and `other`, which shows what the node relays.
 */
struct HandlerTestingSetup : public P2PTestingSetup {
    CNode& from;
    CNode& other;

    HandlerTestingSetup() : from(AddPeer()), other(AddPeer()) {}

    /** The commands queued for the peer since the last call. */
    Commands Take(CNode& peer)
    {
        Commands commands = SentCommands(peer);
        ClearSent(peer);
        return commands;
    }

    /** Nothing is queued for either peer. */
    bool NothingSent() { return SentMessages(from).empty() && SentMessages(other).empty(); }

    /** A user ID auction in NS with one bid, which makes it active. */
    uint256 ActiveAuction(const std::string& name)
    {
        const uint256 auctionId = NewAuction(name);
        uint256 bidId;
        bool placed = aMgr->PlaceBid(auctionId, MakeKeyID(), 5000 * COIN, bidId, 0);
        assert(placed);
        ClearSent(from);
        ClearSent(other);
        return auctionId;
    }

    /** A pending user ID auction in NS; the announcement that creating it caused is dropped. */
    uint256 PendingAuction(const std::string& name)
    {
        const uint256 auctionId = NewAuction(name);
        ClearSent(from);
        ClearSent(other);
        return auctionId;
    }

    /** A pending namespace auction held by the namespace manager. */
    uint256 NamespaceAuction(const std::string& nsId, const CKeyID& owner)
    {
        uint256 auctionId;
        bool created = nsMgr->CreateNamespaceAuction(nsId, owner, 20000 * COIN, 7, AUCTION_STANDARD, auctionId);
        assert(created);
        ClearSent(from);
        ClearSent(other);
        return auctionId;
    }

    CAuctionFinalize FinalizeMessage(const uint256& auctionId)
    {
        CAuctionFinalize msg;
        msg.timestamp = GetTime();
        msg.auctionId = auctionId;
        msg.winningBidId = InsecureRand256();
        msg.finalPrice = 1;
        msg.creator = MakeKeyID();
        return msg;
    }

    CReddIDConnection ConnectionMessage(const std::string& fromId, const std::string& toId, SocialConnectionType type)
    {
        CReddIDConnection msg;
        msg.timestamp = GetTime();
        msg.connection = TestConnection(fromId, toId, type);
        return msg;
    }

    CReddIDReputationUpdate ReputationMessage(const std::string& reddId, double score, int64_t lastCalculated)
    {
        CReddIDReputationUpdate msg;
        msg.timestamp = GetTime();
        msg.reputation.reddId = reddId;
        msg.reputation.overallScore = score;
        msg.reputation.longevityScore = score;
        msg.reputation.reliabilityScore = score;
        msg.reputation.engagementScore = score;
        msg.reputation.verificationScore = score;
        msg.reputation.auctionScore = score;
        msg.reputation.lastCalculated = lastCalculated;
        return msg;
    }

    ReddIDReputation GetReputation(const std::string& reddId)
    {
        ReddIDReputation reputation;
        bool found = pMgr->GetReputation(reddId, reputation);
        assert(found);
        return reputation;
    }

    NamespaceInfo GetNamespace(const std::string& nsId)
    {
        NamespaceInfo info;
        bool found = nsMgr->GetNamespaceInfo(nsId, info);
        assert(found);
        return info;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(reddid_p2p_handler_tests, HandlerTestingSetup)

// ---------------------------------------------------------------------------
// nsaucann
// ---------------------------------------------------------------------------

namespace {
CNamespaceAuctionAnnounce NamespaceAnnounce(const std::string& nsId)
{
    CNamespaceAuctionAnnounce msg;
    msg.timestamp = T0;
    msg.auctionId = InsecureRand256();
    msg.namespaceId = nsId;
    msg.creator = CKeyID(uint160(std::vector<unsigned char>(20, 0x11)));
    msg.startTime = T0;
    msg.endTime = T0 + 7 * DAY;
    msg.reservePrice = 20000 * COIN;
    msg.type = AUCTION_STANDARD;
    return msg;
}
} // namespace

BOOST_AUTO_TEST_CASE(nsaucann_rejections)
{
    // Empty and over-long namespace IDs
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", NamespaceAnnounce("")), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", NamespaceAnnounce(std::string(MAX_NAMESPACE_LENGTH + 1, 'a'))), Outcome::REJECTED);
    // Not a valid namespace ID
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", NamespaceAnnounce("Fresh")), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", NamespaceAnnounce("-fresh")), Outcome::REJECTED);
    // The namespace exists already
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", NamespaceAnnounce(NS)), Outcome::REJECTED);

    // The auction does not end after it starts
    CNamespaceAuctionAnnounce badTimes = NamespaceAnnounce("fresh");
    badTimes.endTime = badTimes.startTime;
    {
        // No announcement is ever accepted (see the next case), so the reason
        // for the rejection is read from the log
        LogCounter rejected("Invalid auction times for fresh");
        BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", badTimes), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(rejected.count, 1);
    }

    // The reserve price is below the minimum for a namespace of this length
    CNamespaceAuctionAnnounce cheap = NamespaceAnnounce("fresh");
    cheap.reservePrice = 10000 * COIN - 1;
    {
        LogCounter rejected("Reserve price too low for namespace fresh");
        BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", cheap), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(rejected.count, 1);
    }

    // A payload that is not an announcement
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucann", uint256::ONE), Outcome::REJECTED);

    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: a namespace auction announcement can never create an auction.
// The handler requires that the namespace does not exist yet, forces the
// creator to the null key and hands the auction to AuctionManager, which
// requires that the namespace exists and that the creator is not null.
BOOST_AUTO_TEST_CASE(nsaucann_can_never_succeed)
{
    const CNamespaceAuctionAnnounce msg = NamespaceAnnounce("fresh");
    {
        LogCounter failed("Failed to create auction from announcement");
        LogCounter noNamespace("Error: AuctionManager::ValidateAuctionParameters: Namespace not found: fresh");
        BOOST_CHECK_EQUAL(SendMsg(from, "nsaucann", msg), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(failed.count, 1);
        BOOST_CHECK_EQUAL(noNamespace.count, 1);
    }
    AuctionInfo auction;
    BOOST_CHECK(!aMgr->GetAuctionInfo(msg.auctionId, auction));
    BOOST_CHECK(!nsMgr->GetAuctionInfo(msg.auctionId, auction));
    BOOST_CHECK(nsMgr->GetActiveAuctions().empty());
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK(NothingSent());
}

// The announcement that this node makes for a namespace auction of its own is
// rejected by the handler, so another node running the same code rejects it.
BOOST_AUTO_TEST_CASE(nsaucann_rejects_what_the_namespace_manager_announces)
{
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("fresh", MakeKeyID(), 20000 * COIN, 7, AUCTION_STANDARD, auctionId));
    const std::vector<SentMessage> sent = SentMessages(other);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_REQUIRE_EQUAL(sent[0].command, "nsaucann");
    const CNamespaceAuctionAnnounce announced = Decode<CNamespaceAuctionAnnounce>(sent[0]);
    ClearSent(from);
    ClearSent(other);

    // This node knows the auction, through the database that the namespace
    // manager and the auction manager share: the handler reports success and
    // does nothing.
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucann", announced), Outcome::ACCEPTED);
    BOOST_CHECK(NothingSent());

    // A node that does not know it: the same announcement under another ID
    CNamespaceAuctionAnnounce unknown = announced;
    unknown.auctionId = InsecureRand256();
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucann", unknown), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());
}

// ---------------------------------------------------------------------------
// nsaucbid
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(nsaucbid_rejections)
{
    const CKeyID bidder = MakeKeyID();

    // Unknown auction
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucbid", BidMessage(InsecureRand256(), bidder, 6000 * COIN)), Outcome::REJECTED);

    // Below the reserve price
    const uint256 auctionId = PendingAuction("alice");
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucbid", BidMessage(auctionId, bidder, 5000 * COIN - 1)), Outcome::REJECTED);
    BOOST_CHECK(GetAuction(aMgr, auctionId).state == AUCTION_PENDING);

    // Below the minimum increment over the current bid: 5% in this namespace
    const uint256 active = ActiveAuction("bobby");
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucbid", BidMessage(active, bidder, 5250 * COIN - 1)), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, active).currentBid, 5000 * COIN);

    // A canceled auction
    const uint256 canceled = PendingAuction("carol");
    BOOST_REQUIRE(aMgr->CancelAuction(canceled, creator));
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucbid", BidMessage(canceled, bidder, 6000 * COIN)), Outcome::REJECTED);

    // An auction that has ended
    const uint256 ended = PendingAuction("david");
    SetMockTime(GetAuction(aMgr, ended).endTime);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucbid", BidMessage(ended, bidder, 6000 * COIN)), Outcome::REJECTED);
    SetMockTime(T0);

    // A payload that is not a bid
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucbid", uint256::ONE), Outcome::REJECTED);

    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    ClearSent(from);
    ClearSent(other);
}

// SUSPECTED BUG: a bid message places a bid. No transaction carries the bid,
// nothing shows that the sender controls the bidder key and no deposit is
// locked. The message is named after namespace auctions but bids on any
// auction that the auction manager holds, which are the user ID auctions.
BOOST_AUTO_TEST_CASE(nsaucbid_places_a_bid_without_transaction_or_deposit)
{
    const uint256 auctionId = PendingAuction("alice");
    const CKeyID victim = MakeKeyID();
    CAuctionBid msg = BidMessage(auctionId, victim, 9000 * COIN);
    // The bid ID and the deposit of the message are ignored
    msg.bidId = InsecureRand256();
    msg.depositAmount = 1;

    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", msg), Outcome::ACCEPTED);

    const AuctionInfo auction = GetAuction(aMgr, auctionId);
    BOOST_CHECK(auction.state == AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, 9000 * COIN);
    BOOST_CHECK(auction.currentBidder == victim);

    const std::vector<BidInfo> bids = aMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidder == victim);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, 9000 * COIN);
    BOOST_CHECK_EQUAL(bids[0].bidTime, msg.timestamp);
    BOOST_CHECK(bids[0].bidId == ExpectedBidId(auctionId, victim, 9000 * COIN, msg.timestamp));
    BOOST_CHECK(bids[0].bidId != msg.bidId);
    BOOST_CHECK_EQUAL(bids[0].depositAmount, auction.depositAmount);
    BOOST_CHECK(bids[0].depositUtxo.IsNull());
    BOOST_CHECK(bids[0].txid.IsNull());
    // Nothing is locked for it
    BOOST_CHECK(aMgr->GetDepositLocksForAuction(auctionId).empty());
    // It is in the database as well
    BidInfo stored;
    BOOST_CHECK(db->ReadBid(bids[0].bidId, stored));

    // The auction manager announces the bid as a user ID bid to every
    // subscribed peer, the sender included, and the handler then relays the
    // message as it came to every subscribed peer but the sender.
    BOOST_CHECK(Take(from) == (Commands{"uidaucbid"}));
    BOOST_CHECK(Take(other) == (Commands{"uidaucbid", "nsaucbid"}));

    // A bid that this node knows under the ID of the message is not placed again
    CAuctionBid known = BidMessage(auctionId, MakeKeyID(), 20000 * COIN);
    known.bidId = bids[0].bidId;
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", known), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 9000 * COIN);
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the time of the bid is the timestamp of the message, which
// the peer chooses, and the anti-sniping rule sets the end of the auction from
// it: an auction that would run for a week can be ended in six hours, or kept
// open for as long as the peer likes.
BOOST_AUTO_TEST_CASE(nsaucbid_moves_the_end_of_the_auction_with_its_timestamp)
{
    // Shortened: "bid" 23 hours before the end
    const uint256 shortened = PendingAuction("alice");
    const int64_t end = GetAuction(aMgr, shortened).endTime;
    BOOST_CHECK_EQUAL(end, T0 + 7 * DAY);
    CAuctionBid early = BidMessage(shortened, MakeKeyID(), 6000 * COIN);
    early.timestamp = T0 - 18 * HOUR;
    // Not within a day of the end: nothing moves
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", early), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, shortened).endTime, end);
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(shortened)[0].bidTime, T0 - 18 * HOUR);

    CAuctionBid sniping = BidMessage(shortened, MakeKeyID(), 7000 * COIN);
    sniping.timestamp = end - 7 * DAY + 1;
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", sniping), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, shortened).endTime, end);

    CAuctionBid shorten = BidMessage(shortened, MakeKeyID(), 8000 * COIN);
    shorten.timestamp = end - 23 * HOUR;
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", shorten), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, shortened).endTime, end - 17 * HOUR);

    // Lengthened: "bid" a thousand days after the end
    const uint256 lengthened = PendingAuction("bobby");
    CAuctionBid late = BidMessage(lengthened, MakeKeyID(), 6000 * COIN);
    late.timestamp = end + 1000 * DAY;
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", late), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, lengthened).endTime, end + 1000 * DAY + 6 * HOUR);

    // A timestamp of 0 is replaced by the clock of this node
    const uint256 untimed = PendingAuction("carol");
    CAuctionBid zero = BidMessage(untimed, MakeKeyID(), 6000 * COIN);
    zero.timestamp = 0;
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", zero), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(untimed)[0].bidTime, T0);
}

// Current behaviour: the auction manager makes a pending auction active before
// it checks the bid, so a bid that is then refused, here because it is in the
// name of the creator, leaves the auction active.
BOOST_AUTO_TEST_CASE(nsaucbid_that_is_refused_can_still_activate_the_auction)
{
    const uint256 auctionId = PendingAuction("alice");
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", BidMessage(auctionId, creator, 6000 * COIN)), Outcome::REJECTED);
    const AuctionInfo auction = GetAuction(aMgr, auctionId);
    BOOST_CHECK(auction.state == AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, 0);
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(NothingSent());
}

// Current behaviour: the handler finds a namespace auction, which the
// namespace manager holds, through the database that the two managers share,
// but the auction manager, which is to place the bid, does not hold it.
// A bid on a real namespace auction is always refused.
BOOST_AUTO_TEST_CASE(nsaucbid_cannot_bid_on_a_namespace_auction)
{
    const uint256 auctionId = NamespaceAuction("fresh", MakeKeyID());
    {
        LogCounter notFound("Error: AuctionManager::PlaceBid: Auction not found: " + auctionId.ToString());
        BOOST_CHECK_EQUAL(SendMsg(from, "nsaucbid", BidMessage(auctionId, MakeKeyID(), 30000 * COIN)), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(notFound.count, 1);
    }
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK(auction.state == AUCTION_PENDING);
    BOOST_CHECK_EQUAL(auction.currentBid, 0);
    BOOST_CHECK(NothingSent());
}

// ---------------------------------------------------------------------------
// nsaucfin
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(nsaucfin_rejections)
{
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucfin", FinalizeMessage(InsecureRand256())), Outcome::REJECTED);
    // A pending auction cannot be finalized
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucfin", FinalizeMessage(PendingAuction("alice"))), Outcome::REJECTED);
    const uint256 canceled = PendingAuction("bobby");
    BOOST_REQUIRE(aMgr->CancelAuction(canceled, creator));
    ClearSent(from);
    ClearSent(other);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucfin", FinalizeMessage(canceled)), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsaucfin", uint256::ONE), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the finalization of an active auction is relayed and nothing
// else happens. The winning bid, the price and the creator of the message are
// not looked at, the auction is not finalized, and the node does not remember
// that it has seen the message: every copy is relayed again.
BOOST_AUTO_TEST_CASE(nsaucfin_is_relayed_without_changing_anything)
{
    const uint256 auctionId = ActiveAuction("alice");
    const AuctionInfo before = GetAuction(aMgr, auctionId);
    const CAuctionFinalize msg = FinalizeMessage(auctionId);

    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_EQUAL(SendMsg(from, "nsaucfin", msg), Outcome::ACCEPTED);
        BOOST_CHECK(Take(other) == (Commands{"nsaucfin"}));
        BOOST_CHECK(Take(from).empty());
    }
    // The message is relayed byte for byte
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucfin", msg), Outcome::ACCEPTED);
    BOOST_CHECK(SentMessages(other)[0].payload == PayloadBytes(msg));

    const AuctionInfo after = GetAuction(aMgr, auctionId);
    BOOST_CHECK(PayloadBytes(after) == PayloadBytes(before));
    BOOST_CHECK(after.state == AUCTION_ACTIVE);
}

// The same for a namespace auction, which the handler finds through the
// database that the auction manager shares with the namespace manager.
BOOST_AUTO_TEST_CASE(nsaucfin_is_relayed_for_a_namespace_auction)
{
    const CKeyID owner = MakeKeyID();
    const uint256 auctionId = NamespaceAuction("fresh", owner);
    // Pending: refused
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucfin", FinalizeMessage(auctionId)), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());

    uint256 bidId;
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(auctionId, MakeKeyID(), 21000 * COIN, bidId));
    ClearSent(from);
    ClearSent(other);
    BOOST_CHECK_EQUAL(SendMsg(from, "nsaucfin", FinalizeMessage(auctionId)), Outcome::ACCEPTED);
    BOOST_CHECK(Take(other) == (Commands{"nsaucfin"}));
    BOOST_CHECK(Take(from).empty());

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK(auction.state == AUCTION_ACTIVE);
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
}

// ---------------------------------------------------------------------------
// nsauccan
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(nsauccan_rejections)
{
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsauccan", InsecureRand256()), Outcome::REJECTED);

    // An auction that was canceled already
    const CKeyID owner = MakeKeyID();
    const uint256 canceled = NamespaceAuction("fresh", owner);
    BOOST_REQUIRE(nsMgr->CancelNamespaceAuction(canceled, owner));
    ClearSent(from);
    ClearSent(other);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsauccan", canceled), Outcome::REJECTED);

    // An active auction with a bid
    const uint256 active = NamespaceAuction("other", owner);
    uint256 bidId;
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(active, MakeKeyID(), 21000 * COIN, bidId));
    ClearSent(from);
    ClearSent(other);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsauccan", active), Outcome::REJECTED);

    // A payload that is too short for an auction ID
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsauccan", uint8_t{1}), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the cancellation of a pending auction is relayed and nothing
// else happens: the auction is not canceled, anyone can send the message, and
// every copy is relayed again.
BOOST_AUTO_TEST_CASE(nsauccan_is_relayed_without_changing_anything)
{
    const uint256 auctionId = NamespaceAuction("fresh", MakeKeyID());
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_EQUAL(SendMsg(from, "nsauccan", auctionId), Outcome::ACCEPTED);
        BOOST_CHECK(Take(other) == (Commands{"nsauccan"}));
        BOOST_CHECK(Take(from).empty());
    }
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK(auction.state == AUCTION_PENDING);

    // Current behaviour: the namespace manager finds user ID auctions through
    // the database that it shares with the auction manager, so the
    // cancellation of a user ID auction is relayed under this command too.
    const uint256 userAuction = PendingAuction("alice");
    BOOST_CHECK_EQUAL(SendMsg(from, "nsauccan", userAuction), Outcome::ACCEPTED);
    BOOST_CHECK(Take(other) == (Commands{"nsauccan"}));
    BOOST_CHECK(GetAuction(aMgr, userAuction).state == AUCTION_PENDING);
}

// ---------------------------------------------------------------------------
// nsconreq
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(nsconreq)
{
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconreq", std::string()), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconreq", std::string(MAX_NAMESPACE_LENGTH + 1, 'a')), Outcome::REJECTED);
    // A namespace that this node does not know: no reply
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconreq", std::string("unknown")), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());

    // A known namespace: the configuration goes to the peer that asked
    BOOST_CHECK(nsMgr->AddPricingTier(NS, 5, 8000 * COIN));
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconreq", NS), Outcome::ACCEPTED);
    BOOST_CHECK(SentMessages(other).empty());
    const std::vector<SentMessage> sent = SentMessages(from);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "nsconres");
    const CNamespaceConfig reply = Decode<CNamespaceConfig>(sent[0]);
    BOOST_CHECK(PayloadBytes(reply.config) == PayloadBytes(GetNamespace(NS)));
    BOOST_REQUIRE_EQUAL(reply.pricingTiers.size(), 1U);
    BOOST_CHECK_EQUAL(reply.pricingTiers[0].minPrice, 8000 * COIN);

    // A peer that is not subscribed is answered as well
    CNode& stranger = AddPeer(/* subscribe */ false);
    BOOST_CHECK_EQUAL(SendMsg(stranger, "nsconreq", NS), Outcome::ACCEPTED);
    BOOST_CHECK(SentCommands(stranger) == (Commands{"nsconres"}));
}

// SUSPECTED BUG: the reply to a request can be a message that the handler of
// nsconres rejects, because the stored hash of a namespace is not kept in step
// with its configuration. NamespaceManager::CreateNamespace computes the hash
// before it sets the expiry of a namespace that comes without one, and
// RenewNamespace and TransferNamespace change the expiry and the owner, which
// the hash covers, without computing it again. A node that asks for such a
// namespace cannot use the answer.
BOOST_AUTO_TEST_CASE(nsconreq_reply_can_be_rejected_by_the_handler_of_nsconres)
{
    // The namespace of the fixture was created without an expiry
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconreq", NS), Outcome::ACCEPTED);
    const CNamespaceConfig reply = Decode<CNamespaceConfig>(SentMessages(from)[0]);
    BOOST_CHECK(reply.config.expiration != 0);
    BOOST_CHECK(reply.config.configHash != nsMgr->CalculateConfigHash(reply.config));
    NamespaceInfo withoutExpiry = reply.config;
    withoutExpiry.expiration = 0;
    BOOST_CHECK(reply.config.configHash == nsMgr->CalculateConfigHash(withoutExpiry));

    // The hash is checked before anything that depends on what the node
    // knows, so this is what a node that does not know the namespace says
    {
        LogCounter mismatch("Config hash mismatch for namespace " + NS);
        BOOST_CHECK_EQUAL(SendMsg(other, "nsconres", reply), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(mismatch.count, 1);
    }

    // A namespace that was created with an expiry has a hash that matches
    const CKeyID owner = MakeKeyID();
    CNamespaceConfig msg = ConfigMessage("fresh", owner);
    msg.config.expiration = T0 + 100 * DAY;
    msg.config.configHash = nsMgr->CalculateConfigHash(msg.config);
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", msg), Outcome::ACCEPTED);
    ClearSent(from);
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconreq", std::string("fresh")), Outcome::ACCEPTED);
    const CNamespaceConfig matching = Decode<CNamespaceConfig>(SentMessages(from)[0]);
    BOOST_CHECK(matching.config.configHash == nsMgr->CalculateConfigHash(matching.config));
    ClearSent(from);

    // Until it is transferred: the record of the namespace manager has the
    // new owner and the hash of the old configuration
    const CKeyID buyer = MakeKeyID();
    BOOST_REQUIRE(nsMgr->TransferNamespace("fresh", owner, buyer));
    NamespaceInfo record;
    for (const NamespaceInfo& info : nsMgr->GetNamespaces()) {
        if (info.id == "fresh") record = info;
    }
    BOOST_CHECK(record.owner == buyer);
    BOOST_CHECK(record.configHash == matching.config.configHash);
    BOOST_CHECK(record.configHash != nsMgr->CalculateConfigHash(record));

    // A peer that asks now is told the new owner, with that hash: the reply
    // is the record as it is, and not as it was when it was last looked up
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconreq", std::string("fresh")), Outcome::ACCEPTED);
    const CNamespaceConfig transferred = Decode<CNamespaceConfig>(SentMessages(from)[0]);
    BOOST_CHECK(transferred.config.owner == buyer);
    BOOST_CHECK(transferred.config.configHash == matching.config.configHash);
}

// ---------------------------------------------------------------------------
// nsconres
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(nsconres_rejections)
{
    const CKeyID owner = MakeKeyID();
    auto rehash = [&](CNamespaceConfig msg) {
        msg.config.configHash = nsMgr->CalculateConfigHash(msg.config);
        return msg;
    };

    // Empty and over-long IDs
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", ConfigMessage("", owner)), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", ConfigMessage(std::string(MAX_NAMESPACE_LENGTH + 1, 'a'), owner)), Outcome::REJECTED);

    // The shares do not add up to 100
    CNamespaceConfig shares = ConfigMessage("fresh", owner);
    shares.config.burnPct = 71;
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", rehash(shares)), Outcome::REJECTED);

    // A minimum bid increment outside 1% to 20%: a peer cannot give a node a
    // namespace in which the next bid may be lower, or has to be absurdly higher
    for (double increment : {-5.0, 0.0, 0.99, 20.01, 21474836.0}) {
        CNamespaceConfig bad = ConfigMessage("fresh", owner);
        bad.config.minBidIncrement = increment;
        BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", rehash(bad)), Outcome::REJECTED);
    }

    // The hash of the message is not the hash of its configuration
    CNamespaceConfig badHash = ConfigMessage("fresh", owner);
    badHash.config.maxLength = 20;
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", badHash), Outcome::REJECTED);
    badHash = ConfigMessage("fresh", owner);
    badHash.config.configHash.SetNull();
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", badHash), Outcome::REJECTED);

    // Configurations that the namespace manager refuses
    CNamespaceConfig badId = ConfigMessage("Fresh", owner);
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", badId), Outcome::REJECTED);
    CNamespaceConfig badLength = ConfigMessage("fresh", owner);
    badLength.config.minLength = 0;
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", rehash(badLength)), Outcome::REJECTED);
    CNamespaceConfig badShare = ConfigMessage("fresh", owner);
    badShare.config.burnPct = 40;
    badShare.config.nodePct = 45;
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", rehash(badShare)), Outcome::REJECTED);

    // A payload that is not a configuration
    BOOST_CHECK_EQUAL(SendFromNewPeer("nsconres", uint256::ONE), Outcome::REJECTED);

    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK(NothingSent());
}

BOOST_AUTO_TEST_CASE(nsconres_pricing_tier_limit)
{
    PricingTier tier;
    tier.namespaceId = "fresh";
    tier.minPrice = 6000 * COIN;

    CNamespaceConfig tooMany = ConfigMessage("fresh", MakeKeyID());
    for (size_t i = 0; i <= MAX_PRICING_TIERS; ++i) {
        tier.minLength = i + 1;
        tooMany.pricingTiers.push_back(tier);
    }
    BOOST_CHECK_EQUAL(tooMany.pricingTiers.size(), MAX_PRICING_TIERS + 1);
    {
        LogCounter rejected("Rejecting namespace config with 101 pricing tiers (max 100)");
        BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", tooMany), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(rejected.count, 1);
    }
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK(NothingSent());

    CNamespaceConfig most = ConfigMessage("fresh", MakeKeyID());
    most.pricingTiers.assign(tooMany.pricingTiers.begin(), tooMany.pricingTiers.end() - 1);
    BOOST_CHECK_EQUAL(most.pricingTiers.size(), MAX_PRICING_TIERS);
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", most), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(nsMgr->GetPricingTiers("fresh").size(), MAX_PRICING_TIERS);
}

// SUSPECTED BUG: a namespace configuration that nobody asked for creates the
// namespace, with the owner that the peer chose, and is relayed. No auction
// was held and no transaction is behind it. The "hash verification" compares
// the configuration with a hash that the same message carries, which shows
// only that the message is consistent with itself.
BOOST_AUTO_TEST_CASE(nsconres_creates_a_namespace_with_the_owner_the_peer_chose)
{
    const CKeyID attacker = MakeKeyID();
    CNamespaceConfig msg = ConfigMessage("fresh", attacker);
    msg.config.lastUpdated = 12345;
    PricingTier tier;
    // The namespace of a tier is ignored: the tier goes to the namespace of the message
    tier.namespaceId = "elsewhere";
    tier.minLength = 4;
    tier.minPrice = 6000 * COIN;
    msg.pricingTiers.push_back(tier);

    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", msg), Outcome::ACCEPTED);

    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK(nsMgr->IsOwner("fresh", attacker));
    const NamespaceInfo stored = GetNamespace("fresh");
    BOOST_CHECK(stored.owner == attacker);
    // The time of the update is that of this node; the expiry follows from it
    BOOST_CHECK_EQUAL(stored.lastUpdated, T0);
    BOOST_CHECK_EQUAL(stored.expiration, T0 + 365 * DAY);
    const std::vector<PricingTier> tiers = nsMgr->GetPricingTiers("fresh");
    BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
    BOOST_CHECK_EQUAL(tiers[0].namespaceId, "fresh");
    BOOST_CHECK(nsMgr->GetPricingTiers("elsewhere").empty());
    // It is in the database as well
    NamespaceInfo inDb;
    BOOST_CHECK(db->ReadNamespace("fresh", inDb));
    BOOST_CHECK(inDb.owner == attacker);

    // Relayed as it came, to the subscribed peers other than the sender
    BOOST_CHECK(Take(from).empty());
    const std::vector<SentMessage> relayed = SentMessages(other);
    BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
    BOOST_CHECK_EQUAL(relayed[0].command, "nsconres");
    BOOST_CHECK(relayed[0].payload == PayloadBytes(msg));

    // User ID auctions can be announced in it at once, at the price of its tier
    CUserIDAuctionAnnounce announce = UserAnnounce("alice", MakeKeyID());
    announce.namespaceId = "fresh";
    BOOST_CHECK_EQUAL(SendMsg(from, "uidaucann", announce), Outcome::REJECTED);
    announce.reservePrice = 6000 * COIN;
    BOOST_CHECK_EQUAL(SendMsg(from, "uidaucann", announce), Outcome::ACCEPTED);
}

// SUSPECTED BUG: the expiry of the namespace is taken from the message when
// it is not 0.
BOOST_AUTO_TEST_CASE(nsconres_takes_the_expiry_from_the_peer)
{
    CNamespaceConfig msg = ConfigMessage("fresh", MakeKeyID());
    msg.config.expiration = FAR_FUTURE;
    msg.config.configHash = nsMgr->CalculateConfigHash(msg.config);
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", msg), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetNamespace("fresh").expiration, FAR_FUTURE);
}

// A namespace that this node knows is never replaced: an older or equally old
// configuration is acknowledged and dropped, and a newer one is refused by
// NamespaceManager::CreateNamespace, which does not overwrite.
BOOST_AUTO_TEST_CASE(nsconres_does_not_replace_a_known_namespace)
{
    const NamespaceInfo before = GetNamespace(NS);
    const CKeyID attacker = MakeKeyID();

    CNamespaceConfig older = ConfigMessage(NS, attacker);
    older.config.lastUpdated = before.lastUpdated;
    BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", older), Outcome::ACCEPTED);
    BOOST_CHECK(PayloadBytes(GetNamespace(NS)) == PayloadBytes(before));
    BOOST_CHECK(NothingSent());

    CNamespaceConfig newer = ConfigMessage(NS, attacker);
    newer.config.lastUpdated = FAR_FUTURE;
    {
        LogCounter failed("Failed to create namespace " + NS);
        BOOST_CHECK_EQUAL(SendMsg(from, "nsconres", newer), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(failed.count, 1);
    }
    BOOST_CHECK(PayloadBytes(GetNamespace(NS)) == PayloadBytes(before));
    BOOST_CHECK(nsMgr->IsOwner(NS, creator));
    BOOST_CHECK(NothingSent());
}

// ---------------------------------------------------------------------------
// uidaucann, ridaucann
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(uidaucann_rejections)
{
    const CKeyID seller = MakeKeyID();
    NamespaceInfo strict;
    strict.id = "strict";
    strict.owner = MakeKeyID();
    BOOST_REQUIRE(nsMgr->CreateNamespace(strict));
    UserIDInfo taken;
    taken.name = "taken";
    taken.namespaceId = NS;
    taken.owner = MakeKeyID();
    taken.expirationTime = T0 + 1;
    BOOST_REQUIRE(db->WriteUserID("taken", NS, taken));

    for (const std::string command : {"uidaucann", "ridaucann"}) {
        auto with = [&](const std::function<void(CUserIDAuctionAnnounce&)>& change) {
            CUserIDAuctionAnnounce msg = UserAnnounce("alice", seller);
            change(msg);
            return SendFromNewPeer(command, msg);
        };

        // Names
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.name = ""; }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.name = std::string(MAX_REDDID_LENGTH + 1, 'a'); }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.name = "ab"; }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.name = "Alice"; }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.name = "al ice"; }), Outcome::REJECTED);
        // Digits, hyphens and underscores where the namespace does not allow them
        for (const std::string name : {"alice1", "al-ice", "al_ice"}) {
            BOOST_CHECK_EQUAL(with([&](CUserIDAuctionAnnounce& m) { m.name = name; m.namespaceId = "strict"; }), Outcome::REJECTED);
        }
        // Namespaces
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.namespaceId = ""; }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.namespaceId = std::string(MAX_NAMESPACE_LENGTH + 1, 'a'); }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.namespaceId = "unknown"; }), Outcome::REJECTED);
        // The user ID exists and has not expired
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.name = "taken"; }), Outcome::REJECTED);
        // Duration outside the 3 to 7 days of the namespace
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.endTime = m.startTime + 3 * DAY - 1; }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.endTime = m.startTime + 8 * DAY; }), Outcome::REJECTED);
        // Reserve price below the minimum
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.reservePrice = 5000 * COIN - 1; }), Outcome::REJECTED);
        // What the auction manager refuses: no creator, no start time
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.creator = CKeyID(); }), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(with([](CUserIDAuctionAnnounce& m) { m.startTime = 0; m.endTime = 7 * DAY; }), Outcome::REJECTED);
        // A payload that is not an announcement
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, uint256::ONE), Outcome::REJECTED);
    }

    BOOST_CHECK(aMgr->GetActiveAuctions().empty());
    AuctionInfo auction;
    BOOST_CHECK(!aMgr->GetAuctionInfo(ExpectedAuctionId("alice", NS, T0), auction));
    BOOST_CHECK(NothingSent());
}

// What the namespace allows is accepted.
BOOST_AUTO_TEST_CASE(uidaucann_follows_the_rules_of_the_namespace)
{
    for (const std::string& name : std::vector<std::string>{"alice1", "al-ice", "al_ice", "abc", std::string(32, 'a')}) {
        BOOST_CHECK_EQUAL(SendFromNewPeer("uidaucann", UserAnnounce(name, MakeKeyID())), Outcome::ACCEPTED);
    }
    // The user ID exists but has expired
    UserIDInfo expired;
    expired.name = "lapsed";
    expired.namespaceId = NS;
    expired.owner = MakeKeyID();
    expired.expirationTime = T0;
    BOOST_REQUIRE(db->WriteUserID("lapsed", NS, expired));
    BOOST_CHECK_EQUAL(SendFromNewPeer("uidaucann", UserAnnounce("lapsed", MakeKeyID())), Outcome::ACCEPTED);
}

// SUSPECTED BUG: an auction announcement creates the auction, with the creator
// and the start time that the peer chose. No transaction is behind it and
// nothing shows that the sender controls the creator key.
BOOST_AUTO_TEST_CASE(uidaucann_creates_an_auction_with_the_creator_the_peer_chose)
{
    int n = 0;
    for (const std::string command : {"uidaucann", "ridaucann"}) {
        const std::string name = "alice" + ToString(n++);
        const CKeyID victim = MakeKeyID();
        CUserIDAuctionAnnounce msg = UserAnnounce(name, victim);
        // A start time in the past, or in the future: it is not compared with the clock
        msg.startTime = (n == 1) ? T0 - 100 * DAY : T0 + 100 * DAY;
        msg.endTime = msg.startTime + 5 * DAY;
        msg.reservePrice = 7000 * COIN;
        msg.type = AUCTION_PREMIUM;
        // The auction ID of the message is not used
        msg.auctionId = InsecureRand256();

        BOOST_CHECK_EQUAL(SendMsg(from, command, msg), Outcome::ACCEPTED);

        const uint256 auctionId = ExpectedAuctionId(name, NS, msg.startTime);
        AuctionInfo unused;
        BOOST_CHECK(!aMgr->GetAuctionInfo(msg.auctionId, unused));
        const AuctionInfo auction = GetAuction(aMgr, auctionId);
        BOOST_CHECK_EQUAL(auction.name, name);
        BOOST_CHECK_EQUAL(auction.namespaceId, NS);
        BOOST_CHECK(auction.creator == victim);
        BOOST_CHECK_EQUAL(auction.startTime, msg.startTime);
        BOOST_CHECK_EQUAL(auction.endTime, msg.endTime);
        BOOST_CHECK_EQUAL(auction.reservePrice, 7000 * COIN);
        BOOST_CHECK_EQUAL(auction.depositAmount, 700 * COIN);
        BOOST_CHECK(auction.type == AUCTION_PREMIUM);
        BOOST_CHECK(auction.state == AUCTION_PENDING);
        BOOST_CHECK(auction.txid.IsNull());
        AuctionInfo stored;
        BOOST_CHECK(db->ReadAuction(auctionId, stored));

        // The auction manager announces the new auction to every subscribed
        // peer, the sender included; the handler then relays the message as it
        // came, always as "uidaucann", to every subscribed peer but the sender.
        const std::vector<SentMessage> back = SentMessages(from);
        BOOST_REQUIRE_EQUAL(back.size(), 1U);
        BOOST_CHECK_EQUAL(back[0].command, "uidaucann");
        BOOST_CHECK(Decode<CUserIDAuctionAnnounce>(back[0]).auctionId == auctionId);
        const std::vector<SentMessage> relayed = SentMessages(other);
        BOOST_REQUIRE_EQUAL(relayed.size(), 2U);
        BOOST_CHECK_EQUAL(relayed[0].command, "uidaucann");
        BOOST_CHECK(relayed[0].payload == back[0].payload);
        BOOST_CHECK_EQUAL(relayed[1].command, "uidaucann");
        BOOST_CHECK(relayed[1].payload == PayloadBytes(msg));
        ClearSent(from);
        ClearSent(other);

        // The same message again: the node does not recognise the auction by
        // the ID of the message, and the auction manager refuses the duplicate
        BOOST_CHECK_EQUAL(SendMsg(from, command, msg), Outcome::REJECTED);
        // With the ID that the node computed it is recognised and dropped
        msg.auctionId = auctionId;
        BOOST_CHECK_EQUAL(SendMsg(from, command, msg), Outcome::ACCEPTED);
        BOOST_CHECK(NothingSent());
    }
}

// ---------------------------------------------------------------------------
// uidaucbid, ridaucbid
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(uidaucbid_rejections)
{
    const CKeyID bidder = MakeKeyID();
    const uint256 pending = PendingAuction("alice");
    const uint256 active = ActiveAuction("bobby");
    const uint256 canceled = PendingAuction("carol");
    BOOST_REQUIRE(aMgr->CancelAuction(canceled, creator));
    // A namespace auction has no name. The auction manager does not hold it
    // but finds it in the database that it shares with the namespace manager.
    const uint256 namelessId = NamespaceAuction("fresh", MakeKeyID());

    for (const std::string command : {"uidaucbid", "ridaucbid"}) {
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, BidMessage(InsecureRand256(), bidder, 6000 * COIN)), Outcome::REJECTED);
        {
            LogCounter nameless("Auction " + namelessId.ToString() + " is not a user ID auction");
            BOOST_CHECK_EQUAL(SendFromNewPeer(command, BidMessage(namelessId, bidder, 30000 * COIN)), Outcome::REJECTED);
            BOOST_CHECK_EQUAL(nameless.count, 1);
        }
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, BidMessage(canceled, bidder, 6000 * COIN)), Outcome::REJECTED);
        // Below the reserve price, for the first bid
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, BidMessage(pending, bidder, 5000 * COIN - 1)), Outcome::REJECTED);
        // Below the minimum increment, for a later bid
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, BidMessage(active, bidder, 5250 * COIN - 1)), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, uint256::ONE), Outcome::REJECTED);
    }
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: a user ID bid is relayed and nothing else happens. The bid is
// not placed, the node does not remember that it has seen the message, so
// every copy is relayed again, and the end of the auction is not checked.
BOOST_AUTO_TEST_CASE(uidaucbid_is_relayed_without_changing_anything)
{
    const uint256 auctionId = PendingAuction("alice");
    const AuctionInfo before = GetAuction(aMgr, auctionId);

    for (const std::string command : {"uidaucbid", "ridaucbid"}) {
        const CAuctionBid msg = BidMessage(auctionId, MakeKeyID(), 6000 * COIN);
        for (int i = 0; i < 3; ++i) {
            BOOST_CHECK_EQUAL(SendMsg(from, command, msg), Outcome::ACCEPTED);
            // Both commands are relayed as "uidaucbid"
            const std::vector<SentMessage> relayed = SentMessages(other);
            BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
            BOOST_CHECK_EQUAL(relayed[0].command, "uidaucbid");
            BOOST_CHECK(relayed[0].payload == PayloadBytes(msg));
            ClearSent(other);
            BOOST_CHECK(Take(from).empty());
        }
    }
    BOOST_CHECK(PayloadBytes(GetAuction(aMgr, auctionId)) == PayloadBytes(before));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());

    // After the end of the auction
    SetMockTime(before.endTime + DAY);
    BOOST_CHECK_EQUAL(SendMsg(from, "uidaucbid", BidMessage(auctionId, MakeKeyID(), 6000 * COIN)), Outcome::ACCEPTED);
    BOOST_CHECK(Take(other) == (Commands{"uidaucbid"}));
}

// ---------------------------------------------------------------------------
// uidaucfin, ridaucfin
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(uidaucfin_rejections)
{
    const uint256 pending = PendingAuction("alice");
    const uint256 active = ActiveAuction("bobby");
    const uint256 namelessId = NamespaceAuction("fresh", MakeKeyID());

    for (const std::string command : {"uidaucfin", "ridaucfin"}) {
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, FinalizeMessage(InsecureRand256())), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, FinalizeMessage(namelessId)), Outcome::REJECTED);
        // A pending auction, and an active one that has not ended
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, FinalizeMessage(pending)), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, FinalizeMessage(active)), Outcome::REJECTED);
        BOOST_CHECK_EQUAL(SendFromNewPeer(command, uint256::ONE), Outcome::REJECTED);
    }
    // One second before the end
    SetMockTime(GetAuction(aMgr, active).endTime - 1);
    BOOST_CHECK_EQUAL(SendFromNewPeer("uidaucfin", FinalizeMessage(active)), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the finalization of an active auction that has ended is
// relayed and nothing else happens: the auction is not finalized, the winning
// bid, the price and the creator of the message are not looked at, and every
// copy is relayed again.
BOOST_AUTO_TEST_CASE(uidaucfin_is_relayed_without_changing_anything)
{
    const uint256 auctionId = ActiveAuction("alice");
    const AuctionInfo before = GetAuction(aMgr, auctionId);
    SetMockTime(before.endTime);

    for (const std::string command : {"uidaucfin", "ridaucfin"}) {
        const CAuctionFinalize msg = FinalizeMessage(auctionId);
        for (int i = 0; i < 3; ++i) {
            BOOST_CHECK_EQUAL(SendMsg(from, command, msg), Outcome::ACCEPTED);
            // Both commands are relayed as "uidaucfin"
            const std::vector<SentMessage> relayed = SentMessages(other);
            BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
            BOOST_CHECK_EQUAL(relayed[0].command, "uidaucfin");
            BOOST_CHECK(relayed[0].payload == PayloadBytes(msg));
            ClearSent(other);
            BOOST_CHECK(Take(from).empty());
        }
    }
    const AuctionInfo after = GetAuction(aMgr, auctionId);
    BOOST_CHECK(PayloadBytes(after) == PayloadBytes(before));
    BOOST_CHECK(after.state == AUCTION_ACTIVE);
}

// ---------------------------------------------------------------------------
// uidauccan
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(uidauccan_rejections)
{
    const uint256 active = ActiveAuction("bobby");
    const uint256 canceled = PendingAuction("carol");
    BOOST_REQUIRE(aMgr->CancelAuction(canceled, creator));
    const uint256 namelessId = NamespaceAuction("fresh", MakeKeyID());

    BOOST_CHECK_EQUAL(SendFromNewPeer("uidauccan", InsecureRand256()), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("uidauccan", namelessId), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("uidauccan", canceled), Outcome::REJECTED);
    // An active auction with a bid
    BOOST_CHECK_EQUAL(SendFromNewPeer("uidauccan", active), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("uidauccan", uint8_t{1}), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the cancellation of a pending auction is relayed and nothing
// else happens: the auction is not canceled, anyone can send the message, and
// every copy is relayed again.
BOOST_AUTO_TEST_CASE(uidauccan_is_relayed_without_changing_anything)
{
    const uint256 auctionId = PendingAuction("alice");
    const AuctionInfo before = GetAuction(aMgr, auctionId);
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_EQUAL(SendMsg(from, "uidauccan", auctionId), Outcome::ACCEPTED);
        const std::vector<SentMessage> relayed = SentMessages(other);
        BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
        BOOST_CHECK_EQUAL(relayed[0].command, "uidauccan");
        BOOST_CHECK(relayed[0].payload == PayloadBytes(auctionId));
        ClearSent(other);
        BOOST_CHECK(Take(from).empty());
    }
    BOOST_CHECK(PayloadBytes(GetAuction(aMgr, auctionId)) == PayloadBytes(before));
    BOOST_CHECK(GetAuction(aMgr, auctionId).state == AUCTION_PENDING);
}

// ---------------------------------------------------------------------------
// ridprofup
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ridprofup_rejections)
{
    const CKeyID owner = MakeKeyID();
    auto with = [&](const std::function<void(CReddIDProfileUpdate&)>& change) {
        CReddIDProfileUpdate msg = ProfileMessage(TestProfile("alice", owner));
        change(msg);
        return SendFromNewPeer("ridprofup", msg);
    };

    // The ReddID of the message
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = ""; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = "ab"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = std::string(MAX_REDDID_LENGTH + 1, 'a'); }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = "Alice"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = "al-ice"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = "_alice"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = "alice_"; }), Outcome::REJECTED);
    // Display name and bio
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.displayName = std::string(65, 'd'); }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.bio = std::string(257, 'b'); }), Outcome::REJECTED);
    // What the profile manager refuses: the ReddID of the profile, the verification status
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.reddId = "Alice"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.reddId = ""; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.verificationStatus = static_cast<VerificationStatus>(4); }), Outcome::REJECTED);
    // A payload that is not a profile update
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridprofup", uint256::ONE), Outcome::REJECTED);

    BOOST_CHECK(!pMgr->ProfileExists("alice"));
    BOOST_CHECK(NothingSent());

    // The longest display name and bio are accepted
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) {
        m.profile.displayName = std::string(64, 'd');
        m.profile.bio = std::string(256, 'b');
    }), Outcome::ACCEPTED);
}

// SUSPECTED BUG: a profile update creates a profile. There is no signature:
// nothing shows that the sender controls the owner key, and no transaction is
// behind the message. The hash of the message is not looked at.
BOOST_AUTO_TEST_CASE(ridprofup_creates_a_profile_without_a_signature)
{
    const CKeyID victim = MakeKeyID();
    ReddIDProfile profile = TestProfile("alice", victim);
    profile.verificationStatus = VERIFICATION_OFFICIAL;
    CReddIDProfileUpdate msg = ProfileMessage(profile);
    msg.profileHash = InsecureRand256();

    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", msg), Outcome::ACCEPTED);

    BOOST_REQUIRE(pMgr->ProfileExists("alice"));
    BOOST_CHECK(PayloadBytes(GetProfile("alice")) == PayloadBytes(profile));
    BOOST_CHECK(GetProfile("alice").verificationStatus == VERIFICATION_OFFICIAL);
    BOOST_CHECK(PayloadBytes(GetStoredProfile("alice")) == PayloadBytes(profile));
    // A reputation comes with it
    BOOST_CHECK_EQUAL(GetReputation("alice").overallScore, 50.0);

    // Creating a profile is not announced by the profile manager; the handler
    // relays the message as it came to every subscribed peer but the sender
    BOOST_CHECK(Take(from).empty());
    const std::vector<SentMessage> relayed = SentMessages(other);
    BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
    BOOST_CHECK_EQUAL(relayed[0].command, "ridprofup");
    BOOST_CHECK(relayed[0].payload == PayloadBytes(msg));
}

// SUSPECTED BUG: a profile update overwrites the profile when the owner key ID
// of the message equals that of the profile. The key ID is public; there is
// no signature. The verification status is whatever the message says.
BOOST_AUTO_TEST_CASE(ridprofup_overwrites_a_profile_when_the_owner_key_id_matches)
{
    const CKeyID owner = MakeKeyID();
    AddProfile("alice", owner);

    ReddIDProfile forged = TestProfile("alice", owner);
    forged.displayName = "Mallory";
    forged.bio = "forged";
    forged.verificationStatus = VERIFICATION_OFFICIAL;
    forged.active = false;
    forged.expirationTime = 1;
    forged.lastUpdated = T0 + 1;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", ProfileMessage(forged)), Outcome::ACCEPTED);
    BOOST_CHECK(PayloadBytes(GetProfile("alice")) == PayloadBytes(forged));
    BOOST_CHECK(PayloadBytes(GetStoredProfile("alice")) == PayloadBytes(forged));

    // The profile manager announces the update to every subscribed peer, the
    // sender included, and the handler relays the message to all but the sender
    BOOST_CHECK(Take(from) == (Commands{"ridprofup"}));
    BOOST_CHECK(Take(other) == (Commands{"ridprofup", "ridprofup"}));

    // Another owner key ID: refused
    ReddIDProfile stolen = forged;
    stolen.owner = MakeKeyID();
    stolen.lastUpdated = T0 + 2;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", ProfileMessage(stolen)), Outcome::REJECTED);
    BOOST_CHECK(GetProfile("alice").owner == owner);

    // Not newer than what this node has: acknowledged and dropped
    ReddIDProfile stale = TestProfile("alice", owner);
    stale.lastUpdated = T0 + 1;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", ProfileMessage(stale)), Outcome::ACCEPTED);
    BOOST_CHECK(PayloadBytes(GetProfile("alice")) == PayloadBytes(forged));
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the handler validates the ReddID of the message and looks up
// the profile of that name to see whether the update is newer, but stores the
// profile of the message under the ReddID inside the profile. With a made-up
// name in the message an update overwrites a profile that is newer.
BOOST_AUTO_TEST_CASE(ridprofup_validates_one_reddid_and_stores_another)
{
    const CKeyID owner = MakeKeyID();
    ReddIDProfile current = TestProfile("alice", owner);
    current.lastUpdated = T0 + 1000;
    AddProfile(current);

    ReddIDProfile old = TestProfile("alice", owner);
    old.bio = "old";
    old.lastUpdated = T0 - 1000;

    // Under its own name the old profile is dropped
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", ProfileMessage(old)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").bio, "Test bio");

    CReddIDProfileUpdate msg = ProfileMessage(old);
    msg.reddId = "decoy";
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", msg), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").bio, "old");
    BOOST_CHECK_EQUAL(GetProfile("alice").lastUpdated, T0 - 1000);
    BOOST_CHECK(!pMgr->ProfileExists("decoy"));
}

// SUSPECTED BUG: the time of the last update is taken from the message. A
// profile that claims the largest possible time can never be updated by a
// message again.
BOOST_AUTO_TEST_CASE(ridprofup_with_the_largest_timestamp_wins_for_ever)
{
    const CKeyID owner = MakeKeyID();
    AddProfile("alice", owner);

    ReddIDProfile forged = TestProfile("alice", owner);
    forged.bio = "forged";
    forged.lastUpdated = FAR_FUTURE;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", ProfileMessage(forged)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").lastUpdated, FAR_FUTURE);
    ClearSent(from);
    ClearSent(other);

    SetMockTime(T0 + 365 * DAY);
    ReddIDProfile honest = TestProfile("alice", owner);
    honest.bio = "honest";
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", ProfileMessage(honest)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofres", ProfileMessage(honest)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").bio, "forged");
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the social data and the messaging key of a profile have no
// limit of their own. A message may be 1 MB, and a profile of nearly that
// size is stored in memory and in the database and relayed.
BOOST_AUTO_TEST_CASE(ridprofup_stores_fields_of_any_size)
{
    ReddIDProfile large = TestProfile("alice", MakeKeyID());
    large.socialData = std::string(500000, 's');
    large.messagingPubkey = std::vector<unsigned char>(400000, 0x02);
    const CReddIDProfileUpdate msg = ProfileMessage(large);
    BOOST_CHECK(Payload(msg).size() > 900000);
    BOOST_CHECK(Payload(msg).size() <= MAX_REDDID_MESSAGE_SIZE);

    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofup", msg), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").socialData.size(), 500000U);
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").messagingPubkey.size(), 400000U);
    BOOST_CHECK(SentMessages(other)[0].payload.size() > 900000);
}

// ---------------------------------------------------------------------------
// ridprofreq
// ---------------------------------------------------------------------------

namespace {
CReddIDProfileRequest ProfileRequest(const std::string& reddId)
{
    CReddIDProfileRequest msg;
    msg.timestamp = T0;
    msg.reddId = reddId;
    return msg;
}
} // namespace

BOOST_AUTO_TEST_CASE(ridprofreq_rejections)
{
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridprofreq", ProfileRequest("")), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridprofreq", ProfileRequest(std::string(MAX_REDDID_LENGTH + 1, 'a'))), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridprofreq", ProfileRequest("unknown")), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridprofreq", uint8_t{1}), Outcome::REJECTED);
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: the reply to a profile request is sent with RelayMessage and
// the requester as the exception: every subscribed peer gets the profile,
// except the one that asked for it.
BOOST_AUTO_TEST_CASE(ridprofreq_answers_everyone_but_the_requester)
{
    const ReddIDProfile profile = AddProfile("alice");

    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofreq", ProfileRequest("alice")), Outcome::ACCEPTED);
    BOOST_CHECK(SentMessages(from).empty());
    const std::vector<SentMessage> sent = SentMessages(other);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "ridprofres");
    const CReddIDProfileUpdate reply = Decode<CReddIDProfileUpdate>(sent[0]);
    BOOST_CHECK_EQUAL(reply.timestamp, T0);
    BOOST_CHECK_EQUAL(reply.reddId, "alice");
    BOOST_CHECK(PayloadBytes(reply.profile) == PayloadBytes(profile));
    BOOST_CHECK(reply.profileHash == pMgr->CalculateProfileHash(profile));

    // A requester that is not subscribed gets nothing either
    ClearSent(other);
    CNode& stranger = AddPeer(/* subscribe */ false);
    BOOST_CHECK_EQUAL(SendMsg(stranger, "ridprofreq", ProfileRequest("alice")), Outcome::ACCEPTED);
    BOOST_CHECK(SentMessages(stranger).empty());
    BOOST_CHECK(Take(other) == (Commands{"ridprofres"}));
    BOOST_CHECK(Take(from) == (Commands{"ridprofres"}));
}

// ---------------------------------------------------------------------------
// ridprofres
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ridprofres_rejections)
{
    const CKeyID owner = MakeKeyID();
    auto with = [&](const std::function<void(CReddIDProfileUpdate&)>& change, bool rehash = true) {
        CReddIDProfileUpdate msg = ProfileMessage(TestProfile("alice", owner));
        change(msg);
        if (rehash) msg.profileHash = pMgr->CalculateProfileHash(msg.profile);
        return SendFromNewPeer("ridprofres", msg);
    };

    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = ""; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = "ab"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.reddId = std::string(MAX_REDDID_LENGTH + 1, 'a'); }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.displayName = std::string(65, 'd'); }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.bio = std::string(257, 'b'); }), Outcome::REJECTED);
    // The hash of the message is not the hash of its profile
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.bio = "changed"; }, /* rehash */ false), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profileHash.SetNull(); }, /* rehash */ false), Outcome::REJECTED);
    // What the profile manager refuses
    BOOST_CHECK_EQUAL(with([](CReddIDProfileUpdate& m) { m.profile.reddId = "Alice"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridprofres", uint256::ONE), Outcome::REJECTED);

    BOOST_CHECK(!pMgr->ProfileExists("alice"));
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: a profile response that nobody asked for creates a profile,
// or overwrites one when the owner key ID matches. The node does not track
// what it requested, there is no signature, and the "hash verification"
// compares the profile with a hash that the same message carries, never with
// anything recorded on the chain.
BOOST_AUTO_TEST_CASE(ridprofres_unsolicited_creates_and_overwrites_profiles)
{
    const CKeyID victim = MakeKeyID();
    ReddIDProfile profile = TestProfile("alice", victim);
    profile.verificationStatus = VERIFICATION_OFFICIAL;

    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofres", ProfileMessage(profile)), Outcome::ACCEPTED);
    BOOST_REQUIRE(pMgr->ProfileExists("alice"));
    BOOST_CHECK(PayloadBytes(GetProfile("alice")) == PayloadBytes(profile));
    BOOST_CHECK(PayloadBytes(GetStoredProfile("alice")) == PayloadBytes(profile));
    // A response is not relayed, and creating a profile is not announced
    BOOST_CHECK(NothingSent());

    ReddIDProfile forged = profile;
    forged.bio = "forged";
    forged.lastUpdated = T0 + 1;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofres", ProfileMessage(forged)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").bio, "forged");
    // The handler does not relay, but the profile manager announces the
    // update to every subscribed peer, the sender included
    BOOST_CHECK(Take(from) == (Commands{"ridprofup"}));
    BOOST_CHECK(Take(other) == (Commands{"ridprofup"}));

    // Another owner key ID: refused
    ReddIDProfile stolen = forged;
    stolen.owner = MakeKeyID();
    stolen.lastUpdated = T0 + 2;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofres", ProfileMessage(stolen)), Outcome::REJECTED);
    // Not newer: acknowledged and dropped
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofres", ProfileMessage(profile)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetProfile("alice").bio, "forged");
    BOOST_CHECK(NothingSent());
}

// Current behaviour: a response checks only the length of the ReddID of the
// message, not its characters; the profile inside is what gets validated and
// stored.
BOOST_AUTO_TEST_CASE(ridprofres_checks_only_the_length_of_the_reddid_of_the_message)
{
    CReddIDProfileUpdate msg = ProfileMessage(TestProfile("alice", MakeKeyID()));
    msg.reddId = "NOT A NAME";
    BOOST_CHECK_EQUAL(SendMsg(from, "ridprofres", msg), Outcome::ACCEPTED);
    BOOST_CHECK(pMgr->ProfileExists("alice"));
}

// ---------------------------------------------------------------------------
// ridconn
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ridconn_rejections)
{
    AddProfile("alice");
    AddProfile("bobby");
    auto with = [&](const std::function<void(CReddIDConnection&)>& change) {
        CReddIDConnection msg = ConnectionMessage("alice", "bobby", CONNECTION_FOLLOW);
        change(msg);
        return SendFromNewPeer("ridconn", msg);
    };

    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.toReddId = "alice"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.connectionType = static_cast<SocialConnectionType>(4); }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.visibility = -1; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.visibility = 3; }), Outcome::REJECTED);
    // One of the two profiles does not exist
    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.fromReddId = "nobody"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.toReddId = "nobody"; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(with([](CReddIDConnection& m) { m.connection.fromReddId = ""; }), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridconn", uint8_t{1}), Outcome::REJECTED);

    BOOST_CHECK(pMgr->GetConnections("alice").empty());
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: a connection message records a connection between any two
// profiles that exist. There is no signature and no transaction: nothing
// shows that the owner of the source profile made the connection. An
// endorsement counts towards the reputation of the target.
BOOST_AUTO_TEST_CASE(ridconn_records_a_connection_for_any_profile)
{
    AddProfile("alice");
    AddProfile("bobby");
    CReddIDConnection msg = ConnectionMessage("alice", "bobby", CONNECTION_ENDORSE);
    msg.connection.visibility = 2;
    msg.connection.txid = InsecureRand256();

    BOOST_CHECK_EQUAL(SendMsg(from, "ridconn", msg), Outcome::ACCEPTED);

    ReddIDConnection stored;
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bobby", stored));
    BOOST_CHECK(PayloadBytes(stored) == PayloadBytes(msg.connection));
    BOOST_CHECK(db->ReadConnection("alice", "bobby", stored));
    BOOST_CHECK(PayloadBytes(stored) == PayloadBytes(msg.connection));
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("bobby", CONNECTION_ENDORSE), 1);

    BOOST_CHECK(Take(from).empty());
    const std::vector<SentMessage> relayed = SentMessages(other);
    BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
    BOOST_CHECK_EQUAL(relayed[0].command, "ridconn");
    BOOST_CHECK(relayed[0].payload == PayloadBytes(msg));
    ClearSent(other);

    // Not newer than the connection that this node has: acknowledged and dropped
    CReddIDConnection same = msg;
    same.connection.connectionType = CONNECTION_BLOCK;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridconn", same), Outcome::ACCEPTED);
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bobby", stored));
    BOOST_CHECK(stored.connectionType == CONNECTION_ENDORSE);
    BOOST_CHECK(NothingSent());

    // Newer: the connection is replaced, here an endorsement by a block
    CReddIDConnection newer = same;
    newer.connection.lastInteraction = T0 + 1;
    BOOST_CHECK_EQUAL(SendMsg(from, "ridconn", newer), Outcome::ACCEPTED);
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bobby", stored));
    BOOST_CHECK(stored.connectionType == CONNECTION_BLOCK);
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
    BOOST_CHECK(Take(other) == (Commands{"ridconn"}));
}

// SUSPECTED BUG: the time of the last interaction is taken from the message,
// and a connection that claims the largest possible time can never be
// replaced by a message again. The metadata has no limit of its own.
BOOST_AUTO_TEST_CASE(ridconn_with_the_largest_timestamp_wins_for_ever)
{
    AddProfile("alice");
    AddProfile("bobby");
    CReddIDConnection forged = ConnectionMessage("alice", "bobby", CONNECTION_BLOCK);
    forged.connection.lastInteraction = FAR_FUTURE;
    forged.connection.metadata = std::string(900000, 'm');
    BOOST_CHECK_EQUAL(SendMsg(from, "ridconn", forged), Outcome::ACCEPTED);
    ClearSent(from);
    ClearSent(other);

    SetMockTime(T0 + 365 * DAY);
    BOOST_CHECK_EQUAL(SendMsg(from, "ridconn", ConnectionMessage("alice", "bobby", CONNECTION_FRIEND)), Outcome::ACCEPTED);
    ReddIDConnection stored;
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bobby", stored));
    BOOST_CHECK(stored.connectionType == CONNECTION_BLOCK);
    BOOST_CHECK_EQUAL(stored.lastInteraction, FAR_FUTURE);
    BOOST_CHECK_EQUAL(stored.metadata.size(), 900000U);
    BOOST_CHECK(NothingSent());
}

// ---------------------------------------------------------------------------
// ridrep
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ridrep_rejections)
{
    AddProfile("alice");
    const ReddIDReputation before = GetReputation("alice");

    BOOST_CHECK_EQUAL(SendFromNewPeer("ridrep", ReputationMessage("nobody", 50, T0 + 1)), Outcome::REJECTED);
    // Each of the six scores must be between 0 and 100
    for (int field = 0; field < 6; ++field) {
        for (double bad : {-1.0, 100.01}) {
            CReddIDReputationUpdate msg = ReputationMessage("alice", 50, T0 + 1);
            double* scores[] = {&msg.reputation.overallScore, &msg.reputation.longevityScore,
                                &msg.reputation.reliabilityScore, &msg.reputation.engagementScore,
                                &msg.reputation.verificationScore, &msg.reputation.auctionScore};
            *scores[field] = bad;
            BOOST_CHECK_MESSAGE(SendFromNewPeer("ridrep", msg) == Outcome::REJECTED, "score " << field << " = " << bad);
        }
    }
    BOOST_CHECK_EQUAL(SendFromNewPeer("ridrep", uint8_t{1}), Outcome::REJECTED);

    BOOST_CHECK(PayloadBytes(GetReputation("alice")) == PayloadBytes(before));
    BOOST_CHECK(NothingSent());
}

// SUSPECTED BUG: a reputation message replaces the reputation of any profile
// by the scores that the peer chose. Nothing is calculated or checked: the
// proof and the signatures of the message are not looked at.
BOOST_AUTO_TEST_CASE(ridrep_replaces_the_reputation_of_any_profile)
{
    AddProfile("alice");
    BOOST_CHECK_EQUAL(GetReputation("alice").overallScore, 50.0);
    BOOST_CHECK_EQUAL(GetReputation("alice").lastCalculated, T0);

    CReddIDReputationUpdate msg = ReputationMessage("alice", 100, T0 + 1);
    msg.reputation.calculatorSignatures = "not a signature";
    BOOST_CHECK_EQUAL(SendMsg(from, "ridrep", msg), Outcome::ACCEPTED);

    BOOST_CHECK(PayloadBytes(GetReputation("alice")) == PayloadBytes(msg.reputation));
    BOOST_CHECK_EQUAL(GetReputation("alice").overallScore, 100.0);
    ReddIDReputation stored;
    BOOST_REQUIRE(db->ReadReputation("alice", stored));
    BOOST_CHECK_EQUAL(stored.overallScore, 100.0);

    BOOST_CHECK(Take(from).empty());
    const std::vector<SentMessage> relayed = SentMessages(other);
    BOOST_REQUIRE_EQUAL(relayed.size(), 1U);
    BOOST_CHECK_EQUAL(relayed[0].command, "ridrep");
    BOOST_CHECK(relayed[0].payload == PayloadBytes(msg));
    ClearSent(other);

    // Not newer: acknowledged and dropped
    BOOST_CHECK_EQUAL(SendMsg(from, "ridrep", ReputationMessage("alice", 0, T0 + 1)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetReputation("alice").overallScore, 100.0);
    BOOST_CHECK(NothingSent());
    // Newer, and as bad as it gets
    BOOST_CHECK_EQUAL(SendMsg(from, "ridrep", ReputationMessage("alice", 0, T0 + 2)), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetReputation("alice").overallScore, 0.0);
}

// SUSPECTED BUG: the time of the calculation is taken from the message, and a
// reputation that claims the largest possible time can never be replaced by a
// message again. The signatures field has no limit of its own.
BOOST_AUTO_TEST_CASE(ridrep_with_the_largest_timestamp_wins_for_ever)
{
    AddProfile("alice");
    CReddIDReputationUpdate forged = ReputationMessage("alice", 0, FAR_FUTURE);
    forged.reputation.calculatorSignatures = std::string(900000, 'c');
    BOOST_CHECK_EQUAL(SendMsg(from, "ridrep", forged), Outcome::ACCEPTED);
    ClearSent(from);
    ClearSent(other);

    SetMockTime(T0 + 365 * DAY);
    BOOST_CHECK_EQUAL(SendMsg(from, "ridrep", ReputationMessage("alice", 80, GetTime())), Outcome::ACCEPTED);
    BOOST_CHECK_EQUAL(GetReputation("alice").overallScore, 0.0);
    BOOST_CHECK_EQUAL(GetReputation("alice").lastCalculated, FAR_FUTURE);
    BOOST_CHECK_EQUAL(GetReputation("alice").calculatorSignatures.size(), 900000U);
    BOOST_CHECK(NothingSent());
}

BOOST_AUTO_TEST_SUITE_END()
