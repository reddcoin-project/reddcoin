// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/reddid_test_fixture.h>
#include <uint256.h>
#include <util/strencodings.h>

// Helper: build a CTransaction with OP_RETURN + P2PKH output
static CTransaction MakeOpReturnTx(const CScript& opReturnScript, const CKeyID& addr)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(GetRandHash(), 0);
    mtx.vout.push_back(CTxOut(0, opReturnScript));
    CScript p2pkh = GetScriptForDestination(PKHash(addr));
    mtx.vout.push_back(CTxOut(1000, p2pkh));
    return CTransaction(mtx);
}

BOOST_FIXTURE_TEST_SUITE(reddid_reorg_tests, ReddIDTestingSetup)

// ============================================================================
// Group 1: Auction Create & Bid Undo (~12 assertions)
// State is set up via direct manager API, then undone via OP_RETURN TXs.
// ============================================================================

BOOST_AUTO_TEST_CASE(undo_auction_create_and_bid)
{
    AuctionManager* auctionMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(auctionMgr != nullptr);

    CKeyID nsOwner = MakeKeyID();
    CreateTestNamespace("reorgns", nsOwner);

    // --- Test 1: Create auction directly, undo via OP_RETURN TX ---
    AuctionInfo auction;
    auction.name = "reorgtest";
    auction.namespaceId = "reorgns";
    auction.creator = nsOwner;
    auction.startTime = GetTime() - 86400;
    auction.endTime = GetTime() + (4 * 86400);
    auction.reservePrice = 5000 * COIN;
    auction.state = AUCTION_PENDING;
    auction.type = AUCTION_STANDARD;

    uint256 auctionId;
    BOOST_CHECK(auctionMgr->CreateAuction(auction, auctionId));

    // Verify auction exists
    AuctionInfo check;
    BOOST_CHECK(auctionMgr->GetAuctionInfo(auctionId, check));
    BOOST_CHECK_EQUAL(check.name, "reorgtest");

    // Build OP_RETURN TX matching the create
    CScript createOpReturn = reddidManager->CreateUserIDAuctionOpReturn("reorgtest", "reorgns");
    BOOST_CHECK(!createOpReturn.empty());
    CTransaction createTx = MakeOpReturnTx(createOpReturn, nsOwner);

    // Undo the auction create
    BOOST_CHECK(auctionMgr->UndoTransaction(createTx, 100));

    // Verify auction is gone
    AuctionInfo after;
    BOOST_CHECK(!auctionMgr->GetAuctionInfo(auctionId, after));

    // --- Test 2: Create auction, place bids, undo bids ---
    AuctionInfo auction2;
    auction2.name = "bidundo";
    auction2.namespaceId = "reorgns";
    auction2.creator = nsOwner;
    auction2.startTime = GetTime() - 86400;
    auction2.endTime = GetTime() + (4 * 86400);
    auction2.reservePrice = 5000 * COIN;
    auction2.state = AUCTION_PENDING;
    auction2.type = AUCTION_STANDARD;

    uint256 auctionId2;
    BOOST_CHECK(auctionMgr->CreateAuction(auction2, auctionId2));

    CKeyID bidder1 = MakeKeyID();
    CKeyID bidder2 = MakeKeyID();
    uint256 bidId1, bidId2;
    BOOST_CHECK(auctionMgr->PlaceBid(auctionId2, bidder1, 6000 * COIN, bidId1));
    BOOST_CHECK(auctionMgr->PlaceBid(auctionId2, bidder2, 8000 * COIN, bidId2));

    // Verify highest bid
    AuctionInfo auctionCheck;
    auctionMgr->GetAuctionInfo(auctionId2, auctionCheck);
    BOOST_CHECK_EQUAL(auctionCheck.currentBid, 8000 * COIN);

    // Undo bid2
    CScript bid2OpReturn = reddidManager->CreateUserIDBidOpReturn(auctionId2, 8000 * COIN);
    CTransaction bid2Tx = MakeOpReturnTx(bid2OpReturn, bidder2);
    BOOST_CHECK(auctionMgr->UndoTransaction(bid2Tx, 102));

    auctionMgr->GetAuctionInfo(auctionId2, auctionCheck);
    BOOST_CHECK_EQUAL(auctionCheck.currentBid, 6000 * COIN);

    // Undo bid1
    CScript bid1OpReturn = reddidManager->CreateUserIDBidOpReturn(auctionId2, 6000 * COIN);
    CTransaction bid1Tx = MakeOpReturnTx(bid1OpReturn, bidder1);
    BOOST_CHECK(auctionMgr->UndoTransaction(bid1Tx, 101));

    auctionMgr->GetAuctionInfo(auctionId2, auctionCheck);
    BOOST_CHECK_EQUAL(auctionCheck.currentBid, 0);
    BOOST_CHECK(auctionCheck.state == AUCTION_PENDING);
}

// ============================================================================
// Group 2: Deposit Lock Undo (~10 assertions)
// Creates deposit lock directly, then undoes via OP_BID_DEPOSIT TX.
// ============================================================================

BOOST_AUTO_TEST_CASE(undo_deposit_lock)
{
    AuctionManager* auctionMgr = reddidManager->GetAuctionManager();
    BOOST_REQUIRE(auctionMgr != nullptr);

    CKeyID nsOwner = MakeKeyID();
    CreateTestNamespace("depns", nsOwner);

    // Create auction
    AuctionInfo auction;
    auction.name = "deptest";
    auction.namespaceId = "depns";
    auction.creator = nsOwner;
    auction.startTime = GetTime() - 86400;
    auction.endTime = GetTime() + (4 * 86400);
    auction.reservePrice = 5000 * COIN;
    auction.state = AUCTION_PENDING;
    auction.type = AUCTION_STANDARD;

    uint256 auctionId;
    BOOST_CHECK(auctionMgr->CreateAuction(auction, auctionId));

    // Set up deposit state directly: create bid + deposit lock
    CKeyID bidder = MakeKeyID();
    CAmount bidAmount = 7000 * COIN;
    CAmount depositAmount = 1050 * COIN;

    uint256 bidId;
    BOOST_CHECK(auctionMgr->PlaceBid(auctionId, bidder, bidAmount, bidId));

    // Build the OP_BID_DEPOSIT TX (for undo purposes)
    CScript depOpReturn = reddidManager->CreateBidDepositOpReturn(auctionId, bidAmount, 1, bidder);
    BOOST_CHECK(!depOpReturn.empty());

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint(GetRandHash(), 0);
    mtx.vout.push_back(CTxOut(0, depOpReturn));
    CScript depositScript = GetScriptForDestination(PKHash(bidder));
    mtx.vout.push_back(CTxOut(depositAmount, depositScript));
    CTransaction depTx(mtx);

    // Create deposit lock at the same UTXO the undo will look for
    COutPoint depUtxo(depTx.GetHash(), 1);
    BOOST_CHECK(auctionMgr->CreateDepositLock(depUtxo, auctionId, bidId, bidder, depositAmount, 100));

    // Verify deposit is locked
    BOOST_CHECK(auctionMgr->IsDepositLocked(depUtxo));
    DepositLock lock;
    BOOST_CHECK(auctionMgr->GetDepositLock(depUtxo, lock));
    BOOST_CHECK(lock.state == DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK_EQUAL(lock.amount, depositAmount);

    // Update auction totalDepositsLocked to match
    AuctionInfo aInfo;
    auctionMgr->GetAuctionInfo(auctionId, aInfo);

    // Now undo the BID_DEPOSIT TX
    BOOST_CHECK(auctionMgr->UndoTransaction(depTx, 100));

    // Deposit lock should be gone
    BOOST_CHECK(!auctionMgr->IsDepositLocked(depUtxo));

    // Bid should be removed, auction should revert
    auctionMgr->GetAuctionInfo(auctionId, aInfo);
    BOOST_CHECK_EQUAL(aInfo.currentBid, 0);
}

// ============================================================================
// Group 3: Namespace Undo & Full Block Undo (~10 assertions)
// Creates namespace auction state directly, undoes via OP_RETURN TX.
// ============================================================================

BOOST_AUTO_TEST_CASE(undo_namespace_and_block)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    BOOST_REQUIRE(nsMgr != nullptr);

    CKeyID creator = MakeKeyID();

    // --- Test: Create namespace auction directly, undo via TX ---
    uint256 nsAuctionId;
    BOOST_CHECK(nsMgr->CreateNamespaceAuction("undons", creator, 10000 * COIN, 14, AUCTION_STANDARD, nsAuctionId));

    // Verify it exists
    AuctionInfo nsCheck;
    BOOST_CHECK(nsMgr->GetAuctionInfo(nsAuctionId, nsCheck));

    // Build the OP_RETURN TX for namespace create
    CScript nsCreateOpReturn = reddidManager->CreateNamespaceAuctionOpReturn("undons");
    CTransaction nsCreateTx = MakeOpReturnTx(nsCreateOpReturn, creator);

    // Undo it
    BOOST_CHECK(nsMgr->UndoTransaction(nsCreateTx, 200));

    // Should be gone
    AuctionInfo nsAfter;
    BOOST_CHECK(!nsMgr->GetAuctionInfo(nsAuctionId, nsAfter));

    // --- Test: Full block undo ---
    CBlock block;
    // Coinbase
    CMutableTransaction coinbaseMtx;
    coinbaseMtx.vin.resize(1);
    coinbaseMtx.vin[0].prevout.SetNull();
    coinbaseMtx.vin[0].scriptSig = CScript() << 300 << OP_0;
    coinbaseMtx.vout.push_back(CTxOut(50 * COIN, CScript() << OP_TRUE));
    block.vtx.push_back(MakeTransactionRef(CTransaction(coinbaseMtx)));

    // Create another namespace auction
    CKeyID creator2 = MakeKeyID();
    uint256 nsAuctionId2;
    BOOST_CHECK(nsMgr->CreateNamespaceAuction("blkns", creator2, 10000 * COIN, 14, AUCTION_STANDARD, nsAuctionId2));

    // Build matching TX and add to block
    CScript ns2OpReturn = reddidManager->CreateNamespaceAuctionOpReturn("blkns");
    CTransaction tx1 = MakeOpReturnTx(ns2OpReturn, creator2);
    block.vtx.push_back(MakeTransactionRef(tx1));

    // Verify state exists
    AuctionInfo blkCheck;
    BOOST_CHECK(nsMgr->GetAuctionInfo(nsAuctionId2, blkCheck));

    // Undo entire block
    BOOST_CHECK(reddidManager->UndoBlock(block, 300));

    // State should be reversed
    BOOST_CHECK(!nsMgr->GetAuctionInfo(nsAuctionId2, blkCheck));
}

// ============================================================================
// Group 4: Profile Undo (~6 assertions)
// ============================================================================

BOOST_AUTO_TEST_CASE(undo_profile_operations)
{
    ProfileManager* profileMgr = reddidManager->GetProfileManager();
    BOOST_REQUIRE(profileMgr != nullptr);

    // Create a profile
    ReddIDProfile profile;
    profile.reddId = "undoprofile";
    profile.displayName = "Undo Test";
    profile.bio = "Testing profile undo";
    profile.creationTime = GetTime();

    BOOST_CHECK(profileMgr->CreateProfile(profile));

    // Verify exists
    ReddIDProfile readProfile;
    BOOST_CHECK(profileMgr->GetProfile("undoprofile", readProfile));
    BOOST_CHECK_EQUAL(readProfile.displayName, "Undo Test");

    // Build profile update OP_RETURN and undo
    uint256 profileHash = GetRandHash();
    CScript profileOpReturn = reddidManager->CreateProfileUpdateOpReturn("undoprofile", profileHash);
    BOOST_CHECK(!profileOpReturn.empty());
    CKeyID dummy = MakeKeyID();
    CTransaction profileTx = MakeOpReturnTx(profileOpReturn, dummy);

    BOOST_CHECK(profileMgr->UndoTransaction(profileTx, 400));

    // Profile should be erased
    ReddIDProfile afterUndo;
    BOOST_CHECK(!profileMgr->GetProfile("undoprofile", afterUndo));

    // --- Test connection undo ---
    ReddIDProfile from, to;
    from.reddId = "connfrom";
    from.displayName = "From";
    from.creationTime = GetTime();
    to.reddId = "connto";
    to.displayName = "To";
    to.creationTime = GetTime();

    profileMgr->CreateProfile(from);
    profileMgr->CreateProfile(to);

    ReddIDConnection conn;
    conn.fromReddId = "connfrom";
    conn.toReddId = "connto";
    conn.connectionType = CONNECTION_FOLLOW;
    conn.creationTime = GetTime();
    conn.txid = GetRandHash();
    BOOST_CHECK(profileMgr->CreateConnection(conn));

    auto conns = profileMgr->GetConnections("connfrom");
    BOOST_CHECK_EQUAL(conns.size(), 1u);

    CScript connOpReturn = reddidManager->CreateConnectionOpReturn("connfrom", "connto", CONNECTION_FOLLOW);
    CTransaction connTx = MakeOpReturnTx(connOpReturn, dummy);
    BOOST_CHECK(profileMgr->UndoTransaction(connTx, 401));

    auto afterConns = profileMgr->GetConnections("connfrom");
    BOOST_CHECK_EQUAL(afterConns.size(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
