// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/reddid_auction_test_util.h>
#include <uint256.h>
#include <util/time.h>

#include <string>
#include <vector>

using namespace reddid_test;

namespace {
/** A transaction that spends the given outpoints and has no outputs. */
CTransaction SpendOf(const std::vector<COutPoint>& inputs)
{
    CMutableTransaction mtx;
    for (const COutPoint& input : inputs) {
        mtx.vin.emplace_back(input);
    }
    return CTransaction(mtx);
}

/** A transaction that spends the given outpoint and carries the record. */
CTransaction SpendWithRecord(const COutPoint& input, const CScript& record)
{
    return CTransaction(RecordTx(input, record));
}
} // namespace

/**
 * The settlement half of AuctionManager, called directly: BeginSettlement,
 * CompleteSettlement, ProcessDefault, ReleaseLosingDeposits, the checks on
 * spending locked deposits, the bidder statistics and the periodic tasks.
 */
BOOST_FIXTURE_TEST_SUITE(reddid_settlement_tests, AuctionTestingSetup)

// ===================================================================
// Category 1: BeginSettlement
// ===================================================================

BOOST_AUTO_TEST_CASE(begin_settlement_preconditions)
{
    CTransaction tx = SpendOf({});

    BOOST_CHECK(!aMgr->BeginSettlement(uint256S("01"), tx));

    // A pending auction, before and after its end
    uint256 pendingId = NewAuction("alice");
    BOOST_CHECK(!aMgr->BeginSettlement(pendingId, tx));

    // An active auction that has not ended
    uint256 activeId = NewAuction("bob");
    PlaceDepositBid(activeId, 6000 * COIN);
    BOOST_CHECK(!aMgr->BeginSettlement(activeId, tx));
    SetMockTime(T0 + 7 * DAY - 1);
    BOOST_CHECK(!aMgr->BeginSettlement(activeId, tx));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activeId).state, AUCTION_ACTIVE);

    SetMockTime(T0 + 7 * DAY);
    BOOST_CHECK(!aMgr->BeginSettlement(pendingId, tx));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, pendingId).state, AUCTION_PENDING);
    BOOST_CHECK(aMgr->BeginSettlement(activeId, tx));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activeId).state, AUCTION_SETTLEMENT);

    // Settlement begins once
    BOOST_CHECK(!aMgr->BeginSettlement(activeId, tx));
}

BOOST_AUTO_TEST_CASE(begin_settlement_of_active_auction_without_bids_cancels_it)
{
    uint256 auctionId = NewAuction("alice");
    // A rejected bid leaves the auction active and without a bid
    uint256 bidId;
    BOOST_REQUIRE(!aMgr->PlaceBid(auctionId, MakeKeyID(), 1 * COIN, bidId));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);

    EndAuction(auctionId);
    BOOST_CHECK(aMgr->BeginSettlement(auctionId, SpendOf({})));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_CANCELED);
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 0);
    BOOST_CHECK(info.finalizeTxid.IsNull());
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());
}

BOOST_AUTO_TEST_CASE(begin_settlement_marks_winner_and_releases_losing_deposits)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid low = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid mid = PlaceDepositBid(auctionId, 7000 * COIN);
    DepositBid high = PlaceDepositBid(auctionId, 8000 * COIN);

    EndAuction(auctionId);
    CTransaction finalizeTx = SpendWithRecord(COutPoint(uint256S("f1"), 0), RecordScript(OP_AUCTION_FINALIZE, auctionId));
    BOOST_CHECK(aMgr->BeginSettlement(auctionId, finalizeTx));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_SETTLEMENT);
    BOOST_CHECK(info.finalizeTxid == finalizeTx.GetHash());
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 4320);
    BOOST_CHECK(info.settleTxid.IsNull());
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());

    BidInfo winningBid = GetBid(aMgr, high.bidId);
    BOOST_CHECK(winningBid.isWinner);
    BOOST_CHECK(!winningBid.refunded);
    BOOST_CHECK_EQUAL(winningBid.depositState, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK(aMgr->IsDepositLocked(high.utxo));

    for (const DepositBid* loser : {&low, &mid}) {
        BidInfo bid = GetBid(aMgr, loser->bidId);
        BOOST_CHECK(!bid.isWinner);
        BOOST_CHECK(bid.refunded);
        BOOST_CHECK_EQUAL(bid.depositState, DEPOSIT_RELEASED_LOST);
        BOOST_CHECK(!aMgr->IsDepositLocked(loser->utxo));
        BOOST_CHECK_EQUAL(GetLock(aMgr, loser->utxo).state, DEPOSIT_RELEASED_LOST);

        BidInfo stored;
        BOOST_REQUIRE(db->ReadBid(loser->bidId, stored));
        BOOST_CHECK(stored.refunded);
        BOOST_CHECK_EQUAL(stored.depositState, DEPOSIT_RELEASED_LOST);
        DepositLock storedLock;
        BOOST_REQUIRE(db->ReadDepositLock(loser->utxo, storedLock));
        BOOST_CHECK_EQUAL(storedLock.state, DEPOSIT_RELEASED_LOST);
    }

    BidInfo storedWinner;
    BOOST_REQUIRE(db->ReadBid(high.bidId, storedWinner));
    BOOST_CHECK(storedWinner.isWinner);
    AuctionInfo storedAuction;
    BOOST_REQUIRE(db->ReadAuction(auctionId, storedAuction));
    BOOST_CHECK_EQUAL(storedAuction.state, AUCTION_SETTLEMENT);
    BOOST_CHECK_EQUAL(storedAuction.settlementDeadlineHeight, 4320);

    // Nothing is registered yet
    UserIDInfo userId;
    BOOST_CHECK(!db->ReadUserID("alice", NS, userId));
}

BOOST_AUTO_TEST_CASE(begin_settlement_deadline_is_tip_height_plus_window)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 100;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    PlaceDepositBid(auctionId, 6000 * COIN);
    EndAuction(auctionId);

    FakeChainHeight tip(chainstate, 250);
    BOOST_CHECK(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).settlementDeadlineHeight, 350);
}

// Current behaviour: the winner is the highest bid whose deposit is still
// locked, which need not be the bid that the auction records as its current
// bid.
BOOST_AUTO_TEST_CASE(begin_settlement_skips_bids_whose_deposit_is_not_locked)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid low = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid mid = PlaceDepositBid(auctionId, 7000 * COIN);
    DepositBid high = PlaceDepositBid(auctionId, 8000 * COIN);
    BOOST_REQUIRE(aMgr->UpdateDepositLock(high.utxo, DEPOSIT_RELEASED_LOST));

    EndAuction(auctionId);
    BOOST_CHECK(aMgr->BeginSettlement(auctionId, SpendOf({})));

    BOOST_CHECK(GetBid(aMgr, mid.bidId).isWinner);
    BOOST_CHECK(!GetBid(aMgr, high.bidId).isWinner);
    BOOST_CHECK(!GetBid(aMgr, low.bidId).isWinner);
    BOOST_CHECK(aMgr->IsDepositLocked(mid.utxo));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.currentBid, 8000 * COIN);
    BOOST_CHECK(info.currentBidder == high.bidder);
    BOOST_CHECK(aMgr->IsHighestBidder(auctionId, high.bidder));
    BOOST_CHECK(!aMgr->IsHighestBidder(auctionId, mid.bidder));
}

BOOST_AUTO_TEST_CASE(begin_settlement_fails_when_no_deposit_is_locked)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid only = PlaceDepositBid(auctionId, 6000 * COIN);
    BOOST_REQUIRE(aMgr->UpdateDepositLock(only.utxo, DEPOSIT_RELEASED_LOST));

    EndAuction(auctionId);
    BOOST_CHECK(!aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);
    BOOST_CHECK(!GetBid(aMgr, only.bidId).isWinner);
}

// SUSPECTED BUG (documented, not fixed): a bid without a deposit takes part
// in the settlement of an auction that has deposits. When it is the highest it
// wins, and since it has no deposit to spend, CompleteSettlement accepts any
// transaction for it: the name is registered without the winner having paid
// or locked anything.
BOOST_AUTO_TEST_CASE(bid_without_deposit_can_win_and_settle_for_free)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid backed = PlaceDepositBid(auctionId, 6000 * COIN);
    CKeyID freeRider = MakeKeyID();
    uint256 freeBid;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, freeRider, 8000 * COIN, freeBid));

    EndAuction(auctionId);
    BOOST_CHECK(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_CHECK(GetBid(aMgr, freeBid).isWinner);
    BOOST_CHECK_EQUAL(GetLock(aMgr, backed.utxo).state, DEPOSIT_RELEASED_LOST);

    BOOST_CHECK(aMgr->CompleteSettlement(auctionId, freeBid, SpendOf({})));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_COMPLETED);
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == freeRider);
    // The winning bid is marked as if a deposit had been consumed
    BOOST_CHECK_EQUAL(GetBid(aMgr, freeBid).depositState, DEPOSIT_CONSUMED_WON);
    BOOST_CHECK(GetBid(aMgr, freeBid).depositUtxo.IsNull());
}

// SUSPECTED BUG (documented, not fixed): when a bidder raises a bid, the
// earlier bid is marked as refunded but its deposit stays locked.
// ReleaseLosingDeposits skips bids that are already marked as refunded, so
// the deposit of the replaced bid is never released.
BOOST_AUTO_TEST_CASE(deposit_of_a_replaced_bid_is_never_released)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    DepositBid first = PlaceDepositBid(auctionId, bidder, 6000 * COIN);
    DepositBid raised = PlaceDepositBid(auctionId, bidder, 8000 * COIN);

    BOOST_CHECK(GetBid(aMgr, first.bidId).refunded);
    BOOST_CHECK(aMgr->IsDepositLocked(first.utxo));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).totalDepositsLocked, 900 * COIN + 1200 * COIN);

    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_REQUIRE(GetBid(aMgr, raised.bidId).isWinner);
    BOOST_CHECK(aMgr->IsDepositLocked(first.utxo));
    BOOST_CHECK_EQUAL(GetBid(aMgr, first.bidId).depositState, DEPOSIT_LOCKED_ACTIVE);

    BOOST_REQUIRE(aMgr->CompleteSettlement(auctionId, raised.bidId, SpendOf({raised.utxo})));
    BOOST_CHECK(aMgr->IsDepositLocked(first.utxo));

    CMutableTransaction spend;
    spend.vin.emplace_back(first.utxo);
    spend.vout.emplace_back(first.deposit, P2PKH(bidder));
    BOOST_CHECK(!aMgr->CheckDepositLocks(CTransaction(spend)));
}

// ===================================================================
// Category 2: CompleteSettlement
// ===================================================================

// SUSPECTED BUG (documented, not fixed): CompleteSettlement looks at the
// inputs of the settlement transaction only. The transaction that completes
// the settlement below has no outputs at all: the winning bid of 8,000 RDD is
// never paid, and the 1,200 RDD deposit is not directed anywhere.
BOOST_AUTO_TEST_CASE(complete_settlement_checks_no_outputs)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);

    // Not in settlement
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, SpendOf({winner.utxo})));

    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, SpendOf({})));

    BOOST_CHECK(!aMgr->CompleteSettlement(uint256S("01"), winner.bidId, SpendOf({winner.utxo})));
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, uint256S("02"), SpendOf({winner.utxo})));
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, loser.bidId, SpendOf({loser.utxo})));
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, SpendOf({})));
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, SpendOf({loser.utxo})));
    // The right outpoint hash with another output index
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, SpendOf({COutPoint(winner.utxo.hash, 0)})));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));

    SetMockTime(GetTime() + 2 * DAY);
    CTransaction settleTx = SpendOf({COutPoint(uint256S("aa"), 5), winner.utxo});
    BOOST_REQUIRE(settleTx.vout.empty());
    BOOST_CHECK(aMgr->CompleteSettlement(auctionId, winner.bidId, settleTx));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_COMPLETED);
    BOOST_CHECK(info.settleTxid == settleTx.GetHash());
    BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));

    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK_EQUAL(userId.name, "alice");
    BOOST_CHECK_EQUAL(userId.namespaceId, NS);
    BOOST_CHECK(userId.owner == winner.bidder);
    BOOST_CHECK_EQUAL(userId.registrationTime, GetTime());
    BOOST_CHECK_EQUAL(userId.expirationTime, GetTime() + 365 * DAY);
    BOOST_CHECK_EQUAL(userId.lastTransaction, GetTime());
    BOOST_CHECK_EQUAL(userId.transactionCount, 1);

    DepositLock lock = GetLock(aMgr, winner.utxo);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_CONSUMED_WON);
    BOOST_CHECK_EQUAL(lock.releaseHeight, 0);
    BOOST_CHECK_EQUAL(GetBid(aMgr, winner.bidId).depositState, DEPOSIT_CONSUMED_WON);
    BidInfo storedBid;
    BOOST_REQUIRE(db->ReadBid(winner.bidId, storedBid));
    BOOST_CHECK_EQUAL(storedBid.depositState, DEPOSIT_CONSUMED_WON);
    AuctionInfo storedAuction;
    BOOST_REQUIRE(db->ReadAuction(auctionId, storedAuction));
    BOOST_CHECK_EQUAL(storedAuction.state, AUCTION_COMPLETED);

    // Completed once
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, settleTx));

    // The name is taken now
    AuctionInfo again = UserAuction("alice", NS, creator, 5000 * COIN, GetTime());
    uint256 againId;
    BOOST_CHECK(!aMgr->CreateAuction(again, againId));
}

BOOST_AUTO_TEST_CASE(settlement_deadline_is_enforced_by_tip_height)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 10;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 settledId = NewAuction("alice");
    DepositBid settler = PlaceDepositBid(settledId, 6000 * COIN);
    uint256 lateId = NewAuction("bob");
    DepositBid late = PlaceDepositBid(lateId, 6000 * COIN);
    EndAuction(settledId);
    BOOST_REQUIRE(aMgr->BeginSettlement(settledId, SpendOf({})));
    BOOST_REQUIRE(aMgr->BeginSettlement(lateId, SpendOf({})));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, settledId).settlementDeadlineHeight, 10);

    {
        // At the deadline the winner can still settle and cannot be defaulted
        FakeChainHeight tip(chainstate, 10);
        BOOST_CHECK(!aMgr->ProcessDefault(settledId, settler.bidId));
        BOOST_CHECK(aMgr->CompleteSettlement(settledId, settler.bidId, SpendOf({settler.utxo})));
        BOOST_CHECK_EQUAL(GetLock(aMgr, settler.utxo).releaseHeight, 10);
        BOOST_CHECK(!aMgr->ProcessDefault(lateId, late.bidId));
    }
    {
        // One block later it is the other way round
        FakeChainHeight tip(chainstate, 11);
        BOOST_CHECK(!aMgr->CompleteSettlement(lateId, late.bidId, SpendOf({late.utxo})));
        BOOST_CHECK_EQUAL(GetAuction(aMgr, lateId).state, AUCTION_SETTLEMENT);
        BOOST_CHECK(aMgr->ProcessDefault(lateId, late.bidId));
        BOOST_CHECK_EQUAL(GetAuction(aMgr, lateId).state, AUCTION_DEFAULTED);
        DepositLock lock = GetLock(aMgr, late.utxo);
        BOOST_CHECK_EQUAL(lock.state, DEPOSIT_FORFEITED);
        BOOST_CHECK_EQUAL(lock.releaseHeight, 11);
        // The re-auction records the height at which it was created
        BOOST_CHECK_EQUAL(GetAuction(aMgr, ExpectedAuctionId("bob", NS, GetTime())).blockHeight, 11);
    }
}

// ===================================================================
// Category 3: ProcessDefault
// ===================================================================

// SUSPECTED BUG (documented, not fixed): BeginSettlement marks the winning
// bid with isWinner and ProcessDefault does not clear the mark, so a bidder
// who defaulted keeps the auction among the auctions won (see the statistics
// below). The deposit lock is forfeited and the name is re-auctioned.
BOOST_AUTO_TEST_CASE(process_default_keeps_the_winner_mark)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);

    BOOST_CHECK(!aMgr->ProcessDefault(uint256S("01"), winner.bidId));
    // Not in settlement
    BOOST_CHECK(!aMgr->ProcessDefault(auctionId, winner.bidId));

    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_CHECK(!aMgr->ProcessDefault(auctionId, loser.bidId));
    BOOST_CHECK(!aMgr->ProcessDefault(auctionId, uint256S("02")));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);

    BOOST_CHECK(aMgr->ProcessDefault(auctionId, winner.bidId));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_DEFAULTED);
    BOOST_CHECK(info.currentBidder == winner.bidder);
    BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));

    BidInfo bid = GetBid(aMgr, winner.bidId);
    BOOST_CHECK(bid.isWinner);
    BOOST_CHECK_EQUAL(bid.depositState, DEPOSIT_FORFEITED);
    BOOST_CHECK(!bid.refunded);
    BidInfo storedBid;
    BOOST_REQUIRE(db->ReadBid(winner.bidId, storedBid));
    BOOST_CHECK(storedBid.isWinner);
    BOOST_CHECK_EQUAL(storedBid.depositState, DEPOSIT_FORFEITED);

    BOOST_CHECK_EQUAL(GetLock(aMgr, winner.utxo).state, DEPOSIT_FORFEITED);
    BOOST_CHECK(!aMgr->IsDepositLocked(winner.utxo));
    // The loser's deposit was released when the settlement began and stays released
    BOOST_CHECK_EQUAL(GetLock(aMgr, loser.utxo).state, DEPOSIT_RELEASED_LOST);

    BidderStats stats;
    BOOST_REQUIRE(aMgr->GetBidderStats(winner.bidder, stats));
    BOOST_CHECK_EQUAL(stats.auctionWins, 1);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 1);
    BOOST_CHECK_EQUAL(stats.depositsForfeit, 1);

    // Nothing was registered, and the name is on auction again
    UserIDInfo userId;
    BOOST_CHECK(!db->ReadUserID("alice", NS, userId));
    AuctionInfo reAuction = GetAuction(aMgr, ExpectedAuctionId("alice", NS, GetTime()));
    BOOST_CHECK(reAuction.previousAuctionId == auctionId);
    BOOST_CHECK_EQUAL(reAuction.reAuctionCount, 1);
    BOOST_CHECK_EQUAL(reAuction.state, AUCTION_PENDING);

    // Defaulted once; the settlement cannot be completed any more
    BOOST_CHECK(!aMgr->ProcessDefault(auctionId, winner.bidId));
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, SpendOf({winner.utxo})));
}

// Current behaviour: ProcessDefault does not look at the result of
// CreateReAuction. Here the minimum price of the name was raised after the
// first auction, so the re-auction at the old reserve price is rejected; the
// default is processed all the same and the name is left without an auction.
BOOST_AUTO_TEST_CASE(process_default_succeeds_when_the_re_auction_cannot_be_created)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_REQUIRE(reddidManager->GetNamespaceManager()->AddPricingTier(NS, 3, 9000 * COIN));

    BOOST_CHECK(aMgr->ProcessDefault(auctionId, winner.bidId));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_DEFAULTED);

    AuctionInfo info;
    BOOST_CHECK(!aMgr->GetAuctionInfo(ExpectedAuctionId("alice", NS, GetTime()), info));
    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK_EQUAL(auctionIds.size(), 1U);
}

BOOST_AUTO_TEST_CASE(create_re_auction_chains_to_the_original)
{
    uint256 auctionId = NewAuction("alice", 6000 * COIN, 3 * DAY);
    AuctionInfo original = GetAuction(aMgr, auctionId);

    SetMockTime(T0 + 100);
    uint256 firstId;
    BOOST_REQUIRE(aMgr->CreateReAuction(original, firstId));
    BOOST_CHECK(firstId == ExpectedAuctionId("alice", NS, T0 + 100));
    AuctionInfo first = GetAuction(aMgr, firstId);
    BOOST_CHECK(first.creator == creator);
    BOOST_CHECK_EQUAL(first.reservePrice, 6000 * COIN);
    BOOST_CHECK_EQUAL(first.startTime, T0 + 100);
    // Current behaviour: the re-auction always runs for seven days, whatever
    // the original ran for
    BOOST_CHECK_EQUAL(first.endTime, T0 + 100 + 7 * DAY);
    BOOST_CHECK_EQUAL(first.reAuctionCount, 1);
    BOOST_CHECK(first.previousAuctionId == auctionId);
    BOOST_CHECK(first.txid.IsNull());

    SetMockTime(T0 + 200);
    uint256 secondId;
    BOOST_REQUIRE(aMgr->CreateReAuction(first, secondId));
    AuctionInfo second = GetAuction(aMgr, secondId);
    BOOST_CHECK_EQUAL(second.reAuctionCount, 2);
    BOOST_CHECK(second.previousAuctionId == firstId);

    // Current behaviour: within the same second the ID is taken
    uint256 thirdId;
    BOOST_CHECK(!aMgr->CreateReAuction(first, thirdId));
}

// ===================================================================
// Category 4: ReleaseLosingDeposits
// ===================================================================

BOOST_AUTO_TEST_CASE(release_losing_deposits)
{
    BOOST_CHECK(!aMgr->ReleaseLosingDeposits(uint256S("01"), uint256S("02")));

    uint256 auctionId = NewAuction("alice");
    DepositBid low = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid high = PlaceDepositBid(auctionId, 8000 * COIN);
    uint256 plainBid;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 9000 * COIN, plainBid));

    // The bid named as the winner keeps its lock
    BOOST_CHECK(aMgr->ReleaseLosingDeposits(auctionId, high.bidId));
    BOOST_CHECK(aMgr->IsDepositLocked(high.utxo));
    BOOST_CHECK(!GetBid(aMgr, high.bidId).refunded);
    BOOST_CHECK_EQUAL(GetLock(aMgr, low.utxo).state, DEPOSIT_RELEASED_LOST);
    BOOST_CHECK(GetBid(aMgr, low.bidId).refunded);
    BOOST_CHECK_EQUAL(GetBid(aMgr, low.bidId).depositState, DEPOSIT_RELEASED_LOST);
    // A bid without a deposit is left alone
    BOOST_CHECK(!GetBid(aMgr, plainBid).refunded);

    // Current behaviour: the auction itself is not touched, not even its
    // deposit total
    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.totalDepositsLocked, 900 * COIN + 1200 * COIN);

    // With an ID that is no bid of the auction, every remaining deposit is released
    BOOST_CHECK(aMgr->ReleaseLosingDeposits(auctionId, uint256()));
    BOOST_CHECK_EQUAL(GetLock(aMgr, high.utxo).state, DEPOSIT_RELEASED_LOST);
    BOOST_CHECK(GetBid(aMgr, high.bidId).refunded);
}

// ===================================================================
// Category 5: Spending locked deposits
// ===================================================================

// SUSPECTED BUG (documented, not fixed): a transaction may spend an actively
// locked deposit when it carries an OP_AUCTION_SETTLE record with the ID of
// the auction. Nothing else is looked at: the auction below is still running
// and has no winner, the record names no bid, and yet the deposit can be
// spent.
BOOST_AUTO_TEST_CASE(settle_record_unlocks_an_active_deposit_at_any_time)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid bid = PlaceDepositBid(auctionId, 6000 * COIN);
    uint256 otherId = NewAuction("bob");
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);

    // No record
    CMutableTransaction plain;
    plain.vin.emplace_back(bid.utxo);
    plain.vout.emplace_back(bid.deposit, P2PKH(bid.bidder));
    BOOST_CHECK(!aMgr->IsValidSettlementTx(CTransaction(plain), bid.utxo));
    BOOST_CHECK(!aMgr->CheckDepositLocks(CTransaction(plain)));

    // A settle record for another auction, a record of another kind, a truncated auction ID
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendWithRecord(bid.utxo, CreateAuctionSettleOpReturn(otherId, bid.bidId))));
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendWithRecord(bid.utxo, RecordScript(OP_AUCTION_DEFAULT, auctionId, bid.bidId))));
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendWithRecord(bid.utxo, RecordScript(OP_AUCTION_SETTLE, std::vector<unsigned char>(31, 1)))));

    // A settle record with the auction ID and a bid ID that does not exist
    CTransaction withBogusBid = SpendWithRecord(bid.utxo, CreateAuctionSettleOpReturn(auctionId, uint256S("0bad")));
    BOOST_CHECK(aMgr->IsValidSettlementTx(withBogusBid, bid.utxo));
    BOOST_CHECK(aMgr->CheckDepositLocks(withBogusBid));

    // A settle record with the auction ID and no bid ID at all
    CTransaction withoutBid = SpendWithRecord(bid.utxo, RecordScript(OP_AUCTION_SETTLE, auctionId));
    BOOST_CHECK(aMgr->IsValidSettlementTx(withoutBid, bid.utxo));
    BOOST_CHECK(aMgr->CheckDepositLocks(withoutBid));

    // The check does not change anything: the deposit is still recorded as locked
    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));

    // An outpoint without a lock is no settlement
    BOOST_CHECK(!aMgr->IsValidSettlementTx(withoutBid, COutPoint(uint256S("77"), 0)));
}

// SUSPECTED BUG (documented, not fixed): IsValidForfeitTx accepts every
// transaction while no community fund address is configured, which is the
// default. With an address configured it requires an OP_DEPOSIT_FORFEIT
// record and nothing more: the record need not name the auction or the bid,
// and no output has to pay the configured address.
BOOST_AUTO_TEST_CASE(forfeited_deposit_spending_rules)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    BOOST_CHECK(config.communityFundAddress.empty());
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid bid = PlaceDepositBid(auctionId, 6000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_REQUIRE(aMgr->ProcessDefault(auctionId, bid.bidId));
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, bid.utxo).state, DEPOSIT_FORFEITED);

    // The defaulting bidder takes the forfeited deposit back
    CMutableTransaction takeBack;
    takeBack.vin.emplace_back(bid.utxo);
    takeBack.vout.emplace_back(bid.deposit, P2PKH(bid.bidder));
    BOOST_CHECK(aMgr->IsValidForfeitTx(CTransaction(takeBack), bid.utxo));
    BOOST_CHECK(aMgr->CheckDepositLocks(CTransaction(takeBack)));

    config.communityFundAddress = "community-fund-address";
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    BOOST_CHECK(!aMgr->IsValidForfeitTx(CTransaction(takeBack), bid.utxo));
    BOOST_CHECK(!aMgr->CheckDepositLocks(CTransaction(takeBack)));
    // A settle record does not help with a forfeited deposit
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendWithRecord(bid.utxo, CreateAuctionSettleOpReturn(auctionId, bid.bidId))));

    // The same payment back to the bidder, with a bare forfeit record added
    takeBack.vout.emplace_back(0, RecordScript(OP_DEPOSIT_FORFEIT, std::vector<unsigned char>{}));
    BOOST_CHECK(aMgr->IsValidForfeitTx(CTransaction(takeBack), bid.utxo));
    BOOST_CHECK(aMgr->CheckDepositLocks(CTransaction(takeBack)));

    // An outpoint without a lock is no forfeit
    BOOST_CHECK(!aMgr->IsValidForfeitTx(CTransaction(takeBack), COutPoint(uint256S("77"), 0)));
}

BOOST_AUTO_TEST_CASE(check_deposit_locks_looks_at_every_input)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid released = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid consumed = PlaceDepositBid(auctionId, 7000 * COIN);
    DepositBid active = PlaceDepositBid(auctionId, 8000 * COIN);
    BOOST_REQUIRE(aMgr->UpdateDepositLock(released.utxo, DEPOSIT_RELEASED_LOST));
    BOOST_REQUIRE(aMgr->UpdateDepositLock(consumed.utxo, DEPOSIT_CONSUMED_WON));
    COutPoint unlocked(uint256S("55"), 0);

    BOOST_CHECK(aMgr->CheckDepositLocks(SpendOf({})));
    BOOST_CHECK(aMgr->CheckDepositLocks(SpendOf({unlocked})));
    BOOST_CHECK(aMgr->CheckDepositLocks(SpendOf({released.utxo})));
    BOOST_CHECK(aMgr->CheckDepositLocks(SpendOf({consumed.utxo})));
    BOOST_CHECK(aMgr->CheckDepositLocks(SpendOf({unlocked, released.utxo, consumed.utxo})));
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendOf({active.utxo})));
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendOf({unlocked, released.utxo, consumed.utxo, active.utxo})));
    BOOST_CHECK(!aMgr->CheckDepositLocks(SpendOf({active.utxo, unlocked})));

    // Another output of the transaction that holds a locked deposit is not locked
    BOOST_CHECK(aMgr->CheckDepositLocks(SpendOf({COutPoint(active.utxo.hash, 0)})));
}

// ===================================================================
// Category 6: Bidder statistics
// ===================================================================

BOOST_AUTO_TEST_CASE(bidder_stats_of_unknown_bidder_are_zero)
{
    uint256 auctionId = NewAuction("alice");
    PlaceDepositBid(auctionId, 6000 * COIN);

    BidderStats stats;
    stats.totalBids = 99;
    BOOST_CHECK(aMgr->GetBidderStats(MakeKeyID(), stats));
    BOOST_CHECK_EQUAL(stats.totalBids, 0);
    BOOST_CHECK_EQUAL(stats.auctionWins, 0);
    BOOST_CHECK_EQUAL(stats.auctionParticipations, 0);
    BOOST_CHECK_EQUAL(stats.depositsTotal, 0);
    BOOST_CHECK_EQUAL(stats.depositsCompleted, 0);
    BOOST_CHECK_EQUAL(stats.depositsForfeit, 0);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 0);
}

BOOST_AUTO_TEST_CASE(bidder_stats_count_bids_wins_deposits_and_defaults)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    CKeyID bidder = MakeKeyID();
    BidderStats stats;

    // Auction 1: bids, is outbid, raises, is outbid again and loses
    uint256 lostId = NewAuction("alice");
    DepositBid lostFirst = PlaceDepositBid(lostId, bidder, 6000 * COIN);
    PlaceDepositBid(lostId, 7000 * COIN);
    DepositBid lostRaise = PlaceDepositBid(lostId, bidder, 8000 * COIN);
    PlaceDepositBid(lostId, 9000 * COIN);
    // Auction 2: wins and settles
    uint256 wonId = NewAuction("bob");
    DepositBid won = PlaceDepositBid(wonId, bidder, 6000 * COIN);
    // Auction 3: wins and defaults
    uint256 defaultedId = NewAuction("carol");
    DepositBid defaulted = PlaceDepositBid(defaultedId, bidder, 6000 * COIN);

    BOOST_REQUIRE(aMgr->GetBidderStats(bidder, stats));
    BOOST_CHECK_EQUAL(stats.totalBids, 4);
    BOOST_CHECK_EQUAL(stats.auctionParticipations, 3);
    BOOST_CHECK_EQUAL(stats.auctionWins, 0);
    BOOST_CHECK_EQUAL(stats.depositsTotal, 4);
    BOOST_CHECK_EQUAL(stats.depositsCompleted, 0);
    BOOST_CHECK_EQUAL(stats.depositsForfeit, 0);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 0);

    EndAuction(lostId);
    BOOST_REQUIRE(aMgr->BeginSettlement(lostId, SpendOf({})));
    BOOST_REQUIRE(aMgr->BeginSettlement(wonId, SpendOf({})));
    BOOST_REQUIRE(aMgr->BeginSettlement(defaultedId, SpendOf({})));
    BOOST_REQUIRE(aMgr->CompleteSettlement(wonId, won.bidId, SpendOf({won.utxo})));
    BOOST_REQUIRE(aMgr->ProcessDefault(defaultedId, defaulted.bidId));

    BOOST_REQUIRE(aMgr->GetBidderStats(bidder, stats));
    BOOST_CHECK_EQUAL(stats.totalBids, 4);
    BOOST_CHECK_EQUAL(stats.auctionParticipations, 3);
    // The settled auction and the defaulted one (see process_default_keeps_the_winner_mark)
    BOOST_CHECK_EQUAL(stats.auctionWins, 2);
    BOOST_CHECK_EQUAL(stats.depositsTotal, 4);
    // The released deposit of the raised bid and the consumed deposit of the settled
    // auction; the deposit of the bid that was raised is still locked (see
    // deposit_of_a_replaced_bid_is_never_released)
    BOOST_CHECK_EQUAL(stats.depositsCompleted, 2);
    BOOST_CHECK_EQUAL(stats.depositsForfeit, 1);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 1);
    BOOST_CHECK(aMgr->IsDepositLocked(lostFirst.utxo));
    BOOST_CHECK_EQUAL(GetLock(aMgr, lostRaise.utxo).state, DEPOSIT_RELEASED_LOST);

    // Current behaviour: the statistics are derived from the deposit locks that
    // exist. Once the forfeit record has removed the lock, the forfeited deposit
    // is no longer counted, as a deposit or as a forfeit.
    BOOST_REQUIRE(Process(TxFrom(MakeKeyID(), RecordScript(OP_DEPOSIT_FORFEIT, defaultedId, defaulted.bidId))));
    BOOST_REQUIRE(aMgr->GetBidderStats(bidder, stats));
    BOOST_CHECK_EQUAL(stats.depositsTotal, 3);
    BOOST_CHECK_EQUAL(stats.depositsForfeit, 0);
    BOOST_CHECK_EQUAL(stats.auctionDefaults, 1);
    BOOST_CHECK_EQUAL(stats.auctionWins, 2);
}

// ===================================================================
// Category 7: Periodic tasks
// ===================================================================

// Current behaviour: CheckSettlementDeadlines only logs. An auction whose
// winner has missed the deadline stays in settlement, with the deposit
// locked, until an OP_AUCTION_DEFAULT record is processed.
BOOST_AUTO_TEST_CASE(check_settlement_deadlines_changes_nothing)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 6000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).settlementDeadlineHeight, 4320);

    aMgr->CheckSettlementDeadlines(4320);
    aMgr->CheckSettlementDeadlines(4321);
    aMgr->CheckSettlementDeadlines(1000000);

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK(GetBid(aMgr, winner.bidId).isWinner);

    {
        FakeChainHeight tip(chainstate, 5000);
        aMgr->CheckExpiredAuctions();
    }
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
}

BOOST_AUTO_TEST_CASE(check_expired_auctions_cancels_pending_and_finalizes_active)
{
    uint256 pendingId = NewAuction("alice", 5000 * COIN, 3 * DAY);
    uint256 activeId = NewAuction("bob", 5000 * COIN, 3 * DAY);
    uint256 runningId = NewAuction("carol", 5000 * COIN, 7 * DAY);
    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(activeId, bidder, 6000 * COIN, bidId));
    BOOST_REQUIRE(aMgr->PlaceBid(runningId, bidder, 6000 * COIN, bidId));

    // Nothing has expired
    BOOST_CHECK(!aMgr->ProcessExpiredAuctions(T0 + 3 * DAY - 1));
    aMgr->CheckExpiredAuctions();
    BOOST_CHECK_EQUAL(GetAuction(aMgr, pendingId).state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activeId).state, AUCTION_ACTIVE);

    SetMockTime(T0 + 3 * DAY);
    aMgr->CheckExpiredAuctions();

    BOOST_CHECK_EQUAL(GetAuction(aMgr, pendingId).state, AUCTION_CANCELED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activeId).state, AUCTION_FINALIZED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, runningId).state, AUCTION_ACTIVE);
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("bob", NS, userId));
    BOOST_CHECK(userId.owner == bidder);
    BOOST_CHECK_EQUAL(aMgr->GetActiveAuctions().size(), 1U);

    // Nothing left to do
    BOOST_CHECK(!aMgr->ProcessExpiredAuctions(T0 + 3 * DAY));
}

// Current behaviour: ProcessExpiredAuctions compares against the time it is
// given, but FinalizeAuction, which it calls, compares against the clock. An
// active auction that has expired by the given time and not by the clock is
// reported as processed and left as it was.
BOOST_AUTO_TEST_CASE(process_expired_auctions_finalizes_by_the_clock_not_the_given_time)
{
    uint256 auctionId = NewAuction("alice", 5000 * COIN, 3 * DAY);
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 6000 * COIN, bidId));

    BOOST_CHECK(aMgr->ProcessExpiredAuctions(T0 + 3 * DAY));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);
}

// SUSPECTED BUG (documented, not fixed): ProcessExpiredAuctions finalizes
// every expired active auction with FinalizeAuction, the path for auctions
// without deposits. An auction with deposits that expires before its creator
// sends the finalize record therefore skips settlement: the name is
// registered to the highest bidder at once, without the settlement
// transaction, and the deposits of winner and losers stay locked with no
// record left that could release them.
BOOST_AUTO_TEST_CASE(expiry_of_an_auction_with_deposits_bypasses_settlement)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);

    EndAuction(auctionId);
    BOOST_CHECK(aMgr->ProcessExpiredAuctions(GetTime()));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_FINALIZED);
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 0);
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == winner.bidder);

    BOOST_CHECK(GetBid(aMgr, winner.bidId).isWinner);
    BOOST_CHECK(GetBid(aMgr, loser.bidId).refunded);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK(aMgr->IsDepositLocked(loser.utxo));
    BOOST_CHECK_EQUAL(GetBid(aMgr, loser.bidId).depositState, DEPOSIT_LOCKED_ACTIVE);

    // Neither settlement nor default applies to a finalized auction
    BOOST_CHECK(!aMgr->BeginSettlement(auctionId, SpendOf({})));
    BOOST_CHECK(!aMgr->CompleteSettlement(auctionId, winner.bidId, SpendOf({winner.utxo})));
    BOOST_CHECK(!aMgr->ProcessDefault(auctionId, winner.bidId));
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK(aMgr->IsDepositLocked(loser.utxo));
}

BOOST_AUTO_TEST_SUITE_END()
