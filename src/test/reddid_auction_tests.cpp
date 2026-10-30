// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid_db.h>
#include <test/reddid_auction_test_util.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <key.h>

#include <limits>
#include <string>
#include <vector>

using namespace reddid_test;

// Helper: create a user ID auction through AuctionManager with proper setup
static bool CreateUserIDAuction(AuctionManager* aMgr, const std::string& name,
                                const std::string& nsId, const CKeyID& creator,
                                CAmount reserve, int durationDays, uint256& auctionId)
{
    AuctionInfo auction;
    auction.name = name;
    auction.namespaceId = nsId;
    auction.creator = creator;
    auction.reservePrice = reserve;
    auction.startTime = GetTime();
    auction.endTime = auction.startTime + durationDays * 86400;
    auction.type = AUCTION_STANDARD;
    return aMgr->CreateAuction(auction, auctionId);
}

// Helper: create a namespace auction through AuctionManager
static bool CreateNSAuction(AuctionManager* aMgr, const std::string& nsId,
                            const CKeyID& creator, CAmount reserve,
                            int durationDays, uint256& auctionId)
{
    AuctionInfo auction;
    auction.name = "";  // empty name = namespace auction
    auction.namespaceId = nsId;
    auction.creator = creator;
    auction.reservePrice = reserve;
    auction.startTime = GetTime();
    auction.endTime = auction.startTime + durationDays * 86400;
    auction.type = AUCTION_STANDARD;
    return aMgr->CreateAuction(auction, auctionId);
}

BOOST_FIXTURE_TEST_SUITE(reddid_auction_tests, ReddIDTestingSetup)

// ===================================================================
// Combined test: State machine transitions + Bid validation
// Tests: pending state, bid activation, finalize, cancel, no-bid finalize,
//        bid on canceled, reserve price, increment, self-bid, bidder tracking
// ===================================================================

BOOST_AUTO_TEST_CASE(state_machine_and_bid_validation)
{
    AuctionManager* aMgr = reddidManager->GetAuctionManager();

    // --- State: auction created in PENDING state ---
    {
        BOOST_TEST_MESSAGE("auction_created_in_pending_state");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("aucns1", creator);

        uint256 auctionId;
        BOOST_CHECK(CreateUserIDAuction(aMgr, "alice", "aucns1", creator, 5000 * COIN, 7, auctionId));

        AuctionInfo info;
        BOOST_CHECK(aMgr->GetAuctionInfo(auctionId, info));
        BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    }

    // --- State: first bid activates auction ---
    {
        BOOST_TEST_MESSAGE("first_bid_activates_auction");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("aucns2", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "bob", "aucns2", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        BOOST_CHECK(aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId));

        AuctionInfo info;
        BOOST_CHECK(aMgr->GetAuctionInfo(auctionId, info));
        BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
        BOOST_CHECK(aMgr->IsAuctionActive(auctionId));
    }

    // --- State: finalize sets FINALIZED state ---
    {
        BOOST_TEST_MESSAGE("finalize_sets_finalized_state");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("aucns3", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "charlie", "aucns3", creator, 5000 * COIN, 3, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId);

        SetMockTime(GetTime() + 3 * 86400 + 60);
        BOOST_CHECK(aMgr->FinalizeAuction(auctionId));

        AuctionInfo info;
        BOOST_CHECK(aMgr->GetAuctionInfo(auctionId, info));
        BOOST_CHECK_EQUAL(info.state, AUCTION_FINALIZED);
        BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));
        SetMockTime(0);
    }

    // --- State: cancel sets CANCELED state ---
    {
        BOOST_TEST_MESSAGE("cancel_sets_canceled_state");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("aucns4", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "dave", "aucns4", creator, 5000 * COIN, 7, auctionId);

        BOOST_CHECK(aMgr->CancelAuction(auctionId, creator));

        AuctionInfo info;
        BOOST_CHECK(aMgr->GetAuctionInfo(auctionId, info));
        BOOST_CHECK_EQUAL(info.state, AUCTION_CANCELED);
        BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));
    }

    // --- State: finalize with no bids ---
    {
        BOOST_TEST_MESSAGE("finalize_no_bids_cancels");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("aucns5", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "eve", "aucns5", creator, 5000 * COIN, 3, auctionId);

        SetMockTime(GetTime() + 3 * 86400 + 60);
        bool result = aMgr->FinalizeAuction(auctionId);
        // Without bids the auction was never activated, so FinalizeAuction fails
        BOOST_CHECK(!result || !aMgr->IsAuctionActive(auctionId));
        SetMockTime(0);
    }

    // --- State: bid on canceled auction fails ---
    {
        BOOST_TEST_MESSAGE("bid_on_canceled_auction_fails");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("aucns6", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "frank", "aucns6", creator, 5000 * COIN, 7, auctionId);
        aMgr->CancelAuction(auctionId, creator);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        BOOST_CHECK(!aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId));
    }

    // --- Bid: below reserve fails ---
    {
        BOOST_TEST_MESSAGE("bid_below_reserve_fails");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns1", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "low", "bidns1", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        BOOST_CHECK(!aMgr->PlaceBid(auctionId, bidder, 4999 * COIN, bidId));
    }

    // --- Bid: meets reserve succeeds ---
    {
        BOOST_TEST_MESSAGE("bid_meets_reserve_succeeds");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns2", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "exact", "bidns2", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        BOOST_CHECK(aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId));
        BOOST_CHECK(!bidId.IsNull());
    }

    // --- Bid: increment too small fails ---
    {
        BOOST_TEST_MESSAGE("bid_increment_too_small_fails");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns3", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "incr", "bidns3", creator, 10000 * COIN, 7, auctionId);

        CKeyID bidder1 = MakeKeyID();
        uint256 bidId1;
        aMgr->PlaceBid(auctionId, bidder1, 10000 * COIN, bidId1);

        CKeyID bidder2 = MakeKeyID();
        uint256 bidId2;
        BOOST_CHECK(!aMgr->PlaceBid(auctionId, bidder2, 10400 * COIN, bidId2));
    }

    // --- Bid: sufficient increment succeeds ---
    {
        BOOST_TEST_MESSAGE("bid_increment_sufficient_succeeds");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns4", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "good", "bidns4", creator, 10000 * COIN, 7, auctionId);

        CKeyID bidder1 = MakeKeyID();
        uint256 bidId1;
        aMgr->PlaceBid(auctionId, bidder1, 10000 * COIN, bidId1);

        CKeyID bidder2 = MakeKeyID();
        uint256 bidId2;
        BOOST_CHECK(aMgr->PlaceBid(auctionId, bidder2, 10500 * COIN, bidId2));
    }

    // --- Bid: self-bid fails ---
    {
        BOOST_TEST_MESSAGE("self_bid_fails");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns5", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "self", "bidns5", creator, 5000 * COIN, 7, auctionId);

        uint256 bidId;
        BOOST_CHECK(!aMgr->PlaceBid(auctionId, creator, 5000 * COIN, bidId));
    }

    // --- Bid: updates current bidder ---
    {
        BOOST_TEST_MESSAGE("bid_updates_current_bidder");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns6", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "track", "bidns6", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder1 = MakeKeyID();
        uint256 bidId1;
        aMgr->PlaceBid(auctionId, bidder1, 5000 * COIN, bidId1);

        AuctionInfo info;
        aMgr->GetAuctionInfo(auctionId, info);
        BOOST_CHECK(info.currentBidder == bidder1);
        BOOST_CHECK_EQUAL(info.currentBid, 5000 * COIN);

        CKeyID bidder2 = MakeKeyID();
        uint256 bidId2;
        aMgr->PlaceBid(auctionId, bidder2, 5500 * COIN, bidId2);

        aMgr->GetAuctionInfo(auctionId, info);
        BOOST_CHECK(info.currentBidder == bidder2);
        BOOST_CHECK_EQUAL(info.currentBid, 5500 * COIN);
    }

    // --- Bid: multiple bidders, highest wins ---
    {
        BOOST_TEST_MESSAGE("multiple_bidders_highest_wins");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("bidns7", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "multi", "bidns7", creator, 5000 * COIN, 3, auctionId);

        CKeyID bidder1 = MakeKeyID();
        CKeyID bidder2 = MakeKeyID();
        CKeyID bidder3 = MakeKeyID();
        uint256 bid1, bid2, bid3;

        aMgr->PlaceBid(auctionId, bidder1, 5000 * COIN, bid1);
        aMgr->PlaceBid(auctionId, bidder2, 5500 * COIN, bid2);
        aMgr->PlaceBid(auctionId, bidder3, 6000 * COIN, bid3);

        BidInfo highest = aMgr->GetHighestBid(auctionId);
        BOOST_CHECK(highest.bidder == bidder3);
        BOOST_CHECK_EQUAL(highest.bidAmount, 6000 * COIN);
        BOOST_CHECK(aMgr->IsHighestBidder(auctionId, bidder3));
    }
}

// ===================================================================
// Combined test: Anti-sniping + ID generation
// Tests: late bid extension, early bid no extension, namespace 12h extension,
//        auction ID determinism, bid ID determinism
// ===================================================================

BOOST_AUTO_TEST_CASE(antisniping_idgen_finalization_queries_cancel)
{
    AuctionManager* aMgr = reddidManager->GetAuctionManager();
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();

    // --- Anti-sniping: late bid extends user auction by 6 hours ---
    {
        BOOST_TEST_MESSAGE("late_bid_extends_user_auction");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("snipe1", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "snipe", "snipe1", creator, 5000 * COIN, 3, auctionId);

        AuctionInfo infoBefore;
        aMgr->GetAuctionInfo(auctionId, infoBefore);
        int64_t originalEnd = infoBefore.endTime;

        CKeyID bidder1 = MakeKeyID();
        uint256 bidId1;
        aMgr->PlaceBid(auctionId, bidder1, 5000 * COIN, bidId1);

        int64_t lateBidTime = originalEnd - 3600;  // 1 hour before end
        CKeyID bidder2 = MakeKeyID();
        uint256 bidId2;
        aMgr->PlaceBid(auctionId, bidder2, 5500 * COIN, bidId2, lateBidTime);

        AuctionInfo infoAfter;
        aMgr->GetAuctionInfo(auctionId, infoAfter);
        BOOST_CHECK(infoAfter.endTime > originalEnd);
        BOOST_CHECK_EQUAL(infoAfter.endTime, lateBidTime + 6 * 3600);
    }

    // --- Anti-sniping: early bid does not extend ---
    {
        BOOST_TEST_MESSAGE("early_bid_does_not_extend");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("snipe2", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "early", "snipe2", creator, 5000 * COIN, 7, auctionId);

        AuctionInfo infoBefore;
        aMgr->GetAuctionInfo(auctionId, infoBefore);
        int64_t originalEnd = infoBefore.endTime;

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        int64_t earlyBidTime = infoBefore.startTime + 86400;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId, earlyBidTime);

        AuctionInfo infoAfter;
        aMgr->GetAuctionInfo(auctionId, infoAfter);
        BOOST_CHECK_EQUAL(infoAfter.endTime, originalEnd);
    }

    // --- Anti-sniping: namespace auction extends by 12 hours ---
    {
        BOOST_TEST_MESSAGE("namespace_auction_extends_12_hours");
        CKeyID creator = MakeKeyID();

        CAmount reserve = nsMgr->CalculateMinPrice("snipens");
        uint256 auctionId;
        // NS auctions go through NamespaceManager (not AuctionManager)
        BOOST_CHECK(nsMgr->CreateNamespaceAuction("snipens", creator, reserve, 3, AUCTION_STANDARD, auctionId));

        AuctionInfo infoBefore;
        nsMgr->GetAuctionInfo(auctionId, infoBefore);
        int64_t originalEnd = infoBefore.endTime;

        // First bid activates the auction (placed early, no extension)
        CKeyID bidder1 = MakeKeyID();
        uint256 bidId1;
        nsMgr->BidOnNamespaceAuction(auctionId, bidder1, reserve, bidId1);

        // Set mock time to within 24h of end, then place second bid
        int64_t lateBidTime = originalEnd - 3600;
        SetMockTime(lateBidTime);
        CKeyID bidder2 = MakeKeyID();
        uint256 bidId2;
        CAmount nextBid = reserve + (reserve * 5 / 100) + COIN;
        nsMgr->BidOnNamespaceAuction(auctionId, bidder2, nextBid, bidId2);

        AuctionInfo infoAfter;
        nsMgr->GetAuctionInfo(auctionId, infoAfter);
        BOOST_CHECK(infoAfter.endTime > originalEnd);
        BOOST_CHECK_EQUAL(infoAfter.endTime, lateBidTime + 12 * 3600);
        SetMockTime(0);
    }

    // --- ID generation: auction IDs differ by time ---
    {
        BOOST_TEST_MESSAGE("auction_id_is_deterministic");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("detns1", creator);

        int64_t now = GetTime();
        SetMockTime(now);

        uint256 auctionId1;
        CreateUserIDAuction(aMgr, "det", "detns1", creator, 5000 * COIN, 7, auctionId1);

        SetMockTime(now + 1);
        uint256 auctionId2;
        CreateUserIDAuction(aMgr, "det2", "detns1", creator, 5000 * COIN, 7, auctionId2);

        BOOST_CHECK(auctionId1 != auctionId2);
        BOOST_CHECK(!auctionId1.IsNull());
        BOOST_CHECK(!auctionId2.IsNull());
        SetMockTime(0);
    }

    // --- ID generation: bid ID is non-null and correct ---
    {
        BOOST_TEST_MESSAGE("bid_id_is_deterministic");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("detns2", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "biddet", "detns2", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId);

        BOOST_CHECK(!bidId.IsNull());

        BidInfo bidInfo;
        BOOST_CHECK(aMgr->GetBidInfo(bidId, bidInfo));
        BOOST_CHECK(bidInfo.bidder == bidder);
        BOOST_CHECK_EQUAL(bidInfo.bidAmount, 5000 * COIN);
    }

    // --- Finalize: creates user ID with winner as owner ---
    {
        BOOST_TEST_MESSAGE("finalize_creates_userid");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("finns1", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "winner", "finns1", creator, 5000 * COIN, 3, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId);

        SetMockTime(GetTime() + 3 * 86400 + 60);
        BOOST_CHECK(aMgr->FinalizeAuction(auctionId));

        UserIDInfo userInfo;
        BOOST_CHECK(db->ReadUserID("winner", "finns1", userInfo));
        BOOST_CHECK(userInfo.owner == bidder);
        BOOST_CHECK_EQUAL(userInfo.name, "winner");
        BOOST_CHECK_EQUAL(userInfo.namespaceId, "finns1");
        SetMockTime(0);
    }

    // --- Finalize: expiration matches namespace renewalPeriod ---
    {
        BOOST_TEST_MESSAGE("finalize_expiration_matches_namespace");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("finns2", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "expiry", "finns2", creator, 5000 * COIN, 3, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId);

        int64_t finalizeTime = GetTime() + 3 * 86400 + 60;
        SetMockTime(finalizeTime);
        aMgr->FinalizeAuction(auctionId);

        UserIDInfo userInfo;
        db->ReadUserID("expiry", "finns2", userInfo);
        int64_t expectedExpiry = finalizeTime + 365 * 86400;
        BOOST_CHECK(std::abs(userInfo.expirationTime - expectedExpiry) < 10);
        SetMockTime(0);
    }

    // --- Finalize: losing bids marked refunded ---
    {
        BOOST_TEST_MESSAGE("losing_bids_marked_refunded");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("finns3", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "refund", "finns3", creator, 5000 * COIN, 3, auctionId);

        CKeyID bidder1 = MakeKeyID();
        CKeyID bidder2 = MakeKeyID();
        uint256 bid1, bid2;
        aMgr->PlaceBid(auctionId, bidder1, 5000 * COIN, bid1);
        aMgr->PlaceBid(auctionId, bidder2, 5500 * COIN, bid2);

        SetMockTime(GetTime() + 3 * 86400 + 60);
        aMgr->FinalizeAuction(auctionId);

        BidInfo winInfo;
        aMgr->GetBidInfo(bid2, winInfo);
        BOOST_CHECK(winInfo.isWinner);

        BidInfo loseInfo;
        aMgr->GetBidInfo(bid1, loseInfo);
        BOOST_CHECK(loseInfo.refunded);
        SetMockTime(0);
    }

    // --- Finalize: same bidder raise refunds old bid ---
    {
        BOOST_TEST_MESSAGE("same_bidder_raise_refunds_old");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("finns4", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "raise", "finns4", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bid1, bid2;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bid1);
        aMgr->PlaceBid(auctionId, bidder, 6000 * COIN, bid2);

        BidInfo oldBid;
        aMgr->GetBidInfo(bid1, oldBid);
        BOOST_CHECK(oldBid.refunded);

        AuctionInfo info;
        aMgr->GetAuctionInfo(auctionId, info);
        BOOST_CHECK_EQUAL(info.currentBid, 6000 * COIN);
    }

    // --- Finalize: ProcessExpiredAuctions ---
    {
        BOOST_TEST_MESSAGE("process_expired_auctions");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("finns5", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "expire", "finns5", creator, 5000 * COIN, 3, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId);

        int64_t future = GetTime() + 3 * 86400 + 60;
        SetMockTime(future);
        BOOST_CHECK(aMgr->ProcessExpiredAuctions(future));

        AuctionInfo info;
        aMgr->GetAuctionInfo(auctionId, info);
        BOOST_CHECK_EQUAL(info.state, AUCTION_FINALIZED);
        SetMockTime(0);
    }

    // --- Query: GetActiveAuctions filters correctly ---
    {
        BOOST_TEST_MESSAGE("get_active_auctions_filters");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("qryns1", creator);

        uint256 id1, id2;
        CreateUserIDAuction(aMgr, "active1", "qryns1", creator, 5000 * COIN, 7, id1);
        CreateUserIDAuction(aMgr, "active2", "qryns1", creator, 5000 * COIN, 7, id2);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(id1, bidder, 5000 * COIN, bidId);

        std::vector<AuctionInfo> active = aMgr->GetActiveAuctions();
        bool foundActive = false;
        for (const auto& a : active) {
            if (a.auctionId == id1) foundActive = true;
        }
        BOOST_CHECK(foundActive);
    }

    // --- Query: GetAuctionsByNamespace ---
    {
        BOOST_TEST_MESSAGE("get_auctions_by_namespace");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("qryns2", creator);
        CreateTestNamespace("qryns3", creator);

        uint256 id1, id2, id3;
        BOOST_CHECK(CreateUserIDAuction(aMgr, "ns2a", "qryns2", creator, 5000 * COIN, 7, id1));
        BOOST_CHECK(CreateUserIDAuction(aMgr, "ns2b", "qryns2", creator, 5000 * COIN, 7, id2));
        BOOST_CHECK(CreateUserIDAuction(aMgr, "ns3a", "qryns3", creator, 5000 * COIN, 7, id3));

        // GetAuctionsByNamespace only returns ACTIVE auctions, so place bids to activate
        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(id1, bidder, 5000 * COIN, bidId);
        aMgr->PlaceBid(id2, bidder, 5000 * COIN, bidId);
        aMgr->PlaceBid(id3, bidder, 5000 * COIN, bidId);

        std::vector<AuctionInfo> ns2Auctions = aMgr->GetAuctionsByNamespace("qryns2");
        BOOST_CHECK_EQUAL(ns2Auctions.size(), 2u);
    }

    // --- Query: GetHighestBid ---
    {
        BOOST_TEST_MESSAGE("get_highest_bid_returns_correct");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("qryns4", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "highest", "qryns4", creator, 5000 * COIN, 7, auctionId);

        CKeyID b1 = MakeKeyID(), b2 = MakeKeyID(), b3 = MakeKeyID();
        uint256 bid1, bid2, bid3;
        aMgr->PlaceBid(auctionId, b1, 5000 * COIN, bid1);
        aMgr->PlaceBid(auctionId, b2, 5500 * COIN, bid2);
        aMgr->PlaceBid(auctionId, b3, 6000 * COIN, bid3);

        BidInfo highest = aMgr->GetHighestBid(auctionId);
        BOOST_CHECK_EQUAL(highest.bidAmount, 6000 * COIN);
        BOOST_CHECK(highest.bidder == b3);
    }

    // --- Query: GetAuctionBids returns all ---
    {
        BOOST_TEST_MESSAGE("get_auction_bids_returns_all");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("qryns5", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "allbids", "qryns5", creator, 5000 * COIN, 7, auctionId);

        CKeyID b1 = MakeKeyID(), b2 = MakeKeyID();
        uint256 bid1, bid2;
        aMgr->PlaceBid(auctionId, b1, 5000 * COIN, bid1);
        aMgr->PlaceBid(auctionId, b2, 5500 * COIN, bid2);

        std::vector<BidInfo> bids = aMgr->GetAuctionBids(auctionId);
        BOOST_CHECK_EQUAL(bids.size(), 2u);
    }

    // --- Cancel: non-creator fails ---
    {
        BOOST_TEST_MESSAGE("cancel_by_non_creator_fails");
        CKeyID creator = MakeKeyID();
        CKeyID imposter = MakeKeyID();
        CreateTestNamespace("canns1", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "nocan", "canns1", creator, 5000 * COIN, 7, auctionId);

        BOOST_CHECK(!aMgr->CancelAuction(auctionId, imposter));
    }

    // --- Cancel: with active bids fails ---
    {
        BOOST_TEST_MESSAGE("cancel_with_bids_active_fails");
        CKeyID creator = MakeKeyID();
        CreateTestNamespace("canns2", creator);

        uint256 auctionId;
        CreateUserIDAuction(aMgr, "hasbid", "canns2", creator, 5000 * COIN, 7, auctionId);

        CKeyID bidder = MakeKeyID();
        uint256 bidId;
        aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, bidId);

        BOOST_CHECK(!aMgr->CancelAuction(auctionId, creator));
    }
}

// ===================================================================
// Auction parameters (ValidateAuctionParameters, through CreateAuction)
// ===================================================================

BOOST_FIXTURE_TEST_CASE(create_auction_rejects_invalid_parameters, AuctionTestingSetup)
{
    uint256 auctionId;
    auto create = [&](AuctionInfo auction) { return aMgr->CreateAuction(auction, auctionId); };
    const AuctionInfo valid = UserAuction("alice", NS, creator, 5000 * COIN, T0);

    AuctionInfo auction = valid;
    auction.namespaceId = "";
    BOOST_CHECK(!create(auction));

    auction = valid;
    auction.namespaceId = "nosuchns";
    BOOST_CHECK(!create(auction));

    auction = valid;
    auction.creator = CKeyID();
    BOOST_CHECK(!create(auction));

    // Start and end times
    auction = valid;
    auction.startTime = 0;
    BOOST_CHECK(!create(auction));
    auction.startTime = -1;
    BOOST_CHECK(!create(auction));
    auction = valid;
    auction.endTime = auction.startTime;
    BOOST_CHECK(!create(auction));
    auction.endTime = auction.startTime - 1;
    BOOST_CHECK(!create(auction));

    // Reserve price
    auction = valid;
    auction.reservePrice = -1;
    BOOST_CHECK(!create(auction));
    auction.reservePrice = 5000 * COIN - 1;
    BOOST_CHECK(!create(auction));

    // Nothing was created so far
    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK(auctionIds.empty());

    BOOST_CHECK(create(valid));
}

// Current behaviour: the duration is compared in whole days, rounded down, so
// an auction may run for up to one second less than a day beyond the maximum
// of its namespace.
BOOST_FIXTURE_TEST_CASE(create_auction_duration_is_counted_in_whole_days, AuctionTestingSetup)
{
    // The namespace allows 3 to 7 days
    uint256 auctionId;
    auto create = [&](const std::string& name, int64_t duration) {
        AuctionInfo auction = UserAuction(name, NS, creator, 5000 * COIN, T0, duration);
        return aMgr->CreateAuction(auction, auctionId);
    };

    BOOST_CHECK(!create("short", 1));
    BOOST_CHECK(!create("short", 3 * DAY - 1));
    BOOST_CHECK(create("minimum", 3 * DAY));
    BOOST_CHECK(create("maximum", 7 * DAY));
    BOOST_CHECK(create("almosteight", 8 * DAY - 1));
    BOOST_CHECK(!create("long", 8 * DAY));
}

// Current behaviour: the checks on the name are those of the namespace
// (length, digits, hyphens, underscores). A name may begin or end with an
// underscore here, which ReddIDManager::ValidateReddID does not allow for a
// ReddID.
BOOST_FIXTURE_TEST_CASE(create_auction_name_rules, AuctionTestingSetup)
{
    uint256 auctionId;
    auto create = [&](const std::string& name, const std::string& ns) {
        AuctionInfo auction = UserAuction(name, ns, creator, 5000 * COIN, T0);
        return aMgr->CreateAuction(auction, auctionId);
    };

    // The namespace of the fixture takes 3 to 32 characters of a-z, 0-9, '-' and '_'
    BOOST_CHECK(!create("ab", NS));
    BOOST_CHECK(create("abc", NS));
    BOOST_CHECK(create(std::string(32, 'a'), NS));
    BOOST_CHECK(!create(std::string(33, 'a'), NS));

    BOOST_CHECK(!create("Alice", NS));
    BOOST_CHECK(!create("al ce", NS));
    BOOST_CHECK(!create("al.ce", NS));
    BOOST_CHECK(create("a1ice", NS));
    BOOST_CHECK(create("123", NS));

    BOOST_CHECK(create("a-b", NS));
    BOOST_CHECK(!create("-ab", NS));
    BOOST_CHECK(!create("ab-", NS));
    BOOST_CHECK(!create("a--b", NS));

    BOOST_CHECK(create("a_b", NS));
    BOOST_CHECK(create("_ab", NS));
    BOOST_CHECK(create("ab_", NS));
    BOOST_CHECK(create("a__b", NS));
    BOOST_CHECK(!reddidManager->ValidateReddID("_ab"));

    // A namespace that takes letters only
    NamespaceInfo lettersOnly;
    lettersOnly.id = "letters";
    lettersOnly.owner = creator;
    lettersOnly.allowNumbers = false;
    lettersOnly.allowHyphens = false;
    lettersOnly.allowUnderscores = false;
    lettersOnly.minLength = 4;
    lettersOnly.maxLength = 6;
    lettersOnly.renewalPeriod = 365;
    lettersOnly.gracePeriod = 30;
    lettersOnly.namespaceRevenuePct = 10;
    lettersOnly.burnPct = 70;
    lettersOnly.nodePct = 15;
    lettersOnly.devPct = 5;
    lettersOnly.minAuctionDuration = 3;
    lettersOnly.maxAuctionDuration = 7;
    lettersOnly.minBidIncrement = 5.0;
    BOOST_REQUIRE(reddidManager->GetNamespaceManager()->CreateNamespace(lettersOnly));

    BOOST_CHECK(!create("abc", "letters"));
    BOOST_CHECK(create("abcd", "letters"));
    BOOST_CHECK(create("abcdef", "letters"));
    BOOST_CHECK(!create("abcdefg", "letters"));
    BOOST_CHECK(!create("abc1", "letters"));
    BOOST_CHECK(!create("ab-c", "letters"));
    BOOST_CHECK(!create("ab_c", "letters"));
}

BOOST_FIXTURE_TEST_CASE(create_auction_for_registered_name, AuctionTestingSetup)
{
    UserIDInfo registered;
    registered.name = "taken";
    registered.namespaceId = NS;
    registered.owner = MakeKeyID();
    registered.registrationTime = T0 - 365 * DAY;
    registered.expirationTime = T0 + 1;
    BOOST_REQUIRE(db->WriteUserID("taken", NS, registered));

    uint256 auctionId;
    AuctionInfo auction = UserAuction("taken", NS, creator, 5000 * COIN, T0);
    BOOST_CHECK(!aMgr->CreateAuction(auction, auctionId));

    // The same name in another namespace is free
    CreateTestNamespace("otherns", creator);
    AuctionInfo elsewhere = UserAuction("taken", "otherns", creator, 5000 * COIN, T0);
    BOOST_CHECK(aMgr->CreateAuction(elsewhere, auctionId));

    // Once the registration has expired the name can be auctioned again
    SetMockTime(T0 + 1);
    BOOST_CHECK(aMgr->CreateAuction(auction, auctionId));
}

BOOST_FIXTURE_TEST_CASE(create_auction_reserve_must_meet_pricing_tier, AuctionTestingSetup)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, 9000 * COIN));
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 6, 2000 * COIN));

    uint256 auctionId;
    auto create = [&](const std::string& name, CAmount reserve) {
        AuctionInfo auction = UserAuction(name, NS, creator, reserve, T0);
        return aMgr->CreateAuction(auction, auctionId);
    };

    BOOST_CHECK(!create("short", 9000 * COIN - 1));
    BOOST_CHECK(create("short", 9000 * COIN));
    BOOST_CHECK(!create("longer", 2000 * COIN - 1));
    BOOST_CHECK(create("longer", 2000 * COIN));
}

BOOST_FIXTURE_TEST_CASE(create_auction_sets_derived_fields, AuctionTestingSetup)
{
    AuctionInfo auction = UserAuction("alice", NS, creator, 5000 * COIN + 5, T0);
    // Whatever the caller puts into these fields is replaced
    auction.auctionId = uint256S("dead");
    auction.state = AUCTION_FINALIZED;
    auction.depositAmount = 1;
    auction.blockHeight = 999;

    uint256 auctionId;
    {
        FakeChainHeight tip(chainstate, 42);
        BOOST_REQUIRE(aMgr->CreateAuction(auction, auctionId));
    }

    BOOST_CHECK(auctionId == ExpectedAuctionId("alice", NS, T0));
    // The caller's copy is updated
    BOOST_CHECK(auction.auctionId == auctionId);
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK(info.auctionId == auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    // 10% of the reserve price, rounded down
    BOOST_CHECK_EQUAL(info.depositAmount, 500 * COIN);
    BOOST_CHECK_EQUAL(info.blockHeight, 42);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK(info.currentBidder.IsNull());
    BOOST_CHECK_EQUAL(info.reAuctionCount, 0);
    BOOST_CHECK(info.previousAuctionId.IsNull());

    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(auctionId, stored));
    BOOST_CHECK(stored.auctionId == auctionId);
    BOOST_CHECK_EQUAL(stored.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(stored.blockHeight, 42);

    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK(!aMgr->IsAuctionEnded(auctionId));
    // Current behaviour: GetAuctionsByNamespace omits pending auctions
    BOOST_CHECK(aMgr->GetAuctionsByNamespace(NS).empty());
}

// SUSPECTED BUG (documented, not fixed): the auction ID is the hash of the
// name, the namespace and the start time, and an auction is a duplicate only
// when the ID exists already. Any number of auctions for one name can
// therefore run side by side as long as their start times differ.
BOOST_FIXTURE_TEST_CASE(create_auction_allows_concurrent_auctions_for_one_name, AuctionTestingSetup)
{
    uint256 first, second, third;
    AuctionInfo auction = UserAuction("alice", NS, creator, 5000 * COIN, T0);
    BOOST_CHECK(aMgr->CreateAuction(auction, first));
    // Same name, namespace and start time
    BOOST_CHECK(!aMgr->CreateAuction(auction, second));
    AuctionInfo sameIdOtherTerms = UserAuction("alice", NS, MakeKeyID(), 9000 * COIN, T0, 3 * DAY);
    BOOST_CHECK(!aMgr->CreateAuction(sameIdOtherTerms, second));

    AuctionInfo later = UserAuction("alice", NS, creator, 5000 * COIN, T0 + 1);
    BOOST_CHECK(aMgr->CreateAuction(later, second));
    BOOST_CHECK(first != second);

    // Both take bids, and both can be won
    CKeyID bidderA = MakeKeyID();
    CKeyID bidderB = MakeKeyID();
    uint256 bidId;
    BOOST_CHECK(aMgr->PlaceBid(first, bidderA, 5000 * COIN, bidId));
    BOOST_CHECK(aMgr->PlaceBid(second, bidderB, 5000 * COIN, bidId));
    SetMockTime(T0 + 7 * DAY + 1);
    BOOST_CHECK(aMgr->FinalizeAuction(first));
    BOOST_CHECK(aMgr->FinalizeAuction(second));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, first).state, AUCTION_FINALIZED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, second).state, AUCTION_FINALIZED);

    // The registration of the first winner was overwritten by the second
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == bidderB);

    // Current behaviour: the start time is not compared with the clock either
    AuctionInfo longAgo = UserAuction("bob", NS, creator, 5000 * COIN, 1, 3 * DAY);
    BOOST_CHECK(aMgr->CreateAuction(longAgo, third));
    AuctionInfo farAhead = UserAuction("carol", NS, creator, 5000 * COIN, T0 + 1000 * DAY, 3 * DAY);
    BOOST_CHECK(aMgr->CreateAuction(farAhead, third));
    // An auction that has not started yet takes bids
    BOOST_CHECK(aMgr->PlaceBid(third, bidderA, 5000 * COIN, bidId));
}

// Current behaviour: a namespace auction (empty name) passes the parameter
// checks only if its namespace exists and is available at the same time, and
// a namespace that exists is not available. AuctionManager::CreateAuction
// therefore never creates a namespace auction; those are created by
// NamespaceManager::CreateNamespaceAuction.
BOOST_FIXTURE_TEST_CASE(namespace_auction_cannot_be_created_through_auction_manager, AuctionTestingSetup)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    uint256 auctionId;

    // The namespace exists: not available
    BOOST_CHECK(!CreateNSAuction(aMgr, NS, creator, nsMgr->CalculateMinPrice(NS), 7, auctionId));
    // The namespace does not exist: not found
    BOOST_CHECK(!CreateNSAuction(aMgr, "freens", creator, nsMgr->CalculateMinPrice("freens"), 7, auctionId));

    BOOST_CHECK(nsMgr->CreateNamespaceAuction("freens", creator, nsMgr->CalculateMinPrice("freens"), 7, AUCTION_STANDARD, auctionId));
    // The auction manager does not hold the auction; it finds it in the database
    AuctionInfo info;
    BOOST_CHECK(aMgr->GetAuctionInfo(auctionId, info));
    BOOST_CHECK(info.name.empty());
    uint256 bidId;
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, MakeKeyID(), info.reservePrice, bidId));
}

// ===================================================================
// Auction and bid IDs
// ===================================================================

BOOST_FIXTURE_TEST_CASE(auction_and_bid_ids, AuctionTestingSetup)
{
    // Auction ID = Hash(name, namespace, start time), as serialized for the network
    uint256 auctionId = NewAuction("alice");
    BOOST_CHECK(auctionId == ExpectedAuctionId("alice", NS, T0));
    BOOST_CHECK_EQUAL(auctionId.GetHex(), "d0620430f39e4ecb325e4cb24f78a5580cd6d38de39d0d7132d30ceda71a9e65");

    // Each component changes the ID
    CreateTestNamespace("otherns", creator);
    uint256 otherName = NewAuction("alicf");
    AuctionInfo inOtherNs = UserAuction("alice", "otherns", creator, 5000 * COIN, T0);
    uint256 otherNs;
    BOOST_REQUIRE(aMgr->CreateAuction(inOtherNs, otherNs));
    SetMockTime(T0 + 1);
    uint256 otherTime = NewAuction("alice");
    SetMockTime(T0);
    BOOST_CHECK(otherName == ExpectedAuctionId("alicf", NS, T0));
    BOOST_CHECK(otherNs == ExpectedAuctionId("alice", "otherns", T0));
    BOOST_CHECK(otherTime == ExpectedAuctionId("alice", NS, T0 + 1));
    BOOST_CHECK(auctionId != otherName);
    BOOST_CHECK(auctionId != otherNs);
    BOOST_CHECK(auctionId != otherTime);
    // The creator, the reserve price and the end time are not part of the ID
    BOOST_CHECK(GetAuction(aMgr, auctionId).creator == GetAuction(aMgr, otherTime).creator);

    // Bid ID = Hash(auction ID, bidder, amount, bid time)
    CKeyID bidder(uint160(ParseHex("00112233445566778899aabbccddeeff00112233")));
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, bidder, 6000 * COIN, bidId, T0 + 5));
    BOOST_CHECK(bidId == ExpectedBidId(auctionId, bidder, 6000 * COIN, T0 + 5));
    BOOST_CHECK_EQUAL(bidId.GetHex(), "7276f704f9b90942a729387241ca6cae57ed5e6a303cd64aad05344b1da0fe07");
    BOOST_CHECK(GetBid(aMgr, bidId).bidId == bidId);

    // Without a bid time the clock is used
    CKeyID second = MakeKeyID();
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, second, 7000 * COIN, bidId));
    BOOST_CHECK(bidId == ExpectedBidId(auctionId, second, 7000 * COIN, T0));
    BOOST_CHECK_EQUAL(GetBid(aMgr, bidId).bidTime, T0);
}

// ===================================================================
// Placing bids
// ===================================================================

// SUSPECTED BUG (documented, not fixed): PlaceBid moves a pending auction to
// the active state before it validates the bid. A bid that is then rejected
// leaves the auction active, with no bid.
BOOST_FIXTURE_TEST_CASE(rejected_bid_still_activates_the_auction, AuctionTestingSetup)
{
    uint256 bidId;

    uint256 byCreator = NewAuction("alice");
    BOOST_CHECK(!aMgr->PlaceBid(byCreator, creator, 5000 * COIN, bidId));
    uint256 belowReserve = NewAuction("bob");
    BOOST_CHECK(!aMgr->PlaceBid(belowReserve, MakeKeyID(), 5000 * COIN - 1, bidId));

    for (const uint256& auctionId : {byCreator, belowReserve}) {
        AuctionInfo info = GetAuction(aMgr, auctionId);
        BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
        BOOST_CHECK_EQUAL(info.currentBid, 0);
        BOOST_CHECK(info.currentBidder.IsNull());
        BOOST_CHECK(aMgr->IsAuctionActive(auctionId));
        BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
        AuctionInfo stored;
        BOOST_REQUIRE(db->ReadAuction(auctionId, stored));
        BOOST_CHECK_EQUAL(stored.state, AUCTION_ACTIVE);
    }
    BOOST_CHECK_EQUAL(aMgr->GetActiveAuctions().size(), 2U);
    BOOST_CHECK_EQUAL(aMgr->GetAuctionsByNamespace(NS).size(), 2U);

    // Such an auction is not canceled as an expired pending auction; it is
    // finalized as an active one, which cancels it for lack of bids
    SetMockTime(T0 + 7 * DAY);
    BOOST_CHECK(aMgr->ProcessExpiredAuctions(GetTime()));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, byCreator).state, AUCTION_CANCELED);
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());

    // A bid on an unknown auction is rejected outright
    BOOST_CHECK(!aMgr->PlaceBid(uint256S("01"), MakeKeyID(), 5000 * COIN, bidId));
}

// Current behaviour: a higher bid from a bidder who has bid before does not
// replace the earlier bid. A second bid record is added and the first one is
// marked as refunded, in memory and in the database.
BOOST_FIXTURE_TEST_CASE(raised_bid_is_a_second_bid_record, AuctionTestingSetup)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    uint256 firstBid, secondBid, rejected;

    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, firstBid));
    // The raise has to clear the minimum increment over the bidder's own bid
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, bidder, 5000 * COIN, rejected));
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, bidder, 5250 * COIN - 1, rejected));
    BOOST_CHECK(!GetBid(aMgr, firstBid).refunded);

    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, bidder, 6000 * COIN, secondBid));
    BOOST_CHECK(firstBid != secondBid);

    std::vector<BidInfo> bids = aMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 2U);
    BOOST_CHECK(bids[0].bidId == firstBid);
    BOOST_CHECK(bids[0].refunded);
    BOOST_CHECK(!bids[0].isWinner);
    BOOST_CHECK(bids[1].bidId == secondBid);
    BOOST_CHECK(!bids[1].refunded);
    BOOST_CHECK(bids[0].bidder == bids[1].bidder);

    BidInfo stored;
    BOOST_REQUIRE(db->ReadBid(firstBid, stored));
    BOOST_CHECK(stored.refunded);
    BOOST_REQUIRE(db->ReadBid(secondBid, stored));
    BOOST_CHECK(!stored.refunded);

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.currentBid, 6000 * COIN);
    BOOST_CHECK(info.currentBidder == bidder);
    BOOST_CHECK(aMgr->GetHighestBid(auctionId).bidId == secondBid);

    BidderStats stats;
    BOOST_REQUIRE(aMgr->GetBidderStats(bidder, stats));
    BOOST_CHECK_EQUAL(stats.totalBids, 2);
    BOOST_CHECK_EQUAL(stats.auctionParticipations, 1);
}

// The minimum next bid is the current bid and the minimum increment of the
// namespace on it, rounded down to a whole base unit, for every amount and in
// every build.
BOOST_FIXTURE_TEST_CASE(minimum_increment_is_exact, AuctionTestingSetup)
{
    uint256 bidId;

    uint256 exactId = NewAuction("exact");
    BOOST_REQUIRE(aMgr->PlaceBid(exactId, MakeKeyID(), 5000 * COIN, bidId));
    BOOST_CHECK(!aMgr->PlaceBid(exactId, MakeKeyID(), 5250 * COIN - 1, bidId));
    BOOST_CHECK(aMgr->PlaceBid(exactId, MakeKeyID(), 5250 * COIN, bidId));

    uint256 smallId = NewAuction("small");
    BOOST_REQUIRE(aMgr->PlaceBid(smallId, MakeKeyID(), 5000 * COIN + 1, bidId));
    // 105% of 500000000001 is 525000000001.05
    BOOST_CHECK(!aMgr->PlaceBid(smallId, MakeKeyID(), 525000000000, bidId));
    BOOST_CHECK(aMgr->PlaceBid(smallId, MakeKeyID(), 525000000001, bidId));

    // Amounts above 2^53 base units, which a double cannot hold.
    // 100,000,000.00000001 RDD: 105% is 10500000000000001.05 base units
    uint256 lowId = NewAuction("roundsdown");
    const CAmount lowCurrent = 10000000000000001;
    BOOST_REQUIRE(aMgr->PlaceBid(lowId, MakeKeyID(), lowCurrent, bidId));
    BOOST_CHECK(!aMgr->PlaceBid(lowId, MakeKeyID(), 10500000000000000, bidId));
    BOOST_CHECK(aMgr->PlaceBid(lowId, MakeKeyID(), 10500000000000001, bidId));

    // 100,000,000.00000003 RDD: 105% is 10500000000000003.15 base units
    uint256 highId = NewAuction("alsodown");
    const CAmount highCurrent = 10000000000000003;
    BOOST_REQUIRE(aMgr->PlaceBid(highId, MakeKeyID(), highCurrent, bidId));
    BOOST_CHECK(!aMgr->PlaceBid(highId, MakeKeyID(), 10500000000000002, bidId));
    BOOST_CHECK(aMgr->PlaceBid(highId, MakeKeyID(), 10500000000000003, bidId));

    // 5% more than all the money there can be is more than an amount can
    // hold: the minimum is then the largest amount, and not an overflow
    uint256 topId = NewAuction("highest");
    BOOST_REQUIRE(aMgr->PlaceBid(topId, MakeKeyID(), MAX_MONEY, bidId));
    BOOST_CHECK(!aMgr->PlaceBid(topId, MakeKeyID(), MAX_MONEY + 1, bidId));
    BOOST_CHECK(!aMgr->PlaceBid(topId, MakeKeyID(), std::numeric_limits<CAmount>::max() - 1, bidId));
}

// The increment is the one of the namespace, which can have two decimals
BOOST_FIXTURE_TEST_CASE(minimum_increment_of_the_namespace, AuctionTestingSetup)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    NamespaceInfo info;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo(NS, info));
    info.id = "decimals";
    info.minBidIncrement = 7.55;
    BOOST_REQUIRE(nsMgr->CreateNamespace(info));

    uint256 bidId, auctionId;
    AuctionInfo auction = reddid_test::UserAuction("alice", "decimals", creator, 5000 * COIN, GetTime());
    BOOST_REQUIRE(aMgr->CreateAuction(auction, auctionId));
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 5000 * COIN + 1, bidId));
    // 107.55% of 500000000001 is 537750000001.0755
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, MakeKeyID(), 537750000000, bidId));
    BOOST_CHECK(aMgr->PlaceBid(auctionId, MakeKeyID(), 537750000001, bidId));
}

BOOST_FIXTURE_TEST_CASE(bid_on_ended_or_closed_auction_is_rejected, AuctionTestingSetup)
{
    uint256 bidId;
    uint256 auctionId = NewAuction("alice");
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 5000 * COIN, bidId));

    SetMockTime(T0 + 7 * DAY - 1);
    BOOST_CHECK(aMgr->PlaceBid(auctionId, MakeKeyID(), 6000 * COIN, bidId));

    // That bid, one second before the end, moved the end to six hours after it
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).endTime, T0 + 7 * DAY - 1 + 6 * HOUR);
    SetMockTime(T0 + 7 * DAY - 1 + 6 * HOUR - 1);
    BOOST_CHECK(aMgr->PlaceBid(auctionId, MakeKeyID(), 7000 * COIN, bidId));
    SetMockTime(GetAuction(aMgr, auctionId).endTime);
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, MakeKeyID(), 8000 * COIN, bidId));

    BOOST_REQUIRE(aMgr->FinalizeAuction(auctionId));
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, MakeKeyID(), 9000 * COIN, bidId));
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 3U);
}

// ===================================================================
// Anti-sniping extension (ExtendAuctionIfNeeded, through PlaceBid)
// ===================================================================

// SUSPECTED BUG (documented, not fixed): a bid in the last 24 hours sets the
// end of a user ID auction to six hours after the bid. For a bid less than
// six hours before the end that is an extension; for a bid between 24 and 6
// hours before the end it brings the end forward, by up to 18 hours.
BOOST_FIXTURE_TEST_CASE(late_bid_extension_boundaries, AuctionTestingSetup)
{
    const int64_t end = T0 + 7 * DAY;
    uint256 bidId;

    // Exactly 24 hours before the end: unchanged
    uint256 atBoundary = NewAuction("boundary");
    BOOST_REQUIRE(aMgr->PlaceBid(atBoundary, MakeKeyID(), 5000 * COIN, bidId, end - 24 * HOUR));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, atBoundary).endTime, end);

    // One second later: the auction now ends almost 18 hours earlier
    uint256 shortened = NewAuction("shortened");
    BOOST_REQUIRE(aMgr->PlaceBid(shortened, MakeKeyID(), 5000 * COIN, bidId, end - 24 * HOUR + 1));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, shortened).endTime, end - 18 * HOUR + 1);
    BOOST_CHECK(GetAuction(aMgr, shortened).endTime < end);

    // Exactly six hours before the end: unchanged in effect
    uint256 sixHours = NewAuction("sixhours");
    BOOST_REQUIRE(aMgr->PlaceBid(sixHours, MakeKeyID(), 5000 * COIN, bidId, end - 6 * HOUR));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, sixHours).endTime, end);

    // In the last six hours: extended
    uint256 extended = NewAuction("extended");
    BOOST_REQUIRE(aMgr->PlaceBid(extended, MakeKeyID(), 5000 * COIN, bidId, end - 1));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, extended).endTime, end - 1 + 6 * HOUR);

    // Every late bid moves the end again
    BOOST_REQUIRE(aMgr->PlaceBid(extended, MakeKeyID(), 6000 * COIN, bidId, end + HOUR));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, extended).endTime, end + 7 * HOUR);
    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(extended, stored));
    BOOST_CHECK_EQUAL(stored.endTime, end + 7 * HOUR);

    // Current behaviour: the bid time is whatever the caller passes. It is not
    // compared with the clock (which still reads T0 here), so it can place the
    // end of the auction anywhere.
    uint256 anywhere = NewAuction("anywhere");
    BOOST_REQUIRE(aMgr->PlaceBid(anywhere, MakeKeyID(), 5000 * COIN, bidId, T0 + 1000 * DAY));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, anywhere).endTime, T0 + 1000 * DAY + 6 * HOUR);
    BOOST_CHECK_EQUAL(GetBid(aMgr, bidId).bidTime, T0 + 1000 * DAY);
}

// ===================================================================
// Loading and saving
// ===================================================================

BOOST_FIXTURE_TEST_CASE(second_auction_manager_loads_everything_from_the_database, AuctionTestingSetup)
{
    uint256 pendingId = NewAuction("pending");
    uint256 activeId = NewAuction("active");
    DepositBid first = PlaceDepositBid(activeId, 6000 * COIN);
    DepositBid second = PlaceDepositBid(activeId, 8000 * COIN);
    uint256 canceledId = NewAuction("canceled");
    BOOST_REQUIRE(aMgr->CancelAuction(canceledId, creator));

    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.depositPercentage = 33;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));
    // The default configurations of the first manager exist in memory only
    AuctionConfig stored;
    BOOST_CHECK(!db->ReadAuctionConfig("namespace", stored));
    BOOST_CHECK(db->ReadAuctionConfig("userid", stored));

    {
        AuctionManager fresh(*reddidManager);
        BOOST_CHECK(!fresh.IsInitialized());
        BOOST_REQUIRE(fresh.Init(reddidManager.get()));
        BOOST_CHECK(fresh.IsInitialized());
        BOOST_CHECK(!fresh.IsRunning());
        BOOST_CHECK(fresh.GetDB() == db);
        BOOST_CHECK(fresh.GetNamespaceManager() == reddidManager->GetNamespaceManager());

        BOOST_CHECK_EQUAL(GetAuction(&fresh, pendingId).state, AUCTION_PENDING);
        BOOST_CHECK_EQUAL(GetAuction(&fresh, canceledId).state, AUCTION_CANCELED);
        AuctionInfo active = GetAuction(&fresh, activeId);
        BOOST_CHECK_EQUAL(active.state, AUCTION_ACTIVE);
        BOOST_CHECK_EQUAL(active.currentBid, 8000 * COIN);
        BOOST_CHECK(active.currentBidder == second.bidder);
        BOOST_CHECK_EQUAL(active.totalDepositsLocked, 900 * COIN + 1200 * COIN);

        std::vector<AuctionInfo> activeAuctions = fresh.GetActiveAuctions();
        BOOST_REQUIRE_EQUAL(activeAuctions.size(), 1U);
        BOOST_CHECK(activeAuctions[0].auctionId == activeId);

        BOOST_CHECK_EQUAL(fresh.GetAuctionBids(activeId).size(), 2U);
        BOOST_CHECK(fresh.GetHighestBid(activeId).bidId == second.bidId);
        BOOST_CHECK(GetBid(&fresh, first.bidId).depositUtxo == first.utxo);
        BOOST_CHECK(fresh.IsDepositLocked(first.utxo));
        BOOST_CHECK(fresh.IsDepositLocked(second.utxo));
        BOOST_CHECK_EQUAL(fresh.GetDepositLocksForAuction(activeId).size(), 2U);

        BOOST_REQUIRE(fresh.GetAuctionConfig("userid", config));
        BOOST_CHECK_EQUAL(config.depositPercentage, 33);
        BOOST_REQUIRE(fresh.GetAuctionConfig("namespace", config));
        BOOST_CHECK_EQUAL(config.depositPercentage, 20);
        BOOST_REQUIRE(fresh.GetAuctionConfig("reddid", config));
        BOOST_CHECK_EQUAL(config.depositPercentage, 15);
        BOOST_CHECK(!fresh.GetAuctionConfig("other", config));

        // It can carry on where the first one stopped
        BOOST_CHECK(fresh.Start());
        BOOST_CHECK(fresh.IsRunning());
        uint256 bidId;
        BOOST_CHECK(fresh.PlaceBid(activeId, MakeKeyID(), 9000 * COIN, bidId));
        fresh.Interrupt();
        BOOST_CHECK(!fresh.IsRunning());
        BOOST_CHECK(fresh.Stop());
        BOOST_CHECK(!fresh.IsInitialized());
        BOOST_CHECK(fresh.GetDB() == nullptr);
    }

    // Stopping saved the default configurations as well
    BOOST_CHECK(db->ReadAuctionConfig("namespace", stored));
    BOOST_CHECK(db->ReadAuctionConfig("reddid", stored));
    // The first manager knows nothing of the bid that the second one took
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(activeId).size(), 2U);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activeId).currentBid, 8000 * COIN);
    AuctionInfo storedAuction;
    BOOST_REQUIRE(db->ReadAuction(activeId, storedAuction));
    BOOST_CHECK_EQUAL(storedAuction.currentBid, 9000 * COIN);
}

BOOST_FIXTURE_TEST_CASE(auction_manager_that_was_not_initialized_refuses_to_work, AuctionTestingSetup)
{
    AuctionManager idle(*reddidManager);
    BOOST_CHECK(!idle.IsInitialized());
    BOOST_CHECK(!idle.IsRunning());
    BOOST_CHECK(idle.GetDB() == nullptr);
    BOOST_CHECK(idle.GetNamespaceManager() == nullptr);
    BOOST_CHECK(idle.GetP2P() == nullptr);

    BOOST_CHECK(!idle.Start());
    BOOST_CHECK(!idle.Init(nullptr));
    BOOST_CHECK(!idle.Save());

    uint256 id;
    AuctionInfo auction = UserAuction("alice", NS, creator, 5000 * COIN, T0);
    BOOST_CHECK(!idle.CreateAuction(auction, id));
    BOOST_CHECK(!idle.PlaceBid(uint256S("01"), MakeKeyID(), 5000 * COIN, id));
    BOOST_CHECK(!idle.FinalizeAuction(uint256S("01")));
    BOOST_CHECK(!idle.CancelAuction(uint256S("01"), creator));
    BOOST_CHECK(!idle.ProcessExpiredAuctions(T0));
    CMutableTransaction mtx = TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, uint256S("01")));
    BOOST_CHECK(!idle.ProcessTransaction(CTransaction(mtx), 1));
    BOOST_CHECK(!idle.UndoTransaction(CTransaction(mtx), 1));
    AuctionInfo info;
    BOOST_CHECK(!idle.GetAuctionInfo(uint256S("01"), info));
    BidInfo bid;
    BOOST_CHECK(!idle.GetBidInfo(uint256S("01"), bid));
    BOOST_CHECK(idle.GetAuctionBids(uint256S("01")).empty());
    BOOST_CHECK(idle.GetActiveAuctions().empty());

    // Interrupt and Stop are no-ops
    idle.Interrupt();
    BOOST_CHECK(idle.Stop());
}

BOOST_AUTO_TEST_SUITE_END()
