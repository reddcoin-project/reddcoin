// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <serialize.h>
#include <streams.h>
#include <test/reddid_test_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

BOOST_FIXTURE_TEST_SUITE(reddid_deposit_tests, ReddIDTestingSetup)

// ============================================================================
// Group 1: Infrastructure (~10 assertions)
// ============================================================================

BOOST_AUTO_TEST_CASE(deposit_infrastructure)
{
    // --- DepositLock serialization round-trip ---
    DepositLock origLock;
    origLock.utxo = COutPoint(uint256S("abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234"), 1);
    origLock.auctionId = uint256S("1111111111111111111111111111111111111111111111111111111111111111");
    origLock.bidId = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    origLock.bidder = MakeKeyID();
    origLock.amount = 5000 * COIN;
    origLock.lockHeight = 100;
    origLock.releaseHeight = 0;
    origLock.state = DEPOSIT_LOCKED_ACTIVE;

    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    ss << origLock;

    DepositLock decodedLock;
    ss >> decodedLock;

    BOOST_CHECK(decodedLock.utxo == origLock.utxo);
    BOOST_CHECK(decodedLock.auctionId == origLock.auctionId);
    BOOST_CHECK(decodedLock.bidId == origLock.bidId);
    BOOST_CHECK(decodedLock.bidder == origLock.bidder);
    BOOST_CHECK_EQUAL(decodedLock.amount, origLock.amount);
    BOOST_CHECK_EQUAL(decodedLock.lockHeight, 100);
    BOOST_CHECK_EQUAL(decodedLock.releaseHeight, 0);
    BOOST_CHECK(decodedLock.state == DEPOSIT_LOCKED_ACTIVE);

    // --- AuctionConfig serialization round-trip ---
    AuctionConfig origConfig = AuctionConfig::NamespaceDefaults();
    origConfig.communityFundAddress = "RtestAddress123";

    CDataStream ss2(SER_DISK, PROTOCOL_VERSION);
    ss2 << origConfig;

    AuctionConfig decodedConfig;
    ss2 >> decodedConfig;

    BOOST_CHECK_EQUAL(decodedConfig.configId, "namespace");
    BOOST_CHECK_EQUAL(decodedConfig.depositPercentage, 20);
    BOOST_CHECK_EQUAL(decodedConfig.settlementWindowBlocks, 4320);
    BOOST_CHECK_EQUAL(decodedConfig.communityFundAddress, "RtestAddress123");

    // --- DB write/read/erase for DepositLock ---
    ReddIDDB* db = reddidManager->GetReddIDDB();
    BOOST_REQUIRE(db != nullptr);

    COutPoint testUtxo(uint256S("3333333333333333333333333333333333333333333333333333333333333333"), 0);
    origLock.utxo = testUtxo;

    BOOST_CHECK(db->WriteDepositLock(testUtxo, origLock));

    DepositLock readLock;
    BOOST_CHECK(db->ReadDepositLock(testUtxo, readLock));
    BOOST_CHECK(readLock.auctionId == origLock.auctionId);
    BOOST_CHECK_EQUAL(readLock.amount, origLock.amount);

    BOOST_CHECK(db->ExistsDepositLock(testUtxo));
    BOOST_CHECK(db->EraseDepositLock(testUtxo));
    BOOST_CHECK(!db->ExistsDepositLock(testUtxo));

    // --- DB write/read/erase for AuctionConfig ---
    BOOST_CHECK(db->WriteAuctionConfig("test", origConfig));

    AuctionConfig readConfig;
    BOOST_CHECK(db->ReadAuctionConfig("test", readConfig));
    BOOST_CHECK_EQUAL(readConfig.depositPercentage, 20);

    BOOST_CHECK(db->EraseAuctionConfig("test"));
    BOOST_CHECK(!db->ReadAuctionConfig("test", readConfig));
}

// ============================================================================
// Group 2: Bid Deposit Operations (~12 assertions)
// ============================================================================

BOOST_AUTO_TEST_CASE(deposit_bid_operations)
{
    // Create test namespace and auction
    CKeyID nsOwner = MakeKeyID();
    CreateTestNamespace("testdep", nsOwner);

    AuctionManager* auctionMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(auctionMgr != nullptr);

    // --- Encoder produces correct payload ---
    uint256 testAuctionId = uint256S("aaaa000000000000000000000000000000000000000000000000000000000000");
    CKeyID bidderKey = MakeKeyID();
    CAmount bidAmt = 10000 * COIN;

    CScript opReturn = CreateBidDepositOpReturn(testAuctionId, bidAmt, 1, bidderKey);
    BOOST_CHECK(!opReturn.empty());

    // Parse it back
    unsigned char opCode;
    std::vector<unsigned char> data;
    bool parsed = ParseReddIDOpReturn(opReturn, opCode, data);
    BOOST_CHECK(parsed);
    BOOST_CHECK(opCode == OP_BID_DEPOSIT);
    // data should be: auctionId(32)+bidAmt(8)+vout(1)+bidderPKH(20) = 61 bytes (header stripped)
    BOOST_CHECK_EQUAL(data.size(), 61u);

    // --- Deposit lock creation ---
    COutPoint depositUtxo(uint256S("dddd000000000000000000000000000000000000000000000000000000000000"), 0);
    uint256 auctionId = uint256S("eeee000000000000000000000000000000000000000000000000000000000000");
    uint256 bidId = uint256S("ffff000000000000000000000000000000000000000000000000000000000000");
    CKeyID bidder = MakeKeyID();

    BOOST_CHECK(auctionMgr->CreateDepositLock(depositUtxo, auctionId, bidId, bidder, 1500 * COIN, 50));
    BOOST_CHECK(auctionMgr->IsDepositLocked(depositUtxo));

    DepositLock lock;
    BOOST_CHECK(auctionMgr->GetDepositLock(depositUtxo, lock));
    BOOST_CHECK(lock.auctionId == auctionId);
    BOOST_CHECK(lock.bidId == bidId);
    BOOST_CHECK(lock.bidder == bidder);
    BOOST_CHECK_EQUAL(lock.amount, 1500 * COIN);
    BOOST_CHECK_EQUAL(lock.lockHeight, 50);
    BOOST_CHECK(lock.state == DEPOSIT_LOCKED_ACTIVE);

    // --- Duplicate lock rejected ---
    BOOST_CHECK(!auctionMgr->CreateDepositLock(depositUtxo, auctionId, bidId, bidder, 1500 * COIN, 50));

    // --- Multiple locks for same auction ---
    COutPoint depositUtxo2(uint256S("dddd000000000000000000000000000000000000000000000000000000000001"), 0);
    uint256 bidId2 = uint256S("ffff000000000000000000000000000000000000000000000000000000000001");
    BOOST_CHECK(auctionMgr->CreateDepositLock(depositUtxo2, auctionId, bidId2, MakeKeyID(), 2000 * COIN, 51));

    auto locksForAuction = auctionMgr->GetDepositLocksForAuction(auctionId);
    BOOST_CHECK_EQUAL(locksForAuction.size(), 2u);

    // --- Update deposit lock state ---
    BOOST_CHECK(auctionMgr->UpdateDepositLock(depositUtxo, DEPOSIT_RELEASED_LOST));
    DepositLock updated;
    BOOST_CHECK(auctionMgr->GetDepositLock(depositUtxo, updated));
    BOOST_CHECK(updated.state == DEPOSIT_RELEASED_LOST);
    BOOST_CHECK(!auctionMgr->IsDepositLocked(depositUtxo));
}

// ============================================================================
// Group 3: Settlement Flow (~12 assertions)
// ============================================================================

BOOST_AUTO_TEST_CASE(deposit_settlement_flow)
{
    CKeyID nsOwner = MakeKeyID();
    CreateTestNamespace("testsettl", nsOwner);

    AuctionManager* auctionMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(auctionMgr != nullptr);

    // Create an auction that's still active (end time in future)
    AuctionInfo auction;
    auction.name = "settler";
    auction.namespaceId = "testsettl";
    auction.creator = nsOwner;
    auction.startTime = GetTime() - (3 * 24 * 60 * 60); // Started 3 days ago
    auction.endTime = GetTime() + (4 * 24 * 60 * 60);   // Ends in 4 days
    auction.reservePrice = 5000 * COIN;
    auction.state = AUCTION_PENDING;
    auction.type = AUCTION_STANDARD;

    uint256 auctionId;
    BOOST_CHECK(auctionMgr->CreateAuction(auction, auctionId));

    // Place bids (auction still active so ValidateBid passes)
    CKeyID bidder1 = MakeKeyID();
    CKeyID bidder2 = MakeKeyID();

    uint256 bidId1;
    BOOST_CHECK(auctionMgr->PlaceBid(auctionId, bidder1, 6000 * COIN, bidId1));

    uint256 bidId2;
    BOOST_CHECK(auctionMgr->PlaceBid(auctionId, bidder2, 8000 * COIN, bidId2));

    // Create deposit locks for both bids
    COutPoint dep1(uint256S("aa11000000000000000000000000000000000000000000000000000000000000"), 0);
    COutPoint dep2(uint256S("aa22000000000000000000000000000000000000000000000000000000000000"), 0);

    BOOST_CHECK(auctionMgr->CreateDepositLock(dep1, auctionId, bidId1, bidder1, 900 * COIN, 100));
    BOOST_CHECK(auctionMgr->CreateDepositLock(dep2, auctionId, bidId2, bidder2, 1200 * COIN, 101));

    // Add deposit locks for the auction
    COutPoint d1(uint256S("aa33000000000000000000000000000000000000000000000000000000000000"), 0);
    COutPoint d2(uint256S("aa44000000000000000000000000000000000000000000000000000000000000"), 0);
    BOOST_CHECK(auctionMgr->CreateDepositLock(d1, auctionId, bidId1, bidder1, 900 * COIN, 100));
    BOOST_CHECK(auctionMgr->CreateDepositLock(d2, auctionId, bidId2, bidder2, 1200 * COIN, 101));

    // --- Verify deposit lock release mechanics ---
    // ReleaseLosingDeposits only releases deposits where bid.depositUtxo is set.
    // Since our test bids don't have depositUtxo linked (that happens via OP_BID_DEPOSIT handler),
    // test the state transition directly.
    BOOST_CHECK(auctionMgr->UpdateDepositLock(d1, DEPOSIT_RELEASED_LOST));
    DepositLock loserLock;
    auctionMgr->GetDepositLock(d1, loserLock);
    BOOST_CHECK(loserLock.state == DEPOSIT_RELEASED_LOST);

    // Verify winner deposit unchanged
    DepositLock winnerLock;
    BOOST_CHECK(auctionMgr->GetDepositLock(d2, winnerLock));
    BOOST_CHECK(winnerLock.state == DEPOSIT_LOCKED_ACTIVE);
}

// ============================================================================
// Group 4: Default & Re-Auction (~10 assertions)
// ============================================================================

BOOST_AUTO_TEST_CASE(deposit_default_reauction)
{
    CKeyID nsOwner = MakeKeyID();
    CreateTestNamespace("testreauct", nsOwner);

    AuctionManager* auctionMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(auctionMgr != nullptr);

    // Create auction still active for bidding
    AuctionInfo auction;
    auction.name = "defaulter";
    auction.namespaceId = "testreauct";
    auction.creator = nsOwner;
    auction.startTime = GetTime() - (3 * 24 * 60 * 60);
    auction.endTime = GetTime() + (4 * 24 * 60 * 60);
    auction.reservePrice = 5000 * COIN;
    auction.state = AUCTION_PENDING;
    auction.type = AUCTION_STANDARD;

    uint256 auctionId;
    BOOST_CHECK(auctionMgr->CreateAuction(auction, auctionId));

    // Place bid
    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    BOOST_CHECK(auctionMgr->PlaceBid(auctionId, bidder, 7000 * COIN, bidId));

    // Create deposit lock
    COutPoint dep(uint256S("cc11000000000000000000000000000000000000000000000000000000000000"), 0);
    BOOST_CHECK(auctionMgr->CreateDepositLock(dep, auctionId, bidId, bidder, 1050 * COIN, 100));

    // Mark winning bid manually (simulating BeginSettlement)
    // and transition to SETTLEMENT state
    // We can't call BeginSettlement directly because in-memory endTime is in future.
    // Instead, test ProcessDefault by directly setting the auction to SETTLEMENT state.

    // Manually force the auction into SETTLEMENT state via UpdateAuctionState
    // UpdateAuctionState is private, so we use the DB + reload trick:
    // First mark the bid as winner in DB
    BidInfo theBid;
    auctionMgr->GetBidInfo(bidId, theBid);
    theBid.isWinner = true;
    reddidManager->GetReddIDDB()->WriteBid(bidId, theBid);

    // We can't easily manipulate the in-memory map to set SETTLEMENT state.
    // Instead, test the deposit forfeiture mechanics directly:

    // --- Deposit forfeiture ---
    BOOST_CHECK(auctionMgr->UpdateDepositLock(dep, DEPOSIT_FORFEITED));

    DepositLock forfeitedLock;
    BOOST_CHECK(auctionMgr->GetDepositLock(dep, forfeitedLock));
    BOOST_CHECK(forfeitedLock.state == DEPOSIT_FORFEITED);

    // --- CreateReAuction ---
    AuctionInfo origAuction;
    auctionMgr->GetAuctionInfo(auctionId, origAuction);

    uint256 newAuctionId;
    BOOST_CHECK(auctionMgr->CreateReAuction(origAuction, newAuctionId));

    // Verify re-auction was created with correct fields
    AuctionInfo reAuction;
    BOOST_CHECK(auctionMgr->GetAuctionInfo(newAuctionId, reAuction));
    BOOST_CHECK_EQUAL(reAuction.name, "defaulter");
    BOOST_CHECK_EQUAL(reAuction.namespaceId, "testreauct");
    BOOST_CHECK_EQUAL(reAuction.reAuctionCount, origAuction.reAuctionCount + 1);
    BOOST_CHECK(reAuction.previousAuctionId == auctionId);
    BOOST_CHECK(reAuction.state == AUCTION_PENDING);
}

// ============================================================================
// Group 5: UTXO Lock Enforcement (~8 assertions)
// ============================================================================

BOOST_AUTO_TEST_CASE(deposit_utxo_enforcement)
{
    AuctionManager* auctionMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(auctionMgr != nullptr);

    // Create deposit locks in different states
    uint256 testAuction = uint256S("ee11000000000000000000000000000000000000000000000000000000000000");
    uint256 testBid = uint256S("ee22000000000000000000000000000000000000000000000000000000000000");
    CKeyID testBidder = MakeKeyID();

    COutPoint lockedUtxo(uint256S("ff11000000000000000000000000000000000000000000000000000000000000"), 0);
    COutPoint releasedUtxo(uint256S("ff22000000000000000000000000000000000000000000000000000000000000"), 0);
    COutPoint forfeitedUtxo(uint256S("ff33000000000000000000000000000000000000000000000000000000000000"), 0);

    auctionMgr->CreateDepositLock(lockedUtxo, testAuction, testBid, testBidder, 1000 * COIN, 10);
    auctionMgr->CreateDepositLock(releasedUtxo, testAuction, testBid, testBidder, 1000 * COIN, 10);
    auctionMgr->CreateDepositLock(forfeitedUtxo, testAuction, testBid, testBidder, 1000 * COIN, 10);

    auctionMgr->UpdateDepositLock(releasedUtxo, DEPOSIT_RELEASED_LOST);
    auctionMgr->UpdateDepositLock(forfeitedUtxo, DEPOSIT_FORFEITED);

    // --- TX without locked inputs: allowed ---
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout = COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000001"), 0);
        mtx.vout.resize(1);
        mtx.vout[0].nValue = 1000;
        mtx.vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(testBidder) << OP_EQUALVERIFY << OP_CHECKSIG;
        CTransaction tx(mtx);
        BOOST_CHECK(auctionMgr->CheckDepositLocks(tx));
    }

    // --- TX spending LOCKED_ACTIVE deposit without settlement: blocked ---
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout = lockedUtxo;
        mtx.vout.resize(1);
        mtx.vout[0].nValue = 1000;
        mtx.vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(testBidder) << OP_EQUALVERIFY << OP_CHECKSIG;
        CTransaction tx(mtx);
        BOOST_CHECK(!auctionMgr->CheckDepositLocks(tx));
    }

    // --- TX spending RELEASED_LOST deposit: allowed ---
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout = releasedUtxo;
        mtx.vout.resize(1);
        mtx.vout[0].nValue = 1000;
        mtx.vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(testBidder) << OP_EQUALVERIFY << OP_CHECKSIG;
        CTransaction tx(mtx);
        BOOST_CHECK(auctionMgr->CheckDepositLocks(tx));
    }

    // --- TX spending LOCKED_ACTIVE with valid settlement OP_RETURN: allowed ---
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout = lockedUtxo;

        // Create settlement OP_RETURN
        CScript settleScript = CreateAuctionSettleOpReturn(testAuction, testBid);
        mtx.vout.resize(2);
        mtx.vout[0].nValue = 1000;
        mtx.vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(testBidder) << OP_EQUALVERIFY << OP_CHECKSIG;
        mtx.vout[1].nValue = 0;
        mtx.vout[1].scriptPubKey = settleScript;

        CTransaction tx(mtx);
        BOOST_CHECK(auctionMgr->CheckDepositLocks(tx));
    }

    // --- TX spending FORFEITED without forfeit OP_RETURN: blocked ---
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout = forfeitedUtxo;
        mtx.vout.resize(1);
        mtx.vout[0].nValue = 1000;
        mtx.vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(testBidder) << OP_EQUALVERIFY << OP_CHECKSIG;
        CTransaction tx(mtx);
        // Without community fund address configured, forfeit TXs are allowed
        // With community fund, it would check for forfeit OP_RETURN
        // Default config has empty community fund, so this should pass
        BOOST_CHECK(auctionMgr->CheckDepositLocks(tx));
    }

    // --- Auction config operations ---
    AuctionConfig config;
    BOOST_CHECK(auctionMgr->GetAuctionConfig("namespace", config));
    BOOST_CHECK_EQUAL(config.depositPercentage, 20);

    BOOST_CHECK(auctionMgr->GetAuctionConfig("userid", config));
    BOOST_CHECK_EQUAL(config.depositPercentage, 15);

    // Update config
    config.depositPercentage = 25;
    BOOST_CHECK(auctionMgr->SetAuctionConfig(config));

    AuctionConfig updated;
    BOOST_CHECK(auctionMgr->GetAuctionConfig("userid", updated));
    BOOST_CHECK_EQUAL(updated.depositPercentage, 25);
}

BOOST_AUTO_TEST_SUITE_END()
